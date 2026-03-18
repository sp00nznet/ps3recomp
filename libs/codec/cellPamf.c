/*
 * ps3recomp - cellPamf HLE implementation
 *
 * Parses PAMF container headers to extract stream information.
 * The actual PAMF header format is parsed to provide correct stream
 * counts and codec parameters to cellDmux/cellVdec/cellAdec.
 */

#include "cellPamf.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * PAMF file header layout (big-endian on disc)
 *
 * Offset  Size  Description
 * 0x00    4     Magic "PAMF"
 * 0x04    4     Version (typically 0x00040100)
 * 0x08    4     Data offset (header size / start of mux data)
 * 0x0C    4     Data size
 * 0x10    4     Stream descriptor table offset
 * 0x14    2     Number of streams
 * 0x18    4     Presentation start time (90kHz ticks)
 * 0x1C    4     Presentation end time (90kHz ticks)
 * 0x20    4     Mux rate (bytes/sec)
 * 0x24    4     EP table offset
 * 0x28    4     EP count
 *
 * We access these via raw byte offsets rather than a packed struct to
 * avoid alignment/padding issues across compilers.
 * -----------------------------------------------------------------------*/

#define PAMF_OFF_MAGIC            0x00
#define PAMF_OFF_VERSION          0x04
#define PAMF_OFF_DATA_OFFSET      0x08
#define PAMF_OFF_DATA_SIZE        0x0C
#define PAMF_OFF_STREAM_TABLE     0x10
#define PAMF_OFF_NUM_STREAMS      0x14
#define PAMF_OFF_PRESENT_START    0x18
#define PAMF_OFF_PRESENT_END      0x1C
#define PAMF_OFF_MUX_RATE         0x20
#define PAMF_OFF_EP_OFFSET        0x24
#define PAMF_OFF_EP_COUNT         0x28

/* Minimum valid header size (enough to read all fields above) */
#define PAMF_MIN_HEADER_SIZE      0x2C

/* Stream descriptor size (fixed 24 bytes per descriptor) */
#define PAMF_STREAM_DESC_SIZE     24

/* Raw stream type values in the PAMF file (different from SDK constants!) */
#define PAMF_RAW_TYPE_AVC         1
#define PAMF_RAW_TYPE_M2V         2
#define PAMF_RAW_TYPE_ATRAC3PLUS  3
#define PAMF_RAW_TYPE_AC3         4
#define PAMF_RAW_TYPE_LPCM        5
#define PAMF_RAW_TYPE_USERDATA    6

/* ---------------------------------------------------------------------------
 * Endian helpers (PAMF is big-endian)
 * -----------------------------------------------------------------------*/
static u32 be32(const void* p)
{
    const u8* b = (const u8*)p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}

static u64 be64(const void* p)
{
    const u8* b = (const u8*)p;
    return ((u64)be32(b) << 32) | be32(b + 4);
}

static u16 be16(const void* p)
{
    const u8* b = (const u8*)p;
    return ((u16)b[0] << 8) | b[1];
}

/* ---------------------------------------------------------------------------
 * Raw-type to SDK-type conversion
 * -----------------------------------------------------------------------*/
static s32 raw_type_to_sdk(u8 rawType)
{
    switch (rawType) {
    case PAMF_RAW_TYPE_AVC:        return CELL_PAMF_STREAM_TYPE_AVC;
    case PAMF_RAW_TYPE_M2V:        return CELL_PAMF_STREAM_TYPE_M2V;
    case PAMF_RAW_TYPE_ATRAC3PLUS: return CELL_PAMF_STREAM_TYPE_ATRAC3PLUS;
    case PAMF_RAW_TYPE_AC3:        return CELL_PAMF_STREAM_TYPE_AC3;
    case PAMF_RAW_TYPE_LPCM:       return CELL_PAMF_STREAM_TYPE_PAMF_LPCM;
    case PAMF_RAW_TYPE_USERDATA:   return CELL_PAMF_STREAM_TYPE_USER_DATA;
    default:                       return -1;
    }
}

/* ---------------------------------------------------------------------------
 * Reader lifecycle
 * -----------------------------------------------------------------------*/

