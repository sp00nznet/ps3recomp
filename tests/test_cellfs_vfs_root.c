/* Real CellFs translator plus sys_fs fallback, reading real temporary files. */
#include "../libs/filesystem/cellFs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* sys_fs.c asks cellGame whether the booted title is an HDD game; this fixture
 * has no cellGame, and ld64 will not leave a weak reference undefined. */
const char* cellGame_get_hdd_title_id(void) { return 0; }

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static int read_fixture(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr,"cannot open translated path: %s\n",path); return 0; }
    char data[16] = {0}; size_t n = fread(data,1,sizeof(data),f); fclose(f);
    return n == 9 && memcmp(data,"disc-data",9) == 0;
}
int main(int argc,char**argv) {
    CHECK(argc==4);
    const char* mode=argv[1]; const char* root=argv[2]; const char* expected=argv[3];
    const char* guest="/dev_bdvd/PS3_GAME/USRDIR/movie.pam";
    char out[1024];
    if (!strcmp(mode,"absolute")) {
        cellfs_set_root_path("must-not-prefix-absolute-mapping");
        cellfs_add_path_mapping("/dev_bdvd/",root);
    } else if (!strcmp(mode,"relative")) {
        cellfs_set_root_path(root);
        cellfs_add_path_mapping("/dev_bdvd/","assets/");
    }
    CHECK(cellfs_translate_path(guest,out,sizeof(out))==0);
    CHECK(strcmp(out,expected)==0);
    CHECK(read_fixture(out));
    char device[1024];
    CHECK(cellfs_translate_path("/dev_bdvd",device,sizeof(device))==0);
    CHECK(strlen(device)>0);
    CHECK(cellfs_translate_path("/dev_bdvd_typo/file",out,sizeof(out))==-1);
    struct { char out[4]; char sentinel[4]; } small;
    memset(&small,0xA5,sizeof(small));
    CHECK(cellfs_translate_path(guest,small.out,sizeof(small.out))==-1);
    CHECK(!memcmp(small.sentinel,"\xA5\xA5\xA5\xA5",4));
    CHECK(cellfs_translate_path(guest,out,0)==-1);
    CHECK(cellfs_translate_path(NULL,out,sizeof(out))==-1);
    CHECK(cellfs_translate_path(guest,NULL,sizeof(out))==-1);
    if (!strcmp(mode,"env")) {
        CHECK(cellfs_translate_path("/dev_hdd0/game/x",out,sizeof(out))==0);
        CHECK(!strcmp(out,"./gamedata/dev_hdd0/game/x"));
        /* Real fallback expands USRDIR into PS3_GAME/USRDIR. Its result must
         * not report success after truncating into the caller's buffer. */
        char direct[1024]; snprintf(direct,sizeof(direct),"%s/USRDIR/movie.pam",root);
        CHECK(cellfs_translate_path("/dev_bdvd/USRDIR/movie.pam",out,strlen(direct)+1)==-1);
        CHECK(cellfs_translate_path("/dev_bdvd/USRDIR/movie.pam",out,sizeof(out))==0);
        CHECK(!strcmp(out,expected)); CHECK(read_fixture(out));
    }
    printf("%s: %u CellFs VFS checks PASS\n",mode,checks);
    return 0;
}
