// Beatswitch inside the plugin: the real processor (with the recorder and the host-tempo capture), the real editor and its Beatswitch panel,
// driven the way a user and a host would drive them, on synthetic songs whose truth is known exactly.
//   * live audio recorded through the processor, with a host that reports its tempo: bar | beat times, detune and key per section, the delta;
//   * a host tempo RAMP with a varispeed song: "use host tempo" gives a varispeed verdict and the tempo where the track sits on A440;
//   * a dropped file (written to disk, decoded by the plugin's FileAnalyzer) cut at a typed time, a typed tempo for bar | beat;
//   * bad input says what is wrong, cancel stops the analysis, and the audio passes through bit-identical while recording.
#include "PluginEditor.h"
#include "gen/warp_synth.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

namespace
{
int fails = 0, checks = 0;

void check (bool ok, const char* what, const juce::String& detail = {})
{
    ++checks;
    if (! ok) ++fails;
    std::printf ("  %s  %s%s%s\n", ok ? "ok  " : "FAIL", what, detail.isEmpty() ? "" : "  ", detail.toRawUTF8());
}

bool near (double a, double b, double tol) { return std::fabs (a - b) <= tol; }
juce::String num (double v, int d = 2) { return juce::String (v, d); }

// A host with a tempo (as a function of time), 4/4 unless told otherwise; bar 1 beat 1 is at time 0 of the audio.
class Host : public juce::AudioPlayHead
{
public:
    double bpm = 120.0, ppq = 0.0, time = 0.0;
    int numerator = 4, denominator = 4;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm);
        info.setPpqPosition (ppq);
        info.setTimeInSeconds (time);
        info.setTimeSignature (TimeSignature { numerator, denominator });
        info.setIsPlaying (true);
        return info;
    }
};

// Plays `x` through the processor in 512-sample blocks as a host would, with the host's tempo following bpmAt (time) and its position counting quarter notes.
// Returns the number of blocks whose output differs from the input (must be 0). `speed`: how much faster than real time the audio is fed.
long long play (TuningMatchProcessor& proc, Host& host, const synth::Signal& x, double sr, const std::function<double (double)>& bpmAt, double speed, double jumpAtSeconds = -1.0)
{
    long long mismatches = 0;
    juce::MidiBuffer midi;
    const int blocksPerSleep = std::max (1, (int) std::lround (25.0 * speed * 0.001 * sr / 512.0));
    double ppq = 0.0;
    for (size_t pos = 0, b = 0; pos < x.size(); pos += 512, ++b)
    {
        const int len = (int) std::min<size_t> (512, x.size() - pos);
        const double t = (double) pos / sr, dur = (double) len / sr;
        host.bpm = bpmAt (t + 0.5 * dur);
        if (jumpAtSeconds >= 0.0 && t >= jumpAtSeconds) { ppq += 16.0; jumpAtSeconds = -1.0; }      // a seek: the host's position jumps four bars
        host.ppq = ppq;
        host.time = t;
        ppq += host.bpm / 60.0 * dur;
        juce::AudioBuffer<float> buf (2, len), ref;
        for (int i = 0; i < len; ++i) { buf.setSample (0, i, x[pos + (size_t) i]); buf.setSample (1, i, x[pos + (size_t) i] * 0.9f); }
        ref.makeCopyOf (buf);
        proc.processBlock (buf, midi);
        for (int ch = 0; ch < 2; ++ch) mismatches += std::memcmp (buf.getReadPointer (ch), ref.getReadPointer (ch), (size_t) len * sizeof (float)) != 0;
        if (b % (size_t) blocksPerSleep == 0) juce::Thread::sleep (25);      // let the analysis thread keep up
    }
    juce::Thread::sleep (800);
    return mismatches;
}

void waitFor (BeatswitchPanel& panel)
{
    for (int i = 0; i < 6000 && panel.busy(); ++i) juce::Thread::sleep (50);
    panel.poll();
}

void waitForFile (TuningMatchProcessor& proc)
{
    for (int i = 0; i < 6000 && (proc.fileSnapshot().busy() || proc.fileSnapshot().count == 0); ++i) juce::Thread::sleep (50);
}

// two songs one after the other: A (A minor, +20 cents) then B (E-flat major, -15 cents), 120 BPM, 30 s each
synth::Signal twoSongs (double sr)
{
    synth::ChordSongSpec a, b;
    a.tonic = 9; a.minor = true; a.seconds = 30.0; a.seed = 11;
    b.tonic = 3; b.minor = false; b.seconds = 30.0; b.seed = 12;
    auto x = synth::renderChordSong (a, synth::constantPlan (120.0, 20.0), sr);
    const auto y = synth::renderChordSong (b, synth::constantPlan (120.0, -15.0), sr);
    x.insert (x.end(), y.begin(), y.end());
    return x;
}

