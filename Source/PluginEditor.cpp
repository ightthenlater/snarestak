#include "PluginEditor.h"
#include "BuiltinSamples.h"

namespace snarestack
{

static juce::Font uiFont (float size, bool bold = false)
{
    auto opts = juce::FontOptions (size);
    if (bold) opts = opts.withStyle ("Bold");
    return juce::Font (opts);
}

//==============================================================================
StackLook::StackLook()
{
    setColour (juce::Label::textColourId, Colours::text);
    setColour (juce::TextButton::buttonColourId, Colours::panelEdge);
    setColour (juce::TextButton::buttonOnColourId, Colours::detect);
    setColour (juce::TextButton::textColourOffId, Colours::text);
    setColour (juce::TextButton::textColourOnId, Colours::bg);
    setColour (juce::PopupMenu::backgroundColourId, Colours::panel);
    setColour (juce::PopupMenu::textColourId, Colours::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Colours::accent.withAlpha (0.85f));
    setColour (juce::PopupMenu::highlightedTextColourId, Colours::bg);
    setColour (juce::PopupMenu::headerTextColourId, Colours::dim);
}

void StackLook::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                  float startAngle, float endAngle, juce::Slider& s)
{
    const auto colour = s.findColour (juce::Slider::rotarySliderFillColourId);
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (3.0f);
    const float radius = std::min (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);
    const float track = std::max (2.5f, radius * 0.13f);

    juce::Path bgArc;
    bgArc.addCentredArc (centre.x, centre.y, radius - track * 0.5f, radius - track * 0.5f, 0.0f, startAngle, endAngle, true);
    g.setColour (Colours::panelEdge.brighter (0.15f));
    g.strokePath (bgArc, juce::PathStrokeType (track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // bipolar parameters (pan, pitch, align) fill from the centre
    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0 && std::abs (s.getMinimum() + s.getMaximum()) < 1.0e-3;
    const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
    if (std::abs (angle - from) > 0.001f)
    {
        juce::Path valueArc;
        valueArc.addCentredArc (centre.x, centre.y, radius - track * 0.5f, radius - track * 0.5f, 0.0f,
                                std::min (from, angle), std::max (from, angle), true);
        g.setColour (s.isEnabled() ? colour : colour.withSaturation (0.0f));
        g.strokePath (valueArc, juce::PathStrokeType (track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const float knobR = radius - track * 2.2f;
    g.setGradientFill (juce::ColourGradient (Colours::panelEdge.brighter (0.35f), centre.x, centre.y - knobR,
                                             Colours::panelEdge.darker (0.4f), centre.x, centre.y + knobR, false));
    g.fillEllipse (centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f);

    juce::Path pointer;
    pointer.addRoundedRectangle (-1.5f, -knobR + 2.0f, 3.0f, knobR * 0.55f, 1.5f);
    g.setColour (Colours::text);
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));
}

void StackLook::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    auto c = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId)
                                : b.findColour (juce::TextButton::buttonColourId);
    if (down) c = c.darker (0.2f);
    else if (over) c = c.brighter (0.12f);
    g.setColour (c);
    g.fillRoundedRectangle (r, 5.0f);
}

void StackLook::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    g.setFont (getTextButtonFont (b, b.getHeight()));
    g.setColour (b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (8, 0), juce::Justification::centred, 1);
}

juce::Font StackLook::getTextButtonFont (juce::TextButton&, int h)
{
    return uiFont (std::min (14.0f, h * 0.55f), true);
}

