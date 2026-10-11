#include "remoteos/host_platform.hpp"
#include "display_config.hpp"

#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#ifdef REMOTEOS_HAVE_EVDI
#include <evdi_lib.h>
#include <poll.h>
static_assert(LIBEVDI_VERSION_MAJOR==1 && LIBEVDI_VERSION_MINOR==14 && LIBEVDI_VERSION_PATCH==11,
              "RemoteOS requires the pinned EVDI 1.14.11 headers; see packaging/linux/dependencies.lock.json");
#endif

namespace remoteos::platform {
namespace {

void checked_ioctl(int fd, unsigned long request, unsigned long value) {
    if (::ioctl(fd, request, value) < 0)
        throw std::runtime_error(std::string("uinput ioctl: ") + std::strerror(errno));
}
bool keyboard_code(unsigned code) {
    return (code>0 && code<BTN_MISC) || (code>=KEY_OK && code<BTN_DPAD_UP) ||
           (code>=KEY_ALS_TOGGLE && code<BTN_TRIGGER_HAPPY);
}
bool mouse_code(unsigned code) { return code>=BTN_LEFT && code<=BTN_TASK; }

#ifdef REMOTEOS_HAVE_EVDI
std::string evdi_version_error() {
    evdi_lib_version version{};evdi_get_lib_version(&version);
    if(version.version_major!=1||version.version_minor!=14||version.version_patchlevel!=11)
        return "Runtime libevdi must match the pinned version 1.14.11";
    std::ifstream file("/sys/module/evdi/version");std::string module_version;
    if(!std::getline(file,module_version)||module_version!="1.14.11")
        return "Load the signed EVDI 1.14.11 module qualified for this exact host kernel";
    return {};
}

void layout(const std::vector<DisplayConfig>& displays,bool restore) {
    std::vector<std::string> args{"/usr/libexec/remoteos-display-layout",restore?"restore":"apply"};
    for(const auto& d:displays)
        args.push_back(std::to_string(d.id)+":"+std::to_string(d.width)+":"+std::to_string(d.height)+":"+std::to_string(d.x)+":"+std::to_string(d.y));
    std::vector<char*> argv;
    for(auto& arg:args)argv.push_back(arg.data());
    argv.push_back(nullptr);
    const pid_t child=fork();
    if(child<0)throw std::runtime_error("Cannot launch display-layout helper");
    if(child==0){execv(argv[0],argv.data());_exit(127);}
    int status=0;pid_t waited;
    do{waited=waitpid(child,&status,0);}while(waited<0&&errno==EINTR);
    if(waited<0||!WIFEXITED(status)||WEXITSTATUS(status)!=0)
        throw std::runtime_error("Display layout failed; configure /etc/remoteos/displays.json and LightDM Xauthority");
}

// EDID 1.3 with one preferred progressive detailed timing at 60 Hz. Frame
// streaming rate is independent from monitor scanout rate.
std::array<unsigned char, 128> edid(const DisplayConfig& d) {
    std::array<unsigned char, 128> e{};
    const unsigned char magic[]{0,255,255,255,255,255,255,0};
    std::copy(std::begin(magic), std::end(magic), e.begin());
    e[8]=0x49; e[9]=0xAF; // RMO
    e[10]=static_cast<unsigned char>(d.id); e[11]=1;
    e[12]=static_cast<unsigned char>(d.id); e[16]=1; e[17]=36;
    e[18]=1; e[19]=3; e[20]=0x80; e[21]=31; e[22]=17; e[23]=120; e[24]=0x0a;
    // sRGB chromaticities; unused standard timings use 0x01,0x01.
    e[25]=0xee; e[26]=0x91; e[27]=0xa3; e[28]=0x54; e[29]=0x4c;
    e[30]=0x99; e[31]=0x26; e[32]=0x0f; e[33]=0x50; e[34]=0x54;
    for (unsigned i=38; i<54; ++i) e[i]=1;
    const unsigned hb=160, vb=28, clock=(d.width+hb)*(d.height+vb)*60/10000;
    auto* t=e.data()+54;
    t[0]=clock&255; t[1]=clock>>8;
    t[2]=d.width&255; t[3]=hb; t[4]=((d.width>>8)<<4)|(hb>>8);
    t[5]=d.height&255; t[6]=vb; t[7]=((d.height>>8)<<4)|(vb>>8);
    t[8]=48; t[9]=32; t[10]=(3<<4)|5; t[11]=0;
    t[12]=310&255; t[13]=170; t[14]=0x10; t[17]=0x1e;
    e[75]=0xfc;
    const std::string name="RemoteOS " + std::to_string(d.id);
    std::fill(e.begin()+77,e.begin()+90,' ');
    std::copy_n(name.begin(),std::min<std::size_t>(name.size(),12),e.begin()+77);
    e[89]='\n';
    e[93]=0xfd; e[95]=50; e[96]=75; e[97]=20; e[98]=100; e[99]=20;
    unsigned sum=0; for (unsigned i=0;i<127;++i) sum+=e[i];
    e[127]=static_cast<unsigned char>(-sum);
    return e;
}

struct EvdiDisplay {
    DisplayConfig config;
    evdi_handle handle=nullptr;
    evdi_event_context context{};
    std::vector<std::uint8_t> pixels;
    unsigned width=0, height=0;
    bool registered=false, pending=false, ready=false, wrong_mode=false;

