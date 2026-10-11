#include "backend.hpp"
#include "input.hpp"
#include "cursor.hpp"
#include "fullscreen.hpp"
#include "pointer.hpp"
#include "relative_mouse.hpp"
#include "power_policy.hpp"
#include "remoteos/client_session.hpp"
#include "remoteos/devices.hpp"
#include "remoteos/discovery.hpp"
#include "remoteos/identity.hpp"
#include "remoteos/media.hpp"
#include "setup_state.hpp"
#include "telemetry.hpp"
#include "usb_tunnels.hpp"
#include "resource_tunnels.hpp"
#include "remoteos/resources.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gdk/gdkx.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <iomanip>
#include <iostream>
#include <linux/input.h>
#include <map>
#include <memory>
#include <openssl/crypto.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef REMOTEOS_ASSET_DIR
#define REMOTEOS_ASSET_DIR "/usr/share/remoteos"
#endif
namespace {
using remoteos::Json;
using namespace remoteos::client;
using Work = std::function<std::string()>;
using Done = std::function<void(const std::string &, const std::string &)>;
struct WorkItem {
  Work work;
  Done done;
};
void background(Work work, Done done) {
  auto *item = new WorkItem{std::move(work), std::move(done)};
  auto *task = g_task_new(
      nullptr, nullptr,
      +[](GObject *, GAsyncResult *result, gpointer) {
        auto *task = G_TASK(result);
        auto *item = static_cast<WorkItem *>(g_task_get_task_data(task));
        GError *error = nullptr;
        auto *value =
            static_cast<std::string *>(g_task_propagate_pointer(task, &error));
        item->done(value ? *value : std::string{},
                   error ? error->message : std::string{});
        delete value;
        if (error)
          g_error_free(error);
      },
      nullptr);
  g_task_set_task_data(
      task, item, +[](gpointer data) { delete static_cast<WorkItem *>(data); });
  g_task_run_in_thread(
      task, +[](GTask *task, gpointer, gpointer data, GCancellable *) {
        try {
          g_task_return_pointer(
              task, new std::string(static_cast<WorkItem *>(data)->work()),
              +[](gpointer p) { delete static_cast<std::string *>(p); });
        } catch (const std::exception &e) {
          g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                                  e.what());
        }
      });
  g_object_unref(task);
}
void command(const std::vector<std::string> &args, Done done) {
  std::vector<const char *> argv;
  for (const auto &s : args)
    argv.push_back(s.c_str());
  argv.push_back(nullptr);
  GError *error = nullptr;
  auto *process = g_subprocess_newv(
      argv.data(),
      static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                    G_SUBPROCESS_FLAGS_STDERR_PIPE),
      &error);
  if (!process) {
    done({}, error ? error->message : "Cannot start service command");
    g_clear_error(&error);
    return;
  }
  auto *callback = new Done(std::move(done));
  g_subprocess_communicate_utf8_async(
      process, nullptr, nullptr,
      +[](GObject *object, GAsyncResult *result, gpointer user) {
        auto *cb = static_cast<Done *>(user);
        auto *process = G_SUBPROCESS(object);
        gchar *out = nullptr, *err = nullptr;
        GError *error = nullptr;
        const bool communicated = g_subprocess_communicate_utf8_finish(
            process, result, &out, &err, &error);
        (*cb)(out ? out : "", !communicated
                                  ? (error ? error->message : "Command failed")
                              : !g_subprocess_get_successful(process)
                                  ? (err ? err : "Command failed")
                                  : "");
        g_free(out);
        g_free(err);
        g_clear_error(&error);
        delete cb;
        g_object_unref(process);
      },
      callback);
}
void on_main(std::function<void()> fn) {
  auto *callback = new std::function<void()>(std::move(fn));
  g_main_context_invoke(
      nullptr,
      +[](gpointer p) -> gboolean {
        std::unique_ptr<std::function<void()>> fn(
            static_cast<std::function<void()> *>(p));
        (*fn)();
        return G_SOURCE_REMOVE;
      },
      callback);
}
GtkWidget *label(const char *text, const char *style = nullptr) {
  auto *widget = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(widget), 0);
  gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
  if (style)
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), style);
  return widget;
}
GtkWidget *button(GtkWidget *box, const char *text, std::function<void()> cb,
                  const char *style = nullptr) {
  auto *widget = gtk_button_new_with_label(text);
  if (style)
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), style);
  auto *callback = new std::function<void()>(std::move(cb));
  g_signal_connect_data(
      widget, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) {
        (*static_cast<std::function<void()> *>(p))();
      }),
      callback,
      +[](gpointer p, GClosure *) {
        delete static_cast<std::function<void()> *>(p);
      },
      G_CONNECT_DEFAULT);
  gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
  return widget;
}
GtkWidget *entry(GtkWidget *box, const char *title, const char *placeholder,
                 bool secret = false) {
  gtk_box_pack_start(GTK_BOX(box), label(title), FALSE, FALSE, 0);
  auto *widget = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(widget), placeholder);
  gtk_entry_set_visibility(GTK_ENTRY(widget), !secret);
  gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
  return widget;
}
std::string text(GtkWidget *entry) {
  return gtk_entry_get_text(GTK_ENTRY(entry));
}
std::string supervisor(const Json &message) {
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    throw std::runtime_error("Cannot create supervisor connection");
  struct Close {
    int fd;
    ~Close() { ::close(fd); }
  } closer{fd};
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, "/run/remoteos/supervisor.sock",
               sizeof(addr.sun_path) - 1);
  // USB bind/unbind helpers have a 15 second bound. This call always runs on
  // a background task, leaving local input and the power overlay responsive.
  timeval timeout{20, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    throw std::runtime_error("Client supervisor is unavailable");
  const auto request = message.dump() + "\n";
  std::size_t offset = 0;
  while (offset < request.size()) {
    const auto count = send(fd, request.data() + offset,
                            request.size() - offset, MSG_NOSIGNAL);
    if (count < 1)
      throw std::runtime_error("Supervisor request failed");
    offset += static_cast<std::size_t>(count);
  }
  std::string response;
  char buffer[4096];
  while (response.find('\n') == std::string::npos) {
    const auto count = recv(fd, buffer, sizeof(buffer), 0);
    if (count < 1)
      throw std::runtime_error("Client supervisor did not respond");
    response.append(buffer, static_cast<std::size_t>(count));
    if (response.size() > 65536)
      throw std::runtime_error("Supervisor response exceeded size limit");
  }
  const auto result = Json::parse(response);
  if (!result.value("ok", false))
    throw std::runtime_error(
        result.value("error", std::string("Supervisor operation failed")));
  return response;
}
std::string supervisor(const char *action) {
  return supervisor(Json{{"action", action}});
}
class Shell {
#ifdef REMOTEOS_CLIENT_UI_TEST
  friend struct ClientUiFixture;
#endif
public:
  Shell(std::string state, bool windowed, bool input, bool automatic, bool discovery = true, bool desktop = false)
      : state_(std::move(state)), desktop_input_(desktop), windowed_(windowed), discovery_enabled_(discovery) {
    media_.set_desktop_output(desktop);
    window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window_), 1024, 700);
    gtk_window_set_title(GTK_WINDOW(window_), "RemoteOS");
    g_signal_connect(
        window_, "delete-event",
        G_CALLBACK(+[](GtkWidget *, GdkEvent *, gpointer p) -> gboolean {
          static_cast<Shell *>(p)->stop();
          gtk_main_quit();
          return TRUE;
        }),
        this);
    auto *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window_), column);
    auto *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    header_ = header;
    gtk_style_context_add_class(gtk_widget_get_style_context(header),
                                "remoteos-header");
    gtk_box_pack_start(GTK_BOX(column), header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), label("RemoteOS", "remoteos-title"),
                       TRUE, TRUE, 0);
    network_button_ = gtk_button_new();
    auto *network_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    network_icon_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(network_icon_, 24, 24);
    network_label_ = gtk_label_new("Checking network…");
    gtk_label_set_ellipsize(GTK_LABEL(network_label_), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(network_label_), 18);
    gtk_box_pack_start(GTK_BOX(network_box), network_icon_, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(network_box), network_label_, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(network_button_), network_box);
    gtk_box_pack_start(GTK_BOX(header), network_button_, FALSE, FALSE, 0);
    g_signal_connect(network_button_, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) {
      static_cast<Shell *>(p)->open_network_settings();
    }), this);
    g_signal_connect(network_icon_, "draw", G_CALLBACK(+[](GtkWidget *, cairo_t *cr, gpointer p) -> gboolean {
      auto *self = static_cast<Shell *>(p);
      const auto &net = self->network_status_;
      cairo_set_line_width(cr, 2);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      if (net.usable) cairo_set_source_rgb(cr, 0.96, 0.96, 0.96);
      else cairo_set_source_rgb(cr, 0.66, 0.66, 0.66);
      if (net.wired) {
        cairo_rectangle(cr, 4, 3, 16, 11); cairo_stroke(cr);
        cairo_move_to(cr, 12, 14); cairo_line_to(cr, 12, 20);
        cairo_move_to(cr, 6, 20); cairo_line_to(cr, 18, 20); cairo_stroke(cr);
      } else {
        const int bars = !net.associated ? 0 : !net.signal_known ? 3 :
                         net.signal_dbm >= -55 ? 3 : net.signal_dbm >= -70 ? 2 : 1;
        for (int level = 1; level <= 3; ++level) {
          cairo_set_source_rgb(cr, level <= bars ? 0.96 : 0.39,
                                   level <= bars ? 0.96 : 0.39,
                                   level <= bars ? 0.96 : 0.39);
          cairo_arc(cr, 12, 21, level * 5, 3.9269908, 5.4977871); cairo_stroke(cr);
        }
        cairo_arc(cr, 12, 21, 1, 0, 6.2831853); cairo_fill(cr);
        if (!net.associated) {
          cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
          cairo_move_to(cr, 4, 4); cairo_line_to(cr, 21, 21); cairo_stroke(cr);
        }
      }
      return TRUE;
    }), this);
    diagnostics_button_ =
        button(header, "Diagnostics", [this] { toggle_diagnostics(); });
    button(header, "Client menu", [this] { toggle_overlay(); });
    ephemeral_warning_ =
        label("This session is using temporary storage. Saved pairing and "
              "network settings will be lost after power off.",
              "remoteos-warning");
    gtk_widget_set_margin_start(ephemeral_warning_, 24);
    gtk_widget_set_margin_end(ephemeral_warning_, 24);
    gtk_widget_set_margin_top(ephemeral_warning_, 8);
    gtk_widget_set_margin_bottom(ephemeral_warning_, 8);
    gtk_widget_set_no_show_all(ephemeral_warning_, TRUE);
    gtk_box_pack_start(GTK_BOX(column), ephemeral_warning_, FALSE, FALSE, 0);
    if (ephemeral_state())
      gtk_widget_show(ephemeral_warning_);
    stack_ = gtk_stack_new();
    // Only the visible setup page determines the local window's minimum size.
    gtk_stack_set_homogeneous(GTK_STACK(stack_), FALSE);
    gtk_box_pack_start(GTK_BOX(column), stack_, TRUE, TRUE, 0);
    auto *setup_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_stack_add_named(GTK_STACK(stack_), setup_box, "setup");
    progress_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_set_homogeneous(GTK_BOX(progress_), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(progress_),
                                "remoteos-progress");
    const char *steps[] = {"1  Welcome", "2  Network", "3  Connection",
                           "4  Host pairing"};
    for (const auto *step : steps) {
      auto *item = label(step);
      gtk_style_context_add_class(gtk_widget_get_style_context(item),
                                  "remoteos-step");
      gtk_box_pack_start(GTK_BOX(progress_), item, TRUE, TRUE, 0);
      step_labels_.push_back(item);
    }
    gtk_box_pack_start(GTK_BOX(setup_box), progress_, FALSE, FALSE, 0);
    notebook_ = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(setup_box), notebook_, TRUE, TRUE, 0);
    welcome_page(notebook_);
    network_page(notebook_);
    host_page(notebook_);
    diagnostics_page(notebook_);
    footer_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(footer_),
                                "remoteos-setup-footer");
    gtk_box_pack_start(GTK_BOX(setup_box), footer_, FALSE, FALSE, 0);
    back_button_ = button(footer_, "Back", [this] {
      setup_.back();
      change_setup_step();
    });
    direct_button_ = button(footer_, "Use a direct connection", [this] {
      setup_.use_direct();
      change_setup_step();
      say("Direct connection selected. The laptop and host need a reachable "
          "network; internet is optional.");
    });
    auto *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(footer_), spacer, TRUE, TRUE, 0);
    next_button_ = button(
        footer_, "Get started", [this] { next_setup_step(); }, "primary");
    // Keep the video drawable out of the setup hierarchy. Even hidden local
    // controls, theme requisitions or a remapped setup window must not take
    // pixels away from the remote desktop.
    session_window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(session_window_), "RemoteOS remote desktop");
    gtk_window_set_decorated(GTK_WINDOW(session_window_), windowed_);
    g_signal_connect(session_window_, "delete-event",
                     G_CALLBACK(+[](GtkWidget *, GdkEvent *, gpointer p) -> gboolean {
                       static_cast<Shell *>(p)->stop();
                       return TRUE;
                     }), this);
    video_ = gtk_drawing_area_new();
    gtk_widget_set_hexpand(video_, TRUE);
    gtk_widget_set_vexpand(video_, TRUE);
    gtk_container_add(GTK_CONTAINER(session_window_), video_);
    g_signal_connect(video_, "size-allocate", G_CALLBACK(+[](GtkWidget*, GtkAllocation* allocation, gpointer p) {
      auto* self = static_cast<Shell*>(p);
      if (self->viewport_width_ == allocation->width && self->viewport_height_ == allocation->height) return;
      self->viewport_width_ = allocation->width;
      self->viewport_height_ = allocation->height;
      if (self->connected_) self->schedule_session_fullscreen();
    }), this);
    pointer_events(video_);
    cursor_ = std::make_unique<SessionCursor>(gdk_display_get_default(), !desktop_input_);
    if (desktop_input_) {
      gtk_widget_add_events(session_window_, GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK | GDK_FOCUS_CHANGE_MASK);
      g_signal_connect(session_window_, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* e, gpointer p) -> gboolean {
        return static_cast<Shell*>(p)->desktop_key(e);
      }), this);
      g_signal_connect(session_window_, "key-release-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* e, gpointer p) -> gboolean {
        return static_cast<Shell*>(p)->desktop_key(e);
      }), this);
      g_signal_connect(session_window_, "focus-out-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer p) -> gboolean {
        auto* self = static_cast<Shell*>(p);
        self->relative_mouse_.stop(); self->release_all(); return FALSE;
      }), this);
      g_signal_connect(session_window_, "focus-in-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer p) -> gboolean {
        static_cast<Shell*>(p)->update_mouse_capture(); return FALSE;
      }), this);
    }
    status_ =
        label("Set up a connection to your remote desktop.", "remoteos-status");
    gtk_box_pack_start(GTK_BOX(column), status_, FALSE, FALSE, 0);
    build_overlay();
    if (input)
      broker_ = std::make_unique<InputBroker>(
          [this](InputEvent e) { input_event(e); },
          [this] { toggle_overlay(); }, [this] { release_all(); },
          [this](const std::string &error) {
            stop();
            say("Input forwarding stopped: " + error);
          }, [this](bool closed) {
            if (closed && !lid_closed_) suspend_client();
            lid_closed_ = closed;
          });
    load_profile();
    refresh_host_list();
    load_setup();
    if(desktop_input_&&!setup_.complete){setup_.step=SetupState::Host;setup_.direct=true;}
    gtk_widget_show_all(window_);
    update_setup();
    gtk_expander_set_expanded(GTK_EXPANDER(host_options_), setup_.complete);
    if (setup_.complete)
      gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook_), 3);
    gtk_widget_hide(overlay_);
    if (!windowed_) {
      auto *display = gdk_display_get_default();
      auto *monitor = gdk_display_get_primary_monitor(display);
      if (!monitor)
        monitor = gdk_display_get_monitor(display, 0);
      fullscreen_window(window_, monitor);
    }
    g_timeout_add_seconds(
        1,
        +[](gpointer p) -> gboolean {
          static_cast<Shell *>(p)->refresh_status();
          return G_SOURCE_CONTINUE;
        },
        this);
    g_timeout_add_seconds(
        5,
        +[](gpointer p) -> gboolean {
          static_cast<Shell *>(p)->tailscale_status();
          return G_SOURCE_CONTINUE;
        },
        this);
    g_signal_connect(gdk_display_get_default(), "monitor-added",
                     G_CALLBACK(+[](GdkDisplay *, GdkMonitor *, gpointer p) {
                       static_cast<Shell *>(p)->monitors_changed();
                     }),
                     this);
    g_signal_connect(gdk_display_get_default(), "monitor-removed",
                     G_CALLBACK(+[](GdkDisplay *, GdkMonitor *, gpointer p) {
                       static_cast<Shell *>(p)->monitors_changed();
                     }),
                     this);
    startup_auto_connect_ = automatic && auto_connect_ && setup_.complete;
    startup_check_ = automatic;
    g_timeout_add_seconds(2, +[](gpointer p) -> gboolean {
      static_cast<Shell *>(p)->network_status();
      return G_SOURCE_CONTINUE;
    }, this);
    network_status();
    g_timeout_add_seconds(10, +[](gpointer p) -> gboolean {
      static_cast<Shell *>(p)->discover(); return G_SOURCE_CONTINUE;
    }, this);
    discover();
  }
  ~Shell() { stop(); }

