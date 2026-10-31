// Vector artwork for the Von De Larsen editor: rocket-with-mic knob, brain, chisel.
// Everything is drawn with juce::Path, no image assets.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace vdl {

inline const juce::Colour kBlack   { 0xff0b0b0e };
inline const juce::Colour kInk     { 0xff18181d };
inline const juce::Colour kRed     { 0xffd4181f };
inline const juce::Colour kHotRed  { 0xffff3b3b };
inline const juce::Colour kDarkRed { 0xff5c0a10 };
inline const juce::Colour kCream   { 0xfff6eedb };
inline const juce::Colour kYellow  { 0xffffd23f };
inline const juce::Colour kSteel   { 0xffb9bec6 };

inline juce::String T(const char* utf8) { return juce::String::fromUTF8(utf8); }

// Condensed display face: Oswald if installed, otherwise a condensed face that ships with macOS.
inline juce::Font titleFont(float h)
{
    static const juce::String name = [] {
        const auto all = juce::Font::findAllTypefaceNames();
        for (const char* n : { "Oswald", "DIN Condensed", "Avenir Next Condensed", "Impact" })
            if (all.contains(n)) return juce::String(n);
        return juce::String();
    }();
    return name.isNotEmpty() ? juce::Font(juce::FontOptions(name, h, juce::Font::bold))
                             : juce::Font(juce::FontOptions(h, juce::Font::bold));
}

inline juce::Font monoFont(float h, bool bold = true)
{
    return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), h,
                                        bold ? juce::Font::bold : juce::Font::plain));
}

inline juce::Font bodyFont(float h, bool bold = true)
{
    return juce::Font(juce::FontOptions(h, bold ? juce::Font::bold : juce::Font::plain));
}

