// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect-stubd, options and helper modes: the -a key=value list, usage(), profile loading, and
 * the helper modes (--ctl, --bridge-echo, --check-profile). Moved out of dect-stubd.c unchanged.
 */
#define _POSIX_C_SOURCE 200809L
#include "stubd_internal.h"

/*
 * The -a keys whose value is a word, a path or carries a ':', not a
 * number. Each needs its value: given bare, the parser used to read
 * through a NULL '=' (security audit 2026-10-29, F5).
 */
static const char *const audio_word_keys[] = {
	"seqstep", "play", "playpcm", "playraw", "led",
	"statefile", "profiles", "ctl", "linename", "remote", "sockgroup",
	"clip", "memdump", "latemail", "timesync", "codeclist", "infoie",
	"lane2", "lane3", "cnip", "rfpicheck",
};

static int audio_word_key(const char *key, size_t klen)
{
	size_t i;

	for (i = 0; i < sizeof(audio_word_keys) / sizeof(audio_word_keys[0]);
	     i++)
		if (strlen(audio_word_keys[i]) == klen &&
		    !memcmp(key, audio_word_keys[i], klen))
			return 1;
	return 0;
}

/* The value of an -a key=value pair as a string: eq is its '=', end its end. */
static char *audio_value(const char *eq, const char *end)
{
	size_t n = (size_t)(end - (eq + 1));
	char *v = malloc(n + 1);

	if (v) {
		memcpy(v, eq + 1, n);
		v[n] = '\0';
	}
	return v;
}

/*
 * Parse -a key[=value][,key[=value]...] into struct audio_cfg. Returns 0 on
 * success, -1 on a malformed spec.
 */
/*
 * An -a key we do not know is ignored, not fatal: a key an older release
 * took (a lab option since removed) may linger in the UCI options, and
 * must not keep the base off the air.
 */
static void audio_unknown(const char *key, size_t klen)
{
	fprintf(stderr, "dect-stubd: -a %.*s: unknown key, ignored\n",
		(int)klen, key);
}

