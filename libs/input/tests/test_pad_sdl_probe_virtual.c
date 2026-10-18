/* Exercises the actual probe and SDL conversion with a process-local virtual
 * joystick. It does not emulate physical gyro or claim hardware support.
 * The runner checks the emitted CSV for the virtual device's exact axes. */
#define main probe_main
#include "../../../tools/pad_sdl_probe.c"
#undef main

int main(void)
{
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER)!=0) return 10;
    int index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,6,SDL_CONTROLLER_BUTTON_MAX,0);
    if (index<0 || !SDL_IsGameController(index)) return 11;
    SDL_Joystick* joystick=SDL_JoystickOpen(index);
    if (!joystick) return 12;
    const Sint16 axes[4]={-32768,32767,0,-1};
    for (int i=0;i<4;i++) if (SDL_JoystickSetVirtualAxis(joystick,i,axes[i])!=0) return 13;
    SDL_JoystickUpdate();
    fprintf(stderr,"[fixture] virtual_instance=%d\n",(int)SDL_JoystickInstanceID(joystick));
    char* args[]={"pad_sdl_probe","--seconds","1","--interval-ms","100"};
    return probe_main(5,args);
}
