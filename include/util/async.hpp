#pragma once

#include <gio/gio.h>
#include <giomm/cancellable.h>
#include <glibmm/error.h>
#include <spdlog/spdlog.h>

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace waybar::util {

// Call from the UI thread. Work must own its inputs and never access widgets/the receiver.
// Completion runs in the caller's thread-default main context, and is skipped on cancellation.
// Cancellation releases the receiver promptly even if an uninterruptible lookup is still running.
// The worker's owned inputs/result remain alive until that lookup actually returns.
template <typename Result>
void runAsync(const Glib::RefPtr<Gio::Cancellable>& cancellable,
              std::function<Result(GCancellable*)> work,
              std::function<void(std::optional<Result>, std::string)> completion) {
  using Work = std::function<Result(GCancellable*)>;
  using Completion = std::function<void(std::optional<Result>, std::string)>;
  auto* task = g_task_new(
      nullptr, cancellable ? cancellable->gobj() : nullptr,
      [](GObject*, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Completion> done(static_cast<Completion*>(data));
        GError* error = nullptr;
        std::unique_ptr<Result> value(
            static_cast<Result*>(g_task_propagate_pointer(G_TASK(result), &error)));
        const bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        std::string message = error ? error->message : "";
        g_clear_error(&error);
        if (cancelled) {
          return;
        }
        try {
          (*done)(value ? std::optional<Result>(std::move(*value)) : std::nullopt,
                  std::move(message));
        } catch (const Glib::Error& e) {
          spdlog::warn("Background task completion failed: {}", std::string(e.what()));
        } catch (const std::exception& e) {
          spdlog::warn("Background task completion failed: {}", e.what());
        } catch (...) {
          spdlog::warn("Background task completion failed with an unknown exception");
        }
      },
      new Completion(std::move(completion)));
  g_task_set_task_data(task, new Work(std::move(work)),
                       [](gpointer data) { delete static_cast<Work*>(data); });
  g_task_set_return_on_cancel(task, TRUE);
  g_task_run_in_thread(task, [](GTask* task, gpointer, gpointer data, GCancellable* cancellable) {
    if (g_task_return_error_if_cancelled(task)) {
      return;
    }
    try {
      auto value = std::make_unique<Result>((*static_cast<Work*>(data))(cancellable));
      g_task_return_pointer(task, value.release(),
                            [](gpointer result) { delete static_cast<Result*>(result); });
    } catch (const Glib::Error& e) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", e.what().c_str());
    } catch (const std::exception& e) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", e.what());
    } catch (...) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "Unknown background task failure");
    }
  });
  g_object_unref(task);
}

}  // namespace waybar::util