    static void mode_changed(evdi_mode mode, void* user) {
        auto& self=*static_cast<EvdiDisplay*>(user);
        if (self.registered) evdi_unregister_buffer(self.handle,1);
        self.registered=false; self.pending=false; self.ready=false;
        // libevdi supplies DRM XRGB8888 as B,G,R,X on little-endian hosts.
        if (mode.width != static_cast<int>(self.config.width) ||
            mode.height != static_cast<int>(self.config.height) ||
            mode.bits_per_pixel != 32 || mode.pixel_format != 0x34325258U) {
            self.wrong_mode=true; return;
        }
        self.wrong_mode=false; self.width=mode.width; self.height=mode.height;
        self.pixels.assign(static_cast<std::size_t>(self.width)*self.height*4,0);
        evdi_buffer buffer{};
        buffer.id=1; buffer.buffer=self.pixels.data(); buffer.width=mode.width;
        buffer.height=mode.height; buffer.stride=mode.width*4;
        evdi_register_buffer(self.handle,buffer); self.registered=true;
    }
    static void update_ready(int buffer,void* user) {
        if (buffer==1) static_cast<EvdiDisplay*>(user)->ready=true;
    }
    ~EvdiDisplay() {
        if (handle) {
            if (registered) evdi_unregister_buffer(handle,1);
            evdi_disconnect(handle); evdi_close(handle);
        }
    }
};
#endif

class LinuxHost final : public HostPlatform {
public:
    ~LinuxHost() override { stop(); }

    Capabilities capabilities() const override {
        Capabilities c;
        c.input=::access("/dev/uinput",W_OK)==0;
        if (!c.input) c.diagnostics.emplace_back("Writable /dev/uinput is required");
#ifdef REMOTEOS_HAVE_EVDI
        const auto dependency_error=evdi_version_error();
        unsigned available=0;
        if(dependency_error.empty()) {
            for (int card=0;card<64;++card)if(evdi_check_device(card)==AVAILABLE)++available;
        }else c.diagnostics.push_back(dependency_error);
        c.capture=available>0; c.virtual_displays=available>=2;
        if (available<2) c.diagnostics.emplace_back("Load evdi initial_device_count=2 and grant DRM access");
#else
        c.diagnostics.emplace_back("Built without libevdi: Linux capture and virtual displays unavailable");
#endif
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            if (!capture_error_.empty()) c.diagnostics.push_back(capture_error_);
        }
        c.diagnostics.emplace_back("Unattended greeter/lock qualification has not been completed");
        return c;
    }

