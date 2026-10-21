// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect-maildump: recover the STUB mail stream from a raw TDM capture.
 *
 * `dect-stubd -w FILE` records every TDM slot of every frame, and the
 * control slot is one of them, so a capture already contains the complete
 * chip -> host mail stream -- including the payload bytes the daemon's own
 * log line truncated. Re-framing the control slot offline therefore turns
 * every capture ever taken into a full mail log, retroactively and without
 * touching the radio.
 *
 * The framing and the decoder are libopendectcell's own (dect_rx_push/pull), so
 * this tool agrees with the daemon by construction rather than by a second
 * implementation of the same rules.
 *
 *   dect-maildump call.raw                 # every mail
 *   dect-maildump call.raw -p f625         # only the terminal statistics
 *   dect-maildump call.raw -c 8 -w 1       # slots per frame, control word
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dect_mail.h"

#define FRAME_HZ 8000u

static void usage(void)
{
	fprintf(stderr,
		"usage: dect-maildump CAPTURE [-c slots] [-w word] [-p prim] [-q]\n"
		"  -c N   16-bit words per frame in the capture (default 8)\n"
		"  -w N   which word carries the receive control slot (default 1)\n"
		"  -p HEX only print this primitive (e.g. -p f625)\n"
		"  -q     print one line per mail without the payload\n"
		"\n"
		"Times are milliseconds from the start of the capture, which is\n"
		"the daemon's own start, so they line up with its log.\n");
}

int main(int argc, char **argv)
{
	const char *path = NULL;
	unsigned int slots = 8u, word = 1u;
	long want_prim = -1;
	int quiet = 0, i;
	FILE *f;
	long size;
	unsigned char *data;
	size_t frame_bytes, frames, fi;
	struct dect_rx rx;
	unsigned long count = 0, shown = 0;

	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-') {
			if (path) { usage(); return 2; }
			path = argv[i];
		} else if (!strcmp(argv[i], "-q")) {
			quiet = 1;
		} else if (i + 1 >= argc) {
			usage(); return 2;
		} else if (!strcmp(argv[i], "-c")) {
			slots = (unsigned int)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-w")) {
			word = (unsigned int)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-p")) {
			want_prim = (long)strtoul(argv[++i], NULL, 16);
		} else {
			usage(); return 2;
		}
	}
	if (!path || !slots || word >= slots) { usage(); return 2; }

	f = fopen(path, "rb");
	if (!f) { perror(path); return 1; }
	if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET)) {
		perror(path); fclose(f); return 1;
	}
	data = malloc((size_t)size);
	if (!data) { fprintf(stderr, "out of memory\n"); fclose(f); return 1; }
	if (fread(data, 1, (size_t)size, f) != (size_t)size) {
		fprintf(stderr, "%s: short read\n", path);
		free(data); fclose(f); return 1;
	}
	fclose(f);

	frame_bytes = slots * 2u;
	frames = (size_t)size / frame_bytes;
	printf("# %s: %zu frames, %.1f s, %u slots, control word %u\n",
	       path, frames, (double)frames / FRAME_HZ, slots, word);

	dect_rx_init(&rx);
	for (fi = 0; fi < frames; fi++) {
		const unsigned char *fr = data + fi * frame_bytes;
		struct dect_mail m;
		unsigned char pair[2];

		pair[0] = fr[word * 2u];
		pair[1] = fr[word * 2u + 1u];
		dect_rx_push(&rx, pair, 2);
		while (dect_rx_pull(&rx, &m)) {
			unsigned int k;

			count++;
			if (want_prim >= 0 && m.prim != (uint16_t)want_prim)
				continue;
			shown++;
			printf("[%7.3f] src %02x dst %02x prim %04x len %u",
			       (double)fi / FRAME_HZ, m.src, m.dst, m.prim,
			       m.param_len);
			if (!quiet)
				for (k = 0; k < m.param_len; k++)
					printf(" %02x", m.param[k]);
			printf("\n");
		}
	}
	free(data);
	printf("# %lu mails recovered, %lu shown; "
	       "seq_gaps %u csum_errors %u framing_errors %u resyncs %u\n",
	       count, shown, rx.stats.seq_gaps, rx.stats.csum_errors,
	       rx.stats.framing_errors, rx.stats.resyncs);
	return 0;
}
