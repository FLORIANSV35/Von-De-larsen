#include "VdlEditor.h"

#include "VdlArt.h"

using namespace juce;
using namespace vdl;

namespace {
const Colour kDim { 0xffa05a5a };
const Colour kPanel { 0xff120b0d };
const Colour kEdge { 0xff3a1014 };

String groupInt(double v)
{
    const auto s = String(static_cast<int64>(std::llround(jmax(0.0, v))));
    String out;
    const int n = s.length();
    for (int i = 0; i < n; ++i) {
        out += s.substring(i, i + 1);
        if (((n - i - 1) % 3) == 0 && i < n - 1) out += " ";
    }
    return out;
}

String clock(double seconds, bool forceHours)
{
    const int t = static_cast<int>(jmax(0.0, seconds));
    const int h = t / 3600, m = (t / 60) % 60, s = t % 60;
    return (h > 0 || forceHours) ? String::formatted("%d:%02d:%02d", h, m, s) : String::formatted("%02d:%02d", m, s);
}

void label(Graphics& g, const String& text, Rectangle<float> r, const Font& f, Colour c, Justification j = Justification::centred)
{
    g.setColour(c);
    g.setFont(f);
    g.drawText(text, r, j);
}

void hazard(Graphics& g, Rectangle<float> r)
{
    Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(r.toNearestInt());
    g.setColour(kBlack);
    g.fillRect(r);
    g.setColour(kRed);
    const float hgt = r.getHeight();
    for (float x = r.getX() - hgt; x < r.getRight(); x += 24.f) {
        Path p;
        p.addQuadrilateral(x, r.getBottom(), x + 12.f, r.getBottom(), x + 12.f + hgt, r.getY(), x + hgt, r.getY());
        g.fillPath(p);
    }
}

struct HelpEntry { const char* title; const char* body; };
const HelpEntry kHelp[] = {
    { "RÉDUCTION DE BRUIT", "Combien le bruit de fond baisse (public, ventilation, souffle), de 0 à 30 dB. Plus la fusée penche, plus la flamme grandit. Trop haut, la voix devient creuse : montez par petits pas." },
    { "SENSIBILITÉ", "Vitesse à laquelle le détecteur crie « larsen ! ». Haut : il réagit tôt, mais peut confondre une note tenue très pure. Bas : il attend que le sifflement s'installe." },
    { "COUPURE MAX", "Profondeur maximale de chaque filtre anti-larsen, de 6 à 40 dB. Si le larsen est trop fort pour les filtres, un frein coupe tout le son d'un coup, et le journal dit de combien baisser le volume." },
    { "CISEAU OU CERVEAU", "Méthode de débruitage. Ciseau : calcul classique, prévisible. Cerveau : réseau neuronal, plus fin sur la voix. Au milieu, on mélange. Sans modèle installé, seul le ciseau travaille." },
    { "MASQUE NEURONAL", "Active ou coupe le réseau neuronal. Coupé, le ciseau travaille seul et le réseau ne guide plus le détecteur de larsen. La latence ne change pas." },
    { "HIGH CPU", "Fenêtre d'analyse divisée par deux : latence de 5,3 à 2,7 ms (affichée sur le bouton), pour 4 fois plus de calcul. Débruitage un peu moins fin, surtout côté ciseau. Le détecteur de larsen n'est pas concerné." },
    { "PORTE VOCALE", "Quand personne ne parle, la porte baisse le son du montant du curseur (0 à 40 dB), ce qui limite aussi le larsen. Elle s'ouvre aussitôt, attend 0,35 s, se ferme en 0,2 s. Voyant : VOIX ou SILENCE." },
    { "AUTOMIX", "Plusieurs micros, un seul gain à partager : celui qui parle prend la place des autres, comme un automixeur Dugan. Le curseur règle la baisse des micros silencieux. Les barres montrent le gain de chaque canal." },
    { "LARSENS ÉVITÉS", "Nombre de fois où une fréquence qui sifflait a été attrapée par un filtre. Un larsen qui revient après 10 s de calme compte pour un nouveau." },
    { "TEMPS ÉVITÉ", "Durée cumulée pendant laquelle au moins un filtre retenait un larsen. C'est le temps pendant lequel la salle aurait sifflé sans le plugin." },
    { "ALTITUDE", "Fantaisie de bord : 50 m d'altitude par dB de coupure appliqué en ce moment. Un gros larsen, une grosse altitude. Le plafond atteint est gardé." },
    { "TEMPS DE VOL", "Durée de traitement audio, sauvegardée avec le projet. La fusée avance à 11,2 km/s sur la barre Terre-Lune : compter 9 h 30 de vol pour arriver." },
    { "APPRENDRE LA SALLE", "Quelqu'un parle dans le micro. Le gain monte doucement, seulement pendant que vous parlez, jusqu'à faire siffler la salle. Chaque larsen trouvé devient un filtre permanent, gardé avec le projet. EFFACER les retire." },
    { "BYPASS", "Le bouton rond en haut à droite. Allumé, le son passe sans aucun traitement, juste retardé de la latence du plugin pour rester aligné, avec un fondu de 10 ms. Le bypass de votre logiciel audio fait pareil." },
    { "CANAUX ET AUDIO", "Standalone seulement. Canaux : de 1 à 8 micros, chacun avec son anti-larsen et son débruiteur. Audio : choix de la carte son, des entrées et des sorties. Le plugin suit les canaux de la piste." },
    { "VIDER", "Remet le journal de bord à zéro : larsens, temps évité, altitude, temps de vol et distance. Les réglages du plugin ne changent pas." },
};
} // namespace

