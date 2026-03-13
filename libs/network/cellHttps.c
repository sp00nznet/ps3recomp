/*
 * ps3recomp - cellHttps HLE implementation
 *
 * HTTPS (HTTP over TLS) client. Extends cellHttp with SSL/TLS support.
 * Stub — no actual TLS handshake or encryption. Certificate management
 * APIs accept and store data but verification always succeeds.
 * Games that only check init/end lifecycle will work; actual encrypted
 * connections require a real TLS library (OpenSSL, mbedTLS, etc.).
 */

#include "cellHttps.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define HTTPS_MAX_HANDLES 8

typedef struct {
    int  in_use;
    u32  sslVersion;
    u32  verifyPeer;
    u32  verifyHost;
    int  hasCACert;
    int  hasClientCert;
} HttpsSlot;

static HttpsSlot s_slots[HTTPS_MAX_HANDLES];
static int s_https_initialized = 0;

static int https_alloc(void)
{
    for (int i = 0; i < HTTPS_MAX_HANDLES; i++) {
        if (!s_slots[i].in_use) return i;
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * API
 * -----------------------------------------------------------------------*/

s32 cellHttpsInit(const CellHttpsConfig* config, CellHttpsHandle* handle)
{
    printf("[cellHttps] Init()\n");

    if (!handle) return (s32)CELL_HTTPS_ERROR_INVALID_ARGUMENT;

    int slot = https_alloc();
    if (slot < 0) return (s32)CELL_HTTPS_ERROR_OUT_OF_MEMORY;

    s_slots[slot].in_use = 1;
    s_slots[slot].hasCACert = 0;
    s_slots[slot].hasClientCert = 0;

    if (config) {
        s_slots[slot].sslVersion = config->sslVersion;
        s_slots[slot].verifyPeer = config->verifyPeer;
        s_slots[slot].verifyHost = config->verifyHost;
    } else {
        s_slots[slot].sslVersion = CELL_HTTPS_SSLVERSION_TLSV1;
        s_slots[slot].verifyPeer = 1;
        s_slots[slot].verifyHost = 1;
    }

    *handle = (CellHttpsHandle)slot;
    s_https_initialized = 1;
    return CELL_OK;
}

s32 cellHttpsEnd(CellHttpsHandle handle)
{
    printf("[cellHttps] End(%u)\n", handle);

    if (handle >= HTTPS_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_HTTPS_ERROR_INVALID_ARGUMENT;

    memset(&s_slots[handle], 0, sizeof(HttpsSlot));
    return CELL_OK;
}

s32 cellHttpsSetCACert(CellHttpsHandle handle, const void* cert,
                       u32 certSize, u32 certType)
{
    (void)cert; (void)certSize;
    printf("[cellHttps] SetCACert(handle=%u, size=%u, type=%u)\n",
           handle, certSize, certType);

    if (handle >= HTTPS_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_HTTPS_ERROR_INVALID_ARGUMENT;

    if (certType != CELL_HTTPS_CERT_TYPE_X509 &&
        certType != CELL_HTTPS_CERT_TYPE_PKCS12)
        return (s32)CELL_HTTPS_ERROR_INVALID_ARGUMENT;

    /* Accept cert data but don't actually process it */
    s_slots[handle].hasCACert = 1;
    return CELL_OK;
}

s32 cellHttpsSetClientCert(CellHttpsHandle handle, const void* cert,
                           u32 certSize, const void* key, u32 keySize)
{
    (void)cert; (void)certSize; (void)key; (void)keySize;
    printf("[cellHttps] SetClientCert(handle=%u)\n", handle);

    if (handle >= HTTPS_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_HTTPS_ERROR_INVALID_ARGUMENT;

    s_slots[handle].hasClientCert = 1;
    return CELL_OK;
}

s32 cellHttpsClearCerts(CellHttpsHandle handle)
{
    printf("[cellHttps] ClearCerts(handle=%u)\n", handle);

    if (handle >= HTTPS_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_HTTPS_ERROR_INVALID_ARGUMENT;

    s_slots[handle].hasCACert = 0;
    s_slots[handle].hasClientCert = 0;
    return CELL_OK;
}

s32 cellHttpsSetVerifyLevel(CellHttpsHandle handle, u32 verifyPeer, u32 verifyHost)
{
    printf("[cellHttps] SetVerifyLevel(handle=%u, peer=%u, host=%u)\n",
           handle, verifyPeer, verifyHost);

    if (handle >= HTTPS_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_HTTPS_ERROR_INVALID_ARGUMENT;

    s_slots[handle].verifyPeer = verifyPeer;
    s_slots[handle].verifyHost = verifyHost;
    return CELL_OK;
}

s32 cellHttpsGetCertInfo(CellHttpsHandle handle, CellHttpsCertInfo* info)
{
    printf("[cellHttps] GetCertInfo(handle=%u)\n", handle);

    if (handle >= HTTPS_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_HTTPS_ERROR_INVALID_ARGUMENT;
    if (!info) return (s32)CELL_HTTPS_ERROR_INVALID_ARGUMENT;

    /* No real TLS connection, so no certificate to report */
    memset(info, 0, sizeof(*info));
    return (s32)CELL_HTTPS_ERROR_NOT_SUPPORTED;
}
