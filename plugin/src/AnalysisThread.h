#pragma once
#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>
#include <centsible/analyzer.h>
#include <centsible/sections.h>

// Lock-free "latest value" hand-off between exactly one writer thread and one reader thread.
// The writer never blocks the reader and vice versa; the reader always sees a complete snapshot.
template <typename T>
class TripleBuffer
{
public:
    void write (const T& v)
    {
        slots[(size_t) writeIdx] = v;
        const int previous = shared.exchange (writeIdx | dirtyBit, std::memory_order_acq_rel);
        writeIdx = previous & 3;
    }

    // Returns the newest published value (or the last one read, if nothing new).
    const T& read()
    {
        if (shared.load (std::memory_order_acquire) & dirtyBit)
        {
            const int previous = shared.exchange (readIdx, std::memory_order_acq_rel);
            readIdx = previous & 3;
        }
        return slots[(size_t) readIdx];
    }

private:
    static constexpr int dirtyBit = 4;
    std::array<T, 3> slots {};
    std::atomic<int> shared { 1 };
    int writeIdx = 0, readIdx = 2;
};

// What the host says about its transport at the start of a block (read on the audio thread, handed over without locks).
struct HostInfo
{
    std::int64_t sample = 0;       // index of the block's first sample in the stream the plugin has pushed so far
    double bpm = 0.0, ppq = 0.0, timeSeconds = 0.0;
    int numerator = 4, denominator = 4;
    bool hasBpm = false, hasPpq = false, hasTime = false, playing = false;
};

// A copy of the audio the plugin heard while "record" was on, with the host's tempo as it went by.
struct RecordingSnapshot
{
    std::shared_ptr<const std::vector<float>> audio;
    double sampleRate = 0.0;
    double seconds = 0.0;
    bool full = false;                 // the memory cap was reached: later audio was not kept
    centsible::TempoMap hostTempo;     // time since the recording started -> the host's BPM
    bool tempoKnown = false;
    bool barsKnown = false;            // the host also told where the bars are (its position in quarter notes)
    double firstBarSeconds = 0.0;      // where bar 1 beat 1 of the host's timeline is on the recording's clock
    int beatsPerBar = 4;
    double quartersPerBeat = 1.0;
    int jumps = 0;                     // times the host's position jumped (seek, loop) while recording: its bars no longer fit the recording
    double droppedSeconds = 0.0;       // audio that did not reach the recording while it was on (the analysis thread could not keep up): later times are off
};

// What the recording holds so far, without copying the audio (cheap enough to poll from the editor).
struct RecordingInfo
{
    double seconds = 0.0;
    bool full = false;
    int jumps = 0;
    double droppedSeconds = 0.0;       // see RecordingSnapshot
    bool tempoKnown = false, barsKnown = false;
    double bpmMin = 0.0, bpmMax = 0.0;
};

// Runs centsible::Analyzer off the audio thread. The audio thread only calls push(), which copies
// into a pre-allocated single-producer/single-consumer FIFO (no locks, no allocation). The
// analysis thread drains it and publishes centsible::Reading snapshots through a TripleBuffer.
// While "record" is on, the analysis thread (never the audio thread) also keeps a copy of the audio, and the host's tempo, for Beatswitch.
class AnalysisThread : private juce::Thread
{
public:
    // the analysis never needs more than a few hundred kB of stack (checked by the tests, which run on a 256 kB stack); 2 MB leaves ample headroom whatever the host's default is
    AnalysisThread() : juce::Thread ("Centsible analysis", (size_t) 2 * 1024 * 1024), fifo (fifoSize), storage (fifoSize), hostFifo (hostFifoSize), hostStorage (hostFifoSize) {}
    ~AnalysisThread() override { stopThread (2000); }

    // Not for the audio thread.
    void start (double sampleRate)
    {
        stopThread (2000);
        fifo.reset();
        hostFifo.reset();
        estimator.prepare (sampleRate);
        sampleRateHz = sampleRate;
        slowLevel = 0.0f;
        resetRequested = false;
        pushedSamples = 0;
        consumedSamples = 0;
        droppedSamples = 0;
        clearRecording();
        snapshots.write (estimator.reading());
        startThread();
    }

