/* D3D12 device creation shared by the D3D12 backend and the live renderer.
 *
 * Order: the hardware adapters in EnumAdapterByGpuPreference order (high
 * performance first, so a hybrid laptop gets its discrete GPU), skipping
 * software adapters; then the default adapter (D3D12CreateDevice(NULL)); then
 * WARP (IDXGIFactory4::EnumWarpAdapter, Microsoft's software rasteriser), so
 * the program still starts on a machine with no D3D12 driver (a VM, a server).
 *
 * RSX_D3D12_WARP: unset = the order above, "1" = WARP only, "0" = never WARP
 * (no device when the hardware and default adapters fail, as before).
 *
 * A software adapter is one with DXGI_ADAPTER_FLAG_SOFTWARE, or the Basic
 * Render Driver by its ids (vendor 0x1414, device 0x008C): that adapter has
 * been seen enumerated without the flag in a Windows VM, and a test of the
 * flag alone then takes WARP for the hardware adapter.
 *
 * The order is a pure function, rsx_pick_device, tested on any host by
 * libs/video/tests/test_d3d12_device_pick.c; the Windows part below it needs
 * <windows.h> and <d3d12.h> first. */
#ifndef RSX_D3D12_DEVICE_H
#define RSX_D3D12_DEVICE_H

#include <stdio.h>

enum { RSX_WARP_AUTO = 0, RSX_WARP_FORCE = 1, RSX_WARP_NEVER = 2 };

/* RSX_D3D12_WARP's value -> mode. Anything but unset, "", "1" or "0" is auto
 * with *bad = 1. */
static inline int rsx_warp_mode_parse(const char* e, int* bad)
{
    if (bad) *bad = 0;
    if (!e || !*e) return RSX_WARP_AUTO;
    if (e[0] == '1' && !e[1]) return RSX_WARP_FORCE;
    if (e[0] == '0' && !e[1]) return RSX_WARP_NEVER;
    if (bad) *bad = 1;
    return RSX_WARP_AUTO;
}

/* The Basic Render Driver (WARP) adapter's ids. */
#define RSX_WARP_VENDOR 0x1414u
#define RSX_WARP_DEVICE 0x008Cu

static inline int rsx_adapter_is_software(int flag_software, unsigned vendor, unsigned device)
{
    return flag_software || (vendor == RSX_WARP_VENDOR && device == RSX_WARP_DEVICE);
}

/* rsx_pick_device: a[0..n) is the adapter list in preference order.
 * try_fn(ctx, which) returns nonzero when it created a device on: which >= 0,
 * the adapter at that index; RSX_PICK_DEFAULT, the default adapter;
 * RSX_PICK_WARP, the WARP adapter. Returns the which that worked, or
 * RSX_PICK_NONE. */
#define RSX_PICK_NONE    (-1)
#define RSX_PICK_DEFAULT (-2)
#define RSX_PICK_WARP    (-3)
typedef struct { unsigned vendor, device; int flag_software; } rsx_adapter_info;
typedef int (*rsx_pick_try_fn)(void* ctx, int which);

static inline int rsx_pick_device(int mode, const rsx_adapter_info* a, int n,
                                  rsx_pick_try_fn try_fn, void* ctx)
{
    if (mode != RSX_WARP_FORCE) {
        for (int i = 0; i < n; i++) {
            if (rsx_adapter_is_software(a[i].flag_software, a[i].vendor, a[i].device)) continue;
            if (try_fn(ctx, i)) return i;
        }
        if (try_fn(ctx, RSX_PICK_DEFAULT)) return RSX_PICK_DEFAULT;
    }
    if (mode != RSX_WARP_NEVER && try_fn(ctx, RSX_PICK_WARP)) return RSX_PICK_WARP;
    return RSX_PICK_NONE;
}

#ifdef _WIN32
#include <stdlib.h>
#include <dxgi1_6.h>

#define RSX_MAX_ADAPTERS 16

/* One "[D3D12] device" line for the device that was created. */
static inline void rsx_d3d12_log_device(ID3D12Device* dev, IDXGIFactory4* f,
                                        const char* who, const char* how)
{
    char name[256] = "?";
    unsigned vendor = 0, device = 0;
    int software = -1;
    unsigned long long vram = 0;
    LUID luid = {0, 0};
    dev->lpVtbl->GetAdapterLuid(dev, &luid);
    IDXGIAdapter1* ad = NULL;
    if (SUCCEEDED(f->lpVtbl->EnumAdapterByLuid(f, luid, &IID_IDXGIAdapter1, (void**)&ad)) && ad) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(ad->lpVtbl->GetDesc1(ad, &d))) {
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, (int)sizeof name, NULL, NULL);
            vendor = d.VendorId;
            device = d.DeviceId;
            software = rsx_adapter_is_software((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0, vendor, device);
            vram = (unsigned long long)d.DedicatedVideoMemory >> 20;
        }
        ad->lpVtbl->Release(ad);
    }
    printf("[D3D12] device (%s, %s): \"%s\" vendor=0x%04X device=0x%04X software=%d VRAM %llu MB\n",
           who, how, name, vendor, device, software, vram);
}

