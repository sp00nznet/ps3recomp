/*
 * ps3recomp - cellOskDialog HLE implementation
 *
 * Provides an on-screen keyboard stub.  When a game requests text input,
 * the prompt is logged and an empty string (or configurable default) is
 * returned immediately.  In a full implementation this would show an
 * actual input dialog.
 *
 * A build with a native host (osk_native.h, opt-in) routes LoadAsync,
 * UnloadAsync, GetInputText and Abort to it instead: the user answers a real
 * dialog and the title sees the normal LOADED/FINISHED/UNLOADED lifecycle.
 */

#include "cellOskDialog.h"
#include "osk_native.h"
#include <stdio.h>
#include <string.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../guest_struct.h"   /* GUEST_EA, vm_read/vm_write: guest EA -> host */

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_osk_initialized = 0;
static int  s_osk_loaded = 0;
static char s_default_response[CELL_OSK_MAX_TEXT_LENGTH + 1] = "";

/* Stored input result (UTF-16) */
static CellOskDialogChar s_result_text[CELL_OSK_MAX_TEXT_LENGTH + 1];
static u32               s_result_length = 0;

/* Convert narrow string to UTF-16LE (simple ASCII subset) */
static void osk_ascii_to_utf16(const char* src, CellOskDialogChar* dst,
                                u32 maxChars)
{
    u32 i;
    for (i = 0; i < maxChars && src[i] != '\0'; i++)
        dst[i] = (CellOskDialogChar)src[i];
    dst[i] = 0;
}

/* Convert UTF-16LE to narrow string for logging (ASCII subset) */
static void osk_utf16_to_ascii(const CellOskDialogChar* src, char* dst,
                                u32 maxChars)
{
    u32 i;
    for (i = 0; i < maxChars && src[i] != 0; i++)
        dst[i] = (src[i] < 128) ? (char)src[i] : '?';
    dst[i] = '\0';
}

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * Guest struct access
 *
 * CellOskDialogInputFieldInfo is { CellOskDialogChar* message;
 * CellOskDialogChar* init_text; u32 limit_length; } and
 * CellOskDialogCallbackReturnParam is { s32 result; u32 numCharsResultString;
 * CellOskDialogChar* pResultString; }. Both lead with (or contain) pointers,
 * which the guest stores in four bytes and we in eight, so a cast misplaces
 * every field after the first. The strings are UTF-16 and big-endian, so the
 * code units come across one at a time too.
 * -----------------------------------------------------------------------*/
enum {
    OSKIF_O_MESSAGE = 0, OSKIF_O_INIT_TEXT = 4, OSKIF_O_LIMIT_LENGTH = 8,
    OSKRET_O_RESULT = 0, OSKRET_O_NUMCHARS = 4, OSKRET_O_PRESULTSTRING = 8
};

/* Copy a NUL-terminated big-endian UTF-16 string out of guest memory. */
static u32 osk_guest_utf16(u32 ea, CellOskDialogChar* dst, u32 max)
{
    u32 n = 0;
    if (!ea)
        return 0;
    while (n < max) {
        u16 c = vm_read16(ea + n * 2);
        if (!c)
            break;
        dst[n++] = c;
    }
    dst[n] = 0;
    return n;
}

