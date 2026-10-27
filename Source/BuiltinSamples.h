#pragma once

// Procedurally synthesized snare samples, so the plugin makes sound out of the box
// without shipping (or licensing) any recordings. Generated at the host sample rate.

#include <juce_audio_basics/juce_audio_basics.h>

namespace snarestack
{

enum class BuiltinSnare
{
    Crack = 0,   // bright, mid-length, lots of wire
    Fat,         // low tuning, long body, darker wires
    Tight,       // high tuning, short and snappy
    Rimshot,     // rim ring + hard click
    NumBuiltins
};

juce::String getBuiltinName (int index);
juce::AudioBuffer<float> makeBuiltinSnare (int index, double sampleRate);

} // namespace snarestack
