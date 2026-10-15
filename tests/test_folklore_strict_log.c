/* Synthetic concurrent-output child. Parent verifies one complete fatal line. */
#include "ps3emu/strict_boot.h"
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
static atomic_uint fragments=0;
#ifdef _WIN32
static DWORD WINAPI noise(void* argument)
#else
static void* noise(void* argument)
#endif
{
    (void)argument;
    for (;;) {
        fputs("competing-output ",stdout);
        atomic_fetch_add(&fragments,1);
    }
    return 0;
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
#ifdef _WIN32
    HANDLE thread=CreateThread(NULL,0,noise,NULL,0,NULL);
    if (!thread) return 2;
#else
    pthread_t thread;
    if (pthread_create(&thread,NULL,noise,NULL)) return 2;
#endif
    while (atomic_load(&fragments)<64) {}
    ps3_strict_boot_guard("synthetic-concurrent-log", "method=0x%04X data=0x%08X",0x1d88,0x12d0);
    return 3;
}
