#pragma once

// SnareStack — transient detector
// Header-only, no JUCE dependency, so it can be unit-tested on its own.
//
// How it works
//   1. The input is band-limited (4th-order high-pass + 4th-order low-pass) so the
//      detector only "hears" the part of the spectrum that belongs to the snare.
//   2. Energy is measured in short hops (2 ms). A hit is a hop whose attack energy
//      (first-differenced band signal) is at least `thresholdDb` above the average
//      of hops n-4..n-2, and whose band level is above `floorDb`.
//   2b. Bleed rejection: the energy below and above the detection band is measured
//      too. A "hit" that is much louder below the band (kick) or above it (hats)
//      than inside it is thrown away.
//   3. After a trigger, the detector watches 2 more hops to find the peak level
//      (used for velocity), then refines the onset to sample accuracy by finding
//      the first sample that reaches 40% of the hit's peak amplitude, and
//      compensates for the band filters' group delay.
//   4. Hits are reported a bounded number of samples after they happen
//      (getMaxReportDelay()), which lets the plugin report a fixed latency to the
//      host and place triggered samples exactly on the original transient.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <vector>

namespace snarestack
{

struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    void reset() noexcept { z1 = z2 = 0.0; }

    void setLowpass (double fs, double freq, double q)  { design (fs, freq, q, true); }
    void setHighpass (double fs, double freq, double q) { design (fs, freq, q, false); }

    inline float process (float x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float) y;
    }

    std::complex<double> response (double fs, double freq) const
    {
        const double w = 2.0 * pi * freq / fs;
        const std::complex<double> z1c = std::polar (1.0, -w);
        const std::complex<double> z2c = std::polar (1.0, -2.0 * w);
        return (b0 + b1 * z1c + b2 * z2c) / (1.0 + a1 * z1c + a2 * z2c);
    }

    static constexpr double pi = 3.14159265358979323846;

private:
    void design (double fs, double freq, double q, bool lowpass)
    {
        freq = std::clamp (freq, 10.0, fs * 0.45);
        const double w0 = 2.0 * pi * freq / fs;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * q);
        const double a0 = 1.0 + alpha;

        if (lowpass) { b0 = (1.0 - cw) * 0.5; b1 = 1.0 - cw;    b2 = (1.0 - cw) * 0.5; }
        else         { b0 = (1.0 + cw) * 0.5; b1 = -(1.0 + cw); b2 = (1.0 + cw) * 0.5; }
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha;

        b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    }
};

struct DetectorSettings
{
    float highpassHz  = 150.0f;  // detection band, low edge
    float lowpassHz   = 6000.0f; // detection band, high edge
    float thresholdDb = 9.0f;    // how sharply the level must jump to count as a hit
    float floorDb     = -45.0f;  // anything quieter than this (band RMS, dBFS) is ignored
    float retriggerMs = 60.0f;   // minimum time between hits
    // Bleed rejection: a hit is thrown away if the spectrum below the detection band
    // (kick bleed) or above it (hi-hat bleed) is this many dB louder than the band
    // itself. Lower = stricter. Set to >= 40 to switch a check off.
    float kickRejectDb = 0.0f;
    float hatRejectDb  = 8.8f;
};

inline bool differs (float a, float b) noexcept { return std::abs (a - b) > 1.0e-6f; }

inline bool operator!= (const DetectorSettings& a, const DetectorSettings& b) noexcept
{
    return differs (a.highpassHz, b.highpassHz) || differs (a.lowpassHz, b.lowpassHz)
        || differs (a.thresholdDb, b.thresholdDb) || differs (a.floorDb, b.floorDb)
        || differs (a.retriggerMs, b.retriggerMs) || differs (a.kickRejectDb, b.kickRejectDb)
        || differs (a.hatRejectDb, b.hatRejectDb);
}

struct Hit
{
    int64_t sample;     // absolute input-sample index of the onset
    float   peakDb;     // peak band level of the hit (dBFS RMS over 2 ms)
    float   relativeDb; // peak relative to the loudest recent hit (<= 0)
    float   lowDb;      // peak level below the detection band during the hit
    float   highDb;     // peak level above the detection band during the hit
};

class TransientDetector
{
public:
    static constexpr int peakSearchHops = 2;
    static constexpr int historyHops    = 4; // hops kept; the reference skips the newest one

