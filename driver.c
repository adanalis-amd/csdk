#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include "hip.h"
#include "rocp_csdk.h"

#define NUM_EVENTS (10)

extern int launch_kernel(int device_id);


const char desiredEvents[NUM_EVENTS][128] = { 
"CPF_CMP_UTCL1_STALL_ON_TRANSLATION:DIMENSION_INSTANCE=0:DIMENSION_XCC=0:device=0",
"CPF_CMP_UTCL1_STALL_ON_TRANSLATION:DIMENSION_INSTANCE=0:DIMENSION_XCC=1:device=0",
"CPF_CMP_UTCL1_STALL_ON_TRANSLATION:DIMENSION_INSTANCE=0:DIMENSION_XCC=2:device=0",
"CPF_CMP_UTCL1_STALL_ON_TRANSLATION:DIMENSION_INSTANCE=0:DIMENSION_XCC=3:device=0",
"CPF_CMP_UTCL1_STALL_ON_TRANSLATION:DIMENSION_INSTANCE=0:DIMENSION_XCC=4:device=0",
"CPF_CMP_UTCL1_STALL_ON_TRANSLATION:DIMENSION_INSTANCE=0:DIMENSION_XCC=5:device=0",
"CPF_CMP_UTCL1_STALL_ON_TRANSLATION:DIMENSION_INSTANCE=0:DIMENSION_XCC=6:device=0",
"CPF_CMP_UTCL1_STALL_ON_TRANSLATION:DIMENSION_INSTANCE=0:DIMENSION_XCC=7:device=0",
"CPF_CMP_UTCL1_STALL_ON_TRANSLATION:device=0",
"SQ_WAVES:device=0"
};

const char **gen_event_list(){
    const char **event_list;
    event_list = (const char **)calloc(NUM_EVENTS, sizeof(char *));
    for(int i=0; i<NUM_EVENTS; i++){
        event_list[i] = desiredEvents[i];
    }

    return event_list;
}

volatile int gv=0;

void *thread_main(void *arg){
    long long counters[NUM_EVENTS] = { 0 };
    while(0==gv){;}
    for(int i=0; i<10; i++){
        printf("Sample: %2d\n", gv);
        fflush(stdout);
        rocp_csdk_read(counters);
        for (int i = 0; i <NUM_EVENTS; ++i) {
            printf("%s: %lld\n", desiredEvents[i], counters[i]);
            fflush(stdout);
        }
        printf("\n");
	fflush(stdout);
        usleep(80*1000);
        ++gv;
    }
    return NULL;
}


int main(int argc, char *argv[])
{
    int papi_errno;

    int dev_count;
    if (hipGetDeviceCount(&dev_count) != hipSuccess){
        printf("Error while counting AMD devices\n");
    }

    rocp_csdk_start(gen_event_list(), NUM_EVENTS);

    pthread_t tid;
    pthread_create(&tid, NULL, thread_main, NULL);

    printf("---------------------  launch_kernel(0)\n");
    gv = 1;
    papi_errno = launch_kernel(0);

    usleep(20000);

    rocp_csdk_stop();

    rocp_csdk_shutdown();

    return 0;
}
