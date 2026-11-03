#pragma once

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace waybar::util {

// Opt-in scope timings for startup and callbacks. Log entry before doing the work so a
// blocked operation is visible even if it never returns; match entry/exit by scope ID.
class ScopedTrace {
 public:
  template <typename... Args>
  explicit ScopedTrace(fmt::format_string<Args...> format, Args&&... args) {
    auto logger = spdlog::default_logger();
    if (!logger || !logger->should_log(spdlog::level::trace)) {
      return;
    }
    logger_ = std::move(logger);
    label_ = fmt::format(format, std::forward<Args>(args)...);
    id_ = next_id_.fetch_add(1, std::memory_order_relaxed);
    exceptions_ = std::uncaught_exceptions();
    started_ = std::chrono::steady_clock::now();
    logger_->trace("[timing] begin #{} {}", id_, label_);
    logger_->flush();
  }

  ScopedTrace(const ScopedTrace&) = delete;
  ScopedTrace& operator=(const ScopedTrace&) = delete;
  ScopedTrace(ScopedTrace&&) = delete;
  ScopedTrace& operator=(ScopedTrace&&) = delete;

  ~ScopedTrace() noexcept {
    if (!logger_) {
      return;
    }
    try {
      const auto elapsed =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started_);
      logger_->trace("[timing] end #{} {} elapsed_ms={:.3f} unwinding={}", id_, label_,
                     elapsed.count(), std::uncaught_exceptions() > exceptions_);
      logger_->flush();
    } catch (...) {
      // Diagnostics must not replace an exception already unwinding the traced operation.
    }
  }

 private:
  inline static std::atomic<std::uint64_t> next_id_{1};
  std::shared_ptr<spdlog::logger> logger_;
  std::string label_;
  std::uint64_t id_ = 0;
  int exceptions_ = 0;
  std::chrono::steady_clock::time_point started_;
};

}  // namespace waybar::util
