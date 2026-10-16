/* Internal to ppu_fs.cpp: checked guest ABI over the asynchronous host engine.
 * No game data or generated guest code. Read-only AIO; writes remain guarded. */
#ifndef PPU_FS_AIO_BRIDGE_H
#define PPU_FS_AIO_BRIDGE_H

static thread_local ppu_context* fs_aio_current_caller;
struct FsAioCallerScope {
    ppu_context* previous;
    explicit FsAioCallerScope(ppu_context* context):previous(fs_aio_current_caller) {fs_aio_current_caller=context;}
    ~FsAioCallerScope() {fs_aio_current_caller=previous;}
};
static ppu_fs_aio::Engine& fs_aio_engine()
{
    static ppu_fs_aio::Engine engine({
        [] {return ppu_callback_worker_enter()==0;},
        [] {if(ppu_callback_worker_leave()!=0)throw std::runtime_error("AIO callback context teardown failed");}
    });
    return engine;
}
static int32_t fs_aio_error(ppu_fs_aio::Error error)
{
    using E=ppu_fs_aio::Error;
    switch(error) {
    case E::ok:case E::already_initialized:return 0;
    case E::invalid_mount:case E::invalid_request:case E::unknown_request:case E::too_late:return (int32_t)0x80010002u;
    case E::not_initialized:case E::stopping:return (int32_t)0x8001002Fu;
    case E::capacity:case E::busy:return (int32_t)0x8001000Au;
    case E::publication_failed:return CELL_FS_STAT_EFAULT;
    default:return CELL_FS_STAT_EIO;
    }
}
/* The local firmware path helper accepts 2..1055 bytes, and keeps the leading
 * slash plus first component (at most31 bytes). Check every guest byte first. */
static int32_t fs_aio_mount(uint32_t ea,std::string& mount)
{
    if(!ea)return CELL_FS_STAT_EFAULT;
    char path[1056];size_t length=0;
    for(;length<sizeof path;++length) {
        const uint64_t at=(uint64_t)ea+length;
        if(at>UINT32_MAX || !fs_stat_guest_span((uint32_t)at,1,false))return CELL_FS_STAT_EFAULT;
        path[length]=(char)vm_base[at];
        if(!path[length])break;
    }
    if(length==sizeof path || length<2 || path[0]!='/')return (int32_t)0x80010002u;
    mount=fs_mount_identity(path);
    if(mount.empty())return (int32_t)0x80010002u;
    static const char* supported[]={"/dev_bdvd","/app_home","/dev_hdd0","/dev_hdd1","/dev_flash","/host_root","/dev_usb000","/dev_usb"};
    bool known=false;for(const char* value:supported)if(mount==value){known=true;break;}
    return known?0:(int32_t)0x80010002u;
}

static void cellFsAioInit(ppu_context* ctx)
{
    std::string mount;int32_t error=fs_aio_mount((uint32_t)ctx->gpr[3],mount);
    if(!error) {
        std::lock_guard<std::recursive_mutex> lock(fs_files_mutex);
        char path[1100];bool truncated=false;
        const std::string root=mount+"/";host_path(path,sizeof path,root.c_str(),&truncated);
        struct stat st{};
        if(truncated || stat(path,&st)!=0 || (st.st_mode&S_IFMT)!=S_IFDIR)error=(int32_t)0x80010002u;
    }
    if(!error)error=fs_aio_error(fs_aio_engine().init(mount));
    fprintf(stderr,"[fs] aio init mount='%s' path_ea=0x%08X result=0x%08X lr=0x%08X tid=%llu\n",
        mount.c_str(),(uint32_t)ctx->gpr[3],(uint32_t)error,(uint32_t)ctx->lr,(unsigned long long)ctx->thread_id);
    ctx->gpr[3]=(uint64_t)(int64_t)error;
}
static void cellFsAioFinish(ppu_context* ctx)
{
    std::string mount;int32_t error=fs_aio_mount((uint32_t)ctx->gpr[3],mount);
    if(!error) {
        FsAioCallerScope caller(ctx);
        const auto result=fs_aio_engine().finish(mount);
        error=result==ppu_fs_aio::Error::not_initialized?(int32_t)0x80010002u:fs_aio_error(result);
        if(!error && fs_aio_engine().health()!=ppu_fs_aio::Error::ok)error=CELL_FS_STAT_EIO;
    }
    ctx->gpr[3]=(uint64_t)(int64_t)error;
}
static void cellFsAioCancel(ppu_context* ctx)
{
    FsAioCallerScope caller(ctx);
    int32_t error=fs_aio_error(fs_aio_engine().cancel((int32_t)ctx->gpr[3]));
    if(!error && fs_aio_engine().health()!=ppu_fs_aio::Error::ok)error=CELL_FS_STAT_EIO;
    ctx->gpr[3]=(uint64_t)(int64_t)error;
}

