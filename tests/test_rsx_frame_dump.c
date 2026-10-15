#include "../libs/video/rsx_frame_dump.h"
int main(int argc,char** argv)
{
    if(argc!=2)return 2;
    const unsigned char pixels[]={255,0,0,255,0,255,0,255};
    if(rsx_dump_ppm(argv[1],pixels,2,1))return 3;
    if(rsx_dump_ppm(argv[1],pixels,0,1)!=-1)return 4;
    unsigned char actual[64]={0};
    FILE* f=fopen(argv[1],"rb");if(!f)return 5;
    size_t size=fread(actual,1,sizeof(actual),f);fclose(f);
    const unsigned char expected[]={'P','6','\n','2',' ','1','\n','2','5','5','\n',255,0,0,0,255,0};
    if(size!=sizeof(expected)||memcmp(actual,expected,size))return 6;
    const unsigned char replacement[]={0,0,255,255};
    if(rsx_dump_ppm(argv[1],replacement,1,1))return 7;
    f=fopen(argv[1],"rb");if(!f)return 8;
    size=fread(actual,1,sizeof(actual),f);fclose(f);
    if(size!=14||actual[3]!='1'||actual[11]!=0||actual[12]!=0||actual[13]!=255)return 9;
    puts("frame dump: exact RGB, rejected write preserves frame, replacement passed");
    return 0;
}
