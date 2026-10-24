#!/usr/bin/env python3
"""PS3_TPROF: bounded sample collection, registered threads, filtering and PC attribution."""
from pathlib import Path
import os, subprocess, tempfile, platform, json
ROOT = Path(__file__).resolve().parents[1]

def main():
 if platform.system() != 'Darwin' or platform.machine() != 'arm64':
  print(json.dumps({'status':'SKIP','reason':'macOS arm64 only'})); return
 with tempfile.TemporaryDirectory(prefix='tprof-') as td:
  t = Path(td); exe = t/'tprof'
  q = subprocess.run(['cc','-O1','-g','-pthread','-I'+str(ROOT/'include'),str(ROOT/'runtime/diagnostics/tests/test_ps3_tprof.c'),'-o',str(exe)],capture_output=True,text=True,timeout=30)
  assert q.returncode == 0, q.stderr
  env = {k:v for k,v in os.environ.items() if not k.startswith('PS3_TPROF')}
  q = subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=12)
  assert q.returncode == 0, q.stdout+q.stderr
  syms = {}
  for line in subprocess.run(['nm','-n',str(exe)],capture_output=True,text=True,check=True).stdout.splitlines():
   p = line.split()
   if len(p) >= 3 and p[1] in 'tT': syms[p[2]] = int(p[0],16)
  lo = syms['_tprof_test_spin']; hi = min([v for v in syms.values() if v > lo],default=lo+0x100)
  results = []
  # 256 records give each registered kind at least 100 samples. Five seconds
  # bounds the sampler; the fixture waits for its completed dump, not a sleep.
  for name, ppu, interval in [('spu',False,'1000'),('ppu',True,'1000'),('slow',True,'10000')]:
   prof = t/(name+'.txt')
   runenv = {**env,'PS3_TPROF':str(prof),'PS3_TPROF_TRIGGER':str(t/(name+'.go')),
             'PS3_TPROF_SECONDS':'5','PS3_TPROF_CAP':'256','PS3_TPROF_INTERVAL_US':interval}
   if ppu: runenv['PS3_TPROF_PPU'] = '1'
   q = subprocess.run([str(exe)],env=runenv,capture_output=True,text=True,timeout=12)
   assert q.returncode == 0, (name,q.stdout+q.stderr)
   hits = n = 0
   for line in prof.read_text().splitlines():
    if not line.startswith('S 7 '): continue
    n += 1; hits += lo <= int(line.split()[3],16) < hi
   assert n >= 100 and hits*10 >= n*9, (name,hits,n)
   results.append({'case':name,'spinner_samples':n,'in_spin':hits})
  # A genuinely insufficient profile must still fail the count assertions.
  q = subprocess.run([str(exe)],env={**runenv,'PS3_TPROF':str(t/'short.txt'),
                     'PS3_TPROF_TRIGGER':str(t/'short.go'),'PS3_TPROF_SECONDS':'0.01'},
                     capture_output=True,text=True,timeout=12)
  assert q.returncode != 0 and 'header=' in q.stdout and 'FAIL' in q.stdout, q.stdout+q.stderr
  print(json.dumps({'status':'PASS','profiles':results,'insufficient_profile':'rejected'}))
if __name__ == '__main__': main()