s32 cellPamfReaderInitialize(CellPamfReader* reader, void* pamfAddr,
                              u32 pamfSize, u32 attribute)
{
    (void)attribute;

    printf("[cellPamf] ReaderInitialize(addr=%p, size=%u)\n", pamfAddr, pamfSize);

    if (!reader || !pamfAddr || pamfSize < PAMF_MIN_HEADER_SIZE)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* base = (const u8*)pamfAddr;

    /* Verify magic */
    u32 magic = be32(base + PAMF_OFF_MAGIC);
    if (magic != CELL_PAMF_MAGIC) {
        printf("[cellPamf] Invalid PAMF magic: 0x%08X\n", magic);
        return (s32)CELL_PAMF_ERROR_INVALID_HEADER;
    }

    reader->pamfAddr = pamfAddr;
    reader->pamfSize = pamfSize;
    reader->dataOffset = be32(base + PAMF_OFF_DATA_OFFSET);
    reader->dataSize = be32(base + PAMF_OFF_DATA_SIZE);
    reader->streamTableOffset = be32(base + PAMF_OFF_STREAM_TABLE);
    reader->numStreams = (u8)be16(base + PAMF_OFF_NUM_STREAMS);
    reader->currentStream = 0;
    reader->currentEP = 0;

    u32 muxRate = be32(base + PAMF_OFF_MUX_RATE);

    printf("[cellPamf] PAMF v%08X: %u streams, dataOffset=0x%X, dataSize=%u, "
           "streamTableOff=0x%X, muxRate=%u\n",
           be32(base + PAMF_OFF_VERSION),
           reader->numStreams, reader->dataOffset, reader->dataSize,
           reader->streamTableOffset, muxRate);

    /* Log each stream descriptor */
    for (u32 i = 0; i < reader->numStreams; i++) {
        const u8* desc = base + reader->streamTableOffset + i * PAMF_STREAM_DESC_SIZE;
        u8 rawType = desc[0];
        u8 channel = desc[1];
        s32 sdkType = raw_type_to_sdk(rawType);
        printf("[cellPamf]   stream[%u]: rawType=%u (sdk=%d) channel=%u\n",
               i, rawType, sdkType, channel);
    }

    return CELL_OK;
}

s32 cellPamfReaderGetPresentationStartTime(CellPamfReader* reader, u64* startTime)
{
    if (!reader || !startTime)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* base = (const u8*)reader->pamfAddr;
    *startTime = (u64)be32(base + PAMF_OFF_PRESENT_START);
    return CELL_OK;
}

s32 cellPamfReaderGetPresentationEndTime(CellPamfReader* reader, u64* endTime)
{
    if (!reader || !endTime)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* base = (const u8*)reader->pamfAddr;
    *endTime = (u64)be32(base + PAMF_OFF_PRESENT_END);
    return CELL_OK;
}

