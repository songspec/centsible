// Command-line front end of the Beatswitch analyses (sections, tempo automation). Included by main.cpp.
//   --sections "0:45, 1:30"      cut the track at these times (m:ss.ms, seconds, or bar|beat with --bpm) and analyse every section on its own
//   --sections auto              cut where the detune jumps or the tempo steps (the suggested boundaries of --automation)
//   --automation                 detune over time against tempo over time: varispeed or time-stretch, reference tempo, suggested boundaries
//       --tempo-points "0=120, 0:20=120, 1:20=140"   tempo points (time=bpm) to use instead of measuring the tempo; --tempo-step makes them steps
//       --reference-bpm <bpm>    the tempo the source ran at, if known
//       --boundaries "0:50"      regions for the report (default: the suggested boundaries)
//       --curve-csv out.csv      the detune / tempo curve, one row per window
//   --bpm <bpm> --beats-per-bar <n> --first-bar <time>   how to read bar|beat times
#pragma once
#include <centsible/sections.h>
#include <cstdio>
#include <fstream>
#include <string>

namespace beatswitch_cli
{
struct Args
{
    std::string sections, boundaries, tempoPoints, curveCsv;
    bool automation = false, tempoStep = false;
    double bpm = 0.0, referenceBpm = 0.0, firstBar = 0.0;
    int beatsPerBar = 4;
    bool wanted() const { return ! sections.empty() || automation; }
};

inline std::string signedStr (double v, int decimals = 1)
{
    char b[64];
    std::snprintf (b, sizeof b, "%+.*f", decimals, v);
    return b;
}

inline void printKeyLine (const centsible::KeyEstimate& k, const char* indent)
{
    if (! k.valid) { std::printf ("%skey: not enough pitched content\n", indent); return; }
    std::printf ("%skey %s, %.0f %%   %s;  %s\n", indent, centsible::keyLabel (k).c_str(), 100.0 * (double) k.probability, centsible::keyAmbiguityText (k).c_str(), centsible::runnerUpText (k).c_str());
}

inline void printSections (const std::string& name, double seconds, const centsible::SectionsAnalysis& r, const centsible::TimeContext& tc)
{
    std::printf ("%s  (%s)\n", name.c_str(), centsible::formatTimestamp (seconds).c_str());
    for (const auto& s : r.sections)
    {
        std::string bars = centsible::formatBarBeat (s.start, tc);
        std::printf ("  section %s  %s - %s%s%s%s   (analysed %s - %s)\n", s.label.c_str(), centsible::formatTimestamp (s.start).c_str(), centsible::formatTimestamp (s.end).c_str(),
                     bars.empty() ? "" : "  [bar ", bars.c_str(), bars.empty() ? "" : "]", centsible::formatTimestamp (s.usedStart).c_str(), centsible::formatTimestamp (s.usedEnd).c_str());
        if (! s.ok) { std::printf ("    not analysed: %s\n", s.note.c_str()); continue; }
        const auto& t = s.analysis.tuning;
        if (t.valid)
        {
            std::printf ("    detune %s +- %.2f cents   (A4 = %.2f Hz)   %s   confidence %.2f\n", signedStr (t.cents, 2).c_str(), (double) t.ciCents, (double) t.refHz, t.locked ? "LOCKED" : "not locked", (double) t.confidence);
            if (t.hasSecondary) std::printf ("    also %s cents (%d %% of the evidence)\n", signedStr (t.secondaryCents).c_str(), (int) std::lround (t.secondaryShare * 100.0f));
            if (t.hasDrift) std::printf ("    drifts: later half %s cents from the earlier half\n", signedStr (t.driftCents).c_str());
        }
        else
            std::printf ("    detune: %s\n", s.note.c_str());
        printKeyLine (s.analysis.key, "    ");
        if (s.analysis.tempo.valid)
            std::printf ("    %.1f BPM%s\n", (double) s.analysis.tempo.bpm, s.analysis.tempo.altBpm > 0.0f ? (" (or " + std::to_string (s.analysis.tempo.altBpm).substr (0, 5) + ")").c_str() : "");
    }
    for (const auto& d : r.deltas)
    {
        std::printf ("  %s -> %s:", centsible::sectionLabel (d.from).c_str(), centsible::sectionLabel (d.to).c_str());
        if (d.tuningKnown) std::printf (" detune %s +- %.2f cents (%s)", signedStr (d.cents, 2).c_str(), d.ci, d.significant ? "significant" : "within the error");
        else std::printf (" detune unknown");
        if (d.keysKnown)
            std::printf (";  %s%s", d.sameKey ? "same key" : d.sameScale ? "same scale (relative key or mode)" : ("scale moves " + signedStr (d.semitones, 0) + " semitones").c_str(),
                         d.sameScale ? "" : (", " + signedStr (d.totalCents, 1) + " cents in total").c_str());
        if (d.tempoKnown) std::printf (";  tempo x%.3f (%s cents)", d.tempoRatio, signedStr (d.tempoCents, 0).c_str());
        std::printf ("\n");
    }
}

inline void printAutomation (const std::string& name, const centsible::TempoAutomationResult& r)
{
    std::printf ("%s\n", name.c_str());
    if (! r.valid) { std::printf ("  %s\n", r.summary.c_str()); return; }
    std::printf ("  verdict: %s%s\n", centsible::behaviourName (r.behaviour), r.tempoFromMap ? "   (tempo taken from the given points)" : "");
    std::printf ("  %s\n", r.summary.c_str());
    std::printf ("  tempo %.1f - %.1f BPM (median %.1f);  detune %s -> %s cents;  slope %.2f;  spread %.1f cents as varispeed vs %.1f cents as constant;  tempo jitter %.0f cents\n", r.minBpm, r.maxBpm,
                r.medianBpm, signedStr (r.centsStart).c_str(), signedStr (r.centsEnd).c_str(), r.slope, r.spreadVarispeed, r.spreadFixed, r.tempoNoise);
    if (r.referenceBpm > 0.0) std::printf ("  reference tempo (0 cents): %.2f BPM\n", r.referenceBpm);
    std::printf ("  regions:\n");
    int i = 0;
    for (const auto& g : r.regions)
    {
        std::printf ("   %2d  %s - %s   ", ++i, centsible::formatTimestamp (g.start).c_str(), centsible::formatTimestamp (g.end).c_str());
        if (! g.ok) { std::printf ("too little pitched content\n"); continue; }
        if (g.pitchMoves) std::printf ("detune %s -> %s cents", signedStr (g.centsStart).c_str(), signedStr (g.centsEnd).c_str());
        else std::printf ("detune %s +- %.2f cents", signedStr (g.cents, 2).c_str(), g.ci);
        if (g.bpm > 0.0) std::printf ("   %.1f BPM%s", g.bpm, g.tempoMoves ? (" (" + std::to_string (g.bpmMin).substr (0, 5) + " - " + std::to_string (g.bpmMax).substr (0, 5) + ")").c_str() : "");
        std::printf ("   %s", centsible::behaviourName (g.behaviour));
        if (g.key.valid) std::printf ("   %s %.0f %%", centsible::keyLabel (g.key).c_str(), 100.0 * (double) g.key.probability);
        std::printf ("\n");
    }
}

inline void writeCurve (const std::string& path, const centsible::TempoAutomationResult& r)
{
    std::ofstream f (path);
    f << "time,detune_valid,cents,ci,unwrapped,mass,tempo_valid,bpm,tempo_cents\n";
    for (const auto& c : r.curve)
    {
        char line[256];
        std::snprintf (line, sizeof line, "%.2f,%d,%.3f,%.3f,%.3f,%.1f,%d,%.3f,%.2f\n", c.time, (int) c.detuneValid, c.cents, c.ci, c.unwrapped, c.mass, (int) c.tempoValid, c.bpm, c.tempoCents);
        f << line;
    }
}

// "0=120, 0:20=120, 1:20=140" -> map. Returns false and an error message on bad input.
inline bool parseTempoPoints (const std::string& text, bool linear, centsible::TempoMap& map, std::string& error)
{
    map = centsible::TempoMap();
    map.linear = linear;
    std::string cur;
    std::vector<std::string> pieces;
    for (char c : text + ",")
    {
        if (c == ',' || c == ';' || c == '\n') { if (! cur.empty()) pieces.push_back (cur); cur.clear(); }
        else cur += c;
    }
    for (const auto& p : pieces)
    {
        const auto eq = p.find ('=');
        if (eq == std::string::npos) { error = "'" + p + "': a tempo point is written time=bpm, like 1:20=140"; return false; }
        const auto t = centsible::parseTimestamp (p.substr (0, eq));
        const double bpm = std::atof (p.c_str() + eq + 1);
        if (! t.ok) { error = t.error; return false; }
        if (! (bpm > 20.0 && bpm < 400.0)) { error = "'" + p + "': the tempo must be between 20 and 400 BPM"; return false; }
        map.add (t.seconds, bpm);
    }
    if (map.empty()) { error = "no tempo points given"; return false; }
    return true;
}
} // namespace beatswitch_cli