    void prepare (double sampleRate)
    {
        fs       = sampleRate;
        hop      = std::max (16, (int) std::lround (fs * 0.002));
        ringSize = hop * 16;
        ring.assign ((size_t) ringSize, 0.0f);
        envLen   = std::max (4, (int) std::lround (fs * envMs * 0.001));
        envScratch.assign ((size_t) ringSize, 0.0f);
        updateFilters();
        updateRefractory();
        reset();
    }

    void reset()
    {
        for (auto* f : { &hp1, &hp2, &lp1, &lp2, &lowA, &lowB, &highA, &highB, &highC, &highD })
            f->reset();
        for (auto& e : historyEnergy) e = 1.0e-14;
        lowEnergy = highEnergy = 0.0;
        rejectedCount = 0;
        std::fill (ring.begin(), ring.end(), 0.0f);
        sampleCounter = 0;
        hopCounter    = 0;
        hopFill       = 0;
        energy        = 0.0;
        attackEnergy  = 0.0;
        prevY         = 0.0f;
        searching     = false;
        lastHitHop    = std::numeric_limits<int64_t>::min() / 2;
        loudestDb     = -200.0f;
        lastLevelDb   = -120.0f;
        lastRiseDb    = 0.0f;
    }

    void setSettings (const DetectorSettings& s)
    {
        const bool filtersChanged = differs (s.highpassHz, settings.highpassHz) || differs (s.lowpassHz, settings.lowpassHz);
        settings = s;
        if (filtersChanged) updateFilters();
        updateRefractory();
    }

    const DetectorSettings& getSettings() const noexcept { return settings; }
    int     getHopSize() const noexcept                  { return hop; }
    int64_t getSamplesProcessed() const noexcept         { return sampleCounter; }
    float   getLastLevelDb() const noexcept              { return lastLevelDb; }
    float   getGroupDelaySamples() const noexcept        { return groupDelay; }
    int     getRejectedCount() const noexcept            { return rejectedCount; }

    // Upper bound on (time a hit is reported) - (hit's onset sample).
    int getMaxReportDelay() const noexcept { return (peakSearchHops + 5) * hop; }

    // in:         mono input
    // monitorOut: optional, receives the band-filtered signal ("listen" mode)
    // onHit(const Hit&): called when a hit is confirmed
    // onHop(float levelDb, float riseDb): called once per 2 ms hop (for metering)
    template <typename OnHit, typename OnHop>
    void process (const float* in, float* monitorOut, int numSamples, OnHit&& onHit, OnHop&& onHop)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float x = in[i];
            const float y = hp2.process (hp1.process (lp2.process (lp1.process (x))));
            if (monitorOut != nullptr) monitorOut[i] = y;

            ring[(size_t) (sampleCounter % ringSize)] = y;
            const float d = y - prevY; // first difference: emphasises the attack (stick, wires) over ringing body
            prevY = y;
            attackEnergy += (double) d * d;
            energy += (double) y * y;

            const float lo = lowB.process (lowA.process (x));
            const float hi = highD.process (highC.process (highB.process (highA.process (x))));
            lowEnergy  += (double) lo * lo;
            highEnergy += (double) hi * hi;
            ++sampleCounter;

            if (++hopFill == hop)
            {
                endOfHop (onHit, onHop);
                hopFill = 0;
                energy  = 0.0;
                attackEnergy = 0.0;
                lowEnergy = highEnergy = 0.0;
            }
        }
    }

    template <typename OnHit>
    void process (const float* in, int numSamples, OnHit&& onHit)
    {
        process (in, nullptr, numSamples, onHit, [] (float, float) {});
    }

