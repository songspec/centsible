// Key research tool: runs the PRODUCT feature extractor (core/src/key_detector.h) on every audio file of a directory and writes the aggregated
// features as CSV, so the stage-2 weights can be trained on exactly what ships (tools/key_train.py).
//   key_dump <out.csv> <tuning csv: file,..,cents> <shift cents> <audio dir> [hpss] [nogate] [beats=N]
// `shift cents` resamples the audio first (transposition / tuning invariance tests); the tuning from the csv is shifted along.
#include "key_detector.h"
#include "tempo_detector.h"
#include "gen/audio_io.h"
#include "gen/resample.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
static double wrapC (double c) { return c - 100.0 * std::floor ((c + 50.0) / 100.0); }

int main (int argc, char** argv)
{
    if (argc < 5) { std::printf ("usage: key_dump <out.csv> <tuning csv> <shift cents> <audio dir>\n"); return 2; }
    std::map<std::string, double> tuning;
    {
        std::ifstream t (argv[2]);
        std::string line;
        std::getline (t, line);
        while (std::getline (t, line))
        {
            const auto q = line.find ("\",");
            if (line.empty() || line[0] != '"' || q == std::string::npos) continue;
            std::stringstream ss (line.substr (q + 2));
            std::string a, b, c;
            std::getline (ss, a, ','); std::getline (ss, b, ','); std::getline (ss, c, ',');
            tuning[line.substr (1, q - 1)] = std::atof (c.c_str());
        }
    }
    const double shift = std::atof (argv[3]);
    centsible::keydetail::KeyFeatureOptions base;
    int beatsPerUnit = 0;
    for (int i = 5; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "hpss") base.hpss = true;
        else if (a == "nogate") base.sustainGate = false;
        else if (a.rfind ("beats=", 0) == 0) beatsPerUnit = std::atoi (a.c_str() + 6);
    }
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator (fs::u8path (argv[4])))
    {
        const auto ext = e.path().extension().string();
        if (ext == ".flac" || ext == ".mp3" || ext == ".wav") files.push_back (e.path());
    }
    std::sort (files.begin(), files.end());
    std::vector<std::string> lines (files.size());
    std::atomic<size_t> next { 0 };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::max (2u, std::thread::hardware_concurrency()); ++t)
        pool.emplace_back ([&]
        {
            for (size_t i; (i = next++) < files.size();)
            {
                const auto u8 = files[i].filename().u8string();
                const std::string name (u8.begin(), u8.end());
                if (! tuning.count (name)) continue;
                double sr = 0;
                auto x = loadMono (juce::File (juce::String (files[i].wstring().c_str())), sr, 300.0);
                if (shift != 0.0) x = resample (x, std::pow (2.0, shift / 1200.0));
                auto opt = base;
                std::vector<double> beats;
                if (beatsPerUnit > 0)
                {
                    const auto env = centsible::tempodetail::computeOnsetEnvelope (x.data(), x.size(), sr, 0);
                    beats = centsible::tempodetail::trackBeats (env, centsible::tempodetail::trackTempo (env));
                    opt.beats = &beats; opt.beatsPerUnit = beatsPerUnit;
                }
                const auto f = centsible::keydetail::computeKeyFeatures (x.data(), x.size(), sr, wrapC (tuning[name] + shift), opt);
                std::ostringstream o;
                o << '"' << name << '"';
                for (const auto* c : { &f.peaksGated, &f.cancBass, &f.cancGated }) for (double v : *c) o << ',' << v;
                o << ',' << f.frames << ',' << f.seconds;
                lines[i] = o.str();
            }
        });
    for (auto& t : pool) t.join();
    std::ofstream out (fs::u8path (argv[1]), std::ios::binary);
    out << "file";
    for (const char* g : { "pg", "cb", "cg" }) for (int i = 0; i < 12; ++i) out << ',' << g << i;
    out << ",frames,seconds\n";
    for (const auto& l : lines) if (! l.empty()) out << l << '\n';
    return 0;
}
