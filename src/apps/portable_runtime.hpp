#pragma once
#include <filesystem>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace remoteos::app {
// Executables work when opened directly, without a launcher or installed SDK.
// Only a sibling portable runtime activates this override.
inline void portable_runtime() {
#if defined(_WIN32) || defined(__APPLE__)
  std::filesystem::path executable;
#ifdef _WIN32
  wchar_t name[32768]; const auto n=GetModuleFileNameW(nullptr,name,32768);
  if(!n||n>=32768)return; executable=std::wstring(name,n);
#else
  uint32_t n=0;_NSGetExecutablePath(nullptr,&n);std::string name(n,'\0');
  if(_NSGetExecutablePath(name.data(),&n))return;executable=std::filesystem::weakly_canonical(name.c_str());
#endif
  const auto base=executable.parent_path();
#ifdef _WIN32
  const auto prefix=base.parent_path();
#else
  const auto prefix=base.parent_path()/"Frameworks/GStreamer.framework/Versions/1.0";
#endif
  const auto plugins=prefix/"lib/gstreamer-1.0";
  if(!std::filesystem::is_directory(plugins))return;
#ifdef _WIN32
  // The scanner in libexec inherits this process-only DLL search path.
  const auto* inherited=_wgetenv(L"PATH");
  const auto search=base.wstring()+L";"+(inherited?inherited:L"");
  SetEnvironmentVariableW(L"PATH",search.c_str());
#endif
  auto environment=[](const char* key,const std::filesystem::path& value){
#ifdef _WIN32
    std::wstring wide(key,key+std::char_traits<char>::length(key));SetEnvironmentVariableW(wide.c_str(),value.wstring().c_str());
#else
    setenv(key,value.c_str(),1);
#endif
  };
  environment("GST_PLUGIN_PATH_1_0",plugins);
  environment("GST_PLUGIN_SYSTEM_PATH_1_0",std::filesystem::path(""));
  const auto scanner=prefix/"libexec/gstreamer-1.0"/
#ifdef _WIN32
    "gst-plugin-scanner.exe";
#else
    "gst-plugin-scanner";
#endif
  if(std::filesystem::exists(scanner))environment("GST_PLUGIN_SCANNER_1_0",scanner);
#ifdef _WIN32
  const auto* cache=_wgetenv(L"LOCALAPPDATA");
  if(cache){const auto dir=std::filesystem::path(cache)/"RemoteOS/GStreamer";std::error_code ec;std::filesystem::create_directories(dir,ec);if(!ec)environment("GST_REGISTRY_1_0",dir/"registry-0.1.20.bin");}
#else
  const auto* cache=std::getenv("HOME");
  if(cache){const auto dir=std::filesystem::path(cache)/"Library/Caches/RemoteOS";std::error_code ec;std::filesystem::create_directories(dir,ec);if(!ec)environment("GST_REGISTRY_1_0",dir/"registry-0.1.20.bin");}
#endif
#endif
}
}