int parse_audio(struct audio_cfg *a, const char *spec)
{
	while (*spec) {
		const char *key = spec, *eq;
		unsigned long v = 0;
		size_t klen;
		char *end;

		while (*spec && *spec != ',')
			spec++;
		eq = memchr(key, '=', (size_t)(spec - key));
		klen = eq ? (size_t)(eq - key) : (size_t)(spec - key);
		if (!eq && audio_word_key(key, klen))
			return -1;
		if (eq) {
			v = strtoul(eq + 1, &end, 0);
			/* These carry a ':', a path or a word, not a number. */
			if (end != spec && !audio_word_key(key, klen))
				return -1;
		}
		if (eq && klen == 8 && !memcmp(key, "slotmask", 8)) {
			a->slot_mask = (uint8_t)v;
			a->slot_mask_fixed = 1;
		}
		else if (!eq && klen == 9 && !memcmp(key, "noslotcfg", 9))
			a->no_slot_cfg = 1;
		else if (eq && klen == 6 && !memcmp(key, "attcfg", 6))
			a->att_cfg = (uint8_t)v;
		else if (eq && klen == 7 && !memcmp(key, "attflag", 7))
			a->att_flag = (uint8_t)v;
		else if (eq && klen == 5 && !memcmp(key, "ipuio", 5))
			a->ipuio = (uint32_t)v, a->ipuio_set = 1;
		else if (klen == 7 && !memcmp(key, "regopen", 7))
			a->reg_open = 1, a->reg_window_s = eq ? (unsigned)v : 0u;
		else if (klen == 9 && !memcmp(key, "regwindow", 9))
			a->reg_signal_s = (unsigned)v;
		else if (!eq && klen == 8 && !memcmp(key, "twocalls", 8))
			a->two_calls = 1;
		else if (!eq && klen == 7 && !memcmp(key, "locauth", 7))
			a->locauth = 1;
		else if (!eq && klen == 9 && !memcmp(key, "nolocauth", 9))
			a->locauth = 0;
		else if (!eq && klen == 9 && !memcmp(key, "authfirst", 9))
			a->auth_first = 1;
		else if (!eq && klen == 11 && !memcmp(key, "noauthfirst", 11))
			a->auth_first = 0;
		else if (!eq && klen == 5 && !memcmp(key, "noctl", 5))
			a->no_ctl = 1;
		else if (eq && klen == 5 && (!memcmp(key, "lane2", 5) ||
					     !memcmp(key, "lane3", 5))) {
			unsigned ch, w, k = key[4] == '2' ? 0u : 1u;

			/* channel 1..3 (A-0124: three voice channels) */
			if (sscanf(eq + 1, "%u:%u", &ch, &w) != 2 || !ch ||
			    ch > 3 || w > 63)
				return -1;
			a->lane_ch[k] = (uint8_t)ch;
			a->lane_word[k] = (uint8_t)w;
			a->lane_set[k] = 1;
		}
		else if (eq && klen == 7 && !memcmp(key, "subslot", 7)) {
			if (v < 1u || v > DECT_MM_MAX_SLOT)
				return -1;
			a->sub_slot = (unsigned)v;
		}
		else if (eq && klen == 9 && !memcmp(key, "codeclist", 9)) {
			const char *h = eq + 1;
			size_t n = 0;

			while (h < spec && n < sizeof(a->codeclist)) {
				char buf[3];
				char *he;

				if (spec - h < 2)
					return -1;
				buf[0] = h[0];
				buf[1] = h[1];
				buf[2] = 0;
				a->codeclist[n] = (uint8_t)strtoul(buf, &he, 16);
				if (*he)
					return -1;
				n++;
				h += 2;
			}
			if (h != spec || n < 4)
				return -1;	/* a list needs at least one codec */
			a->codeclist_len = n;
		}
		else if (eq && klen == 6 && !memcmp(key, "infoie", 6)) {
			const char *h = eq + 1;
			size_t n = 0;

			while (h < spec && n < sizeof(a->info_ie)) {
				char buf[3];
				char *he;

				if (spec - h < 2)
					return -1;
				buf[0] = h[0];
				buf[1] = h[1];
				buf[2] = 0;
				a->info_ie[n] = (uint8_t)strtoul(buf, &he, 16);
				if (*he)
					return -1;
				n++;
				h += 2;
			}
			if (h != spec || n < 2)
				return -1;	/* at least one element */
			a->info_ie_len = n;
		}
		else if (!eq && klen == 6 && !memcmp(key, "capall", 6))
			a->cap_all = 1;
		else if (!eq && klen == 8 && !memcmp(key, "loopback", 8))
			a->loopback = 1;
		else if (klen == 4 && !memcmp(key, "ring", 4))
			a->ring_ms = (unsigned)v;
		else if (klen == 10 && !memcmp(key, "ringattach", 10))
			a->ring_attach_ms = (unsigned)v;
		else if (klen == 8 && !memcmp(key, "basicsvc", 8))
			a->basic_svc = (uint8_t)v, a->basic_svc_set = 1;
		else if (!eq && klen == 10 && !memcmp(key, "narrowband", 10))
			a->narrowband = 1;
		else if (!eq && klen == 9 && !memcmp(key, "nooverlap", 9))
			a->no_overlap = 1;
		else if (klen == 8 && !memcmp(key, "dialwait", 8))
			a->dial_wait_ms = (unsigned)v;
		else if (klen == 7 && !memcmp(key, "alertms", 7))
			a->alert_ms = (unsigned)v;
		else if (!eq && klen == 9 && !memcmp(key, "bindfirst", 9))
			a->bind_first = 1;
		else if (!eq && klen == 3 && !memcmp(key, "nlr", 3))
			a->nlr = 1;
		else if (!eq && klen == 5 && !memcmp(key, "nonlr", 5))
			a->nlr = 0;
		else if (!eq && klen == 13 && !memcmp(key, "allcallstatus", 13))
			a->all_call_status = 1;
		else if (!eq && klen == 12 && !memcmp(key, "nocallstatus", 12))
			a->no_call_status = 1;
		else if (klen == 10 && !memcmp(key, "setuptries", 10)) {
			/*
			 * How long to keep re-offering a CC-SETUP the local CC
			 * entity refuses because the handset's own call is up.
			 *
			 * The default 8 x 2 s = 16 s is SHORTER than our own
			 * F<CC.03> setup timer (20 s), so the ring always gave
			 * up before the timer that would clear a stuck
			 * transaction could fire -- which is exactly how F4.36b
			 * came back void. The A540 also opens a CAT-iq list
			 * access call roughly every 17 s and holds it ~13 s,
			 * so a budget under ~20 s can miss every window.
			 */
			if (v < 1u || v > 200u)
				return -1;
			a->setup_tries = (unsigned)v;
		}
		else if (klen == 8 && !memcmp(key, "timesync", 8)) {
			if (!eq)
				return -1;
			if (!strncmp(eq + 1, "auto", 4))
				a->timesync = TIMESYNC_AUTO;
			else if (!strncmp(eq + 1, "force", 5))
				a->timesync = TIMESYNC_FORCE;
			else if (!strncmp(eq + 1, "off", 3))
				a->timesync = TIMESYNC_OFF;
			else
				return -1;
		}
		else if (klen == 10 && !memcmp(key, "timeupdate", 10)) {
			/* minutes, up to 30 days */
			if (!eq || v > 43200u)
				return -1;
			a->time_every_min = (unsigned)v;
		}
		else if (klen == 6 && !memcmp(key, "signal", 6)) {
			a->signal_value = (uint8_t)v;
			a->signal_on = 1;
			a->signal_set = 1;
		}
		else if (!eq && klen == 8 && !memcmp(key, "nosignal", 8))
			a->signal_on = 0;	/* control runs only, see run 18 */
		else if (!eq && klen == 8 && !memcmp(key, "playsync", 8))
			a->play_sync = 1;
		else if (klen == 9 && !memcmp(key, "memdumpms", 9))
			a->memdump_ms = (unsigned)v;
		else if (klen == 7 && !memcmp(key, "memdump", 7)) {
			unsigned long len = 32;

			if (a->n_memdump >= sizeof(a->memdump) /
					    sizeof(a->memdump[0])) {
				fprintf(stderr, "too many memdump= regions "
					"(max %zu)\n",
					sizeof(a->memdump) /
					sizeof(a->memdump[0]));
				return -1;
			}
			if (!eq)
				return -1;
			if (*end == ':')
				len = strtoul(end + 1, &end, 0);
			if (end != spec || !len || len > 0x10000u)
				return -1;
			a->memdump[a->n_memdump].addr = (uint32_t)v;
			a->memdump[a->n_memdump].len = (uint32_t)len;
			a->n_memdump++;
		}
		else if (klen == 6 && !memcmp(key, "lineid", 6)) {
			if (v >= DECT_CALLINFO_LINE_NONE)
				return -1;
			a->callinfo.has_line = 1;
			a->callinfo.line_id = (uint8_t)v;
		}
		else if (klen == 8 && !memcmp(key, "linetype", 8)) {
			if (v > 3u)
				return -1;
			a->callinfo.has_line_type = 1;
			a->callinfo.line_type = (uint8_t)v;
		}
		else if (klen == 6 && !memcmp(key, "callid", 6)) {
			if (v > 0x7Fu)
				return -1;
			a->callinfo.has_call_id = 1;
			a->callinfo.call_id = (uint8_t)v;
		}
		else if (klen == 10 && !memcmp(key, "callstatus", 10)) {
			if (v > 0x7Fu)
				return -1;
			a->callinfo.has_call_status = 1;
			a->callinfo.call_status = (uint8_t)v;
		}
		else if ((klen == 4 && !memcmp(key, "clip", 4))
			 || (klen == 4 && !memcmp(key, "cnip", 4))) {
			/* text, not a number: take the value verbatim */
			char *text = audio_value(eq, spec);

			if (!text)
				return -1;
			if (key[1] == 'l')
				a->clip_number = text;
			else
				a->clip_name = text;
		}
		else if ((klen == 4 && !memcmp(key, "play", 4))
			 || (klen == 7 && !memcmp(key, "playpcm", 7))
			 || (klen == 7 && !memcmp(key, "playraw", 7))) {
			/* value is a path, not a number: take it verbatim */
			char *path = audio_value(eq, spec);

			if (!path)
				return -1;
			a->play_path = path;
			a->play_pcm = (klen == 7 && key[4] == 'p');
			a->play_raw = (klen == 7 && key[4] == 'r');
		}
		else if (klen == 8 && !memcmp(key, "toneword", 8)) {
			a->lane0_word = (uint8_t)v;
			a->lane0_word_set = 1;
		}
		else if (klen == 6 && !memcmp(key, "remode", 6)) {
			a->remode_ms = (unsigned)v;
			a->remode_set = 1;
		} else if (eq && klen == 8 && !memcmp(key, "linename", 8)) {
			char *name = audio_value(eq, spec);

			if (!name)
				return -1;
			a->line_name = name;
		} else if (eq && klen == 9 && !memcmp(key, "rfpicheck", 9)) {
			unsigned k, b;

			if (spec - (eq + 1) != 10)
				return -1;
			for (k = 0; k < 5u; k++) {
				if (sscanf(eq + 1 + 2u * k, "%2x", &b) != 1)
					return -1;
				a->rfpi_check[k] = (uint8_t)b;
			}
			a->rfpi_check_set = 1;
		} else if (eq && klen == 3 && !memcmp(key, "ctl", 3)) {
			char *path = audio_value(eq, spec);

			if (!path)
				return -1;
			a->ctl_path = path;
		} else if (eq && klen == 8 && !memcmp(key, "profiles", 8)) {
			char *path = audio_value(eq, spec);

			if (!path)
				return -1;
			a->profile_dir = path;
		} else if (eq && klen == 9 && !memcmp(key, "sockgroup", 9)) {
			char *name = audio_value(eq, spec);

			if (!name)
				return -1;
			a->sock_group = name;
		} else if (eq && klen == 9 && !memcmp(key, "statefile", 9)) {
			char *path = audio_value(eq, spec);

			if (!path)
				return -1;
			a->state_path = path;
		} else if (eq && klen == 6 && !memcmp(key, "remote", 6)) {
			char *path = audio_value(eq, spec);

			if (!path)
				return -1;
			a->remote_path = path;
		} else if (klen == 10 && !memcmp(key, "fetchdelay", 10)) {
			a->fetch_delay_ms = (unsigned)v;
		} else if (klen == 8 && !memcmp(key, "latemail", 8)) {
			const char *rest = end + 1;
			size_t rl = (size_t)(spec - rest);
			struct dect_mail chk;

			if (*end != ':' || !rl || a->n_latemail >=
			    sizeof(a->latemail) / sizeof(a->latemail[0]) ||
			    rl >= sizeof(a->latemail[0].spec))
				return -1;
			memcpy(a->latemail[a->n_latemail].spec, rest, rl);
			a->latemail[a->n_latemail].spec[rl] = 0;
			if (parse_mail_spec(a->latemail[a->n_latemail].spec,
					    &chk))
				return -1;
			a->latemail[a->n_latemail].ms = (unsigned)v;
			a->n_latemail++;
		} else if (klen == 12 && !memcmp(key, "installdummy", 12)) {
			if (v < 1 || v > DECT_MM_MAX_SLOT)
				return -1;
			a->install_dummy = (unsigned)v;
		} else if (klen == 10 && !memcmp(key, "tpuioffset", 10)) {
			if (v > 200)
				return -1;
			a->tpui_offset = (unsigned)v;
		} else if (klen == 10 && !memcmp(key, "keycheckms", 10)) {
			a->keycheck_ms = (unsigned)v;
			a->keycheck_set = 1;
		} else if (klen == 8 && !memcmp(key, "locvalid", 8)) {
			if (v > 255)
				return -1;
			a->locate_validity = (unsigned)v;
		} else if (!eq && klen == 10 && !memcmp(key, "nochipkeys", 10)) {
			a->no_chip_keys = 1;
		} else if (klen == 8 && !memcmp(key, "holdback", 8)) {
			a->holdback_ms = (unsigned)v;
		} else if (!eq && klen == 11 && !memcmp(key, "nostatefile", 11)) {
			a->no_state = 1;
		} else if (eq && klen == 3 && !memcmp(key, "led", 3)) {
			char *path = audio_value(eq, spec);

			if (!path)
				return -1;
			a->led_path = path;
		} else if (!eq && klen == 5 && !memcmp(key, "noled", 5)) {
			a->no_led = 1;
		} else if (!eq && klen == 8 && !memcmp(key, "noremode", 8)) {
			a->remode_ms = 0;
			a->remode_set = 1;
		}
		else if (klen == 3 && !memcmp(key, "seq", 3))
			a->seq_ms = (unsigned)v;
		else if (klen == 7 && !memcmp(key, "seqstep", 7)) {
			if (a->n_seq >= sizeof(a->seq_step) /
					sizeof(a->seq_step[0]))
				return -1;
			a->seq_step[a->n_seq++] = eq + 1;
		}
		else if (!eq)
			audio_unknown(key, klen);	/* or its value missing */
		else if (klen == 3 && !memcmp(key, "sel", 3)) {
			if (v < 1u || v > 6u)	/* [CR A-CodecModes-3] */
				return -1;
			a->sel = (uint8_t)v;
		}
		else if (klen == 4 && !memcmp(key, "gain", 4))
			a->gain = (uint8_t)v;
		else if (klen == 5 && !memcmp(key, "flags", 5))
			a->flags = (uint8_t)v;
		else if (klen == 4 && !memcmp(key, "mask", 4))
			a->mask = (uint16_t)v;
		else if (klen == 4 && !memcmp(key, "mode", 4))
			a->mode = (uint8_t)v;
		else if (klen == 5 && !memcmp(key, "codec", 5))
			a->codec = (uint8_t)v;
		else if (klen == 6 && !memcmp(key, "txidle", 6))
			a->tx_idle = (uint8_t)v, a->have_tx_idle = 1;
		else if (klen == 9 && !memcmp(key, "voiceidle", 9))
			a->tx_voice_idle = (uint8_t)v, a->have_voice_idle = 1;
		else if (klen == 6 && !memcmp(key, "status", 6))
			a->status_ms = (unsigned)v;
		else
			audio_unknown(key, klen);
		if (*spec == ',')
			spec++;
	}
	return 0;
}

