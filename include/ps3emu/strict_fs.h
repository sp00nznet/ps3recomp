/* Host guest-I/O containment for diagnostic runs. Not an OS sandbox and
 * not resistant to concurrent filesystem substitution (TOCTOU). */
#ifndef PS3EMU_STRICT_FS_H
#define PS3EMU_STRICT_FS_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#include "ps3emu/strict_fs_posix.h"
#ifdef __ANDROID__
#include <fcntl.h>
#include <sys/syscall.h>
#endif
#endif
static inline int ps3_strict_fs_readonly(void) {
    const char* e=getenv("PS3_STRICT_FS_READONLY"); return e && !strcmp(e,"1");
}
static inline const char* ps3_strict_fs_root(void) {
    const char* e=getenv("PS3_STRICT_FS_WRITE_ROOT"); return e && *e ? e : NULL;
}
static inline int ps3_strict_fs_enabled(void) {
    return ps3_strict_fs_readonly() || ps3_strict_fs_root()!=NULL;
}
static inline void ps3_strict_fs_fail(const char* op,const char* path,const char* reason) {
    fprintf(stderr,"[strict-fs] FATAL category=filesystem-mutation operation=%s path=%s reason=%s exit=86\n",op,path?path:"<unavailable>",reason);
    fflush(NULL); _Exit(86);
}
#ifdef _WIN32
/* Deliberately bounded ANSI paths. Reject ambiguous Win32 spellings instead
 * of relying on normalization to erase ADS, traversal, device or alias syntax. */
static inline int ps3_strict_fs_path_syntax(const char* p) {
    if(!p || strlen(p)>=MAX_PATH || !(((p[0]>='A'&&p[0]<='Z')||(p[0]>='a'&&p[0]<='z')) && p[1]==':' && (p[2]=='/'||p[2]=='\\'))) return 0;
    const char* s=p+3;
    while(*s) {
        const char* e=s; while(*e && *e!='/' && *e!='\\') {
            unsigned char c=(unsigned char)*e;
            if(c<32 || c>=127 || strchr(":<>\"|?*",c)) return 0;
            ++e;
        }
        size_t n=(size_t)(e-s); if(!n || s[n-1]=='.' || s[n-1]==' ') return 0;
        char name[MAX_PATH]; memcpy(name,s,n); name[n]=0;
        char* dot=strchr(name,'.'); if(dot)*dot=0;
        if(!_stricmp(name,"CON")||!_stricmp(name,"PRN")||!_stricmp(name,"AUX")||!_stricmp(name,"NUL")||!_stricmp(name,"CLOCK$")||
           ((strlen(name)==4)&&(!_strnicmp(name,"COM",3)||!_strnicmp(name,"LPT",3))&&name[3]>='0'&&name[3]<='9')) return 0;
        s=*e?e+1:e;
    }
    return 1;
}
static inline int ps3_strict_fs_handle_path(HANDLE h,char* out) {
    char raw[MAX_PATH+8]; DWORD n=GetFinalPathNameByHandleA(h,raw,sizeof raw,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!n || n>=sizeof raw || strncmp(raw,"\\\\?\\",4)) return 0;
    if(!ps3_strict_fs_path_syntax(raw+4)) return 0;
    strcpy(out,raw+4); return 1;
}
/* Check every existing ancestor without following reparse points, resolve the
 * deepest existing parent by handle, then append only validated new components. */
