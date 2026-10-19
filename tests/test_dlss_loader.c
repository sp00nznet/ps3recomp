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
    CHECK(loader.identity.project_uuid==loader.uuid);
    CHECK(loader.identity.runtime_directory_utf16==(const uint16_t*)loader.runtime_path);
    CHECK(loader.identity.log_directory_utf16==(const uint16_t*)loader.log_path);
    saved=loader;
    CHECK(!rsx_dlss_loader_open(&loader,bridge,runtime,logs,uuid,error,sizeof error));
    CHECK(!memcmp(&loader,&saved,sizeof loader));
    rsx_dlss_loader_close(&loader);CHECK(!memcmp(&loader,&empty,sizeof loader));
    rsx_dlss_loader_close(&loader);CHECK(!memcmp(&loader,&empty,sizeof loader));
    printf("DLSS loader: PASS %d checks; real bridge loaded, all8exports; no GPU evaluation\n",checks);
    return 0;
}
