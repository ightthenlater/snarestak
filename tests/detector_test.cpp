// Offline test for the transient detector.
// Synthesizes a snare-mic track (snare hits + ghost notes + kick and hi-hat bleed
// + room noise) with known onset times, runs the detector over it in random
// block sizes, and reports recall, false triggers, timing error and dynamics tracking.
//
// Build: c++ -O2 -std=c++17 -I../Source detector_test.cpp -o detector_test

#include "TransientDetector.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace snarestack;

struct Truth { int64_t sample; float amp; bool ghost; bool onKick = false; };

static constexpr double kPi = 3.14159265358979323846;

static void addSnare (std::vector<float>& buf, int64_t at, float amp, double fs, std::mt19937& rng)
{
    std::normal_distribution<float> n (0.0f, 1.0f);
    float hpState = 0.0f, prev = 0.0f;
    const int len = (int) (0.35 * fs);
    double ph1 = 0, ph2 = 0;
    for (int i = 0; i < len && at + i < (int64_t) buf.size(); ++i)
    {
        const double t = i / fs;
        const double attack = std::min (1.0, t / 0.0003);
        const double f1 = 185.0 + 60.0 * std::exp (-t / 0.008);
        const double f2 = 330.0 + 80.0 * std::exp (-t / 0.008);
        ph1 += 2 * kPi * f1 / fs;
        ph2 += 2 * kPi * f2 / fs;
        const double body = (0.8 * std::sin (ph1) + 0.4 * std::sin (ph2)) * std::exp (-t / 0.09);
        const float w = n (rng);
        hpState = 0.92f * (hpState + w - prev); // crude high-pass for "wires"
        prev = w;
        const double wires = 0.45 * hpState * std::exp (-t / 0.15);
        const double stick = 1.2 * hpState * std::exp (-t / 0.004); // stick impact burst
        buf[(size_t) (at + i)] += (float) (amp * attack * (body + wires + stick));
    }
}

static void addKickBleed (std::vector<float>& buf, int64_t at, float amp, double fs, std::mt19937& rng)
{
    std::normal_distribution<float> n (0.0f, 1.0f);
    const int len = (int) (0.4 * fs);
    double ph = 0;
    float prev = 0;
    for (int i = 0; i < len && at + i < (int64_t) buf.size(); ++i)
    {
        const double t = i / fs;
        const double f = 52.0 + 90.0 * std::exp (-t / 0.03);
        ph += 2 * kPi * f / fs;
        const float w = n (rng);
        const double click = (w - prev) * std::exp (-t / 0.002) * 0.15; // beater click
        prev = w;
        buf[(size_t) (at + i)] += (float) (amp * (std::sin (ph) * std::exp (-t / 0.25) + click));
    }
}

static void addHatBleed (std::vector<float>& buf, int64_t at, float amp, double fs, std::mt19937& rng)
{
    std::normal_distribution<float> n (0.0f, 1.0f);
    const int len = (int) (0.12 * fs);
    Biquad f1, f2;
    f1.setHighpass (fs, 6000.0, 0.54119610);
    f2.setHighpass (fs, 6000.0, 1.30656296);
    for (int i = 0; i < len && at + i < (int64_t) buf.size(); ++i)
    {
        const double t = i / fs;
        const float hp = f2.process (f1.process (n (rng)));
        buf[(size_t) (at + i)] += (float) (amp * hp * std::exp (-t / 0.03));
    }
}

struct Result { int detected = 0, missedMain = 0, missedGhost = 0, falseHits = 0; double worstRealBleed = -200, quietestFalseBleed = 200; double minMainDb = 0, minGhostDb = 0, maxFalseDb = -200; double meanErrMs = 0, maxErrMs = 0, maxDelay = 0, velCorr = 0; int totalMain = 0, totalGhost = 0, missedGhostOnKick = 0, totalGhostOnKick = 0; };

