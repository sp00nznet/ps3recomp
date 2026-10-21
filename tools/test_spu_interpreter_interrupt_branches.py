#!/usr/bin/env python3
"""Real interpreter/microstep/lifted branch feature-bit regression."""
import os,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1];spu=root/'runtime/spu'
if os.name=='nt':print('SKIP: POSIX compiler/linker fixture');raise SystemExit(0)
sources=['spu_channels.c','spu_drain.c','spu_lockstep.c','spu_coherency.c','spu_workload.c','spurs_policy.c','spurs_job.c','spu_tsp_weak.c','spu_vm_pagemap.c','spurs_policy_blob_weak.c','spu_interp.c']
env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))}
with tempfile.TemporaryDirectory(prefix='spu-interrupt-branches-') as tmp:
 tmp=Path(tmp)
 from spu_disasm import spu_decode
 from spu_lifter import SPULifter
 codes={0x35080000,0x35082500,0x35040100}
 for op in [0x1a8,0x1a9,0x1aa,0x128,0x129,0x12a,0x12b]:
  for bits in [0,0x40000,0x80000]:codes.add((op<<21)|bits|(7<<7)|5)
 for op in range(0x128,0x12c):codes.add((op<<21)|0x80000|(7<<7)|7)
 codes.add(0x35080000|(7<<7))
 body=[];entries=[]
 for code in sorted(codes):
  ins=spu_decode(code,0x4000);assert ins.mnemonic!='.word'
  f=SPULifter(prefix=f'irq_{code:x}_').lift_function([ins],0x4000,0x4004)
  body+=[f'static void {f.name}(spu_context* ctx) {{',*f.body_lines]
  if ins.mnemonic in ['biz','binz','bihz','bihnz']:body.append('ctx->pc=0x4004;') # single-instruction local fallthrough
  body.append('}')
  entries.append(f'case 0x{code:X}: {f.name}(ctx);return;')
 body+=['static void lifted_run(uint32_t opcode,spu_context* ctx){switch(opcode){',*entries,'default:abort();}}']
 (tmp/'interrupt_branches_lifted.h').write_text('\n'.join(body)+'\n')
 for lifted in [False,True]:
  binary=tmp/('lifted' if lifted else 'interp')
  subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG',*(['-DBRANCH_LIFTED'] if lifted else []),'-I'+str(tmp),'-I'+str(root/'include'),'-I'+str(spu),'-I'+str(root/'runtime/platform'),str(spu/'tests/test_spu_interpreter_interrupt_branches.c'),*[str(spu/x) for x in sources],str(root/'runtime/platform/win32_compat.c'),'-pthread','-lm','-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True,timeout=5,env=env,cwd=tmp)
