#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BuiltinSamples.h"

namespace snarestack
{

juce::String slotParamId (int slot, const char* name)
{
    return "s" + juce::String (slot + 1) + name;
}

static const int defaultBuiltinForSlot[numSlots] = { 0, 1, 2, 3 };
static const bool defaultOnForSlot[numSlots]     = { true, true, false, false };
static const float defaultGainForSlot[numSlots]  = { 0.0f, -6.0f, -6.0f, -9.0f };

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout SnareStackProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto pct = [] (float v, int) { return String (juce::roundToInt (v)) + "%"; };
    auto db  = [] (float v, int) { return v <= -59.9f ? String ("Off") : String (v, 1) + " dB"; };
    auto hz  = [] (float v, int) { return v >= 1000.0f ? String (v / 1000.0f, 1) + " kHz" : String (juce::roundToInt (v)) + " Hz"; };
    auto ms  = [] (float v, int) { return String (v, 1) + " ms"; };

    auto addFloat = [&] (const String& id, const String& name, NormalisableRange<float> range, float def,
                         std::function<String (float, int)> toText)
    {
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, range, def,
                                                           AudioParameterFloatAttributes().withStringFromValueFunction (toText)));
    };

    // detection
    addFloat ("sens",    "Sensitivity",  { 0.0f, 100.0f, 0.1f }, 50.0f, pct);
    addFloat ("floor",   "Floor",        { -70.0f, -10.0f, 0.1f }, -45.0f, [] (float v, int) { return String (v, 1) + " dB"; });
    addFloat ("retrig",  "Retrigger",    { 10.0f, 200.0f, 0.1f, 0.6f }, 60.0f, ms);
    addFloat ("hpf",     "Detect HPF",   { 40.0f, 1000.0f, 1.0f, 0.4f }, 150.0f, hz);
    addFloat ("lpf",     "Detect LPF",   { 1500.0f, 16000.0f, 1.0f, 0.4f }, 6000.0f, hz);
    addFloat ("kickrej", "Kick Reject",  { 0.0f, 100.0f, 0.1f }, 89.0f, pct);
    addFloat ("hatrej",  "Hat Reject",   { 0.0f, 100.0f, 0.1f }, 40.0f, pct);
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "listen", 1 }, "Listen to Detector", false));

    // mix
    addFloat ("dyn",   "Dynamics", { 0.0f, 100.0f, 0.1f }, 75.0f, pct);
    addFloat ("dry",   "Original", { -60.0f, 6.0f, 0.1f, 2.0f }, 0.0f, db);
    addFloat ("wet",   "Samples",  { -60.0f, 6.0f, 0.1f, 2.0f }, -3.0f, db);
    addFloat ("align", "Align",    { -5.0f, 5.0f, 0.01f }, 0.0f, ms);
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "midinote", 1 }, "MIDI Note", 0, 127, 38));

    // sample slots
    for (int s = 0; s < numSlots; ++s)
    {
        const String n = "Layer " + String (s + 1) + " ";
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { slotParamId (s, "on"), 1 }, n + "On", defaultOnForSlot[s]));
        addFloat (slotParamId (s, "gain"),  n + "Level", { -36.0f, 12.0f, 0.1f }, defaultGainForSlot[s],
                  [] (float v, int) { return String (v, 1) + " dB"; });
        addFloat (slotParamId (s, "pitch"), n + "Pitch", { -12.0f, 12.0f, 0.01f }, 0.0f,
                  [] (float v, int) { return (v > 0 ? "+" : "") + String (v, 1) + " st"; });
        addFloat (slotParamId (s, "pan"),   n + "Pan",   { -100.0f, 100.0f, 0.1f }, 0.0f,
                  [] (float v, int) { return std::abs (v) < 0.5f ? String ("C") : (v < 0 ? "L" : "R") + String (juce::roundToInt (std::abs (v))); });
        addFloat (slotParamId (s, "len"),   n + "Length", { 5.0f, 100.0f, 0.1f }, 100.0f, pct);
    }
    return layout;
}

