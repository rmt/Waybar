#include "util/async_icon.hpp"

#include <giomm/file.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <utility>

#include "util/async.hpp"
#include "util/gtk_icon.hpp"
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
      if (source.worker) {
        auto worker = source.worker;
        auto self = shared_from_this();
        runAsync<IconWorkerResult>(
            cancellable_,
            [worker = std::move(worker), size = size_, scale = scale_](GCancellable* cancellable) {
              return worker(size, scale, cancellable);
            },
            [self](std::optional<IconWorkerResult> result, std::string) {
              if (self->cancellable_->is_cancelled()) return;
              if (result && result->pixbuf) {
                if (result->symbolic) self->tint_symbolic(result->pixbuf);
                self->complete(std::move(result->pixbuf));
              } else {
                self->next();
              }
            });
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
      auto info = source.theme ? source.theme->lookup_icon(source.icon, size_, scale_,
                                                           Gtk::ICON_LOOKUP_FORCE_SIZE)
                               : DefaultGtkIconThemeWrapper::lookup_icon(
                                     source.icon, size_, scale_, Gtk::ICON_LOOKUP_FORCE_SIZE);
      if (!info) return false;
      auto owned_info = std::make_shared<Gtk::IconInfo>(std::move(info));
      const bool symbolic = source.symbolic && style_;
      auto done = [self = shared_from_this(), owned_info,
                   symbolic](Glib::RefPtr<Gio::AsyncResult>& result) {
        self->loaded(owned_info, symbolic, result);
      };
      if (symbolic) {
        owned_info->load_symbolic_for_context_async(style_, done, cancellable_);
      } else {
        owned_info->load_icon_async(done, cancellable_);
      }
      return true;
    } catch (const Glib::Error&) {
      // Try the next source when lookup or scheduling fails.
    }
    return false;
  }

  void tint_symbolic(const Glib::RefPtr<Gdk::Pixbuf>& pixbuf) {
    if (!style_ || !pixbuf || !pixbuf->get_has_alpha() || pixbuf->get_n_channels() != 4) return;
    const auto color = style_->get_color(style_->get_state());
    const auto red = static_cast<guint8>(color.get_red() * 255);
    const auto green = static_cast<guint8>(color.get_green() * 255);
    const auto blue = static_cast<guint8>(color.get_blue() * 255);
    const auto alpha = color.get_alpha();
    for (int y = 0; y < pixbuf->get_height(); ++y) {
      guint8* pixel = pixbuf->get_pixels() + y * pixbuf->get_rowstride();
      for (int x = 0; x < pixbuf->get_width(); ++x, pixel += 4) {
        pixel[0] = red;
        pixel[1] = green;
        pixel[2] = blue;
        pixel[3] = static_cast<guint8>(pixel[3] * alpha);
      }
    }
  }

  void loaded(const std::shared_ptr<Gtk::IconInfo>& info, bool symbolic,
              const Glib::RefPtr<Gio::AsyncResult>& result) {
    Glib::RefPtr<Gdk::Pixbuf> pixbuf;
    // Always finish GTK's operation, including cancellation, to release its result resources.
    try {
      if (symbolic) {
        bool was_symbolic = false;
        pixbuf = info->load_symbolic_for_context_finish(result, was_symbolic);
      } else {
        pixbuf = info->load_icon_finish(result);
      }
    } catch (const Glib::Error&) {
      // Missing/corrupt sources fall through to the next candidate.
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
