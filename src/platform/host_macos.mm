#include "remoteos/host_platform.hpp"
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>

@interface RemoteOSCapture : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic) unsigned displayId;
@property(nonatomic) std::function<void(remoteos::platform::VideoFrame)> frame;
@property(nonatomic) std::function<void(std::string)> failure;
@end
@implementation RemoteOSCapture
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
  if(self.failure)self.failure(error.localizedDescription.UTF8String?:"Screen recording stopped");
}
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
  if(type!=SCStreamOutputTypeScreen||!CMSampleBufferIsValid(sample))return;
  auto* pixel=CMSampleBufferGetImageBuffer(sample);if(!pixel)return;
  CVPixelBufferLockBaseAddress(pixel,kCVPixelBufferLock_ReadOnly);
  remoteos::platform::VideoFrame output;output.display_id=self.displayId;output.width=CVPixelBufferGetWidth(pixel);output.height=CVPixelBufferGetHeight(pixel);output.stride=CVPixelBufferGetBytesPerRow(pixel);output.timestamp=std::chrono::steady_clock::now();
  const auto count=size_t(output.stride)*output.height;auto* bytes=static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixel));if(bytes&&count<128*1024*1024)output.bgra.assign(bytes,bytes+count);
  CVPixelBufferUnlockBaseAddress(pixel,kCVPixelBufferLock_ReadOnly);
  if(!output.bgra.empty()&&self.frame)self.frame(std::move(output));
}
@end
namespace remoteos::platform {
namespace {
CGKeyCode key_code(unsigned key){
  switch(key){
#define K(ev,apple) case ev:return apple;
    K(1,kVK_Escape) K(2,kVK_ANSI_1) K(3,kVK_ANSI_2) K(4,kVK_ANSI_3) K(5,kVK_ANSI_4) K(6,kVK_ANSI_5) K(7,kVK_ANSI_6) K(8,kVK_ANSI_7) K(9,kVK_ANSI_8) K(10,kVK_ANSI_9) K(11,kVK_ANSI_0) K(12,kVK_ANSI_Minus) K(13,kVK_ANSI_Equal) K(14,kVK_Delete) K(15,kVK_Tab)
    K(16,kVK_ANSI_Q) K(17,kVK_ANSI_W) K(18,kVK_ANSI_E) K(19,kVK_ANSI_R) K(20,kVK_ANSI_T) K(21,kVK_ANSI_Y) K(22,kVK_ANSI_U) K(23,kVK_ANSI_I) K(24,kVK_ANSI_O) K(25,kVK_ANSI_P) K(26,kVK_ANSI_LeftBracket) K(27,kVK_ANSI_RightBracket) K(28,kVK_Return) K(29,kVK_Control)
    K(30,kVK_ANSI_A) K(31,kVK_ANSI_S) K(32,kVK_ANSI_D) K(33,kVK_ANSI_F) K(34,kVK_ANSI_G) K(35,kVK_ANSI_H) K(36,kVK_ANSI_J) K(37,kVK_ANSI_K) K(38,kVK_ANSI_L) K(39,kVK_ANSI_Semicolon) K(40,kVK_ANSI_Quote) K(41,kVK_ANSI_Grave) K(42,kVK_Shift) K(43,kVK_ANSI_Backslash)
    K(44,kVK_ANSI_Z) K(45,kVK_ANSI_X) K(46,kVK_ANSI_C) K(47,kVK_ANSI_V) K(48,kVK_ANSI_B) K(49,kVK_ANSI_N) K(50,kVK_ANSI_M) K(51,kVK_ANSI_Comma) K(52,kVK_ANSI_Period) K(53,kVK_ANSI_Slash) K(54,kVK_RightShift) K(56,kVK_Option) K(57,kVK_Space) K(58,kVK_CapsLock)
    K(59,kVK_F1) K(60,kVK_F2) K(61,kVK_F3) K(62,kVK_F4) K(63,kVK_F5) K(64,kVK_F6) K(65,kVK_F7) K(66,kVK_F8) K(67,kVK_F9) K(68,kVK_F10) K(87,kVK_F11) K(88,kVK_F12)
    K(97,kVK_RightControl) K(100,kVK_RightOption) K(102,kVK_Home) K(103,kVK_UpArrow) K(104,kVK_PageUp) K(105,kVK_LeftArrow) K(106,kVK_RightArrow) K(107,kVK_End) K(108,kVK_DownArrow) K(109,kVK_PageDown) K(111,kVK_ForwardDelete) K(125,kVK_Command) K(126,kVK_RightCommand)
#undef K
    default:throw std::runtime_error("Key is not mapped on macOS");
  }
}
class MacDesktop final:public HostPlatform {
  SCShareableContent* content_=nil;
  NSMutableArray<SCStream*>* streams_=[NSMutableArray new];
  NSMutableArray<RemoteOSCapture*>* outputs_=[NSMutableArray new];
  dispatch_queue_t queue_=dispatch_queue_create("org.remoteos.capture",DISPATCH_QUEUE_SERIAL);
  std::atomic<bool> cancelled_{false};std::set<unsigned> held_,buttons_;std::vector<DisplayConfig> layouts_;
  CGEventFlags flags_=0;CGPoint point_=CGPointZero;
  mutable std::mutex error_mutex_;std::string error_;
public:
  ~MacDesktop(){stop();}
  Capabilities capabilities()const override {Capabilities c;c.capture=content_!=nil;c.input=AXIsProcessTrusted();c.diagnostics={"macOS application sharing requires Screen Recording and Accessibility permission; login/secure prompts are outside app access", "Audio playback capture is not implemented in this backend"};return c;}
  void prepare(const std::string&)override {
    cancelled_=false;
    // The containing GUI requests consent on its main thread. A helper must
    // report missing grants, rather than show prompts from a setup worker.
    if(!CGPreflightScreenCaptureAccess())throw std::runtime_error("Approve RemoteOS Host in Screen Recording, then quit and reopen the app");
    if(!AXIsProcessTrusted())throw std::runtime_error("Approve RemoteOS Host in Accessibility, then start sharing again");
    dispatch_semaphore_t ready=dispatch_semaphore_create(0);__block SCShareableContent* found=nil;__block NSString* failure=nil;
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:YES completionHandler:^(SCShareableContent* c,NSError* e){found=c;failure=e.localizedDescription;dispatch_semaphore_signal(ready);}];
    for(unsigned i=0;i<100;++i){if(cancelled_)throw std::runtime_error("Sharing cancelled");if(dispatch_semaphore_wait(ready,dispatch_time(DISPATCH_TIME_NOW,100*NSEC_PER_MSEC))==0){if(!found)throw std::runtime_error(failure?failure.UTF8String:"No display grant");content_=found;return;}}
    throw std::runtime_error("ScreenCaptureKit permission request timed out");
  }
  void cancel_prepare()noexcept override {cancelled_=true;}
  std::string session_error()const override {std::lock_guard<std::mutex> lock(error_mutex_);return error_;}
  std::vector<DisplayConfig> desktop_displays()const override {std::vector<DisplayConfig> out;for(SCDisplay* display in content_.displays){if(out.size()>=2)break;const auto bounds=CGDisplayBounds(display.displayID);out.push_back({unsigned(out.size()),unsigned(display.width),unsigned(display.height),int(bounds.origin.x),int(bounds.origin.y),30});}return out;}
  void start(const std::vector<DisplayConfig>& displays,FrameCallback callback)override {
    stop();{std::lock_guard<std::mutex> lock(error_mutex_);error_.clear();}layouts_=displays;if(displays.empty()||displays.size()>content_.displays.count)throw std::runtime_error("Display selection is unavailable");
    for(size_t i=0;i<displays.size();++i){SCDisplay* display=content_.displays[i];SCContentFilter* filter=[[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];SCStreamConfiguration* config=[SCStreamConfiguration new];config.width=display.width;config.height=display.height;config.pixelFormat=kCVPixelFormatType_32BGRA;config.minimumFrameInterval=CMTimeMake(1,displays[i].fps);config.queueDepth=3;config.showsCursor=YES;
      RemoteOSCapture* output=[RemoteOSCapture new];output.displayId=displays[i].id;output.frame=callback;output.failure=[this](std::string message){std::lock_guard<std::mutex> lock(error_mutex_);error_=std::move(message);};SCStream* stream=[[SCStream alloc] initWithFilter:filter configuration:config delegate:output];NSError* error=nil;if(![stream addStreamOutput:output type:SCStreamOutputTypeScreen sampleHandlerQueue:queue_ error:&error])throw std::runtime_error(error.localizedDescription.UTF8String);
      dispatch_semaphore_t ready=dispatch_semaphore_create(0);__block NSError* startError=nil;[stream startCaptureWithCompletionHandler:^(NSError* e){startError=e;dispatch_semaphore_signal(ready);}];if(dispatch_semaphore_wait(ready,dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC))||startError)throw std::runtime_error(startError?startError.localizedDescription.UTF8String:"Capture startup timed out");[streams_ addObject:stream];[outputs_ addObject:output];
    }
  }
  void stop()noexcept override {release_all();for(SCStream* s in streams_){dispatch_semaphore_t done=dispatch_semaphore_create(0);[s stopCaptureWithCompletionHandler:^(NSError*){dispatch_semaphore_signal(done);}];dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,3*NSEC_PER_SEC));}dispatch_sync(queue_,^{});[streams_ removeAllObjects];[outputs_ removeAllObjects];layouts_.clear();}
  void inject(const InputEvent& e)override {
    if(!AXIsProcessTrusted())throw std::runtime_error("Accessibility permission was revoked");CGEventRef event=nullptr;
    if(e.kind==InputKind::key){const auto key=key_code(e.key);if(e.down)held_.insert(e.key);else held_.erase(e.key);flags_=0;for(auto k:held_){if(k==42||k==54)flags_|=kCGEventFlagMaskShift;if(k==29||k==97)flags_|=kCGEventFlagMaskControl;if(k==56||k==100)flags_|=kCGEventFlagMaskAlternate;if(k==125||k==126)flags_|=kCGEventFlagMaskCommand;}
      event=CGEventCreateKeyboardEvent(nullptr,key,e.down);CGEventSetIntegerValueField(event,kCGKeyboardEventAutorepeat,e.repeat);
    }else if(e.kind==InputKind::absolute_pointer||e.kind==InputKind::relative_pointer){
      if(e.kind==InputKind::relative_pointer){auto current=CGEventCreate(nullptr);if(current){point_=CGEventGetLocation(current);CFRelease(current);}point_.x+=e.delta_x;point_.y+=e.delta_y;}else{for(size_t i=0;i<layouts_.size();++i){const auto& d=layouts_[i];if(e.x>=d.x&&e.y>=d.y&&e.x<d.x+int(d.width)&&e.y<d.y+int(d.height)){const auto bounds=CGDisplayBounds(content_.displays[i].displayID);point_={bounds.origin.x+(e.x-d.x)*bounds.size.width/d.width,bounds.origin.y+(e.y-d.y)*bounds.size.height/d.height};break;}}}
      const auto type=buttons_.count(272)?kCGEventLeftMouseDragged:buttons_.count(273)?kCGEventRightMouseDragged:buttons_.count(274)?kCGEventOtherMouseDragged:kCGEventMouseMoved;event=CGEventCreateMouseEvent(nullptr,type,point_,buttons_.count(273)?kCGMouseButtonRight:buttons_.count(274)?kCGMouseButtonCenter:kCGMouseButtonLeft);
      if(event&&e.kind==InputKind::relative_pointer){CGEventSetIntegerValueField(event,kCGMouseEventDeltaX,e.delta_x);CGEventSetIntegerValueField(event,kCGMouseEventDeltaY,e.delta_y);}
    }else if(e.kind==InputKind::button){if(e.key<272||e.key>274)throw std::runtime_error("Unsupported pointer button");if(e.down)buttons_.insert(e.key);else buttons_.erase(e.key);const auto type=e.key==272?(e.down?kCGEventLeftMouseDown:kCGEventLeftMouseUp):e.key==273?(e.down?kCGEventRightMouseDown:kCGEventRightMouseUp):(e.down?kCGEventOtherMouseDown:kCGEventOtherMouseUp);event=CGEventCreateMouseEvent(nullptr,type,point_,e.key==272?kCGMouseButtonLeft:e.key==273?kCGMouseButtonRight:kCGMouseButtonCenter);
    }else if(e.kind==InputKind::wheel)event=CGEventCreateScrollWheelEvent(nullptr,kCGScrollEventUnitLine,2,e.delta_y,e.delta_x);
    if(event){CGEventSetFlags(event,flags_);CGEventPost(kCGHIDEventTap,event);CFRelease(event);}
  }
  void release_all()noexcept override {const auto keys=held_,buttons=buttons_;for(auto key:keys)try{InputEvent e;e.key=key;e.kind=InputKind::key;e.down=false;inject(e);}catch(...){}for(auto key:buttons)try{InputEvent e;e.key=key;e.kind=InputKind::button;e.down=false;inject(e);}catch(...){}held_.clear();buttons_.clear();flags_=0;}
};
}
std::unique_ptr<HostPlatform> make_desktop_host_platform(){return std::make_unique<MacDesktop>();}
std::unique_ptr<HostPlatform> make_host_platform(){return make_desktop_host_platform();}
}
