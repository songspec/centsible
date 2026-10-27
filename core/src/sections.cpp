// Beatswitch: sections cut at timestamps, and tempo automation (detune over time against tempo over time). See centsible/sections.h.
// Offline code; allocates freely and is never called from the audio thread.
#include <centsible/sections.h>
#include "detune_timeline.h"
#include "tempo_detector.h"
#include "stats_util.h"
#include "track_internal.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numeric>

namespace centsible
{
namespace
{
using namespace detail::util;

std::string trimmed (const std::string& s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace ((unsigned char) s[a])) ++a;
    while (b > a && std::isspace ((unsigned char) s[b - 1])) --b;
    return s.substr (a, b - a);
}

// strict decimal number: digits with at most one '.', no sign, no exponent
bool parseDecimal (const std::string& s, double& v)
{
    if (s.empty()) return false;
    int dots = 0, digits = 0;
    for (char c : s)
    {
        if (c == '.') ++dots;
        else if (std::isdigit ((unsigned char) c)) ++digits;
        else return false;
    }
    if (dots > 1 || digits == 0) return false;
    v = std::strtod (s.c_str(), nullptr);
    return true;
}

bool isInteger (const std::string& s) { return ! s.empty() && std::all_of (s.begin(), s.end(), [] (char c) { return std::isdigit ((unsigned char) c) != 0; }); }
} // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------
// tempo map

TempoMap TempoMap::constant (double bpm)
{
    TempoMap m;
    m.add (0.0, bpm);
    return m;
}

void TempoMap::add (double timeSeconds, double bpm)
{
    if (! (bpm > 0.0) || ! std::isfinite (timeSeconds) || ! std::isfinite (bpm)) return;
    for (auto& p : pts)
        if (std::fabs (p.time - timeSeconds) < 1e-9) { p.bpm = bpm; return; }
    pts.push_back ({ timeSeconds, bpm });
    std::sort (pts.begin(), pts.end(), [] (const TempoPoint& a, const TempoPoint& b) { return a.time < b.time; });
}

double TempoMap::bpmAt (double t) const
{
    if (pts.empty()) return 0.0;
    if (t <= pts.front().time) return pts.front().bpm;
    if (t >= pts.back().time) return pts.back().bpm;
    const auto it = std::upper_bound (pts.begin(), pts.end(), t, [] (double v, const TempoPoint& p) { return v < p.time; });
    const auto& b = *it;
    const auto& a = *(it - 1);
    return linear ? a.bpm + (b.bpm - a.bpm) * (t - a.time) / (b.time - a.time) : a.bpm;
}

namespace
{
// beats from the first point's time to t (negative before it)
double beatsFromFirst (const std::vector<TempoPoint>& pts, bool linear, double t)
{
    if (pts.empty()) return 0.0;
    const double t0 = pts.front().time;
    if (t <= t0) return pts.front().bpm * (t - t0) / 60.0;
    double beats = 0.0;
    for (std::size_t i = 0; i + 1 < pts.size(); ++i)
    {
        const double a = pts[i].time, b = pts[i + 1].time;
        if (t <= a) break;
        const double cover = std::min (t, b) - a;
        if (linear)
        {
            const double be = pts[i].bpm + (pts[i + 1].bpm - pts[i].bpm) * cover / (b - a);
            beats += 0.5 * (pts[i].bpm + be) * cover / 60.0;
        }
        else
            beats += pts[i].bpm * cover / 60.0;
    }
    if (t > pts.back().time) beats += pts.back().bpm * (t - pts.back().time) / 60.0;
    return beats;
}
} // namespace

double TempoMap::beatsAt (double t) const
{
    return beatsFromFirst (pts, linear, t) - beatsFromFirst (pts, linear, 0.0);
}

