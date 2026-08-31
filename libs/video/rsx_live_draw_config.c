/*
 * Neutral definitions for the state the live draw engine reads from its host
 * title.
 *
 * rsx_live_draw.c comes from caner's (canersaka) Yakuza: Dead Souls port, where
 * these are owned by the Yakuza runner: a config snapshot built from the
 * environment before main(), plus a few flags its A010 scene-debugging work
 * toggles at run time. The engine only ever reads them, and every path they
 * gate is Yakuza-specific, so a title that does not have them wants them all
 * off rather than absent.
 *
 * Defining them here keeps the engine buildable for any title without editing
 * it. A port that genuinely wants these behaviours should define them itself
 * and drop this file from its build rather than assign to them, since the
 * config object is const by design (read once, never mutated on a hot path).
 */

#include "ps3emu/yz_runtime_config.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* All-zero: every optional behaviour and repair disabled. */
const yz_runtime_config g_yz_runtime_config;

#ifdef _WIN32
volatile LONG               g_yz_a010_reference_camera_active = 0;
volatile LONG               g_yz_a010_root_active             = 0;
volatile LONG               g_yz_movement_proof_phase         = 0;
volatile unsigned long long g_yz_auto_start_tick              = 0;
#endif
