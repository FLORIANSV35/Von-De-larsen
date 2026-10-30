"""Trains the Neural Engine denoising mask and exports it to CoreML.

The model reproduces the exact front-end of core/src/SpectralDenoiser.cpp:
  - 256-sample frames, hop 128, window sin(pi*j/256), 48 kHz
  - features = 10*log10(|X|^2 * (pi/256)^2 + 1e-10) for the 129 bins
It outputs one gain per bin (0..1). Because the plugin applies the mask computed
from frame t to frame t+1 (zero extra audio latency), the target for the
features of frame t is the ideal mask of frame t+1.

Usage (macOS with Apple Silicon trains on the GPU through MPS):
    pip install torch coremltools numpy scipy soundfile
    python tools/train_mask.py --speech /data/clean_speech --noise /data/noise \
        --steps 60000 --out out/denoise.mlpackage
    xcrun coremlcompiler compile out/denoise.mlpackage out/
    mkdir -p "$HOME/Library/Application Support/VonDeLarsen"
    cp -R out/denoise.mlmodelc "$HOME/Library/Application Support/VonDeLarsen/"

--speech / --noise are folders of .wav/.flac files (any sample rate, resampled to
48 kHz). Without --noise, synthetic noise (white, pink, hum, babble-like) is used.
For live sound, put crowd / music / stage noise in --noise. --demo trains on
synthetic data only, to check the pipeline end to end (the result is not useful).
"""
import argparse
import glob
import math
import os
import random

import numpy as np
import torch
import torch.nn as nn

FS, N, HOP, BINS, HIDDEN = 48000, 256, 128, 129, 128
AMP = math.pi / N
WIN = torch.sin(math.pi * torch.arange(N) / N)


# --------------------------------------------------------------------------- model
class MaskNet(nn.Module):
    """Training form: whole sequences. features [B,T,129] -> mask [B,T,129]."""

    def __init__(self):
        super().__init__()
        self.inp = nn.Linear(BINS, HIDDEN)
        self.gru = nn.GRU(HIDDEN, HIDDEN, batch_first=True)
        self.out = nn.Linear(HIDDEN, BINS)

    def forward(self, f):
        x = torch.relu(self.inp((f + 60.0) / 60.0))
        h, _ = self.gru(x)
        return torch.sigmoid(self.out(h))


class StepNet(nn.Module):
    """Inference form exported to CoreML: one frame, explicit recurrent state."""

    def __init__(self, net):
        super().__init__()
        self.inp, self.out = net.inp, net.out
        self.cell = nn.GRUCell(HIDDEN, HIDDEN)
        g = net.gru
        self.cell.weight_ih.data.copy_(g.weight_ih_l0)
        self.cell.weight_hh.data.copy_(g.weight_hh_l0)
        self.cell.bias_ih.data.copy_(g.bias_ih_l0)
        self.cell.bias_hh.data.copy_(g.bias_hh_l0)

    def forward(self, features, state_in):
        x = torch.relu(self.inp((features + 60.0) / 60.0))
        h = self.cell(x, state_in)
        return torch.sigmoid(self.out(h)), h


# --------------------------------------------------------------------------- audio
def stft(x):
    """x [B,L] -> complex [B,T,129], frames of N with hop HOP, same as the C++ code."""
    fr = x.unfold(-1, N, HOP) * WIN.to(x.device)
    return torch.fft.rfft(fr, dim=-1)


def features(spec):
    return 10.0 * torch.log10(spec.abs() ** 2 * AMP**2 + 1e-10)


