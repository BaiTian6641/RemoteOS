#include "remoteos/devices.hpp"
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/sysmacros.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace remoteos::devices {
namespace {
namespace fs=std::filesystem;
std::string read(const fs::path& path) {
    std::ifstream stream(path);std::string value;
    if(stream)std::getline(stream,value);
    while(!value.empty() && (value.back()=='\r'||value.back()=='\n'||value.back()==' '))value.pop_back();
    return value;
}
std::string lower(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});return value;
}
std::set<std::string> mounted(const std::string& path) {
    std::ifstream file(path);
    if(!file)throw std::runtime_error("Cannot inspect mounted filesystems");
    std::set<std::string> result;
    for(std::string line;std::getline(file,line);) {
        if(line.empty())continue;
        std::istringstream fields(line);std::string id,parent,device;
        if(!(fields>>id>>parent>>device))throw std::runtime_error("Malformed mountinfo");
        result.insert(device);
    }
    if(path=="/proc/self/mountinfo") {
        // Swap has no mountinfo entry. Treat its underlying block device or
        // swap-file filesystem as busy as well.
        std::ifstream swaps("/proc/swaps");
        if(!swaps)throw std::runtime_error("Cannot inspect active swap devices");
        for(std::string line;std::getline(swaps,line);) {
            std::istringstream row(line);std::string filename;row>>filename;
            if(filename.empty()||filename=="Filename")continue;
            struct stat info{};
            if(::stat(filename.c_str(),&info)<0)throw std::runtime_error("Cannot identify active swap backing device");
            const auto dev=S_ISBLK(info.st_mode)?info.st_rdev:info.st_dev;
            result.insert(std::to_string(major(dev))+":"+std::to_string(minor(dev)));
        }
    }
    return result;
}
struct DeviceDetails {UsbDevice device;std::vector<std::string> numbers;bool net=false,storage=false,hub=false,hid=false;};
DeviceDetails inspect(const fs::path& link) {
    DeviceDetails result;auto& d=result.device;
    d.bus_id=link.filename().string();d.vid=lower(read(link/"idVendor"));d.pid=lower(read(link/"idProduct"));
    d.serial=read(link/"serial");d.label=read(link/"manufacturer")+" "+read(link/"product");
    const auto device_class=read(link/"bDeviceClass");
    const auto classify=[&](unsigned value){
        d.interface_classes.push_back(value);
        result.hub=result.hub||value==9;result.hid=result.hid||value==3;
        result.storage=result.storage||value==8;
        result.net=result.net||value==2||value==10||value==0xe0;
    };
    if(!device_class.empty())classify(std::stoul(device_class,nullptr,16));
    const auto root=fs::canonical(link);
    for(const auto& child:fs::recursive_directory_iterator(root,fs::directory_options::none)) {
        if(child.is_symlink())continue;
        const auto name=child.path().filename().string();
        if(name=="bInterfaceClass")classify(std::stoul(read(child.path()),nullptr,16));
        if(name=="net"&&child.is_directory()&&!fs::is_empty(child.path()))result.net=true;
        if(name=="dev" && child.path().string().find("/block/")!=std::string::npos) {
            result.storage=true;d.block_nodes.push_back("/dev/"+child.path().parent_path().filename().string());
            result.numbers.push_back(read(child.path()));
        }
    }
    return result;
}
bool approved(const UsbDevice& d,const UsbPolicy& policy) {
    for(const auto& entry:policy.approved)
        if(lower(entry.vid)==d.vid && lower(entry.pid)==d.pid && (entry.serial.empty()||entry.serial==d.serial))return true;
    return false;
}
bool protected_disk(const fs::path& device,const UsbPolicy& policy,const fs::path& sysroot) {
    const auto real=fs::canonical(device).string()+"/";
    for(const auto& disk:policy.protected_block_devices) {
        const auto sysdev=sysroot/"class/block"/fs::path(disk).filename();
        if(!fs::exists(sysdev))throw std::runtime_error("Configured boot disk is missing; requalify its identity");
        const auto path=fs::canonical(sysdev).string()+"/";
        if(path.compare(0,real.size(),real)==0)return true;
    }
    return false;
}
void root_only() {if(::geteuid()!=0)throw std::runtime_error("USB operation requires the privileged supervisor");}
std::string trusted_record(const fs::path& path) {
    const int fd=open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)throw std::runtime_error("Owned USB/IP import record is unavailable");
    struct stat info{};
    if(fstat(fd,&info)<0||!S_ISREG(info.st_mode)||info.st_uid!=0||(info.st_mode&0022)||info.st_size>1024) {
        close(fd);throw std::runtime_error("USB/IP import record is not trusted root-owned state");
    }
    char bytes[1025]{};const auto count=::read(fd,bytes,1024);close(fd);
    if(count<=0)throw std::runtime_error("Cannot read USB/IP import record");
    return std::string(bytes,count);
}
std::string execute(const std::vector<std::string>& args) {
    root_only();
    if(args.empty()||args[0].empty()||args[0][0]!='/')throw std::invalid_argument("USB/IP executable must be an absolute path");
    std::vector<char*> argv;
    for(const auto& arg:args)argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    int pipefd[2];if(pipe2(pipefd,O_CLOEXEC|O_NONBLOCK)<0)throw std::runtime_error("Cannot create command pipe");
    const pid_t child=fork();
    if(child<0){close(pipefd[0]);close(pipefd[1]);throw std::runtime_error("Cannot start USB/IP tool");}
    if(child==0) {
        close(pipefd[0]);dup2(pipefd[1],STDOUT_FILENO);dup2(pipefd[1],STDERR_FILENO);close(pipefd[1]);
        execv(argv[0],argv.data());_exit(127);
    }
    close(pipefd[1]);std::string output;int status=0;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    bool finished=false;
    while(!finished) {
        char data[4096];ssize_t count;
        while((count=::read(pipefd[0],data,sizeof(data)))>0) {
            if(output.size()+static_cast<std::size_t>(count)<=65536)output.append(data,count);
        }
        const auto waited=waitpid(child,&status,WNOHANG);
        if(waited==child)finished=true;
        else if(waited<0 && errno!=EINTR){kill(child,SIGKILL);waitpid(child,&status,0);close(pipefd[0]);throw std::runtime_error("Cannot wait for USB/IP tool");}
        else if(std::chrono::steady_clock::now()>deadline){kill(child,SIGKILL);waitpid(child,&status,0);close(pipefd[0]);throw std::runtime_error("USB/IP command timed out");}
        else std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    char remaining[4096];ssize_t count;
    while((count=::read(pipefd[0],remaining,sizeof(remaining)))>0)
        if(output.size()+static_cast<std::size_t>(count)<=65536)output.append(remaining,count);
    close(pipefd[0]);
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)throw std::runtime_error("USB/IP command failed: "+output);
    return output;
}
void validate_bus(const std::string& bus){if(!valid_bus_id(bus))throw std::invalid_argument("Invalid USB bus identifier");}
}

