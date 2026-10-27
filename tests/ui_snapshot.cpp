// Renders the real editor offscreen to a PNG, driven by the real processor + analysis thread (no mock data).
//   centsible_ui_snapshot <out.png> <detuneCents | audio file> <seconds> [hold=0] [scale=2] [width=440] [speed=2] [options]
// "drop:<file>[|<file2>]" instead of the second argument simulates dropping one audio file (tuning, key, tempo) or two (match advice) on the editor.
// If the second argument is an existing audio file, the first <seconds> of it are played through the plugin; otherwise a synthetic chord
// progression detuned by that many cents is. speed = how many times faster than real time the audio is fed (the analysis thread must keep up).
//
// Options (after the positional arguments) open the Beatswitch panel and drive it like a user would:
//   --beatswitch sections|tempo   open the panel in that mode
//   --times "1:25" or "1:25, 2:40"  the switch box (two times switch the third part on)      --bpms "140,150,"   the BPM boxes
//   --live                        use the live source: the audio is recorded while it is played through the plugin (host tempo from --host-bpm)
//   --host-bpm 128[:140]          a host that reports this tempo (a ramp from the first to the second over the audio), 4/4, bar 1 at time 0
//   --host-tempo                  tempo mode: use the host's tempo instead of measuring it
//   --analyse                     press "analyse" and wait for the result
//   --scroll 300                  scroll the results (design pixels)
//   --typing "text"               put this in the times field without analysing (to look at the field)
#include "PluginEditor.h"
#include <cmath>
#include <cstdio>
#include <map>

namespace
{
    // A host with a tempo (constant, or a linear ramp over `seconds`), 4/4, bar 1 beat 1 at the start of the audio.
    class FakeHost : public juce::AudioPlayHead
    {
    public:
        double sampleRate = 48000.0, bpm0 = 120.0, bpm1 = 120.0, seconds = 1.0;
        std::atomic<juce::int64> samples { 0 };