def load_files(folder):
    import soundfile as sf
    from scipy.signal import resample_poly

    out = []
    for p in sorted(glob.glob(os.path.join(folder, "**", "*"), recursive=True)):
        if not p.lower().endswith((".wav", ".flac", ".ogg")):
            continue
        try:
            x, sr = sf.read(p, dtype="float32", always_2d=True)
        except Exception:
            continue
        x = x.mean(axis=1)
        if sr != FS:
            g = math.gcd(sr, FS)
            x = resample_poly(x, FS // g, sr // g).astype(np.float32)
        if len(x) > FS:
            out.append(x)
    if not out:
        raise SystemExit(f"no usable audio in {folder}")
    return out


def synth_noise(n, rng):
    kind = rng.choice(["white", "pink", "hum", "babble"])
    w = rng.standard_normal(n).astype(np.float32)
    if kind == "white":
        return w
    spec = np.fft.rfft(w)
    f = np.maximum(np.arange(len(spec)), 1)
    if kind == "pink":
        return np.fft.irfft(spec / np.sqrt(f), n).astype(np.float32)
    if kind == "hum":
        t = np.arange(n) / FS
        h = sum(np.sin(2 * np.pi * 50 * k * t + rng.uniform(0, 6.28)) / k for k in range(1, 8))
        return (h + 0.1 * w).astype(np.float32)
    return np.fft.irfft(spec * np.exp(-((np.arange(len(spec)) * FS / n - 600) / 900) ** 2), n).astype(np.float32)


def synth_speech(n, rng):
    """Only for --demo: harmonic bursts with moving pitch."""
    t = np.arange(n) / FS
    f0 = rng.uniform(90, 300) * (1 + 0.2 * np.sin(2 * np.pi * rng.uniform(0.5, 3) * t))
    ph = 2 * np.pi * np.cumsum(f0) / FS
    s = sum(np.sin(h * ph) / h for h in range(1, 25))
    env = (np.sin(2 * np.pi * rng.uniform(1, 4) * t) > -0.2).astype(np.float32)
    return (s * env * 0.2).astype(np.float32)


def crop(x, n, rng):
    if len(x) <= n:
        return np.pad(x, (0, n - len(x)))
    s = rng.integers(0, len(x) - n)
    return x[s : s + n]


def make_batch(speech, noise, batch, seconds, rng, device):
    n = int(seconds * FS)
    cl, nz = [], []
    for _ in range(batch):
        s = crop(speech[rng.integers(len(speech))], n, rng) if speech else synth_speech(n, rng)
        v = crop(noise[rng.integers(len(noise))], n, rng) if noise else synth_noise(n, rng)
        s = s / (np.abs(s).max() + 1e-6) * rng.uniform(0.1, 0.9)
        snr = rng.uniform(-5, 20)
        v = v / (np.sqrt((v**2).mean()) + 1e-8) * np.sqrt((s**2).mean() + 1e-10) * 10 ** (-snr / 20)
        cl.append(s)
        nz.append(v)
    c = torch.from_numpy(np.stack(cl)).to(device)
    z = torch.from_numpy(np.stack(nz)).to(device)
    return c, z


# --------------------------------------------------------------------------- train
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--speech")
    ap.add_argument("--noise")
    ap.add_argument("--steps", type=int, default=60000)
    ap.add_argument("--batch", type=int, default=16)
    ap.add_argument("--seconds", type=float, default=4.0)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--out", default="out/denoise.mlpackage")
    ap.add_argument("--demo", action="store_true", help="synthetic data only, pipeline check")
    ap.add_argument("--seed", type=int, default=0)
    a = ap.parse_args()
    if not a.demo and not a.speech:
        raise SystemExit("give --speech <folder> (or --demo)")

    rng = np.random.default_rng(a.seed)
    random.seed(a.seed)
    torch.manual_seed(a.seed)
    dev = "cuda" if torch.cuda.is_available() else "mps" if torch.backends.mps.is_available() else "cpu"
    print("device:", dev)

    speech = load_files(a.speech) if a.speech else None
    noise = load_files(a.noise) if a.noise else None
    print(f"speech files: {len(speech) if speech else 'synthetic'}, noise files: {len(noise) if noise else 'synthetic'}")

    net = MaskNet().to(dev)
    opt = torch.optim.AdamW(net.parameters(), lr=a.lr)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, a.steps)

    for step in range(1, a.steps + 1):
        c, z = make_batch(speech, noise, a.batch, a.seconds, rng, dev)
        C, Y = stft(c), stft(c + z)
        mc, my = C.abs(), Y.abs()
        pred = net(features(Y))[:, :-1]  # mask from frame t ...
        # ... is applied to frame t+1; compressed-magnitude loss, like the C++ application
        est = pred * my[:, 1:]
        loss = torch.mean((est.clamp_min(1e-6) ** 0.3 - mc[:, 1:].clamp_min(1e-6) ** 0.3) ** 2)
        opt.zero_grad()
        loss.backward()
        nn.utils.clip_grad_norm_(net.parameters(), 1.0)
        opt.step()
        sched.step()
        if step % 200 == 0 or step == 1:
            print(f"step {step}/{a.steps} loss {loss.item():.5f}")
        if step % 5000 == 0:
            torch.save(net.state_dict(), "mask_net.pt")

    torch.save(net.state_dict(), "mask_net.pt")
    export(net.cpu().eval(), a.out)


def export(net, path):
    import coremltools as ct

    step = StepNet(net).eval()
    ex = (torch.zeros(1, BINS), torch.zeros(1, HIDDEN))
    traced = torch.jit.trace(step, ex)
    model = ct.convert(
        traced,
        convert_to="mlprogram",
        minimum_deployment_target=ct.target.macOS13,
        compute_precision=ct.precision.FLOAT16,
        inputs=[ct.TensorType(name="features", shape=(1, BINS)), ct.TensorType(name="state_in", shape=(1, HIDDEN))],
        outputs=[ct.TensorType(name="mask"), ct.TensorType(name="state_out")],
    )
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    model.save(path)
    print("saved", path)

    # step-by-step must equal the sequence model (also checks the CoreML output on macOS)
    h, outs = torch.zeros(1, HIDDEN), []
    torch.manual_seed(1)
    feats = torch.randn(1, 6, BINS) * 20 - 40
    seq = net(feats).detach()
    for t in range(6):
        m, h = step(feats[:, t], h)
        outs.append(m.detach())
    err = (torch.stack(outs, 1) - seq).abs().max().item()
    print(f"torch step-vs-sequence max error: {err:.2e}")
    assert err < 1e-4
    try:
        h = np.zeros((1, HIDDEN), np.float32)
        worst = 0.0
        for t in range(6):
            r = model.predict({"features": feats[:, t].numpy(), "state_in": h})
            h = r["state_out"]
            worst = max(worst, float(np.abs(r["mask"] - seq[:, t].numpy()).max()))
        print(f"CoreML vs torch max error: {worst:.2e} (float16)")
    except Exception as e:  # prediction only works on macOS
        print("CoreML prediction check skipped:", type(e).__name__)


if __name__ == "__main__":
    main()
