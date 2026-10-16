#define _GNU_SOURCE
#include <SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static atomic_int ready,done;static int serialized;
static void* video(void* unused){
 (void)unused;unsigned flags=SDL_INIT_VIDEO|(serialized?(SDL_INIT_GAMECONTROLLER|SDL_INIT_AUDIO):0);
 if(SDL_InitSubSystem(flags)){fprintf(stderr,"init:%s\n",SDL_GetError());atomic_store(&ready,-1);return NULL;}
 SDL_Window*w=SDL_CreateWindow("Folklore isolated input probe",0,0,64,64,SDL_WINDOW_HIDDEN);
 if(!w){fprintf(stderr,"window:%s\n",SDL_GetError());atomic_store(&ready,-1);SDL_Quit();return NULL;}
 atomic_store(&ready,1);
 while(!atomic_load(&done)){SDL_Event e;while(SDL_PollEvent(&e)){}usleep(1000);}
 SDL_DestroyWindow(w);SDL_Quit();return NULL;
}
/* Default is the corrected lifecycle. The explicit reproduction mode is
 * diagnostic only and may abort in SDL2-compat while initializing devices. */
int main(int argc,char**argv){
 serialized=!(argc>1&&!strcmp(argv[1],"reproduce-split-init"));pthread_t t;
 if(pthread_create(&t,NULL,video,NULL))return 2;
 while(!atomic_load(&ready))usleep(1000);
 if(atomic_load(&ready)<0){pthread_join(t,NULL);return 3;}
 if(!serialized&&SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)){fprintf(stderr,"controller-init:%s\n",SDL_GetError());atomic_store(&done,1);pthread_join(t,NULL);return 4;}
 int opened=0;SDL_GameController*g[16]={0};
 SDL_LockJoysticks();int count=SDL_NumJoysticks();
 for(int i=0;i<count&&opened<16;i++)if(SDL_IsGameController(i)){g[opened]=SDL_GameControllerOpen(i);if(g[opened])opened++;}
 SDL_UnlockJoysticks();
 for(int i=0;i<opened;i++){
  if(SDL_GameControllerHasSensor(g[i],SDL_SENSOR_ACCEL))SDL_GameControllerSetSensorEnabled(g[i],SDL_SENSOR_ACCEL,SDL_TRUE);
  if(SDL_GameControllerHasSensor(g[i],SDL_SENSOR_GYRO))SDL_GameControllerSetSensorEnabled(g[i],SDL_SENSOR_GYRO,SDL_TRUE);
 }
 unsigned valid=0;
 for(int k=0;k<250;k++){
  SDL_GameControllerUpdate();SDL_LockJoysticks();(void)SDL_NumJoysticks();SDL_UnlockJoysticks();
  for(int i=0;i<opened;i++){float a[3];Uint64 stamp=0;(void)SDL_GameControllerGetButton(g[i],SDL_CONTROLLER_BUTTON_A);valid+=SDL_GameControllerGetSensorDataWithTimestamp(g[i],SDL_SENSOR_ACCEL,&stamp,a,3)==0;}
  usleep(1000);
 }
 for(int i=0;i<opened;i++)SDL_GameControllerClose(g[i]);
 atomic_store(&done,1);pthread_join(t,NULL);
 printf("SDL_THREAD_PROBE mode=%s devices=%d opened=%d accel_samples=%u PASS\n",serialized?"serialized":"split-init",count,opened,valid);return 0;
}
