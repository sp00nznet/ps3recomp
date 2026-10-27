#include "../rsx_metal_backend.m"
#include <assert.h>
#include <IOKit/graphics/IOGraphicsTypes.h>

/* Observe Metal independently of the production handler, including dropped
 * frames whose presentedTime is zero. This is fixture-only instrumentation. */
@interface FixturePresentations : NSObject {
@public
 pthread_mutex_t lock;
 unsigned callbacks;
 unsigned positive;
 double last_positive, last_callback;
 double times[6];
}
@end
@implementation FixturePresentations
- (instancetype)init {
 if ((self=[super init])) pthread_mutex_init(&lock,NULL);
 return self;
}
- (void)dealloc { pthread_mutex_destroy(&lock); }
@end

/* Only presentation callbacks are controlled here. Passing this substitute to
 * fps_track_drawable executes the same registration and callback as Metal. */
@interface FixtureDrawable : NSObject <MTLDrawable>
@property(nonatomic) CFTimeInterval presentedTime;
@property(nonatomic) NSUInteger drawableID;
@property(nonatomic,strong) NSMutableArray *handlers;
- (void)emitPresentedTime:(double)time;
@end
@implementation FixtureDrawable
- (instancetype)init {
 if ((self=[super init])) _handlers=[NSMutableArray new];
 return self;
}
- (void)addPresentedHandler:(MTLDrawablePresentedHandler)handler {
 [_handlers addObject:[handler copy]];
}
- (void)emitPresentedTime:(double)time {
 _presentedTime=time;
 for (MTLDrawablePresentedHandler handler in _handlers) handler(self);
}
- (void)present { assert(!"Use explicit fixture timestamps"); }
- (void)presentAtTime:(CFTimeInterval)time { (void)time;[self present]; }
- (void)presentAfterMinimumDuration:(CFTimeInterval)duration { (void)duration;[self present]; }
@end

static void check_callback_contract(void) {
 MetalFrameRateSession *old=[MetalFrameRateSession new];
 pthread_mutex_lock(&s_fps_session_lock);s_fps_session=old;
 pthread_mutex_unlock(&s_fps_session_lock);
 FixtureDrawable *drawable=[FixtureDrawable new];
 fps_track_drawable((id<CAMetalDrawable>)drawable);
 assert(drawable.handlers.count==1 && old->stats.displayed==0);
 [drawable emitPresentedTime:0]; /* completion/drop is not a display frame */
 assert(old->stats.displayed==0);
 [drawable emitPresentedTime:10];
 assert(old->stats.displayed==1 && old->stats.last_presented==10);
 [drawable emitPresentedTime:10]; /* duplicates and stale callbacks */
 [drawable emitPresentedTime:9];
 [drawable emitPresentedTime:NAN];
 [drawable emitPresentedTime:INFINITY];
 assert(old->stats.displayed==1 && old->stats.last_presented==10);
 [drawable emitPresentedTime:10.25];
 assert(old->stats.displayed==2 && old->stats.guest==0);
 assert(old->stats.interval_count==1 && old->stats.interval_sum==250);
 __weak MetalFrameRateSession *retained=old;
 fps_stop();
 old=nil;
 assert(retained); /* compositor-owned handler must keep its session alive */
 MetalFrameRateSession *late=retained;
 MetalFrameRateSession *fresh=[MetalFrameRateSession new];
 pthread_mutex_lock(&s_fps_session_lock);s_fps_session=fresh;
 pthread_mutex_unlock(&s_fps_session_lock);
 [drawable emitPresentedTime:10.5]; /* retained old handler after restart */
 assert(late->stats.displayed==3 && fresh->stats.displayed==0);
 rsx_metal_backend_note_guest_frame();
 assert(fresh->stats.guest==1 && late->stats.guest==0);
 fps_stop();
 FixtureDrawable *disabled=[FixtureDrawable new];
 fps_track_drawable((id<CAMetalDrawable>)disabled);
 fps_track_drawable(nil);
 assert(disabled.handlers.count==0);
 fprintf(stderr,"[FPS fixture] deterministic callback contract passed\n");
}

