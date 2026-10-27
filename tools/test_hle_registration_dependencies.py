#!/usr/bin/env python3
"""Build the real template's HLE rule incrementally against tiny SDK sources."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
from nid_database import compute_nid

ROOT=Path(__file__).resolve().parents[1]
if not shutil.which('cmake'):
    if os.environ.get('CI'): raise SystemExit('CMake is required for this CI regression')
    print('SKIP: CMake is unavailable')
    raise SystemExit(0)
text=(ROOT/'templates/project/CMakeLists.txt').read_text()
start=text.index('if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")')
end=text.index('# ---------------------------------------------------------------------------',start)
rule=text[start:end]

with tempfile.TemporaryDirectory(prefix='hle-registration-deps-') as tmp:
    root=Path(tmp); sdk=root/'sdk'; (sdk/'tools').mkdir(parents=True)
    (sdk/'libs/system').mkdir(parents=True)
    for name in ['gen_hle_nids.py','nid_database.py']:
        shutil.copyfile(ROOT/'tools'/name,sdk/'tools'/name)
    module=sdk/'libs/system/cellFixture.c'
    module.write_text('int cellFixtureFirst(void) { return 0; }\n')
    project=root/'project';project.mkdir()
    (project/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\n'
        'project(HleDependencyFixture LANGUAGES NONE)\n'
        'set(PS3RECOMP_DIR "'+sdk.as_posix()+'")\n'
        'set(RECOMP_DIR "'+(root/'lift').as_posix()+'")\n'+rule+
        'add_custom_target(registration ALL DEPENDS "${HLE_NID_TABLE}")\n')
    build=root/'build'
    generator=['-G','Ninja'] if shutil.which('ninja') else []
    subprocess.run(['cmake','-S',str(project),'-B',str(build),*generator,
        '-DPython3_EXECUTABLE='+sys.executable],check=True,capture_output=True,text=True)
    def build_table():
        result=subprocess.run(['cmake','--build',str(build)],capture_output=True,text=True)
        assert result.returncode==0,result.stdout+result.stderr
        return (build/'ppu_hle_nids.cpp').read_text()
    output=build/'ppu_hle_nids.cpp'
    assert 'cellFixtureFirst' in build_table()
    stamp=output.stat().st_mtime_ns
    build_table();assert output.stat().st_mtime_ns==stamp,'Unchanged inputs regenerated registration'
    def update(path,content):
        # Also work on filesystems/build tools with one-second mtime resolution.
        while int(time.time()) <= int(output.stat().st_mtime): time.sleep(.02)
        path.write_text(content)
    update(module,module.read_text()+'int cellFixtureAdded(void) { return 0; }\n')
    assert 'cellFixtureAdded' in build_table(),'Existing library edit did not regenerate registration'
    added=sdk/'libs/system/cellNewFixture.c'
    update(added,'int cellNewFixtureHandler(void) { return 0; }\n')
    assert 'cellNewFixtureHandler' in build_table(),'New module did not regenerate registration'
    # Make/file systems can compare timestamps at one-second resolution.
    while int(time.time()) <= int(output.stat().st_mtime): time.sleep(.02)
    added.unlink()
    assert 'cellNewFixtureHandler' not in build_table(),'Deleted module retained stale registration'
    database=sdk/'tools/nid_database.py'
    update(database,database.read_text()+'\n_original_compute_nid = compute_nid\n'
        'def compute_nid(name):\n    return _original_compute_nid(name) ^ 1\n')
    expected=compute_nid('cellFixtureFirst')^1
    assert f'0x{expected:08X}u, "cellFixtureFirst"' in build_table(),'NID database edit did not regenerate registration'
    print('PASS: real template incremental registration handles existing/new/deleted modules and NID database edits')
