// centsible_cli: the same analysis core as the plugin, offline and faster than real time.
//   centsible_cli [options] <file | folder | wildcard>...
//     --csv <out.csv>       write one row per file: file, seconds, cents, ci, a4_hz, confidence, locked, secondary, key, bpm, correction
//     --match <target> <source>   tuning + key advice for making <source> sit on <target>
//     --seconds <n>         analyse at most the first n seconds of every file (default: whole file)
//     --jobs <n>            files analysed in parallel (default: number of cores)
//     --sections "0:45, 1:30"  --automation [--tempo-points "0=120, 1:20=140"] ...   Beatswitch: see beatswitch_cli.h
#include "beatswitch_cli.h"
#include <centsible/sections.h>
#include <centsible/track.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <thread>

namespace
{
struct Result
{
    juce::File file;
    bool ok = false;
    centsible::TrackAnalysis a;
};

std::vector<float> loadMono (const juce::File& f, double& sr, double maxSeconds)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (! r) return {};
    sr = r->sampleRate;
    const int n = (int) std::min<juce::int64> (r->lengthInSamples, maxSeconds > 0.0 ? (juce::int64) (maxSeconds * sr) : r->lengthInSamples);
    juce::AudioBuffer<float> buf ((int) r->numChannels, n);
    r->read (&buf, 0, n, 0, true, true);
    std::vector<float> mono ((size_t) n, 0.0f);
    for (int c = 0; c < buf.getNumChannels(); ++c)
        for (int i = 0; i < n; ++i) mono[(size_t) i] += buf.getSample (c, i) / (float) buf.getNumChannels();
    return mono;
}

bool isAudio (const juce::File& f)
{
    const auto ext = f.getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".flac" || ext == ".mp3" || ext == ".ogg" || ext == ".aif" || ext == ".aiff" || ext == ".m4a";
}

void expand (const juce::String& arg, std::vector<juce::File>& out)
{
    juce::File f (juce::File::getCurrentWorkingDirectory().getChildFile (arg));
    if (f.isDirectory())
    {
        for (const auto& c : f.findChildFiles (juce::File::findFiles, false))
            if (isAudio (c)) out.push_back (c);
    }
    else if (f.existsAsFile())
        out.push_back (f);
    else if (arg.containsAnyOf ("*?"))
    {
        const auto parent = juce::File::getCurrentWorkingDirectory().getChildFile (arg).getParentDirectory();
        for (const auto& c : parent.findChildFiles (juce::File::findFiles, false, juce::File (arg).getFileName()))
            out.push_back (c);
    }
    else
        std::printf ("not found: %s\n", arg.toRawUTF8());
}

juce::String signedCents (double v) { return juce::String::formatted ("%+.1f", v); }

void printKey (const centsible::KeyEstimate& k, const char* indent = "  ")
{
    if (! k.valid) { std::printf ("%skey: not enough pitched content\n", indent); return; }
    std::printf ("%skey %s, %.0f %% (confidence %.2f)%s\n", indent, centsible::keyLabel (k).c_str(), 100.0 * (double) k.probability, (double) k.confidence,
                 k.betweenKeys ? ", between two keys: the neighbouring semitone is as likely" : "");
    std::printf ("%s  %s%s; %s\n", indent, centsible::keyAmbiguityText (k).c_str(), k.relativeAmbiguous ? "  <- relative major/minor ambiguity" : "", centsible::runnerUpText (k).c_str());
    std::printf ("%s  modes of this scale: %s\n", indent, centsible::modeSummary (k, 4).c_str());
}

