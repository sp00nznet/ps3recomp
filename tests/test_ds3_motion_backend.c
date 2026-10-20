#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ds3_motion_backend.h"

/* CPU-only. A complete I/O seam is installed before any backend start.
 * Lifecycle calls have one owner; only sample polling runs concurrently.
 * Deliberately blocked callbacks and all synchronization have deadlines. */
enum { WAIT_MS = 2500, PROMPT_STOP_MS = 300 };
static atomic_int selected, selections, opens, closes, bad_io;
static atomic_int feature_calls, f2_calls, zero_calls, fail_f2, malformed_f2, feature_error;
static atomic_int block_select, select_entered, select_release;
static atomic_int block_feature_call, feature_entered, feature_release;
static atomic_int stop_returned, poll_quit, poll_done, poll_failed, stopped_visible;
static atomic_int poll_started, poll_after_stop;
static atomic_uint_fast64_t now_us;
static unsigned char payload[128];
static size_t payload_size;

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#expr);exit(1); \
} } while (0)

static int wait_value(atomic_int *value,int expected,Uint32 timeout) {
    Uint32 started=SDL_GetTicks();
    do {
        if(atomic_load(value)==expected)return 1;
        SDL_Delay(1);
    } while((Uint32)(SDL_GetTicks()-started)<timeout);
    return atomic_load(value)==expected;
}
static int mock_select(uint16_t vid,uint16_t pid,char *path,size_t capacity) {
    atomic_fetch_add(&selections,1);
    if(vid!=0x054c||pid!=0x0268||capacity<5) {
        atomic_store(&bad_io,1);return -1;
    }
    memcpy(path,"mock",5);
    if(atomic_load(&block_select)) {
        atomic_store(&select_entered,1);
        if(!wait_value(&select_release,1,WAIT_MS)) {
            atomic_store(&bad_io,1);return -1;
        }
    }
    return atomic_load(&selected);
}
static void *mock_open(const char *path) {
    if(strcmp(path,"mock")!=0)atomic_store(&bad_io,1);
    atomic_fetch_add(&opens,1);
    return &payload;
}
static int mock_feature(void *device,unsigned char *bytes,size_t capacity) {
    int call=atomic_fetch_add(&feature_calls,1)+1;
    if(device!=&payload||capacity<payload_size) {
        atomic_store(&bad_io,1);return -1;
    }
    unsigned char requested=bytes[0];
    if(requested==0xf2)atomic_fetch_add(&f2_calls,1);
    else if(requested==0)atomic_fetch_add(&zero_calls,1);
    else { atomic_store(&bad_io,1);return -1; }
    if(call==atomic_load(&block_feature_call)) {
        atomic_store(&feature_entered,1);
        if(!wait_value(&feature_release,1,WAIT_MS)) {
            atomic_store(&bad_io,1);return -1;
        }
        /* Returning valid counts after stop must never publish a late sample. */
    }
    if(atomic_load(&feature_error)||(requested==0xf2&&atomic_load(&fail_f2)))
        return -1;
    memcpy(bytes,payload,payload_size);
    bytes[0]=requested;
    if(requested==0xf2&&atomic_load(&malformed_f2))bytes[2]=0;
    return (int)payload_size;
}
static void mock_close(void *device) {
    if(device!=&payload)atomic_store(&bad_io,1);
    atomic_fetch_add(&closes,1);
}
static uint64_t mock_now(void) { return atomic_load(&now_us); }
static const ds3_motion_io mock_io={
    mock_select,mock_open,mock_feature,mock_close,mock_now
};
static void reap(void) {
    Uint32 started=SDL_GetTicks();
    do {
        if(ds3_motion_service())return;
        SDL_Delay(1);
    } while((Uint32)(SDL_GetTicks()-started)<WAIT_MS);
    CHECK(0&&"DS3 worker did not complete within deadline");
}
static int wait_state(ds3_motion_state expected) {
    Uint32 started=SDL_GetTicks();
    do {
        ds3_motion_sample sample;
        (void)ds3_motion_get_sample(&sample);
        if(sample.state==expected)return 1;
        SDL_Delay(1);
    } while((Uint32)(SDL_GetTicks()-started)<WAIT_MS);
    return 0;
}
static int wait_sample(ds3_motion_sample *out) {
    Uint32 started=SDL_GetTicks();
    do {
        if(ds3_motion_get_sample(out))return 1;
        SDL_Delay(1);
    } while((Uint32)(SDL_GetTicks()-started)<WAIT_MS);
    return 0;
}
static void check_empty(ds3_motion_state expected) {
    ds3_motion_sample sample;
    memset(&sample,0x55,sizeof(sample));
    CHECK(ds3_motion_get_sample(&sample)==0);
    CHECK(sample.state==expected&&sample.fresh_valid==0&&sample.monotonic_us==0);
    for(int i=0;i<4;++i)CHECK(sample.sensor[i]==0);
}
static void reset_mock(void) {
    CHECK(ds3_motion_service()==1);
    CHECK(ds3_motion_test_install_io(&mock_io)==1);
    atomic_store(&selected,1);atomic_store(&selections,0);
    atomic_store(&opens,0);atomic_store(&closes,0);atomic_store(&bad_io,0);
    atomic_store(&feature_calls,0);atomic_store(&f2_calls,0);atomic_store(&zero_calls,0);
    atomic_store(&fail_f2,0);atomic_store(&malformed_f2,0);atomic_store(&feature_error,0);
    atomic_store(&block_select,0);atomic_store(&select_entered,0);atomic_store(&select_release,0);
    atomic_store(&block_feature_call,0);atomic_store(&feature_entered,0);atomic_store(&feature_release,0);
    atomic_store(&now_us,1000000);
    /* Asymmetric oracle in guest order X,Y,Z,G. Physical SXS wire order
     * is X,Z,Y,G, little endian, with no Windows X sign inversion. */
    memset(payload,0x96,sizeof(payload));payload_size=63;
    payload[0]=0xf2;payload[1]=0;payload[2]=0x3f;
    payload[42]=0x23;payload[43]=0x01; /* X=291 */
    payload[44]=0x55;payload[45]=0x00; /* Z=85 */
    payload[46]=0xab;payload[47]=0x02; /* Y=683 */
    payload[48]=0xff;payload[49]=0x03; /* G=1023 */
}
static void stop_reap(void) {
    ds3_motion_stop();check_empty(DS3_MOTION_STOPPED);reap();
    CHECK(atomic_load(&bad_io)==0);
}
static int stop_owner(void *unused) {
    (void)unused;ds3_motion_stop();atomic_store(&stop_returned,1);return 0;
}
static int poll_samples(void *unused) {
    (void)unused;
    while(!atomic_load(&poll_quit)) {
        int stopped=atomic_load(&stopped_visible);
        ds3_motion_sample sample;
        int valid=ds3_motion_get_sample(&sample);
        atomic_store(&poll_started,1);
        if(stopped&&(valid||sample.fresh_valid||sample.state!=DS3_MOTION_STOPPED))
            atomic_store(&poll_failed,1);
        if(stopped)atomic_store(&poll_after_stop,1);
        SDL_Delay(1);
    }
    atomic_store(&poll_done,1);return 0;
}
static void require_prompt_stop(atomic_int *release) {
    atomic_store(&stop_returned,0);
    SDL_Thread *thread=SDL_CreateThread(stop_owner,"ds3-stop-owner",NULL);
    CHECK(thread!=NULL);
    int prompt=wait_value(&stop_returned,1,PROMPT_STOP_MS);
    if(!prompt) {
        atomic_store(release,1);
        CHECK(wait_value(&stop_returned,1,WAIT_MS));
        SDL_WaitThread(thread,NULL);
        CHECK(prompt);
    }
    SDL_WaitThread(thread,NULL);
    check_empty(DS3_MOTION_STOPPED);
}
static void test_ids_and_idle(void) {
    reset_mock();
    CHECK(ds3_motion_start(0x0000,0x0268)==0);
    CHECK(ds3_motion_start(0x054c,0x0000)==0);
    CHECK(atomic_load(&selections)==0);
    CHECK(ds3_motion_service()==1);
    CHECK(ds3_motion_get_sample(NULL)==0);
    ds3_motion_stop();ds3_motion_stop();check_empty(DS3_MOTION_STOPPED);
}
static void test_selection_completion_and_retry(void) {
    const int results[]={0,-2,-1};
    const ds3_motion_state states[]={
        DS3_MOTION_NO_DEVICE,DS3_MOTION_AMBIGUOUS,DS3_MOTION_IO_ERROR
    };
    for(int i=0;i<3;++i) {
        reset_mock();atomic_store(&selected,results[i]);
        CHECK(ds3_motion_start(0x054c,0x0268)==1);
        CHECK(wait_state(states[i]));reap();check_empty(states[i]);
        CHECK(atomic_load(&selections)==1&&atomic_load(&opens)==0);
        CHECK(ds3_motion_service()==1);check_empty(states[i]);
        CHECK(atomic_load(&selections)==1); /* service never retries */
        CHECK(ds3_motion_start(0x054c,0x0268)==1);
        CHECK(wait_state(states[i]));reap();check_empty(states[i]);
        CHECK(atomic_load(&selections)==2); /* deliberate explicit retry */
        ds3_motion_stop();check_empty(DS3_MOTION_STOPPED);
        ds3_motion_stop();check_empty(DS3_MOTION_STOPPED);
    }
}
static void test_blocked_select_stop(void) {
    reset_mock();atomic_store(&block_select,1);
    CHECK(ds3_motion_start(0x054c,0x0268)==1);
    CHECK(wait_value(&select_entered,1,WAIT_MS));
    require_prompt_stop(&select_release);
    CHECK(ds3_motion_service()==0);
    CHECK(ds3_motion_start(0x054c,0x0268)==0);
    CHECK(ds3_motion_test_install_io(&mock_io)==0);
    CHECK(atomic_load(&selections)==1&&atomic_load(&opens)==0);
    atomic_store(&select_release,1);reap();
    check_empty(DS3_MOTION_STOPPED);
    CHECK(atomic_load(&feature_calls)==0&&atomic_load(&bad_io)==0);
}
static void check_counts(const ds3_motion_sample *sample) {
    CHECK(sample->sensor[0]==291);
    CHECK(sample->sensor[1]==683);
    CHECK(sample->sensor[2]==85);
    CHECK(sample->sensor[3]==1023);
}
static void test_decode(void) {
    reset_mock();
    ds3_motion_sample sample;
    for(int id=0;id<2;++id) {
        payload[0]=id?0:0xf2;
        const size_t sizes[]={50,63,64};
        for(size_t i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
            CHECK(ds3_motion_test_decode(payload,sizes[i],&sample)==1);
            check_counts(&sample);
            CHECK(sample.fresh_valid==1);
        }
    }
    CHECK(ds3_motion_test_decode(NULL,63,&sample)==0);
    CHECK(ds3_motion_test_decode(payload,63,NULL)==0);
    CHECK(ds3_motion_test_decode(payload,0,&sample)==0);
    CHECK(ds3_motion_test_decode(payload,49,&sample)==0);
    CHECK(ds3_motion_test_decode(payload,65,&sample)==0);
    payload[0]=1;CHECK(ds3_motion_test_decode(payload,63,&sample)==0);
    payload[0]=0xf2;
    payload[1]=1;CHECK(ds3_motion_test_decode(payload,63,&sample)==0);payload[1]=0;
    payload[2]=0x40;CHECK(ds3_motion_test_decode(payload,63,&sample)==0);payload[2]=0x3f;
    for(int i=0;i<4;++i) {
        int offset=42+i*2;
        unsigned char lo=payload[offset],hi=payload[offset+1];
        payload[offset]=0;payload[offset+1]=4; /* 1024 invalid */
        CHECK(ds3_motion_test_decode(payload,63,&sample)==0);
        payload[offset]=0xff;payload[offset+1]=0xff;
        CHECK(ds3_motion_test_decode(payload,63,&sample)==0);
        payload[offset]=0;payload[offset+1]=0; /* zero remains a real raw count */
        CHECK(ds3_motion_test_decode(payload,63,&sample)==1);
        payload[offset]=lo;payload[offset+1]=hi;
    }
}
static void test_probe_and_freshness(void) {
    for(int fallback=0;fallback<3;++fallback) {
        reset_mock();atomic_store(&fail_f2,fallback==1);
        atomic_store(&malformed_f2,fallback==2);
        /* Let startup and at least one subsequent read succeed, then hold
         * publication still while advancing the controlled monotonic clock. */
        atomic_store(&block_feature_call,fallback?5:4);
        CHECK(ds3_motion_start(0x054c,0x0268)==1);
        CHECK(wait_value(&feature_entered,1,WAIT_MS));
        ds3_motion_sample sample;
        CHECK(wait_sample(&sample));check_counts(&sample);
        CHECK(sample.state==DS3_MOTION_RUNNING&&sample.monotonic_us==1000000);
        CHECK(ds3_motion_service()==0);
        CHECK(ds3_motion_test_install_io(&mock_io)==0);
        CHECK(ds3_motion_start(0x054c,0x0268)==0);
        if(fallback)CHECK(atomic_load(&f2_calls)==1&&atomic_load(&zero_calls)>=3);
        else CHECK(atomic_load(&f2_calls)>=3&&atomic_load(&zero_calls)==0);
        atomic_store(&now_us,1100000);CHECK(ds3_motion_get_sample(&sample)==1);
        atomic_store(&now_us,1100001);check_empty(DS3_MOTION_RUNNING);
        ds3_motion_stop();check_empty(DS3_MOTION_STOPPED);
        atomic_store(&feature_release,1);reap();check_empty(DS3_MOTION_STOPPED);
        CHECK(atomic_load(&bad_io)==0&&atomic_load(&closes)==1);
    }
    reset_mock();atomic_store(&block_feature_call,4);
    CHECK(ds3_motion_start(0x054c,0x0268)==1);
    CHECK(wait_value(&feature_entered,1,WAIT_MS));
    ds3_motion_sample sample;CHECK(wait_sample(&sample));
    atomic_store(&now_us,999999);check_empty(DS3_MOTION_RUNNING);
    ds3_motion_stop();atomic_store(&feature_release,1);reap();
}
static void test_feature_error_and_explicit_restart(void) {
    reset_mock();atomic_store(&feature_error,1);
    CHECK(ds3_motion_start(0x054c,0x0268)==1);
    CHECK(wait_state(DS3_MOTION_IO_ERROR));reap();check_empty(DS3_MOTION_IO_ERROR);
    int calls=atomic_load(&feature_calls);
    CHECK(calls==2&&atomic_load(&f2_calls)==1&&atomic_load(&zero_calls)==1);
    CHECK(ds3_motion_service()==1&&atomic_load(&feature_calls)==calls);
    atomic_store(&feature_error,0);atomic_store(&block_feature_call,calls+4);
    CHECK(ds3_motion_start(0x054c,0x0268)==1);
    CHECK(wait_value(&feature_entered,1,WAIT_MS));
    ds3_motion_sample sample;CHECK(wait_sample(&sample));check_counts(&sample);
    ds3_motion_stop();atomic_store(&feature_release,1);reap();
    check_empty(DS3_MOTION_STOPPED);
    CHECK(atomic_load(&closes)==2&&atomic_load(&bad_io)==0);
    reset_mock();payload[2]=0;
    CHECK(ds3_motion_start(0x054c,0x0268)==1);
    CHECK(wait_state(DS3_MOTION_IO_ERROR));reap();check_empty(DS3_MOTION_IO_ERROR);
    CHECK(atomic_load(&feature_calls)==2); /* both startup responses malformed */
    ds3_motion_stop();check_empty(DS3_MOTION_STOPPED);
    reset_mock();atomic_store(&block_feature_call,4);
    CHECK(ds3_motion_start(0x054c,0x0268)==1);
    CHECK(wait_value(&feature_entered,1,WAIT_MS));
    CHECK(wait_sample(&sample));
    atomic_store(&feature_error,1);atomic_store(&feature_release,1);
    CHECK(wait_state(DS3_MOTION_IO_ERROR));reap();check_empty(DS3_MOTION_IO_ERROR);
    CHECK(atomic_load(&zero_calls)==0); /* never change report IDs after startup */
    ds3_motion_stop();check_empty(DS3_MOTION_STOPPED);
}
static void test_blocked_feature_stop(void) {
    reset_mock();atomic_store(&block_feature_call,4);
    CHECK(ds3_motion_start(0x054c,0x0268)==1);
    CHECK(wait_value(&feature_entered,1,WAIT_MS));
    ds3_motion_sample sample;CHECK(wait_sample(&sample));
    atomic_store(&poll_quit,0);atomic_store(&poll_done,0);
    atomic_store(&poll_failed,0);atomic_store(&stopped_visible,0);
    atomic_store(&poll_started,0);atomic_store(&poll_after_stop,0);
    SDL_Thread *poller=SDL_CreateThread(poll_samples,"ds3-sample-poller",NULL);
    CHECK(poller!=NULL);
    CHECK(wait_value(&poll_started,1,WAIT_MS));
    require_prompt_stop(&feature_release);
    atomic_store(&stopped_visible,1);
    CHECK(ds3_motion_service()==0);
    CHECK(ds3_motion_start(0x054c,0x0268)==0);
    CHECK(ds3_motion_test_install_io(&mock_io)==0);
    CHECK(atomic_load(&selections)==1&&atomic_load(&feature_release)==0);
    ds3_motion_stop();check_empty(DS3_MOTION_STOPPED);
    atomic_store(&feature_release,1);reap();check_empty(DS3_MOTION_STOPPED);
    CHECK(wait_value(&poll_after_stop,1,WAIT_MS));
    atomic_store(&poll_quit,1);CHECK(wait_value(&poll_done,1,WAIT_MS));
    SDL_WaitThread(poller,NULL);
    CHECK(atomic_load(&poll_failed)==0&&atomic_load(&bad_io)==0);
    CHECK(atomic_load(&closes)==1);
    atomic_store(&block_feature_call,atomic_load(&feature_calls)+4);
    atomic_store(&feature_entered,0);atomic_store(&feature_release,0);
    CHECK(ds3_motion_start(0x054c,0x0268)==1);
    CHECK(wait_value(&feature_entered,1,WAIT_MS));
    CHECK(wait_sample(&sample));check_counts(&sample);
    ds3_motion_stop();atomic_store(&feature_release,1);reap();
    check_empty(DS3_MOTION_STOPPED);
}
int main(void) {
    SDL_SetMainReady();CHECK(SDL_Init(SDL_INIT_TIMER)==0);
    CHECK(ds3_motion_test_install_io(&mock_io)==1);
    test_decode();test_ids_and_idle();test_selection_completion_and_retry();
    test_blocked_select_stop();test_probe_and_freshness();
    test_feature_error_and_explicit_restart();test_blocked_feature_stop();
    stop_reap();CHECK(ds3_motion_service()==1);SDL_Quit();
    puts("PASS: CPU-only DS3 SXS framing, native counts, probe fallback, freshness, terminal retry and nonblocking lifecycle");
    return 0;
}
