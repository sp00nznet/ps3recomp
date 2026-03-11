/*
 * ps3recomp - cellSaveData HLE
 *
 * Game save data management: list/load/save/delete with callback-driven flow.
 */

#ifndef PS3RECOMP_CELL_SAVEDATA_H
#define PS3RECOMP_CELL_SAVEDATA_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define CELL_SAVEDATA_VERSION_CURRENT    0

#define CELL_SAVEDATA_DIRNAME_SIZE       32
#define CELL_SAVEDATA_FILENAME_SIZE      13
#define CELL_SAVEDATA_TITLE_SIZE         128
#define CELL_SAVEDATA_SUBTITLE_SIZE      128
#define CELL_SAVEDATA_DETAIL_SIZE        1024
#define CELL_SAVEDATA_PREFIX_SIZE        256
#define CELL_SAVEDATA_LISTITEM_MAX       2048
#define CELL_SAVEDATA_SECUREFILE_MAX     113
#define CELL_SAVEDATA_DIRLIST_MAX        2048
#define CELL_SAVEDATA_INVALIDMSG_MAX     256
#define CELL_SAVEDATA_INDICATORMSG_MAX   64
#define CELL_SAVEDATA_SYSP_TITLE_SIZE    128
#define CELL_SAVEDATA_SYSP_SUBTITLE_SIZE 128
#define CELL_SAVEDATA_SYSP_DETAIL_SIZE   1024
#define CELL_SAVEDATA_SYSP_LPARAM_SIZE   8

/* PARAM.SFO parameter IDs */
#define CELL_SAVEDATA_ATTR_NORMAL       0
#define CELL_SAVEDATA_ATTR_NODUPLICATE  1

/* File operation types */
#define CELL_SAVEDATA_FILEOP_READ       0
#define CELL_SAVEDATA_FILEOP_WRITE      1
#define CELL_SAVEDATA_FILEOP_DELETE     2
#define CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC  3

/* File type */
#define CELL_SAVEDATA_FILETYPE_SECUREFILE   0
#define CELL_SAVEDATA_FILETYPE_NORMALFILE   1
#define CELL_SAVEDATA_FILETYPE_CONTENT_ICON0  2
#define CELL_SAVEDATA_FILETYPE_CONTENT_ICON1  3
#define CELL_SAVEDATA_FILETYPE_CONTENT_PIC1   4
#define CELL_SAVEDATA_FILETYPE_CONTENT_SND0   5

/* Callback result values */
#define CELL_SAVEDATA_CBRESULT_OK_LAST      0
#define CELL_SAVEDATA_CBRESULT_OK_NEXT      1
#define CELL_SAVEDATA_CBRESULT_ERR_NOSPACE  (-1)
#define CELL_SAVEDATA_CBRESULT_ERR_FAILURE  (-2)
#define CELL_SAVEDATA_CBRESULT_ERR_BROKEN   (-3)
#define CELL_SAVEDATA_CBRESULT_ERR_NODATA   (-4)
#define CELL_SAVEDATA_CBRESULT_ERR_INVALID  (-5)

/* Sort types */
#define CELL_SAVEDATA_SORTTYPE_MODIFIEDTIME   0
#define CELL_SAVEDATA_SORTTYPE_SUBTITLE       1

/* Sort order */
#define CELL_SAVEDATA_SORTORDER_DESCENT   0
#define CELL_SAVEDATA_SORTORDER_ASCENT    1

