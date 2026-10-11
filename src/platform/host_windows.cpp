#define NOMINMAX
#include "remoteos/host_platform.hpp"
#include "display_config.hpp"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wtsapi32.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace remoteos::platform {
namespace {
template<class T> class Com {
public:
    Com()=default; Com(const Com&)=delete; Com& operator=(const Com&)=delete;
    Com(Com&& rhs) noexcept : p_(rhs.p_) { rhs.p_=nullptr; }
    Com& operator=(Com&& rhs) noexcept { reset(); p_=rhs.p_; rhs.p_=nullptr; return *this; }
    ~Com(){reset();}
    void reset(){if(p_) p_->Release(); p_=nullptr;}
    T* get() const{return p_;} T** out(){reset();return &p_;}
    T* operator->()const{return p_;} explicit operator bool()const{return p_!=nullptr;}
private:T* p_=nullptr;
};
void check(HRESULT hr,const char* what) {
    if(FAILED(hr)) throw std::runtime_error(std::string(what)+" (HRESULT "+std::to_string(static_cast<unsigned long>(hr))+")");
}
std::wstring environment(const std::wstring& name) {
    const DWORD count=GetEnvironmentVariableW(name.c_str(),nullptr,0);
    if(!count) return {};
    std::wstring value(count,L'\0'); GetEnvironmentVariableW(name.c_str(),value.data(),count);
    value.resize(count-1); return value;
}
bool system_identity() {
    HANDLE token=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return false;
    std::vector<unsigned char> storage(1024); DWORD needed=0;
    const BOOL ok=GetTokenInformation(token,TokenUser,storage.data(),static_cast<DWORD>(storage.size()),&needed);
    CloseHandle(token);
    return ok && IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid,WinLocalSystemSid);
}
struct DesktopOutput { std::wstring name; DisplayConfig geometry; bool primary = false; };
std::vector<DesktopOutput> desktop_outputs() {
    std::vector<DesktopOutput> outputs;
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW device{}; device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, i, &device, 0)) break;
        if (!(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ||
            (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)) continue;
        DEVMODEW mode{}; mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode) ||
            mode.dmPelsWidth < 320 || mode.dmPelsHeight < 200) continue;
        outputs.push_back({device.DeviceName, {0, mode.dmPelsWidth, mode.dmPelsHeight,
                           mode.dmPosition.x, mode.dmPosition.y, 30},
                           bool(device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)});
    }
    std::stable_sort(outputs.begin(), outputs.end(), [](const auto& a, const auto& b) { return a.primary > b.primary; });
    if (outputs.size() > 2) outputs.resize(2);
    for (std::size_t i = 0; i < outputs.size(); ++i) outputs[i].geometry.id = unsigned(i);
    return outputs;
}
class Desktop {
public:
    Desktop():original_(GetThreadDesktop(GetCurrentThreadId())){}
    ~Desktop(){if(current_){SetThreadDesktop(original_);CloseDesktop(current_);}}
    bool attach() {
        HDESK next=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS|DESKTOP_WRITEOBJECTS|DESKTOP_SWITCHDESKTOP);
        if(!next) throw std::runtime_error("Cannot open active desktop; secure desktop needs LocalSystem helper");
        wchar_t name[256]{}; DWORD needed=0;
        if(!GetUserObjectInformationW(next,UOI_NAME,name,sizeof(name),&needed)) {
            CloseDesktop(next); throw std::runtime_error("Cannot identify active desktop");
        }
        if(current_ && name_==name) {CloseDesktop(next);return false;}
        if(!SetThreadDesktop(next)){CloseDesktop(next);throw std::runtime_error("Cannot attach thread to active desktop");}
        if(current_) CloseDesktop(current_);
        current_=next; name_=name; return true;
    }
private: HDESK current_=nullptr, original_=nullptr; std::wstring name_;
};

