#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "TransientDetector.h"

#include <array>
#include <atomic>

namespace snarestack
{

static constexpr int numSlots = 4;
static constexpr int voicesPerSlot = 8;

// One loaded sample (built-in or from a file).
struct SampleData
{
    juce::String name;                   // shown in the UI
    juce::String source;                 // "builtin:N" or an absolute file path
    juce::AudioBuffer<float> original;   // as loaded, stereo
    double originalRate = 44100.0;
    juce::AudioBuffer<float> playback;   // resampled to the host rate, stereo
    std::vector<float> peaks;            // for the waveform thumbnail
};

struct Voice
{
    bool   active = false;
    double pos = 0.0;
    double increment = 1.0;
    double endPos = 0.0;      // sample position where the voice stops (length control)
    double fadeStart = 0.0;   // fade-out begins here
    float  gainL = 1.0f, gainR = 1.0f;
    int    startOffset = 0;   // first sample in the current block to render
};

struct Slot
{
    std::unique_ptr<SampleData> data;
    juce::SpinLock lock;      // guards `data` and `voices`
    std::array<Voice, voicesPerSlot> voices {};
    int nextVoice = 0;
    std::atomic<int> triggerCount { 0 }; // UI flash
};

// What the detection display draws: one point per 2 ms hop.
struct DisplayPoint
{
    float levelDb;
    float hitDb;   // relative level of a hit that ended in this hop, or NaN
};

class SnareStackProcessor final : public juce::AudioProcessor
{
public:
    SnareStackProcessor();
    ~SnareStackProcessor() override = default;

    // AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 1.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Samples (message thread)
    bool loadSampleFile (int slot, const juce::File& file, juce::String& error);
    void loadBuiltin (int slot, int builtinIndex);
    void clearSlot (int slot);
    juce::String getSlotName (int slot);
    juce::String getSlotSource (int slot);
    std::vector<float> getSlotPeaks (int slot);
    int getSlotTriggerCount (int slot) const { return slots[(size_t) slot].triggerCount.load(); }

    // UI feedback
    int  readDisplay (DisplayPoint* dest, int maxPoints);
    int  getHitCount() const noexcept { return hitCount.load(); }
    double getDisplayRateHz() const noexcept { return currentRate / std::max (1, detector.getHopSize()); }
    int  getLatencyMs() const noexcept { return (int) std::lround (getLatencySamples() * 1000.0 / currentRate); }
    std::atomic<int> slotsVersion { 0 }; // bumped whenever a slot's sample changes

    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    juce::AudioFormatManager formatManager;

private:
    void installSample (int slot, std::unique_ptr<SampleData> data);
    void prepareSample (SampleData& d) const;
    void loadFromSource (int slot, const juce::String& source);
    DetectorSettings readDetectorSettings() const;
    void startVoices (int offset, float hitRelativeDb);
    void renderVoices (juce::AudioBuffer<float>& wet, int numSamples);

    TransientDetector detector;
    DetectorSettings lastSettings;
    double currentRate = 44100.0;
    int latency = 0;
    int alignHeadroom = 0;

    std::array<Slot, numSlots> slots;

    // dry (and listen-mode) delay lines, so the original lines up with the triggered samples
    juce::AudioBuffer<float> dryDelay, monitorDelay;
    int delayWrite = 0;

    juce::AudioBuffer<float> mono, monitor, wet;

    struct PendingTrigger { int64_t time; float relDb; };
    std::array<PendingTrigger, 64> pending {};
    int numPending = 0;

    struct MidiEvent { int64_t time; int note; int velocity; }; // velocity 0 = note-off
    std::array<MidiEvent, 128> midiEvents {};
    int numMidiEvents = 0;

    juce::SmoothedValue<float> dryGain, wetGain;

    // display FIFO (audio -> UI)
    juce::AbstractFifo displayFifo { 4096 };
    std::array<DisplayPoint, 4096> displayBuffer {};
    float pendingHitDb = std::numeric_limits<float>::quiet_NaN();
    std::atomic<int> hitCount { 0 };

    // cached parameter pointers
    std::atomic<float>* pSens = nullptr;
    std::atomic<float>* pFloor = nullptr;
    std::atomic<float>* pRetrig = nullptr;
    std::atomic<float>* pHpf = nullptr;
    std::atomic<float>* pLpf = nullptr;
    std::atomic<float>* pKickRej = nullptr;
    std::atomic<float>* pHatRej = nullptr;
    std::atomic<float>* pDyn = nullptr;
    std::atomic<float>* pDry = nullptr;
    std::atomic<float>* pWet = nullptr;
    std::atomic<float>* pAlign = nullptr;
    std::atomic<float>* pListen = nullptr;
    std::atomic<float>* pMidiNote = nullptr;
    std::array<std::atomic<float>*, numSlots> pOn {}, pGain {}, pPitch {}, pPan {}, pLen {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SnareStackProcessor)
};

// Parameter mapping helpers, shared with the editor
inline float sensitivityToThresholdDb (float pct) { return 15.0f - 0.12f * pct; } // 0% -> 15 dB, 100% -> 3 dB
inline float rejectToMarginDb (float pct)          { return pct <= 0.5f ? 100.0f : 16.0f - 0.18f * pct; } // 0% = off, 100% -> -2 dB

juce::String slotParamId (int slot, const char* name);

} // namespace snarestack
