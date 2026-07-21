/*
 * ps3recomp - PPU HLE bridge (NID -> host function dispatch)
 *
 * Connects the recompiled game's firmware imports to our HLE C libraries.
 * The game calls an imported function through its import stub; the lifter
 * emits `ps3_hle_call(<NID>, ctx)` for those addresses (see ppu_lifter.py
 * --imports). This resolves the NID to a registered HLE handler and marshals
 * the PPC calling convention into a native C call.
 *
 * PPC64 ELFv1 integer/pointer ABI: arguments in r3..r10 (gpr[3..10]), return
 * value in r3. The generic adapter casts the handler to a uint64-in/uint64-out
 * function and passes the 8 GPR argument slots; this covers the large majority
 * of cellXxx APIs (integer args/handles, s32 return). Functions that take or
 * return *pointers* need host<->guest address translation and so require a
 * per-function wrapper -- the generic path passes the raw value through.
 *
 * Compiled as C++ (matches the lifted output). Game-agnostic.
 */
#include "ppu_recomp.h"   /* ppu_context */
#include "ps3emu/nid.h"   /* ps3_nid_table, ps3_nid_entry */
#include <stdlib.h>       /* getenv */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>       /* getenv, atoi (boot trace) */

/* Single flat NID -> handler table (all modules share it; resolution is by
 * NID which is globally unique). Sized for the firmware import surface. */
#define HLE_NID_CAP 4096
static ps3_nid_entry  g_hle_storage[HLE_NID_CAP];
static ps3_nid_table  g_hle_nids;
static int            g_hle_inited = 0;

extern "C" void ps3_hle_register(uint32_t nid, const char* name, void* handler)
{
    if (!g_hle_inited) { ps3_nid_table_init(&g_hle_nids, g_hle_storage, HLE_NID_CAP); g_hle_inited = 1; }
    ps3_nid_table_add(&g_hle_nids, nid, name, handler);
}

extern "C" uint32_t ps3_hle_count(void) { return g_hle_inited ? g_hle_nids.count : 0; }

/* Context-aware handlers: functions that need the full ppu_context (to read
 * args beyond the generic ABI, set registers like r13, touch memory, etc.).
 * Registered separately and dispatched before the generic table. */
typedef void (*hle_ctx_fn)(ppu_context*);
#define HLE_CTX_CAP 256
static struct { uint32_t nid; hle_ctx_fn fn; } g_ctx[HLE_CTX_CAP];
static uint32_t g_ctx_count = 0;

extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, hle_ctx_fn fn)
{
    (void)name;
    if (g_ctx_count < HLE_CTX_CAP) { g_ctx[g_ctx_count].nid = nid; g_ctx[g_ctx_count].fn = fn; g_ctx_count++; }
}

/* Generic PPC integer/pointer ABI adapter. */
typedef uint64_t (*hle_generic)(uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t, uint64_t);

/* Host VM store (defined in ppu_loader.cpp) — used for the TOC save below. */
void vm_write64(uint64_t addr, uint64_t val);

/* Breadcrumb for the crash reporter: the last firmware import dispatched, so a
 * host AV inside an HLE handler names the culprit NID/function. */
extern "C" uint32_t    g_last_hle_nid  = 0;
extern "C" const char* g_last_hle_name = "";

/* Real-PRX bridge: a loaded system PRX (libsre = cellSpurs/cellSync) may export
 * this NID. If so, dispatch into the REAL recompiled Sony code (its OPD -> our
 * indirect dispatcher -> the registered lifted libsre function) instead of the
 * HLE stub. prx_resolve_export returns 0 when no PRX exports the NID, so this is
 * a no-op when no PRX is loaded. */