    void start(const std::vector<DisplayConfig>& displays,FrameCallback callback) override {
        stop(); detail::validate_displays(displays);
        {std::lock_guard<std::mutex> lock(error_mutex_);capture_error_.clear();}
        if (!callback) throw std::invalid_argument("Frame callback is required");
#ifndef REMOTEOS_HAVE_EVDI
        (void)callback;
        throw std::runtime_error("Linux host needs libevdi; configure with REMOTEOS_HAVE_EVDI");
#else
        try {
            const auto dependency_error=evdi_version_error();
            if(!dependency_error.empty())throw std::runtime_error(dependency_error);
            unsigned needed=0;
            for (int card=0;card<64 && needed<displays.size();++card) {
                if(evdi_check_device(card)!=AVAILABLE) continue;
                auto output=std::make_unique<EvdiDisplay>();
                output->config=displays[needed]; output->handle=evdi_open(card);
                if(!output->handle) continue;
                output->context.mode_changed_handler=&EvdiDisplay::mode_changed;
                output->context.update_ready_handler=&EvdiDisplay::update_ready;
                output->context.user_data=output.get();
                const auto bytes=edid(output->config);
                evdi_connect2(output->handle,bytes.data(),bytes.size(),1920*1080,1920*1080*60);
                // Disabled cursor events ask EVDI to compose the cursor into frames.
                evdi_enable_cursor_events(output->handle,false);
                outputs_.push_back(std::move(output)); ++needed;
            }
            if (outputs_.size()!=displays.size()) throw std::runtime_error("Insufficient unowned EVDI displays");
            open_input(displays);
            callback_=std::move(callback); running_=true;
            worker_=std::thread([this] { capture(); });
            layout_active_=true;
            layout(displays,false);
        } catch (...) { stop(); throw; }
#endif
    }

    void stop() noexcept override {
        running_=false;
        if (worker_.joinable()) worker_.join();
        release_all();
        if (input_>=0) { ::ioctl(input_,UI_DEV_DESTROY); ::close(input_); input_=-1; }
#ifdef REMOTEOS_HAVE_EVDI
        if(layout_active_){try{layout({},true);}catch(...){} layout_active_=false;}
        outputs_.clear();
#endif
        callback_={};
    }

    void inject(const InputEvent& event) override {
        if (input_<0) throw std::runtime_error("Input device is not active");
        switch(event.kind) {
        case InputKind::key:
        case InputKind::button:
            if(event.key>=KEY_CNT || event.key==KEY_POWER ||
               (event.kind==InputKind::key&&!keyboard_code(event.key)) ||
               (event.kind==InputKind::button&&!mouse_code(event.key)))
                throw std::invalid_argument("Unsupported or reserved key");
            if(event.repeat && (!event.down || event.kind!=InputKind::key))
                throw std::invalid_argument("Repeat is valid only for key-down events");
            emit(EV_KEY,event.key,event.repeat?2:(event.down?1:0));
            if(event.down) held_.insert(event.key); else held_.erase(event.key);
            break;
        case InputKind::absolute_pointer:
            emit(EV_ABS,ABS_X,std::clamp(event.x,0,max_x_));
            emit(EV_ABS,ABS_Y,std::clamp(event.y,0,max_y_)); break;
        case InputKind::relative_pointer:
            emit(EV_REL,REL_X,event.delta_x); emit(EV_REL,REL_Y,event.delta_y); break;
        case InputKind::wheel:
            emit(EV_REL,REL_HWHEEL,event.delta_x); emit(EV_REL,REL_WHEEL,event.delta_y); break;
        case InputKind::secure_attention:
            throw std::invalid_argument("Secure-attention operation is Windows only");
        }
        emit(EV_SYN,SYN_REPORT,0);
    }

