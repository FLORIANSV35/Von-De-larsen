#include "VdlLookAndFeel.h"

#include "VdlArt.h"

namespace vdl {
using namespace juce;

namespace {
const Colour kLedOff { 0xff35100f };
const Colour kDim { 0xffa05a5a };

void ring(Graphics& g, Point<float> c, float r, float thickness)
{
    g.drawEllipse(c.x - r, c.y - r, r * 2.f, r * 2.f, thickness);
}

void glassDisc(Graphics& g, Point<float> c, float r)
{
    g.setGradientFill(ColourGradient(Colour(0xff2a1316), c.x - r * 0.3f, c.y - r * 0.4f, Colour(0xff050506),
                                     c.x + r * 0.7f, c.y + r * 0.7f, true));
    g.fillEllipse(c.x - r, c.y - r, r * 2.f, r * 2.f);
    g.setColour(kBlack);
    ring(g, c, r, 3.f);
}

void ledTicks(Graphics& g, Point<float> c, float r0, float r1, float rMajor, int n, int majorEvery, float pos,
              float a0, float a1, float thick)
{
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        const float a = a0 + t * (a1 - a0);
        const bool major = (i % majorEvery) == 0;
        const Line<float> l(c.getPointOnCircumference(r0, a), c.getPointOnCircumference(major ? rMajor : r1, a));
        if (t <= pos + 1.0e-4f) {
            g.setColour(kHotRed.withAlpha(0.22f));
            g.drawLine(l, thick * 1.9f);
            g.setColour(kHotRed);
        } else {
            g.setColour(kLedOff);
        }
        g.drawLine(l, thick);
    }
}
} // namespace

LookAndFeel::LookAndFeel()
{
    setColour(TooltipWindow::backgroundColourId, kBlack);
    setColour(TooltipWindow::textColourId, kCream);
    setColour(TooltipWindow::outlineColourId, kRed);
}

void LookAndFeel::drawRotarySlider(Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, Slider& s)
{
    const bool big = static_cast<int>(s.getProperties()["kind"]) == 0;
    const float strip = big ? 40.f : 34.f;
    const Rectangle<float> b(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h));
    const float d = jmin(b.getWidth(), b.getHeight() - strip);
    const Point<float> c(b.getCentreX(), b.getY() + d * 0.5f);
    const float outer = d * 0.5f - 2.f;
    const float ang = a0 + pos * (a1 - a0);

    g.setColour(kBlack);
    g.fillEllipse(c.x - outer, c.y - outer, outer * 2.f, outer * 2.f);
    g.setColour(kRed);
    ring(g, c, outer - (big ? 1.5f : 1.25f), big ? 3.f : 2.5f);

    if (big) {
        const float R = outer * 0.931f, sc = R / 108.f;
        const float r2 = outer * 0.897f;
        g.setColour(Colour(0xff101014));
        g.fillEllipse(c.x - r2, c.y - r2, r2 * 2.f, r2 * 2.f);
        g.setColour(Colour(0xff2a2a30));
        ring(g, c, r2, 2.f);
        ledTicks(g, c, 0.8f * R, 0.93f * R, 0.96f * R, 41, 5, pos, a0, a1, 5.f * sc);
        glassDisc(g, c, 0.74f * R);
        g.setColour(kHotRed.withAlpha(0.22f));
        for (float f : { 0.25f, 0.5f, 0.72f }) ring(g, c, R * f, 1.f);
        g.setColour(kHotRed.withAlpha(0.18f));
        g.drawLine(c.x - 0.74f * R, c.y, c.x + 0.74f * R, c.y, 1.f);
        g.drawLine(c.x, c.y - 0.74f * R, c.x, c.y + 0.74f * R, 1.f);

        Graphics::ScopedSaveState ss(g);
        Path clip;
        clip.addEllipse(c.x - 0.73f * R, c.y - 0.73f * R, 1.46f * R, 1.46f * R);
        g.reduceClipRegion(clip);
        {
            Graphics::ScopedSaveState s2(g);
            g.addTransform(AffineTransform::rotation(ang, c.x, c.y));
            drawRocket(g, c, R * 0.0092f, pos);
        }
        g.addTransform(AffineTransform::rotation(degreesToRadians(-25.f), c.x - 0.25f * R, c.y - 0.45f * R));
        g.setColour(Colours::white.withAlpha(0.05f));
        g.fillEllipse(c.x - 0.6f * R, c.y - 0.58f * R, 0.7f * R, 0.26f * R);
    } else {
        const float r = outer * 0.903f, sc = r / 56.f;
        glassDisc(g, c, r * 0.62f);
        ledTicks(g, c, 0.74f * r, 0.95f * r, 0.98f * r, 21, 5, pos, a0, a1, 4.f * sc);
        {
            Graphics::ScopedSaveState ss(g);
            g.addTransform(AffineTransform::rotation(ang, c.x, c.y));
            Path tri;
            tri.addTriangle(c.x, c.y - 0.54f * r, c.x - 5.f * sc, c.y, c.x + 5.f * sc, c.y);
            g.setColour(kCream);
            g.fillPath(tri);
            g.setColour(kBlack);
            g.strokePath(tri, PathStrokeType(1.5f));
        }
        g.setColour(kRed);
        g.fillEllipse(c.x - 5.f * sc, c.y - 5.f * sc, 10.f * sc, 10.f * sc);
        g.setColour(kBlack);
        ring(g, c, 5.f * sc, 1.5f);
    }

    const float bw = big ? 124.f : 76.f, bh = big ? 30.f : 24.f;
    const Rectangle<float> box(c.x - bw * 0.5f, c.y + outer + (big ? 8.f : 10.f), bw, bh);
    g.setColour(kBlack);
    g.fillRect(box);
    g.setColour(kRed);
    g.drawRect(box, big ? 2.f : 1.8f);
    g.setColour(kHotRed);
    g.setFont(monoFont(big ? 24.f : 17.f).withExtraKerningFactor(0.08f));
    g.drawText(s.getTextFromValue(s.getValue()), box, Justification::centred);
}