//==============================================================================
Knob::Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const juce::String& captionText,
            juce::Colour colour)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.setColour (juce::Slider::rotarySliderFillColourId, colour);
    slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    addAndMakeVisible (slider);

    caption.setText (captionText, juce::dontSendNotification);
    caption.setJustificationType (juce::Justification::centred);
    caption.setFont (uiFont (11.5f, true));
    caption.setColour (juce::Label::textColourId, Colours::dim);
    caption.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (caption);

    value.setJustificationType (juce::Justification::centred);
    value.setFont (uiFont (11.5f));
    value.setColour (juce::Label::textColourId, Colours::text);
    value.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (value);

    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, paramId, slider);

    if (auto* param = state.getParameter (paramId))
    {
        auto update = [this, param]
        {
            value.setText (param->getText (param->convertTo0to1 ((float) slider.getValue()), 16), juce::dontSendNotification);
        };
        slider.onValueChange = update;
        update();
    }
    slider.setDoubleClickReturnValue (true, state.getParameterRange (paramId).convertFrom0to1 (state.getParameter (paramId)->getDefaultValue()));
}

void Knob::resized()
{
    auto r = getLocalBounds();
    caption.setBounds (r.removeFromTop (15));
    value.setBounds (r.removeFromBottom (15));
    slider.setBounds (r);
}

//==============================================================================
void PowerButton::paintButton (juce::Graphics& g, bool over, bool)
{
    const auto r = getLocalBounds().toFloat().reduced (2.0f);
    const bool on = getToggleState();
    g.setColour (on ? colour : Colours::panelEdge.brighter (over ? 0.3f : 0.15f));
    g.fillEllipse (r);
    if (on)
    {
        g.setColour (colour.withAlpha (0.35f));
        g.drawEllipse (r.expanded (1.5f), 2.0f);
    }
    // power glyph
    const auto c = r.getCentre();
    const float rad = r.getWidth() * 0.26f;
    juce::Path p;
    p.addCentredArc (c.x, c.y, rad, rad, 0.0f, 0.7f, juce::MathConstants<float>::twoPi - 0.7f, true);
    p.startNewSubPath (c.x, c.y - rad * 1.25f);
    p.lineTo (c.x, c.y - rad * 0.2f);
    g.setColour (on ? Colours::bg : Colours::dim);
    g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

//==============================================================================
DetectionDisplay::DetectionDisplay (SnareStackProcessor& p) : proc (p)
{
    history.assign (1500, DisplayPoint { -120.0f, std::numeric_limits<float>::quiet_NaN() });
}

void DetectionDisplay::push (const DisplayPoint* pts, int n)
{
    for (int i = 0; i < n; ++i)
    {
        history[(size_t) writePos] = pts[i];
        writePos = (writePos + 1) % (int) history.size();
    }
    if (n > 0) repaint();
}

void DetectionDisplay::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (Colours::bg);
    g.fillRoundedRectangle (r, 6.0f);

    constexpr float minDb = -72.0f, maxDb = 0.0f;
    auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (minDb, maxDb, db), minDb, maxDb, r.getBottom() - 2.0f, r.getY() + 4.0f); };

    // grid
    g.setFont (uiFont (10.0f));
    for (float db = -60.0f; db <= -12.0f; db += 12.0f)
    {
        const float y = yFor (db);
        g.setColour (Colours::panelEdge);
        g.drawHorizontalLine ((int) y, r.getX() + 30.0f, r.getRight() - 4.0f);
        g.setColour (Colours::dim.withAlpha (0.7f));
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (r.getX() + 4.0f, y - 6.0f, 24.0f, 12.0f), juce::Justification::left);
    }

    const int N = (int) history.size();
    const float x0 = r.getX() + 30.0f, w = r.getRight() - 4.0f - x0;
    auto xFor = [&] (int i) { return x0 + w * (float) i / (float) (N - 1); };

    // band level
    juce::Path level;
    level.startNewSubPath (x0, r.getBottom());
    for (int i = 0; i < N; ++i)
        level.lineTo (xFor (i), yFor (history[(size_t) ((writePos + i) % N)].levelDb));
    level.lineTo (x0 + w, r.getBottom());
    level.closeSubPath();
    g.setColour (Colours::detect.withAlpha (0.22f));
    g.fillPath (level);
    g.setColour (Colours::detect.withAlpha (0.9f));
    juce::Path outline;
    for (int i = 0; i < N; ++i)
    {
        const auto pt = juce::Point<float> (xFor (i), yFor (history[(size_t) ((writePos + i) % N)].levelDb));
        if (i == 0) outline.startNewSubPath (pt); else outline.lineTo (pt);
    }
    g.strokePath (outline, juce::PathStrokeType (1.2f));

    // floor
    const float floorDb = proc.apvts.getRawParameterValue ("floor")->load();
    const float fy = yFor (floorDb);
    juce::Path floorLine;
    floorLine.startNewSubPath (x0, fy);
    floorLine.lineTo (x0 + w, fy);
    juce::Path dashed;
    const float dashes[] = { 5.0f, 4.0f };
    juce::PathStrokeType (1.0f).createDashedStroke (dashed, floorLine, dashes, 2);
    g.setColour (Colours::text.withAlpha (0.45f));
    g.fillPath (dashed);
    g.drawText ("floor", juce::Rectangle<float> (x0 + w - 40.0f, fy - 13.0f, 38.0f, 12.0f), juce::Justification::right);

    // hits
    for (int i = 0; i < N; ++i)
    {
        const float hit = history[(size_t) ((writePos + i) % N)].hitDb;
        if (std::isnan (hit)) continue;
        const float strength = juce::jlimit (0.15f, 1.0f, 1.0f + hit / 36.0f);
        const float x = xFor (i);
        g.setColour (Colours::accent.withAlpha (0.35f + 0.65f * strength));
        g.fillRect (juce::Rectangle<float> (x - 1.0f, r.getY() + 4.0f + (1.0f - strength) * (r.getHeight() - 8.0f) * 0.6f,
                                            2.0f, r.getHeight() - 8.0f));
        g.fillEllipse (x - 3.0f, r.getY() + 3.0f, 6.0f, 6.0f);
    }

    g.setColour (Colours::panelEdge);
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
}