static inline int ps3_strict_fs_canonical(const char* path,char* out,int must_exist) {
    char full[MAX_PATH], probe[MAX_PATH];
    if(!ps3_strict_fs_path_syntax(path)) return 0;
    DWORD n=GetFullPathNameA(path,MAX_PATH,full,NULL); if(!n||n>=MAX_PATH)return 0;
    for(char* p=full;*p;p++)if(*p=='/')*p='\\';
    size_t len=strlen(full); while(len>3&&full[len-1]=='\\')full[--len]=0;
    for(size_t i=3;i<=len;i++) if(full[i]=='\\'||!full[i]) {
        char saved=full[i]; full[i]=0; DWORD attr=GetFileAttributesA(full); full[i]=saved;
        if(attr!=INVALID_FILE_ATTRIBUTES) { if(attr&FILE_ATTRIBUTE_REPARSE_POINT)return 0; }
        else { DWORD error=GetLastError(); if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)return 0; break; }
    }
    strcpy(probe,full);
    while(GetFileAttributesA(probe)==INVALID_FILE_ATTRIBUTES) {
        if(must_exist)return 0;
        DWORD error=GetLastError(); if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)return 0;
        char* slash=strrchr(probe,'\\'); if(!slash)return 0;
        if(slash==probe+2){probe[3]=0;break;} *slash=0;
    }
    DWORD attr=GetFileAttributesA(probe); if(attr==INVALID_FILE_ATTRIBUTES||(attr&FILE_ATTRIBUTE_REPARSE_POINT))return 0;
    if(strlen(probe)<len && !(attr&FILE_ATTRIBUTE_DIRECTORY))return 0;
    HANDLE h=CreateFileA(probe,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(h==INVALID_HANDLE_VALUE)return 0;
    BY_HANDLE_FILE_INFORMATION info;
    int links_ok=(attr&FILE_ATTRIBUTE_DIRECTORY) || (GetFileInformationByHandle(h,&info)&&info.nNumberOfLinks==1);
    int ok=links_ok&&ps3_strict_fs_handle_path(h,out); CloseHandle(h); if(!ok)return 0;
    const char* suffix=full+strlen(probe);
    if(strlen(out)+strlen(suffix)>=MAX_PATH)return 0;
    strcat(out,suffix); return 1;
}
static inline int ps3_strict_fs_within(const char* path,const char* root) {
    size_t n=strlen(root); if(_strnicmp(path,root,n))return 0;
    return !path[n] || (n==3&&root[2]=='\\') || path[n]=='\\';
}
#endif
static inline void ps3_strict_fs_mutation(const char* operation,const char* path) {
    if(ps3_strict_fs_readonly())ps3_strict_fs_fail(operation,path,"readonly-mutation");
    const char* root=ps3_strict_fs_root(); if(!root)return;
#ifdef _WIN32
    char canonical_root[MAX_PATH], canonical_path[MAX_PATH];
    if(!ps3_strict_fs_canonical(root,canonical_root,1) || !(GetFileAttributesA(canonical_root)&FILE_ATTRIBUTE_DIRECTORY))ps3_strict_fs_fail(operation,path,"invalid-write-root");
    if(!ps3_strict_fs_canonical(path,canonical_path,0))ps3_strict_fs_fail(operation,path,"invalid-path-or-reparse");
    if(!ps3_strict_fs_within(canonical_path,canonical_root))ps3_strict_fs_fail(operation,path,"outside-write-root");
#else
    const char* reason=ps3_strict_fs_posix_path(root,path);
    if(reason)ps3_strict_fs_fail(operation,path,reason);
#endif
}
static inline void ps3_strict_fs_unimplemented(const char* operation,const char* path) {
    if(ps3_strict_fs_enabled())ps3_strict_fs_fail(operation,path,"unimplemented-mutation");
}
#ifdef _WIN32
static inline void ps3_strict_fs_native_handle(const char* operation,HANDLE handle) {
    if(!ps3_strict_fs_enabled())return;
    if(ps3_strict_fs_readonly())ps3_strict_fs_fail(operation,"<open handle>","readonly-mutation");
    char path[MAX_PATH]; BY_HANDLE_FILE_INFORMATION info;
    if(!ps3_strict_fs_handle_path(handle,path))ps3_strict_fs_fail(operation,NULL,"unresolved-handle-path");
    if(!GetFileInformationByHandle(handle,&info)||info.nNumberOfLinks!=1)ps3_strict_fs_fail(operation,path,"ambiguous-file-links");
    ps3_strict_fs_mutation(operation,path);
}
#endif
static inline void ps3_strict_fs_stream(const char* operation,FILE* stream) {
    (void)stream;
    if(!ps3_strict_fs_enabled())return;
    if(ps3_strict_fs_readonly())ps3_strict_fs_fail(operation,"<open stream>","readonly-mutation");
#ifdef _WIN32
    if(!stream)ps3_strict_fs_fail(operation,NULL,"invalid-stream");
    intptr_t raw=_get_osfhandle(_fileno(stream)); char path[MAX_PATH];
    if(raw==-1 || !ps3_strict_fs_handle_path((HANDLE)raw,path))ps3_strict_fs_fail(operation,NULL,"unresolved-stream-path");
    /* Hard links can name the same file outside the allowed tree. */
    BY_HANDLE_FILE_INFORMATION info;
    if(!GetFileInformationByHandle((HANDLE)raw,&info)||info.nNumberOfLinks!=1)ps3_strict_fs_fail(operation,path,"ambiguous-file-links");
    ps3_strict_fs_mutation(operation,path);
#else
    const char* reason=ps3_strict_fs_posix_stream(ps3_strict_fs_root(),stream);
    if(reason)ps3_strict_fs_fail(operation,NULL,reason);
#endif
}
static inline FILE* ps3_strict_fs_fopen(const char* path,const char* mode) {
    int write_mode=mode&&strpbrk(mode,"wa+")!=NULL;
    if(write_mode)ps3_strict_fs_mutation("fopen-write",path);
    FILE* f=fopen(path,mode); if(f&&write_mode)ps3_strict_fs_stream("fopen-write-handle",f); return f;
}
static inline size_t ps3_strict_fs_fwrite(const void* data,size_t size,size_t count,FILE* stream) {
    ps3_strict_fs_stream("fwrite",stream); return fwrite(data,size,count,stream);
}
static inline FILE* ps3_strict_fs_tmpfile(void) {
    ps3_strict_fs_unimplemented("tmpfile","<anonymous temporary file>"); return tmpfile();
}
static inline int ps3_strict_fs_remove(const char* path) {
    ps3_strict_fs_mutation("remove",path); return remove(path);
}
/* Unlink is file-only: C remove() may delete an empty directory on POSIX.
 * Keep the same containment checks as the other host mutation wrappers. */
