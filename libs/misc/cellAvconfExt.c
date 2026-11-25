/*
 * ps3recomp - cellAvconfExt HLE implementation
 *
 * Reports standard LPCM stereo 48kHz as available audio output.
 * Games query this to decide audio format before opening cellAudio ports.
 */

#include "cellAvconfExt.h"
#include "ps3emu/endian.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write32: translate + byte-swap */
#include "../../runtime/ppu/ppu_context.h"  /* the float argument of SetGamma is in f1 */
#include <stdlib.h>

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
static u8    s_audio_channel   = CELL_AUDIO_OUT_CHNUM_2;
static u8    s_audio_encoder   = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
static u32   s_audio_downmixer = CELL_AUDIO_OUT_DOWNMIXER_NONE;

static u32 audio_layout_for_channels(u8 ch)
{
    if (ch >= CELL_AUDIO_OUT_CHNUM_8) return CELL_AUDIO_OUT_SPEAKER_LAYOUT_8CH_LREClrxy;
    if (ch >= CELL_AUDIO_OUT_CHNUM_6) return CELL_AUDIO_OUT_SPEAKER_LAYOUT_6CH_LREClr;
    return CELL_AUDIO_OUT_SPEAKER_LAYOUT_2CH;
}

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
    info->state = 1;    /* CELL_AUDIO_OUT_DEVICE_STATE_AVAILABLE (verified against RPCS3) */
    info->latency = ps3_bswap16(13);
    info->availableModeCount = 3;
    info->availableModes[0].type    = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
    info->availableModes[0].channel = CELL_AUDIO_OUT_CHNUM_2;
    info->availableModes[0].fs      = CELL_AUDIO_OUT_FS_48KHZ;
    info->availableModes[0].layout  = ps3_bswap32(CELL_AUDIO_OUT_SPEAKER_LAYOUT_2CH);
    info->availableModes[1].type    = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
    info->availableModes[1].channel = CELL_AUDIO_OUT_CHNUM_6;
    info->availableModes[1].fs      = CELL_AUDIO_OUT_FS_48KHZ;
    info->availableModes[1].layout  = ps3_bswap32(CELL_AUDIO_OUT_SPEAKER_LAYOUT_6CH_LREClr);
    info->availableModes[2].type    = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
    info->availableModes[2].channel = CELL_AUDIO_OUT_CHNUM_8;
    info->availableModes[2].fs      = CELL_AUDIO_OUT_FS_48KHZ;
    info->availableModes[2].layout  = ps3_bswap32(CELL_AUDIO_OUT_SPEAKER_LAYOUT_8CH_LREClrxy);

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
    uint32_t cfg_ea = (uint32_t)(uintptr_t)config;
    memset(vm_base + cfg_ea, 0, sizeof(CellAudioOutConfiguration));
    vm_write8(cfg_ea + 0, s_audio_channel);
    vm_write8(cfg_ea + 1, s_audio_encoder);
    vm_write32(cfg_ea + 12, s_audio_downmixer);

    return CELL_OK;
}

/* cellAudioOutGetState(audioOut, deviceIndex, state) -- the audio sibling of
 * cellVideoOutGetState. A title checks it before configuring output; Virtua
 * Fighter 5 calls it once during boot and it was the last unresolved import in
 * the whole title. The device is always present and enabled here: there is no
 * "no audio device" case a recompiled port can be in.
 *
 * PS3 CellAudioOutState (20 bytes, big-endian; verified against RPCS3 GDB trace):
 *   +0x00 u8  state        (0 = ENABLED)
 *   +0x01 u8  encoder      (0 = LPCM)
 *   +0x02 u8  reserved[6]  (6 bytes, NOT 2)
 *   +0x08 u32 downMixer    (0 = NONE, 1 = TYPE_A, 2 = TYPE_B)
 *   +0x0C CellAudioOutSoundMode soundMode:
 *         +0x0C u8  type
 *         +0x0D u8  channel
 *         +0x0E u8  fs
 *         +0x0F u8  reserved
 *         +0x10 u32 layout (1 = 2CH, 3 = 6CH, 7 = 8CH) */
