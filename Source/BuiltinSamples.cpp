#include "BuiltinSamples.h"
#include "TransientDetector.h" // for Biquad

#include <random>

namespace snarestack
{

namespace
{
struct Recipe
{
    const char* name;
    double f0, f0Start, pitchTau, bodyDecay;      // fundamental drum mode
    double mode2Ratio, mode2Gain, mode2Decay;     // second (inharmonic) mode
    double wireGain, wireHp, wireLp, wireDecay;   // snare wires (filtered noise)
    double stickGain, stickDecay;                 // stick impact
    double drive, lengthSec;
};

constexpr Recipe recipes[] = {
    { "Crack",   215.0, 300.0, 0.006, 0.070, 1.72, 0.45, 0.045, 0.55, 1800.0,  9500.0, 0.130, 0.9, 0.0025, 1.4, 0.45 },
    { "Fat",     172.0, 245.0, 0.009, 0.140, 1.60, 0.35, 0.090, 0.40, 1200.0,  6500.0, 0.220, 0.6, 0.0030, 1.8, 0.70 },
    { "Tight",   265.0, 360.0, 0.004, 0.040, 1.80, 0.40, 0.030, 0.60, 3000.0, 12000.0, 0.080, 1.0, 0.0018, 1.2, 0.30 },
    { "Rimshot", 440.0, 520.0, 0.003, 0.030, 2.45, 0.70, 0.050, 0.15, 2500.0,  9000.0, 0.060, 1.4, 0.0012, 2.0, 0.30 },
};
} // namespace

juce::String getBuiltinName (int index)
{
    if (index < 0 || index >= (int) BuiltinSnare::NumBuiltins) return {};
    return recipes[index].name;
}

juce::AudioBuffer<float> makeBuiltinSnare (int index, double fs)
{
    index = juce::jlimit (0, (int) BuiltinSnare::NumBuiltins - 1, index);
    const auto& r = recipes[index];
    const int len = (int) (r.lengthSec * fs);

    juce::AudioBuffer<float> out (2, len);
    out.clear();

    std::mt19937 rng ((unsigned) (index + 1) * 7919u);
    std::uniform_real_distribution<float> uni (-1.0f, 1.0f);

    Biquad wHp1, wHp2, wLp, sHp;
    wHp1.setHighpass (fs, r.wireHp, 0.7071);
    wHp2.setHighpass (fs, r.wireHp, 0.7071);
    wLp.setLowpass (fs, std::min (r.wireLp, fs * 0.45), 0.7071);
    sHp.setHighpass (fs, 1000.0, 0.7071);

    constexpr double twoPi = 6.283185307179586;
    double ph1 = 0.0, ph2 = 0.0;
    auto* L = out.getWritePointer (0);
    const double fadeStart = len - 0.01 * fs;

    for (int i = 0; i < len; ++i)
    {
        const double t = i / fs;
        const double f = r.f0 + (r.f0Start - r.f0) * std::exp (-t / r.pitchTau);
        ph1 += twoPi * f / fs;
        ph2 += twoPi * f * r.mode2Ratio / fs;

        const double body  = std::sin (ph1) * std::exp (-t / r.bodyDecay)
                           + r.mode2Gain * std::sin (ph2) * std::exp (-t / r.mode2Decay);
        const float  noise = uni (rng);
        const double wires = r.wireGain * wLp.process (wHp2.process (wHp1.process (noise))) * std::exp (-t / r.wireDecay);
        const double stick = r.stickGain * sHp.process (noise) * std::exp (-t / r.stickDecay);

        double x = body + wires * 2.0 + stick;
        x *= std::min (1.0, t / 0.00015);                       // tiny attack ramp, no click
        x = std::tanh (r.drive * x) / std::tanh (r.drive);      // a bit of drum-bus glue
        if (i > fadeStart) x *= (len - i) / (len - fadeStart);  // clean tail end
        L[i] = (float) x;
    }

    const float peak = out.getMagnitude (0, 0, len);
    if (peak > 0.0f) out.applyGain (0, 0, len, juce::Decibels::decibelsToGain (-1.0f) / peak);
    out.copyFrom (1, 0, out, 0, 0, len);
    return out;
}

} // namespace snarestack