// ------------------------------------------------------------------------------------------ help overlay
class HelpOverlay : public Component {
public:
    explicit HelpOverlay(VonDeLarsenProcessor& p) : proc_(p) { setInterceptsMouseClicks(true, false); }

    std::function<void()> onClose;

    void mouseDown(const MouseEvent&) override
    {
        if (onClose) onClose();
    }

    void paint(Graphics& g) override
    {
        g.fillAll(Colour(0xfa0a0a0d));
        g.setColour(kRed);
        g.drawRect(getLocalBounds().toFloat().reduced(8.f), 2.f);
        label(g, T("NOTICE DE BORD"), { 34.f, 16.f, 560.f, 52.f }, titleFont(31.f).withExtraKerningFactor(0.3f), kCream, Justification::centredLeft);
        label(g, T("Chaque réglage, en clair. Passez la souris sur un bouton pour une aide courte."), { 36.f, 66.f, 600.f, 18.f },
              monoFont(11.f, false), kDim, Justification::centredLeft);

        for (int i = 0; i < static_cast<int>(std::size(kHelp)); ++i) {
            const float x = (i % 2) ? 366.f : 34.f;
            const float y = 96.f + static_cast<float>(i / 2) * 85.f;
            const Rectangle<float> card(x, y, 320.f, 79.f);
            g.setColour(kPanel);
            g.fillRect(card);
            g.setColour(kEdge);
            g.drawRect(card, 1.5f);
            g.setColour(kRed);
            g.fillEllipse(x + 8.f, y + 8.f, 20.f, 20.f);
            label(g, String(i + 1), { x + 8.f, y + 8.f, 20.f, 20.f }, monoFont(12.f), kCream);
            label(g, T(kHelp[i].title), { x + 36.f, y + 8.f, 270.f, 20.f }, monoFont(12.5f).withExtraKerningFactor(0.12f), kHotRed, Justification::centredLeft);
            g.setColour(kCream.withAlpha(0.9f));
            g.setFont(bodyFont(11.f, false));
            g.drawMultiLineText(T(kHelp[i].body), static_cast<int>(x) + 12, static_cast<int>(y) + 40, 296);
        }

        String foot = T("Cliquez n'importe où pour fermer.");
        const double sr = proc_.getSampleRate();
        if (sr > 0) {
            const double plug = proc_.getLatencySamples() * 1000.0 / sr;
            String lat = String::formatted("Latence : plugin %.1f ms", plug);
            if (proc_.hostHooks.deviceLatencyMs) {
                const double dev = proc_.hostHooks.deviceLatencyMs();
                lat += String::formatted(T(" + carte son %.1f ms = %.1f ms").toRawUTF8(), dev, plug + dev);
            }
            foot = lat + String::formatted(" (%.1f kHz).   ", sr / 1000.0) + foot;
        }
        label(g, foot, { 34.f, 790.f, 652.f, 16.f }, monoFont(11.f, false), kDim);
    }

private:
    VonDeLarsenProcessor& proc_;
};

