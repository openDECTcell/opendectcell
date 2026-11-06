// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect-stubd, the link to the chip: which STUB build the chip runs, the mail log and the
 * verification table, the SSC realignment glue, sending mails, and the bring-up (control-slot
 * search, SSC measurement, the six PCM steps, tear-down). Moved out of dect-stubd.c unchanged.
 */
#define _POSIX_C_SOURCE 200809L
#include "stubd_internal.h"

/* fopen(path, "r"), if path is root's own and only root may write it. */
FILE *fopen_root_only(const char *path)
{
	struct stat st;
	FILE *f = fopen(path, "r");

	if (f && (fstat(fileno(f), &st) || st.st_uid != ROOT_UID ||
		  (st.st_mode & (S_IWGRP | S_IWOTH)) || !S_ISREG(st.st_mode))) {
		fprintf(stderr, "ignoring %s: not root's own file\n", path);
		fclose(f);
		f = NULL;
	}
	return f;
}

int stub_build_read(char *name, size_t cap)
{
	FILE *f = fopen_root_only(DECT_STUB_RECORD);
	size_t n;

	name[0] = '\0';
#ifdef DECT_STUB_RECORD_OLD
	if (!f)
		f = fopen_root_only(DECT_STUB_RECORD_OLD);
#endif
	if (!f)
		return STUB_UNKNOWN;
	if (!fgets(name, (int)cap, f))
		name[0] = '\0';
	fclose(f);
	n = strcspn(name, "\r\n");
	name[n] = '\0';
	if (!n)
		return STUB_UNKNOWN;
	return strcmp(name, DECT_STUB_0825) ? STUB_OTHER : STUB_0825;
}

/*
 * Whether parameter byte i of m is key material: the 16 UAK octets of a
 * 0x020A subscription record, or a byte of a 0xFFF1 memory-read reply
 * (sub 0x08: address in [1..4], data from [7]) that falls in the UAK table.
 */
static int mail_byte_is_key(const struct dect_mail *m, unsigned int i)
{
	uint32_t addr;

	if (m->prim == 0x020Au)
		return i >= 1u && i <= DECT_MM_UAK_LEN;
	if (m->prim == 0x0208u)		/* the call key */
		return i >= 1u && i <= DECT_MM_DCK_LEN;
	if (m->prim != 0xFFF1u || m->param_len < 7u || m->param[0] != 0x08u
	    || i < 7u)
		return 0;
	addr = (uint32_t)m->param[1] | ((uint32_t)m->param[2] << 8)
	     | ((uint32_t)m->param[3] << 16) | ((uint32_t)m->param[4] << 24);
	addr += i - 7u;
	return addr >= SUB_UAK_BASE && addr < SUB_UAK_END;
}

/*
 * Log a mail. Parameter bytes are hidden by default: the 0xF601/0xF602
 * answers carry box identity (RFPI-like) data that must not leak into
 * shared logs or issues. Pass -p to show them for local debugging only.
 * Key bytes print as ** even under -p: the UAK in a 0x020A record or in a
 * read-back of the table. Until 2026-10-24 -p printed both, in the clear,
 * in every run that loaded or registered a handset.
 */
void log_mail(const struct dect_mail *m, const char *dir, int show_params)
{
	unsigned int i;

	printf("[%5llu] %s src %02x dst %02x prim %04x len %u",
	       (unsigned long long)(mono_ms() - g_start), dir, m->src, m->dst,
	       m->prim, m->param_len);
	if (!show_params) {
		printf("%s\n", m->param_len ? " [params hidden]" : "");
		return;
	}
	/*
	 * Print the whole payload. The old 24-byte cut silently discarded the
	 * part of 0xF625 that matters most: its 28 per-terminal counters sit
	 * at offsets 16..43 [CR COMMAND_SET 8.1], so every run up to
	 * 2026-10-20 logged 8 of them and threw the other 20 away.
	 */
	for (i = 0; i < m->param_len && i < sizeof(m->param); i++) {
		if (mail_byte_is_key(m, i))
			printf(" **");
		else
			printf(" %02x", m->param[i]);
	}
	/*
	 * The build stamp is ASCII ("RAM: <date>"). It arrives once, as the
	 * first mail after the first contact following a chip boot, so a run
	 * that reuses an already-loaded STUB never sees it. Decoding it here
	 * makes "which chip build is actually loaded" readable in the log,
	 * which is the confirmation the 08.25/07.56 blob swap needs.
	 */
	if (m->prim == DECT_PRIM_BUILD_STAMP && m->param_len) {
		printf("  \"");
		for (i = 0; i < m->param_len && i < sizeof(m->param); i++)
			putchar((m->param[i] >= 0x20 && m->param[i] < 0x7f)
				? (char)m->param[i] : '.');
		printf("\"");
	}
	printf("\n");
}

