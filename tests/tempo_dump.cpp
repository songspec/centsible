// Tempo research tool: pulse-train scores (no prior) on a 60-200 BPM grid (0.5 BPM steps) for three onset bands, per file, as CSV (tools/tempo_exp.py).
//   tempo_dump <out.csv> <audio dir>
#include "tempo_detector.h"
#include "gen/audio_io.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
int main (int argc, char** argv)
{
    if (argc < 3) { std::printf ("usage: tempo_dump <out.csv> <audio dir>\n"); return 2; }
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator (fs::u8path (argv[2])))
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
                double sr = 0;
                const auto x = loadMono (juce::File (juce::String (files[i].wstring().c_str())), sr, 300.0);
                const auto u8 = files[i].filename().u8string();
                std::ostringstream o;
                o << '"' << std::string (u8.begin(), u8.end()) << '"';
                for (int band = 0; band < 3; ++band)
                {
                    const auto c = centsible::tempodetail::computeTempoCurve (x.data(), x.size(), sr, band);
                    for (double bpm = 60.0; bpm <= 200.0; bpm += 0.5) o << ',' << (c.valid ? c.pulse (bpm) : 0.0);
                }
                lines[i] = o.str();
            }
        });
    for (auto& t : pool) t.join();
    std::ofstream out (fs::u8path (argv[1]), std::ios::binary);
    out << "file";
    for (int band = 0; band < 3; ++band) for (double bpm = 60.0; bpm <= 200.0; bpm += 0.5) out << ",b" << band << "_" << bpm;
    out << '\n';
    for (const auto& l : lines) out << l << '\n';
    return 0;
}
