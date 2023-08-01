/*
 *  server_example_inputs.c
 *
 *  This example demonstrates how to use inputs, GOOSE and SMV subscribing.
 *
 */

#include "inputs_api.h"
#include "iec61850_model_extensions.h"
#include "iec61850_dynamic_model_extensions.h"
#include "iec61850_config_file_parser_extensions.h"
#include "LNParse.h"
#include <libiec61850/sv_publisher.h>

#include <libiec61850/iec61850_server.h>
#include <libiec61850/hal_thread.h> /* for Thread_sleep() */


#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include <string.h> //strlen 
#include <errno.h> 
#include <unistd.h> //close 
#include <arpa/inet.h> //close 
#include <sys/types.h> 
#include <sys/socket.h> 
#include <netinet/in.h> 
#include <sys/time.h> //FD_SET, FD_ISSET, FD_ZERO macros 

#include <dirent.h>
#include <sys/stat.h>
#include <dlfcn.h>

#include <libiec61850/hal_socket.h>
#include "timestep_config.h"

static int global_step = 0;
static int global_timestep_type = TIMESTEP_TYPE_LOCAL; //TIMESTEP_TYPE_REMOTE
static int running = 0;
static IedServer iedServer = NULL;

typedef int (*lib_init_func)(IedModel* model, IedModel_extensions* model_ex);

void IEC61850_server_timestep_next_step()
{
	global_step++;
	//printf("step: %i\n",global_step);
}

void IEC61850_server_timestep_sync(int local)
{
    while(local >= global_step)
		Thread_sleep(1);
}

int IEC61850_server_timestep_async(int local)
{
    if(local == global_step)
	{
		Thread_sleep(1);
		return 1;
	}
	else
	{
		return 0;
	}
	
}

int IEC61850_server_timestep_type()
{ 
	return global_timestep_type; 
}

void sigint_handler(int signalId)
{
	running = 0;
}

int open_server_running()
{
	return running;
}


int main(int argc, char** argv) {

	IedModel* iedModel_local = NULL;//&iedModel;
	IedModel_extensions* iedExtendedModel_local = NULL;//&iedExtendedModel;

	int port = 102;
	char* ethernetIfcID = "lo";

	if (argc > 1) {
		ethernetIfcID = argv[1];

		printf("Using interface: %s\n", ethernetIfcID);
	}
	if(argc > 2 )
	{
		port = atoi(argv[2]);
	}
	if(argc > 3 )
	{
		iedModel_local = ConfigFileParser_createModelFromConfigFileEx(argv[3]);
		iedExtendedModel_local = ConfigFileParser_createModelFromConfigFileEx_inputs(argv[4],iedModel_local);

		if(iedModel_local == NULL|| iedExtendedModel_local == NULL)
		{
			printf("Parsing dynamic config failed! Exit.\n");
			exit(-1);
		}
	}
	if(argc > 5)
	{
		if(argv[5][0] == 'L')
			global_timestep_type = TIMESTEP_TYPE_LOCAL;
		if(argv[5][0] == 'R')
			global_timestep_type = TIMESTEP_TYPE_REMOTE;
	}
	else
	{
		if(iedModel_local == NULL|| iedExtendedModel_local == NULL)
		{
			printf("No valid model provided! Exit.\n");
			exit(-1);
		}
	}
	

	iedServer = IedServer_create(iedModel_local);

	GooseReceiver GSEreceiver = GooseReceiver_create();
	SVReceiver SMVreceiver = SVReceiver_create();

	/* set GOOSE interface for all GOOSE publishers (GCBs) */
	IedServer_setGooseInterfaceId(iedServer, ethernetIfcID);
	//goose subscriber
	GooseReceiver_setInterfaceId(GSEreceiver, ethernetIfcID);
	//smv subscriber
	SVReceiver_setInterfaceId(SMVreceiver, ethernetIfcID);


	//subscribe to datasets and local DA's based on iput/extRef, and generate one list with all inputValues
	LinkedList allInputValues = subscribeToGOOSEInputs(iedExtendedModel_local, GSEreceiver);
	LinkedList temp = allInputValues;
	temp = LinkedList_getLastElement(temp);
	temp->next = subscribeToSMVInputs(iedExtendedModel_local, SMVreceiver);
	temp = LinkedList_getLastElement(temp);
	temp->next = subscribeToLocalDAInputs(iedExtendedModel_local, iedModel_local,iedServer);

	//start subscribers
    GooseReceiver_start(GSEreceiver);
    SVReceiver_start(SMVreceiver);

    if (GooseReceiver_isRunning(GSEreceiver) || SVReceiver_isRunning(SMVreceiver)) 
	{
		printf("receivers working...\n");
	}
	else
	{
		printf("WARNING: no receivers are running\n");
	}

	/* MMS server will be instructed to start listening to client connections. */
	IedServer_start(iedServer, port);
	running = 1;

	if (!IedServer_isRunning(iedServer)) {
		printf("Starting server failed! Exit.\n");
		IedServer_destroy(iedServer);
		exit(-1);
	}

	//call initializers for sampled value control blocks and start publishing
	iedExtendedModel_local->SMVControlInstances = attachSMV(iedServer, iedModel_local, ethernetIfcID, allInputValues);

	//call all initializers for logical nodes in the model
	attachLogicalNodes(iedServer, iedExtendedModel_local, allInputValues);

	/* Start GOOSE publishing */
	IedServer_enableGoosePublishing(iedServer);

	/* PLUGIN SYSTEM */
	// load all .so in plugin folder, and call init
	// plugins are allowed to call exported functions from main executable

	DIR *d;
	struct dirent *dir;
	d = opendir("./plugin");
	if (d) {
		while ((dir = readdir(d)) != NULL) {
			void * so_handle = NULL;
			lib_init_func  init_func = NULL;

			size_t namelen = strlen(dir->d_name);

			if(dir->d_type != DT_REG || // only regular files
				strcmp(".",dir->d_name) == 0 || strcmp("..",dir->d_name) == 0 || // ignore . and ..
				namelen < 4 || //ensure namelength is large enough to be calles ?.so
				strcmp(".so",dir->d_name + namelen-3) != 0) // check if name ends with .so
			{
				continue;
			}
			printf("loading plugin: %s\n", dir->d_name);

			char * fullname = malloc(namelen + 10);
			strcpy(fullname,"./plugin/");
			strncat(fullname,dir->d_name,namelen + 10);
			so_handle = dlopen(fullname, RTLD_NOW | RTLD_GLOBAL);
			free(fullname);

			if (so_handle == NULL)
			{
				printf("ERROR: Unable to open lib: %s\n", dlerror());
				continue;
			}
			init_func = dlsym(so_handle, "init");

			if (init_func == NULL) {
				printf("ERROR: Unable to get symbol\n");
				continue;
			}
			if(init_func(iedModel_local,iedExtendedModel_local) != 0)
			{
				printf("ERROR: could not succesfully run init of plugin: %s", dir->d_name);
			}

		}
		closedir(d);
	}

	signal(SIGINT, sigint_handler);
	while (running) {
		Thread_sleep(1000);
		fflush(stdout);//ensure logging is flushed every second
	}

	/* stop MMS server - close TCP server socket and all client sockets */
	IedServer_stop(iedServer);

	/* Cleanup - free all resources */
	IedServer_destroy(iedServer);

	GooseReceiver_stop(GSEreceiver);

    GooseReceiver_destroy(GSEreceiver);

	SVReceiver_stop(SMVreceiver);
     /* Cleanup and free resources */
    SVReceiver_destroy(SMVreceiver);
} /* main() */
