// Headless end-to-end test of the real SnareStack processor.
// Checks: dry passthrough is exactly the input delayed by the reported latency,
// triggered samples land on the original hits, MIDI notes line up, dynamics follow
// the input, user samples load (with leading-silence trim), and state is restored.
// Optionally renders the editor to a PNG (pass an output path as argv[1]).

#include "../Source/PluginProcessor.h"
#include "../Source/BuiltinSamples.h"
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdio>
#include <cstdlib>
#include <random>

using namespace snarestack;

static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::printf ("  [%s] %s\n", ok ? "ok" : "FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}

static void setParam (SnareStackProcessor& p, const juce::String& id, float value)
{
    auto* param = p.apvts.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

struct Track
{
    juce::AudioBuffer<float> audio;
    std::vector<int64_t> hits;
    std::vector<float> amps;
};

static Track makeTrack (double fs)
{
    // a real-ish snare track: the "Fat" builtin as the recorded snare, at varying
    // levels, plus kick bleed
    Track t;
    const int len = (int) (8.0 * fs);
    t.audio.setSize (2, len);
    t.audio.clear();
    const auto snare = makeBuiltinSnare (1, fs);
    std::mt19937 rng (7);
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    constexpr double pi = 3.14159265358979323846;

    for (int k = 0; k < 15; ++k)
    {
        const int64_t at = (int64_t) ((0.3 + 0.5 * k) * fs) + (int64_t) (u (rng) * 200);
        const float amp = 0.25f + 0.6f * u (rng);
        for (int ch = 0; ch < 2; ++ch)
            t.audio.addFrom (ch, (int) at, snare, ch, 0, std::min (snare.getNumSamples(), len - (int) at), amp);
        t.hits.push_back (at);
        t.amps.push_back (amp);

        // kick bleed half way between snares
        const int64_t kAt = at + (int64_t) (0.25 * fs);
        double ph = 0;
        for (int i = 0; i < (int) (0.3 * fs) && kAt + i < len; ++i)
        {
            const double tt = i / fs;
            ph += 2 * pi * (50.0 + 60.0 * std::exp (-tt / 0.02)) / fs;
            const float v = (float) (0.35 * std::sin (ph) * std::exp (-tt / 0.2));
            t.audio.addSample (0, (int) (kAt + i), v);
            t.audio.addSample (1, (int) (kAt + i), v);
        }
    }
    return t;
}

struct RunResult
{
    juce::AudioBuffer<float> out;
    std::vector<std::pair<int64_t, juce::MidiMessage>> midi;
};

static RunResult run (SnareStackProcessor& p, const juce::AudioBuffer<float>& in, int block)
{
    RunResult r;
    r.out.makeCopyOf (in);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < in.getNumSamples(); pos += block)
    {
        const int n = std::min (block, in.getNumSamples() - pos);
        juce::AudioBuffer<float> view (r.out.getArrayOfWritePointers(), 2, pos, n);
        midi.clear();
        p.processBlock (view, midi);
        for (const auto meta : midi)
            r.midi.push_back ({ pos + meta.samplePosition, meta.getMessage() });
    }
    return r;
}

static int64_t findOnset (const juce::AudioBuffer<float>& b, int64_t from, int64_t to, float frac)
{
    float peak = 0.0f;
    for (int64_t i = from; i < to; ++i) peak = std::max (peak, std::abs (b.getSample (0, (int) i)));
    for (int64_t i = from; i < to; ++i)
        if (std::abs (b.getSample (0, (int) i)) >= frac * peak) return i;
    return -1;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        std::printf ("== %.0f Hz ==\n", fs);
        auto proc = std::unique_ptr<SnareStackProcessor> (dynamic_cast<SnareStackProcessor*> (createPluginFilter()));
        proc->setPlayConfigDetails (2, 2, fs, 512);
        proc->prepareToPlay (fs, 512);
        const int L = proc->getLatencySamples();
        std::printf ("  latency: %d samples (%.2f ms)\n", L, L * 1000.0 / fs);

        const auto track = makeTrack (fs);

        // 1) dry only: output must be the input delayed by exactly L samples
        setParam (*proc, "dry", 0.0f);
        setParam (*proc, "wet", -60.0f);
        {
            proc->prepareToPlay (fs, 512);
            const auto r = run (*proc, track.audio, 333);
            float maxErr = 0.0f;
            for (int i = L; i < track.audio.getNumSamples(); ++i)
                for (int ch = 0; ch < 2; ++ch)
                    maxErr = std::max (maxErr, std::abs (r.out.getSample (ch, i) - track.audio.getSample (ch, i - L)));
            check (maxErr < 1.0e-6f, "original passes through untouched, delayed by the reported latency (max err "
                                         + juce::String (maxErr, 8) + ")");

            // MIDI: one note-on per hit, at hit + latency
            std::vector<int64_t> ons;
            for (auto& e : r.midi) if (e.second.isNoteOn()) ons.push_back (e.first);
            check (ons.size() == track.hits.size(), "MIDI: " + juce::String ((int) ons.size()) + " note-ons for "
                                                       + juce::String ((int) track.hits.size()) + " hits");
            double worst = 0;
            for (size_t i = 0; i < std::min (ons.size(), track.hits.size()); ++i)
                worst = std::max (worst, std::abs ((double) (ons[i] - (track.hits[i] + L))) * 1000.0 / fs);
            check (worst < 1.5, "MIDI notes line up with the delayed original (worst " + juce::String (worst, 2) + " ms)");
        }

        // 2) samples only, layer 1: each triggered sample must start on the original hit
        setParam (*proc, "dry", -60.0f);
        setParam (*proc, "wet", 0.0f);
        setParam (*proc, slotParamId (1, "on"), 0.0f);
        setParam (*proc, "dyn", 100.0f);
        {
            proc->prepareToPlay (fs, 512);
            const auto r = run (*proc, track.audio, 480);
            double worst = 0, sum = 0;
            int found = 0;
            std::vector<float> outPeaks;
            for (size_t k = 0; k < track.hits.size(); ++k)
            {
                const int64_t expect = track.hits[k] + L;
                const int64_t on = findOnset (r.out, expect - (int64_t) (0.01 * fs), expect + (int64_t) (0.02 * fs), 0.05f);
                if (on < 0) continue;
                ++found;
                const double err = (on - expect) * 1000.0 / fs;
                worst = std::max (worst, std::abs (err));
                sum += err;
                outPeaks.push_back (r.out.getMagnitude (0, (int) expect, (int) (0.02 * fs)));
            }
            check (found == (int) track.hits.size(), "a sample fired for every hit (" + juce::String (found) + ")");
            check (worst < 1.5, "triggered samples land on the original hits (mean " + juce::String (sum / std::max (1, found), 3)
                                    + " ms, worst " + juce::String (worst, 3) + " ms)");

            // dynamics: louder original hit -> louder sample
            double mx = 0, my = 0;
            const size_t nP = std::min (outPeaks.size(), track.amps.size());
            // (the dynamics reference is the loudest recent hit, so skip the first few
            // hits while it calibrates)
            const size_t skip = 4;
            for (size_t i = skip; i < nP; ++i) { mx += std::log (track.amps[i]); my += std::log (outPeaks[i]); }
            mx /= (nP - skip); my /= (nP - skip);
            double sxy = 0, sxx = 0, syy = 0;
            for (size_t i = skip; i < nP; ++i)
            {
                const double a = std::log (track.amps[i]) - mx, b = std::log (outPeaks[i]) - my;
                sxy += a * b; sxx += a * a; syy += b * b;
            }
            check (sxy / std::sqrt (sxx * syy) > 0.93, "sample levels follow the original's dynamics (r = "
                                                         + juce::String (sxy / std::sqrt (sxx * syy), 3) + ")");

            // nothing fires on kick bleed
            int bleedTriggers = 0;
            for (size_t k = 0; k < track.hits.size(); ++k)
            {
                const int64_t kick = track.hits[k] + (int64_t) (0.25 * fs) + L;
                if (kick + (int64_t) (0.05 * fs) < r.out.getNumSamples()
                    && r.out.getMagnitude (0, (int) kick + (int) (0.002 * fs), (int) (0.01 * fs))
                         > 3.0f * r.out.getMagnitude (0, (int) kick - (int) (0.01 * fs), (int) (0.008 * fs)) + 1.0e-4f)
                    ++bleedTriggers;
            }
            check (bleedTriggers == 0, "no samples fired on kick bleed (" + juce::String (bleedTriggers) + ")");

            float peak = r.out.getMagnitude (0, r.out.getNumSamples());
            check (peak < 1.5f && std::isfinite (peak), "output is sane (peak " + juce::String (peak, 3) + ")");
        }

        // 3) align knob shifts samples
        setParam (*proc, "align", 2.0f);
        {
            proc->prepareToPlay (fs, 512);
            const auto r = run (*proc, track.audio, 256);
            const int64_t expect = track.hits[3] + L + (int64_t) std::lround (0.002 * fs);
            const int64_t on = findOnset (r.out, expect - (int64_t) (0.01 * fs), expect + (int64_t) (0.02 * fs), 0.05f);
            check (std::abs ((double) (on - expect)) * 1000.0 / fs < 1.0, "Align +2 ms moves the samples 2 ms later");
        }
        setParam (*proc, "align", 0.0f);
    }

    // 4) user sample loading + state recall
    std::printf ("== samples & state ==\n");
    {
        auto proc = std::unique_ptr<SnareStackProcessor> (dynamic_cast<SnareStackProcessor*> (createPluginFilter()));
        proc->setPlayConfigDetails (2, 2, 48000.0, 512);
        proc->prepareToPlay (48000.0, 512);

        // write a 44.1 kHz WAV with 100 ms of leading silence
        const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("snarestack_test_snare.wav");
        {
            auto snare = makeBuiltinSnare (2, 44100.0);
            juce::AudioBuffer<float> withGap (2, snare.getNumSamples() + 4410);
            withGap.clear();
            for (int ch = 0; ch < 2; ++ch) withGap.copyFrom (ch, 4410, snare, ch, 0, snare.getNumSamples());
            withGap.applyGain (0.3f);
            tmp.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os (tmp.createOutputStream().release());
            auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (24));
            check (writer != nullptr, "wrote a test WAV");
            if (writer != nullptr) writer->writeFromAudioSampleBuffer (withGap, 0, withGap.getNumSamples());
        }

        juce::String error;
        const bool ok = proc->loadSampleFile (2, tmp, error);
        check (ok, "loaded a 44.1 kHz WAV into layer 3 " + error);
        check (proc->getSlotName (2) == "snarestack_test_snare", "layer name is the file name");
        {
            // the loaded sample should start within 1 ms of its transient (silence trimmed) and be normalized
            auto peaks = proc->getSlotPeaks (2);
            check (! peaks.empty() && peaks[0] > 0.05f, "leading silence was trimmed");
        }

        setParam (*proc, "sens", 77.0f);
        setParam (*proc, slotParamId (2, "pitch"), -3.0f);
        proc->loadBuiltin (0, 3);

        juce::MemoryBlock state;
        proc->getStateInformation (state);

        auto proc2 = std::unique_ptr<SnareStackProcessor> (dynamic_cast<SnareStackProcessor*> (createPluginFilter()));
        proc2->setStateInformation (state.getData(), (int) state.getSize());
        check (std::abs (proc2->apvts.getRawParameterValue ("sens")->load() - 77.0f) < 0.05f, "parameters restored");
        check (std::abs (proc2->apvts.getRawParameterValue (slotParamId (2, "pitch"))->load() + 3.0f) < 0.02f, "layer parameters restored");
        check (proc2->getSlotSource (2) == tmp.getFullPathName(), "user sample path restored");
        check (proc2->getSlotName (0) == "Rimshot", "built-in choice restored");

        tmp.deleteFile();
        auto proc3 = std::unique_ptr<SnareStackProcessor> (dynamic_cast<SnareStackProcessor*> (createPluginFilter()));
        proc3->setStateInformation (state.getData(), (int) state.getSize());
        check (proc3->getSlotName (2).startsWith ("Missing"), "a deleted sample shows as Missing instead of crashing");

        // 5) editor snapshot
        if (argc > 1)
        {
            proc2->prepareToPlay (48000.0, 512);
            const auto track = makeTrack (48000.0);
            std::unique_ptr<juce::AudioProcessorEditor> ed (proc2->createEditor());
            ed->setVisible (true);
            // feed audio in chunks and let the editor timer pull display data
            juce::AudioBuffer<float> buf;
            buf.makeCopyOf (track.audio);
            juce::MidiBuffer midi;
            for (int pos = 0; pos + 512 <= (int) (3.2 * 48000.0); pos += 512)
            {
                juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, pos, 512);
                proc2->processBlock (view, midi);
                if ((pos / 512) % 3 == 0)
                    if (auto* timer = dynamic_cast<juce::Timer*> (ed.get())) timer->timerCallback();
            }
            if (auto* timer = dynamic_cast<juce::Timer*> (ed.get())) timer->timerCallback();
            const auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
            const juce::File out { juce::String (argv[1]) };
            out.deleteFile();
            juce::FileOutputStream fos (out);
            juce::PNGImageFormat png;
            check (png.writeImageToStream (img, fos), "editor rendered to " + out.getFileName());
        }
    }

    std::printf (failures == 0 ? "ALL HOST TESTS PASSED\n" : "%d HOST TEST(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
