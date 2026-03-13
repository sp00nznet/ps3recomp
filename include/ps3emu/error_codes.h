/*
 * ps3recomp - PS3 / CellOS error codes
 *
 * Based on the Cell OS Lv-2 specification.  All codes are negative 32-bit
 * integers except CELL_OK (0).  Module-specific error bases are defined at
 * the bottom of this file.
 */

#ifndef PS3EMU_ERROR_CODES_H
#define PS3EMU_ERROR_CODES_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Generic system error codes  (0x8001_xxxx)
 * -----------------------------------------------------------------------*/
#define CELL_OK                         0x00000000

#define CELL_EAGAIN                     0x80010001  /* Resource temporarily unavailable */
#define CELL_EINVAL                     0x80010002  /* Invalid argument */
#define CELL_ENOSYS                     0x80010003  /* Function not implemented */
#define CELL_ENOMEM                     0x80010004  /* Not enough memory */
#define CELL_ESRCH                      0x80010005  /* No such entity */
#define CELL_ENOENT                     0x80010006  /* No such file or directory */
#define CELL_ENOEXEC                    0x80010007  /* Exec format error */
#define CELL_EDEADLK                    0x80010008  /* Resource deadlock avoided */
#define CELL_EPERM                      0x80010009  /* Not permitted */
#define CELL_EBUSY                      0x8001000A  /* Device or resource busy */
#define CELL_ETIMEDOUT                  0x8001000B  /* Connection timed out */
#define CELL_EABORT                     0x8001000C  /* Operation aborted */
#define CELL_EFAULT                     0x8001000D  /* Bad address */
#define CELL_ESTAT                      0x8001000F  /* State error */
#define CELL_EALIGN                     0x80010010  /* Alignment error */
#define CELL_EKRESOURCE                 0x80010011  /* Kernel resource shortage */
#define CELL_EISDIR                     0x80010012  /* Is a directory */
#define CELL_ECANCELED                  0x80010013  /* Operation canceled */
#define CELL_EEXIST                     0x80010014  /* Already exists */
#define CELL_EISCONN                    0x80010015  /* Already connected */
#define CELL_ENOTCONN                   0x80010016  /* Not connected */
#define CELL_EAUTHFAIL                  0x80010017  /* Authentication failed */
#define CELL_ENOTMSELF                  0x80010018  /* Not a valid MSELF */
#define CELL_ESYSVER                    0x80010019  /* System version error */
#define CELL_EAUTHDENIED                0x80010020  /* Authorization denied */
#define CELL_EOPNOTSUPP                 0x80010021  /* Operation not supported */
#define CELL_ENOTDIR                    0x80010022  /* Not a directory */
#define CELL_ERANGE                     0x80010023  /* Result out of range */
#define CELL_ENAMETOOLONG               0x80010024  /* Name too long */
#define CELL_EBADF                      0x80010025  /* Bad file descriptor */
#define CELL_EFSSPECIFIC                0x80010025  /* FS-specific error (alias) */
#define CELL_EOVERFLOW                  0x80010026  /* Overflow */
#define CELL_ENOTMOUNTED                0x80010027  /* Not mounted */
#define CELL_ENOTSUP                    0x80010028  /* Not supported */
#define CELL_ENOTSDATA                  0x80010029  /* Not SDATA */
#define CELL_ESDKVER                    0x80010030  /* SDK version error */
#define CELL_ENOLICDISC                 0x80010031  /* No license disc */
#define CELL_ENOLICENT                  0x80010032  /* No license entitlement */

/* ---------------------------------------------------------------------------
 * Mutex / synchronization errors  (0x80010100-range)
 * -----------------------------------------------------------------------*/
#define CELL_EMUTEX_NOT_OWNED           0x80010120
#define CELL_EMUTEX_LOCK_NOT_ALLOWED    0x80010121
#define CELL_EMUTEX_UNLOCK_NOT_OWNED    0x80010122
#define CELL_EMUTEX_DEADLK              0x80010123

#define CELL_ECOND_NOT_OWNED            0x80010130
#define CELL_ECOND_DEADLK               0x80010131

/* ---------------------------------------------------------------------------
 * Module-specific error bases
 *
 * Each module defines errors as (base | local_code).
 * Convention:  0x8002_XXYY  where XX = module index.
 * -----------------------------------------------------------------------*/
#define CELL_ERROR_BASE_SYSMODULE       0x80012000
#define CELL_ERROR_BASE_FS              0x80010700
#define CELL_ERROR_BASE_AUDIO           0x80310700
#define CELL_ERROR_BASE_VIDEO           0x8002B100
#define CELL_ERROR_BASE_GCM             0x80210700
#define CELL_ERROR_BASE_NET             0x80130100
#define CELL_ERROR_BASE_PAD             0x80121100
#define CELL_ERROR_BASE_KB              0x80121200
#define CELL_ERROR_BASE_MOUSE           0x80121300
#define CELL_ERROR_BASE_SYSUTIL         0x8002B000
#define CELL_ERROR_BASE_SYSUTIL_SAVE    0x8002B400
#define CELL_ERROR_BASE_SYSUTIL_GAME    0x8002B600
#define CELL_ERROR_BASE_PNG             0x80611200
#define CELL_ERROR_BASE_JPG             0x80611300
#define CELL_ERROR_BASE_FONT            0x80540000
#define CELL_ERROR_BASE_SPURS           0x80410700
#define CELL_ERROR_BASE_FIBER           0x80410800
#define CELL_ERROR_BASE_SYNC            0x80410100
#define CELL_ERROR_BASE_USBD            0x80110100
#define CELL_ERROR_BASE_HTTP            0x80710100
#define CELL_ERROR_BASE_SSL             0x80720100
#define CELL_ERROR_BASE_VDEC            0x80610100
#define CELL_ERROR_BASE_ADEC            0x80610200

