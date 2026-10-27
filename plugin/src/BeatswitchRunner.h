#pragma once
#include <juce_core/juce_core.h>
#include <centsible/sections.h>
#include <atomic>
#include <memory>
#include <vector>

// Runs one Beatswitch analysis (sections or tempo automation) on its own thread, with progress and cancel. The message thread starts it and polls it;
// the audio thread never sees it. The analysis is the library's (centsible/sections.h): the same code as the command line.
class BeatswitchRunner : private juce::Thread
{
public:
    enum class Mode { sections, tempo };
    enum class State { idle, running, done, cancelled };

    struct Job
    {
        Mode mode = Mode::sections;
        std::shared_ptr<const std::vector<float>> audio;
        double sampleRate = 0.0;
        std::vector<double> typedBpm;         // the tempos the user typed for the parts (0: not given): shown next to what was heard
        std::vector<double> boundaries;       // sections: the cuts; tempo: the regions the user asked for (empty: the suggested boundaries)
        centsible::TempoMap tempoMap;         // used when valid: the tempo points the user typed, or the host's tempo (tempo mode: instead of measuring)
        centsible::TempoMap barsMap;          // only for showing bar | beat next to the times (and what the times were read with): the typed or the host's tempo
        double referenceBpm = 0.0;
        centsible::TimeContext time;          // how the times were read (bar | beat); its tempoMap pointer is filled in from `tempoMap` when needed
        juce::String sourceName;
        juce::String warning;                 // something the reader of the results must know (a recording with gaps)
    };

    struct Snapshot
    {
        State state = State::idle;
        Mode mode = Mode::sections;
        double progress = 0.0;
        juce::String stage;
        double elapsed = 0.0;
        double audioSeconds = 0.0;
        std::vector<double> typedBpm;
        juce::String sourceName, warning;
        centsible::SectionsAnalysis sections;
        centsible::TempoAutomationResult automation;
        centsible::TempoMap tempo;
        centsible::TimeContext time;
        bool tempoFromHostOrUser = false;
        juce::String bars (double seconds) const        // "17|1" when a tempo is known, else ""
        {
            auto ctx = time;
            ctx.tempoMap = tempo.valid() ? &tempo : nullptr;
            return juce::String (centsible::formatBarBeat (seconds, ctx));
        }
    };

    BeatswitchRunner() : juce::Thread ("Centsible beatswitch", (size_t) 4 * 1024 * 1024) {}
    ~BeatswitchRunner() override { stopThread (15000); }

    // Message thread. Any run in progress is cancelled first.
    void start (Job j)
    {
        stopThread (15000);
        {
            const juce::ScopedLock sl (lock);
            job = std::move (j);
            state = {};
            state.state = State::running;
            state.mode = job.mode;
            state.sourceName = job.sourceName;
            state.warning = job.warning;
            state.typedBpm = job.typedBpm;
            state.audioSeconds = job.sampleRate > 0.0 && job.audio ? (double) job.audio->size() / job.sampleRate : 0.0;
            state.tempo = job.barsMap.valid() ? job.barsMap : job.tempoMap;
            state.time = job.time;
            state.tempoFromHostOrUser = job.tempoMap.valid();
            startedAt = juce::Time::getMillisecondCounterHiRes();
        }
        startThread();
    }

    void cancel() { signalThreadShouldExit(); }
    bool running() const { const juce::ScopedLock sl (lock); return state.state == State::running; }

    Snapshot snapshot() const
    {
        const juce::ScopedLock sl (lock);
        auto s = state;
        if (s.state == State::running) s.elapsed = (juce::Time::getMillisecondCounterHiRes() - startedAt) * 0.001;
        return s;
    }

    void clear()
    {
        stopThread (15000);
        const juce::ScopedLock sl (lock);
        state = {};
    }

private:
    void run() override
    {
        Job j;
        {
            const juce::ScopedLock sl (lock);
            j = job;
        }
        auto progress = [this] (double f, const char* stage)
        {
            const juce::ScopedLock sl (lock);
            state.progress = std::max (state.progress, std::min (1.0, f));
            state.stage = stage;
            return ! threadShouldExit();
        };
        centsible::SectionsAnalysis sec;
        centsible::TempoAutomationResult aut;
        bool cancelled = false;
        if (j.audio && ! j.audio->empty() && j.sampleRate > 0.0)
        {
            if (j.mode == Mode::sections)
            {
                sec = centsible::analyzeSections (j.audio->data(), j.audio->size(), j.sampleRate, j.boundaries, {}, progress);
                cancelled = sec.cancelled;
            }
            else
            {
                centsible::AutomationOptions o;
                if (j.tempoMap.valid()) o.tempoMap = &j.tempoMap;
                o.referenceBpm = j.referenceBpm;
                o.boundaries = j.boundaries;
                aut = centsible::analyzeTempoAutomation (j.audio->data(), j.audio->size(), j.sampleRate, o, progress);
                cancelled = aut.cancelled;
            }
        }
        const juce::ScopedLock sl (lock);
        state.sections = std::move (sec);
        state.automation = std::move (aut);
        state.progress = 1.0;
        state.elapsed = (juce::Time::getMillisecondCounterHiRes() - startedAt) * 0.001;
        state.state = cancelled || threadShouldExit() ? State::cancelled : State::done;
    }

    mutable juce::CriticalSection lock;
    Job job;
    Snapshot state;
    double startedAt = 0.0;
};