struct ScanCode { WORD code=0; bool extended=false; };
ScanCode scan_code(unsigned key) {
    // Linux evdev and AT set-1 agree for the original PC keyboard keys.
    if(key>=1 && key<=83) return {static_cast<WORD>(key),false};
    if(key==86 || key==87 || key==88) return {static_cast<WORD>(key),false};
    switch(key) {
    case 96:return {0x1c,true}; case 97:return {0x1d,true};
    case 98:return {0x35,true}; case 99:return {0x37,true}; case 100:return {0x38,true};
    case 102:return {0x47,true};case 103:return {0x48,true};case 104:return {0x49,true};
    case 105:return {0x4b,true};case 106:return {0x4d,true};case 107:return {0x4f,true};
    case 108:return {0x50,true};case 109:return {0x51,true};case 110:return {0x52,true};
    case 111:return {0x53,true};case 113:return {0x20,true};case 114:return {0x2e,true};
    case 115:return {0x30,true};case 119:return {0x45,false};
    case 125:return {0x5b,true};case 126:return {0x5c,true};case 127:return {0x5d,true};
    default:throw std::invalid_argument("Key has no supported Windows scan-code mapping");
    }
}
WORD virtual_key(unsigned key) {
    if(key>=183 && key<=194)return static_cast<WORD>(VK_F13+key-183);
    switch(key) {
    case 119:return VK_PAUSE;
    case 128:return VK_BROWSER_STOP;
    case 140:return VK_LAUNCH_APP2;
    case 142:return VK_SLEEP;
    case 150:case 172:return VK_BROWSER_HOME;
    case 155:return VK_LAUNCH_MAIL;
    case 156:case 364:return VK_BROWSER_FAVORITES;
    case 157:return VK_LAUNCH_APP1;
    case 158:return VK_BROWSER_BACK;
    case 159:return VK_BROWSER_FORWARD;
    case 163:return VK_MEDIA_NEXT_TRACK;
    case 164:return VK_MEDIA_PLAY_PAUSE;
    case 165:return VK_MEDIA_PREV_TRACK;
    case 166:return VK_MEDIA_STOP;
    case 173:return VK_BROWSER_REFRESH;
    case 217:return VK_BROWSER_SEARCH;
    case 226:return VK_LAUNCH_MEDIA_SELECT;
    default:return 0;
    }
}
struct Output {
    DisplayConfig config;
    int source_x = 0, source_y = 0;
    unsigned source_width = 0, source_height = 0;
    std::wstring name;
    DEVMODEW original{};
    bool restore=false;
    Com<IDXGIOutput1> output;
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<IDXGIOutputDuplication> duplicate;
    Com<ID3D11Texture2D> staging;
    HDC cursor_dc=nullptr;
    HBITMAP cursor_bitmap=nullptr;
    HGDIOBJ previous_bitmap=nullptr;
    void* cursor_pixels=nullptr;
    ~Output(){
        if(previous_bitmap)SelectObject(cursor_dc,previous_bitmap);
        if(cursor_bitmap)DeleteObject(cursor_bitmap);
        if(cursor_dc)DeleteDC(cursor_dc);
        if(restore) ChangeDisplaySettingsExW(name.c_str(),&original,nullptr,0,nullptr);
    }
};

