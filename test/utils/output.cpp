#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include <vector>

#include "util/output.hpp"

TEST_CASE("pending output removal lookup compares pointers without dereferencing",
          "[util][output]") {
  // These deliberately are not valid objects.  The regression this covers was caused by
  // dereferencing a stale waybar_output* while handling deferred monitor-removal callbacks.
  auto* removed = reinterpret_cast<waybar::waybar_output*>(0x1000);
  auto* stale = reinterpret_cast<waybar::waybar_output*>(0x2000);
  std::vector<waybar::waybar_output*> outputs_pending_removal{removed};

  CHECK(waybar::isOutputPendingRemoval(removed, outputs_pending_removal));
  CHECK_FALSE(waybar::isOutputPendingRemoval(stale, outputs_pending_removal));
  CHECK_FALSE(waybar::isOutputPendingRemoval(nullptr, outputs_pending_removal));
}
