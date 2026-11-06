#!/usr/bin/env python3
"""Real GET discovery, residency/code epochs, list stalls and publication.

Only temporary source copies acquire counters/publication observation points.
--output-dir retains the exact compiled binaries, argv and output for review.
"""
import argparse,json,os,pathlib,subprocess,tempfile
if os.name == 'nt':
    print('SKIP: standalone runtime fixture requires a POSIX compiler')
    raise SystemExit(0)
ROOT=pathlib.Path(__file__).resolve().parents[1];SPU=ROOT/'runtime/spu'
parser=argparse.ArgumentParser()
parser.add_argument('--output-dir',type=pathlib.Path)
parser.add_argument('--source',type=pathlib.Path)
parser.add_argument('--mutants',action='store_true')
args=parser.parse_args()
units=['spu_drain','spu_lockstep','spu_coherency','spu_workload','spurs_policy','spurs_job','spu_tsp_weak','spu_vm_pagemap','spurs_policy_blob_weak','spu_interp']
env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))}
source=(args.source or SPU/'spu_channels.c').read_text()
def instrument(text):
    text=text.replace('#include "../','#include "'+str(SPU.parent)+'/')
    needle='for (int i = 0; i < nsrc; i++) {'
    assert text.count(needle)==1
    text=text.replace(needle,needle+'\n        ++allsize_scans;')
    start=text.index('void spu_overlay_register_sig(')
    end=text.index('\n}',start)
    body=text[start:end]
    needle='__atomic_store_n(&s_ovl_src_count, s_ovl_src_count + 1, __ATOMIC_RELEASE);'
    if needle not in body:needle='s_ovl_src_count++;'
    assert body.count(needle)==1
    text=text[:start]+body.replace(needle,needle+'\n        allsize_sig_published();')+text[end:]
    return 'static unsigned allsize_scans;\nvoid allsize_sig_published(void);\n'+text
def run(work):
    fixture=(SPU/'tests/test_spu_mfc_slots.c').read_text()
    fixture=fixture.replace('#include "../spu_channels.c"','#include "allsize_channels.c"')
    fixture=fixture.replace('#include "../spu_lifted_job.h"','#include "'+str(SPU/'spu_lifted_job.h')+'"')
    (work/'allsize_fixture.h').write_text(fixture)
    records=[]
    def variant(name,text,sanitize=False):
        where=work/name;where.mkdir(exist_ok=True)
        (where/'allsize_channels.c').write_text(instrument(text))
        exe=where/'fixture'
        cmd=[os.environ.get('CC','cc'),'-std=gnu17','-O2','-UNDEBUG']
        if sanitize:cmd+=['-fsanitize=undefined','-fno-sanitize-recover=all']
        cmd+=['-I'+str(where),'-I'+str(work),'-I'+str(ROOT/'include'),'-I'+str(SPU),'-I'+str(ROOT/'runtime/platform'),str(SPU/'tests/test_spu_overlay_allsize.c'),*[str(SPU/(u+'.c')) for u in units],str(ROOT/'runtime/platform/win32_compat.c'),'-pthread','-lm','-o',str(exe)]
        (where/'build-argv.json').write_text(json.dumps(cmd,indent=2)+'\n')
        build=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
        (where/'build.stdout').write_text(build.stdout);(where/'build.stderr').write_text(build.stderr)
        assert not build.returncode,build.stdout+build.stderr
        results=[]
        for mode in ['0','1','shadow']:
            for case in ['core','full']:
                argv=[str(exe),case]
                r=subprocess.run(argv,env=dict(env,SPU_DISPATCH_CACHE=mode),capture_output=True,text=True,timeout=15)
                label=mode+'-'+case
                (where/(label+'.stdout')).write_text(r.stdout);(where/(label+'.stderr')).write_text(r.stderr)
                records.append({'name':name,'mode':mode,'case':case,'argv':argv,'returncode':r.returncode})
                results.append(r)
                print(name,mode,case,r.returncode,r.stdout.strip(),flush=True)
        return results
    good=True
    for name,san in [('o2',False),('ubsan',True)]:
        results=variant(name,source,san)
        good=good and all(not r.returncode for r in results)
    if args.mutants:
        changes={
            'reject-matches':('!spu_ovl_ea_known(ea)', '1'),
            'drop-residency':('    for (unsigned slot = 0; slot < 4; ++slot) {\n        if (!ctx->resident_code[slot].image_id) continue;', '    for (unsigned slot = 4; slot < 4; ++slot) {\n        if (!ctx->resident_code[slot].image_id) continue;'),
            'ignore-signatures':('!atomic_load_explicit(&s_ovl_has_sig, memory_order_acquire)', '1'),
            'late-signature-flag':('        atomic_store_explicit(&s_ovl_has_sig, 1, memory_order_release);\n        __atomic_store_n(&s_ovl_src_count, s_ovl_src_count + 1, __ATOMIC_RELEASE);',
                                    '        __atomic_store_n(&s_ovl_src_count, s_ovl_src_count + 1, __ATOMIC_RELEASE);\n        atomic_store_explicit(&s_ovl_has_sig, 1, memory_order_release);'),
        }
        for name,(old,new) in changes.items():
            assert source.count(old)==1,(name,source.count(old))
            results=variant('mutant-'+name,source.replace(old,new))
            assert any(r.returncode and 'FAIL:' in r.stdout for r in results),name
            print('PASS rejected bug control',name,flush=True)
    (work/'runs.json').write_text(json.dumps(records,indent=2)+'\n')
    if not good:raise SystemExit(1)
    print('PASS real all-size overlay discovery preservation',flush=True)
if args.output_dir:
    args.output_dir.mkdir(parents=True,exist_ok=True);run(args.output_dir.resolve())
else:
    with tempfile.TemporaryDirectory(prefix='spu-overlay-allsize-') as td:run(pathlib.Path(td))