std::vector<UsbDevice> enumerate_usb(const UsbPolicy& policy,const std::string& sys_root,const std::string& mountinfo) {
    const auto mounts=mounted(mountinfo);std::vector<UsbDevice> devices;
    const auto directory=fs::path(sys_root)/"bus/usb/devices";
    if(!fs::exists(directory))return devices;
    for(const auto& item:fs::directory_iterator(directory)) {
        if(!valid_bus_id(item.path().filename().string()))continue;
        UsbDevice failure;failure.bus_id=item.path().filename().string();
        try {
            auto info=inspect(item.path());auto& d=info.device;
            if(d.vid.empty()||d.pid.empty())d.reason="USB identity is unavailable";
            else if(info.hub)d.reason="USB hubs cannot be forwarded";
            else if(info.hid)d.reason="HID stays under the client input broker";
            else if(info.net)d.reason="Network adapters cannot be forwarded";
            else if(info.storage && policy.protected_block_devices.empty())d.reason="Configure the client boot-disk exclusion before forwarding storage";
            else if(info.storage && protected_disk(item.path(),policy,sys_root))d.reason="Client boot/storage backing device is protected";
            else if(std::any_of(info.numbers.begin(),info.numbers.end(),[&](const auto& n){return mounts.count(n)>0;}))d.reason="Unmount/eject all local volumes before forwarding this device";
            else if(!approved(d,policy))d.reason="Device model/serial is not in the approved-device policy";
            else {d.eligible=true;d.reason="Approved; bind only after establishing an authenticated device tunnel";}
            devices.push_back(std::move(d));
        }catch(const std::exception& error){failure.reason=error.what();devices.push_back(std::move(failure));}
    }
    std::sort(devices.begin(),devices.end(),[](const auto& a,const auto& b){return a.bus_id<b.bus_id;});return devices;
}