/* sysmodule convenience errors */
#define CELL_SYSMODULE_ERROR_DUPLICATED     (CELL_ERROR_BASE_SYSMODULE | 0x01)
#define CELL_SYSMODULE_ERROR_UNKNOWN        (CELL_ERROR_BASE_SYSMODULE | 0x02)
#define CELL_SYSMODULE_ERROR_UNLOADED       (CELL_ERROR_BASE_SYSMODULE | 0x03)
#define CELL_SYSMODULE_ERROR_FATAL          (CELL_ERROR_BASE_SYSMODULE | 0xFF)

/* sysutil errors */
#define CELL_SYSUTIL_ERROR_TYPE             (CELL_ERROR_BASE_SYSUTIL | 0x01)
#define CELL_SYSUTIL_ERROR_VALUE            (CELL_ERROR_BASE_SYSUTIL | 0x02)
#define CELL_SYSUTIL_ERROR_SIZE             (CELL_ERROR_BASE_SYSUTIL | 0x03)
#define CELL_SYSUTIL_ERROR_NUM              (CELL_ERROR_BASE_SYSUTIL | 0x04)

/* pad errors */
#define CELL_PAD_ERROR_FATAL                (CELL_ERROR_BASE_PAD | 0x01)
#define CELL_PAD_ERROR_INVALID_PARAMETER    (CELL_ERROR_BASE_PAD | 0x02)
#define CELL_PAD_ERROR_ALREADY_OPENED       (CELL_ERROR_BASE_PAD | 0x03)
#define CELL_PAD_ERROR_NOT_OPENED           (CELL_ERROR_BASE_PAD | 0x04)
#define CELL_PAD_ERROR_NO_DEVICE            (CELL_ERROR_BASE_PAD | 0x05)

/* audio errors */
#define CELL_AUDIO_ERROR_ALREADY_INIT       (CELL_ERROR_BASE_AUDIO | 0x01)
#define CELL_AUDIO_ERROR_AUDIOSYSTEM        (CELL_ERROR_BASE_AUDIO | 0x02)
#define CELL_AUDIO_ERROR_NOT_INIT           (CELL_ERROR_BASE_AUDIO | 0x03)
#define CELL_AUDIO_ERROR_PARAM              (CELL_ERROR_BASE_AUDIO | 0x04)
#define CELL_AUDIO_ERROR_PORT_FULL          (CELL_ERROR_BASE_AUDIO | 0x05)
#define CELL_AUDIO_ERROR_PORT_ALREADY_RUN   (CELL_ERROR_BASE_AUDIO | 0x06)
#define CELL_AUDIO_ERROR_PORT_NOT_OPEN      (CELL_ERROR_BASE_AUDIO | 0x07)
#define CELL_AUDIO_ERROR_PORT_NOT_RUN       (CELL_ERROR_BASE_AUDIO | 0x08)

/* GCM errors */
#define CELL_GCM_ERROR_FAILURE              (CELL_ERROR_BASE_GCM | 0xFF)
#define CELL_GCM_ERROR_NO_IO_PAGE_TABLE     (CELL_ERROR_BASE_GCM | 0x01)
#define CELL_GCM_ERROR_INVALID_ENUM         (CELL_ERROR_BASE_GCM | 0x02)
#define CELL_GCM_ERROR_INVALID_VALUE        (CELL_ERROR_BASE_GCM | 0x03)
#define CELL_GCM_ERROR_INVALID_ALIGNMENT    (CELL_ERROR_BASE_GCM | 0x04)
#define CELL_GCM_ERROR_ADDRESS_OVERWRAP     (CELL_ERROR_BASE_GCM | 0x05)

/* ---------------------------------------------------------------------------
 * Helper macros
 * -----------------------------------------------------------------------*/

/* True if value represents an error (high bit set). */
#define CELL_IS_ERROR(code)   (((uint32_t)(code) & 0x80000000u) != 0)

/* Return from HLE function on error. */
#define CELL_RETURN_IF_ERROR(expr) \
    do {                                            \
        int32_t _rc = (int32_t)(expr);              \
        if (CELL_IS_ERROR(_rc)) return _rc;         \
    } while (0)

/* Build a module-specific error code. */
#define CELL_MODULE_ERROR(base, code)  ((int32_t)((base) | (code)))

#ifdef __cplusplus
#include <cstdio>

/* Pretty-print an error code for debug logging. */
static inline const char* cell_error_name(int32_t code)
{
    static thread_local char buf[32];
    switch ((uint32_t)code)
    {
    case CELL_OK:          return "CELL_OK";
    case CELL_EAGAIN:      return "CELL_EAGAIN";
    case CELL_EINVAL:      return "CELL_EINVAL";
    case CELL_ENOSYS:      return "CELL_ENOSYS";
    case CELL_ENOMEM:      return "CELL_ENOMEM";
    case CELL_ESRCH:       return "CELL_ESRCH";
    case CELL_ENOENT:      return "CELL_ENOENT";
    case CELL_EPERM:       return "CELL_EPERM";
    case CELL_EBUSY:       return "CELL_EBUSY";
    case CELL_ETIMEDOUT:   return "CELL_ETIMEDOUT";
    case CELL_EABORT:      return "CELL_EABORT";
    case CELL_EFAULT:      return "CELL_EFAULT";
    case CELL_EEXIST:      return "CELL_EEXIST";
    default:
        snprintf(buf, sizeof(buf), "0x%08X", (unsigned)code);
        return buf;
    }
}
#endif /* __cplusplus */

#endif /* PS3EMU_ERROR_CODES_H */