// Rocket drawn pointing up, centred on (centre). k = pixels per design unit (rocket ~108 units tall).
// flame 0..1 sets the exhaust length.
inline void drawRocket(juce::Graphics& g, juce::Point<float> centre, float k, float flame)
{
    using namespace juce;
    Graphics::ScopedSaveState ss(g);
    g.addTransform(AffineTransform::translation(0.f, 10.f).scaled(k).translated(centre.x, centre.y));

    const PathStrokeType outline(2.2f, PathStrokeType::curved, PathStrokeType::rounded);

    // --- fins (behind the body)
    auto fin = [&](float sx) {
        Path p;
        p.startNewSubPath(sx * 15.f, 6.f);
        p.cubicTo(sx * 28.f, 16.f, sx * 34.f, 32.f, sx * 35.f, 46.f);
        p.lineTo(sx * 24.f, 43.f);
        p.lineTo(sx * 15.f, 33.f);
        p.closeSubPath();
        g.setColour(kRed);
        g.fillPath(p);
        g.setColour(kBlack);
        g.strokePath(p, outline);
    };
    fin(-1.f);
    fin(1.f);
    {
        Path p;
        p.startNewSubPath(-5.f, 28.f);
        p.lineTo(0.f, 50.f);
        p.lineTo(5.f, 28.f);
        p.closeSubPath();
        g.setColour(kDarkRed);
        g.fillPath(p);
        g.setColour(kBlack);
        g.strokePath(p, outline);
    }

    // --- exhaust flame
    {
        const float L = 5.f + 17.f * flame;
        Path f;
        f.startNewSubPath(-8.f, 38.f);
        f.cubicTo(-9.f, 38.f + L * 0.6f, -2.f, 38.f + L * 0.8f, 0.f, 38.f + L);
        f.cubicTo(2.f, 38.f + L * 0.8f, 9.f, 38.f + L * 0.6f, 8.f, 38.f);
        f.closeSubPath();
        g.setGradientFill(ColourGradient(kYellow, 0.f, 38.f, kHotRed, 0.f, 38.f + L, false));
        g.fillPath(f);
        g.setColour(kBlack);
        g.strokePath(f, PathStrokeType(1.6f, PathStrokeType::curved, PathStrokeType::rounded));
        Path core;
        core.startNewSubPath(-4.f, 38.f);
        core.quadraticTo(-3.f, 38.f + L * 0.55f, 0.f, 38.f + L * 0.62f);
        core.quadraticTo(3.f, 38.f + L * 0.55f, 4.f, 38.f);
        core.closeSubPath();
        g.setColour(kCream.withAlpha(0.9f));
        g.fillPath(core);
    }

    // --- nozzle
    {
        Path n;
        n.startNewSubPath(-11.f, 34.f);
        n.lineTo(11.f, 34.f);
        n.lineTo(8.f, 40.f);
        n.lineTo(-8.f, 40.f);
        n.closeSubPath();
        g.setColour(Colour(0xff3a3a42));
        g.fillPath(n);
        g.setColour(kBlack);
        g.strokePath(n, outline);
    }

    auto shade = [&](const Path& p) {
        Graphics::ScopedSaveState s2(g);
        g.reduceClipRegion(p);
        ColourGradient sh(Colours::black.withAlpha(0.5f), -18.f, 0.f, Colours::black.withAlpha(0.55f), 18.f, 0.f, false);
        sh.addColour(0.28, Colours::transparentBlack);
        sh.addColour(0.62, Colours::transparentBlack);
        g.setGradientFill(sh);
        g.fillRect(-20.f, -50.f, 40.f, 100.f);
    };

    // --- checkered body
    Path body;
    body.addRectangle(-18.f, -16.f, 36.f, 52.f);
    {
        Graphics::ScopedSaveState s2(g);
        g.reduceClipRegion(body);
        g.setColour(kCream);
        g.fillRect(-18.f, -16.f, 36.f, 52.f);
        g.setColour(kRed);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c)
                if (((r + c) & 1) == 0) g.fillRect(-18.f + 12.f * (float) c, -16.f + 13.f * (float) r, 12.f, 13.f);
    }
    shade(body);
    g.setColour(Colours::white.withAlpha(0.22f));
    g.fillRoundedRectangle(-12.f, -12.f, 3.5f, 44.f, 1.7f);
    g.setColour(kBlack);
    g.strokePath(body, outline);

    // --- porthole
    g.setColour(kBlack);
    g.fillEllipse(-8.f, -5.f, 16.f, 16.f);
    g.setGradientFill(ColourGradient(Colour(0xfffff3b0), -2.f, 0.f, Colour(0xff6fb7d8), 4.f, 9.f, true));
    g.fillEllipse(-5.8f, -2.8f, 11.6f, 11.6f);
    g.setColour(Colours::white.withAlpha(0.8f));
    g.fillEllipse(-3.6f, -1.6f, 4.f, 2.6f);

    // --- red nose cone
    Path nose;
    nose.startNewSubPath(-18.f, -16.f);
    nose.cubicTo(-17.f, -30.f, -9.f, -38.f, -5.f, -42.f);
    nose.lineTo(5.f, -42.f);
    nose.cubicTo(9.f, -38.f, 17.f, -30.f, 18.f, -16.f);
    nose.closeSubPath();
    g.setColour(kRed);
    g.fillPath(nose);
    shade(nose);
    g.setColour(kBlack);
    g.strokePath(nose, outline);

    // --- microphone windscreen: a big round foam ball that swallows the whole nose cone, held by a red ring
    Path foam;
    foam.addEllipse(-28.f, -64.f, 56.f, 56.f);
    g.setGradientFill(ColourGradient(Colour(0xff5e5e66), -10.f, -50.f, Colour(0xff111114), 18.f, -14.f, true));
    g.fillPath(foam);
    static const float pores[][3] = {
        { -8.f, -58.f, 1.2f },
        { 5.f, -60.f, 1.f },
        { -15.f, -50.f, 1.3f },
        { -2.f, -52.f, 1.1f },
        { 10.f, -52.f, 1.2f },
        { 17.f, -44.f, 1.1f },
        { -18.f, -40.f, 1.2f },
        { -9.f, -42.f, 1.f },
        { 3.f, -44.f, 1.3f },
        { 12.f, -36.f, 1.f },
        { -14.f, -30.f, 1.2f },
        { -4.f, -34.f, 1.1f },
        { 6.f, -28.f, 1.3f },
        { 18.f, -26.f, 1.f },
        { -19.f, -22.f, 1.1f },
        { -9.f, -24.f, 1.f },
        { 0.f, -21.f, 1.2f },
        { 13.f, -20.f, 1.1f },
        { -3.f, -58.f, 0.8f },
        { 14.f, -48.f, 0.9f },
        { -12.f, -36.f, 0.9f },
        { 8.f, -42.f, 0.8f },
        { -6.f, -47.f, 0.9f },
        { 2.f, -30.f, 0.9f },
        { -16.f, -44.f, 0.8f },
        { 19.f, -35.f, 0.8f },
        { -24.f, -36.f, 1.1f },
        { 24.f, -34.f, 1.f },
        { 22.f, -46.f, 1.f },
        { -22.f, -46.f, 1.f },
        { -23.f, -28.f, 0.9f },
        { 22.f, -24.f, 0.9f },
        { 0.f, -33.f, 0.8f },
        { -10.f, -56.f, 0.8f } };
    for (auto& p : pores) {
        g.setColour(Colours::black.withAlpha(0.65f));
        g.fillEllipse(p[0] - p[2], p[1] - p[2], p[2] * 2.f, p[2] * 2.f);
        g.setColour(Colours::white.withAlpha(0.14f));
        g.fillEllipse(p[0] - p[2] + 0.9f, p[1] - p[2] - 0.9f, p[2] * 1.2f, p[2] * 1.2f);
    }
    g.setColour(Colours::white.withAlpha(0.28f));
    Path hl;
    hl.addCentredArc(0.f, -36.f, 22.f, 22.f, 0.f, degreesToRadians(-75.f), degreesToRadians(-25.f), true);
    g.strokePath(hl, PathStrokeType(2.2f, PathStrokeType::curved, PathStrokeType::rounded));
    g.setColour(kBlack);
    g.strokePath(foam, PathStrokeType(2.f, PathStrokeType::curved, PathStrokeType::rounded));
    g.setColour(kRed);
    g.fillRoundedRectangle(-21.f, -17.f, 42.f, 6.f, 2.f);
    g.setColour(kBlack);
    g.drawRoundedRectangle(-21.f, -17.f, 42.f, 6.f, 2.f, 1.6f);
}