//==============================================================================
SnareStackProcessor::SnareStackProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "SnareStack", createLayout())
{
    formatManager.registerBasicFormats();

    pSens     = apvts.getRawParameterValue ("sens");
    pFloor    = apvts.getRawParameterValue ("floor");
    pRetrig   = apvts.getRawParameterValue ("retrig");
    pHpf      = apvts.getRawParameterValue ("hpf");
    pLpf      = apvts.getRawParameterValue ("lpf");
    pKickRej  = apvts.getRawParameterValue ("kickrej");
    pHatRej   = apvts.getRawParameterValue ("hatrej");
    pDyn      = apvts.getRawParameterValue ("dyn");
    pDry      = apvts.getRawParameterValue ("dry");
    pWet      = apvts.getRawParameterValue ("wet");
    pAlign    = apvts.getRawParameterValue ("align");
    pListen   = apvts.getRawParameterValue ("listen");
    pMidiNote = apvts.getRawParameterValue ("midinote");
    for (int s = 0; s < numSlots; ++s)
    {
        pOn[(size_t) s]    = apvts.getRawParameterValue (slotParamId (s, "on"));
        pGain[(size_t) s]  = apvts.getRawParameterValue (slotParamId (s, "gain"));
        pPitch[(size_t) s] = apvts.getRawParameterValue (slotParamId (s, "pitch"));
        pPan[(size_t) s]   = apvts.getRawParameterValue (slotParamId (s, "pan"));
        pLen[(size_t) s]   = apvts.getRawParameterValue (slotParamId (s, "len"));
    }

    detector.prepare (currentRate);
    for (int s = 0; s < numSlots; ++s)
        loadBuiltin (s, defaultBuiltinForSlot[s]);
}

bool SnareStackProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo()) return false;
    return in.size() <= out.size();
}

//==============================================================================
void SnareStackProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentRate = sampleRate;
    detector.prepare (sampleRate);
    lastSettings = readDetectorSettings();
    detector.setSettings (lastSettings);

    alignHeadroom = (int) std::ceil (0.005 * sampleRate);
    latency = detector.getMaxReportDelay() + alignHeadroom + 1;
    setLatencySamples (latency);

    dryDelay.setSize (2, latency);
    dryDelay.clear();
    monitorDelay.setSize (1, latency);
    monitorDelay.clear();
    delayWrite = 0;

    const int cap = std::max (samplesPerBlock, 512);
    mono.setSize (1, cap);
    monitor.setSize (1, cap);
    wet.setSize (2, cap);

    dryGain.reset (sampleRate, 0.02);
    wetGain.reset (sampleRate, 0.02);
    dryGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (pDry->load(), -59.9f));
    wetGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (pWet->load(), -59.9f));

    numPending = 0;
    numMidiEvents = 0;
    displayFifo.reset();

    // re-render every sample at the new rate
    for (int s = 0; s < numSlots; ++s)
    {
        const auto src = getSlotSource (s);
        if (src.startsWith ("builtin:"))
        {
            loadBuiltin (s, src.fromFirstOccurrenceOf ("builtin:", false, false).getIntValue());
        }
        else
        {
            auto& slot = slots[(size_t) s];
            const juce::SpinLock::ScopedLockType lock (slot.lock);
            if (slot.data != nullptr) prepareSample (*slot.data);
            for (auto& v : slot.voices) v.active = false;
        }
    }
}

DetectorSettings SnareStackProcessor::readDetectorSettings() const
{
    DetectorSettings s;
    s.thresholdDb  = sensitivityToThresholdDb (pSens->load());
    s.floorDb      = pFloor->load();
    s.retriggerMs  = pRetrig->load();
    s.highpassHz   = pHpf->load();
    s.lowpassHz    = pLpf->load();
    s.kickRejectDb = rejectToMarginDb (pKickRej->load());
    s.hatRejectDb  = rejectToMarginDb (pHatRej->load());
    return s;
}

