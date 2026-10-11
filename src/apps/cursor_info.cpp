#include <X11/Xlib.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/Xfixes.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>

int main(int argc, char **argv) {
  try {
    const bool initialize = argc == 2 && std::string(argv[1]) == "--initialize";
    if (argc > 2 || (argc == 2 && !initialize)) {
      std::cerr << "Usage: remoteos-cursor-info [--initialize]\n";
      return 2;
    }
    std::unique_ptr<Display, decltype(&XCloseDisplay)> display(XOpenDisplay(nullptr), XCloseDisplay);
    if (!display)
      throw std::runtime_error("Cannot open the authorized X display");
    if (initialize) {
      XcursorSetTheme(display.get(), "Adwaita");
      XcursorSetDefaultSize(display.get(), 24);
      const Cursor cursor = XcursorLibraryLoadCursor(display.get(), "left_ptr");
      if (!cursor)
        throw std::runtime_error("Cannot load the Adwaita left_ptr cursor");
      XDefineCursor(display.get(), DefaultRootWindow(display.get()), cursor);
      XFreeCursor(display.get(), cursor);
      XSync(display.get(), False);
    }
    int event = 0, error = 0;
    if (!XFixesQueryExtension(display.get(), &event, &error))
      throw std::runtime_error("XFixes cursor inspection is unavailable");
    std::unique_ptr<XFixesCursorImage, decltype(&XFree)> cursor(XFixesGetCursorImage(display.get()), XFree);
    if (!cursor)
      throw std::runtime_error("Cannot query the current cursor");
    const auto &image = *cursor;
    const unsigned long count = static_cast<unsigned long>(image.width) * image.height;
    if (count > 1024 * 1024)
      throw std::runtime_error("Cursor dimensions exceed the diagnostic limit");
    unsigned long opaque = 0, nontransparent = 0, light = 0, dark = 0;
    for (unsigned long i = 0; i < count; ++i) {
      const auto pixel = image.pixels[i];
      const unsigned alpha = (pixel >> 24) & 255;
      if (!alpha)
        continue;
      ++nontransparent;
      if (alpha == 255)
        ++opaque;
      const unsigned r = (pixel >> 16) & 255, g = (pixel >> 8) & 255, b = pixel & 255;
      if (alpha >= 192 && r >= 192 && g >= 192 && b >= 192)
        ++light;
      if (alpha >= 192 && r <= 64 && g <= 64 && b <= 64)
        ++dark;
    }
    nlohmann::json result{{"cursor_initialized", initialize},
                          {"argb_supported", static_cast<bool>(XcursorSupportsARGB(display.get()))},
                          {"position", {image.x, image.y}}, {"size", {image.width, image.height}},
                          {"hotspot", {image.xhot, image.yhot}},
                          {"nontransparent_pixels", nontransparent}, {"opaque_pixels", opaque},
                          {"light_pixels", light}, {"dark_pixels", dark},
                          {"name", image.name ? image.name : ""}};
    std::cout << "X11 cursor: " << result.dump() << '\n';
    return nontransparent ? 0 : 1;
  } catch (const std::exception &error) {
    std::cerr << "X11 cursor diagnostic failed: " << error.what() << '\n';
    return 1;
  }
}
