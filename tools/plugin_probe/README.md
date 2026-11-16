# plugin_probe : mesurer un plugin VST3 comme une boîte noire

Charge un VST3 (le nôtre ou un autre), lui envoie de l'audio et lit ce qui sort. Il n'inspecte rien du plugin :
pas de désassemblage, pas de lecture de son code. À n'utiliser que sur un plugin dont on a le droit de se servir.

```sh
cmake -S tools/plugin_probe -B build-probe && cmake --build build-probe -j
PROBE=$(find build-probe -name probe -type f -perm +111 | head -1)
$PROBE "Von De Larsen.vst3" info          # paramètres, latence, voies
$PROBE "Von De Larsen.vst3" latency       # impulsion : retard mesuré
WAV=voix.wav $PROBE plugin.vst3 speech2   # parole sans boucle : retard et variation de niveau (8 s de chauffe)
WAV=voix.wav $PROBE plugin.vst3 tone2     # tonalité de 1 kHz après la parole : vitesse et profondeur de coupure
WAV=voix.wav G=3.0 SECS=30 FROM=8 $PROBE plugin.vst3 voiceloop    # parole dans une boucle qui siffle (1 résonance)
WAV=voix.wav G=3.0 SECS=30 FROM=8 $PROBE plugin.vst3 voiceloop3   # idem avec 3 résonances (800, 1500, 3100 Hz)
WAV=voix.wav SECS=120 $PROBE plugin.vst3 mutemap                  # quand la sortie devient silencieuse (démos)
$PROBE plugin.vst3 howl1 "bypass=1"       # larsen amorcé par du bruit seul, avec un réglage imposé
```

- `voix.wav` : WAV 16 bits mono, 48 kHz (par exemple `say -o voix.wav --file-format=WAVE --data-format=LEI16@48000 "..."`).
- Le réglage d'un paramètre se donne en fin de ligne, `nom=valeur` (valeur normalisée 0..1, le nom se cherche par fragment).
- La boucle : micro = voix + salle (un filtre passe-bande Q 8, gain `G`) alimentée par la sortie retardée de 10 ms.
  `G = 1,4` fait siffler la salle à vide (+3 dB de gain de boucle), `3,0` la fait siffler fort (+9,5 dB).
- Résultat : niveau de la sortie par fenêtres de 10 ms (p50, p90, p99, max) et durée passée au-dessus de −6 dBFS.
  Sans traitement (`bypass=1`), la boucle sature pendant presque toute la mesure.
- Limites : une seule salle simulée, voix de synthèse, une voie. Les chiffres comparent des réglages, ils ne remplacent
  pas un essai en salle.