//==============================================================================
SlotStrip::SlotStrip (SnareStackProcessor& p, int slotIndex)
    : proc (p), index (slotIndex),
      level  (p.apvts, slotParamId (slotIndex, "gain"),  "LEVEL"),
      pitch  (p.apvts, slotParamId (slotIndex, "pitch"), "PITCH"),
      pan    (p.apvts, slotParamId (slotIndex, "pan"),   "PAN"),
      length (p.apvts, slotParamId (slotIndex, "len"),   "LENGTH")
{
    addAndMakeVisible (power);
    powerAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, slotParamId (slotIndex, "on"), power);
    power.onStateChange = [this] { repaint(); };
    power.setTooltip ("Layer on/off");

    nameButton.onClick = [this] { showMenu(); };
    nameButton.setTooltip ("Choose a built-in snare or load your own sample (you can also drag a file here)");
    addAndMakeVisible (nameButton);

    for (auto* k : { &level, &pitch, &pan, &length })
        addAndMakeVisible (*k);

    refresh();
}

void SlotStrip::refresh()
{
    nameButton.setButtonText (proc.getSlotName (index) + juce::String::fromUTF8 ("  \xe2\x96\xbe"));
    peaks = proc.getSlotPeaks (index);
    repaint();
}

void SlotStrip::tick()
{
    const int tc = proc.getSlotTriggerCount (index);
    if (tc != lastTriggerCount)
    {
        lastTriggerCount = tc;
        flash = 1.0f;
    }
    else if (flash > 0.0f)
    {
        flash = std::max (0.0f, flash - 0.12f);
    }
    else
    {
        return;
    }
    repaint (waveArea);
}

