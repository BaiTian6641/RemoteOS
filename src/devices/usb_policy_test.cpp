#include "remoteos/devices.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace fs=std::filesystem;
using namespace remoteos::devices;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void write(const fs::path& path,const std::string& value){fs::create_directories(path.parent_path());std::ofstream(path)<<value<<'\n';}
struct Fixture {
    fs::path dir;
    Fixture(){char pattern[]="/tmp/remoteos-usb-test-XXXXXX";const auto result=mkdtemp(pattern);if(!result)throw std::runtime_error("mkdtemp");dir=result;fs::create_directories(dir/"sys/bus/usb/devices");write(dir/"mountinfo","");}
    ~Fixture(){std::error_code error;fs::remove_all(dir,error);}
    fs::path device(const std::string& bus,unsigned type){
        const auto path=dir/"sys/devices"/bus;fs::create_directories(path);
        write(path/"idVendor","1234");write(path/"idProduct","5678");write(path/"serial","approved");
        char value[3];snprintf(value,sizeof(value),"%02x",type);write(path/"bDeviceClass",value);
        fs::create_directory_symlink(path,dir/"sys/bus/usb/devices"/bus);return path;
    }
    UsbDevice inspect(const UsbPolicy& policy){auto all=enumerate_usb(policy,(dir/"sys").string(),(dir/"mountinfo").string());require(all.size()==1,"one device expected");return all.front();}
};
}
int main(){
    try {
        require(valid_bus_id("1-2.3"),"valid bus id rejected");
        for(const auto& bad:{"usb1","1-2:1.0","../1-2","1-2;reboot","0-1","1-0","-1-2","1-2\n"})require(!valid_bus_id(bad),"unsafe bus id accepted");
        UsbPolicy approved{{{"1234","5678","approved"}}, {}};
        {
            Fixture f;f.device("1-2",0x0e);require(f.inspect(approved).eligible,"approved camera rejected");
            require(!f.inspect({}).eligible,"unapproved camera accepted");
            auto other=approved;other.approved[0].serial="different";require(!f.inspect(other).eligible,"wrong serial accepted");
        }
        for(unsigned type:{3U,9U,2U,10U,0xe0U}){
            Fixture f;f.device("1-2",type);require(!f.inspect(approved).eligible,"HID/hub/network accepted");
        }
        {
            Fixture f;auto d=f.device("1-2",0xff);write(d/"interface/net/wlan0/ifindex","2");
            require(!f.inspect(approved).eligible,"vendor-class network device accepted");
        }
        {
            Fixture f;auto d=f.device("1-2",0);write(d/"interface/bInterfaceClass","03");
            require(!f.inspect(approved).eligible,"composite HID accepted");
        }
        {
            Fixture f;auto d=f.device("1-2",8);write(d/"block/sda/dev","8:0");
            require(!f.inspect(approved).eligible,"storage accepted without boot exclusion");
            fs::create_directories(f.dir/"sys/class/block");
            fs::create_directory_symlink(d/"block/sda",f.dir/"sys/class/block/sda");
            auto boot=approved;boot.protected_block_devices={"/dev/sda"};
            require(!f.inspect(boot).eligible,"boot disk accepted");
            fs::create_directories(f.dir/"sys/devices/internal/block/sdb");
            fs::create_directory_symlink(f.dir/"sys/devices/internal/block/sdb",f.dir/"sys/class/block/sdb");
            boot.protected_block_devices={"/dev/sdb"};
            require(f.inspect(boot).eligible,"unmounted approved non-boot storage rejected");
            write(f.dir/"mountinfo","23 22 8:0 / /media/test rw - ext4 /dev/sda rw");
            require(!f.inspect(boot).eligible,"mounted storage accepted");
        }
        std::cout<<"USB policy tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