/* Focus position */
#define CELL_SAVEDATA_FOCUSPOS_DIRNAME     0
#define CELL_SAVEDATA_FOCUSPOS_LISTHEAD    1
#define CELL_SAVEDATA_FOCUSPOS_LISTTAIL    2
#define CELL_SAVEDATA_FOCUSPOS_LATEST      3
#define CELL_SAVEDATA_FOCUSPOS_OLDEST      4
#define CELL_SAVEDATA_FOCUSPOS_NEWDATA     5

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_SAVEDATA_ERROR_CBRESULT       (s32)(CELL_ERROR_BASE_SYSUTIL_SAVE | 0x01)
#define CELL_SAVEDATA_ERROR_ACCESS_ERROR   (s32)(CELL_ERROR_BASE_SYSUTIL_SAVE | 0x02)
#define CELL_SAVEDATA_ERROR_INTERNAL       (s32)(CELL_ERROR_BASE_SYSUTIL_SAVE | 0x03)
#define CELL_SAVEDATA_ERROR_PARAM          (s32)(CELL_ERROR_BASE_SYSUTIL_SAVE | 0x04)
#define CELL_SAVEDATA_ERROR_NOSPACE        (s32)(CELL_ERROR_BASE_SYSUTIL_SAVE | 0x05)
#define CELL_SAVEDATA_ERROR_BROKEN         (s32)(CELL_ERROR_BASE_SYSUTIL_SAVE | 0x06)
#define CELL_SAVEDATA_ERROR_FAILURE        (s32)(CELL_ERROR_BASE_SYSUTIL_SAVE | 0x07)
#define CELL_SAVEDATA_ERROR_BUSY           (s32)(CELL_ERROR_BASE_SYSUTIL_SAVE | 0x08)
#define CELL_SAVEDATA_ERROR_NOUSER         (s32)(CELL_ERROR_BASE_SYSUTIL_SAVE | 0x09)
#define CELL_SAVEDATA_ERROR_SIZEOVER       (s32)(CELL_ERROR_BASE_SYSUTIL_SAVE | 0x0A)
#define CELL_SAVEDATA_ERROR_NODATA         (s32)(CELL_ERROR_BASE_SYSUTIL_SAVE | 0x0B)
#define CELL_SAVEDATA_ERROR_NOTSUPPORTED   (s32)(CELL_ERROR_BASE_SYSUTIL_SAVE | 0x0C)

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

typedef struct CellSaveDataCBResult {
    s32  result;
    u32  progressBarInc;
    s32  errNeedSizeKB;
    char invalidMsg[CELL_SAVEDATA_INVALIDMSG_MAX];
} CellSaveDataCBResult;

typedef struct CellSaveDataDirList {
    char dirName[CELL_SAVEDATA_DIRNAME_SIZE];
    char listParam[CELL_SAVEDATA_SYSP_LPARAM_SIZE];
    char reserved[8];
} CellSaveDataDirList;

typedef struct CellSaveDataListGet {
    u32  dirListNum;
    CellSaveDataDirList* dirList;
} CellSaveDataListGet;

typedef struct CellSaveDataListSet {
    u32  focusPosition;
    char* focusDirName;
    u32  fixedListNum;
    CellSaveDataDirList* fixedList;
    char reserved[12];
} CellSaveDataListSet;

typedef struct CellSaveDataNewDataIcon {
    char* title;
    u32   iconBufSize;
    void* iconBuf;
} CellSaveDataNewDataIcon;

typedef struct CellSaveDataListNewData {
    u32  iconPosition;
    char* dirName;
    CellSaveDataNewDataIcon* icon;
    char reserved[8];
} CellSaveDataListNewData;

typedef struct CellSaveDataSystemFileParam {
    char title[CELL_SAVEDATA_SYSP_TITLE_SIZE];
    char subTitle[CELL_SAVEDATA_SYSP_SUBTITLE_SIZE];
    char detail[CELL_SAVEDATA_SYSP_DETAIL_SIZE];
    u32  attribute;
    char reserved2[4];
    char listParam[CELL_SAVEDATA_SYSP_LPARAM_SIZE];
    char reserved[256];
} CellSaveDataSystemFileParam;

typedef struct CellSaveDataDirStat {
    s64  st_atime;
    s64  st_mtime;
    s64  st_ctime;
    char dirName[CELL_SAVEDATA_DIRNAME_SIZE];
} CellSaveDataDirStat;

typedef struct CellSaveDataFileStat {
    u32  fileType;
    char reserved1[4];
    u64  st_size;
    s64  st_atime;
    s64  st_mtime;
    s64  st_ctime;
    char fileName[CELL_SAVEDATA_FILENAME_SIZE + 3]; /* padded */
    char reserved2[3];
} CellSaveDataFileStat;

