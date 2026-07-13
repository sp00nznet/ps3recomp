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
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleA(const char*);
static inline void* ps3_GetModuleHandleA(const char* m){ return GetModuleHandleA(m); }

extern "C" void ps3_hle_call(uint32_t nid, ppu_context* ctx)
{
    g_last_hle_nid = nid;

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
    /* sys_process_exit (abort path): dump the guest back-chain so we see WHO aborted.
     * Always-on for a NONZERO exit code (error/abort) -- the rare loader-thread
     * abort in LBP is a race we can't reliably reproduce, so capture it whenever
     * it fires. Clean code=0 exits (normal shutdown) stay quiet. */
    if (nid == 0xE6F2C1E7u && ((uint32_t)ctx->gpr[3] != 0 || getenv("FLOW_EXITCHAIN"))) {
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

    /* Real libsre (loaded PRX) takes priority over the HLE stub. */
    {
        uint32_t opd = prx_resolve_export(nid);
        if (opd) {
            uint32_t code = vm_read32(opd);
            uint32_t toc  = vm_read32(opd + 4);
            { static int64_t st=-2; if(st==-2){const char*e=getenv("YDKJ_SPURSTRACE"); st=e?1:0;}
              if (st) fprintf(stderr, "[SPURSTRACE] nid=0x%08X -> libsre code=0x%08X  r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X\n",
                  nid, code, (uint32_t)ctx->gpr[3],(uint32_t)ctx->gpr[4],(uint32_t)ctx->gpr[5],(uint32_t)ctx->gpr[6]); }
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
            { static int _ri=-1; if(_ri<0)_ri=getenv("FLOW_REENTRY_ISO")?1:0;
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
        static int logged = 0;
        if (logged < 40) { fprintf(stderr, "[hle] unresolved NID 0x%08X\n", nid); logged++; }
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
