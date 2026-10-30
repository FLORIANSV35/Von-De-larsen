# Notes de reprise : Von De Larsen

Résumé de la conversation de conception, pour reprendre le travail dans VS Code.
Branche : `claude/voice-feedback-denoise-plugin` (rien n'est encore sur `main`, pas de PR ouverte).

## Cahier des charges (décisions prises)

- Plugin **voix uniquement** : annule le **larsen** (feedback) et réduit le **bruit de fond**.
- Utilise le **Neural Engine** du Mac (CoreML, Apple Silicon).
- **Latence totale ≤ 10 ms** (réponse à « tenue 5 ms » : « max 10 ms »). Actuel : **5,33 ms à 48 kHz**.
- Format : **JUCE, AU + VST3** (2 canaux max).
- Contexte : **sono live** (micro + haut-parleurs, vraie boucle acoustique).
- **Standalone multicanal**, nombre de canaux réglable de **1 à 8**.
- **Plusieurs notchs simultanés** (jusqu'à 8, `kMaxNotches`).
- **Mélange réseau + classique** pour le bruit et pour le larsen.

## Architecture

```
micro ─▶ FeedbackSuppressor ─▶ SpectralDenoiser ─▶ sortie
          (notchs temporels,     (STFT 256, hop 128,
           0 ms de latence)       latence = 256 éch.)
               ▲                        ▲
               │ indice voix/bruit      │ masque (asynchrone, 1 hop de retard)
               └────────── AsyncMaskProvider ── thread ── CoreML ── Neural Engine
```

- **`FeedbackSuppressor`** (`core/`) : FFT 1024 d'analyse en parallèle (sans retarder l'audio), détection de pics
  étroits persistants et croissants (ratio pic/voisins), rejet des signaux harmoniques (voix), jusqu'à 8
  filtres notch (Q 30, −18 dB max par défaut), maintenus 10 s. Le masque réseau sert d'indice : seuils −4 dB
  là où « pas de voix », +3 dB là où « voix ». Il ne peut pas bloquer un vrai larsen.
- **`SpectralDenoiser`** : Wiener à décision dirigée + suivi de minimum. Gain final = moyenne géométrique
  `g_wiener^(1-m) · g_réseau^m` (curseur « Neural share », défaut 0,7, 0 = classique seul).
- **`AsyncMaskProvider`** : boîtes aux lettres sans verrou audio ↔ thread d'inférence. Un modèle lent ne
  bloque jamais l'audio ; masque périmé (> 4 hops) → retour au Wiener.
- **Pourquoi asynchrone** : CoreML n'a aucune garantie temps réel (dispatch ~ms avec gigue). Le masque de la
  trame t est appliqué à la trame t+1 : **zéro latence audio ajoutée**, seulement ~2,7 ms de fraîcheur.

## Arborescence

| Chemin | Contenu |
|---|---|
| `core/` | Bibliothèque C++ portable (FFT, biquad, anti-larsen, débruiteur, mailbox async) + pont CoreML (`CoreMlMaskProvider.mm`, stub hors Apple) |
| `tests/test_core.cpp` | Tests DSP (voir plus bas) |
| `plugin/` | Plugin JUCE AU/VST3 (2 canaux) |
| `standalone/` | App JUCE multicanal 1–8 canaux, choix de l'interface audio, latence affichée en direct |
| `tools/bench_mask.cpp` | Mesure si le modèle tient le rythme des hops (un toutes les 2,67 ms) |
| `tools/make_placeholder_model.py` | Modèle CoreML à poids aléatoires, **pour mesurer la latence seulement** |
| `tools/train_mask.py` | Entraînement du masque + export CoreML (non testé) |
| `README.md` | Documentation utilisateur |

## État vérifié / non vérifié

**Vérifié (Linux, `ctest` passe)** : tout `core/` et `tests/` :
- reconstruction parfaite et latence (gain neutre = simple retard de 256 éch.) ;
- gain de SNR du débruiteur (~+8 dB sur signal de test) ;
- boucle de larsen simulée : −1,3 dBFS sans plugin → −60 dBFS avec ; 1 résonance, puis **3 résonances
  simultanées** (900/2400/5200 Hz, 3 notchs actifs) ;
- aucun faux positif sur voix soutenue synthétique (110/220/440 Hz) ;
- mailbox asynchrone ; mélange réseau/classique ; indice voix fixé à 0/0,5/1 sans bloquer le larsen.

**Jamais compilé ni exécuté (pas de Mac pendant l'écriture)** :
- `core/src/CoreMlMaskProvider.mm`
- `plugin/` et `standalone/` (JUCE 8.0.4 via FetchContent)
- `tools/train_mask.py` et `tools/make_placeholder_model.py`

Attendre des erreurs de compilation au premier build sur Mac.

## Commandes

```sh
# Cœur DSP + tests (toutes plateformes)
cmake -S . -B build -DVDL_BUILD_PLUGIN=OFF -DVDL_BUILD_STANDALONE=OFF
cmake --build build -j && ctest --test-dir build --output-on-failure

# Mac Apple Silicon : tout (plugin + standalone), JUCE téléchargé automatiquement
cmake -S . -B build && cmake --build build --config Release -j
```

## Modèle neuronal : plan

Contrat du modèle (float32) : entrée `features` [1,129] (puissance en dBFS par bin, STFT 256 / hop 128,
fenêtre `sin(πj/256)`, `10·log10(|X|²·(π/256)²+1e-10)`), entrée optionnelle `state_in` [1,S], sortie `mask`
[1,129] (0..1), sortie optionnelle `state_out`. Dossier d'installation :
`~/Library/Application Support/VonDeLarsen/denoise.mlmodelc`.

Ordre conseillé sur le Mac :

```sh
pip install torch coremltools numpy scipy soundfile
# 1. Test du script (données synthétiques, modèle inutilisable)
python tools/train_mask.py --demo --steps 300 --out out/demo.mlpackage
# 2. Benchmark Neural Engine
xcrun coremlcompiler compile out/demo.mlpackage out/
./build/tests/bench_mask out/demo.mlmodelc      # doit afficher "OK: model keeps up with the hop rate"
# 3. Vrai entraînement
python tools/train_mask.py --speech <voix_propre> --noise <bruits> --steps 60000 --out out/denoise.mlpackage
xcrun coremlcompiler compile out/denoise.mlpackage out/
```

- Données : voix propre (LibriSpeech, VCTK, DNS Challenge) ; bruits adaptés à la sono (public, musique,
  scène). Références : DeepFilterNet (2/3), RNNoise (xiph), DNS Challenge, Voicebank+DEMAND, MUSAN, FSD50K.
- Je n'ai pas pu entraîner depuis la session cloud : le proxy bloquait les hébergeurs de données, pas de GPU.
- Si `bench_mask` dit que le modèle est trop lent : réduire `HIDDEN` dans `train_mask.py`.
- Le modèle **n'apprend pas en direct** : poids figés. Seuls s'adaptent le bruit estimé (Wiener) et les notchs
  du larsen. Idées si besoin : petit modèle de la voix du chanteur ; réglage adaptatif à la salle.

## À faire

1. Premier build sur Mac, corriger les erreurs (`.mm`, JUCE plugin, standalone).
2. Tester `train_mask.py --demo`, puis `bench_mask` : le Neural Engine tient-il 375 inférences/s par canal ?
   (8 canaux = 8 threads d'inférence = ~3000/s : à mesurer.)
3. Entraîner un vrai modèle.
4. Régler les seuils du détecteur de larsen sur de vrais enregistrements (chant tenu, parole forte) ; les tests
   n'ont que des résonances et des voix synthétiques.
5. Tester en conditions réelles (micro + enceintes), idéalement latence mesurée de bout en bout.
6. Régler à l'oreille : part du réseau (défaut 0,7), seuils d'indice (−4/+3 dB), coupure max (18 dB).
7. Interface graphique (actuellement éditeur générique JUCE pour le plugin).
8. Options : plus de 8 notchs (`kMaxNotches`), réglage du nombre de notchs, latence réduite avec `kN = 128`
   (2,7 ms, débruitage moins fin), plugin à plus de 2 canaux, ouvrir une PR.

## Points de vigilance

- Le détecteur de larsen confond un ton **parfaitement stable** avec un larsen ; la parade actuelle est le rejet
  des signaux avec harmoniques en rapports entiers. Chant tenu très pur : à surveiller.
- Les notchs sont libérés après 10 s sans détection : le larsen peut revenir et être redétecté (cycle lent).
- `CoreMlMaskProvider.mm` gère les sorties float16 et les strides ANE ; premier appel = compilation ANE
  (warm-up fait dans le thread worker).
- Le plugin AU/VST3 n'a pas de paramètre bypass : utiliser le bypass de l'hôte.
- Ne mettre aucun appel bloquant ni allocation dans le chemin audio (`process`).
