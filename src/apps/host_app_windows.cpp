#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include "host_app_state.hpp"
#include "portable_runtime.hpp"
#include <chrono>
#include <vector>

namespace {
std::wstring quote(const std::wstring& value) {
  std::wstring result = L"\"";
  unsigned slashes = 0;
  for (auto ch : value) {
    if (ch == L'\\') { ++slashes; continue; }
    if (ch == L'"') result.append(slashes * 2 + 1, L'\\');
    else result.append(slashes, L'\\');
    slashes = 0; result += ch;
  }
  result.append(slashes * 2, L'\\'); result += L'"'; return result;
}
std::wstring widen(const std::string& value) {
  const auto size = MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0);
  std::wstring result(size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), result.data(), size);
  return result;
}
class HostWindow {
public:
  explicit HostWindow(remoteos::app::HostState state) : state_(std::move(state)) {}
  ~HostWindow() { cleanup(); if (font_) DeleteObject(font_); if (background_) DeleteObject(background_); }
  void open(HINSTANCE instance) {
    WNDCLASSW type{}; type.lpfnWndProc = procedure; type.hInstance = instance;
    type.lpszClassName = L"RemoteOS.Host.Application"; type.hCursor = LoadCursor(nullptr, IDC_ARROW);
    background_ = CreateSolidBrush(RGB(22,22,22)); type.hbrBackground = background_;
    RegisterClassW(&type);
    window_ = CreateWindowExW(0, type.lpszClassName, L"RemoteOS Host", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT,CW_USEDEFAULT,760,890,nullptr,nullptr,instance,this);
    if (!window_) throw std::runtime_error("Cannot create the host window");
    font_ = CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"IBM Plex Sans");
    int y = 20;
    label(L"RemoteOS Host — Share your desktop", 24,y,690,32); y += 42;
    label(L"Keep this app open. It shares existing monitors without installing display, input or USB drivers.",24,y,690,48); y += 58;
    label(L"Listen address",24,y,150,24); bind_ = control(L"EDIT",L"0.0.0.0",WS_BORDER|ES_AUTOHSCROLL,190,y,310,28,0); y += 40;
    label(L"Control port",24,y,150,24); port_ = control(L"EDIT",L"47010",WS_BORDER|ES_NUMBER,190,y,150,28,0); y += 40;
    audio_ = control(L"BUTTON",L"Share playback audio",BS_AUTOCHECKBOX,24,y,650,28,0);
    SendMessageW(audio_,BM_SETCHECK,BST_CHECKED,0); y += 42;
    label(L"Frame rate",24,y,150,24); fps_=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL,190,y,220,140,0);
    for(const auto* value:{L"15 fps",L"20 fps",L"30 fps",L"60 fps"})SendMessageW(fps_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(value));SendMessageW(fps_,CB_SETCURSEL,2,0);y+=40;
    label(L"Media",24,y,150,24); transport_=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST,190,y,450,100,0);
    SendMessageW(transport_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Trusted LAN / Tailscale UDP"));SendMessageW(transport_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Encrypted UDP"));SendMessageW(transport_,CB_SETCURSEL,0,0);y+=40;
    label(L"Find this PC in the HP's host list, or use its IP address or Tailscale name.",24,y,690,28); y += 38;
    label(L"Verification value — compare on the HP",24,y,690,24); y += 28;
    verification_ = control(L"EDIT",L"—",WS_BORDER|ES_READONLY|ES_AUTOHSCROLL,24,y,690,30,0); y += 44;
    label(L"Verified host fingerprint",24,y,690,24); y += 28;
    fingerprint_ = control(L"EDIT",L"Start sharing to create the host identity.",WS_BORDER|ES_READONLY|ES_AUTOHSCROLL,24,y,690,30,0); y += 44;
    label(L"Six-digit pairing PIN — one use, expires after 10 minutes",24,y,690,24); y += 28;
    code_ = control(L"EDIT",L"—",WS_BORDER|ES_READONLY|ES_AUTOHSCROLL,24,y,690,30,0); y += 46;
    start_ = control(L"BUTTON",L"Start sharing",BS_PUSHBUTTON,24,y,320,40,1);
    stop_ = control(L"BUTTON",L"Stop sharing",BS_PUSHBUTTON,365,y,349,40,2);
    EnableWindow(stop_,FALSE); y += 54;
    status_ = label(L"Sharing is off.",24,y,690,40); y += 44;
    resources_=control(L"EDIT",L"Webcam: add its host address as an OBS Browser Source, then Start Virtual Camera. SD: open its WebDAV address.",WS_BORDER|ES_READONLY|ES_MULTILINE|ES_AUTOVSCROLL,24,y,690,92,0);y+=102;
    label(L"Stay logged in. UAC/secure desktops and higher-privilege windows are outside ordinary application access.",24,y,690,45);
    ShowWindow(window_,SW_SHOW); UpdateWindow(window_);
    SetTimer(window_,1,250,nullptr);
  }
  HWND handle() const { return window_; }