s32 cellAudioOutGetState(u32 audioOut, u32 deviceIndex, void* state)
{
    (void)deviceIndex;
    uint32_t ea = (uint32_t)(uintptr_t)state;
    printf("[cellAvconfExt] AudioOutGetState(audioOut=%u)\n", audioOut);
    if (!ea)
        return CELL_EINVAL;

    vm_write8(ea + 0, 0);                                   /* ENABLED        */
    vm_write8(ea + 1, s_audio_encoder);
    for (uint32_t i = 2; i < 8; i++) vm_write8(ea + i, 0);  /* reserved[6]    */
    vm_write32(ea + 8, s_audio_downmixer);                  /* downMixer      */
    vm_write8(ea + 12, s_audio_encoder);                    /* soundMode.type */
    vm_write8(ea + 13, s_audio_channel);                    /* soundMode.ch   */
    vm_write8(ea + 14, CELL_AUDIO_OUT_FS_48KHZ);            /* soundMode.fs   */
    vm_write8(ea + 15, 0);                                  /* soundMode.res  */
    vm_write32(ea + 16, audio_layout_for_channels(s_audio_channel));
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
    if (gamma < 0.8f || gamma > 1.2f) return (s32)0x8002b223;   /* PARAMETER_OUT_OF_RANGE */
    s_gamma = gamma;
    return CELL_OK;
}

/* The gamma is a float, which the PPU ABI passes in f1, not r4: registered
 * through a plain C prototype it arrived as garbage (inFAMOUS 2 logged 0.00)
 * and the game's brightness setting never reached the screen. The context
 * handler reads f1 (avconf_register_ctx, called with the other ctx handlers). */
static void hle_video_out_set_gamma(ppu_context* ctx)
{
    ctx->gpr[3] = (u64)(s64)cellVideoOutSetGamma((u32)ctx->gpr[3], (float)ctx->fpr[1]);
}

void avconf_register_ctx(void)
{
    extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
    ps3_hle_register_ctx(0xC7020F62u, "cellVideoOutSetGamma", hle_video_out_set_gamma);
}

/* The display gamma for the presenter: what the title set (0.8..1.2, 1 by
 * default), or PS3_DISPLAY_GAMMA=<g> to override it. The video out applies
 * it as out = in^gamma, like RPCS3's calibration pass. */
float cellAvconfExt_display_gamma(void)
{
    static float env = -1.0f;
    if (env < 0.0f) { const char* e = getenv("PS3_DISPLAY_GAMMA"); env = e ? (float)atof(e) : 0.0f; }
    return env > 0.0f ? env : s_gamma;
}

/* cellAudioOutConfigure(audioOut, config, option, waitForEvent) -- set the
 * audio output mode. We accept any config and report success (request id 0);
 * the title queries availability separately. NID 0x4692AB35. */
s32 cellAudioOutConfigure(u32 audioOut, void* config, void* option, u32 waitForEvent)
{
    (void)option; (void)waitForEvent;
    uint32_t cfg_ea = (uint32_t)(uintptr_t)config;
    if (cfg_ea) {
        u8 ch = vm_read8(cfg_ea + 0);
        if (ch == CELL_AUDIO_OUT_CHNUM_2 || ch == CELL_AUDIO_OUT_CHNUM_6 || ch == CELL_AUDIO_OUT_CHNUM_8)
            s_audio_channel = ch;
        s_audio_encoder   = vm_read8(cfg_ea + 1);
        s_audio_downmixer = vm_read32(cfg_ea + 12);
    }
    printf("[cellAvconfExt] AudioOutConfigure(audioOut=%u, ch=%u, enc=%u, dm=%u) -> ok\n",
           audioOut, s_audio_channel, s_audio_encoder, s_audio_downmixer);
    return 0;
}