/* Parse -k off. Returns 0 on success, -1 on a malformed spec. */
int parse_chip_cipher(struct chip_cipher *cc, const char *spec)
{
	if (!strcmp(spec, "off"))
		cc->disabled = 1;
	else
		return -1;
	return 0;
}


void usage(void)
{
	fprintf(stderr,
		"usage: dect-stubd [options]\n"
		"  -d DEV   device (default /dev/dect0)\n"
		"  -c N     slots per frame (default 8)\n"
		"  -T SLOT  transmit control slot (skip probing)\n"
		"  -R SLOT  receive control slot (skip probing)\n"
		"  -t SECS  run for SECS seconds then exit (default: until signal)\n"
		"  -v       log every mail (prim and length; parameters hidden)\n"
		"  -p       also log parameter bytes and NWK messages in full (identity\n"
		"           data, challenges, phonebook: do\n"
		"           not share such logs) -- implies -v\n"
		"  -M SRC:DST:PRIM[:HEX]  after contact, send this mail (repeatable,\n"
		"           one per ~100 ms; e.g. the MAC bring-up sequence)\n"
		"  -b FILE  offer the handsets a Contact List (CAT-iq phonebook) from\n"
		"           FILE, re-read when it changes; what handsets change\n"
		"           is appended to FILE.changes (see dect_contacts.h)\n"
		"  -K       cell mode: a cell of an openDECTcell controller. No stack and\n"
		"           no handsets here: every chip mail goes to dect-cell on\n"
		"           " CELL_PATH ", the radio stays parked\n"
		"           until the controller turns it on (not with -g)\n"
		"  -g       base mode: bring up the radio (registration closed\n"
		"           unless -a regopen) and run the host C-plane stack for\n"
		"           the handsets (parks the radio on exit)\n"
		"  -P PIN   registration PIN / AC, 4 to 8 digits (default 0000;\n"
		"           used with -g); -P @FILE reads it from a file only\n"
		"           root may write\n"
		"  -s PATH  subscription store, holds the handset key; reloaded into\n"
		"           the chip at start-up (default /etc/opendectcell-sub, \"\" = off).\n"
		"           Repeat for every handset the base holds: all are served\n"
		"           and installed in their chip slots; -a ring= rings the\n"
		"           first, a bridge call-in the one it names\n"
		"  -k off   the call key for the chip (0x0208, stock's layout,\n"
		"           [CR A-0126]) goes out before every CIPHER-REQUEST; -k off\n"
		"           withholds it\n"
		"  -w FILE  capture the call's voice slot to FILE (raw, 2 bytes per\n"
		"           frame, while a call is bound) -- holds call audio, keep\n"
		"           it local\n"
		"  -V SLOT  capture only this slot (default: every slot, so the\n"
		"           voice can be found offline)\n"
		"  -C       do not start call ciphering. The handset ciphers the whole\n"
		"           bearer once it sees CIPHER-REQUEST, so captured voice is\n"
		"           ciphertext; -C keeps the call in the clear for audio work\n"
		"           (the handset then drops it as unencrypted)\n"
		"  -a SPEC  audio bind parameters, comma separated [CR A-0029]:\n"
		"           sel=1..6 gain=N (0x0251), flags=N mask=N mode=N codec=N\n"
		"           = 0x0230 {link, mode, mask, master, extra}: flags is the\n"
		"           mode byte (low nibble = channel, 0x40 = wideband class;\n"
		"           0x41 works, 0x40 alone binds nothing), mode is master and\n"
		"           codec is extra\n"
		"           timesync=auto|force|off  send <<TIME-DATE>> after a\n"
		"                    location registration. auto (default) only when\n"
		"                    the kernel says the clock is disciplined;\n"
		"                    force trusts it anyway; off never sends\n"
		"           timeupdate=MIN  also send it to every handset every\n"
		"                    MIN minutes, paging the ones without a\n"
		"                    link (0, the default: only after a location\n"
		"                    registration)\n"
		"           toneword=N the TDM word of the main call's speech\n"
		"           (default 0; on the SSC transport the lane after the\n"
		"           control lane)\n"
		"           txidle=N voiceidle=N set the transmit filler bytes\n"
		"           slotmask=N pins the 0x017c byte, the locked-in handset\n"
		"                     mask (default: computed from the slots with\n"
		"                     a record, [CR A-0129]); noslotcfg omits it\n"
		"           attcfg=N  0x020A config byte after the UAK (stock 0x01\n"
		"                     per Gigaset, 0x02 C4; default 0x00) [CR A-0113]\n"
		"           attflag=N 0x0247 install flag byte (stock bit0=1,\n"
		"                     A540 0x03; default 0x00) [CR A-0113]\n"
		"           sockgroup=NAME the control and audio sockets belong to\n"
		"                     this group, mode 0660 (default: root, 0600)\n"
		"           ipuio=N   assign a registering handset IPUI O number N\n"
		"                     (up to 32 bits) instead of its IPUI N (B4a)\n"
		"           rfpicheck=HEX10 compare the RFPI the chip reports in\n"
		"                     0xF601 with these 5 bytes and log only the\n"
		"                     verdict (B0)\n"
		"           profiles=DIR  where the handset profiles are read from\n"
		"                     (default /etc/opendectcell/profiles)\n"
		"           ring=MS calls the handset MS ms after start-up: page\n"
		"           (0x0300) then an FP-originated CC-SETUP\n"
		"           ringattach=MS the same, but MS ms after the handset's\n"
		"                     FIRST C-plane contact (unattended runs; a lost\n"
		"                     A540 re-locks anywhere from 2 s to minutes later)\n"
		"           play=FILE transmits a raw G.722 stream (8000 bytes/s) to\n"
		"           the handset while a call is up, looping -- a clean\n"
		"           downlink source, unlike the loopback\n"
		"           playpcm=FILE does the same from 16 kHz signed 16-bit\n"
		"           little-endian PCM, encoding it to G.722 at start-up\n"
		"           signal=N <<SIGNAL>> N in the SETUP (0x40..0x47 =\n"
		"           alerting pattern 0..7; default 0x41, external call)\n"
		"           nosignal     omit it: the A540 then rings silently\n"
		"           setuptries=N re-offers a refused CC-SETUP N times\n"
		"           codeclist=HEX <<CODEC-LIST>> value octets for CC-SETUP\n"
		"           (default none: basic speech means G.726)\n"
		"           infoie=HEX   encoded IEs sent once in a CC-INFO when the\n"
		"           handset rings on our call (e.g. <<MULTI-DISPLAY>>)\n"
		"           subslot=N    chip subscription slot 1..6 (TPUI + UAK slot)\n"
		"           locauth      authenticate a handset with its UAK inside its\n"
		"                        location registration (the default;\n"
		"                        nolocauth switches it off)\n"
		"           authfirst    authenticate a handset with its UAK before\n"
		"                        its call goes anywhere or an answer is\n"
		"                        passed on, and end the call on any failure\n"
		"                        (the default; noauthfirst: the old order)\n"
		"           linename=NAME the line's name in the Line Settings\n"
		"                        List (\"Line 1\"; no commas)\n"
		"           ctl=PATH     the control socket, default\n"
		"                        " DECT_CTL_PATH " (0600; noctl: none)\n"
		"           twocalls     up to three calls at once: a handset may call\n"
		"           or be called while another is in a call. Side links get\n"
		"           their own audio channel (2 -> TDM word 3, 3 -> word 2;\n"
		"           lane2=CH:WORD, lane3= to override) and their speech goes\n"
		"           through the bridge as the main call's does\n"
		"           regopen[=SECS] open registration at start-up (enrol a\n"
		"           handset; closed after SECS, default never). Without it\n"
		"           registration is closed and subscribed handsets attach\n"
		"           regwindow=SECS how long a window opened by SIGUSR1\n"
		"           (opendectcell.init register) stays open (default 120)\n"
		"           basicsvc=N sets <<BASIC-SERVICE>> (default 0x88: high\n"
		"           nibble call class, low nibble 8 = wideband speech;\n"
		"           0x80 with narrowband)\n"
		"           nocallstatus no <<CALL-INFORMATION>> call status in our\n"
		"           SETUP-ACK/CALL-PROC/ALERTING/CONNECT to a handset's call\n"
		"           nonlr        do not release the link after its last call or\n"
		"           list session (default: release, 0x0202, a DISC first\n"
		"           after list access, as stock)\n"
		"           allcallstatus call status to every handset, not only to\n"
		"           those reporting NG-DECT Part 3 (the default since API 34)\n"
		"           alertms=N    ALERTING after CALL-PROC, CONNECT N ms later\n"
		"           bindfirst    bind the audio after CALL-PROC, CONNECT once\n"
		"           it is up (EN 300 175-5 9.3.1.8)\n"
		"           nooverlap    CONNECT a handset's call at once instead of\n"
		"           SETUP-ACK, digits, then CONNECT dialwait=MS (3000)\n"
		"           after the last digit\n"
		"           narrowband   never wideband: SETUP basic speech, CONNECT\n"
		"           chooses G.726, every call gets the mode-2 bind. By\n"
		"           default a call whose codec is G.722 gets the wideband\n"
		"           bind (flags 0x41, codec byte 0) and a phase-locked\n"
		"           G.722 downlink (play=/playpcm=); playraw= lanes are\n"
		"           format-specific, so use narrowband with linear ones\n"
		"           memdump=ADDR[:LEN] sweeps chip memory with the\n"
		"           read-address debug mail, logging each reply; repeat\n"
		"           the key for several ranges, memdumpms=N sets the\n"
		"           interval (default 50). Polling starts at link-up, so\n"
		"           one run gives an idle baseline and the live state.\n"
		"           Read only; never writes chip memory.\n"
		"           playsync locks play= to the received lane's word boundary\n"
		"           in a narrowband call too (a wideband call always locks)\n"
		"           statefile=PATH writes the base's state as JSON, at most\n"
		"           once a second (default " STATE_DEFAULT_PATH ");\n"
		"           nostatefile turns it off\n"
		"           led=PATH drives this LED brightness file (default\n"
		"           " LED_DEFAULT_PATH ", the 7520's\n"
		"           Fon/DECT LED): on while the radio is up, ~5 Hz blink on\n"
		"           C-plane traffic or a call, off when parked; noled leaves\n"
		"           it alone\n"
		"           remode=MS sends a second 0x0230 with flag bit 6 clear\n"
		"           MS ms after 0x022D, asking for mode 2 (the narrowband\n"
		"           G.726 path, which takes signed 16-bit linear PCM at one\n"
		"           sample per frame on the lane). Default: 500 if flags has\n"
		"           bit 6 set, else none -- with bit 6 clear the chip\n"
		"           reaches mode 2 from the first assign, and a re-assign\n"
		"           only adds a burst of noise. Less than 500 races the\n"
		"           chip's own pass and is overwritten\n"
		"           noremode never sends the re-assign\n"
		"           seq=MS with repeated seqstep=FLAGS:CODEC walks a list of\n"
		"           0x0230 configurations, MS ms per window, starting when\n"
		"           the call binds -- the same instant playraw restarts the\n"
		"           lane file, so a file built with matching windows keeps\n"
		"           format and mode aligned. Window 0 is left as the bind\n"
		"           left it. Pair with memdump of the route words so every\n"
		"           window carries proof of the state it was really in.\n"
		"           playraw=FILE replays a verbatim TDM lane instead (two\n"
		"           bytes per frame, 16000 B/s, as -w captures it): no\n"
		"           cadence logic, nothing interpreted\n"
		"           clip=NUMBER cnip=NAME put a calling-party number\n"
		"           (0x6C, printable IA5) and name (0x6D, UTF-8) in the\n"
		"           FP-originated CC-SETUP, both presented as allowed and\n"
		"           user-provided-not-screened. Omitted options send no IE.\n"
		"           lineid=N (0..126) linetype=N (bit 0 VoIP delay, bit 1\n"
		"           in-band second calls) callid=N callstatus=N add a\n"
		"           <<CALL-INFORMATION>> (0x7E) to the FP-originated\n"
		"           CC-SETUP; CAT-iq 2.0 marks an external call by its line\n"
		"           id. Our one SIP line is lineid=1,linetype=1, the\n"
		"           line the line list names.\n"
		"           loopback echoes the received bearer word (toneword)\n"
		"           straight back to the handset, byte for byte -- the\n"
		"           lane carries coded G.722, so this needs no encoder\n"
		"           capall makes -w capture from start-up, not only while a\n"
		"           call is bound (for radio-off TDM probing)\n"
		"           status=MS polls the chip's per-link connection status\n"
		"           (0xF624, read-only) every MS ms while a call is bound and\n"
		"           prints its slot, carrier and signal levels\n"
		"           holdback=MS nochipkeys remote=PATH fetchdelay=MS\n"
		"           locvalid=N: the multi-cell tests M2 and M4\n"
		"           (docs/journals/multicell-experiments.md)\n"
		"           latemail=MS:SRC:DST:PRIM[:HEX] send this raw mail MS ms\n"
		"           after the subscriptions were reloaded (repeatable, up\n"
		"           to 8, in list order); -M sends before the radio is on\n"
		"           installdummy=N send 0x0247 {N, 1, 0} after the slot\n"
		"                     clears, no key written (bench test, A-0134)\n"
		"           tpuioffset=N assign TPUI {0x00,0x11,slot+N} instead of the\n"
		"           slot's own (a TPUI that is not slot-derived, docs/more-than-six.md T-C)\n"
		"           keycheckms=MS how long the UAK read-back and slot audit\n"
		"           wait after the writes (default 0; 1000 on the 7412's\n"
		"           06.88): the key-write delay sweep of docs/more-than-six.md\n"
		"           Default: sel=1,gain=0x2b,flags=1,mask=0,mode=0,codec=0\n"
		"           (flags=0x41 is the wideband bind used at G7),\n"
		"           txidle=0xff,voiceidle=0xff\n");
}

