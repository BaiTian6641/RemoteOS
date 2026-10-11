#pragma once
#include "remoteos/protocol.hpp"
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <pwd.h>
namespace remoteos::sd {
inline std::string value(const std::filesystem::path& p){std::ifstream f(p);std::string v;std::getline(f,v);return v;}
inline bool is_card(const std::string& name){
  if(!std::regex_match(name,std::regex("mmcblk[0-9]+(p[0-9]+)?|sd[a-z]+[0-9]*")))return false;
  auto sys=std::filesystem::path("/sys/class/block")/name;
  if(!std::filesystem::exists(sys))return false;
  if(name.rfind("mmcblk",0)==0){auto parent=std::regex_replace(name,std::regex("p[0-9]+$"),"");return value(std::filesystem::path("/sys/class/block")/parent/"device/type")=="SD";}
  struct stat st{};if(stat(("/dev/"+name).c_str(),&st)<0)return false;
  std::ifstream udev("/run/udev/data/b"+std::to_string(major(st.st_rdev))+":"+std::to_string(minor(st.st_rdev)));std::string line;
  while(std::getline(udev,line))if(line=="E:ID_DRIVE_FLASH_SD=1")return true;
  // USB readers may place the SD identity on the whole disk rather than partition.
  const auto disk=std::regex_replace(name,std::regex("[0-9]+$"),"");return disk!=name&&is_card(disk);
}
inline std::filesystem::path disk(std::filesystem::path path){return std::filesystem::exists(path/"partition")?path.parent_path():path;}
inline bool system_volume(const std::string& name,const std::filesystem::path& classes="/sys/class/block",const std::filesystem::path& mounts="/proc/self/mountinfo",const std::filesystem::path& devices="/sys/dev/block"){
  const auto selected=disk(std::filesystem::canonical(classes/name));
  std::ifstream file(mounts);if(!file)throw std::runtime_error("Cannot identify the system disk");std::string line;
  while(std::getline(file,line)){
    std::istringstream fields(line);std::string id,parent,device,root,target;fields>>id>>parent>>device>>root>>target;
    if(target!="/"&&target!="/state"&&target!="/boot"&&target!="/data")continue;
    if(!std::regex_match(device,std::regex("[0-9]+:[0-9]+")))continue;
    if(std::filesystem::exists(devices/device)&&disk(std::filesystem::canonical(devices/device))==selected)return true;
  }return false;
}
inline remoteos::Json list(){remoteos::Json cards=remoteos::Json::array();for(const auto& entry:std::filesystem::directory_iterator("/sys/class/block")){const auto name=entry.path().filename().string();if(is_card(name)&&!system_volume(name))cards.push_back({{"device",name},{"bytes",std::stoull(value(entry.path()/"size"))*512ULL}});}return {{"ok",true},{"cards",cards}};}
inline remoteos::Json mount_card(const std::string& name,bool writable){
  if(!is_card(name)||system_volume(name))throw std::runtime_error("Select a removable SD card; system disks are protected");
  const auto path="/run/remoteos-sd/"+name;
  std::filesystem::create_directories(path);
  if(std::filesystem::canonical(path)!=path)throw std::runtime_error("Unsafe SD mount point link");
  struct stat st{};if(lstat(path.c_str(),&st)<0||!S_ISDIR(st.st_mode)||st.st_uid!=0)throw std::runtime_error("Unsafe SD mount point");
  auto* user=getpwnam("remoteos");if(!user)throw std::runtime_error("Client account is unavailable");
  // Never remount an externally mounted volume or silently change its access.
  struct stat block{};if(stat(("/dev/"+name).c_str(),&block)<0||!S_ISBLK(block.st_mode))throw std::runtime_error("SD device vanished");
  const auto identity=std::to_string(major(block.st_rdev))+":"+std::to_string(minor(block.st_rdev));
  std::ifstream f("/proc/self/mountinfo");std::string line;
  while(std::getline(f,line)){std::istringstream fields(line);std::string id,parent,device;fields>>id>>parent>>device;if(device==identity)throw std::runtime_error("SD card is already mounted; unmount it before sharing");}
  const auto opts="uid="+std::to_string(user->pw_uid)+",gid="+std::to_string(user->pw_gid)+",fmask=0137,dmask=0027";
  const auto flags=MS_NODEV|MS_NOSUID|MS_NOEXEC|(writable?0:MS_RDONLY);
  bool ok=false;for(const char* fs:{"vfat","exfat"})if(::mount(("/dev/"+name).c_str(),path.c_str(),fs,flags,opts.c_str())==0){ok=true;break;}
  if(!ok)throw std::runtime_error("Cannot mount SD card: v1 supports FAT/FAT32 and exFAT");
  return {{"ok",true},{"path",path}};
}
inline void unmount_all(){const auto base=std::filesystem::path("/run/remoteos-sd");if(!std::filesystem::exists(base))return;for(auto& e:std::filesystem::directory_iterator(base)){if(e.is_directory()&&::umount(e.path().c_str())<0&&errno!=EINVAL)throw std::runtime_error("SD card is busy; cannot unmount safely");}}
}
