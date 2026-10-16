#ifndef PPU_FS_MOUNT_IDENTITY_H
#define PPU_FS_MOUNT_IDENTITY_H
#include <cstring>
#include <string>

inline std::string ppu_fs_mount_identity(const char* guest)
{
    if(!guest || guest[0]!='/')return {};
    const char* end=std::strchr(guest+1,'/');
    const size_t length=end?(size_t)(end-guest):std::strlen(guest);
    if(length<2 || length>31)return {};
    return std::string(guest,length);
}

inline std::string ppu_fs_file_mount_identity(const char* guest)
{
    const std::string mount=ppu_fs_mount_identity(guest);
    // host_path already resolves this disc-relative asset alias under the
    // game's disc root. AIO must use that backing mount's initialized queue.
    return mount=="/USRDIR" ? "/dev_bdvd" : mount;
}
#endif
