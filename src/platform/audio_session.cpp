#include "remoteos/audio_session.hpp"
#include "audio_ipc.hpp"
#include <dlfcn.h>
#include <poll.h>
#include <sys/wait.h>
#include <signal.h>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <utility>
#include <mutex>

namespace remoteos::audio {
namespace {
template<typename F>F symbol(void* library,const char* name) {
    void* pointer=dlsym(library,name);F function=nullptr;
    static_assert(sizeof(pointer)==sizeof(function));std::memcpy(&function,&pointer,sizeof(function));return function;
}
bool same(const Session& a,const Session& b){return a.available==b.available&&a.uid==b.uid&&a.id==b.id;}
}
struct SessionSource::Impl {
    void* library=nullptr;
    int(*seat_active)(const char*,char**,uid_t*)=nullptr;
    int(*session_class)(const char*,char**)=nullptr;
    int(*session_type)(const char*,char**)=nullptr;
    int(*session_active)(const char*)=nullptr;
    std::string helper;
    Session previous;
    std::mutex state_mutex;
    explicit Impl(std::string path):helper(std::move(path)) {
        if(helper.empty()||helper.front()!='/')throw std::invalid_argument("Audio-session helper needs an absolute path");
        library=dlopen("libsystemd.so.0",RTLD_NOW|RTLD_LOCAL);
        if(library) {
            seat_active=symbol<decltype(seat_active)>(library,"sd_seat_get_active");
            session_class=symbol<decltype(session_class)>(library,"sd_session_get_class");
            session_type=symbol<decltype(session_type)>(library,"sd_session_get_type");
            session_active=symbol<decltype(session_active)>(library,"sd_session_is_active");
        }
    }
    ~Impl(){if(library)dlclose(library);}
    Session refresh() {
        Session result;char* id=nullptr;char* kind=nullptr;char* type=nullptr;uid_t uid=0;
        if(!seat_active||!session_class||!session_type||!session_active)result.reason="logind session discovery is unavailable";
        else if(seat_active("seat0",&id,&uid)<0||!id)result.reason="No active seat0 user session";
        else {
            result.id=id;result.uid=uid;
            if(session_class(id,&kind)<0||!kind||std::string(kind)!="user")result.reason="Login greeter or non-user session; audio is unavailable";
            else if(session_type(id,&type)<0||!type||std::string(type)!="x11")result.reason="Audio qualification currently requires the active Xorg user session";
            else if(session_active(id)<=0)result.reason="The user session is no longer active";
            else result.available=true;
        }
        std::free(id);std::free(kind);std::free(type);
        std::lock_guard<std::mutex> lock(state_mutex);
        result.generation=previous.generation+(same(previous,result)?0:1);
        previous=result;return result;
    }
    int connect(const Session& expected) {
        const auto current=refresh();
        if(!current.available||!same(current,expected))throw std::runtime_error("Active audio session changed or is unavailable");
        // A privileged host must never execute a user-writable credential helper.
        const int executable=open(helper.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        struct stat metadata{};
        if(executable<0||fstat(executable,&metadata)<0||!S_ISREG(metadata.st_mode)||(metadata.st_mode&0022)||metadata.st_uid!=0) {
            if(executable>=0)close(executable);
            throw std::runtime_error("Audio-session helper must be installed root-owned and not group/world writable");
        }
        int pair[2];if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair)<0){close(executable);throw std::runtime_error("Cannot create audio-session handoff socket");}
        const std::string descriptor=std::to_string(pair[1]),user=std::to_string(current.uid);
        char* arguments[]{helper.data(),const_cast<char*>(descriptor.c_str()),const_cast<char*>(user.c_str()),nullptr};
        const pid_t child=fork();
        if(child<0){close(executable);close(pair[0]);close(pair[1]);throw std::runtime_error("Cannot spawn audio-session helper");}
        if(child==0) {
            close(pair[0]);
            if(fcntl(pair[1],F_SETFD,0)<0)_exit(126);
            fexecve(executable,arguments,environ);_exit(127);
        }
        close(executable);close(pair[1]);int fd=-1,status=0;bool reaped=false;
        try {
            pollfd event{pair[0],POLLIN,0};int ready;
            do{ready=poll(&event,1,3000);}while(ready<0&&errno==EINTR);
            if(ready<=0||!(event.revents&POLLIN))throw std::runtime_error("Audio-session helper timed out or exited without a descriptor");
            fd=detail::receive_reply(pair[0],current.uid);close(pair[0]);pair[0]=-1;
            const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(1);
            pid_t waited=0;
            do {
                waited=waitpid(child,&status,WNOHANG);
                if(waited==0)std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }while((waited==0||(waited<0&&errno==EINTR))&&std::chrono::steady_clock::now()<until);
            reaped=waited==child;
            if(waited!=child||!WIFEXITED(status)||WEXITSTATUS(status)!=0)throw std::runtime_error("Audio-session helper did not finish successfully");
            if(!same(refresh(),current))throw std::runtime_error("User session changed while acquiring PipeWire");
            return fd;
        }catch(...) {
            if(pair[0]>=0)close(pair[0]);
            if(fd>=0)close(fd);
            if(!reaped){kill(child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}}
            throw;
        }
    }
};
SessionSource::SessionSource(std::string helper):impl_(std::make_unique<Impl>(std::move(helper))){}
SessionSource::~SessionSource()=default;
Session SessionSource::refresh(){return impl_->refresh();}
int SessionSource::connect(const Session& expected){return impl_->connect(expected);}
} // namespace remoteos::audio
