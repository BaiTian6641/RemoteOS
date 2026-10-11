#import <Cocoa/Cocoa.h>
#include "host_app_state.hpp"
#include "portable_runtime.hpp"
#include <cstring>
#include <memory>
#import <ApplicationServices/ApplicationServices.h>
namespace {std::filesystem::path startup_receipt;}
@interface RemoteOSHostWindow : NSObject <NSApplicationDelegate>
@end
@implementation RemoteOSHostWindow {
  NSWindow* window;NSTextField *address,*port,*status,*pin,*fingerprint,*resources;
  NSPopUpButton *fps,*transport;NSButton *start,*stop;NSTask* task;NSPipe* pipe;
  std::unique_ptr<remoteos::app::HostState> state;
  NSString* lastFailure;BOOL stopping;NSMutableData* logTail;
}
- (NSTextField*)label:(NSString*)text x:(CGFloat)x y:(CGFloat)y width:(CGFloat)width {
  NSTextField* field=[NSTextField labelWithString:text];field.frame=NSMakeRect(x,y,width,44);field.textColor=[NSColor colorWithWhite:0.96 alpha:1];field.font=[NSFont fontWithName:@"IBM Plex Sans" size:16]?:[NSFont systemFontOfSize:16];field.selectable=YES;field.maximumNumberOfLines=3;[window.contentView addSubview:field];return field;
}
- (void)launchFailure:(NSString*)why {
  NSLog(@"RemoteOS startup: %@",why);
  if(!startup_receipt.empty()){try{remoteos::write_private_file(startup_receipt,remoteos::Json{{"status","failed"},{"message",why.UTF8String?:"Startup failed"}}.dump());}catch(...){}[NSApp terminate:nil];return;}
  NSAlert* alert=[NSAlert new];alert.messageText=@"RemoteOS could not start";alert.informativeText=why;[alert runModal];[NSApp terminate:nil];
}
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
 @try {try {
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,760,650) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];window.title=@"RemoteOS Host";window.backgroundColor=[NSColor colorWithWhite:0.086 alpha:1];window.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  [self label:@"RemoteOS Host — Share your desktop" x:24 y:582 width:712];[self label:@"Keep this app open. Approve Screen Recording and Accessibility in System Settings." x:24 y:532 width:712];
  address=[[NSTextField alloc] initWithFrame:NSMakeRect(24,492,360,28)];address.stringValue=@"0.0.0.0";[window.contentView addSubview:address];port=[[NSTextField alloc] initWithFrame:NSMakeRect(400,492,150,28)];port.stringValue=@"47010";[window.contentView addSubview:port];
  fps=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(24,440,230,32) pullsDown:NO];[fps addItemsWithTitles:@[@"15 fps",@"20 fps",@"30 fps",@"60 fps"]];[fps selectItemAtIndex:2];[window.contentView addSubview:fps];
  transport=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(280,440,420,32) pullsDown:NO];[transport addItemsWithTitles:@[@"Trusted LAN / Tailscale UDP",@"Encrypted UDP"]];[window.contentView addSubview:transport];
  [self label:@"Verification fingerprint — compare with the client" x:24 y:384 width:712];fingerprint=[self label:@"Start sharing to create the identity." x:24 y:343 width:712];fingerprint.font=[NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightRegular];
  pin=[self label:@"Pairing PIN appears here; valid once for 10 minutes." x:24 y:288 width:712];
  start=[NSButton buttonWithTitle:@"Start sharing" target:self action:@selector(startSharing:)];start.frame=NSMakeRect(24,232,330,40);[window.contentView addSubview:start];stop=[NSButton buttonWithTitle:@"Stop sharing" target:self action:@selector(stopSharing:)];stop.frame=NSMakeRect(380,232,330,40);stop.enabled=NO;[window.contentView addSubview:stop];
  status=[self label:@"Sharing is off. This app shares existing displays. Playback audio and independent desktops are unavailable on this macOS backend." x:24 y:169 width:712];
  resources=[self label:@"Webcam: add its host URL as an OBS Browser Source, then Start Virtual Camera. SD files are shared using WebDAV." x:24 y:73 width:712];resources.maximumNumberOfLines=5;resources.frame=NSMakeRect(24,30,712,105);
  state=std::make_unique<remoteos::app::HostState>(([NSBundle.mainBundle.executablePath stringByDeletingLastPathComponent].UTF8String+std::string("/remoteos-host")),std::filesystem::path(NSHomeDirectory().UTF8String)/"Library/Application Support/RemoteOS/Host");
  for(unsigned i=0;i<4;++i){NSArray* names=@[@"Check permissions",@"Screen Recording",@"Accessibility",@"Open host log"];SEL actions[]={@selector(checkPermissions:),@selector(screenSettings:),@selector(accessibilitySettings:),@selector(openLog:)};NSButton* button=[NSButton buttonWithTitle:names[i] target:self action:actions[i]];button.frame=NSMakeRect(24+i*176,136,168,28);[window.contentView addSubview:button];}
  logTail=[NSMutableData new];
  [window center];[window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];
  if(!startup_receipt.empty()){
    remoteos::write_private_file(startup_receipt,remoteos::Json{{"status","passed"},{"visible_window",bool(window.visible)},{"state_directory",state->directory.u8string()},{"screen_recording",bool(CGPreflightScreenCaptureAccess())},{"accessibility",bool(AXIsProcessTrusted())},{"permissions_requested",false}}.dump());
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,100*NSEC_PER_MSEC),dispatch_get_main_queue(),^{[NSApp terminate:nil];});
  }
 }catch(const std::exception& e){[self launchFailure:[NSString stringWithUTF8String:e.what()]?:@"Startup failed"];}}
 @catch(NSException* e){[self launchFailure:e.reason?:@"A macOS application error occurred"];}
}
- (void)screenSettings:(id)sender {[[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"]];}
- (void)accessibilitySettings:(id)sender {[[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"]];}
- (void)openLog:(id)sender {if(state)[[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:(state->directory/"last-host.log").c_str()]]];}
- (void)checkPermissions:(id)sender {
  if(!CGPreflightScreenCaptureAccess())CGRequestScreenCaptureAccess();
  NSDictionary* options=@{(__bridge NSString*)kAXTrustedCheckOptionPrompt:@YES};
  const bool input=AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
  status.stringValue=CGPreflightScreenCaptureAccess()&&input?@"Screen Recording and Accessibility are granted. Start sharing when ready.":@"Allow RemoteOS Host in Screen Recording and Accessibility. After Screen Recording changes, quit and reopen the app.";
}
- (void)startSharing:(id)sender {
  if(task.running)return;
  if(!CGPreflightScreenCaptureAccess()||!AXIsProcessTrusted()){[self checkPermissions:nil];if(!CGPreflightScreenCaptureAccess()||!AXIsProcessTrusted())return;}
  lastFailure=nil;stopping=NO;[logTail setLength:0];
  try {
    static const unsigned rates[]={15,20,30,60};auto config=state->prepare(address.stringValue.UTF8String,remoteos::app::parse_port(port.stringValue.UTF8String),false,"","monitors",1366,768,rates[fps.indexOfSelectedItem],transport.indexOfSelectedItem==0?"rtp-udp":"dtls-srtp");
    task=[NSTask new];task.executableURL=[NSURL fileURLWithPath:[NSString stringWithUTF8String:state->executable.c_str()]];task.arguments=@[@"--desktop",@"--config",[NSString stringWithUTF8String:config.c_str()]];pipe=[NSPipe pipe];task.standardOutput=pipe;task.standardError=pipe;
    __weak RemoteOSHostWindow* weakSelf=self;__block NSMutableData* pending=[NSMutableData new];
    pipe.fileHandleForReading.readabilityHandler=^(NSFileHandle* handle){NSData* data=handle.availableData;if(!data.length)return;dispatch_async(dispatch_get_main_queue(),^{[weakSelf recordOutput:data];});[pending appendData:data];while(true){const auto* bytes=static_cast<const char*>(pending.bytes);const auto* end=static_cast<const char*>(memchr(bytes,'\n',pending.length));if(!end)break;NSData* line=[pending subdataWithRange:NSMakeRange(0,end-bytes)];[pending replaceBytesInRange:NSMakeRange(0,end-bytes+1) withBytes:nullptr length:0];NSDictionary* message=[NSJSONSerialization JSONObjectWithData:line options:0 error:nil];if(message)dispatch_async(dispatch_get_main_queue(),^{[weakSelf receive:message];});}if(pending.length>65536)[pending setLength:0];};
    task.terminationHandler=^(NSTask*){dispatch_async(dispatch_get_main_queue(),^{[weakSelf stopped];});};NSError* error=nil;if(![task launchAndReturnError:&error])throw std::runtime_error(error.localizedDescription.UTF8String);
    start.enabled=NO;stop.enabled=YES;address.enabled=NO;port.enabled=NO;fps.enabled=NO;transport.enabled=NO;pin.stringValue=[NSString stringWithFormat:@"Pairing PIN: %s",state->code.c_str()];fingerprint.stringValue=[NSString stringWithUTF8String:state->fingerprint.c_str()];status.stringValue=@"Preparing desktop sharing…";
  }catch(const std::exception& e){status.stringValue=[NSString stringWithUTF8String:e.what()];}
}
- (void)recordOutput:(NSData*)data {
  [logTail appendData:data];if(logTail.length>65536)[logTail replaceBytesInRange:NSMakeRange(0,logTail.length-65536) withBytes:nullptr length:0];
  if(state)try{remoteos::write_private_file(state->directory/"last-host.log",std::string(static_cast<const char*>(logTail.bytes),logTail.length));}catch(...){}
}
- (void)receive:(NSDictionary*)message {
  NSString* event=message[@"event"];
  if([event isEqual:@"listening"])status.stringValue=@"Ready. Find this Mac in the client host list, then compare the fingerprint and pair with the PIN.";
  else if([event isEqual:@"resource-status"]){NSMutableString* text=[NSMutableString stringWithString:@"Host-only device addresses:"];for(NSDictionary* item in message[@"resources"])[text appendFormat:@"\n%@: %@",item[@"resource"],item[@"url"]];resources.stringValue=text;}
  else if([event isEqual:@"sharing-failed"]){lastFailure=message[@"message"]?:@"Sharing failed.";status.stringValue=lastFailure;}
  else if([event isEqual:@"session-started"])status.stringValue=@"Client connected. Sharing MPEG-2 over UDP.";
}
- (void)stopSharing:(id)sender {stopping=YES;if(task.running)[task terminate];}
- (void)stopped {if(!stopping&&task.terminationStatus!=0)status.stringValue=lastFailure?:[NSString stringWithFormat:@"Host exited (%d). Open host log for the full error.",task.terminationStatus];else if(stopping)status.stringValue=@"Sharing is off.";pipe.fileHandleForReading.readabilityHandler=nil;start.enabled=YES;stop.enabled=NO;address.enabled=YES;port.enabled=YES;fps.enabled=YES;transport.enabled=YES;pin.stringValue=@"Sharing is off.";task=nil;}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)app {[self stopSharing:nil];return NSTerminateNow;}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {return YES;}
@end
int main(int argc,char** argv){@autoreleasepool{
  for(int i=1;i+1<argc;++i)if(std::string(argv[i])=="--startup-check")startup_receipt=std::filesystem::u8path(argv[++i]);
  @try {try {
    remoteos::app::portable_runtime();NSApplication* application=[NSApplication sharedApplication];
    __attribute__((objc_precise_lifetime)) RemoteOSHostWindow* delegate=[RemoteOSHostWindow new];
    application.delegate=delegate;[application run];
  }catch(const std::exception& e){RemoteOSHostWindow* failure=[RemoteOSHostWindow new];[failure launchFailure:[NSString stringWithUTF8String:e.what()]?:@"Startup failed"];return 1;}}
  @catch(NSException* e){RemoteOSHostWindow* failure=[RemoteOSHostWindow new];[failure launchFailure:e.reason?:@"A macOS application error occurred"];return 1;}
}return 0;}
