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
 * PAMF file header structure (big-endian on disc)
 * -----------------------------------------------------------------------*/
typedef struct {
    u32 magic;           /* "PAMF" */
    u32 version;
    u32 dataOffset;      /* offset to mux data */
    u32 dataSize;        /* mux data size */
    u32 reserved1;
    u32 reserved2;
    u32 epOffset;        /* entry point table offset */
    u32 epCount;         /* number of entry points */
    u32 streamCount;     /* number of elementary streams */
    u32 reserved3;
    u64 presentStart;    /* presentation start time (90kHz) */
    u64 presentEnd;      /* presentation end time */
    u32 muxRate;         /* max mux rate in bytes/sec */
} PamfHeader;

/* Stream descriptor within PAMF header */
typedef struct {
    u8  streamType;
    u8  streamIndex;
    u8  streamId;
    u8  privateStreamId;
    u32 epOffset;
    u32 epCount;
    /* Codec-specific data follows */
} PamfStreamDesc;

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
 * Reader lifecycle
 * -----------------------------------------------------------------------*/

s32 cellPamfReaderInitialize(CellPamfReader* reader, void* pamfAddr,
                              u32 pamfSize, u32 attribute)
{
    (void)attribute;

    printf("[cellPamf] ReaderInitialize(addr=%p, size=%u)\n", pamfAddr, pamfSize);

    if (!reader || !pamfAddr || pamfSize < sizeof(PamfHeader))
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const PamfHeader* hdr = (const PamfHeader*)pamfAddr;

    /* Verify magic */
    if (be32(&hdr->magic) != CELL_PAMF_MAGIC) {
        printf("[cellPamf] Invalid PAMF magic: 0x%08X\n", be32(&hdr->magic));
        return (s32)CELL_PAMF_ERROR_INVALID_HEADER;
    }

    reader->pamfAddr = pamfAddr;
    reader->pamfSize = pamfSize;
    reader->dataOffset = be32(&hdr->dataOffset);
    reader->dataSize = be32(&hdr->dataSize);
    reader->numStreams = (u8)be32(&hdr->streamCount);
    reader->currentStream = 0;
    reader->currentEP = 0;

    printf("[cellPamf] PAMF: %u streams, dataOffset=0x%X, dataSize=%u, muxRate=%u\n",
           reader->numStreams, reader->dataOffset, reader->dataSize,
           be32(&hdr->muxRate));

    return CELL_OK;
}

s32 cellPamfReaderGetPresentationStartTime(CellPamfReader* reader, u64* startTime)
{
    if (!reader || !startTime)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const PamfHeader* hdr = (const PamfHeader*)reader->pamfAddr;
    *startTime = be64(&hdr->presentStart);
    return CELL_OK;
}

s32 cellPamfReaderGetPresentationEndTime(CellPamfReader* reader, u64* endTime)
{
    if (!reader || !endTime)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const PamfHeader* hdr = (const PamfHeader*)reader->pamfAddr;
    *endTime = be64(&hdr->presentEnd);
    return CELL_OK;
}

s32 cellPamfReaderGetMuxRateBound(CellPamfReader* reader, u32* muxRate)
{
    if (!reader || !muxRate)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const PamfHeader* hdr = (const PamfHeader*)reader->pamfAddr;
    *muxRate = be32(&hdr->muxRate);
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

/* Get stream descriptor by index */
static const PamfStreamDesc* get_stream_desc(CellPamfReader* reader, u32 index)
{
    if (!reader || index >= reader->numStreams)
        return NULL;

    /* Stream descriptors start at offset 0x80 in the PAMF header (typical) */
    const u8* base = (const u8*)reader->pamfAddr;
    const u8* desc_base = base + 0x80;
    /* Each descriptor is approximately 24 bytes + codec-specific data */
    return (const PamfStreamDesc*)(desc_base + index * 24);
}

s32 cellPamfReaderGetNumberOfSpecificStreams(CellPamfReader* reader, u8 streamType)
{
    if (!reader)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    s32 count = 0;
    for (u32 i = 0; i < reader->numStreams; i++) {
        const PamfStreamDesc* desc = get_stream_desc(reader, i);
        if (desc && desc->streamType == streamType)
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
        const PamfStreamDesc* desc = get_stream_desc(reader, i);
        if (desc && desc->streamType == streamType) {
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

s32 cellPamfReaderGetStreamInfo(CellPamfReader* reader, void* info, u32 infoSize)
{
    if (!reader || !info)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const PamfStreamDesc* desc = get_stream_desc(reader, reader->currentStream);
    if (!desc)
        return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;

    /* Provide basic codec info based on stream type */
    switch (desc->streamType) {
    case CELL_PAMF_STREAM_TYPE_AVC:
        if (infoSize >= sizeof(CellPamfAvcInfo)) {
            CellPamfAvcInfo* avc = (CellPamfAvcInfo*)info;
            memset(avc, 0, sizeof(*avc));
            avc->profileIdc = 100;     /* High profile */
            avc->levelIdc = 41;        /* Level 4.1 */
            avc->horizontalSize = 1280;
            avc->verticalSize = 720;
            avc->frameMbsOnlyFlag = 1;
            avc->frameRateCode = 5;    /* 29.97 fps */
        }
        break;
    case CELL_PAMF_STREAM_TYPE_ATRAC3PLUS:
        if (infoSize >= sizeof(CellPamfAtrac3plusInfo)) {
            CellPamfAtrac3plusInfo* at3 = (CellPamfAtrac3plusInfo*)info;
            at3->samplingFrequency = 48000;
            at3->numberOfChannels = 2;
        }
        break;
    case CELL_PAMF_STREAM_TYPE_PAMF_LPCM:
        if (infoSize >= sizeof(CellPamfLpcmInfo)) {
            CellPamfLpcmInfo* lpcm = (CellPamfLpcmInfo*)info;
            lpcm->samplingFrequency = 48000;
            lpcm->numberOfChannels = 2;
            lpcm->bitsPerSample = 16;
        }
        break;
    case CELL_PAMF_STREAM_TYPE_AC3:
        if (infoSize >= sizeof(CellPamfAc3Info)) {
            CellPamfAc3Info* ac3 = (CellPamfAc3Info*)info;
            ac3->samplingFrequency = 48000;
            ac3->numberOfChannels = 6;
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

    const PamfHeader* hdr = (const PamfHeader*)reader->pamfAddr;
    *numEp = (s32)be32(&hdr->epCount);
    return CELL_OK;
}

s32 cellPamfReaderGetEp(CellPamfReader* reader, u32 epIndex, CellPamfEp* ep)
{
    if (!reader || !ep)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const PamfHeader* hdr = (const PamfHeader*)reader->pamfAddr;
    u32 epCount = be32(&hdr->epCount);
    if (epIndex >= epCount)
        return (s32)CELL_PAMF_ERROR_EP_NOT_FOUND;

    /* EP table entries are at epOffset, each 8 bytes (pts + offset) */
    u32 epTableOff = be32(&hdr->epOffset);
    const u8* epData = (const u8*)reader->pamfAddr + epTableOff + epIndex * 8;
    ep->pts = be32(epData);
    ep->rpnOffset = be32(epData + 4);
    return CELL_OK;
}
