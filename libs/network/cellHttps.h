/*
 * ps3recomp - cellHttps HLE
 *
 * HTTPS (HTTP over TLS) client. Extends cellHttp with SSL/TLS support.
 * Stub — init/end lifecycle, certificate management, transactions
 * fall back to plain HTTP (no TLS without a crypto library).
 */

#ifndef PS3RECOMP_CELL_HTTPS_H
#define PS3RECOMP_CELL_HTTPS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes. cellHttps shares libhttp.sprx's CELL_ERROR_BASE_HTTP (0x80710000):
 * firmware 4.93 cellHttpsEnd() before init -> 0x80710002. The other values
 * reuse the matching cellHttp.h codes; SSL_FAILURE / CERT_VERIFY_FAILED /
 * NOT_SUPPORTED are HLE-local placeholders, not measured. */
#define CELL_HTTPS_ERROR_ALREADY_INITIALIZED   (CELL_ERROR_BASE_HTTP | 0x01) /* 0x80710001 */
#define CELL_HTTPS_ERROR_NOT_INITIALIZED       (CELL_ERROR_BASE_HTTP | 0x02) /* 0x80710002 (measured) */
#define CELL_HTTPS_ERROR_OUT_OF_MEMORY         (CELL_ERROR_BASE_HTTP | 0x03) /* = CELL_HTTP_ERROR_NO_MEMORY */
#define CELL_HTTPS_ERROR_INVALID_ARGUMENT      (CELL_ERROR_BASE_HTTP | 0x05) /* = CELL_HTTP_ERROR_INVALID_PARAMETER */
#define CELL_HTTPS_ERROR_SSL_FAILURE           (CELL_ERROR_BASE_HTTP | 0x30)
#define CELL_HTTPS_ERROR_CERT_VERIFY_FAILED    (CELL_ERROR_BASE_HTTP | 0x31)
#define CELL_HTTPS_ERROR_NOT_SUPPORTED         (CELL_ERROR_BASE_HTTP | 0x32)

/* SSL version */
#define CELL_HTTPS_SSLVERSION_SSLV3    0
#define CELL_HTTPS_SSLVERSION_TLSV1    1

/* Certificate type */
#define CELL_HTTPS_CERT_TYPE_X509      0
#define CELL_HTTPS_CERT_TYPE_PKCS12    1

/* Types */
typedef u32 CellHttpsHandle;
typedef u32 CellHttpsCertHandle;

typedef struct CellHttpsConfig {
    u32 sslVersion;
    u32 verifyPeer;
    u32 verifyHost;
    void* caCertPool;
    u32 caCertPoolSize;
    u32 reserved[4];
} CellHttpsConfig;

typedef struct CellHttpsCertInfo {
    u32 type;
    u32 serialLen;
    const u8* serial;
    const char* issuer;
    const char* subject;
    u64 notBefore;
    u64 notAfter;
} CellHttpsCertInfo;

/* Mirrors RPCS3's CellHttpsData: one CA-cert blob {ptr, size} in a caList array. */
typedef struct CellHttpsData {
    char* ptr;
    u32   size;
} CellHttpsData;

/* Functions */
s32 cellHttpsInit(u32 caCertNum, const CellHttpsData* caList);
s32 cellHttpsEnd(void);

s32 cellHttpsSetCACert(CellHttpsHandle handle, const void* cert,
                       u32 certSize, u32 certType);
s32 cellHttpsSetClientCert(CellHttpsHandle handle, const void* cert,
                           u32 certSize, const void* key, u32 keySize);
s32 cellHttpsClearCerts(CellHttpsHandle handle);

s32 cellHttpsSetVerifyLevel(CellHttpsHandle handle, u32 verifyPeer, u32 verifyHost);
s32 cellHttpsGetCertInfo(CellHttpsHandle handle, CellHttpsCertInfo* info);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_HTTPS_H */
