#include "util/icon_loader.hpp"

#include <giomm/themedicon.h>

#include "util/string.hpp"

std::vector<std::string> IconLoader::search_prefix() {
  std::vector<std::string> prefixes;

  const char* home_env = std::getenv("HOME");
  std::string home_dir = home_env ? home_env : "";
  if (!home_dir.empty()) {
    prefixes.push_back(home_dir + "/.local/share/");
  }

  auto xdg_data_dirs = std::getenv("XDG_DATA_DIRS");
  if (!xdg_data_dirs) {
    prefixes.emplace_back("/usr/share/");
    prefixes.emplace_back("/usr/local/share/");
  } else {
    std::string xdg_data_dirs_str(xdg_data_dirs);
    size_t start = 0;
    size_t end = 0;

    do {
      end = xdg_data_dirs_str.find(':', start);
      auto p = xdg_data_dirs_str.substr(start, end - start);
      prefixes.push_back(trim(p) + "/");

      start = end == std::string::npos ? end : end + 1;
    } while (end != std::string::npos);
  }

  for (auto& p : prefixes) spdlog::debug("Using 'desktop' search path prefix: {}", p);

  return prefixes;
}

Glib::RefPtr<Gio::DesktopAppInfo> IconLoader::get_app_info_by_name(
    const std::string& app_id, const std::vector<std::string>& prefixes,
    GCancellable* cancellable) {
  // App ids come from other clients: they name a desktop entry and are never a path
  if (app_id.empty() || app_id.find('/') != std::string::npos) {
    return {};
  }

  static constexpr const char* app_folders[] = {"", "applications/", "applications/kde/",
                                                "applications/org.kde."};
  static constexpr const char* suffixes[] = {"", ".desktop"};

  for (auto const& prefix : prefixes) {
    for (auto const& folder : app_folders) {
      for (auto const& suffix : suffixes) {
        if (cancellable && g_cancellable_is_cancelled(cancellable)) {
          return {};
        }
        auto app_info_ =
            Gio::DesktopAppInfo::create_from_filename(prefix + folder + app_id + suffix);
        if (!app_info_) {
          continue;
        }

        return app_info_;
      }
    }
  }

  return {};
}

Glib::RefPtr<Gio::DesktopAppInfo> IconLoader::get_desktop_app_info(
    const std::string& app_id, const std::vector<std::string>& prefixes, guint& search_words_left,
    GCancellable* cancellable) {
  if (app_id.empty() || (cancellable && g_cancellable_is_cancelled(cancellable))) {
    return {};
  }
  auto app_info = get_app_info_by_name(app_id, prefixes, cancellable);
  if (app_info) {
    return app_info;
  }

  // GLib's desktop app search requires valid UTF-8.
  if ((cancellable && g_cancellable_is_cancelled(cancellable)) ||
      !g_utf8_validate(app_id.c_str(), app_id.size(), nullptr)) {
    return {};
  }

  // The search walks the index of installed applications once per word of app_id.
  gchar** search_words = g_str_tokenize_and_fold(app_id.c_str(), nullptr, nullptr);
  const guint n_search_words = g_strv_length(search_words);
  g_strfreev(search_words);
  if (n_search_words == 0 || n_search_words > search_words_left) {
    return {};
  }
  search_words_left -= n_search_words;

  std::string desktop_file = "";

  gchar*** desktop_list = g_desktop_app_info_search(app_id.c_str());
  if (desktop_list != nullptr && desktop_list[0] != nullptr) {
    for (size_t i = 0; desktop_list[0][i]; i++) {
      if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        break;
      }
      if (desktop_file == "") {
        desktop_file = desktop_list[0][i];
      } else {
        auto tmp_info = Gio::DesktopAppInfo::create(desktop_list[0][i]);
        if (!tmp_info)
          // see https://github.com/Alexays/Waybar/issues/1446
          continue;

        auto startup_class = tmp_info->get_startup_wm_class();
        if (startup_class == app_id) {
          desktop_file = desktop_list[0][i];
          break;
        }
      }
    }
  }
  if (desktop_list != nullptr) {
    // One strv per match score, all owned by the caller
    for (size_t group = 0; desktop_list[group]; group++) {
      g_strfreev(desktop_list[group]);
    }
    g_free(desktop_list);
  }

  if (desktop_file.empty()) {
    return {};
  }
  return get_app_info_by_name(desktop_file, prefixes, cancellable);
}

Glib::RefPtr<Gdk::Pixbuf> IconLoader::load_icon_from_file(std::string const& icon_path, int size) {
  try {
    auto pb = Gdk::Pixbuf::create_from_file(icon_path, size, size);
    return pb;
  } catch (...) {
    return {};
  }
}

std::string IconLoader::get_icon_name_from_icon_theme(
    const Glib::RefPtr<Gtk::IconTheme>& icon_theme, const std::string& app_id) {
  if (icon_theme->lookup_icon(app_id, 24)) return app_id;

  return "";
}

