#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include "hip.h"
#include "rocp_csdk.h"

#define NUM_EVENTS (10)
#define SAMPLE_INTERVAL_US (80 * 1000)
#define SHUTDOWN_DELAY_US  (20000)

extern int launch_kernel(int device_id);


const char *desiredEvents[NUM_EVENTS] = {
    "CPF_CMP_UTCL1_STALL_ON_TRANSLATION:DIMENSION_INSTANCE=0:DIMENSION_XCC=0",
    "CPF_CMP_UTCL1_STALL_ON_TRANSLATION:DIMENSION_INSTANCE=0:DIMENSION_XCC=1",
    "CPF_CMP_UTCL1_STALL_ON_TRANSLATION:DIMENSION_INSTANCE=0:DIMENSION_XCC=2",
    "CPF_CMP_UTCL1_STALL_ON_TRANSLATION:DIMENSION_INSTANCE=0:DIMENSION_XCC=3",
    "CPF_CMP_UTCL1_STALL_ON_TRANSLATION:DIMENSION_INSTANCE=0:DIMENSION_XCC=4",
    "CPF_CMP_UTCL1_STALL_ON_TRANSLATION:DIMENSION_INSTANCE=0:DIMENSION_XCC=5",
    "CPF_CMP_UTCL1_STALL_ON_TRANSLATION:DIMENSION_INSTANCE=0:DIMENSION_XCC=6",
    "CPF_CMP_UTCL1_STALL_ON_TRANSLATION:DIMENSION_INSTANCE=0:DIMENSION_XCC=7",
    "CPF_CMP_UTCL1_STALL_ON_TRANSLATION",
    "SQ_WAVES"
};

atomic_int gv = 0;

void *thread_main(void *arg){
    long long counters[NUM_EVENTS] = { 0 };
    while(0 == atomic_load(&gv)){;}
    for(int i=0; i<10; i++){
        printf("Sample: %2d\n", atomic_load(&gv));
        fflush(stdout);
        rocp_csdk_read(counters);
        for (int j = 0; j < NUM_EVENTS; ++j) {
            printf("%s: %lld\n", desiredEvents[j], counters[j]);
            fflush(stdout);
        }
        printf("\n");
        fflush(stdout);
        usleep(SAMPLE_INTERVAL_US);
        atomic_fetch_add(&gv, 1);
    }
    return NULL;
}


int main(int argc, char *argv[])
{
    int dev_count;
    if (hipGetDeviceCount(&dev_count) != hipSuccess){
        fprintf(stderr, "Error while counting AMD devices\n");
        return 1;
    }

    rocp_csdk_start(desiredEvents, NUM_EVENTS);

    pthread_t tid;
    if (pthread_create(&tid, NULL, thread_main, NULL) != 0) {
        perror("pthread_create failed");
        rocp_csdk_shutdown();
        return 1;
    }

    printf("---------------------  launch_kernel(0)\n");
    atomic_store(&gv, 1);
    int kernel_ret = launch_kernel(0);
    if (kernel_ret != 0) {
        fprintf(stderr, "launch_kernel failed with code %d\n", kernel_ret);
    }

    pthread_join(tid, NULL);

    usleep(SHUTDOWN_DELAY_US);

    rocp_csdk_stop();

    rocp_csdk_shutdown();

    return 0;
}