static inline int ps3_strict_fs_unlink(const char* path) {
    ps3_strict_fs_mutation("unlink",path);
#ifdef _WIN32
    /* The CRT reports EACCES for directories; PS3 reports EISDIR. */
    struct stat st;
    if(stat(path,&st)==0 && (st.st_mode&S_IFMT)==S_IFDIR) {errno=EISDIR;return -1;}
    const int result=_unlink(path);
    const int unlink_error=errno;
    if(result!=0 && unlink_error==ENOENT) {
        /* Windows CRT folds a file used as an ancestor into ENOENT. Only
         * refine that result when an existing non-directory proves it. */
        char ancestor[1100];
        const size_t length=strlen(path);
        if(length<sizeof ancestor) {
            memcpy(ancestor,path,length+1);
            for(size_t i=length;i>0;--i) {
                if(ancestor[i-1]!='/' && ancestor[i-1]!='\\')continue;
                ancestor[i-1]=0;
                if(stat(ancestor,&st)==0) {
                    if((st.st_mode&S_IFMT)!=S_IFDIR) {errno=ENOTDIR;return result;}
                    break;
                }
            }
        }
    }
    errno=unlink_error;
    return result;
#else
    return unlink(path);
#endif
}
static inline int ps3_strict_fs_rename(const char* from,const char* to) {
    ps3_strict_fs_mutation("rename-source",from); ps3_strict_fs_mutation("rename-destination",to); return rename(from,to);
}
/* A new metadata commit must never replace a competing creator's file. */
static inline int ps3_strict_fs_commit_new_file(const char* from,const char* to) {
    ps3_strict_fs_mutation("commit-new-source",from);
    ps3_strict_fs_mutation("commit-new-destination",to);
#ifdef _WIN32
    if(!MoveFileExA(from,to,MOVEFILE_WRITE_THROUGH)) {errno=EACCES;return -1;}
    return 0;
#elif defined(__ANDROID__)
    /* Android app policy forbids hard links, including within private storage.
     * Use the kernel's atomic no-replace rename without the API 30 libc symbol
     * so API 26 builds remain loadable. Unsupported kernels fail closed. */
    return (int)syscall(SYS_renameat2,AT_FDCWD,from,AT_FDCWD,to,RENAME_NOREPLACE);
#else
    /* link atomically creates a new name and fails if that name exists.
     * Unlike renameat2(NOREPLACE), it also works on the tested WSL Q mount.
     * Both names have been validated above. The temporary second link is
     * removed directly here, without passing through the single-link guard. */
    if(link(from,to))return -1;
    return unlink(from);
#endif
}
/* Atomic sibling replacement for completed metadata files. Windows CRT rename
 * refuses an existing destination, so validate both names and file identities
 * before MoveFileEx. This is containment, not a TOCTOU-resistant OS sandbox. */
