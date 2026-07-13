/*
 * ps3recomp - cellNetCtl HLE implementation
 *
 * Provides network control state and info queries.  Returns sensible
 * defaults so that games see an active wired network connection.
 */

#include "cellNetCtl.h"
#include "cellSysutil.h"   /* cellSysutilQueueEvent, CELL_SYSUTIL_MAX_CALLBACKS */
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_write32 (guest mem) */
#include "ps3emu/endian.h" /* ps3_bswap32 -- CellNetCtlInfo/NatInfo integer fields are guest big-endian */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* The generic HLE adapter passes GUEST addresses for pointer args; translate to
 * a host pointer (struct out-params) or write scalars big-endian via vm_write32. */
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <ifaddrs.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_netctl_initialized = 0;

/* Offline by default: titles gate their whole online boot path on this state
 * and hang waiting on PSN jobs that can never complete if we claim a live
 * connection. A real console with no link reports Disconnected and games take
 * their offline path (verified against LBP's working RPCS3 boot, where
 * GetState never reported a connection). PS3_NET_ONLINE=1 restores the old
 * fake-connected behaviour for online experiments. */
static int netctl_online(void)
{
    static int v = -1;
    if (v < 0) v = getenv("PS3_NET_ONLINE") ? 1 : 0;
    return v;
}

typedef struct {
    int              in_use;
    cellNetCtlHandler handler;
    void*            arg;
} NetCtlHandlerSlot;

static NetCtlHandlerSlot s_handlers[CELL_NET_CTL_HANDLER_MAX];