    // Audio thread: wait-free. Drops audio if the analysis thread has fallen far behind.
    void push (const float* mono, int num)
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (num, s1, n1, s2, n2);
        if (n1 + n2 < num)
        {
            // the analysis thread is far behind: this block is lost (the recording knows: see RecordingInfo::droppedSeconds)
            fifo.finishedWrite (0);
            droppedSamples.store (droppedSamples.load (std::memory_order_relaxed) + num, std::memory_order_relaxed);
            return;
        }
        std::copy (mono, mono + n1, storage.begin() + s1);
        std::copy (mono + n1, mono + n1 + n2, storage.begin() + s2);
        fifo.finishedWrite (n1 + n2);
        pushedSamples.store (pushedSamples.load (std::memory_order_relaxed) + num, std::memory_order_release);
    }

    // Audio thread: the host's transport at the start of the block that is about to be pushed. Wait-free; dropped if the analysis thread is far behind.
    bool wantsHostInfo() const { return recording.load (std::memory_order_relaxed); }
    void pushHost (HostInfo h)
    {
        h.sample = pushedSamples.load (std::memory_order_relaxed);
        int s1, n1, s2, n2;
        hostFifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 + n2 < 1) { hostFifo.finishedWrite (0); return; }
        hostStorage[(size_t) (n1 > 0 ? s1 : s2)] = h;
        hostFifo.finishedWrite (1);
    }

    void requestReset() { resetRequested = true; }

    // Message thread only (single reader).
    const centsible::Reading& getReading() { return snapshots.read(); }

    // ---- recording (message thread) -----------------------------------------------------------------------------------------------------------------
    // About 128 MB of mono float at the host's rate: 11 minutes at 48 kHz. Later audio is not kept (the recording says `full`).
    static constexpr std::int64_t maxRecordSamples = (std::int64_t) 32 * 1024 * 1024;

    void setRecording (bool on)
    {
        if (on == recording.load()) return;
        const juce::ScopedLock sl (recLock);
        if (on)
        {
            segments.push_back ({ consumedSamples.load(), recordedLength });
            droppedMark = droppedSamples.load();
            recording = true;
        }
        else
        {
            recording = false;
            if (droppedMark >= 0) droppedWhileRecording += droppedSamples.load() - droppedMark;
            droppedMark = -1;
        }
    }
    bool isRecording() const { return recording.load(); }

    void clearRecording()
    {
        const juce::ScopedLock sl (recLock);
        blocks.clear();
        segments.clear();
        recordedLength = 0;
        tempoMap.clear();
        lastBpm = 0.0;
        anchorPpq = 0.0; anchorTime = 0.0; haveAnchor = false; hostJumps = 0; syncPpq = 0.0; lastJumpTime = -10.0;
        hostNumerator = 4; hostDenominator = 4;
        full = false;
        droppedWhileRecording = 0;
        droppedMark = recording.load() ? droppedSamples.load() : -1;
        if (recording.load()) segments.push_back ({ consumedSamples.load(), 0 });
    }

    // seconds of audio the recording lost because the analysis thread could not keep up (call with recLock held)
    double droppedSecondsLocked() const
    {
        const std::int64_t n = droppedWhileRecording + (droppedMark >= 0 ? droppedSamples.load() - droppedMark : 0);
        return sampleRateHz > 0.0 ? (double) n / sampleRateHz : 0.0;
    }

    double recordedSeconds() const
    {
        const juce::ScopedLock sl (recLock);
        return sampleRateHz > 0.0 ? (double) recordedLength / sampleRateHz : 0.0;
    }

    RecordingInfo recordingInfo() const
    {
        RecordingInfo r;
        const juce::ScopedLock sl (recLock);
        r.seconds = sampleRateHz > 0.0 ? (double) recordedLength / sampleRateHz : 0.0;
        r.full = full;
        r.droppedSeconds = droppedSecondsLocked();
        r.jumps = hostJumps;
        r.tempoKnown = tempoMap.valid();
        r.barsKnown = haveAnchor && tempoMap.valid();
        for (const auto& p : tempoMap.points())
        {
            r.bpmMin = r.bpmMin == 0.0 ? p.bpm : std::min (r.bpmMin, p.bpm);
            r.bpmMax = std::max (r.bpmMax, p.bpm);
        }
        return r;
    }

    RecordingSnapshot snapshotRecording() const
    {
        RecordingSnapshot r;
        const juce::ScopedLock sl (recLock);
        r.sampleRate = sampleRateHz;
        r.full = full;
        r.droppedSeconds = droppedSecondsLocked();
        r.jumps = hostJumps;
        auto audio = std::make_shared<std::vector<float>>();
        audio->reserve ((size_t) recordedLength);
        for (const auto& b : blocks) audio->insert (audio->end(), b.begin(), b.end());
        r.seconds = sampleRateHz > 0.0 ? (double) audio->size() / sampleRateHz : 0.0;
        r.audio = std::move (audio);
        r.hostTempo = tempoMap;
        r.tempoKnown = tempoMap.valid();
        r.beatsPerBar = std::max (1, hostNumerator);
        r.quartersPerBeat = hostDenominator > 0 ? 4.0 / (double) hostDenominator : 1.0;
        if (haveAnchor && tempoMap.valid())
        {
            r.barsKnown = true;
            // bar 1 beat 1 is where the host's position (in quarter notes) is 0
            r.firstBarSeconds = tempoMap.timeOfBeat (tempoMap.beatsAt (anchorTime) - anchorPpq);
        }
        return r;
    }