/* ---- handset profiles (docs/handset-profiles.md) ----------------------- */

static void profile_say(void *ctx, unsigned line, const char *msg)
{
	const char *path = ctx;

	if (line)
		printf("profile: %s:%u: %s\n", path, line, msg);
	else
		printf("profile: %s: %s\n", path, msg);
}

/* Read and parse one file; 0 on success. */
int profile_load(const char *path, struct dect_profile *p)
{
	static char text[16384];
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f) {
		printf("profile: %s: %s\n", path, strerror(errno));
		return -1;
	}
	n = fread(text, 1, sizeof(text), f);
	fclose(f);
	if (n == sizeof(text)) {
		printf("profile: %s: larger than %zu octets\n", path,
		       sizeof(text));
		return -1;
	}
	return dect_profile_parse(text, n, p, profile_say, (void *)path);
}

/*
 * dect-stubd --ctl LINE...: a client for the control socket, for scripts
 * and the bench (the box has no socat). Sends each LINE, prints what the
 * daemon writes back for a second after the last one, then exits.
 * OPENDECTCELL_CTL names another socket path.
 */
int ctl_client(int n, char **lines)
{
	const char *path = getenv("OPENDECTCELL_CTL");
	struct sockaddr_un a;
	char buf[DECT_CTL_LINE_MAX];
	uint64_t until;
	int fd, i;

	if (!path)
		path = DECT_CTL_PATH;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(a.sun_path))
		return 2;
	strcpy(a.sun_path, path);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a))) {
		fprintf(stderr, "ctl: %s: %s\n", path, strerror(errno));
		if (fd >= 0)
			close(fd);
		return 1;
	}
	for (i = 0; i < n; i++) {
		size_t len = strlen(lines[i]);

		if (len + 1 > sizeof(buf)) {
			fprintf(stderr, "ctl: line %d too long\n", i + 1);
			close(fd);
			return 2;
		}
		/* one argument, one line: an embedded break would add a
		 * command (security audit 2026-10-29, F5) */
		if (strpbrk(lines[i], "\r\n")) {
			fprintf(stderr, "ctl: line %d holds a line break\n",
				i + 1);
			close(fd);
			return 2;
		}
		memcpy(buf, lines[i], len);
		buf[len++] = '\n';
		if (write(fd, buf, len) != (ssize_t)len) {
			fprintf(stderr, "ctl: write: %s\n", strerror(errno));
			close(fd);
			return 1;
		}
	}
	until = mono_ms() + 1000u;
	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		uint64_t now = mono_ms();
		ssize_t r;

		if (now >= until || poll(&pfd, 1, (int)(until - now)) <= 0)
			break;
		r = read(fd, buf, sizeof(buf));
		if (r <= 0)
			break;
		fwrite(buf, 1, (size_t)r, stdout);
	}
	close(fd);
	return 0;
}

