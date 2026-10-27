#include "cellSysmodule.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    const u16 invalid[] = {0, 0x44, 0xefff, 0xf057, 0xf100, 0xffff};
    const u16 valid[] = {0xf000, 0xf035, 0xf044, 0xf056};
    for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        u16 id = valid[i];
        assert(cellSysmoduleIsLoadedEx(id) == CELL_SYSMODULE_ERROR_UNLOADED);
        assert(cellSysmoduleLoadModule(id) == CELL_OK);
        assert(cellSysmoduleIsLoadedEx(id) == CELL_OK);
        assert(cellSysmoduleIsLoaded(id) == CELL_OK);
        assert(cellSysmoduleUnloadModule(id) == CELL_OK);
        assert(cellSysmoduleIsLoadedEx(id) == CELL_SYSMODULE_ERROR_UNLOADED);
    }
    assert(cellSysmoduleLoadModule(0x44) == CELL_OK);
    assert(cellSysmoduleIsLoadedEx(0xf044) == CELL_SYSMODULE_ERROR_UNLOADED);
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(cellSysmoduleIsLoadedEx(invalid[i]) == CELL_SYSMODULE_ERROR_UNKNOWN);
    assert(cellSysmoduleIsLoaded(0x44) == CELL_OK);
    puts("PASS: extended module query, shared state, ID bounds and namespace separation");
    return 0;
}