/*
 * What we can and cannot know about a command after we send it.
 *
 * The maintainer asked whether we check that anything we tell the chip
 * actually took effect. We do not, almost anywhere, and two findings make
 * that worse than it sounds: `[CR A-0070]` says the dispatcher SILENTLY DROPS
 * anything that is not a command, so a wrong primitive gives no error and no
 * reply; and `[CR A-0075]` says 0x0242 is emitted on the no-act early-out, so
 * the one reply we had been treating as a confirmation never was one.
 *
 * `[CR A-0086]` then settled how much of it matters, and the answer is: less
 * than it looks, with one exception.
 *
 *   - **The chip has no NAK and no error indication of any kind**, so a
 *     read-back is the only check that can exist. There is nothing to listen
 *     for, and we were not missing a mechanism.
 *   - **Stock does not verify either**, and stock has run this chip in the
 *     field for years -- but it periodically **re-asserts** most subscription
 *     state, so a silently lost write self-heals. That is how it gets away
 *     with it, and it is a better design than checking.
 *
 *     We re-assert too: the stored subscriptions go back into the chip at
 *     start-up, and 0x017C, the locked-in handset mask, follows every
 *     registration and deregistration [CR A-0129]. The subscription table --
 *     the thing that has actually caused us trouble -- gets an audit as well
 *     as the UAK read-back.
 *   - **The UAK is the exception**: written once at enrolment, never
 *     re-asserted, and it is the credential a handset needs to attach. Stock
 *     has that hole too.
 *
 * So we check the UAK (dect_base_uak_verify) and deliberately do not build
 * general command verification. This table stays because it keeps the
 * assumption visible -- a quiet log should not read as a verified one.
 */
enum verify_class {
	VERIFY_QUERY,		/* a read or poll: the reply IS the result */
	VERIFY_READBACK,	/* a state change we verify by reading it back */
	VERIFY_READABLE,	/* a state change, readable, that we do not check */
	VERIFY_NONE,		/* a state change with nothing to read */
};

static const struct {
	uint16_t prim;
	enum verify_class cls;
	const char *what;
	const char *note;
} verify_tbl[] = {
	{ 0xFFF1u, VERIFY_QUERY,     "memory read",            "the reply is the result" },
	{ 0xF624u, VERIFY_QUERY,     "status poll",            "answered by 0xF625" },
	{ 0x020Au, VERIFY_READBACK,  "subscription record",    "read back at 0x9518 + (slot-1)*0x11 [A-0085]" },
	{ 0x0247u, VERIFY_READBACK,  "subscription slot op",   "slot occupancy audited at 0x9518 after every clear" },
	{ 0x0230u, VERIFY_READABLE,  "assign audio channel",   "0x0242 is 'processed', NOT 'applied' [A-0075]; real gate is record +0x116 bit 5" },
	{ 0x016Au, VERIFY_READABLE,  "channel config",         "matrix is readable; we do not check" },
	{ 0x0251u, VERIFY_READABLE,  "set channel params",     "matrix is readable; we do not check" },
	{ 0x022Du, VERIFY_READABLE,  "audio path on",          "record +0x116 bit 6; we do not check" },
	{ 0x0233u, VERIFY_READABLE,  "audio disconnect",       "record +0x116 bit 6; we do not check" },
	{ 0x0208u, VERIFY_NONE,      "cipher enable",          "field mapping unproven [A-0028]" },
	{ 0x0300u, VERIFY_NONE,      "page a handset",         "a page response is a NWK event, not a confirmation we paged" },
	{ 0xF626u, VERIFY_NONE,      "DSP route",              "no known readable effect" },
	{ 0x015Bu, VERIFY_NONE,      "radio on/park",          "no known readable effect" },
	{ 0x022Bu, VERIFY_NONE,      "enable token",           "no known readable effect" },
	{ 0x022Cu, VERIFY_NONE,      "close registration",     "no known readable effect" },
	{ 0xF600u, VERIFY_QUERY,     "contact",                "answered by 0xF601" },
	{ 0x0244u, VERIFY_NONE,      "transmit C-plane",       "DLC acks the frame; the chip never says it sent it" },
};

/*
 * The tally lives on struct stubd, which is declared before this table, so
 * its size is written out by hand. Catch a mismatch at compile time rather
 * than overflowing it -- it would be a poor joke to get this wrong in the
 * change whose whole point is checking things.
 */
_Static_assert(VERIFY_TBL_N <= sizeof(((struct stubd *)0)->verify_n)
			       / sizeof(((struct stubd *)0)->verify_n[0]),
	       "verify_n[] is smaller than verify_tbl[]");

