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

/* Error codes */
#define CELL_HTTPS_ERROR_NOT_INITIALIZED       0x80711101
#define CELL_HTTPS_ERROR_ALREADY_INITIALIZED   0x80711102
#define CELL_HTTPS_ERROR_INVALID_ARGUMENT      0x80711103
#define CELL_HTTPS_ERROR_OUT_OF_MEMORY         0x80711104
#define CELL_HTTPS_ERROR_SSL_FAILURE           0x80711105
#define CELL_HTTPS_ERROR_CERT_VERIFY_FAILED    0x80711106
#define CELL_HTTPS_ERROR_NOT_SUPPORTED         0x80711107

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

/* Functions */
s32 cellHttpsInit(const CellHttpsConfig* config, CellHttpsHandle* handle);
s32 cellHttpsEnd(CellHttpsHandle handle);

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
