/*
 * ps3recomp - Module registration framework
 *
 * Each PS3 PRX module is represented by a ps3_module struct that holds its
 * name, NID function/variable tables, and lifecycle hooks.  The DECLARE /
 * REGISTER macros give a concise way to populate these tables.
 */

#ifndef PS3EMU_MODULE_H
#define PS3EMU_MODULE_H

#include "nid.h"
#include "error_codes.h"

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Maximum registrations per module (static allocation).  Override at build
 * time with -DPS3_MODULE_MAX_FUNCS=N if you need more.
 * -----------------------------------------------------------------------*/
#ifndef PS3_MODULE_MAX_FUNCS
#define PS3_MODULE_MAX_FUNCS  512
#endif

#ifndef PS3_MODULE_MAX_VARS
#define PS3_MODULE_MAX_VARS   64
#endif

#define PS3_MAX_MODULES       128

/* ---------------------------------------------------------------------------
 * Module lifecycle callbacks
 * -----------------------------------------------------------------------*/
typedef int32_t (*ps3_module_init_fn)(void);
typedef void    (*ps3_module_shutdown_fn)(void);

/* ---------------------------------------------------------------------------
 * ps3_module -- describes one PRX module.
 * -----------------------------------------------------------------------*/
typedef struct ps3_module {
    const char*           name;
    bool                  loaded;

    /* NID tables (functions and variables) */
    ps3_nid_table         func_table;
    ps3_nid_entry         func_storage[PS3_MODULE_MAX_FUNCS];

    ps3_nid_table         var_table;
    ps3_nid_entry         var_storage[PS3_MODULE_MAX_VARS];

    /* Lifecycle hooks (may be NULL) */
    ps3_module_init_fn    on_load;
    ps3_module_shutdown_fn on_unload;
} ps3_module;

/* ---------------------------------------------------------------------------
 * Module registry -- global array of all declared modules.
 * -----------------------------------------------------------------------*/
typedef struct ps3_module_registry {
    ps3_module* modules[PS3_MAX_MODULES];
    uint32_t    count;
} ps3_module_registry;

/* The single global registry instance (defined in exactly one TU). */
extern ps3_module_registry g_ps3_module_registry;

static inline void ps3_module_init(ps3_module* m, const char* name)
{
    m->name   = name;
    m->loaded = false;
    m->on_load   = NULL;
    m->on_unload = NULL;
    ps3_nid_table_init(&m->func_table, m->func_storage, PS3_MODULE_MAX_FUNCS);
    ps3_nid_table_init(&m->var_table,  m->var_storage,  PS3_MODULE_MAX_VARS);
}

static inline int32_t ps3_module_load(ps3_module* m)
{
    if (m->loaded) return CELL_OK;
    if (m->on_load) {
        int32_t rc = m->on_load();
        if (CELL_IS_ERROR(rc)) return rc;
    }
    m->loaded = true;
    return CELL_OK;
}

static inline void ps3_module_unload(ps3_module* m)
{
    if (!m->loaded) return;
    if (m->on_unload) m->on_unload();
    m->loaded = false;
}

/* Register a module in the global registry. */
static inline int ps3_register_module(ps3_module* m)
{
    if (g_ps3_module_registry.count >= PS3_MAX_MODULES) return -1;
    g_ps3_module_registry.modules[g_ps3_module_registry.count++] = m;
    return 0;
}

/* Lookup a module by name. */
static inline ps3_module* ps3_find_module(const char* name)
{
    for (uint32_t i = 0; i < g_ps3_module_registry.count; i++) {
        if (strcmp(g_ps3_module_registry.modules[i]->name, name) == 0)
            return g_ps3_module_registry.modules[i];
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Import / export resolution
 *
 * Walk all loaded modules looking for a handler that matches the given NID.
 * Returns the handler pointer or NULL.
 * -----------------------------------------------------------------------*/
static inline void* ps3_resolve_func_nid(uint32_t nid)
{
    for (uint32_t i = 0; i < g_ps3_module_registry.count; i++) {
        ps3_module* m = g_ps3_module_registry.modules[i];
        if (!m->loaded) continue;
        ps3_nid_entry* e = ps3_nid_table_find(&m->func_table, nid);
        if (e) return e->handler;
    }
    return NULL;
}

static inline void* ps3_resolve_var_nid(uint32_t nid)
{
    for (uint32_t i = 0; i < g_ps3_module_registry.count; i++) {
        ps3_module* m = g_ps3_module_registry.modules[i];
        if (!m->loaded) continue;
        ps3_nid_entry* e = ps3_nid_table_find(&m->var_table, nid);
        if (e) return e->handler;
    }
    return NULL;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ---------------------------------------------------------------------------
 * C++ registration macros
 * -----------------------------------------------------------------------*/
#ifdef __cplusplus

/*
 * DECLARE_PS3_MODULE(varname, module_name)
 *   Declares a static ps3_module and a constructor that registers it.
 *
 * REGISTER_FUNC(func)
 *   Registers `func` by computing its NID from the symbol name.
 *
 * REGISTER_FNID(nid, func)
 *   Registers `func` under an explicit NID.
 *
 * Usage:
 *
 *   DECLARE_PS3_MODULE(cellSysutil, "cellSysutil")
 *   {
 *       REGISTER_FUNC(cellSysutilGetSystemParamInt);
 *       REGISTER_FNID(0x189A74DA, cellSysutilCheckCallback);
 *   }
 */

#define DECLARE_PS3_MODULE(varname, module_name)                            \
    static ps3_module varname;                                              \
    static void _ps3_register_##varname();                                  \
    namespace {                                                             \
        struct _ps3_auto_##varname {                                        \
            _ps3_auto_##varname() {                                         \
                ps3_module_init(&varname, module_name);                     \
                _ps3_register_##varname();                                  \
                ps3_register_module(&varname);                              \
            }                                                               \
        };                                                                  \
        static _ps3_auto_##varname _ps3_inst_##varname;                     \
    }                                                                       \
    static void _ps3_register_##varname()

/* Inside a DECLARE_PS3_MODULE body: */
#define REGISTER_FUNC(func)         REG_FUNC(varname, func)
#define REGISTER_FNID(nid, func)    REG_FNID(varname, nid, func)
#define REGISTER_VAR(var)           REG_VAR(varname, var)
#define REGISTER_VAR_FNID(nid, var) REG_VAR_FNID(varname, nid, var)

#define SET_MODULE_INIT(fn)     varname.on_load   = (fn)
#define SET_MODULE_SHUTDOWN(fn) varname.on_unload  = (fn)

#endif /* __cplusplus */

#endif /* PS3EMU_MODULE_H */