void printResult (const Result& r)
{
    if (! r.ok) { std::printf ("%s\n  cannot decode\n", r.file.getFileName().toRawUTF8()); return; }
    const auto& t = r.a.tuning;
    std::printf ("%s\n", r.file.getFileName().toRawUTF8());
    if (! t.valid) { std::printf ("  no steady pitched content found\n"); return; }
    std::printf ("  %s cents +- %.1f   (A4 = %.2f Hz)   %s   confidence %.2f\n", signedCents (t.cents).toRawUTF8(), (double) t.ciCents, (double) t.refHz,
                 t.locked ? "LOCKED" : "not locked", (double) t.confidence);
    const double other = t.cents < 0 ? t.cents + 100.0 : t.cents - 100.0;
    std::printf ("  (= %s cents from one semitone %s)   shift %s cents to fix\n", signedCents (other).toRawUTF8(), t.cents < 0 ? "down" : "up", signedCents (-t.cents).toRawUTF8());
    if (t.hasSecondary) std::printf ("  also: %s cents (%d%% of the evidence)\n", signedCents (t.secondaryCents).toRawUTF8(), (int) std::lround (t.secondaryShare * 100.0f));
    if (t.hasDrift) std::printf ("  drift: the later half of the song is %s cents from the earlier half (the reading is the average)\n", signedCents (t.driftCents).toRawUTF8());
    if (r.a.tempo.valid)
    {
        std::printf ("  %.1f BPM", (double) r.a.tempo.bpm);
        if (r.a.tempo.altBpm > 0.0f) std::printf (" (or %.1f)", (double) r.a.tempo.altBpm);
        std::printf (" (confidence %.2f)\n", (double) r.a.tempo.confidence);
    }
    printKey (r.a.key);
}
} // namespace

