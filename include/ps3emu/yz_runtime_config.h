#ifndef PS3EMU_YZ_RUNTIME_CONFIG_H
#define PS3EMU_YZ_RUNTIME_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Process configuration used by the Yakuza runner's normal execution path.
 *
 * The object is constructed before main(), so each behavior/repair option is
 * read from the environment exactly once.  Hot paths only read this immutable
 * snapshot.  Observation-only controls deliberately do not live here: clean
 * performance lanes compile their tracing out.
 */
typedef struct yz_runtime_config {
    int movie_hle;
    int auto_start;
    int auto_new_game;
    int auto_new_game_circle;

    int skip_voice;
    int a010_render_enable;
    int a010_model_retry;
    int a010_palette_repair;
    int a010_camera_handoff;
    int a010_stage_classify_repair;
    int a010_force_stage_meshes;
    int a010_force_stage_main;
    int a010_stage_group_wait;
    int a010_start_reference;
    int a010_reference_874;

    int xf_ieee;
    int no_taskreload;
    int ctxshadow;
    int no_ctxshadow;
    int no_ctxsave;
    int no_resvsw;
    int resume_hold_ms;
    int no_ctxrestore;
    int ch_nonblock;
    int ch_strict;
    int no_throw_retry;
    int nolaunch;
    int no_mgmt;
    int fixexit;
    int starttask_hook;
    int job_wildcard_ok;
    int kern_wildcard_ok;
    int sentinel_guard;
    int no_imgstack;
    int no_modreload;
    int no_launch_unwind;
    int noresume;
    int coret_gen;
    int coret_legacy;
    int pollforce;
    int nohelpret;
    int no_recwait;
    int no_dmaguard;
    int nullpage_fail;
    int no_llfast;
    int no_spubackoff;
    int no_lrwake;
    int frc;
    int frc3;
    int force_wid0;
    int force_wid1;
    int clearrun;
    int clearrun3;
    int tagread_repair;
    int spu_lockstep;
    unsigned long long lockstep_quantum;
} yz_runtime_config;

extern const yz_runtime_config g_yz_runtime_config;

/* Emits one canonical value list and its stable FNV-1a fingerprint. */
void yz_runtime_config_print_fingerprint(void);

#ifdef __cplusplus
}
#endif

#endif