int bridge_echo(int argc, char **argv)
{
	const char *path = getenv("OPENDECTCELL_CTL");
	struct sockaddr_un a;
	char in[DECT_CTL_LINE_MAX * 2], line[DECT_CTL_LINE_MAX];
	static int16_t ring[ECHO_DELAY][AB_FRAME / 2u];
	unsigned rw = 0, rn = 0, answer_call = 0;
	/*
	 * DECT_ECHO_TONE=1: instead of the echo, a group of 1 kHz pulses every 2 s from the moment the audio
	 * channel opens, silence between: 1 pulse (80 ms on, 80 ms off) in the first group, 2 in the second,
	 * up to 10, then again. It tells a handset whose speaker opens late from one whose microphone is
	 * silent (the O201, 2026-11-05): the listener says how many pulses the first group they heard had,
	 * which is the time since the channel opened in units of 2 s.
	 */
	int tone = getenv("DECT_ECHO_TONE") != NULL;
	unsigned tick = 0;
	static const int16_t sine16[16] = { 0, 3827, 7071, 9239, 10000, 9239, 7071, 3827,
					    0, -3827, -7071, -9239, -10000, -9239, -7071, -3827 };
	uint64_t answer_at = 0;
	size_t il = 0;
	int fd, afd = -1, n;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	if (strlen(path ? path : DECT_CTL_PATH) >= sizeof(a.sun_path))
		return 2;
	strcpy(a.sun_path, path ? path : DECT_CTL_PATH);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a))) {
		fprintf(stderr, "echo: %s: %s\n", a.sun_path, strerror(errno));
		return 1;
	}
	n = snprintf(line, sizeof(line), "bridge\n");
	if (argc >= 1 && !strcmp(argv[0], "ring"))
		n += snprintf(line + n, sizeof(line) - (size_t)n,
			      "call-in line=1 number=%s name=\"Echo test\" "
			      "sip=echo%s%s\n", argc >= 2 ? argv[1] : "0301234567",
			      argc >= 3 ? " hs=" : "", argc >= 3 ? argv[2] : "");
	if (write(fd, line, (size_t)n) != n)
		return 1;
	printf("echo: bridge up -- dial any number on a handset\n");
	fflush(stdout);
	while (!stop) {
		struct pollfd p[2] = { { .fd = fd, .events = POLLIN },
				       { .fd = afd, .events = POLLIN } };
		uint64_t now = mono_ms();

		if (answer_call && now >= answer_at) {
			n = snprintf(line, sizeof(line),
				     "remote-answered call=%u\n", answer_call);
			if (write(fd, line, (size_t)n) != n)
				break;
			printf("echo: call %u answered\n", answer_call);
			answer_call = 0;
		}
		if (poll(p, afd >= 0 ? 2 : 1, 100) < 0 && errno != EINTR)
			break;
		if (afd >= 0 && (p[1].revents & (POLLIN | POLLHUP))) {
			ssize_t r = recv(afd, ring[rw], sizeof(ring[0]),
					 MSG_DONTWAIT);

			if (r <= 0) {
				close(afd);
				afd = -1;
				printf("echo: audio closed\n");
			} else {
				rw = (rw + 1u) % ECHO_DELAY;
				if (rn < ECHO_DELAY)
					rn++;
				if (tone) {
					static int16_t beep[AB_FRAME / 2u];
					size_t i, ns = (size_t)r / sizeof(int16_t);

					if (ns > AB_FRAME / 2u)
						ns = AB_FRAME / 2u;
					for (i = 0; i < ns; i++)
						beep[i] = (tick % 100u) < 8u * (tick / 100u % 10u + 1u) && (tick % 8u) < 4u ? sine16[i % 16u] : 0;
					if (tick % 100u == 0)
						printf("echo: group of %u pulses at audio frame %u (%u ms)\n", tick / 100u % 10u + 1u, tick, tick * 20u);
					tick++;
					(void)send(afd, beep, ns * sizeof(int16_t), MSG_DONTWAIT | MSG_NOSIGNAL);
				} else if (rn == ECHO_DELAY) {
					/* the frame from half a second ago */
					(void)send(afd, ring[rw], (size_t)r,
						   MSG_DONTWAIT | MSG_NOSIGNAL);
				}
			}
		}
		if (!(p[0].revents & (POLLIN | POLLHUP)))
			continue;
		{
			ssize_t r = read(fd, in + il, sizeof(in) - il - 1u);
			char *nl;

			if (r <= 0)
				break;
			il += (size_t)r;
			while ((nl = memchr(in, '\n', il)) != NULL) {
				struct dect_ctl_msg m;
				size_t l = (size_t)(nl - in);
				unsigned id;

				printf("echo: << %.*s\n", (int)l, in);
				if (!dect_ctl_parse(in, l, &m) &&
				    !dect_ctl_get_uint(&m, "call", 1, 127, &id)) {
					if (!strcmp(m.verb, "call-out")) {
						n = snprintf(line, sizeof(line),
							"remote-ringing call=%u\n", id);
						if (write(fd, line, (size_t)n) != n)
							stop = 1;
						answer_call = id;
						answer_at = mono_ms() + 1500u;
					} else if (!strcmp(m.verb, "audio") &&
						   dect_ctl_get(&m, "path") &&
						   afd < 0) {
						struct sockaddr_un aa;

						memset(&aa, 0, sizeof(aa));
						aa.sun_family = AF_UNIX;
						snprintf(aa.sun_path,
							 sizeof(aa.sun_path), "%s",
							 dect_ctl_get(&m, "path"));
						afd = socket(AF_UNIX,
							     SOCK_SEQPACKET, 0);
						/* the connection names its call */
						n = snprintf(line, sizeof(line),
							     "call=%u", id);
						if (afd >= 0 &&
						    (connect(afd, (struct sockaddr *)&aa,
							     sizeof(aa)) ||
						     send(afd, line, (size_t)n,
							  MSG_NOSIGNAL) != n)) {
							close(afd);
							afd = -1;
						}
						rn = rw = 0;
						tick = 0;
						printf("echo: audio %s\n",
						       afd >= 0 ? "connected"
								: "FAILED");
					} else if (!strcmp(m.verb, "released") &&
						   afd >= 0) {
						close(afd);
						afd = -1;
					}
				}
				memmove(in, nl + 1, il - l - 1u);
				il -= l + 1u;
			}
			if (il == sizeof(in) - 1u)
				il = 0;
			fflush(stdout);
		}
	}
	if (afd >= 0)
		close(afd);
	close(fd);
	return 0;
}

