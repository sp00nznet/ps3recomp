/*
 * ps3recomp - cellSaveData HLE implementation
 *
 * Game save data management with callback-driven flow.
 * Save data is stored under: {root}/gamedata/dev_hdd0/home/00000001/savedata/{dirName}/
 */

#include "cellSaveData.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#  define HOST_MKDIR(p) _mkdir(p)
#  define HOST_STAT     _stat64
#  define HOST_STAT_T   struct __stat64
#else
#  include <unistd.h>
#  include <dirent.h>
#  include <sys/types.h>
#  define HOST_MKDIR(p) mkdir(p, 0755)
#  define HOST_STAT     stat
#  define HOST_STAT_T   struct stat
#endif

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -----------------------------------------------------------------------*/

static char s_save_root[1024] = "./gamedata/dev_hdd0/home/00000001/savedata";

/* Ensure directory (and parents) exist */
static void ensure_dirs(const char* path)
{
    char tmp[1024];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            HOST_MKDIR(tmp);
            *p = saved;
        }
    }
    HOST_MKDIR(tmp);
}

static void build_save_path(char* buf, size_t buf_size, const char* dirName)
{
    snprintf(buf, buf_size, "%s/%s", s_save_root, dirName);
#ifdef _WIN32
    for (char* p = buf; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif
}

static int dir_exists(const char* path)
{
    HOST_STAT_T st;
    if (HOST_STAT(path, &st) != 0)
        return 0;
#ifdef _WIN32
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

/* Enumerate save directories matching a prefix. Returns count, fills dirList up to max. */
static u32 enumerate_save_dirs(const char* prefix, CellSaveDataDirList* dirList, u32 max)
{
    u32 count = 0;

    ensure_dirs(s_save_root);

#ifdef _WIN32
    {
        char search[1024];
        snprintf(search, sizeof(search), "%s\\*", s_save_root);
        for (char* p = search; *p; p++) {
            if (*p == '/') *p = '\\';
        }

        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(search, &fd);
        if (hFind == INVALID_HANDLE_VALUE)
            return 0;

        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;
            if (fd.cFileName[0] == '.')
                continue;
            if (prefix && prefix[0] && strncmp(fd.cFileName, prefix, strlen(prefix)) != 0)
                continue;
            if (count < max && dirList) {
                memset(&dirList[count], 0, sizeof(CellSaveDataDirList));
                strncpy(dirList[count].dirName, fd.cFileName,
                        CELL_SAVEDATA_DIRNAME_SIZE - 1);
            }
            count++;
        } while (FindNextFileA(hFind, &fd));

        FindClose(hFind);
    }
#else
    {
        DIR* dp = opendir(s_save_root);
        if (!dp)
            return 0;

        struct dirent* de;
        while ((de = readdir(dp)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            /* Check it's a directory */
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", s_save_root, de->d_name);
            if (!dir_exists(full))
                continue;
            if (prefix && prefix[0] && strncmp(de->d_name, prefix, strlen(prefix)) != 0)
                continue;
            if (count < max && dirList) {
                memset(&dirList[count], 0, sizeof(CellSaveDataDirList));
                strncpy(dirList[count].dirName, de->d_name,
                        CELL_SAVEDATA_DIRNAME_SIZE - 1);
            }
            count++;
        }
        closedir(dp);
    }
#endif

    return count;
}

/* Enumerate files in a save directory. Returns count, fills fileList up to max. */
static u32 enumerate_save_files(const char* save_path,
                                 CellSaveDataFileStat* fileList, u32 max)
{
    u32 count = 0;

#ifdef _WIN32
    {
        char search[1024];
        snprintf(search, sizeof(search), "%s\\*", save_path);
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(search, &fd);
        if (hFind == INVALID_HANDLE_VALUE)
            return 0;

        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (count < max && fileList) {
                memset(&fileList[count], 0, sizeof(CellSaveDataFileStat));
                fileList[count].fileType = CELL_SAVEDATA_FILETYPE_NORMALFILE;
                strncpy(fileList[count].fileName, fd.cFileName,
                        CELL_SAVEDATA_FILENAME_SIZE - 1);
                ULARGE_INTEGER sz;
                sz.HighPart = fd.nFileSizeHigh;
                sz.LowPart  = fd.nFileSizeLow;
                fileList[count].st_size = (u64)sz.QuadPart;
            }
            count++;
        } while (FindNextFileA(hFind, &fd));

        FindClose(hFind);
    }
#else
    {
        DIR* dp = opendir(save_path);
        if (!dp)
            return 0;

        struct dirent* de;
        while ((de = readdir(dp)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", save_path, de->d_name);
            HOST_STAT_T st;
            if (HOST_STAT(full, &st) != 0)
                continue;
#ifdef _WIN32
            if (st.st_mode & _S_IFDIR)
                continue;
#else
            if (S_ISDIR(st.st_mode))
                continue;
#endif
            if (count < max && fileList) {
                memset(&fileList[count], 0, sizeof(CellSaveDataFileStat));
                fileList[count].fileType = CELL_SAVEDATA_FILETYPE_NORMALFILE;
                strncpy(fileList[count].fileName, de->d_name,
                        CELL_SAVEDATA_FILENAME_SIZE - 1);
                fileList[count].st_size = (u64)st.st_size;
                fileList[count].st_atime = (s64)st.st_atime;
                fileList[count].st_mtime = (s64)st.st_mtime;
                fileList[count].st_ctime = (s64)st.st_ctime;
            }
            count++;
        }
        closedir(dp);
    }
#endif

    return count;
}

/* Execute file callback: read/write/delete files in the save directory */
static s32 process_file_op(const char* save_path, CellSaveDataFileSet* set)
{
    if (!set || !set->fileName)
        return CELL_OK;

    char file_path[1024];
    snprintf(file_path, sizeof(file_path), "%s/%s", save_path, set->fileName);
#ifdef _WIN32
    for (char* p = file_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif

    switch (set->fileOperation) {
    case CELL_SAVEDATA_FILEOP_READ: {
        FILE* fp = fopen(file_path, "rb");
        if (!fp) {
            printf("[cellSaveData] file read: cannot open '%s'\n", file_path);
            return 0; /* excSize = 0 */
        }
        if (set->fileOffset > 0) {
#ifdef _MSC_VER
            _fseeki64(fp, (long long)set->fileOffset, SEEK_SET);
#else
            fseeko(fp, (off_t)set->fileOffset, SEEK_SET);
#endif
        }
        size_t read_size = (size_t)set->fileSize;
        if (read_size > set->fileBufSize) read_size = set->fileBufSize;
        size_t got = fread(set->fileBuf, 1, read_size, fp);
        fclose(fp);
        return (s32)got;
    }

    case CELL_SAVEDATA_FILEOP_WRITE:
    case CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC: {
        ensure_dirs(save_path);
        const char* mode;
        if (set->fileOperation == CELL_SAVEDATA_FILEOP_WRITE && set->fileOffset == 0) {
            mode = "wb";
        } else {
            mode = "r+b";
        }
        FILE* fp = fopen(file_path, mode);
        if (!fp) {
            fp = fopen(file_path, "wb");
        }
        if (!fp) {
            printf("[cellSaveData] file write: cannot open '%s'\n", file_path);
            return 0;
        }
        if (set->fileOffset > 0) {
#ifdef _MSC_VER
            _fseeki64(fp, (long long)set->fileOffset, SEEK_SET);
#else
            fseeko(fp, (off_t)set->fileOffset, SEEK_SET);
#endif
        }
        size_t write_size = (size_t)set->fileSize;
        if (write_size > set->fileBufSize) write_size = set->fileBufSize;
        size_t wrote = fwrite(set->fileBuf, 1, write_size, fp);
        fclose(fp);
        return (s32)wrote;
    }

    case CELL_SAVEDATA_FILEOP_DELETE:
        remove(file_path);
        return 0;

    default:
        return 0;
    }
}

/* Write a simplified PARAM.SFO with the save's title/subtitle/detail */
static void write_param_sfo(const char* save_path, const CellSaveDataSystemFileParam* param)
{
    if (!param) return;

    char sfo_path[1024];
    snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO", save_path);
#ifdef _WIN32
    for (char* p = sfo_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif

    FILE* fp = fopen(sfo_path, "wb");
    if (!fp) return;

    /* Write a simplified text-based PARAM.SFO for easy debugging.
       Games don't read this directly - our stat callback fills it from here. */
    fprintf(fp, "TITLE=%s\n", param->title);
    fprintf(fp, "SUB_TITLE=%s\n", param->subTitle);
    fprintf(fp, "DETAIL=%s\n", param->detail);
    fprintf(fp, "ATTRIBUTE=%u\n", param->attribute);
    fprintf(fp, "LIST_PARAM=%s\n", param->listParam);
    fclose(fp);
}

/* Read simplified PARAM.SFO */
static void read_param_sfo(const char* save_path, CellSaveDataSystemFileParam* param)
{
    if (!param) return;
    memset(param, 0, sizeof(CellSaveDataSystemFileParam));

    char sfo_path[1024];
    snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO", save_path);
#ifdef _WIN32
    for (char* p = sfo_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif

    FILE* fp = fopen(sfo_path, "rb");
    if (!fp) return;

    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        /* Remove trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (strncmp(line, "TITLE=", 6) == 0)
            strncpy(param->title, line + 6, CELL_SAVEDATA_SYSP_TITLE_SIZE - 1);
        else if (strncmp(line, "SUB_TITLE=", 10) == 0)
            strncpy(param->subTitle, line + 10, CELL_SAVEDATA_SYSP_SUBTITLE_SIZE - 1);
        else if (strncmp(line, "DETAIL=", 7) == 0)
            strncpy(param->detail, line + 7, CELL_SAVEDATA_SYSP_DETAIL_SIZE - 1);
        else if (strncmp(line, "ATTRIBUTE=", 10) == 0)
            param->attribute = (u32)atoi(line + 10);
        else if (strncmp(line, "LIST_PARAM=", 11) == 0)
            strncpy(param->listParam, line + 11, CELL_SAVEDATA_SYSP_LPARAM_SIZE - 1);
    }
    fclose(fp);
}

/* Core save/load implementation shared by List/Fixed/Auto variants */
static s32 savedata_execute(const char* dirName, int is_save,
                             CellSaveDataSetBuf* setBuf,
                             CellSaveDataStatCallback funcStat,
                             CellSaveDataFileCallback funcFile,
                             void* userdata)
{
    if (!dirName || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    char save_path[1024];
    build_save_path(save_path, sizeof(save_path), dirName);

    int is_new = !dir_exists(save_path);

    printf("[cellSaveData] %s dir='%s' (new=%d)\n",
           is_save ? "SAVE" : "LOAD", dirName, is_new);

    /* If loading and directory doesn't exist, it's not an error -
       the stat callback will see isNewData=1 and can handle it */

    /* Prepare stat get */
    u32 file_list_max = setBuf ? setBuf->fileListMax : 64;
    CellSaveDataFileStat* fileList = NULL;
    u32 fileNum = 0;

    if (file_list_max > 0) {
        fileList = (CellSaveDataFileStat*)calloc(file_list_max, sizeof(CellSaveDataFileStat));
        if (!is_new) {
            fileNum = enumerate_save_files(save_path, fileList, file_list_max);
        }
    }

    CellSaveDataStatGet statGet;
    memset(&statGet, 0, sizeof(statGet));
    statGet.hddFreeSizeKB = 1024 * 1024; /* report 1GB free */
    statGet.isNewData = is_new ? 1 : 0;
    strncpy(statGet.dir.dirName, dirName, CELL_SAVEDATA_DIRNAME_SIZE - 1);

    if (!is_new) {
        HOST_STAT_T hst;
        if (HOST_STAT(save_path, &hst) == 0) {
            statGet.dir.st_atime = (s64)hst.st_atime;
            statGet.dir.st_mtime = (s64)hst.st_mtime;
            statGet.dir.st_ctime = (s64)hst.st_ctime;
        }
        read_param_sfo(save_path, &statGet.getParam);
    }

    statGet.fileNum = fileNum;
    statGet.fileListNum = fileNum < file_list_max ? fileNum : file_list_max;
    statGet.fileList = fileList;
    statGet.bind = 0;
    statGet.sizeKB = 0;
    statGet.sysSizeKB = 0;

    /* Call stat callback */
    CellSaveDataCBResult cbResult;
    memset(&cbResult, 0, sizeof(cbResult));
    cbResult.result = CELL_SAVEDATA_CBRESULT_OK_NEXT;

    CellSaveDataStatSet statSet;
    memset(&statSet, 0, sizeof(statSet));

    funcStat(&cbResult, &statGet, &statSet);

    if (cbResult.result < 0) {
        printf("[cellSaveData] stat callback returned error %d\n", cbResult.result);
        free(fileList);
        if (cbResult.result == CELL_SAVEDATA_CBRESULT_ERR_NODATA)
            return CELL_SAVEDATA_ERROR_NODATA;
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }

    /* Write PARAM.SFO if stat set provided params and we're saving */
    if (is_save && statSet.setParam) {
        ensure_dirs(save_path);
        write_param_sfo(save_path, statSet.setParam);
    }

    /* Call file callback repeatedly */
    if (funcFile && cbResult.result == CELL_SAVEDATA_CBRESULT_OK_NEXT) {
        while (1) {
            CellSaveDataFileGet fileGet;
            memset(&fileGet, 0, sizeof(fileGet));

            CellSaveDataFileSet fileSet;
            memset(&fileSet, 0, sizeof(fileSet));

            cbResult.result = CELL_SAVEDATA_CBRESULT_OK_NEXT;

            funcFile(&cbResult, &fileGet, &fileSet);

            if (cbResult.result == CELL_SAVEDATA_CBRESULT_OK_LAST ||
                cbResult.result < 0) {
                /* Process last operation if set */
                if (fileSet.fileName && fileSet.fileBuf) {
                    s32 exc = process_file_op(save_path, &fileSet);
                    (void)exc;
                }
                break;
            }

            if (fileSet.fileName && fileSet.fileBuf) {
                s32 exc = process_file_op(save_path, &fileSet);
                (void)exc;
            } else {
                /* No file operation requested, done */
                break;
            }
        }
    }

    free(fileList);

    if (cbResult.result < 0) {
        if (cbResult.result == CELL_SAVEDATA_CBRESULT_ERR_NODATA)
            return CELL_SAVEDATA_ERROR_NODATA;
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }

    printf("[cellSaveData] %s complete for '%s'\n",
           is_save ? "SAVE" : "LOAD", dirName);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellSaveDataListSave2(u32 version, CellSaveDataSetList* setList,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataListCallback funcList,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata)
{
    printf("[cellSaveData] ListSave2(version=%u)\n", version);

    if (!setList || !setBuf || !funcList || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    /* Enumerate existing save dirs */
    u32 dir_max = setBuf->dirListMax;
    if (dir_max == 0) dir_max = CELL_SAVEDATA_DIRLIST_MAX;
    CellSaveDataDirList* dirList = (CellSaveDataDirList*)calloc(dir_max, sizeof(CellSaveDataDirList));

    u32 dirCount = enumerate_save_dirs(
        setList->dirNamePrefix ? setList->dirNamePrefix : "",
        dirList, dir_max);

    /* Call list callback */
    CellSaveDataCBResult cbResult;
    memset(&cbResult, 0, sizeof(cbResult));
    cbResult.result = CELL_SAVEDATA_CBRESULT_OK_NEXT;

    CellSaveDataListGet listGet;
    memset(&listGet, 0, sizeof(listGet));
    listGet.dirListNum = dirCount < dir_max ? dirCount : dir_max;
    listGet.dirList = dirList;

    CellSaveDataListSet listSet;
    memset(&listSet, 0, sizeof(listSet));

    funcList(&cbResult, &listGet, &listSet);

    if (cbResult.result < 0) {
        free(dirList);
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }

    /* Determine selected directory name */
    const char* selectedDir = NULL;
    if (listSet.fixedList && listSet.fixedListNum > 0) {
        selectedDir = listSet.fixedList[0].dirName;
    } else if (listSet.focusDirName) {
        selectedDir = listSet.focusDirName;
    } else if (dirCount > 0) {
        selectedDir = dirList[0].dirName;
    }

    if (!selectedDir || selectedDir[0] == '\0') {
        free(dirList);
        return CELL_SAVEDATA_ERROR_NODATA;
    }

    s32 result = savedata_execute(selectedDir, 1, setBuf, funcStat, funcFile, userdata);
    free(dirList);
    return result;
}

s32 cellSaveDataListLoad2(u32 version, CellSaveDataSetList* setList,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataListCallback funcList,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata)
{
    printf("[cellSaveData] ListLoad2(version=%u)\n", version);

    if (!setList || !setBuf || !funcList || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    u32 dir_max = setBuf->dirListMax;
    if (dir_max == 0) dir_max = CELL_SAVEDATA_DIRLIST_MAX;
    CellSaveDataDirList* dirList = (CellSaveDataDirList*)calloc(dir_max, sizeof(CellSaveDataDirList));

    u32 dirCount = enumerate_save_dirs(
        setList->dirNamePrefix ? setList->dirNamePrefix : "",
        dirList, dir_max);

    CellSaveDataCBResult cbResult;
    memset(&cbResult, 0, sizeof(cbResult));
    cbResult.result = CELL_SAVEDATA_CBRESULT_OK_NEXT;

    CellSaveDataListGet listGet;
    memset(&listGet, 0, sizeof(listGet));
    listGet.dirListNum = dirCount < dir_max ? dirCount : dir_max;
    listGet.dirList = dirList;

    CellSaveDataListSet listSet;
    memset(&listSet, 0, sizeof(listSet));

    funcList(&cbResult, &listGet, &listSet);

    if (cbResult.result < 0) {
        free(dirList);
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }

    const char* selectedDir = NULL;
    if (listSet.fixedList && listSet.fixedListNum > 0) {
        selectedDir = listSet.fixedList[0].dirName;
    } else if (listSet.focusDirName) {
        selectedDir = listSet.focusDirName;
    } else if (dirCount > 0) {
        selectedDir = dirList[0].dirName;
    }

    if (!selectedDir || selectedDir[0] == '\0') {
        free(dirList);
        return CELL_SAVEDATA_ERROR_NODATA;
    }

    s32 result = savedata_execute(selectedDir, 0, setBuf, funcStat, funcFile, userdata);
    free(dirList);
    return result;
}

s32 cellSaveDataFixedSave2(u32 version, CellSaveDataSetList* setList,
                            CellSaveDataSetBuf* setBuf,
                            CellSaveDataFixedCallback funcFixed,
                            CellSaveDataStatCallback funcStat,
                            CellSaveDataFileCallback funcFile,
                            u32 container, void* userdata)
{
    printf("[cellSaveData] FixedSave2(version=%u)\n", version);

    if (!setList || !setBuf || !funcFixed || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    u32 dir_max = setBuf->dirListMax;
    if (dir_max == 0) dir_max = CELL_SAVEDATA_DIRLIST_MAX;
    CellSaveDataDirList* dirList = (CellSaveDataDirList*)calloc(dir_max, sizeof(CellSaveDataDirList));

    u32 dirCount = enumerate_save_dirs(
        setList->dirNamePrefix ? setList->dirNamePrefix : "",
        dirList, dir_max);

    CellSaveDataCBResult cbResult;
    memset(&cbResult, 0, sizeof(cbResult));
    cbResult.result = CELL_SAVEDATA_CBRESULT_OK_NEXT;

    CellSaveDataListGet listGet;
    memset(&listGet, 0, sizeof(listGet));
    listGet.dirListNum = dirCount < dir_max ? dirCount : dir_max;
    listGet.dirList = dirList;

    CellSaveDataListSet listSet;
    memset(&listSet, 0, sizeof(listSet));

    funcFixed(&cbResult, &listGet, &listSet);

    if (cbResult.result < 0) {
        free(dirList);
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }

    const char* selectedDir = NULL;
    if (listSet.fixedList && listSet.fixedListNum > 0) {
        selectedDir = listSet.fixedList[0].dirName;
    } else if (listSet.focusDirName) {
        selectedDir = listSet.focusDirName;
    }

    if (!selectedDir || selectedDir[0] == '\0') {
        free(dirList);
        return CELL_SAVEDATA_ERROR_NODATA;
    }

    s32 result = savedata_execute(selectedDir, 1, setBuf, funcStat, funcFile, userdata);
    free(dirList);
    return result;
}

s32 cellSaveDataFixedLoad2(u32 version, CellSaveDataSetList* setList,
                            CellSaveDataSetBuf* setBuf,
                            CellSaveDataFixedCallback funcFixed,
                            CellSaveDataStatCallback funcStat,
                            CellSaveDataFileCallback funcFile,
                            u32 container, void* userdata)
{
    printf("[cellSaveData] FixedLoad2(version=%u)\n", version);

    if (!setList || !setBuf || !funcFixed || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    u32 dir_max = setBuf->dirListMax;
    if (dir_max == 0) dir_max = CELL_SAVEDATA_DIRLIST_MAX;
    CellSaveDataDirList* dirList = (CellSaveDataDirList*)calloc(dir_max, sizeof(CellSaveDataDirList));

    u32 dirCount = enumerate_save_dirs(
        setList->dirNamePrefix ? setList->dirNamePrefix : "",
        dirList, dir_max);

    CellSaveDataCBResult cbResult;
    memset(&cbResult, 0, sizeof(cbResult));
    cbResult.result = CELL_SAVEDATA_CBRESULT_OK_NEXT;

    CellSaveDataListGet listGet;
    memset(&listGet, 0, sizeof(listGet));
    listGet.dirListNum = dirCount < dir_max ? dirCount : dir_max;
    listGet.dirList = dirList;

    CellSaveDataListSet listSet;
    memset(&listSet, 0, sizeof(listSet));

    funcFixed(&cbResult, &listGet, &listSet);

    if (cbResult.result < 0) {
        free(dirList);
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }

    const char* selectedDir = NULL;
    if (listSet.fixedList && listSet.fixedListNum > 0) {
        selectedDir = listSet.fixedList[0].dirName;
    } else if (listSet.focusDirName) {
        selectedDir = listSet.focusDirName;
    }

    if (!selectedDir || selectedDir[0] == '\0') {
        free(dirList);
        return CELL_SAVEDATA_ERROR_NODATA;
    }

    s32 result = savedata_execute(selectedDir, 0, setBuf, funcStat, funcFile, userdata);
    free(dirList);
    return result;
}

s32 cellSaveDataAutoSave2(u32 version, const char* dirName,
                           u32 errDialog,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata)
{
    printf("[cellSaveData] AutoSave2(version=%u, dir='%s')\n",
           version, dirName ? dirName : "<null>");

    if (!dirName || !setBuf || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    return savedata_execute(dirName, 1, setBuf, funcStat, funcFile, userdata);
}

s32 cellSaveDataAutoLoad2(u32 version, const char* dirName,
                           u32 errDialog,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata)
{
    printf("[cellSaveData] AutoLoad2(version=%u, dir='%s')\n",
           version, dirName ? dirName : "<null>");

    if (!dirName || !setBuf || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    return savedata_execute(dirName, 0, setBuf, funcStat, funcFile, userdata);
}

s32 cellSaveDataDelete2(u32 container)
{
    printf("[cellSaveData] Delete2(container=%u)\n", container);

    /* Without a directory name we can't delete anything meaningful.
       This variant typically shows a UI for deletion - just succeed. */
    return CELL_OK;
}