static void verify_count(struct stubd *s, uint16_t prim)
{
	size_t i;

	for (i = 0; i < VERIFY_TBL_N; i++) {
		if (verify_tbl[i].prim == prim) {
			if (s->verify_n[i] < UINT32_MAX)
				s->verify_n[i]++;
			return;
		}
	}
	if (s->verify_other < UINT32_MAX)
		s->verify_other++;
}

/*
 * One block per run. Turns "we never check anything" from a fact buried in
 * the source into a line somebody reads after every bench run.
 */
void verify_report(const struct stubd *s)
{
	unsigned long reads = 0, changes = 0, verified = 0;
	size_t i;

	for (i = 0; i < VERIFY_TBL_N; i++) {
		switch (verify_tbl[i].cls) {
		case VERIFY_QUERY:
			reads += s->verify_n[i];
			break;
		case VERIFY_READBACK:
			changes += s->verify_n[i];
			if (s->stub_build == STUB_0825 || s->uak_0688)
				verified += s->verify_n[i];
			break;
		default:
			changes += s->verify_n[i];
			break;
		}
	}
	changes += s->verify_other;
	if (!reads && !changes)
		return;

	printf("chip: command verification summary\n");
	for (i = 0; i < VERIFY_TBL_N; i++) {
		if (!s->verify_n[i])
			continue;
		int ok = verify_tbl[i].cls == VERIFY_QUERY ||
			 (verify_tbl[i].cls == VERIFY_READBACK &&
			  (s->stub_build == STUB_0825 || s->uak_0688));

		printf("chip:   0x%04X x%-5u %-22s %s (%s)\n",
		       verify_tbl[i].prim, s->verify_n[i], verify_tbl[i].what,
		       ok ? "verified" : "NOT VERIFIED",
		       verify_tbl[i].cls == VERIFY_READBACK && !ok
			       ? "the read-back address is the 08.25 build's"
			       : verify_tbl[i].note);
	}
	if (s->verify_other)
		printf("chip:   (%u other command%s, not classified)\n",
		       s->verify_other, s->verify_other == 1 ? "" : "s");
	/*
	 * Count reads separately. Lumping them in reads as "88 % verified"
	 * when every state change is unchecked, which is the opposite of the
	 * truth. A summary that flatters is worse than no summary.
	 */
	/*
	 * Say whether anyone else was on the air. A run with another handset
	 * competing for this link cannot be read as a result about the handset
	 * we were serving, and the run should declare that itself rather than
	 * leave it to be reconstructed from the log afterwards.
	 */
	if (s->base.foreign_seg || s->base.foreign_conn)
		printf("reg: *** OTHER HANDSETS WERE ON AIR *** %lu C-plane "
		       "segment(s) for another handle (last: %u) and %lu "
		       "connection-up(s) refused. This link serves one "
		       "handset; a run with competing traffic is NOT a clean "
		       "measurement. Power the others off.\n",
		       (unsigned long)s->base.foreign_seg,
		       (unsigned)s->base.foreign_seg_handle,
		       (unsigned long)s->base.foreign_conn);
	printf("chip: %lu state-changing command%s sent, %lu verified by "
	       "read-back. %lu query/read%s.\n",
	       changes, changes == 1 ? "" : "s", verified,
	       reads, reads == 1 ? "" : "s");
	if (changes > verified)
		printf("chip: the other %lu were sent and assumed -- the chip "
		       "has no error indication to have missed [A-0086]. "
		       "See docs/verification-gap.md\n", changes - verified);
	fflush(stdout);
}

/* ---- SSC transport (7412): bit alignment and byte order ---- */

/* Both transports' block headers are read through the same code. */
_Static_assert(sizeof(struct ssc_pcm_block) == sizeof(struct dect_pcm_block),
	       "block headers differ in size");

_Static_assert(SSC_PCM_BF_DROPPED == DECT_PCM_BF_DROPPED,
	       "dropped flags differ");

int pcm_block_ok(const struct stubd *s, const void *hdr)
{
	const struct dect_pcm_block *h = hdr;

	return h->magic == (s->ssc ? SSC_PCM_BLOCK_MAGIC : DECT_PCM_BLOCK_MAGIC);
}

/*
 * Receive, in place: the chip's lanes onto slot boundaries, each slot a
 * little-endian sample as in the 7520's DMA buffers (ssc_align.h). The
 * stream comes out one slot late; slot probing does not mind.
 */
void ssc_rx_fix(struct stubd *s, uint8_t *fr, size_t len)
{
	if (!s->ssc || !s->ssc_aligned || len < 2u ||
	    len > s->slots * 2u * s->fpb)
		return;
	ssc_align_rx(&s->ssc_al, fr, s->ssc_rx_t, len);
	ssc_lane_delay_apply(&s->ssc_ld, fr, len);
}