typedef struct CellSaveDataStatGet {
    s32  hddFreeSizeKB;
    u32  isNewData;
    CellSaveDataDirStat dir;
    CellSaveDataSystemFileParam getParam;
    u32  bind;
    s32  sizeKB;
    s32  sysSizeKB;
    u32  fileNum;
    u32  fileListNum;
    CellSaveDataFileStat* fileList;
} CellSaveDataStatGet;

typedef struct CellSaveDataStatSet {
    CellSaveDataSystemFileParam* setParam;
    u32  reCreateMode;
    CellSaveDataNewDataIcon* indicator;
} CellSaveDataStatSet;

typedef struct CellSaveDataFileGet {
    u32  excSize;
    char reserved[64];
} CellSaveDataFileGet;

typedef struct CellSaveDataFileSet {
    u32  fileOperation;
    void* reserved;
    u32  fileType;
    u8   secureFileId[16];
    char* fileName;
    u32  fileOffset;
    u32  fileSize;
    u32  fileBufSize;
    void* fileBuf;
} CellSaveDataFileSet;

typedef struct CellSaveDataDoneGet {
    s32  excResult;
    char dirName[CELL_SAVEDATA_DIRNAME_SIZE];
    s32  hddFreeSizeKB;
    char reserved[64];
} CellSaveDataDoneGet;

/* ---------------------------------------------------------------------------
 * Callback function types
 * -----------------------------------------------------------------------*/
typedef void (*CellSaveDataListCallback)(CellSaveDataCBResult* cbResult,
                                          CellSaveDataListGet* get,
                                          CellSaveDataListSet* set);

typedef void (*CellSaveDataFixedCallback)(CellSaveDataCBResult* cbResult,
                                           CellSaveDataListGet* get,
                                           CellSaveDataListSet* set);

typedef void (*CellSaveDataStatCallback)(CellSaveDataCBResult* cbResult,
                                          CellSaveDataStatGet* get,
                                          CellSaveDataStatSet* set);

typedef void (*CellSaveDataFileCallback)(CellSaveDataCBResult* cbResult,
                                          CellSaveDataFileGet* get,
                                          CellSaveDataFileSet* set);

typedef void (*CellSaveDataDoneCallback)(CellSaveDataCBResult* cbResult,
                                          CellSaveDataDoneGet* get);

/* ---------------------------------------------------------------------------
 * Set parameters
 * -----------------------------------------------------------------------*/

typedef struct CellSaveDataSetList {
    u32  sortType;
    u32  sortOrder;
    char* dirNamePrefix;
} CellSaveDataSetList;

typedef struct CellSaveDataSetBuf {
    u32  dirListMax;
    u32  fileListMax;
    u32  reserved[6];
    u32  bufSize;
    void* buf;
} CellSaveDataSetBuf;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellSaveDataListSave2(u32 version, CellSaveDataSetList* setList,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataListCallback funcList,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata);

s32 cellSaveDataListLoad2(u32 version, CellSaveDataSetList* setList,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataListCallback funcList,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata);

s32 cellSaveDataFixedSave2(u32 version, CellSaveDataSetList* setList,
                            CellSaveDataSetBuf* setBuf,
                            CellSaveDataFixedCallback funcFixed,
                            CellSaveDataStatCallback funcStat,
                            CellSaveDataFileCallback funcFile,
                            u32 container, void* userdata);

s32 cellSaveDataFixedLoad2(u32 version, CellSaveDataSetList* setList,
                            CellSaveDataSetBuf* setBuf,
                            CellSaveDataFixedCallback funcFixed,
                            CellSaveDataStatCallback funcStat,
                            CellSaveDataFileCallback funcFile,
                            u32 container, void* userdata);

s32 cellSaveDataAutoSave2(u32 version, const char* dirName,
                           u32 errDialog,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata);

s32 cellSaveDataAutoLoad2(u32 version, const char* dirName,
                           u32 errDialog,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata);

s32 cellSaveDataDelete2(u32 container);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SAVEDATA_H */
