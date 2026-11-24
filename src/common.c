#include <time.h>
#include "common.h"

volatile sig_atomic_t g_stop = 0;

double lp_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