void LookAndFeel::drawLinearSlider(Graphics& g, int x, int y, int w, int h, float sliderPos, float, float,
                                   Slider::SliderStyle, Slider& s)
{
    if (static_cast<int>(s.getProperties()["kind"]) == 2) { // compact fader (voice gate depth)
        const float thumbR = static_cast<float>(getSliderThumbRadius(s));
        const float cy = static_cast<float>(y) + static_cast<float>(h) * 0.5f;
        const float x0 = static_cast<float>(x) + thumbR, x1 = static_cast<float>(x + w) - thumbR;
        const bool enabled = s.isEnabled();
        if (!enabled) g.beginTransparencyLayer(0.4f);
        g.setColour(kDim);
        for (int i = 0; i <= 10; ++i) {
            const float tx = x0 + static_cast<float>(i) * (x1 - x0) / 10.f;
            g.drawLine(tx, cy - ((i % 5) == 0 ? 11.f : 9.f), tx, cy - 7.f, 1.2f);
        }
        g.setColour(kBlack);
        g.fillRect(x0, cy - 4.f, x1 - x0, 8.f);
        g.setColour(kRed);
        g.drawRect(x0, cy - 4.f, x1 - x0, 8.f, 1.5f);
        g.setColour(kHotRed);
        g.fillRect(x0 + 1.5f, cy - 2.5f, jmax(0.f, sliderPos - x0 - 1.5f), 5.f);
        const Rectangle<float> th(sliderPos - 7.f, cy - 12.f, 14.f, 24.f);
        g.setColour(Colour(0xff1a1a1f));
        g.fillRect(th);
        g.setColour(kRed);
        g.fillRect(th.getX(), th.getY(), th.getWidth(), 7.f);
        g.drawRect(th, 2.f);
        g.setColour(kCream);
        g.drawLine(th.getX(), cy, th.getRight(), cy, 2.f);
        if (!enabled) g.endTransparencyLayer();
        return;
    }

    const float cy = static_cast<float>(y) + static_cast<float>(h) * 0.5f;
    const float thumbR = static_cast<float>(getSliderThumbRadius(s));
    const float x0 = static_cast<float>(x) + thumbR, x1 = static_cast<float>(x + w) - thumbR;

    g.setColour(kDim);
    for (int i = 0; i <= 20; ++i) {
        const float tx = x0 + static_cast<float>(i) * (x1 - x0) / 20.f;
        g.drawLine(tx, cy - ((i % 5) == 0 ? 18.f : 14.f), tx, cy - 8.f, 1.5f);
    }
    g.setColour(kBlack);
    g.fillRect(x0, cy - 6.f, x1 - x0, 12.f);
    g.setColour(kRed);
    g.drawRect(x0, cy - 6.f, x1 - x0, 12.f, 2.f);
    g.setColour(kHotRed);
    g.fillRect(x0 + 2.f, cy - 3.f, jmax(0.f, sliderPos - x0 - 2.f), 6.f);

    const Rectangle<float> th(sliderPos - 11.f, cy - 22.f, 22.f, 44.f);
    g.setColour(Colour(0xff1a1a1f));
    g.fillRect(th);
    g.setColour(kRed);
    g.fillRect(th.getX(), th.getY(), th.getWidth(), 12.f);
    g.drawRect(th, 2.5f);
    g.setColour(kCream);
    g.drawLine(th.getX(), cy, th.getRight(), cy, 2.5f);
}