/* "10,11,12" for the lane positions in mask; "-" for none. */
void lanes_str(char *out, size_t cap, uint64_t mask)
{
	size_t n = 0;
	unsigned int i;

	out[0] = '\0';
	for (i = 0; i < 64u && n + 4u < cap; i++)
		if (mask & (1ull << i))
			n += (size_t)snprintf(out + n, cap - n, "%s%u",
					      n ? "," : "", i);
	if (!n && cap > 1u)
		snprintf(out, cap, "-");
}

/*
 * The SSC alignment watch, once a second on a raw block (before
 * ssc_rx_fix()): where the chip's idle lanes sit (ssc_idle_lanes()) and the
 * driver's loss counters. Prints the first measurement, any change that
 * holds for two checks, any lost data and every session death, with the
 * counter deltas since the last line. Reads only; changes nothing.
 *   lanes rotated, offset kept, overruns up -> the SSC lost words;
 *   offset changed, counters still          -> a bit slipped on the bus;
 *   lanes rotating step by step             -> the frame length is wrong;
 *   nothing moved at a death                -> the STUB stopped talking.
 */
void ssc_watch(struct stubd *s, const uint8_t *raw, size_t len,
		      uint64_t now)
{
	struct ssc_pcm_stats st;
	char was[200], is[200];
	const char *why = NULL;
	uint64_t mask;
	int off, rot;

	if (!s->ssc || !s->slots || now < s->ssc_w_next)
		return;
	s->ssc_w_next = now + 1000u;
	off = ssc_idle_lanes(raw, len, s->slots, &mask);
	memset(&st, 0, sizeof(st));
	if (ioctl(s->fd, SSC_PCM_IOC_STATS, &st))
		return;
	if (s->ssc_w_off == -2) {
		lanes_str(is, sizeof(is), mask);
		printf("ssc: watch: idle lanes at bit %d, positions %s; "
		       "driver frames %llu overruns %u dropped %llu "
		       "tx_underruns %u\n", off, is,
		       (unsigned long long)st.frames, st.overruns,
		       (unsigned long long)st.dropped, st.tx_underruns);
		s->ssc_w_off = s->ssc_w_pend_off = off;
		s->ssc_w_mask = s->ssc_w_pend_mask = mask;
		s->ssc_w_st = st;
		s->ssc_w_deaths = s->sess.stats.deaths;
		return;
	}
	if (off != s->ssc_w_off || mask != s->ssc_w_mask) {
		/* one check is noise (a lane in use); two in a row are not */
		if (off == s->ssc_w_pend_off && mask == s->ssc_w_pend_mask)
			why = "LANES MOVED";
		s->ssc_w_pend_off = off;
		s->ssc_w_pend_mask = mask;
	} else {
		s->ssc_w_pend_off = off;
		s->ssc_w_pend_mask = mask;
	}
	if (!why && (st.overruns != s->ssc_w_st.overruns ||
		     st.dropped != s->ssc_w_st.dropped ||
		     st.tx_underruns != s->ssc_w_st.tx_underruns))
		why = "driver lost data";
	if (s->sess.stats.deaths != s->ssc_w_deaths)
		why = "STUB SILENT";
	if (!why)
		return;
	lanes_str(was, sizeof(was), s->ssc_w_mask);
	lanes_str(is, sizeof(is), mask);
	rot = ssc_lanes_rotation(s->ssc_w_mask, mask, s->slots);
	printf("ssc: watch: %s at %llu ms: bit %d -> %d, idle lanes %s -> %s "
	       "(%s%d); driver +%llu frames, overruns +%u, dropped +%llu, "
	       "tx_underruns +%u\n", why,
	       (unsigned long long)(now - g_start), s->ssc_w_off, off, was, is,
	       rot < 0 ? "no rotation " : "rotated by ", rot < 0 ? 0 : rot,
	       (unsigned long long)(st.frames - s->ssc_w_st.frames),
	       st.overruns - s->ssc_w_st.overruns,
	       (unsigned long long)(st.dropped - s->ssc_w_st.dropped),
	       st.tx_underruns - s->ssc_w_st.tx_underruns);
	fflush(stdout);
	s->ssc_w_off = off;
	s->ssc_w_mask = mask;
	s->ssc_w_st = st;
	s->ssc_w_deaths = s->sess.stats.deaths;
}

/*
 * Write whole frames. On the SSC transport, the inverse of ssc_rx_fix().
 * burst: a one-off write between the driver's idle frames (probing, before
 * the block loop runs); its head then comes from the idle frame, and one
 * idle frame is appended so the bits the shift pushes past the end are idle
 * bits too. Returns the bytes of frames taken, as write() would.
 */