//==============================================================================
void SnareStackProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int n      = buffer.getNumSamples();
    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    midi.clear();
    if (n <= 0) return;

    if (mono.getNumSamples() < n) // host sent a bigger block than promised
    {
        mono.setSize (1, n, false, false, true);
        monitor.setSize (1, n, false, false, true);
        wet.setSize (2, n, false, false, true);
    }

    // detector settings (cheap to compare every block)
    {
        const auto s = readDetectorSettings();
        if (s != lastSettings)
        {
            detector.setSettings (s);
            lastSettings = s;
        }
    }

    // mono sum of the input for detection
    auto* m = mono.getWritePointer (0);
    if (numIn <= 0)
    {
        juce::FloatVectorOperations::clear (m, n);
    }
    else
    {
        juce::FloatVectorOperations::copy (m, buffer.getReadPointer (0), n);
        for (int ch = 1; ch < numIn; ++ch)
            juce::FloatVectorOperations::add (m, buffer.getReadPointer (ch), n);
        if (numIn > 1) juce::FloatVectorOperations::multiply (m, 1.0f / (float) numIn, n);
    }

    // mono in, stereo out: duplicate the input so the dry signal appears on both sides
    for (int ch = std::max (1, numIn); ch < numOut; ++ch)
    {
        if (numIn > 0) buffer.copyFrom (ch, 0, buffer, numIn - 1, 0, n);
        else           buffer.clear (ch, 0, n);
    }

    const int64_t blockStart  = detector.getSamplesProcessed();
    const int     alignSamples = (int) std::lround (pAlign->load() * 0.001 * currentRate);
    const int     midiNote     = juce::jlimit (0, 127, (int) pMidiNote->load());

    // MIDI notes that are due this block, collected while detecting
    struct MidiHit { int64_t time; int velocity; };
    std::array<MidiHit, 16> midiHits {};
    int numMidiHits = 0;

    detector.process (m, monitor.getWritePointer (0), n,
        [&] (const Hit& h)
        {
            // Output = input delayed by `latency`, so the original hit leaves the plugin at
            // h.sample + latency. Triggered samples start exactly there (+ user alignment).
            if (numPending < (int) pending.size())
                pending[(size_t) numPending++] = { h.sample + latency + alignSamples, h.relativeDb };

            const int vel = juce::jlimit (1, 127, juce::roundToInt (127.0f + h.relativeDb * (126.0f / 36.0f)));
            if (numMidiHits < (int) midiHits.size())
                midiHits[(size_t) numMidiHits++] = { h.sample + latency, vel };

            pendingHitDb = h.relativeDb;
            hitCount.fetch_add (1);
        },
        [&] (float levelDb, float)
        {
            const DisplayPoint p { levelDb, pendingHitDb };
            pendingHitDb = std::numeric_limits<float>::quiet_NaN();
            const auto scope = displayFifo.write (1);
            if (scope.blockSize1 > 0)      displayBuffer[(size_t) scope.startIndex1] = p;
            else if (scope.blockSize2 > 0) displayBuffer[(size_t) scope.startIndex2] = p;
        });

    // delay the original (and the detector monitor) by the plugin latency
    const int D = dryDelay.getNumSamples();
    for (int ch = 0; ch < numOut; ++ch)
    {
        auto* data = buffer.getWritePointer (ch);
        auto* d    = dryDelay.getWritePointer (std::min (ch, 1));
        int w = delayWrite;
        for (int i = 0; i < n; ++i)
        {
            const float x = data[i];
            data[i] = d[w];
            d[w] = x;
            if (++w == D) w = 0;
        }
    }
    {
        auto* mon = monitor.getWritePointer (0);
        auto* d   = monitorDelay.getWritePointer (0);
        int w = delayWrite;
        for (int i = 0; i < n; ++i)
        {
            const float x = mon[i];
            mon[i] = d[w];
            d[w] = x;
            if (++w == D) w = 0;
        }
    }
    delayWrite = (int) ((delayWrite + (int64_t) n) % D);

    // start the samples that are due in this block, then render all voices
    wet.clear (0, 0, n);
    wet.clear (1, 0, n);
    {
        int keep = 0;
        for (int i = 0; i < numPending; ++i)
        {
            const auto& p = pending[(size_t) i];
            if (p.time < blockStart + n)
                startVoices ((int) std::max<int64_t> (0, p.time - blockStart), p.relDb);
            else
                pending[(size_t) keep++] = p;
        }
        numPending = keep;
    }
    renderVoices (wet, n);

    // mix
    const bool listen = pListen->load() > 0.5f;
    dryGain.setTargetValue (juce::Decibels::decibelsToGain (pDry->load(), -59.9f));
    wetGain.setTargetValue (juce::Decibels::decibelsToGain (pWet->load(), -59.9f));

    if (listen)
    {
        for (int ch = 0; ch < numOut; ++ch)
            buffer.copyFrom (ch, 0, monitor, 0, 0, n);
        dryGain.skip (n);
        wetGain.skip (n);
    }
    else
    {
        const float* wl = wet.getReadPointer (0);
        const float* wr = wet.getReadPointer (1);
        float* outL = buffer.getWritePointer (0);
        float* outR = numOut > 1 ? buffer.getWritePointer (1) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            const float gd = dryGain.getNextValue();
            const float gw = wetGain.getNextValue();
            if (outR != nullptr)
            {
                outL[i] = gd * outL[i] + gw * wl[i];
                outR[i] = gd * outR[i] + gw * wr[i];
            }
            else
            {
                outL[i] = gd * outL[i] + gw * 0.5f * (wl[i] + wr[i]);
            }
        }
    }

    // MIDI out: one note per hit on channel 10 (GM drums), lined up with the delayed original
    for (int i = 0; i < numMidiHits; ++i)
    {
        const auto& h = midiHits[(size_t) i];
        if (numMidiEvents + 2 <= (int) midiEvents.size())
        {
            midiEvents[(size_t) numMidiEvents++] = { h.time, midiNote, h.velocity };
            midiEvents[(size_t) numMidiEvents++] = { h.time + (int64_t) (0.03 * currentRate), midiNote, 0 };
        }
    }
    {
        int keep = 0;
        for (int i = 0; i < numMidiEvents; ++i)
        {
            const auto e = midiEvents[(size_t) i];
            if (e.time < blockStart + n)
            {
                const int offset = (int) juce::jlimit<int64_t> (0, n - 1, e.time - blockStart);
                midi.addEvent (e.velocity > 0 ? juce::MidiMessage::noteOn (10, e.note, (juce::uint8) e.velocity)
                                              : juce::MidiMessage::noteOff (10, e.note),
                               offset);
            }
            else
            {
                midiEvents[(size_t) keep++] = e;
            }
        }
        numMidiEvents = keep;
    }
}

