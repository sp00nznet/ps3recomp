/*
 * ps3recomp - cellAvconfExt HLE implementation
 *
 * Reports standard LPCM stereo 48kHz as available audio output.
 * Games query this to decide audio format before opening cellAudio ports.
 */

#include "cellAvconfExt.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write32: translate + byte-swap */

/* The HLE ABI adapter passes pointer parameters as raw guest addresses, so a
 * bare deref here writes to whatever host address shares that number. These
 * three out-params were dereferenced directly, and cellAudioOutGetDeviceInfo
 * memset()s through one -- an access violation the moment a title reaches audio
 * device setup, which is exactly where Tokyo Jungle crashed. Both device structs
 * are all-u8, so translation alone is correct for them; the float needs a
 * byte-swapped store. */
extern u8* vm_base;
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static float s_gamma = 1.0f;

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioOutGetSoundAvailability(u32 audioOut, u32 type, u32 fs, u32 option)
{
    (void)audioOut;
    (void)option;

    /* Report the SAME capability set a real PS3/RPCS3 reports, not just stereo.
     * Golden trace (RPCS3.log, real YDKJ boot) shows:
     *   cellAudioOut: found support for Linear PCM 2 Ch. / 5.1 Ch. / 7.1 Ch.
     *   cellAudioOut: found support for Dolby Digital 5.1 Ch. / DTS 5.1 Ch.
     * This call returns the MAX channel count available for (type, fs). Under-reporting
     * stereo (the old behaviour) makes FMOD's init probe fail its 8/6-channel checks and
     * then fall through to DTS/AC3 probes that also returned 0 -- YDKJ ends up with no
     * FMOD threads at all. Returning the real 7.1 for LPCM matches the console. */
    if (type == CELL_AUDIO_OUT_CODING_TYPE_LPCM) {
        if (fs & CELL_AUDIO_OUT_FS_48KHZ) return CELL_AUDIO_OUT_CHNUM_6;  /* 5.1 (A/B: was 8) */
        return CELL_AUDIO_OUT_CHNUM_2;
    }

    /* Dolby Digital / DTS: real hardware reports 5.1 for these. */
    if (type == CELL_AUDIO_OUT_CODING_TYPE_AC3 || type == CELL_AUDIO_OUT_CODING_TYPE_DTS)
        return CELL_AUDIO_OUT_CHNUM_6;

    return 0; /* not available */
}

s32 cellAudioOutGetSoundAvailability2(u32 audioOut, u32 type, u32 fs, u32 ch, u32 option)
{
    (void)audioOut;
    (void)option;

    if (type == CELL_AUDIO_OUT_CODING_TYPE_LPCM &&
        (fs & CELL_AUDIO_OUT_FS_48KHZ) &&
        (ch == CELL_AUDIO_OUT_CHNUM_2 || ch == CELL_AUDIO_OUT_CHNUM_6 || ch == CELL_AUDIO_OUT_CHNUM_8))
        return 1; /* available */

    return 0;
}

s32 cellAudioOutGetDeviceInfo(u32 audioOut, u32 deviceIndex,
                               CellAudioOutDeviceInfo* info)
{
    (void)audioOut;
    (void)deviceIndex;

    printf("[cellAvconfExt] AudioOutGetDeviceInfo(out=%u, dev=%u)\n",
           audioOut, deviceIndex);

    if (!info)
        return CELL_EINVAL;
    info = GUEST_PTR(info, CellAudioOutDeviceInfo*);

    memset(info, 0, sizeof(CellAudioOutDeviceInfo));

    /* Report the mode set a real PS3 reports. Golden trace (RPCS3.log, real YDKJ boot):
     *   "found support for Linear PCM 2 Ch. / 5.1 Ch. / 7.1 Ch."
     *   "found support for Dolby Digital 5.1 Ch. / DTS 5.1 Ch."
     * Advertising a single stereo mode here while GetSoundAvailability reports 5.1/7.1 is
     * self-inconsistent, and middleware (FMOD) validates these against each other. */
    info->portType = 0; /* HDMI */
    info->state = 2;    /* connected */
    info->availableModeCount = 3;
    info->availableModes[0].type    = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
    info->availableModes[0].channel = CELL_AUDIO_OUT_CHNUM_2;
    info->availableModes[0].fs      = CELL_AUDIO_OUT_FS_48KHZ;
    info->availableModes[1].type    = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
    info->availableModes[1].channel = CELL_AUDIO_OUT_CHNUM_6;
    info->availableModes[1].fs      = CELL_AUDIO_OUT_FS_48KHZ;
    info->availableModes[2].type    = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
    info->availableModes[2].channel = CELL_AUDIO_OUT_CHNUM_8;
    info->availableModes[2].fs      = CELL_AUDIO_OUT_FS_48KHZ;

    return CELL_OK;
}

