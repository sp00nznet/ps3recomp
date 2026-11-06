/* Windows link stubs for test_spu_group_lifecycle.c.
 *
 * The fixture includes lv2_register.c whole and relies on the linker dropping what it does not use (-dead_strip /
 * --gc-sections). The COFF linker resolves every reference first, so on Windows the symbols the rest of the runtime
 * would supply are defined here. None of them is reached by the checks in the fixture: the functions answer 0 and the
 * data is zero. Build it as
 *   clang -std=gnu17 -I include -I runtime -I runtime/syscalls -I runtime/spu -I libs/video
 *     runtime/spu/tests/test_spu_group_lifecycle.c runtime/spu/tests/test_spu_group_lifecycle_win_stubs.c
 *     runtime/spu/spu_lifted_thread.c -o test_spu_group_lifecycle.exe
 * (no declarations are visible here, so the definitions carry no types: x64 does not mangle them.) */
#ifdef _WIN32
/* data */
char function_table[64];
char function_table_count[64];
char g_ps3_sdk_version[64];
char g_spu_interp_last_pc[64];
char g_spu_interp_steps[64];
char g_spu_out_mbox_hook[64];
char g_spu_user_event_hook[64];
char g_spurs_kernel_hook[64];
char g_ydkj_spurs_ctx_ea[64];
char ppu_guard_page[64];

/* functions */
long long np_psnr_identity() { return 0; }
long long ppu_dump_guest_stack() { return 0; }
long long ppu_guest_caller() { return 0; }
long long ppu_guest_callstack() { return 0; }
long long ppu_log_host_chain() { return 0; }
long long ps3_hle_register_ctx() { return 0; }
long long ps3_spu_image_source_ea() { return 0; }
long long spu_ch_wake() { return 0; }
long long spu_coh_unregister() { return 0; }
long long spu_have_function() { return 0; }
long long spu_image_of_function() { return 0; }
long long spu_indirect_branch() { return 0; }
long long spu_interp_run() { return 0; }
long long spu_lookup_ppu_fallback() { return 0; }
long long spu_mfc_release() { return 0; }
long long spu_raw_note_image() { return 0; }
long long spu_register_ppu_fallback() { return 0; }
long long spu_registry_fallback() { return 0; }
long long spu_run_with_halt() { return 0; }
long long spurs_job_ls_for_handle() { return 0; }
long long sys_cond_init() { return 0; }
long long sys_event_init() { return 0; }
long long sys_event_queue_push_by_id() { return 0; }
long long sys_fs_init() { return 0; }
long long sys_fs_translate_path() { return 0; }
long long sys_memory_init() { return 0; }
long long sys_mutex_init() { return 0; }
long long sys_ppu_thread_init() { return 0; }
long long sys_process_exit() { return 0; }
long long sys_process_getpid() { return 0; }
long long sys_raw_spu_init() { return 0; }
long long sys_rsx_init() { return 0; }
long long sys_rwlock_init() { return 0; }
long long sys_semaphore_init() { return 0; }
long long sys_timer_init() { return 0; }
long long sys_vm_init() { return 0; }
#endif
