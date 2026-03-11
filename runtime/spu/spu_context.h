/*
 * ps3recomp - SPU (Synergistic Processing Unit) execution context
 *
 * Models the full architectural state of an SPU:
 *   - 128 x 128-bit general-purpose registers
 *   - 256 KB local store
 *   - Channel state (MFC command queue, mailboxes, signal notification)
 */

#ifndef SPU_CONTEXT_H
#define SPU_CONTEXT_H

#include "../../include/ps3emu/ps3types.h"
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Local store size: 256 KB */
#define SPU_LS_SIZE         (256 * 1024)
#define SPU_LS_MASK         (SPU_LS_SIZE - 1)

/* Maximum number of MFC tag groups */
#define SPU_MFC_MAX_TAGS    32

/* Mailbox / signal capacities */
#define SPU_MBOX_DEPTH      1    /* SPU write outbound mailbox depth */
#define SPU_INTR_MBOX_DEPTH 1    /* SPU write inbound interrupt mailbox depth */

/* ---------------------------------------------------------------------------
 * SPU channel IDs
 * -----------------------------------------------------------------------*/
#define SPU_RdEventStat     0
#define SPU_WrEventMask     1
#define SPU_WrEventAck      2
#define SPU_RdSigNotify1    3
#define SPU_RdSigNotify2    4
#define SPU_WrDec           7
#define SPU_RdDec           8
#define SPU_RdEventMask     11
#define SPU_RdMachStat      13
#define SPU_WrSRR0          14
#define SPU_RdSRR0          15
#define SPU_WrOutMbox       28
#define SPU_RdInMbox        29
#define SPU_WrOutIntrMbox   30

/* MFC channels */
#define MFC_WrMSSyncReq     9
#define MFC_RdTagMask       12
#define MFC_LSA             16
#define MFC_EAH             17
#define MFC_EAL             18
#define MFC_Size            19
#define MFC_TagID           20
#define MFC_Cmd             21
#define MFC_WrTagMask       22
#define MFC_WrTagUpdate     23
#define MFC_RdTagStat       24
#define MFC_RdListStallStat 25
#define MFC_WrListStallAck  26
#define MFC_RdAtomicStat    27

/* ---------------------------------------------------------------------------
 * MFC DMA command opcodes
 * -----------------------------------------------------------------------*/
#define MFC_PUT_CMD         0x20
#define MFC_PUTB_CMD        0x21
#define MFC_PUTF_CMD        0x22
#define MFC_GET_CMD         0x40
#define MFC_GETB_CMD        0x41
#define MFC_GETF_CMD        0x42
#define MFC_PUTL_CMD        0x24
#define MFC_PUTLB_CMD       0x25
#define MFC_PUTLF_CMD       0x26
#define MFC_GETL_CMD        0x44
#define MFC_GETLB_CMD       0x45
#define MFC_GETLF_CMD       0x46
#define MFC_SNDSIG_CMD      0xA0
#define MFC_BARRIER_CMD     0xC0
#define MFC_EIEIO_CMD       0xC8
#define MFC_SYNC_CMD        0xCC

/* ---------------------------------------------------------------------------
 * Channel state
 * -----------------------------------------------------------------------*/
typedef struct spu_channel {
    uint32_t value;
    uint32_t count;   /* number of valid entries (0 or 1 for most channels) */
} spu_channel;

/* ---------------------------------------------------------------------------
 * SPU execution context
 * -----------------------------------------------------------------------*/