private:
    template <typename OnHit, typename OnHop>
    void endOfHop (OnHit& onHit, OnHop& onHop)
    {
        const double meanAttack = attackEnergy / hop + 1.0e-14;
        const float level = (float) (10.0 * std::log10 (energy / hop + 1.0e-12));
        const float lowLevel  = (float) (10.0 * std::log10 (lowEnergy / hop + 1.0e-12));
        const float highLevel = (float) (10.0 * std::log10 (highEnergy / hop + 1.0e-12));
        const int64_t hopIdx = hopCounter++;

        // Rise is measured on the first-differenced signal, which weights the attack
        // (stick, wires) over the ringing body, so hits stand out even on top of the
        // previous hit's tail (rolls, flams). Reference = mean of the previous 6 ms;
        // averaging keeps beating between drum modes from looking like new attacks.
        // The hop right before is skipped so attacks that ramp up over ~4 ms
        // (softer sticks, compressed or saturated snares) still show their full rise.
        double refEnergy = 0.0;
        for (int k = 1; k < historyHops; ++k) refEnergy += historyEnergy[k];
        refEnergy /= (historyHops - 1);
        const float rise = (float) (10.0 * std::log10 (meanAttack / refEnergy));

        lastLevelDb = level;
        lastRiseDb  = rise;

        // the "loudest recent hit" (the dynamics reference) forgets very slowly
        // (6 dB per minute), so one freak hit doesn't squash a whole song
        loudestDb -= 0.1f * (float) hop / (float) fs;

        if (searching)
        {
            peakDb = std::max (peakDb, level);
            peakLowDb  = std::max (peakLowDb, lowLevel);
            peakHighDb = std::max (peakHighDb, highLevel);
            if (hopIdx - onsetHop >= peakSearchHops)
                finalize (onHit);
        }
        else if (hopIdx - lastHitHop >= refractoryHops
                 && rise >= settings.thresholdDb
                 && level >= settings.floorDb)
        {
            searching  = true;
            onsetHop   = hopIdx;
            peakDb     = level;
            peakLowDb  = lowLevel;
            peakHighDb = highLevel;
        }

        for (int k = historyHops - 1; k > 0; --k) historyEnergy[k] = historyEnergy[k - 1];
        historyEnergy[0] = meanAttack;

        onHop (level, rise);
    }

    template <typename OnHit>
    void finalize (OnHit& onHit)
    {
        searching = false;

        // Spectral bleed rejection: a kick leaking into the snare mic is far louder
        // below the detection band than inside it; a hi-hat is far louder above it.
        // Rejected hits don't start the retrigger lockout, so a real hit right after
        // still gets through.
        if ((settings.kickRejectDb < 40.0f && peakLowDb - peakDb > settings.kickRejectDb)
            || (settings.hatRejectDb < 40.0f && peakHighDb - peakDb > settings.hatRejectDb))
        {
            ++rejectedCount;
            return;
        }

        lastHitHop = onsetHop;

        if (peakDb > loudestDb) loudestDb = peakDb;
        const float rel = std::min (0.0f, peakDb - loudestDb);

        // Sample-accurate onset, from a 1 ms envelope of the attack signal (the
        // first-differenced band signal, which has a clean step at the stick impact
        // even when the drum body rings or beats): find the envelope peak, measure the
        // baseline in the hop before the attack (silence, or the previous hit's tail),
        // then take the first point that climbs 15% of the way up and stays there.
        const int64_t oldest = std::max<int64_t> (0, sampleCounter - ringSize + envLen + 1);
        const int64_t start  = std::max<int64_t> (oldest, (onsetHop - 3) * (int64_t) hop);
        const int64_t end    = sampleCounter;
        const int count = (int) (end - start);

        double acc = 0.0;
        for (int k = 0; k < envLen; ++k)
        {
            const float v = ring[(size_t) ((start - envLen + k) % ringSize)];
            acc += (double) v * v;
        }
        for (int i = 0; i < count; ++i)
        {
            const float vin  = ring[(size_t) ((start + i) % ringSize)];
            const float vout = ring[(size_t) ((start + i - envLen) % ringSize)];
            acc += (double) vin * vin - (double) vout * vout;
            envScratch[(size_t) i] = (float) std::max (0.0, acc);
        }

        const int searchFrom = (int) std::min<int64_t> (count - 1, hop); // peak must be after the baseline hop
        int peakIdx = searchFrom;
        for (int i = searchFrom; i < count; ++i)
            if (envScratch[(size_t) i] > envScratch[(size_t) peakIdx]) peakIdx = i;

        // baseline = mean envelope over the hop before the attack (previous hit's tail or silence)
        double base = 0.0;
        const int baseEnd = std::max (1, std::min (hop, peakIdx));
        for (int i = 0; i < baseEnd; ++i) base += envScratch[(size_t) i];
        base /= baseEnd;

        const float hi = envScratch[(size_t) peakIdx];
        const float level = (float) (base + onsetFraction * (hi - base));
        // First crossing of `level` that holds for 0.5 ms. A forward search isn't fooled
        // by dips later in the attack (stick -> wires -> body), and the hold keeps a
        // momentary wobble in a ringing tail from counting. The trigger compares hop n
        // against hops n-4..n-2, so the attack began in hop n-1 or later: the search
        // starts half a hop before that.
        const int holdLen = std::max (2, (int) std::lround (fs * 0.0005));
        const float holdLevel = (float) (base + 0.7 * (level - base));
        const int firstCandidate = std::min (peakIdx, std::max (baseEnd, 2 * hop - hop / 2));
        int onsetIdx = peakIdx;
        for (int i = firstCandidate; i <= peakIdx; ++i)
        {
            if (envScratch[(size_t) i] < level) continue;
            bool holds = true;
            for (int k = i + 1; k < std::min (count, i + holdLen); ++k)
                if (envScratch[(size_t) k] < holdLevel) { holds = false; break; }
            if (holds) { onsetIdx = i; break; }
        }

        // a step through a boxcar of length N crosses fraction f after f*N samples
        int64_t onset = start + onsetIdx - (int64_t) std::lround (onsetFraction * envLen);

        onset -= (int64_t) std::lround (groupDelay);
        onset = std::max<int64_t> (0, onset);

        onHit (Hit { onset, peakDb, rel, peakLowDb, peakHighDb });
    }

    void updateFilters()
    {
        if (fs <= 0.0) return;
        const double hpF = std::clamp ((double) settings.highpassHz, 20.0, fs * 0.4);
        const double lpF = std::clamp ((double) settings.lowpassHz, hpF * 1.5, fs * 0.45);
        // 4th-order Butterworth = two biquads with these Qs
        hp1.setHighpass (fs, hpF, 0.54119610);
        hp2.setHighpass (fs, hpF, 1.30656296);
        lp1.setLowpass (fs, lpF, 0.54119610);
        lp2.setLowpass (fs, lpF, 1.30656296);

        // side bands for bleed rejection: everything below / above the detection band
        lowA.setLowpass (fs, hpF, 0.54119610);
        lowB.setLowpass (fs, hpF, 1.30656296);
        highA.setHighpass (fs, lpF, 0.54119610);
        highB.setHighpass (fs, lpF, 1.30656296);
        // cap the high side band at 16 kHz so the comparison doesn't depend on the sample rate
        const double capF = std::max (lpF * 1.2, std::min (16000.0, fs * 0.45));
        highC.setLowpass (fs, capF, 0.54119610);
        highD.setLowpass (fs, capF, 1.30656296);

        // group delay of the whole cascade at the band's geometric centre
        const double fc = std::sqrt (hpF * lpF);
        const double df = fc * 0.01;
        auto phaseAt = [&] (double f)
        {
            return std::arg (hp1.response (fs, f) * hp2.response (fs, f) * lp1.response (fs, f) * lp2.response (fs, f));
        };
        double dphi = phaseAt (fc + df) - phaseAt (fc - df);
        while (dphi > Biquad::pi)  dphi -= 2.0 * Biquad::pi;
        while (dphi < -Biquad::pi) dphi += 2.0 * Biquad::pi;
        const double dw = 2.0 * Biquad::pi * (2.0 * df) / fs;
        groupDelay = (float) std::clamp (-dphi / dw, 0.0, (double) hop);
    }

    void updateRefractory()
    {
        if (fs <= 0.0 || hop <= 0) return;
        refractoryHops = std::max<int64_t> (peakSearchHops + 1,
                                            (int64_t) std::ceil (settings.retriggerMs * 0.001 * fs / hop));
    }

    DetectorSettings settings;
    double fs = 0.0;
    int hop = 96, ringSize = 0;
    Biquad hp1, hp2, lp1, lp2, lowA, lowB, highA, highB, highC, highD;
    double lowEnergy = 0.0, highEnergy = 0.0;
    float peakLowDb = -120.0f, peakHighDb = -120.0f;
    int rejectedCount = 0;
    float groupDelay = 0.0f;

    std::vector<float> ring, envScratch;
    int envLen = 48;
public:
    double envMs = 1.0;           // onset envelope length (tunable for tests)
    float onsetFraction = 0.15f;  // how far up the rise counts as the onset
private:
    double historyEnergy[historyHops] {};
    int64_t sampleCounter = 0, hopCounter = 0;
    int hopFill = 0;
    double energy = 0.0, attackEnergy = 0.0;
    float prevY = 0.0f;

    bool searching = false;
    int64_t onsetHop = 0, lastHitHop = 0, refractoryHops = 1;
    float peakDb = -120.0f, loudestDb = -200.0f;
    float lastLevelDb = -120.0f, lastRiseDb = 0.0f;
};

} // namespace snarestack
