/* Bounded, standalone SDL controller evidence. No game, window, GPU, rumble,
 * saved mapping, or device setting changes. --motion temporarily enables
 * available sensors and disables only those this process enabled on exit.
 * Build: clang -std=c11 -Wall -Wextra -Werror pad_sdl_probe.c -lSDL2 -o pad_sdl_probe.exe
 * Run: pad_sdl_probe --seconds 20 > axes.csv
 * Physical motion is opt-in: add --motion. Never labels stick mapping gyro. */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../libs/input/pad_motion_sdl.h"

typedef struct ProbePad {
    SDL_GameController* controller;
    SDL_JoystickID instance;
    PadMotionSDL motion;
    int accel_was_on, gyro_was_on;
    int sensors_requested;
} ProbePad;

static int number(const char* text,int lo,int hi,int* out)
{
    char* end=NULL;
    long n=strtol(text,&end,10);
    if (!*text || *end || n<lo || n>hi) return 0;
    *out=(int)n; return 1;
}

static void restore_sensors(ProbePad* p)
{
    if (p->controller && p->sensors_requested) {
        /* Restore the exact entry state even if enabling only one sensor
         * succeeded and the adapter rolled it back. */
        SDL_GameControllerSetSensorEnabled(p->controller,SDL_SENSOR_ACCEL,(SDL_bool)p->accel_was_on);
        SDL_GameControllerSetSensorEnabled(p->controller,SDL_SENSOR_GYRO,(SDL_bool)p->gyro_was_on);
    }
}

static void close_pad(ProbePad* p)
{
    if (!p->controller) return;
    restore_sensors(p);
    SDL_GameControllerClose(p->controller);
    memset(p,0,sizeof *p);
}

int main(int argc,char** argv)
{
    int seconds=10,interval=16,want_motion=0;
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--motion")) want_motion=1;
        else if (!strcmp(argv[i],"--seconds") && i+1<argc) {
            if (!number(argv[++i],1,120,&seconds)) return 2;
        } else if (!strcmp(argv[i],"--interval-ms") && i+1<argc) {
            if (!number(argv[++i],4,1000,&interval)) return 2;
        } else {
            fprintf(stderr,"Usage: pad_sdl_probe [--seconds 1..120] [--interval-ms 4..1000] [--motion]\n");
            return 2;
        }
    }
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_SENSOR)!=0) {
        fprintf(stderr,"[pad-probe] init failed: %s\n",SDL_GetError()); return 1;
    }
    SDL_version version; SDL_GetVersion(&version);
    fprintf(stderr,"[pad-probe] SDL %u.%u.%u joysticks=%d motion_requested=%d\n",
        version.major,version.minor,version.patch,SDL_NumJoysticks(),want_motion);
    ProbePad pads[4]={0};
    int count=0;
    for (int i=0;i<SDL_NumJoysticks() && count<4;i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_GameController* gc=SDL_GameControllerOpen(i);
        if (!gc) continue;
        ProbePad* p=&pads[count++];
        p->controller=gc;
        p->instance=SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
        p->accel_was_on=SDL_GameControllerIsSensorEnabled(gc,SDL_SENSOR_ACCEL);
        p->gyro_was_on=SDL_GameControllerIsSensorEnabled(gc,SDL_SENSOR_GYRO);
        if (want_motion && pad_motion_sdl_capable(gc)) {
            p->sensors_requested=1;
            if (!pad_motion_sdl_set_enabled(&p->motion,gc,1)) restore_sensors(p);
        }
        fprintf(stderr,"[pad-probe] slot=%d instance=%d name=%s vid=%04x pid=%04x accel=%d gyro=%d enabled=%d\n",
            count-1,(int)p->instance,SDL_GameControllerName(gc),
            SDL_GameControllerGetVendor(gc),SDL_GameControllerGetProduct(gc),
            SDL_GameControllerHasSensor(gc,SDL_SENSOR_ACCEL),
            SDL_GameControllerHasSensor(gc,SDL_SENSOR_GYRO),p->motion.enabled);
    }
    if (!count) {
        fprintf(stderr,"[pad-probe] no SDL game controller available; physical input unverified\n");
        SDL_Quit(); return 3;
    }
    puts("elapsed_ms,slot,instance,raw_lx,raw_ly,raw_rx,raw_ry,ps3_lx,ps3_ly,ps3_rx,ps3_ry,buttons,accel,gyro,motion_valid,sensor_x,sensor_y,sensor_z,sensor_g,sensor_us");
    Uint64 start=SDL_GetTicks64();
    do {
        SDL_PumpEvents(); SDL_GameControllerUpdate();
        for (int i=0;i<count;i++) {
            ProbePad* p=&pads[i]; SDL_GameController* gc=p->controller;
            if (!gc) continue;
            if (!SDL_GameControllerGetAttached(gc)) {
                fprintf(stderr,"[pad-probe] slot=%d disconnected\n",i); close_pad(p); continue;
            }
            int raw[4],ps3[4];
            for (int a=0;a<4;a++) {
                raw[a]=SDL_GameControllerGetAxis(gc,(SDL_GameControllerAxis)a);
                /* Exactly cellPad's SDL conversion: full -32768..32767
                 * becomes 0..255, centre 128; no guessed deadzone/curve. */
                ps3[a]=(raw[a]+32768)/256;
            }
            unsigned buttons=0;
            for (int b=0;b<SDL_CONTROLLER_BUTTON_MAX && b<32;b++)
                if (SDL_GameControllerGetButton(gc,(SDL_GameControllerButton)b)) buttons|=1u<<b;
            if (want_motion) pad_motion_sdl_poll(&p->motion,gc);
            printf("%llu,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%d,%d,%d,%u,%u,%u,%u,%llu\n",
                (unsigned long long)(SDL_GetTicks64()-start),i,(int)p->instance,
                raw[0],raw[1],raw[2],raw[3],ps3[0],ps3[1],ps3[2],ps3[3],buttons,
                SDL_GameControllerHasSensor(gc,SDL_SENSOR_ACCEL),SDL_GameControllerHasSensor(gc,SDL_SENSOR_GYRO),
                p->motion.valid,p->motion.sample[0],p->motion.sample[1],p->motion.sample[2],p->motion.sample[3],
                (unsigned long long)p->motion.timestamp);
        }
        SDL_Delay((Uint32)interval);
    } while (SDL_GetTicks64()-start < (Uint64)seconds*1000);
    for (int i=0;i<count;i++) close_pad(&pads[i]);
    int output_failed=fflush(stdout)!=0 || ferror(stdout);
    SDL_Quit(); return output_failed ? 1 : 0;
}