void bind_usb(const std::string& bus_id,const UsbPolicy& policy,const std::string& tool) {
    validate_bus(bus_id);root_only();
    for(const auto& device:enumerate_usb(policy))if(device.bus_id==bus_id) {
        if(!device.eligible)throw std::runtime_error(device.reason);
        execute({tool,"bind","--busid",bus_id});return;
    }
    throw std::runtime_error("USB device disappeared before export");
}
void unbind_usb(const std::string& bus_id,const std::string& tool) {
    validate_bus(bus_id);execute({tool,"unbind","--busid",bus_id});
}
std::string attach_loopback(const std::string& bus_id,std::uint16_t local_port,const std::string& tool) {
    validate_bus(bus_id);if(local_port==0)throw std::invalid_argument("An authenticated tunnel port is required");
    return execute({tool,"--tcp-port",std::to_string(local_port),"attach","--remote","127.0.0.1","--busid",bus_id});
}
void detach_import(unsigned port,const std::string& tool) {
    root_only();if(port>65535)throw std::invalid_argument("Invalid VHCI port");
    std::string bus;
    for(const auto& controller:fs::directory_iterator("/sys/devices/platform")) {
        if(controller.path().filename().string().find("vhci_hcd")!=0)continue;
        for(const auto& attribute:fs::directory_iterator(controller)) {
            if(attribute.path().filename().string().find("status")!=0 || !attribute.is_regular_file())continue;
            std::ifstream state(attribute.path());
            for(std::string line;std::getline(state,line);) {
                std::istringstream row(line);std::string hub,dev,socket,local;unsigned candidate,status,speed;
                if(row>>hub>>candidate>>status>>speed>>dev>>socket>>local)
                    if(candidate==port && valid_bus_id(local))bus=local;
            }
        }
    }
    if(bus.empty())throw std::runtime_error("Imported device not found at requested VHCI port");
    const auto device=inspect(fs::path("/sys/bus/usb/devices")/bus);
    const auto mounts=mounted("/proc/self/mountinfo");
    for(const auto& number:device.numbers)if(mounts.count(number))throw std::runtime_error("Eject imported storage in the host desktop before detaching");
    for(const auto& node:device.device.block_nodes) {
        const int fd=open(node.c_str(),O_RDONLY|O_CLOEXEC);
        if(fd<0)throw std::runtime_error("Cannot open imported storage for flush");
        const int sync_result=fsync(fd),flush_result=ioctl(fd,BLKFLSBUF,0);close(fd);
        if(sync_result<0||flush_result<0)throw std::runtime_error("Imported storage flush failed; device stays attached");
    }
    execute({tool,"detach","--port",std::to_string(port)});
}
unsigned import_device(const std::string& bus_id,std::uint16_t local_port,const std::string& tool) {
    validate_bus(bus_id);root_only();
    // Linux usbip records remote host/port/bus in root-only portN files after
    // successful attach. Match the unique loopback tunnel endpoint, never an
    // arbitrary newly observed USB device or an unrelated VHCI allocation.
    const fs::path state="/run/vhci_hcd";
    const auto find_port=[&]()->std::vector<unsigned>{
        std::vector<unsigned> ports;
        if(!fs::exists(state))return ports;
        for(const auto& entry:fs::directory_iterator(state)) {
            const auto name=entry.path().filename().string();
            if(!std::regex_match(name,std::regex("port[0-9]+")))continue;
            std::istringstream record(trusted_record(entry.path()));std::string host,bus;unsigned port=0;
            if(record>>host>>port>>bus)
                if(host=="127.0.0.1"&&port==local_port&&bus==bus_id)ports.push_back(std::stoul(name.substr(4)));
        }
        return ports;
    };
    const auto before=find_port();
    if(!before.empty())throw std::runtime_error("Tunnel endpoint already has a VHCI import record; remove its stale record or detach before retrying");
    attach_loopback(bus_id,local_port,tool);
    const auto ports=find_port();
    if(ports.size()!=1)throw std::runtime_error("USB/IP attached but its unique VHCI port record is unavailable; inspect usbip port before retrying");
    return ports.front();
}
void detach_device(unsigned port,const std::string& tool){detach_import(port,tool);}
void abandon_import(unsigned port,const std::string& bus,std::uint16_t local_port,const std::string& tool) {
    root_only();validate_bus(bus);
    if(port>65535||local_port==0)throw std::invalid_argument("Invalid owned import endpoint");
    std::istringstream record(trusted_record(fs::path("/run/vhci_hcd")/("port"+std::to_string(port))));
    std::string host,recorded_bus;unsigned recorded_port=0;
    if(!(record>>host>>recorded_port>>recorded_bus)||host!="127.0.0.1"||recorded_port!=local_port||recorded_bus!=bus)
        throw std::runtime_error("Refusing to detach an import not owned by this device tunnel");
    execute({tool,"detach","--port",std::to_string(port)});
}
} // namespace remoteos::devices
