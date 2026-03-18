/*
 * ps3recomp - cellNet HLE implementation
 *
 * Network core initialization. On Windows, initializes Winsock.
 * Provides DNS resolver stubs.
 */

#include "cellNet.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <netdb.h>
#include <arpa/inet.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_net_initialized = 0;
static int s_net_pool_initialized = 0;

#define MAX_RESOLVERS 8

typedef struct {
    int in_use;
    u32 resolved_addr;
    int done;
} ResolverSlot;

static ResolverSlot s_resolvers[MAX_RESOLVERS];

/* ---------------------------------------------------------------------------
 * Core network init/term
 *
 * cellNetCtlInit, cellNetCtlTerm are implemented in cellNetCtl.c.
 * sys_net_initialize_network_ex, sys_net_finalize_network are in sysNet.c.
 * We do NOT redefine them here to avoid duplicate symbol errors.
 * -----------------------------------------------------------------------*/

s32 cellNetInitialize(void)
{
    printf("[cellNet] Initialize()\n");

    if (s_net_initialized)
        return (s32)CELL_NET_ERROR_ALREADY_INITIALIZED;

    if (!s_net_pool_initialized)
        sys_net_initialize_network_ex(0);

    memset(s_resolvers, 0, sizeof(s_resolvers));
    s_net_initialized = 1;
    return CELL_OK;
}

s32 cellNetFinalize(void)
{
    printf("[cellNet] Finalize()\n");

    if (!s_net_initialized)
        return (s32)CELL_NET_ERROR_NOT_INITIALIZED;

    s_net_initialized = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * DNS resolver
 * -----------------------------------------------------------------------*/

s32 cellNetResolverCreate(u32* resolver_id)
{
    printf("[cellNet] ResolverCreate()\n");

    if (!resolver_id)
        return (s32)CELL_NET_ERROR_INVALID_ARG;

    for (int i = 0; i < MAX_RESOLVERS; i++) {
        if (!s_resolvers[i].in_use) {
            s_resolvers[i].in_use = 1;
            s_resolvers[i].done = 0;
            s_resolvers[i].resolved_addr = 0;
            *resolver_id = (u32)i;
            return CELL_OK;
        }
    }
    return (s32)CELL_NET_ERROR_NO_MEMORY;
}

s32 cellNetResolverDestroy(u32 resolver_id)
{
    printf("[cellNet] ResolverDestroy(id=%u)\n", resolver_id);

    if (resolver_id >= MAX_RESOLVERS || !s_resolvers[resolver_id].in_use)
        return (s32)CELL_NET_ERROR_INVALID_ARG;

    s_resolvers[resolver_id].in_use = 0;
    return CELL_OK;
}

s32 cellNetResolverStartAsynDNS(u32 resolver_id, const char* hostname,
                                  u32* addr, u32 timeout)
{
    (void)timeout;

    printf("[cellNet] ResolverStartAsynDNS(id=%u, host=%s)\n",
           resolver_id, hostname ? hostname : "(null)");

    if (resolver_id >= MAX_RESOLVERS || !s_resolvers[resolver_id].in_use)
        return (s32)CELL_NET_ERROR_INVALID_ARG;

    if (!hostname || !addr)
        return (s32)CELL_NET_ERROR_INVALID_ARG;

    /* Synchronous DNS lookup using host resolver */
    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(hostname, NULL, &hints, &result);
    if (rc == 0 && result) {
        struct sockaddr_in* sin = (struct sockaddr_in*)result->ai_addr;
        *addr = sin->sin_addr.s_addr;
        s_resolvers[resolver_id].resolved_addr = *addr;
        s_resolvers[resolver_id].done = 1;
        freeaddrinfo(result);

        printf("[cellNet] Resolved %s -> 0x%08X\n", hostname, *addr);
        return CELL_OK;
    }

    printf("[cellNet] DNS lookup failed for %s\n", hostname);
    s_resolvers[resolver_id].done = 1;
    return (s32)CELL_NET_ERROR_INVALID_ARG;
}

s32 cellNetResolverPollDNS(u32 resolver_id, s32* result)
{
    if (resolver_id >= MAX_RESOLVERS || !s_resolvers[resolver_id].in_use)
        return (s32)CELL_NET_ERROR_INVALID_ARG;

    if (result)
        *result = s_resolvers[resolver_id].done ? 0 : 1;

    return CELL_OK;
}
