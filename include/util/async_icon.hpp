#pragma once

#include <gio/gio.h>
#include <giomm/cancellable.h>
#include <giomm/icon.h>
#include <gtkmm/icontheme.h>
#include <gtkmm/stylecontext.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace waybar::util {

// Worker-produced pixbufs are returned to the GTK main thread before they are used.
// symbolic is true only for files whose name carries the symbolic convention.
struct IconWorkerResult {
  Glib::RefPtr<Gdk::Pixbuf> pixbuf;
  bool symbolic = false;
};

using IconWorker = std::function<IconWorkerResult(int, int, GCancellable*)>;


struct IconSource {
  Glib::RefPtr<Gio::Icon> icon;
  // Empty means the shared default theme, looked up under its existing mutex.
  Glib::RefPtr<Gtk::IconTheme> theme;
  Glib::RefPtr<Gdk::Pixbuf> pixbuf;
  bool symbolic = false;
  IconWorker worker;
};

using IconSources = std::vector<IconSource>;
using IconReady = std::function<void(Glib::RefPtr<Gdk::Pixbuf>)>;

// GTK theme calls, finishes and completion callbacks run on the UI thread. Worker sources run
// through util::runAsync and return their decoded pixbuf to that thread. Sources are tried in order.
// Cancel before destroying the receiver or replacing a request; cancelled requests never invoke
// ready, including cache hits which GTK may otherwise complete successfully.
void loadIconAsync(IconSources sources, int size, int scale,
                   const Glib::RefPtr<Gtk::StyleContext>& style,
                   const Glib::RefPtr<Gio::Cancellable>& cancellable, IconReady ready);

// Make a file-backed GIcon without testing/opening the path synchronously.
IconSource fileIconSource(const std::string& path);

}  // namespace waybar::util