static ppu_fs_aio::Result fs_aio_host_read(const std::shared_ptr<FsFile>& file,
    uint8_t* memory,uint32_t vm_size,uint32_t buffer,uint64_t offset,uint64_t size)
{
    using R=ppu_fs_aio::ResultCode;
    auto failed=[](int32_t error,uint64_t bytes=0){return ppu_fs_aio::Result{R::io_error,bytes,error};};
    if(vm_base!=memory || ppu_vm_size!=vm_size || (size && !fs_stat_guest_span(buffer,(size_t)size,true)))
        return failed(CELL_FS_STAT_EFAULT);
    if(!size)return {R::success,0,0};
    int native_fd;
    {
        std::lock_guard<std::recursive_mutex> lock(fs_files_mutex);
        if((file->flags&3u)!=CELL_FS_O_RDONLY && fflush(file->stream)!=0)return failed(CELL_FS_STAT_EIO);
        native_fd=fileno(file->stream);
    }
    if(native_fd<0)return failed((int32_t)0x8001002Au);
    uint64_t total=0;
#ifdef _WIN32
    const intptr_t raw=_get_osfhandle(native_fd);
    if(raw==-1)return failed((int32_t)0x8001002Au);
    /* ReOpenFile retains the existing file object with an independent file
     * pointer. OVERLAPPED offsets preserve the original FILE buffer/EOF state. */
    HANDLE handle=ReOpenFile((HANDLE)raw,GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,FILE_FLAG_OVERLAPPED|FILE_FLAG_RANDOM_ACCESS);
    if(handle==INVALID_HANDLE_VALUE)return failed(CELL_FS_STAT_EIO);
    struct Close {HANDLE value;~Close(){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);}} owned{handle};
    HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!event)return failed((int32_t)0x80010004u);
    Close owned_event{event};
    while(total<size) {
        const DWORD wanted=(DWORD)std::min<uint64_t>(size-total,1024u*1024u);
        OVERLAPPED operation{};const uint64_t at=offset+total;
        operation.Offset=(DWORD)at;operation.OffsetHigh=(DWORD)(at>>32);operation.hEvent=event;
        if(!ResetEvent(event))return failed(CELL_FS_STAT_EIO,total);
        DWORD got=0;BOOL ok=ReadFile(handle,memory+buffer+total,wanted,nullptr,&operation);
        if(ok || GetLastError()==ERROR_IO_PENDING)ok=GetOverlappedResult(handle,&operation,&got,TRUE);
        if(!ok) {
            const DWORD error=GetLastError();
            if(error==ERROR_HANDLE_EOF)break;
            return failed(error==ERROR_NOT_ENOUGH_MEMORY?(int32_t)0x80010004u:CELL_FS_STAT_EIO,total);
        }
        total+=got;if(got<wanted)break;
    }
#else
    while(total<size) {
        const size_t wanted=(size_t)std::min<uint64_t>(size-total,1024u*1024u);
        const ssize_t got=pread(native_fd,memory+buffer+total,wanted,(off_t)(offset+total));
        if(got<0) {if(errno==EINTR)continue;return failed(errno==EBADF?(int32_t)0x8001002Au:CELL_FS_STAT_EIO,total);}
        total+=(uint64_t)got;if((size_t)got<wanted)break;
    }
#endif
    return {R::success,total,0};
}

