#!/usr/bin/env python3
"""Bound the actual parked-list diagnostic without changing DMA behavior."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class ResumeElementsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc=shutil.which(os.environ.get('CC','clang'))
        if not cc:raise unittest.SkipTest('C compiler required')
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        cls.exe=Path(cls.tmp.name)/('resume-elements.exe' if os.name=='nt' else 'resume-elements')
        cmd=[cc,'-std=gnu17','-O2','-I',str(ROOT/'include'),'-I',str(ROOT/'runtime/spu'),'-I',str(ROOT/'runtime/platform'),str(ROOT/'runtime/spu/tests/test_mfc_resume_elements.c'),str(ROOT/'runtime/platform/win32_compat.c'),'-o',str(cls.exe)]
        if os.name!='nt':cmd.insert(1,'-pthread')
        if os.environ.get('MFC_TEST_SANITIZE'):cmd[1:1]=['-fsanitize=undefined','-fno-sanitize-recover=all']
        subprocess.run(cmd,check=True,capture_output=True,text=True)
    def run_mode(self,mode):
        env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','YDKJ_','LBP_'))}
        p=subprocess.run([str(self.exe),mode],capture_output=True,text=True,env=env,timeout=30)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertIn('OK: actual parked/resume',p.stdout)
        self.assertEqual(p.stderr.count('[mfc-list] RESUME-ELEMS '),16)
    def test_empty_resume_beyond_cap(self):self.run_mode('empty')
    def test_actual_get_and_put_beyond_cap(self):self.run_mode('transfer')
    def test_repark_then_complete_beyond_cap(self):self.run_mode('repark')
    def test_concurrent_empty_resumes_share_cap(self):self.run_mode('concurrent')
if __name__=='__main__':unittest.main()