void SnareStackProcessor::startVoices (int offset, float hitRelativeDb)
{
    const float dyn = pDyn->load() * 0.01f;
    const float hitGain = juce::Decibels::decibelsToGain (std::max (-48.0f, hitRelativeDb * dyn));

    for (int s = 0; s < numSlots; ++s)
    {
        if (pOn[(size_t) s]->load() < 0.5f) continue;

        auto& slot = slots[(size_t) s];
        const juce::SpinLock::ScopedTryLockType lock (slot.lock);
        if (! lock.isLocked() || slot.data == nullptr) continue;

        const int len = slot.data->playback.getNumSamples();
        if (len < 2) continue;

        auto& v = slot.voices[(size_t) slot.nextVoice];
        slot.nextVoice = (slot.nextVoice + 1) % voicesPerSlot;

        const float lenFrac = juce::jlimit (0.05f, 1.0f, pLen[(size_t) s]->load() * 0.01f);
        const float pan     = juce::jlimit (-1.0f, 1.0f, pPan[(size_t) s]->load() * 0.01f);
        const float angle   = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        const float gain    = juce::Decibels::decibelsToGain (pGain[(size_t) s]->load()) * hitGain;

        v.active      = true;
        v.pos         = 0.0;
        v.increment   = std::pow (2.0, pPitch[(size_t) s]->load() / 12.0);
        v.endPos      = (len - 1) * (double) lenFrac;
        v.fadeStart   = lenFrac >= 0.999f ? v.endPos : v.endPos * 0.6;
        v.gainL       = gain * std::cos (angle) * juce::MathConstants<float>::sqrt2;
        v.gainR       = gain * std::sin (angle) * juce::MathConstants<float>::sqrt2;
        v.startOffset = offset;

        slot.triggerCount.fetch_add (1);
    }
}

