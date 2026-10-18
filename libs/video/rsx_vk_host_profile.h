#ifndef RSX_VK_HOST_PROFILE_H
#define RSX_VK_HOST_PROFILE_H
/* Graphics-thread host wall times; fence wait is not a GPU timestamp. */
static FILE* vhp_file;
static int vhp_initialized;
static Uint64 vhp_frequency, vhp_rows;
static Uint64 vhp_tick(void)
{
    if (!vhp_initialized) {
        vhp_initialized = 1;
        const char* path = getenv("PS3RECOMP_VK_HOST_PROFILE_CSV");
        if (path && *path) {
            vhp_frequency = SDL_GetPerformanceFrequency();
            if (!vhp_frequency || vhp_frequency > 1000000000u) return 0;
            vhp_file = fopen(path, "wb");
            if (vhp_file) fprintf(vhp_file,"end_ns,draws,queue_ns,wait_ns,pool_ns,result,site\n");
        }
    }
    if (!vhp_file) return 0;
    Uint64 t = SDL_GetPerformanceCounter();
    return (t/vhp_frequency)*1000000000u + ((t%vhp_frequency)*1000000000u)/vhp_frequency;
}
static void vhp_record(u32 draws, Uint64 begin, Uint64 queued, Uint64 waited, int result, const char* site)
{
    if (!begin) return;
    Uint64 end = vhp_tick();
    fprintf(vhp_file,"%llu,%u,%llu,%llu,%llu,%d,%s\n",
            (unsigned long long)end,draws,
            (unsigned long long)(queued-begin),(unsigned long long)(waited-queued),
            (unsigned long long)(end-waited),result,site);
    if (++vhp_rows % 256 == 0) fflush(vhp_file);
}
static void vhp_close(void)
{
    if (vhp_file) fclose(vhp_file);
    vhp_file=NULL; vhp_initialized=0; vhp_rows=0;
}
#endif
