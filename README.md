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
| `SpectralDenoiser` | Wiener à décision dirigée + suivi de minimum pour le bruit, plus la porte vocale. Si un modèle CoreML est présent, son masque est **mélangé** au gain classique (moyenne géométrique, curseur « Neural share », 0 = classique seul). | 256 éch. |
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

`VonDeLarsenApp` : même moteur et même interface « Poste de pilotage » que le plugin. En haut à gauche, le
sélecteur « Canaux » (1–8) ; en haut à droite, « Réglages audio » (choix de la carte son, des entrées et des
sorties). Le canal *i* en entrée devient le canal *i* en sortie, chacun avec son propre anti-larsen et
débruiteur. Réglages partagés : sensibilité larsen, coupure max, réduction de bruit, mélange ciseau/cerveau.
La notice « ? » affiche la latence (plugin + carte son). L'état, la carte son choisie et le journal de bord sont
sauvegardés à la fermeture dans `~/Library/Application Support/VonDeLarsen/standalone.xml`.
Compiler avec `-DVDL_BUILD_STANDALONE=ON` (activé par défaut sur macOS).
Avec un modèle CoreML, chaque canal lance son propre thread d'inférence : 8 canaux = 8 × 375 inférences/s,
à mesurer avec `bench_mask` avant de compter dessus.

## Porte vocale

Comme le plugin ne traite que la voix, une porte baisse le son quand personne ne parle (réglages « Porte vocale »
et « Affaiblissement sans voix », 0 à 40 dB, 12 dB par défaut). Une fois par hop, `SpectralDenoiser` juge la bande
vocale (~190 Hz à 3,9 kHz) : avec le réseau, il regarde la moyenne du masque pondérée par l'énergie (voix ≈ 0,85,
bruit seul ≈ 0,01 à 0,05 sur le modèle installé) ; sans réseau, il compare l'énergie au plancher de bruit suivi
(seuil entre 1,5 et 7,5 dB). La porte s'ouvre aussitôt, attend 350 ms, puis se ferme en 200 ms avec une rampe sans
clic. Elle n'ajoute aucune latence : le détecteur voit 5 ms d'avance sur la sortie. Fermer la porte baisse aussi le
gain de boucle, donc les risques de larsen. Le voyant « VOIX / SILENCE » montre l'état.

Limite : un chant tenu très pur, ou une pause de plus d'une demi-seconde en pleine phrase, peut fermer la porte.
Les silences courts entre deux mots ne la ferment pas (350 ms de maintien).

## High CPU (latence mini)

Bouton « HIGH CPU » sous le curseur ciseau/cerveau (paramètre `highCpu`, éteint par défaut). Il raccourcit la fenêtre
d'analyse du débruiteur de 256 à 128 échantillons (rembourrée à zéro jusqu'à la FFT de 256) et le pas de 128 à 32 :
la **latence passe de 5,3 ms à 2,7 ms** (chiffre affiché sur le bouton, annoncé à l'hôte), le filtrage tourne 4 fois
plus souvent. Le réseau neuronal garde son rythme d'entraînement : il voit toujours une fenêtre de 256 toutes les
128 échantillons, et son dernier masque s'applique à chaque trame courte. Le bypass reste aligné au bit près (test
`VonDeLarsenPreview x highcpu`). Le détecteur de larsen n'est pas concerné (il n'ajoute aucune latence).

**Ce que ça ne fait pas : améliorer la qualité.** Mesuré sur de la parole synthétique (`say`) avec bruit rose ou blanc
à −30..−40 dBFS, en SI-SDR contre la parole propre : avec le réseau, High CPU perd 0,1 à 1,1 dB par rapport au mode
normal (+2,5 à +3,2 dB gagnés contre +3,0 à +3,9 dB) ; en classique seul, il perd 1,5 à 3 dB, la fenêtre courte
distinguant mal la voix du bruit. Le bruit des pauses baisse un peu plus (1 dB). C'est le compromis de la physique
(latence contre résolution en fréquence) : on a essayé le lissage des gains en fréquence et le calcul des gains sur la
fenêtre longue, aucun ne rattrape. À utiliser pour la latence, avec le curseur du côté cerveau.

