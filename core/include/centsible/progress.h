#pragma once
#include <functional>

namespace centsible
{
// Long offline analyses (sections, tempo automation) report how far they are and let the caller stop them.
// fraction: 0..1 over the whole job; stage: a short label for a progress indicator ("listening", "fitting detune", ...).
// Return false to cancel: the analysis returns early with `cancelled` set.
using ProgressFn = std::function<bool (double fraction, const char* stage)>;
} // namespace centsible
