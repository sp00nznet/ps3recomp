#if defined(RSX_DLSS_LOADER_MOCK)
/* Build as three DLL fixtures: SR-only, partial FG, and complete FG. */
#define EXPORT(name) __declspec(dllexport) void name(void) {}
EXPORT(rsx_dlss_instance_extensions)
EXPORT(rsx_dlss_device_extensions)
EXPORT(rsx_dlss_create)
EXPORT(rsx_dlss_destroy)
EXPORT(rsx_dlss_get_report)
EXPORT(rsx_dlss_optimal_size)
EXPORT(rsx_dlss_configure)
EXPORT(rsx_dlss_evaluate)
#if defined(RSX_DLSS_LOADER_MOCK_PARTIAL_FG) || defined(RSX_DLSS_LOADER_MOCK_FULL_FG)
EXPORT(rsx_dlss_fg_instance_extensions)
#endif
#if defined(RSX_DLSS_LOADER_MOCK_FULL_FG)
EXPORT(rsx_dlss_fg_device_extensions)
EXPORT(rsx_dlss_fg_capability)
EXPORT(rsx_dlss_fg_get_report)
EXPORT(rsx_dlss_fg_configure)
EXPORT(rsx_dlss_fg_evaluate)
#endif
#else
#include "../libs/video/rsx_dlss_loader.h"
#include <stdlib.h>
static rsx_dlss_loader loader,saved,empty;
static int checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(int argc,char **argv)
{
    if(argc!=4){fprintf(stderr,"Usage: test_dlss_loader bridge runtime logs\n");return 2;}
    wchar_t bridge[32768],runtime[32768],logs[32768];char error[256];
    CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,argv[1],-1,bridge,32768)>0);
    CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,argv[2],-1,runtime,32768)>0);
    CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,argv[3],-1,logs,32768)>0);
    const char *uuid="2924e792-424a-48cf-9944-757b36582b73";
    CHECK(!rsx_dlss_loader_open(&loader,L"missing.dll",runtime,logs,uuid,error,sizeof error));
    CHECK(!memcmp(&loader,&empty,sizeof loader));
    CHECK(!rsx_dlss_loader_open(&loader,bridge,runtime,logs,"",error,sizeof error));
    CHECK(!memcmp(&loader,&empty,sizeof loader));
    CHECK(rsx_dlss_loader_open(&loader,bridge,runtime,logs,uuid,error,sizeof error));
    CHECK(loader.module && loader.instance_extensions && loader.device_extensions && loader.create && loader.destroy);
    CHECK(loader.get_report && loader.optimal_size && loader.configure && loader.evaluate);
    int fg_exports=0;
    static const char *const fg_names[]={
        "rsx_dlss_fg_instance_extensions", "rsx_dlss_fg_device_extensions", "rsx_dlss_fg_capability",
        "rsx_dlss_fg_get_report", "rsx_dlss_fg_configure", "rsx_dlss_fg_evaluate"};
    for(size_t i=0;i<6;i++)fg_exports+=GetProcAddress(loader.module,fg_names[i])!=NULL;
    CHECK((fg_exports==6)==(loader.fg_instance_extensions!=NULL));
    CHECK((fg_exports==6)==(loader.fg_device_extensions!=NULL));
    CHECK((fg_exports==6)==(loader.fg_capability!=NULL));
    CHECK((fg_exports==6)==(loader.fg_get_report!=NULL));
    CHECK((fg_exports==6)==(loader.fg_configure!=NULL));
    CHECK((fg_exports==6)==(loader.fg_evaluate!=NULL));
    CHECK(loader.identity.project_uuid==loader.uuid);
    CHECK(loader.identity.runtime_directory_utf16==(const uint16_t*)loader.runtime_path);
    CHECK(loader.identity.log_directory_utf16==(const uint16_t*)loader.log_path);
    saved=loader;
    CHECK(!rsx_dlss_loader_open(&loader,bridge,runtime,logs,uuid,error,sizeof error));
    CHECK(!memcmp(&loader,&saved,sizeof loader));
    rsx_dlss_loader_close(&loader);CHECK(!memcmp(&loader,&empty,sizeof loader));
    rsx_dlss_loader_close(&loader);CHECK(!memcmp(&loader,&empty,sizeof loader));
    printf("DLSS loader: PASS %d checks; SR exports=8 FG exports=%d FG group enabled=%d; no GPU evaluation\n",
        checks,fg_exports,fg_exports==6);
    return 0;
}
#endif
