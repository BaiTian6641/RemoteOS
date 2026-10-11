#include "host_app_state.hpp"
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#include <algorithm>
#include <csignal>
#include <chrono>
#include <iostream>
#include <unistd.h>

namespace {
class HostWindow {
public:
  explicit HostWindow(remoteos::app::HostState state) : state_(std::move(state)) {
    window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window_), "RemoteOS Host");
    gtk_window_set_default_size(GTK_WINDOW(window_), 720, 720);
    auto *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 20);
    gtk_container_add(GTK_CONTAINER(window_), outer);
    auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_add(GTK_CONTAINER(scroll), box);
    add_label(box, "RemoteOS Host", "title");
    add_label(box, "Choose the HP desktop", "heading");
    add_label(box, "Keep this application open while using the HP.");
    add_label(box, "Video frame-rate limit", "heading");
    frame_rate_ = gtk_combo_box_text_new();
    gtk_widget_set_name(frame_rate_, "remoteos-frame-rate");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(frame_rate_), "30", "30 fps · responsive desktop");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(frame_rate_), "60", "60 fps · fresh-frame interactive profile");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(frame_rate_), "20", "20 fps · reduce client decoding load");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(frame_rate_), "15", "15 fps · reduce client decoding load");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(frame_rate_), "60");
    gtk_box_pack_start(GTK_BOX(box), frame_rate_, FALSE, FALSE, 0);
    add_label(box, "Network mode", "heading");
    transport_ = gtk_combo_box_text_new();
    gtk_widget_set_name(transport_, "remoteos-media-transport");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(transport_), "rtp-udp", "Trusted LAN / Tailscale · plain UDP media");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(transport_), "dtls-srtp", "Encrypted UDP media · DTLS-SRTP");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(transport_), "rtp-udp");
    gtk_box_pack_start(GTK_BOX(box), transport_, FALSE, FALSE, 0);
    add_label(box, "Display source", "heading");
    source_ = gtk_combo_box_text_new();
    gtk_widget_set_name(source_, "remoteos-desktop-source");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(source_), "independent", "Independent HP desktop · use host simultaneously");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(source_), "monitors", "Share current monitors");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(source_), "kde-native", "Extend current KDE desktop · HP native pixels");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(source_), "virtual", "Virtual display · Wayland portal");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(source_), "independent");
    gtk_box_pack_start(GTK_BOX(box), source_, FALSE, FALSE, 0);
    environment_ = gtk_combo_box_text_new();
    gtk_widget_set_name(environment_, "remoteos-independent-environment");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(environment_), "auto", "Match logged-in desktop · KDE Plasma");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(environment_), "minimal", "Minimal desktop · Openbox");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(environment_), "auto");
    gtk_box_pack_start(GTK_BOX(box), environment_, FALSE, FALSE, 0);
#ifdef GDK_WINDOWING_WAYLAND
    wayland_ = GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default());
