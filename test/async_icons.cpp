#include <giomm/themedicon.h>
#include <glibmm/main.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/label.h>
#include <gtkmm/main.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "util/async_icon.hpp"
#include "util/scope_guard.hpp"

namespace {

using Pixbuf = Glib::RefPtr<Gdk::Pixbuf>;
using waybar::util::IconSources;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename Predicate>
bool until(Predicate done) {
  auto context = Glib::MainContext::get_default();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    context->iteration(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return done();
}

struct Result {
  bool done = false;
  Pixbuf pixbuf;
  std::thread::id thread;
};

Pixbuf load(IconSources sources, int size, int scale,
            const Glib::RefPtr<Gtk::StyleContext>& style = {}) {
  auto result = std::make_shared<Result>();
  auto cancellable = Gio::Cancellable::create();
  waybar::util::ScopeGuard cancel([cancellable] { cancellable->cancel(); });
  waybar::util::loadIconAsync(std::move(sources), size, scale, style, cancellable,
                              [result](Pixbuf pixbuf) {
                                result->pixbuf = pixbuf;
                                result->thread = std::this_thread::get_id();
                                result->done = true;
                              });
  require(until([&] { return result->done; }), "Async icon load timed out");
  require(result->thread == std::this_thread::get_id(), "Icon completed on the wrong thread");
  return result->pixbuf;
}

bool color(const Pixbuf& pixbuf, int red, int green, int blue) {
  if (!pixbuf) return false;
  const auto* pixel = pixbuf->get_pixels() + (pixbuf->get_height() / 2) * pixbuf->get_rowstride() +
                      (pixbuf->get_width() / 2) * pixbuf->get_n_channels();
  return std::abs(pixel[0] - red) < 5 && std::abs(pixel[1] - green) < 5 &&
         std::abs(pixel[2] - blue) < 5;
}

void svg(const std::filesystem::path& path, const char* fill, int width = 16, int height = 16) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  file << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "\" height=\"" << height
       << "\"><rect width=\"100%\" height=\"100%\" fill=\"" << fill << "\"/></svg>";
}

}  // namespace

int main(int argc, char** argv) {
  // This suite is exercised by the private headless smoke test, not by opening a desktop window.
  if (!gtk_init_check(&argc, &argv)) return 77;
  Gtk::Main gtk(argc, argv);
  GError* error = nullptr;
  char* temporary = g_dir_make_tmp("waybar-async-icons-XXXXXX", &error);
  if (!temporary) {
    std::cerr << (error ? error->message : "Cannot create test directory") << '\n';
    g_clear_error(&error);
    return 1;
  }
  const std::filesystem::path root(temporary);
  g_free(temporary);
  waybar::util::ScopeGuard cleanup([root] {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  });

  try {
    svg(root / "red.svg", "#ff0000");
    svg(root / "blue.svg", "#0000ff");
    svg(root / "wide.svg", "#ff0000", 32, 16);
    std::ofstream(root / "broken.svg") << "not an image";
    auto file = [&](const char* name) {
      return waybar::util::fileIconSource((root / name).string());
    };

    auto red = load({file("missing.svg"), file("broken.svg"), file("red.svg")}, 16, 1);
    require(color(red, 255, 0, 0), "Missing/corrupt file fallback failed");
    auto wide = load({file("wide.svg")}, 16, 1);
    require(wide && wide->get_width() == 2 * wide->get_height(), "Aspect ratio was lost");
    auto doubled = load({file("red.svg")}, 16, 2);
    require(doubled && doubled->get_width() == 32 && doubled->get_height() == 32,
            "File icon did not honor output scale");
    auto direct = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, true, 8, 16, 16);
    direct->fill(0x00ff00ff);
    require(color(load({file("missing.svg"), {.pixbuf = direct}}, 16, 1), 0, 255, 0),
            "Cached SNI pixmap fallback failed");

    const auto theme_root = root / "icons" / "probe";
    std::filesystem::create_directories(theme_root);
    std::ofstream(theme_root / "index.theme")
        << "[Icon Theme]\nName=Probe\nDirectories=16x16/apps\n"
           "[16x16/apps]\nSize=16\nType=Scalable\nMinSize=1\nMaxSize=128\nContext=Applications\n";
    svg(theme_root / "16x16/apps/probe.svg", "#ff0000");
    svg(theme_root / "16x16/apps/probe-symbolic.svg", "#bebebe");
    auto theme = Gtk::IconTheme::create();
    theme->set_search_path({(root / "icons").string()});
    theme->set_custom_theme("probe");
    auto named = Gio::ThemedIcon::create("probe");
    require(color(load({{.icon = named, .theme = theme}}, 16, 1), 255, 0, 0),
            "Named custom-theme icon failed");
    auto named_double = load({{.icon = named, .theme = theme}}, 16, 2);
    require(named_double && named_double->get_width() == 32, "Named HiDPI icon failed");
    Gtk::Label label;
    auto css = Gtk::CssProvider::create();
    css->load_from_data("* { color: #00ff00; }");
    auto style = label.get_style_context();
    style->add_provider(css, GTK_STYLE_PROVIDER_PRIORITY_USER);
    auto symbolic = Gio::ThemedIcon::create("probe-symbolic");
    require(color(load({{.icon = symbolic, .theme = theme, .symbolic = true}}, 16, 1, style), 0,
                  255, 0),
            "Symbolic foreground color was lost");

    // Cancelled completions must not touch an obsolete receiver, even for cached GTK loads.
    auto cancelled = Gio::Cancellable::create();
    auto receiver = std::make_shared<Result>();
    std::weak_ptr<Result> released = receiver;
    int obsolete_calls = 0;
    waybar::util::loadIconAsync({file("red.svg")}, 16, 1, {}, cancelled,
                                [receiver, &obsolete_calls](Pixbuf) { ++obsolete_calls; });
    receiver.reset();
    cancelled->cancel();
    auto blue = load({file("blue.svg")}, 16, 1);
    require(color(blue, 0, 0, 255), "Replacement icon load failed");
    require(until([&] { return released.expired(); }),
            "Cancelled icon request leaked its receiver");
    require(obsolete_calls == 0, "Cancelled result replaced a newer icon");
    require(!load({file("missing.svg")}, 16, 1), "Missing icon unexpectedly succeeded");
    std::cout << "Async icons passed: file/name/pixmap fallbacks, aspect ratio, HiDPI, symbolic "
                 "colors, cancellation and stale results.\n";
    return 0;
  } catch (const Glib::Error& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
