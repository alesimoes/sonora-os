#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Writes the equivalence test's .nam files and input signal into ../data/.
#
# The models are ours: seeded random weights in the format's architectures,
# no capture of any device, no third-party name in their metadata (ADR-0011
# §4.6, §5). Their weights are dyadic (k / 2^n), so their decimal text
# names one float exactly and any JSON reader lands on the same value.
#
# Shapes:
#   tiny      two arrays of the format's smallest kind, Tanh
#   gated     a gated Tanh array, then ReLU with kernel 2 and the format's
#             newer keys at their neutral values
#   mixed     five arrays: Hardtanh, Softsign (gated), Hardswish,
#             LeakyReLU, Sigmoid
#   feather   the trainer's "feather" size, Fasttanh
#   standard  the trainer's "standard" size (13 802 weights), Tanh
#   lstm_1x16 an LSTM, one layer of 16 (1 201 weights; the trainer's LSTM
#             preset, 1 x 24, is in --presets for timing)
#   lstm_2x8  an LSTM, two stacked layers of 8 (905 weights)
#
# Usage: make_models.py [OUT_DIR]   (default: ../data beside this file)
#        make_models.py --presets DIR   (nano, feather, lite, standard; timing)
import json
import math
import os
import random
import struct
import sys

STANDARD_DIL = [1, 2, 4, 8, 16, 32, 64, 128, 256, 512]
SHORT_DIL = [1, 2, 4, 8, 16, 32, 64]
LONG_DIL = [128, 256, 512, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512]


def array(inp, ch, head, k, dil, act, gated=False, head_bias=False, extra=None):
    a = {
        "input_size": inp,
        "condition_size": 1,
        "head_size": head,
        "channels": ch,
        "kernel_size": k,
        "dilations": dil,
        "activation": act,
        "gated": gated,
        "head_bias": head_bias,
    }
    if extra:
        a.update(extra)
    return a


MODELS = {
    "tiny": [
        array(1, 3, 2, 3, [1, 2], "Tanh"),
        array(3, 2, 1, 3, [8], "Tanh", head_bias=True),
    ],
    "gated": [
        array(1, 4, 4, 3, [1, 2, 4, 8], "Tanh", gated=True),
        array(4, 4, 1, 2, [16, 32, 1], "ReLU", head_bias=True,
              extra={"bottleneck": 4, "layer1x1": {"active": True, "groups": 1},
                     "head1x1": {"active": False, "out_channels": 4, "groups": 1},
                     "groups_input": 1}),
    ],
    "mixed": [
        array(1, 3, 3, 3, [1, 4], "Hardtanh"),
        array(3, 3, 3, 3, [2, 8], "Softsign", gated=True),
        array(3, 3, 2, 2, [1, 16], "Hardswish", head_bias=True),
        array(3, 2, 2, 3, [3, 5], "LeakyReLU"),
        array(2, 2, 1, 3, [32], "Sigmoid", head_bias=True),
    ],
    "feather": [
        array(1, 8, 4, 3, SHORT_DIL, "Fasttanh"),
        array(8, 4, 1, 3, LONG_DIL, "Fasttanh", head_bias=True),
    ],
    "standard": [
        array(1, 16, 8, 3, STANDARD_DIL, "Tanh"),
        array(16, 8, 1, 3, STANDARD_DIL, "Tanh", head_bias=True),
    ],
}


def weights_for(layers, rng):
    """The format's order (common/dsp neural/wavenet.h, WaveNet::load)."""
    w = []

    def put(n, fan):
        # A power of two near 1.5 / sqrt(fan), times k / 128.
        e = round(math.log2(math.sqrt(fan) / 1.5))
        for _ in range(n):
            w.append(rng.randint(-128, 128) / 128.0 / (2.0 ** e))

    for a in layers:
        c, k, h, i = a["channels"], a["kernel_size"], a["head_size"], a["input_size"]
        g = 2 if a["gated"] else 1
        put(c * i, i)
        for _ in a["dilations"]:
            put(g * c * c * k, c * k)
            put(g * c, 16)  # bias
            put(g * c, 1)   # input mixin
            put(c * c, c)   # residual 1x1
            put(c, 16)      # residual bias
        put(h * c, c)
        if a["head_bias"]:
            put(h, 16)
    w.append(0.02)  # the head scale, as the trainer writes it
    return w


