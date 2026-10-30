#pragma once
#include <memory>
#include <string>

#include "vc/AsyncMaskProvider.h"

namespace vc {

// Loads a CoreML model (.mlmodelc, or .mlpackage which is compiled on load) and
// runs it on the CPU + Neural Engine from a worker thread.
//
// Model contract (all float32, nBins = 129):
//   input  "features"  [1, nBins]   power in dBFS per bin (see IMaskProvider)
//   input  "state_in"  [1, S]       optional recurrent state
//   output "mask"      [1, nBins]   gains in 0..1
//   output "state_out" [1, S]       optional, fed back as state_in
//
// Returns nullptr and sets `error` on failure, and always on non-Apple
// platforms. Loading can take hundreds of ms: call it off the audio thread.
std::unique_ptr<AsyncMaskProvider> makeCoreMlMaskProvider(const std::string& modelPath, int nBins, std::string& error);

} // namespace vc
