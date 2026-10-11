#include <algorithm>
#include <cstdio>
#include <gtk/gtk.h>
#include <string>
#include <unistd.h>
#include <vector>
#include <webkit2/webkit2.h>
#ifndef REMOTEOS_ASSET_DIR
#define REMOTEOS_ASSET_DIR "/usr/share/remoteos"
#endif

namespace {
std::vector<GtkWidget *> windows;
struct Window {
  GtkWidget *window{}, *address{}, *message{};
  WebKitWebView *view{};
};
bool safe_url(const char *text) {
  if (!text)
    return false;
  const std::string url(text);
  if (url.size() > 8192 ||
      std::any_of(url.begin(), url.end(), [](unsigned char c) {
        return c <= 32 || c == 127 || c == '\\';
      }))
    return false;
  const std::size_t start = url.rfind("https://", 0) == 0  ? 8
                            : url.rfind("http://", 0) == 0 ? 7
                                                           : 0;
  if (!start)
    return false;
  const auto authority =
      url.substr(start, url.find_first_of("/?#", start) - start);
  return !authority.empty() && authority.find('@') == std::string::npos;
}
void info(Window *w, const char *message) {
  gtk_label_set_text(GTK_LABEL(w->message), message);
  gtk_widget_show(w->message);
}
void load(Window *w) {
  const char *url = gtk_entry_get_text(GTK_ENTRY(w->address));
  if (!safe_url(url)) {
    info(w,
         "Enter an http:// or https:// address without embedded credentials.");
    return;
  }
  gtk_widget_hide(w->message);
  webkit_web_view_load_uri(w->view, url);
}
Window *make_window(WebKitWebContext *, WebKitWebView *related = nullptr);
WebKitWebView *create_popup(WebKitWebView *parent, WebKitNavigationAction *,
                            gpointer) {
  auto *window = make_window(webkit_web_view_get_context(parent), parent);
  return window->view;
}
Window *make_window(WebKitWebContext *context, WebKitWebView *related) {
  auto *w = new Window;
  w->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  GdkRectangle geometry{0, 0, 1024, 576};
  auto *display = gdk_display_get_default();
  auto *monitor = gdk_display_get_primary_monitor(display);
  if (!monitor)
    monitor = gdk_display_get_monitor(display, 0);
  if (monitor)
    gdk_monitor_get_geometry(monitor, &geometry);
  gtk_window_set_default_size(GTK_WINDOW(w->window), geometry.width,
                              geometry.height);
  gtk_window_set_title(GTK_WINDOW(w->window), "RemoteOS • Network sign-in");
  windows.push_back(w->window);
  auto *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(w->window), column);
  auto *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(bar), 8);
  gtk_box_pack_start(GTK_BOX(column), bar, FALSE, FALSE, 0);
  auto add = [&](const char *text, GCallback cb) {
    auto *b = gtk_button_new_with_label(text);
    gtk_box_pack_start(GTK_BOX(bar), b, FALSE, FALSE, 0);
    g_signal_connect(b, "clicked", cb, w);
  };
  add("Back", G_CALLBACK(+[](GtkButton *, gpointer p) {
        webkit_web_view_go_back(static_cast<Window *>(p)->view);
      }));
  add("Reload", G_CALLBACK(+[](GtkButton *, gpointer p) {
        webkit_web_view_reload(static_cast<Window *>(p)->view);
      }));
  w->address = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(w->address), "https://");
  gtk_box_pack_start(GTK_BOX(bar), w->address, TRUE, TRUE, 0);
  g_signal_connect(w->address, "activate",
                   G_CALLBACK(+[](GtkEntry *, gpointer p) {
                     load(static_cast<Window *>(p));
                   }),
                   w);
  add("Go", G_CALLBACK(+[](GtkButton *, gpointer p) {
        load(static_cast<Window *>(p));
      }));
  add("Done", G_CALLBACK(+[](GtkButton *, gpointer p) {
        gtk_widget_destroy(static_cast<Window *>(p)->window);
      }));
  w->message = gtk_label_new(nullptr);
  gtk_label_set_line_wrap(GTK_LABEL(w->message), TRUE);
  gtk_widget_set_margin_start(w->message, 12);
  gtk_widget_set_margin_end(w->message, 12);
  gtk_widget_set_margin_top(w->message, 12);
  gtk_widget_set_margin_bottom(w->message, 12);
  gtk_box_pack_start(GTK_BOX(column), w->message, FALSE, FALSE, 0);
  w->view =
      WEBKIT_WEB_VIEW(related ? webkit_web_view_new_with_related_view(related)
                              : webkit_web_view_new_with_context(context));
  auto *settings = webkit_web_view_get_settings(w->view);
  webkit_settings_set_enable_developer_extras(settings, FALSE);
  webkit_settings_set_enable_write_console_messages_to_stdout(settings, FALSE);
  webkit_settings_set_hardware_acceleration_policy(
      settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
  // Keep the stock browser user agent. Authentication providers decide whether
  // this independently launched browser is supported; never spoof a browser.
  gtk_box_pack_start(GTK_BOX(column), GTK_WIDGET(w->view), TRUE, TRUE, 0);
  g_signal_connect(w->view, "notify::uri",
                   G_CALLBACK(+[](GObject *, GParamSpec *, gpointer p) {
                     auto *w = static_cast<Window *>(p);
                     if (auto *uri = webkit_web_view_get_uri(w->view))
                       gtk_entry_set_text(GTK_ENTRY(w->address), uri);
                   }),
                   w);
  g_signal_connect(
      w->view, "load-failed",
      G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent, const gchar *,
                     GError *error, gpointer p) -> gboolean {
        info(static_cast<Window *>(p), error->message);
        return FALSE;
      }),
      w);
  g_signal_connect(
      w->view, "load-failed-with-tls-errors",
      G_CALLBACK(+[](WebKitWebView *, const gchar *, GTlsCertificate *,
                     GTlsCertificateFlags, gpointer p) -> gboolean {
        info(static_cast<Window *>(p),
             "The site's certificate could not be verified. Check the client "
             "clock and network. Certificate checks cannot be bypassed.");
        return TRUE;
      }),
      w);
  g_signal_connect(
      w->view, "web-process-terminated",
      G_CALLBACK(
          +[](WebKitWebView *, WebKitWebProcessTerminationReason, gpointer p) {
            info(static_cast<Window *>(p),
                 "The browser process stopped. Close unused windows and "
                 "reload, or choose Done to return to setup.");
          }),
      w);
  g_signal_connect(
      w->view, "permission-request",
      G_CALLBACK(+[](WebKitWebView *, WebKitPermissionRequest *request,
                     gpointer) -> gboolean {
        webkit_permission_request_deny(request);
        return TRUE;
      }),
      nullptr);
  g_signal_connect(w->view, "create", G_CALLBACK(create_popup), nullptr);
  g_signal_connect(w->view, "close",
                   G_CALLBACK(+[](WebKitWebView *, gpointer p) {
                     gtk_widget_destroy(static_cast<Window *>(p)->window);
                   }),
                   w);
  g_signal_connect(
      w->view, "decide-policy",
      G_CALLBACK(+[](WebKitWebView *, WebKitPolicyDecision *decision,
                     WebKitPolicyDecisionType type, gpointer p) -> gboolean {
        if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION ||
            type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
          auto *action =
              webkit_navigation_policy_decision_get_navigation_action(
                  WEBKIT_NAVIGATION_POLICY_DECISION(decision));
          const auto *uri = webkit_uri_request_get_uri(
              webkit_navigation_action_get_request(action));
          if (!safe_url(uri) && std::string(uri ? uri : "") != "about:blank") {
            webkit_policy_decision_ignore(decision);
            info(static_cast<Window *>(p), "Only HTTP and HTTPS navigation is "
                                           "allowed in this sign-in browser.");
            return TRUE;
          }
        }
        return FALSE;
      }),
      w);
  g_signal_connect(
      w->window, "destroy", G_CALLBACK(+[](GtkWidget *widget, gpointer p) {
        windows.erase(std::remove(windows.begin(), windows.end(), widget),
                      windows.end());
        delete static_cast<Window *>(p);
        if (windows.empty())
          gtk_main_quit();
      }),
      w);
  gtk_widget_show_all(w->window);
  gtk_widget_hide(w->message);
  // The appliance has no window manager. Apply the panel geometry explicitly
  // as well as the fullscreen hint, including for the 1024x576 panel variant.
  gtk_window_move(GTK_WINDOW(w->window), geometry.x, geometry.y);
  gtk_window_resize(GTK_WINDOW(w->window), geometry.width, geometry.height);
  gtk_window_fullscreen(GTK_WINDOW(w->window));
  return w;
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 2 || !safe_url(argv[1])) {
    std::fprintf(stderr, "Usage: remoteos-browser http[s]://address\n");
    return 2;
  }
  if (geteuid() == 0) {
    std::fprintf(stderr, "The authentication browser must run as the "
                         "unprivileged remoteos user.\n");
    return 1;
  }
  // No persistent authentication profile; tailscaled owns the durable device
  // identity. Browser subprocesses must remain sandboxed.
  g_unsetenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS");
  g_setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", TRUE);
  gtk_init(&argc, &argv);
  auto *css = gtk_css_provider_new();
  const std::string css_path = std::string(g_getenv("REMOTEOS_ASSET_DIR")
                                               ? g_getenv("REMOTEOS_ASSET_DIR")
                                               : REMOTEOS_ASSET_DIR) +
                               "/carbon.css";
  GError *css_error = nullptr;
  gtk_css_provider_load_from_path(css, css_path.c_str(), &css_error);
  if (css_error)
    g_error_free(css_error);
  else
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);
  auto *context = webkit_web_context_new_ephemeral();
  webkit_web_context_set_sandbox_enabled(context, TRUE);
  webkit_website_data_manager_set_tls_errors_policy(
      webkit_web_context_get_website_data_manager(context),
      WEBKIT_TLS_ERRORS_POLICY_FAIL);
  g_signal_connect(
      context, "download-started",
      G_CALLBACK(+[](WebKitWebContext *, WebKitDownload *download, gpointer) {
        webkit_download_cancel(download);
      }),
      nullptr);
  auto *window = make_window(context);
  gtk_entry_set_text(GTK_ENTRY(window->address), argv[1]);
  load(window);
  gtk_main();
  g_object_unref(context);
  return 0;
}
