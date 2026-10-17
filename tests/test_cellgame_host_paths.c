/* Reuse the isolated guest-memory and callback stubs of the full SFO fixture. */
#define main gamedata_fixture_main
#include "test_folklore_gamedata.c"
#undef main

int main(int argc, char** argv)
{
    if(argc != 2) return 2; /* An explicit owned scratch directory. */
    char root[200], expected[256], overflow[CELL_GAME_HOST_PATH_MAX + 16];
    memset(root, 'r', sizeof root - 1); root[sizeof root - 1] = 0;
    memset(overflow, 'x', sizeof overflow - 1); overflow[sizeof overflow - 1] = 0;
    _putenv_s("PS3_HDD0_ROOT", root);
    s_content_path_resolved = 0;
    snprintf(expected, sizeof expected, "%s/game", root);
    check(content_root() && !strcmp(content_root(), expected), "long environment root preserved");
    check(CELL_GAME_PATH_MAX == 128, "guest path ABI unchanged");

    _putenv_s("PS3_HDD0_ROOT", "");
    ppu_vfs_root = root; s_content_path_resolved = 0;
    check(content_root() && !strcmp(content_root(), expected), "long VFS root preserved");
    cellGame_set_content_path(root);
    check(content_root() && !strcmp(content_root(), root), "long explicit root preserved");
    check(cellGameBootCheck(NULL, NULL, NULL, NULL) == CELL_OK, "long boot paths supported");
    snprintf(expected, sizeof expected, "%s/BLES00000/USRDIR", root);
#ifdef _WIN32
    for(char* p=expected;*p;++p)if(*p=='/')*p='\\';
#endif
    check(!strcmp(s_usrdir_path, expected), "boot USRDIR is complete");

    _putenv_s("PS3_HDD0_ROOT", overflow); s_content_path_resolved = 0;
    check(content_root() == NULL, "oversized environment root rejected");
    check(cellGameBootCheck(NULL, NULL, NULL, NULL) == CELL_GAME_ERROR_PARAM, "invalid root fails boot");
    cellGame_set_content_path(overflow);
    check(content_root() == NULL, "oversized explicit root rejected");
    check(cellGameDataCheck(CELL_GAME_GAMETYPE_DISC, NULL, NULL) == CELL_GAME_ERROR_PARAM,
          "invalid root fails data check without filesystem access");

    vm_base = calloc(1, ppu_vm_size);
    if(!vm_base) return 2;
    strcpy((char*)vm_base+0x1000, "TEST12345"); g_ps3_guest_caller = callback;
    check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)
          == CELL_GAMEDATA_ERROR_PARAM, "invalid root fails callback path");
    check(calls == 0, "invalid root never dispatches callback");
    check(!atomic_flag_test_and_set(&gamedata_in_callback), "callback guard released on invalid root");
    atomic_flag_clear(&gamedata_in_callback);
    cellGame_set_content_path(root);
    check(content_root() && !strcmp(content_root(), root), "valid explicit root recovers after rejection");

    cellGame_set_content_path(argv[1]);
    ppu_vfs_root = NULL;
    s_check_is_disc = 0;
    strcpy(s_check_dir, "TEST12345");
    check(cellGameContentPermit((char*)(uintptr_t)0x1100,(char*)(uintptr_t)0x1200) == CELL_OK,
          "explicit host root permits content with NULL VFS root");
    check(!strcmp((char*)vm_base+0x1100,"/dev_hdd0/game/TEST12345"), "guest content path unchanged");
    check(!strcmp((char*)vm_base+0x1200,"/dev_hdd0/game/TEST12345/USRDIR"), "guest USRDIR path unchanged");
    char actual[CELL_GAME_HOST_PATH_MAX];
    snprintf(actual,sizeof actual,"%s/TEST12345/USRDIR",argv[1]);
    check(dir_exists(actual), "content created under full explicit host root");
    ppu_vfs_root = overflow;
    check(cellGameContentPermit(NULL,NULL) == CELL_OK, "overridden VFS root not used as second destination");
    cellGame_set_content_path(overflow);
    check(cellGameContentPermit(NULL,NULL) == CELL_GAME_ERROR_PARAM, "invalid root rejected before directory creation");
    free(vm_base);
    printf("host path checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
