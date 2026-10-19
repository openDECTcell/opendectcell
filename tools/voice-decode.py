#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Work out how a captured DECT voice slot is encoded.

`dect-stubd -w FILE` dumps the call's TDM slot raw: two bytes per frame, in
frame order, nothing interpreted. The slot itself is measured (channel 1 is
TDM slot 0), but what sits inside those two bytes is not. At 8000 frames/s the
plausible readings are:

  * 16-bit linear PCM at 8 kHz (either byte order)
  * A-law at 16 kHz, both bytes being consecutive samples
  * A-law at 8 kHz in one byte of each pair, the other byte padding

This script scores each reading and prints the ranking, then writes the best
few out as WAV files to listen to. The scoring is deliberately crude but it
separates speech from noise well: real voice has most of its energy below
1 kHz, a high zero-crossing count only in fricatives, and -- crucially -- it
has silences, so its short-term energy varies a lot. A misread stream looks
like white noise: flat spectrum, near-constant energy.

Usage:  voice-decode.py capture.raw [-o outdir]
"""
import argparse
import math
import os
import struct
import sys
import wave

# ITU-T G.711 A-law, same table as dect_audio.c.
def alaw_decode(a):
    a ^= 0x55
    sign = a & 0x80
    exponent = (a & 0x70) >> 4
    mantissa = a & 0x0F
    if exponent == 0:
        sample = (mantissa << 4) + 8
    else:
        sample = ((mantissa << 4) + 0x108) << (exponent - 1)
    return -sample if sign else sample


ALAW = [alaw_decode(i) for i in range(256)]


# ITU-T G.711 mu-law, the other companding AVM's DSP could plausibly use.
def ulaw_decode(u):
    u = ~u & 0xFF
    sign = u & 0x80
    exponent = (u >> 4) & 0x07
    mantissa = u & 0x0F
    sample = ((mantissa << 3) + 0x84) << exponent
    sample -= 0x84
    return -sample if sign else sample


ULAW = [ulaw_decode(i) for i in range(256)]


def readings(data):
    """Yield (name, rate, samples) for every candidate interpretation.

    A-0029 Sec.4 warns that a 16-bit TDM word may hold two independent 8-bit
    voice channels, one companded sample per byte lane. So the per-lane
    readings matter at least as much as the 16-bit ones, and a lane may be
    A-law or mu-law.
    """
    n = len(data) // 2 * 2
    data = data[:n]

    yield ('pcm16-le @8k', 8000, list(struct.unpack('<%dh' % (n // 2), data)))
    yield ('pcm16-be @8k', 8000, list(struct.unpack('>%dh' % (n // 2), data)))
    yield ('alaw-both @16k', 16000, [ALAW[b] for b in data])
    yield ('ulaw-both @16k', 16000, [ULAW[b] for b in data])
    yield ('alaw-low @8k', 8000, [ALAW[b] for b in data[0::2]])
    yield ('alaw-high @8k', 8000, [ALAW[b] for b in data[1::2]])
    yield ('ulaw-low @8k', 8000, [ULAW[b] for b in data[0::2]])
    yield ('ulaw-high @8k', 8000, [ULAW[b] for b in data[1::2]])


def score(samples, rate):
    """Higher is more speech-like. Returns (score, detail dict)."""
    if len(samples) < rate // 2:
        return -1e9, {}

    # Normalise so the measures do not depend on the reading's amplitude.
    peak = max(1, max(abs(s) for s in samples))
    xs = [s / peak for s in samples]

    # Short-term energy over 20 ms frames: speech has silences, noise does not.
    win = max(1, rate // 50)
    energies = []
    for i in range(0, len(xs) - win, win):
        frame = xs[i:i + win]
        energies.append(math.sqrt(sum(v * v for v in frame) / win))
    if not energies:
        return -1e9, {}
    mean_e = sum(energies) / len(energies)
    if mean_e <= 1e-9:
        return -1e9, {}
    spread = math.sqrt(sum((e - mean_e) ** 2 for e in energies) / len(energies))
    dynamics = spread / mean_e

    # Zero crossings per second: voiced speech is a few hundred to ~2k,
    # white noise sits near rate/2.
    zc = sum(1 for i in range(1, len(xs)) if (xs[i - 1] < 0) != (xs[i] < 0))
    zcr = zc * rate / len(xs)
    zcr_norm = zcr / (rate / 2)

    # Adjacent-sample correlation: speech is strongly correlated, noise is not.
    num = sum(xs[i] * xs[i - 1] for i in range(1, len(xs)))
    den = sum(v * v for v in xs)
    corr = num / den if den else 0.0

    # Correlated and dynamic and not hissing.
    s = corr * 2.0 + dynamics - zcr_norm * 2.0

    # A constant or near-DC stream scores a perfect correlation of 1.0 without
    # being audio at all -- that is what a padding byte looks like. Speech
    # always crosses zero at least a few dozen times a second.
    if zcr < 20.0:
        s -= 10.0

    return s, {'corr': corr, 'dynamics': dynamics, 'zcr': zcr, 'rms': mean_e}


def write_wav(path, samples, rate):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        clipped = [max(-32768, min(32767, int(s))) for s in samples]
        w.writeframes(struct.pack('<%dh' % len(clipped), *clipped))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('-o', '--outdir', default=None,
                    help='write WAVs here (default: alongside the capture)')
    ap.add_argument('-s', '--slots', type=int, default=0,
                    help='frames hold this many interleaved 16-bit slots; '
                         'each is scored separately (0 = one slot only)')
    ap.add_argument('--skip', type=float, default=0.0,
                    help='ignore the first N seconds (e.g. before audio starts)')
    args = ap.parse_args()

    with open(args.capture, 'rb') as f:
        data = f.read()
    if len(data) < 32:
        sys.exit('capture too short: %d bytes' % len(data))

    nslots = max(1, args.slots)
    frame = nslots * 2
    nframes = len(data) // frame
    if args.skip:
        off = int(args.skip * 8000) * frame
        data, nframes = data[off:], nframes - int(args.skip * 8000)
    print('%d bytes, %d slots, %d frames = %.2f s at 8000 frames/s\n'
          % (len(data), nslots, nframes, nframes / 8000.0))

    outdir = args.outdir or os.path.dirname(os.path.abspath(args.capture))
    os.makedirs(outdir, exist_ok=True)

    results = []
    for slot in range(nslots):
        if nslots == 1:
            sdata = data
        else:
            sdata = bytearray()
            for f in range(nframes):
                sdata += data[f * frame + slot * 2: f * frame + slot * 2 + 2]
            sdata = bytes(sdata)
        # A slot that never changes cannot be speech; skip it cheaply so the
        # ranking is not cluttered by the seven idle slots.
        # Raw DMA words are the two 16-bit halves of each 32-bit group swapped,
        # so logical TDM slot = raw ^ 1 [CR A-0035]. Label both: raw is what
        # the capture holds, logical is what the channel tables talk about
        # (dect1 = logical 1 = raw 0, low byte).
        label = 'raw%d/log%d' % (slot, slot ^ 1)
        if len(set(sdata)) <= 2:
            print('%s: constant (%s) -- skipped'
                  % (label, ' '.join('%02x' % b for b in sorted(set(sdata)))))
            continue
        for name, rate, samples in readings(sdata):
            sc, detail = score(samples, rate)
            results.append((sc, '%s %s' % (label, name), rate, samples, detail))
    results.sort(key=lambda r: -r[0])

    print()
    print('%-26s %7s  %7s  %9s  %8s  %6s' %
          ('reading', 'score', 'corr', 'dynamics', 'zcr/s', 'rms'))
    for sc, name, rate, _, d in results[:12]:
        if not d:
            print('%-26s %7s' % (name, 'n/a'))
            continue
        print('%-26s %7.3f  %7.3f  %9.3f  %8.0f  %6.4f' %
              (name, sc, d['corr'], d['dynamics'], d['zcr'], d['rms']))

    base = os.path.splitext(os.path.basename(args.capture))[0]
    print()
    for sc, name, rate, samples, d in results[:3]:
        if not d:
            continue
        safe = name.replace(' ', '.').replace('@', '-')
        path = os.path.join(outdir, '%s.%s.wav' % (base, safe))
        write_wav(path, samples, rate)
        print('wrote %s' % path)
    print('\nListen to the top-ranked file. Speech means the reading is right;'
          '\nhiss or a buzz at the wrong pitch means try the next one.')


if __name__ == '__main__':
    main()
