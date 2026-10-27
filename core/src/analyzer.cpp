#include <centsible/analyzer.h>
#include "spectral_likelihood.h"
#include <algorithm>

namespace centsible
{
struct Analyzer::Impl
{
    detail::SpectralLikelihoodEstimator estimator;
};

Analyzer::Analyzer() : impl (std::make_unique<Impl>()) {}
Analyzer::~Analyzer() = default;

void Analyzer::prepare (double sampleRate) { impl->estimator.prepare (sampleRate); }
void Analyzer::reset() { impl->estimator.reset(); }
void Analyzer::hintBoundary() { impl->estimator.hintBoundary(); }
void Analyzer::push (const float* mono, int numSamples) { impl->estimator.push (mono, numSamples); }
const Reading& Analyzer::reading() const { return impl->estimator.reading(); }

Reading analyzeBuffer (const float* mono, std::size_t numSamples, double sampleRate)
{
    Analyzer a;
    a.prepare (sampleRate);
    for (std::size_t pos = 0; pos < numSamples; pos += 512)
        a.push (mono + pos, (int) std::min<std::size_t> (512, numSamples - pos));
    return a.reading();
}
} // namespace centsible
