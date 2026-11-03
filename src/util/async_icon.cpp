#include "util/async_icon.hpp"

#include <giomm/file.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>

#include "util/gtk_icon.hpp"
#include "util/trace.hpp"

namespace waybar::util {
namespace {

class IconRequest : public std::enable_shared_from_this<IconRequest> {
 public:
  IconRequest(IconSources sources, int size, int scale, Glib::RefPtr<Gtk::StyleContext> style,
              Glib::RefPtr<Gio::Cancellable> cancellable, IconReady ready)
      : sources_(std::move(sources)),
        size_(std::max(1, size)),
        scale_(std::max(1, scale)),
        style_(std::move(style)),
        cancellable_(std::move(cancellable)),
        ready_(std::move(ready)) {}

  void next() {
    while (!cancellable_->is_cancelled() && index_ < sources_.size()) {
      const auto source_index = index_++;
      auto& source = sources_[source_index];
      if (source.pixbuf) {
        complete(source.pixbuf);
        return;
      }
      if (!source.icon) continue;
      if (G_IS_FILE_ICON(source.icon->gobj())) {
        // SNI names may also be relative paths. Check them asynchronously so a normal
        // theme name does not provoke GTK's missing/corrupt-pixbuf warning for a missing file.
        struct PendingFile {
          std::shared_ptr<IconRequest> request;
          std::size_t index;
        };
        auto* pending = new PendingFile{shared_from_this(), source_index};
        g_file_query_info_async(
            g_file_icon_get_file(G_FILE_ICON(source.icon->gobj())), G_FILE_ATTRIBUTE_STANDARD_TYPE,
            G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, cancellable_->gobj(),
            [](GObject* object, GAsyncResult* result, gpointer data) {
              std::unique_ptr<PendingFile> pending(static_cast<PendingFile*>(data));
              GError* error = nullptr;
              auto* info = g_file_query_info_finish(G_FILE(object), result, &error);
              const bool exists = info && g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR;
              g_clear_object(&info);
              g_clear_error(&error);
              if (pending->request->cancellable_->is_cancelled()) return;
              if (!exists || !pending->request->start_decode(pending->index)) {
                pending->request->next();
              }
            },
            pending);
        return;
      }
      if (start_decode(source_index)) return;
    }
    complete({});
  }

 private:
  bool start_decode(std::size_t source_index) {
    auto& source = sources_[source_index];
    try {
      Gtk::IconInfo info;
      {
        ScopedTrace trace("Icon theme lookup icon={} size={} scale={}",
                          std::string(source.icon->to_string()), size_, scale_);
        info = source.theme ? source.theme->lookup_icon(source.icon, size_, scale_,
                                                        Gtk::ICON_LOOKUP_FORCE_SIZE)
                            : DefaultGtkIconThemeWrapper::lookup_icon(source.icon, size_, scale_,
                                                                      Gtk::ICON_LOOKUP_FORCE_SIZE);
      }
      if (!info) return false;
      auto owned_info = std::make_shared<Gtk::IconInfo>(std::move(info));
      const bool symbolic = source.symbolic && style_;
      auto done = [self = shared_from_this(), owned_info,
                   symbolic](Glib::RefPtr<Gio::AsyncResult>& result) {
        self->loaded(owned_info, symbolic, result);
      };
      decode_started_ = std::chrono::steady_clock::now();
      decode_label_ = source.icon->to_string();
      ScopedTrace trace("Icon async decode scheduled size={} scale={}", size_, scale_);
      if (symbolic) {
        owned_info->load_symbolic_for_context_async(style_, done, cancellable_);
      } else {
        owned_info->load_icon_async(done, cancellable_);
      }
      return true;
    } catch (const Glib::Error& e) {
      spdlog::trace("Icon lookup failed: {}", std::string(e.what()));
    }
    return false;
  }

  void loaded(const std::shared_ptr<Gtk::IconInfo>& info, bool symbolic,
              const Glib::RefPtr<Gio::AsyncResult>& result) {
    ScopedTrace trace("Icon async decode finished size={} scale={}", size_, scale_);
    const auto elapsed = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - decode_started_)
                             .count();
    spdlog::trace("[timing] Icon async decode latency_ms={:.3f} icon={} cancelled={}", elapsed,
                  decode_label_, cancellable_->is_cancelled());
    Glib::RefPtr<Gdk::Pixbuf> pixbuf;
    // Always finish GTK's operation, including cancellation, to release its result resources.
    try {
      if (symbolic) {
        bool was_symbolic = false;
        pixbuf = info->load_symbolic_for_context_finish(result, was_symbolic);
      } else {
        pixbuf = info->load_icon_finish(result);
      }
    } catch (const Glib::Error& e) {
      if (!cancellable_->is_cancelled()) {
        spdlog::trace("Icon decode failed: {}", std::string(e.what()));
      }
    }
    if (cancellable_->is_cancelled()) return;
    if (pixbuf) {
      complete(pixbuf);
    } else {
      next();
    }
  }

  void complete(Glib::RefPtr<Gdk::Pixbuf> pixbuf) {
    if (cancellable_->is_cancelled()) return;
    auto ready = std::move(ready_);
    try {
      ready(std::move(pixbuf));
    } catch (const Glib::Error& e) {
      spdlog::warn("Icon completion failed: {}", std::string(e.what()));
    } catch (const std::exception& e) {
      spdlog::warn("Icon completion failed: {}", e.what());
    }
  }

  IconSources sources_;
  std::size_t index_ = 0;
  int size_, scale_;
  Glib::RefPtr<Gtk::StyleContext> style_;
  Glib::RefPtr<Gio::Cancellable> cancellable_;
  IconReady ready_;
  std::chrono::steady_clock::time_point decode_started_;
  std::string decode_label_;
};

}  // namespace

void loadIconAsync(IconSources sources, int size, int scale,
                   const Glib::RefPtr<Gtk::StyleContext>& style,
                   const Glib::RefPtr<Gio::Cancellable>& cancellable, IconReady ready) {
  auto token = cancellable ? cancellable : Gio::Cancellable::create();
  std::make_shared<IconRequest>(std::move(sources), size, scale, style, token, std::move(ready))
      ->next();
}

IconSource fileIconSource(const std::string& path) {
  return {.icon = Gio::Icon::create(Gio::File::create_for_path(path)->get_path())};
}

}  // namespace waybar::util