s32 cellAudioOutGetConfiguration(u32 audioOut,
                                  CellAudioOutConfiguration* config,
                                  void* option, u32 optionSize)
{
    (void)audioOut;
    (void)option;
    (void)optionSize;

    printf("[cellAvconfExt] AudioOutGetConfiguration()\n");

    if (!config)
        return CELL_EINVAL;
    config = GUEST_PTR(config, CellAudioOutConfiguration*);

    memset(config, 0, sizeof(CellAudioOutConfiguration));
    config->channel = CELL_AUDIO_OUT_CHNUM_2;
    config->encoder = CELL_AUDIO_OUT_CODING_TYPE_LPCM;

    return CELL_OK;
}

/* cellAudioOutGetState(audioOut, deviceIndex, state) -- the audio sibling of
 * cellVideoOutGetState. A title checks it before configuring output; Virtua
 * Fighter 5 calls it once during boot and it was the last unresolved import in
 * the whole title. The device is always present and enabled here: there is no
 * "no audio device" case a recompiled port can be in.
 *
 * CellAudioOutState { u8 state; u8 encoder; u8 reserved[2]; u32 downMixer;
 *                     CellAudioOutSoundMode soundMode; } -- written big-endian
 * a byte at a time, which needs no struct definition to get right. */
s32 cellAudioOutGetState(u32 audioOut, u32 deviceIndex, void* state)
{
    (void)deviceIndex;
    uint32_t ea = (uint32_t)(uintptr_t)state;
    printf("[cellAvconfExt] AudioOutGetState(audioOut=%u)\n", audioOut);
    if (!ea)
        return CELL_EINVAL;

    vm_write8(ea + 0, 0);                                   /* ENABLED        */
    vm_write8(ea + 1, CELL_AUDIO_OUT_CODING_TYPE_LPCM);
    vm_write8(ea + 2, 0);
    vm_write8(ea + 3, 0);
    vm_write32(ea + 4, 0);                                  /* no down-mixer  */
    vm_write8(ea + 8, CELL_AUDIO_OUT_CODING_TYPE_LPCM);     /* soundMode      */
    vm_write8(ea + 9, CELL_AUDIO_OUT_CHNUM_2);
    vm_write8(ea + 10, CELL_AUDIO_OUT_FS_48KHZ);
    vm_write8(ea + 11, 0);
    vm_write32(ea + 12, 0);
    return CELL_OK;
}

s32 cellAudioOutSetCopyControl(u32 audioOut, u32 control)
{
    (void)audioOut;
    printf("[cellAvconfExt] AudioOutSetCopyControl(control=%u)\n", control);
    return CELL_OK;
}

s32 cellAudioOutGetNumberOfDevice(u32 audioOut)
{
    (void)audioOut;
    return 1; /* one audio output device */
}

s32 cellVideoOutGetGamma(u32 videoOut, float* gamma)
{
    (void)videoOut;
    if (!gamma) return CELL_EINVAL;
    { float g = s_gamma; uint32_t bits; memcpy(&bits, &g, 4);
      vm_write32((uint32_t)(uintptr_t)gamma, bits); }
    return CELL_OK;
}

s32 cellVideoOutSetGamma(u32 videoOut, float gamma)
{
    (void)videoOut;
    printf("[cellAvconfExt] VideoOutSetGamma(%.2f)\n", gamma);
    s_gamma = gamma;
    return CELL_OK;
}

/* cellAudioOutConfigure(audioOut, config, option, waitForEvent) -- set the
 * audio output mode. We accept any config and report success (request id 0);
 * the title queries availability separately. NID 0x4692AB35. */
s32 cellAudioOutConfigure(u32 audioOut, void* config, void* option, u32 waitForEvent)
{
    (void)config; (void)option; (void)waitForEvent;
    printf("[cellAvconfExt] AudioOutConfigure(audioOut=%u) -> ok\n", audioOut);
    return 0;
}
