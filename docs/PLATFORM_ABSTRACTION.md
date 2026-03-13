# Platform Abstraction Layer

How ps3recomp handles cross-platform compatibility between Windows and POSIX (Linux/macOS) systems.

---

## Table of Contents

1. [Design Philosophy](#design-philosophy)
2. [Threading](#threading)
3. [Synchronization Primitives](#synchronization-primitives)
4. [Networking](#networking)
5. [Timers and Clocks](#timers-and-clocks)
6. [Memory Management](#memory-management)
7. [Audio](#audio)
8. [Input](#input)
9. [Cryptographic RNG](#cryptographic-rng)
10. [Filesystem Differences](#filesystem-differences)
11. [Fiber / Coroutine Support](#fiber--coroutine-support)

---

## Design Philosophy

ps3recomp uses **inline platform switches** (`#ifdef _WIN32`) rather than a separate abstraction layer. This keeps the code direct and avoids unnecessary indirection.

The pattern is:

```c
int32_t some_operation(void)
{
#ifdef _WIN32
    // Windows implementation using Win32 API
    HANDLE h = CreateSomething(...);
    if (!h) return CELL_ENOMEM;
#else
    // POSIX implementation using pthreads/mmap/etc.
    int rc = posix_function(...);
    if (rc != 0) return CELL_ENOMEM;
#endif
    return CELL_OK;
}
```

This approach is used consistently across all runtime and HLE module code.

---

## Threading

### Thread Creation

| PS3 API | Windows | POSIX |
|---------|---------|-------|
| `sys_ppu_thread_create` | `CreateThread()` | `pthread_create()` |
| `sys_ppu_thread_exit` | `ExitThread()` | `pthread_exit()` |
| `sys_ppu_thread_join` | `WaitForSingleObject()` | `pthread_join()` |
| `sys_ppu_thread_detach` | `CloseHandle()` | `pthread_detach()` |
| `sys_ppu_thread_yield` | `SwitchToThread()` | `sched_yield()` |
| `sys_ppu_thread_rename` | `SetThreadDescription()` | `pthread_setname_np()` |

### Thread Local Storage

Thread-local `ppu_context` pointers are stored using:
- Windows: `__declspec(thread)` or `TlsAlloc()` / `TlsGetValue()`
- POSIX: `__thread` or `pthread_key_create()` / `pthread_getspecific()`

### Thread Priority

PS3 priorities (0 = highest, 3071 = lowest) are mapped approximately to host priorities:

**Windows:**
```c
if (priority < 500)       SetThreadPriority(h, THREAD_PRIORITY_HIGHEST);
else if (priority < 1000) SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
else if (priority < 2000) SetThreadPriority(h, THREAD_PRIORITY_NORMAL);
else if (priority < 2500) SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
else                      SetThreadPriority(h, THREAD_PRIORITY_LOWEST);
```

**POSIX:**
```c
struct sched_param param;
param.sched_priority = sched_get_priority_max(SCHED_OTHER)
                     - (priority * range / 3071);
pthread_setschedparam(thread, SCHED_OTHER, &param);
```

---

## Synchronization Primitives

### Mutexes

| Feature | Windows | POSIX |
|---------|---------|-------|
| **Type** | `CRITICAL_SECTION` | `pthread_mutex_t` |
| **Init** | `InitializeCriticalSection()` | `pthread_mutex_init()` |
| **Lock** | `EnterCriticalSection()` | `pthread_mutex_lock()` |
| **Try Lock** | `TryEnterCriticalSection()` | `pthread_mutex_trylock()` |
| **Unlock** | `LeaveCriticalSection()` | `pthread_mutex_unlock()` |
| **Destroy** | `DeleteCriticalSection()` | `pthread_mutex_destroy()` |
| **Recursive** | Always recursive by default | Set `PTHREAD_MUTEX_RECURSIVE` attr |

**Why `CRITICAL_SECTION` over `CreateMutex`?**
- `CRITICAL_SECTION` is much faster (user-space fast path, no kernel transition)
- Sufficient for intra-process synchronization (which is all we need)
- No named-object overhead

### Condition Variables

| Feature | Windows | POSIX |
|---------|---------|-------|
| **Type** | `CONDITION_VARIABLE` | `pthread_cond_t` |
| **Init** | `InitializeConditionVariable()` | `pthread_cond_init()` |
| **Wait** | `SleepConditionVariableCS()` | `pthread_cond_wait()` |
| **Timed Wait** | `SleepConditionVariableCS(timeout)` | `pthread_cond_timedwait()` |
| **Signal** | `WakeConditionVariable()` | `pthread_cond_signal()` |
| **Broadcast** | `WakeAllConditionVariable()` | `pthread_cond_broadcast()` |
| **Destroy** | No-op (statically allocated) | `pthread_cond_destroy()` |

### Semaphores

| Feature | Windows | POSIX |
|---------|---------|-------|
| **Type** | `HANDLE` (Win32 Semaphore) | `sem_t` |
| **Create** | `CreateSemaphoreW(NULL, init, max, NULL)` | `sem_init(&sem, 0, init)` |
| **Wait** | `WaitForSingleObject(h, INFINITE)` | `sem_wait(&sem)` |
| **Try Wait** | `WaitForSingleObject(h, 0)` | `sem_trywait(&sem)` |
| **Timed Wait** | `WaitForSingleObject(h, ms)` | `sem_timedwait(&sem, &ts)` |
| **Post** | `ReleaseSemaphore(h, count, NULL)` | `sem_post(&sem)` × count |
| **Destroy** | `CloseHandle(h)` | `sem_destroy(&sem)` |

### Read-Write Locks

| Feature | Windows | POSIX |
|---------|---------|-------|
| **Type** | `SRWLOCK` | `pthread_rwlock_t` |
| **Init** | `InitializeSRWLock()` | `pthread_rwlock_init()` |
| **Read Lock** | `AcquireSRWLockShared()` | `pthread_rwlock_rdlock()` |
| **Write Lock** | `AcquireSRWLockExclusive()` | `pthread_rwlock_wrlock()` |
| **Try Read** | `TryAcquireSRWLockShared()` | `pthread_rwlock_tryrdlock()` |
| **Try Write** | `TryAcquireSRWLockExclusive()` | `pthread_rwlock_trywrlock()` |
| **Read Unlock** | `ReleaseSRWLockShared()` | `pthread_rwlock_unlock()` |
| **Write Unlock** | `ReleaseSRWLockExclusive()` | `pthread_rwlock_unlock()` |

---

## Networking

### Socket API

| Operation | Windows (Winsock2) | POSIX |
|-----------|-------------------|-------|
| **Initialize** | `WSAStartup(MAKEWORD(2,2), &wsa)` | No-op |
| **Shutdown** | `WSACleanup()` | No-op |
| **Socket** | `socket()` | `socket()` |
| **Close** | `closesocket(fd)` | `close(fd)` |
| **Error** | `WSAGetLastError()` | `errno` |
| **Non-blocking** | `ioctlsocket(fd, FIONBIO, &mode)` | `fcntl(fd, F_SETFL, O_NONBLOCK)` |
| **Poll** | `WSAPoll()` | `poll()` |

### Error Code Translation

Winsock errors are translated to PS3 errno values:

```c
static int winsock_to_ps3_errno(int wsa_err)
{
    switch (wsa_err) {
    case WSAEWOULDBLOCK:   return SYS_NET_EWOULDBLOCK;
    case WSAECONNREFUSED:  return SYS_NET_ECONNREFUSED;
    case WSAETIMEDOUT:     return SYS_NET_ETIMEDOUT;
    case WSAEINPROGRESS:   return SYS_NET_EINPROGRESS;
    case WSAEALREADY:      return SYS_NET_EALREADY;
    case WSAECONNRESET:    return SYS_NET_ECONNRESET;
    // ... more mappings
    default:               return SYS_NET_EINVAL;
    }
}
```

### DNS Resolution

Both platforms use `getaddrinfo()` for DNS resolution (Winsock2 and POSIX both support it). The main difference is initialization — Winsock2 requires `WSAStartup()` before any network calls.

### Socket Address Handling

PS3 uses big-endian `sockaddr_in`. The conversion:

```c
// PS3 sockaddr → host sockaddr
struct sockaddr_in host_addr;
host_addr.sin_family = AF_INET;
host_addr.sin_port = ps3_addr->sin_port;      // Already in network byte order
host_addr.sin_addr.s_addr = ps3_addr->sin_addr; // Already in network byte order
```

---

## Timers and Clocks

### High-Resolution Timer

| Operation | Windows | POSIX |
|-----------|---------|-------|
| **Get current time** | `QueryPerformanceCounter(&li)` | `clock_gettime(CLOCK_MONOTONIC, &ts)` |
| **Get frequency** | `QueryPerformanceFrequency(&li)` | Fixed: 1,000,000,000 (nanoseconds) |
| **Convert to PS3 ticks** | `ticks = li.QuadPart * 79800000 / freq` | `ticks = ts.tv_sec * 79800000 + ts.tv_nsec * 79800000 / 1000000000` |

### Wall Clock Time

| Operation | Windows | POSIX |
|-----------|---------|-------|
| **Current time** | `GetSystemTimeAsFileTime()` → convert | `clock_gettime(CLOCK_REALTIME, &ts)` |
| **Local time** | `GetLocalTime(&st)` | `localtime_r(&time, &tm)` |
| **Timezone** | `GetTimeZoneInformation(&tz)` | `tm.tm_gmtoff` |

### Sleep

| Operation | Windows | POSIX |
|-----------|---------|-------|
| **Microsecond sleep** | `Sleep(usec / 1000)` (ms granularity) | `usleep(usec)` |
| **Second sleep** | `Sleep(sec * 1000)` | `sleep(sec)` |

Note: Windows `Sleep()` has millisecond granularity. For sub-millisecond sleeps, a busy-wait or `timeBeginPeriod(1)` may be needed for accuracy.

---

## Memory Management

### Virtual Memory

| Operation | Windows | POSIX |
|-----------|---------|-------|
| **Reserve** | `VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS)` | `mmap(NULL, size, PROT_NONE, MAP_PRIVATE\|MAP_ANONYMOUS\|MAP_NORESERVE, -1, 0)` |
| **Commit** | `VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE)` | `mprotect(addr, size, PROT_READ\|PROT_WRITE)` |
| **Protect** | `VirtualProtect(addr, size, prot, &old)` | `mprotect(addr, size, prot)` |
| **Release** | `VirtualFree(base, 0, MEM_RELEASE)` | `munmap(base, size)` |

### Protection Flags

| Desired Access | Windows | POSIX |
|---------------|---------|-------|
| No access | `PAGE_NOACCESS` | `PROT_NONE` |
| Read only | `PAGE_READONLY` | `PROT_READ` |
| Read/Write | `PAGE_READWRITE` | `PROT_READ \| PROT_WRITE` |
| Execute/Read | `PAGE_EXECUTE_READ` | `PROT_EXEC \| PROT_READ` |
| All | `PAGE_EXECUTE_READWRITE` | `PROT_READ \| PROT_WRITE \| PROT_EXEC` |

---

## Audio

### Backends

| Platform | Primary Backend | Fallback |
|----------|----------------|----------|
| Windows | WASAPI | SDL2 |
| Linux | SDL2 | PulseAudio (potential future) |
| macOS | SDL2 | CoreAudio (potential future) |

### WASAPI (Windows)

```c
// Exclusive mode for low latency
IAudioClient* client;
IMMDevice* device;
// CoCreateInstance → IMMDeviceEnumerator → GetDefaultAudioEndpoint
// client->Initialize(AUDCLK_SHARED, 0, bufferDuration, 0, &fmt, NULL)
// client->GetService(IID_IAudioRenderClient, &renderClient)
// renderClient->GetBuffer(frames, &data)
// ... write PCM samples ...
// renderClient->ReleaseBuffer(frames, 0)
```

### SDL2 (Cross-platform)

```c
SDL_AudioSpec spec = {
    .freq = 48000,
    .format = AUDIO_F32SYS,
    .channels = 2,
    .samples = 256,
    .callback = audio_callback,
    .userdata = &audio_state
};
SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &spec, &obtained, 0);
SDL_PauseAudioDevice(dev, 0);  // Start playback
```

---

## Input

### Gamepad Backends

| Platform | Primary Backend | Notes |
|----------|----------------|-------|
| Windows | XInput | Native Xbox controller support; DS3 via DS3 drivers |
| Linux/macOS | SDL2 GameController | Supports DualShock 3/4, Xbox, Switch Pro, etc. |

### XInput (Windows)

```c
XINPUT_STATE state;
if (XInputGetState(port, &state) == ERROR_SUCCESS) {
    // Map to PS3 pad data
    pad_data->button[0] |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_A)
                           ? CELL_PAD_CTRL_CROSS : 0;
    pad_data->button[4] = (state.Gamepad.sThumbLX + 32768) >> 8;  // Left X
    // ...
}
```

### SDL2 GameController

```c
SDL_GameController* gc = SDL_GameControllerOpen(index);
if (gc) {
    int16_t lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
    bool cross = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A);
    // Map to PS3 pad data
}
```

---

## Cryptographic RNG

| Platform | API | Notes |
|----------|-----|-------|
| Windows | `BCryptGenRandom(NULL, buf, len, BCRYPT_USE_SYSTEM_PREFERRED_RNG)` | Requires `bcrypt.lib` |
| POSIX | `read(open("/dev/urandom", O_RDONLY), buf, len)` | Always available on Linux/macOS |

Used by cellSsl for certificate-related operations and random number generation.

---

## Filesystem Differences

### Path Separators

PS3 uses `/` (forward slash). Windows uses `\` (backslash) but the Win32 API accepts both. ps3recomp uses `/` internally and lets the OS handle it.

### File Attributes

PS3's `cellFsStat` returns big-endian metadata. The host-to-PS3 conversion:

```c
// Host stat → PS3 CellFsStat
ps3_stat->st_mode   = host_to_be32(host_stat.st_mode);
ps3_stat->st_size   = host_to_be64(host_stat.st_size);
ps3_stat->st_atime  = host_to_be64(host_stat.st_atime);
ps3_stat->st_mtime  = host_to_be64(host_stat.st_mtime);
ps3_stat->st_ctime  = host_to_be64(host_stat.st_ctime);
```

### File Open Flags

| PS3 Flag | Value | Windows | POSIX |
|----------|-------|---------|-------|
| `CELL_FS_O_RDONLY` | 0 | `GENERIC_READ` | `O_RDONLY` |
| `CELL_FS_O_WRONLY` | 1 | `GENERIC_WRITE` | `O_WRONLY` |
| `CELL_FS_O_RDWR` | 2 | `GENERIC_READ \| GENERIC_WRITE` | `O_RDWR` |
| `CELL_FS_O_CREAT` | 0x100 | `CREATE_ALWAYS` | `O_CREAT` |
| `CELL_FS_O_TRUNC` | 0x200 | `TRUNCATE_EXISTING` | `O_TRUNC` |
| `CELL_FS_O_APPEND` | 0x400 | `FILE_APPEND_DATA` | `O_APPEND` |

---

## Fiber / Coroutine Support

### Windows Fibers

```c
// Convert current thread to a fiber
ConvertThreadToFiber(NULL);

// Create a new fiber
LPVOID fiber = CreateFiber(stack_size, fiber_proc, param);

// Switch to fiber
SwitchToFiber(fiber);

// Delete fiber
DeleteFiber(fiber);
```

### POSIX ucontext

```c
ucontext_t ctx;
getcontext(&ctx);
ctx.uc_stack.ss_sp = stack_memory;
ctx.uc_stack.ss_size = stack_size;
ctx.uc_link = &return_context;
makecontext(&ctx, fiber_proc, 1, param);

// Switch to fiber
swapcontext(&current, &ctx);
```

**Note:** `ucontext` is deprecated on macOS but still works. For production macOS builds, consider using `setjmp`/`longjmp` with manual stack management, or platform-specific alternatives.
