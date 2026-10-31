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
| `tests/test_core.cpp` | Tests DSP (voir plus bas), dont les compteurs du journal de bord et la porte vocale |
| `plugin/` | Plugin JUCE AU/VST3 (2 canaux). Éditeur « Poste de pilotage » : `VdlEditor.*` (fenêtre, bouton « ? », journal de bord), `VdlLookAndFeel.*` (boutons, curseur), `VdlArt.h` (fusée, cerveau, ciseau en vecteur), `PreviewMain.cpp` (rend l'éditeur en PNG) |
| `standalone/` | App JUCE multicanal 1–8 canaux : pilote directement un `VonDeLarsenProcessor` et affiche le même éditeur que le plugin, plus « Canaux » et « Réglages audio » |
| `tools/bench_mask.cpp` | Mesure si le modèle tient le rythme des hops (un toutes les 2,67 ms) |
| `tools/make_placeholder_model.py` | Modèle CoreML à poids aléatoires, **pour mesurer la latence seulement** |
| `tools/train_mask.py` | Entraînement du masque + export CoreML (testé en mode `--demo`, voir plus bas) |
| `README.md` | Documentation utilisateur |

## État vérifié / non vérifié

**Vérifié (Linux et Mac, `ctest` passe)** : tout `core/` et `tests/` :
- reconstruction parfaite et latence (gain neutre = simple retard de 256 éch.) ;
- gain de SNR du débruiteur (~+8 dB sur signal de test) ;
- boucle de larsen simulée : −1,3 dBFS sans plugin → −60 dBFS avec ; 1 résonance, puis **3 résonances
  simultanées** (900/2400/5200 Hz, 3 notchs actifs) ;
- aucun faux positif sur voix soutenue synthétique (110/220/440 Hz) ;
- mailbox asynchrone ; mélange réseau/classique ; indice voix fixé à 0/0,5/1 sans bloquer le larsen ;
- compteurs du journal de bord (`FeedbackSuppressor::stats()`) : larsens attrapés, temps retenu, coupe actuelle et record.

**Vérifié sur Mac (Apple Silicon, macOS 15)** :
- `core/src/CoreMlMaskProvider.mm` compile ;
- `train_mask.py --demo` s'entraîne (GPU MPS) et exporte le modèle CoreML ; l'écart torch/CoreML est ~1e-3 (float16) ;
- `bench_mask` sur le modèle démo : 80 µs typique, 617 µs au pire pour un hop de 2666 µs ; 8 instances en parallèle
  (8 canaux) servent 100 % des hops (pics ~5 ms au premier appel, compilation ANE) ;
- `plugin/` (processeur + éditeur) compile avec JUCE 8.0.4 et se rend en PNG via l'outil de prévisualisation.

**Jamais compilé ni exécuté** :
- cibles `plugin` AU/VST3 et `standalone/` complètes (seul l'outil de prévisualisation a été construit) ;
- chargement réel du modèle dans un hôte audio, test avec micro et enceintes.

## Commandes

```sh
# Cœur DSP + tests (toutes plateformes)
cmake -S . -B build -DVDL_BUILD_PLUGIN=OFF -DVDL_BUILD_STANDALONE=OFF
cmake --build build -j && ctest --test-dir build --output-on-failure

# Mac Apple Silicon : tout (plugin + standalone), JUCE téléchargé automatiquement
cmake -S . -B build && cmake --build build --config Release -j

# Voir l'éditeur sans l'installer : rend un PNG (le 2e argument « help » ouvre la notice « ? »)
cmake -S . -B build-ui -DVDL_BUILD_TESTS=OFF -DVDL_BUILD_STANDALONE=OFF -DVDL_BUILD_UI_PREVIEW=ON
cmake --build build-ui --target VonDeLarsenPreview -j
build-ui/plugin/VonDeLarsenPreview_artefacts/Release/VonDeLarsenPreview editor.png [help]
```

## Modèle installé (30 oct. 2026)

`~/Library/Application Support/VonDeLarsen/denoise.mlmodelc`, copie des sources dans `.../training/`
(`denoise.mlpackage`, `mask_net.pt`, `eval_model.py`). Entraînement : `train_mask.py --speech <LibriSpeech dev-clean-2>
--steps 4000 --batch 8 --seconds 3` (~0,33 s par pas), bruit synthétique du script (pas de `--noise`).
Évalué avec `eval_model.py` (mêmes voix que l'entraînement, bruits synthétiques) :

| SNR d'entrée | Erreur sans traitement | Erreur avec le modèle | Réduction |
|---|---|---|---|
| −5 à 3 dB | 0,302 | 0,0107 | 96 % |
| 3 à 10 dB | 0,147 | 0,0064 | 96 % |
| 10 à 20 dB | 0,084 | 0,0039 | 95 % |

Voix propre : −0,8 dB d'énergie. `bench_mask` : 75 µs typique, 608 µs au pire pour un hop de 2 666 µs.
Limites : voix déjà vues pendant l'entraînement, bruit synthétique seulement, aucun test à l'oreille ni en salle.
Pour mieux faire : bruits réels de sono (public, musique, scène) via `--noise`, voix chantée, plus de pas, et un
jeu de voix de validation mis de côté.

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
- Vitesse mesurée sur Mac (MPS, batch 16 × 4 s) : ~1,1 s par pas, soit ~18 h pour 60 000 pas. Prévoir de réduire
  `--steps`, `--batch` ou `--seconds`, ou d'entraîner sur un GPU CUDA.
- L'export CoreML ne passait pas avec `nn.GRUCell` (op `unsafe_chunk` non gérée par coremltools) : la cellule GRU est
  maintenant écrite à la main dans `StepNet`, même ordre de portes que `nn.GRU`. Sans Xcode complet,
  `xcrun coremlcompiler` n'existe pas : charger le `.mlpackage` avec `coremltools.models.MLModel`, puis copier
  `get_compiled_model_path()` vers `out/denoise.mlmodelc`.
- Je n'ai pas pu entraîner depuis la session cloud : le proxy bloquait les hébergeurs de données, pas de GPU.
- Si `bench_mask` dit que le modèle est trop lent : réduire `HIDDEN` dans `train_mask.py`.
- Le modèle **n'apprend pas en direct** : poids figés. Seuls s'adaptent le bruit estimé (Wiener) et les notchs
  du larsen. Idées si besoin : petit modèle de la voix du chanteur ; réglage adaptatif à la salle.

## Interface et journal de bord

Visuel retenu : « Poste de pilotage » (rouge et noir, console de bord). Le gros bouton central règle la réduction de
bruit, avec une fusée à damier qui sert d'aiguille et une bonette de micro ronde sur le nez. Le curseur de mélange
va du ciseau (classique) au cerveau (neural). Le bouton « ? » ouvre une notice qui explique chaque réglage ; les
boutons ont aussi des infobulles.

Le journal de bord vient de `VonDeLarsenProcessor::telemetry()` :
- **Larsens évités** : fréquences attrapées par un notch (canal le plus chargé) ;
- **Temps évité** : durée pendant laquelle au moins un notch retenait un larsen ;
- **Altitude** : fantaisie, 50 m par dB de coupure appliqué en ce moment, avec le plafond atteint ;
- **Temps de vol** : durée de traitement audio ; distance = temps × 11,2 km/s (vitesse de libération), barre Terre-Lune.

Les totaux sont sauvegardés dans l'état du projet (`tmEvents`, `tmAvoided`, `tmFlight`, `tmRecord`, `tmLastHz`).
Le bouton « VIDER » remet tout à zéro. 
Le standalone réutilise le moteur et l'éditeur du plugin (`standalone/CMakeLists.txt` compile les sources de
`plugin/`). Le processeur gère 8 canaux (`kMaxChannels`), les bus du plugin restent mono/stéréo. Les contrôles
propres au standalone passent par `VonDeLarsenProcessor::hostHooks` : si les fonctions sont renseignées, l'éditeur
affiche « Canaux » et « Réglages audio ». Changer le nombre de canaux arrête l'audio, appelle `setActiveChannels()`
puis relance (donc `prepareToPlay()`). Seuls les canaux actifs sont préparés et ont un thread d'inférence.

## Porte vocale

Dans `SpectralDenoiser::runFrame()` : score de voix par hop (bande 190 Hz à 3,9 kHz), mélange classique/réseau
selon « Neural share ». Classique : `clamp((SNR_dB - 4) / 8)` contre le plancher de bruit suivi. Réseau :
`clamp((masque pondéré par l'énergie - 0,25) / 0,35)`, seuils calibrés sur le modèle installé (voix médiane 0,85,
pauses 0,05, bruit seul 0,01). Hystérésis : ouvre à 0,5, reste ouvert tant que le score dépasse 0,3. Maintien 350 ms (150 ms coupait la voix faible, mesuré sur parole synthétique),
fermeture linéaire en dB sur 200 ms, rampe dans le bloc de sortie. Par défaut **éteinte dans le cœur**
(`setGate(false, 20)`), **allumée dans le plugin** (paramètres `gateOn` et `gateDepth`). Tests dans `test_core.cpp` :
silences −20 dB, voix intacte (0 dB), attaques non coupées, pilotage par masque neuronal. À régler à l'oreille :
seuils 0,25/0,35, maintien, fermeture ; refaire la calibration si le modèle change.

## Bypass

`vc::BypassMixer` (`core/`) garde l'entrée dans un historique (poussée avant le traitement) ; quand le bypass est actif,
le signal traité est mélangé en 10 ms avec l'entrée **retardée de la latence du plugin** (256 éch.), donc le calage ne
bouge pas et il n'y a pas de clic. Appelé tout à la fin de `processBlock()`, après l'automix. Tests : bypass coupé =
sortie traitée au bit près, bypass actif = entrée retardée au bit près, fondu de 10 ms (9,98 ms mesurés), pas de saut
d'échantillon, bloc de plus de 16 384 échantillons laissé traité. L'éditeur affiche « BYPASS ACTIF » en haut et dans le
journal de bord.

## Automix

`vc::AutoMixer` (`core/`) est appelé à la fin de `VonDeLarsenProcessor::processBlock()` sur tous les canaux actifs,
avec le `voiceScore()` de chaque débruiteur (calculé à chaque hop même porte coupée). Paramètres `autoMixOn` (coupé par
défaut, le standalone l'active au premier lancement) et `autoMixDepth` (6 à 40 dB). Tests : un orateur 0 dB / autres
−20 dB, deux orateurs −2,9 dB chacun (somme des puissances 1,02), quatre micros inactifs −6 dB, passage de parole
(−0,03 dB en moins de 100 ms), désactivé = identique au bit près, gains figés en silence numérique. À régler à
l'oreille : temps d'ouverture/fermeture, poids d'un micro « bruit » (−20 dB), plancher. Pas encore de priorité par
canal ni de lien entre plusieurs instances.

## Mode learn et annuleur adaptatif (31 oct. 2026)

**Mode learn** (`FeedbackSuppressor::setLearn`, `RingOut`, `VonDeLarsenProcessor::startLearn/stopLearn/clearLearned`). Demandé
d'abord « à vide » (salle silencieuse), puis précisé : **pendant que quelqu'un parle**. Donc la montée de gain ne progresse que
tant que `voiceScore() > 0.5` ; la chaîne (porte, débruiteur, automix) n'est PAS modifiée pendant l'apprentissage ; la voie
rapide du détecteur reste coupée (elle se trompe sur la parole). Les filtres ouverts pendant l'apprentissage sont marqués
`fixed` : jamais relâchés, jamais évincés, conservés par `reset()`/`prepare()`, exportés par `learnedNotch()` et sauvegardés dans
l'état (propriétés `learned0` à `learned7`, chaîne `freq:coupe:q;…`). Passage message vers audio par un seqlock
(`setLearnedNotches` → `applyPendingLoad` à chaque hop). La bande de fréquences de l'éditeur lit un instantané atomique
(`slotViews`). Fenêtre 720×810, réduite au démarrage du standalone si l'écran est trop bas.
Tests : `testLearnAndRingOut` (salle simulée avec voix : résonance trouvée à 1 817 Hz, 21 dB, gain revenu à 0, pas de faux
filtre en salle calme, pas de montée sans voix, notches non relâchés après 14 s, conservés après changement de fréquence
d'échantillonnage). Validation hors dépôt (`learn_real.cpp`) : vraie parole + chaîne complète + modèle, voir README.
À faire : un apprentissage de la voix de l'orateur (profil de spectre et de pente) pour rouvrir la voie rapide sans faux
déclenchement ; marge réglable ; afficher le gain atteint sur la bande.

**Mode High CPU : refait, réduit à la latence.** L'utilisateur voulait un mode lourd pour tout le plugin, gagnant en latence ET en
qualité. L'annuleur de retour (seul gain de qualité mesuré) demandait un bruit de mesure dans la sono, refusé pour « Apprendre la
salle » : retiré (`FeedbackCanceller` reste dans `core/`, testé, non branché). Le bouton `highCpu` est revenu avec un autre
contenu : `SpectralDenoiser::setLowLatency` (fenêtre 128, pas 32, latence 128 échantillons = 2,7 ms). Gain de latence réel et
vérifié (bypass aligné au bit près à chaud) ; **pas de gain de qualité** : avec le réseau −0,1 à −1,1 dB de SI-SDR, en classique
seul −1,5 à −3 dB (voir README, section High CPU). Essayés sans succès : lissage des gains en fréquence, gains calculés sur la
fenêtre longue puis appliqués à la trame courte (désalignés de 64 échantillons, pire). Banc de mesure (hors dépôt) : parole `say`
+ bruit, SI-SDR contre la parole propre. Restent l'indicateur de charge (`cpuLoad`) et `tools/train_mask.py --hidden --layers`.
Pour gagner aussi en qualité il faudrait un modèle entraîné pour fenêtre de 128, ce qui reste à faire.
Ce qui a été essayé et mesuré (à ne pas refaire) :
- recouvrement de 75 % du débruiteur (hop 64) : aucun gain, le double de calcul ;
- gros modèle `--hidden 256 --layers 2`, 1 800 pas, ~40 min : même erreur retirée que le petit sur les bruits synthétiques ;
- le détecteur de larsen ne gagne rien à tourner plus souvent : le délai vient de la preuve à réunir ;
- annuleur : PEM nuisible (blanchit les résonances à apprendre), apprentissage continu avec la voix divergent ou faux, apprentissage
  dans les pauses sans bruit faux (+7,9 dB d'erreur, larsen de 21 s), mesure au bruit −22 dBFS correcte (−30 dB) mais refusée.
Pistes si on rouvre : mesure au bruit très bas (−45 dBFS) mais longue (plus de 100 s, l'erreur baisse en 1/√durée), balayage plutôt
que bruit blanc, de vrais bruits de sono pour juger un gros modèle.

**Annuleur adaptatif de retour** (`FeedbackCanceller`, partitionné fréquentiel, 32×128 coefficients, première prise retardée de D
blocs). Détails de la mise au point :
1. PEM (blanchiment par prédiction linéaire de l'erreur) : **retiré**. Il modélise l'erreur, qui contient l'écho de la salle,
   et blanchit justement les résonances que le filtre doit apprendre : en boucle ouverte l'erreur du filtre restait à −2,7 dB
   au lieu de −54 dB sans lui. `setWhitening(true)` existe encore, coupé par défaut.
2. Apprendre en continu avec la voix : diverge (voix synthétique), ou se dégrade (−0,9 dB de voix sur 40 s de parole réelle,
   filtre à +13 dB de la vraie salle). Adapter seulement dans les pauses : n'a pas suffi non plus.
3. Closed loop avec bruit indépendant : l'erreur du filtre vaut −30 dB à −20 dBFS, −25 dB à −25 dBFS, −15 dB à −35 dBFS,
   −5 dB à −45 dBFS : l'identification demande une excitation indépendante assez forte.
4. Seule voie qui marche : mesure au bruit blanc de 10 s (−22 dBFS), puis filtre **figé** (c'était dans « Apprendre la salle » avec High CPU, retiré à la demande de l'utilisateur)
   (`setLearning(false)`, `setAdapt(false)`). Garde d'énergie (sortie ≤ 1,1 × entrée par bloc). Test de bout en bout du
   processeur complet : `VonDeLarsenPreview x selftest` (mesure, retour lu −5,9 dB pour −6,0, apprentissage avec voix, sauvegarde
   et restauration du projet).
Le filtre appris est exporté en prises temporelles (`exportTaps`, 4 096 flottants) et sauvegardé en base64 dans l'état
(`afc0` à `afc7`, plus `afcDmin0`…) ; `importTaps` ne fait aucune allocation et peut être appelé dans le thread audio, avec
décalage si la taille de bloc de l'hôte a changé. `prepareToPlay` conserve le filtre appris. `peakLoopGainDb()` lit dans le
filtre le niveau de retour le plus fort (FFT de 8 192, hors thread audio). Charge CPU mesurée dans `processBlock` et
affichée dans le bouton. À explorer : suivi très lent (pas minuscule) une fois figé, filtre plus long, mesure par balayage
plutôt que bruit blanc, un bruit moins gênant pour le public (rose, plus bas, plus court) avec un filtre moins précis.

## Détecteur de larsen v2 (30-31 oct. 2026)

Refonte de `FeedbackSuppressor` après un test réel (micro du Mac vers ses haut-parleurs : le larsen restait), puis une
seconde correction quand l'utilisateur a senti que « ça baisse le volume général ».

**Cause du volume qui baissait** : un premier jet du frein se déclenchait sur la parole réelle (coupures jusqu'à 38 dB).
Mes simulations avec une voix synthétique ne le montraient pas. Banc de mesure : `level.cpp` (niveau de vraie parole
étage par étage) et `fp_corpus.cpp` (faux déclenchements sur 243 s de LibriSpeech), dans le scratchpad de la session.
État final : 0 déclenchement sur le corpus avec le modèle, parole −0,8 dB de bout en bout (automix à 2 canaux : −3,1 dB
si les deux canaux captent le même micro). Le standalone envoyait aussi un micro mono dans le haut-parleur gauche
seulement : maintenant un seul micro alimente toutes les sorties et le nombre de canaux est ramené aux entrées réelles.

**Ce qui est dans le code** : hop 128 ; filtre ouvert à 12 dB aussitôt confirmé ; 12 filtres ; Q 20 qui s'élargit
jusqu'à 6 quand un larsen saute dans une résonance large ; seuls les pics à moins de 24 dB du plus fort du spectre
comptent ; le rejet « harmonique » ne s'applique pas à un pic 15 dB au-dessus de ses partenaires ; frein de gain tiré
d'une raie confirmée qui grandit encore (`raiseGuard()`), avec plancher mémorisé dès la première alarme (moitié de la
coupure) et qui baisse à mesure que les filtres se creusent ; garde-fou « entrée chaude » ; coupure max par défaut 24 dB.

**Pistes essayées et abandonnées** (utile pour ne pas les refaire) :
- déclencher le frein sur la croissance du niveau seule : se déclenche sur chaque début de mot (même pente, mêmes bandes,
  la fenêtre de 21 ms transforme une marche en rampe de 8 images) ;
- facteur de crête : parole 4 à 9 dB, début de larsen 5 à 14 dB, ne sépare pas ; pureté de raie (3 bins / 9 bins) :
  parole 0,2 à 0,93, larsen naissant 0,47 à 0,65, ne sépare pas non plus ;
- indice du réseau (`setVoiceHint`) pour rejeter la voix sur la voie rapide : le masque d'un mot qui commence décrit
  encore le silence précédent (une image de retard) donc dit « bruit » ; 394 faux filtres en 243 s ;
- voie rapide « croissance exponentielle régulière » : 6 images = 394 faux déclenchements, 10 images = 35, et elle rate
  les larsens qui saturent avant 27 ms. Reste dans le code derrière `setFastPath()` (faux par défaut).
Le réseau reconnaît bien un larsen médium/aigu comme du bruit (masque 0,12 à 0,19 à 1,9 et 3,2 kHz) mais prend un
larsen grave à 600 Hz pour de la voix (0,97) et les consonnes sifflantes pour de la voix (0,98).

**À explorer si la réaction ne suffit pas** : décaleur de fréquence de 4 à 6 Hz sans latence (paire de passe-tout, gagne
5 à 8 dB de gain avant larsen, aucun risque pour la voix) ; annuleur adaptatif de retour à erreur de prédiction (PEM-AFC,
filtre fréquentiel partitionné de 4 096 coefficients : environ 25 Mflop/s par canal, donc largement dans le budget de
calcul ; il faut décorréler la voix du signal de référence) ; sonde inaudible pour mesurer le gain de boucle.

Réglages à reprendre à l'oreille : `kConfirmStrong` (8), `kConfirmGrowing` (10), `kFirstCutDb` (12), `kGuardMaxDb` (30),
relâchement du frein 2 dB/s, seuils de croissance (3 à 5 dB par 10 ms selon la sensibilité), fenêtre de 24 dB.

## Temps de réaction au larsen (simulation)

Voir le README. Détecteur seul sur boucles dures : premier filtre 140 à 310 ms, audible 150 à 290 ms, puis silence.
Chaîne complète sans voix : le larsen ne démarre pas (porte et débruiteur retirent ~35 dB). À refaire sur une vraie salle.

## À faire

1. Construire la cible plugin AU/VST3 complète sur Mac (la copie vers `~/Library/Audio/Plug-Ins` est automatique) et
   la charger dans un hôte.
2. Entraîner un vrai modèle (voir vitesse plus haut).
3. Régler les seuils du détecteur de larsen sur de vrais enregistrements (chant tenu, parole forte) ; les tests
   n'ont que des résonances et des voix synthétiques.
4. Tester en conditions réelles (micro + enceintes), idéalement latence mesurée de bout en bout.
5. Régler à l'oreille : part du réseau (défaut 0,7), seuils d'indice (−4/+3 dB), coupure max (18 dB).
6. Options : plus de 8 notchs (`kMaxNotches`), réglage du nombre de notchs, latence réduite avec `kN = 128`
   (2,7 ms, débruitage moins fin), plugin à plus de 2 canaux, ouvrir une PR.

## Points de vigilance

- Le détecteur de larsen confond un ton **parfaitement stable** avec un larsen ; la parade actuelle est le rejet
  des signaux avec harmoniques en rapports entiers. Chant tenu très pur : à surveiller.
- Les notchs sont libérés après 10 s sans détection : le larsen peut revenir et être redétecté (cycle lent).
- `CoreMlMaskProvider.mm` gère les sorties float16 et les strides ANE ; premier appel = compilation ANE
  (warm-up fait dans le thread worker).
- Le bypass (`bypass`, bouton rond en haut à droite) est aussi branché sur le bypass de l'hôte via `getBypassParameter()`. Le traitement continue en dessous : le retour est immédiat, mais les compteurs du journal de bord continuent de tourner pendant le bypass.
- Ne mettre aucun appel bloquant ni allocation dans le chemin audio (`process`).