int main (int argc, char** argv)
{
    std::vector<juce::File> files;
    juce::String csvPath, matchTarget, matchSource;
    double maxSeconds = 0.0;
    beatswitch_cli::Args bs;
    unsigned jobs = std::max (1u, std::thread::hardware_concurrency());
    for (int a = 1; a < argc; ++a)
    {
        const juce::String arg (argv[a]);
        if (arg == "--csv" && a + 1 < argc) csvPath = argv[++a];
        else if (arg == "--match" && a + 2 < argc) { matchTarget = argv[++a]; matchSource = argv[++a]; }
        else if (arg == "--seconds" && a + 1 < argc) maxSeconds = std::atof (argv[++a]);
        else if (arg == "--sections" && a + 1 < argc) bs.sections = argv[++a];
        else if (arg == "--automation") bs.automation = true;
        else if (arg == "--tempo-points" && a + 1 < argc) bs.tempoPoints = argv[++a];
        else if (arg == "--tempo-step") bs.tempoStep = true;
        else if (arg == "--reference-bpm" && a + 1 < argc) bs.referenceBpm = std::atof (argv[++a]);
        else if (arg == "--boundaries" && a + 1 < argc) bs.boundaries = argv[++a];
        else if (arg == "--curve-csv" && a + 1 < argc) bs.curveCsv = argv[++a];
        else if (arg == "--bpm" && a + 1 < argc) bs.bpm = std::atof (argv[++a]);
        else if (arg == "--beats-per-bar" && a + 1 < argc) bs.beatsPerBar = std::max (1, std::atoi (argv[++a]));
        else if (arg == "--first-bar" && a + 1 < argc) bs.firstBar = std::atof (argv[++a]);
        else if (arg == "--jobs" && a + 1 < argc) jobs = (unsigned) std::max (1, std::atoi (argv[++a]));
        else expand (arg, files);
    }
    if (matchTarget.isNotEmpty())
    {
        files.clear();
        expand (matchTarget, files);
        expand (matchSource, files);
        if (files.size() != 2) { std::printf ("--match needs exactly two readable files\n"); return 2; }
    }
    if (files.empty())
    {
        std::printf ("usage: centsible_cli [--csv out.csv] [--seconds n] [--jobs n] <file | folder | wildcard>...\n"
                     "       centsible_cli --match <target> <source>\n");
        return 2;
    }

    if (bs.wanted())
    {
        // Beatswitch: one file after the other (each analysis is long; its progress goes to stderr)
        centsible::TempoMap map;
        std::string error;
        if (! bs.tempoPoints.empty() && ! beatswitch_cli::parseTempoPoints (bs.tempoPoints, ! bs.tempoStep, map, error)) { std::printf ("%s\n", error.c_str()); return 2; }
        centsible::TimeContext tc;
        tc.bpm = bs.bpm; tc.beatsPerBar = bs.beatsPerBar; tc.firstBarSeconds = bs.firstBar;
        if (map.valid()) tc.tempoMap = &map;
        centsible::ParsedTimes cuts, regionCuts;
        const bool autoSections = bs.sections == "auto";      // --sections auto: cut where the detune jumps or the tempo steps (the suggested boundaries)
        if (! bs.sections.empty() && ! autoSections) { cuts = centsible::parseTimestampList (bs.sections, tc); if (! cuts.ok) { std::printf ("%s\n", cuts.error.c_str()); return 2; } }
        if (! bs.boundaries.empty()) { regionCuts = centsible::parseTimestampList (bs.boundaries, tc); if (! regionCuts.ok) { std::printf ("%s\n", regionCuts.error.c_str()); return 2; } }
        for (const auto& f : files)
        {
            double sr = 0.0;
            const auto mono = loadMono (f, sr, maxSeconds);
            if (mono.empty()) { std::printf ("%s\n  cannot decode\n", f.getFileName().toRawUTF8()); continue; }
            const std::string name = f.getFileName().toStdString();
            const double seconds = (double) mono.size() / sr;
            auto progress = [&] (double frac, const char* stage) { std::fprintf (stderr, "\r  %s: %3.0f %%   ", stage, 100.0 * frac); return true; };
            if (! bs.sections.empty())
            {
                auto cutTimes = cuts.seconds;
                if (autoSections)
                {
                    centsible::AutomationOptions o;
                    if (map.valid()) o.tempoMap = &map;
                    o.regionKeys = false;
                    const auto found = centsible::analyzeTempoAutomation (mono.data(), mono.size(), sr, o, progress);
                    for (const auto& b : found.boundaries) if (! b.passageStart && ! b.passageEnd) cutTimes.push_back (b.time);
                    std::printf ("%s: %d suggested boundaries%s\n", name.c_str(), (int) cutTimes.size(), cutTimes.empty() ? " (the track reads as one section)" : "");
                }
                const auto r = centsible::analyzeSections (mono.data(), mono.size(), sr, cutTimes, {}, progress);
                std::fprintf (stderr, "\r                              \r");
                beatswitch_cli::printSections (name, seconds, r, tc);
            }
            if (bs.automation)
            {
                centsible::AutomationOptions o;
                if (map.valid()) o.tempoMap = &map;
                o.referenceBpm = bs.referenceBpm;
                o.boundaries = regionCuts.seconds;
                const auto r = centsible::analyzeTempoAutomation (mono.data(), mono.size(), sr, o, progress);
                std::fprintf (stderr, "\r                              \r");
                beatswitch_cli::printAutomation (name, r);
                if (! bs.curveCsv.empty()) { beatswitch_cli::writeCurve (bs.curveCsv, r); std::printf ("  wrote %s\n", bs.curveCsv.c_str()); }
            }
        }
        return 0;
    }

    std::vector<Result> results (files.size());
    std::atomic<size_t> next { 0 };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::min<unsigned> (jobs, (unsigned) files.size()); ++t)
        pool.emplace_back ([&]
        {
            for (size_t i; (i = next++) < files.size();)
            {
                results[i].file = files[i];
                double sr = 0.0;
                const auto mono = loadMono (files[i], sr, maxSeconds);
                if (mono.empty()) continue;
                results[i].a = centsible::analyzeTrack (mono.data(), mono.size(), sr);
                results[i].ok = true;
            }
        });
    for (auto& t : pool) t.join();

    if (csvPath.isNotEmpty())
    {
        std::ofstream csv (csvPath.toRawUTF8());
        csv << "file,seconds,valid,cents,ci_cents,a4_hz,confidence,locked,secondary_cents,secondary_share,key,key_confidence,bpm,bpm_alt,bpm_confidence,correction_cents,drift_cents,key_detail,key_relative,key_scale_confidence,key_between,key_probability,key_camelot,key_runner_up,key_runner_up_probability,key_relative_probability,key_relative_ambiguous\n";
        for (const auto& r : results)
        {
            if (! r.ok) { csv << '"' << r.file.getFileName().toRawUTF8() << "\",,0\n"; continue; }
            const auto& t = r.a.tuning;
            char line[768];
            std::snprintf (line, sizeof line, "\"%s\",%.1f,%d,%.2f,%.2f,%.2f,%.2f,%d,%.1f,%.2f,%s,%.2f,%.1f,%.1f,%.2f,%.2f,%.1f,\"%s\",\"%s\",%.2f,%d", r.file.getFileName().toRawUTF8(), r.a.seconds, (int) t.valid,
                           (double) t.cents, (double) t.ciCents, (double) t.refHz, (double) t.confidence, (int) t.locked, t.hasSecondary ? (double) t.secondaryCents : 0.0,
                           t.hasSecondary ? (double) t.secondaryShare : 0.0, r.a.key.valid ? centsible::keyName (r.a.key.root, r.a.key.minor).c_str() : "",
                           (double) r.a.key.confidence, r.a.tempo.valid ? (double) r.a.tempo.bpm : 0.0, (double) r.a.tempo.altBpm, (double) r.a.tempo.confidence, -(double) t.cents, t.hasDrift ? (double) t.driftCents : 0.0,
                           r.a.key.valid ? centsible::keyDetail (r.a.key).c_str() : "", r.a.key.valid ? centsible::relativeKeyName (r.a.key).c_str() : "", (double) r.a.key.scaleConfidence, (int) r.a.key.betweenKeys);
            csv << line;
            const auto& k = r.a.key;
            char extra[256];
            std::snprintf (extra, sizeof extra, ",%.3f,%s,%s,%.3f,%.3f,%d\n", k.valid ? (double) k.probability : 0.0, k.valid ? centsible::camelot (k.root, k.minor).c_str() : "",
                           k.valid ? centsible::keyName (k.runnerUpRoot, k.runnerUpMinor).c_str() : "", k.valid ? (double) k.runnerUpProbability : 0.0, k.valid ? (double) k.relativeProbability : 0.0, (int) k.relativeAmbiguous);
            csv << extra;
        }
        std::printf ("wrote %s (%d files)\n", csvPath.toRawUTF8(), (int) results.size());
    }

    for (const auto& r : results) printResult (r);

    if (matchTarget.isNotEmpty() && results[0].ok && results[1].ok)
    {
        const auto m = centsible::matchAdvice (results[0].a, results[1].a);
        std::printf ("\nmatch: make %s sit on %s\n", results[1].file.getFileName().toRawUTF8(), results[0].file.getFileName().toRawUTF8());
        std::printf ("  tuning only: shift %s by %s cents\n", results[1].file.getFileName().toRawUTF8(), signedCents (m.fineCents).toRawUTF8());
        if (m.keysKnown)
            std::printf ("  tuning + key: shift by %+d semitone(s) and %s cents (= %s cents in total); keys %s -> %s%s\n", m.semitones, signedCents (m.fineCents).toRawUTF8(),
                         signedCents (m.totalCents).toRawUTF8(), centsible::keyName (results[1].a.key.root, results[1].a.key.minor).c_str(),
                         centsible::keyName (results[0].a.key.root, results[0].a.key.minor).c_str(), m.sameKeyFamily ? " (already the same key)" : "");
        if (results[0].a.tempo.valid && results[1].a.tempo.valid)
            std::printf ("  tempo: %.1f BPM vs %.1f BPM (ratio %.3f)\n", (double) results[0].a.tempo.bpm, (double) results[1].a.tempo.bpm, (double) (results[0].a.tempo.bpm / results[1].a.tempo.bpm));
    }
    return 0;
}