def write_model(path, name, layers, seed):
    rng = random.Random(seed)
    doc = {
        "version": "0.5.4",
        "metadata": {"name": "Sonora test network " + name, "loudness": -18.0},
        "architecture": "WaveNet",
        "config": {"layers": layers, "head": None, "head_scale": 0.02},
        "weights": weights_for(layers, rng),
        "sample_rate": 48000,
    }
    with open(path, "w", newline="\n") as f:
        json.dump(doc, f, separators=(",", ":"))
        f.write("\n")


# name: (num_layers, hidden_size), ADR-0111.
LSTMS = {
    "lstm_1x16": (1, 16),
    "lstm_2x8": (2, 8),
}


def lstm_weights_for(layers, hidden, rng):
    """The format's order (common/dsp neural/lstm.h, lstm_weight_count):
    per layer W [4H][in + H], bias [4H], initial h [H], initial c [H]; then
    the head [H] and its bias."""
    w = []

    def put(n, fan):
        e = round(math.log2(math.sqrt(fan) / 1.5))
        for _ in range(n):
            w.append(rng.randint(-128, 128) / 128.0 / (2.0 ** e))

    for layer in range(layers):
        nin = (1 if layer == 0 else hidden) + hidden
        put(4 * hidden * nin, nin)
        put(4 * hidden, 16)  # bias_ih + bias_hh, summed on export
        put(hidden, 16)      # initial h
        put(hidden, 16)      # initial c
    put(hidden, hidden)      # head
    put(1, 64)               # head bias
    return w


def write_lstm(path, name, layers, hidden, seed):
    rng = random.Random(seed)
    doc = {
        "version": "0.5.4",
        "metadata": {"name": "Sonora test network " + name, "loudness": -18.0},
        "architecture": "LSTM",
        "config": {"num_layers": layers, "input_size": 1, "hidden_size": hidden},
        "weights": lstm_weights_for(layers, hidden, rng),
        "sample_rate": 48000,
    }
    with open(path, "w", newline="\n") as f:
        json.dump(doc, f, separators=(",", ":"))
        f.write("\n")


def write_input(path, n=8192):
    """Silence, a quiet sweep, a hot sweep, noise and a step: every regime
    of the activations, from linear to saturated."""
    rng = random.Random(7)
    x = []
    phase = 0.0
    for t in range(n):
        if t < 256:
            v = 0.0
        elif t < 3000:
            f = 80.0 * (40.0 ** ((t - 256) / 2744.0))
            phase += 2.0 * math.pi * f / 48000.0
            v = 0.01 * math.sin(phase)
        elif t < 6000:
            f = 3200.0 * (0.025 ** ((t - 3000) / 3000.0))
            phase += 2.0 * math.pi * f / 48000.0
            v = 1.5 * math.sin(phase)
        elif t < 7500:
            v = 0.5 * (2.0 * rng.random() - 1.0)
        else:
            v = 0.8
        x.append(v)
    with open(path, "wb") as f:
        f.write(struct.pack("<%df" % n, *x))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    if len(sys.argv) == 3 and sys.argv[1] == "--presets":
        # generate.sh --bench: the trainer's four sizes, for timing only.
        os.makedirs(sys.argv[2], exist_ok=True)
        for c, name in ((4, "nano"), (8, "feather"), (12, "lite")):
            layers = [array(1, c, c // 2, 3, SHORT_DIL, "Tanh"),
                      array(c, c // 2, 1, 3, LONG_DIL, "Tanh", head_bias=True)]
            write_model(os.path.join(sys.argv[2], name + ".nam"), name, layers, c)
        write_model(os.path.join(sys.argv[2], "standard.nam"), "standard", MODELS["standard"], 16)
        # The trainer's LSTM preset (1 x 24) and the two test shapes.
        for layers, hidden in ((1, 24), (1, 16), (2, 8)):
            name = "lstm_%dx%d" % (layers, hidden)
            write_lstm(os.path.join(sys.argv[2], name + ".nam"), name, layers, hidden, 300 + hidden)
        return
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "data")
    os.makedirs(out, exist_ok=True)
    for seed, (name, layers) in enumerate(MODELS.items(), start=101):
        write_model(os.path.join(out, name + ".nam"), name, layers, seed)
    for seed, (name, (layers, hidden)) in enumerate(LSTMS.items(), start=201):
        write_lstm(os.path.join(out, name + ".nam"), name, layers, hidden, seed)
    write_input(os.path.join(out, "input.f32"))


if __name__ == "__main__":
    main()
