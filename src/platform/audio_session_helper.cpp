#include "audio_ipc.hpp"
#include <limits>

int main(int argc,char** argv) {
    if(argc!=3)return 2;
    try {
        std::size_t used=0;const auto descriptor=std::stoul(argv[1],&used);
        if(used!=std::strlen(argv[1])||descriptor>std::numeric_limits<int>::max())return 2;
        const auto uid=std::stoul(argv[2],&used);
        if(used!=std::strlen(argv[2])||uid>=std::numeric_limits<uid_t>::max())return 2;
        const auto path="/run/user/"+std::to_string(uid)+"/pipewire-0";
        return remoteos::audio::detail::connect_as_user(static_cast<int>(descriptor),static_cast<uid_t>(uid),path);
    } catch(...){return 2;}
}
