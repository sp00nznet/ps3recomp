# Debug console

> Ask a title that is **already running** what it is doing, without a rebuild.

Set `PS3_DEBUG` to a file path. A console thread watches that file; write one
command into it and the answer is appended to `<path>.out`, then the command
file is truncated ready for the next one.

```bash
RSX_LIVE_DRAW=1 PS3_DEBUG=dbg.txt ./build/simpsons.exe vfs/PS3_GAME/USRDIR/EBOOT.elf &

echo stat            > dbg.txt ; sleep 1 ; cat dbg.txt.out
echo threads         > dbg.txt
echo "mem 10000 32"  > dbg.txt
```

| command | what it answers |
|---|---|
| `threads` | stack of every guest thread, symbolised to `func_XXXXXXXX+off` |
| `hle` | the last HLE call the runtime dispatched |
| `stat` | uptime, flip count, HLE breadcrumb, whether `vm_base` is mapped |
| `mem <hex> [len]` | hexdump of guest memory |
| `poke32 <hex> <val>` | write a guest u32 (big-endian, as the guest sees it) |
| `knobs [prefix]` | which diagnostics *this run* was started with |
| `help` | the above |

## Why a file and not a socket

No listening port inside a game process, nothing for a firewall to prompt
about, and it drives from a shell script the same way `PAD_FILE` already does —
which is how these titles get driven to an interesting state in the first place.

## Why it is read-only apart from `poke32`

There is no `set` command, and that is deliberate rather than unfinished. The
runtime has ~470 diagnostic environment variables (see
[DIAGNOSTICS.md](DIAGNOSTICS.md)) but the large majority are read once at first
use and cached in a function-local static, so they are launch-time settings —
`set` would appear to work and change nothing. `knobs` reports what the run
actually started with, which is the honest half of that.

## What it is for

The motivating case: a title that wedges ninety seconds in. The hang watchdog
samples thread stacks at fixed times after start, so catching a late hang meant
editing the runtime and rebuilding — a full cycle per question. `threads` on
demand answers it directly, and `stat` distinguishes *stopped* from *still
running but not drawing* without trawling a 200,000-line log.

A spin and a deadlock look identical in a log and are told apart by CPU time,
so check that too — a process burning cores while emitting nothing is spinning,
not blocked.
