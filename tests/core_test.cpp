#include "remoteos/identity.hpp"
#include "remoteos/protocol.hpp"
#include "remoteos/discovery.hpp"
#include "../src/media/packet_pacer.hpp"
#include "../src/media/mpeg2_freshness.hpp"
#include "../src/client/pointer.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace remoteos;
void require(bool condition, const char *why) {
  if (!condition)
    throw std::runtime_error(why);
}
template <typename F> void rejects(F fn) {
  bool rejected = false;
  try {
    fn();
  } catch (const std::exception &) {
    rejected = true;
  }
  require(rejected, "expected invalid input rejection");
}
int main() {
  try {
    media_detail::Mpeg2Freshness freshness;
    const std::uint8_t intra[] = {0, 0, 1, 0xb3, 0x40, 0x24, 0, 0, 1, 0, 0, 8};
    const std::uint8_t predicted[] = {0, 0, 1, 0, 0, 16};
    require(freshness.accept(predicted, sizeof(predicted), false, false), "Fresh predicted picture was discarded");
    require(!freshness.accept(predicted, sizeof(predicted), true, false), "Stale MPEG picture was decoded");
    require(!freshness.accept(predicted, sizeof(predicted), false, false), "Broken prediction chain resumed without an I picture");
    require(!freshness.accept(intra, sizeof(intra), true, false), "Stale I picture restarted playback");
    require(freshness.accept(intra, sizeof(intra), false, false), "Fresh I picture did not restore playback");
    require(!freshness.accept(predicted, sizeof(predicted), false, true), "Packet discontinuity decoded a dependent picture");
    require(!freshness.accept(intra, 5, false, false), "Truncated header restored reference synchronization");
    require(freshness.accept(intra, sizeof(intra), false, false), "Recovery after discontinuity failed");
    Display panel{"internal", 1366, 768, 0, 0, 1366, 768, 30, 6000000};
    Display vga{"vga", 1920, 1080, 1366, 0, 1920, 1080, 30, 12000000};
    auto ds = parse_displays(Json::array({panel, vga}));
    require(ds.size() == 2, "dual displays");
    // These rates were verified against the pinned MPEG-2 encoder, including
    // its frame-rate extensions. Every adaptation result must remain encodable.
    const std::set<int> encodable{5,  6,  8,  9,  10, 12, 15,
                                  16, 18, 20, 24, 25, 30, 50, 60};
    for (int fps = 5; fps <= 60; ++fps) {
      auto profile = panel;
      profile.fps = fps;
      if (!encodable.count(fps)) {
        rejects([&] { parse_displays(Json::array({profile})); });
        continue;
      }
      parse_displays(Json::array({profile}));
      Adaptation controller(profile);
      for (int i = 0; i < 30; ++i) {
        controller.sample(90, 0.05, 100);
        parse_displays(Json::array({controller.profile()}));
      }
      for (int i = 0; i < 300; ++i) {
        controller.sample(20, 0, 0);
        parse_displays(Json::array({controller.profile()}));
      }
      require(controller.profile().fps == fps &&
                  controller.profile().encoded_width == profile.encoded_width,
              "supported MPEG-2 profile recovers without invalid intermediate "
              "rates");
    }
    validate_message({{"type", "video_profile"}, {"displays", Json::array({panel})}});
    rejects([&] { validate_message({{"type", "video_profile"}, {"displays", Json::array()}}); });
    rejects([&] { validate_message({{"type", "start"}, {"displays", Json::array({panel})}, {"live_video_reconfigure", "yes"}}); });
    auto raw = panel;
    raw.codec = "raw-i420";
    raw.fps = 60;
    raw.bitrate = 0;
    const auto mixed = parse_displays(Json::array({raw, vga}));
    require(mixed.front().codec == "raw-i420" && mixed.front().fps == 60,
            "raw 60fps profile");
    rejects([&] {
      auto bad = panel;
      bad.fps = 61;
      parse_displays(Json::array({bad}));
    });
    Adaptation raw_adaptation(raw);
    for (int i = 0; i < 15; ++i)
      raw_adaptation.sample(90, 0, 0);
    require(raw_adaptation.profile().codec == "raw-i420" &&
                raw_adaptation.profile().bitrate == 0 &&
                raw_adaptation.profile().fps < 60,
            "raw adaptation preserves format and no encoder bitrate");
    rejects([&] {
      auto bad = Json(panel);
      bad["encoded_width"] = 1365;
      Display d = bad.get<Display>();
      (void)d;
    });
    rejects([&] {
      auto bad = vga;
      bad.x = 0;
      parse_displays(Json::array({panel, bad}));
    });
    rejects([&] { validate_message({{"type", "hello"}, {"version", 2}}); });
    rejects([&] {
      validate_message(
          {{"type", "input"}, {"kind", "key"}, {"code", 116}, {"down", true}});
    });
    rejects([&] {
      validate_message(
          {{"type", "input"}, {"kind", "key"}, {"code", 30}, {"down", 1}});
    });
    validate_message(
        {{"type", "input"}, {"kind", "key"}, {"code", 30}, {"down", true}});
    auto scaled = vga;
    scaled.encoded_width = 1280;
    scaled.encoded_height = 720;
    auto mapped = map_pointer(scaled, 640, 360);
    require(std::abs(mapped.first - 2326) < 1 && std::abs(mapped.second - 540) < 1,
            "encoded coordinate mapping");
    const auto corner = map_pointer(scaled, 1279, 719);
    require(corner.first == 3285 && corner.second == 1079, "scaled pointer reaches the final native pixel");
    const auto origin = map_pointer(panel, 0, 0), last = map_pointer(panel, 1365, 767);
    require(origin.first == 0 && origin.second == 0 && last.first == 1365 && last.second == 767, "native pointer mapping is one-to-one");
    const std::vector<client::ScreenRectangle> screens{{0, 0, 1366, 768}, {-1920, 0, 1920, 1200}};
    const auto native_pointer = client::pointer_message(ds, screens, 110, 120);
    require(native_pointer && native_pointer->at("x") == 110 && native_pointer->at("y") == 120, "processed touchpad/mouse coordinates forward without a raw-device multiplier");
    const auto vga_pointer = client::pointer_message(ds, screens, -1, 1139);
    require(vga_pointer && vga_pointer->at("display") == "vga" && vga_pointer->at("x") == 1919 && vga_pointer->at("y") == 1079, "VGA root coordinates map across a negative monitor position and letterboxing");
    require(!client::pointer_message(ds, screens, 3000, 3000), "out-of-screen motion cannot jump the host pointer");
    auto over_budget = ds;
    for (auto& display : over_budget) display.bitrate = 30000000;
    limit_video_bandwidth(over_budget, default_bandwidth_bps);
    require(over_budget[0].bitrate + over_budget[1].bitrate <= 32000000, "two monitors share the 40 Mbps connection allocation");
    require(over_budget[0].width == ds[0].width && over_budget[1].height == ds[1].height, "bandwidth allocation preserves the logical desktop");
    auto bad_raw = std::vector<Display>{raw};
    rejects([&] { limit_video_bandwidth(bad_raw, default_bandwidth_bps); });
    rejects([&] { limit_video_bandwidth(over_budget, 0); });
    for (unsigned i = 0; i < 20; ++i) {
      const auto pin = random_pairing_pin();
      require(pin.size() == 6 && pin.find_first_not_of("0123456789") == std::string::npos, "pairing PIN is six numeric digits");
    }
    const std::string nonce(24, 'a'), advertised_fp(64, 'b');
    const auto packet = Json{{"type", "remoteos-host"}, {"version", 1}, {"nonce", nonce}, {"name", "Test host"}, {"port", 47010}, {"fingerprint", advertised_fp}}.dump();
    const auto discovered = parse_discovery_reply(packet, nonce, "127.0.0.1");
    rejects([&] { parse_discovery_reply(packet, std::string(24, 'c'), "127.0.0.1"); });
    rejects([&] { parse_discovery_reply(packet, nonce, "0.0.0.0"); });
    auto saved = discovered; saved.saved = true;
    auto replaced = discovered; replaced.fingerprint = std::string(64, 'c');
    const auto hosts = merge_hosts({saved}, {replaced});
    require(hosts.size() == 1 && hosts.front().identity_changed && hosts.front().fingerprint == advertised_fp, "discovery cannot replace a paired host identity");
    media_detail::PacketPacer pacer(8000000);
    const auto now = media_detail::PacketPacer::Clock::now();
    auto send_at = now;
    for (unsigned i = 0; i < 100; ++i) send_at = pacer.reserve(872, now);
    require(send_at - now == std::chrono::milliseconds(99), "packet starts preserve payload plus tunnel overhead pacing");
    require(pacer.reserve(872, now + std::chrono::seconds(1)) == now + std::chrono::seconds(1), "an idle link sends the first packet immediately");
    for (unsigned i = 0; i < 10; ++i) send_at = pacer.reserve(872, now + std::chrono::seconds(1));
    require(send_at >= now + std::chrono::milliseconds(1008), "idle credit permits only bounded packet catch-up");
    media_detail::PacketPacer tuning(8000000);
    tuning.reserve(872, now);
    tuning.set_rate(16000000);
    require(tuning.reserve(872, now) == now + std::chrono::milliseconds(1), "live pacing change preserves the reserved deadline");
    require(tuning.reserve(872, now) == now + std::chrono::microseconds(1500), "live pacing change uses the new wire allocation");
    media_detail::PacketPacer scheduling(8000000);
    auto deadline = scheduling.reserve(872, now);
    for (unsigned i = 0; i < 100; ++i) deadline = scheduling.reserve(872, deadline + std::chrono::microseconds(1100));
    require(deadline == now + std::chrono::milliseconds(110), "late timer wakeups do not incur a second packet serialization wait");
    auto fast_panel = panel; fast_panel.fps = 60;
    require(Json(fast_panel).get<Display>().fps == 60, "MPEG-2 supports a negotiated 60 fps profile");
    Adaptation fast(fast_panel, true);
    for (unsigned i = 0; i < 6; ++i) fast.sample(95, 0, 0);
    require(fast.profile().fps == 30, "fast MPEG-2 falls back through supported frame rates");
    for (unsigned i = 0; i < 60; ++i) fast.sample(40, 0, 0);
    require(fast.profile().fps == 60, "fast MPEG-2 recovers across the 30-to-50 rate gap");
    Adaptation a(panel);
    require(!a.sample(90, 0, 0), "hysteresis1");
    require(!a.sample(90, 0, 0), "hysteresis2");
    require(a.sample(90, 0, 0) && a.profile().fps == 25,
            "CPU fallback before resolution");
    require(a.profile().width == 1366 && a.profile().encoded_width == 1366,
            "logical layout maintained");
    Adaptation readable(panel, true);
    for (int i = 0; i < 90; ++i) readable.sample(95, 0.1, 100);
    require(readable.profile().encoded_width == panel.width && readable.profile().encoded_height == panel.height &&
            readable.profile().fps < panel.fps, "native text profile reduces frame rate without resizing pixels");
    require(!readable.sample(95, 0.1, 100), "native profile does not restart an unchanged MPEG-2 frame-rate floor");
    for (int i = 0; i < 120; ++i) readable.sample(20, 0, 0);
    require(readable.profile().fps == panel.fps && readable.profile().encoded_width == panel.width,
            "native profile recovers frame rate while retaining readable pixels");
    Adaptation dropping(panel, true);
    for (int i = 0; i < 3; ++i) dropping.sample(50, 0, 0, 0.4);
    const auto presentation_fps = dropping.profile().fps;
    require(presentation_fps <= 18 && dropping.profile().encoded_width == panel.width &&
            dropping.profile().bitrate == panel.bitrate,
            "late presentation reduces frame rate even with an empty queue and moderate total CPU");
    for (int i = 0; i < 60; ++i) dropping.sample(20, 0, 0, 0.04);
    require(dropping.profile().fps == presentation_fps, "persistent presentation drops prevent premature recovery");
    for (int i = 0; i < 90; ++i) dropping.sample(20, 0, 0, 0);
    require(dropping.profile().fps == 30, "stable presentation restores requested frame rate");
    Adaptation congested(fast_panel, true);
    for (int i = 0; i < 3; ++i) congested.sample(40, .05, 0, .5);
    require(congested.profile().fps == 60 && congested.profile().bitrate < fast_panel.bitrate &&
            congested.profile().encoded_width == fast_panel.width && congested.reason().find("packet loss") != std::string::npos,
            "loss with spare decode capacity reduces traffic rather than ratcheting fps at the same bitrate");
    for (int i = 0; i < 150; ++i) congested.sample(40, .005, 0);
    require(congested.profile().fps == 60 && congested.profile().bitrate == fast_panel.bitrate,
            "minor ongoing packet loss permits a stable delivery recovery");
    Adaptation slow_decode(fast_panel, true);
    for (int i = 0; i < 3; ++i) slow_decode.sample(40, 0, 0, 0, 0, 20);
    require(slow_decode.profile().fps == 30 && slow_decode.profile().bitrate == fast_panel.bitrate,
            "measured decode cost selects a sustainable legal rate even when SMT CPU percentage is moderate");
    for (int i = 0; i < 120; ++i) slow_decode.sample(60, 0, 0, 0, 30, 20);
    require(slow_decode.profile().fps == 30,
            "20 ms decode cost cannot repeatedly oscillate between 30 and 50 fps");
    for (int i = 0; i < 24; ++i) slow_decode.sample(40, .008, 0, .02, 0, 5);
    require(slow_decode.profile().fps == 60, "sustained spare decode capacity restores the requested frame rate");
    Adaptation recovery(fast_panel, true);
    for (int i = 0; i < 15; ++i) recovery.sample(60, 0, 0, .5, 30, 5, 1, 0, .5);
    require(recovery.profile().fps == 60 && recovery.profile().bitrate == fast_panel.bitrate,
            "reference recovery discards do not masquerade as display or network overload");
    for (int i = 0; i < 3; ++i) recovery.sample(60, 0, 0, .2, 30, 5, 1, .2, 0);
    require(recovery.profile().fps < 60 && recovery.profile().bitrate == fast_panel.bitrate,
            "real presentation drops reduce picture workload while preserving the quality budget");
    Adaptation backlog(fast_panel, true);
    for (int i = 0; i < 3; ++i) backlog.sample(60, 0, 0, .2, 100, 5, 1, 0, .2);
    require(backlog.profile().fps < 60 && backlog.profile().bitrate == fast_panel.bitrate,
            "receiver timeline backlog alone cannot be misclassified as bandwidth congestion");
    Adaptation automatic(fast_panel, true, 18000000);
    for (int i = 0; i < 72; ++i) automatic.sample(40, 0, 0, 0, 30, 5);
    require(automatic.profile().bitrate == 18000000 && automatic.profile().fps == 60,
            "automatic quality may use an authorized higher ceiling after stable delivery");
    Adaptation explicit_budget(fast_panel, true);
    for (int i = 0; i < 120; ++i) explicit_budget.sample(40, 0, 0, 0, 30, 5);
    require(explicit_budget.profile().bitrate == fast_panel.bitrate,
            "an explicit bitrate profile is never silently raised");
    Adaptation balanced(fast_panel, true, 12000000);
    for (int i = 0; i < 3; ++i) balanced.sample(60, 0, 0, 0, 30, 20);
    for (int i = 0; i < 60; ++i) balanced.sample(60, 0, 0, 0, 30, 20);
    require(balanced.profile().fps == 30 && balanced.profile().bitrate == 12000000,
            "a decode-limited receiver can improve quality without probing unsustainable frame rates");
    rejects([&] { Adaptation invalid(fast_panel, true, 30000001); });
    rejects([&] { Adaptation invalid(fast_panel, true, fast_panel.bitrate - 1); });
    Adaptation rapid(fast_panel, true);
    for (int i = 0; i < 100; ++i) rapid.sample(95, 0, 0, 0, 0, 0, .001);
    require(rapid.profile().fps == 60, "rapid polling cannot manufacture sustained decoder pressure");
    require(!rapid.sample(-1, 0, 0) && !rapid.sample(40, 1.1, 0) && !rapid.sample(40, 0, -1) &&
            !rapid.sample(40, 0, 0, 0, 0, 0, 1, -.5) &&
            !rapid.sample(40, 0, 0, 0, 0, 0, 1, 0, 1.1),
            "invalid feedback cannot alter an adaptive profile");
    auto dir = std::filesystem::temp_directory_path() /
               ("remoteos-test-" + random_token(8));
    ensure_identity(dir, "test");
    auto fp = certificate_fingerprint(dir / "certificate.pem");
    require(fp.size() == 64, "identity fingerprint");
    ensure_identity(dir, "test");
    require(fp == certificate_fingerprint(dir / "certificate.pem"),
            "identity stable");
    require(normalize_fingerprint(fp) == fp, "fingerprint normalize");
    rejects([] { normalize_fingerprint("1234"); });
    write_private_file(dir / "state.json", "{}");
    require(read_private_file(dir / "state.json") == "{}",
            "private state roundtrip");
    std::filesystem::remove_all(dir);
    std::cout << "core policy and identity tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