typedef struct {
    IDXGIFactory4* f;
    ID3D12Device** out;
    IDXGIAdapter1** ads;
    const char* who;
} rsx_try_ctx;

/* rsx_pick_device's callback: one D3D12CreateDevice; a failure is logged with
 * its HRESULT. */
static inline int rsx_try_create(void* vctx, int which)
{
    rsx_try_ctx* c = (rsx_try_ctx*)vctx;
    IUnknown* adapter = NULL;
    IDXGIAdapter* warp = NULL;
    HRESULT hr;
    *c->out = NULL;
    if (which >= 0) {
        adapter = (IUnknown*)c->ads[which];
    } else if (which == RSX_PICK_WARP) {
        hr = c->f->lpVtbl->EnumWarpAdapter(c->f, &IID_IDXGIAdapter, (void**)&warp);
        if (FAILED(hr) || !warp) {
            printf("[D3D12] EnumWarpAdapter failed (0x%08lX) (%s)\n", (unsigned long)hr, c->who);
            return 0;
        }
        adapter = (IUnknown*)warp;
    }
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)c->out);
    if (warp) warp->lpVtbl->Release(warp);
    if (SUCCEEDED(hr) && *c->out) return 1;
    *c->out = NULL;
    if (which >= 0)
        printf("[D3D12] D3D12CreateDevice on adapter %d failed (0x%08lX) (%s)\n",
               which, (unsigned long)hr, c->who);
    else
        printf("[D3D12] D3D12CreateDevice on %s failed (0x%08lX) (%s)\n",
               which == RSX_PICK_WARP ? "WARP" : "the default adapter", (unsigned long)hr, c->who);
    return 0;
}

/* Create *out by the order at the top of this file. pref orders the adapter
 * list (DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE normally); on a DXGI without
 * IDXGIFactory6 the list is EnumAdapters1's order. -> 0 hardware or default
 * adapter, 1 WARP, -1 no device. */
static inline int rsx_d3d12_create_device(IDXGIFactory4* f, ID3D12Device** out,
                                          const char* who, DXGI_GPU_PREFERENCE pref)
{
    *out = NULL;
    int bad = 0;
    const int mode = rsx_warp_mode_parse(getenv("RSX_D3D12_WARP"), &bad);
    if (bad)
        printf("[D3D12] RSX_D3D12_WARP=%s not understood (1 = WARP only, 0 = never): ignored\n",
               getenv("RSX_D3D12_WARP"));

    rsx_adapter_info infos[RSX_MAX_ADAPTERS];
    IDXGIAdapter1* ads[RSX_MAX_ADAPTERS];
    int n = 0;
    if (mode != RSX_WARP_FORCE) {
        IDXGIFactory6* f6 = NULL;
        if (FAILED(f->lpVtbl->QueryInterface(f, &IID_IDXGIFactory6, (void**)&f6))) f6 = NULL;
        for (UINT i = 0; n < RSX_MAX_ADAPTERS; i++) {
            IDXGIAdapter1* ad = NULL;
            HRESULT hr = f6 ? f6->lpVtbl->EnumAdapterByGpuPreference(f6, i, pref, &IID_IDXGIAdapter1, (void**)&ad)
                            : f->lpVtbl->EnumAdapters1(f, i, &ad);
            if (FAILED(hr) || !ad) break;
            DXGI_ADAPTER_DESC1 d;
            if (FAILED(ad->lpVtbl->GetDesc1(ad, &d))) { ad->lpVtbl->Release(ad); continue; }
            infos[n].vendor = d.VendorId;
            infos[n].device = d.DeviceId;
            infos[n].flag_software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            ads[n++] = ad;
        }
        if (f6) f6->lpVtbl->Release(f6);
    }

    rsx_try_ctx tc = { f, out, ads, who };
    const int which = rsx_pick_device(mode, infos, n, rsx_try_create, &tc);
    for (int i = 0; i < n; i++) ads[i]->lpVtbl->Release(ads[i]);
    if (which == RSX_PICK_NONE) {
        printf("[D3D12] ERROR: no D3D12 device on any adapter (%s)%s\n", who,
               mode == RSX_WARP_NEVER ? "; RSX_D3D12_WARP=0 rules out WARP" : "");
        return -1;
    }
    rsx_d3d12_log_device(*out, f, who,
                         which >= 0 ? "adapter by GPU preference"
                         : which == RSX_PICK_DEFAULT ? "default adapter"
                         : mode == RSX_WARP_FORCE ? "WARP, RSX_D3D12_WARP=1"
                         : "WARP, no hardware device");
    return which == RSX_PICK_WARP ? 1 : 0;
}
#endif /* _WIN32 */
#endif /* RSX_D3D12_DEVICE_H */
