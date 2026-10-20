#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Conformance cross-check for src/libopendectcell/dect_g722.c against ffmpeg's
# independent G.722 implementation. Not part of `make check`: it needs ffmpeg
# and takes a few seconds.
#
# What it establishes, and what it does not:
#   - Our decoder on ffmpeg's bitstream must reach the same SNR as ffmpeg's own
#     decoder. That is the interoperability direction that matters for us,
#     because the handset's decoder is the one we cannot change.
#   - ffmpeg's decoder on our bitstream must reach the same SNR too, which says
#     our encoder emits a valid stream.
#   - It does NOT establish bit-exactness with ITU-T G.722. The normative test
#     vectors ship with the Recommendation and we do not have them; if they
#     ever become available, run them and this script becomes redundant.
#
# Expect roughly 28-29 dB on the built-in signal, all four ways, and a sample
# delay of 22 (the QMF group delay).
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

command -v ffmpeg >/dev/null || { echo "ffmpeg not found"; exit 1; }

cat > "$tmp/cli.c" <<'CEOF'
#include "dect_g722.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
	static int16_t pcm[1 << 21];
	static uint8_t buf[1 << 21];
	struct dect_g722_state st;
	size_t n;

	dect_g722_init(&st);
	if (argc > 1 && !strcmp(argv[1], "enc")) {
		n = fread(pcm, 2, sizeof(pcm) / 2, stdin);
		n = dect_g722_encode(&st, pcm, n, buf);
		fwrite(buf, 1, n, stdout);
	} else {
		n = fread(buf, 1, sizeof(buf), stdin);
		n = dect_g722_decode(&st, buf, n, pcm);
		fwrite(pcm, 2, n, stdout);
	}
	return 0;
}
CEOF
${CC:-cc} -std=c11 -O2 -I "$root/src/libopendectcell" -o "$tmp/cli" "$tmp/cli.c" \
	"$root/src/libopendectcell/dect_g722.c"

# One second at 16 kHz: two voice-band tones, one above 4 kHz, one chirp
# through the crossover, with slow amplitude modulation.
python3 - "$tmp/in.raw" <<'PEOF'
import math, struct, sys
N = 16000
o = []
for n in range(N):
    t = n / 16000.0
    v = (0.35 * math.sin(2 * math.pi * 440 * t)
         + 0.25 * math.sin(2 * math.pi * 1270 * t)
         + 0.18 * math.sin(2 * math.pi * 3100 * t)
         + 0.12 * math.sin(2 * math.pi * (200 + 6000 * t) * t))
    v *= 0.6 + 0.4 * math.sin(2 * math.pi * 3 * t)
    o.append(max(-32768, min(32767, int(v * 20000))))
open(sys.argv[1], 'wb').write(struct.pack('<%dh' % N, *o))
PEOF

ff="ffmpeg -hide_banner -loglevel error"
$ff -f s16le -ar 16000 -ac 1 -i "$tmp/in.raw" -c:a g722 -f g722 "$tmp/ff.g722" -y
"$tmp/cli" enc < "$tmp/in.raw" > "$tmp/our.g722"

$ff -f g722 -i "$tmp/ff.g722"  -f s16le -ar 16000 -ac 1 "$tmp/ff_ff.raw"  -y
$ff -f g722 -i "$tmp/our.g722" -f s16le -ar 16000 -ac 1 "$tmp/ff_our.raw" -y
"$tmp/cli" dec < "$tmp/ff.g722"  > "$tmp/our_ff.raw"
"$tmp/cli" dec < "$tmp/our.g722" > "$tmp/our_our.raw"

python3 - "$tmp" <<'PEOF'
import struct, sys, math
d = sys.argv[1]
def rd(p):
    b = open('%s/%s' % (d, p), 'rb').read()
    return struct.unpack('<%dh' % (len(b) // 2), b)
ref = rd('in.raw')
def snr(y, lag):
    n = min(len(ref), len(y)) - lag
    num = sum(float(ref[i]) ** 2 for i in range(n))
    den = sum((float(ref[i]) - y[i + lag]) ** 2 for i in range(n))
    return 10 * math.log10(num / den) if den else float('inf')
rows = [('ffmpeg enc -> ffmpeg dec', 'ff_ff.raw'),
        ('ffmpeg enc -> our dec   ', 'our_ff.raw'),
        ('our enc    -> ffmpeg dec', 'ff_our.raw'),
        ('our enc    -> our dec   ', 'our_our.raw')]
res = {}
for name, p in rows:
    y = rd(p)
    s, lag = max((snr(y, l), l) for l in range(40))
    res[name] = s
    print('%s  SNR %6.2f dB  (delay %d samples)' % (name, s, lag))
base = res['ffmpeg enc -> ffmpeg dec']
worst = min(res.values())
print()
if worst < base - 0.5:
    print('FAIL: worst path is %.2f dB below ffmpeg round trip' % (base - worst))
    sys.exit(1)
if worst < 20.0:
    print('FAIL: SNR too low (%.2f dB)' % worst)
    sys.exit(1)
print('PASS: every path within %.2f dB of the ffmpeg round trip' % (base - worst))
PEOF
