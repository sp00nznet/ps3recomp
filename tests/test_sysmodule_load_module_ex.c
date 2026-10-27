#include "cellSysmodule.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    /* Public Ex IDs, including both endpoints and MAG's NP EULA module. */
    const u16 allowed[] = {0xf005,0xf00a,0xf010,0xf035,0xf043,0xf044,0xf056};
    /* Core IDs, holes inside the Ex range, namespace bounds, plus modules
     * whose load depends on unsupported process-PARAM.SFO permissions. */
    const u16 rejected[] = {0,0x44,0xefff,0xf000,0xf004,0xf006,0xf042,
                            0xf053,0xf057,0xf100,0xffff,0xf020,0xf024,0xf03e};
    for(unsigned i=0;i<sizeof(allowed)/sizeof(allowed[0]);++i) {
        u16 id=allowed[i];
        assert(cellSysmoduleIsLoadedEx(id)==(s32)CELL_SYSMODULE_ERROR_UNLOADED);
        assert(cellSysmoduleLoadModuleEx(id)==CELL_OK);
        assert(cellSysmoduleIsLoadedEx(id)==CELL_OK);
        assert(cellSysmoduleIsLoaded(id)==CELL_OK);
        assert(cellSysmoduleLoadModuleEx(id)==CELL_OK); /* existing HLE idempotence */
        assert(cellSysmoduleUnloadModule(id)==CELL_OK);
        assert(cellSysmoduleIsLoadedEx(id)==(s32)CELL_SYSMODULE_ERROR_UNLOADED);
    }
    assert(cellSysmoduleLoadModule(0x44)==CELL_OK);
    assert(cellSysmoduleLoadModuleEx(0xf044)==CELL_OK);
    assert(cellSysmoduleUnloadModule(0xf044)==CELL_OK);
    assert(cellSysmoduleIsLoaded(0x44)==CELL_OK);
    for(unsigned i=0;i<sizeof(rejected)/sizeof(rejected[0]);++i) {
        u16 id=rejected[i];
        s32 before=cellSysmoduleIsLoaded(id);
        assert(cellSysmoduleLoadModuleEx(id)==(s32)CELL_SYSMODULE_ERROR_UNKNOWN);
        assert(cellSysmoduleIsLoaded(id)==before);
    }
    /* An already-tracked but disallowed ID must still be rejected. */
    assert(cellSysmoduleLoadModule(0xf042)==CELL_OK);
    assert(cellSysmoduleLoadModuleEx(0xf042)==(s32)CELL_SYSMODULE_ERROR_UNKNOWN);
    assert(cellSysmoduleIsLoadedEx(0xf042)==CELL_OK);
    puts("PASS: extended load registration, state transitions, ID policy and namespace isolation");
}