ssize_t pcm_write(struct stubd *s, const uint8_t *frames, size_t len,
			 int burst)
{
	size_t fb = s->slots * 2u, tail = burst ? fb : 0u, n = len + tail;
	ssize_t w;

	if (!s->ssc)
		return write(s->fd, frames, len);
	if (n > s->ssc_tx_cap) {
		errno = EMSGSIZE;
		return -1;
	}
	ssc_align_tx(&s->ssc_al, s->ssc_tx_out, s->ssc_tx_t, frames, len, tail,
		     burst);
	w = write(s->fd, s->ssc_tx_out, n);
	if (w > 0)
		ssc_align_tx_commit(&s->ssc_al, s->ssc_tx_t, (size_t)w);
	if (w > (ssize_t)len)
		w = (ssize_t)len;
	return w;
}

/*
 * Send a mail: encode the transmit element (libopendectcell), two bytes per frame
 * in the transmit control slot. Once run_loop() runs, the bytes join the
 * control-slot queue that tx_block() drains; before that (probing, bring-up)
 * they go out at once in a burst of frames, idle (0xFF) elsewhere.
 */
int send_mail(struct stubd *s, unsigned int tx_slot,
		     const struct dect_mail *m)
{
	uint8_t el[DECT_TX_ELEM_MAX], *frames;
	size_t fb = s->slots * 2u, nf, k;
	int el_len = dect_mail_encode(m, s->tx_seq, el, sizeof(el));
	ssize_t w;

	if (el_len <= 0 || tx_slot >= s->slots)
		return -1;
	verify_count(s, (uint16_t)m->prim);
#ifdef STUBD_TEST
	stubd_test_sent(m);
	return 0;
#endif

	/*
	 * Once the ring is running every write goes through it, so the mail is
	 * appended to the control-slot byte queue and the block loop paces it
	 * out. Before that -- control-slot probing and bring-up -- there is no
	 * ring yet, so the original burst write is still used.
	 */
	if (s->tx_ring) {
		if (s->txq_len + (size_t)el_len > sizeof(s->txq)) {
			s->txq_drops++;
			return -1;
		}
		for (k = 0; k < (size_t)el_len; k++)
			s->txq[(s->txq_head + s->txq_len + k) % sizeof(s->txq)] =
				el[k];
		s->txq_len += (size_t)el_len;
		if (s->txq_len > s->txq_hwm)
			s->txq_hwm = s->txq_len;
		s->tx_seq++;
		return 0;
	}

	nf = (size_t)el_len / 2u;
	frames = malloc(nf * fb);
	if (!frames)
		return -1;
	memset(frames, 0xff, nf * fb);
	for (k = 0; k < nf; k++)
		memcpy(frames + k * fb + tx_slot * 2u, el + 2u * k, 2);
	w = pcm_write(s, frames, nf * fb, 1);
	free(frames);
	if (w != (ssize_t)(nf * fb))
		return -1;
	s->tx_seq++;
	return 0;
}

/*
 * A command to the chip from our PCM task, on the control slot: to task dst
 * (the contact task for every command but the 0xFFF1 debug requests), with
 * n parameter bytes p. Logged under -v; log_mail() masks key bytes.
 */
void chip_mail_to(struct stubd *s, uint8_t dst, uint16_t prim,
			 const uint8_t *p, uint16_t n)
{
	struct dect_mail m;

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = dst;
	m.prim = prim;
	if (n > sizeof(m.param))
		n = sizeof(m.param);
	memcpy(m.param, p, n);
	m.param_len = n;
	(void)dect_base_send(&s->base, &m);	/* base_send() logs it */
}

void chip_mail(struct stubd *s, uint16_t prim, const uint8_t *p,
		      uint16_t n)
{
	chip_mail_to(s, DECT_TASK_CONTACT_DST, prim, p, n);
}

/* Read n bytes of chip memory at addr; see dect_base_mem_read(). */
void chip_mem_read(struct stubd *s, uint32_t addr, uint16_t n)
{
	dect_base_mem_read(&s->base, addr, n);
}

/*
 * 0x0230 ASSIGN_AUDIO_CHANNEL in its full form: {link, mode byte, TDM slot
 * mask (LE), master, extra} [CR A-CodecModes-1]; the -a keys call the mode
 * byte flags, the master byte mode and the extra byte codec.
 */
void chip_assign(struct stubd *s, uint8_t handle, uint8_t flags,
			uint16_t mask, uint8_t mode, uint8_t codec)
{
	dect_base_audio_assign(&s->base, handle, flags, mask, mode, codec);
}

/* A received lane word that carries bearer data rather than a fill. */
int lane_word_live(uint32_t v)
{
	return v <= 0xFFFFu && v != 0x0000u && v != 0xFEFEu && v != 0xFFFFu;
}

/*
 * Parse "SRC:DST:PRIM[:HEX]" into a mail. HEX must be pairs of hex digits:
 * the loop used to step two characters at a time with "%2x", which takes
 * one digit as readily as two, so an odd-length string ("...:1") stepped
 * past the terminating NUL and read beyond it. Found by the second external
 * review, the clean-room record/an external review-1-24102026.md, finding 8.
 */