// Cartoon brain, r should be roughly square. glow 0..1 brightens it and adds a lightning bolt.
inline void drawBrain(juce::Graphics& g, juce::Rectangle<float> r, float glow)
{
    using namespace juce;
    Graphics::ScopedSaveState ss(g);
    g.addTransform(AffineTransform::scale(r.getWidth() / 100.f).translated(r.getCentreX(), r.getCentreY()));

    if (glow > 0.03f) {
        g.setGradientFill(ColourGradient(kHotRed.withAlpha(0.55f * glow), 0.f, 0.f, Colours::transparentBlack, 0.f, 62.f, true));
        g.fillEllipse(-64.f, -64.f, 128.f, 128.f);
    }

    struct E { float cx, cy, rx, ry; };
    static const E lobes[] = { { -22, -8, 26, 24 }, { 20, -10, 28, 26 }, { 0, -24, 26, 20 },
                               { -34, 10, 18, 16 }, { 30, 12, 20, 16 }, { -4, 8, 34, 22 } };
    Path all;
    for (auto& e : lobes) all.addEllipse(e.cx - e.rx, e.cy - e.ry, e.rx * 2.f, e.ry * 2.f);
    all.addEllipse(6.f, 18.f, 32.f, 20.f);                       // cerebellum
    all.addRoundedRectangle(-4.f, 24.f, 12.f, 22.f, 4.f);        // brain stem

    g.setColour(kBlack);
    g.strokePath(all, PathStrokeType(7.f, PathStrokeType::curved, PathStrokeType::rounded));
    g.setGradientFill(ColourGradient(Colour(0xffff9aa8).interpolatedWith(kCream, 0.2f * glow), 0.f, -44.f,
                                     Colour(0xffd23a52), 0.f, 36.f, false));
    g.fillPath(all);

    g.setColour(Colour(0xff8e1020));
    const PathStrokeType fold(2.3f, PathStrokeType::curved, PathStrokeType::rounded);
    Path f;
    f.startNewSubPath(0.f, -40.f); f.cubicTo(-5.f, -22.f, 5.f, -10.f, 0.f, 10.f);
    f.startNewSubPath(-36.f, -8.f); f.quadraticTo(-28.f, -22.f, -18.f, -12.f); f.quadraticTo(-10.f, -4.f, -16.f, 8.f);
    f.startNewSubPath(-40.f, 10.f); f.quadraticTo(-32.f, 2.f, -24.f, 12.f); f.quadraticTo(-18.f, 20.f, -22.f, 24.f);
    f.startNewSubPath(8.f, -30.f);  f.quadraticTo(16.f, -40.f, 24.f, -30.f); f.quadraticTo(32.f, -22.f, 24.f, -14.f);
    f.startNewSubPath(10.f, -6.f);  f.quadraticTo(18.f, -14.f, 28.f, -4.f); f.quadraticTo(36.f, 4.f, 28.f, 12.f);
    f.startNewSubPath(12.f, 14.f);  f.quadraticTo(20.f, 8.f, 30.f, 18.f); f.quadraticTo(38.f, 24.f, 44.f, 18.f);
    g.strokePath(f, fold);

    if (glow > 0.45f) {
        Path bolt;
        bolt.startNewSubPath(5.f, -34.f); bolt.lineTo(-9.f, -4.f); bolt.lineTo(0.f, -4.f);
        bolt.lineTo(-7.f, 22.f);          bolt.lineTo(13.f, -9.f); bolt.lineTo(3.f, -9.f);
        bolt.lineTo(11.f, -34.f);         bolt.closeSubPath();
        g.setColour(kYellow.withAlpha(jmin(1.f, (glow - 0.45f) * 2.2f)));
        g.fillPath(bolt);
        g.setColour(kBlack.withAlpha(jmin(1.f, (glow - 0.45f) * 2.2f)));
        g.strokePath(bolt, PathStrokeType(1.5f, PathStrokeType::curved, PathStrokeType::rounded));
    }
}