static inline int ps3_strict_fs_replace_file(const char* from,const char* to) {
    ps3_strict_fs_mutation("replace-source",from);
    ps3_strict_fs_mutation("replace-destination",to);
#ifdef _WIN32
    const char* names[2]={from,to};
    for(int i=0;i<2;i++) {
        DWORD attr=GetFileAttributesA(names[i]);
        if(attr==INVALID_FILE_ATTRIBUTES) {
            if(i==1 && (GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND))continue;
            errno=ENOENT;return -1;
        }
        if((attr&FILE_ATTRIBUTE_DIRECTORY)||(attr&FILE_ATTRIBUTE_REPARSE_POINT)) {errno=EACCES;return -1;}
        HANDLE h=CreateFileA(names[i],FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                             NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        if(h==INVALID_HANDLE_VALUE) {errno=EACCES;return -1;}
        BY_HANDLE_FILE_INFORMATION info;
        int ok=GetFileInformationByHandle(h,&info)&&info.nNumberOfLinks==1;
        CloseHandle(h);
        if(!ok){errno=EACCES;return -1;}
    }
    if(!MoveFileExA(from,to,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {errno=EACCES;return -1;}
    return 0;
#else
    return rename(from,to);
#endif
}
static inline int ps3_strict_fs_mkdir(const char* path,int mode) {
    if(ps3_strict_fs_readonly())ps3_strict_fs_fail("mkdir",path,"readonly-mutation");
#ifdef _WIN32
    if(ps3_strict_fs_root()) {
        char expanded[MAX_PATH], canonical[MAX_PATH]; const char* checked=path;
        if(path&&strlen(path)==2&&path[1]==':'){snprintf(expanded,sizeof expanded,"%s\\",path);checked=expanded;}
        /* Returning EEXIST here makes ancestor checks mutation-free even when
         * the existing ancestor is outside the configured write root. */
        if(ps3_strict_fs_canonical(checked,canonical,1) && (GetFileAttributesA(canonical)&FILE_ATTRIBUTE_DIRECTORY)) {errno=EEXIST;return -1;}
    }
#else
    if(ps3_strict_fs_root()) {
        char checked[PS3_STRICT_FS_POSIX_CAP];
        /* Existing safe ancestors need no mutation even outside the root. */
        if(!ps3_strict_fs_posix_syntax(path,checked) &&
           !ps3_strict_fs_posix_walk(checked,1,1)) {errno=EEXIST;return -1;}
    }
#endif
    ps3_strict_fs_mutation("mkdir",path);
#ifdef _WIN32
    (void)mode; return _mkdir(path);
#else
    return mkdir(path,(mode_t)mode);
#endif
}
#endif