int parse_mail_spec(const char *spec, struct dect_mail *m)
{
	unsigned long src, dst, prim;
	const char *par = NULL;
	char *end;
	size_t plen = 0, k;

	memset(m, 0, sizeof(*m));
	src = strtoul(spec, &end, 0);
	if (*end != ':')
		return -1;
	dst = strtoul(end + 1, &end, 0);
	if (*end != ':')
		return -1;
	prim = strtoul(end + 1, &end, 0);
	if (*end == ':')
		par = end + 1;
	else if (*end)
		return -1;
	if (src > 0xff || dst > 0xff || prim > 0xffff)
		return -1;
	if (par) {
		plen = strlen(par);
		if (plen % 2u || plen / 2u > DECT_MAIL_PARAM_MAX)
			return -1;
		for (k = 0; k < plen; k++)
			if (!isxdigit((unsigned char)par[k]))
				return -1;
	}
	m->src = (uint8_t)src;
	m->dst = (uint8_t)dst;
	m->prim = (uint16_t)prim;
	for (k = 0; k < plen; k += 2) {
		unsigned int v;

		if (sscanf(par + k, "%2x", &v) != 1)
			return -1;
		m->param[m->param_len++] = (uint8_t)v;
	}
	return 0;
}

/* Parse a mail spec and send it in the control slot. */
int send_mail_spec(struct stubd *s, const char *spec)
{
	struct dect_mail m;

	if (parse_mail_spec(spec, &m))
		return -1;
	if (send_mail(s, (unsigned int)s->tx_slot, &m))
		return -1;
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);
	return 0;
}

/*
 * Read up to max_blocks of frame data into buf (block payload only), in the
 * 7520's layout once the SSC transport is aligned.
 */
static ssize_t read_frames(struct stubd *s, unsigned int max_blocks,
			   uint8_t *buf, size_t cap, unsigned int timeout_ms)
{
	int fd = s->fd;
	size_t block = s->slots * 2u * s->fpb;
	size_t slot = (sizeof(struct dect_pcm_block) + block + 7u) & ~(size_t)7u;
	uint8_t *tmp = malloc(slot * 16u);
	size_t got = 0;
	unsigned int blocks = 0;

	if (!tmp)
		return -1;
	while (blocks < max_blocks && !stop) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		ssize_t n;
		size_t off;
		int pr = poll(&pfd, 1, (int)timeout_ms);

		if (pr < 0 && errno == EINTR)
			break;
		if (pr <= 0)
			break;
		n = read(fd, tmp, slot * 16u);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			free(tmp);
			return -1;
		}
		for (off = 0; off + slot <= (size_t)n && blocks < max_blocks;
		     off += slot) {
			const struct dect_pcm_block *h = (const void *)(tmp + off);

			if (!pcm_block_ok(s, h) || h->len != block)
				continue;
			if (got + block > cap)
				goto done;
			memcpy(buf + got, tmp + off + sizeof(*h), block);
			ssc_rx_fix(s, buf + got, block);
			got += block;
			blocks++;
		}
	}
done:
	free(tmp);
	return (ssize_t)got;
}

/*
 * Does the byte stream of one slot in frame data hold a contact answer?
 * Every mail in the window counts, not just the first: on a freshly booted
 * chip the answer comes right after the 0xFFF2 build stamp
 * (docs/stub-pcm-control-channel.md), and a search that looked at the
 * first mail only never found the 7362 SL's control slot after an upload
 * (2026-11-01, docs/journals/bench-7362sl-g2.md). Pulling as we push keeps a window
 * longer than the receive buffer (DECT_RX_BUF) whole.
 */
static int slot_has_contact_ack(const uint8_t *data, size_t frames,
				unsigned int slots, unsigned int slot,
				struct dect_mail *out)
{
	struct dect_rx rx;
	uint8_t pair[2];
	size_t f;

	dect_rx_init(&rx);
	for (f = 0; f < frames; f++) {
		memcpy(pair, data + (f * slots + slot) * 2u, 2);
		dect_rx_push(&rx, pair, 2);
		while (dect_rx_pull(&rx, out))
			if (out->prim == DECT_PRIM_CONTACT_ACK1 ||
			    out->prim == DECT_PRIM_CONTACT_ACK2)
				return 1;
	}
	return 0;
}

/*
 * Find the control slot pair by probing: send the contact in each transmit
 * slot, then look for a valid STUB answer in any receive slot. Safe because
 * the STUB ignores the contact in every slot but its control slot
 * (docs/stub-pcm-control-channel.md). Sets s->tx_slot and s->rx_slot.
 */