s32 cellPamfReaderGetMuxRateBound(CellPamfReader* reader, u32* muxRate)
{
    if (!reader || !muxRate)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* base = (const u8*)reader->pamfAddr;
    *muxRate = be32(base + PAMF_OFF_MUX_RATE);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Stream queries
 * -----------------------------------------------------------------------*/

s32 cellPamfReaderGetNumberOfStreams(CellPamfReader* reader)
{
    if (!reader)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    return reader->numStreams;
}

/* Get pointer to raw stream descriptor bytes by absolute index */
static const u8* get_stream_desc_raw(CellPamfReader* reader, u32 index)
{
    if (!reader || index >= reader->numStreams)
        return NULL;

    const u8* base = (const u8*)reader->pamfAddr;
    return base + reader->streamTableOffset + index * PAMF_STREAM_DESC_SIZE;
}

/* Get SDK stream type for a given absolute stream index */
static s32 get_stream_sdk_type(CellPamfReader* reader, u32 index)
{
    const u8* desc = get_stream_desc_raw(reader, index);
    if (!desc) return -1;
    return raw_type_to_sdk(desc[0]);
}

s32 cellPamfReaderGetNumberOfSpecificStreams(CellPamfReader* reader, u8 streamType)
{
    if (!reader)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    s32 count = 0;
    for (u32 i = 0; i < reader->numStreams; i++) {
        if (get_stream_sdk_type(reader, i) == (s32)streamType)
            count++;
    }
    return count;
}

s32 cellPamfReaderSetStreamWithType(CellPamfReader* reader, u8 streamType, u32 streamIndex)
{
    if (!reader)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    u32 found = 0;
    for (u32 i = 0; i < reader->numStreams; i++) {
        if (get_stream_sdk_type(reader, i) == (s32)streamType) {
            if (found == streamIndex) {
                reader->currentStream = (u8)i;
                return CELL_OK;
            }
            found++;
        }
    }
    return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;
}

s32 cellPamfReaderSetStreamWithTypeAndChannel(CellPamfReader* reader, u8 streamType, u32 channel)
{
    return cellPamfReaderSetStreamWithType(reader, streamType, channel);
}

s32 cellPamfReaderSetStreamWithIndex(CellPamfReader* reader, u32 streamIndex)
{
    if (!reader)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    if (streamIndex >= reader->numStreams)
        return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;

    reader->currentStream = (u8)streamIndex;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Sample rate / channel lookup helpers for audio descriptors
 * -----------------------------------------------------------------------*/
static u32 audio_sample_rate(u8 code)
{
    switch (code) {
    case 1: return 44100;
    case 2: return 48000;
    default: return 48000;
    }
}

static u8 audio_channels(u8 code)
{
    switch (code) {
    case 1: return 1;
    case 2: return 2;
    case 6: return 6;
    case 8: return 8;
    default: return 2;
    }
}

static u8 lpcm_bits(u8 code)
{
    switch (code) {
    case 1: return 16;
    case 2: return 24;
    default: return 16;
    }
}

/* ---------------------------------------------------------------------------
 * Stream info extraction — reads real codec parameters from descriptors
 *
 * Stream descriptor layout (24 bytes, big-endian):
 *   Byte  0:    stream type (raw: 1=AVC, 2=M2V, 3=ATRAC3+, 4=AC3, 5=LPCM)
 *   Byte  1:    channel index
 *   For AVC (type 1):
 *     Byte  2:  profileIdc (66=baseline, 77=main, 100=high)
 *     Byte  3:  levelIdc
 *     Byte  4:  frameMbsOnlyFlag (bit 7), videoSignalInfoFlag (bit 6)
 *     Byte  5:  frameRateCode (upper nibble), aspectRatioIdc (lower nibble)
 *     Bytes 6-7:   sarWidth  (be16)
 *     Bytes 8-9:   sarHeight (be16)
 *     Bytes 14-15:  horizontalSize (be16)
 *     Bytes 16-17:  verticalSize   (be16)
 *     Bytes 18-19:  frameCropLeft/Right
 *     Bytes 20-21:  frameCropTop/Bottom
 *   For ATRAC3+ (type 3) / AC3 (type 4):
 *     Byte 10:  channel configuration
 *     Byte 12:  sample rate code
 *   For LPCM (type 5):
 *     Byte 10:  channel configuration
 *     Byte 11:  bits-per-sample code
 *     Byte 12:  sample rate code
 * -----------------------------------------------------------------------*/

s32 cellPamfReaderGetStreamInfo(CellPamfReader* reader, void* info, u32 infoSize)
{
    if (!reader || !info)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* desc = get_stream_desc_raw(reader, reader->currentStream);
    if (!desc)
        return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;

    s32 sdkType = raw_type_to_sdk(desc[0]);

    switch (sdkType) {
    case CELL_PAMF_STREAM_TYPE_AVC:
        if (infoSize >= sizeof(CellPamfAvcInfo)) {
            CellPamfAvcInfo* avc = (CellPamfAvcInfo*)info;
            memset(avc, 0, sizeof(*avc));
            avc->profileIdc            = desc[2];
            avc->levelIdc              = desc[3];
            avc->frameMbsOnlyFlag      = (desc[4] >> 7) & 1;
            avc->videoSignalInfoFlag   = (desc[4] >> 6) & 1;
            avc->frameRateCode         = (desc[5] >> 4) & 0x0F;
            avc->aspectRatioIdc        = desc[5] & 0x0F;
            avc->sarWidth              = be16(desc + 6);
            avc->sarHeight             = be16(desc + 8);
            avc->horizontalSize        = be16(desc + 14);
            avc->verticalSize          = be16(desc + 16);
            avc->frameCropLeftOffset   = desc[18];
            avc->frameCropRightOffset  = desc[19];
            avc->frameCropTopOffset    = desc[20];
            avc->frameCropBottomOffset = desc[21];

            printf("[cellPamf] AVC info: profile=%u level=%u %ux%u "
                   "frameRate=%u aspect=%u\n",
                   avc->profileIdc, avc->levelIdc,
                   avc->horizontalSize, avc->verticalSize,
                   avc->frameRateCode, avc->aspectRatioIdc);
        }
        break;

    case CELL_PAMF_STREAM_TYPE_ATRAC3PLUS:
        if (infoSize >= sizeof(CellPamfAtrac3plusInfo)) {
            CellPamfAtrac3plusInfo* at3 = (CellPamfAtrac3plusInfo*)info;
            memset(at3, 0, sizeof(*at3));
            at3->numberOfChannels  = audio_channels(desc[10]);
            at3->samplingFrequency = audio_sample_rate(desc[12]);

            printf("[cellPamf] ATRAC3+ info: %u ch, %u Hz\n",
                   at3->numberOfChannels, at3->samplingFrequency);
        }
        break;

    case CELL_PAMF_STREAM_TYPE_PAMF_LPCM:
        if (infoSize >= sizeof(CellPamfLpcmInfo)) {
            CellPamfLpcmInfo* lpcm = (CellPamfLpcmInfo*)info;
            memset(lpcm, 0, sizeof(*lpcm));
            lpcm->numberOfChannels  = audio_channels(desc[10]);
            lpcm->bitsPerSample     = lpcm_bits(desc[11]);
            lpcm->samplingFrequency = audio_sample_rate(desc[12]);

            printf("[cellPamf] LPCM info: %u ch, %u Hz, %u bits\n",
                   lpcm->numberOfChannels, lpcm->samplingFrequency,
                   lpcm->bitsPerSample);
        }
        break;

    case CELL_PAMF_STREAM_TYPE_AC3:
        if (infoSize >= sizeof(CellPamfAc3Info)) {
            CellPamfAc3Info* ac3 = (CellPamfAc3Info*)info;
            memset(ac3, 0, sizeof(*ac3));
            ac3->numberOfChannels  = audio_channels(desc[10]);
            ac3->samplingFrequency = audio_sample_rate(desc[12]);

            printf("[cellPamf] AC3 info: %u ch, %u Hz\n",
                   ac3->numberOfChannels, ac3->samplingFrequency);
        }
        break;

    default:
        return (s32)CELL_PAMF_ERROR_NOT_AVAILABLE;
    }

    return CELL_OK;
}

s32 cellPamfReaderGetStreamIndex(CellPamfReader* reader)
{
    if (!reader)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    return reader->currentStream;
}

s32 cellPamfStreamTypeToEsFilterId(u8 streamType, u8 streamIndex,
                                     void* esFilterId)
{
    (void)streamType; (void)streamIndex;
    if (!esFilterId)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    /* Set filter ID bytes (type + index) */
    u8* id = (u8*)esFilterId;
    id[0] = streamType;
    id[1] = streamIndex;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Entry point navigation
 * -----------------------------------------------------------------------*/

s32 cellPamfReaderGetNumberOfEp(CellPamfReader* reader, s32* numEp)
{
    if (!reader || !numEp)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* base = (const u8*)reader->pamfAddr;
    *numEp = (s32)be32(base + PAMF_OFF_EP_COUNT);
    return CELL_OK;
}

s32 cellPamfReaderGetEp(CellPamfReader* reader, u32 epIndex, CellPamfEp* ep)
{
    if (!reader || !ep)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* base = (const u8*)reader->pamfAddr;
    u32 epCount = be32(base + PAMF_OFF_EP_COUNT);
    if (epIndex >= epCount)
        return (s32)CELL_PAMF_ERROR_EP_NOT_FOUND;

    /* EP table entries are at epOffset, each 8 bytes (pts + offset) */
    u32 epTableOff = be32(base + PAMF_OFF_EP_OFFSET);
    const u8* epData = base + epTableOff + epIndex * 8;
    ep->pts = be32(epData);
    ep->rpnOffset = be32(epData + 4);
    return CELL_OK;
}