double TempoMap::timeOfBeat (double beats) const
{
    if (pts.empty()) return 0.0;
    double lo = 0.0, hi = 1.0;
    double minBpm = pts.front().bpm;
    for (const auto& p : pts) minBpm = std::min (minBpm, p.bpm);
    if (beats < 0.0) { hi = 0.0; lo = beats * 60.0 / minBpm - 1.0; while (beatsAt (lo) > beats) lo *= 2.0; }
    else { hi = beats * 60.0 / minBpm + 1.0; while (beatsAt (hi) < beats) hi *= 2.0; }
    for (int it = 0; it < 80; ++it)
    {
        const double mid = 0.5 * (lo + hi);
        if (beatsAt (mid) < beats) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// timestamps

namespace
{
double beatsToSeconds (double beats, const TimeContext& ctx)
{
    if (ctx.tempoMap && ctx.tempoMap->valid())
        return ctx.tempoMap->timeOfBeat (ctx.tempoMap->beatsAt (ctx.firstBarSeconds) + beats);
    return ctx.firstBarSeconds + beats * 60.0 / ctx.bpm;
}

double secondsToBeats (double seconds, const TimeContext& ctx)
{
    if (ctx.tempoMap && ctx.tempoMap->valid())
        return ctx.tempoMap->beatsAt (seconds) - ctx.tempoMap->beatsAt (ctx.firstBarSeconds);
    return (seconds - ctx.firstBarSeconds) * ctx.bpm / 60.0;
}

ParsedTime fail (const std::string& text, const std::string& why)
{
    ParsedTime r;
    r.error = "'" + text + "': " + why;
    return r;
}
} // namespace

ParsedTime parseTimestamp (const std::string& textIn, const TimeContext& ctx)
{
    const std::string text = trimmed (textIn);
    if (text.empty()) return fail (textIn, "empty");
    std::string low = text;
    for (auto& c : low) c = (char) std::tolower ((unsigned char) c);
    ParsedTime r;

    // bar | beat
    const bool hasBar = low.find ('|') != std::string::npos;
    const bool barWord = low.rfind ("bar", 0) == 0 || (low.size() > 1 && low[0] == 'b' && std::isdigit ((unsigned char) low[1]));
    if (hasBar || barWord)
    {
        if (! ((ctx.tempoMap && ctx.tempoMap->valid()) || ctx.bpm > 0.0)) return fail (text, "a bar | beat position needs a tempo (enter the tempo or the tempo points first)");
        std::string barS, beatS = "1";
        if (hasBar)
        {
            const auto bar = low.find ('|');
            barS = trimmed (low.substr (0, bar));
            beatS = trimmed (low.substr (bar + 1));
        }
        else
        {
            barS = trimmed (low.substr (low.rfind ("bar", 0) == 0 ? 3 : 1));
        }
        double bar = 0.0, beat = 1.0;
        if (! parseDecimal (barS, bar) || bar < 1.0 || bar != std::floor (bar)) return fail (text, "the bar must be a whole number from 1 (as in 17|1)");
        if (! parseDecimal (beatS, beat) || beat < 1.0) return fail (text, "the beat must be a number from 1 (as in 17|2.5)");
        const int bpb = std::max (1, ctx.beatsPerBar);
        if (beat >= (double) bpb + 1.0) return fail (text, "beat " + fmt ("%.4g", beat) + " does not exist in a bar of " + std::to_string (bpb) + " beats");
        r.seconds = beatsToSeconds (((bar - 1.0) * bpb + (beat - 1.0)) * std::max (1e-3, ctx.quartersPerBeat), ctx);
        r.ok = std::isfinite (r.seconds);
        if (r.seconds < -1e-6) { r.ok = false; r.error = "'" + text + "': before the start of the audio"; }
        return r;
    }

    // clock: m:ss.ms or h:mm:ss.ms
    if (low.find (':') != std::string::npos)
    {
        std::vector<std::string> parts;
        std::size_t a = 0;
        for (;;)
        {
            const auto c = low.find (':', a);
            parts.push_back (trimmed (low.substr (a, c == std::string::npos ? std::string::npos : c - a)));
            if (c == std::string::npos) break;
            a = c + 1;
        }
        if (parts.size() < 2 || parts.size() > 3) return fail (text, "use minutes:seconds, like 1:23.5 (or hours:minutes:seconds)");
        for (std::size_t i = 0; i + 1 < parts.size(); ++i)
            if (! isInteger (parts[i])) return fail (text, "minutes and hours must be whole numbers, like 1:23.5");
        double sec = 0.0;
        if (! parseDecimal (parts.back(), sec)) return fail (text, "cannot read the seconds, use something like 1:23.5");
        if (sec >= 60.0) return fail (text, "seconds must be below 60 (write 2:15 for two minutes and fifteen seconds)");
        const double first = std::atof (parts[0].c_str());
        if (parts.size() == 3)
        {
            const double mins = std::atof (parts[1].c_str());
            if (mins >= 60.0) return fail (text, "minutes must be below 60 when hours are given");
            r.seconds = first * 3600.0 + mins * 60.0 + sec;
        }
        else
            r.seconds = first * 60.0 + sec;
        r.ok = true;
        return r;
    }

    // plain seconds
    std::string num = low;
    if (! num.empty() && num.back() == 's') num.pop_back();
    double v = 0.0;
    if (! parseDecimal (trimmed (num), v)) return fail (text, "cannot read this as a time (use 1:23.5, 83.5 or, with a tempo, 17|1)");
    r.seconds = v;
    r.ok = true;
    return r;
}

ParsedTimes parseTimestampList (const std::string& text, const TimeContext& ctx)
{
    ParsedTimes out;
    std::vector<std::string> pieces;
    std::string cur;
    auto flush = [&] { const auto t = trimmed (cur); if (! t.empty()) pieces.push_back (t); cur.clear(); };
    for (char c : text)
    {
        if (c == ',' || c == ';' || c == '\n' || c == '\r') flush();
        else cur += c;
    }
    flush();
    // a piece with spaces is several times ("1:00 2:30"), except "bar 17"
    std::vector<std::string> tokens;
    for (const auto& p : pieces)
    {
        std::string low = p;
        for (auto& c : low) c = (char) std::tolower ((unsigned char) c);
        if (p.find (' ') == std::string::npos || low.rfind ("bar", 0) == 0) { tokens.push_back (p); continue; }
        std::string t;
        for (char c : p + " ")
        {
            if (std::isspace ((unsigned char) c)) { if (! t.empty()) tokens.push_back (t); t.clear(); }
            else t += c;
        }
    }
    for (const auto& tk : tokens)
    {
        const auto r = parseTimestamp (tk, ctx);
        if (! r.ok) { out.ok = false; out.error = r.error; return out; }
        out.seconds.push_back (r.seconds);
    }
    std::sort (out.seconds.begin(), out.seconds.end());
    out.seconds.erase (std::unique (out.seconds.begin(), out.seconds.end(), [] (double a, double b) { return std::fabs (a - b) < 1e-3; }), out.seconds.end());
    return out;
}

ParsedTempo parseTempoText (const std::string& text, bool linear)
{
    ParsedTempo out;
    out.map.linear = linear;
    std::vector<std::string> pieces;
    std::string cur;
    for (char c : text + ",")
    {
        if (c == ',' || c == ';' || c == '\n' || c == '\r') { const auto t = trimmed (cur); if (! t.empty()) pieces.push_back (t); cur.clear(); }
        else cur += c;
    }
    if (pieces.empty()) { out.error = "no tempo given"; return out; }

    auto bpmOf = [] (std::string s, double& bpm)
    {
        s = trimmed (s);
        std::string low = s;
        for (auto& c : low) c = (char) std::tolower ((unsigned char) c);
        if (low.size() >= 3 && low.compare (low.size() - 3, 3, "bpm") == 0) low = trimmed (low.substr (0, low.size() - 3));
        return parseDecimal (low, bpm) && bpm > 20.0 && bpm < 400.0;
    };

    if (pieces.size() == 1 && pieces[0].find ('=') == std::string::npos)
    {
        double bpm = 0.0;
        if (! bpmOf (pieces[0], bpm)) { out.error = "'" + pieces[0] + "': the tempo must be a number of BPM between 20 and 400 (or points like 0=120, 1:20=140)"; return out; }
        out.map.add (0.0, bpm);
        out.ok = true;
        return out;
    }
    for (const auto& p : pieces)
    {
        const auto eq = p.find ('=');
        if (eq == std::string::npos) { out.error = "'" + p + "': a tempo point is written time=bpm, like 1:20=140"; return out; }
        const auto t = parseTimestamp (p.substr (0, eq));
        if (! t.ok) { out.error = t.error; return out; }
        double bpm = 0.0;
        if (! bpmOf (p.substr (eq + 1), bpm)) { out.error = "'" + p + "': the tempo must be between 20 and 400 BPM"; return out; }
        out.map.add (t.seconds, bpm);
    }
    out.ok = out.map.valid();
    return out;
}

std::string formatTimestamp (double seconds)
{
    if (! std::isfinite (seconds)) return "";
    const bool neg = seconds < 0.0;
    long long ms = std::llround (std::fabs (seconds) * 1000.0);
    const long long h = ms / 3600000, m = (ms / 60000) % 60, s = (ms / 1000) % 60, f = ms % 1000;
    char buf[64];
    if (h > 0) std::snprintf (buf, sizeof buf, "%s%lld:%02lld:%02lld.%03lld", neg ? "-" : "", h, m, s, f);
    else std::snprintf (buf, sizeof buf, "%s%lld:%02lld.%03lld", neg ? "-" : "", m, s, f);
    return buf;
}

std::string formatBarBeat (double seconds, const TimeContext& ctx)
{
    if (! ((ctx.tempoMap && ctx.tempoMap->valid()) || ctx.bpm > 0.0)) return "";
    const int bpb = std::max (1, ctx.beatsPerBar);
    const double beats = secondsToBeats (seconds, ctx) / std::max (1e-3, ctx.quartersPerBeat);
    const double bar = std::floor (beats / bpb + 1e-9);
    double beat = beats - bar * bpb + 1.0;
    beat = std::round (beat * 100.0) / 100.0;
    int barNo = (int) bar + 1;
    if (beat >= bpb + 1.0) { beat -= bpb; ++barNo; }
    char buf[64];
    std::snprintf (buf, sizeof buf, "%d|%g", barNo, beat);
    return buf;
}

std::string sectionLabel (int index)
{
    std::string s;
    for (int i = index; i >= 0; i = i / 26 - 1) s.insert (s.begin(), (char) ('A' + i % 26));
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// sections

SectionDelta compareSections (const SectionResult& a, const SectionResult& b)
{
    SectionDelta d;
    d.from = a.index;
    d.to = b.index;
    const auto& ta = a.analysis.tuning;
    const auto& tb = b.analysis.tuning;
    if (a.ok && b.ok && ta.valid && tb.valid)
    {
        d.tuningKnown = true;
        d.cents = wrapCents ((double) tb.cents - (double) ta.cents);
        d.ci = std::hypot ((double) ta.ciCents, (double) tb.ciCents);
        d.significant = std::fabs (d.cents) > d.ci;
    }
    const auto& ka = a.analysis.key;
    const auto& kb = b.analysis.key;
    if (a.ok && b.ok && ka.valid && kb.valid)
    {
        d.keysKnown = true;
        int semis = ((kb.scaleRoot - ka.scaleRoot) % 12 + 12) % 12;
        if (semis > 5) semis -= 12;
        d.semitones = semis;
        d.sameScale = semis == 0;
        d.sameKey = ka.root == kb.root && ka.minor == kb.minor;
    }
    d.totalCents = d.semitones * 100.0 + d.cents;
    if (a.ok && b.ok && a.analysis.tempo.valid && b.analysis.tempo.valid && a.analysis.tempo.bpm > 0.0f)
    {
        d.tempoKnown = true;
        d.tempoRatio = (double) b.analysis.tempo.bpm / (double) a.analysis.tempo.bpm;
        d.tempoCents = 1200.0 * std::log2 (d.tempoRatio);
    }
    return d;
}

SectionsAnalysis analyzeSections (const float* mono, std::size_t n, double sr, const std::vector<double>& boundaries, const SectionOptions& opt, const ProgressFn& progress)
{
    SectionsAnalysis out;
    out.seconds = (double) n / sr;
    std::vector<double> cuts;
    for (double b : boundaries)
        if (std::isfinite (b) && b > 0.05 && b < out.seconds - 0.05) cuts.push_back (b);
    std::sort (cuts.begin(), cuts.end());
    cuts.erase (std::unique (cuts.begin(), cuts.end(), [] (double a, double b) { return std::fabs (a - b) < 0.05; }), cuts.end());

    std::vector<double> edges { 0.0 };
    edges.insert (edges.end(), cuts.begin(), cuts.end());
    edges.push_back (out.seconds);
    const int count = (int) edges.size() - 1;
    for (int i = 0; i < count; ++i)
    {
        SectionResult s;
        s.index = i;
        s.label = sectionLabel (i);
        s.start = edges[(size_t) i];
        s.end = edges[(size_t) i + 1];
        s.usedStart = s.start + (i > 0 ? opt.trimSeconds : 0.0);
        s.usedEnd = s.end - (i < count - 1 ? opt.trimSeconds : 0.0);
        out.sections.push_back (std::move (s));
    }

    double totalUsed = 0.0;
    for (const auto& s : out.sections) totalUsed += std::max (0.0, s.usedEnd - s.usedStart);
    double done = 0.0;
    for (auto& s : out.sections)
    {
        const double len = s.usedEnd - s.usedStart;
        if (len < opt.minSeconds)
        {
            s.note = "only " + fmt ("%.1f", std::max (0.0, len)) + " s of audio after trimming the boundaries (needs " + fmt ("%.0f", opt.minSeconds) + " s)";
            continue;
        }
        const std::size_t a = (std::size_t) std::llround (s.usedStart * sr), b = std::min (n, (std::size_t) std::llround (s.usedEnd * sr));
        const double f0 = done / std::max (1e-9, totalUsed), f1 = (done + len) / std::max (1e-9, totalUsed);
        s.analysis = detail::analyzeTrackProgress (mono + a, b - a, sr, progress, f0, f1);
        if (s.analysis.cancelled) { out.cancelled = true; return out; }
        s.ok = true;
        if (! s.analysis.tuning.valid) s.note = "no steady pitched content (noise, drums or a very sparse part)";
        done += len;
    }
    if (progress && ! progress (1.0, "done")) out.cancelled = true;
    for (std::size_t i = 0; i + 1 < out.sections.size(); ++i) out.deltas.push_back (compareSections (out.sections[i], out.sections[i + 1]));
    return out;
}

const char* behaviourName (TempoBehaviour b)
{
    switch (b)
    {
        case TempoBehaviour::Constant: return "constant";
        case TempoBehaviour::Varispeed: return "varispeed";
        case TempoBehaviour::TimeStretch: return "time-stretched";
        case TempoBehaviour::PitchMoves: return "pitch moves";
        case TempoBehaviour::Mixed: return "mixed";
        default: return "unknown";
    }
}
} // namespace centsible
