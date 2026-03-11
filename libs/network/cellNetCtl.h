/*
 * ps3recomp - cellNetCtl HLE
 *
 * Network control: connection state, network info, NAT detection.
 */

#ifndef PS3RECOMP_CELL_NETCTL_H
#define PS3RECOMP_CELL_NETCTL_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_NET_CTL_ERROR_NOT_INITIALIZED      0x80130101
#define CELL_NET_CTL_ERROR_NOT_TERMINATED       0x80130102
#define CELL_NET_CTL_ERROR_HANDLER_MAX          0x80130103
#define CELL_NET_CTL_ERROR_ID_NOT_FOUND         0x80130104
#define CELL_NET_CTL_ERROR_INVALID_ID           0x80130105
#define CELL_NET_CTL_ERROR_INVALID_CODE         0x80130106
#define CELL_NET_CTL_ERROR_INVALID_ADDR         0x80130107
#define CELL_NET_CTL_ERROR_NOT_CONNECTED        0x80130108
#define CELL_NET_CTL_ERROR_NOT_AVAIL            0x80130109
#define CELL_NET_CTL_ERROR_INVALID_TYPE         0x8013010A
#define CELL_NET_CTL_ERROR_INVALID_SIZE         0x8013010B
#define CELL_NET_CTL_ERROR_DIALOG_CANCELED      0x80130190
#define CELL_NET_CTL_ERROR_DIALOG_ABORTED       0x80130191

/* ---------------------------------------------------------------------------
 * Connection state
 * -----------------------------------------------------------------------*/
#define CELL_NET_CTL_STATE_Disconnected     0
#define CELL_NET_CTL_STATE_Connecting        1
#define CELL_NET_CTL_STATE_IPObtaining       2
#define CELL_NET_CTL_STATE_IPObtained        3

/* ---------------------------------------------------------------------------
 * NAT types
 * -----------------------------------------------------------------------*/
#define CELL_NET_CTL_NATINFO_NAT_TYPE_1     1   /* Open */
#define CELL_NET_CTL_NATINFO_NAT_TYPE_2     2   /* Moderate */
#define CELL_NET_CTL_NATINFO_NAT_TYPE_3     3   /* Strict */

/* ---------------------------------------------------------------------------
 * Info codes (for cellNetCtlGetInfo)
 * -----------------------------------------------------------------------*/
#define CELL_NET_CTL_INFO_DEVICE            1
#define CELL_NET_CTL_INFO_ETHER_ADDR        2
#define CELL_NET_CTL_INFO_MTU               3
#define CELL_NET_CTL_INFO_LINK              4
#define CELL_NET_CTL_INFO_LINK_TYPE         5
#define CELL_NET_CTL_INFO_BSSID             6
#define CELL_NET_CTL_INFO_SSID              7
#define CELL_NET_CTL_INFO_WLAN_SECURITY     8
#define CELL_NET_CTL_INFO_8021X_TYPE        9
#define CELL_NET_CTL_INFO_8021X_AUTH_NAME   10
#define CELL_NET_CTL_INFO_RSSI              11
#define CELL_NET_CTL_INFO_CHANNEL           12
#define CELL_NET_CTL_INFO_IP_CONFIG         13
#define CELL_NET_CTL_INFO_DHCP_HOSTNAME     14
#define CELL_NET_CTL_INFO_PPPOE_AUTH_NAME   15
#define CELL_NET_CTL_INFO_IP_ADDRESS        16
#define CELL_NET_CTL_INFO_NETMASK           17
#define CELL_NET_CTL_INFO_DEFAULT_ROUTE     18
#define CELL_NET_CTL_INFO_PRIMARY_DNS       19
#define CELL_NET_CTL_INFO_SECONDARY_DNS     20
#define CELL_NET_CTL_INFO_HTTP_PROXY_CONFIG 21
#define CELL_NET_CTL_INFO_HTTP_PROXY_SERVER 22
#define CELL_NET_CTL_INFO_HTTP_PROXY_PORT   23
#define CELL_NET_CTL_INFO_UPNP_CONFIG       24

/* Device types */
#define CELL_NET_CTL_DEVICE_WIRED           0
#define CELL_NET_CTL_DEVICE_WIRELESS        1

/* Link states */
#define CELL_NET_CTL_LINK_DISCONNECTED      0
#define CELL_NET_CTL_LINK_CONNECTED         1

/* Link types */
#define CELL_NET_CTL_LINK_TYPE_AUTO             0
#define CELL_NET_CTL_LINK_TYPE_10BASE_HALF      1
#define CELL_NET_CTL_LINK_TYPE_10BASE_FULL      2
#define CELL_NET_CTL_LINK_TYPE_100BASE_HALF     3
#define CELL_NET_CTL_LINK_TYPE_100BASE_FULL     4
#define CELL_NET_CTL_LINK_TYPE_1000BASE_FULL    5

/* Max handler callbacks */
#define CELL_NET_CTL_HANDLER_MAX            3

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

typedef struct CellNetCtlEtherAddr {
    u8 data[6];
    u8 padding[2];
} CellNetCtlEtherAddr;

typedef struct CellNetCtlSSID {
    u8 data[32];
    u8 term;
    u8 padding[3];
} CellNetCtlSSID;

typedef union CellNetCtlInfo {
    u32                 device;
    CellNetCtlEtherAddr ether_addr;
    u32                 mtu;
    u32                 link;
    u32                 link_type;
    CellNetCtlEtherAddr bssid;
    CellNetCtlSSID      ssid;
    u32                 wlan_security;
    u32                 auth_8021x_type;
    char                auth_8021x_auth_name[128];
    u8                  rssi;
    u8                  channel;
    u32                 ip_config;
    char                dhcp_hostname[256];
    char                pppoe_auth_name[128];
    char                ip_address[16];
    char                netmask[16];
    char                default_route[16];
    char                primary_dns[16];
    char                secondary_dns[16];
    u32                 http_proxy_config;
    char                http_proxy_server[256];
    u16                 http_proxy_port;
    u32                 upnp_config;
} CellNetCtlInfo;

typedef struct CellNetCtlNatInfo {
    u32 size;
    u32 nat_type;
    u32 stun_status;
    u32 upnp_status;
} CellNetCtlNatInfo;

/* Event handler callback */
typedef void (*cellNetCtlHandler)(s32 prev_state, s32 new_state,
                                  s32 event, s32 error_code, void* arg);

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellNetCtlInit(void);
s32 cellNetCtlTerm(void);

s32 cellNetCtlGetState(s32* state);
s32 cellNetCtlGetInfo(s32 code, CellNetCtlInfo* info);
s32 cellNetCtlGetNatInfo(CellNetCtlNatInfo* natInfo);

s32 cellNetCtlAddHandler(cellNetCtlHandler handler, void* arg, s32* hid);
s32 cellNetCtlDelHandler(s32 hid);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_NETCTL_H */