static void cellFsAioRead(ppu_context* ctx)
{
    const uint32_t request=(uint32_t)ctx->gpr[3],id_pointer=(uint32_t)ctx->gpr[4],callback=(uint32_t)ctx->gpr[5];
    auto reject=[&](int32_t error,const char* reason){
        if(error) {
            static std::atomic<unsigned> failures{0};
            if(failures.fetch_add(1)<16)
                fprintf(stderr,"[fs] aio reject reason=%s error=0x%08X request=0x%08X id_pointer=0x%08X callback=0x%08X lr=0x%08X tid=%llu\n",
                    reason,(uint32_t)error,request,id_pointer,callback,(uint32_t)ctx->lr,
                    (unsigned long long)ctx->thread_id);
        }
        ctx->gpr[3]=(uint64_t)(int64_t)error;
    };
    if(!fs_stat_guest_span(request,0x28,false) || !fs_stat_guest_span(id_pointer,4,true) ||
       !fs_stat_guest_span(callback,8,false)) {reject(CELL_FS_STAT_EFAULT,"guest-arguments");return;}
    const int fd=(int)(int32_t)vm_read32(request);
    const uint64_t offset=vm_read64(request+8),size=vm_read64(request+0x18);
    const uint32_t buffer=vm_read32(request+0x10);
    if(size>SIZE_MAX || offset>INT64_MAX || size>(uint64_t)INT64_MAX-offset) {reject((int32_t)0x80010002u,"size-or-offset");return;}
    if(size && !fs_stat_guest_span(buffer,(size_t)size,true)) {reject(CELL_FS_STAT_EFAULT,"destination");return;}
    uint32_t code=0,toc=0;
    if(ppu_callback_worker_resolve(callback,&code,&toc)!=0) {reject(CELL_FS_STAT_EFAULT,"callback");return;}
    std::shared_ptr<FsFile> file;
    {
        std::lock_guard<std::recursive_mutex> lock(fs_files_mutex);
        if(fd>=3 && fd<FS_MAX)file=g_file_refs[fd];
    }
    if(!file || (file->flags&3u)==CELL_FS_O_WRONLY) {reject((int32_t)0x8001002Au,"file-descriptor");return;}
    uint8_t* const memory=vm_base;const uint32_t vm_size=ppu_vm_size;
    const auto submission=fs_aio_engine().submit(file->mount,
        [file,memory,vm_size,buffer,offset,size] {return fs_aio_host_read(file,memory,vm_size,buffer,offset,size);},
        [request,code,toc,memory,vm_size](ppu_fs_aio::Id id,const ppu_fs_aio::Result& result) {
            if(vm_base!=memory || ppu_vm_size!=vm_size)throw std::runtime_error("AIO VM lifetime changed");
            int32_t error=0;
            if(result.code==ppu_fs_aio::ResultCode::cancelled)error=(int32_t)0x80010013u;
            else if(result.code!=ppu_fs_aio::ResultCode::success)error=result.detail?result.detail:CELL_FS_STAT_EIO;
            uint64_t args[8]={request,(uint64_t)(int64_t)error,(uint64_t)(int64_t)id,result.bytes,0,0,0,0};
            uint64_t ignored=0;
            const int delivered=fs_aio_current_caller?
                ppu_callback_call_current(fs_aio_current_caller,code,toc,args,&ignored):
                ppu_callback_worker_call_resolved(code,toc,args,&ignored);
            if(delivered!=0) {
                ppu_strict_boot_guard("aio-callback-failure",fs_aio_current_caller,"target",code);
                throw std::runtime_error("AIO callback dispatch failed");
            }
            static std::atomic<unsigned> count{0};
            if(count.fetch_add(1)<16)fprintf(stderr,"[fs] aio complete id=%d error=0x%08X bytes=%llu context=%s\n",
                id,(uint32_t)error,(unsigned long long)result.bytes,fs_aio_current_caller?"caller":"worker");
        },
        [id_pointer,memory,vm_size](ppu_fs_aio::Id id) {
            if(vm_base!=memory || ppu_vm_size!=vm_size || !fs_stat_guest_span(id_pointer,4,true))return false;
            vm_write32(id_pointer,(uint32_t)id);return true;
        });
    const int32_t error=fs_aio_error(submission.error);
    static std::atomic<unsigned> count{0};
    if(count.fetch_add(1)<16)fprintf(stderr,"[fs] aio submit fd=%d mount='%s' offset=%llu bytes=%llu id=%d result=0x%08X lr=0x%08X\n",
        fd,file->mount.c_str(),(unsigned long long)offset,(unsigned long long)size,submission.id,(uint32_t)error,(uint32_t)ctx->lr);
    reject(error,"submission");
}
static void cellFsAioWrite(ppu_context* ctx)
{
    ps3_strict_fs_unimplemented("cellFsAioWrite","<unsupported asynchronous write>");
    ctx->gpr[3]=(uint64_t)(int64_t)CELL_FS_STAT_EIO;
}
extern "C" int32_t ppu_fs_aio_shutdown(void)
{
    const auto result=fs_aio_engine().shutdown();
    if(result!=ppu_fs_aio::Error::ok || fs_aio_engine().health()!=ppu_fs_aio::Error::ok)return CELL_FS_STAT_EIO;
    return ppu_callback_workers_quiesce()==0?0:CELL_FS_STAT_EIO;
}
#endif
