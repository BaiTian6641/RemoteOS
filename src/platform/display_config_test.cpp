#include "display_config.hpp"
#include "remoteos/protocol.hpp"
#include <iostream>

int main() {
    try {
        using namespace remoteos;
        auto logical=parse_displays(Json::array({
            {{"id","internal"},{"width",1366},{"height",768},{"codec","raw-i420"},{"fps",60}},
            {{"id","vga"},{"width",1920},{"height",1080},{"x",1366},{"fps",30}}
        }));
        std::vector<platform::DisplayConfig> capture;
        for(std::size_t i=0;i<logical.size();++i) {
            const auto& d=logical[i];
            capture.push_back({unsigned(i),unsigned(d.width),unsigned(d.height),d.x,d.y,unsigned(d.fps)});
        }
        platform::detail::validate_displays(capture);
        capture.front().fps=61;
        bool rejected=false;
        try{platform::detail::validate_displays(capture);}catch(const std::invalid_argument&){rejected=true;}
        if(!rejected)throw std::runtime_error("Capture accepted an unsupported rate");
        using namespace std::chrono;
        const steady_clock::time_point start{};
        auto next=platform::detail::next_capture(start,start,60);
        if(next-start!=microseconds(16667))throw std::runtime_error("60fps period rounded down to milliseconds");
        next=platform::detail::next_capture(start+milliseconds(17),next,60);
        if(next-start!=microseconds(33334))throw std::runtime_error("Small capture jitter changed the target phase");
        next=platform::detail::next_capture(start+seconds(1),next,60);
        if(next!=start+seconds(1)+microseconds(16667))throw std::runtime_error("Capture catches up stale frames after a stall");
        std::cout<<"Mixed raw60/MPEG2 protocol profiles accepted by shared platform validation; capture pacing passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
