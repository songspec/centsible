// Developer tool (not part of ctest): caches the pooled tonal / low-band evidence histograms of every song of a folder at several exact pitch shifts, so that
// decisions of the fit (which tuning, which template) can be re-run in seconds instead of re-analysing the audio (tests/fit_variants.cpp).
//   hist_cache <audio dir> <out dir> <maxSeconds> <shift,shift,...> [jobs]
// One file per song: records of { float shift; double massTonal, massLow; float tonal[400]; float low[400]; }.
#include "gen/audio_io.h"
#include "gen/resample.h"
#include <centsible/reading.h>
#include "fft.h"
#define private public
#include "spectral_likelihood.h"
#undef private
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using namespace centsible::detail;

int main (int argc, char** argv)
{
    if (argc < 5) { std::printf ("usage: hist_cache <audio dir> <out dir> <maxSeconds> <shifts> [jobs]\n"); return 2; }
    const fs::path dir = fs::u8path (argv[1]), out = fs::u8path (argv[2]);
    const double maxSeconds = std::atof (argv[3]);
    std::vector<double> shifts;
    {
        std::stringstream ss (argv[4]);
        std::string tok;
        while (std::getline (ss, tok, ',')) shifts.push_back (std::atof (tok.c_str()));
    }
    const unsigned jobs = argc > 5 ? (unsigned) std::atoi (argv[5]) : std::max (2u, std::thread::hardware_concurrency());
    fs::create_directories (out);
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator (dir))
    {
        const auto ext = e.path().extension().string();
        if (ext == ".flac" || ext == ".mp3" || ext == ".wav") files.push_back (e.path());
    }
    std::sort (files.begin(), files.end());
    std::atomic<size_t> next { 0 };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < jobs; ++t)
        pool.emplace_back ([&]
        {
            for (;;)
            {
                const size_t i = next++;
                if (i >= files.size()) return;
                const fs::path target = out / fs::path (files[i].filename().native() + L".hist");
                std::error_code ec;
                if (fs::exists (target, ec) && fs::file_size (target, ec) > 0) continue;
                double sr = 0;
                const auto u8 = files[i].u8string();
                std::vector<float> x;
                {
                    juce::AudioFormatManager fm;
                    fm.registerBasicFormats();
                    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (juce::File (juce::String::fromUTF8 (reinterpret_cast<const char*> (u8.c_str())))));
                    if (! rd || rd->lengthInSamples < 1000) continue;                                // undecodable: skip, do not stop the batch
                    sr = rd->sampleRate;
                    const int n = (int) std::min<juce::int64> (rd->lengthInSamples, (juce::int64) (maxSeconds * sr));
                    juce::AudioBuffer<float> buf ((int) rd->numChannels, n);
                    rd->read (&buf, 0, n, 0, true, true);
                    x.assign ((size_t) n, 0.0f);
                    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                        for (int k = 0; k < n; ++k) x[(size_t) k] += buf.getSample (ch, k) / (float) buf.getNumChannels();
                }
                FILE* f = _wfopen (target.c_str(), L"wb");
                if (! f) continue;
                for (double shift : shifts)
                {
                    const auto y = shift == 0.0 ? x : resample (x, std::pow (2.0, shift / 1200.0));
                    auto est = std::make_unique<SpectralLikelihoodEstimator>();
                    est->prepare (sr);
                    for (size_t pos = 0; pos < y.size(); pos += 512) est->push (y.data() + pos, (int) std::min<size_t> (512, y.size() - pos));
                    Hist T, L;
                    double mt, ml;
                    est->sumTonal (T, mt, 0, true);
                    est->sumLow (L, ml, true);
                    const float sh = (float) shift;
                    std::fwrite (&sh, sizeof sh, 1, f);
                    std::fwrite (&mt, sizeof mt, 1, f);
                    std::fwrite (&ml, sizeof ml, 1, f);
                    std::fwrite (T.data(), sizeof (float), kBins, f);
                    std::fwrite (L.data(), sizeof (float), kBins, f);
                }
                std::fclose (f);
                std::printf ("done %s\n", files[i].filename().string().c_str());
                std::fflush (stdout);
            }
        });
    for (auto& t : pool) t.join();
    return 0;
}