int find_control_slot(struct stubd *s, unsigned int fpb)
{
	size_t block = s->slots * 2u * fpb;
	size_t cap = block * PROBE_BLOCKS;
	uint8_t *data = malloc(cap);
	unsigned int tx, rx;
	int ret = -1;

	if (!data)
		return -1;
	for (tx = 0; tx < s->slots && !stop; tx++) {
		struct dect_mail cm, ans;
		ssize_t n;
		size_t frames;

		dect_session_contact_mail(&s->sess, &cm);
		if (send_mail(s, tx, &cm)) {
			if (s->verbose)
				fprintf(stderr, "probe: send in slot %u failed\n", tx);
			continue;
		}
		n = read_frames(s, PROBE_BLOCKS, data, cap, 500);
		if (n <= 0)
			continue;
		frames = (size_t)n / (s->slots * 2u);
		for (rx = 0; rx < s->slots; rx++) {
			/*
			 * Only a contact ACK proves the chip read our mail on
			 * THIS tx slot. The chip also emits unsolicited
			 * reports on its own schedule -- 0xFFF1 statistics
			 * and 0xF618 every 5 s, 0xF623 scan reports -- so
			 * accepting any mail at all lets one of those land
			 * inside the 500 ms probe window and lock the search
			 * onto a slot the chip never read. The daemon then
			 * runs with a transmit slot nothing listens to: mails
			 * go nowhere, the radio is never brought up, and the
			 * handset cannot see the base, while the log still
			 * says "control slot found" and the chip's periodic
			 * reports keep arriving. That is 2026-10-23 vF46 --
			 * "tx 0, rx 1 (answer prim fff1)", session contacts 3
			 * answers 0 -- against the working "tx 1, rx 1
			 * (answer prim f601)" of the day before.
			 */
			if (!slot_has_contact_ack(data, frames, s->slots, rx,
						  &ans))
				continue;
			s->tx_slot = (int)tx;
			s->rx_slot = (int)rx;
			printf("control slot found: tx %u, rx %u (answer prim %04x)\n",
			       tx, rx, ans.prim);
			/*
			 * This answer carries the RFPI the chip broadcasts, and
			 * the run loop never sees it again: keep it for the
			 * controller (STATUS tag 6, HELLO tag 17).
			 */
			if (ans.prim == DECT_PRIM_CONTACT_ACK1 && ans.param_len >= 5u) {
				memcpy(s->cell.rfpi, ans.param, sizeof(s->cell.rfpi));
				s->cell.have_rfpi = 1;
				printf("chip: broadcasts RFPI %02x%02x%02x%02x%02x\n",
				       s->cell.rfpi[0], s->cell.rfpi[1], s->cell.rfpi[2],
				       s->cell.rfpi[3], s->cell.rfpi[4]);
			}
			ret = 0;
			goto out;
		}
	}
out:
	free(data);
	return ret;
}

/*
 * Measure the bit offset of the chip's 16-bit lanes in the SSC's stream:
 * the offset at which the most lanes read 0xFEFE, the chip's idle pattern
 * (three such lanes per frame on the 7412's 06.88 STUB in mode 0x119,
 * observed 2026-10-29). Where two idle lanes are adjacent the pattern also
 * matches 8 bits later (two per frame there), so the control-slot search
 * retries at offset + 8 before it gives up (ssc_flip()).
 */
static int ssc_measure(struct stubd *s)
{
	size_t block = s->slots * 2u * s->fpb, cap = block * 16u;
	uint8_t *d = malloc(cap);
	unsigned int cnt[16], best;
	ssize_t n = 0;
	int tries, o;

	if (!d)
		return -1;
	/* The chip clocks the bus once its STUB runs; give it 10 s. */
	for (tries = 0; tries < 50 && n <= 0 && !stop; tries++)
		n = read_frames(s, 16, d, cap, 200);
	if (n <= 0) {
		fprintf(stderr, "ssc: no frames -- the chip is not clocking "
			"the bus\n");
		free(d);
		return -1;
	}
	o = ssc_idle_offset(d, (size_t)n, cnt);
	free(d);
	if (o < 0) {
		fprintf(stderr, "ssc: no idle lane (0xFEFE) in %zd bytes -- "
			"is the STUB running?\n", n);
		return -1;
	}
	best = (unsigned int)o;
	ssc_align_init(&s->ssc_al, best);
	s->ssc_aligned = 1;
	printf("ssc: lane bit offset %u (%u idle lanes; %u at offset %u)\n",
	       best, cnt[best], cnt[(best + 8u) % 16u], (best + 8u) % 16u);
	return 0;
}

/* The other offset at which the idle pattern reads (see ssc_measure()). */
void ssc_flip(struct stubd *s)
{
	ssc_align_init(&s->ssc_al, s->ssc_al.off + 8u);
	printf("ssc: trying lane bit offset %u\n", s->ssc_al.off);
}

