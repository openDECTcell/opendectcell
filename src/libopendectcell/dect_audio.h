/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Audio helpers for the voice slots: G.711 A-law conversion and a simple
 * tone analysis. The DECT STUB carries 8-bit A-law samples in the voice
 * slots (observed: the sample is the low byte of the 16-bit slot word).
 *
 * Used to verify the STUB test tone received in a voice slot (gate G4) and
 * to generate a tone to transmit for the loopback test. Pure functions,
 * no I/O; unit-tested on the PC.
 */
#ifndef OPENDECTCELL_DECT_AUDIO_H
#define OPENDECTCELL_DECT_AUDIO_H

#include <stddef.h>
#include <stdint.h>

/* G.711 A-law <-> 16-bit linear PCM (ITU-T, Sun reference algorithm). */
int16_t dect_alaw_decode(uint8_t a);
uint8_t dect_alaw_encode(int16_t pcm);

/* Voice-slot sample rate on the PCM bus. */
#define DECT_AUDIO_RATE_HZ	8000u

struct dect_tone_stats {
	size_t   samples;
	double   mean;		/* DC offset (linear) */
	double   rms;		/* AC RMS after removing the mean */
	int16_t  peak;		/* max |sample - mean|, clamped to int16 */
	double   freq_hz;	/* dominant frequency from zero crossings */
	uint32_t crossings;	/* mean-crossings counted */
};

/*
 * Analyse decoded linear samples: DC offset, AC RMS, peak and a
 * zero-crossing frequency estimate at the given sample rate. Silence gives
 * rms ~ 0 and freq 0; a clean tone gives freq near its frequency.
 */
void dect_tone_analyze(const int16_t *pcm, size_t n, uint32_t rate,
		       struct dect_tone_stats *s);

/* Decode n A-law bytes into pcm[n] in place-friendly form (separate buffers). */
void dect_alaw_decode_buf(const uint8_t *alaw, int16_t *pcm, size_t n);

/*
 * 8 kHz <-> 16 kHz for a narrowband call's speech on the bridge's 16 kHz
 * audio socket: a 39-tap half-band low-pass (4 kHz; -0.4 dB at 3.4 kHz,
 * below -60 dB from 5 kHz), one state per direction and call.
 */
#define DECT_RS2_TAPS	39u

struct dect_rs2 {
	int16_t x[DECT_RS2_TAPS];	/* the last inputs at 16 kHz, newest last */
};

void dect_rs2_init(struct dect_rs2 *r);
/* n samples at 16 kHz (n even) to n / 2 at 8 kHz; returns n / 2. */
size_t dect_rs2_down(struct dect_rs2 *r, const int16_t *in, size_t n,
		     int16_t *out);
/* n samples at 8 kHz to 2 n at 16 kHz; returns 2 n. */
size_t dect_rs2_up(struct dect_rs2 *r, const int16_t *in, size_t n,
		   int16_t *out);

#endif /* OPENDECTCELL_DECT_AUDIO_H */