typedef struct spu_context {
    /* 128 general-purpose 128-bit registers */
    u128     gpr[128] __attribute__((aligned(16)));

    /* 256 KB local store, 16-byte aligned */
    uint8_t  ls[SPU_LS_SIZE] __attribute__((aligned(16)));

    /* Program counter (local store address, 0-0x3FFFF) */
    uint32_t pc;

    /* SPU status (running, stopped, etc.) */
    uint32_t status;
    #define SPU_STATUS_STOPPED      0x0
    #define SPU_STATUS_RUNNING      0x1
    #define SPU_STATUS_STOPPED_BY_STOP  0x2
    #define SPU_STATUS_STOPPED_BY_HALT  0x4
    #define SPU_STATUS_WAITING_CHANNEL  0x8
    #define SPU_STATUS_SINGLE_STEP  0x10

    /* SPU thread identification */
    uint32_t spu_id;
    uint32_t spu_group_id;

    /* Decrementer (a free-running down counter) */
    uint32_t decrementer;

    /* SRR0 - Save/Restore Register (exception return address) */
    uint32_t srr0;

    /* Event status / mask */
    uint32_t event_status;
    uint32_t event_mask;

    /* Channels */
    spu_channel ch_out_mbox;        /* SPU -> PPU outbound mailbox */
    spu_channel ch_in_mbox;         /* PPU -> SPU inbound mailbox */
    spu_channel ch_out_intr_mbox;   /* SPU -> PPU interrupt mailbox */
    spu_channel ch_sig_notify[2];   /* Signal notification 1 & 2 */

    /* MFC command staging registers (written via channels before issuing cmd) */
    uint32_t mfc_lsa;
    uint32_t mfc_eah;
    uint32_t mfc_eal;
    uint32_t mfc_size;
    uint32_t mfc_tag;

    /* MFC tag completion mask and status */
    uint32_t mfc_tag_mask;
    uint32_t mfc_tag_status;

} spu_context;

/* ---------------------------------------------------------------------------
 * Initialization
 * -----------------------------------------------------------------------*/
static inline void spu_context_init(spu_context* ctx, uint32_t spu_id)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->spu_id = spu_id;
    ctx->status = SPU_STATUS_STOPPED;
}

/* ---------------------------------------------------------------------------
 * Local store access helpers
 * -----------------------------------------------------------------------*/
static inline uint8_t* spu_ls_ptr(spu_context* ctx, uint32_t lsa)
{
    return &ctx->ls[lsa & SPU_LS_MASK];
}

static inline uint32_t spu_ls_read32(const spu_context* ctx, uint32_t lsa)
{
    lsa &= SPU_LS_MASK;
    const uint8_t* p = &ctx->ls[lsa];
    /* SPU local store is big-endian */
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline void spu_ls_write32(spu_context* ctx, uint32_t lsa, uint32_t val)
{
    lsa &= SPU_LS_MASK;
    uint8_t* p = &ctx->ls[lsa];
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >> 8);
    p[3] = (uint8_t)(val);
}

static inline u128 spu_ls_read128(const spu_context* ctx, uint32_t lsa)
{
    u128 v;
    lsa &= SPU_LS_MASK & ~0xFu; /* align to 16 */
    memcpy(&v, &ctx->ls[lsa], 16);
    return v;
}

static inline void spu_ls_write128(spu_context* ctx, uint32_t lsa, u128 val)
{
    lsa &= SPU_LS_MASK & ~0xFu;
    memcpy(&ctx->ls[lsa], &val, 16);
}

/* ---------------------------------------------------------------------------
 * Preferred slot extraction
 *
 * SPU instructions operate on the "preferred slot" of a 128-bit register,
 * which is the leftmost (highest-address in big-endian) element.
 * For word operations, preferred slot = element 0 of the _u32 array
 * (since our u128 stores big-endian element order).
 * -----------------------------------------------------------------------*/
static inline uint32_t spu_preferred_u32(const u128* reg)
{
    return reg->_u32[0];
}

static inline int32_t spu_preferred_s32(const u128* reg)
{
    return reg->_s32[0];
}

static inline float spu_preferred_f32(const u128* reg)
{
    return reg->_f32[0];
}

static inline uint64_t spu_preferred_u64(const u128* reg)
{
    return reg->_u64[0];
}

/* Create a register with a value splatted to the preferred word slot */
static inline u128 spu_make_preferred_u32(uint32_t val)
{
    u128 r;
    memset(&r, 0, sizeof(r));
    r._u32[0] = val;
    return r;
}

/* ---------------------------------------------------------------------------
 * Channel read/write helpers
 * -----------------------------------------------------------------------*/
static inline void spu_channel_write(spu_channel* ch, uint32_t val)
{
    ch->value = val;
    ch->count = 1;
}

static inline uint32_t spu_channel_read(spu_channel* ch)
{
    uint32_t val = ch->value;
    ch->count = 0;
    return val;
}

static inline int spu_channel_has_data(const spu_channel* ch)
{
    return ch->count > 0;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPU_CONTEXT_H */
