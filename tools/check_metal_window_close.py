"""Exercise actual AppKit notification APIs and production pump/helper bodies."""
import pathlib,re,subprocess,sys,json,tempfile
sdk=pathlib.Path(sys.argv[1]) if len(sys.argv)>1 else pathlib.Path(__file__).resolve().parents[1];r=pathlib.Path(tempfile.mkdtemp(prefix='ps3recomp-window-close-'));s=(sdk/'libs/video/rsx_metal_backend.m').read_text()
def fun(name):
 m=re.search(r'(?:static )?(?:int|void) '+name+r'\([^\n]*\)\n\{',s);assert m,name
 i=m.end();depth=1
 while depth:
  if s[i]=='{':depth+=1
  elif s[i]=='}':depth-=1
  i+=1
 return s[m.start():i]
helpers=''
if 'static void window_close_observer_start(' in s:
 helpers=fun('window_close_observer_stop')+'\n'+fun('window_close_observer_start')
else:helpers='static void window_close_observer_start(NSWindow* w){(void)w;}\nstatic void window_close_observer_stop(void){}'
init=s[s.index('int rsx_metal_backend_init('):s.index('void rsx_metal_backend_shutdown(')]
tail=init[init.index('        s_ready'):init.index('        rsx_draw_engine_set_backend')]
assert re.fullmatch(r'\s*s_ready\s*=\s*1;\s*(?:s_closed\s*=\s*0;\s*)?',tail), 'readiness tail changed: review required'
pump=fun('pump_messages_impl')+'\n'+fun('rsx_metal_backend_pump_messages')+'\nstatic void actual_readiness_tail(void) {\n'+tail+'\n}';body=r'''#import <AppKit/AppKit.h>
#include <stdatomic.h>
#include <stdio.h>
#define TARGET_OS_IPHONE 0
static NSWindow* s_window;static int s_headless,s_ready=1;
static atomic_int s_closed;static id s_window_close_observer;
'''+helpers+'\n'+pump+r'''
static int checks,failures;
#define T(c,m) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",m);}}while(0)
static NSWindow* make(void){NSWindow*w=[[NSWindow alloc]initWithContentRect:NSMakeRect(-10000,-10000,64,64) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];[w setReleasedWhenClosed:NO];return w;}
int main(void){@autoreleasepool{[NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];s_window=make();window_close_observer_start(s_window);s_ready=0;[s_window close];actual_readiness_tail();T(rsx_metal_backend_pump_messages()==-1,"close during initialization survives actual readiness tail");s_window=make();window_close_observer_start(s_window);
T(pump_messages_impl()==0,"initial ordered-out owned window is not close");
[s_window orderFront:nil];T(pump_messages_impl()==0,"visible remains active");
[s_window orderOut:nil];T(pump_messages_impl()==0,"orderOut is not close");
[s_window orderFront:nil];[NSApp hide:nil];T(pump_messages_impl()==0,"hide is not close");[NSApp unhide:nil];
[s_window miniaturize:nil];T(pump_messages_impl()==0,"miniaturization is not close");[s_window deminiaturize:nil];
NSWindow* foreign=make();[foreign close];T(pump_messages_impl()==0,"foreign window close does not stop owned session");
[s_window close];T(pump_messages_impl()==-1,"actual owned window close is observed");T(pump_messages_impl()==-1,"close remains sticky while initialized");
window_close_observer_stop();atomic_store(&s_closed,0);[[NSNotificationCenter defaultCenter]postNotificationName:NSWindowWillCloseNotification object:s_window];T(pump_messages_impl()==0,"removed observer cannot mutate close state");
NSWindow* old=s_window;s_window=make();window_close_observer_start(s_window);[[NSNotificationCenter defaultCenter]postNotificationName:NSWindowWillCloseNotification object:old];T(pump_messages_impl()==0,"old generation close does not affect replacement");[s_window close];T(pump_messages_impl()==-1,"replacement owned close observed");
s_headless=1;T(rsx_metal_backend_pump_messages()==0,"headless never reports stale window close");s_headless=0;s_ready=0;T(rsx_metal_backend_pump_messages()==0,"failed/not-ready initialization never reports close");s_ready=1;window_close_observer_stop();T(s_window_close_observer==nil,"observer token released on stop");[old close];s_window=nil;
printf("window notification checks=%d failures=%d\n",checks,failures);return failures?1:0;}}
'''
p=r/'window_notification.m';p.write_text(body);cmd=['clang','-O2','-fsanitize=undefined','-fno-sanitize-recover=all','-fobjc-arc',str(p),'-framework','Cocoa','-o',str(r/'window_notification')]
q=subprocess.run(cmd,capture_output=True);(r/'notification-build.log').write_bytes(q.stdout+q.stderr);assert q.returncode==0,q.stderr.decode();q=subprocess.run([str(r/'window_notification')],capture_output=True,timeout=15);(r/'notification.stdout').write_bytes(q.stdout);(r/'notification.stderr').write_bytes(q.stderr);print(q.stdout.decode(),end='');print(q.stderr.decode(),end='',file=sys.stderr);print('fixture artifacts:',r);sys.exit(q.returncode)
