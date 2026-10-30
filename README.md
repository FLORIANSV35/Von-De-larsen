# Von De Larsen

Plugin vocal (AU + VST3, macOS Apple Silicon) : **anti-larsen** et **réduction de bruit de fond**,
pensé pour la voix en sono live. Latence totale du plugin : **5,3 ms à 48 kHz** (objectif ≤ 10 ms).

## Architecture

```
micro ─▶ FeedbackSuppressor ─▶ SpectralDenoiser ─▶ sortie
          (notchs temporels,     (STFT 256, hop 128,
           0 ms de latence)       latence = 256 éch.)
                                        ▲
                                        │ masque (asynchrone, 1 hop de retard)
                          AsyncMaskProvider ── thread ── CoreML ── Neural Engine
```

| Bloc | Rôle | Latence |
|---|---|---|
| `FeedbackSuppressor` | Analyse FFT 1024 en parallèle (sans retarder l'audio) qui repère les pics étroits, persistants et croissants (ratio pic/voisins, rejet des signaux harmoniques = voix). Place jusqu'à 8 filtres notch (Q = 30, jusqu'à −18 dB par défaut) maintenus 10 s. | 0 |
| `SpectralDenoiser` | Wiener à décision dirigée + suivi de minimum pour le bruit. Si un modèle CoreML est présent, son masque est **mélangé** au gain classique (moyenne géométrique, curseur « Neural share », 0 = classique seul). | 256 éch. |
| `AsyncMaskProvider` | Boîtes aux lettres sans verrou entre le thread audio et le thread d'inférence. Un modèle lent ne bloque jamais l'audio : le plugin retombe sur le Wiener classique si le masque a plus de 4 hops. | 0 |

### Mélange réseau + classique

- **Bruit** : `g = g_wiener^(1-m) · g_réseau^m`. Le gain de Wiener est toujours calculé, donc si le réseau
  est en retard ou absent on retombe sur le classique sans coupure.
- **Larsen** : le détecteur classique reste seul décideur, mais le masque du réseau sert d'indice. Là où le
  réseau dit « pas de la voix », les seuils baissent de 4 dB (réaction plus rapide). Là où il dit
  « voix », ils montent de 3 dB. Un indice faux ne peut donc jamais empêcher un vrai larsen d'être coupé
  (testé avec un indice figé à 0, 0,5 et 1).

### Pourquoi le Neural Engine est asynchrone

Le Neural Engine n'est accessible que via CoreML, qui n'offre aucune garantie temps réel (latence de
dispatch de l'ordre de la milliseconde, avec de la gigue). Le laisser dans le chemin audio ferait
craquer le son au moindre retard. Le masque est donc calculé sur la trame précédente et appliqué à la
trame courante : il ne coûte **aucune latence audio**, seulement ~2,7 ms de « fraîcheur » du masque.
Le larsen, lui, doit réagir très vite, il reste donc en DSP pur.

## Compiler et tester (n'importe quelle plateforme)

```sh
cmake -S . -B build -DVDL_BUILD_PLUGIN=OFF && cmake --build build -j
./build/tests/test_core
```

Les tests couvrent : reconstruction parfaite et latence, gain de SNR, boucle de larsen simulée
(−1 dBFS sans plugin → −60 dBFS avec), absence de faux positifs sur voix soutenue (110/220/440 Hz) et
boîte aux lettres asynchrone.

## Compiler le plugin (macOS, Apple Silicon)

```sh
cmake -S . -B build -DVDL_BUILD_PLUGIN=ON && cmake --build build --config Release
```

JUCE 8.0.4 est téléchargé automatiquement. Le plugin est copié dans `~/Library/Audio/Plug-Ins/`.

## Application standalone multicanal (1 à 8 canaux)

`VonDeLarsenApp` : choix de l'interface audio et du nombre de canaux (1–8, menu « Channels »). Le canal *i*
en entrée devient le canal *i* en sortie, chacun avec son propre anti-larsen et débruiteur. Réglages
partagés : sensibilité larsen, coupure max, réduction de bruit. La latence totale (plugin + interface
audio) est affichée en direct. Compiler avec `-DVDL_BUILD_STANDALONE=ON` (activé par défaut sur macOS).
Avec un modèle CoreML, chaque canal lance son propre thread d'inférence : 8 canaux = 8 × 375 inférences/s,
à mesurer avec `bench_mask` avant de compter dessus.

## Activer le Neural Engine

1. Générer un modèle de test (poids aléatoires, **uniquement pour mesurer la latence**) :
   `python tools/make_placeholder_model.py out/denoise.mlpackage`
   puis `xcrun coremlcompiler compile out/denoise.mlpackage out/`.
2. Mesurer : `./build/tests/bench_mask out/denoise.mlmodelc`. Le modèle doit traiter ≥ 95 % des hops
   (un toutes les 2,67 ms). Sinon le masque est périmé et on retombe sur le Wiener.
3. Pour l'utiliser dans le plugin, placer le modèle dans
   `~/Library/Application Support/VonDeLarsen/denoise.mlmodelc`.

Contrat du modèle (float32) : entrée `features` [1,129] (puissance en dBFS par bin), entrée optionnelle
`state_in` [1,S], sortie `mask` [1,129] (gains 0..1), sortie optionnelle `state_out` [1,S].

## État du projet

Vérifié sur Linux : tout le cœur DSP (`core/`, `tests/`).
**Jamais compilé ni exécuté** (pas de Mac disponible lors de l'écriture) : `core/src/CoreMlMaskProvider.mm`,
`plugin/`, `standalone/` et `tools/make_placeholder_model.py`. Attendre quelques corrections au premier build.

À faire :
- **Entraîner un vrai modèle de débruitage** (type DeepFilterNet/RNNoise réduit) respectant le contrat
  ci-dessus. Aucun modèle n'est fourni ; sans lui, le plugin utilise le Wiener classique.
- Mesurer le temps d'inférence réel sur ton Mac (`bench_mask`) et ajuster la taille du modèle.
- Régler les seuils du détecteur de larsen sur de vrais enregistrements (chant tenu, parole forte).
- Tester en conditions réelles avec micro + enceintes : les tests actuels simulent une seule résonance.
- Interface graphique (actuellement l'éditeur générique JUCE).