    void release_all() noexcept override {
        if(input_<0) { held_.clear(); return; }
        for(auto code:held_) { try { emit(EV_KEY,code,0); } catch (...) {} }
        try { emit(EV_SYN,SYN_REPORT,0); } catch (...) {}
        held_.clear();
    }

private:
    void emit(unsigned short type,unsigned short code,int value) {
        input_event event{}; event.type=type; event.code=code; event.value=value;
        ssize_t written;
        do { written=::write(input_,&event,sizeof(event)); } while(written<0 && errno==EINTR);
        if(written!=sizeof(event)) throw std::runtime_error("uinput write failed");
    }
    void open_input(const std::vector<DisplayConfig>& displays) {
        max_x_=0;max_y_=0;
        input_=::open("/dev/uinput",O_WRONLY|O_CLOEXEC);
        if(input_<0) throw std::runtime_error("Cannot open /dev/uinput");
        for(const auto& d:displays) {
            max_x_=std::max(max_x_,d.x+static_cast<int>(d.width)-1);
            max_y_=std::max(max_y_,d.y+static_cast<int>(d.height)-1);
        }
        checked_ioctl(input_,UI_SET_EVBIT,EV_KEY);
        // Declaring joystick/tablet/touch buttons changes libinput's device
        // classification. Advertise keyboard keys and actual mouse buttons.
        for(unsigned code=1;code<KEY_CNT;++code)
            if(code!=KEY_POWER && (keyboard_code(code)||mouse_code(code)))checked_ioctl(input_,UI_SET_KEYBIT,code);
        checked_ioctl(input_,UI_SET_EVBIT,EV_REL);
        for(auto code:{REL_X,REL_Y,REL_WHEEL,REL_HWHEEL}) checked_ioctl(input_,UI_SET_RELBIT,code);
        checked_ioctl(input_,UI_SET_EVBIT,EV_ABS);
        for(auto code:{ABS_X,ABS_Y}) {
            checked_ioctl(input_,UI_SET_ABSBIT,code);
            uinput_abs_setup abs{}; abs.code=code;
            abs.absinfo.minimum=0; abs.absinfo.maximum=code==ABS_X?max_x_:max_y_;
            if(::ioctl(input_,UI_ABS_SETUP,&abs)<0) throw std::runtime_error("Cannot configure absolute pointer");
        }
        uinput_setup setup{}; setup.id.bustype=BUS_VIRTUAL;
        setup.id.vendor=0x1; setup.id.product=0x2140; setup.id.version=1;
        std::strncpy(setup.name,"RemoteOS authenticated input",sizeof(setup.name)-1);
        if(::ioctl(input_,UI_DEV_SETUP,&setup)<0 || ::ioctl(input_,UI_DEV_CREATE)<0)
            throw std::runtime_error("Cannot create uinput device");
    }
#ifdef REMOTEOS_HAVE_EVDI
    void capture() noexcept {
        try {
            std::vector<std::chrono::steady_clock::time_point> next(outputs_.size());
            while(running_) {
                for(std::size_t i=0;i<outputs_.size();++i) {
                    auto& d=*outputs_[i];
                    pollfd event{evdi_get_event_ready(d.handle),POLLIN,0};
                    const int result=::poll(&event,1,5);
                    if(result<0 && errno!=EINTR) throw std::runtime_error("EVDI poll failed");
                    if(event.revents&(POLLERR|POLLHUP|POLLNVAL)) throw std::runtime_error("EVDI device disappeared");
                    if(event.revents&POLLIN) evdi_handle_events(d.handle,&d.context);
                    if(d.wrong_mode) throw std::runtime_error("EVDI mode does not match requested logical resolution/BGRA format");
                    const auto now=std::chrono::steady_clock::now();
                    if(d.registered && !d.pending && now>=next[i]) {
                        d.ready=evdi_request_update(d.handle,1); d.pending=true;
                    }
                    if(d.ready) {
                        std::array<evdi_rect,16> rects{}; int count=rects.size();
                        evdi_grab_pixels(d.handle,rects.data(),&count);
                        VideoFrame frame;
                        frame.display_id=d.config.id; frame.width=d.width; frame.height=d.height;
                        frame.stride=d.width*4; frame.timestamp=now; frame.bgra=d.pixels;
                        callback_(std::move(frame));
                        d.ready=false; d.pending=false;
                        next[i]=detail::next_capture(now,next[i],d.config.fps);
                    }
                }
            }
        } catch(const std::exception& e) {
            std::lock_guard<std::mutex> lock(error_mutex_); capture_error_=e.what(); running_=false;
        } catch(...) {
            std::lock_guard<std::mutex> lock(error_mutex_); capture_error_="Capture callback failed"; running_=false;
        }
    }
    std::vector<std::unique_ptr<EvdiDisplay>> outputs_;
    bool layout_active_=false;
#endif
    int input_=-1, max_x_=0, max_y_=0;
    std::set<unsigned short> held_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    FrameCallback callback_;
    mutable std::mutex error_mutex_;
    std::string capture_error_;
};

} // namespace
std::unique_ptr<HostPlatform> make_host_platform() { return std::make_unique<LinuxHost>(); }
} // namespace remoteos::platform
