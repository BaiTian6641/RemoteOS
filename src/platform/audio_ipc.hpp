#pragma once
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace remoteos::audio::detail {
struct Reply {
    std::uint32_t magic=0x524F4155;
    std::int32_t error=0;
    std::uint32_t uid=0;
    char reason[192]{};
};
inline void send_reply(int channel,int fd,const Reply& reply) {
    iovec data{const_cast<Reply*>(&reply),sizeof(reply)};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};message.msg_iov=&data;message.msg_iovlen=1;
    if(fd>=0) {
        message.msg_control=control;message.msg_controllen=sizeof(control);
        auto* header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(header),&fd,sizeof(fd));
    }
    if(sendmsg(channel,&message,MSG_NOSIGNAL)!=sizeof(reply))throw std::runtime_error("Audio descriptor handoff failed");
}
inline int receive_reply(int channel,uid_t expected) {
    Reply reply{};iovec data{&reply,sizeof(reply)};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int)*4)]{};
    msghdr message{};message.msg_iov=&data;message.msg_iovlen=1;
    message.msg_control=control;message.msg_controllen=sizeof(control);
    const auto count=recvmsg(channel,&message,MSG_CMSG_CLOEXEC);
    int fd=-1;unsigned descriptors=0;
    for(auto* header=CMSG_FIRSTHDR(&message);header;header=CMSG_NXTHDR(&message,header)) {
        if(header->cmsg_level!=SOL_SOCKET||header->cmsg_type!=SCM_RIGHTS)continue;
        if(header->cmsg_len<CMSG_LEN(0))continue;
        const auto size=(header->cmsg_len-CMSG_LEN(0))/sizeof(int);
        for(std::size_t i=0;i<size;++i) {
            int item=-1;std::memcpy(&item,CMSG_DATA(header)+i*sizeof(int),sizeof(item));
            if(descriptors++)close(item);else fd=item;
        }
    }
    const auto fail=[&](const std::string& reason)->int{if(fd>=0)close(fd);throw std::runtime_error(reason);};
    if(count!=sizeof(reply)||(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC))||reply.magic!=0x524F4155||reply.uid!=expected)
        return fail("Invalid audio-session descriptor reply");
    if(reply.error) {
        reply.reason[sizeof(reply.reason)-1]='\0';return fail(std::string("User audio unavailable: ")+reply.reason);
    }
    if(descriptors!=1||fd<0)return fail("Audio-session helper returned no unique descriptor");
    ucred peer{};socklen_t length=sizeof(peer);
    if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&length)<0 || peer.uid!=expected)
        return fail("PipeWire server UID does not match the active user");
    int type=0;length=sizeof(type);sockaddr_un address{};socklen_t address_length=sizeof(address);
    if(getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&length)<0 || type!=SOCK_STREAM ||
       getpeername(fd,reinterpret_cast<sockaddr*>(&address),&address_length)<0 || address.sun_family!=AF_UNIX)
        return fail("PipeWire descriptor is not a connected local stream socket");
    return fd;
}
// Only called in the separately exec'd helper, or in a single-threaded test
// child. The production helper supplies the fixed /run/user/UID/pipewire-0 path.
inline int connect_as_user(int channel,uid_t uid,const std::string& socket_path)noexcept {
    Reply reply;reply.uid=uid;int pipewire=-1;
    try {
        passwd* account=getpwuid(uid);
        if(!account)throw std::runtime_error("Active user account no longer exists");
        const gid_t gid=account->pw_gid;
        if(geteuid()==0) {
            if(setgroups(0,nullptr)<0||setgid(gid)<0||setuid(uid)<0)throw std::runtime_error("Cannot drop to active user credentials");
        } else if(geteuid()!=uid || getegid()!=gid)throw std::runtime_error("Helper identity does not match active user");
        if(geteuid()!=uid || getegid()!=gid)throw std::runtime_error("Active user credential verification failed");
        sockaddr_un address{};address.sun_family=AF_UNIX;
        if(socket_path.size()>=sizeof(address.sun_path))throw std::runtime_error("PipeWire socket path is too long");
        std::memcpy(address.sun_path,socket_path.c_str(),socket_path.size()+1);
        pipewire=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
        if(pipewire<0 || ::connect(pipewire,reinterpret_cast<sockaddr*>(&address),sizeof(address))<0)
            throw std::runtime_error(std::string("Cannot connect to user PipeWire: ")+std::strerror(errno));
        ucred peer{};socklen_t size=sizeof(peer);
        if(getsockopt(pipewire,SOL_SOCKET,SO_PEERCRED,&peer,&size)<0||peer.uid!=uid)
            throw std::runtime_error("User PipeWire socket has an unexpected owner");
        send_reply(channel,pipewire,reply);close(pipewire);return 0;
    } catch(const std::exception& error) {
        if(pipewire>=0)close(pipewire);
        reply.error=1;
        std::snprintf(reply.reason,sizeof(reply.reason),"%s",error.what());
        try{send_reply(channel,-1,reply);}catch(...){}return 1;
    }
}
} // namespace remoteos::audio::detail