bool IconLoader::image_load_icon(Gtk::Image& image, const Glib::RefPtr<Gtk::IconTheme>& icon_theme,
                                 Glib::RefPtr<Gio::DesktopAppInfo> app_info, int size) {
  std::string ret_icon_name = "unknown";
  if (app_info) {
    std::string icon_name =
        get_icon_name_from_icon_theme(icon_theme, app_info->get_startup_wm_class());
    if (!icon_name.empty()) {
      ret_icon_name = icon_name;
    } else {
      if (app_info->get_icon()) {
        ret_icon_name = app_info->get_icon()->to_string();
      }
    }
  }

  Glib::RefPtr<Gdk::Pixbuf> pixbuf;
  auto scaled_icon_size = size * image.get_scale_factor();

  try {
    pixbuf = icon_theme->load_icon(ret_icon_name, scaled_icon_size, Gtk::ICON_LOOKUP_FORCE_SIZE);
  } catch (...) {
    if (Glib::file_test(ret_icon_name, Glib::FILE_TEST_EXISTS)) {
      pixbuf = load_icon_from_file(ret_icon_name, scaled_icon_size);
    } else {
      try {
        pixbuf = DefaultGtkIconThemeWrapper::load_icon(
            "image-missing", scaled_icon_size, Gtk::IconLookupFlags::ICON_LOOKUP_FORCE_SIZE);
      } catch (...) {
        pixbuf = {};
      }
    }
  }

  if (pixbuf) {
    if (pixbuf->get_width() != scaled_icon_size && pixbuf->get_height() > 0) {
      int width = scaled_icon_size * pixbuf->get_width() / pixbuf->get_height();
      pixbuf = pixbuf->scale_simple(width, scaled_icon_size, Gdk::InterpType::INTERP_BILINEAR);
    }
    auto surface = Gdk::Cairo::create_surface_from_pixbuf(pixbuf, image.get_scale_factor(),
                                                          image.get_window());
    image.set(surface);
    return true;
  }

  return false;
}

void IconLoader::add_custom_icon_theme(const std::string& theme_name) {
  auto icon_theme = Gtk::IconTheme::create();
  icon_theme->set_custom_theme(theme_name);
  custom_icon_themes_.push_back(icon_theme);
  spdlog::debug("Use custom icon theme: {}", theme_name);
}

bool IconLoader::image_load_icon(Gtk::Image& image, Glib::RefPtr<Gio::DesktopAppInfo> app_info,
                                 int size) const {
  for (auto& icon_theme : custom_icon_themes_) {
    if (image_load_icon(image, icon_theme, app_info, size)) {
      return true;
    }
  }
  return image_load_icon(image, default_icon_theme_, app_info, size);
}

void IconLoader::load_icon_async(Glib::RefPtr<Gio::DesktopAppInfo> app_info, int size, int scale,
                                 const Glib::RefPtr<Gio::Cancellable>& cancellable,
                                 waybar::util::IconReady ready) const {
  waybar::util::IconSources sources;
  const auto append_theme = [&](const Glib::RefPtr<Gtk::IconTheme>& theme) {
    if (app_info) {
      const auto wm_class = app_info->get_startup_wm_class();
      if (!wm_class.empty()) {
        sources.push_back({.icon = Gio::ThemedIcon::create(wm_class), .theme = theme});
      }
      if (auto icon = app_info->get_icon()) {
        sources.push_back({.icon = icon, .theme = theme});
      }
    } else {
      sources.push_back({.icon = Gio::ThemedIcon::create("unknown"), .theme = theme});
    }
  };
  for (const auto& theme : custom_icon_themes_) append_theme(theme);
  append_theme({});
  sources.push_back({.icon = Gio::ThemedIcon::create("image-missing")});
  waybar::util::loadIconAsync(std::move(sources), size, scale, {}, cancellable, std::move(ready));
}

Glib::RefPtr<Gio::DesktopAppInfo> IconLoader::get_app_info_from_app_id_list(
    const std::string& app_id_list, GCancellable* cancellable) {
  std::string app_id;
  std::istringstream stream(app_id_list);
  // Keep the search paths alive while a cancelled worker unwinds during shutdown.
  const auto prefixes = search_prefix();
  // Bound the cost of desktop app searches across the whole peer-supplied list.
  guint search_words_left = 32;
  const auto lookup = [&](const std::string& id) {
    return get_desktop_app_info(id, prefixes, search_words_left, cancellable);
  };

  // Wayfire sends a space-separated list of app ids. Other compositors send a single id.
  for (int i = 0; i < 8 && stream >> app_id; i++) {
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      return {};
    }
    auto app_info = lookup(app_id);
    if (app_info) {
      return app_info;
    }

    auto lower_app_id = app_id;
    std::ranges::transform(lower_app_id, lower_app_id.begin(), [](char c) {
      return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    if (lower_app_id != app_id) {
      app_info = lookup(lower_app_id);
      if (app_info) {
        return app_info;
      }
    }

    // npos + 1 wraps to 0: no '.' keeps the whole id
    auto app_name = app_id.substr(app_id.rfind('.') + 1);
    if (app_name != app_id) {
      app_info = lookup(app_name);
      if (app_info) {
        return app_info;
      }
    }

    app_name = app_id.substr(0, app_id.find('-'));
    if (app_name != app_id) {
      app_info = lookup(app_name);
      if (app_info) {
        return app_info;
      }
    }
  }
  return {};
}
