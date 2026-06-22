#pragma once

#include <algorithm>
#include <vector>

namespace waybar {

struct waybar_output;

// Compare output pointer values only.  During deferred monitor removal a Bar may still hold
// a waybar_output* whose pointee has already been removed, so callers must not dereference it
// just to decide whether the Bar belongs to an output being removed.
inline bool isOutputPendingRemoval(
    const waybar_output* output, const std::vector<waybar_output*>& outputs_pending_removal) {
  return std::find(outputs_pending_removal.begin(), outputs_pending_removal.end(), output) !=
         outputs_pending_removal.end();
}

}  // namespace waybar
