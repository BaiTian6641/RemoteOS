#import <Cocoa/Cocoa.h>
#include "host_app_state.hpp"
#include "portable_runtime.hpp"
#include <cstring>
#include <memory>
@interface RemoteOSHostWindow : NSObject <NSApplicationDelegate>
@end
@implementation RemoteOSHostWindow {
  NSWindow* window;NSTextField *address,*port,*status,*pin,*fingerprint,*resources;
  NSPopUpButton *fps,*transport;NSButton *start,*stop;NSTask* task;NSPipe* pipe;
  std::unique_ptr<remoteos::app::HostState> state;
}
- (NSTextField*)label:(NSString*)text x:(CGFloat)x y:(CGFloat)y width:(CGFloat)width {
  NSTextField* field=[NSTextField labelWithString:text];field.frame=NSMakeRect(x,y,width,44);field.textColor=[NSColor colorWithWhite:0.96 alpha:1];field.font=[NSFont fontWithName:@"IBM Plex Sans" size:16]?:[NSFont systemFontOfSize:16];field.selectable=YES;field.maximumNumberOfLines=3;[window.contentView addSubview:field];return field;
}
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
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
  [window center];[window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];
}
- (void)startSharing:(id)sender {
  if(task.running)return;
  try {
    static const unsigned rates[]={15,20,30,60};auto config=state->prepare(address.stringValue.UTF8String,remoteos::app::parse_port(port.stringValue.UTF8String),false,"","monitors",1366,768,rates[fps.indexOfSelectedItem],transport.indexOfSelectedItem==0?"rtp-udp":"dtls-srtp");
    task=[NSTask new];task.executableURL=[NSURL fileURLWithPath:[NSString stringWithUTF8String:state->executable.c_str()]];task.arguments=@[@"--desktop",@"--config",[NSString stringWithUTF8String:config.c_str()]];pipe=[NSPipe pipe];task.standardOutput=pipe;task.standardError=pipe;
    __weak RemoteOSHostWindow* weakSelf=self;__block NSMutableData* pending=[NSMutableData new];
    pipe.fileHandleForReading.readabilityHandler=^(NSFileHandle* handle){NSData* data=handle.availableData;if(!data.length)return;[pending appendData:data];while(true){const auto* bytes=static_cast<const char*>(pending.bytes);const auto* end=static_cast<const char*>(memchr(bytes,'\n',pending.length));if(!end)break;NSData* line=[pending subdataWithRange:NSMakeRange(0,end-bytes)];[pending replaceBytesInRange:NSMakeRange(0,end-bytes+1) withBytes:nullptr length:0];NSDictionary* message=[NSJSONSerialization JSONObjectWithData:line options:0 error:nil];if(message)dispatch_async(dispatch_get_main_queue(),^{[weakSelf receive:message];});}if(pending.length>65536)[pending setLength:0];};
    task.terminationHandler=^(NSTask*){dispatch_async(dispatch_get_main_queue(),^{[weakSelf stopped];});};NSError* error=nil;if(![task launchAndReturnError:&error])throw std::runtime_error(error.localizedDescription.UTF8String);
    start.enabled=NO;stop.enabled=YES;address.enabled=NO;port.enabled=NO;fps.enabled=NO;transport.enabled=NO;pin.stringValue=[NSString stringWithFormat:@"Pairing PIN: %s",state->code.c_str()];fingerprint.stringValue=[NSString stringWithUTF8String:state->fingerprint.c_str()];status.stringValue=@"Preparing desktop sharing…";
  }catch(const std::exception& e){status.stringValue=[NSString stringWithUTF8String:e.what()];}
}
- (void)receive:(NSDictionary*)message {
  NSString* event=message[@"event"];
  if([event isEqual:@"listening"])status.stringValue=@"Ready. Find this Mac in the client host list, then compare the fingerprint and pair with the PIN.";
  else if([event isEqual:@"resource-status"]){NSMutableString* text=[NSMutableString stringWithString:@"Host-only device addresses:"];for(NSDictionary* item in message[@"resources"])[text appendFormat:@"\n%@: %@",item[@"resource"],item[@"url"]];resources.stringValue=text;}
  else if([event isEqual:@"sharing-failed"])status.stringValue=message[@"message"]?:@"Sharing failed.";
  else if([event isEqual:@"session-started"])status.stringValue=@"Client connected. Sharing MPEG-2 over UDP.";
}
- (void)stopSharing:(id)sender {if(task.running)[task terminate];}
- (void)stopped {pipe.fileHandleForReading.readabilityHandler=nil;start.enabled=YES;stop.enabled=NO;address.enabled=YES;port.enabled=YES;fps.enabled=YES;transport.enabled=YES;pin.stringValue=@"Sharing is off.";task=nil;}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)app {[self stopSharing:nil];return NSTerminateNow;}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {return YES;}
@end
int main(int argc,char** argv){remoteos::app::portable_runtime();@autoreleasepool{NSApplication* application=[NSApplication sharedApplication];RemoteOSHostWindow* delegate=[RemoteOSHostWindow new];application.delegate=delegate;[application run];}return 0;}
