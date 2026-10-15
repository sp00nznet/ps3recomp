/* Pure gate regression: cc -std=c17 -Wall -Wextra -Werror this-file.c */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../libs/input/pad_dialog_filter.h"

static int errors;
static void check(int condition,const char* why)
{
    if(!condition){++errors;fprintf(stderr,"FAIL: %s\n",why);}
}

static PadDialogRaw neutral(void)
{
    PadDialogRaw r={0};
    for(unsigned i=0;i<4;++i)r.stick[i]=128;
    return r;
}

int main(void)
{
    PadDialogFilter port[4];
    memset(port,0,sizeof(port));
    PadDialogRaw n=neutral(), held=n;
    held.buttons=0x0009; /* SELECT and START, including keyboard fallback. */
    for(unsigned p=0;p<4;++p)
        check(!pad_dialog_filter_step(&port[p],0,0,1,&held),
              "normal host input passes before any dialog");
    for(unsigned p=0;p<4;++p)
        check(pad_dialog_filter_step(&port[p],1,7,2,&held),
              "active dialog suppresses every port");
    check(pad_dialog_filter_step(&port[0],0,7,2,&n),
          "cached neutral after close cannot release");
    check(pad_dialog_filter_step(&port[0],0,7,3,&held),
          "fresh held buttons cannot carry into game");
    check(!pad_dialog_filter_step(&port[0],0,7,4,&n),
          "later fresh neutral poll releases quarantine");
    check(!pad_dialog_filter_step(&port[0],0,7,4,&held),
          "ordinary input passes once released");
    check(pad_dialog_filter_step(&port[1],0,7,2,&held),
          "port one remains independently quarantined");
    check(!pad_dialog_filter_step(&port[1],0,7,3,&n),
          "port one independently releases on fresh neutral");

    PadDialogFilter unseen={0};
    check(pad_dialog_filter_step(&unseen,0,8,10,&n),
          "epoch first seen after closure still quarantines");
    check(pad_dialog_filter_step(&unseen,0,8,10,&n),
          "repeated cached neutral cannot release unseen epoch");
    check(!pad_dialog_filter_step(&unseen,0,8,11,&n),
          "unseen epoch releases after later real neutral poll");
    check(pad_dialog_filter_step(&unseen,1,9,12,&n),
          "new generation suppresses after previous release");
    check(pad_dialog_filter_step(&unseen,0,9,13,&held),
          "new generation waits through held input");

    PadDialogRaw trigger=n;
    trigger.trigger[0]=30;
    check(!pad_dialog_raw_neutral(&trigger),"trigger over noise floor is held");
    check(pad_dialog_filter_step(&unseen,0,9,14,&trigger),
          "held trigger blocks release");
    PadDialogRaw stick=n;
    stick.stick[2]=151;
    check(!pad_dialog_raw_neutral(&stick),"stick outside deadzone is held");
    check(pad_dialog_filter_step(&unseen,0,9,15,&stick),
          "held stick blocks release");
    PadDialogRaw jitter=n;
    jitter.stick[0]=120; jitter.stick[1]=136; jitter.trigger[1]=8;
    check(pad_dialog_raw_neutral(&jitter),"small stick/trigger noise is neutral");
    check(!pad_dialog_filter_step(&unseen,0,9,16,&jitter),
          "fresh neutral with noise releases");
    PadDialogRaw pressure=n;
    pressure.pressure=1;
    check(!pad_dialog_raw_neutral(&pressure),"pressure signal blocks release");
    check(pad_dialog_filter_step(&port[2],0,7,20,&held),
          "remaining port remains held after dialog");

    printf("pad dialog filter: %s\n",errors?"FAIL":"PASS");
    return errors?1:0;
}