void sectionsChecks (const BeatswitchRunner::Snapshot& s, double cutAt, const char* tag)
{
    check (s.state == BeatswitchRunner::State::done, tag, "state " + juce::String ((int) s.state));
    check (s.sections.sections.size() == 2, "two sections");
    if (s.sections.sections.size() != 2) return;
    const auto& A = s.sections.sections[0];
    const auto& B = s.sections.sections[1];
    check (near (A.end, cutAt, 0.05) && near (B.start, cutAt, 0.05), "the cut is where it was asked for", num (A.end, 3) + " s");
    check (A.ok && B.ok, "both sections analysed");
    check (near (A.analysis.tuning.cents, 20.0, 1.5), "A detune +20", num (A.analysis.tuning.cents) + " +- " + num (A.analysis.tuning.ciCents));
    check (near (B.analysis.tuning.cents, -15.0, 1.5), "B detune -15", num (B.analysis.tuning.cents) + " +- " + num (B.analysis.tuning.ciCents));
    check (A.analysis.key.valid && A.analysis.key.scaleRoot == 0, "A: the scale of A minor (C major's notes)", juce::String (centsible::keyLabel (A.analysis.key)));
    check (B.analysis.key.valid && B.analysis.key.scaleRoot == 3, "B: the scale of E-flat major", juce::String (centsible::keyLabel (B.analysis.key)));
    check (A.analysis.tempo.valid && near (A.analysis.tempo.bpm, 120.0, 2.0), "A tempo 120", num (A.analysis.tempo.bpm, 1));
    if (s.sections.deltas.size() == 1)
    {
        const auto& d = s.sections.deltas[0];
        check (d.tuningKnown && near (d.cents, -35.0, 2.0) && d.significant, "A -> B: -35 cents, significant", num (d.cents) + " +- " + num (d.ci));
        check (d.keysKnown && d.semitones == 3 && ! d.sameScale, "A -> B: the scale moves up 3 semitones", juce::String (d.semitones));
    }
    else
        check (false, "one delta between two sections");
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const double sr = 48000.0;
    const auto songs = twoSongs (sr);
    Host host;

    // ----------------------------------------------------------------------------------------------------------------------------------------
    std::printf ("1. live audio, host tempo 120, cut at bar 16 (= 30 s)\n");
    {
        TuningMatchProcessor proc;
        proc.setPlayConfigDetails (2, 2, sr, 512);
        proc.prepareToPlay (sr, 512);
        proc.setPlayHead (&host);
        proc.setRecording (true);
        const auto mismatches = play (proc, host, songs, sr, [] (double) { return 120.0; }, 3.0);
        proc.setRecording (false);
        check (mismatches == 0, "the audio passes through bit-identical while recording", juce::String (mismatches) + " blocks differ");
        const auto info = proc.recordingInfo();
        check (near (info.seconds, 60.0, 0.05), "everything was recorded", num (info.seconds, 3) + " s");
        check (info.tempoKnown && info.barsKnown && near (info.bpmMin, 120.0, 0.01) && near (info.bpmMax, 120.0, 0.01), "the host's tempo was captured", num (info.bpmMax, 2) + " BPM");

        TuningMatchEditor editor (proc);
        editor.setSize (440, 700);
        editor.showBeatswitch (true);
        auto& panel = editor.beatswitchPanel();
        panel.setUseLive (true);
        
        panel.setTimes ("16|1");
        panel.analyse();
        check (panel.busy() || panel.snapshot().state != BeatswitchRunner::State::idle, "the analysis started", panel.problem());
        waitFor (panel);
        const auto s = panel.snapshot();
        sectionsChecks (s, 30.0, "analysis finished");
        check (s.bars (30.0) == "16|1", "bar | beat shown next to the times", juce::String (s.bars (30.0)));
        check (s.bars (0.0) == "1|1", "bar 1 is the start", juce::String (s.bars (0.0)));
        check (s.tempoFromHostOrUser == false, "(sections mode: the tempo is only used to read bar | beat)");

        // the same cut in seconds and in m:ss gives the same sections
        panel.setTimes ("0:30");
        panel.analyse();
        waitFor (panel);
        sectionsChecks (panel.snapshot(), 30.0, "the same cut typed as 0:30");

        // more than one cut: three sections, labelled A, B, C
        panel.setTimes ("10, 30.0");
        panel.analyse();
        waitFor (panel);
        const auto s3 = panel.snapshot();
        check (s3.sections.sections.size() == 3 && s3.sections.deltas.size() == 2, "a third part gives three sections with two deltas");

        // the "mark switch" button writes the time of the recording so far into the field
        panel.setTimes ("");
        panel.markSwitch();
        check (panel.problem().isEmpty(), "mark switch accepted after a recording", panel.problem());

        // bar 200 is far past the end of the recording
        panel.setTimes ("200|1");
        panel.analyse();
        check (! panel.busy() && panel.problem().isNotEmpty(), "a time past the end of the audio is refused", panel.problem());
    }

    // ----------------------------------------------------------------------------------------------------------------------------------------
    std::printf ("2. live audio, host tempo RAMP 100 -> 120 BPM, a varispeed song (the pitch follows the tempo)\n");
    {
        const double bpmRef = 110.0, base = 10.0;
        const auto plan = synth::rampPlan (100.0, 120.0, 10.0, 50.0, true, base, bpmRef);
        synth::ChordSongSpec spec;
        spec.tonic = 7; spec.minor = false; spec.seconds = 60.0; spec.seed = 21;
        const auto x = synth::renderChordSong (spec, plan, sr);

        TuningMatchProcessor proc;
        proc.setPlayConfigDetails (2, 2, sr, 512);
        proc.prepareToPlay (sr, 512);
        proc.setPlayHead (&host);
        proc.setRecording (true);
        const auto mismatches = play (proc, host, x, sr, [&] (double t) { return plan.bpm (t); }, 3.0);
        proc.setRecording (false);
        check (mismatches == 0, "pass-through is bit-identical", juce::String (mismatches) + " blocks differ");
        const auto info = proc.recordingInfo();
        check (info.jumps == 0, "a tempo ramp is not mistaken for a seek", juce::String (info.jumps));
        check (info.tempoKnown && near (info.bpmMin, 100.0, 0.5) && near (info.bpmMax, 120.0, 0.5), "the host's tempo ramp was captured", num (info.bpmMin, 1) + " - " + num (info.bpmMax, 1));

        TuningMatchEditor editor (proc);
        editor.setSize (440, 700);
        editor.showBeatswitch (true);
        auto& panel = editor.beatswitchPanel();
        panel.setUseLive (true);
        
        panel.setUseHostTempo (true);
        panel.analyseCurve();
        waitFor (panel);
        const auto s = panel.snapshot();
        const auto& r = s.automation;
        check (s.state == BeatswitchRunner::State::done && r.valid, "tempo analysis finished", r.summary.substr (0, 80).c_str());
        check (r.behaviour == centsible::TempoBehaviour::Varispeed, "verdict: varispeed", centsible::behaviourName (r.behaviour));
        check (r.tempoFromMap, "the tempo came from the host, not from the audio");
        check (near (r.bpmStart, 100.0, 1.5) && near (r.bpmEnd, 120.0, 1.5), "tempo 100 -> 120", num (r.bpmStart, 1) + " -> " + num (r.bpmEnd, 1));
        // the detune of a song is only defined modulo 100 cents (a whole semitone more or less is another key, not another tuning): the first window is read in
        // [-50, 50) and the curve continues from there, so the START is compared modulo 100 and the RISE exactly
        const double startTruth = base + 1200.0 * std::log2 (100.0 / bpmRef), rise = 1200.0 * std::log2 (120.0 / 100.0);
        check (std::fabs (centsible::wrapCents (r.centsStart - startTruth)) < 6.0 && near (r.centsEnd - r.centsStart, rise, 6.0), "detune follows it: starts at -155 (= +45 mod 100), rises 316 cents",
              num (r.centsStart, 1) + " -> " + num (r.centsEnd, 1) + " (rise " + num (r.centsEnd - r.centsStart, 1) + ")");
        const double zero = bpmRef * std::pow (2.0, -base / 1200.0);
        bool haveZero = std::fabs (r.referenceBpm - zero) < 1.5;
        for (double z : r.zeroBpms) haveZero = haveZero || std::fabs (z - zero) < 1.5;
        check (haveZero, "the tempo where it sits on A440 (about 109.4 BPM) is among the reported ones", num (r.referenceBpm, 1));

        // measured instead of taken from the host: the same verdict from the audio alone
        panel.setUseHostTempo (false);
        panel.analyseCurve();
        waitFor (panel);
        const auto m = panel.snapshot().automation;
        check (m.valid && ! m.tempoFromMap && m.behaviour == centsible::TempoBehaviour::Varispeed, "measured from the audio alone: varispeed as well", centsible::behaviourName (m.behaviour));
    }

    // ----------------------------------------------------------------------------------------------------------------------------------------
    std::printf ("3. a dropped file, a typed time and a typed tempo\n");
    {
        const auto path = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("centsible_beatswitch_plugin_test.wav");
        {
            path.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (path), sr, 1, 24, {}, 0));
            const float* ch[1] = { songs.data() };
            if (w) w->writeFromFloatArrays (ch, 1, (int) songs.size());
        }
        TuningMatchProcessor proc;
        proc.setPlayConfigDetails (2, 2, sr, 512);
        proc.prepareToPlay (sr, 512);
        TuningMatchEditor editor (proc);
        editor.setSize (440, 700);
        editor.showBeatswitch (true);
        auto& panel = editor.beatswitchPanel();

        panel.setUseLive (false);
        panel.setTimes ("0:30");
        panel.analyse();
        check (! panel.busy() && panel.problem().isNotEmpty(), "no file yet: it says so", panel.problem());

        proc.analyzeFiles (juce::StringArray (path.getFullPathName()));
        waitForFile (proc);
        panel.poll();
        check (panel.sourceText().contains ("centsible_beatswitch_plugin_test"), "the panel shows the file", panel.sourceText());
        
        panel.analyse();
        waitFor (panel);
        sectionsChecks (panel.snapshot(), 30.0, "dropped file cut at 0:30");

        panel.setTimes ("16|1");
        panel.analyse();
        check (! panel.busy() && panel.problem().containsIgnoreCase ("tempo"), "bar | beat without a tempo asks for one", panel.problem());

        panel.setTempoText ("90");
        panel.analyse();
        waitFor (panel);
        const auto s = panel.snapshot();
        check (s.sections.sections.size() == 2 && near (s.sections.sections[0].end, 40.0, 0.01), "with 90 BPM typed, bar 16 is at 40 s", s.sections.sections.empty() ? juce::String() : num (s.sections.sections[0].end, 3));
        check (s.bars (40.0) == "16|1", "and shown as 16|1", juce::String (s.bars (40.0)));

        panel.setTempoText ("ninety");
        panel.analyse();
        check (! panel.busy() && panel.problem().isNotEmpty(), "a tempo that is not a number is refused", panel.problem());
        panel.setTempoText ({});
        panel.setTimes ("abc");
        panel.analyse();
        check (! panel.busy() && panel.problem().contains ("abc"), "a time that is not a time is refused, naming it", panel.problem());
        panel.setTimes ("");
        panel.analyse();
        check (! panel.busy() && panel.problem().isNotEmpty(), "sections without a time are refused", panel.problem());

        // tempo mode on the same file: finds the switch by itself (the two songs differ by 35 cents)
        
        panel.setTimes ("");
        panel.analyseCurve();
        // cancel it at once: the analysis stops quickly and the panel says cancelled
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        juce::Thread::sleep (300);
        panel.cancel();
        waitFor (panel);
        check (panel.snapshot().state == BeatswitchRunner::State::cancelled, "cancel stops the analysis", juce::String ((int) panel.snapshot().state));
        check (juce::Time::getMillisecondCounterHiRes() - t0 < 4000.0, "...quickly", num ((juce::Time::getMillisecondCounterHiRes() - t0) * 0.001, 2) + " s");

        panel.analyseCurve();
        waitFor (panel);
        const auto a = panel.snapshot().automation;
        check (a.valid && a.regions.size() == 2, "tempo mode finds the switch: two regions", juce::String ((int) a.regions.size()));
        if (a.regions.size() == 2)
        {
            check (near (a.regions[1].start, 30.0, 1.5), "the switch is at 30 s", num (a.regions[1].start, 2));
            check (near (a.regions[0].cents, 20.0, 2.0) && near (a.regions[1].cents, -15.0, 2.0), "the regions are +20 and -15 cents", num (a.regions[0].cents) + " / " + num (a.regions[1].cents));
        }
        // typed BPMs are shown, and a varispeed is recognised from them: the second song here is -35 cents lower, 120 BPM both: no tempo change, so no sentence
        panel.setTimes ("0:30");
        panel.setBpms ("120", "125");
        panel.analyse();
        waitFor (panel);
        check (panel.snapshot().typedBpm.size() == 2 && near (panel.snapshot().typedBpm[1], 125.0, 0.01), "typed BPMs are carried into the results", juce::String ((int) panel.snapshot().typedBpm.size()));
        panel.setBpms ("1x0", "");
        panel.analyse();
        check (! panel.busy() && panel.problem().contains ("First half BPM"), "a BPM that is not a number is refused, naming the box", panel.problem());
        panel.setBpms ("", "");
        panel.setThirdPart (true);
        panel.analyse();
        check (! panel.busy() && panel.problem().containsIgnoreCase ("again"), "the third part asks for its second switch", panel.problem());
        panel.setThirdPart (false);
        panel.setTimes ("0:30");

        // the suggested switch becomes the cut with one button, and the results can be copied as text
        panel.useSuggestedCuts();
        waitFor (panel);
        const auto sc = panel.snapshot();
        check (sc.mode == BeatswitchRunner::Mode::sections && sc.sections.sections.size() == 2, "use as cuts: the suggested switch becomes the cut and sections run", juce::String ((int) sc.sections.sections.size()));
        const auto txt = panel.resultsText();
        check (txt.contains ("->") && txt.contains ("BPM"), "copy results: the two halves and their difference as text", txt.replace ("\n", " | ").substring (0, 100));
        path.deleteFile();
    }

    // ----------------------------------------------------------------------------------------------------------------------------------------
    std::printf ("5. a seek while recording is noticed\n");
    {
        TuningMatchProcessor proc;
        proc.setPlayConfigDetails (2, 2, sr, 512);
        proc.prepareToPlay (sr, 512);
        proc.setPlayHead (&host);
        proc.setRecording (true);
        play (proc, host, songs, sr, [] (double) { return 120.0; }, 3.0, 20.0);
        proc.setRecording (false);
        check (proc.recordingInfo().jumps >= 1, "the host's position jump was counted", juce::String (proc.recordingInfo().jumps));
        TuningMatchEditor editor (proc);
        editor.setSize (440, 700);
        editor.showBeatswitch (true);
        auto& panel = editor.beatswitchPanel();
        panel.setUseLive (true);
        panel.setTimes ("0:30");
        panel.poll();
        check (panel.sourceText().containsIgnoreCase ("seek"), "the panel says so", panel.sourceText());
        panel.analyse();
        waitFor (panel);
        check (panel.snapshot().warning.containsIgnoreCase ("jumped"), "and the results carry the warning", panel.snapshot().warning);
    }

    // ----------------------------------------------------------------------------------------------------------------------------------------
    std::printf ("4. a recording that lost audio (the analysis thread starved) says so\n");
    {
        TuningMatchProcessor proc;
        proc.setPlayConfigDetails (2, 2, sr, 512);
        proc.prepareToPlay (sr, 512);
        proc.setPlayHead (&host);
        proc.setRecording (true);
        // fed as fast as the processor takes it: the 5.5 s FIFO overflows
        const auto mismatches = play (proc, host, songs, sr, [] (double) { return 120.0; }, 1.0e6);
        proc.setRecording (false);
        check (mismatches == 0, "the audio still passes through bit-identical when the recorder is overloaded", juce::String (mismatches) + " blocks differ");
        const auto info = proc.recordingInfo();
        check (info.droppedSeconds > 1.0 && info.seconds < 59.0, "the recording knows it lost audio", num (info.droppedSeconds, 1) + " s lost, " + num (info.seconds, 1) + " s kept");

        TuningMatchEditor editor (proc);
        editor.setSize (440, 700);
        editor.showBeatswitch (true);
        auto& panel = editor.beatswitchPanel();
        panel.setUseLive (true);
        panel.setTimes ("4");                      // (only a few seconds were kept: the sections are too short to analyse, which does not matter here)
        panel.poll();
        check (panel.sourceText().containsIgnoreCase ("gaps"), "the panel says so next to the recording", panel.sourceText());
        panel.analyse();
        check (panel.busy() || panel.snapshot().state != BeatswitchRunner::State::idle, "the analysis ran anyway", panel.problem());
        waitFor (panel);
        check (panel.snapshot().warning.containsIgnoreCase ("lost"), "and the results carry the warning", panel.snapshot().warning);
        proc.clearRecording();
        check (proc.recordingInfo().droppedSeconds == 0.0 && proc.recordedSeconds() == 0.0, "clearing the recording clears the warning");
    }

    std::printf ("\n%d checks, %d failed\n", checks, fails);
    std::printf ("%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