class WindowsHost final : public HostPlatform {
public:
    explicit WindowsHost(bool desktop = false) : desktop_mode_(desktop) {}
    ~WindowsHost()override{stop();}
    std::vector<DisplayConfig> desktop_displays() const override {
        if (!desktop_mode_) return {};
        std::vector<DisplayConfig> displays;
        for (const auto& output : desktop_outputs()) displays.push_back(output.geometry);
        return displays;
    }
    std::string session_error() const override { std::lock_guard<std::mutex> lock(error_mutex_); return error_; }
    void match_desktop(const std::vector<DisplayConfig>& displays) override {
        detail::validate_displays(displays);
        if (!desktop_mode_ || running_) throw std::runtime_error("Stop application capture before changing monitor resolution");
        const auto outputs = desktop_outputs();
        if (displays.size() > outputs.size()) throw std::runtime_error("Connect another host monitor for the second client screen");
        std::vector<std::pair<std::wstring, DEVMODEW>> modes;
        for (std::size_t i = 0; i < displays.size(); ++i) {
            DEVMODEW original{}; original.dmSize = sizeof(original);
            if (!EnumDisplaySettingsW(outputs[i].name.c_str(), ENUM_CURRENT_SETTINGS, &original)) throw std::runtime_error("Cannot save the current host display mode");
            DEVMODEW requested = original;
            requested.dmPelsWidth = displays[i].width; requested.dmPelsHeight = displays[i].height;
            requested.dmPosition.x = displays[i].x; requested.dmPosition.y = displays[i].y;
            requested.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;
            if (ChangeDisplaySettingsExW(outputs[i].name.c_str(), &requested, nullptr, CDS_TEST, nullptr) != DISP_CHANGE_SUCCESSFUL)
                throw std::runtime_error("The Windows display driver does not expose the client's resolution. Select that mode in Windows display settings, or clear Match host resolution on the HP to use host-side scaling");
            changed_modes_.push_back({outputs[i].name, original});
            modes.push_back({outputs[i].name, requested});
        }
        for (auto& mode : modes) if (ChangeDisplaySettingsExW(mode.first.c_str(), &mode.second, nullptr, 0, nullptr) != DISP_CHANGE_SUCCESSFUL) {
            restore_modes(); throw std::runtime_error("Windows could not match the host display to the client");
        }
    }
    Capabilities capabilities()const override {
        Capabilities c; c.input=true;
        DWORD session=0; ProcessIdToSessionId(GetCurrentProcessId(),&session);
        c.capture=session==WTSGetActiveConsoleSessionId();
        c.virtual_displays=!desktop_mode_ && !environment(L"REMOTEOS_DISPLAY_0").empty() && !environment(L"REMOTEOS_DISPLAY_1").empty();
        if(!c.capture)c.diagnostics.emplace_back("Run host helper in the active console session");
        if(!system_identity())c.diagnostics.emplace_back("Current identity cannot capture Windows secure desktop");
        if (desktop_mode_) c.diagnostics.emplace_back("Application mode shares existing monitors without display/USB drivers; elevated windows and secure desktops require additional privileges");
        else {
            if(!c.virtual_displays)c.diagnostics.emplace_back("Provision signed VDD outputs and REMOTEOS_DISPLAY_0/1 mappings");
            c.diagnostics.emplace_back("Driver presence and login/UAC/lock behavior require Windows hardware qualification");
        }
        std::lock_guard<std::mutex> lock(error_mutex_);
        if(!error_.empty())c.diagnostics.push_back(error_);
        return c;
    }
    void start(const std::vector<DisplayConfig>& displays,FrameCallback callback)override {
        stop_capture();
        detail::validate_displays(displays);
        if(!callback)throw std::invalid_argument("Capture callback required");
        try {
            for(std::size_t i=0;i<displays.size();++i) {
                const auto& config=displays[i];
                auto output=std::make_unique<Output>();output->config=config;
                output->source_x = config.x; output->source_y = config.y;
                output->source_width = config.width; output->source_height = config.height;
                if (desktop_mode_) {
                    const auto physical = desktop_outputs();
                    if (i >= physical.size()) throw std::runtime_error("The selected host monitor is no longer connected");
                    output->name = physical[i].name;
                    const auto& geometry = physical[i].geometry;
                    output->source_x = geometry.x; output->source_y = geometry.y;
                    output->source_width = geometry.width; output->source_height = geometry.height;
                    if (geometry.width > 3840 || geometry.height > 2160)
                        throw std::runtime_error("Application capture supports source monitors up to 3840x2160");
                    initialize(*output);
                    outputs_.push_back(std::move(output));
                    continue;
                }
                output->name=environment(L"REMOTEOS_DISPLAY_"+std::to_wstring(i));
                if(output->name.empty())throw std::runtime_error("Explicit signed virtual-output mapping required");
                // Never silently capture a physical monitor when a requested VDD
                // output is missing. Installation controls these exact mappings.
                DISPLAY_DEVICEW device{}; device.cb=sizeof(device); bool virtual_output=false;
                for(DWORD n=0;EnumDisplayDevicesW(nullptr,n,&device,0);++n) {
                    if(output->name==device.DeviceName) {
                        const std::wstring label=device.DeviceString;
                        virtual_output=label.find(L"Virtual Display")!=std::wstring::npos || label.find(L"IddSample")!=std::wstring::npos;
                        break;
                    }
                    device={};device.cb=sizeof(device);
                }
                if(!virtual_output)throw std::runtime_error("Mapped output is not a recognized Virtual Display Driver adapter");
                output->original.dmSize=sizeof(DEVMODEW);
                if(!EnumDisplaySettingsW(output->name.c_str(),ENUM_CURRENT_SETTINGS,&output->original))throw std::runtime_error("Cannot save display mode");
                DEVMODEW mode=output->original;mode.dmPelsWidth=config.width;mode.dmPelsHeight=config.height;
                mode.dmPosition.x=config.x;mode.dmPosition.y=config.y;mode.dmDisplayFrequency=60;mode.dmBitsPerPel=32;
                mode.dmFields=DM_PELSWIDTH|DM_PELSHEIGHT|DM_POSITION|DM_DISPLAYFREQUENCY|DM_BITSPERPEL;
                if(ChangeDisplaySettingsExW(output->name.c_str(),&mode,nullptr,CDS_TEST,nullptr)!=DISP_CHANGE_SUCCESSFUL)throw std::runtime_error("Signed driver does not expose requested mode");
                output->restore=true;
                if(ChangeDisplaySettingsExW(output->name.c_str(),&mode,nullptr,0,nullptr)!=DISP_CHANGE_SUCCESSFUL)throw std::runtime_error("Cannot apply virtual-display layout");
                initialize(*output);
                outputs_.push_back(std::move(output));
            }
            callback_=std::move(callback);running_=true;worker_=std::thread([this]{capture();});
        }catch(...){stop();throw;}
    }
    void stop()noexcept override {
        stop_capture(); restore_modes();
    }
    void stop_capture()noexcept {
        running_=false;if(worker_.joinable())worker_.join();release_all();outputs_.clear();callback_={};
    }
    void restore_modes()noexcept {
        for (auto& mode : changed_modes_)
            if (ChangeDisplaySettingsExW(mode.first.c_str(), &mode.second, nullptr, 0, nullptr) != DISP_CHANGE_SUCCESSFUL) {
                std::lock_guard<std::mutex> lock(error_mutex_); error_ = "Windows could not restore the previous display mode";
            }
        changed_modes_.clear();
    }
    void inject(const InputEvent& e)override {
        input_desktop_.attach();
        if(e.kind==InputKind::secure_attention) {
            // Only the SCM service calls SendSAS; a LocalSystem child process
            // alone does not satisfy SendSAS's service/uiAccess requirements.
            if(!system_identity())throw std::runtime_error("Secure attention requires the LocalSystem broker");
            DWORD read=0; char reply=0; const char command='S';
            if(!CallNamedPipeW(L"\\\\.\\pipe\\RemoteOS.SAS",const_cast<char*>(&command),1,&reply,1,&read,1000) || read!=1 || reply!='O')
                throw std::runtime_error("Secure-attention broker unavailable or software SAS policy denied");
            return;
        }
        INPUT input{};
        switch(e.kind) {
        case InputKind::key: {
            if(e.repeat&&!e.down)throw std::invalid_argument("Repeat is valid only for key-down events");
            if(const auto key=virtual_key(e.key)){input.type=INPUT_KEYBOARD;input.ki.wVk=key;input.ki.dwFlags=e.down?0:KEYEVENTF_KEYUP;break;}
            const auto scan=scan_code(e.key);input.type=INPUT_KEYBOARD;input.ki.wScan=scan.code;
            input.ki.dwFlags=KEYEVENTF_SCANCODE|(scan.extended?KEYEVENTF_EXTENDEDKEY:0)|(e.down?0:KEYEVENTF_KEYUP);break;
        }
        case InputKind::button:
            input.type=INPUT_MOUSE;
            if(e.key==0x110)input.mi.dwFlags=e.down?MOUSEEVENTF_LEFTDOWN:MOUSEEVENTF_LEFTUP;
            else if(e.key==0x111)input.mi.dwFlags=e.down?MOUSEEVENTF_RIGHTDOWN:MOUSEEVENTF_RIGHTUP;
            else if(e.key==0x112)input.mi.dwFlags=e.down?MOUSEEVENTF_MIDDLEDOWN:MOUSEEVENTF_MIDDLEUP;
            else if(e.key==0x113||e.key==0x116){input.mi.dwFlags=e.down?MOUSEEVENTF_XDOWN:MOUSEEVENTF_XUP;input.mi.mouseData=XBUTTON1;}
            else if(e.key==0x114||e.key==0x115){input.mi.dwFlags=e.down?MOUSEEVENTF_XDOWN:MOUSEEVENTF_XUP;input.mi.mouseData=XBUTTON2;}
            else throw std::invalid_argument("Unsupported mouse button");
            break;
        case InputKind::absolute_pointer: {
            int px = e.x, py = e.y;
            if (desktop_mode_) {
                bool mapped = false;
                for (const auto& output : outputs_) {
                    const auto& d = output->config;
                    if (px < d.x || px >= d.x + int(d.width) || py < d.y || py >= d.y + int(d.height)) continue;
                    px = output->source_x + int(std::int64_t(px - d.x) * (output->source_width - 1) / std::max(1u, d.width - 1));
                    py = output->source_y + int(std::int64_t(py - d.y) * (output->source_height - 1) / std::max(1u, d.height - 1));
                    mapped = true; break;
                }
                if (!mapped) throw std::runtime_error("Pointer is outside the shared monitor layout");
            }
            const int width=GetSystemMetrics(SM_CXVIRTUALSCREEN),height=GetSystemMetrics(SM_CYVIRTUALSCREEN);
            input.type=INPUT_MOUSE;input.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
            input.mi.dx=static_cast<LONG>(std::clamp<std::int64_t>(static_cast<std::int64_t>(px-GetSystemMetrics(SM_XVIRTUALSCREEN))*65535/std::max(1,width-1),0,65535));
            input.mi.dy=static_cast<LONG>(std::clamp<std::int64_t>(static_cast<std::int64_t>(py-GetSystemMetrics(SM_YVIRTUALSCREEN))*65535/std::max(1,height-1),0,65535));break;
        }
        case InputKind::relative_pointer:input.type=INPUT_MOUSE;input.mi.dwFlags=MOUSEEVENTF_MOVE;input.mi.dx=e.delta_x;input.mi.dy=e.delta_y;break;
        case InputKind::wheel:
            input.type=INPUT_MOUSE;
            if(e.delta_y){input.mi.dwFlags=MOUSEEVENTF_WHEEL;input.mi.mouseData=e.delta_y*WHEEL_DELTA;send(input);}
            if(e.delta_x){input.mi.dwFlags=MOUSEEVENTF_HWHEEL;input.mi.mouseData=e.delta_x*WHEEL_DELTA;send(input);}return;
        default:throw std::invalid_argument("Unsupported input event");
        }
        send(input);
        if(e.kind==InputKind::key||e.kind==InputKind::button) {
            if(e.down)held_.insert(e.key);else held_.erase(e.key);
        }
    }
    void release_all()noexcept override {
        const auto held=held_;
        for(auto key:held){try{InputEvent e;e.kind=key>=0x110&&key<=0x117?InputKind::button:InputKind::key;e.key=key;inject(e);}catch(...){}}
        held_.clear();
    }
private:
    static void send(INPUT& input){if(SendInput(1,&input,sizeof(input))!=1)throw std::runtime_error("SendInput failed (desktop privilege or session mismatch)");}
    static void cursor(Output& output,VideoFrame& frame) {
        CURSORINFO state{};state.cbSize=sizeof(state);
        if(!GetCursorInfo(&state)||!(state.flags&CURSOR_SHOWING))return;
        ICONINFO icon{};if(!GetIconInfo(state.hCursor,&icon))return;
        const int x=state.ptScreenPos.x-output.source_x-static_cast<int>(icon.xHotspot);
        const int y=state.ptScreenPos.y-output.source_y-static_cast<int>(icon.yHotspot);
        if(icon.hbmMask)DeleteObject(icon.hbmMask);
        if(icon.hbmColor)DeleteObject(icon.hbmColor);
        if(!output.cursor_dc) {
            output.cursor_dc=CreateCompatibleDC(nullptr);
            BITMAPINFO bitmap{};bitmap.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth=frame.width;bitmap.bmiHeader.biHeight=-static_cast<LONG>(frame.height);
            bitmap.bmiHeader.biPlanes=1;bitmap.bmiHeader.biBitCount=32;bitmap.bmiHeader.biCompression=BI_RGB;
            output.cursor_bitmap=CreateDIBSection(output.cursor_dc,&bitmap,DIB_RGB_COLORS,&output.cursor_pixels,nullptr,0);
            if(!output.cursor_bitmap||!output.cursor_pixels)throw std::runtime_error("Cannot allocate cursor composition surface");
            output.previous_bitmap=SelectObject(output.cursor_dc,output.cursor_bitmap);
        }
        std::memcpy(output.cursor_pixels,frame.bgra.data(),frame.bgra.size());
        if(DrawIconEx(output.cursor_dc,x,y,state.hCursor,0,0,0,nullptr,DI_NORMAL)) {
            GdiFlush();std::memcpy(frame.bgra.data(),output.cursor_pixels,frame.bgra.size());
        }
    }
    static void initialize(Output& target) {
        target.duplicate.reset();target.staging.reset();target.context.reset();target.device.reset();target.output.reset();
        Com<IDXGIFactory1> factory;check(CreateDXGIFactory1(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(factory.out())),"CreateDXGIFactory1");
        for(UINT a=0;;++a) {
            Com<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(a,adapter.out())==DXGI_ERROR_NOT_FOUND)break;
            for(UINT o=0;;++o) {
                Com<IDXGIOutput> output;if(adapter->EnumOutputs(o,output.out())==DXGI_ERROR_NOT_FOUND)break;
                DXGI_OUTPUT_DESC desc{};check(output->GetDesc(&desc),"Get output description");
                if(target.name!=desc.DeviceName)continue;
                if(desc.Rotation!=DXGI_MODE_ROTATION_IDENTITY)throw std::runtime_error("Rotated displays are not supported");
                check(output->QueryInterface(__uuidof(IDXGIOutput1),reinterpret_cast<void**>(target.output.out())),"Query output");
                check(D3D11CreateDevice(adapter.get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,target.device.out(),nullptr,target.context.out()),"Create capture device");
                return;
            }
        }
        throw std::runtime_error("Configured virtual output not present in DXGI");
    }
    void capture()noexcept {
        try {
            Desktop desktop;
            std::vector<std::chrono::steady_clock::time_point> next(outputs_.size());
            while(running_) {
                try {
                    if(desktop.attach())for(auto& d:outputs_)d->duplicate.reset();
                } catch(const std::exception& e) {
                    {std::lock_guard<std::mutex> lock(error_mutex_);error_=e.what();}
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));continue;
                }
                for(std::size_t i=0;i<outputs_.size();++i) {
                    auto& d=*outputs_[i];const auto now=std::chrono::steady_clock::now();
                    if(now<next[i])continue;
                    if(!d.duplicate) {
                        const HRESULT hr=d.output->DuplicateOutput(d.device.get(),d.duplicate.out());
                        if(hr==E_ACCESSDENIED||hr==DXGI_ERROR_SESSION_DISCONNECTED||hr==DXGI_ERROR_UNSUPPORTED) {
                            std::lock_guard<std::mutex> lock(error_mutex_);error_="Capture waits for accessible active desktop";continue;
                        }
                        check(hr,"Duplicate virtual output");
                    }
                    DXGI_OUTDUPL_FRAME_INFO info{};Com<IDXGIResource> resource;
                    const HRESULT hr=d.duplicate->AcquireNextFrame(1,&info,resource.out());
                    if(hr==DXGI_ERROR_WAIT_TIMEOUT)continue;
                    if(hr==DXGI_ERROR_ACCESS_LOST){d.duplicate.reset();continue;}
                    check(hr,"Acquire frame");
                    try {
                        Com<ID3D11Texture2D> texture;check(resource->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(texture.out())),"Query texture");
                        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
                        if(desc.Width!=d.source_width||desc.Height!=d.source_height||desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM)
                            throw std::runtime_error("Virtual display changed to an unsupported mode");
                        if(!d.staging) {
                            desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
                            check(d.device->CreateTexture2D(&desc,nullptr,d.staging.out()),"Create staging texture");
                        }
                        d.context->CopyResource(d.staging.get(),texture.get());
                        D3D11_MAPPED_SUBRESOURCE mapped{};check(d.context->Map(d.staging.get(),0,D3D11_MAP_READ,0,&mapped),"Map frame");
                        VideoFrame frame;frame.display_id=d.config.id;frame.width=desc.Width;frame.height=desc.Height;
                        frame.stride=desc.Width*4;frame.timestamp=now;
                        try {
                            frame.bgra.resize(static_cast<std::size_t>(frame.stride)*frame.height);
                            for(unsigned y=0;y<frame.height;++y)std::memcpy(frame.bgra.data()+y*frame.stride,static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch,frame.stride);
                        }catch(...){d.context->Unmap(d.staging.get(),0);throw;}
                        d.context->Unmap(d.staging.get(),0);
                        if(info.PointerPosition.Visible)cursor(d,frame);
                        callback_(std::move(frame));
                        {std::lock_guard<std::mutex> lock(error_mutex_);error_.clear();}
                    }catch(...){d.duplicate->ReleaseFrame();throw;}
                    d.duplicate->ReleaseFrame();next[i]=detail::next_capture(now,next[i],d.config.fps);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }catch(const std::exception& e){std::lock_guard<std::mutex> lock(error_mutex_);error_=e.what();running_=false;}
        catch(...){std::lock_guard<std::mutex> lock(error_mutex_);error_="Capture callback failed";running_=false;}
    }
    std::vector<std::unique_ptr<Output>> outputs_;
    bool desktop_mode_ = false;
    std::vector<std::pair<std::wstring, DEVMODEW>> changed_modes_;
    FrameCallback callback_;
    std::atomic<bool> running_{false};std::thread worker_;
    std::set<std::uint16_t> held_;Desktop input_desktop_;
    mutable std::mutex error_mutex_;std::string error_;
};
} // namespace
std::unique_ptr<HostPlatform> make_host_platform(){return std::make_unique<WindowsHost>();}
std::unique_ptr<HostPlatform> make_desktop_host_platform(){return std::make_unique<WindowsHost>(true);}
} // namespace remoteos::platform
