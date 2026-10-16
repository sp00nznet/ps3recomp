#include "../libs/video/rsx_capture_depth.h"
#include <stdlib.h>
static unsigned checks;
#define CHECK(v) do {++checks;if(!(v)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#v);return 1;}}while(0)
int main(int argc,char** argv)
{
    CHECK(argc==2);
    FILE* f=fopen(argv[1],"wb");CHECK(f);
    const uint32_t bits[]={0,0x80000000u,0x3f800000u,0x3f000000u,0x3f7fffffu,0x7f800000u,0x7fc12345u};
    float values[7];memcpy(values,bits,sizeof(values));
    CHECK(rsx_capture_depth_write(f,values,7)==0);
    CHECK(rsx_capture_depth_write(f,NULL,0)==0);
    CHECK(rsx_capture_depth_write(f,NULL,1)==-1);
    CHECK(rsx_capture_depth_write(f,values,RSX_CAPTURE_DEPTH_MAX_BYTES/4u+1u)==-1);
    CHECK(fclose(f)==0);CHECK(rsx_capture_depth_write(NULL,values,7)==-1);
    f=fopen(argv[1],"rb");CHECK(f);
    for(size_t i=0;i<7;++i)for(unsigned j=0;j<4;++j)CHECK(fgetc(f)==(int)((bits[i]>>(8*j))&255));
    CHECK(fgetc(f)==EOF);CHECK(rsx_capture_depth_write(f,values,1)==-1);CHECK(fclose(f)==0);
    f=fopen(argv[1],"wb");CHECK(f);
    const size_t count=2049;float* larger=malloc(count*sizeof(float));CHECK(larger);
    for(size_t i=0;i<count;++i)larger[i]=(float)i;
    CHECK(rsx_capture_depth_write(f,larger,count)==0);CHECK(fclose(f)==0);
    f=fopen(argv[1],"rb");CHECK(f);
    for(size_t i=0;i<count;++i) {
        uint32_t expected;memcpy(&expected,larger+i,4);
        for(unsigned j=0;j<4;++j)CHECK(fgetc(f)==(int)((expected>>(8*j))&255));
    }
    CHECK(fgetc(f)==EOF);CHECK(fclose(f)==0);free(larger);
    printf("raw depth capture: %u checks passed\n",checks);return 0;
}
