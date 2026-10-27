#pragma once
#include <centsible/reading.h>
#include <cstddef>
#include <memory>

namespace centsible
{
// Seconds of usable audio the plugin counts up to while it is listening (the lock itself is decided by the confidence interval, not by time).
inline constexpr double kSettleSeconds = 30.0;

// The one analysis entry point. The plugin, the CLI and every test use exactly this class.
// Streaming: prepare(), then push() any number of blocks of MONO audio, read reading() whenever.
// No allocation happens in push() once prepare() has run; call it from one thread (the plugin's analysis thread, never the audio thread).
class Analyzer
{
public:
    Analyzer();
    ~Analyzer();
    Analyzer (const Analyzer&) = delete;
    Analyzer& operator= (const Analyzer&) = delete;

    void prepare (double sampleRate);
    void reset();                         // forget everything (new song)
    void hintBoundary();                  // the host stopped/started: a new song may follow
    void push (const float* mono, int numSamples);
    const Reading& reading() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Offline helper: literally a loop over Analyzer::push in 512-sample blocks.
Reading analyzeBuffer (const float* mono, std::size_t numSamples, double sampleRate);
} // namespace centsible