// ------------------------------------------------------------------------------------------ content
class VdlEditor::Content : public Component, private Timer {
public:
    explicit Content(VonDeLarsenProcessor& p) : proc_(p), overlay_(p)
    {
        auto setupRotary = [this](Slider& s, int kind) {
            s.setSliderStyle(Slider::RotaryVerticalDrag);
            s.setTextBoxStyle(Slider::NoTextBox, true, 0, 0);
            s.setRotaryParameters(MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
            s.setMouseDragSensitivity(220);
            s.getProperties().set("kind", kind);
            addAndMakeVisible(s);
        };
        setupRotary(noise_, 0);
        setupRotary(sens_, 1);
        setupRotary(cut_, 1);

        mix_.setSliderStyle(Slider::LinearHorizontal);
        mix_.setTextBoxStyle(Slider::NoTextBox, true, 0, 0);
        addAndMakeVisible(mix_);
        addAndMakeVisible(neural_);
        neural_.setButtonText("MASQUE NEURONAL");
        addAndMakeVisible(highCpu_);
        highCpu_.setComponentID("cpu");

        addAndMakeVisible(learn_);
        learn_.setButtonText("APPRENDRE LA SALLE");
        learn_.setComponentID("learn");
        learn_.setClickingTogglesState(false); // its state follows the processor
        learn_.onClick = [this] { if (tm_.learning) proc_.stopLearn(); else proc_.startLearn(); };
        learn_.setTooltip(T("Quelqu'un parle dans le micro : le gain monte, les larsens trouvés deviennent des filtres permanents."));
        clearLearn_.setComponentID("reset");
        clearLearn_.setTooltip(T("Retire les filtres appris de la salle."));
        clearLearn_.onClick = [this] { proc_.clearLearned(); tick(); };
        addAndMakeVisible(clearLearn_);
        addAndMakeVisible(bypass_);
        bypass_.setComponentID("bypass");
        addAndMakeVisible(autoOn_);
        autoOn_.setButtonText("AUTOMIX");
        autoDepth_.setSliderStyle(Slider::LinearHorizontal);
        autoDepth_.setTextBoxStyle(Slider::NoTextBox, true, 0, 0);
        autoDepth_.getProperties().set("kind", 2);
        addAndMakeVisible(autoDepth_);

        addAndMakeVisible(gate_);
        gate_.setButtonText("PORTE VOCALE");
        gate_.setComponentID("gate");
        depth_.setSliderStyle(Slider::LinearHorizontal);
        depth_.setTextBoxStyle(Slider::NoTextBox, true, 0, 0);
        depth_.getProperties().set("kind", 2);
        addAndMakeVisible(depth_);

        noiseA_ = std::make_unique<SA>(proc_.apvts, "nrAmount", noise_);
        sensA_ = std::make_unique<SA>(proc_.apvts, "fbSens", sens_);
        cutA_ = std::make_unique<SA>(proc_.apvts, "fbCut", cut_);
        mixA_ = std::make_unique<SA>(proc_.apvts, "nnMix", mix_);
        neuralA_ = std::make_unique<BA>(proc_.apvts, "nnEnable", neural_);
        highCpuA_ = std::make_unique<BA>(proc_.apvts, "highCpu", highCpu_);
        highCpu_.onStateChange = [this] { updateCpuText(); };
        updateCpuText();
        gateA_ = std::make_unique<BA>(proc_.apvts, "gateOn", gate_);
        bypassA_ = std::make_unique<BA>(proc_.apvts, "bypass", bypass_);
        autoOnA_ = std::make_unique<BA>(proc_.apvts, "autoMixOn", autoOn_);
        autoDepthA_ = std::make_unique<SA>(proc_.apvts, "autoMixDepth", autoDepth_);
        depthA_ = std::make_unique<SA>(proc_.apvts, "gateDepth", depth_);

        // Our own value text (set after the attachments, which install theirs).
        noise_.textFromValueFunction = [](double v) { return String(roundToInt(v)) + " dB"; };
        sens_.textFromValueFunction = [](double v) { return String(roundToInt(v * 100.0)) + " %"; };
        cut_.textFromValueFunction = [](double v) { return String(roundToInt(v)) + " dB"; };

        noise_.setTooltip(T("Réduction de bruit : jusqu'où on baisse le bruit de fond (0 à 30 dB)."));
        sens_.setTooltip(T("Sensibilité : à quelle vitesse le détecteur réagit à un larsen."));
        cut_.setTooltip(T("Coupure max : profondeur maximale de chaque filtre anti-larsen."));
        mix_.setTooltip(T("Ciseau (calcul classique) à gauche, cerveau (réseau neuronal) à droite."));
        neural_.setTooltip(T("Active ou coupe le réseau neuronal."));
        highCpu_.setTooltip(T("High CPU : fenêtre d'analyse plus courte, latence divisée par deux, plus de calcul."));
        mix_.onValueChange = [this] { repaint(); };
        depth_.setTooltip(T("Affaiblissement sans voix : de combien le son baisse quand la porte est fermée."));
        gate_.setTooltip(T("Porte vocale : baisse le son quand personne ne parle."));
        depth_.onValueChange = [this] { repaint(34, 506, 652, 50); };
        gate_.onStateChange = [this] {
            depth_.setEnabled(gate_.getToggleState());
            repaint(34, 506, 652, 50);
        };
        depth_.setEnabled(gate_.getToggleState());
        bypass_.setTooltip(T("Bypass : le son passe sans traitement (retardé de la latence du plugin)."));
        autoOn_.setTooltip(T("Automix : partage le gain entre les micros, celui qui parle prend la place."));
        autoDepth_.setTooltip(T("Atténuation des micros silencieux, de 6 à 40 dB."));
        autoDepth_.onValueChange = [this] { repaint(34, 556, 652, 50); };
        autoOn_.onStateChange = [this] {
            autoDepth_.setEnabled(autoOn_.getToggleState());
            repaint(34, 556, 652, 50);
        };
        autoDepth_.setEnabled(autoOn_.getToggleState());

        addChildComponent(overlay_);
        overlay_.onClose = [this] { help_.setToggleState(false, sendNotification); };

        help_.setComponentID("help");
        help_.setClickingTogglesState(true);
        help_.setTooltip(T("Notice de bord : explique chaque réglage."));
        help_.onClick = [this] { showHelp(help_.getToggleState()); };
        addAndMakeVisible(help_);

        chMinus_.setComponentID("step");
        chPlus_.setComponentID("step");
        chMinus_.setTooltip(T("Un canal de moins."));
        chPlus_.setTooltip(T("Un canal de plus (jusqu'à 8)."));
        audio_.setTooltip(T("Choisir la carte son, les entrées et les sorties."));
        chMinus_.onClick = [this] { changeChannels(-1); };
        chPlus_.onClick = [this] { changeChannels(1); };
        audio_.onClick = [this] { if (proc_.hostHooks.openAudioSettings) proc_.hostHooks.openAudioSettings(); };
        for (auto* b : { &chMinus_, &chPlus_, &audio_ }) addChildComponent(*b);
        updateVisibility();

        reset_.setComponentID("reset");
        reset_.setTooltip(T("Remet le journal de bord à zéro."));
        reset_.onClick = [this] {
            proc_.resetTelemetry();
            tick();
        };
        addAndMakeVisible(reset_);

        tm_ = proc_.telemetry();
        learn_.setToggleState(tm_.learning, dontSendNotification);
        startTimerHz(6);
    }

    ~Content() override { setLookAndFeel(nullptr); }

    void showHelp(bool show)
    {
        help_.setToggleState(show, dontSendNotification);
        overlay_.setVisible(show);
        updateVisibility();
    }

    void resized() override
    {
        noise_.setBounds(244, 108, 232, 274);
        sens_.setBounds(48, 170, 124, 158);
        cut_.setBounds(548, 170, 124, 158);
        mix_.setBounds(139, 404, 442, 56);
        neural_.setBounds(50, 478, 300, 28);
        highCpu_.setBounds(370, 478, 300, 28);
        gate_.setBounds(34, 512, 250, 28);
        autoOn_.setBounds(34, 562, 250, 28);
        autoDepth_.setBounds(296, 562, 200, 28);
        depth_.setBounds(296, 512, 200, 28);
        help_.setBounds(650, 24, 28, 28);
        bypass_.setBounds(614, 24, 28, 28);
        learn_.setBounds(34, 612, 216, 28);
        clearLearn_.setBounds(594, 616, 74, 20);
        chMinus_.setBounds(100, 40, 20, 20);
        chPlus_.setBounds(142, 40, 20, 20);
        audio_.setBounds(546, 40, 60, 20);
        reset_.setBounds(612, 666, 64, 16);
        overlay_.setBounds(0, 0, kW, kH);
    }

    void paint(Graphics& g) override
    {
        g.fillAll(Colour(0xff0a0a0d));
        g.setColour(kHotRed.withAlpha(0.07f));
        for (int x = 0; x <= kW; x += 24) g.drawVerticalLine(x, 0.f, static_cast<float>(kH));
        for (int y = 0; y <= kH; y += 24) g.drawHorizontalLine(y, 0.f, static_cast<float>(kW));

        g.setColour(kEdge);
        g.drawRoundedRectangle(8.f, 8.f, 704.f, 794.f, 10.f, 2.f);
        for (auto p : { Point<float>(22.f, 22.f), { 698.f, 22.f }, { 22.f, 788.f }, { 698.f, 788.f } }) {
            g.setColour(Colour(0xff1a1a1f));
            g.fillEllipse(p.x - 5.f, p.y - 5.f, 10.f, 10.f);
            g.setColour(Colour(0xff555555));
            g.drawEllipse(p.x - 5.f, p.y - 5.f, 10.f, 10.f, 1.f);
            g.setColour(Colour(0xff777777));
            g.drawLine(p.x - 3.f, p.y, p.x + 3.f, p.y, 1.2f);
        }

        // header
        g.setColour(kPanel);
        g.fillRect(34.f, 16.f, 652.f, 52.f);
        g.setColour(kEdge);
        g.drawRect(34.f, 16.f, 652.f, 52.f, 1.f);
        label(g, T("TOURNESOL-OS > MODULE ANTI-LARSEN"), { 46.f, 18.f, 330.f, 14.f }, monoFont(11.f, false).withExtraKerningFactor(0.15f), kHotRed, Justification::centredLeft);
        label(g, tm_.bypassed ? T("BYPASS ACTIF · SON NON TRAITÉ") : T("SECTEUR 7 · ALT ") + groupInt(tm_.altitudeM) + " m",
              { 330.f, 18.f, 270.f, 14.f }, monoFont(11.f, false).withExtraKerningFactor(0.15f),
              (tm_.alert || tm_.bypassed) ? kHotRed : kDim, Justification::centredRight);
        if (proc_.hostHooks.setChannels) {
            label(g, T("CANAUX"), { 46.f, 40.f, 54.f, 20.f }, monoFont(10.f, false).withExtraKerningFactor(0.15f), kDim, Justification::centredLeft);
            label(g, String(proc_.activeChannels()), { 120.f, 40.f, 22.f, 20.f }, monoFont(14.f), kHotRed);
        }
        label(g, T("VON DE LARSEN"), { 34.f, 30.f, 652.f, 38.f }, titleFont(31.f).withExtraKerningFactor(0.35f), kCream);
        hazard(g, { 34.f, 72.f, 652.f, 8.f });

        // labels
        const Font mono12 = monoFont(12.f, false).withExtraKerningFactor(0.25f);
        label(g, T("RÉDUCTION DE BRUIT"), { 160.f, 388.f, 400.f, 18.f }, monoFont(13.f, false).withExtraKerningFactor(0.3f), kCream);
        for (auto& [x, a, b] : { std::tuple<float, const char*, const char*>(110.f, "SENSIBILITÉ", "tournesol-o-mètre"),
                                 std::tuple<float, const char*, const char*>(610.f, "COUPURE MAX", "tonnerre de brest") }) {
            label(g, T(a), { x - 80.f, 340.f, 160.f, 16.f }, monoFont(12.f, false).withExtraKerningFactor(0.25f), kCream);
            label(g, T(b), { x - 80.f, 356.f, 160.f, 14.f }, monoFont(10.f, false).withExtraKerningFactor(0.15f), kDim);
        }

        // ciseau (classic) ... cerveau (neural)
        const float mix = static_cast<float>(mix_.getValue());
        drawChisel(g, { 54.f, 404.f, 56.f, 56.f }, 1.f - mix);
        drawBrain(g, { 609.f, 401.f, 58.f, 58.f }, mix);
        label(g, T("CLASSIQUE"), { 22.f, 462.f, 120.f, 16.f }, mono12, kCream);
        label(g, T("NEURAL"), { 578.f, 462.f, 120.f, 16.f }, mono12, kCream);
        label(g, T("CISEAU ── CERVEAU"), { 210.f, 454.f, 300.f, 16.f }, mono12, Colour(0xffff7b7b));

        paintGateRow(g);
        paintAutoRow(g);
        paintLearnRow(g);
        paintTelemetry(g);
        hazard(g, { 34.f, 792.f, 652.f, 8.f });
    }

private:
    using SA = AudioProcessorValueTreeState::SliderAttachment;
    using BA = AudioProcessorValueTreeState::ButtonAttachment;

    void timerCallback() override { tick(); }

    void updateVisibility()
    {
        const bool standalone = static_cast<bool>(proc_.hostHooks.setChannels);
        const bool open = overlay_.isVisible();
        reset_.setVisible(!open);
        for (auto* b : { &chMinus_, &chPlus_, &audio_ }) b->setVisible(standalone && !open);
        gate_.setVisible(!open);
        highCpu_.setVisible(!open);
        depth_.setVisible(!open);
        autoOn_.setVisible(!open);
        autoDepth_.setVisible(!open);
        learn_.setVisible(!open);
        clearLearn_.setVisible(!open);
        bypass_.setVisible(!open);
    }

    void changeChannels(int delta)
    {
        const int n = juce::jlimit(1, VonDeLarsenProcessor::kMaxChannels, proc_.activeChannels() + delta);
        if (n == proc_.activeChannels() || !proc_.hostHooks.setChannels) return;
        proc_.hostHooks.setChannels(n);
        repaint(34, 12, 652, 56);
    }

    // The button carries the latency the host currently gets, so the gain is visible.
    void updateCpuText()
    {
        const double sr = proc_.getSampleRate() > 0 ? proc_.getSampleRate() : 48000.0;
        const bool on = highCpu_.getToggleState();
        const int smp = on ? vc::SpectralDenoiser::kN / 2 : vc::SpectralDenoiser::kN;
        highCpu_.setButtonText("HIGH CPU " + String(smp * 1000.0 / sr, 1).replace(".", ",") + " MS");
    }

    void tick()
    {
        tm_ = proc_.telemetry();
        repaint(34, 12, 652, 26);
        repaint(34, 506, 652, 310);
        learn_.setToggleState(tm_.learning, dontSendNotification);
    }

    void paintLearnRow(Graphics& g)
    {
        // frequency strip, 100 Hz to 10 kHz on a log axis: learned notches stand tall and bright, live ones short and dim
        const Rectangle<float> strip(258.f, 612.f, 326.f, 28.f);
        g.setColour(kBlack);
        g.fillRect(strip);
        g.setColour(tm_.learning ? kRed : kEdge);
        g.drawRect(strip, 1.8f);
        auto xOf = [&strip](float hz) { return strip.getX() + 4.f + (strip.getWidth() - 8.f) * std::log10(jlimit(100.f, 10000.f, hz) / 100.f) / 2.f; };
        g.setColour(kEdge.brighter(0.3f));
        for (float hz : { 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f }) g.drawVerticalLine(roundToInt(xOf(hz)), strip.getY() + 3.f, strip.getY() + 9.f);
        for (auto [hz, name] : { std::pair<float, const char*>(100.f, "100"), { 1000.f, "1k" }, { 10000.f, "10k" } })
            label(g, name, { xOf(hz) - 14.f, strip.getBottom() - 11.f, 28.f, 10.f }, monoFont(8.f, false), kDim);
        for (int i = 0; i < tm_.notchViews; ++i) {
            const auto& nv = tm_.notches[static_cast<size_t>(i)];
            const float x = xOf(nv.hz);
            const float h = jlimit(4.f, 20.f, nv.cutDb * 0.9f);
            g.setColour(nv.fixed ? kHotRed : kDim);
            g.fillRect(x - 1.5f, strip.getBottom() - 4.f - h, 3.f, h);
            if (nv.fixed) { g.setColour(kYellow); g.fillRect(x - 1.5f, strip.getBottom() - 4.f - h, 3.f, 2.f); }
        }

        String msg;
        if (tm_.learning) {
            if (tm_.learnPhase == 3) msg = T("Marge atteinte, retour au gain normal…");
            else if (tm_.learnPhase == 2) msg = T("Larsen trouvé : un filtre se règle, le gain attend.");
            else if (tm_.learnWaiting) msg = T("PARLEZ DANS LE MICRO : le gain ne monte que pendant que vous parlez.");
            else msg = T("Apprentissage : gain +") + String(tm_.learnGainDb, 1) + T(" dB, ") + String(tm_.learnHowls) + (tm_.learnHowls > 1 ? T(" larsens trouvés") : T(" larsen trouvé")) + T(". Parlez normalement.");
        } else if (tm_.learnResult == 1) {
            msg = T("Salle apprise : ") + String(tm_.learnedCount) + (tm_.learnedCount > 1 ? T(" filtres") : T(" filtre")) + T(", marge de +") + String(tm_.learnMarginDb, 0) + T(" dB. Gardés avec le projet.");
        } else if (tm_.learnResult == 2) {
            msg = T("Aucun larsen jusqu'à +18 dB : la salle est saine, rien à apprendre.");
        } else if (tm_.learnResult == 3) {
            msg = T("Apprentissage interrompu : le signal était trop fort. Baissez le volume et recommencez.");
        } else if (tm_.learnResult == 4) {
            msg = T("Apprentissage interrompu : trop long. Parlez pendant l'apprentissage.");
        } else if (tm_.learnedCount > 0) {
            msg = String(tm_.learnedCount) + (tm_.learnedCount > 1 ? T(" filtres appris, gardés avec le projet.") : T(" filtre appris, gardé avec le projet."));
        } else {
            msg = T("Appuyez sur APPRENDRE LA SALLE, puis parlez dans le micro comme pendant le spectacle.");
        }
        label(g, msg, { 34.f, 643.f, 652.f, 12.f }, monoFont(10.f, tm_.learning && tm_.learnWaiting), tm_.learning ? kHotRed : kDim);
    }

    void paintAutoRow(Graphics& g)
    {
        const bool on = tm_.autoMixOn;
        const Rectangle<float> box(500.f, 564.f, 58.f, 24.f);
        g.setColour(kBlack);
        g.fillRect(box);
        g.setColour(on ? kRed : kEdge);
        g.drawRect(box, 1.8f);
        const int depth = roundToInt(static_cast<float>(autoDepth_.getValue()));
        label(g, "-" + String(depth) + " dB", box, monoFont(14.f), on ? kHotRed : Colour(0xff6b2a2a));
        label(g, T(tm_.channels < 2 ? "IL FAUT AU MOINS 2 CANAUX" : "ATTÉNUATION DES MICROS SILENCIEUX"), { 296.f, 591.f, 262.f, 12.f },
              monoFont(9.f, false).withExtraKerningFactor(0.2f), kDim);

        // one meter per channel: the gain the automixer gives it (bottom = -40 dB, top = 0 dB)
        for (int c = 0; c < VonDeLarsenProcessor::kMaxChannels; ++c) {
            const float x = 576.f + static_cast<float>(c) * 14.f;
            const Rectangle<float> bar(x, 564.f, 9.f, 24.f);
            const bool used = c < tm_.channels;
            g.setColour(kBlack);
            g.fillRect(bar);
            if (used) {
                const float f = jlimit(0.f, 1.f, (tm_.autoGainDb[static_cast<size_t>(c)] + 40.f) / 40.f);
                g.setColour(on ? kHotRed : Colour(0xff6b2a2a));
                g.fillRect(bar.getX(), bar.getBottom() - bar.getHeight() * f, bar.getWidth(), bar.getHeight() * f);
            }
            g.setColour(used ? kRed : kEdge);
            g.drawRect(bar, 1.2f);
            label(g, String(c + 1), { x - 2.f, 590.f, 13.f, 11.f }, monoFont(8.f, false), used ? kDim : Colour(0xff4a2024));
        }
    }

    void paintGateRow(Graphics& g)
    {
        const bool on = tm_.gateOn;
        // depth readout
        const Rectangle<float> box(500.f, 514.f, 58.f, 24.f);
        g.setColour(kBlack);
        g.fillRect(box);
        g.setColour(on ? kRed : kEdge);
        g.drawRect(box, 1.8f);
        const int depth = roundToInt(static_cast<float>(depth_.getValue()));
        label(g, depth == 0 ? String("0 dB") : "-" + String(depth) + " dB", box, monoFont(14.f), on ? kHotRed : Colour(0xff6b2a2a));
        label(g, T("AFFAIBLISSEMENT SANS VOIX"), { 296.f, 541.f, 262.f, 12.f }, monoFont(9.f, false).withExtraKerningFactor(0.2f), kDim);

        // voice lamp
        const bool voice = tm_.voice;
        const Rectangle<float> led(572.f, 516.f, 20.f, 20.f);
        if (voice) {
            g.setColour(kHotRed.withAlpha(0.3f));
            g.fillRect(led.expanded(4.f));
            g.setColour(kHotRed);
        } else {
            g.setColour(Colour(0xff35100f));
        }
        g.fillRect(led);
        label(g, !on ? T("PORTE OFF") : voice ? T("VOIX") : T("SILENCE"), { 598.f, 514.f, 88.f, 24.f },
              monoFont(12.f).withExtraKerningFactor(0.15f), voice ? kHotRed : kDim, Justification::centredLeft);
    }

    void paintTelemetry(Graphics& g)
    {
        Graphics::ScopedSaveState shift(g);
        g.addTransform(AffineTransform::translation(0.f, 150.f)); // the gate, automix and learn rows sit above the journal
        g.setColour(kPanel);
        g.fillRect(34.f, 512.f, 652.f, 126.f);
        g.setColour(kEdge);
        g.drawRect(34.f, 512.f, 652.f, 126.f, 1.5f);
        label(g, T("JOURNAL DE BORD"), { 44.f, 516.f, 200.f, 16.f }, monoFont(10.f).withExtraKerningFactor(0.3f), kHotRed, Justification::centredLeft);
        label(g, T("CHARGE PROCESSEUR ") + String(roundToInt(tm_.cpuLoad * 100.f)) + " %", { 380.f, 516.f, 210.f, 16.f }, monoFont(9.5f, false).withExtraKerningFactor(0.12f), kDim, Justification::centredRight);

        const double km = tm_.flightSeconds * VonDeLarsenProcessor::kKmPerSecond;
        struct Cell { String title, value, sub; };
        const Cell cells[] = {
            { T("LARSENS ÉVITÉS"), groupInt(tm_.events),
              tm_.lastFreqHz > 0 ? T("dernier : ") + groupInt(tm_.lastFreqHz) + " Hz" : T("aucun pour l'instant") },
            { T("TEMPS ÉVITÉ"), clock(tm_.avoidedSeconds, false), tm_.avoidedSeconds > 0 ? T("de larsen muselé") : T("ciel dégagé") },
            { T("ALTITUDE"), groupInt(tm_.altitudeM) + " m", T("plafond : ") + groupInt(tm_.recordM) + " m" },
            { T("TEMPS DE VOL"), clock(tm_.flightSeconds, true), groupInt(km) + " km parcourus" },
        };
        for (int i = 0; i < 4; ++i) {
            const float x = 44.f + static_cast<float>(i) * 157.f;
            g.setColour(Colour(0xff0a0709));
            g.fillRect(x, 534.f, 151.f, 54.f);
            g.setColour(Colour(0xff2a1215));
            g.drawRect(x, 534.f, 151.f, 54.f, 1.f);
            label(g, cells[i].title, { x + 8.f, 537.f, 135.f, 12.f }, monoFont(10.f, false).withExtraKerningFactor(0.2f), kDim, Justification::centredLeft);
            label(g, cells[i].value, { x + 8.f, 549.f, 135.f, 26.f }, monoFont(23.f), (i == 2 && tm_.alert) ? kYellow : kHotRed, Justification::centredLeft);
            label(g, cells[i].sub, { x + 8.f, 574.f, 135.f, 12.f }, monoFont(10.f, false), kCream.withAlpha(0.85f), Justification::centredLeft);
        }

        // mission: Earth -> Moon at escape velocity
        const float x0 = 92.f, x1 = 586.f, by = 600.f;
        const float progress = static_cast<float>(jlimit(0.0, 1.0, km / VonDeLarsenProcessor::kMoonKm));
        label(g, T("TERRE"), { 44.f, by - 8.f, 44.f, 16.f }, monoFont(10.f).withExtraKerningFactor(0.15f), kCream, Justification::centredLeft);
        g.setColour(kBlack);
        g.fillRect(x0, by - 4.f, x1 - x0, 8.f);
        g.setColour(kRed);
        g.drawRect(x0, by - 4.f, x1 - x0, 8.f, 1.5f);
        g.setColour(kHotRed);
        g.fillRect(x0 + 1.5f, by - 2.5f, jmax(0.f, (x1 - x0 - 3.f) * progress), 5.f);
        const float px = jlimit(x0 + 8.f, x1 - 8.f, x0 + (x1 - x0) * progress);
        {
            Graphics::ScopedSaveState ss(g);
            g.addTransform(AffineTransform::rotation(MathConstants<float>::halfPi, px, by));
            drawRocket(g, { px, by }, 0.15f, 0.6f);
        }
        g.setGradientFill(ColourGradient(Colour(0xfffff8e4), 600.f, by - 3.f, Colour(0xffbdb08e), 610.f, by + 5.f, true));
        g.fillEllipse(598.f, by - 8.f, 16.f, 16.f);
        g.setColour(kBlack);
        g.drawEllipse(598.f, by - 8.f, 16.f, 16.f, 1.5f);
        label(g, T("LUNE"), { 620.f, by - 8.f, 60.f, 16.f }, monoFont(10.f).withExtraKerningFactor(0.15f), kCream, Justification::centredLeft);
        label(g, groupInt(km) + " km sur " + groupInt(VonDeLarsenProcessor::kMoonKm) + " km", { x0, 608.f, x1 - x0, 12.f },
              monoFont(10.f, false), kDim);

        String msg;
        if (tm_.bypassed) msg = T("BYPASS : le son passe sans aucun traitement.");
        else if (tm_.guardDb >= 3.f) msg = T("ALERTE LARSEN ! Gain coupé de ") + String(roundToInt(tm_.guardDb)) + T(" dB : baissez un peu le volume.");
        else if (tm_.alert) msg = T("ALERTE LARSEN ! Tonnerre de Brest, les filtres s'activent.");
        else if (tm_.events == 0) msg = T("Rien à signaler. Le Professeur Tournesol n'entend rien.");
        else msg = T("Cap sur la Lune. ") + String(tm_.events) + (tm_.events > 1 ? T(" larsens muselés.") : T(" larsen muselé."));
        label(g, msg, { 44.f, 620.f, 632.f, 14.f }, monoFont(11.5f), (tm_.alert || tm_.bypassed) ? kHotRed : kCream);
    }

    VonDeLarsenProcessor& proc_;
    Slider noise_, sens_, cut_, mix_;
    ToggleButton neural_, highCpu_, gate_, autoOn_, bypass_, learn_;
    Slider depth_, autoDepth_;
    TextButton help_ { "?" }, reset_ { "VIDER" }, clearLearn_ { "EFFACER" }, chMinus_ { "-" }, chPlus_ { "+" }, audio_ { "AUDIO" };
    HelpOverlay overlay_;
    std::unique_ptr<SA> noiseA_, sensA_, cutA_, mixA_;
    std::unique_ptr<BA> neuralA_, highCpuA_, gateA_, autoOnA_, bypassA_;
    std::unique_ptr<SA> depthA_, autoDepthA_;
    VonDeLarsenProcessor::Telemetry tm_;
};

// ------------------------------------------------------------------------------------------ editor
VdlEditor::VdlEditor(VonDeLarsenProcessor& p) : AudioProcessorEditor(p)
{
    content_ = std::make_unique<Content>(p);
    content_->setLookAndFeel(&laf_);
    addAndMakeVisible(*content_);
    setResizeLimits(kW * 3 / 4, kH * 3 / 4, kW * 3 / 2, kH * 3 / 2);
    setResizable(true, true);
    getConstrainer()->setFixedAspectRatio(static_cast<double>(kW) / kH);
    setSize(kW, kH);
}

VdlEditor::~VdlEditor()
{
    content_.reset();
}

void VdlEditor::paint(Graphics& g) { g.fillAll(Colour(0xff0a0a0d)); }

void VdlEditor::resized()
{
    // Fit the fixed design space in the window, centred (a host window may not keep our aspect ratio).
    const float s = jmin(static_cast<float>(getWidth()) / static_cast<float>(kW), static_cast<float>(getHeight()) / static_cast<float>(kH));
    const float ox = (static_cast<float>(getWidth()) - s * static_cast<float>(kW)) * 0.5f;
    const float oy = (static_cast<float>(getHeight()) - s * static_cast<float>(kH)) * 0.5f;
    content_->setTransform(AffineTransform::scale(s).translated(ox, oy));
    content_->setBounds(0, 0, kW, kH);
}

void VdlEditor::showHelp(bool show) { content_->showHelp(show); }
