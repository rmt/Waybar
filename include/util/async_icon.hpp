#pragma once

#include <giomm/cancellable.h>
#include <giomm/icon.h>
#include <gtkmm/icontheme.h>
#include <gtkmm/stylecontext.h>

#include <functional>
#include <string>
#include <vector>

namespace waybar::util {

struct IconSource {
  Glib::RefPtr<Gio::Icon> icon;
  // Empty means the shared default theme, looked up under its existing mutex.
  Glib::RefPtr<Gtk::IconTheme> theme;
  Glib::RefPtr<Gdk::Pixbuf> pixbuf;
  bool symbolic = false;
};

using IconSources = std::vector<IconSource>;
using IconReady = std::function<void(Glib::RefPtr<Gdk::Pixbuf>)>;

// All calls, theme lookups, finishes and completion callbacks run on the UI thread.
// GTK asynchronously reads/renders/scales the selected icon. Sources are tried in order.
// Cancel before destroying the receiver or replacing a request; cancelled requests never
// invoke ready, including cache hits which GTK may otherwise complete successfully.
void loadIconAsync(IconSources sources, int size, int scale,
                   const Glib::RefPtr<Gtk::StyleContext>& style,
                   const Glib::RefPtr<Gio::Cancellable>& cancellable, IconReady ready);

// Make a file-backed GIcon without testing/opening the path synchronously.
IconSource fileIconSource(const std::string& path);

}  // namespace waybar::util
