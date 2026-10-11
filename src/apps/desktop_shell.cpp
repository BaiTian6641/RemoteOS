#include <gtk/gtk.h>
#include <gio/gdesktopappinfo.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {
GtkWidget *panel, *background;
const char *runtime_keys[] = {"LD_LIBRARY_PATH", "GST_PLUGIN_PATH", "GST_PLUGIN_PATH_1_0", "GST_PLUGIN_SYSTEM_PATH_1_0", "GST_REGISTRY", "GST_REGISTRY_1_0", "GST_REGISTRY_FORK", "FONTCONFIG_FILE", "PIPEWIRE_MODULE_DIR", "SPA_PLUGIN_DIR", "WIREPLUMBER_MODULE_DIR", "WIREPLUMBER_DATA_DIR"};
char **application_env() {
  auto **env = g_get_environ();
  for (const auto *key : runtime_keys) env = g_environ_unsetenv(env, key);
  return env;
}
void launch(GAppInfo *app, GtkWidget *parent) {
  GError *error = nullptr;
  // Browsers commonly keep singleton state outside XDG directories. Give
  // these known implementations a profile owned by this desktop explicitly.
  const std::string executable = g_app_info_get_executable(app) ? g_app_info_get_executable(app) : "";
  auto *context = gdk_display_get_app_launch_context(gdk_display_get_default());
  for (const auto *key : runtime_keys) g_app_launch_context_unsetenv(G_APP_LAUNCH_CONTEXT(context), key);
  if (executable.find("firefox") != std::string::npos || executable.find("chromium") != std::string::npos || executable.find("google-chrome") != std::string::npos) {
    const std::string profile = std::string(g_get_user_data_dir()) + "/browsers/" + std::filesystem::path(executable).filename().string();
    std::filesystem::create_directories(profile);
    std::vector<const char *> argv{executable.c_str()};
    const std::string chrome_profile = "--user-data-dir=" + profile;
    if (executable.find("firefox") != std::string::npos) { argv.push_back("--no-remote"); argv.push_back("--profile"); argv.push_back(profile.c_str()); }
    else argv.push_back(chrome_profile.c_str());
    argv.push_back(nullptr);
    auto **env = application_env();
    g_spawn_async(nullptr, const_cast<char **>(argv.data()), env, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error);
    g_strfreev(env);
  } else g_app_info_launch(app, nullptr, G_APP_LAUNCH_CONTEXT(context), &error);
  g_object_unref(context);
  if (error) {
    auto *dialog = gtk_message_dialog_new(GTK_WINDOW(parent), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                          "Cannot open application: %s", error->message);
    gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog); g_clear_error(&error);
  }
}
void apps(GtkButton *, gpointer) {
  auto *dialog = gtk_dialog_new_with_buttons("Applications · independent desktop", GTK_WINDOW(panel), GTK_DIALOG_DESTROY_WITH_PARENT,
                                              "Close", GTK_RESPONSE_CLOSE, nullptr);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 480, 420);
  auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
  auto *list = gtk_list_box_new(); gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_container_add(GTK_CONTAINER(scroll), list);
  gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), scroll);
  std::vector<GAppInfo *> entries;
  auto *all = g_app_info_get_all();
  for (auto *item = all; item; item = item->next) if (g_app_info_should_show(G_APP_INFO(item->data))) entries.push_back(G_APP_INFO(item->data));
  std::sort(entries.begin(), entries.end(), [](auto *a, auto *b) { return g_utf8_collate(g_app_info_get_display_name(a), g_app_info_get_display_name(b)) < 0; });
  for (auto *app : entries) {
    auto *button = gtk_button_new_with_label(g_app_info_get_display_name(app));
    g_object_set_data_full(G_OBJECT(button), "app", g_object_ref(app), g_object_unref);
    g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton *button, gpointer p) { launch(G_APP_INFO(g_object_get_data(G_OBJECT(button), "app")), GTK_WIDGET(p)); }), dialog);
    gtk_container_add(GTK_CONTAINER(list), button);
  }
  g_list_free_full(all, g_object_unref);
  gtk_widget_show_all(dialog); gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
}
void geometry() {
  auto *screen = gdk_screen_get_default();
  auto *display = gdk_display_get_default();
  auto *monitor = gdk_display_get_primary_monitor(display);
  if (!monitor) monitor = gdk_display_get_monitor(display, 0);
  GdkRectangle size{}; if (monitor) gdk_monitor_get_geometry(monitor, &size);
  if (size.width <= 0) return;
  gtk_window_move(GTK_WINDOW(panel), size.x, size.y); gtk_window_resize(GTK_WINDOW(panel), size.width, 44);
  auto *x = GDK_DISPLAY_XDISPLAY(display);
  const Window root = DefaultRootWindow(x);
  XWindowAttributes attributes{}; XGetWindowAttributes(x, root, &attributes);
  gtk_window_move(GTK_WINDOW(background), 0, 0); gtk_window_resize(GTK_WINDOW(background), attributes.width, attributes.height);
  if (auto *native = gtk_widget_get_window(panel)) {
    unsigned long strut[12]{}; strut[2] = size.y + 44; strut[8] = size.x; strut[9] = size.x + size.width - 1;
    const auto cardinal = XInternAtom(x, "CARDINAL", False);
    XChangeProperty(x, GDK_WINDOW_XID(native), XInternAtom(x, "_NET_WM_STRUT_PARTIAL", False), cardinal, 32, PropModeReplace, reinterpret_cast<unsigned char *>(strut), 12);
    XFlush(x);
  }
  (void)screen;
}
}
int main(int argc, char **argv) {
  const auto *owned = std::getenv("REMOTEOS_INDEPENDENT_DESKTOP");
  if (!owned || std::string(owned) != "1") return 2;
  gtk_init(&argc, &argv);
  g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, nullptr);
  auto *css = gtk_css_provider_new();
  gtk_css_provider_load_from_data(css, "* {font-family:'IBM Plex Sans',sans-serif;font-size:14px;} window {background:#161616;color:#f4f4f4;} button {background-image:none;background:#393939;color:#f4f4f4;border-radius:0;padding:6px 12px;} button:focus {outline:2px solid white;} .desktop {background:#262626;} label.hint {color:#c6c6c6;font-size:20px;}", -1, nullptr);
  gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION); g_object_unref(css);
  background = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(background), "RemoteOS independent desktop");
  gtk_window_set_type_hint(GTK_WINDOW(background), GDK_WINDOW_TYPE_HINT_DESKTOP);
  gtk_style_context_add_class(gtk_widget_get_style_context(background), "desktop");
  auto *hint = gtk_label_new("Your independent HP desktop\nOpen Applications to start working. Alt+Tab switches windows.");
  gtk_style_context_add_class(gtk_widget_get_style_context(hint), "hint"); gtk_label_set_justify(GTK_LABEL(hint), GTK_JUSTIFY_CENTER);
  gtk_container_add(GTK_CONTAINER(background), hint);
  panel = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_title(GTK_WINDOW(panel), "RemoteOS applications");
  gtk_window_set_type_hint(GTK_WINDOW(panel), GDK_WINDOW_TYPE_HINT_DOCK); gtk_window_set_decorated(GTK_WINDOW(panel), FALSE); gtk_window_set_resizable(GTK_WINDOW(panel), FALSE);
  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_container_add(GTK_CONTAINER(panel), box);
  auto *applications = gtk_button_new_with_label("Applications"); gtk_box_pack_start(GTK_BOX(box), applications, FALSE, FALSE, 0); g_signal_connect(applications, "clicked", G_CALLBACK(apps), nullptr);
  auto *terminal = gtk_button_new_with_label("Terminal"); gtk_box_pack_start(GTK_BOX(box), terminal, FALSE, FALSE, 0);
  g_signal_connect(terminal, "clicked", G_CALLBACK((+[](GtkButton *, gpointer) { GError *error = nullptr; const char *argv[] = {"xterm", nullptr}; auto **env = application_env(); g_spawn_async(g_get_home_dir(), const_cast<char **>(argv), env, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error); g_strfreev(env); if (error) { g_warning("%s", error->message); g_clear_error(&error); } })), nullptr);
  auto *label = gtk_label_new("Independent desktop · save before Stop sharing"); gtk_box_pack_end(GTK_BOX(box), label, FALSE, FALSE, 10);
  gtk_widget_show_all(background); gtk_widget_show_all(panel); geometry();
  g_signal_connect(gdk_screen_get_default(), "monitors-changed", G_CALLBACK(+[](GdkScreen *, gpointer) { geometry(); }), nullptr);
  gtk_main(); return 0;
}
