/*
 * ps3recomp - cellSsl HLE
 *
 * SSL/TLS stub. Provides the API surface for cellHttp's HTTPS support.
 * Actual TLS would require OpenSSL/mbedTLS integration.
 */

#ifndef PS3RECOMP_CELL_SSL_H
#define PS3RECOMP_CELL_SSL_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes (PS3 cell/ssl.h, base 0x80740000; verified against RPCS3)
 * -----------------------------------------------------------------------*/
#define CELL_SSL_ERROR_NOT_INITIALIZED       (CELL_ERROR_BASE_SSL | 0x01) /* 0x80740001 */
#define CELL_SSL_ERROR_ALREADY_INITIALIZED   (CELL_ERROR_BASE_SSL | 0x02) /* 0x80740002 */
#define CELL_SSL_ERROR_NO_MEMORY             (CELL_ERROR_BASE_SSL | 0x03) /* 0x80740003 */
#define CELL_SSL_ERROR_INVALID_ARG           (CELL_ERROR_BASE_SSL | 0x04) /* 0x80740004 */
#define CELL_SSL_ERROR_HANDSHAKE_FAILED      (CELL_ERROR_BASE_SSL | 0x05)
#define CELL_SSL_ERROR_NOT_CONNECTED         (CELL_ERROR_BASE_SSL | 0x06)

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/
typedef u32 CellSslCertId;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellSslInit(void* pool, u32 poolSize);
s32 cellSslEnd(void);

/* Certificate management */
s32 cellSslCertificateLoader(u64 flags, char* buffer, u32 size, u32* required);
s32 cellSslCertGetSerialNumber(CellSslCertId certId, u8* serial, u32* serialSize);
s32 cellSslCertGetPublicKey(CellSslCertId certId, u8* key, u32* keySize);
s32 cellSslCertGetNotBefore(CellSslCertId certId, u64* time);
s32 cellSslCertGetNotAfter(CellSslCertId certId, u64* time);

/* Entropy / RNG */
s32 cellSslGetRandomNumber(u8* buf, u32 size);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SSL_H */