extern "C" uint32_t prx_resolve_export(uint32_t nid);
extern "C" void     ps3_indirect_call(ppu_context* ctx);
extern "C" uint32_t vm_read32(uint64_t a);
extern "C" void     vm_write32(uint64_t a, uint32_t v);
extern "C" uint64_t ppu_guest_call(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
/* Weak: builds that don't link cellGcmSys.c (test harnesses) get a null and skip. */
extern "C" __attribute__((weak)) void ppu_gcm_pump(void);
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleA(const char*);
static inline void* ps3_GetModuleHandleA(const char* m){ return GetModuleHandleA(m); }

extern "C" void ps3_hle_call(uint32_t nid, ppu_context* ctx)
{
    /* Preserve the caller TOC (r2) across the HLE call. ELFv1 makes r2 caller-saved
     * across a cross-module call: the glink stub does `std r2,40(r1)` before jumping and
     * the caller does `ld r2,40(r1)` after. Our HLE import stubs (`ps3_hle_call(nid);
     * return;`) skip that, AND some handlers (cellSpurs* -> 0xAA6269A8 etc.) dispatch
     * into lifted libsre_ns code that returns with r2 = libsre's TOC (0x30039AB0) AND
     * clobbers the caller's reserved TOC slot at 0x28(r1) to 0. The caller's post-call
     * `ld r2,0x28(r1)` then loads 0 -> r2=0 -> every later TOC-relative load reads a null
     * base (e.g. func_002B03AC's `*(r2-0x4B34)` FMOD list head -> null -> infinite walk
     * -> no draws). Snapshot the caller r2+sp now and restore BOTH the live r2 and the
     * ABI slot on every exit path (offset 40 = 0x28 is the reserved TOC doubleword). */
    struct _TocGuard { ppu_context* c; uint64_t toc, sp;
        ~_TocGuard(){ c->gpr[2] = toc; vm_write64(sp + 0x28, toc); } } _tg{ ctx, ctx->gpr[2], ctx->gpr[1] };
    /* Deliver any pending vblank/flip tick on THIS (guest) thread, serialized with
     * guest execution -- the vblank ticker only marks ticks pending; the handlers
     * run here so guest code never executes concurrently on the ticker thread. */
    if (ppu_gcm_pump) ppu_gcm_pump();

    /* YDKJ_TUNERFIX (at the top so it wins over any registered handler): the
     * profiler-presence query sysPrxForUser 0xE0998DBF, called by libsre
     * _cellSpursIsLaunchedFromTuner (0x3000D318), must return 0x8001112E ("profiler not
     * loaded") on a normal run. Anything else trips the usertrace.c:123 assert AND makes
     * the fn report "launched from tuner", so the caller runs tuner/trace setup that fails
     * with CELL_SPURS_CORE_ERROR_STAT -> the SPURS task workload never attaches. */
    if (nid == 0xE0998DBFu && getenv("YDKJ_TUNERFIX")) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8001112E;
        static int _tn = 0; if (_tn++ < 3)
            fprintf(stderr, "[TUNERFIX] sysPrxForUser 0xE0998DBF -> 0x8001112E (profiler not loaded)\n");
        return;
    }
    g_last_hle_nid = nid;
    /* GFX-SCAN: is the menu .gfx ever inflated into guest RAM? (magic 'GFX'=47 46 58) */
    { static long _c=0; if(getenv("YDKJ_GFXSCAN") && (++_c % 200000)==0){ extern uint8_t* vm_base;
        int gfx=0,cfx=0,swf=0,dds=0,png=0; for(uint32_t a=0x10000; a<0x0FF00000u; a++){
          uint8_t m0=vm_base[a],m1=vm_base[a+1],m2=vm_base[a+2],vv=vm_base[a+3];
          /* uncompressed GFx: 'GFX'/'GFC' ver 4..20 */
          if(m0==0x47&&m1==0x46&&(m2==0x58||m2==0x43)&&(vv>=4&&vv<=20)){ if(gfx<3)fprintf(stderr,"[GFX-SCAN] GFX @0x%08X %c%c%c ver=%d\n",a,m0,m1,m2,vv); gfx++; }
          /* compressed GFx: 'CFX' (zlib) */
          else if(m0==0x43&&m1==0x46&&m2==0x58&&(vv>=4&&vv<=20)){ if(cfx<3)fprintf(stderr,"[GFX-SCAN] CFX(zlib) @0x%08X ver=%d\n",a,vv); cfx++; }
          /* SWF: 'FWS'(raw) / 'CWS'(zlib) / 'ZWS'(lzma) */
          else if((m0==0x46||m0==0x43||m0==0x5A)&&m1==0x57&&m2==0x53&&(vv>=4&&vv<=20)){ if(swf<3)fprintf(stderr,"[GFX-SCAN] SWF @0x%08X %c%c%c ver=%d\n",a,m0,m1,m2,vv); swf++; }
          else if(m0==0x44&&m1==0x44&&m2==0x53&&vv==0x20){ dds++; }
          else if(m0==0x89&&m1==0x50&&m2==0x4E&&vv==0x47){ png++; } }
        fprintf(stderr,"[GFX-SCAN #%ld] GFX=%d CFX=%d SWF=%d DDS=%d PNG=%d\n",_c/200000,gfx,cfx,swf,dds,png); fflush(stderr); } }

    /* Boot trace: log the first N HLE calls (PS3_HLE_TRACE=N). Invaluable for
     * new-SDK bring-up (e.g. PSL1GHT) where the failure is "nothing happens". */
    static int s_trace = -2;
    if (s_trace == -2) { const char* e = getenv("PS3_HLE_TRACE"); s_trace = e ? atoi(e) : 0; }
    if (s_trace > 0) {
        s_trace--;
        fprintf(stderr, "[HLETRACE] nid=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X lr=0x%08X\n",
                nid, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
                (uint32_t)ctx->gpr[5], (uint32_t)ctx->lr);
    }
    /* Bad-lock locator (FLOW_BADLOCK): sys_lwmutex_lock (0x1573DC3F) on a garbage/null
     * object (r3 < 0x10000 or high-bit set) in m_InitEntityHierarchy — host backtrace
     * to find the null global it dereferences. Map RVAs via flow.map. */
    { static int _bl=-1; if(_bl<0)_bl=getenv("FLOW_BADLOCK")?1:0;
      if(_bl && nid==0x1573DC3Fu){ uint32_t r3=(uint32_t)ctx->gpr[3];
        if(r3<0x00010000u || r3>=0x80000000u){ static int _n=0; if(_n++<3){
#ifdef _WIN32
          extern unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long,unsigned long,void**,unsigned long*);
          void* bt[44]; unsigned short fr=RtlCaptureStackBackTrace(0,44,bt,0);
          (void)bt;(void)fr;
          /* Guest-stack code-ptr scan (EXACT: a lifted func name IS its guest addr) —
           * the accurate way to recover the caller chain (host-bt nearest-symbol lies). */
          char ln[1500]; int p=snprintf(ln,sizeof ln,"[BADLOCK] r3=0x%08X guest-codeptrs:",r3);
          uint32_t sp=(uint32_t)ctx->gpr[1]; uint32_t prev=0; int found=0;
          for(uint32_t a=sp; a<sp+0x1800 && a<0x0FF00000u && found<40; a+=4){ uint32_t v=vm_read32(a);
            if(((v>=0x00010000u&&v<0x00900000u)||(v>=0x30000000u&&v<0x30100000u)) && v!=0x008969A8u){
              if(v!=prev){ p+=snprintf(ln+p,sizeof(ln)-p," %08X",v); prev=v; found++; } } }
          fprintf(stderr,"%s\n",ln);
          /* dump the allocator pool table: base ptr at TOC-0x315C (0x008969A8-0x315C=0x0089384C),
           * entries table[0..4]. Tells us if pool[1] alone is null (corruption) or many (init-miss). */
          { uint32_t tbase=vm_read32(0x0089384Cu); uint32_t cnt_ptr=vm_read32(0x00893848u);
            fprintf(stderr,"[BADLOCK] pooltable base=0x%08X state=0x%08X count=0x%08X entries:",
              tbase,cnt_ptr,cnt_ptr?vm_read32(cnt_ptr):0);
            for(int i=0;i<5;i++) fprintf(stderr," [%d]=0x%08X",i,tbase?vm_read32(tbase+i*4):0);
            fprintf(stderr,"\n"); }
#endif
        } } } }
    /* Spin-loop locator: print the guest LR for any HLE call whose r3 lands in the
     * lwmutex-spin object region (0x0275E000-0x02761000). Env FLOW_SPINLR. */
    { static int _sl=-1; if(_sl<0)_sl=getenv("FLOW_SPINLR")?1:0;
      if(_sl){ uint32_t r3=(uint32_t)ctx->gpr[3];
        if(nid==0x2F85C0EFu && r3>=0x02740000u && r3<0x02780000u){ static int _n=0; if(_n++<3){
          /* The DRAIN/fragment model leaves the standard LR slots zero, so scan the
           * guest stack for any word in the lifted code range (game 0x10000-0x900000,
           * libsre 0x30000000-0x30100000) — those are saved return addresses, and a
           * lifted func name IS its guest addr (func_XXXXXXXX), so they map directly. */
          char buf[1400]; int p=snprintf(buf,sizeof buf,"[SPINLR] lwmutex_create r3=0x%08X sp=0x%08X codeptrs:",r3,(uint32_t)ctx->gpr[1]);
          uint32_t sp=(uint32_t)ctx->gpr[1]; uint32_t prev=0; int found=0;
          for(uint32_t a=sp; a<sp+0x2400 && a<0x0FF00000u && found<48; a+=4){ uint32_t v=vm_read32(a);
            if((v>=0x00010000u&&v<0x00900000u)||(v>=0x30000000u&&v<0x30100000u)){
              if(v!=prev){ p+=snprintf(buf+p,sizeof(buf)-p," %08X",v); prev=v; found++; } } }
          fprintf(stderr,"%s\n",buf); } } } }
    { static int tr=-1; if(tr<0){const char*e=getenv("FLOW_HLETRACE"); tr=e?1:0;}
      if(tr) fprintf(stderr,"[hletrace] nid=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X\n",
          nid,(uint32_t)ctx->gpr[3],(uint32_t)ctx->gpr[4],(uint32_t)ctx->gpr[5],(uint32_t)ctx->gpr[6]); }
    /* sys_process_exit (abort path): dump the guest back-chain so we see WHO aborted. */
    if (nid == 0xE6F2C1E7u && getenv("FLOW_EXITCHAIN")) {
        uint32_t sp = (uint32_t)ctx->gpr[1];
        fprintf(stderr, "[exit-chain] code=0x%X lr=0x%08X sp=0x%08X\n", (uint32_t)ctx->gpr[3], (uint32_t)ctx->lr, sp);
        /* Scan the guest stack for words in the lifted code range — saved return
         * addresses; a lifted func name IS its guest addr. (back-chain LR slots are
         * 0 under the DRAIN/fragment model, so scan instead of walk.) */
        { uint32_t prev=0; int found=0; char b[1600]; int p=snprintf(b,sizeof b,"[exit-chain] codeptrs:");
          for(uint32_t a=sp; a<sp+0x3000 && a<0x0FF00000u && found<60; a+=4){ uint32_t v=vm_read32(a);
            if(((v>=0x00010000u&&v<0x00900000u)||(v>=0x30000000u&&v<0x30100000u)) && v!=0x008969A8u){
              if(v!=prev){ p+=snprintf(b+p,sizeof(b)-p," %08X",v); prev=v; found++; } } }
          fprintf(stderr,"%s\n",b); }
#ifdef _WIN32
        { extern unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long,unsigned long,void**,unsigned long*);
          extern void* __stdcall GetModuleHandleA(const char*);
          void* bt[40]; unsigned short fr=RtlCaptureStackBackTrace(0,40,bt,0);
          char* mb=(char*)GetModuleHandleA(0); char line[1100]; int p=snprintf(line,sizeof line,"[exit-chain] HOST-bt rva:");
          for(int i=0;i<fr;i++) p+=snprintf(line+p,sizeof(line)-p," %llX",(unsigned long long)((char*)bt[i]-mb));
          fprintf(stderr,"%s\n",line); }
