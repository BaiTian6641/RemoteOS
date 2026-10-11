#define REMOTEOS_CLIENT_UI_TEST 1
#include "../src/client/main.cpp"
#include <cstdio>

namespace {
struct ClientUiFixture {
  Shell& shell;
  void command(const Json& command) {
    const auto action = command.at("action").get<std::string>();
    if (action == "connect") {
      gtk_entry_set_text(GTK_ENTRY(shell.code_), command.value("pin", "").c_str());
      shell.connect_host();
    } else if (action == "power") shell.toggle_overlay();
    else if (action == "disconnect") shell.stop();
    else if (action == "relative") gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(shell.relative_mouse_check_), command.value("enabled", true));
    else if (action == "discover") shell.discover(true);
    else if (action == "prepare_setup") {
      // A setup page can acquire a larger requisition from fonts or diagnostic
      // text. Move/resize the shell as well, as a managed setup window may be.
      auto* setup = gtk_stack_get_child_by_name(GTK_STACK(shell.stack_), "setup");
      gtk_widget_set_size_request(setup, command.value("width", -1), command.value("height", -1));
      gtk_window_unfullscreen(GTK_WINDOW(shell.window_));
      gtk_window_move(GTK_WINDOW(shell.window_), 37, 29);
      gtk_window_resize(GTK_WINDOW(shell.window_), 800, 500);
    }
    else if (action == "reset_setup")
      gtk_widget_set_size_request(gtk_stack_get_child_by_name(GTK_STACK(shell.stack_), "setup"), -1, -1);
    else if (action == "key") shell.session_.send({{"type", "input"}, {"kind", "key"}, {"code", 29}, {"down", true}});
    else if (action == "quit") { shell.stop(); gtk_main_quit(); }
    auto geometry = [](GtkWidget* widget) {
      auto* native = gtk_widget_get_window(widget);
      int x = 0, y = 0, width = 0, height = 0;
      if (native) {
        gdk_window_get_origin(native, &x, &y);
        width = gdk_window_get_width(native);
        height = gdk_window_get_height(native);
      }
      return Json{{"x", x}, {"y", y}, {"width", width}, {"height", height}};
    };
    Json state{{"connected", shell.connected_}, {"overlay", shell.overlay_open_}, {"hosts", shell.listed_hosts_.size()},
               {"saved_hosts", shell.saved_hosts_.size()}, {"status", gtk_label_get_text(GTK_LABEL(shell.status_))},
               {"connecting", shell.connecting_}, {"local_media_verified", shell.local_media_verified_},
               {"relative_mouse", shell.relative_mouse_requested_}, {"mouse_captured", shell.relative_mouse_.active()},
               {"peer_media_ready", shell.peer_media_ready_}, {"offered", !shell.last_offer_.empty()},
               {"requested_displays", shell.options_.displays.size()}, {"media_stats", shell.media_.stats()},
               {"window_geometry", geometry(gtk_widget_get_toplevel(shell.video_))}, {"setup_window_geometry", geometry(shell.window_)}, {"video_geometry", geometry(shell.video_)},
               {"windowed", shell.windowed_},
               {"viewer_mapped", gtk_widget_get_mapped(gtk_widget_get_toplevel(shell.video_)) != FALSE},
               {"overlay_parent_is_viewer", gtk_window_get_transient_for(GTK_WINDOW(shell.overlay_)) == GTK_WINDOW(gtk_widget_get_toplevel(shell.video_))},
               {"setup_window_mapped", gtk_widget_get_mapped(shell.window_) != FALSE},
               {"viewer_decorated", gtk_window_get_decorated(GTK_WINDOW(gtk_widget_get_toplevel(shell.video_))) != FALSE},
               {"video_toplevel_is_setup", gtk_widget_get_toplevel(shell.video_) == shell.window_},
               {"header_visible", gtk_widget_get_visible(shell.header_) != FALSE},
               {"status_visible", gtk_widget_get_visible(shell.status_) != FALSE}};
    std::cout << state.dump() << std::endl;
  }
};
}
int main(int argc, char **argv) {
  if (argc != 2 || !std::getenv("REMOTEOS_PRIVATE_X_TEST")) return 77;
  const auto state_directory = std::string(argv[1]);
  gtk_init(&argc, &argv);
  g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, nullptr);
  auto *css = gtk_css_provider_new();
  gtk_css_provider_load_from_path(css, (std::filesystem::path(REMOTEOS_ASSET_DIR) / "carbon.css").c_str(), nullptr);
  gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);
  Shell shell(state_directory, false, false, false, true, std::getenv("REMOTEOS_DESKTOP_UI_TEST") != nullptr);
  ClientUiFixture fixture{shell};
  g_unix_fd_add(STDIN_FILENO, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP), +[](gint, GIOCondition condition, gpointer p) -> gboolean {
    if (condition & G_IO_HUP) { gtk_main_quit(); return G_SOURCE_REMOVE; }
    char *line = nullptr; std::size_t size = 0;
    const auto read = getline(&line, &size, stdin);
    try { if (read > 0) static_cast<ClientUiFixture *>(p)->command(Json::parse(std::string(line, read))); }
    catch (const std::exception& e) { std::cout << Json{{"error", e.what()}}.dump() << std::endl; }
    free(line); return G_SOURCE_CONTINUE;
  }, &fixture);
  gtk_main();
}