#endif
    resolution_label_ = add_label(box, "HP panel resolution", "heading");
    resolution_ = gtk_combo_box_text_new();
    gtk_widget_set_name(resolution_, "remoteos-native-resolution");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(resolution_), "1024x576", "1024 × 576 · HP 2140 standard panel");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(resolution_), "1366x768", "1366 × 768 · HP 2140 HD panel");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(resolution_), "1920x1080", "1920 × 1080 · external display");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(resolution_), "1024x576");
    gtk_box_pack_start(GTK_BOX(box), resolution_, FALSE, FALSE, 0);
    source_help_ = add_label(box, "");
    update_source_help();
    g_signal_connect(source_, "changed", G_CALLBACK(+[](GtkComboBox *, gpointer p) {
      static_cast<HostWindow *>(p)->update_source_help();
    }), this);
    g_signal_connect(environment_, "changed", G_CALLBACK(+[](GtkComboBox *, gpointer p) {
      static_cast<HostWindow *>(p)->update_source_help();
    }), this);
    add_label(box, "Keeps native pixels and adapts frame rate within this limit. A 40 Mbps limit is a ceiling, not a target transmission rate.");
    add_label(box, "Trusted mode removes RemoteOS video/audio encryption. Tailscale still encrypts its tunnel. Pairing and reliable input keep TLS. Older clients use encrypted media.");
    auto *grid = gtk_grid_new(); gtk_grid_set_column_spacing(GTK_GRID(grid), 12); gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Listen address"), 0, 0, 1, 1);
    bind_ = gtk_entry_new(); gtk_entry_set_text(GTK_ENTRY(bind_), "0.0.0.0");
    gtk_grid_attach(GTK_GRID(grid), bind_, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Control port"), 0, 1, 1, 1);
    port_ = gtk_entry_new(); gtk_entry_set_text(GTK_ENTRY(port_), "47010");
    gtk_grid_attach(GTK_GRID(grid), port_, 1, 1, 1, 1);
    audio_ = gtk_check_button_new_with_label("Share playback audio through PipeWire");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(audio_), TRUE);
    gtk_box_pack_start(GTK_BOX(box), audio_, FALSE, FALSE, 0);
    add_label(box, "This computer appears in the HP's host list while sharing. Discovery uses UDP 47009 on the LAN and reachable Tailscale peers. Direct IP or hostname entry also works.");
    add_label(box, "MPEG-2 capture and compression run here. Both client screens share a 40 Mbps connection budget; audio and input have reserved bandwidth.");
    add_label(box, "Sharing the current Wayland desktop requires screen, keyboard and pointer approval. Independent desktop owns its display/input and does not request the local desktop's portal grant.");
    add_label(box, "Application mode needs a logged-in desktop. Login screens and USB passthrough use the separate service edition.");
    // Pairing remains visible while the longer setup guidance scrolls.
    auto *pairing = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(outer), pairing, FALSE, FALSE, 0);
    source_status_ = add_label(pairing, "No display shared.");
    add_label(pairing, "Verification value · compare on the HP", "heading");
    verification_ = add_label(pairing, "—", "mono");
    gtk_label_set_selectable(GTK_LABEL(verification_), TRUE);
    add_label(pairing, "Six-digit pairing PIN · one use · expires after 10 minutes", "heading");
    code_ = add_label(pairing, "—", "mono");
    gtk_label_set_selectable(GTK_LABEL(code_), TRUE);
    auto *identity_details = gtk_expander_new("Full host fingerprint for manual connection");
    gtk_box_pack_start(GTK_BOX(pairing), identity_details, FALSE, FALSE, 0);
    fingerprint_ = gtk_label_new("Start sharing to create the host identity.");
    gtk_label_set_xalign(GTK_LABEL(fingerprint_), 0);
    gtk_label_set_line_wrap(GTK_LABEL(fingerprint_), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(fingerprint_), PANGO_WRAP_CHAR);
    gtk_label_set_selectable(GTK_LABEL(fingerprint_), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(fingerprint_), "mono");
    gtk_container_add(GTK_CONTAINER(identity_details), fingerprint_);
    auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(outer), row, FALSE, FALSE, 0);
    start_ = gtk_button_new_with_label("Start sharing");
    gtk_style_context_add_class(gtk_widget_get_style_context(start_), "primary");
    stop_ = gtk_button_new_with_label("Stop sharing"); gtk_widget_set_sensitive(stop_, FALSE);
    gtk_box_pack_start(GTK_BOX(row), start_, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), stop_, TRUE, TRUE, 0);
    status_ = add_label(outer, "Sharing is off.");
    resources_ = add_label(outer, "Webcam and SD sharing are off. Camera: add the provided URL as an OBS Browser Source, then Start Virtual Camera.");
    gtk_label_set_selectable(GTK_LABEL(resources_), TRUE);
    g_signal_connect(start_, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) { static_cast<HostWindow *>(p)->start(); }), this);
    g_signal_connect(stop_, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) { static_cast<HostWindow *>(p)->stop(); }), this);
    g_signal_connect(window_, "delete-event", G_CALLBACK(+[](GtkWidget *, GdkEvent *, gpointer p) -> gboolean {
      auto *self = static_cast<HostWindow *>(p);
      if (self->process_ || self->export_pending_) { self->quitting_ = true; self->stop(); }
      else gtk_main_quit();
      return TRUE;
    }), this);
    gtk_widget_show_all(window_);
    update_source_help();
    gtk_widget_grab_focus(wayland_ ? source_ : start_);
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll)), 0);
  }
