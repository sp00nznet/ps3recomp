/* Complete-frame PPM publication for diagnostic readback. */
#ifndef PS3RECOMP_RSX_FRAME_DUMP_H
#define PS3RECOMP_RSX_FRAME_DUMP_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/* One renderer owns a dump path. Replace only after a complete write/close,
 * so an unrelated strict-stop thread cannot truncate the last complete frame.
 * A killed process may leave the disposable .tmp sibling for the next dump. */
static inline int rsx_dump_ppm(const char* path, const unsigned char* rgba,
                               uint32_t width, uint32_t height)
{
    if (!path || !rgba || !width || !height || (uint64_t)width*height>SIZE_MAX/4)
        return -1;
    size_t size=strlen(path);
    if(size>SIZE_MAX-5 || (uint64_t)width*3>SIZE_MAX) return -1;
    char* temporary=(char*)malloc(size+5);
    unsigned char* row=(unsigned char*)malloc((size_t)width*3);
    if(!temporary || !row) { free(temporary);free(row);return -1; }
    memcpy(temporary,path,size);memcpy(temporary+size,".tmp",5);
    FILE* f=fopen(temporary,"wb");
    int failed=!f;
    if(f) {
        failed=fprintf(f,"P6\n%u %u\n255\n",width,height)<0;
        for(uint32_t y=0;y<height && !failed;++y) {
            const unsigned char* src=rgba+(size_t)y*width*4;
            for(uint32_t x=0;x<width;++x) memcpy(row+(size_t)x*3,src+(size_t)x*4,3);
            failed=fwrite(row,3,width,f)!=width;
        }
        if(fclose(f)!=0)failed=1;
        if(!failed) {
#ifdef _WIN32
            failed=!MoveFileExA(temporary,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
#else
            failed=rename(temporary,path)!=0;
#endif
        }
        if(failed)remove(temporary);
    }
    free(row);free(temporary);
    return failed?-1:0;
}
#endif