void SlotStrip::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    const bool on = power.getToggleState();

    g.setColour (Colours::panel);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (dragOver ? Colours::accent : Colours::panelEdge);
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, dragOver ? 2.0f : 1.0f);

    g.setColour (Colours::dim);
    g.setFont (uiFont (10.5f, true));
    g.drawText ("LAYER " + juce::String (index + 1), getLocalBounds().reduced (12, 8).removeFromTop (12), juce::Justification::topRight);

    // waveform
    const auto wa = waveArea.toFloat();
    g.setColour (Colours::bg);
    g.fillRoundedRectangle (wa, 5.0f);
    if (flash > 0.0f)
    {
        g.setColour (Colours::accent.withAlpha (0.25f * flash));
        g.fillRoundedRectangle (wa, 5.0f);
    }
    if (! peaks.empty())
    {
        const float mid = wa.getCentreY(), half = wa.getHeight() * 0.45f;
        juce::Path p;
        const int nb = (int) peaks.size();
        p.startNewSubPath (wa.getX() + 3.0f, mid);
        for (int i = 0; i < nb; ++i)
            p.lineTo (wa.getX() + 3.0f + (wa.getWidth() - 6.0f) * (float) i / (float) (nb - 1), mid - peaks[(size_t) i] * half);
        for (int i = nb - 1; i >= 0; --i)
            p.lineTo (wa.getX() + 3.0f + (wa.getWidth() - 6.0f) * (float) i / (float) (nb - 1), mid + peaks[(size_t) i] * half);
        p.closeSubPath();
        g.setColour ((on ? Colours::accent : Colours::dim).withAlpha (on ? 0.85f : 0.4f));
        g.fillPath (p);

        // length marker
        const float lenFrac = proc.apvts.getRawParameterValue (slotParamId (index, "len"))->load() * 0.01f;
        if (lenFrac < 0.995f)
        {
            const float x = wa.getX() + 3.0f + (wa.getWidth() - 6.0f) * lenFrac;
            g.setColour (Colours::bg.withAlpha (0.65f));
            g.fillRect (juce::Rectangle<float> (x, wa.getY(), wa.getRight() - x, wa.getHeight()).reduced (0.0f, 1.0f));
            g.setColour (Colours::text.withAlpha (0.5f));
            g.drawVerticalLine ((int) x, wa.getY() + 2.0f, wa.getBottom() - 2.0f);
        }
    }
    else
    {
        g.setColour (Colours::dim);
        g.setFont (uiFont (11.0f));
        g.drawText ("Drop a sample here", waveArea, juce::Justification::centred);
    }
}

void SlotStrip::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto top = r.removeFromTop (26);
    power.setBounds (top.removeFromLeft (26));
    top.removeFromLeft (6);
    top.removeFromRight (52);
    nameButton.setBounds (top);
    r.removeFromTop (8);
    waveArea = r.removeFromTop (46);
    r.removeFromTop (8);
    const int kw = r.getWidth() / 4;
    for (auto* k : { &level, &pitch, &pan, &length })
        k->setBounds (r.removeFromLeft (kw));
}

void SlotStrip::showMenu()
{
    juce::PopupMenu builtins;
    for (int i = 0; i < (int) BuiltinSnare::NumBuiltins; ++i)
        builtins.addItem (100 + i, getBuiltinName (i), true, proc.getSlotSource (index) == "builtin:" + juce::String (i));

    juce::PopupMenu m;
    m.addSectionHeader ("Layer " + juce::String (index + 1));
    m.addItem (1, "Load sample file...");
    m.addSubMenu ("Built-in snares", builtins);
    m.addSeparator();
    m.addItem (2, "Clear");

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&nameButton),
        [safe = juce::Component::SafePointer<SlotStrip> (this)] (int result)
        {
            if (safe == nullptr || result == 0) return;
            auto& self = *safe;
            if (result == 1)
            {
                self.chooser = std::make_unique<juce::FileChooser> ("Load a snare sample",
                                                                     juce::File::getSpecialLocation (juce::File::userHomeDirectory),
                                                                     self.proc.formatManager.getWildcardForAllFormats());
                self.chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                    [safe] (const juce::FileChooser& fc)
                    {
                        if (safe != nullptr && fc.getResult().existsAsFile())
                            safe->loadFile (fc.getResult());
                    });
            }
            else if (result == 2)
            {
                self.proc.clearSlot (self.index);
            }
            else if (result >= 100)
            {
                self.proc.loadBuiltin (self.index, result - 100);
            }
        });
}