private:
  static GtkWidget *add_label(GtkWidget *box, const char *text, const char *style = "") {
    auto *item = gtk_label_new(text); gtk_label_set_xalign(GTK_LABEL(item), 0);
    gtk_label_set_line_wrap(GTK_LABEL(item), TRUE); gtk_label_set_line_wrap_mode(GTK_LABEL(item), PANGO_WRAP_WORD_CHAR);
    gtk_style_context_add_class(gtk_widget_get_style_context(item), style);
    gtk_box_pack_start(GTK_BOX(box), item, FALSE, FALSE, 0); return item;
  }
  void message(const std::string& text) { gtk_label_set_text(GTK_LABEL(status_), text.c_str()); }
  std::string desktop_source() const {
    const auto *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(source_));
    return id ? id : "monitors";
  }
  void update_source_help() {
    const bool independent = desktop_source() == "independent";
    const auto *environment = gtk_combo_box_get_active_id(GTK_COMBO_BOX(environment_));
    const bool minimal = independent && environment && std::string(environment) == "minimal";
    gtk_widget_set_sensitive(environment_, independent && !process_);
    const bool native = independent || (wayland_ && desktop_source() == "kde-native");
    if (stop_) gtk_button_set_label(GTK_BUTTON(stop_), independent ? "Stop desktop · closes apps" : "Stop sharing");
    gtk_widget_set_visible(resolution_label_, native); gtk_widget_set_visible(resolution_, native);
    const auto text = minimal
      ? "Starts a private Openbox desktop with separate display, input, app settings, session bus and audio. Apps use the same Unix account and files. Both HP screens use native pixels. Stop desktop closes its apps; save your work first."
      : independent
      ? "Runs a separate KDE Plasma desktop with your theme, wallpaper, panels and widgets copied into private settings. Input, windows, audio and the session bus belong to the HP desktop. Apps use the same Unix account and files. Both HP screens use native pixels. Stop desktop closes its apps; save your work first. Other environments can explicitly choose Minimal desktop."
      : !wayland_
      ? "Xorg shares existing monitors. Match host resolution on the HP uses RandR; the previous display layout is restored on disconnect."
      : native
        ? "Extends the current KDE desktop at the selected HP resolution with 100% scale. Input belongs to the same desktop session. Keep Match host resolution enabled on the HP."
        : desktop_source() == "virtual"
        ? "Adds a virtual monitor to the current desktop through the permission portal. Plasma 6.5 creates it at 1920×1080. The portal controls its size. This uses the local session's input."
        : "Shares existing host screens and their input. For exact matching, set their resolution to the HP's size in the host display settings.";
    gtk_label_set_text(GTK_LABEL(source_help_), text);
  }
  void display_status(const remoteos::Json& value) {
    const auto source = value.value("desktop_source", "monitors");
    const bool native = source == "kde-native";
    std::string text = source == "independent" ? "Independent HP desktop · native pixels: " : native ? "KDE extension · 100% scale: " : source == "virtual" ? "Virtual display capture: " : "Monitor capture: ";
    const auto displays = value.value("source_displays", remoteos::Json::array());
    if (displays.empty()) text += "desktop ready";
    for (std::size_t i = 0; i < displays.size(); ++i) {
      if (i) text += " + ";
      text += std::to_string(displays[i].value("width", 0u)) + "×" + std::to_string(displays[i].value("height", 0u));
    }
    gtk_label_set_text(GTK_LABEL(source_status_), text.c_str());
  }
  void read_line() {
    g_data_input_stream_read_line_async(input_, G_PRIORITY_DEFAULT, nullptr,
      +[](GObject *object, GAsyncResult *result, gpointer p) {
        auto *self = static_cast<HostWindow *>(p); GError *error = nullptr; gsize size = 0;
        auto *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(object), result, &size, &error);
        if (self->input_ != G_DATA_INPUT_STREAM(object)) {
          g_free(line); g_clear_error(&error); return;
        }
        if (line) {
          std::string text(line, std::min<gsize>(size, 16384));
          try {
            const auto value = remoteos::Json::parse(text);
            const auto event = value.value("event", "");
            if (event == "preparing") self->message(self->desktop_source() == "independent" ? "Preparing the independent HP desktop…" : "Preparing desktop sharing. On Wayland, approve screen, keyboard and pointer access in the permission dialog.");
            else if (event == "listening") {
              self->display_status(value);
              gtk_label_set_text(GTK_LABEL(self->code_), self->state_.code.c_str());
              self->message("Ready for the paired HP. Control port " + std::to_string(value.value("port", 47010)) + "; media uses UDP 47020–47022.");
            }
            else if (event == "session-started") self->message("Sharing " + std::to_string(value.value("monitors", 1)) + " monitor(s) · MPEG-2 · " + std::to_string(value.value("bandwidth_limit_bps", remoteos::default_bandwidth_bps) / 1000000) + " Mbps connection limit.");
            else if(event=="resource-status") {
              std::string text="Host-only device addresses";
              for(const auto& item:value.value("resources",remoteos::Json::array()))text+="\n"+item.value("resource",std::string{})+": "+item.value("url",std::string{});
              gtk_label_set_text(GTK_LABEL(self->resources_),text.c_str());
            }
            else if (event == "session-performance") {
              std::string report;
              for (const auto &decision : value.value("adaptation", remoteos::Json::array())) {
                if (!report.empty()) report += "\n";
                report += decision.value("id", "Display") + ": requested " + std::to_string(decision.value("requested_fps", 0)) +
                    " · sending " + std::to_string(decision.value("effective_fps", 0)) + " fps · " + decision.value("reason", "");
              }
              if (!report.empty()) self->message(report);
            }
            else if (event == "session-stopped") self->message("Client disconnected. Ready for another connection.");
            else if (event == "sharing-failed") { self->child_error_ = value.value("message", "Desktop sharing ended"); self->message(self->child_error_); }
          } catch (...) { if (text.find("remoteos-host:") != std::string::npos) { self->child_error_ = text; self->message(text); } }
          g_free(line);
          if (self->process_ && self->input_ == G_DATA_INPUT_STREAM(object)) self->read_line();
        } else {
          self->output_finished_ = true;
          if (self->process_finished_) self->finish_process();
        }
        g_clear_error(&error);
      }, this);
  }
  void start() {
    if (process_ || export_pending_) return;
#ifdef GDK_WINDOWING_WAYLAND
    auto *native = gtk_widget_get_window(window_);
    if (desktop_source() != "independent" && GDK_IS_WAYLAND_WINDOW(native)) {
      export_pending_ = true; gtk_widget_set_sensitive(start_, FALSE); gtk_widget_set_sensitive(stop_, TRUE);
      message("Preparing the desktop permission dialog…");
      if (gdk_wayland_window_export_handle(native, +[](GdkWindow *, const char *handle, gpointer p) {
        auto *self = static_cast<HostWindow *>(p); self->export_pending_ = false; self->exported_ = true;
        if (self->export_canceled_) {
          self->unexport(); self->export_canceled_ = false;
          gtk_widget_set_sensitive(self->start_, TRUE); gtk_widget_set_sensitive(self->stop_, FALSE);
          self->message("Sharing is off."); if (self->quitting_) gtk_main_quit(); return;
        }
        self->start_host("wayland:" + std::string(handle));
      }, this, nullptr)) return;
      // Empty parent is valid on compositors without xdg-foreign export.
      export_pending_ = false;
    }
#endif
    start_host("");
  }
  void unexport() {
#ifdef GDK_WINDOWING_WAYLAND
    if (exported_) { gdk_wayland_window_unexport_handle(gtk_widget_get_window(window_)); exported_ = false; }
#endif
  }
  void start_host(const std::string& parent) {
    try {
      if (geteuid() == 0) throw std::runtime_error("Open the host application as your normal desktop user");
      const auto *resolution = gtk_combo_box_get_active_id(GTK_COMBO_BOX(resolution_));
      const auto *rate = gtk_combo_box_get_active_id(GTK_COMBO_BOX(frame_rate_));
      const auto *transport = gtk_combo_box_get_active_id(GTK_COMBO_BOX(transport_));
      const std::string size = resolution ? resolution : "1024x576";
      const auto config = state_.prepare(gtk_entry_get_text(GTK_ENTRY(bind_)),
        remoteos::app::parse_port(gtk_entry_get_text(GTK_ENTRY(port_))), gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(audio_)), parent, desktop_source(),
        size == "1366x768" ? 1366u : size == "1920x1080" ? 1920u : 1024u,
        size == "1366x768" ? 768u : size == "1920x1080" ? 1080u : 576u,
        rate ? unsigned(std::stoi(rate)) : 60u, transport ? transport : "rtp-udp",
        gtk_combo_box_get_active_id(GTK_COMBO_BOX(environment_)) ? gtk_combo_box_get_active_id(GTK_COMBO_BOX(environment_)) : "auto");
      GError *error = nullptr;
      if (desktop_source() == "independent") {
        const auto launcher = std::filesystem::read_symlink("/proc/self/exe").parent_path() / "remoteos-independent-desktop.py";
        if (!std::filesystem::is_regular_file(launcher)) throw std::runtime_error("Place remoteos-independent-desktop.py beside the host executable");
        process_ = g_subprocess_new(static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE),
          &error, "python3", launcher.c_str(), "--host", state_.executable.c_str(), "--config", config.c_str(), nullptr);
      } else process_ = g_subprocess_new(static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE),
                                  &error, state_.executable.c_str(), "--config", config.c_str(), "--desktop", nullptr);
      if (!process_) { const auto why = std::string(error ? error->message : "Cannot start the host"); g_clear_error(&error); throw std::runtime_error(why); }
      gtk_label_set_text(GTK_LABEL(fingerprint_), state_.fingerprint.c_str());
      const auto fp = state_.fingerprint.substr(0, 12);
      gtk_label_set_text(GTK_LABEL(verification_), (fp.substr(0, 4) + " " + fp.substr(4, 4) + " " + fp.substr(8, 4)).c_str());
      gtk_label_set_text(GTK_LABEL(code_), desktop_source() == "independent" ? "Starting independent desktop…" : "Awaiting desktop permission…");
      gtk_widget_set_sensitive(start_, FALSE); gtk_widget_set_sensitive(stop_, TRUE);
      gtk_widget_set_sensitive(bind_, FALSE); gtk_widget_set_sensitive(port_, FALSE); gtk_widget_set_sensitive(audio_, FALSE);
      gtk_widget_set_sensitive(source_, FALSE);
      gtk_widget_set_sensitive(environment_, FALSE);
      gtk_widget_set_sensitive(resolution_, FALSE);
      gtk_widget_set_sensitive(frame_rate_, FALSE);
      gtk_widget_set_sensitive(transport_, FALSE);
      stopping_ = false; process_finished_ = false; output_finished_ = false;
      child_error_.clear(); message("Starting desktop sharing…");
      input_ = g_data_input_stream_new(g_subprocess_get_stdout_pipe(process_)); read_line();
      g_subprocess_wait_async(process_, nullptr, +[](GObject *object, GAsyncResult *result, gpointer p) {
        auto *self = static_cast<HostWindow *>(p); GError *error = nullptr;
        g_subprocess_wait_finish(G_SUBPROCESS(object), result, &error); g_clear_error(&error);
        if (self->stop_timer_) { g_source_remove(self->stop_timer_); self->stop_timer_ = 0; }
        self->process_finished_ = true;
        // Drain final JSON/error lines before resetting the window; a denied
        // portal request can make the child exit before stdout is dispatched.
        if (self->output_finished_) self->finish_process();
      }, this);
    } catch (const std::exception& error) {
      unexport(); gtk_widget_set_sensitive(start_, TRUE); gtk_widget_set_sensitive(stop_, FALSE); message(error.what());
    }
  }
  void finish_process() {
    const bool normal = stopping_;
    g_clear_object(&input_); g_clear_object(&process_); unexport();
    gtk_widget_set_sensitive(start_, TRUE); gtk_widget_set_sensitive(stop_, FALSE);
    gtk_widget_set_sensitive(bind_, TRUE); gtk_widget_set_sensitive(port_, TRUE); gtk_widget_set_sensitive(audio_, TRUE);
    gtk_widget_set_sensitive(source_, TRUE);
    gtk_widget_set_sensitive(environment_, desktop_source() == "independent");
    gtk_widget_set_sensitive(resolution_, TRUE);
    gtk_widget_set_sensitive(frame_rate_, TRUE);
    gtk_widget_set_sensitive(transport_, TRUE);
    gtk_label_set_text(GTK_LABEL(source_status_), "No display shared.");
    gtk_label_set_text(GTK_LABEL(code_), "—");
    if (normal) message("Sharing is off.");
    else if (!child_error_.empty()) message(child_error_);
    else message("The host stopped. Check the listen address, desktop session and media dependencies.");
    if (quitting_) gtk_main_quit();
  }
  void stop() {
    if (export_pending_) { export_canceled_ = true; message("Canceling desktop sharing…"); return; }
    if (!process_ || stopping_) return;
    stopping_ = true; message("Stopping sharing and releasing remote input…");
    g_subprocess_send_signal(process_, SIGTERM);
    stop_timer_ = g_timeout_add_seconds(5, +[](gpointer p) -> gboolean {
      auto *self = static_cast<HostWindow *>(p);
      self->stop_timer_ = 0;
      if (self->process_ && self->stopping_) g_subprocess_force_exit(self->process_);
      return G_SOURCE_REMOVE;
    }, this);
  }
  remoteos::app::HostState state_;
  GtkWidget *window_{}, *bind_{}, *port_{}, *audio_{}, *source_{}, *environment_{}, *resolution_{}, *frame_rate_{}, *transport_{}, *resolution_label_{}, *source_help_{}, *source_status_{}, *fingerprint_{}, *verification_{}, *code_{}, *start_{}, *stop_{}, *status_{}, *resources_{};
  GSubprocess *process_ = nullptr;
  GDataInputStream *input_ = nullptr;
  guint stop_timer_ = 0;
  bool stopping_ = false, quitting_ = false, wayland_ = false;
  bool export_pending_ = false, export_canceled_ = false, exported_ = false;
  bool process_finished_ = false, output_finished_ = false;
  std::string child_error_;
};
}
int main(int argc, char **argv) {
  try {
    std::filesystem::path state, host = std::filesystem::read_symlink("/proc/self/exe").parent_path() / "remoteos-host";
    for (int i = 1; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--state-dir" && i + 1 < argc) state = argv[++i];
      else if (argument == "--host" && i + 1 < argc) host = argv[++i];
      else if (argument == "--help") { std::cout << "remoteos-host-app [--state-dir DIR] [--host EXECUTABLE]\n"; return 0; }
      else throw std::runtime_error("Unknown host application argument");
    }
    g_set_application_name("RemoteOS Host");
    argc = 1; gtk_init(&argc, &argv);
    auto *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
      "* {font-family:'IBM Plex Sans',sans-serif;font-size:15px;} window {background:#161616;color:#f4f4f4;}"
      ".title {font-size:28px;} .heading {font-size:18px;font-weight:600;} .mono {font-family:monospace;}"
      "button {background-image:none;background:#393939;color:#f4f4f4;border-radius:0;padding:12px;}"
      "button.primary {background:#0f62fe;} button:focus,entry:focus {outline:2px solid white;}"
      "entry {background:#393939;color:#f4f4f4;border-radius:0;padding:8px;}"
      "combobox button {padding:8px;} combobox menu {background:#262626;color:#f4f4f4;}", -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    HostWindow window(remoteos::app::HostState(host, state)); gtk_main(); return 0;
  } catch (const std::exception& error) { std::cerr << "RemoteOS Host: " << error.what() << '\n'; return 1; }
}