void SnareStackProcessor::renderVoices (juce::AudioBuffer<float>& out, int numSamples)
{
    float* outL = out.getWritePointer (0);
    float* outR = out.getWritePointer (1);

    for (auto& slot : slots)
    {
        const juce::SpinLock::ScopedTryLockType lock (slot.lock);
        if (! lock.isLocked() || slot.data == nullptr) continue;

        const auto& buf = slot.data->playback;
        const int len = buf.getNumSamples();
        if (len < 2) continue;
        const float* L = buf.getReadPointer (0);
        const float* R = buf.getReadPointer (1);

        for (auto& v : slot.voices)
        {
            if (! v.active) continue;
            const double fadeLen = std::max (1.0, v.endPos - v.fadeStart);

            for (int i = v.startOffset; i < numSamples; ++i)
            {
                if (v.pos >= v.endPos) { v.active = false; break; }

                const int i0 = (int) v.pos;
                const int i1 = std::min (i0 + 1, len - 1);
                const float fr = (float) (v.pos - i0);
                float env = 1.0f;
                if (v.pos > v.fadeStart)
                {
                    env = (float) ((v.endPos - v.pos) / fadeLen);
                    env *= env;
                }
                outL[i] += (L[i0] + fr * (L[i1] - L[i0])) * v.gainL * env;
                outR[i] += (R[i0] + fr * (R[i1] - R[i0])) * v.gainR * env;
                v.pos += v.increment;
            }
            v.startOffset = 0;
        }
    }
}

int SnareStackProcessor::readDisplay (DisplayPoint* dest, int maxPoints)
{
    const auto scope = displayFifo.read (std::min (maxPoints, displayFifo.getNumReady()));
    int k = 0;
    for (int i = 0; i < scope.blockSize1; ++i) dest[k++] = displayBuffer[(size_t) (scope.startIndex1 + i)];
    for (int i = 0; i < scope.blockSize2; ++i) dest[k++] = displayBuffer[(size_t) (scope.startIndex2 + i)];
    return k;
}

//==============================================================================
// Samples

void SnareStackProcessor::prepareSample (SampleData& d) const
{
    const int inLen = d.original.getNumSamples();
    if (inLen == 0)
    {
        d.playback.setSize (2, 0);
        d.peaks.clear();
        return;
    }

    if (std::abs (d.originalRate - currentRate) < 0.5)
    {
        d.playback.makeCopyOf (d.original);
    }
    else
    {
        const double ratio = d.originalRate / currentRate; // input samples per output sample
        const int outLen = (int) std::ceil (inLen / ratio);
        d.playback.setSize (2, outLen);
        juce::AudioBuffer<float> padded (2, inLen + 16);
        padded.clear();
        for (int ch = 0; ch < 2; ++ch)
        {
            padded.copyFrom (ch, 0, d.original, ch, 0, inLen);
            juce::LagrangeInterpolator interp;
            interp.process (ratio, padded.getReadPointer (ch), d.playback.getWritePointer (ch), outLen);
        }
    }

    // waveform overview
    constexpr int bins = 160;
    d.peaks.assign (bins, 0.0f);
    const int len = d.playback.getNumSamples();
    for (int b = 0; b < bins; ++b)
    {
        const int a = (int) ((int64_t) len * b / bins);
        const int e = std::max (a + 1, (int) ((int64_t) len * (b + 1) / bins));
        float mx = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            mx = std::max (mx, d.playback.getMagnitude (ch, a, std::min (e, len) - a));
        d.peaks[(size_t) b] = mx;
    }
}

void SnareStackProcessor::installSample (int s, std::unique_ptr<SampleData> data)
{
    auto& slot = slots[(size_t) s];
    {
        const juce::SpinLock::ScopedLockType lock (slot.lock);
        std::swap (slot.data, data);
        for (auto& v : slot.voices) v.active = false;
    }
    data.reset(); // old sample freed here, off the audio thread
    slotsVersion.fetch_add (1);
}

void SnareStackProcessor::loadBuiltin (int s, int builtinIndex)
{
    auto d = std::make_unique<SampleData>();
    d->name = getBuiltinName (builtinIndex);
    d->source = "builtin:" + juce::String (builtinIndex);
    d->original = makeBuiltinSnare (builtinIndex, currentRate);
    d->originalRate = currentRate;
    prepareSample (*d);
    installSample (s, std::move (d));
}

void SnareStackProcessor::clearSlot (int s)
{
    auto d = std::make_unique<SampleData>();
    d->name = "Empty";
    installSample (s, std::move (d));
}

