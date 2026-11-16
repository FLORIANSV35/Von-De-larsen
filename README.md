# Von De Larsen

**Anti-feedback and noise reduction for live voice.** An AU / VST3 plugin and a multichannel standalone app for
macOS (Apple Silicon). The goal: push the voice very loud through a PA **without howling**, with as little delay as
possible and without the level moving by itself.

- **Anti-feedback ("larsen")**: adaptive notch filters placed in the time domain (0 ms), with a runaway brake you can
  switch on for unstable rooms.
- **Noise reduction**: a classic Wiener estimator blended with a neural mask running on the Apple Neural Engine (CoreML).
- **Zero-latency mode** ("High CPU"): the denoiser adds 0 ms instead of 5.3 ms.
- **Learn the room**: raise the gain while someone talks; every feedback frequency found becomes a permanent notch.
- Voice gate, Dugan-style automixer for 2–8 microphones, latency-aligned bypass, flight-log telemetry.
- Red-and-black cockpit UI with a rocket knob. Yes, really.

> Status: tested in simulation and in a JUCE VST3 test host. **Not yet validated in a DAW with a real microphone and a
> real PA.** See [Limits](#limits).

## Contents

1. [Install](#install-macos-apple-silicon)
2. [Controls](#controls)
3. [Measured performance](#measured-performance)
4. [How it works](#how-it-works)
5. [Build and test](#build-and-test)
6. [Neural model](#neural-model)
7. [Tools](#tools)
8. [Limits](#limits)

## Install (macOS, Apple Silicon)

### Easiest: the installer

Download `VonDeLarsen-<version>.pkg` from the [latest release](../../releases/latest) and open it. It installs everything:

| Component (each can be unticked under "Customize") | Destination |
|---|---|
| Standalone app, 1–8 channels | `/Applications` |
| **Audio Unit** plugin | `/Library/Audio/Plug-Ins/Components` |
| **VST3** plugin | `/Library/Audio/Plug-Ins/VST3` |
| **Neural Engine profile** (the CoreML denoising model) | `/Library/Application Support/VonDeLarsen/denoise.mlmodelc` |

The installer asks for an administrator password. It is **not signed or notarised** (no Apple Developer account), so
macOS refuses to open it on double-click: right-click it and choose **Open**, or run
`xattr -d com.apple.quarantine ~/Downloads/VonDeLarsen-*.pkg`.

### By hand

The release also ships `VonDeLarsen-macOS.zip`, `VonDeLarsen-AU.zip` and `VonDeLarsen-VST3.zip` (no model inside).

```sh
cd ~/Downloads
unzip VonDeLarsen-macOS.zip -d /Applications
mkdir -p ~/Library/Audio/Plug-Ins/Components ~/Library/Audio/Plug-Ins/VST3
unzip VonDeLarsen-AU.zip   -d ~/Library/Audio/Plug-Ins/Components
unzip VonDeLarsen-VST3.zip -d ~/Library/Audio/Plug-Ins/VST3
```

**Remove the macOS quarantine** (otherwise "is damaged" or "cannot be opened"):

```sh
xattr -dr com.apple.quarantine "/Applications/Von De Larsen.app"
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/"Von De Larsen.component"
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"Von De Larsen.vst3"
```

**Execute permission** (only if you get "Permission denied" because an unzip tool dropped it; the zips keep it):

```sh
chmod +x "/Applications/Von De Larsen.app/Contents/MacOS/Von De Larsen"
chmod +x ~/Library/Audio/Plug-Ins/Components/"Von De Larsen.component/Contents/MacOS/Von De Larsen"
chmod +x ~/Library/Audio/Plug-Ins/VST3/"Von De Larsen.vst3/Contents/MacOS/Von De Larsen"
```

**Neural model** (optional; without it the classic denoiser works alone and the "Neuronal" button reads "NEURONAL ABSENT"):

```sh
mkdir -p ~/Library/Application\ Support/VonDeLarsen
cp -R denoise.mlmodelc ~/Library/Application\ Support/VonDeLarsen/   # your own model wins over the installer's
```

Then: allow microphone access on first launch of the app, restart your audio software and rescan plugins
(Logic: Settings → Plug-ins → Reset & Rescan; Reaper: Preferences → Plug-ins → VST → Re-scan). If Logic rejects the
Audio Unit, validate it with `auval -v aufx Vdl1 Vdls`.

**Uninstall:** delete `/Applications/Von De Larsen.app`, `Von De Larsen.component` and `Von De Larsen.vst3` (from
`/Library/Audio/Plug-Ins/…` or `~/Library/Audio/Plug-Ins/…`), the folders `/Library/Application Support/VonDeLarsen`
and `~/Library/Application Support/VonDeLarsen`, and `~/Library/VonDeLarsen` (standalone settings).

## Controls

| Control | What it does | Default |
|---|---|---|
| **Noise reduction** (big rocket knob) | Maximum noise attenuation, 0–30 dB. Above 3 kHz it only goes half as deep (in dB) to protect sibilants. | 15 dB |
| **Sensitivity** | How early the detector calls a howl. High reacts sooner but may mistake a very pure sustained note. | 50 % |
| **Max cut** | Depth limit of each notch, 6–40 dB. | 24 dB |
| **Chisel ↔ Brain** slider | Classic Wiener (left) to neural mask (right). The two are blended as `g = g_wiener^(1-m) · g_neural^m`. | 0.7 |
| **Neuronal** | Turns the neural mask on or off. Reads "ABSENT" when no model was found. | on |
| **High CPU** | **Zero-latency** denoiser (0 ms instead of 5.3 ms). The button shows the latency. | off |
| **Brake** | When a howl outgrows the notches, cut the whole signal at once. Off = the level never moves by itself. | **off** |
| **Voice gate** + depth | Lowers the sound by 0–40 dB when nobody is speaking. | **off**, 12 dB |
| **Automix** + depth | Dugan-style gain sharing between 2–8 mics. Plugin: off. Standalone: on at first launch. | off, 20 dB |
| **Learn the room** | See [Learn the room](#learn-the-room). | |
| **Bypass** (round button) | Dry sound delayed by the plugin latency, with a 10 ms crossfade. Follows the host's bypass. | off |
| **?** | Built-in help for every control, with the current latency. | |

The flight log shows howls caught, time avoided, a fun "altitude" (50 m per dB of cut), flight time, a progress bar
to the Moon, and the CPU load.

**No automatic level drop.** The plugin never lowers the voice by itself: the gate and the brake are off by default,
and the neural input is level-normalised (see [Neural model](#neural-model)). The one remaining effect is the noise
reduction itself, about 1 dB on average where noise dominates; lower the big knob to remove it.

## Measured performance

All numbers come from simulations with synthetic room models and text-to-speech voices (macOS `say`, plus 243 s of
LibriSpeech for false-positive checks). They compare settings and detect regressions; they do not replace a room test.

### Latency

| Mode | Latency at 48 kHz |
|---|---|
| Normal | 256 samples = **5.33 ms** (the denoiser; the notches add none) |
| **High CPU** | **0 samples reported; 0–2 samples measured** (0.04 ms) |

Measured with an impulse through the VST3 loaded as a black box. The bypass stays aligned bit for bit in both modes.
The host's audio buffer and the sound card add their own latency, which no plugin controls.

### Level kept on the voice

Gain applied to the speech itself (default settings, pink-ish room noise at −60 and −50 dBFS), measured through the
VST3 by projecting the output on the clean speech:

| Speech peak at the input | −15 dBFS | −25 dBFS | −35 dBFS | −45 dBFS |
|---|---|---|---|---|
| Gain on the voice | −0.6 dB | −1.0 to −1.4 dB | −0.8 to −1.2 dB | −0.8 to −1.2 dB |

Before the neural input was level-normalised the same test gave −3.2 dB at −35 dBFS and up to −8 dB at −45 dBFS: the
model had only been trained on speech peaking at −20…−1 dBFS and cut quieter voices as if they were noise.

### Noise reduction quality

SNR gain of the STFT magnitude against the clean speech (phase-blind), TTS voices at −20 dBFS, pink noise at −35 dBFS:

| | Normal mode | High CPU (0 ms) |
|---|---|---|
| With the neural mask (mix 0.7) | +5.2 / +5.2 dB | +5.2 / +5.4 dB |
| Classic only (mix 0) | +3.0 / +4.6 dB | +2.6 / +4.2 dB |

(two voices each). A phase-sensitive metric (SI-SDR) is lower for High CPU (about +1 dB instead of +3 dB with the mask):
the zero-latency filter is minimum-phase, like any minimum-phase EQ, so it shifts phase without changing magnitude.
Our metrics do not capture how that sounds: **listen to it**. With the mask, High CPU loses about 0.3–1.2 dB more at
3–6 kHz than normal mode; with classic only, 1–3 dB more.

### Howl suppression

Speech looped through a simulated room (band-pass resonances, Q 8, 10 ms delay, hard-clipping speaker), measured through
the VST3 over 22 s, time spent with output peaks above −6 dBFS (howl or clipping):

| Loop gain | Unprocessed | Normal mode | High CPU |
|---|---|---|---|
| +3 dB (stable loop for the plugin, 1 or 3 resonances) | howls for 11.7 s of 12 s | 0 s | 0 s |
| +9.5 dB, 1 resonance | not measured | 0 s | 0 to 0.13 s |
| +9.5 dB, 3 resonances | not measured | 0 to 0.15 s | 0 to 0.16 s |

Standalone detector (no gate, no denoiser), hard simulated loops (loud voice, clipping speaker, wide Q 6 resonances):
first notch in 140–310 ms, audible howl (> −20 dBFS) for 150–290 ms, then silence (−63 to −76 dBFS), including at
+20 dB. Worst remaining case: four wide resonances at +20 dB (−30 dBFS residual).
**False triggers on real speech** (41 LibriSpeech utterances, 243 s, levels 0.15–0.6, full chain with the model): 0 notches,
0 dB of brake. These detector figures predate v0.2.4 and were not repeated for it. High CPU results were measured on the
v0.2.3 build.

Why not faster? A word onset and a howl look alike for about 30 ms (same dominant band, same rising slope, same crest
factor). An experimental fast path catches a howl in 25–40 ms but opens 35 useless notches in 243 s of speech, so it is off.

### Learn the room

Real speech, full chain with the model, simulated room, gain swept up to +18 dB only while the voice is present:

| Room | Result |
|---|---|
| Healthy (loop gain −45 dB) | no howl, **no notch learned** (speech alone teaches nothing) |
| Two resonances (900, 2400 Hz) | both found, notches kept with a margin of about +6 dB |

Tested at microphone levels from −20 to +6 dB around a typical speech level. At −40 dB it sometimes finds only one of
two resonances; at +12 dB it aborts on purpose (the signal is too hot to raise the gain safely).

### CPU

About 0.5–0.7 % of one core per channel in the simulations. High CPU adds roughly 15 million multiply-adds per second
per channel. With the neural model each channel runs its own inference thread: 8 channels = 8 × 375 inferences/s
(measure with `bench_mask` before relying on it).

## How it works

```
mic ─▶ FeedbackSuppressor ─▶ SpectralDenoiser ─▶ voice gate ─▶ output
       (time-domain notches,   (STFT 256, hop 128,
        0 ms)                   Wiener + neural mask)
                                      ▲
                                      │ mask (asynchronous, ~1 hop old)
                        AsyncMaskProvider ─ thread ─ CoreML ─ Neural Engine
```

| Block | Role | Latency |
|---|---|---|
| `FeedbackSuppressor` | A 1024-point FFT every 128 samples (in parallel, the audio is not delayed) finds narrow, persistent, growing peaks and rejects harmonic ones (voice). Up to 12 adaptive notches that widen when a howl jumps within a broad resonance. Optional runaway brake. | 0 |
| `SpectralDenoiser` | Decision-directed Wiener estimator with minimum tracking for the noise, blended with the neural mask; voice gate. | 256 samples, or 0 in High CPU |
| `AsyncMaskProvider` | Lock-free mailboxes between the audio thread and the inference thread. A slow model never blocks audio: after 4 stale hops the plugin falls back to the classic gain. | 0 |
| `AutoMixer` | `g_i² = Q_i / ΣQ`, power weighted by each channel's voice score; one talker keeps 0 dB, the others drop to the floor. | 0 |
| `BypassMixer` | Keeps a history of the dry input; crossfades in 10 ms, aligned to the plugin latency. | 0 |

### Zero-latency denoiser (High CPU)

Gains are still estimated on the last 256 samples at the normal pace, with the same neural mask. But instead of applying
them in the frequency domain (which means waiting for a window to fill), each set of gains becomes a **minimum-phase FIR
filter** (cepstral method: log magnitude, fold, exponentiate; 256 taps, a 1024-point cepstrum) that filters the current
samples directly, crossfading from one filter to the next. The filter's own delay is 0–2 samples. Tried and rejected: smaller
estimation hops (8–64 samples, progressively worse because the noise estimator is tuned for 128), 160 vs 256 taps (no
difference in quality), and delaying the signal to realign the gains (under 1 dB gain).

### Voice gate

Once per hop the speech band (about 190 Hz–3.9 kHz) must stand 1.5–7.5 dB above the tracked noise floor, a ratio that does
not depend on the input level. The neural mask only adds a quarter of its score: the real model takes faint steady noise
for clean signal and says "voice" in 86–99 % of the pauses, so on its own it would never let the gate close. The gate
opens at once, holds for 350 ms and closes over 200 ms with a ramp.

### Learn the room

Press the button while someone speaks into the microphone as they will during the show. The output gain rises slowly
(0.5 dB/s, up to +18 dB) **only while voice is present**; the chain is untouched, so the gate and denoiser work in
real conditions. Every howl that appears becomes a **permanent notch** (up to 8) that is never released and is saved with
the project. The climb pauses 2 s at each howl, goes on until 6 dB above the last one, then returns to 0 dB in 2 s. It
stops by itself if the input gets too hot (above −3 dBFS) or after 4 minutes. No test noise is ever sent into the PA.

### Neural and classic, together

The classic Wiener gain is always computed, so if the model is late or missing the sound never drops out. For feedback
detection the mask is only a hint: where it says "not voice" thresholds fall by 4 dB, where it says "voice" they rise by
3 dB, so a wrong hint cannot stop a real howl from being cut (tested with a hint frozen at 0, 0.5 and 1).

### Why the Neural Engine runs asynchronously

CoreML gives no real-time guarantee (about a millisecond of dispatch latency, with jitter). The mask is computed on the
previous frame and applied to the current one: no extra audio latency, only about 2.7 ms of mask freshness.

## Build and test

Portable core and tests (any platform):

```sh
cmake -S . -B build -DVDL_BUILD_PLUGIN=OFF && cmake --build build -j
./build/tests/test_core
```

The tests cover reconstruction and latency, SNR gain, zero latency, howl loops, false positives on sustained voices,
the voice gate (including weak syllables), automix, bypass alignment, learn-the-room, the brake switch and the async mailbox.

Plugin, app and installer (macOS, Apple Silicon, JUCE 8.0.4 fetched automatically):

```sh
cmake -S . -B build -DVDL_BUILD_PLUGIN=ON -DVDL_BUILD_STANDALONE=ON && cmake --build build --config Release
tools/make_installer.sh 0.3.0     # .pkg in dist/ (re-signs the bundles ad hoc; model from ~/Library/Application Support/VonDeLarsen)
```

The plugin is copied to `~/Library/Audio/Plug-Ins/` after each build. `VonDeLarsenPreview` (`-DVDL_BUILD_UI_PREVIEW=ON`)
renders the editor to PNG and runs processor self-tests (`x selftest`, `x highcpu`, `x uilearn`).

## Neural model

Contract (float32): input `features` [1,129] (power in dBFS per bin), optional `state_in` [1,S]; output `mask` [1,129]
(gains 0..1), optional `state_out` [1,S]. `tools/train_mask.py` trains a small GRU mask network on speech plus noise and
exports it to CoreML; `tools/bench_mask.cpp` checks that inference keeps up (≥ 95 % of the hops, one every 2.67 ms).

The shipped model was trained 4000 steps (about 22 minutes on Apple Silicon) on LibriSpeech dev-clean-2 with **synthetic**
noise (white, pink, 50 Hz hum, filtered crowd noise). On similar clips it removes about 95 % of the magnitude error, with
a 0.8 dB loss on clean voice. It has never seen real room noise, music or singing: it is a starting point, not a final model.

**Level handling.** Training speech peaked at −20…−1 dBFS, so the spectrum given to the model is lifted into that range
(at most 40 dB, never above the noise floor + 12 dB). Its mask is a gain, which does not depend on the level it was
computed at.

## Tools

| Tool | Purpose |
|---|---|
| `tools/train_mask.py` | Train and export the CoreML mask model (`--hidden`, `--layers`). |
| `tools/make_installer.sh` | Build the `.pkg` installer (app, AU, VST3, model). |
| `tools/plugin_probe/` | Load any VST3 as a black box and measure latency, level and behaviour in a howling loop. |
| `tools/bench_mask.cpp` | Neural Engine inference benchmark. |
| `tools/make_placeholder_model.py` | Random-weights model, **only to measure latency**. |

## Limits

- Never run in a DAW with a real microphone and a real PA. The simulations use one or three resonances, TTS voices and
  one channel. The AU has not been through `auval`.
- Arm64 only (the Neural Engine path needs Apple Silicon). The installer is unsigned.
- High CPU's phase behaviour has not been judged by ear, and it lets a little more howl through in very strong loops.
- The neural model is small and trained on synthetic noise; it cannot tell faint noise from voice, which is why the gate
  does not rely on it.
- When the loop gain exceeds what the notches can fix, the optional brake lowers the voice too (16–30 dB in extreme cases)
  and the flight log says how much to lower the volume. That is physics: the loop gain must get back below 1.
- A single spurious learned notch can happen (about one in 2 minutes of speech): clear it and learn again.
- The adaptive feedback canceller in `core/FeedbackCanceller` is **not wired into the plugin**: it works very well in simulation
  but only learns with an audible probe noise, which "Learn the room" must not send. Notes and measurements: `NOTES.md`.

Development notes (in French) with all experiments, including abandoned ones: [`NOTES.md`](NOTES.md).
