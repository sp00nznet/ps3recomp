/* Worker-owned Win32 overlapped transport. SDL is used only for enumeration.
 * Never wait indefinitely for a write or for driver cancellation. Persistent
 * buffers/OVERLAPPEDs remain alive until the kernel signals completion. */
#include <windows.h>
#include <stdlib.h>
typedef struct G7WinDevice {
    HANDLE file;
    OVERLAPPED rd, wr;
    unsigned char input[64], output[64];
    int reading, writing;
} G7WinDevice;
static G7WinDevice *g_quarantined;
static int win_complete(G7WinDevice *d) {
    return (!d->reading || WaitForSingleObject(d->rd.hEvent, 0) == WAIT_OBJECT_0) &&
           (!d->writing || WaitForSingleObject(d->wr.hEvent, 0) == WAIT_OBJECT_0);
}
static void win_free(G7WinDevice *d) {
    if (d->rd.hEvent) CloseHandle(d->rd.hEvent);
    if (d->wr.hEvent) CloseHandle(d->wr.hEvent);
    free(d);
}
static void *real_open(const char *path) {
    if (g_quarantined) {
        if (!win_complete(g_quarantined)) return NULL;
        win_free(g_quarantined); g_quarantined = NULL;
    }
    wchar_t wide[1024];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 1024)) return NULL;
    G7WinDevice *d = (G7WinDevice *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->file = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (d->file == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->rd.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    d->wr.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!d->rd.hEvent || !d->wr.hEvent) { CloseHandle(d->file); win_free(d); return NULL; }
    return d;
}
static int real_read(void *device, unsigned char *buffer, size_t size, int timeout) {
    G7WinDevice *d = (G7WinDevice *)device;
    DWORD n = 0;
    if (size < sizeof(d->input)) return -1;
    if (!d->reading) {
        ResetEvent(d->rd.hEvent);
        if (ReadFile(d->file, d->input, sizeof(d->input), &n, &d->rd)) {
            memcpy(buffer, d->input, n); return (int)n;
        }
        if (GetLastError() != ERROR_IO_PENDING) return -1;
        d->reading = 1;
    }
    DWORD wait = WaitForSingleObject(d->rd.hEvent, timeout > 0 ? (DWORD)timeout : 0);
    if (wait == WAIT_TIMEOUT) return 0;
    if (wait != WAIT_OBJECT_0) return -1;
    d->reading = 0;
    if (!GetOverlappedResult(d->file, &d->rd, &n, FALSE)) return -1;
    memcpy(buffer, d->input, n); return (int)n;
}
static int real_write(void *device, const unsigned char *buffer, size_t size) {
    G7WinDevice *d = (G7WinDevice *)device;
    DWORD n = 0;
    if (size != sizeof(d->output) || d->writing) return -1;
    memcpy(d->output, buffer, size);
    ResetEvent(d->wr.hEvent);
    if (WriteFile(d->file, d->output, (DWORD)size, &n, &d->wr)) return (int)n;
    if (GetLastError() != ERROR_IO_PENDING) return -1;
    d->writing = 1;
    if (WaitForSingleObject(d->wr.hEvent, 25) != WAIT_OBJECT_0) {
        CancelIoEx(d->file, &d->wr);
        return -1;
    }
    d->writing = 0;
    return GetOverlappedResult(d->file, &d->wr, &n, FALSE) ? (int)n : -1;
}
static void real_close(void *device) {
    G7WinDevice *d = (G7WinDevice *)device;
    CancelIoEx(d->file, NULL);
    if (d->reading) WaitForSingleObject(d->rd.hEvent, 25);
    if (d->writing) WaitForSingleObject(d->wr.hEvent, 25);
    CloseHandle(d->file);
    if (win_complete(d)) win_free(d);
    else g_quarantined = d; /* At most one; new opens blocked until completion. */
}
