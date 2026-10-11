#include "backend.hpp"
#include "input.hpp"
#include "setup_state.hpp"
#include "telemetry.hpp"
#include <iostream>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>
using namespace remoteos::client;
static void require(bool condition, const char *reason) {
  if (!condition)
    throw std::runtime_error(reason);
}
int main() {
  try {
    auto telemetry_dir = (std::filesystem::temp_directory_path() / "remoteos-telemetry-XXXXXX").string();
    require(::mkdtemp(telemetry_dir.data()) != nullptr, "create private telemetry fixture");
    const char *saved_runtime_pointer = std::getenv("XDG_RUNTIME_DIR");
    const bool had_runtime = saved_runtime_pointer != nullptr;
    const std::string saved_runtime = had_runtime ? saved_runtime_pointer : "";
    ::setenv("XDG_RUNTIME_DIR", telemetry_dir.c_str(), 1);
    Telemetry telemetry;
    remoteos::Display internal, vga;
    internal.id = "internal"; internal.bitrate = 12000000;
    vga.id = "vga"; vga.bitrate = 9000000;
    const remoteos::Json counters{{"loss", 0.01}, {"queue_ms", 18}, {"streams", remoteos::Json::array({
        {{"id", "vga"}, {"fps", 27}, {"decoded_fps", 29}, {"receive_mbps", 8.5}, {"compressed_queue_drops", 7}},
        {{"id", "internal"}, {"fps", 50}, {"decoded_fps", 58}, {"decode_ms", 11.2},
         {"receiver_timeline_delay_ms", 35.3}, {"presentation_drop_fraction", 0.05},
         {"recovery_drop_fraction", 0.15}, {"receive_mbps", 11.8}, {"compressed_queue_drops", 3}}
    })}};
    auto read_telemetry = [&] {
      std::ifstream input(std::filesystem::path(telemetry_dir) / "remoteos-metrics.csv");
      std::string header, row;
      std::getline(input, header); std::getline(input, row);
      std::map<std::string, std::string> fields;
      std::istringstream headers(header), values(row);
      std::string key, value;
      while (std::getline(headers, key, ',')) {
        require(bool(std::getline(values, value, ',')), "telemetry columns retain their values");
        fields[key] = value;
      }
      require(!std::getline(values, value, ','), "telemetry has no surplus columns");
      return fields;
    };
    telemetry.publish(true, {internal, vga}, counters, 9.5, 0);
    auto fields = read_telemetry();
    require(fields.size() == 32 && fields["internal_bitrate_bps"] == "12000000" &&
            fields["internal_presentation_drop_fraction"] == "0.0500" &&
            fields["internal_recovery_drop_fraction"] == "0.1500" &&
            fields["vga_receive_mbps"] == "8.5000" && fields["internal_fps"] == "50.0000",
            "persist per-display decode/drop/receive stages by ID regardless of stream order");
    struct stat telemetry_info{};
    require(::stat((std::filesystem::path(telemetry_dir) / "remoteos-metrics.csv").c_str(), &telemetry_info) == 0 &&
            (telemetry_info.st_mode & 0777) == 0600, "telemetry remains private");
    telemetry.publish(false, {internal, vga}, counters, -1, 0);
    fields = read_telemetry();
    require(fields["internal_codec"] == "none" && fields["internal_bitrate_bps"] == "0" &&
            fields["internal_receive_mbps"] == "0.0000", "disconnected snapshots never repeat active display counters");
    std::filesystem::remove_all(telemetry_dir);
    if (had_runtime) ::setenv("XDG_RUNTIME_DIR", saved_runtime.c_str(), 1);
    else ::unsetenv("XDG_RUNTIME_DIR");
    auto cpu_path = (std::filesystem::temp_directory_path() / "remoteos-cpu-XXXXXX").string();
    const int cpu_file = mkstemp(cpu_path.data());
    require(cpu_file >= 0, "create CPU counter fixture");
    close(cpu_file);
    StatusReader reader;
    std::ofstream(cpu_path) << "cpu  200 0 0 200 0 0 0 0 100 0\n"
                              "cpu0 100 0 0 100 0 0 0 0 50 0\n"
                              "cpu1 100 0 0 100 0 0 0 0 50 0\n";
    reader.read(cpu_path);
    std::ofstream(cpu_path) << "cpu  300 0 0 300 0 0 0 0 150 0\n"
                              "cpu0 200 0 0 100 0 0 0 0 100 0\n"
                              "cpu1 100 0 0 200 0 0 0 0 50 0\n";
    const auto saturated = reader.read(cpu_path);
    std::filesystem::remove(cpu_path);
    require(std::abs(saturated.cpu_percent - 50) < 0.01 &&
            std::abs(saturated.busiest_cpu_percent - 100) < 0.01,
            "a saturated Atom logical CPU is visible even at 50% aggregate; guest ticks are not counted twice");
    auto setup = SetupState::restore(false, false, 0, false, false, false);
    require(!setup.complete && setup.step == SetupState::Welcome,
            "first boot starts at Welcome");
    require(setup.advance(false) && setup.step == SetupState::Network,
            "welcome advances offline");
    require(setup.advance(false) && setup.step == SetupState::Tailscale,
            "LAN network needs no internet assertion");
    require(!setup.advance(false),
            "Tailscale cannot report readiness before Running");
    setup.use_direct();
    require(
        setup.direct && setup.step == SetupState::Host && !setup.complete,
        "explicit direct connection skips enrollment without completing setup");
    setup = SetupState::restore(true, true, int(setup.step), setup.direct,
                                false, true);
    require(setup.direct && !setup.complete,
            "saved pairing must not complete an in-progress wizard");
    setup.back();
    require(setup.step == SetupState::Tailscale,
            "return from host retains connection choice");
    require(setup.advance(true) && !setup.direct,
            "verified Tailscale replaces explicit direct preference");
    require(!setup.authenticated_session_ready(false, true, true),
            "peer readiness alone cannot complete setup");
    require(!setup.authenticated_session_ready(true, false, true),
            "local media verification alone cannot complete setup");
    require(!setup.authenticated_session_ready(true, true, false),
            "input handoff failure cannot complete setup");
    require(setup.authenticated_session_ready(true, true, true),
            "both media peers and input are ready");
    require(setup.complete && setup.step == SetupState::Host,
            "only ready session completes first run");
    require(SetupState::restore(false, false, 0, false, false, true).complete,
            "existing paired clients retain normal tabs");
    require(!SetupState::restore(true, false, 3, false, true, true).complete,
            "corrupt wizard record fails safe even when a profile exists");
    require(SetupState::restore(true, true, 99, false, true, true).step ==
                SetupState::Welcome,
            "invalid persisted step resets safely");
    const auto browser_return = SetupState::restore(
        true, true, SetupState::Tailscale, false, false, false);
    require(browser_return.step == SetupState::Tailscale &&
                !browser_return.complete,
            "browser return and reboot retain enrollment step");
    for (const auto code : {KEY_KP1, KEY_VOLUMEUP, KEY_BRIGHTNESSUP, KEY_PLAY,
                            KEY_ALS_TOGGLE, KEY_FN_F1})
      require(keyboard_key(code), "recognize keypad/media/WMI keyboard keys");
    for (const auto code : {KEY_RESERVED, KEY_POWER, BTN_LEFT, BTN_JOYSTICK,
                            BTN_TOOL_PEN, BTN_DPAD_UP, BTN_TRIGGER_HAPPY1})
      require(!keyboard_key(code), "exclude local power and button classes");
    require(valid_http_url("https://login.tailscale.com/a?b=c"),
            "HTTPS auth URL");
    require(valid_http_url("http://192.168.1.1/login"), "HTTP captive portal");
    require(!valid_http_url("file:///etc/passwd"), "reject local files");
    require(!valid_http_url("javascript:alert(1)"), "reject script schemes");
    require(!valid_http_url("https://user:password@example.com"),
            "reject embedded credentials");
    require(!valid_http_url("https://example.com/\r\nX-Header: yes"),
            "reject URL control characters");
    require(!valid_http_url("https:///path"), "reject missing authority");
    require(!valid_http_url("https://example.com\\@evil.example"),
            "reject ambiguous authority");
    require(hex_encode("a\"\\\n") == "61225c0a",
            "SSID must use injection-safe hex encoding");
    const auto points = parse_scan_results(
        "bssid / frequency / signal level / flags / ssid\n"
        "00:11:22:33:44:55\t2412\t-60\t[WPA2-PSK-CCMP][ESS]\tOffice\\x20WiFi\n"
        "00:11:22:33:44:66\t2412\t-40\t[WEP][ESS]\tLegacy\n"
        "00:11:22:33:44:77\t2412\t-50\t[ESS]\tOpen\n"
        "00:11:22:33:44:88\t2412\t-80\t[WPA2-EAP-CCMP][ESS]\tEnterprise\n"
        "00:11:22:33:44:99\t2412\t-70\t[RSN-OWE-CCMP][ESS]\tEnhanced\n"
        "bad row\n");
    require(points.size() == 5, "parse scan rows");
    require(points[0].ssid == "Legacy" && !points[0].supported, "reject WEP");
    require(points[1].ssid == "Open" && points[1].supported &&
                !points[1].secure,
            "allow open Wi-Fi");
    require(points[2].ssid == "Office WiFi" && points[2].supported &&
                points[2].secure,
            "decode SSID and allow WPA2 Personal");
    require(!points[3].supported && !points[4].supported,
            "reject OWE and enterprise modes");
    const auto online = parse_wifi_status("wpa_state=COMPLETED\nssid=Office\\x20WiFi\n", "RSSI=-62\n");
    require(online.associated && online.ssid == "Office WiFi" && online.signal_known &&
            online.signal_dbm == -62 && !online.usable,
            "association and signal do not claim DHCP or Internet readiness");
    const auto pending = parse_wifi_status("wpa_state=SCANNING\n", "FAIL\n");
    require(!pending.associated && !pending.signal_known && !pending.usable,
            "scanning service does not imply a connection");
    std::cout << "Client backend tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