#endif
    }
    /* PPC64 ELFv1 cross-module ABI: the caller restores its TOC right after the
     * call with `ld r2, 0x28(r1)`, expecting the import stub to have saved the
     * caller's r2 into that slot. The real .lib.stub trampoline did this; the
     * lifted --hle-stubs body (ps3_hle_call) doesn't, so without this every
     * import call leaves the caller with a garbage r2 -> all later TOC-relative
     * loads (the C++ ctor list, globals, ...) read garbage -> boot corruption. */
    { static int _h=-1; if(_h<0)_h=getenv("FLOW_HLETOC")?1:0;
      if(_h && (uint32_t)(ctx->gpr[1]+0x28)==0x0FEFF268u){ static int _n=0; if(_n++<6){
        char* mb=(char*)ps3_GetModuleHandleA(0); void* ra0=__builtin_return_address(0); void* ra1=__builtin_return_address(1);
        fprintf(stderr,"[HLETOC] corrupt nid=0x%08X name=%s r1=0x%08X  host_ra0_rva=0x%llX ra1_rva=0x%llX\n",
          nid,g_last_hle_name?g_last_hle_name:"?",(uint32_t)ctx->gpr[1],
          (unsigned long long)((char*)ra0-mb),(unsigned long long)((char*)ra1-mb)); } } }
    /* FLOW_NOSPILL: with the lifted code using a constant main TOC (--main-toc/TOCFIX),
     * the caller no longer reads [r1+0x28] to restore r2, so this ABI TOC-spill is
     * unnecessary — and in the frameless-cascade it can clobber a caller frame slot
     * (e.g. the deserializer's this-pointer). Skip it to test that theory. */
    { static int _ns=-1; if(_ns<0)_ns=getenv("FLOW_NOSPILL")?1:0;
      if(!_ns) vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]); }

    /* Real libsre (loaded PRX) takes priority over the HLE stub -- EXCEPT for NIDs
     * listed in YDKJ_FORCE_HLE (comma-separated hex). Some LLE entry points fail
     * against our partially-modelled SPURS state and the CALLER ASSERTS on it:
     * YDKJ's CRI calls _cellSpursEventFlagInitialize (0x5EF96465), libsre returns
     * 0x80410910 (CELL_SPURS_TASK_ERROR_STAT), and CRI's own
     * "CellSpursTaskset.cc:423: ret == CELL_OK -- assertion failed" kills its
     * taskset init => the movie decode task is never created. Forcing the HLE
     * (which succeeds) lets the caller proceed. */
    {
        uint32_t opd = prx_resolve_export(nid);
        if (opd) {
            static const char* s_force = (const char*)1;
            if (s_force == (const char*)1) s_force = getenv("YDKJ_FORCE_HLE");
            if (s_force && *s_force) {
                char want[16]; snprintf(want, sizeof want, "%08X", nid);
                /* case-insensitive substring match on the 8-hex-digit NID */
                for (const char* p2 = s_force; *p2; p2++) {
                    int k = 0;
                    while (k < 8 && p2[k] &&
                           (p2[k] == want[k] ||
                            (p2[k] >= 'a' && p2[k] - 32 == want[k]))) k++;
                    if (k == 8) {
                        static int _n = 0;
                        if (_n++ < 8)
                            fprintf(stderr, "[FORCE-HLE] nid=0x%08X: bypassing libsre, using HLE\n", nid);
                        opd = 0;   /* fall through to the HLE handler below */
                        break;
                    }
                }
            }
        }
        if (opd) {
            uint32_t code = vm_read32(opd);
            uint32_t toc  = vm_read32(opd + 4);
            { static int64_t st=-2; if(st==-2){const char*e=getenv("YDKJ_SPURSTRACE"); st=e?1:0;}
              if (st) fprintf(stderr, "[SPURSTRACE] nid=0x%08X -> libsre code=0x%08X  r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X lr=0x%08X\n",
                  nid, code, (uint32_t)ctx->gpr[3],(uint32_t)ctx->gpr[4],(uint32_t)ctx->gpr[5],(uint32_t)ctx->gpr[6],(uint32_t)ctx->lr);
              /* Dump the struct state at the failing task-attach calls: libsre 0x300158C4
               * (nid 0x87630976) STATs unless struct+0xC==0xFF & +0xE in{1,3}; 0x30015AA4
               * (0x22AAB31D) validates the same struct. Show what our recomp left there. */
              if (st && (nid==0x87630976u || nid==0x22AAB31Du)) {
                  extern uint8_t* vm_base; uint32_t s3=(uint32_t)ctx->gpr[3];
                  #define _RW(o) ((vm_base[s3+(o)]<<24)|(vm_base[s3+(o)+1]<<16)|(vm_base[s3+(o)+2]<<8)|vm_base[s3+(o)+3])
                  /* Full task-descriptor dump: +0xC state(need FF), +0xE type, +0x74/+0x7C
                   * handles (the helper 0x3000b140 does an atomic on *(+0x74)+0xda8 -> if 0,
                   * garbage), +0x4/+0x7 stamp preconds. */
                  fprintf(stderr, "  [state] struct@0x%08X:", s3);
                  for (uint32_t o=0; o<0x80; o+=16) {
                      fprintf(stderr, " |+%02X:", o);
                      for (uint32_t k=0;k<16;k+=4) fprintf(stderr, "%08X ", _RW(o+k));
                  }
                  fprintf(stderr, "\n    +0C=%02X(need FF) +0E=%02X +74=%08X +7C=%08X\n",
                      vm_base[s3+0xC], vm_base[s3+0xE], _RW(0x74), _RW(0x7C));
                  /* The helper 0x3000b140 does a bitmap atomic on taskset(+0x74)+0xda8 --
                   * the task-slot free bitmap CreateTaskset should have initialized. Dump it. */
                  uint32_t ts = _RW(0x74);
                  if (ts >= 0x40000000u && ts < 0x50000000u) {
                      #define _RT(o) ((vm_base[ts+(o)]<<24)|(vm_base[ts+(o)+1]<<16)|(vm_base[ts+(o)+2]<<8)|vm_base[ts+(o)+3])
                      fprintf(stderr, "    taskset@0x%08X +0xda8 slot-bitmap: %08X %08X %08X %08X | +0x00:%08X +0x08:%08X\n",
                          ts, _RT(0xda8), _RT(0xdac), _RT(0xdb0), _RT(0xdb4), _RT(0x00), _RT(0x08));
                      #undef _RT
                  }
                  #undef _RW
              }
            }
            /* Arm a page-guard on the CellSpurs struct at cellSpursInitializeWithAttribute2
             * ENTRY (nid 0xAA6269A8, r3=&spurs) — before the init writes it — to catch
             * where its struct stores actually land. Env YDKJ_GUARD_INST. */
            if (nid == 0xAA6269A8u && getenv("YDKJ_GUARD_INST")) {
                extern void ppu_guard_page(uint32_t);
                fprintf(stderr, "[GUARD] arming on CellSpurs &spurs=0x%08X at init entry\n", (uint32_t)ctx->gpr[3]);
                ppu_guard_page((uint32_t)ctx->gpr[3]);
            }
            ctx->gpr[2] = toc;            /* libsre's own TOC */
            ctx->ctr    = code;
            /* FLOW_REENTRY_ISO: the libsre fn re-enters lifted guest code at the PPU
             * caller's r1; if it (or a frameless mid-entry inside it) runs at an
             * overlapping frame, its stores clobber the caller's live frame (the
             * deserializer this-ptr corruption). Drop r1 to a fresh guard slice so the
             * re-entered frame can never overlap the caller. Args are in regs (ABI);
             * results go via r3 / guest pointers, so isolating the frame is safe. */
            /* Default ON: without isolation the libsre fn runs at the caller's r1 and
             * its prologue `std r2,40(r1)` clobbers the caller's reserved TOC slot at
             * 0x28(r1) -> caller reloads r2=0 -> null TOC -> func_002B03AC spin -> no
             * draws. Isolating r1 to a fresh slice keeps the caller frame intact.
             * Opt out with YDKJ_NO_REENTRY_ISO=1. */
            { static int _ri=-1; if(_ri<0){ const char* e=getenv("YDKJ_NO_REENTRY_ISO"); _ri = (e && e[0]=='1') ? 0 : 1; }
              if(_ri){ uint64_t _sr1=ctx->gpr[1]; ctx->gpr[1]=(ctx->gpr[1]-0x1000)&~0xFull;
                ps3_indirect_call(ctx); ctx->gpr[1]=_sr1; }
              else ps3_indirect_call(ctx); }       /* -> registered lifted libsre fn; r3=ret */
            { static int64_t st=-2; if(st==-2){const char*e=getenv("YDKJ_SPURSTRACE"); st=e?1:0;}
              if (st) fprintf(stderr, "[SPURSTRACE] nid=0x%08X RETURNED r3=0x%08X\n",
                  nid, (uint32_t)ctx->gpr[3]); }
            /* cellSpursInitialize (0xACFC8DBC) just returned -> the CellSpurs
             * instance is now populated; spawn the lifted SPURS SPU kernel against
             * it (flОw libsre never calls group_start; spawning during init reads
             * an empty context and recurses to a stack overflow). Weak so a build
             * without flow_spurs_kernel.c still links. */
            if (nid == 0xACFC8DBCu) {
                extern void flow_spurs_kernel_spawn_postinit(void) __attribute__((weak));
                if (getenv("FLOW_SPURSKERNEL") && flow_spurs_kernel_spawn_postinit)
                    flow_spurs_kernel_spawn_postinit();
            }
            return;
        }
    }

    for (uint32_t i = 0; i < g_ctx_count; i++)
        if (g_ctx[i].nid == nid) { g_ctx[i].fn(ctx); return; }

    ps3_nid_entry* e = g_hle_inited ? ps3_nid_table_find(&g_hle_nids, nid) : nullptr;
    if (!e || !e->handler) {
        /* YDKJ_TUNERFIX: sysPrxForUser 0xE0998DBF is the profiler-presence query called
         * by libsre _cellSpursIsLaunchedFromTuner (0x3000D318). On a normal (non-tuner)
         * run it must return 0x8001112E ("profiler not loaded"); an unresolved-NID error
         * instead trips the usertrace.c:123 assert AND makes the fn return "launched=1",
         * so the caller does tuner/trace setup that fails with CELL_SPURS_CORE_ERROR_STAT
         * -> the SPURS task workload never attaches -> no movie decode. Return not-loaded. */
        if (nid == 0xE0998DBFu && getenv("YDKJ_TUNERFIX")) {
            ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8001112E;
            static int _tn = 0; if (_tn++ < 3)
                fprintf(stderr, "[TUNERFIX] sysPrxForUser 0xE0998DBF -> 0x8001112E (profiler not loaded)\n");
            return;
        }
        static int logged = 0;
        if (logged < 40) { fprintf(stderr, "[hle] unresolved NID 0x%08X\n", nid); logged++; }
        /* cellSaveData call-shape capture: dump r3-r10 + resolve r7/r8 as OPD
         * callbacks (statCallback/fileCallback) to learn the cellSaveDataAutoLoad2
         * protocol so it can be implemented. */
        /* YDKJ_SAVEDATA: minimal cellSaveDataAutoLoad2 (NID 0xCDC6AEFD). The game
         * auto-loads its "BLUS30569-AUTO-" save; unimplemented = null completion =
         * state never transitions. Implement the no-save/new-user path: invoke the
         * game's statCallback (r8 OPD) with a CellSaveDataStatGet{isNewData=1}, honor
         * its result, return CELL_SAVEDATA_RET_OK(0). Legit HLE (real first-run behavior). */
        if (nid==0xCDC6AEFDu) { static int _sdi=-1; if(_sdi<0)_sdi=getenv("YDKJ_SAVEDATA")?1:0;
          if(_sdi){ static int _once=0; if(_once++<2){
            uint32_t statCb=(uint32_t)ctx->gpr[8];               /* statCallback OPD */
            uint32_t SC=0x02000000u, SG=0x02000100u, SS=0x02000900u; /* scratch structs */
            for(uint32_t a=0x02000000u;a<0x02001000u;a+=4) vm_write32(a,0);
            /* CellSaveDataStatGet: hddFreeSizeKB@0, isNewData@4, dir@8(dirStat), getParam@0x40 */
            vm_write32(SG+0x00, 0x100000);   /* hddFreeSizeKB */
            vm_write32(SG+0x04, 1);          /* isNewData = 1 (no existing save) */
            /* copy dirName ("BLUS30569-AUTO-") into dir.dirName @ 0x08+0x18=0x20 */
            { uint32_t src=(uint32_t)ctx->gpr[5]; for(int i=0;i<31;i++){ uint32_t w=vm_read32((src+i)&~3u); uint8_t b=(w>>((3-((src+i)&3))*8))&0xFF; vm_write32((SG+0x20+i)&~3u, (vm_read32((SG+0x20+i)&~3u) & ~(0xFFu<<((3-((SG+0x20+i)&3))*8))) | ((uint32_t)b<<((3-((SG+0x20+i)&3))*8))); if(!b)break; } }
            fprintf(stderr,"[SAVEDATA-HLE] cellSaveDataAutoLoad2: calling game statCallback OPD=0x%08X (isNewData=1)\n",statCb);
            uint64_t r=ppu_guest_call(statCb, SC, SG, SS, 0);
            int32_t cbres=(int32_t)vm_read32(SC+0x00);
            fprintf(stderr,"[SAVEDATA-HLE] statCallback returned r3=0x%llX cbResult->result=%d -> returning OK\n",(unsigned long long)r,cbres);
            ctx->gpr[3]=0; /* CELL_SAVEDATA_RET_OK */
            return;
          } } }
        if (nid==0xCDC6AEFDu || nid==0x27CB8BC2u) { static int _sd=0; if(_sd++<3){
            extern uint32_t vm_read32(uint64_t);
            uint32_t r7=(uint32_t)ctx->gpr[7], r8=(uint32_t)ctx->gpr[8];
            uint32_t r7c=r7?vm_read32(r7):0, r8c=r8?vm_read32(r8):0;
            fprintf(stderr,"[SAVEDATA] nid=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X r7(statCb OPD)=0x%08X->code=func_%08X r8(fileCb OPD)=0x%08X->code=func_%08X r9=0x%08X r10=0x%08X\n",
                nid,(uint32_t)ctx->gpr[3],(uint32_t)ctx->gpr[4],(uint32_t)ctx->gpr[5],(uint32_t)ctx->gpr[6],r7,r7c,r8,r8c,(uint32_t)ctx->gpr[9],(uint32_t)ctx->gpr[10]);
            /* if r4 is a dirName string, dump it */
            uint32_t r4=(uint32_t)ctx->gpr[4]; if(r4>0x10000&&r4<0x10000000){ char nm[32]; for(int i=0;i<31;i++){uint32_t b=vm_read32((r4+i)&~3u); nm[i]=(char)((b>>((3-((r4+i)&3))*8))&0xFF); if(!nm[i])break;} nm[31]=0; fprintf(stderr,"[SAVEDATA]   r4 dirName='%s'\n",nm); } }
        }
        /* YDKJ_NETOFFLINE: unresolved network NIDs (sys_net/cellHttp/cellSysutil)
         * default r3=0 (fake success) -> the game stores a NULL handle as an object
         * and later virtual-calls it -> the 0xC708C708 null-vtable crash. Return a
         * negative error instead so the game's online-content check takes its
         * graceful OFFLINE-FAILURE path and proceeds with the local Persistent.zip. */
        { static int _no=-1; if(_no<0)_no=getenv("YDKJ_NETOFFLINE")?1:0;
          if(_no){ switch(nid){
            case 0x139A9E9Bu: case 0x9FB6228Eu: case 0x05893E7Cu:
            case 0x52AAC4FAu: case 0x9638F766u: case 0x522180BCu:
              ctx->gpr[3] = 0x80010002u; /* generic failure (net unavailable) */
              return;
          } } }
        ctx->gpr[3] = 0;   /* CELL_OK-ish so the game keeps going */
        return;
    }
    g_last_hle_name = e->name;
    if (nid == 0xD0B1D189u /*cellGcmSetTile*/ || nid == 0xDC09357Eu /*SetDisplayBuffer*/) {
        static int _g=0; if (_g++ < 8)
            fprintf(stderr, "[hle-trace] %s lr=0x%08X cia=0x%08X r3..r8=%08X %08X %08X %08X %08X %08X\n",
                    e->name, (uint32_t)ctx->lr, (uint32_t)ctx->cia,
                    (uint32_t)ctx->gpr[3],(uint32_t)ctx->gpr[4],(uint32_t)ctx->gpr[5],
                    (uint32_t)ctx->gpr[6],(uint32_t)ctx->gpr[7],(uint32_t)ctx->gpr[8]);
    }
    hle_generic fn = (hle_generic)e->handler;
    uint64_t r = fn(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6],
                    ctx->gpr[7], ctx->gpr[8], ctx->gpr[9], ctx->gpr[10]);
    ctx->gpr[3] = r;   /* PPC return value */
    /* PTR-LEAK detector (FLOW_PTRLEAK): the game stores HLE return values as
     * guest pointers; if an HLE returns a value out of guest RAM (>= 0x10000000)
     * that isn't a CELL_ERROR code (0x8001xxxx), it's leaking a truncated HOST
     * pointer -> becomes an unresolved guest call later (e.g. 0xC708C708). */
    { static int _pl=-1; if(_pl<0)_pl=getenv("FLOW_PTRLEAK")?1:0;
      if(_pl){ uint32_t rv=(uint32_t)r;
        if(rv>=0x10000000u && (rv & 0xFFFF0000u)!=0x80010000u && (rv & 0xFFFF0000u)!=0x80020000u){
          static int _n=0; if(_n++<40) fprintf(stderr,"[PTRLEAK] NID 0x%08X %s returned 0x%08X (out-of-guest-RAM host ptr?)\n",
            nid, e->name?e->name:"?", rv); } } }
}

/* Populated by the generated registration unit (gen_hle_nids.py). Weak so a
 * build without it still links (no HLE registered -> imports log + return 0). */
extern "C" void ppu_hle_register_all(void) __attribute__((weak));
extern "C" void ppu_hle_register_all(void) {}

extern "C" void ppu_hle_init(void) { ppu_hle_register_all(); }