private:
    static constexpr int fifoSize = 1 << 18; // ~5.5 s at 48 kHz
    static constexpr int hostFifoSize = 1024;
    static constexpr std::size_t blockSamples = (std::size_t) 1 << 20;

    struct Segment { std::int64_t stream, recorded; };

    // analysis thread
    void record (const float* x, int n, std::int64_t streamStart)
    {
        (void) streamStart;
        const juce::ScopedLock sl (recLock);
        for (int i = 0; i < n && recordedLength < maxRecordSamples;)
        {
            if (blocks.empty() || blocks.back().size() >= blockSamples)
            {
                blocks.emplace_back();
                blocks.back().reserve (blockSamples);
            }
            auto& b = blocks.back();
            const int take = (int) std::min<std::int64_t> ({ (std::int64_t) (n - i), (std::int64_t) (blockSamples - b.size()), maxRecordSamples - recordedLength });
            b.insert (b.end(), x + i, x + i + take);
            recordedLength += take;
            i += take;
        }
        if (recordedLength >= maxRecordSamples) full = true;
    }

    // analysis thread: where in the recording (seconds) a sample of the stream landed, or a negative number if it was not recorded
    double recordedTimeOf (std::int64_t streamSample) const
    {
        for (std::size_t i = segments.size(); i-- > 0;)
            if (streamSample >= segments[i].stream)
            {
                const std::int64_t rec = segments[i].recorded + (streamSample - segments[i].stream);
                return rec <= recordedLength && sampleRateHz > 0.0 ? (double) rec / sampleRateHz : -1.0;
            }
        return -1.0;
    }

    void takeHost()
    {
        while (hostFifo.getNumReady() > 0)
        {
            int s1, n1, s2, n2;
            hostFifo.prepareToRead (1, s1, n1, s2, n2);
            const HostInfo h = hostStorage[(size_t) (n1 > 0 ? s1 : s2)];
            hostFifo.finishedRead (1);
            if (! recording.load()) continue;
            const juce::ScopedLock sl (recLock);
            const double t = recordedTimeOf (h.sample);
            if (t < 0.0) continue;
            hostNumerator = std::max (1, h.numerator); hostDenominator = std::max (1, h.denominator);
            if (h.hasBpm && h.bpm > 20.0 && h.bpm < 400.0 && (lastBpm <= 0.0 || std::fabs (h.bpm - lastBpm) > 0.02))
            {
                tempoMap.add (t, h.bpm);
                lastBpm = h.bpm;
            }
            if (! haveAnchor && h.hasPpq && tempoMap.valid()) { anchorPpq = h.ppq; anchorTime = t; haveAnchor = true; }
            else if (haveAnchor && h.hasPpq && tempoMap.valid())
            {
                // the host's position must advance with the recording: a jump of more than a beat is a seek or a loop
                const double expected = tempoMap.beatsAt (t) - tempoMap.beatsAt (anchorTime);
                if (std::fabs ((h.ppq - anchorPpq - syncPpq) - expected) > 1.5 && t - lastJumpTime > 1.0) { ++hostJumps; lastJumpTime = t; syncPpq = h.ppq - expected - anchorPpq; }
            }
        }
    }

    void run() override
    {
        std::vector<float> chunk (4096);
        while (! threadShouldExit())
        {
            if (resetRequested.exchange (false))
            {
                estimator.reset();
                snapshots.write (estimator.reading());
            }

            takeHost();
            const int ready = fifo.getNumReady();
            if (ready == 0) { wait (15); continue; }

            const int num_ = std::min (ready, (int) chunk.size());
            int s1, n1, s2, n2;
            fifo.prepareToRead (num_, s1, n1, s2, n2);
            std::copy (storage.begin() + s1, storage.begin() + s1 + n1, chunk.begin());
            std::copy (storage.begin() + s2, storage.begin() + s2 + n2, chunk.begin() + n1);
            fifo.finishedRead (n1 + n2);

            const std::int64_t streamStart = consumedSamples.load (std::memory_order_relaxed);
            if (recording.load (std::memory_order_relaxed)) record (chunk.data(), n1 + n2, streamStart);
            consumedSamples.store (streamStart + n1 + n2, std::memory_order_release);
            estimator.push (chunk.data(), n1 + n2);

            // cheap level metering and onset detection for the visuals (runs here, never on the audio thread)
            double sumSq = 0.0;
            for (int i = 0; i < n1 + n2; ++i) sumSq += (double) chunk[(size_t) i] * chunk[(size_t) i];
            const float rawLevel = (float) std::sqrt (sumSq / std::max (1, n1 + n2));
            const float level = std::isfinite (rawLevel) ? rawLevel : 0.0f;   // NaN / Inf from upstream must not reach the visuals
            const float dtChunk = (float) (n1 + n2) / (float) sampleRateHz;
            slowLevel += (level - slowLevel) * (1.0f - std::exp (-dtChunk / 0.25f));
            sinceOnset += dtChunk;
            if (level > 0.01f && level > 1.7f * slowLevel && sinceOnset > 0.12f) { ++onsets; sinceOnset = 0.0f; }
            auto snapshot = estimator.reading();
            snapshot.rms = level;
            snapshot.onsetCount = onsets;
            snapshots.write (snapshot);
        }
    }

    juce::AbstractFifo fifo;
    std::vector<float> storage;
    juce::AbstractFifo hostFifo;
    std::vector<HostInfo> hostStorage;
    centsible::Analyzer estimator;
    TripleBuffer<centsible::Reading> snapshots;
    std::atomic<bool> resetRequested { false };
    double sampleRateHz = 48000.0;
    float slowLevel = 0.0f, sinceOnset = 1.0f;
    unsigned onsets = 0;

    std::atomic<std::int64_t> pushedSamples { 0 };      // written by the audio thread only
    std::atomic<std::int64_t> consumedSamples { 0 };    // written by the analysis thread only
    std::atomic<std::int64_t> droppedSamples { 0 };     // samples the audio thread had to drop (FIFO full); written by the audio thread only
    std::int64_t droppedWhileRecording = 0, droppedMark = -1;   // (recLock)
    std::atomic<bool> recording { false };
    mutable juce::CriticalSection recLock;             // never taken on the audio thread
    std::vector<std::vector<float>> blocks;
    std::vector<Segment> segments;
    std::int64_t recordedLength = 0;
    centsible::TempoMap tempoMap;
    double lastBpm = 0.0, anchorPpq = 0.0, anchorTime = 0.0;
    bool haveAnchor = false, full = false;
    int hostJumps = 0;
    double syncPpq = 0.0;              // the host's position after a seek, relative to where it should be (so one seek is counted once)
    double lastJumpTime = -10.0;
    int hostNumerator = 4, hostDenominator = 4;
};
