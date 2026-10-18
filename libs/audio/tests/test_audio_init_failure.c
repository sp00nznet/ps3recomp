/* Compile with PS3RECOMP_AUDIO_USE_SDL2; nonexistent driver makes this a
 * CPU-only failure-path test. It never opens an audio endpoint. */
#define SDL_MAIN_HANDLED
#include "../cellAudio.c"
#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
int sys_event_queue_push_by_id(uint32_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e)
{ (void)a;(void)b;(void)c;(void)d;(void)e;return 0; }
uint32_t sys_event_find_queue_by_key(uint64_t key) { (void)key; return 0; }
uint32_t sys_event_queue_create_direct(uint64_t key,int32_t size) { (void)key;(void)size;return 0; }
#ifdef _WIN32
static DWORD WINAPI held_thread(void* gate) { WaitForSingleObject((HANDLE)gate,INFINITE); return 0; }
#endif
int main(void)
{
    /* Direct mixer test uses host-owned synthetic BE PCM and no device.
     * Protects against a timing repair masking or altering actual samples. */
    mutex_init(&s_audio_mutex);
    AudioPortSlot* port=&s_ports[0];
    port->in_use=port->running=1;
    port->param.nChannel=8;port->param.nBlock=8;port->param.level=1.0f;
    port->buffer=calloc(8*256*8,sizeof(float));assert(port->buffer);
    for(unsigned frame=0;frame<256;frame++) {
        float left=0.125f,right=-0.25f;
        uint32_t l,r;memcpy(&l,&left,4);memcpy(&r,&right,4);
        l=__builtin_bswap32(l);r=__builtin_bswap32(r);
        memcpy(port->buffer+frame*8,&l,4);memcpy(port->buffer+frame*8+1,&r,4);
    }
    audio_mix_one_block();
    for(unsigned frame=0;frame<256;frame++) {
        assert(s_mix_buffer[frame*2]==0.125f && s_mix_buffer[frame*2+1]==-0.25f);
        for(unsigned ch=0;ch<8;ch++)assert(port->buffer[frame*8+ch]==0.0f);
    }
    assert(port->read_index==1);
    audio_mix_one_block();
    for(unsigned i=0;i<512;i++)assert(s_mix_buffer[i]==0.0f);
    free(port->buffer);memset(s_ports,0,sizeof(s_ports));mutex_destroy(&s_audio_mutex);
    assert(SDL_setenv("SDL_AUDIODRIVER","folklore-test-nonexistent-driver",1)==0);
    for(unsigned attempt=0;attempt<2;attempt++) {
        assert(cellAudioInit()==(s32)CELL_AUDIO_ERROR_AUDIOSYSTEM);
        assert(!s_audio_initialized && !s_mix_thread_running && !s_sdl_audio_dev);
        assert(cellAudioQuit()==(s32)CELL_AUDIO_ERROR_NOT_INIT);
    }
#ifdef _WIN32
    /* A delayed worker must retain its handle/backend/mutex on quit timeout;
     * a later retry may clean up after actual exit. No forced termination. */
    HANDLE gate=CreateEventW(NULL,TRUE,FALSE,NULL);assert(gate);
    mutex_init(&s_audio_mutex);s_audio_initialized=1;s_mix_thread_running=1;
    s_mix_thread=CreateThread(NULL,0,held_thread,gate,0,NULL);assert(s_mix_thread);
    assert(cellAudioQuit()==(s32)CELL_AUDIO_ERROR_AUDIOSYSTEM);
    assert(s_audio_initialized && s_mix_thread);
    mutex_lock(&s_audio_mutex);mutex_unlock(&s_audio_mutex);
    SetEvent(gate);
    assert(cellAudioQuit()==CELL_OK);
    assert(!s_audio_initialized && !s_mix_thread);
    CloseHandle(gate);
#endif
    puts("PASS: BE PCM and stereo orientation preserved, consumed ring cleared, no stale replay; failed backend reports error and permits retry");
    return 0;
}