private:
  std::string state_, wireless_,
      tailscale_state_ = "Not checked", portal_url_, host_label_,
      tailscale_route_ = "No matching Tailscale peer";
  double latency_ms_ = -1;
  bool desktop_input_ = false;
  std::string tls_cipher_;
  bool host_usb_ = false, host_resources_=false;
  ResourceTunnels resource_tunnels_;
  std::shared_ptr<remoteos::devices::CameraExport> camera_export_;
  std::shared_ptr<remoteos::devices::FileShare> file_share_;
  std::string camera_cap_,file_cap_,resource_report_="Device forwarding is off.";
  GtkWidget *resources_window_=nullptr,*resource_status_=nullptr,*sd_entry_=nullptr,*sd_write_=nullptr;
  bool camera_busy_=false,sd_mounted_=false;
  std::string audio_status_ = "Not connected";
  bool recording_ = false;
  std::string recording_status_;
  bool windowed_, auto_connect_ = false, connected_ = false,
                  overlay_open_ = false, tail_busy_ = false,
                  network_busy_ = false, connecting_ = false;
  guint fullscreen_idle_ = 0;
  int viewport_width_ = 0, viewport_height_ = 0;
  bool lid_closed_ = false, suspending_ = false;
  BatteryAlerts battery_alerts_;
  GtkWidget* battery_notice_{};
  SetupState setup_;
  bool diagnostics_open_ = false, tailscale_running_ = false,
       enrollment_busy_ = false, existing_pair_ = false,
       local_media_verified_ = false, peer_media_ready_ = false,
       input_handoff_ready_ = false;
  std::uint64_t setup_operation_ = 0;
  GtkWidget *notebook_{}, *progress_{}, *footer_{}, *back_button_{},
      *next_button_{}, *direct_button_{}, *diagnostics_button_{},
      *host_actions_{}, *tailscale_action_{}, *wifi_action_{}, *host_options_{};
  std::vector<GtkWidget *> step_labels_, page_boxes_;
  GtkWidget *diagnostics_scroll_{};
  GtkWidget *network_button_{}, *network_icon_{}, *network_label_{}, *network_prompt_{};
  NetworkStatus network_status_;
  bool network_status_busy_ = false, startup_auto_connect_ = false,
       startup_check_ = false, startup_prompted_ = false, internet_check_busy_ = false;
  std::string internet_status_ = "Not checked";
  std::chrono::steady_clock::time_point startup_time_ = std::chrono::steady_clock::now();
  std::string setup_save_error_, profile_save_error_;
  GtkWidget *window_{}, *session_window_{}, *header_{}, *stack_{}, *status_{}, *video_{},
      *overlay_{}, *overlay_status_{}, *networks_{}, *password_{}, *host_{},
      *fingerprint_{}, *code_{}, *auto_check_{}, *tail_status_{},
      *portal_entry_{}, *usb_window_{}, *usb_list_{}, *usb_status_{},
      *resume_button_{}, *diagnostics_view_{}, *ephemeral_warning_{},
      *stream_profile_{};
  std::vector<AccessPoint> access_points_;
  GtkWidget *host_list_{}, *discovery_status_{}, *port_entry_{}, *trust_check_{}, *verification_{}, *match_check_{};
  std::vector<remoteos::HostEntry> saved_hosts_, discovered_hosts_, listed_hosts_;
  std::vector<std::string> tailnet_addresses_;
  bool discovery_busy_ = false, fingerprint_from_discovery_ = false, discovery_enabled_ = true;
  std::vector<GtkWidget *> external_windows_;
  remoteos::ClientOptions options_;
  remoteos::ClientSession session_;
  remoteos::MediaReceiver media_;
  UsbTunnels usb_tunnels_;
  std::set<std::string> usb_pending_, usb_exported_;
  std::map<std::string, std::string> usb_states_;
  std::map<std::string, std::string> usb_tokens_;
  bool usb_busy_ = false, usb_lease_busy_ = false;
  unsigned usb_lease_tick_ = 0;
  std::unique_ptr<InputBroker> broker_;
  std::unique_ptr<SessionCursor> cursor_;
  StatusReader status_reader_;
  Telemetry telemetry_;
  GSubprocess *browser_ = nullptr;
  std::vector<ScreenRectangle> screen_rectangles_;
  double scroll_x_ = 0, scroll_y_ = 0;
  RelativeMouseCapture relative_mouse_;
  bool relative_mouse_requested_ = false;
  GtkWidget* relative_mouse_check_ = nullptr;
  Json last_offer_;
  Json host_media_;
  Json adaptation_status_;
  std::uint64_t generation_ = 0, media_generation_ = 0;
  guint reconnect_timer_ = 0;
  unsigned reconnect_delay_ = 3;
  void say(const std::string &s) {
    gtk_label_set_text(GTK_LABEL(status_), s.c_str());
  }
  GtkWidget *page(GtkWidget *notebook, const char *title) {
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 24);
    page_boxes_.push_back(box);
    auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(scroll), box);
    auto *tab = label(title);
    gtk_label_set_line_wrap(GTK_LABEL(tab), FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scroll, tab);
    return box;
  }
  void welcome_page(GtkWidget *notebook) {
    auto *box = page(notebook, "Welcome");
    gtk_box_pack_start(GTK_BOX(box),
                       label("Set up your remote laptop", "remoteos-title"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(box),
        label("Your desktop runs on your host computer. This laptop connects "
              "your screens, keyboard, sound and supported devices."),
        FALSE, FALSE, 0);
    auto *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(card),
                                "remoteos-card");
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(card),
                       label("Have these ready", "remoteos-section-title"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(card),
        label("•  A Wi-Fi network or Ethernet cable\n•  A computer running the "
              "RemoteOS host software\n•  Its address, certificate fingerprint "
              "and one-time pairing code"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(box),
        label("You can use Tailscale for remote access, or connect directly "
              "over your local network. Diagnostics and client power controls "
              "stay available throughout setup.",
              "remoteos-subtitle"),
        FALSE, FALSE, 0);
  }
  void load_setup() {
    const auto path = std::filesystem::path(state_) / "setup.json";
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    bool valid = false, direct = false, complete = false;
    int step = 0;
    if (exists) {
      try {
        auto j = Json::parse(remoteos::read_private_file(path));
        valid = j.at("version") == 1 && j.at("step").is_number_integer() &&
                j.at("direct").is_boolean() && j.at("complete").is_boolean();
        if (valid) {
          step = j.at("step").get<int>();
          direct = j.at("direct").get<bool>();
          complete = j.at("complete").get<bool>();
        }
      } catch (...) {
      }
    }
    setup_ = SetupState::restore(exists || bool(error), valid, step, direct,
                                 complete, existing_pair_);
    // Persist the initial in-progress record before any pairing event can save
    // a legacy-looking host profile. Resuming never stores passwords or codes.
    if (!setup_.complete)
      save_setup();
  }
  void save_setup() {
    try {
      std::filesystem::create_directories(state_);
      remoteos::write_private_file(std::filesystem::path(state_) / "setup.json",
                                   Json{{"version", 1},
                                        {"step", int(setup_.step)},
                                        {"direct", setup_.direct},
                                        {"complete", setup_.complete}}
                                           .dump(2) +
                                       "\n");
      setup_save_error_.clear();
    } catch (const std::exception &error) {
      setup_save_error_ =
          std::string("Setup progress could not be saved: ") + error.what();
      say(setup_save_error_);
    }
  }
  void update_setup() {
    const bool first_run = !setup_.complete;
    for (auto *box : page_boxes_) {
      gtk_container_set_border_width(GTK_CONTAINER(box), first_run ? 16 : 24);
      gtk_box_set_spacing(GTK_BOX(box), first_run ? 8 : 12);
    }
    gtk_widget_set_size_request(diagnostics_scroll_, -1, first_run ? 140 : 260);
    auto *context = gtk_widget_get_style_context(window_);
    if (first_run)
      gtk_style_context_add_class(context, "remoteos-first-run");
    else
      gtk_style_context_remove_class(context, "remoteos-first-run");
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook_), !first_run);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook_), !first_run);
    gtk_widget_set_visible(progress_, first_run);
    gtk_widget_set_visible(footer_, first_run);
    gtk_widget_set_visible(
        gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook_), 0), first_run);
    gtk_widget_set_visible(host_actions_, !first_run);
    gtk_widget_set_visible(tailscale_action_, !first_run);
    if(desktop_input_){
      gtk_widget_hide(gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook_),0));
      gtk_widget_hide(gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook_),1));
      gtk_widget_hide(gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook_),2));
      gtk_widget_hide(progress_);
      gtk_widget_hide(tailscale_action_);
    }
    auto *wifi_context = gtk_widget_get_style_context(wifi_action_);
    if (first_run)
      gtk_style_context_remove_class(wifi_context, "primary");
    else
      gtk_style_context_add_class(wifi_context, "primary");
    gtk_button_set_label(GTK_BUTTON(diagnostics_button_),
                         first_run && diagnostics_open_ ? "Back to setup"
                                                        : "Diagnostics");
    if (!first_run)
      return;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook_),
                                  diagnostics_open_ ? 4 : int(setup_.step));
    for (std::size_t i = 0; i < step_labels_.size(); ++i) {
      auto *step_context = gtk_widget_get_style_context(step_labels_[i]);
      gtk_style_context_remove_class(step_context, "current");
      gtk_style_context_remove_class(step_context, "done");
      if (int(i) == int(setup_.step))
        gtk_style_context_add_class(step_context, "current");
      else if (int(i) < int(setup_.step))
        gtk_style_context_add_class(step_context, "done");
    }
    gtk_widget_set_visible(
        back_button_, !diagnostics_open_ && setup_.step != SetupState::Welcome);
    gtk_widget_set_visible(direct_button_,
                           !diagnostics_open_ &&
                               setup_.step == SetupState::Tailscale);
    const char *next = diagnostics_open_                    ? "Back to setup"
                       : setup_.step == SetupState::Welcome ? "Get started"
                       : setup_.step == SetupState::Network ? "Continue"
                       : setup_.step == SetupState::Tailscale
                           ? (tailscale_running_ ? "Continue with Tailscale"
                                                 : "Sign in to Tailscale")
                           : "Pair and connect";
    if (setup_.step == SetupState::Host && existing_pair_ && !diagnostics_open_)
      next = "Connect to host";
    gtk_button_set_label(GTK_BUTTON(next_button_), next);
    gtk_widget_set_sensitive(next_button_,
                             !connecting_ && !enrollment_busy_ && !browser_);
    gtk_widget_set_sensitive(back_button_, !enrollment_busy_ && !browser_);
    if(desktop_input_)gtk_widget_hide(back_button_);
    gtk_widget_set_sensitive(direct_button_, !enrollment_busy_ && !browser_);
  }
  void change_setup_step() {
    cancel_reconnect();
    ++setup_operation_;
    enrollment_busy_ = false;
    diagnostics_open_ = false;
    say(setup_.step == SetupState::Network
            ? "Connect to Wi-Fi or plug in Ethernet, then continue."
        : setup_.step == SetupState::Tailscale
            ? "Choose Tailscale or a direct connection to your host."
        : setup_.step == SetupState::Host
            ? "Enter the address and pairing details shown by the RemoteOS "
              "host."
            : "Set up a connection to your remote desktop.");
    save_setup();
    update_setup();
    gtk_widget_grab_focus(next_button_);
    if (setup_.step == SetupState::Network)
      scan();
    else if (setup_.step == SetupState::Tailscale)
      tailscale_status();
    else if (setup_.step == SetupState::Host)
      gtk_widget_grab_focus(host_);
  }
  void next_setup_step() {
    if (diagnostics_open_) {
      diagnostics_open_ = false;
      update_setup();
    } else if (setup_.step == SetupState::Host) {
      connect_host();
    } else if (setup_.step == SetupState::Tailscale && !tailscale_running_) {
      enroll();
    } else if (setup_.advance(tailscale_running_)) {
      change_setup_step();
    }
  }
  void toggle_diagnostics() {
    cancel_reconnect();
    ++setup_operation_;
    enrollment_busy_ = false;
    if (!setup_.complete) {
      diagnostics_open_ = !diagnostics_open_;
      update_setup();
    } else {
      gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook_), 4);
    }
    refresh_report();
  }
  void network_page(GtkWidget *notebook) {
    auto *box = page(notebook, "Network");
    gtk_box_pack_start(GTK_BOX(box),
                       label("Connect this laptop", "remoteos-title"), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       label("Ethernet connects automatically. Wi-Fi supports "
                             "open and WPA2 Personal networks. Direct LAN "
                             "access does not require internet."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label("Wi-Fi network"), FALSE, FALSE, 0);
    networks_ = gtk_combo_box_text_new();
    gtk_box_pack_start(GTK_BOX(box), networks_, FALSE, FALSE, 0);
    password_ =
        entry(box, "Wi-Fi password", "Leave empty for an open network", true);
    auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    button(row, "Scan Wi-Fi", [this] { scan(); });
    wifi_action_ =
        button(row, "Connect Wi-Fi", [this] { join_wifi(); }, "primary");
    button(row, "Check internet", [this] { check_internet(); });
    gtk_box_pack_start(GTK_BOX(box),
                       label("No networks? Check the laptop’s physical "
                             "wireless switch, or use Ethernet.",
                             "remoteos-subtitle"),
                       FALSE, FALSE, 0);
    auto *portal_expander =
        gtk_expander_new("Network sign-in / captive portal");
    gtk_box_pack_start(GTK_BOX(box), portal_expander, FALSE, FALSE, 0);
    auto *portal_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(portal_expander), portal_box);
    portal_entry_ = entry(portal_box, "Network sign-in address",
                          "http://network-login-address");
    button(portal_box, "Open network sign-in", [this] {
      auto url = text(portal_entry_);
      if (url.empty())
        url = portal_url_;
      if (url.empty()) {
        say("Check internet first, or enter the network's sign-in address.");
        return;
      }
      open_browser(url);
    });
    auto *tailscale = page(notebook, "Tailscale");
    gtk_box_pack_start(
        GTK_BOX(tailscale),
        label("Pair this laptop with Tailscale", "remoteos-title"), FALSE,
        FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tailscale),
                       label("Connect to the internet first. Google sign-in "
                             "opens in a separate browser on this laptop. "
                             "Choose Done in the browser to return here."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(tailscale),
        label("For a host reachable over your local network, choose Use a "
              "direct connection. You can set up Tailscale later.",
              "remoteos-subtitle"),
        FALSE, FALSE, 0);
    tail_status_ = label("Checking whether this laptop is signed in…");
    gtk_box_pack_start(GTK_BOX(tailscale), tail_status_, FALSE, FALSE, 0);
    tailscale_action_ = button(
        tailscale, "Sign in to Tailscale on this laptop", [this] { enroll(); },
        "primary");
  }
  void host_page(GtkWidget *notebook) {
    auto *box = page(notebook, "Remote host");
    gtk_box_pack_start(GTK_BOX(box),
                       label("Your remote desktop", "remoteos-title"), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label("Hosts", "remoteos-section-title"), FALSE, FALSE, 0);
    host_list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(host_list_), GTK_SELECTION_SINGLE);
    auto *hosts_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_size_request(hosts_scroll, -1, 112);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(hosts_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(hosts_scroll), host_list_);
    gtk_box_pack_start(GTK_BOX(box), hosts_scroll, FALSE, FALSE, 0);
    g_signal_connect(host_list_, "row-activated", G_CALLBACK(+[](GtkListBox *, GtkListBoxRow *row, gpointer p) {
      static_cast<Shell *>(p)->select_host(gtk_list_box_row_get_index(row));
    }), this);
    g_signal_connect(host_list_, "row-selected", G_CALLBACK(+[](GtkListBox *, GtkListBoxRow *row, gpointer p) {
      if (row) static_cast<Shell *>(p)->select_host(gtk_list_box_row_get_index(row));
    }), this);
    auto *discovery_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(box), discovery_row, FALSE, FALSE, 0);
    button(discovery_row, "Find hosts", [this] { discover(true); });
    discovery_status_ = label("Searching your local network…");
    gtk_box_pack_start(GTK_BOX(discovery_row), discovery_status_, TRUE, TRUE, 0);
    auto *manual_host = gtk_expander_new("Connect by IP address or hostname");
    gtk_box_pack_start(GTK_BOX(box), manual_host, FALSE, FALSE, 0);
    auto *manual_fields = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(manual_host), manual_fields);
    host_ = entry(manual_fields, "Host IP address or hostname",
                  "192.168.1.10 or my-host.tailnet.ts.net");
    fingerprint_ = entry(manual_fields, "Verified host certificate fingerprint",
                         "SHA-256 fingerprint displayed by the host");
    verification_ = label("Select a host, or enter its verified fingerprint.");
    gtk_box_pack_start(GTK_BOX(box), verification_, FALSE, FALSE, 0);
    trust_check_ = gtk_check_button_new_with_label("Verification value matches the host app");
    gtk_box_pack_start(GTK_BOX(box), trust_check_, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(trust_check_, TRUE);
    code_ = entry(box, "Pairing PIN", "Six digits shown in the host app; only needed once");
    gtk_entry_set_max_length(GTK_ENTRY(code_), 16);
    gtk_entry_set_input_purpose(GTK_ENTRY(code_), GTK_INPUT_PURPOSE_DIGITS);
    host_options_ = gtk_expander_new("Display and startup options");
    gtk_box_pack_start(GTK_BOX(box), host_options_, FALSE, FALSE, 0);
    auto *options = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(host_options_), options);
    gtk_box_pack_start(GTK_BOX(options), label("Streaming profile"), FALSE,
                       FALSE, 0);
    stream_profile_ = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(stream_profile_), "wifi",
                              "MPEG-2 · host-selected frame rate · 40 Mbps limit");
    gtk_combo_box_set_active(GTK_COMBO_BOX(stream_profile_), 0);
    gtk_box_pack_start(GTK_BOX(options), stream_profile_, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(options),
                       label("The host captures, scales and compresses both screens. "
                             "The HP decodes MPEG-2 and displays it through XVideo. "
                             "Video shares the 40 Mbps budget with audio and control."),
                       FALSE, FALSE, 0);
    port_entry_ = entry(options, "Host control port", "47010");
    gtk_entry_set_text(GTK_ENTRY(port_entry_), "47010");
    match_check_ = gtk_check_button_new_with_label("Match the host desktop to this laptop's screen resolution");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(match_check_), TRUE);
    gtk_box_pack_start(GTK_BOX(options), match_check_, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(options), label("On Wayland, choose the display source in the RemoteOS Host app. The desktop portal controls virtual output sizes. "
      "On KDE, choose HP native virtual display in RemoteOS Host and select this laptop's panel resolution for readable text. "
      "A mismatched resolution is reported; clearing this option allows scaling only with other display sources."), FALSE, FALSE, 0);
    auto_check_ = gtk_check_button_new_with_label(
        "Connect automatically after this laptop boots");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_check_), TRUE);
    gtk_box_pack_start(GTK_BOX(options), auto_check_, FALSE, FALSE, 0);
    auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    host_actions_ = row;
    button(row, "Connect", [this] { connect_host(); }, "primary");
    button(row, "Disconnect", [this] {
      if (!confirm_disconnect())
        return;
      stop();
      say("Disconnected.");
    });
    gtk_box_pack_start(
        GTK_BOX(box),
        label("Compare the verification value on the host before entering its PIN. Tailscale "
              "enrollment and RemoteOS pairing are separate."),
        FALSE, FALSE, 0);
  }
  bool ephemeral_state() const {
    return g_file_test("/run/remoteos/ephemeral-state", G_FILE_TEST_EXISTS);
  }
  std::string hardware_report() const {
    std::ifstream report("/run/remoteos/hardware.txt", std::ios::binary);
    if (!report)
      throw std::runtime_error(
          "The hardware report is unavailable. The diagnostic image generates "
          "/run/remoteos/hardware.txt during startup.");
    std::string data(256 * 1024, '\0');
    report.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<std::size_t>(report.gcount()));
    return data;
  }
  void refresh_report() {
    std::string report;
    try {
      report = hardware_report();
    } catch (const std::exception &error) {
      report = error.what();
    }
    gchar *valid =
        g_utf8_make_valid(report.data(), static_cast<gssize>(report.size()));
    gtk_text_buffer_set_text(
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(diagnostics_view_)), valid, -1);
    g_free(valid);
  }
  void record_performance() {
    if (recording_) {
      say(recording_status_);
      return;
    }
    try {
      const auto folder = std::filesystem::path(state_) / "reports";
      std::filesystem::create_directories(folder);
      auto *now = g_date_time_new_now_local();
      gchar *date = g_date_time_format(now, "%Y%m%d-%H%M%S");
      const auto path = folder / (std::string("performance-") + date + "-" +
                                  remoteos::random_token(4) + ".csv");
      g_free(date);
      g_date_time_unref(now);
      recording_ = true;
      recording_status_ =
          "Recording 30 minutes of performance to " + path.string();
      say(recording_status_);
      command({"/usr/bin/remoteos-sample", "1800", path.string()},
              [this, path](const std::string &, const std::string &error) {
                recording_ = false;
                recording_status_ =
                    error.empty()
                        ? "Performance recording saved to " + path.string()
                        : "Performance recording stopped: " + error;
                say(recording_status_);
              });
    } catch (const std::exception &error) {
      recording_ = false;
      say(error.what());
    }
  }
  void diagnostics_page(GtkWidget *notebook) {
    auto *box = page(notebook, "Diagnostics");
    gtk_box_pack_start(GTK_BOX(box),
                       label("Hardware qualification", "remoteos-title"), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(box),
        label("Inspect the startup inventory before qualifying Wi-Fi, webcam "
              "modes, or both displays. Google enrollment and sustained "
              "performance still need tests on this laptop."),
        FALSE, FALSE, 0);
    auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    diagnostics_scroll_ = scroll;
    gtk_widget_set_size_request(scroll, -1, 260);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    diagnostics_view_ = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(diagnostics_view_), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(diagnostics_view_), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(diagnostics_view_), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(diagnostics_view_),
                                GTK_WRAP_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(diagnostics_view_),
                                "remoteos-report");
    gtk_container_add(GTK_CONTAINER(scroll), diagnostics_view_);
    auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    button(row, "Refresh report", [this] { refresh_report(); });
    button(
        row, "Save hardware report",
        [this] {
          try {
            const auto report = hardware_report();
            const auto folder = std::filesystem::path(state_) / "reports";
            std::filesystem::create_directories(folder);
            const auto path = folder / "hardware.txt";
            remoteos::write_private_file(path, report);
            say("Report saved to " + path.string() +
                (ephemeral_state()
                     ? ". Temporary storage: copy it before powering off."
                     : "."));
          } catch (const std::exception &error) {
            say(error.what());
          }
        },
        "primary");
    button(row, "Record 30 min performance", [this] { record_performance(); });
    refresh_report();
  }
  void build_overlay() {
    overlay_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(overlay_), "RemoteOS client controls");
    gtk_window_set_transient_for(GTK_WINDOW(overlay_), GTK_WINDOW(window_));
    gtk_window_set_modal(GTK_WINDOW(overlay_), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(overlay_), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(overlay_), 570, 500);
    gtk_window_set_position(GTK_WINDOW(overlay_), GTK_WIN_POS_CENTER_ON_PARENT);
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(box),
                                "remoteos-overlay");
    gtk_container_add(GTK_CONTAINER(overlay_), box);
    gtk_box_pack_start(GTK_BOX(box), label("Client controls", "remoteos-title"),
                       FALSE, FALSE, 0);
    battery_notice_ = label("", "remoteos-warning");
    gtk_widget_set_no_show_all(battery_notice_, TRUE);
    gtk_box_pack_start(GTK_BOX(box), battery_notice_, FALSE, FALSE, 0);
    overlay_status_ = label("");
    gtk_label_set_selectable(GTK_LABEL(overlay_status_), FALSE);
    auto *status_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(status_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(status_scroll, -1, 120);
    gtk_container_add(GTK_CONTAINER(status_scroll), overlay_status_);
    gtk_box_pack_start(GTK_BOX(box), status_scroll, TRUE, TRUE, 0);
    resume_button_ = button(
        box, "Resume remote desktop", [this] { close_overlay(); }, "primary");
    relative_mouse_check_ = gtk_check_button_new_with_label("Relative mouse (Blender / 3D)");
    gtk_box_pack_start(GTK_BOX(box), relative_mouse_check_, FALSE, FALSE, 0);
    g_signal_connect(relative_mouse_check_, "toggled", G_CALLBACK(+[](GtkToggleButton* toggle, gpointer p) {
      auto* self = static_cast<Shell*>(p);
      self->relative_mouse_requested_ = gtk_toggle_button_get_active(toggle);
      self->relative_mouse_.stop(); self->release_all();
      if (self->connected_) self->save_profile(self->options_.host_fingerprint);
      self->update_mouse_capture();
    }), this);
    gtk_box_pack_start(GTK_BOX(box), label("Captures mouse movement. Open client controls to release it."), FALSE, FALSE, 0);
    button(box, "Webcam and SD files", [this] { show_resources(); });
    button(box, "USB devices", [this] { show_usb(); });
    button(box, "Disconnect and open setup", [this] {
      if (!confirm_disconnect())
        return;
      stop();
      close_overlay();
      say("Disconnected. Network and host setup are available.");
    });
    auto *volume = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(box), volume, FALSE, FALSE, 0);
    button(volume, "Volume −", [this] { change_volume("5%-"); });
    button(volume, "Volume +", [this] { change_volume("5%+"); });
    button(volume, "Record 30 min", [this] { record_performance(); });
    auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    if (!desktop_input_) button(row, "Restart client", [this] { power("reboot"); });
    button(row, "Suspend client", [this] { suspend_client(); });
    if (!desktop_input_) button(row, "Power off client", [this] { power("poweroff"); }, "danger");
    else button(row, "Exit RemoteOS", [this] { stop(); gtk_main_quit(); });
    g_signal_connect(
        overlay_, "delete-event",
        G_CALLBACK(+[](GtkWidget *, GdkEvent *, gpointer p) -> gboolean {
          static_cast<Shell *>(p)->close_overlay();
          return TRUE;
        }),
        this);
  }
  void resources_message(const std::string& message){resource_report_=message;if(resource_status_)gtk_label_set_text(GTK_LABEL(resource_status_),message.c_str());}
  void stop_resources() {
    resource_tunnels_.stop();camera_export_.reset();file_share_.reset();camera_cap_.clear();file_cap_.clear();
    if(sd_mounted_){sd_mounted_=false;background([]{return supervisor("sd_unmount");},[this](const auto&,const auto& error){if(!error.empty())say(error);});}
    if(resources_window_)gtk_widget_hide(resources_window_);resources_message("Device forwarding is off.");
  }
  void show_resources(){
    if(!resources_window_){
      resources_window_=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(resources_window_),"RemoteOS webcam and SD files");
      gtk_window_set_transient_for(GTK_WINDOW(resources_window_),GTK_WINDOW(overlay_));gtk_window_set_modal(GTK_WINDOW(resources_window_),TRUE);gtk_window_set_default_size(GTK_WINDOW(resources_window_),780,500);
      auto* box=gtk_box_new(GTK_ORIENTATION_VERTICAL,16);gtk_container_set_border_width(GTK_CONTAINER(box),24);gtk_container_add(GTK_CONTAINER(resources_window_),box);
      gtk_box_pack_start(GTK_BOX(box),label("Webcam and shared SD files"),FALSE,FALSE,0);
      gtk_box_pack_start(GTK_BOX(box),label("Webcam: host-side compression; 320×240 at 5 fps. Add the host URL to OBS Browser Source, then Start Virtual Camera. Webcam stops on disconnect, setup/browser mode and suspend."),FALSE,FALSE,0);
      auto* row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);gtk_box_pack_start(GTK_BOX(box),row,FALSE,FALSE,0);
      button(row,"Start webcam",[this]{start_camera();});button(row,"Stop webcam",[this]{if(connected_)session_.send({{"type","detach_resource"},{"resource","camera"}});resource_tunnels_.close("camera");camera_export_.reset();camera_cap_.clear();resources_message("Webcam stopped.");});
      sd_entry_=entry(box,"SD card",desktop_input_?"Choose a mounted SD folder":"SD partition, for example mmcblk0p1");
      sd_write_=gtk_check_button_new_with_label("Allow file uploads and deletion (read-only by default)");gtk_box_pack_start(GTK_BOX(box),sd_write_,FALSE,FALSE,0);
      row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);gtk_box_pack_start(GTK_BOX(box),row,FALSE,FALSE,0);
      button(row,desktop_input_?"Choose shared folder":"Detect SD card",[this]{
        if(desktop_input_){auto* chooser=gtk_file_chooser_dialog_new("Select mounted SD folder",GTK_WINDOW(resources_window_),GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,"Cancel",GTK_RESPONSE_CANCEL,"Select folder",GTK_RESPONSE_ACCEPT,nullptr);if(gtk_dialog_run(GTK_DIALOG(chooser))==GTK_RESPONSE_ACCEPT){auto* path=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));gtk_entry_set_text(GTK_ENTRY(sd_entry_),path);g_free(path);}gtk_widget_destroy(chooser);}
        else background([]{return supervisor("sd_list");},[this](const auto& result,const auto& error){if(!error.empty()){resources_message(error);return;}const auto cards=Json::parse(result).value("cards",Json::array());if(cards.empty())resources_message("No removable SD card detected. Insert a FAT/FAT32 or exFAT card.");else{std::string report="SD cards:";for(const auto& card:cards)report+=" "+card.at("device").template get<std::string>();gtk_entry_set_text(GTK_ENTRY(sd_entry_),cards.back().at("device").template get<std::string>().c_str());resources_message(report);}});
      });
      button(row,"Share SD files",[this]{start_files();});button(row,"Stop sharing",[this]{if(connected_)session_.send({{"type","detach_resource"},{"resource","files"}});resource_tunnels_.close("files");file_share_.reset();file_cap_.clear();if(sd_mounted_){sd_mounted_=false;background([]{return supervisor("sd_unmount");},[this](const auto&,const auto& error){resources_message(error.empty()?"SD card safely unmounted.":error);});}});
      resource_status_=label(resource_report_.c_str());gtk_label_set_selectable(GTK_LABEL(resource_status_),TRUE);gtk_box_pack_start(GTK_BOX(box),resource_status_,TRUE,TRUE,0);
      button(box,"Done",[this]{gtk_widget_hide(resources_window_);});g_signal_connect(resources_window_,"delete-event",G_CALLBACK(+[](GtkWidget* w,GdkEvent*,gpointer)->gboolean{gtk_widget_hide(w);return TRUE;}),nullptr);
    }
    gtk_widget_show_all(resources_window_);gtk_window_present(GTK_WINDOW(resources_window_));
    if(!connected_||!host_resources_)resources_message("Connect to a 0.1.18 host application to forward devices.");
  }
  void start_camera(){
    if(!connected_||!host_resources_||camera_busy_||camera_export_)return;camera_busy_=true;camera_cap_=remoteos::random_token(32);const auto cap=camera_cap_;const auto generation=generation_;
    auto created=std::make_shared<std::shared_ptr<remoteos::devices::CameraExport>>();
    resources_message("Opening webcam…");background([created,cap]{*created=std::make_shared<remoteos::devices::CameraExport>(cap);return std::string{};},[this,created,cap,generation](const auto&,const auto& error){camera_busy_=false;if(generation!=generation_)return;if(!error.empty()){resources_message(error);return;}camera_export_=*created;session_.send({{"type","attach_resource"},{"resource","camera"},{"capability",cap}});resources_message("Webcam ON · forwarding to host. Stop webcam to close the camera.");});
  }
  void start_files(){
    if(!connected_||!host_resources_||file_share_)return;const auto selection=text(sd_entry_);const bool writable=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sd_write_));const auto generation=generation_;
    background([selection,writable,desktop=desktop_input_]{if(desktop)return Json{{"path",selection}}.dump();return supervisor(Json{{"action","sd_mount"},{"device",selection},{"writable",writable}});},[this,writable,generation](const auto& result,const auto& error){
      if(generation!=generation_){if(!desktop_input_)background([]{return supervisor("sd_unmount");},[](const auto&,const auto&){});return;}
      if(!error.empty()){resources_message(error);return;}sd_mounted_=!desktop_input_;try{file_cap_=remoteos::random_token(32);file_share_=std::make_shared<remoteos::devices::FileShare>(Json::parse(result).at("path").template get<std::string>(),file_cap_,writable);session_.send({{"type","attach_resource"},{"resource","files"},{"capability",file_cap_}});}catch(const std::exception& e){resources_message(e.what());}
    });
  }
  void usb_message(const std::string &message) {
    if (usb_status_)
      gtk_label_set_text(GTK_LABEL(usb_status_), message.c_str());
    say(message);
  }
  void show_usb() {
    if (!usb_window_) {
      usb_window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
      gtk_window_set_title(GTK_WINDOW(usb_window_), "RemoteOS USB devices");
      gtk_window_set_transient_for(GTK_WINDOW(usb_window_),
                                   GTK_WINDOW(overlay_));
      gtk_window_set_modal(GTK_WINDOW(usb_window_), TRUE);
      gtk_window_set_default_size(GTK_WINDOW(usb_window_), 760, 460);
      auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
      gtk_container_set_border_width(GTK_CONTAINER(box), 24);
      gtk_container_add(GTK_CONTAINER(usb_window_), box);
      gtk_box_pack_start(GTK_BOX(box), label("USB devices", "remoteos-title"),
                         FALSE, FALSE, 0);
      gtk_box_pack_start(
          GTK_BOX(box),
          label("Forward only qualified devices. Eject storage in the host "
                "desktop before detaching. A lost connection behaves like "
                "unplugging the device."),
          FALSE, FALSE, 0);
      auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
      gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
      usb_list_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
      gtk_container_add(GTK_CONTAINER(scroll), usb_list_);
      usb_status_ = label("");
      gtk_box_pack_start(GTK_BOX(box), usb_status_, FALSE, FALSE, 0);
      auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
      button(row, "Refresh devices", [this] { refresh_usb(); });
      button(row, "Done", [this] { gtk_widget_hide(usb_window_); });
      g_signal_connect(
          usb_window_, "delete-event",
          G_CALLBACK(+[](GtkWidget *window, GdkEvent *, gpointer) -> gboolean {
            gtk_widget_hide(window);
            return TRUE;
          }),
          nullptr);
    }
    gtk_widget_show_all(usb_window_);
    gtk_window_present(GTK_WINDOW(usb_window_));
    refresh_usb();
    if (!host_usb_)
      usb_message("USB forwarding is unavailable on this host or no host is "
                  "connected.");
  }
  void refresh_usb() {
    if (usb_busy_ || !usb_list_)
      return;
    usb_busy_ = true;
    background(
        [] { return supervisor("usb_list"); },
        [this](const std::string &output, const std::string &error) {
          usb_busy_ = false;
          if (!error.empty()) {
            usb_message(error);
            return;
          }
          try {
            const auto response = Json::parse(output);
            const auto &devices = response.at("devices");
            if (!devices.is_array())
              throw std::runtime_error("Invalid USB device response");
            auto *children =
                gtk_container_get_children(GTK_CONTAINER(usb_list_));
            for (auto *item = children; item; item = item->next)
              gtk_widget_destroy(GTK_WIDGET(item->data));
            g_list_free(children);
            if (devices.empty())
              gtk_box_pack_start(GTK_BOX(usb_list_),
                                 label("No USB devices found."), FALSE, FALSE,
                                 0);
            for (const auto &device : devices) {
              const auto bus = device.at("bus_id").get<std::string>();
              if (!remoteos::devices::valid_bus_id(bus))
                continue;
              auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
              gtk_box_pack_start(GTK_BOX(usb_list_), row, FALSE, FALSE, 0);
              auto caption = bus + " · " +
                             device.value("label", std::string("USB device"));
              caption += "\n" + (usb_states_.count(bus)
                                     ? usb_states_[bus]
                                     : device.value("reason", std::string{}));
              auto *description = label(caption.c_str());
              gtk_widget_set_hexpand(description, TRUE);
              gtk_label_set_max_width_chars(GTK_LABEL(description), 65);
              gtk_box_pack_start(GTK_BOX(row), description, TRUE, TRUE, 0);
              const bool exported =
                  usb_exported_.count(bus) || device.value("exported", false);
              auto *action = button(row, exported ? "Detach" : "Forward",
                                    [this, bus, exported] {
                                      if (exported)
                                        detach_usb(bus);
                                      else
                                        attach_usb(bus);
                                    });
              gtk_widget_set_sensitive(
                  action, connected_ && host_usb_ && !usb_pending_.count(bus) &&
                              (exported || device.value("eligible", false)));
            }
            gtk_widget_show_all(usb_list_);
          } catch (const std::exception &e) {
            usb_message(e.what());
          }
        });
  }
  void attach_usb(const std::string &bus) {
    if (!connected_ || !host_usb_ || !remoteos::devices::valid_bus_id(bus) ||
        usb_pending_.count(bus))
      return;
    usb_pending_.insert(bus);
    usb_states_[bus] = "Requesting authenticated host relay";
    try {
      session_.send(Json{{"type", "attach_device"}, {"bus_id", bus}});
      refresh_usb();
    } catch (const std::exception &e) {
      usb_pending_.erase(bus);
      usb_message(e.what());
    }
  }
  void open_usb_tunnel(const Json &event) {
    const auto bus = event.at("bus_id").get<std::string>();
    // An authenticated host cannot export a device without a matching local
    // user selection. The privileged supervisor independently rechecks policy.
    if (!remoteos::devices::valid_bus_id(bus) || !usb_pending_.count(bus) ||
        usb_exported_.count(bus) || !connected_)
      throw std::runtime_error("Unsolicited USB export request rejected");
    const auto generation = generation_;
    const auto session = event.at("session_id").get<std::string>();
    const auto token = event.at("token").get<std::string>();
    const auto port = event.at("port").get<unsigned>();
    if (port < 1 || port > 65535 || token.size() < 32)
      throw std::runtime_error("Invalid USB relay authorization");
    usb_tokens_[bus] = token;
    usb_states_[bus] = "Exporting qualified device";
    usb_exported_.insert(bus);
    background(
        [bus] {
          return supervisor(Json{{"action", "usb_export"}, {"bus_id", bus}});
        },
        [this, bus, generation, session, token,
         port](const std::string &output, const std::string &error) {
          if (generation != generation_) {
            if (usb_exported_.count(bus))
              return; // A newer session already owns this device.
            background(
                [bus] {
                  return supervisor(
                      Json{{"action", "usb_unexport"}, {"bus_id", bus}});
                },
                [](const std::string &, const std::string &) {});
            return;
          }
          if (!error.empty()) {
            cancel_usb_grant(bus, session, token);
            usb_tokens_.erase(bus);
            usb_pending_.erase(bus);
            usb_exported_.erase(bus);
            usb_states_[bus] = error;
            usb_message(error);
            refresh_usb();
            return;
          }
          try {
            const auto reply = Json::parse(output);
            const auto local_port = reply.value("local_port", 3240u);
            const auto host = last_offer_.value("host", options_.host);
            usb_tunnels_.open(
                options_.identity_dir, host, port, options_.host_fingerprint,
                session, bus, token, local_port,
                [this, bus, generation, session,
                 token](bool connected, const std::string &detail) {
                  on_main([this, bus, generation, session, token, connected,
                           detail] {
                    if (generation != generation_ || !usb_tokens_.count(bus) ||
                        usb_tokens_.at(bus) != token)
                      return;
                    if (connected) {
                      usb_states_[bus] =
                          "Authenticated relay ready; importing on host";
                      try {
                        session_.send(Json{{"type", "attach_device"},
                                           {"bus_id", bus},
                                           {"ready", true},
                                           {"session_id", session}});
                      } catch (const std::exception &e) {
                        usb_message(e.what());
                      }
                    } else {
                      if (!usb_exported_.count(bus))
                        return;
                      usb_pending_.erase(bus);
                      usb_states_[bus] = "Relay stopped: " + detail;
                      usb_message(usb_states_[bus]);
                      cancel_usb_grant(bus, session, token);
                      unexport_usb(bus);
                    }
                    refresh_usb();
                  });
                });
          } catch (const std::exception &e) {
            cancel_usb_grant(bus, session, token);
            usb_pending_.erase(bus);
            usb_message(e.what());
            unexport_usb(bus);
          }
        });
  }
  void cancel_usb_grant(const std::string &bus, const std::string &session,
                        const std::string &token) {
    if (!session_.running())
      return;
    try {
      session_.send({{"type", "detach_device"},
                     {"bus_id", bus},
                     {"cancel", true},
                     {"session_id", session},
                     {"token", token}});
    } catch (...) {
      // A lost control connection revokes all grants on the host.
    }
  }
  void detach_usb(const std::string &bus) {
    if (!session_.running() || !remoteos::devices::valid_bus_id(bus))
      return;
    usb_pending_.insert(bus);
    usb_states_[bus] = "Waiting for host eject and flush";
    try {
      session_.send(Json{{"type", "detach_device"}, {"bus_id", bus}});
      refresh_usb();
    } catch (const std::exception &e) {
      usb_pending_.erase(bus);
      usb_message(e.what());
    }
  }
  void unexport_usb(const std::string &bus) {
    usb_tokens_.erase(bus);
    usb_tunnels_.close(bus);
    usb_exported_.erase(bus);
    usb_pending_.insert(bus);
    usb_states_[bus] = "Restoring local device";
    const auto generation = generation_;
    background(
        [bus] {
          return supervisor(Json{{"action", "usb_unexport"}, {"bus_id", bus}});
        },
        [this, bus, generation](const std::string &, const std::string &error) {
          if (generation != generation_)
            return;
          usb_pending_.erase(bus);
          if (!error.empty()) {
            usb_states_[bus] = error;
            usb_message(error);
          } else
            usb_states_[bus] = "Local device restored";
          refresh_usb();
        });
  }
  void stop_usb() {
    usb_tokens_.clear();
    usb_tunnels_.stop();
    for (const auto &bus : usb_exported_)
      background(
          [bus] {
            return supervisor(
                Json{{"action", "usb_unexport"}, {"bus_id", bus}});
          },
          [](const std::string &, const std::string &) {});
    usb_exported_.clear();
    usb_pending_.clear();
    usb_states_.clear();
    if (usb_window_)
      gtk_widget_hide(usb_window_);
  }
  void maintain_usb_lease() {
    if (usb_exported_.empty() || usb_lease_busy_ || ++usb_lease_tick_ % 5 != 0)
      return;
    usb_lease_busy_ = true;
    background([] { return supervisor("usb_keepalive"); },
               [this](const std::string &, const std::string &error) {
                 usb_lease_busy_ = false;
                 if (!error.empty())
                   usb_message("USB lease refresh failed: " + error);
               });
  }
  void scan() {
    if (network_busy_)
      return;
    network_busy_ = true;
    say("Scanning Wi-Fi…");
    background(
        [] {
          auto interface = find_wireless_interface();
          if (interface.empty())
            throw std::runtime_error(
                "No wireless adapter was found. Use Ethernet or install the "
                "matching driver and firmware.");
          WpaControl control(interface);
          if (control.command("SCAN") != "OK")
            throw std::runtime_error("Wi-Fi scan could not start");
          return interface;
        },
        [this](const std::string &interface, const std::string &error) {
          network_busy_ = false;
          if (!error.empty()) {
            say(error);
            return;
          }
          wireless_ = interface;
          g_timeout_add_seconds(
              3,
              +[](gpointer p) -> gboolean {
                static_cast<Shell *>(p)->scan_results();
                return G_SOURCE_REMOVE;
              },
              this);
        });
  }
  void open_network_settings() {
    if(desktop_input_){say("Use your desktop network settings and installed Tailscale app. RemoteOS uses their connection.");return;}
    if ((connected_ || connecting_) && !confirm_disconnect()) return;
    startup_auto_connect_ = false;
    stop();
    close_overlay();
    diagnostics_open_ = false;
    if (!setup_.complete) {
      setup_.step = SetupState::Network;
      change_setup_step();
    } else {
      update_setup();
      gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook_), 1);
      scan();
    }
  }
  void prompt_network(const std::string &reason) {
    if (network_prompt_ || connected_ || browser_) return;
    startup_prompted_ = true;
    startup_auto_connect_ = false;
    // A pending connection must not take input ownership behind the modal.
    // stop() invalidates callbacks, releases input and cancels reconnects.
    stop();
    network_prompt_ = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL,
                                             GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                             "Connect this laptop");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(network_prompt_),
      "%s\nConnect to Wi-Fi or plug in Ethernet. Internet is needed for Tailscale sign-in; a direct LAN connection can work offline.", reason.c_str());
    gtk_dialog_add_button(GTK_DIALOG(network_prompt_), "Continue offline", GTK_RESPONSE_CANCEL);
    auto *setup_button = gtk_dialog_add_button(GTK_DIALOG(network_prompt_), "Set up connection", GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class(gtk_widget_get_style_context(setup_button), "primary");
    gtk_dialog_set_default_response(GTK_DIALOG(network_prompt_), GTK_RESPONSE_ACCEPT);
    g_signal_connect(network_prompt_, "response", G_CALLBACK(+[](GtkDialog *dialog, gint response, gpointer p) {
      auto *self = static_cast<Shell *>(p);
      self->network_prompt_ = nullptr;
      gtk_widget_destroy(GTK_WIDGET(dialog));
      if (response == GTK_RESPONSE_ACCEPT) self->open_network_settings();
      else self->startup_auto_connect_ = false;
    }), this);
    gtk_widget_show_all(network_prompt_);
  }
  void network_status() {
    if(desktop_input_){
      network_status_.usable=g_network_monitor_get_network_available(g_network_monitor_get_default());
      gtk_label_set_text(GTK_LABEL(network_label_),network_status_.usable?"System network ready":"System network offline");
      if(startup_auto_connect_&&network_status_.usable){startup_auto_connect_=false;connect_host();}return;
    }
    if (network_status_busy_) return;
    network_status_busy_ = true;
    background([] {
      const auto s = read_network_status();
      return Json{{"interface", s.interface}, {"ssid", s.ssid}, {"state", s.state},
                  {"address", s.address}, {"error", s.error}, {"signal", s.signal_dbm},
                  {"signal_known", s.signal_known}, {"associated", s.associated},
                  {"wired", s.wired}, {"usable", s.usable}}.dump();
    }, [this](const std::string &response, const std::string &error) {
      network_status_busy_ = false;
      const auto previous_address = network_status_.address;
      const auto previous_ssid = network_status_.ssid;
      const auto previous_usable = network_status_.usable;
      if (error.empty()) {
        const auto s = Json::parse(response);
        network_status_ = {s.value("interface", ""), s.value("ssid", ""), s.value("state", ""),
                           s.value("address", ""), s.value("error", ""), s.value("signal", -100),
                           s.value("signal_known", false), s.value("associated", false),
                           s.value("wired", false), s.value("usable", false)};
      } else { network_status_.usable = false; network_status_.error = error; }
      const auto &net = network_status_;
      if (previous_address != net.address || previous_ssid != net.ssid || previous_usable != net.usable)
        internet_status_ = "Not checked";
      const std::string name = net.wired ? "Ethernet connected" : net.usable ? "Wi-Fi · " + net.ssid :
                              net.associated ? "Wi-Fi · Getting an address" : "Wi-Fi disconnected";
      gtk_label_set_text(GTK_LABEL(network_label_), name.c_str());
      const auto detail = name + (net.address.empty() ? "" : "\n" + net.address) +
                          (net.signal_known && !net.wired ? "\nSignal: " + std::to_string(net.signal_dbm) + " dBm" : "") +
                          "\nInternet: " + internet_status_ +
                          (net.error.empty() ? "" : "\n" + net.error);
      gtk_widget_set_tooltip_text(network_button_, detail.c_str());
      gtk_widget_queue_draw(network_icon_);
      if (startup_check_ && !net.usable && !startup_prompted_ &&
          std::chrono::steady_clock::now() - startup_time_ >= std::chrono::seconds(20))
        prompt_network("No Wi-Fi or Ethernet connection is ready after startup.");
      if (startup_auto_connect_ && net.usable && !network_prompt_ && !browser_ && !diagnostics_open_) {
        startup_auto_connect_ = false;
        connect_host();
      }
      if (startup_check_ && net.usable && internet_status_ == "Not checked" &&
          !internet_check_busy_ && !connected_ && !browser_ && !setup_.direct)
        check_internet();
    });
  }
  void scan_results() {
    if (network_busy_)
      return;
    network_busy_ = true;
    background(
        [interface = wireless_] {
          WpaControl control(interface);
          return control.command("SCAN_RESULTS");
        },
        [this](const std::string &result, const std::string &error) {
          network_busy_ = false;
          if (!error.empty()) {
            say(error);
            return;
          }
          access_points_ = parse_scan_results(result);
          gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(networks_));
          for (const auto &ap : access_points_) {
            const auto title =
                ap.ssid + "  ·  " + std::to_string(ap.signal_dbm) + " dBm" +
                (ap.supported ? (ap.secure ? "  ·  WPA2" : "  ·  Open")
                              : "  ·  Unsupported security");
            gchar *valid = g_utf8_make_valid(title.c_str(),
                                             static_cast<gssize>(title.size()));
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(networks_),
                                           valid);
            g_free(valid);
          }
          if (!access_points_.empty())
            gtk_combo_box_set_active(GTK_COMBO_BOX(networks_), 0);
          say(access_points_.empty()
                  ? "No visible Wi-Fi networks found."
                  : "Select a network and enter its password.");
        });
  }
  void join_wifi() {
    if (network_busy_)
      return;
    const int index = gtk_combo_box_get_active(GTK_COMBO_BOX(networks_));
    if (index < 0 || static_cast<std::size_t>(index) >= access_points_.size()) {
      say("Scan and select a Wi-Fi network first.");
      return;
    }
    auto ap = access_points_[static_cast<std::size_t>(index)];
    if (!ap.supported) {
      say("This network requires unsupported security. Use open or WPA2 "
          "Personal Wi-Fi.");
      return;
    }
    auto password = text(password_);
    gtk_entry_set_text(GTK_ENTRY(password_), "");
    network_busy_ = true;
    say("Connecting to Wi-Fi…");
    background(
        [interface = wireless_, ap, password = std::move(password)]() mutable {
          WpaControl control(interface);
          control.connect(ap.ssid, std::move(password), ap.secure);
          return std::string{};
        },
        [this](const std::string &, const std::string &error) {
          network_busy_ = false;
          say(error.empty() ? "Wi-Fi profile saved. Wait for DHCP, then check "
                              "internet connectivity."
                            : error);
        });
  }
  void check_internet() {
    if (internet_check_busy_) return;
    std::ifstream config("/etc/remoteos/connectivity-url");
    std::string endpoint;
    std::getline(config, endpoint);
    if (!valid_http_url(endpoint)) {
      internet_status_ = "Probe not configured";
      say("Configure /etc/remoteos/connectivity-url with the controlled HTTP "
          "endpoint that returns status 204.");
      return;
    }
    internet_check_busy_ = true;
    internet_status_ = "Checking";
    say("Checking internet and captive-portal status…");
    const auto operation = setup_operation_;
    const auto address = network_status_.address;
    const auto ssid = network_status_.ssid;
    command({"curl", "--silent", "--show-error", "--max-time", "12",
             "--max-filesize", "1024", "--proto", "=http,https", "--output",
             "/dev/null", "--write-out", "%{http_code}\n%{redirect_url}", "--",
             endpoint},
            [this, endpoint, operation, address, ssid](const std::string &output,
                                        const std::string &error) {
              internet_check_busy_ = false;
              if (operation != setup_operation_ || address != network_status_.address || ssid != network_status_.ssid) {
                internet_status_ = "Not checked";
                return;
              }
              if (!error.empty()) {
                internet_status_ = "Unavailable";
                say("Connectivity check failed: " + error);
                if (startup_check_ && !startup_prompted_)
                  prompt_network("The network connected, but Internet access could not be verified.");
                return;
              }
              const auto end = output.find('\n');
              const auto status = output.substr(0, end);
              if (status == "204") {
                internet_status_ = "Available";
                portal_url_.clear();
                say("Internet connectivity is available.");
                return;
              }
              auto redirect = end == std::string::npos ? std::string{}
                                                       : output.substr(end + 1);
              portal_url_ = valid_http_url(redirect) ? redirect : endpoint;
              internet_status_ = "Network sign-in required";
              gtk_entry_set_text(GTK_ENTRY(portal_entry_), portal_url_.c_str());
              say("The network may require sign-in. Open network sign-in to "
                  "continue.");
              if (startup_check_ && !startup_prompted_)
                prompt_network("This network needs browser sign-in before Internet access is available.");
            });
  }
  void update_tailscale_route(const Json &status) {
    tailscale_route_ = "No matching Tailscale peer";
    if (!status.contains("Peer") || !status["Peer"].is_object())
      return;
    const auto address = last_offer_.value("host", options_.host);
    auto normalize = [](std::string value) {
      while (!value.empty() && value.back() == '.')
        value.pop_back();
      std::transform(value.begin(), value.end(), value.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      return value;
    };
    const auto hostname = normalize(options_.host);
    for (const auto &item : status["Peer"].items()) {
      const auto &peer = item.value();
      auto dns = normalize(peer.value("DNSName", std::string{}));
      bool match =
          !hostname.empty() &&
          (dns == hostname ||
           normalize(peer.value("HostName", std::string{})) == hostname);
      if (!match && !hostname.empty() &&
          hostname.find('.') == std::string::npos)
        match = dns.rfind(hostname + ".", 0) == 0;
      if (peer.contains("TailscaleIPs") && peer["TailscaleIPs"].is_array())
        for (const auto &ip : peer["TailscaleIPs"])
          if (ip.is_string() && (ip.get<std::string>() == address ||
                                 ip.get<std::string>() == options_.host))
            match = true;
      if (!match)
        continue;
      if (!peer.value("Active", false)) {
        tailscale_route_ = "Peer present; route idle";
        return;
      }
      if (!peer.value("CurAddr", std::string{}).empty())
        tailscale_route_ = "Direct";
      else if (!peer.value("PeerRelay", std::string{}).empty())
        tailscale_route_ = "Peer relay";
      else if (!peer.value("Relay", std::string{}).empty())
        tailscale_route_ = "DERP relay · " + peer["Relay"].get<std::string>();
      else
        tailscale_route_ = "Peer active; route not reported";
      return;
    }
  }
  void tailscale_status() {
#ifdef REMOTEOS_CLIENT_UI_TEST
    return; // Isolated UI probes never read the user's tailnet.
#endif
    if (tail_busy_)
      return;
    tail_busy_ = true;
    command({"tailscale", "--socket=/run/tailscale/tailscaled.sock", "status", "--json"}, [this](
                                                   const std::string &output,
                                                   const std::string &error) {
      tail_busy_ = false;
      tailscale_running_ = false;
      if (!error.empty()) {
        tailscale_state_ = "Unavailable";
      } else {
        try {
          auto j = Json::parse(output);
          tailnet_addresses_.clear();
          if (j.contains("Peer") && j["Peer"].is_object()) for (const auto& peer : j["Peer"]) {
            if (!peer.value("Online", false)) continue;
            if (peer.contains("TailscaleIPs") && peer["TailscaleIPs"].is_array()) for (const auto& ip : peer["TailscaleIPs"]) {
              if (ip.is_string() && tailnet_addresses_.size() < 64) tailnet_addresses_.push_back(ip.get<std::string>());
            }
          }
          update_tailscale_route(j);
          tailscale_state_ = j.value("BackendState", std::string("Unknown"));
          tailscale_running_ = tailscale_state_ == "Running";
          if (j.contains("TailscaleIPs") && j["TailscaleIPs"].is_array())
            for (const auto &ip : j["TailscaleIPs"])
              if (ip.is_string())
                tailscale_state_ += "  " + ip.get<std::string>();
        } catch (...) {
          tailscale_state_ = "Invalid status response";
        }
      }
      const auto message =
          tailscale_running_ ? "Signed in to Tailscale. " + tailscale_state_
          : tailscale_state_ == "NeedsLogin" ? std::string("Not signed in yet.")
          : tailscale_state_ == "Unavailable"
              ? std::string("Tailscale is unavailable. You can still use a "
                            "direct connection.")
              : "Tailscale status: " + tailscale_state_;
      gtk_label_set_text(GTK_LABEL(tail_status_), message.c_str());
      update_setup();
    });
  }
  void enroll() {
    if (enrollment_busy_)
      return;
    stop();
    enrollment_busy_ = true;
    update_setup();
    say("Starting Tailscale enrollment…");
    const auto operation = setup_operation_;
    background([] { return supervisor("tailscale_prepare"); },
      [this, operation](const std::string &, const std::string &failure) {
        if (operation != setup_operation_)
          return;
        if (!failure.empty()) {
          enrollment_busy_ = false;
          update_setup();
          say("Tailscale enrollment could not start: " + failure);
          return;
        }
    command(
        {"tailscale", "--socket=/run/tailscale/tailscaled.sock", "up", "--operator=remoteos", "--json", "--timeout=5s"},
        [this, operation](const std::string &output, const std::string &error) {
          if (operation != setup_operation_)
            return;
          enrollment_busy_ = false;
          update_setup();
          std::string url;
          std::istringstream lines(output);
          for (std::string line; std::getline(lines, line);) {
            try {
              auto j = Json::parse(line);
              url = j.value("AuthURL", j.value("authURL", std::string{}));
              if (!url.empty())
                break;
            } catch (...) {
            }
          }
          if (url.empty()) {
            try {
              auto j = Json::parse(output);
              url = j.value("AuthURL", std::string{});
            } catch (...) {
            }
          }
          if (!url.empty() && valid_http_url(url)) {
            open_browser(url);
            return;
          }
          tailscale_status();
          say(error.empty() ? "Tailscale is already enrolled, or did not "
                              "provide an enrollment URL."
                            : "Tailscale enrollment: " + error);
        });
      });
  }
  void open_browser(const std::string &url) {
    if (!valid_http_url(url)) {
      say("Only valid HTTP and HTTPS sign-in addresses are allowed.");
      return;
    }
    stop();
    auto *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_set_child_setup(
        launcher, +[](gpointer) { setsid(); }, nullptr, nullptr);
    GError *error = nullptr;
    browser_ = g_subprocess_launcher_spawn(launcher, &error, "remoteos-browser",
                                           url.c_str(), nullptr);
    g_object_unref(launcher);
    if (!browser_) {
      say(error ? error->message : "Sign-in browser could not start");
      g_clear_error(&error);
      return;
    }
    update_setup();
    say("Sign-in browser is open. Choose Done in the browser to return to "
        "setup.");
    g_subprocess_wait_async(
        browser_, nullptr,
        +[](GObject *process, GAsyncResult *result, gpointer p) {
          auto *self = static_cast<Shell *>(p);
          GError *error = nullptr;
          g_subprocess_wait_finish(G_SUBPROCESS(process), result, &error);
          g_clear_error(&error);
          if (self->browser_ == G_SUBPROCESS(process)) {
            g_clear_object(&self->browser_);
            self->say(g_subprocess_get_successful(G_SUBPROCESS(process))
                          ? "Browser closed. Check network or Tailscale "
                            "status, then connect."
                          : "The sign-in browser exited unexpectedly. Check "
                            "the diagnostic log for memory, sandbox, or "
                            "runtime-directory errors.");
            self->update_setup();
            self->tailscale_status();
          }
        },
        this);
  }
  void stop_browser() {
    if (!browser_)
      return;
    const auto *id = g_subprocess_get_identifier(browser_);
    if (id) {
      char *end = nullptr;
      const long pid = std::strtol(id, &end, 10);
      if (end && *end == '\0' && pid > 1) {
        kill(-static_cast<pid_t>(pid), SIGKILL);
      }
    }
    g_subprocess_force_exit(browser_);
    GError *error = nullptr;
    g_subprocess_wait(browser_, nullptr, &error);
    g_clear_error(&error);
    g_clear_object(&browser_);
  }
  void load_profile() {
    try {
      const auto hosts = Json::parse(remoteos::read_private_file(std::filesystem::path(state_) / "hosts.json"));
      if (!hosts.is_array() || hosts.size() > 32) throw std::runtime_error("Invalid saved host list");
      saved_hosts_ = hosts.get<std::vector<remoteos::HostEntry>>();
      for (auto& host : saved_hosts_) host.saved = true;
    } catch (...) {}
    try {
      std::ifstream file(std::filesystem::path(state_) / "profile.json");
      Json j;
      file >> j;
      gtk_entry_set_text(GTK_ENTRY(host_), j.value("host", "").c_str());
      gtk_entry_set_text(GTK_ENTRY(fingerprint_),
                         j.value("fingerprint", "").c_str());
      options_.port = j.value("port", remoteos::default_port);
      gtk_entry_set_text(GTK_ENTRY(port_entry_), std::to_string(options_.port).c_str());
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(match_check_), j.value("match_host_resolution", true));
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(relative_mouse_check_), j.value("relative_mouse", false));
      auto_connect_ = j.value("auto_connect", true);
      gtk_combo_box_set_active_id(
          GTK_COMBO_BOX(stream_profile_),
          "wifi");
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_check_),
                                   auto_connect_);
      const auto pin = Json::parse(remoteos::read_private_file(
          std::filesystem::path(state_) / "identity" / "host.json"));
      existing_pair_ =
          !text(host_).empty() && pin.value("host", "") == text(host_) &&
          pin.value("port", 0u) == options_.port &&
          remoteos::normalize_fingerprint(pin.value("fingerprint", "")) ==
              remoteos::normalize_fingerprint(text(fingerprint_)) &&
          !remoteos::normalize_fingerprint(text(fingerprint_)).empty();
    } catch (...) {
    }
    for (const auto& host : saved_hosts_)
      if (host.address == text(host_) && host.port == options_.port && host.fingerprint == text(fingerprint_)) existing_pair_ = true;
    if (existing_pair_ && saved_hosts_.empty()) {
      remoteos::HostEntry host;
      host.name = text(host_).substr(0, 80); host.address = text(host_); host.port = options_.port;
      host.fingerprint = text(fingerprint_); host.saved = true; saved_hosts_.push_back(std::move(host));
    }
    if (!text(host_).empty()) gtk_label_set_text(GTK_LABEL(verification_), (std::string(existing_pair_ ? "Saved host · " : "Host · ") + text(host_) + ":" + std::to_string(options_.port)).c_str());
  }
  void save_profile(const std::string &fingerprint) {
    try {
      std::filesystem::create_directories(state_);
      remoteos::write_private_file(
          std::filesystem::path(state_) / "profile.json",
          Json{{"host", options_.host},
               {"fingerprint", fingerprint},
               {"port", options_.port},
               {"match_host_resolution", options_.match_host_resolution},
               {"relative_mouse", relative_mouse_requested_},
               {"stream_profile", "wifi"},
               {"auto_connect", gtk_toggle_button_get_active(
                                    GTK_TOGGLE_BUTTON(auto_check_)) != FALSE}}
                  .dump(2) +
              "\n");
      auto found = std::find_if(saved_hosts_.begin(), saved_hosts_.end(), [&](const auto& h) { return h.address == options_.host && h.port == options_.port; });
      remoteos::HostEntry host;
      host.name = options_.host.substr(0, 80); host.address = options_.host; host.port = options_.port; host.fingerprint = fingerprint; host.saved = true;
      for (const auto& listed : listed_hosts_) if (listed.address == host.address && listed.port == host.port && listed.fingerprint == fingerprint) host.name = listed.name;
      if (found != saved_hosts_.end()) *found = host;
      else if (saved_hosts_.size() < 32) saved_hosts_.push_back(host);
      remoteos::write_private_file(std::filesystem::path(state_) / "hosts.json", Json(saved_hosts_).dump(2) + "\n");
      refresh_host_list();
      profile_save_error_.clear();
    } catch (const std::exception &e) {
      profile_save_error_ =
          std::string("Host profile could not be saved: ") + e.what();
      say(profile_save_error_);
    }
  }
  void refresh_host_list() {
    auto *children = gtk_container_get_children(GTK_CONTAINER(host_list_));
    for (auto *item = children; item; item = item->next) gtk_widget_destroy(GTK_WIDGET(item->data));
    g_list_free(children);
    listed_hosts_ = remoteos::merge_hosts(saved_hosts_, discovered_hosts_);
    for (const auto& host : listed_hosts_) {
      const auto status = host.identity_changed ? "Identity changed · verify again" : host.saved ? (host.available ? "Saved · Available" : "Saved · Not discovered") : "Available · Pair with PIN";
      auto *item = label((host.name + "  ·  " + status + "\n" + host.address + ":" + std::to_string(host.port)).c_str());
      gtk_widget_set_margin_start(item, 12); gtk_widget_set_margin_end(item, 12);
      gtk_widget_set_margin_top(item, 8); gtk_widget_set_margin_bottom(item, 8);
      gtk_list_box_insert(GTK_LIST_BOX(host_list_), item, -1);
    }
    gtk_widget_show_all(host_list_);
  }
  void select_host(int index) {
    if (index < 0 || std::size_t(index) >= listed_hosts_.size() || connected_ || connecting_) return;
    const auto& host = listed_hosts_[std::size_t(index)];
    if (host.identity_changed) {
      say("This host advertises a changed identity. Verify the new fingerprint in its app and enter it manually before pairing again."); return;
    }
    gtk_entry_set_text(GTK_ENTRY(host_), host.address.c_str());
    gtk_entry_set_text(GTK_ENTRY(port_entry_), std::to_string(host.port).c_str());
    gtk_entry_set_text(GTK_ENTRY(fingerprint_), host.fingerprint.c_str());
    gtk_entry_set_text(GTK_ENTRY(code_), "");
    existing_pair_ = host.saved;
    fingerprint_from_discovery_ = !host.saved;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(trust_check_), FALSE);
    if (host.saved) gtk_widget_hide(trust_check_); else gtk_widget_show(trust_check_);
    const auto fp = host.fingerprint.substr(0, 12);
    gtk_label_set_text(GTK_LABEL(verification_), (std::string(host.saved ? "Saved host · " : "Compare on host · ") + fp.substr(0, 4) + " " + fp.substr(4, 4) + " " + fp.substr(8, 4)).c_str());
    say(host.saved ? "Saved host selected. Connect without entering its PIN." : "Compare the verification value, then enter the six-digit PIN from the host app.");
  }
  void discover(bool manual = false) {
    if (!discovery_enabled_ || discovery_busy_ || connecting_ || connected_ || browser_) return;
    if (!manual && !network_status_.usable) return;
    discovery_busy_ = true;
    gtk_label_set_text(GTK_LABEL(discovery_status_), "Searching local network and online Tailscale peers…");
    auto addresses = tailnet_addresses_;
    bool local_network = true;
#ifdef REMOTEOS_CLIENT_UI_TEST
    addresses = {"127.0.0.1"}; local_network = false;
#endif
    background([addresses, local_network] { return Json(remoteos::discover_hosts(addresses, local_network)).dump(); }, [this](const std::string& output, const std::string& error) {
      discovery_busy_ = false;
      if (!error.empty()) { gtk_label_set_text(GTK_LABEL(discovery_status_), error.c_str()); return; }
      try {
        discovered_hosts_ = Json::parse(output).get<std::vector<remoteos::HostEntry>>();
        for (auto& host : discovered_hosts_) host.available = true;
        refresh_host_list();
        const auto status = discovered_hosts_.empty() ? std::string("No sharing hosts found. Start the host app, or enter its address.") : std::to_string(discovered_hosts_.size()) + " sharing host(s) found";
        gtk_label_set_text(GTK_LABEL(discovery_status_), status.c_str());
      } catch (const std::exception& e) { gtk_label_set_text(GTK_LABEL(discovery_status_), e.what()); }
    });
  }
  void create_video_windows() {
    for (auto *w : external_windows_)
      gtk_widget_destroy(w);
    external_windows_.clear();
    options_.displays.clear();
    options_.automatic_video_bitrate = true;
    options_.live_video_reconfigure = true;
    screen_rectangles_.clear();
    auto *display = gdk_display_get_default();
    auto *primary = gdk_display_get_primary_monitor(display);
    if (!primary)
      primary = gdk_display_get_monitor(display, 0);
    GdkRectangle geometry{0, 0, 1366, 768};
    if (primary)
      gdk_monitor_get_geometry(primary, &geometry);
    remoteos::Display internal;
    internal.id = "internal";
    internal.width = geometry.width;
    internal.height = geometry.height;
    internal.encoded_width = internal.width;
    internal.encoded_height = internal.height;
    internal.fps = 60; // Application host selects its ceiling; older hosts negotiate 30.
    options_.displays.push_back(internal);
    screen_rectangles_.push_back({geometry.x, geometry.y, geometry.width, geometry.height});
    gtk_window_set_default_size(GTK_WINDOW(session_window_), geometry.width,
                                geometry.height);
    gtk_widget_show_all(session_window_);
    gtk_widget_realize(video_);
    cursor_->set_window(gtk_widget_get_window(video_));
    if (!windowed_)
      fullscreen_window(session_window_, primary);
    for (int i = 0; !desktop_input_ && i < gdk_display_get_n_monitors(display) &&
                    options_.displays.size() < 2;
         ++i) {
      auto *monitor = gdk_display_get_monitor(display, i);
      if (monitor == primary)
        continue;
      GdkRectangle external;
      gdk_monitor_get_geometry(monitor, &external);
      remoteos::Display vga;
      vga.id = "vga";
      vga.width = std::min(1920, external.width);
      vga.height = std::min(1080, external.height);
      vga.x = internal.width;
      vga.encoded_width = vga.width;
      vga.encoded_height = vga.height;
      vga.bitrate = 9000000;
      vga.fps = 60;
      options_.displays.push_back(vga);
      screen_rectangles_.push_back({external.x, external.y, external.width, external.height});
      auto *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
      gtk_window_set_decorated(GTK_WINDOW(w), FALSE);
      gtk_window_move(GTK_WINDOW(w), external.x, external.y);
      gtk_window_set_default_size(GTK_WINDOW(w), external.width,
                                  external.height);
      pointer_events(w);
      gtk_widget_show_all(w);
      fullscreen_window(w, monitor);
      external_windows_.push_back(w);
    }
  }
  void schedule_session_fullscreen() {
    if (fullscreen_idle_) return;
    // Reassert the separate viewer after GTK has processed map/configure
    // events. It contains only the video drawable, with no setup requisition.
    fullscreen_idle_ = g_idle_add(+[](gpointer p) -> gboolean {
      auto* self = static_cast<Shell*>(p);
      self->fullscreen_idle_ = 0;
      if (!self->connected_ || self->overlay_open_) return G_SOURCE_REMOVE;
      auto* display = gdk_display_get_default();
      auto* monitor = gdk_display_get_primary_monitor(display);
      if (!monitor) monitor = gdk_display_get_monitor(display, 0);
      if (!self->windowed_) {
        fullscreen_window(self->session_window_, monitor);
        gtk_window_present(GTK_WINDOW(self->session_window_));
      }
      gdk_display_sync(display);
      self->media_.refresh_windows();
      self->update_mouse_capture();
      return G_SOURCE_REMOVE;
    }, this);
  }
  void connect_host() {
    if (connecting_ || connected_)
      return;
    ++setup_operation_;
    enrollment_busy_ = false;
    stop_browser();
    auto hostname = text(host_);
    if (hostname.empty()) {
      say("Enter a host IP address or hostname.");
      return;
    }
    if (!remoteos::media_available()) {
      say("This build has no media receiver. Rebuild with "
          "REMOTEOS_WITH_MEDIA=ON before starting a desktop session.");
      return;
    }
    try {
      options_.host = hostname;
      const auto port_text = text(port_entry_);
      std::size_t consumed = 0;
      options_.port = std::stoul(port_text, &consumed);
      if (consumed != port_text.size() || options_.port < 1024 || options_.port > 65535) throw std::runtime_error("Use a host port between 1024 and 65535");
      options_.bandwidth_limit_bps = remoteos::default_bandwidth_bps;
      options_.match_host_resolution = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(match_check_));
      if (fingerprint_from_discovery_ && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(trust_check_)))
        throw std::runtime_error("Compare the verification value in the host app, then check that it matches before pairing");
      options_.identity_dir =
          (std::filesystem::path(state_) / "identity").string();
      options_.host_fingerprint =
          remoteos::normalize_fingerprint(text(fingerprint_));
      if (options_.host_fingerprint.empty())
        throw std::runtime_error(
            "Enter the verified host fingerprint before connecting");
      options_.pairing_code = text(code_);
      gtk_entry_set_text(GTK_ENTRY(code_), "");
      create_video_windows();
      gtk_widget_hide(window_);
      gtk_widget_realize(video_);
      connecting_ = true;
      update_setup();
      host_label_ = hostname;
      say("Authenticating with " + hostname + "…");
      const auto generation = ++generation_;
      session_.connect(options_, [this, generation](const Json &event) {
        on_main([this, event, generation] {
          if (generation == generation_)
            session_event(event);
        });
      });
    } catch (const std::exception &e) {
      connecting_ = false;
      update_setup();
      say(e.what());
      gtk_widget_hide(session_window_);
      gtk_widget_show(window_);
      gtk_window_present(GTK_WINDOW(window_));
    }
  }
  void session_event(const Json &event) {
    const auto type = event.value("type", std::string{});
    try {
      if (type == "error") {
        const auto message =
            event.value("message", std::string("Connection failed"));
        const bool transient =
            connected_ || message.find("connect failed") != std::string::npos ||
            message.find("DNS resolution") != std::string::npos ||
            message.find("timed out") != std::string::npos;
        const bool retry =
            setup_.complete && transient &&
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auto_check_));
        stop();
        say(message);
        if (retry)
          schedule_reconnect();
      } else if (type == "capabilities") {
        host_usb_ = event.value("usb", false);
        host_resources_=event.value("userspace_devices",false);
      } else if(type=="open_resource") {
        const auto id=event.value("resource",std::string{});
        unsigned local=0;if(id=="camera"&&camera_export_)local=camera_export_->port();else if(id=="files"&&file_share_)local=file_share_->port();
        if(!local||!connected_)throw std::runtime_error("Resource request has no local exporter");
        const auto resource_generation=generation_;
        resource_tunnels_.open(options_.identity_dir,last_offer_.value("host",options_.host),event.at("port").get<unsigned>(),options_.host_fingerprint,event.at("session_id").get<std::string>(),id,event.at("token").get<std::string>(),local,[this,id,resource_generation](bool ready,const std::string& detail){on_main([this,id,ready,detail,resource_generation]{if(resource_generation==generation_)resources_message(resource_report_+"\n"+id+": "+(ready?"Authenticated relay connected":detail));});});
      } else if(type=="resource_status") {
        resource_report_="Forwarding active. Camera: 320×240 YUYV, 5 fps. SD transfers limited to 4.2 Mb/s.\nHost addresses (open on host):";
        for(const auto& item:event.value("resources",Json::array()))resource_report_+="\n"+item.value("resource",std::string{})+": "+item.value("url",std::string{});
        resources_message(resource_report_);
      } else if (type == "open_tunnel") {
        try {
          open_usb_tunnel(event);
        } catch (const std::exception &error) {
          usb_message(error.what());
        }
      } else if (type == "device_list") {
        for (const auto &device : event.value("devices", Json::array())) {
          const auto bus = device.value("bus_id", std::string{});
          if (!remoteos::devices::valid_bus_id(bus))
            continue;
          usb_states_[bus] = device.value("state", std::string("Unknown"));
          usb_pending_.erase(bus);
        }
        refresh_usb();
      } else if (type == "detach_device" && event.value("detached", false)) {
        unexport_usb(event.at("bus_id").get<std::string>());
      } else if (type == "status" && event.contains("host_media")) {
        if (event["host_media"].is_object()) host_media_ = event["host_media"];
        if (event.contains("adaptation") && event["adaptation"].is_array()) adaptation_status_ = event["adaptation"];
      } else if (type == "status" && event.contains("device_error")) {
        const auto bus = event.value("bus_id", std::string{});
        if (event.contains("device_token") &&
            (!usb_tokens_.count(bus) ||
             usb_tokens_.at(bus) != event["device_token"]))
          return;
        const bool detach_failed =
            usb_states_[bus] == "Waiting for host eject and flush";
        usb_pending_.erase(bus);
        usb_states_[bus] = event.at("device_error").get<std::string>();
        usb_message(usb_states_[bus]);
        if (!detach_failed && usb_exported_.count(bus))
          unexport_usb(bus);
        refresh_usb();
      } else if (type == "status" && event.contains("audio_status") &&
                 event["audio_status"].is_string()) {
        audio_status_ = event["audio_status"].get<std::string>();
      } else if (type == "status" && event.contains("latency_ms") &&
                 event["latency_ms"].is_number()) {
        latency_ms_ = event["latency_ms"].get<double>();
      } else if (type == "paired") {
        gtk_entry_set_text(
            GTK_ENTRY(fingerprint_),
            event.value("fingerprint", options_.host_fingerprint).c_str());
        save_profile(event.value("fingerprint", options_.host_fingerprint));
      } else if (type == "state") {
        if (event.contains("tls_cipher")) tls_cipher_ = event.value("tls_cipher", "");
        const auto state = event.value("state", "");
        if (state == "disconnected") {
          stop();
          say("Connection lost. Check network and reconnect.");
        } else
          say("Host: " + options_.host + "  ·  " + state);
      } else if (type == "video_profile") {
        const auto profiles = remoteos::parse_displays(event.at("displays"));
        // A tuning update preserves windows, media sockets, audio and input.
        // The receiver rejects geometry/codec changes on this path.
        if (!media_.update_profiles(profiles))
          throw std::runtime_error("Incompatible live video profile");
        options_.displays = profiles;
        last_offer_["displays"] = profiles;
      } else if (type == "offer") {
        auto offer = event;
        if (!offer.contains("host"))
          offer["host"] = options_.host;
        last_offer_ = offer;
        audio_status_ =
            offer.value("audio", false)
                ? "Opus · 48 kHz stereo"
                : offer.value("audio_status",
                              std::string("Host playback unavailable"));
        if (audio_status_.empty())
          audio_status_ = "Host playback unavailable";
        if (offer.contains("displays"))
          options_.displays = remoteos::parse_displays(offer["displays"]);
        connected_ = false;
        connecting_ = true;
        cursor_->set_hidden(false);
        if (broker_)
          broker_->set_forwarding(false);
        std::vector<std::uintptr_t> windows;
        windows.push_back(GDK_WINDOW_XID(gtk_widget_get_window(video_)));
        for (auto *window : external_windows_)
          windows.push_back(GDK_WINDOW_XID(gtk_widget_get_window(window)));
#ifdef REMOTEOS_CLIENT_UI_TEST
        if(!desktop_input_)std::fill(windows.begin(), windows.end(), 0); // Appliance tests lack XVideo; desktop tests use real ximagesink.
#endif
        const auto generation = generation_;
        const auto media_generation = ++media_generation_;
        local_media_verified_ = false;
        peer_media_ready_ = false;
        input_handoff_ready_ = !broker_;
        media_.start(
            offer, windows,
            remoteos::combined_identity_pem(options_.identity_dir),
            [this, generation, media_generation](const Json &message) {
              on_main([this, message, generation, media_generation] {
                if (generation != generation_ ||
                    media_generation != media_generation_)
                  return;
                const auto type = message.value("type", "");
                if (type == "media_verified") {
                  local_media_verified_ = true;
                  session_.send(Json{{"type", "media_ready"}});
                  finish_setup_if_ready();
                } else if (type == "keyframe")
                  session_.send(message);
                else if (type == "error") {
                  stop();
                  say(message.value("message", "Media pipeline failed"));
                }
              });
            });
      } else if (type == "media_ready") {
        media_.authorize();
        peer_media_ready_ = true;
        connecting_ = false;
        connected_ = true;
        reconnect_delay_ = 3;
        gtk_widget_hide(header_);
        gtk_widget_hide(status_);
        gtk_widget_hide(ephemeral_warning_);
        gtk_widget_hide(window_);
        gtk_window_set_transient_for(GTK_WINDOW(overlay_), GTK_WINDOW(session_window_));
        save_profile(options_.host_fingerprint);
        if (broker_ && !overlay_open_) {
          broker_->set_forwarding(true);
          input_handoff_ready_ = connected_ && broker_->forwarding();
        }
        if (desktop_input_) input_handoff_ready_ = true;
        if (!connected_)
          return;
        cursor_->set_hidden(!overlay_open_);
        finish_setup_if_ready();
        schedule_session_fullscreen();
        say("Connected to " + options_.host + " · MPEG-2 · " +
            (last_offer_.value("media_transport", "dtls-srtp") == "rtp-udp" ? "trusted UDP" : "encrypted UDP") + " · 40 Mbps limit" +
            " · press the power button for client controls");
      }
    } catch (const std::exception &e) {
      stop();
      say(e.what());
    }
  }
  void finish_setup_if_ready() {
    if (setup_.authenticated_session_ready(
            local_media_verified_, peer_media_ready_,
            connected_ && input_handoff_ready_)) {
      existing_pair_ = true;
      save_setup();
      update_setup();
      gtk_expander_set_expanded(GTK_EXPANDER(host_options_), TRUE);
    }
  }
  void release_all() {
    if (session_.running()) {
      try {
        session_.send(Json{{"type", "release_all"}});
      } catch (...) {
      }
    }
    scroll_x_ = scroll_y_ = 0;
  }
  void stop() {
    if (fullscreen_idle_) {
      g_source_remove(fullscreen_idle_);
      fullscreen_idle_ = 0;
    }
    ++setup_operation_;
    enrollment_busy_ = false;
    local_media_verified_ = false;
    peer_media_ready_ = false;
    input_handoff_ready_ = false;
    ++generation_;
    ++media_generation_;
    if (reconnect_timer_) {
      g_source_remove(reconnect_timer_);
      reconnect_timer_ = 0;
    }
    connected_ = false;
    connecting_ = false;
    relative_mouse_.stop();
    if (cursor_) cursor_->set_hidden(false);
    host_usb_ = false;host_resources_=false;
    audio_status_ = "Not connected";
    host_media_ = Json{};
    adaptation_status_ = Json{};
    latency_ms_ = -1;
    if (ephemeral_warning_ && ephemeral_state())
      gtk_widget_show(ephemeral_warning_);
    if (header_)
      gtk_widget_show(header_);
    if (status_)
      gtk_widget_show(status_);
    if (broker_)
      broker_->set_forwarding(false);
    relative_mouse_.stop();
    release_all();
    media_.stop();
    if (session_window_)
      gtk_widget_hide(session_window_);
    if (overlay_ && window_)
      gtk_window_set_transient_for(GTK_WINDOW(overlay_), GTK_WINDOW(window_));
    stop_usb();
    stop_resources();
    session_.disconnect();
    stop_browser();
    for (auto *w : external_windows_)
      gtk_widget_destroy(w);
    external_windows_.clear();
    if (stack_)
      gtk_stack_set_visible_child_name(GTK_STACK(stack_), "setup");
    if (notebook_)
      update_setup();
    if (window_)
      gtk_widget_show(window_);
  }
  void toggle_overlay() {
    if (overlay_open_) {
      close_overlay();
      return;
    }
    if (broker_)
      broker_->set_forwarding(false);
    relative_mouse_.stop();
    release_all();
    overlay_open_ = true;
    cursor_->set_hidden(false);
    refresh_status();
    gtk_button_set_label(GTK_BUTTON(resume_button_),
                         connected_ ? "Resume remote desktop"
                                    : "Back to setup");
    gtk_window_set_transient_for(GTK_WINDOW(overlay_),
                                GTK_WINDOW(connected_ || connecting_ ? session_window_ : window_));
    gtk_widget_show_all(overlay_);
    gtk_window_present(GTK_WINDOW(overlay_));
  }
  void close_overlay() {
    if (usb_window_)
      gtk_widget_hide(usb_window_);
    gtk_widget_hide(overlay_);
    overlay_open_ = false;
    release_all();
    if (broker_ && connected_) {
      broker_->set_forwarding(true);
      input_handoff_ready_ = connected_ && broker_->forwarding();
      finish_setup_if_ready();
    }
    cursor_->set_hidden(connected_ && !overlay_open_);
    if (connected_) schedule_session_fullscreen();
  }
  void refresh_status() {
    maintain_usb_lease();
    const auto s = status_reader_.read();
    const auto warning = battery_alerts_.observe(s.battery_percent, s.battery_state);
    if (warning != BatteryWarning::none) {
      if (!overlay_open_) toggle_overlay();
      const auto message = std::string(warning == BatteryWarning::critical
          ? "Battery critically low: " : "Battery low: ") +
          std::to_string(s.battery_percent) + "% — connect the power adapter.";
      gtk_label_set_text(GTK_LABEL(battery_notice_), message.c_str());
      gtk_widget_show(battery_notice_);
    } else if (s.battery_state != "Discharging" || s.battery_percent > 20) {
      gtk_widget_hide(battery_notice_);
    }
    std::ostringstream out;
    if (ephemeral_state())
      out << "Temporary storage: settings will be lost after power off.\n";
    if (!setup_save_error_.empty())
      out << setup_save_error_ << "\n";
    if (!profile_save_error_.empty())
      out << profile_save_error_ << "\n";
    out << "CPU  " << std::fixed << std::setprecision(0) << s.cpu_percent
        << "% · busiest logical CPU " << s.busiest_cpu_percent
        << "%\nMemory  " << s.used_kib / 1024 << " / " << s.total_kib / 1024
        << " MiB\n";
    if (s.battery_percent >= 0)
      out << "Battery  " << s.battery_percent << "% · " << s.battery_state
          << "\n";
    out << "Host  " << (host_label_.empty() ? "Disconnected" : host_label_)
        << "\nTailscale  " << tailscale_state_ << "\n";
    out << "Network  " << (network_status_.wired ? "Ethernet" : "Wi-Fi")
        << (network_status_.usable ? " connected" : " disconnected")
        << (network_status_.wired || network_status_.ssid.empty() ? "" : " · " + network_status_.ssid)
        << "\nInternet  " << internet_status_ << "\n";
    if (connected_)
      out << "Audio  " << audio_status_ << "\n";
    if (connected_) {
      out << "Route  " << tailscale_route_ << "\n";
      if (latency_ms_ >= 0)
        out << "Control round trip  " << std::setprecision(1) << latency_ms_
            << " ms\n";
    }
    std::ifstream wireless("/proc/net/wireless");
    for (std::string line; std::getline(wireless, line);)
      if (line.find(':') != std::string::npos) {
        std::istringstream row(line.substr(line.find(':') + 1));
        unsigned flags = 0;
        double quality = 0, signal = 0;
        row >> flags >> quality >> signal;
        out << "Wi-Fi  " << signal << " dBm\n";
        break;
      }
    if (connected_)
      for (const auto &display : options_.displays) {
        out << display.id << "  " << display.width << "×" << display.height
            << " logical · " << display.encoded_width << "×"
            << display.encoded_height << " stream · target " << display.fps
            << " fps · "
            << (display.codec == "raw-i420" ? "raw YUV420" : "MPEG-2")
            << " · " << std::setprecision(1) << display.bitrate / 1000000.0 << " Mb/s codec budget\n";
        if (adaptation_status_.is_array()) for (const auto &state : adaptation_status_)
          if (state.value("id", "") == display.id)
            out << "Requested  " << state.value("requested_fps", display.fps) << " fps · " << state.value("reason", "") << "\n";
        if (display.codec == "raw-i420")
          out << "Raw payload target  " << std::setprecision(1)
              << double(display.encoded_width) * display.encoded_height *
                     display.fps * 12.0 / 1000000.0
              << " Mb/s\n";
      }
    if (desktop_input_) out << "Desktop input · Ctrl+Alt+Home opens client controls\n";
    else if (!broker_)
      out << "Input forwarding disabled in this launch\n";
    if (!tls_cipher_.empty()) out << "Control encryption  " << tls_cipher_ << "\n";
    out << "USB devices  " << usb_exported_.size() << " exported · "
        << usb_pending_.size() << " pending\n";
    if (connected_)
      out << "Media transport  " << (last_offer_.value("media_transport", "dtls-srtp") == "rtp-udp"
          ? "RTP/UDP · trusted network · no media encryption" : "RTP/UDP · DTLS-SRTP encryption")
          << "\n";
    if (!recording_status_.empty())
      out << recording_status_ << "\n";
    try {
      const auto stats = media_.stats();
      telemetry_.publish(connected_, options_.displays, stats, latency_ms_,
                         usb_exported_.size());
      if (stats.contains("streams"))
        for (const auto &stream : stats["streams"]) {
          if (stream.value("kind", std::string{}) == "video") {
            out << stream.value("id", std::string("Display")) << " measured  "
                << std::setprecision(1) << stream.value("fps", 0.0) << " displayed fps · "
                << stream.value("decoded_fps", 0.0) << " decoded fps · "
                << stream.value("drop_fraction", 0.0) * 100 << "% drops · video loss "
                << stream.value("loss", 0.0) * 100 << "% · playout "
                << stream.value("playout_latency_ms", 0.0) << " ms · decode "
                << stream.value("decode_ms", 0.0) << " ms · receiver timeline "
                << stream.value("receiver_timeline_delay_ms", 0.0) << " ms\n";
            out << "  " << stream.value("sink", "video output") << " · " << stream.value("output_adapter", "")
                << " · packet repair window " << stream.value("jitter_ms", 40u) << " ms"
                << " · fresh presentation; clock QoS " << (stream.value("clock_qos", false) ? "on" : "off")
                << " · totals: display queue " << stream.value("queue_replaced_frames", 0ull)
                << ", compressed queue " << stream.value("compressed_queue_drops", 0ull)
                << ", decoder " << stream.value("decoder_dropped_frames", 0ull)
                << ", recovery " << stream.value("recovery_discarded_frames", 0ull) << "\n";
            out << "  Received " << stream.value("receive_mbps", 0.0) << " Mb/s · presentation drops "
                << stream.value("presentation_drop_fraction", 0.0) * 100 << "% · recovery discards "
                << stream.value("recovery_drop_fraction", 0.0) * 100 << "%\n";
          } else
            out << "Audio  Opus · " << stream.value("buffers", 0u)
                << " decoded buffers\n";
        }
      if (connected_ && host_media_.is_object() && host_media_.contains("streams"))
        for (const auto &stream : host_media_["streams"])
          out << stream.value("id", "Display") << " host  " << stream.value("capture_fps", 0.0)
              << " capture · " << stream.value("encoded_fps", 0.0) << " encoded fps · "
              << stream.value("rtp_mbps", 0.0) << " Mb/s media UDP\n";
      if (connected_ && stats.contains("loss"))
        out << "Media loss  " << std::setprecision(1)
            << stats.value("loss", 0.0) * 100 << "% · queue "
            << stats.value("queue_ms", 0.0) << " ms\n";
      if (connected_) {
        Json feedback{{"type", "stats"}, {"cpu", std::max(s.cpu_percent, s.busiest_cpu_percent)}};
        for (const auto *key : {"loss", "queue_ms", "drop_fraction"})
          if (stats.contains(key) && stats[key].is_number())
            feedback[key] = stats[key];
        if (stats.contains("streams")) feedback["streams"] = stats["streams"];
        session_.send(feedback);
      }
    } catch (...) {
    }
    gtk_label_set_text(GTK_LABEL(overlay_status_), out.str().c_str());
  }
  bool confirm_disconnect() {
    if (usb_exported_.empty())
      return true;
    auto *dialog = gtk_message_dialog_new(
        GTK_WINDOW(overlay_open_ ? overlay_ : (connected_ || connecting_ ? session_window_ : window_)), GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_CANCEL,
        "Disconnect forwarded USB devices?");
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog),
        "Eject storage in the host desktop first. Disconnecting now unplugs "
        "all forwarded devices from the host.");
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Disconnect",
                          GTK_RESPONSE_ACCEPT);
    const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return response == GTK_RESPONSE_ACCEPT;
  }
  void suspend_client() {
    if (suspending_) return;
    const bool reconnect = connected_ || connecting_;
    suspending_ = true;
    if (broker_) broker_->set_forwarding(false);
    release_all();
    stop();
    close_overlay();
    say("Suspending this client. The host stays running.");
    background([desktop = desktop_input_] {
      if (!desktop) return supervisor("suspend");
      GError* error = nullptr;
      auto* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
      if (!bus) { const std::string why = error ? error->message : "System power service unavailable"; g_clear_error(&error); throw std::runtime_error(why); }
      auto* result = g_dbus_connection_call_sync(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", "Suspend", g_variant_new("(b)", TRUE), nullptr,
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
      g_object_unref(bus);
      if (!result) { const std::string why = error ? error->message : "Suspend failed"; g_clear_error(&error); throw std::runtime_error(why); }
      g_variant_unref(result); return std::string("resumed");
    },
      [this, reconnect](const std::string&, const std::string& error) {
        suspending_ = false;
        lid_closed_ = false;
        say(error.empty() ? "Client resumed. Reconnecting…" : "Suspend failed: " + error);
        if (error.empty() && reconnect) schedule_reconnect();
      });
  }
  void power(const char *action) {
    const std::string requested = action;
    auto *dialog = gtk_message_dialog_new(
        GTK_WINDOW(overlay_), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_CANCEL, "%s",
        requested == "reboot" ? "Restart this client?"
                              : "Power off this client?");
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog), "%s",
        usb_exported_.empty()
            ? "The remote session will disconnect. The host stays running."
            : "The remote session will disconnect and forwarded USB devices "
              "will be unplugged. Eject storage on the host first. The host "
              "stays running.");
    gtk_dialog_add_button(GTK_DIALOG(dialog),
                          requested == "reboot" ? "Restart client"
                                                : "Power off client",
                          GTK_RESPONSE_ACCEPT);
    const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response != GTK_RESPONSE_ACCEPT)
      return;
    stop();
    background([requested] { return supervisor(requested.c_str()); },
               [this](const std::string &result, const std::string &error) {
                 say(error.empty() ? result : error);
               });
  }
  void change_volume(const char *change) {
    command({"amixer", "sset", "Master", change},
            [this](const std::string &, const std::string &error) {
              if (!error.empty()) {
                say("Volume control: " + error);
                gtk_label_set_text(GTK_LABEL(overlay_status_), error.c_str());
              }
            });
  }
  void cancel_reconnect() {
    if (reconnect_timer_) {
      g_source_remove(reconnect_timer_);
      reconnect_timer_ = 0;
    }
  }
  void schedule_reconnect() {
    if (reconnect_timer_ || diagnostics_open_ || browser_ || network_prompt_ ||
        (!setup_.complete && setup_.step != SetupState::Host))
      return;
    const auto delay = reconnect_delay_;
    reconnect_delay_ = std::min(30u, reconnect_delay_ * 2);
    reconnect_timer_ = g_timeout_add_seconds(
        delay,
        +[](gpointer p) -> gboolean {
          auto *self = static_cast<Shell *>(p);
          self->reconnect_timer_ = 0;
          if (!self->diagnostics_open_ && !self->browser_ &&
              (self->setup_.complete || self->setup_.step == SetupState::Host))
            self->connect_host();
          return G_SOURCE_REMOVE;
        },
        this);
  }

  void monitors_changed() {
    if (!session_.running() || (!connected_ && !connecting_))
      return;
    ++media_generation_;
    local_media_verified_ = false;
    peer_media_ready_ = false;
    input_handoff_ready_ = false;
    connected_ = false;
    connecting_ = true;
    cursor_->set_hidden(false);
    if (broker_)
      broker_->set_forwarding(false);
    release_all();
    media_.stop();
    try {
      create_video_windows();
      session_.send(
          Json{{"type", "display_changed"}, {"displays", options_.displays},
               {"bandwidth_limit_bps", options_.bandwidth_limit_bps}, {"match_host_resolution", options_.match_host_resolution}});
      say("Display changed. Renegotiating the active screens…");
    } catch (const std::exception &e) {
      stop();
      say(e.what());
    }
  }
  void pointer_events(GtkWidget *widget) {
    gtk_widget_add_events(widget, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK |
                                    GDK_BUTTON_RELEASE_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    g_signal_connect(widget, "event", G_CALLBACK(+[](GtkWidget *, GdkEvent *event, gpointer p) -> gboolean {
      return static_cast<Shell *>(p)->pointer_event(event);
    }), this);
  }
  void move_pointer(double x, double y) {
    if (desktop_input_ && !screen_rectangles_.empty()) {
      auto* native = gtk_widget_get_window(video_);
      int left=0, top=0; gdk_window_get_origin(native, &left, &top);
      screen_rectangles_[0] = {left, top, gdk_window_get_width(native), gdk_window_get_height(native)};
    }
    if (const auto message = pointer_message(options_.displays, screen_rectangles_, x, y)) session_.send(*message);
  }
  void update_mouse_capture() {
    if (!relative_mouse_requested_ || !connected_ || overlay_open_) {
      relative_mouse_.stop(); return;
    }
    if (desktop_input_ && !gtk_window_is_active(GTK_WINDOW(session_window_))) return;
    try {
      relative_mouse_.start(video_, [this](int dx, int dy) {
        if (connected_ && !overlay_open_)
          session_.send({{"type","input"},{"kind","motion"},{"dx",dx},{"dy",dy}});
      });
    } catch (const std::exception& error) {
      // Leave the mode selected for an explicit Resume retry, but return
      // ownership and show why capture failed instead of moving absolutely.
      relative_mouse_.stop();
      if (!overlay_open_) toggle_overlay();
      gtk_label_set_text(GTK_LABEL(overlay_status_), error.what());
    }
  }
  gboolean pointer_event(GdkEvent *event) {
    bool input_allowed = desktop_input_ || (broker_ && broker_->forwarding());
#ifdef REMOTEOS_CLIENT_UI_TEST
    input_allowed = true; // GDK events only; never open or grab physical evdev devices.
#endif
    if (!connected_ || overlay_open_ || !input_allowed) return FALSE;
    try {
      double x = 0, y = 0;
      if (!relative_mouse_requested_ && gdk_event_get_root_coords(event, &x, &y)) move_pointer(x, y);
      if (relative_mouse_requested_ && !relative_mouse_.active()) return FALSE;
      if (event->type == GDK_BUTTON_PRESS || event->type == GDK_BUTTON_RELEASE) {
        const auto button = event->button.button;
        int code = button == 1 ? BTN_LEFT : button == 2 ? BTN_MIDDLE : button == 3 ? BTN_RIGHT :
                   button == 8 ? BTN_SIDE : button == 9 ? BTN_EXTRA : 0;
        if (code) session_.send({{"type", "input"}, {"kind", "button"}, {"code", code}, {"down", event->type == GDK_BUTTON_PRESS}});
      } else if (event->type == GDK_SCROLL) {
        double dx = 0, dy = 0;
        if (!gdk_event_get_scroll_deltas(event, &dx, &dy)) {
          dx = event->scroll.direction == GDK_SCROLL_RIGHT ? 1 : event->scroll.direction == GDK_SCROLL_LEFT ? -1 : 0;
          dy = event->scroll.direction == GDK_SCROLL_DOWN ? 1 : event->scroll.direction == GDK_SCROLL_UP ? -1 : 0;
        }
        scroll_x_ += dx; scroll_y_ -= dy;
        const auto ix = int(scroll_x_), iy = int(scroll_y_);
        scroll_x_ -= ix; scroll_y_ -= iy;
        if (ix || iy) session_.send({{"type", "input"}, {"kind", "scroll"}, {"dx", ix}, {"dy", iy}});
      } else if (event->type != GDK_MOTION_NOTIFY) return FALSE;
    } catch (const std::exception& e) { stop(); say(e.what()); }
    return TRUE;
  }
  gboolean desktop_key(GdkEventKey* event) {
    if (!connected_ || overlay_open_) return FALSE;
    if (event->keyval == GDK_KEY_Home && (event->state & GDK_CONTROL_MASK) && (event->state & GDK_MOD1_MASK)) {
      if (event->type == GDK_KEY_PRESS) toggle_overlay();
      return TRUE;
    }
    if (event->hardware_keycode < 8) return TRUE;
    const auto code = unsigned(event->hardware_keycode - 8);
    if (!keyboard_key(code)) return TRUE;
    try { session_.send({{"type","input"},{"kind","key"},{"code",code},{"down",event->type == GDK_KEY_PRESS}}); }
    catch (const std::exception& error) { stop(); say(error.what()); }
    return TRUE;
  }
  void input_event(InputEvent e) {
    if (!connected_ || overlay_open_)
      return;
    try {
      if (e.type == EV_KEY) {
        if (!keyboard_key(e.code)) return;
        session_.send(
            Json{{"type", "input"},
                 {"kind", "key"},
                 {"code", e.code},
                 {"down", e.value != 0},
                 {"repeat", e.value == 2}});
      }
    } catch (const std::exception &ex) {
      stop();
      say(ex.what());
    }
  }
};
} // namespace
#ifndef REMOTEOS_CLIENT_UI_TEST
int main(int argc, char **argv) {
  std::string state = "/data/remoteos/client";
  bool windowed = false, input = true, automatic = true, discovery = true, desktop = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--state-dir" && i + 1 < argc)
      state = argv[++i];
    else if (arg == "--windowed")
      windowed = true;
    else if (arg == "--fullscreen")
      windowed = false;
    else if (arg == "--desktop") {
      desktop = true; input = false; windowed = true;
      const auto* data = std::getenv("XDG_DATA_HOME");
      const auto* home = std::getenv("HOME");
      if (data && *data) state = (std::filesystem::path(data)/"remoteos-client").string();
      else if (home && *home) state = (std::filesystem::path(home)/".local/share/remoteos-client").string();
      else { std::cerr << "A user state directory is required\n"; return 2; }
    }
    else if (arg == "--no-input")
      input = false;
    else if (arg == "--no-autoconnect")
      automatic = false;
    else if (arg == "--no-discovery")
      discovery = false;
    else if (arg == "--help") {
      std::cout << "remoteos-client [--state-dir PATH] [--windowed] "
                   "[--desktop] [--fullscreen] [--no-input] [--no-autoconnect] [--no-discovery]\n";
      return 0;
    } else {
      std::cerr << "Unknown option: " << arg << "\n";
      return 2;
    }
  }
  gtk_init(&argc, &argv);
  // Carbon Gray 100 also needs GTK's native indicators and popup menus to use
  // the dark palette when the appliance has no user theme configuration.
  g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme",
               TRUE, nullptr);
  if (!GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
    std::cerr << "RemoteOS requires an X11 display for XVideo presentation.\n";
    return 1;
  }
  auto *css = gtk_css_provider_new();
  const auto path = std::filesystem::path(g_getenv("REMOTEOS_ASSET_DIR")
                                              ? g_getenv("REMOTEOS_ASSET_DIR")
                                              : REMOTEOS_ASSET_DIR) /
                    "carbon.css";
  GError *error = nullptr;
  gtk_css_provider_load_from_path(css, path.c_str(), &error);
  if (error) {
    std::cerr << error->message << "\n";
    g_error_free(error);
  }
  gtk_style_context_add_provider_for_screen(
      gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);
  Shell app(state, windowed, input, automatic, discovery, desktop);
  gtk_main();
  return 0;
}
#endif
