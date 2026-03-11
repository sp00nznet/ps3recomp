/*
 * ps3recomp - cellOskDialog HLE implementation
 *
 * Provides an on-screen keyboard stub.  When a game requests text input,
 * the prompt is logged and an empty string (or configurable default) is
 * returned immediately.  In a full implementation this would show an
 * actual input dialog.
 */

#include "cellOskDialog.h"
#include <stdio.h>
#include <string.h>

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

void cellOskDialogSetDefaultResponse(const char* text)
{
    if (text) {
        strncpy(s_default_response, text, CELL_OSK_MAX_TEXT_LENGTH);
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
    (void)container; (void)dialogParam;

    if (!s_osk_initialized)
        return CELL_OSK_ERROR_NOT_INITIALIZED;

    if (s_osk_loaded)
        return CELL_OSK_ERROR_DIALOG_ALREADY_LOADED;

    s_osk_loaded = 1;

    /* Log the prompt if available */
    if (inputFieldInfo && inputFieldInfo->message) {
        char prompt[256];
        osk_utf16_to_ascii(inputFieldInfo->message, prompt, sizeof(prompt) - 1);
        printf("[cellOskDialog] LoadAsync - Prompt: \"%s\"\n", prompt);
    } else {
        printf("[cellOskDialog] LoadAsync - (no prompt)\n");
    }

    if (inputFieldInfo && inputFieldInfo->init_text) {
        char init[256];
        osk_utf16_to_ascii(inputFieldInfo->init_text, init, sizeof(init) - 1);
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
    } else if (inputFieldInfo && inputFieldInfo->init_text) {
        /* Copy initial text as the response */
        u32 i;
        for (i = 0; i < CELL_OSK_MAX_TEXT_LENGTH &&
             inputFieldInfo->init_text[i] != 0; i++) {
            s_result_text[i] = inputFieldInfo->init_text[i];
        }
        s_result_text[i] = 0;
        s_result_length = i;
    } else {
        s_result_length = 0;
    }

    return CELL_OK;
}

s32 cellOskDialogUnloadAsync(CellOskDialogCallbackReturnParam* result)
{
    if (!s_osk_initialized)
        return CELL_OSK_ERROR_NOT_INITIALIZED;

    if (!s_osk_loaded)
        return CELL_OSK_ERROR_DIALOG_NOT_LOADED;

    printf("[cellOskDialog] UnloadAsync()\n");

    if (result) {
        result->result = CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK;
        result->numCharsResultString = s_result_length;
        if (result->pResultString && s_result_length > 0) {
            memcpy(result->pResultString, s_result_text,
                   (s_result_length + 1) * sizeof(CellOskDialogChar));
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
    *width  = 640;
    *height = 240;
    return CELL_OK;
}

s32 cellOskDialogAbort(void)
{
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
    if (!s_osk_initialized)
        return CELL_OSK_ERROR_NOT_INITIALIZED;

    if (!result)
        return CELL_OSK_ERROR_INVALID_PARAMETER;

    result->result = (s_result_length > 0)
                     ? CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK
                     : CELL_OSK_DIALOG_INPUT_FIELD_RESULT_NO_INPUT_TEXT;
    result->numCharsResultString = s_result_length;

    if (result->pResultString && s_result_length > 0) {
        memcpy(result->pResultString, s_result_text,
               (s_result_length + 1) * sizeof(CellOskDialogChar));
    }

    return CELL_OK;
}