/*
 * The generic SSC PCM transport (7412). The chip's frame has cfg->slots
 * 16-bit slots (-c; 36 on the 7412 in mode 0x119). Starting it after
 * dect-fwload, with the chip already clocking, works as well as starting
 * it before; only the bit offset is new each time (observed on a 7412,
 * 2026-10-29), and ssc_measure() finds it.
 */
static int ssc_bring_up(struct stubd *s, struct dect_pcm_config *cfg,
			const struct ssc_pcm_info *si)
{
	struct ssc_pcm_config sc;
	struct ssc_pcm_tx_idle idle;
	unsigned int step;

	memset(&sc, 0, sizeof(sc));
	sc.slots = cfg->slots;
	sc.frames_per_block = cfg->frames_per_block;
	sc.n_desc = cfg->n_desc ? cfg->n_desc : SSC_N_DESC;
	if (ioctl(s->fd, SSC_PCM_IOC_CONFIG, &sc)) {
		perror("SSC_PCM_CONFIG");
		return -1;
	}
	s->ssc = 1;
	cfg->slots = sc.slots;
	cfg->frames_per_block = sc.frames_per_block;
	cfg->n_desc = sc.n_desc;
	s->slots = sc.slots;
	s->fpb = sc.frames_per_block;
	/* a block, or the longest mail's burst (send_mail()) and its idle tail */
	s->ssc_tx_cap = (size_t)s->slots * 2u *
			(s->fpb > DECT_TX_ELEM_MAX / 2u + 1u ? s->fpb
			 : DECT_TX_ELEM_MAX / 2u + 1u);
	s->ssc_rx_t = malloc((size_t)s->slots * 2u * s->fpb + 2u);
	s->ssc_tx_t = malloc(s->ssc_tx_cap + 2u);
	s->ssc_tx_out = malloc(s->ssc_tx_cap);
	if (!s->ssc_rx_t || !s->ssc_tx_t || !s->ssc_tx_out) {
		fprintf(stderr, "ssc: out of memory\n");
		return -1;
	}
	printf("config: %.32s (uapi %u), slots %u frames %u desc %u tx\n",
	       si->name, si->version, sc.slots, sc.frames_per_block, sc.n_desc);
	/* What the driver sends when no frame is queued: all ones, idle. */
	memset(&idle, 0xff, sizeof(idle));
	if (ioctl(s->fd, SSC_PCM_IOC_TX_IDLE, &idle)) {
		perror("SSC_PCM_TX_IDLE");
		return -1;
	}
	for (step = 1; step <= SSC_PCM_STEP_DMA; step++) {
		if (ioctl(s->fd, SSC_PCM_IOC_STEP, &step)) {
			fprintf(stderr, "step %u: %s\n", step, strerror(errno));
			return -1;
		}
	}
	return ssc_measure(s);
}

int bring_up(struct stubd *s, struct dect_pcm_config *cfg)
{
	struct dect_ioc_info info;
	struct ssc_pcm_info si;
	unsigned int step;

	memset(&info, 0, sizeof(info));
	memset(&si, 0, sizeof(si));
	if (ioctl(s->fd, DECT_IOC_INFO, &info) &&
	    !ioctl(s->fd, SSC_PCM_IOC_INFO, &si))
		return ssc_bring_up(s, cfg, &si);
	if (info.version < 2 || !(info.features & DECT_FEATURE_PCM_RX)) {
		fprintf(stderr, "module without PCM receive (version %u)\n",
			info.version);
		return -1;
	}
	cfg->flags |= DECT_PCM_CF_TX;
	if (ioctl(s->fd, DECT_IOC_PCM_CONFIG, cfg)) {
		perror("PCM_CONFIG");
		return -1;
	}
	s->slots = cfg->slots;
	s->fpb = cfg->frames_per_block;
	printf("config: slots %u rx_delay %u tx_delay %u frames %u desc %u tx\n",
	       cfg->slots, cfg->rx_delay, cfg->tx_delay, cfg->frames_per_block,
	       cfg->n_desc);
	for (step = 1; step <= DECT_PCM_STEP_DMA; step++) {
		if (ioctl(s->fd, DECT_IOC_PCM_STEP, &step)) {
			fprintf(stderr, "step %u: %s\n", step, strerror(errno));
			return -1;
		}
	}
	return 0;
}

void tear_down(struct stubd *s)
{
	unsigned int step = DECT_PCM_STEP_OFF;

	if (s->ssc) {
		step = SSC_PCM_STEP_OFF;
		if (ioctl(s->fd, SSC_PCM_IOC_STEP, &step))
			perror("SSC_PCM_STEP 0");
		free(s->ssc_rx_t);
		free(s->ssc_tx_t);
		free(s->ssc_tx_out);
		return;
	}
	if (ioctl(s->fd, DECT_IOC_PCM_STEP, &step))
		perror("PCM_STEP 0");
}