bool SnareStackProcessor::loadSampleFile (int s, const juce::File& file, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
    {
        error = "Couldn't read " + file.getFileName() + " (use WAV, AIFF, FLAC or OGG).";
        return false;
    }

    const auto maxLen = (juce::int64) (reader->sampleRate * 10.0); // 10 s is plenty for a drum hit
    const int len = (int) std::min (reader->lengthInSamples, maxLen);
    if (len <= 0)
    {
        error = file.getFileName() + " is empty.";
        return false;
    }

    const int fileChans = (int) juce::jlimit (1u, 2u, reader->numChannels);
    juce::AudioBuffer<float> tmp (fileChans, len);
    reader->read (&tmp, 0, len, 0, true, fileChans > 1);

    juce::AudioBuffer<float> stereo (2, len);
    stereo.copyFrom (0, 0, tmp, 0, 0, len);
    stereo.copyFrom (1, 0, tmp, fileChans > 1 ? 1 : 0, 0, len);

    // Trim leading silence so the sample's transient starts right on the hit,
    // and normalize so layers are easy to balance.
    const float peak = std::max (stereo.getMagnitude (0, 0, len), stereo.getMagnitude (1, 0, len));
    if (peak <= 0.0f)
    {
        error = file.getFileName() + " is silent.";
        return false;
    }
    int start = 0;
    const float gate = peak * 0.01f; // -40 dB
    while (start < len - 1 && std::abs (stereo.getSample (0, start)) < gate && std::abs (stereo.getSample (1, start)) < gate)
        ++start;
    start = std::max (0, start - 8);

    auto d = std::make_unique<SampleData>();
    d->name = file.getFileNameWithoutExtension();
    d->source = file.getFullPathName();
    d->originalRate = reader->sampleRate;
    d->original.setSize (2, len - start);
    for (int ch = 0; ch < 2; ++ch)
        d->original.copyFrom (ch, 0, stereo, ch, start, len - start);
    d->original.applyGain (juce::Decibels::decibelsToGain (-1.0f) / peak);

    prepareSample (*d);
    installSample (s, std::move (d));
    return true;
}

void SnareStackProcessor::loadFromSource (int s, const juce::String& source)
{
    if (source.startsWith ("builtin:"))
    {
        loadBuiltin (s, source.fromFirstOccurrenceOf ("builtin:", false, false).getIntValue());
    }
    else if (source.isEmpty())
    {
        clearSlot (s);
    }
    else
    {
        juce::String error;
        const juce::File f (source);
        if (! f.existsAsFile() || ! loadSampleFile (s, f, error))
        {
            // keep the reference so re-saving the project doesn't lose it
            auto d = std::make_unique<SampleData>();
            d->name = "Missing: " + f.getFileName();
            d->source = source;
            installSample (s, std::move (d));
        }
    }
}

juce::String SnareStackProcessor::getSlotName (int s)
{
    auto& slot = slots[(size_t) s];
    const juce::SpinLock::ScopedLockType lock (slot.lock);
    return slot.data != nullptr ? slot.data->name : juce::String ("Empty");
}

juce::String SnareStackProcessor::getSlotSource (int s)
{
    auto& slot = slots[(size_t) s];
    const juce::SpinLock::ScopedLockType lock (slot.lock);
    return slot.data != nullptr ? slot.data->source : juce::String();
}

std::vector<float> SnareStackProcessor::getSlotPeaks (int s)
{
    auto& slot = slots[(size_t) s];
    const juce::SpinLock::ScopedLockType lock (slot.lock);
    return slot.data != nullptr ? slot.data->peaks : std::vector<float> {};
}

//==============================================================================
// State

void SnareStackProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.removeChild (state.getChildWithName ("SLOTS"), nullptr);
    juce::ValueTree slotTree ("SLOTS");
    for (int s = 0; s < numSlots; ++s)
        slotTree.setProperty ("src" + juce::String (s), getSlotSource (s), nullptr);
    state.appendChild (slotTree, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void SnareStackProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;

    auto tree = juce::ValueTree::fromXml (*xml);
    const auto slotTree = tree.getChildWithName ("SLOTS").createCopy();
    tree.removeChild (tree.getChildWithName ("SLOTS"), nullptr);
    apvts.replaceState (tree);

    if (slotTree.isValid())
        for (int s = 0; s < numSlots; ++s)
            loadFromSource (s, slotTree.getProperty ("src" + juce::String (s)).toString());
}

juce::AudioProcessorEditor* SnareStackProcessor::createEditor()
{
    return new SnareStackEditor (*this);
}

} // namespace snarestack

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new snarestack::SnareStackProcessor();
}