// Wood chisel (a "ciseau"), tip pointing to the lower-left.
inline void drawChisel(juce::Graphics& g, juce::Rectangle<float> r, float glow)
{
    using namespace juce;
    Graphics::ScopedSaveState ss(g);
    g.addTransform(AffineTransform::rotation(degreesToRadians(45.f)).scaled(r.getWidth() / 100.f).translated(r.getCentreX(), r.getCentreY()));

    const PathStrokeType outline(2.2f, PathStrokeType::curved, PathStrokeType::rounded);

    // handle
    Path handle;
    handle.addRoundedRectangle(-9.f, -48.f, 18.f, 42.f, 6.f);
    g.setGradientFill(ColourGradient(kHotRed, -9.f, 0.f, kDarkRed, 9.f, 0.f, false));
    g.fillPath(handle);
    g.setColour(kBlack);
    g.strokePath(handle, outline);
    g.setColour(kBlack);
    for (float y : { -40.f, -34.f, -28.f }) g.fillRect(-9.f, y, 18.f, 2.2f);
    g.setColour(Colours::white.withAlpha(0.25f));
    g.fillRoundedRectangle(-6.f, -44.f, 3.f, 34.f, 1.5f);

    // ferrule
    g.setColour(kSteel);
    g.fillRoundedRectangle(-10.f, -8.f, 20.f, 6.f, 2.f);
    g.setColour(kBlack);
    g.drawRoundedRectangle(-10.f, -8.f, 20.f, 6.f, 2.f, 1.8f);

    // blade
    Path blade;
    blade.startNewSubPath(-5.f, -3.f); blade.lineTo(5.f, -3.f); blade.lineTo(10.f, 38.f); blade.lineTo(-10.f, 38.f);
    blade.closeSubPath();
    ColourGradient bg(Colour(0xff8c929c), -10.f, 0.f, Colour(0xff8c929c), 10.f, 0.f, false);
    bg.addColour(0.45, Colour(0xfff1f4f8));
    g.setGradientFill(bg);
    g.fillPath(blade);
    g.setColour(kBlack);
    g.strokePath(blade, outline);

    Path bevel;
    bevel.startNewSubPath(-10.f, 38.f); bevel.lineTo(10.f, 38.f); bevel.lineTo(8.f, 47.f); bevel.lineTo(-8.f, 47.f);
    bevel.closeSubPath();
    g.setColour(Colour(0xffdfe4ea));
    g.fillPath(bevel);
    g.setColour(kBlack);
    g.strokePath(bevel, outline);

    if (glow > 0.05f) {
        Path sp;
        sp.addStar(Point<float>(-2.f, 52.f), 4, 1.6f, 9.f * glow + 2.f, 0.f);
        g.setColour(kCream.withAlpha(glow));
        g.fillPath(sp);
    }
}

} // namespace vdl