static int log_display_environment(void) {
 CGDirectDisplayID displays[16];uint32_t count=0;
 CGError error=CGGetActiveDisplayList(16,displays,&count);
 fprintf(stderr,"[FPS fixture] device=%s active_displays=%u display_error=%d app_active=%d window_visible=%d occlusion_visible=%d layer_size=%.0fx%.0f\n",
     s_dev.name.UTF8String,count,error,NSApp.active,s_window.visible,
     !!(s_window.occlusionState&NSWindowOcclusionStateVisible),
     s_layer.bounds.size.width,s_layer.bounds.size.height);
 for (uint32_t i=0;i<count;i++) fprintf(stderr,
     "[FPS fixture] display=%u online=%d active=%d builtin=%d vendor=%u model=%u pixels=%zux%zu\n",
     displays[i],CGDisplayIsOnline(displays[i]),CGDisplayIsActive(displays[i]),
     CGDisplayIsBuiltin(displays[i]),CGDisplayVendorNumber(displays[i]),
     CGDisplayModelNumber(displays[i]),CGDisplayPixelsWide(displays[i]),CGDisplayPixelsHigh(displays[i]));
 NSArray *windows=CFBridgingRelease(CGWindowListCopyWindowInfo(
     kCGWindowListOptionIncludingWindow,(CGWindowID)s_window.windowNumber));
 NSDictionary *window=windows.firstObject;
 fprintf(stderr,"[FPS fixture] window_number=%ld cg_onscreen=%d cg_layer=%ld cg_bounds=%s\n",
     (long)s_window.windowNumber,[window[(id)kCGWindowIsOnscreen] boolValue],
     [window[(id)kCGWindowLayer] longValue],
     [window[(id)kCGWindowBounds] description].UTF8String);
 CGDirectDisplayID display=[s_window.screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
 uint32_t vendor=CGDisplayVendorNumber(display),model=CGDisplayModelNumber(display);
 /* I/O Kit monitor identity is a stronger precondition than a visible virtual
  * window. It is not proof of physical scanout. Unknown/no-monitor IDs do not
  * promise that Metal can report presentation timestamps. */
 int identified=CGDisplayIsActive(display)&&CGDisplayIsOnline(display)&&
     vendor&&vendor!=UINT32_MAX&&vendor!=kDisplayVendorIDUnknown&&
     model&&model!=UINT32_MAX&&model!=kDisplayProductIDGeneric;
 fprintf(stderr,"[FPS fixture] window_display=%u identified_monitor=%d\n",display,identified);
 return identified;
}

/* Shutdown reaches these only for a fully initialized production engine. */
void rsx_set_backend(rsx_backend* backend) { (void)backend; }
void rsx_draw_engine_shutdown(void) {}
void rsx_draw_engine_set_backend(const rsx_draw_backend* backend) { (void)backend; }
static void* shutdown_worker(void* unused) {
 (void)unused;rsx_metal_backend_shutdown();return NULL;
}

int main(int argc, char **argv) {
 @autoreleasepool {
  assert(argc == 2);
  if (!strcmp(argv[1], "callback_contract")) {
   check_callback_contract();
  } else if (!strcmp(argv[1], "headless_shutdown")) {
   s_headless=1;pthread_t worker;
   assert(pthread_create(&worker,NULL,shutdown_worker,NULL)==0);
   assert(pthread_join(worker,NULL)==0);
  } else if (!strcmp(argv[1], "window") || !strcmp(argv[1], "drawable") || !strcmp(argv[1], "disabled")) {
   setenv("PS3RECOMP_METAL_FPS", "1", 1);
   if (!strcmp(argv[1], "disabled")) unsetenv("PS3RECOMP_METAL_FPS");
   s_dev = MTLCreateSystemDefaultDevice(); assert(s_dev);
   s_width = s_height = 64;
   assert(create_window_impl("FPS fixture") == 0);
   if (!strcmp(argv[1], "disabled")) {
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.2]];
    assert([s_window.title isEqualToString:@"FPS fixture"]);
#ifdef PS3RECOMP_METAL_FRAME_RATE
    assert(!s_fps_session && !s_fps_timer);
#endif
   } else if (!strcmp(argv[1], "drawable")) {
#ifdef PS3RECOMP_METAL_FRAME_RATE
    /* Inspect the callback counters directly in this scenario. Sampling would
     * reset them on the one-second timer while a slower compositor catches up;
     * the separate window scenario verifies that timer. */
    dispatch_source_cancel(s_fps_timer);s_fps_timer=nil;
    s_queue=[s_dev newCommandQueue];assert(s_queue);
    [NSApp finishLaunching];
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
    [s_window display];[CATransaction flush];
    const int identified_monitor=log_display_environment();
    FixturePresentations *observed=[FixturePresentations new];
    /* Read back our own clear, never any other application's image. */
    s_layer.framebufferOnly=NO;
    id<MTLBuffer> pixel=[s_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    assert(pixel);
    id<MTLCommandBuffer> flush=[s_queue commandBuffer];[flush commit];[flush waitUntilCompleted];
    assert(flush.status==MTLCommandBufferStatusCompleted && !flush.error);
    assert(s_fps_session->stats.displayed==0); /* GPU completion is not a presentation */
    for (int frame=0;frame<6;frame++) {
    id<CAMetalDrawable> drawable=[s_layer nextDrawable];assert(drawable);
    fps_track_drawable(drawable);
    [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
     pthread_mutex_lock(&observed->lock);
     const double time=shown.presentedTime;
     observed->times[frame]=time;++observed->callbacks;
     if (isfinite(time)&&time>0&&time>observed->last_positive) {
      ++observed->positive;observed->last_positive=time;
     }
     observed->last_callback=CACurrentMediaTime();
     fprintf(stderr,"[FPS fixture] raw_callback frame=%d presented_time=%.9f\n",frame,time);
     pthread_mutex_unlock(&observed->lock);
    }];
    rsx_metal_backend_note_guest_frame();
    id<MTLCommandBuffer> cb=[s_queue commandBuffer];
    MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=drawable.texture;
    pass.colorAttachments[0].loadAction=MTLLoadActionClear;
    pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    pass.colorAttachments[0].clearColor=MTLClearColorMake(0.1,0.3,0.5,1);
    id<MTLRenderCommandEncoder> encoder=[cb renderCommandEncoderWithDescriptor:pass];
    assert(encoder);[encoder endEncoding];
    id<MTLBlitCommandEncoder> copy=[cb blitCommandEncoder];assert(copy);
    [copy copyFromTexture:drawable.texture sourceSlice:0 sourceLevel:0
        sourceOrigin:MTLOriginMake(32,32,0) sourceSize:MTLSizeMake(1,1,1)
        toBuffer:pixel destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256];
    [copy endEncoding];
    [cb presentDrawable:drawable];[cb commit];[cb waitUntilCompleted];
    assert(cb.status==MTLCommandBufferStatusCompleted && !cb.error);
    const unsigned char *bgra=pixel.contents;
    assert(abs((int)bgra[0]-128)<=1 && abs((int)bgra[1]-77)<=1 &&
        abs((int)bgra[2]-26)<=1 && bgra[3]==255);
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    fprintf(stderr,"[FPS fixture] GPU clears completed=6 readback BGRA=128,77,26,255 (within one byte)\n");
    /* Completion precedes compositor callbacks. Wait for all observations and
     * a quiet interval; a virtual display may never deliver them. */
    const double deadline=CACurrentMediaTime()+5.0;
    for (;;) {
     pthread_mutex_lock(&observed->lock);
     int settled=observed->callbacks==6&&CACurrentMediaTime()-observed->last_callback>=0.05;
     pthread_mutex_unlock(&observed->lock);
     if (settled || CACurrentMediaTime()>=deadline) break;
     [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
    pthread_mutex_lock(&s_fps_session->lock);
    pthread_mutex_lock(&observed->lock);
    fprintf(stderr,"[FPS fixture] raw_callbacks=%u presented_times=%.9f,%.9f,%.9f,%.9f,%.9f,%.9f\n",
        observed->callbacks,observed->times[0],observed->times[1],observed->times[2],
        observed->times[3],observed->times[4],observed->times[5]);
    fprintf(stderr,"[FPS fixture] drawable displayed=%llu guest=%llu raw_positive=%u\n",
        s_fps_session->stats.displayed,s_fps_session->stats.guest,observed->positive);
    assert(observed->callbacks<=6);
    assert(s_fps_session->stats.displayed==observed->positive);
    assert(s_fps_session->stats.last_presented==observed->last_positive);
    if (identified_monitor) assert(s_fps_session->stats.displayed>0);
    if (!observed->positive) fprintf(stderr,
        "[FPS fixture] actual presentation unobserved; zero display FPS verified against raw Metal timestamps\n");
    assert(s_fps_session->stats.guest==6);
    pthread_mutex_unlock(&observed->lock);
    pthread_mutex_unlock(&s_fps_session->lock);
    MetalFrameRateSession *old=s_fps_session;
    fps_stop();fps_start();
    assert(s_fps_session!=old && s_fps_session->stats.displayed==0);
#else
    assert(!"Metal drawable tracking is missing");
#endif
   } else {
   /* No rendering thread is running: the title must still update on a stall. */
   [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.2]];
   assert([s_window.title containsString:@"display 0.0 FPS"]);
   assert([s_window.title containsString:@"guest 0.0 FPS"]);
   }
#ifdef PS3RECOMP_METAL_FRAME_RATE
   fps_stop();
#endif
   [s_window close];
  } else {
#ifdef PS3RECOMP_METAL_FRAME_RATE
   MetalFrameRate stats = {0};
   metal_frame_rate_reset(&stats, 10.0);
   for (int i=1;i<=60;i++) metal_frame_rate_guest(&stats);
   for (int i=1;i<=10;i++) metal_frame_rate_presented(&stats, 10.0+i*0.1);
   MetalFrameRateSample sample = metal_frame_rate_sample(&stats, 11.0);
   assert(fabs(sample.display_fps-10.0)<0.001);
   assert(fabs(sample.guest_fps-60.0)<0.001);
   assert(fabs(sample.average_ms-100.0)<0.001);
   assert(fabs(sample.p95_ms-100.0)<0.001);
   sample = metal_frame_rate_sample(&stats, 13.0);
   assert(sample.display_fps==0 && sample.guest_fps==0);
   assert(sample.average_ms==0 && sample.p95_ms==0);
   assert(fabs(sample.age_seconds-2.0)<0.001);
   metal_frame_rate_presented(&stats, 13.1);
   metal_frame_rate_presented(&stats, 13.1); /* duplicate time cannot add a frame */
   metal_frame_rate_presented(&stats, 12.9); /* out-of-order callback */
   sample = metal_frame_rate_sample(&stats, 14.0);
   assert(sample.display_fps==1 && sample.guest_fps==0);
   assert(fabs(sample.worst_ms-2100.0)<0.001);
   metal_frame_rate_reset(&stats, 20.0);
   metal_frame_rate_presented(&stats, 20.01);
   for (int i=1;i<=300;i++) metal_frame_rate_presented(&stats, 20.01+i*0.01);
   sample=metal_frame_rate_sample(&stats, 24.0);
   assert(sample.display_fps==75.25);
   assert(fabs(sample.p95_ms-10.0)<0.001);
   assert(fabs(sample.worst_ms-10.0)<0.001);
#else
   assert(!"Metal frame-rate measurement is missing");
#endif
  }
  puts("Metal frame-rate scenario passed");
 }
}
