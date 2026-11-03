#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include <giomm/error.h>
#include <giomm/init.h>
#include <glibmm/main.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "util/async.hpp"
#include "util/scope_guard.hpp"

namespace {

struct AsyncContext {
  Glib::RefPtr<Glib::MainContext> context;
  Glib::RefPtr<Gio::Cancellable> cancellable;

  AsyncContext() {
    Gio::init();
    context = Glib::MainContext::create();
    context->push_thread_default();
    cancellable = Gio::Cancellable::create();
  }

  ~AsyncContext() {
    cancellable->cancel();
    while (context->pending()) context->iteration(false);
    context->pop_thread_default();
  }

  template <typename Predicate>
  bool until(Predicate done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      context->iteration(false);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
  }
};

struct WorkerGate {
  std::mutex mutex;
  std::condition_variable condition;
  bool open = false;
  std::atomic<bool> entered = false;
  std::atomic<bool> finished = false;
  std::thread::id worker_thread;

  int run(int result) {
    worker_thread = std::this_thread::get_id();
    entered = true;
    std::unique_lock lock(mutex);
    condition.wait(lock, [this] { return open; });
    finished = true;
    return result;
  }

  void release() {
    std::lock_guard lock(mutex);
    open = true;
    condition.notify_all();
  }
};

}  // namespace

TEST_CASE("background work leaves main context responsive and completes on its owner thread",
          "[util][async]") {
  AsyncContext loop;
  auto gate = std::make_shared<WorkerGate>();
  waybar::util::ScopeGuard release([gate] { gate->release(); });
  const auto main_thread = std::this_thread::get_id();
  std::thread::id completion_thread;
  bool completed = false, idle_ran = false;
  std::optional<int> result;
  std::string error;
  waybar::util::runAsync<int>(
      loop.cancellable, [gate](GCancellable*) { return gate->run(42); },
      [&](std::optional<int> value, std::string message) {
        completion_thread = std::this_thread::get_id();
        result = value;
        error = std::move(message);
        completed = true;
      });
  auto idle = Glib::IdleSource::create();
  idle->connect([&] {
    idle_ran = true;
    return false;
  });
  idle->attach(loop.context);
  REQUIRE(loop.until([&] { return gate->entered && idle_ran; }));
  REQUIRE_FALSE(completed);
  REQUIRE(gate->worker_thread != main_thread);
  gate->release();
  REQUIRE(loop.until([&] { return completed; }));
  REQUIRE(result == 42);
  REQUIRE(error.empty());
  REQUIRE(completion_thread == main_thread);
}

TEST_CASE("cancelling releases completion captures before uninterruptible work finishes",
          "[util][async]") {
  AsyncContext loop;
  auto gate = std::make_shared<WorkerGate>();
  waybar::util::ScopeGuard release([gate] { gate->release(); });
  auto receiver = std::make_shared<int>(0);
  std::weak_ptr<int> lifetime = receiver;
  auto callbacks = std::make_shared<std::atomic<int>>(0);
  waybar::util::runAsync<int>(
      loop.cancellable, [gate](GCancellable*) { return gate->run(1); },
      [receiver = std::move(receiver), callbacks](std::optional<int>, std::string) {
        ++*callbacks;
      });
  REQUIRE(loop.until([&] { return gate->entered.load(); }));
  loop.cancellable->cancel();
  REQUIRE(loop.until([&] { return lifetime.expired(); }));
  REQUIRE_FALSE(gate->finished);
  REQUIRE(callbacks->load() == 0);
  gate->release();
  REQUIRE(loop.until([&] { return gate->finished.load(); }));
  REQUIRE(callbacks->load() == 0);
}

TEST_CASE("background exceptions are delivered as errors on the main context", "[util][async]") {
  AsyncContext loop;
  bool completed = false;
  std::optional<int> result;
  std::string error;
  const auto main_thread = std::this_thread::get_id();
  std::thread::id completion_thread;
  bool gio_error = false;
  SECTION("standard exception") {}
  SECTION("GIO exception") { gio_error = true; }
  waybar::util::runAsync<int>(
      loop.cancellable,
      [gio_error](GCancellable*) -> int {
        if (gio_error) {
          throw Gio::Error(Gio::Error::FAILED, "lookup failed");
        }
        throw std::runtime_error("lookup failed");
      },
      [&](std::optional<int> value, std::string message) {
        result = value;
        error = std::move(message);
        completion_thread = std::this_thread::get_id();
        completed = true;
      });
  REQUIRE(loop.until([&] { return completed; }));
  REQUIRE_FALSE(result);
  REQUIRE(error == "lookup failed");
  REQUIRE(completion_thread == main_thread);
}

TEST_CASE("cancelled stale work cannot replace a newer result", "[util][async]") {
  AsyncContext loop;
  auto gate = std::make_shared<WorkerGate>();
  waybar::util::ScopeGuard release([gate] { gate->release(); });
  std::vector<int> applied;
  waybar::util::runAsync<int>(
      loop.cancellable, [gate](GCancellable*) { return gate->run(1); },
      [&](std::optional<int> value, std::string) { applied.push_back(value.value()); });
  REQUIRE(loop.until([&] { return gate->entered.load(); }));
  loop.cancellable->cancel();
  auto current = Gio::Cancellable::create();
  waybar::util::ScopeGuard cancel([current] { current->cancel(); });
  waybar::util::runAsync<int>(
      current, [](GCancellable*) { return 2; },
      [&](std::optional<int> value, std::string) { applied.push_back(value.value()); });
  REQUIRE(loop.until([&] { return !applied.empty(); }));
  REQUIRE(applied == std::vector<int>{2});
  gate->release();
  REQUIRE(loop.until([&] { return gate->finished.load(); }));
  // Drain any late result. Cancellation must keep the obsolete completion suppressed.
  while (loop.context->pending()) loop.context->iteration(false);
  REQUIRE(applied == std::vector<int>{2});
}

TEST_CASE("already cancelled background requests never run application work", "[util][async]") {
  AsyncContext loop;
  loop.cancellable->cancel();
  auto calls = std::make_shared<std::atomic<int>>(0);
  auto work_lifetime = std::make_shared<int>(0);
  auto completion_lifetime = std::make_shared<int>(0);
  std::weak_ptr<int> work_done = work_lifetime, completion_done = completion_lifetime;
  waybar::util::runAsync<int>(
      loop.cancellable,
      [calls, owner = std::move(work_lifetime)](GCancellable*) {
        ++*calls;
        return 1;
      },
      [calls, owner = std::move(completion_lifetime)](std::optional<int>, std::string) {
        ++*calls;
      });
  REQUIRE(loop.until([&] { return work_done.expired() && completion_done.expired(); }));
  REQUIRE(calls->load() == 0);
}