void LookAndFeel::drawToggleButton(Graphics& g, ToggleButton& btn, bool highlighted, bool)
{
    const auto b = btn.getLocalBounds().toFloat();
    const bool on = btn.getToggleState();

    if (btn.getComponentID() == "bypass") { // round power button: lit red when the processing is bypassed
        const auto r = b.reduced(1.f);
        g.setColour(on ? kRed : kBlack);
        g.fillEllipse(r);
        g.setColour(highlighted ? kHotRed : kRed);
        g.drawEllipse(r, 2.f);
        const auto c = r.getCentre();
        const float rad = r.getWidth() * 0.27f;
        Path icon;
        icon.addCentredArc(c.x, c.y + 0.5f, rad, rad, 0.f, degreesToRadians(40.f), degreesToRadians(320.f), true);
        icon.startNewSubPath(c.x, c.y - rad - 1.5f);
        icon.lineTo(c.x, c.y - 1.f);
        g.setColour(on ? kCream : kHotRed);
        g.strokePath(icon, PathStrokeType(2.2f, PathStrokeType::curved, PathStrokeType::rounded));
        return;
    }

    g.setColour(kBlack);
    g.fillRect(b);
    g.setColour(highlighted ? kHotRed : kRed);
    g.drawRect(b.reduced(1.f), 2.f);

    const Rectangle<float> led(b.getX() + 5.f, b.getCentreY() - 10.f, 20.f, 20.f);
    if (on) {
        g.setColour(kHotRed.withAlpha(0.3f));
        g.fillRect(led.expanded(4.f));
        g.setColour(kHotRed);
    } else {
        g.setColour(kLedOff);
    }
    g.fillRect(led);

    g.setColour(on ? kHotRed : Colour(0xff6b2a2a));
    g.setFont(monoFont(12.5f).withExtraKerningFactor(0.18f));
    if (btn.getComponentID() == "learn") {
        g.setColour(on ? kHotRed : Colour(0xff6b2a2a));
        g.setFont(monoFont(11.5f).withExtraKerningFactor(0.1f));
        g.drawText(on ? String("APPRENTISSAGE EN COURS") : btn.getButtonText(), b.withTrimmedLeft(30.f), Justification::centred);
        return;
    }
    const bool feminine = btn.getComponentID() == "gate";
    const String state = on ? (feminine ? "ACTIVE" : "ACTIF") : String::fromUTF8(feminine ? "COUP\xc3\x89""E" : "COUP\xc3\x89");
    g.drawText(btn.getButtonText() + String::fromUTF8(" \xe2\x80\xa2 ") + state, b.withTrimmedLeft(30.f), Justification::centred);
}

void LookAndFeel::drawButtonBackground(Graphics& g, Button& b, const Colour&, bool highlighted, bool down)
{
    const auto r = b.getLocalBounds().toFloat().reduced(1.f);
    if (b.getComponentID() == "help") {
        g.setColour(b.getToggleState() ? kRed : kBlack);
        g.fillEllipse(r);
        g.setColour(highlighted || down ? kHotRed : kRed);
        g.drawEllipse(r, 2.f);
    } else {
        g.setColour(kBlack);
        g.fillRect(r);
        g.setColour(highlighted ? kHotRed : kDarkRed.brighter(0.4f));
        g.drawRect(r, 1.5f);
    }
}

void LookAndFeel::drawButtonText(Graphics& g, TextButton& b, bool highlighted, bool)
{
    const auto r = b.getLocalBounds().toFloat();
    if (b.getComponentID() == "help") {
        g.setColour(b.getToggleState() ? kCream : kHotRed);
        g.setFont(titleFont(r.getHeight() * 0.85f));
        g.drawText("?", r, Justification::centred);
    } else {
        g.setColour(highlighted ? kCream : kDim);
        g.setFont(monoFont(b.getComponentID() == "step" ? 15.f : 10.f).withExtraKerningFactor(0.15f));
        g.drawText(b.getButtonText(), r, Justification::centred);
    }
}

} // namespace vdl