static Result runScenario (double fs, const std::vector<float>& audio, const std::vector<Truth>& truth,
                           DetectorSettings settings, std::mt19937& rng, bool verbose)
{
    TransientDetector det;
    if (getenv ("ENVMS")) det.envMs = atof (getenv ("ENVMS"));
    if (getenv ("FRAC")) det.onsetFraction = (float) atof (getenv ("FRAC"));
    det.prepare (fs);
    det.setSettings (settings);

    std::vector<Hit> hits;
    double maxDelay = 0;
    std::uniform_int_distribution<int> blockDist (16, 1024);
    size_t pos = 0;
    while (pos < audio.size())
    {
        const int n = (int) std::min<size_t> ((size_t) blockDist (rng), audio.size() - pos);
        det.process (audio.data() + pos, n, [&] (const Hit& h)
        {
            hits.push_back (h);
            // at the latest, the hit is reported at the end of this block
            maxDelay = std::max (maxDelay, (double) (det.getSamplesProcessed() - h.sample));
        });
        // per-sample-accurate delay bound check: the detector reports inside the loop,
        // so the true delay is <= the block-end bound measured above.
        pos += (size_t) n;
    }

    Result r;
    r.maxDelay = maxDelay;
    const double tol = 0.004 * fs;
    std::vector<bool> used (hits.size(), false);
    double errSum = 0;
    std::vector<std::pair<double, double>> velPairs;
    for (auto& t : truth)
    {
        (t.ghost ? r.totalGhost : r.totalMain)++;
        if (t.onKick) r.totalGhostOnKick++;
        int best = -1;
        double bestErr = 1e9;
        for (size_t i = 0; i < hits.size(); ++i)
        {
            if (used[i]) continue;
            const double e = std::abs ((double) (hits[i].sample - t.sample));
            if (e <= tol && e < bestErr) { bestErr = e; best = (int) i; }
        }
        if (best < 0)
        {
            (t.ghost ? r.missedGhost : r.missedMain)++;
            if (t.onKick) r.missedGhostOnKick++;
            continue;
        }
        used[(size_t) best] = true;
        r.detected++;
        auto& m = t.ghost ? r.minGhostDb : r.minMainDb;
        m = std::min (m, (double) hits[(size_t) best].peakDb);
        const auto& hh = hits[(size_t) best];
        if (getenv ("VERBOSE")) std::printf ("    real %s at %.3f peak %.1f low %+.1f high %+.1f\n", t.ghost ? "ghost" : "main", t.sample / fs, hh.peakDb, hh.lowDb - hh.peakDb, hh.highDb - hh.peakDb);
        r.worstRealBleed = std::max (r.worstRealBleed, (double) std::max (hh.lowDb - hh.peakDb, hh.highDb - hh.peakDb));
        const double errMs = (hits[(size_t) best].sample - t.sample) * 1000.0 / fs;
        errSum += errMs;
        r.maxErrMs = std::max (r.maxErrMs, std::abs (errMs));
        velPairs.push_back ({ 20.0 * std::log10 (t.amp), hits[(size_t) best].peakDb });
    }
    for (size_t i = 0; i < hits.size(); ++i)
    {
        if (! used[i])
        {
            r.falseHits++;
            r.maxFalseDb = std::max (r.maxFalseDb, (double) hits[i].peakDb);
            r.quietestFalseBleed = std::min (r.quietestFalseBleed, (double) std::max (hits[i].lowDb - hits[i].peakDb, hits[i].highDb - hits[i].peakDb));
            if (verbose && getenv ("VERBOSE")) std::printf ("    false hit at %.3f s (peak %.1f dB) low %+.1f high %+.1f\n", hits[i].sample / fs, hits[i].peakDb, hits[i].lowDb - hits[i].peakDb, hits[i].highDb - hits[i].peakDb);
        }
    }
    r.meanErrMs = r.detected ? errSum / r.detected : 0;

    // correlation between true hit level (dB) and detected peak level (dB)
    double mx = 0, my = 0;
    for (auto& p : velPairs) { mx += p.first; my += p.second; }
    mx /= velPairs.size(); my /= velPairs.size();
    double sxy = 0, sxx = 0, syy = 0;
    for (auto& p : velPairs) { sxy += (p.first - mx) * (p.second - my); sxx += (p.first - mx) * (p.first - mx); syy += (p.second - my) * (p.second - my); }
    r.velCorr = sxy / std::sqrt (sxx * syy + 1e-12);
    return r;
}

static void report (const char* name, const Result& r, double fs, int maxReportDelay)
{
    std::printf ("%s\n", name);
    std::printf ("  main hits:   %d/%d detected\n", r.totalMain - r.missedMain, r.totalMain);
    std::printf ("  ghost notes: %d/%d detected (%d/%d of those landing on a kick)\n", r.totalGhost - r.missedGhost, r.totalGhost,
                 r.totalGhostOnKick - r.missedGhostOnKick, r.totalGhostOnKick);
    std::printf ("  false triggers (kick/hat bleed etc.): %d\n", r.falseHits);
    std::printf ("  quietest main %.1f dB, quietest ghost %.1f dB, loudest false trigger %.1f dB\n", r.minMainDb, r.minGhostDb, r.maxFalseDb);
    std::printf ("  side-band excess: real hits up to %+.1f dB, false triggers from %+.1f dB\n", r.worstRealBleed, r.quietestFalseBleed);
    std::printf ("  onset timing error: mean %+.3f ms, worst %.3f ms\n", r.meanErrMs, r.maxErrMs);
    std::printf ("  level tracking correlation: %.3f\n", r.velCorr);
    std::printf ("  worst report delay: %.1f ms (bound %.1f ms)\n",
                 r.maxDelay * 1000.0 / fs, maxReportDelay * 1000.0 / fs);
}

