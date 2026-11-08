/* edat.h -- transparent NPDRM (EDAT/SDAT) decryption for the filesystem layer.
 *
 * PSN content ships its payload as an EDAT: an NPD header followed by AES-encrypted
 * blocks. On hardware the guest does NOT decrypt these itself -- it calls
 * sceNpDrmIsAvailable(klicensee, path), which primes the kernel, and every later
 * cellFsOpen/read of that path returns PLAINTEXT. A runtime that opens the file
 * literally hands the guest ciphertext, and the guest reads a garbage header.
 *
 * ps1_netemu hits exactly this: it opens USRDIR/ISO.BIN.EDAT, finds nonsense, and
 * exits with `ExitPS1(): code=3` / `CoreBoot() failed`.
 *
 * So the FS layer resolves it instead: on open, a file beginning "NPD\0" is decrypted
 * once into a cache file and the cache is opened in its place. Every read/seek/stat
 * path above then works unchanged, which is why this is a path substitution rather
 * than a read hook.
 *
 * Scope: uncompressed EDATs with AES-CMAC block hashes -- the common case, and what
 * PSOne Classics use. Compressed blocks and the SHA1-HMAC hash modes are detected and
 * refused by name rather than silently mis-decrypted.
 *
 * Keys are the published NPDRM constants (the same ones RPCS3, scetool and every EDAT
 * tool carry); a license-type-3 "free" EDAT needs no per-console RAP. A type-1/2 EDAT
 * is bound to a RAP we do not have, and is refused with that stated.
 */
#ifndef PS3RECOMP_EDAT_H
#define PS3RECOMP_EDAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True if the file at host_path starts with the NPD magic. Cheap: one 4-byte read. */
int edat_is_npd(const char* host_path);

/* Decrypt in_path -> out_path. 0 on success, negative on failure (reason logged). */
int edat_decrypt_file(const char* in_path, const char* out_path);

/* The path the FS layer should actually open for `host_path`.
 *
 * Returns host_path unchanged when it is not an EDAT, or when decryption fails (the
 * caller then opens the original and the guest reports its own error -- better than
 * an open() failure it cannot attribute). Otherwise returns a cache path, written
 * into `buf`. The cache lives in $PS3RECOMP_EDAT_CACHE (default: a ps3recomp-edat
 * directory under the host temp directory), never beside the source, which is
 * usually the read-only disc tree; it is reused while it is newer than the source.
 * Set PS3_EDAT=0 to disable the whole path. */
const char* edat_resolve(const char* host_path, char* buf, size_t cap);

/* The same resolution for a plain open (cellFsOpen, sys_fs_open): SDATA is left
 * as its raw container, since only cellFsSdataOpen decrypts it; EDAT resolves as
 * above. */
const char* edat_resolve_open(const char* host_path, char* buf, size_t cap);
/* 1 when `host_path` is an NPD container flagged as SDATA. */
int edat_is_sdata(const char* host_path);

/* Hand the runtime a klicensee the guest supplied. On hardware a game that owns
 * free (license type 3) or disc-bound EDATs passes its own klicensee to
 * sceNpDrmIsAvailable / sceNpDrmIsAvailable2 before opening them; the kernel keeps
 * it and decrypts later opens with it. These keys are title-specific and live in
 * the game's code, so a runtime cannot carry them: it has to take them from the
 * guest call. Registered keys are tried before the built-in ones, and each one is
 * still checked against the file's dev_hash before use. Up to 16 are kept (a
 * duplicate is ignored). */
void edat_add_klicensee(const uint8_t klic[16]);

/* Self-check of the AES-128 and AES-CMAC primitives against the FIPS-197 and
 * RFC 4493 vectors. Returns 0 if both match. Run once on first use: silently
 * wrong crypto here produces plausible-looking garbage, which is the single most
 * expensive failure mode this file can have. */
int edat_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_EDAT_H */
