// Beatswitch: timestamps, sections and tempo automation on synthetic songs whose detune, key and tempo are known exactly.
//   * timestamp parsing and the tempo map;
//   * sections: two songs with different tuning, key and tempo joined at a boundary;
//   * tempo automation: varispeed ramp (the pitch follows the tempo), time-stretch (it does not), constant, pitch step, tempo step, a beat switch
//     (everything changes), a manual tempo map;
//   * key probabilities, runner-up, relative major/minor ambiguity and Camelot notation;
//   * progress reporting and cancellation.
#include "gen/warp_synth.h"
#include <centsible/sections.h>
#include <cstdio>
#include <cstring>

using namespace synth;

static int fails = 0;
static const double SR = 44100.0;

static void check (bool ok, const char* name, const std::string& detail = {})
{
    std::printf ("  %-62s %s%s%s\n", name, ok ? "PASS" : "FAIL", detail.empty() ? "" : "   ", detail.c_str());
    fails += ! ok;
}

static std::string num (const char* f, double a, double b = 0.0, double c = 0.0)
{
    char buf[200];
    std::snprintf (buf, sizeof buf, f, a, b, c);
    return buf;
}

static bool keyIs (const centsible::KeyEstimate& k, int tonic, bool minor)    // the key itself or its relative (the known weakness, reported as ambiguity)
{
    if (! k.valid) return false;
    const int rel = minor ? (tonic + 3) % 12 : (tonic + 9) % 12;
    return (k.root == tonic && k.minor == minor) || (k.root == rel && k.minor != minor);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
static void timestampTests()
{
    std::printf ("[timestamps and tempo map]\n");
    using namespace centsible;
    TimeContext none;
    TimeContext c120;
    c120.bpm = 120.0;
    auto sec = [] (const std::string& s, const TimeContext& c, double& out) { const auto r = parseTimestamp (s, c); out = r.seconds; return r.ok; };
    double v = 0.0;
    check (sec ("1:23.5", none, v) && std::fabs (v - 83.5) < 1e-9, "1:23.5 = 83.5 s", num ("%.3f", v));
    check (sec ("0:05", none, v) && std::fabs (v - 5.0) < 1e-9, "0:05 = 5 s");
    check (sec ("12:03.250", none, v) && std::fabs (v - 723.25) < 1e-9, "12:03.250 = 723.25 s");
    check (sec ("1:02:03.25", none, v) && std::fabs (v - 3723.25) < 1e-9, "1:02:03.25 = 3723.25 s");
    check (sec ("83.5", none, v) && std::fabs (v - 83.5) < 1e-9, "plain seconds 83.5");
    check (sec ("83.5s", none, v) && std::fabs (v - 83.5) < 1e-9, "plain seconds 83.5s");
    check (! parseTimestamp ("2:75", none).ok && ! parseTimestamp ("2:75", none).error.empty(), "2:75 is refused with a message", parseTimestamp ("2:75", none).error);
    check (! parseTimestamp ("abc", none).ok, "abc is refused");
    check (! parseTimestamp ("", none).ok, "empty is refused");
    check (! parseTimestamp ("1:2:3:4", none).ok, "1:2:3:4 is refused");
    check (! parseTimestamp ("17|1", none).ok, "bar|beat without a tempo is refused", parseTimestamp ("17|1", none).error);
    check (sec ("17|1", c120, v) && std::fabs (v - 32.0) < 1e-9, "17|1 at 120 BPM = 32 s", num ("%.3f", v));
    check (sec ("17|2.5", c120, v) && std::fabs (v - 32.75) < 1e-9, "17|2.5 at 120 BPM = 32.75 s", num ("%.3f", v));
    check (sec ("bar 17", c120, v) && std::fabs (v - 32.0) < 1e-9, "'bar 17' at 120 BPM = 32 s");
    check (! parseTimestamp ("17|5", c120).ok, "beat 5 does not exist in 4/4", parseTimestamp ("17|5", c120).error);
    {
        TimeContext c = c120;
        c.firstBarSeconds = 1.5;
        check (sec ("1|1", c, v) && std::fabs (v - 1.5) < 1e-9, "bar 1 starts where firstBarSeconds says");
        c.beatsPerBar = 3;
        check (sec ("3|1", c, v) && std::fabs (v - (1.5 + 6.0 * 0.5)) < 1e-9, "3/4 time: 3|1 = 6 beats after bar 1");
        c.beatsPerBar = 6;
        c.quartersPerBeat = 0.5;                       // 6/8: six eighth-note beats per bar, 120 quarter-note BPM
        check (sec ("3|1", c, v) && std::fabs (v - (1.5 + 2.0 * 3.0 * 0.5)) < 1e-9 && sec ("2|4", c, v) && std::fabs (v - (1.5 + (6.0 + 3.0) * 0.25)) < 1e-9, "6/8 time: eighth-note beats", num ("%.3f", v));
        check (formatBarBeat (1.5 + 3.0 * 0.5, c) == "2|1", "6/8 time: formatBarBeat", formatBarBeat (1.5 + 3.0 * 0.5, c));
    }
    {
        const auto r = parseTimestampList ("1:00, 0:30; 1:00\n17|1 2:00", c120);
        check (r.ok && r.seconds.size() == 4 && std::fabs (r.seconds[0] - 30.0) < 1e-9 && std::fabs (r.seconds[1] - 32.0) < 1e-9 && std::fabs (r.seconds[2] - 60.0) < 1e-9 && std::fabs (r.seconds[3] - 120.0) < 1e-9,
               "a list is split, sorted and de-duplicated", r.ok ? num ("%.0f values", (double) r.seconds.size()) : r.error);
        const auto bad = parseTimestampList ("0:30, nonsense", c120);
        check (! bad.ok && bad.error.find ("nonsense") != std::string::npos, "an error names the offending text", bad.error);
    }
    check (formatTimestamp (83.5) == "1:23.500" && formatTimestamp (3723.25) == "1:02:03.250", "formatTimestamp", formatTimestamp (83.5) + " " + formatTimestamp (3723.25));
    check (formatBarBeat (32.75, c120) == "17|2.5" && formatBarBeat (32.0, c120) == "17|1", "formatBarBeat", formatBarBeat (32.75, c120) + " " + formatBarBeat (32.0, c120));
    check (sectionLabel (0) == "A" && sectionLabel (25) == "Z" && sectionLabel (26) == "AA" && sectionLabel (27) == "AB", "section labels A .. Z, AA, AB");

    {
        const auto m = TempoMap::constant (120.0);
        check (std::fabs (m.beatsAt (10.0) - 20.0) < 1e-9 && std::fabs (m.timeOfBeat (20.0) - 10.0) < 1e-6, "constant map: 20 beats in 10 s and back");
        TempoMap ramp;
        ramp.add (0.0, 120.0);
        ramp.add (60.0, 180.0);
        check (std::fabs (ramp.bpmAt (30.0) - 150.0) < 1e-9 && std::fabs (ramp.beatsAt (60.0) - 150.0) < 1e-9 && std::fabs (ramp.timeOfBeat (150.0) - 60.0) < 1e-6,
               "linear ramp 120 -> 180: tempo, beats and inverse", num ("%.4f beats, %.4f s", ramp.beatsAt (60.0), ramp.timeOfBeat (150.0)));
        TempoMap step;
        step.linear = false;
        step.add (0.0, 120.0);
        step.add (30.0, 180.0);
        check (std::fabs (step.bpmAt (29.9) - 120.0) < 1e-9 && std::fabs (step.bpmAt (30.0) - 180.0) < 1e-9 && std::fabs (step.beatsAt (60.0) - 150.0) < 1e-9, "step map");
        TimeContext cm;
        cm.tempoMap = &ramp;
        const auto r = parseTimestamp ("17|1", cm);       // 64 beats into the ramp
        check (r.ok && std::fabs (ramp.beatsAt (r.seconds) - 64.0) < 1e-6, "bar|beat through a tempo map", num ("%.3f s", r.seconds));
    }

    // the tempo as typed in the plugin: one number, or time=bpm points
    {
        const auto one = parseTempoText ("128");
        check (one.ok && one.map.points().size() == 1 && std::fabs (one.map.bpmAt (50.0) - 128.0) < 1e-9, "tempo text '128' is one constant tempo");
        const auto withUnit = parseTempoText (" 127.5 bpm ");
        check (withUnit.ok && std::fabs (withUnit.map.bpmAt (0.0) - 127.5) < 1e-9, "tempo text '127.5 bpm'");
        const auto pts = parseTempoText ("0=100, 1:00=160");
        check (pts.ok && pts.map.points().size() == 2 && pts.map.linear && std::fabs (pts.map.bpmAt (30.0) - 130.0) < 1e-9, "tempo text with points ramps linearly", num ("%.2f", pts.map.bpmAt (30.0)));
        const auto stp = parseTempoText ("0=100; 30=160", false);
        check (stp.ok && ! stp.map.linear && std::fabs (stp.map.bpmAt (29.0) - 100.0) < 1e-9 && std::fabs (stp.map.bpmAt (31.0) - 160.0) < 1e-9, "tempo text with points as steps");
        check (! parseTempoText ("fast").ok && ! parseTempoText ("fast").error.empty(), "tempo text 'fast' is refused with a message", parseTempoText ("fast").error);
        check (! parseTempoText ("5").ok && ! parseTempoText ("900").ok, "tempo outside 20 - 400 BPM is refused");
        check (! parseTempoText ("0=100, 160").ok, "a mix of points and a bare number is refused", parseTempoText ("0=100, 160").error);
        check (! parseTempoText ("x=100").ok && ! parseTempoText ("30=").ok, "bad points are refused", parseTempoText ("x=100").error);
        check (! parseTempoText ("").ok, "empty tempo text is refused");
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------
static void sectionTests()
{
    std::printf ("[sections]\n");
    // A: C major, 120 BPM, +17.0 cents, 40 s.  B: E minor, 128 BPM, -21.0 cents, 40 s.
    ChordSongSpec sa; sa.seconds = 40.0; sa.tonic = 0; sa.minor = false; sa.seed = 3;
    ChordSongSpec sb; sb.seconds = 40.0; sb.tonic = 4; sb.minor = true; sb.seed = 4;
    const auto a = renderChordSong (sa, constantPlan (120.0, 17.0), SR);
    const auto b = renderChordSong (sb, constantPlan (128.0, -21.0), SR);
    const auto x = concat (a, b);
    const auto r = centsible::analyzeSections (x.data(), x.size(), SR, { 40.0 });
    check (r.sections.size() == 2 && r.deltas.size() == 1, "one boundary gives two sections and one delta");
    if (r.sections.size() != 2) return;
    const auto& A = r.sections[0];
    const auto& B = r.sections[1];
    check (A.label == "A" && B.label == "B", "labels A and B");
    check (std::fabs (A.usedEnd - 39.25) < 1e-9 && std::fabs (B.usedStart - 40.75) < 1e-9, "0.75 s trimmed on each side of the boundary", num ("A ends %.2f, B starts %.2f", A.usedEnd, B.usedStart));
    check (A.ok && B.ok && A.analysis.tuning.valid && B.analysis.tuning.valid, "both sections analysed");
    check (std::fabs (A.analysis.tuning.cents - 17.0) <= 0.15, "section A detune +17.0", num ("%+.3f", A.analysis.tuning.cents));
    check (std::fabs (B.analysis.tuning.cents + 21.0) <= 0.15, "section B detune -21.0", num ("%+.3f", B.analysis.tuning.cents));
    check (keyIs (A.analysis.key, 0, false), "section A key C major (or relative)", centsible::keyLabel (A.analysis.key));
    check (keyIs (B.analysis.key, 4, true), "section B key E minor (or relative)", centsible::keyLabel (B.analysis.key));
    check (std::fabs (A.analysis.tempo.bpm - 120.0) <= 1.0 && std::fabs (B.analysis.tempo.bpm - 128.0) <= 1.0, "tempi 120 and 128", num ("%.2f %.2f", A.analysis.tempo.bpm, B.analysis.tempo.bpm));
    const auto& d = r.deltas[0];
    check (d.tuningKnown && std::fabs (d.cents + 38.0) <= 0.3 && d.significant, "delta -38.0 cents, significant", num ("%+.3f +- %.3f", d.cents, d.ci));
    check (d.keysKnown && ! d.sameScale && d.semitones == -5 && std::fabs (d.totalCents - (-538.0)) <= 0.3, "scale moves -5 semitones (G major <- C major), total -538 cents",
           num ("%.0f semitones, total %+.2f", d.semitones, d.totalCents));
    check (d.tempoKnown && std::fabs (d.tempoRatio - 128.0 / 120.0) < 0.02, "tempo ratio 128/120", num ("%.4f", d.tempoRatio));

    // four sections, one of them too short; boundaries unsorted, duplicated and out of range
    const auto r4 = centsible::analyzeSections (x.data(), x.size(), SR, { 60.0, 20.0, 20.0, 25.0, -3.0, 500.0 });
    check (r4.sections.size() == 4, "unsorted / duplicate / out-of-range boundaries are cleaned up", num ("%.0f sections", (double) r4.sections.size()));
    if (r4.sections.size() == 4)
    {
        check (r4.sections[1].label == "B" && ! r4.sections[1].ok && ! r4.sections[1].note.empty(), "a 5 s section is reported as too short, not guessed at", r4.sections[1].note);
        check (! r4.deltas[0].tuningKnown && ! r4.deltas[1].tuningKnown, "deltas next to a skipped section are unknown");
        check (r4.sections[3].ok && std::fabs (r4.sections[3].analysis.tuning.cents + 21.0) <= 0.15, "the last section is still analysed (-21.0)", num ("%+.3f", r4.sections[3].analysis.tuning.cents));
    }

    // no boundary: one section, same as a whole-track analysis
    const auto r1 = centsible::analyzeSections (x.data(), x.size(), SR, {});
    check (r1.sections.size() == 1 && r1.deltas.empty() && r1.sections[0].ok, "no boundary gives one section");
}

// ---------------------------------------------------------------------------------------------------------------------------------------
static void keyTextTests()
{
    std::printf ("[key probabilities, runner-up, relative ambiguity, Camelot]\n");
    using namespace centsible;
    check (camelot (9, true) == "8A" && camelot (0, false) == "8B" && camelot (0, true) == "5A" && camelot (3, false) == "5B" && camelot (6, true) == "11A" && camelot (11, false) == "1B", "Camelot wheel",
           camelot (9, true) + " " + camelot (0, false) + " " + camelot (0, true) + " " + camelot (3, false) + " " + camelot (6, true) + " " + camelot (11, false));
    ChordSongSpec s; s.seconds = 45.0; s.tonic = 0; s.minor = false; s.seed = 9;
    const auto x = renderChordSong (s, constantPlan (110.0, 12.0), SR);
    const auto t = analyzeTrack (x.data(), x.size(), SR);
    const auto& k = t.key;
    check (k.valid && keyIs (k, 0, false), "C major song: key found", keyLabel (k));
    check (k.probability > 0.3f && k.probability <= 1.0f && k.runnerUpProbability <= k.probability + 1e-6f && k.relativeProbability >= 0.0f && k.probability + k.runnerUpProbability <= 1.0001f,
           "probabilities are consistent", num ("p %.2f, runner-up %.2f, relative %.2f", k.probability, k.runnerUpProbability, k.relativeProbability));
    const std::string amb = keyAmbiguityText (k);
    check (amb.find ("/") != std::string::npos && (amb.find ("C major") != std::string::npos || amb.find ("A minor") != std::string::npos), "relative ambiguity text names the pair", amb);
    check (runnerUpText (k).find ("runner-up") != std::string::npos, "runner-up text", runnerUpText (k));
    check (keyLabel (k).find ("(") != std::string::npos, "label carries the Camelot code", keyLabel (k));
    {
        // all seven diatonic modes are named, in the order of the degree of the tonic
        const char* want[7] = { "Ionian", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Aeolian", "Locrian" };
        bool names = true;
        for (int i = 0; i < 7; ++i) names = names && std::string (modeName (i)) == want[i];
        check (names, "the seven modes are named Ionian .. Locrian");
        float sum = 0.0f;
        for (float p : k.modeProbability) sum += p;
        check (std::fabs (sum - 1.0f) < 0.01f, "mode probabilities of the scale sum to 1", num ("%.4f", (double) sum));
        const auto detail = keyDetail (k);
        check (detail.find (std::string (" (") + modeName (k.degree) + ")") != std::string::npos, "every key carries its mode name", detail);
        check (modeSummary (k, 3).find ("%") != std::string::npos, "the mode summary lists probabilities", modeSummary (k, 3));
        KeyEstimate dorian = k;
        dorian.root = 2; dorian.minor = true; dorian.scaleRoot = 0; dorian.degree = 1;
        check (keyLabel (dorian) == "D minor (Dorian) (7A)", "D Dorian is labelled with its Camelot code", keyLabel (dorian));
        KeyEstimate lydian = k;
        lydian.root = 5; lydian.minor = false; lydian.scaleRoot = 0; lydian.degree = 3;
        check (keyLabel (lydian) == "F major (Lydian) (7B)", "F Lydian", keyLabel (lydian));
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------
static centsible::TempoAutomationResult automation (const Plan& plan, double seconds, int tonic, bool minor, const centsible::AutomationOptions& opt = {}, unsigned seed = 1)
{
    ChordSongSpec spec;
    spec.seconds = seconds; spec.tonic = tonic; spec.minor = minor; spec.seed = seed;
    const auto x = renderChordSong (spec, plan, SR);
    return centsible::analyzeTempoAutomation (x.data(), x.size(), SR, opt);
}

static void printBoundaries (const centsible::TempoAutomationResult& r)
{
    for (const auto& b : r.boundaries) std::printf ("      boundary %s: %s (strength %.0f)\n", centsible::formatTimestamp (b.time).c_str(), b.reason.c_str(), b.strength);
}

static void automationTests()
{
    using namespace centsible;
    std::printf ("[tempo automation]\n");

    // varispeed: 120 -> 140 BPM between 20 s and 80 s, the pitch follows (+10 cents at 120 BPM, so +277.1 at 140)
    {
        const auto plan = rampPlan (120.0, 140.0, 20.0, 80.0, true, 10.0, 120.0);
        const auto r = automation (plan, 100.0, 9, true);
        std::printf ("  varispeed 120 -> 140: %s\n", r.summary.c_str());
        check (r.valid && r.behaviour == TempoBehaviour::Varispeed, "varispeed ramp is recognised as varispeed", behaviourName (r.behaviour));
        check (std::fabs (r.slope - 1.0) <= 0.12, "fitted slope close to 1", num ("%.3f", r.slope));
        check (std::fabs (r.bpmStart - 120.0) <= 1.5 && std::fabs (r.bpmEnd - 140.0) <= 1.5, "tempo 120 -> 140 followed", num ("%.2f -> %.2f", r.bpmStart, r.bpmEnd));
        check (std::fabs (r.centsStart - 10.0) <= 2.5 && std::fabs (r.centsEnd - 277.1) <= 4.0, "detune followed on the unwrapped scale (+10 -> +277)", num ("%+.1f -> %+.1f", r.centsStart, r.centsEnd));
        check (std::fabs (r.referenceBpm - 119.31) <= 0.8, "reference tempo where the detune is 0 (119.3 BPM)", num ("%.2f", r.referenceBpm));
        check (r.spreadVarispeed <= 8.0 && r.spreadFixed > 15.0, "varispeed explains the detune far better than a constant does", num ("%.1f vs %.1f", r.spreadVarispeed, r.spreadFixed));
        check (r.summary.find ("Varispeed") != std::string::npos && r.summary.find ("A440") != std::string::npos, "summary says varispeed and names the reference tempo");
        check (r.boundaries.empty(), "a smooth ramp suggests no boundary", std::to_string (r.boundaries.size()));
        printBoundaries (r);
        double worst = 0.0;
        for (const auto& c : r.curve) if (c.detuneValid) worst = std::max (worst, std::fabs (wrapCents (c.cents - plan.cents (c.time))));
        check (worst <= 3.0, "every window within 3 cents of the truth during the ramp", num ("max %.2f", worst));
        check (! r.regions.empty() && r.regions[0].key.valid && keyIs (r.regions[0].key, 9, true), "key (A minor or relative) found across a 267-cent ramp, measured against the tuning curve",
               r.regions.empty() ? "" : keyLabel (r.regions[0].key));
    }

    // time-stretch: same tempo ramp, the pitch stays +23.4
    {
        const auto r = automation (rampPlan (120.0, 140.0, 20.0, 80.0, false, 23.4, 120.0), 100.0, 9, true);
        std::printf ("  time-stretch 120 -> 140: %s\n", r.summary.c_str());
        check (r.valid && r.behaviour == TempoBehaviour::TimeStretch, "time-stretch is recognised as such", behaviourName (r.behaviour));
        check (std::fabs (r.slope) <= 0.12, "fitted slope close to 0", num ("%.3f", r.slope));
        check (std::fabs (wrapCents (r.centsStart - 23.4)) <= 0.5 && std::fabs (wrapCents (r.centsEnd - 23.4)) <= 0.5, "detune stays +23.4", num ("%+.2f / %+.2f", r.centsStart, r.centsEnd));
        check (r.referenceBpm == 0.0, "no reference tempo for a time-stretch");
        check (r.boundaries.empty(), "no boundary", std::to_string (r.boundaries.size()));
    }

    // constant
    {
        const auto r = automation (constantPlan (120.0, 23.4), 80.0, 0, false);
        std::printf ("  constant: %s\n", r.summary.c_str());
        check (r.valid && r.behaviour == TempoBehaviour::Constant, "constant tempo and pitch", behaviourName (r.behaviour));
        check (std::fabs (r.medianBpm - 120.0) <= 1.0, "tempo 120", num ("%.2f", r.medianBpm));
        check (r.regions.size() == 1 && std::fabs (r.regions[0].cents - 23.4) <= 0.15 && r.regions[0].ci < 1.0, "one region at +23.4", r.regions.empty() ? "" : num ("%+.3f +- %.3f", r.regions[0].cents, r.regions[0].ci));
        check (r.boundaries.empty(), "no boundary", std::to_string (r.boundaries.size()));
    }

    // pitch step at 50 s (+5 -> -20), tempo steady
    {
        const Plan plan { [] (double) { return 120.0; }, [] (double t) { return t < 50.0 ? 5.0 : -20.0; } };
        const auto r = automation (plan, 100.0, 2, false);
        std::printf ("  pitch step: %s\n", r.summary.c_str());
        printBoundaries (r);
        check (r.valid && r.behaviour == TempoBehaviour::PitchMoves, "steady tempo, pitch moves", behaviourName (r.behaviour));
        check (r.boundaries.size() == 1 && r.boundaries[0].detuneJump && ! r.boundaries[0].tempoStep, "one detune boundary suggested", std::to_string (r.boundaries.size()));
        if (r.boundaries.size() == 1)
        {
            check (std::fabs (r.boundaries[0].time - 50.0) <= 0.4, "placed at 50 s to within 0.4 s", num ("%.2f s", r.boundaries[0].time));
            check (std::fabs (r.boundaries[0].jumpCents + 25.0) <= 1.5, "jump -25 cents", num ("%+.2f", r.boundaries[0].jumpCents));
        }
        check (r.regions.size() == 2 && std::fabs (r.regions[0].cents - 5.0) <= 0.2 && std::fabs (r.regions[1].cents + 20.0) <= 0.2, "regions +5 and -20", r.regions.size() == 2 ? num ("%+.2f, %+.2f", r.regions[0].cents, r.regions[1].cents) : "");
    }

    // tempo step at 50 s (120 -> 140), pitch constant
    {
        const Plan plan { [] (double t) { return t < 50.0 ? 120.0 : 140.0; }, [] (double) { return 8.0; } };
        const auto r = automation (plan, 100.0, 0, false);
        std::printf ("  tempo step: %s\n", r.summary.c_str());
        printBoundaries (r);
        check (r.valid && r.behaviour == TempoBehaviour::TimeStretch, "tempo steps, pitch does not: time-stretch", behaviourName (r.behaviour));
        check (r.boundaries.size() == 1 && r.boundaries[0].tempoStep && ! r.boundaries[0].detuneJump, "one tempo boundary suggested", std::to_string (r.boundaries.size()));
        if (r.boundaries.size() == 1)
        {
            check (std::fabs (r.boundaries[0].time - 50.0) <= 1.5, "placed at 50 s to within 1.5 s", num ("%.2f s", r.boundaries[0].time));
            check (std::fabs (r.boundaries[0].bpmBefore - 120.0) <= 1.5 && std::fabs (r.boundaries[0].bpmAfter - 140.0) <= 1.5, "120 -> 140 BPM", num ("%.1f -> %.1f", r.boundaries[0].bpmBefore, r.boundaries[0].bpmAfter));
        }
        check (r.regions.size() == 2 && std::fabs (r.regions[0].bpm - 120.0) <= 1.0 && std::fabs (r.regions[1].bpm - 140.0) <= 1.0, "regions at 120 and 140 BPM", r.regions.size() == 2 ? num ("%.2f, %.2f", r.regions[0].bpm, r.regions[1].bpm) : "");
    }

    // a beat switch: C major 120 BPM +10 cents, then E minor 140 BPM +30 cents (a step that is NOT whole semitones of tempo change, so no varispeed look-alike)
    {
        ChordSongSpec sa; sa.seconds = 50.0; sa.tonic = 0; sa.minor = false; sa.seed = 5;
        ChordSongSpec sb; sb.seconds = 50.0; sb.tonic = 4; sb.minor = true; sb.seed = 6;
        const auto x = concat (renderChordSong (sa, constantPlan (120.0, 10.0), SR), renderChordSong (sb, constantPlan (140.0, 30.0), SR));
        const auto r = analyzeTempoAutomation (x.data(), x.size(), SR);
        std::printf ("  beat switch: %s\n", r.summary.c_str());
        printBoundaries (r);
        check (r.valid && r.behaviour == TempoBehaviour::Mixed, "everything changes at once: mixed", behaviourName (r.behaviour));
        check (r.boundaries.size() == 1 && r.boundaries[0].detuneJump && r.boundaries[0].tempoStep, "one boundary, flagged as detune jump AND tempo step", std::to_string (r.boundaries.size()));
        if (r.boundaries.size() == 1) check (std::fabs (r.boundaries[0].time - 50.0) <= 0.5, "placed at 50 s to within 0.5 s", num ("%.2f s", r.boundaries[0].time));
        check (r.regions.size() == 2, "two regions");
        if (r.regions.size() == 2)
        {
            check (std::fabs (r.regions[0].cents - 10.0) <= 0.3 && std::fabs (r.regions[1].cents - 30.0) <= 0.3, "region detunes +10 and +30", num ("%+.2f, %+.2f", r.regions[0].cents, r.regions[1].cents));
            check (keyIs (r.regions[0].key, 0, false) && keyIs (r.regions[1].key, 4, true), "region keys C major and E minor (or relatives)", keyLabel (r.regions[0].key) + " / " + keyLabel (r.regions[1].key));
        }
    }

    // the user gives the boundary: regions follow it instead of the suggestions
    {
        ChordSongSpec sa; sa.seconds = 50.0; sa.tonic = 0; sa.minor = false; sa.seed = 5;
        ChordSongSpec sb; sb.seconds = 50.0; sb.tonic = 4; sb.minor = true; sb.seed = 6;
        const auto x = concat (renderChordSong (sa, constantPlan (120.0, 10.0), SR), renderChordSong (sb, constantPlan (140.0, 30.0), SR));
        AutomationOptions o;
        o.boundaries = { 25.0, 50.0 };
        const auto r = analyzeTempoAutomation (x.data(), x.size(), SR, o);
        check (r.regions.size() == 3 && std::fabs (r.regions[0].end - 25.0) < 1e-9 && std::fabs (r.regions[1].end - 50.0) < 1e-9, "user boundaries define the regions", num ("%.0f regions", (double) r.regions.size()));
        if (r.regions.size() == 3) check (std::fabs (r.regions[0].cents - 10.0) <= 0.3 && std::fabs (r.regions[1].cents - 10.0) <= 0.3 && std::fabs (r.regions[2].cents - 30.0) <= 0.3, "their detunes +10, +10, +30");
    }

    // manual tempo map: the user types the tempo points of a varispeed (tempo is NOT measured from the audio)
    {
        const auto plan = rampPlan (120.0, 140.0, 20.0, 80.0, true, 10.0, 120.0);
        TempoMap map;
        map.add (0.0, 120.0); map.add (20.0, 120.0); map.add (80.0, 140.0); map.add (100.0, 140.0);
        AutomationOptions o;
        o.tempoMap = &map;
        const auto r = automation (plan, 100.0, 9, true, o);
        std::printf ("  manual tempo map: %s\n", r.summary.c_str());
        check (r.valid && r.tempoFromMap && r.behaviour == TempoBehaviour::Varispeed, "manual points: varispeed, tempo taken from the map", behaviourName (r.behaviour));
        check (std::fabs (r.referenceBpm - 119.31) <= 0.5, "reference tempo 119.3 from the map", num ("%.2f", r.referenceBpm));
        // wrong manual points (the user says the tempo is constant): the detune moves, so it is a pitch movement
        TempoMap flat = TempoMap::constant (120.0);
        o.tempoMap = &flat;
        const auto r2 = automation (plan, 100.0, 9, true, o);
        check (r2.valid && r2.behaviour == TempoBehaviour::PitchMoves, "a constant tempo map against a moving pitch: the pitch moves", behaviourName (r2.behaviour));
    }

    // a reference tempo the user knows
    {
        const auto plan = rampPlan (120.0, 140.0, 20.0, 80.0, true, 10.0, 120.0);
        AutomationOptions o;
        o.referenceBpm = 130.0;
        const auto r = automation (plan, 100.0, 9, true, o);
        const double want = wrapCents (10.0 + 1200.0 * std::log2 (130.0 / 120.0));
        check (r.behaviour == TempoBehaviour::Varispeed && std::fabs (wrapCents (r.detuneAtReference - want)) <= 3.0, "detune at a given reference tempo", num ("%+.1f (want %+.1f)", r.detuneAtReference, want));
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------
static void progressTests()
{
    std::printf ("[progress and cancel]\n");
    using namespace centsible;
    ChordSongSpec s; s.seconds = 60.0; s.tonic = 7; s.minor = false;
    const auto x = renderChordSong (s, constantPlan (124.0, -9.0), SR);
    {
        double last = -1.0; bool monotone = true, inRange = true; int calls = 0;
        const auto r = analyzeTempoAutomation (x.data(), x.size(), SR, {}, [&] (double f, const char*) { ++calls; monotone = monotone && f >= last - 1e-9; inRange = inRange && f >= -1e-9 && f <= 1.0 + 1e-9; last = f; return true; });
        check (r.valid && ! r.cancelled && calls >= 5 && monotone && inRange && last >= 0.99, "automation progress goes 0 -> 1 monotonically", num ("%.0f calls, last %.2f", (double) calls, last));
    }
    {
        int calls = 0;
        const auto r = analyzeTempoAutomation (x.data(), x.size(), SR, {}, [&] (double f, const char*) { ++calls; return f < 0.2; });
        check (r.cancelled && ! r.valid, "automation stops when the callback says no", num ("%.0f calls", (double) calls));
    }
    {
        double last = -1.0; bool monotone = true;
        const auto r = analyzeSections (x.data(), x.size(), SR, { 30.0 }, {}, [&] (double f, const char*) { monotone = monotone && f >= last - 1e-9; last = f; return true; });
        check (r.sections.size() == 2 && ! r.cancelled && monotone && last >= 0.99, "sections progress goes 0 -> 1 monotonically", num ("last %.2f", last));
    }
    {
        const auto r = analyzeSections (x.data(), x.size(), SR, { 30.0 }, {}, [&] (double f, const char*) { return f < 0.3; });
        check (r.cancelled, "sections stop when the callback says no");
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------
static bool allFinite (const centsible::TempoAutomationResult& r)
{
    for (const auto& c : r.curve)
        if (! std::isfinite (c.time) || ! std::isfinite (c.cents) || ! std::isfinite (c.ci) || ! std::isfinite (c.unwrapped) || ! std::isfinite (c.bpm) || ! std::isfinite (c.tempoCents)) return false;
    for (const auto& g : r.regions)
        if (! std::isfinite (g.cents) || ! std::isfinite (g.ci) || ! std::isfinite (g.bpm)) return false;
    return std::isfinite (r.slope) && std::isfinite (r.medianBpm) && std::isfinite (r.centsStart) && std::isfinite (r.centsEnd);
}

static void edgeTests()
{
    std::printf ("[edge cases]\n");
    using namespace centsible;
    {
        check (! parseTimestamp ("-5").ok && ! parseTimestamp ("1:23.5.6").ok && ! parseTimestamp ("1::5").ok && ! parseTimestamp ("1:x5").ok, "malformed times are refused");
        const auto r = parseTimestamp ("  1:23 ");
        check (r.ok && std::fabs (r.seconds - 83.0) < 1e-9, "surrounding spaces are ignored");
    }
    {
        std::vector<float> none;
        const auto r = analyzeSections (none.data(), 0, SR, { 5.0 });
        check (r.sections.size() == 1 && ! r.sections[0].ok && ! r.cancelled, "an empty buffer gives one section that is reported as too short");
    }
    {
        std::vector<float> shortAudio ((size_t) (8.0 * SR), 0.0f);
        const auto r = analyzeTempoAutomation (shortAudio.data(), shortAudio.size(), SR);
        check (! r.valid && ! r.summary.empty(), "8 s of audio is refused with a sentence", r.summary);
    }
    {
        std::vector<float> silence ((size_t) (40.0 * SR), 0.0f);
        const auto r = analyzeTempoAutomation (silence.data(), silence.size(), SR);
        check (! r.valid && r.behaviour == TempoBehaviour::Unknown && allFinite (r), "digital silence: no verdict, no crash", r.summary);
        const auto s = analyzeSections (silence.data(), silence.size(), SR, { 20.0 });
        check (s.sections.size() == 2 && s.sections[0].ok && ! s.sections[0].analysis.tuning.valid, "silence in sections: analysed, no pitched content reported", s.sections[0].note);
    }
    {
        std::mt19937 rng (7);
        std::normal_distribution<float> g (0.0f, 0.1f);
        std::vector<float> noise ((size_t) (40.0 * SR));
        for (auto& v : noise) v = g (rng);
        const auto r = analyzeTempoAutomation (noise.data(), noise.size(), SR);
        check (allFinite (r) && ! r.valid, "white noise: nothing is claimed, every number stays finite", r.valid ? behaviourName (r.behaviour) : "not valid");
    }
    {
        // other host sample rates: a 48 kHz varispeed ramp and a 96 kHz steady song
        ChordSongSpec s; s.seconds = 70.0; s.tonic = 2; s.minor = true; s.seed = 11;
        const auto plan = rampPlan (100.0, 115.0, 15.0, 55.0, true, -12.0, 100.0);
        const auto x = renderChordSong (s, plan, 48000.0);
        const auto r = analyzeTempoAutomation (x.data(), x.size(), 48000.0);
        check (r.valid && r.behaviour == TempoBehaviour::Varispeed && std::fabs (r.slope - 1.0) <= 0.15, "48 kHz varispeed 100 -> 115 BPM", r.valid ? num ("slope %.2f", r.slope) + " " + behaviourName (r.behaviour) : std::string ("not valid"));
        ChordSongSpec s2; s2.seconds = 60.0; s2.tonic = 7; s2.minor = false; s2.seed = 12;
        const auto y = renderChordSong (s2, constantPlan (126.0, 31.5), 96000.0);
        const auto r2 = analyzeTempoAutomation (y.data(), y.size(), 96000.0);
        check (r2.valid && r2.behaviour == TempoBehaviour::Constant && ! r2.regions.empty() && std::fabs (r2.regions[0].cents - 31.5) <= 0.2, "96 kHz steady song: constant, +31.5 cents", r2.regions.empty() ? "" : num ("%+.3f", r2.regions[0].cents));
        const auto sec = analyzeSections (y.data(), y.size(), 96000.0, { 30.0 });
        check (sec.sections.size() == 2 && sec.sections[1].ok && std::fabs (sec.sections[1].analysis.tuning.cents - 31.5) <= 0.2, "96 kHz sections", num ("%+.3f", sec.sections[1].analysis.tuning.cents));
    }
}

int main (int argc, char** argv)
{
    const bool quick = argc > 1 && ! std::strcmp (argv[1], "quick");
    timestampTests();
    keyTextTests();
    sectionTests();
    if (! quick)
    {
        automationTests();
        progressTests();
        edgeTests();
    }
    std::printf ("=== beatswitch: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