private:
  HWND control(const wchar_t *type,const wchar_t *text,DWORD style,int x,int y,int w,int h,int id) {
    auto handle = CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,x,y,w,h,window_,
                                  reinterpret_cast<HMENU>(std::intptr_t(id)),nullptr,nullptr);
    SendMessageW(handle,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE); return handle;
  }
  HWND label(const wchar_t *text,int x,int y,int w,int h) { return control(L"STATIC",text,0,x,y,w,h,0); }
  static std::string text(HWND edit) {
    const int count=GetWindowTextLengthW(edit);
    std::wstring wide(count+1,L'\0'); GetWindowTextW(edit,wide.data(),count+1); wide.resize(count);
    const int bytes=WideCharToMultiByte(CP_UTF8,0,wide.data(),count,nullptr,0,nullptr,nullptr);
    std::string result(bytes,'\0'); WideCharToMultiByte(CP_UTF8,0,wide.data(),count,result.data(),bytes,nullptr,nullptr); return result;
  }
  void message(const std::string& value) { const auto wide=widen(value); SetWindowTextW(status_,wide.c_str()); }
  void start() {
    if (process_) return;
    try {
      static const unsigned rates[]={15,20,30,60};
      const auto config=state_.prepare(text(bind_),remoteos::app::parse_port(text(port_)),SendMessageW(audio_,BM_GETCHECK,0,0)==BST_CHECKED,"","monitors",1366,768,rates[SendMessageW(fps_,CB_GETCURSEL,0,0)],SendMessageW(transport_,CB_GETCURSEL,0,0)==0?"rtp-udp":"dtls-srtp");
      auto command=quote(state_.executable.wstring())+L" --desktop --config "+quote(config.wstring());
      STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION info{};
      if (!CreateProcessW(state_.executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED,
                          nullptr,state_.executable.parent_path().c_str(),&startup,&info))
        throw std::runtime_error("Cannot start remoteos-host.exe; check its runtime DLLs");
      process_=info.hProcess;
      const auto name=L"Local\\RemoteOS.Stop."+std::to_wstring(info.dwProcessId);
      stop_event_=CreateEventW(nullptr,TRUE,FALSE,name.c_str());
      if (!stop_event_) { TerminateProcess(process_,1); CloseHandle(info.hThread); cleanup(); throw std::runtime_error("Cannot create the host stop event"); }
      ResumeThread(info.hThread); CloseHandle(info.hThread);
      const auto fingerprint=widen(state_.fingerprint),code=widen(state_.code);
      SetWindowTextW(fingerprint_,fingerprint.c_str()); SetWindowTextW(code_,code.c_str());
      const auto fp = state_.fingerprint.substr(0, 12);
      SetWindowTextW(verification_, widen(fp.substr(0, 4) + " " + fp.substr(4, 4) + " " + fp.substr(8, 4)).c_str());
      EnableWindow(start_,FALSE); EnableWindow(stop_,TRUE); EnableWindow(bind_,FALSE); EnableWindow(port_,FALSE); EnableWindow(audio_,FALSE);
      stopping_=false;
      message("Sharing MPEG-2 with a 40 Mbps connection limit. Compare the verification value, then enter this PIN on the HP. Discovery uses UDP 47009; media uses UDP 47020–47022.");
    } catch(const std::exception& error) { message(error.what()); }
  }
  void stop() {
    if (!process_||stopping_) return;
    stopping_=true; stopped_at_=std::chrono::steady_clock::now();
    SetEvent(stop_event_); message("Stopping sharing and releasing remote input…");
  }
  void poll() {
    if (!process_) return;
    try{const auto path=state_.directory/"resources.json";const auto modified=std::filesystem::last_write_time(path);if(modified!=resources_time_){resources_time_=modified;const auto list=remoteos::Json::parse(remoteos::read_private_file(path));std::string text="Host-only device addresses:";for(const auto& item:list)text+="\r\n"+item.value("resource",std::string{})+": "+item.value("url",std::string{});SetWindowTextW(resources_,widen(text).c_str());}}catch(...){}
    if (stopping_&&std::chrono::steady_clock::now()-stopped_at_>std::chrono::seconds(5)) TerminateProcess(process_,1);
    if (WaitForSingleObject(process_,0)==WAIT_OBJECT_0) {
      DWORD code=0; GetExitCodeProcess(process_,&code); const bool normal=stopping_;
      cleanup(); EnableWindow(start_,TRUE); EnableWindow(stop_,FALSE); EnableWindow(bind_,TRUE); EnableWindow(port_,TRUE); EnableWindow(audio_,TRUE);
      SetWindowTextW(code_,L"—");
      message(normal?"Sharing is off.":"The host exited (code "+std::to_string(code)+"). Check desktop access, the listen address and runtime dependencies.");
      if (quitting_) DestroyWindow(window_);
    }
  }
  void cleanup() { if(process_){CloseHandle(process_);process_=nullptr;} if(stop_event_){CloseHandle(stop_event_);stop_event_=nullptr;} }
  static LRESULT CALLBACK procedure(HWND window,UINT event,WPARAM w,LPARAM l) {
    auto *self=reinterpret_cast<HostWindow*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(event==WM_NCCREATE) { self=static_cast<HostWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
    if(!self) return DefWindowProcW(window,event,w,l);
    if(event==WM_COMMAND&&HIWORD(w)==BN_CLICKED) { if(LOWORD(w)==1)self->start();else if(LOWORD(w)==2)self->stop();return 0; }
    if(event==WM_TIMER) { self->poll();return 0; }
    if(event==WM_CLOSE) { if(self->process_){self->quitting_=true;self->stop();}else DestroyWindow(window);return 0; }
    if(event==WM_DESTROY) { PostQuitMessage(0);return 0; }
    if(event==WM_CTLCOLORSTATIC) { SetTextColor(reinterpret_cast<HDC>(w),RGB(244,244,244)); SetBkColor(reinterpret_cast<HDC>(w),RGB(22,22,22)); return reinterpret_cast<LRESULT>(self->background_); }
    return DefWindowProcW(window,event,w,l);
  }
  remoteos::app::HostState state_;
  HWND window_{},bind_{},port_{},audio_{},fps_{},transport_{},fingerprint_{},verification_{},code_{},start_{},stop_{},status_{},resources_{};
  std::filesystem::file_time_type resources_time_{};
  HANDLE process_=nullptr,stop_event_=nullptr; HFONT font_=nullptr; HBRUSH background_=nullptr;
  bool stopping_=false,quitting_=false; std::chrono::steady_clock::time_point stopped_at_{};
};
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int) {
  remoteos::app::portable_runtime();
  try {
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768);
    const auto host=std::filesystem::path(executable).parent_path()/L"remoteos-host.exe";
    HostWindow app{remoteos::app::HostState(host)}; app.open(instance);
    MSG message{}; while(GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(app.handle(),&message)){TranslateMessage(&message);DispatchMessageW(&message);}} return 0;
  } catch(const std::exception& error){const auto message=widen(error.what());MessageBoxW(nullptr,message.c_str(),L"RemoteOS Host",MB_OK|MB_ICONERROR);return 1;}
}
