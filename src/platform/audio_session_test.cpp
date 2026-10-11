#include "audio_ipc.hpp"
#include "remoteos/audio_session.hpp"
#include <sys/wait.h>
#include <filesystem>
#include <iostream>

int main() {
    namespace fs=std::filesystem;
    fs::path directory;
    try {
        char name[]="/tmp/remoteos-audio-test-XXXXXX";const auto temporary=mkdtemp(name);
        if(!temporary)throw std::runtime_error("mkdtemp failed");
        directory=temporary;
        const auto path=(directory/"pipewire-test").string();
        const int listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
        sockaddr_un address{};address.sun_family=AF_UNIX;std::memcpy(address.sun_path,path.c_str(),path.size()+1);
        if(listener<0||bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))<0||listen(listener,1)<0)throw std::runtime_error("Fake audio socket unavailable");
        int pair[2];if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair)<0)throw std::runtime_error("socketpair failed");
        const auto uid=getuid();const auto child=fork();
        if(child<0)throw std::runtime_error("fork failed");
        if(child==0){close(pair[0]);close(listener);_exit(remoteos::audio::detail::connect_as_user(pair[1],uid,path));}
        close(pair[1]);const int fd=remoteos::audio::detail::receive_reply(pair[0],uid);close(pair[0]);
        const int peer=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC);int status=0;waitpid(child,&status,0);
        if(peer<0||!WIFEXITED(status)||WEXITSTATUS(status)!=0)throw std::runtime_error("Credential helper failed");
        if(!(fcntl(fd,F_GETFD)&FD_CLOEXEC))throw std::runtime_error("Received descriptor is inheritable");
        const char payload[]="user playback connection";char received[sizeof(payload)]{};
        if(write(fd,payload,sizeof(payload))!=sizeof(payload)||read(peer,received,sizeof(received))!=sizeof(received)||std::memcmp(payload,received,sizeof(payload)))throw std::runtime_error("Descriptor handoff corrupted data");
        close(fd);close(peer);close(listener);
        // Descriptor replies are bound to the expected active user identity.
        if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair)<0)throw std::runtime_error("socketpair failed");
        remoteos::audio::detail::Reply wrong;wrong.uid=uid+1;
        remoteos::audio::detail::send_reply(pair[1],-1,wrong);bool rejected=false;
        try{remoteos::audio::detail::receive_reply(pair[0],uid);}catch(...){rejected=true;}
        close(pair[0]);close(pair[1]);if(!rejected)throw std::runtime_error("Mismatched active user was accepted");
        remoteos::audio::SessionSource source;const auto session=source.refresh();
        if(!session.available&&session.reason.empty())throw std::runtime_error("Unavailable session needs a diagnostic");
        fs::remove_all(directory);std::cout<<"Audio session tests passed: user socket, SCM_RIGHTS, UID binding and discovery diagnostics\n";return 0;
    } catch(const std::exception& error){if(!directory.empty())fs::remove_all(directory);std::cerr<<error.what()<<'\n';return 1;}
}
