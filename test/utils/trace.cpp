#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include <spdlog/sinks/ostream_sink.h>

#include <sstream>
#include <stdexcept>
#include <type_traits>

#include "util/trace.hpp"

namespace {

struct TraceLog {
  std::ostringstream output;
  std::shared_ptr<spdlog::logger> previous = spdlog::default_logger();

  explicit TraceLog(spdlog::level::level_enum level) {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_st>(output);
    auto logger = std::make_shared<spdlog::logger>("trace-test", sink);
    logger->set_pattern("%v");
    logger->set_level(level);
    spdlog::set_default_logger(logger);
  }

  ~TraceLog() { spdlog::set_default_logger(previous); }
};

}  // namespace

TEST_CASE("scope tracing is silent at normal log levels", "[util][trace]") {
  TraceLog log(spdlog::level::info);
  {
    waybar::util::ScopedTrace trace("disabled {}", 42);
  }
  REQUIRE(log.output.str().empty());
}

TEST_CASE("scope tracing matches nested entry and exit with elapsed time", "[util][trace]") {
  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<waybar::util::ScopedTrace>);
  STATIC_REQUIRE_FALSE(std::is_move_constructible_v<waybar::util::ScopedTrace>);
  TraceLog log(spdlog::level::trace);
  {
    waybar::util::ScopedTrace outer("outer {}", 42);
    // The begin message is available before the operation completes.
    REQUIRE(log.output.str().find("outer 42") != std::string::npos);
    waybar::util::ScopedTrace inner("inner");
  }

  std::istringstream lines(log.output.str());
  std::string outer_begin, inner_begin, inner_end, outer_end;
  REQUIRE(static_cast<bool>(std::getline(lines, outer_begin)));
  REQUIRE(static_cast<bool>(std::getline(lines, inner_begin)));
  REQUIRE(static_cast<bool>(std::getline(lines, inner_end)));
  REQUIRE(static_cast<bool>(std::getline(lines, outer_end)));
  REQUIRE(outer_begin.find("[timing] begin #") == 0);
  REQUIRE(inner_begin.find("[timing] begin #") == 0);
  REQUIRE(inner_end.find("[timing] end #") == 0);
  REQUIRE(outer_end.find("[timing] end #") == 0);
  REQUIRE(outer_begin.substr(outer_begin.find('#')) != inner_begin.substr(inner_begin.find('#')));
  REQUIRE(inner_end.find(inner_begin.substr(inner_begin.find('#'))) != std::string::npos);
  REQUIRE(outer_end.find(outer_begin.substr(outer_begin.find('#'))) != std::string::npos);
  REQUIRE(outer_end.find("elapsed_ms=") != std::string::npos);
  REQUIRE(outer_end.find("unwinding=false") != std::string::npos);
}

TEST_CASE("scope tracing records exception unwinding", "[util][trace]") {
  TraceLog log(spdlog::level::trace);
  try {
    waybar::util::ScopedTrace trace("throwing operation");
    throw std::runtime_error("test");
  } catch (const std::runtime_error&) {
  }
  REQUIRE(log.output.str().find("[timing] end #") != std::string::npos);
  REQUIRE(log.output.str().find("unwinding=true") != std::string::npos);
}