/* Try to get the host machine's IP address */
static int netctl_get_host_ip(char* buf, size_t buflen)
{
#ifdef _WIN32
    WSADATA wsa;
    char hostname[256];
    struct addrinfo hints, *res;
    int ret;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        goto fallback;

    if (gethostname(hostname, sizeof(hostname)) != 0) {
        WSACleanup();
        goto fallback;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    ret = getaddrinfo(hostname, NULL, &hints, &res);
    if (ret != 0 || !res) {
        WSACleanup();
        goto fallback;
    }

    {
        struct sockaddr_in* addr = (struct sockaddr_in*)res->ai_addr;
        u8* ip = (u8*)&addr->sin_addr;
        snprintf(buf, buflen, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    }
    freeaddrinfo(res);
    /* Don't WSACleanup here -- we may need it later */
    return 1;
#else
    char hostname[256];
    struct addrinfo hints, *res;
    int ret;

    if (gethostname(hostname, sizeof(hostname)) != 0)
        goto fallback;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    ret = getaddrinfo(hostname, NULL, &hints, &res);
    if (ret != 0 || !res)
        goto fallback;

    {
        struct sockaddr_in* addr = (struct sockaddr_in*)res->ai_addr;
        u8* ip = (u8*)&addr->sin_addr;
        snprintf(buf, buflen, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    }
    freeaddrinfo(res);
    return 1;
#endif

fallback:
    snprintf(buf, buflen, "192.168.1.100");
    return 0;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellNetCtlInit(void)
{
    printf("[cellNetCtl] Init()\n");

    if (s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_TERMINATED;

    memset(s_handlers, 0, sizeof(s_handlers));
    s_netctl_initialized = 1;
    return CELL_OK;
}

void cellNetCtlTerm(void)
{
    printf("[cellNetCtl] Term()\n");

    memset(s_handlers, 0, sizeof(s_handlers));
    s_netctl_initialized = 0;
}

s32 cellNetCtlGetState(s32* state)
{
    if (!s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;

    if (!state)
        return CELL_NET_CTL_ERROR_INVALID_ADDR;

    if (!netctl_online()) {
        /* Offline: FAIL the query rather than report state=Disconnected.
         * LBP's connect job (sub_F433C(0)) polls GetState forever until
         * IPObtained and only exits on ret<0 -- and the working RPCS3 boot
         * shows exactly one GetState call failing with NOT_INITIALIZED,
         * after which the game proceeds offline. Still write a sane state
         * for callers that ignore the return value. */
        vm_write32((uint32_t)(uintptr_t)state, CELL_NET_CTL_STATE_Disconnected);
        printf("[cellNetCtl] GetState() -> error NOT_INITIALIZED (offline mode)\n");
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;
    }

    vm_write32((uint32_t)(uintptr_t)state, CELL_NET_CTL_STATE_IPObtained);
    printf("[cellNetCtl] GetState() -> IPObtained\n");
    return CELL_OK;
}

s32 cellNetCtlGetInfo(s32 code, CellNetCtlInfo* info)
{
    if (!s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;

    if (!info)
        return CELL_NET_CTL_ERROR_INVALID_ADDR;

    info = GUEST_PTR(info, CellNetCtlInfo*);
    memset(info, 0, sizeof(CellNetCtlInfo));

    switch (code)
    {
    case CELL_NET_CTL_INFO_DEVICE:
        info->device = ps3_bswap32((u32)CELL_NET_CTL_DEVICE_WIRED);
        break;

    case CELL_NET_CTL_INFO_ETHER_ADDR:
        /* Fake PS3-like MAC: 00:04:1F:xx:xx:xx (Sony OUI) */
        info->ether_addr.data[0] = 0x00;
        info->ether_addr.data[1] = 0x04;
        info->ether_addr.data[2] = 0x1F;
        info->ether_addr.data[3] = 0xAB;
        info->ether_addr.data[4] = 0xCD;
        info->ether_addr.data[5] = 0xEF;
        break;

    case CELL_NET_CTL_INFO_MTU:
        info->mtu = ps3_bswap32(1500u);
        break;

    case CELL_NET_CTL_INFO_LINK:
        info->link = ps3_bswap32((u32)(netctl_online() ? CELL_NET_CTL_LINK_CONNECTED
                                                       : CELL_NET_CTL_LINK_DISCONNECTED));
        break;

    case CELL_NET_CTL_INFO_LINK_TYPE:
        info->link_type = ps3_bswap32((u32)CELL_NET_CTL_LINK_TYPE_1000BASE_FULL);
        break;

    case CELL_NET_CTL_INFO_IP_ADDRESS:
        netctl_get_host_ip(info->ip_address, sizeof(info->ip_address));
        break;

    case CELL_NET_CTL_INFO_NETMASK:
        strncpy(info->netmask, "255.255.255.0", sizeof(info->netmask) - 1);
        break;

    case CELL_NET_CTL_INFO_DEFAULT_ROUTE:
        strncpy(info->default_route, "192.168.1.1",
                sizeof(info->default_route) - 1);
        break;

    case CELL_NET_CTL_INFO_PRIMARY_DNS:
        strncpy(info->primary_dns, "8.8.8.8", sizeof(info->primary_dns) - 1);
        break;

    case CELL_NET_CTL_INFO_SECONDARY_DNS:
        strncpy(info->secondary_dns, "8.8.4.4",
                sizeof(info->secondary_dns) - 1);
        break;

    case CELL_NET_CTL_INFO_IP_CONFIG:
        info->ip_config = 0; /* DHCP */
        break;

    case CELL_NET_CTL_INFO_HTTP_PROXY_CONFIG:
        info->http_proxy_config = 0; /* disabled */
        break;

    case CELL_NET_CTL_INFO_UPNP_CONFIG:
        info->upnp_config = ps3_bswap32(1u); /* enabled */
        break;

    default:
        printf("[cellNetCtl] GetInfo(code=%d) - unknown code\n", code);
        return CELL_NET_CTL_ERROR_INVALID_CODE;
    }

    printf("[cellNetCtl] GetInfo(code=%d) -> OK\n", code);
    return CELL_OK;
}

s32 cellNetCtlGetNatInfo(CellNetCtlNatInfo* natInfo)
{
    if (!s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;

    if (!natInfo)
        return CELL_NET_CTL_ERROR_INVALID_ADDR;

    natInfo = GUEST_PTR(natInfo, CellNetCtlNatInfo*);
    natInfo->size        = ps3_bswap32((u32)sizeof(CellNetCtlNatInfo));
    natInfo->nat_type    = ps3_bswap32((u32)CELL_NET_CTL_NATINFO_NAT_TYPE_2); /* moderate */
    natInfo->stun_status = 0;
    natInfo->upnp_status = 0;

    printf("[cellNetCtl] GetNatInfo() -> NAT Type 2\n");
    return CELL_OK;
}

s32 cellNetCtlAddHandler(cellNetCtlHandler handler, void* arg, s32* hid)
{
    if (!s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;

    if (!handler || !hid)
        return CELL_NET_CTL_ERROR_INVALID_ADDR;

    for (s32 i = 0; i < CELL_NET_CTL_HANDLER_MAX; i++) {
        if (!s_handlers[i].in_use) {
            s_handlers[i].in_use  = 1;
            s_handlers[i].handler = handler;
            s_handlers[i].arg     = arg;
            vm_write32((uint32_t)(uintptr_t)hid, (uint32_t)i);   /* guest out-param */
            printf("[cellNetCtl] AddHandler(hid=%d)\n", i);
            return CELL_OK;
        }
    }

    return CELL_NET_CTL_ERROR_HANDLER_MAX;
}

s32 cellNetCtlDelHandler(s32 hid)
{
    if (!s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;

    if (hid < 0 || hid >= CELL_NET_CTL_HANDLER_MAX)
        return CELL_NET_CTL_ERROR_INVALID_ID;

    if (!s_handlers[hid].in_use)
        return CELL_NET_CTL_ERROR_ID_NOT_FOUND;

    s_handlers[hid].in_use  = 0;
    s_handlers[hid].handler = NULL;
    s_handlers[hid].arg     = NULL;

    printf("[cellNetCtl] DelHandler(hid=%d)\n", hid);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Net-start ("connect to PSN") dialog
 *
 * Real hardware shows a system dialog and signals progress asynchronously
 * through the cellSysutil callback queue (LOADED -> FINISHED), after which the
 * game calls UnloadAsync to retrieve the result. In an offline native recomp
 * there is nothing to show and we are already "connected", so we immediately
 * post LOADED + FINISHED to every registered sysutil callback slot and report a
 * successful result on unload. This unblocks the common boot-time "connecting
 * to network" gate without requiring a real PSN session.
 * -----------------------------------------------------------------------*/

static void netstart_broadcast(u32 status)
{
    /* Sysutil events target a specific slot; the game may have registered its
     * callback on any of them, so notify all and let it filter by status. */
    for (int slot = 0; slot < CELL_SYSUTIL_MAX_CALLBACKS; ++slot)
        cellSysutilQueueEvent(slot, status, 0);
}

s32 cellNetCtlNetStartDialogLoadAsync(const CellNetCtlNetStartDialogParam* param)
{
    (void)param;
    printf("[cellNetCtl] NetStartDialogLoadAsync() -> auto-connect\n");
    netstart_broadcast(CELL_SYSUTIL_NET_CTL_NETSTART_LOADED);
    netstart_broadcast(CELL_SYSUTIL_NET_CTL_NETSTART_FINISHED);
    return CELL_OK;
}

s32 cellNetCtlNetStartDialogAbortAsync(void)
{
    printf("[cellNetCtl] NetStartDialogAbortAsync()\n");
    return CELL_OK;
}

s32 cellNetCtlNetStartDialogUnloadAsync(CellNetCtlNetStartDialogResult* result)
{
    if (result) {
        result = GUEST_PTR(result, CellNetCtlNetStartDialogResult*);
        result->result = 0;   /* 0 = connected; endian-safe */
    }
    printf("[cellNetCtl] NetStartDialogUnloadAsync() -> result=0 (connected)\n");
    netstart_broadcast(CELL_SYSUTIL_NET_CTL_NETSTART_UNLOADED);
    return CELL_OK;
}
