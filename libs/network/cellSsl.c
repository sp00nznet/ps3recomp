/*
 * ps3recomp - cellSsl HLE implementation
 *
 * Stub SSL module. Init/End lifecycle works, certificate functions
 * return empty data, and RNG uses host OS entropy.
 */

#include "cellSsl.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_ssl_initialized = 0;

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellSslInit(void* pool, u32 poolSize)
{
    (void)pool;
    (void)poolSize;
    printf("[cellSsl] Init(poolSize=%u)\n", poolSize);

    if (s_ssl_initialized)
        return (s32)CELL_SSL_ERROR_ALREADY_INITIALIZED;

    s_ssl_initialized = 1;
    return CELL_OK;
}

s32 cellSslEnd(void)
{
    printf("[cellSsl] End()\n");

    if (!s_ssl_initialized)
        return (s32)CELL_SSL_ERROR_NOT_INITIALIZED;

    s_ssl_initialized = 0;
    return CELL_OK;
}

s32 cellSslCertificateLoader(u32 flags)
{
    (void)flags;
    printf("[cellSsl] CertificateLoader(flags=0x%X)\n", flags);
    return CELL_OK;
}

s32 cellSslCertGetSerialNumber(CellSslCertId certId, u8* serial, u32* serialSize)
{
    (void)certId;
    printf("[cellSsl] CertGetSerialNumber()\n");

    if (!serial || !serialSize)
        return (s32)CELL_SSL_ERROR_INVALID_ARG;

    /* Fake serial number */
    if (*serialSize >= 4) {
        serial[0] = 0x01;
        serial[1] = 0x00;
        serial[2] = 0x00;
        serial[3] = 0x01;
        *serialSize = 4;
    }
    return CELL_OK;
}

s32 cellSslCertGetPublicKey(CellSslCertId certId, u8* key, u32* keySize)
{
    (void)certId;
    (void)key;
    printf("[cellSsl] CertGetPublicKey()\n");

    if (keySize)
        *keySize = 0;

    return CELL_OK;
}

s32 cellSslCertGetNotBefore(CellSslCertId certId, u64* time)
{
    (void)certId;
    if (!time) return (s32)CELL_SSL_ERROR_INVALID_ARG;
    *time = 0;
    return CELL_OK;
}

s32 cellSslCertGetNotAfter(CellSslCertId certId, u64* time)
{
    (void)certId;
    if (!time) return (s32)CELL_SSL_ERROR_INVALID_ARG;
    *time = 0xFFFFFFFFFFFFFFFFULL;
    return CELL_OK;
}

s32 cellSslGetRandomNumber(u8* buf, u32 size)
{
    if (!buf || size == 0)
        return (s32)CELL_SSL_ERROR_INVALID_ARG;

#ifdef _WIN32
    NTSTATUS status = BCryptGenRandom(NULL, buf, size,
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        /* Fallback: fill with pseudo-random */
        for (u32 i = 0; i < size; i++)
            buf[i] = (u8)(i * 37 + 13);
    }
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, buf, size);
        close(fd);
    } else {
        for (u32 i = 0; i < size; i++)
            buf[i] = (u8)(i * 37 + 13);
    }
#endif

    return CELL_OK;
}