/*
 * dect-stubd --check-profile FILE [FIRMWARE]: validate one profile without
 * the radio, and for a device profile its manufacturer layer, looked up
 * beside it as <extends>.profile; print what it resolves to.
 */
int check_profile(const char *path, const char *fw)
{
	static struct dect_profile p, mfr;
	struct dect_hs_profile hp;
	const struct dect_profile *m = NULL;
	char desc[1024], mpath[512];
	const char *slash;

	if (profile_load(path, &p)) {
		printf("profile: %s: rejected\n", path);
		return 1;
	}
	if (p.kind == DECT_PROFILE_DEVICE) {
		slash = strrchr(path, '/');
		snprintf(mpath, sizeof(mpath), "%.*s%s.profile",
			 slash ? (int)(slash - path + 1) : 0, path, p.extends);
		if (profile_load(mpath, &mfr) ||
		    mfr.kind != DECT_PROFILE_MANUFACTURER ||
		    strcmp(mfr.id, p.extends)) {
			printf("profile: %s: its manufacturer profile %s is "
			       "missing or bad\n", path, mpath);
			return 1;
		}
		m = &mfr;
	}
	if (p.kind == DECT_PROFILE_MANUFACTURER)
		dect_profile_resolve(&p, NULL, fw, &hp);
	else
		dect_profile_resolve(m, &p, fw, &hp);
	printf("profile: %s: %s \"%s\"%s%s%s, %u firmware section(s)\n",
	       path, p.id, p.name, m ? " (via " : "", m ? m->id : "",
	       m ? ")" : "", p.n_sec - 1u);
	if (dect_profile_describe(&hp, desc, sizeof(desc)))
		printf("profile: resolved%s%s: %s\n", fw ? " for firmware " : "",
		       fw ? fw : "", desc);
	else
		printf("profile: resolved: the standard defaults\n");
	return 0;
}