void cellOskDialogSetDefaultResponse(const char* text)
{
    if (text) {
        strncpy(s_default_response, GUEST_PTR(text, const char*),
                CELL_OSK_MAX_TEXT_LENGTH);
        s_default_response[CELL_OSK_MAX_TEXT_LENGTH] = '\0';
    }
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellOskDialogInit(u32 container)
{
    (void)container;
    printf("[cellOskDialog] Init()\n");

    if (s_osk_initialized)
        return CELL_OSK_ERROR_ALREADY_INITIALIZED;

    s_osk_initialized = 1;
    s_osk_loaded = 0;
    return CELL_OK;
}

s32 cellOskDialogEnd(void)
{
    printf("[cellOskDialog] End()\n");

    if (!s_osk_initialized)
        return CELL_OSK_ERROR_NOT_INITIALIZED;

    s_osk_initialized = 0;
    s_osk_loaded = 0;
    return CELL_OK;
}

s32 cellOskDialogLoadAsync(u32 container,
                           const CellOskDialogParam* dialogParam,
                           const CellOskDialogInputFieldInfo* inputFieldInfo)
{
#ifdef PS3RECOMP_NATIVE_OSK
    return osk_native_load(GUEST_EA(dialogParam), GUEST_EA(inputFieldInfo));
#endif
    (void)container; (void)dialogParam;

    if (!s_osk_initialized)
        return CELL_OSK_ERROR_NOT_INITIALIZED;

    if (s_osk_loaded)
        return CELL_OSK_ERROR_DIALOG_ALREADY_LOADED;

    s_osk_loaded = 1;

    u32 info_ea    = GUEST_EA(inputFieldInfo);
    u32 message_ea = info_ea ? vm_read32(info_ea + OSKIF_O_MESSAGE)   : 0;
    u32 init_ea    = info_ea ? vm_read32(info_ea + OSKIF_O_INIT_TEXT) : 0;

    static CellOskDialogChar host_init[CELL_OSK_MAX_TEXT_LENGTH + 1];
    u32 init_len = osk_guest_utf16(init_ea, host_init, CELL_OSK_MAX_TEXT_LENGTH);

    /* Log the prompt if available */
    if (message_ea) {
        static CellOskDialogChar host_msg[CELL_OSK_MAX_TEXT_LENGTH + 1];
        osk_guest_utf16(message_ea, host_msg, CELL_OSK_MAX_TEXT_LENGTH);
        char prompt[256];
        osk_utf16_to_ascii(host_msg, prompt, sizeof(prompt) - 1);
        printf("[cellOskDialog] LoadAsync - Prompt: \"%s\"\n", prompt);
    } else {
        printf("[cellOskDialog] LoadAsync - (no prompt)\n");
    }

    if (init_ea) {
        char init[256];
        osk_utf16_to_ascii(host_init, init, sizeof(init) - 1);
        printf("[cellOskDialog] Initial text: \"%s\"\n", init);
    }

    /* Prepare the result: use default response or empty string */
    memset(s_result_text, 0, sizeof(s_result_text));
    if (s_default_response[0] != '\0') {
        osk_ascii_to_utf16(s_default_response, s_result_text,
                           CELL_OSK_MAX_TEXT_LENGTH);
        s_result_length = (u32)strlen(s_default_response);
        printf("[cellOskDialog] Returning default response: \"%s\"\n",
               s_default_response);
    } else if (init_ea) {
        /* Copy initial text as the response */
        memcpy(s_result_text, host_init,
               (init_len + 1) * sizeof(CellOskDialogChar));
        s_result_length = init_len;
    } else {
        s_result_length = 0;
    }

    return CELL_OK;
}

s32 cellOskDialogUnloadAsync(CellOskDialogCallbackReturnParam* result)
{
#ifdef PS3RECOMP_NATIVE_OSK
    return osk_native_unload(GUEST_EA(result));
#endif
    if (!s_osk_initialized)
        return CELL_OSK_ERROR_NOT_INITIALIZED;

    if (!s_osk_loaded)
        return CELL_OSK_ERROR_DIALOG_NOT_LOADED;

    printf("[cellOskDialog] UnloadAsync()\n");

    if (result) {
        u32 res_ea = GUEST_EA(result);
        vm_write32(res_ea + OSKRET_O_RESULT,
                   CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK);
        vm_write32(res_ea + OSKRET_O_NUMCHARS, s_result_length);
        u32 str_ea = vm_read32(res_ea + OSKRET_O_PRESULTSTRING);
        if (str_ea && s_result_length > 0) {
            /* UTF-16, big-endian in guest memory: one unit at a time. */
            for (u32 i = 0; i <= s_result_length; i++)
                vm_write16(str_ea + i * 2, s_result_text[i]);
        }
    }

    s_osk_loaded = 0;
    return CELL_OK;
}

s32 cellOskDialogGetSize(u32* width, u32* height)
{
    if (!width || !height)
        return CELL_OSK_ERROR_INVALID_PARAMETER;

    /* Standard PS3 OSK dimensions */
    vm_write32((u32)(uintptr_t)width, (u32)640);
    vm_write32((u32)(uintptr_t)height, (u32)240);
    return CELL_OK;
}

s32 cellOskDialogAbort(void)
{
#ifdef PS3RECOMP_NATIVE_OSK
    return osk_native_abort();
#endif
    printf("[cellOskDialog] Abort()\n");

    if (!s_osk_loaded)
        return CELL_OSK_ERROR_DIALOG_NOT_LOADED;

    s_osk_loaded = 0;
    return CELL_OK;
}

s32 cellOskDialogSetInitialInputDevice(u32 device)
{
    (void)device;
    return CELL_OK;
}

s32 cellOskDialogSetInitialKeyLayout(u32 layout)
{
    (void)layout;
    printf("[cellOskDialog] SetInitialKeyLayout(%u)\n", layout);
    return CELL_OK;
}

s32 cellOskDialogSetLayoutMode(u32 mode)
{
    (void)mode;
    printf("[cellOskDialog] SetLayoutMode(%u)\n", mode);
    return CELL_OK;
}

s32 cellOskDialogSetSeparateWindowOption(u32 option)
{
    (void)option;
    return CELL_OK;
}

s32 cellOskDialogGetInputText(CellOskDialogCallbackReturnParam* result)
{
#ifdef PS3RECOMP_NATIVE_OSK
    return osk_native_get_text(GUEST_EA(result));
#endif
    if (!s_osk_initialized)
        return CELL_OSK_ERROR_NOT_INITIALIZED;

    if (!result)
        return CELL_OSK_ERROR_INVALID_PARAMETER;

    u32 res_ea = GUEST_EA(result);
    vm_write32(res_ea + OSKRET_O_RESULT, (s_result_length > 0)
               ? CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK
               : CELL_OSK_DIALOG_INPUT_FIELD_RESULT_NO_INPUT_TEXT);
    vm_write32(res_ea + OSKRET_O_NUMCHARS, s_result_length);

    u32 str_ea = vm_read32(res_ea + OSKRET_O_PRESULTSTRING);
    if (str_ea && s_result_length > 0) {
        /* UTF-16, big-endian in guest memory: one unit at a time. */
        for (u32 i = 0; i <= s_result_length; i++)
            vm_write16(str_ea + i * 2, s_result_text[i]);
    }

    return CELL_OK;
}
