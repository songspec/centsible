// Key research tool: decodes audio files and caches per-frame chroma features for offline experiments (tools/key_eval.py).
//   key_frames <out dir> <tuning csv: file,cents> <shift cents> <audio file>...
// Writes <out dir>/<name>.f32: float32 matrix [frames][kCols]. Columns:
//   0-11  peaks, all registers      12-23 peaks, bass (80-250 Hz)     24-35 peaks, mid (250-2000 Hz)
//   36-47 harmonic salience         48-59 peaks, sustain-gated        60-71 salience, sustain-gated
//   72 frame level (max magnitude)  73 rms (time domain)              74 frame centre time (s)
//   75-86 sub-bass peaks 28-80 Hz   87-98 bass peaks 28-250 Hz        99-110 iterative harmonic cancellation (all notes)
//   111-122 cancellation, notes below 250 Hz only                      123-134 cancellation, sustain-gated peaks
// `shift cents` resamples the audio by that many cents first (transposition / tuning invariance tests); the tuning from the csv is shifted along.
#include "fft.h"
#include "gen/audio_io.h"
#include "gen/resample.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
constexpr int kCols = 135;

static double wrapC (double c) { return c - 100.0 * std::floor ((c + 50.0) / 100.0); }

static std::vector<float> frames (const std::vector<float>& x, double sr, double tuningCents)
{
    int order = 4;
    while ((double) (1 << order) < 0.34 * sr) ++order;
    const int N = 1 << order, hop = N / 2;
    const double binHz = sr / N, refHz = 440.0 * std::pow (2.0, tuningCents / 1200.0);
    Fft fft (order);
    std::vector<float> window ((size_t) N);
    for (int i = 0; i < N; ++i) window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * i / N));
    const int kLo = (int) std::ceil (28.0 / binHz), kHi = std::min (N / 2 - 3, (int) (5000.0 / binHz));
    std::vector<std::complex<float>> buf ((size_t) N);
    std::vector<std::vector<float>> mags;      // all frames' magnitudes (we need neighbours for the sustain gate)
    std::vector<float> rms;
    for (size_t pos = 0; pos + (size_t) N <= x.size(); pos += (size_t) hop)
    {
        double e = 0;
        for (int i = 0; i < N; ++i) { const float v = x[pos + (size_t) i]; e += (double) v * v; buf[(size_t) i] = { v * window[(size_t) i], 0.0f }; }
        rms.push_back ((float) std::sqrt (e / N));
        fft.forward (buf.data());
        std::vector<float> m ((size_t) N / 2 + 1);
        for (int k = 0; k <= N / 2; ++k) m[(size_t) k] = std::abs (buf[(size_t) k]);
        mags.push_back (std::move (m));
    }
    const int F = (int) mags.size();
    std::vector<float> out ((size_t) F * kCols, 0.0f);
    for (int f = 0; f < F; ++f)
    {
        const auto& m = mags[(size_t) f];
        const auto* mp = &mags[(size_t) std::max (0, f - 1)];
        const auto* mn = &mags[(size_t) std::min (F - 1, f + 1)];
        double peak = 0;
        for (int k = kLo; k <= kHi; ++k) peak = std::max (peak, (double) m[(size_t) k]);
        float* o = &out[(size_t) f * kCols];
        o[72] = (float) peak; o[73] = rms[(size_t) f]; o[74] = (float) ((f * hop + N / 2) / sr);
        if (peak <= 0) continue;
        auto sustain = [&] (int k)    // 1 for a partial that is still there in the neighbouring frames, ~0 for a transient
        {
            const double a = std::max ({ (double) (*mp)[(size_t) k - 1], (double) (*mp)[(size_t) k], (double) (*mp)[(size_t) k + 1] });
            const double b = std::max ({ (double) (*mn)[(size_t) k - 1], (double) (*mn)[(size_t) k], (double) (*mn)[(size_t) k + 1] });
            return std::clamp (std::min (a, b) / std::max (1e-20, (double) m[(size_t) k]), 0.0, 1.0);
        };
        auto addPc = [&] (float* dst, double hz, double w)
        {
            const double s = 12.0 * std::log2 (hz / refHz) + 9.0, nearest = std::round (s), frac = s - nearest;
            const double tri = std::max (0.0, 1.0 - 2.0 * std::fabs (frac));
            dst[((int) nearest % 12 + 12) % 12] += (float) (w * tri);
        };
        for (int k = kLo; k <= kHi; ++k)
        {
            const double mk = m[(size_t) k];
            if (mk < 0.03 * peak || mk < m[(size_t) k - 1] || mk < m[(size_t) k + 1]) continue;
            const double a = std::log (m[(size_t) k - 1] + 1e-12), b = std::log (mk + 1e-12), c = std::log ((double) m[(size_t) k + 1] + 1e-12);
            const double den = a - 2 * b + c, d = den < 0 ? std::clamp (0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
            const double hz = (k + d) * binHz, w = std::log1p (30.0 * mk / peak), g = w * sustain (k);
            if (hz < 80.0) continue;
            addPc (o, hz, w);
            addPc (o + 48, hz, g);
            if (hz < 250.0) addPc (o + 12, hz, w); else if (hz < 2000.0) addPc (o + 24, hz, w);
        }
        // peaks again, with amplitude, for the sub-bass register and for iterative harmonic cancellation
        struct Pk { double hz, amp, gamp; };
        std::vector<Pk> pks;
        for (int k = kLo; k <= kHi; ++k)
        {
            const double mk = m[(size_t) k];
            if (mk < 0.02 * peak || mk < m[(size_t) k - 1] || mk < m[(size_t) k + 1]) continue;
            const double a = std::log (m[(size_t) k - 1] + 1e-12), b = std::log (mk + 1e-12), c = std::log ((double) m[(size_t) k + 1] + 1e-12);
            const double den = a - 2 * b + c, d = den < 0 ? std::clamp (0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
            const double hz = (k + d) * binHz, w = std::log1p (30.0 * mk / peak);
            if (hz < 80.0) addPc (o + 75, hz, w);
            if (hz < 250.0) addPc (o + 87, hz, w);
            pks.push_back ({ hz, mk / peak, mk / peak * sustain (k) });
        }
        for (int pass = 0; pass < 2; ++pass)
        {
            std::vector<double> r (pks.size());
            for (size_t j = 0; j < pks.size(); ++j) r[j] = pass == 0 ? pks[j].amp : pks[j].gamp;
            auto at = [&] (double hz)       // strongest residual peak within +-40 cents of hz
            {
                double best = 0; int bj = -1;
                for (size_t j = 0; j < pks.size(); ++j)
                    if (std::fabs (1200.0 * std::log2 (pks[j].hz / hz)) < 40.0 && r[j] > best) { best = r[j]; bj = (int) j; }
                return std::make_pair (best, bj);
            };
            double first = 0;
            for (int note = 0; note < 8; ++note)
            {
                int bp = -1; double bs = 0;
                for (int p = 24; p <= 90; ++p)
                {
                    const double fp = refHz * std::pow (2.0, (p - 69) / 12.0);
                    double sc = 0;
                    for (int h = 1; h <= 10; ++h) { const double fh = fp * h; if (fh > 5000.0) break; sc += std::pow (0.88, h - 1) * at (fh).first; }
                    if (sc > bs) { bs = sc; bp = p; }
                }
                if (bp < 0) break;
                if (note == 0) first = bs;
                if (bs < 0.12 * first) break;
                const double fp = refHz * std::pow (2.0, (bp - 69) / 12.0);
                double wsum = 0;
                for (int h = 1; h <= 10; ++h) { if (fp * h > 5000.0) break; wsum += std::pow (0.88, h - 1); }
                const double gain = bs / wsum;
                for (int h = 1; h <= 10; ++h)
                {
                    const double fh = fp * h;
                    if (fh > 5000.0) break;
                    const auto [amp, j] = at (fh);
                    if (j >= 0) r[(size_t) j] = std::max (0.0, r[(size_t) j] - gain * std::pow (0.88, h - 1));
                    (void) amp;
                }
                const double act = std::log1p (30.0 * bs / first) / std::log1p (30.0);
                if (pass == 0)
                {
                    o[99 + bp % 12] += (float) act;
                    if (fp < 250.0) o[111 + bp % 12] += (float) act;
                }
                else
                    o[123 + bp % 12] += (float) act;
            }
        }
        // harmonic salience: every note C2..C6 collects its first 8 harmonics (weights 0.8^(h-1)) from the magnitude spectrum
        for (int p = 36; p <= 84; ++p)
        {
            const double fp = refHz * std::pow (2.0, (p - 69) / 12.0);
            double s = 0, sg = 0;
            for (int h = 1; h <= 8; ++h)
            {
                const double fh = fp * h;
                if (fh > 5000.0 || fh > 0.45 * sr) break;
                const double kc = fh / binHz, half = std::max (1.0, 0.0204 * kc);
                int kb = -1; double mb = 0;
                for (int k = (int) std::floor (kc - half); k <= (int) std::ceil (kc + half); ++k)
                    if (k >= 2 && k < N / 2 - 2 && m[(size_t) k] > mb) { mb = m[(size_t) k]; kb = k; }
                if (kb < 0) continue;
                const double v = std::log1p (30.0 * mb / peak) * std::pow (0.8, h - 1);
                s += v; sg += v * sustain (kb);
            }
            o[36 + p % 12] += (float) s;
            o[60 + p % 12] += (float) sg;
        }
    }
    return out;
}

int main (int argc, char** argv)
{
    if (argc < 5) { std::printf ("usage: key_frames <out dir> <tuning csv> <shift cents> <audio dir>\n"); return 2; }
    const fs::path out = argv[1];
    fs::create_directories (out);
    std::map<std::string, double> tuning;
    {
        std::ifstream t (argv[2]);
        std::string line;
        std::getline (t, line);
        while (std::getline (t, line))
        {
            // "file",seconds,valid,cents,...
            const auto q = line.find ("\",");
            if (line.empty() || line[0] != '"' || q == std::string::npos) continue;
            std::stringstream ss (line.substr (q + 2));
            std::string a, b, c;
            std::getline (ss, a, ','); std::getline (ss, b, ','); std::getline (ss, c, ',');
            tuning[line.substr (1, q - 1)] = std::atof (c.c_str());
        }
    }
    const double shift = std::atof (argv[3]);
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator (fs::u8path (argv[4])))
    {
        const auto ext = e.path().extension().string();
        if (ext == ".flac" || ext == ".mp3" || ext == ".wav") files.push_back (e.path());
    }
    std::atomic<size_t> next { 0 };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::max (2u, std::thread::hardware_concurrency()); ++t)
        pool.emplace_back ([&]
        {
            for (size_t i; (i = next++) < files.size();)
            {
                const auto u8 = files[i].filename().u8string();
                const std::string name (u8.begin(), u8.end());          // UTF-8, as written by the CLI
                if (! tuning.count (name)) { std::printf ("no tuning for %s\n", name.c_str()); continue; }
                double sr = 0;
                auto x = loadMono (juce::File (juce::String (files[i].wstring().c_str())), sr, 300.0);
                if (shift != 0.0) x = resample (x, std::pow (2.0, shift / 1200.0));
                const auto f = frames (x, sr, wrapC (tuning[name] + shift));
                std::ofstream o (out / fs::u8path (name + ".f32"), std::ios::binary);
                o.write ((const char*) f.data(), (std::streamsize) (f.size() * sizeof (float)));
            }
        });
    for (auto& t : pool) t.join();
    return 0;
}
