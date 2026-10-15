#include "ps3emu/strict_fs.h"
#include <process.h>
static int child(const char* op,const char* a,const char* b) {
 if(!strcmp(op,"write")){FILE*f=ps3_strict_fs_fopen(a,"wb");if(!f)return 3;ps3_strict_fs_fwrite("ok",1,2,f);fclose(f);return 0;}
 if(!strcmp(op,"descriptor")){FILE*f=fopen(a,"r+b");if(!f)return 3;ps3_strict_fs_fwrite("ok",1,2,f);fclose(f);return 0;}
 if(!strcmp(op,"rename"))return ps3_strict_fs_rename(a,b)?3:0;
 if(!strcmp(op,"mkdir")){int r=ps3_strict_fs_mkdir(a,0755);return r==0||errno==EEXIST?0:3;}
 if(!strcmp(op,"stub")){ps3_strict_fs_unimplemented("test-stub",a);return 0;}
 return 4;
}
static int test(const char* exe,const char* op,const char* a,const char* b,int expected) {
 intptr_t rc=_spawnl(_P_WAIT,exe,exe,"case",op,a,b,NULL);
 printf("TEST %s %s exit=%lld expected=%d\n",op,a,(long long)rc,expected);
 return rc==expected?0:1;
}
int main(int argc,char** argv) {
 if(argc>=4&&!strcmp(argv[1],"case"))return child(argv[2],argv[3],argc>4?argv[4]:"");
 /* _spawnl argument list yields argc=5. */
 if(argc==5&&!strcmp(argv[1],"case"))return child(argv[2],argv[3],argv[4]);
 if(argc!=2)return 2;
 char root[MAX_PATH],sibling[MAX_PATH],outside[MAX_PATH],inside[MAX_PATH],renamed[MAX_PATH],attack[MAX_PATH],link[MAX_PATH];
 snprintf(root,sizeof root,"%s\\allowed",argv[1]);snprintf(sibling,sizeof sibling,"%s\\allowed-sibling",argv[1]);
 snprintf(outside,sizeof outside,"%s\\outside.txt",argv[1]);snprintf(inside,sizeof inside,"%s\\inside.txt",root);snprintf(renamed,sizeof renamed,"%s\\renamed.txt",root);
 _putenv_s("PS3_STRICT_FS_READONLY","");_putenv_s("PS3_STRICT_FS_WRITE_ROOT","");
 if(_mkdir(argv[1])!=0)return 2;_mkdir(root);_mkdir(sibling);FILE*f=fopen(outside,"wb");if(!f)return 2;fwrite("preserve",1,8,f);fclose(f);
 _putenv_s("PS3_STRICT_FS_WRITE_ROOT",root);int failed=0;
 failed+=test(argv[0],"write",inside,"",0);
 failed+=test(argv[0],"descriptor",inside,"",0);
 failed+=test(argv[0],"write",outside,"",86);
 failed+=test(argv[0],"descriptor",outside,"",86);
 snprintf(attack,sizeof attack,"%s\\bad.txt",sibling);failed+=test(argv[0],"write",attack,"",86);
 snprintf(attack,sizeof attack,"%s\\..\\outside.txt",root);failed+=test(argv[0],"write",attack,"",86);
 snprintf(attack,sizeof attack,"%s:stream",inside);failed+=test(argv[0],"write",attack,"",86);
 failed+=test(argv[0],"write","\\\\server\\share\\x","",86);
 failed+=test(argv[0],"write","\\\\?\\C:\\x","",86);
 failed+=test(argv[0],"write","NUL","",86);
 failed+=test(argv[0],"rename",inside,outside,86);
 failed+=test(argv[0],"rename",outside,renamed,86);
 failed+=test(argv[0],"rename",inside,renamed,0);
 failed+=test(argv[0],"mkdir","Q:","",0);
 snprintf(attack,sizeof attack,"%s\\newdir",root);failed+=test(argv[0],"mkdir",attack,"",0);
 snprintf(attack,sizeof attack,"%s\\new-outside",argv[1]);failed+=test(argv[0],"mkdir",attack,"",86);
 failed+=test(argv[0],"stub",root,"",86);
 snprintf(link,sizeof link,"%s\\link",root);
 if(CreateSymbolicLinkA(link,sibling,SYMBOLIC_LINK_FLAG_DIRECTORY|2)){
   snprintf(attack,sizeof attack,"%s\\escape.txt",link);failed+=test(argv[0],"write",attack,"",86);RemoveDirectoryA(link);
 }else{printf("SKIP symlink creation unavailable error=%lu\n",GetLastError());}
 snprintf(link,sizeof link,"%s\\hardlink.txt",root);
 if(CreateHardLinkA(link,outside,NULL)) {failed+=test(argv[0],"write",link,"",86);DeleteFileA(link);}else{printf("SKIP hardlink creation error=%lu\n",GetLastError());}
 _putenv_s("PS3_STRICT_FS_READONLY","1");failed+=test(argv[0],"write",renamed,"",86);failed+=test(argv[0],"mkdir",root,"",86);
 _putenv_s("PS3_STRICT_FS_READONLY","0");_putenv_s("PS3_STRICT_FS_WRITE_ROOT","");failed+=test(argv[0],"write",inside,"",0);
 f=fopen(outside,"rb");char check[9]={0};if(!f)return 2;fread(check,1,8,f);fclose(f);if(strcmp(check,"preserve"))failed++;
 printf("RESULT failures=%d outside-preserved=%d\n",failed,!strcmp(check,"preserve"));return failed?1:0;
}