        juce::Optional<PositionInfo> getPosition() const override
        {
            const double t = (double) samples.load() / sampleRate;
            const double u = std::min (t, seconds);
            const double bpm = bpm0 + (bpm1 - bpm0) * u / seconds;
            const double ppq = (bpm0 * u + 0.5 * (bpm1 - bpm0) * u * u / seconds) / 60.0 + (t > seconds ? bpm1 * (t - seconds) / 60.0 : 0.0);
            PositionInfo info;
            info.setBpm (bpm);
            info.setPpqPosition (ppq);
            info.setTimeInSeconds (t);
            info.setTimeInSamples (samples.load());
            info.setTimeSignature (TimeSignature { 4, 4 });
            info.setIsPlaying (true);
            return info;
        }
    };
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 4) { std::printf ("usage: centsible_ui_snapshot <out.png> <cents|audio file> <seconds> [hold] [scale] [width] [speed] [--beatswitch sections|tempo ...]\n"); return 2; }

    // positional arguments and --options
    std::vector<juce::String> positional;
    std::map<juce::String, juce::String> opt;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a = juce::String::fromUTF8 (argv[i]);
        if (a.startsWith ("--"))
        {
            const bool isFlag = a == "--live" || a == "--analyse" || a == "--host-tempo";
            opt[a.substring (2)] = isFlag || i + 1 >= argc ? juce::String ("1") : juce::String (juce::CharPointer_UTF8 (argv[++i]));
        }
        else positional.push_back (a);
    }
    if (positional.size() < 3) { std::printf ("usage: centsible_ui_snapshot <out.png> <cents|audio file> <seconds> ...\n"); return 2; }
    for (const auto& kv : opt) std::printf ("option %s = '%s'\n", kv.first.toRawUTF8(), kv.second.toRawUTF8());
    const juce::File out (positional[0]);
    const juce::File audioFile (positional[1]);
    const double seconds = positional[2].getDoubleValue();
    const bool hold = positional.size() > 3 && positional[3].getIntValue() != 0;
    const float scale = positional.size() > 4 ? positional[4].getFloatValue() : 2.0f;
    const int width = positional.size() > 5 ? positional[5].getIntValue() : 440;
    const double speed = positional.size() > 6 ? std::max (0.5, positional[6].getDoubleValue()) : 2.0;
    constexpr double pi = juce::MathConstants<double>::pi;

    TuningMatchProcessor proc;
    const juce::String arg2 = positional[1];
    const bool dropMode = arg2.startsWith ("drop:");
    const bool live = opt.count ("live") > 0;
    double sr = 48000.0;
    juce::AudioBuffer<float> source;
    FakeHost host;

    if (dropMode)
    {
        proc.setPlayConfigDetails (2, 2, sr, 512);
        proc.prepareToPlay (sr, 512);
        juce::StringArray paths;
        paths.addTokens (arg2.fromFirstOccurrenceOf ("drop:", false, false), "|", "");
        proc.analyzeFiles (paths);
        for (int waited = 0; proc.fileSnapshot().busy() || waited == 0; ++waited)
        {
            juce::Thread::sleep (100);
            if (waited > 6000) break;
        }
    }
    else if (audioFile.existsAsFile())
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (audioFile));
        if (! reader) return 3;
        sr = reader->sampleRate;
        const int n = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (seconds * sr));
        source.setSize (2, n);
        reader->read (&source, 0, n, 0, true, true);
        if (reader->numChannels == 1) source.copyFrom (1, 0, source, 0, 0, n);
    }
    else if (seconds > 0.0)
    {
        const double detune = arg2.getDoubleValue();
        const int chords[4][3] = { { -12, -9, -5 }, { -16, -12, -9 }, { -14, -10, -7 }, { -17, -13, -10 } };
        const double k = std::pow (2.0, detune / 1200.0);
        const int n = (int) (seconds * sr);
        source.setSize (2, n);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            const auto& c = chords[((int) (t / 1.5)) % 4];
            double s = 0.0;
            for (int semis : c)
                for (int h = 1; h <= 6; ++h) s += std::sin (2.0 * pi * 440.0 * std::pow (2.0, semis / 12.0) * k * h * t) / h;
            source.setSample (0, i, (float) (0.08 * s));
            source.setSample (1, i, (float) (0.08 * s));
        }
    }

    if (! dropMode)
    {
        proc.setPlayConfigDetails (2, 2, sr, 512);
        proc.prepareToPlay (sr, 512);
        if (opt.count ("host-bpm"))
        {
            const auto spec = opt["host-bpm"];
            host.sampleRate = sr;
            host.bpm0 = spec.upToFirstOccurrenceOf (":", false, false).getDoubleValue();
            host.bpm1 = spec.contains (":") ? spec.fromFirstOccurrenceOf (":", false, false).getDoubleValue() : host.bpm0;
            host.seconds = std::max (1.0, (double) source.getNumSamples() / sr);
            proc.setPlayHead (&host);
        }
        if (live) proc.setRecording (true);
    }
    juce::MidiBuffer midi;
    const int blocksPerSleep = std::max (1, (int) std::lround (25.0 * speed * 0.001 * sr / 512.0));
    for (int pos = 0, b = 0; pos < source.getNumSamples(); pos += 512, ++b)
    {
        const int len = std::min (512, source.getNumSamples() - pos);
        juce::AudioBuffer<float> buf (2, len);
        buf.copyFrom (0, 0, source, 0, pos, len);
        buf.copyFrom (1, 0, source, 1, pos, len);
        host.samples = pos;
        proc.processBlock (buf, midi);
        if (b % blocksPerSleep == 0) juce::Thread::sleep (25); // let the analysis thread keep up
    }
    juce::Thread::sleep (800);
    if (live) proc.setRecording (false);
    if (hold) proc.apvts.getParameter ("hold")->setValueNotifyingHost (1.0f);

    std::unique_ptr<TuningMatchEditor> editor (new TuningMatchEditor (proc));
    editor->setSize (width, (int) std::round (width * TuningMatchEditor::designH / TuningMatchEditor::designW));
    editor->snapAnimationsToTarget();

    if (opt.count ("beatswitch"))
    {
        editor->showBeatswitch (true);
        auto& panel = editor->beatswitchPanel();
        panel.setUseLive (live);
        if (opt.count ("times")) panel.setTimes (opt["times"]);
        if (opt.count ("bpms")) { juce::StringArray b; b.addTokens (opt["bpms"], ",", ""); panel.setBpms (b[0], b[1], b[2]); }
        if (opt.count ("host-tempo")) panel.setUseHostTempo (true);
        if (opt.count ("typing")) panel.setTimes (opt["typing"]);
        panel.poll();
        if (opt.count ("analyse"))
        {
            if (opt["beatswitch"] == "tempo") panel.analyseCurve(); else panel.analyse();
            for (int waited = 0; panel.busy() && waited < 6000; ++waited) juce::Thread::sleep (100);
            panel.poll();
            const auto s = panel.snapshot();
            std::printf ("beatswitch: state %d  %.1f s  problem '%s'\n", (int) s.state, s.elapsed, panel.problem().toRawUTF8());
        }
        if (opt.count ("scroll")) panel.scrollTo (opt["scroll"].getFloatValue());
    }
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, scale);

    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    png.writeImageToStream (image, stream);
    const auto r = proc.getReading();
    std::printf ("wrote %s (%d x %d)  reading %+.2f +- %.2f  locked %d  confidence %.2f\n", out.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight(),
                 (double) r.cents, (double) r.ciCents, (int) r.locked, (double) r.confidence);
    editor.reset();
    proc.setPlayHead (nullptr);
    return 0;
}
