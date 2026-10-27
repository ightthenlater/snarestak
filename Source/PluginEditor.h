#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

namespace snarestack
{

namespace Colours
{
    const juce::Colour bg        { 0xff15161a };
    const juce::Colour panel     { 0xff1e2026 };
    const juce::Colour panelEdge { 0xff2c2f38 };
    const juce::Colour text      { 0xffdcdee3 };
    const juce::Colour dim       { 0xff868b98 };
    const juce::Colour accent    { 0xffff7a2f }; // samples / hits
    const juce::Colour detect    { 0xff3fcfc6 }; // detector
}

class StackLook final : public juce::LookAndFeel_V4
{
public:
    StackLook();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
};

// Rotary knob with a caption above and the current value below.
class Knob final : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const juce::String& caption,
          juce::Colour colour = Colours::accent);
    void resized() override;

    juce::Slider slider;

private:
    juce::Label caption, value;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

// A small round on/off button.
class PowerButton final : public juce::ToggleButton
{
public:
    explicit PowerButton (juce::Colour c) : colour (c) {}
    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    juce::Colour colour;
};

// Scrolling view of the detection band level with hit markers and the floor line.
class DetectionDisplay final : public juce::Component
{
public:
    explicit DetectionDisplay (SnareStackProcessor& p);
    void push (const DisplayPoint* pts, int n);
    void paint (juce::Graphics&) override;

private:
    SnareStackProcessor& proc;
    std::vector<DisplayPoint> history;
    int writePos = 0;
};

class SlotStrip final : public juce::Component,
                        public juce::FileDragAndDropTarget
{
public:
    SlotStrip (SnareStackProcessor& p, int slotIndex);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();          // sample changed
    void tick();             // called by the editor timer

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override            { dragOver = false; repaint(); }
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    void showMenu();
    void loadFile (const juce::File&);

    SnareStackProcessor& proc;
    const int index;
    PowerButton power { Colours::accent };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> powerAttachment;
    juce::TextButton nameButton;
    Knob level, pitch, pan, length;
    std::vector<float> peaks;
    juce::Rectangle<int> waveArea;
    std::unique_ptr<juce::FileChooser> chooser;
    int lastTriggerCount = 0;
    float flash = 0.0f;
    bool dragOver = false;
};

class SnareStackEditor final : public juce::AudioProcessorEditor,
                               public juce::Timer
{
public:
    explicit SnareStackEditor (SnareStackProcessor&);
    ~SnareStackEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    SnareStackProcessor& proc;
    StackLook look;

    DetectionDisplay display;
    Knob sens, floor, retrig, hpf, lpf, kickRej, hatRej;
    juce::TextButton listenButton { "Listen" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> listenAttachment;

    std::array<std::unique_ptr<SlotStrip>, numSlots> strips;

    Knob dry, wet, dyn, align, midiNote;

    juce::Rectangle<int> detectPanel, layersPanel, mixPanel;
    int lastHitCount = 0, lastSlotsVersion = -1;
    float ledLevel = 0.0f;
    std::vector<DisplayPoint> scratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SnareStackEditor)
};

} // namespace snarestack
