# psnr client (vendored)

`psnr.c` and `psnr.h` are copied unchanged from
[sp00nznet/psnr](https://github.com/sp00nznet/psnr) `client/` (MIT, same author).
That repository also holds the server they talk to and the protocol reference
(`docs/api.md`). Change them there first, then copy them here.

`libs/network/np_psnr.c` owns the connection. The sceNp modules (Matching2
rooms today) send through it, and it is pumped from `cellSysutilCheckCallback`,
so replies and pushes become guest callbacks on the thread the title polls from.