void SlotStrip::loadFile (const juce::File& f)
{
    juce::String error;
    if (! proc.loadSampleFile (index, f, error))
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "SnareStack", error);
}

bool SlotStrip::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && proc.formatManager.findFormatForFileExtension (juce::File (files[0]).getFileExtension()) != nullptr;
}

void SlotStrip::filesDropped (const juce::StringArray& files, int, int)
{
    dragOver = false;
    if (! files.isEmpty()) loadFile (juce::File (files[0]));
    repaint();
}

//==============================================================================
SnareStackEditor::SnareStackEditor (SnareStackProcessor& p)
    : AudioProcessorEditor (p), proc (p), display (p),
      sens    (p.apvts, "sens",    "SENSITIVITY", Colours::detect),
      floor   (p.apvts, "floor",   "FLOOR",       Colours::detect),
      retrig  (p.apvts, "retrig",  "RETRIGGER",   Colours::detect),
      hpf     (p.apvts, "hpf",     "LOW CUT",     Colours::detect),
      lpf     (p.apvts, "lpf",     "HIGH CUT",    Colours::detect),
      kickRej (p.apvts, "kickrej", "KICK REJECT", Colours::detect),
      hatRej  (p.apvts, "hatrej",  "HAT REJECT",  Colours::detect),
      dry     (p.apvts, "dry",     "ORIGINAL"),
      wet     (p.apvts, "wet",     "SAMPLES"),
      dyn     (p.apvts, "dyn",     "DYNAMICS"),
      align   (p.apvts, "align",   "ALIGN"),
      midiNote (p.apvts, "midinote", "MIDI NOTE")
{
    setLookAndFeel (&look);

    addAndMakeVisible (display);
    for (auto* k : { &sens, &floor, &retrig, &hpf, &lpf, &kickRej, &hatRej, &dry, &wet, &dyn, &align, &midiNote })
        addAndMakeVisible (*k);

    listenButton.setClickingTogglesState (true);
    listenButton.setTooltip ("Hear what the detector hears (the band between Low Cut and High Cut)");
    addAndMakeVisible (listenButton);
    listenAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, "listen", listenButton);

    for (int s = 0; s < numSlots; ++s)
    {
        strips[(size_t) s] = std::make_unique<SlotStrip> (p, s);
        addAndMakeVisible (*strips[(size_t) s]);
    }

    scratch.resize (4096);
    setSize (920, 680);
    startTimerHz (30);
}

SnareStackEditor::~SnareStackEditor()
{
    setLookAndFeel (nullptr);
}

void SnareStackEditor::timerCallback()
{
    const int n = proc.readDisplay (scratch.data(), (int) scratch.size());
    display.push (scratch.data(), n);

    const int hc = proc.getHitCount();
    if (hc != lastHitCount) { lastHitCount = hc; ledLevel = 1.0f; repaint (getLocalBounds().removeFromTop (52)); }
    else if (ledLevel > 0.0f) { ledLevel = std::max (0.0f, ledLevel - 0.15f); repaint (getLocalBounds().removeFromTop (52)); }

    const int v = proc.slotsVersion.load();
    const bool slotsChanged = v != lastSlotsVersion;
    lastSlotsVersion = v;
    for (auto& s : strips)
    {
        if (slotsChanged) s->refresh();
        s->tick();
    }
}

void SnareStackEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::bg);

    // header
    auto header = getLocalBounds().removeFromTop (52).reduced (20, 0);
    {
        // logo: three stacked bars
        auto logo = header.removeFromLeft (28).toFloat().withSizeKeepingCentre (24.0f, 22.0f);
        for (int i = 0; i < 3; ++i)
        {
            g.setColour (i == 0 ? Colours::accent : Colours::accent.withAlpha (0.55f - 0.18f * i));
            g.fillRoundedRectangle (logo.getX() + i * 2.0f, logo.getY() + i * 8.0f, logo.getWidth() - i * 4.0f, 6.0f, 2.0f);
        }
        header.removeFromLeft (8);
        g.setColour (Colours::text);
        g.setFont (uiFont (20.0f, true));
        g.drawText ("SNARESTACK", header.removeFromLeft (150), juce::Justification::centredLeft);
        g.setColour (Colours::dim);
        g.setFont (uiFont (12.0f));
        g.drawText ("detect - layer - blend", header.removeFromLeft (180), juce::Justification::centredLeft);

        // hit LED + latency
        auto right = header.removeFromRight (230);
        auto led = right.removeFromRight (18).toFloat().withSizeKeepingCentre (12.0f, 12.0f);
        g.setColour (Colours::panelEdge);
        g.fillEllipse (led);
        g.setColour (Colours::accent.withAlpha (ledLevel));
        g.fillEllipse (led);
        if (ledLevel > 0.0f)
        {
            g.setColour (Colours::accent.withAlpha (0.3f * ledLevel));
            g.fillEllipse (led.expanded (4.0f));
        }
        right.removeFromRight (8);
        g.setColour (Colours::dim);
        g.setFont (uiFont (11.5f));
        g.drawText ("latency " + juce::String (proc.getLatencyMs()) + " ms (auto-compensated)  |  HIT",
                    right, juce::Justification::centredRight);
    }

    auto drawPanel = [&] (juce::Rectangle<int> area, const juce::String& title, juce::Colour c)
    {
        g.setColour (Colours::panel.withAlpha (0.55f));
        g.fillRoundedRectangle (area.toFloat(), 10.0f);
        g.setColour (Colours::panelEdge);
        g.drawRoundedRectangle (area.toFloat().reduced (0.5f), 10.0f, 1.0f);
        g.setColour (c);
        g.setFont (uiFont (11.5f, true));
        g.drawText (title, area.reduced (14, 8).removeFromTop (14), juce::Justification::topLeft);
    };
    drawPanel (detectPanel, "DETECTION", Colours::detect);
    drawPanel (layersPanel, "LAYERS", Colours::accent);
    drawPanel (mixPanel, "BLEND", Colours::accent);

    g.setColour (Colours::dim);
    g.setFont (uiFont (11.0f));
    auto hint = mixPanel.reduced (16, 10);
    hint.removeFromLeft (5 * 92 + 10);
    g.drawFittedText ("Tune Sensitivity until every snare hit gets an orange marker and nothing else does. "
                      "Raise Floor to ignore quiet bleed; use Listen to hear the detection band. "
                      "Every hit is also sent out as MIDI (channel 10).",
                      hint, juce::Justification::centredLeft, 4);
}

void SnareStackEditor::resized()
{
    auto r = getLocalBounds().reduced (14);
    r.removeFromTop (40);

    detectPanel = r.removeFromTop (238);
    r.removeFromTop (10);
    layersPanel = r.removeFromTop (230);
    r.removeFromTop (10);
    mixPanel = r;

    {
        auto a = detectPanel.reduced (14, 10);
        a.removeFromTop (20);
        display.setBounds (a.removeFromTop (110));
        a.removeFromTop (8);
        auto row = a;
        listenButton.setBounds (row.removeFromRight (86).withSizeKeepingCentre (80, 28));
        const int kw = row.getWidth() / 7;
        for (auto* k : { &sens, &floor, &retrig, &hpf, &lpf, &kickRej, &hatRej })
            k->setBounds (row.removeFromLeft (kw).reduced (4, 0));
    }
    {
        auto a = layersPanel.reduced (10, 8);
        a.removeFromTop (20);
        const int w = (a.getWidth() - 3 * 8) / 4;
        for (int s = 0; s < numSlots; ++s)
        {
            strips[(size_t) s]->setBounds (a.removeFromLeft (w));
            a.removeFromLeft (8);
        }
    }
    {
        auto a = mixPanel.reduced (16, 8);
        a.removeFromTop (14);
        for (auto* k : { &dry, &wet, &dyn, &align, &midiNote })
            k->setBounds (a.removeFromLeft (92).reduced (6, 0));
    }
}

} // namespace snarestack
