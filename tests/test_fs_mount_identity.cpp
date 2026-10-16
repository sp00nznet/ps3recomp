#include "../runtime/ppu/ppu_fs_mount_identity.h"
#include "../runtime/ppu/ppu_fs_aio_engine.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdio>

int main()
{
    assert(ppu_fs_file_mount_identity("/USRDIR/bin/mt.bin_")=="/dev_bdvd");
    assert(ppu_fs_file_mount_identity("/USRDIR")=="/dev_bdvd");
    assert(ppu_fs_file_mount_identity("/USRDIR2/bin/mt.bin_")=="/USRDIR2");
    assert(ppu_fs_file_mount_identity("/dev_hdd0/USRDIR/bin/mt.bin_")=="/dev_hdd0");
    assert(ppu_fs_file_mount_identity("/app_home/USRDIR/bin/mt.bin_")=="/app_home");
    assert(ppu_fs_file_mount_identity(nullptr).empty());
    assert(ppu_fs_file_mount_identity("USRDIR/bin/mt.bin_").empty());
    // Mount initialization itself still requires an explicit real device.
    assert(ppu_fs_mount_identity("/USRDIR/bin/mt.bin_")=="/USRDIR");
    using namespace ppu_fs_aio;
    Engine engine;
    assert(engine.init("/dev_bdvd")==Error::ok);
    std::mutex mutex;
    std::condition_variable changed;
    unsigned completed=0;
    auto work=[] {return Result{ResultCode::success,13911808,0};};
    auto complete=[&](Id,const Result& result) {
        assert(result.code==ResultCode::success && result.bytes==13911808);
        std::lock_guard<std::mutex> lock(mutex);
        ++completed;
        changed.notify_one();
    };
    // Reproduce the failed identity, then submit through the same mount
    // selection used by real opened descriptors. The engine is not bypassed.
    auto publish=[](Id) {return true;};
    assert(engine.submit(ppu_fs_mount_identity("/USRDIR/bin/mt.bin_"),work,complete,publish).error==Error::not_initialized);
    assert(engine.submit(ppu_fs_file_mount_identity("/USRDIR/bin/mt.bin_"),work,complete,publish).error==Error::ok);
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock,std::chrono::seconds(5),[&] {return completed==1;}));
    }
    assert(engine.finish("/dev_bdvd")==Error::ok);
    assert(completed==1);
    assert(engine.shutdown()==Error::ok);
    std::puts("FS mount identity: alias boundaries and real AIO queue regression pass");
}