## Bypass

Le bouton rond (icône d'alimentation) en haut à droite coupe tout le traitement. Le son passe tel quel, retardé de la
latence du plugin (256 échantillons, 128 en High CPU) pour rester aligné, avec un fondu de 10 ms : ni saut de timing ni clic. Le
traitement continue en dessous, donc le retour est immédiat. Le bypass de l'hôte audio (AU/VST3) agit sur le même
paramètre.

## Automix (partage du gain entre les micros)

Avec plusieurs canaux, un automixeur façon Dan Dugan partage le gain : la somme des carrés des gains vaut toujours
un. Un seul orateur garde 0 dB et les autres micros descendent au plancher (réglable, 6 à 40 dB, 20 par défaut) ;
deux orateurs égaux ont −3 dB chacun ; quand personne ne parle, les gains restent partagés (4 micros : −6 dB chacun),
donc le nombre de micros ouverts n'ajoute ni bruit ni larsen. La puissance de chaque canal est pondérée par son score
de voix (celui de la porte vocale) : un micro qui n'entend que du bruit ou la voix d'un voisin ne prend pas le gain.
Ouverture en 10 ms, fermeture en 100 ms, détecteur 5 ms / 150 ms, gains figés en silence numérique, aucune latence.
Les barres de la rangée « Automix » montrent le gain de chaque canal. Il faut au moins 2 canaux. Dans le standalone il
est actif au premier lancement ; dans le plugin il est coupé par défaut (sur un bus stéréo L/R identique, il
retirerait 3 dB à chaque côté). Code : `core/src/AutoMixer.cpp`, branché en fin de `processBlock`.

## Modèle installé

Un premier modèle est installé dans `~/Library/Application Support/VonDeLarsen/denoise.mlmodelc`. Il a été
entraîné 4 000 pas (environ 22 min sur Apple Silicon) sur LibriSpeech dev-clean-2 (voix lues, 1 089 fichiers) avec
du bruit **synthétique** (blanc, rose, ronflement 50 Hz, bruit de foule filtré). Sur des clips bruités de même
nature, il réduit l'erreur de magnitude d'environ 95 % ; la voix propre perd 0,8 dB d'énergie. Il n'a jamais vu de
vrai bruit de salle, de musique ni de chant : c'est un point de départ, pas un modèle final. Les fichiers
d'entraînement sont copiés dans `~/Library/Application Support/VonDeLarsen/training/`.

## Activer le Neural Engine

1. Générer un modèle de test (poids aléatoires, **uniquement pour mesurer la latence**) :
   `python tools/make_placeholder_model.py out/denoise.mlpackage`
   puis le compiler : `xcrun coremlcompiler compile out/denoise.mlpackage out/` (nécessite Xcode complet), ou sans
   Xcode : `coremltools.models.MLModel("out/denoise.mlpackage").get_compiled_model_path()` puis copier ce dossier.
2. Mesurer : `./build/tests/bench_mask out/denoise.mlmodelc`. Le modèle doit traiter ≥ 95 % des hops
   (un toutes les 2,67 ms). Sinon le masque est périmé et on retombe sur le Wiener.
3. Pour l'utiliser dans le plugin, placer le modèle dans
   `~/Library/Application Support/VonDeLarsen/denoise.mlmodelc`.

Contrat du modèle (float32) : entrée `features` [1,129] (puissance en dBFS par bin), entrée optionnelle
`state_in` [1,S], sortie `mask` [1,129] (gains 0..1), sortie optionnelle `state_out` [1,S].

## Apprendre la salle (mode learn)

Bouton « Apprendre la salle ». Pendant le réglage, **quelqu'un parle dans le micro** comme pendant le spectacle. Le gain de
sortie monte doucement (0,5 dB/s, jusqu'à +18 dB), mais **seulement pendant que la voix est présente** : la chaîne n'est pas
modifiée, donc la porte vocale et le débruiteur sont dans leurs conditions réelles et la voix excite la salle comme sur
scène. Chaque larsen qui apparaît devient un **filtre permanent** (8 au plus) qui ne se relâche jamais et qui est sauvegardé
avec le projet. La montée marque une pause de 2 s à chaque larsen trouvé, puis continue jusqu'à 6 dB au-dessus du dernier,
et le gain revient à zéro en 2 s. Elle s'arrête d'elle-même si le signal devient trop chaud (au-dessus de −3 dBFS) ou après
4 minutes. La bande de fréquences à droite montre les filtres : grands et clairs pour les filtres appris, courts et ternes
pour ceux posés en direct. « Effacer » les retire.

Mesuré avec de la vraie parole et la chaîne complète (modèle, porte, débruiteur) dans une salle simulée :

| Salle | Durée | Résultat |
|---|---|---|
| Saine (gain de boucle −45 dB) | 34 s | aucun larsen, **aucun filtre appris** : la parole seule n'apprend rien |
| Deux résonances (900 et 2 400 Hz) | 39 s | filtres à 861 Hz et 2 398 Hz (21 dB), plus un voisin à 2 618 Hz, marge de +10 dB |

Limite : un faux filtre isolé peut arriver (la détection lente en fait environ un en 2 minutes de parole) : « Effacer » et
recommencer.

## Annuleur adaptatif de retour (expérimental, non branché)

`FeedbackCanceller` (dans `core/`, testé, **non utilisé par le plugin**) : un filtre fréquentiel de 4 096 coefficients (85 ms,
environ 1 % de processeur par canal) qui apprend comment ce qui sort des enceintes revient dans le micro à travers la salle et
soustrait ce retour avant la chaîne. Mesuré en simulation, quand il est bien appris, il est très efficace : filtre à −50 dB de la
vraie salle en boucle ouverte, retour annulé de 35 dB, boucle à +20 dB de gain sans larsen (−27 dBFS contre −0,4 dBFS pour le
détecteur seul), 1 filtre étroit à poser au lieu de 8 sur un haut-parleur qui sature, voix intacte à −0,01 dB.

**Je l'ai retiré du produit parce qu'il ne s'apprend pas sans bruit audible.** Il demande une excitation indépendante assez forte :
10 s de bruit à −22 dBFS dans les enceintes donnent un filtre à −30 dB de la vraie salle, −25 dBFS à −25 dB, −35 dBFS à −15 dB,
−45 dBFS à −5 dB. Avec la voix seule comme excitation, c'est faux : un filtre qui apprend en continu diverge (la voix ressemble à
ce que le micro entend déjà, il apprend à la prédire à partir de sa propre sortie), et même en n'adaptant que dans les pauses
et sans aucun bruit, il apprend une salle fausse (erreur +7,9 dB, pire que rien) et laisse le larsen s'installer 21 s sur un
haut-parleur qui sature. Comme « Apprendre la salle » ne doit pas envoyer de bruit dans la sono, il n'y a plus de moyen propre de
l'entraîner : le code reste pour qui accepterait une mesure au bruit.

Aussi essayés et abandonnés pour « tout le plugin » : un recouvrement de 75 % du débruiteur (aucun gain : SNR 3,89 contre 3,94 dB
avec le réseau, 3,27 contre 3,82 sans, pour le double de calcul), et un gros modèle neuronal à deux couches de 256 neurones
(même erreur retirée que le petit : 96,4 / 95,6 / 95,6 % contre 96,5 / 95,6 / 95,4 %, avec des bruits d'entraînement synthétiques
trop faciles pour juger ; `tools/train_mask.py` accepte `--hidden` et `--layers`, poids gardés dans
`~/Library/Application Support/VonDeLarsen/training/hq/`). Le journal de bord affiche la charge processeur.

## Temps de réaction au larsen

Le but du plugin est d'envoyer très fort de la voix sans larsen. Le détecteur (`FeedbackSuppressor`) a trois étages,
tous sans latence, et il n'agit que sur des preuves qui ne peuvent pas être de la voix :

1. **Filtres notch** : analyse toutes les 2,7 ms. Un pic fort et stable pendant 21 ms, ou qui grandit encore au bout
   de 27 ms, ouvre un filtre aussitôt à 12 dB, qui s'approfondit jusqu'à « Coupure max » (24 dB par défaut, jusqu'à
   40). Un larsen qui saute dans une résonance large élargit le filtre voisin au lieu d'en consommer un autre
   (12 filtres). Seuls comptent les pics à moins de 24 dB du plus fort du spectre.
2. **Frein de gain** : un pic confirmé qui grandit encore coupe tout le son d'un coup, de la vitesse de montée plus
   4 dB (6 à 30 dB). Il se relâche de 2 dB/s, jamais plus bas que ce que la boucle a montré qu'elle exige, et ce
   plancher baisse à mesure que les filtres se creusent.
3. **Garde-fou** : une entrée au-dessus de −6 dBFS pendant 80 ms ajoute 6 dB de frein par pas.

**Faux déclenchements sur la parole réelle** (LibriSpeech, 41 phrases de locuteurs différents, 243 s, niveaux de 0,15
à 0,6, dans la chaîne complète avec le modèle) : 0 filtre, 0 dB de frein. Sans le modèle : 2 filtres, 0 dB de frein.

**Réaction sur des boucles dures simulées** (voix forte, haut-parleur qui sature, résonances larges Q 6, détecteur
seul) : premier filtre en 140 à 310 ms, larsen audible (> −20 dBFS) pendant 150 à 290 ms, puis silence (−63 à −76
dBFS), y compris à +20 dB. Cas extrême qui reste mal réglé : 4 résonances larges à +20 dB (résiduel à −30 dBFS). Avant
la refonte : 4 800 ms audible et jamais éteint à +12 dB.

**Dans la chaîne complète, sans voix**, ces mêmes boucles ne démarrent même plus : la porte vocale (−20 dB) et le
débruiteur (−15 dB) retirent environ 35 dB de gain de boucle dès que personne ne parle. Le larsen ne peut naître que
pendant que quelqu'un parle, et c'est là que les 150 à 300 ms ci-dessus s'appliquent.

**Pourquoi pas plus vite ?** Un début de mot et la naissance d'un larsen se ressemblent pendant environ 30 ms : même
bande dominante, même pente de montée, même facteur de crête. Mesuré sur de la vraie parole : pureté de raie, facteur
de crête et pente ne les séparent pas ; le masque du réseau a une image de retard et décrit encore le silence au
début d'un mot. Une voie rapide expérimentale (`setFastPath(true)`, croissance exponentielle régulière sur 27 ms)
détecte le larsen en 25 à 40 ms mais ouvre 35 filtres inutiles en 243 s de parole : coupée. Une version moins stricte
(6 images) en ouvrait 394 et coupait le son de 38 dB, ce qui donnait l'impression que « le volume baisse ».

Limites : quand le gain de boucle dépasse ce que les filtres corrigent, le frein baisse aussi la voix (16 à 30 dB dans
les cas extrêmes) et le journal affiche « Gain coupé de X dB : baissez un peu le volume ». C'est de la physique : il faut
que le gain de boucle repasse sous 1. Le frein peut pomper une fois ou deux avant de trouver son plancher.

**Annuleur adaptatif de retour : voir « Annuleur adaptatif de retour (expérimental, non branché) » plus haut.**

Autre piste : décaleur de fréquence de quelques hertz (gagne 5 à 8 dB de gain avant larsen sans rien détecter).

## État du projet

Vérifié sur Linux et sur Mac : tout le cœur DSP (`core/`, `tests/`), la compilation de `CoreMlMaskProvider.mm`,
du plugin (processeur et éditeur) et du standalone, l'entraînement et l'export CoreML, le benchmark Neural Engine.
**Jamais exécuté** : le plugin AU/VST3 dans un hôte audio, et le standalone avec un vrai micro et des enceintes.

À faire :
- **Entraîner un vrai modèle de débruitage** (type DeepFilterNet/RNNoise réduit) respectant le contrat
  ci-dessus. Aucun modèle n'est fourni ; sans lui, le plugin utilise le Wiener classique.
- Mesurer le temps d'inférence réel sur ton Mac (`bench_mask`) et ajuster la taille du modèle.
- Régler les seuils du détecteur de larsen sur de vrais enregistrements (chant tenu, parole forte).
- Tester en conditions réelles avec micro + enceintes : les tests actuels simulent une seule résonance.
