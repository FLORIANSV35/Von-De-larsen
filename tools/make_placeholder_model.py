"""Builds a RANDOM-WEIGHT recurrent CoreML model with the plugin's I/O contract.

It is only for benchmarking Neural Engine latency (tools/bench_mask) and for
checking that the plugin wiring works. Its masks are noise: do not listen to it.
Replace it with a real trained denoiser exporting the same inputs/outputs.

    pip install torch coremltools
    python tools/make_placeholder_model.py out/denoise.mlpackage
    xcrun coremlcompiler compile out/denoise.mlpackage out/   # -> out/denoise.mlmodelc
"""
import sys

import coremltools as ct
import torch
import torch.nn as nn

BINS, HIDDEN = 129, 128


class Net(nn.Module):
    def __init__(self):
        super().__init__()
        self.inp = nn.Linear(BINS, HIDDEN)
        self.cell = nn.GRUCell(HIDDEN, HIDDEN)
        self.out = nn.Linear(HIDDEN, BINS)

    def forward(self, features, state_in):
        x = torch.relu(self.inp((features + 60.0) / 60.0))
        h = self.cell(x, state_in)
        return torch.sigmoid(self.out(h)), h


def main(path):
    net = Net().eval()
    ex = (torch.zeros(1, BINS), torch.zeros(1, HIDDEN))
    traced = torch.jit.trace(net, ex)
    model = ct.convert(
        traced,
        convert_to="mlprogram",
        minimum_deployment_target=ct.target.macOS13,
        compute_precision=ct.precision.FLOAT16,
        inputs=[ct.TensorType(name="features", shape=(1, BINS)),
                ct.TensorType(name="state_in", shape=(1, HIDDEN))],
        outputs=[ct.TensorType(name="mask"), ct.TensorType(name="state_out")],
    )
    model.save(path)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "denoise.mlpackage")