int main()
{
    int failures = 0;
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        std::mt19937 rng (1234);
        std::uniform_real_distribution<float> u (0.0f, 1.0f);
        const double bpm = 96.0;
        const double sixteenth = 60.0 / bpm / 4.0;
        const int bars = 12;
        std::vector<float> audio ((size_t) ((bars * 16 * sixteenth + 1.0) * fs), 0.0f);
        std::vector<Truth> truth;

        for (int bar = 0; bar < bars; ++bar)
        {
            for (int s = 0; s < 16; ++s)
            {
                // slight human timing jitter (+-3 ms)
                const double jitter = (u (rng) - 0.5) * 0.006;
                const int64_t at = (int64_t) ((((bar * 16) + s) * sixteenth + 0.05 + jitter) * fs);

                if (s == 0 || s == 8 || (s == 10 && bar % 2 == 1))
                    addKickBleed (audio, at, 0.30f, fs, rng);
                if (s % 2 == 0)
                    addHatBleed (audio, at, 0.10f + 0.04f * u (rng), fs, rng);

                if (s == 4 || s == 12)
                {
                    const float amp = 0.5f + 0.4f * u (rng);
                    addSnare (audio, at, amp, fs, rng);
                    truth.push_back ({ at, amp, false });
                }
                else if ((s == 7 || s == 15 || s == 10) && u (rng) < 0.6f)
                {
                    const float amp = 0.10f + 0.06f * u (rng); // ghost notes, ~ -15 dB
                    addSnare (audio, at, amp, fs, rng);
                    truth.push_back ({ at, amp, true, s == 10 && bar % 2 == 1 });
                }
            }
        }
        std::normal_distribution<float> n (0.0f, 0.001f);
        for (auto& x : audio) x += n (rng);

        TransientDetector probe;
        probe.prepare (fs);
        char title[128];
        std::snprintf (title, sizeof (title), "== Groove with ghosts + kick/hat bleed @ %.0f Hz (default settings) ==", fs);
        DetectorSettings defaults;
        if (getenv ("NOREJECT")) defaults.kickRejectDb = defaults.hatRejectDb = 100.0f;
        const auto r = runScenario (fs, audio, truth, defaults, rng, true);
        report (title, r, fs, probe.getMaxReportDelay());
        if (r.missedMain > 0 || (r.missedGhost - r.missedGhostOnKick) * 100 > (r.totalGhost - r.totalGhostOnKick) * 15 || r.falseHits > 3 || r.maxErrMs > 1.5 || r.maxDelay > probe.getMaxReportDelay())
            ++failures;

        // same track with the floor raised: quiet bleed should disappear, ghosts should stay
        DetectorSettings gated;
        gated.floorDb = -30.0f;
        std::snprintf (title, sizeof (title), "== Same groove @ %.0f Hz, floor raised to -30 dB ==", fs);
        const auto rg = runScenario (fs, audio, truth, gated, rng, true);
        report (title, rg, fs, probe.getMaxReportDelay());
        if (rg.missedMain > 0 || (rg.missedGhost - rg.missedGhostOnKick) * 100 > (rg.totalGhost - rg.totalGhostOnKick) * 15 || rg.falseHits > 1) ++failures;

        // snare roll: 32nd notes at 140 bpm (~54 ms apart), crescendo
        std::vector<float> roll ((size_t) (4.0 * fs), 0.0f);
        std::vector<Truth> rollTruth;
        const double gap = 60.0 / 140.0 / 8.0;
        for (int i = 0; i < 48; ++i)
        {
            const int64_t at = (int64_t) ((0.1 + i * gap) * fs);
            const float amp = 0.15f + 0.7f * i / 47.0f;
            addSnare (roll, at, amp, fs, rng);
            rollTruth.push_back ({ at, amp, false });
        }
        for (auto& x : roll) x += n (rng);
        DetectorSettings fast;
        fast.retriggerMs = 40.0f;
        fast.thresholdDb = 5.0f; // rolls: sensitivity turned up
        if (getenv ("ROLLTH")) fast.thresholdDb = (float) atof (getenv ("ROLLTH"));
        std::snprintf (title, sizeof (title), "== 32nd-note roll @ %.0f Hz (retrigger 40 ms, threshold 5 dB) ==", fs);
        const auto r2 = runScenario (fs, roll, rollTruth, fast, rng, true);
        report (title, r2, fs, probe.getMaxReportDelay());
        if (r2.missedMain * 100 > r2.totalMain * 15 || r2.falseHits * 100 > r2.totalMain * 15 || r2.maxErrMs > 3.5 || r2.maxDelay > probe.getMaxReportDelay()) ++failures;
        std::printf ("\n");
    }

    std::printf (failures == 0 ? "ALL SCENARIOS PASSED\n" : "%d SCENARIO(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
