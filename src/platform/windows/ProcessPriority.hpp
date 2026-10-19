#pragma once

#include <string>

namespace ifs {

/// CPU scheduling priority of the whole service process.
/// The service runs next to BeamNG on the same CPU. Kokoro synthesis keeps several cores busy for about a
/// second before each utterance; at "below_normal" Windows always lets the game's threads run first, so
/// speech gets slightly slower instead of the game dropping frames.
class ProcessPriority {
public:
    /// Applies "normal", "below_normal" or "idle". Returns false (and logs why) for an unknown value or
    /// when Windows refuses; the process then keeps its current priority.
    static bool apply(const std::string& level);
};

} // namespace ifs
