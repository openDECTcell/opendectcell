// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect-cell: the link from an openDECTcell cell to its controller
 * (opendectcell-controller docs/cell-protocol.md). It holds the TLS 1.3 link
 * with mutual authentication, enrols, says HELLO, keeps the heartbeat,
 * reconnects, and relays frames between the controller and dect-stubd -K,
 * which owns the chip and talks to us on a local socket in the same frames
 * (§14.2). TLS lives here, in its own process, so a handshake never delays
 * the chip's control lane.
 *
 *   dect-cell enroll 'opendectcell-enroll:v1?h=HOST:PORT&t=TOKEN&ca=PIN'
 *   dect-cell run
 *   dect-cell forget
 *
 * Built against mbedTLS 3.6, OpenWrt's TLS library.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>
#include <psa/crypto.h>

#include "dect_cell.h"
#include "dect_srtp.h"

#ifndef OPENDECTCELL_VERSION
#define OPENDECTCELL_VERSION "dev"
#endif

#define CELL_DIR	"/etc/opendectcell/cell"
#define STUBD_SOCK	"/var/run/opendectcell-cell.sock"
#define AUDIO_SOCK	"/var/run/opendectcell-audio.sock"	/* dect-stubd's */
#define LINK_STATE	"/var/run/opendectcell/cell-link.json"
#define CONNECT_MS	5000u		/* TCP + TLS + HELLO_ACK */
#define BACKOFF_MAX_MS	30000u		/* §6.4 */
#define BACKOFF_RESET_MS 60000u
#define CREDS_POLL_MS	10000u		/* not enrolled: look again */
#define RX_BUF		(2u * DECT_CELL_FRAME_MAX)

static const char *dir = CELL_DIR;
static const char *stubd_path = STUBD_SOCK;
static const char *audio_path = AUDIO_SOCK;
static int allow_plain;		/* -P: the admin allows plain RTP (§8.1) */
static int verbose;
static volatile sig_atomic_t stop;

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;

static void logf_(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("cell: ", stdout);
	vprintf(fmt, ap);
	fputc('\n', stdout);
	fflush(stdout);
	va_end(ap);
}

static uint64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int64_t wall_s(void)
{
	return (int64_t)time(NULL);
}

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

/* ---- files ------------------------------------------------------------ */

static void path_of(char *out, size_t cap, const char *name)
{
	snprintf(out, cap, "%s/%s", dir, name);
}

/* Write a file atomically with mode; 0 or -1. */
static int file_put(const char *name, const void *data, size_t n, mode_t mode)
{
	char p[256], tmp[264];
	int fd;

	path_of(p, sizeof(p), name);
	snprintf(tmp, sizeof(tmp), "%s.tmp", p);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (fd < 0)
		return -1;
	if (write(fd, data, n) != (ssize_t)n || fsync(fd) || close(fd)) {
		(void)unlink(tmp);
		return -1;
	}
	return rename(tmp, p);
}

/* Read a whole file (at most cap - 1 octets, NUL added); its length or -1. */
static long file_get(const char *name, unsigned char *buf, size_t cap)
{
	char p[256];
	FILE *f;
	size_t n;

	path_of(p, sizeof(p), name);
	f = fopen(p, "rb");
	if (!f)
		return -1;
	n = fread(buf, 1, cap - 1u, f);
	if (n == cap - 1u && fgetc(f) != EOF) {
		fclose(f);		/* cut short would not load */
		return -1;
	}
	fclose(f);
	buf[n] = 0;
	return (long)n;
}

/* ---- encodings -------------------------------------------------------- */

static int hexval(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ?
	       c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* RFC 4648 base32, no padding, any case; the length or -1. */
static long base32_decode(const char *s, uint8_t *out, size_t cap)
{
	uint32_t acc = 0;
	unsigned bits = 0;
	size_t n = 0;

	for (; *s && *s != '='; s++) {
		int c = *s, v = c >= 'A' && c <= 'Z' ? c - 'A' :
			c >= 'a' && c <= 'z' ? c - 'a' :
			c >= '2' && c <= '7' ? c - '2' + 26 : -1;

		if (v < 0)
			return -1;
		acc = acc << 5 | (uint32_t)v;
		bits += 5;
		if (bits >= 8) {
			bits -= 8;
			if (n == cap)
				return -1;
			out[n++] = (uint8_t)(acc >> bits);
		}
	}
	return (long)n;
}

static void base64url(const uint8_t *in, size_t n, char *out)
{
	static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnop"
				"qrstuvwxyz0123456789-_";
	size_t i;
	uint32_t acc = 0;
	unsigned bits = 0;

	for (i = 0; i < n; i++) {
		acc = acc << 8 | in[i];
		bits += 8;
		while (bits >= 6) {
			bits -= 6;
			*out++ = a[(acc >> bits) & 63u];
		}
	}
	if (bits)
		*out++ = a[(acc << (6 - bits)) & 63u];
	*out = 0;
}

/* One query value of the enrollment string, %-decoded; 0 or -1. */
static int query_get(const char *q, const char *key, char *out, size_t cap)
{
	size_t kl = strlen(key);

	while (q && *q) {
		if (!strncmp(q, key, kl) && q[kl] == '=') {
			const char *v = q + kl + 1;
			size_t n = 0;

			while (*v && *v != '&') {
				int c = *v++;

				if (c == '%' && hexval(v[0]) >= 0 &&
				    hexval(v[1]) >= 0) {
					c = hexval(v[0]) << 4 | hexval(v[1]);
					v += 2;
				}
				if (n + 1u >= cap)
					return -1;
				out[n++] = (char)c;
			}
			out[n] = 0;
			return 0;
		}
		q = strchr(q, '&');
		if (q)
			q++;
	}
	return -1;
}

/* ---- the controller's certificate (§4.2) ------------------------------ */

static void spki_pin(const mbedtls_x509_crt *c, char out[48])
{
	uint8_t h[32];

	mbedtls_sha256(c->pk_raw.p, c->pk_raw.len, h, 0);
	base64url(h, sizeof(h), out);
}

/*
 * The chain must end in a CA whose public key matches one of our pins, the
 * leaf must verify against that CA for serverAuth, and carry the
 * controller URI. Hostnames are not checked: the pin replaces them. Runs
 * after every handshake, so a resumed session is checked too.
 */
static int verify_controller(const mbedtls_ssl_context *ssl,
			     char pins[][48], unsigned n_pins)
{
	const mbedtls_x509_crt *leaf = mbedtls_ssl_get_peer_cert(ssl), *c;
	const mbedtls_x509_sequence *san;

	if (!leaf)
		return -1;
	for (c = leaf->next; c; c = c->next) {
		mbedtls_x509_crt ca;
		char pin[48];
		uint32_t flags = 0;
		unsigned i;
		int ok = 0;

		spki_pin(c, pin);
		for (i = 0; i < n_pins; i++)
			ok |= !strcmp(pin, pins[i]);
		if (!ok)
			continue;
		mbedtls_x509_crt_init(&ca);
		if (mbedtls_x509_crt_parse_der(&ca, c->raw.p, c->raw.len) ||
		    mbedtls_x509_crt_verify((mbedtls_x509_crt *)leaf, &ca, NULL,
					    NULL, &flags, NULL, NULL) ||
		    mbedtls_x509_crt_check_extended_key_usage(leaf,
			MBEDTLS_OID_SERVER_AUTH,
			MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH))) {
			mbedtls_x509_crt_free(&ca);
			logf_("the controller's certificate does not verify "
			      "against the pinned CA (flags %08x)", flags);
			return -1;
		}
		mbedtls_x509_crt_free(&ca);
		for (san = &leaf->subject_alt_names; san && san->buf.p;
		     san = san->next) {
			mbedtls_x509_subject_alternative_name sn;
			int hit;

			if (mbedtls_x509_parse_subject_alt_name(&san->buf, &sn))
				continue;
			hit = sn.type ==
				MBEDTLS_X509_SAN_UNIFORM_RESOURCE_IDENTIFIER &&
			      sn.san.unstructured_name.len > 23u &&
			      !memcmp(sn.san.unstructured_name.p,
				      "urn:opendectcell:controller:", 23);
			mbedtls_x509_free_subject_alt_name(&sn);
			if (hit)
				return 0;
		}
		logf_("the controller's certificate lacks the controller URI");
		return -1;
	}
	logf_("no CA in the controller's chain matches the pin");
	return -1;
}

/* ---- TLS -------------------------------------------------------------- */

struct tls {
	mbedtls_net_context net;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_x509_crt cert;
	mbedtls_pk_context key;
	int up;
	uint64_t deadline;	/* connect and handshake end by then; 0 = none */
};

static const uint16_t groups[] = { MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1,
				   MBEDTLS_SSL_IANA_TLS_GROUP_NONE };
static const uint16_t sig_algs[] = { MBEDTLS_TLS1_3_SIG_ECDSA_SECP256R1_SHA256,
				     MBEDTLS_TLS1_3_SIG_NONE };

static void tls_close(struct tls *t)
{
	if (t->up)
		(void)mbedtls_ssl_close_notify(&t->ssl);
	mbedtls_ssl_free(&t->ssl);
	mbedtls_ssl_config_free(&t->conf);
	mbedtls_x509_crt_free(&t->cert);
	mbedtls_pk_free(&t->key);
	mbedtls_net_free(&t->net);
	t->up = 0;
}

/*
 * The handshake's reads: each waits at most the time left until the
 * deadline, in slices so that a SIGTERM is seen (a peer that dribbles a
 * byte every few seconds held the handshake for ever: review D2, 6).
 */
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	return mbedtls_net_send(&((struct tls *)ctx)->net, buf, len);
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len,
		    uint32_t timeout)
{
	struct tls *t = ctx;
	uint64_t end = mono_ms() + (timeout ? timeout : 1u);

	if (t->deadline && end > t->deadline)
		end = t->deadline;
	for (;;) {
		uint64_t now = mono_ms();
		int r;

		if (stop || now >= end)
			return MBEDTLS_ERR_SSL_TIMEOUT;
		r = mbedtls_net_recv_timeout(&t->net, buf, len,
					     end - now < 250u ?
					     (uint32_t)(end - now) : 250u);
		if (r != MBEDTLS_ERR_SSL_TIMEOUT)
			return r;
	}
}

/*
 * TCP connect with a time limit: mbedtls_net_connect() blocks in connect()
 * until the kernel gives up, two minutes for a SYN nobody answers.
 */
static int tcp_connect(struct tls *t, const char *host, const char *port)
{
	struct addrinfo hints, *ai, *a;
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port, &hints, &ai))
		return -1;
	for (a = ai; a; a = a->ai_next) {
		struct pollfd p;
		int err = 0;
		socklen_t el = sizeof(err);
		uint64_t now = mono_ms();

		if (now >= t->deadline || stop)
			break;
		fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC |
			    SOCK_NONBLOCK, a->ai_protocol);
		if (fd < 0)
			continue;
		if (connect(fd, a->ai_addr, a->ai_addrlen) && errno != EINPROGRESS)
			goto next;
		p = (struct pollfd){ fd, POLLOUT, 0 };
		while (poll(&p, 1, 250) == 0 && !stop &&
		       mono_ms() < t->deadline)
			;
		if (p.revents & (POLLOUT | POLLERR | POLLHUP) &&
		    !getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) && !err) {
			(void)fcntl(fd, F_SETFL,
				    fcntl(fd, F_GETFL) & ~O_NONBLOCK);
			t->net.fd = fd;
			break;
		}
next:
		close(fd);
		fd = -1;
	}
	freeaddrinfo(ai);
	return fd < 0 ? -1 : 0;
}

/*
 * Connect to host:port and run the TLS 1.3 handshake, with our certificate
 * if key_pem is given (an enrolled cell) and without it (an enrolment).
 * Blocks for at most CONNECT_MS. 0 or -1.
 */
static int tls_open(struct tls *t, const char *addr, const char *key_pem,
		    const uint8_t *cert_der, size_t cert_len,
		    char pins[][48], unsigned n_pins)
{
	char host[200], *port;
	int r;

	memset(t, 0, sizeof(*t));
	t->deadline = mono_ms() + CONNECT_MS;
	mbedtls_net_init(&t->net);
	mbedtls_ssl_init(&t->ssl);
	mbedtls_ssl_config_init(&t->conf);
	mbedtls_x509_crt_init(&t->cert);
	mbedtls_pk_init(&t->key);
	snprintf(host, sizeof(host), "%s", addr);
	port = strrchr(host, ':');
	if (!port)
		return -1;
	*port++ = 0;
	if (host[0] == '[') {			/* [v6]:port */
		memmove(host, host + 1, strlen(host));
		host[strcspn(host, "]")] = 0;
	}
	if (mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
					MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT))
		goto fail;
	mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_3);
	mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_3);
	mbedtls_ssl_conf_groups(&t->conf, groups);
	mbedtls_ssl_conf_sig_algs(&t->conf, sig_algs);
	/* the pin replaces chain verification: checked after the handshake */
	mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
	mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &drbg);
	mbedtls_ssl_conf_read_timeout(&t->conf, CONNECT_MS);
	if (key_pem) {
		if (mbedtls_pk_parse_key(&t->key, (const unsigned char *)key_pem,
					 strlen(key_pem) + 1u, NULL, 0,
					 mbedtls_ctr_drbg_random, &drbg) ||
		    mbedtls_x509_crt_parse_der(&t->cert, cert_der, cert_len) ||
		    mbedtls_ssl_conf_own_cert(&t->conf, &t->cert, &t->key)) {
			logf_("our key or certificate in %s does not load", dir);
			goto fail;
		}
	}
	if (mbedtls_ssl_setup(&t->ssl, &t->conf))
		goto fail;
	if (tcp_connect(t, host, port)) {
		logf_("cannot reach %s", addr);
		goto fail;
	}
	{
		int one = 1;
		struct timeval tv = { CONNECT_MS / 1000u, 0 };

		/* §2: no Nagle; a stuck peer cannot block a write for ever */
		(void)setsockopt(t->net.fd, IPPROTO_TCP, TCP_NODELAY, &one,
				 sizeof(one));
		(void)setsockopt(t->net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv,
				 sizeof(tv));
	}
	mbedtls_ssl_set_bio(&t->ssl, t, bio_send, NULL, bio_recv);
	while ((r = mbedtls_ssl_handshake(&t->ssl))) {
		if (r != MBEDTLS_ERR_SSL_WANT_READ &&
		    r != MBEDTLS_ERR_SSL_WANT_WRITE) {
			logf_("TLS handshake with %s failed (-0x%04x)", addr,
			      (unsigned)-r);
			goto fail;
		}
	}
	if (verify_controller(&t->ssl, pins, n_pins))
		goto fail;
	t->deadline = 0;
	t->up = 1;
	return 0;
fail:
	tls_close(t);
	return -1;
}

/* A whole frame out; 0 or -1. */
static int tls_send(struct tls *t, uint16_t type, uint16_t corr,
		    const uint8_t *body, size_t len)
{
	uint8_t f[DECT_CELL_FRAME_MAX];
	size_t n = dect_cell_put(f, sizeof(f), type, corr, body, len), off = 0;

	if (!n || !t->up)
		return -1;
	while (off < n) {
		int r = mbedtls_ssl_write(&t->ssl, f + off, n - off);

		if (r == MBEDTLS_ERR_SSL_WANT_READ ||
		    r == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (r <= 0)
			return -1;
		off += (size_t)r;
	}
	return 0;
}

/*
 * Read whatever has arrived into buf (len so far); new length, or -1 when
 * the link is gone. With wait_ms 0 it only takes what is ready.
 */
static long tls_recv(struct tls *t, uint8_t *buf, size_t len, size_t cap,
		     uint32_t wait_ms)
{
	mbedtls_ssl_conf_read_timeout(&t->conf, wait_ms ? wait_ms : 1u);
	for (;;) {
		int r;

		if (len == cap)
			return (long)len;	/* the caller frames, then reads on */
		r = mbedtls_ssl_read(&t->ssl, buf + len, cap - len);
		if (r > 0) {
			len += (size_t)r;
			if (!mbedtls_ssl_get_bytes_avail(&t->ssl))
				return (long)len;
			continue;
		}
		if (r == MBEDTLS_ERR_SSL_TIMEOUT ||
		    r == MBEDTLS_ERR_SSL_WANT_READ ||
		    r == MBEDTLS_ERR_SSL_WANT_WRITE)
			return (long)len;
		if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
			continue;
		return -1;			/* closed or broken */
	}
}

/* ---- enrolment (§5) --------------------------------------------------- */

static int rng_init(void)
{
	if (psa_crypto_init() != PSA_SUCCESS)
		return -1;
	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&drbg);
	return mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
				     (const unsigned char *)"dect-cell", 9);
}

static void sw_version(char *out, size_t cap)
{
	snprintf(out, cap, "openDECTcell %s", OPENDECTCELL_VERSION);
}

static void hw_model(char *out, size_t cap)
{
	FILE *f = fopen("/tmp/sysinfo/model", "r");

	out[0] = 0;
	if (f) {
		if (fgets(out, (int)cap, f))
			out[strcspn(out, "\n")] = 0;
		fclose(f);
	}
}

/*
 * §5.2: a new P-256 key made here, which never leaves the cell, as PEM in
 * key_pem; its certificate request at the end of csr_buf, *csr_n octets.
 * 0 or -1.
 */
static int make_key_csr(unsigned char key_pem[512], uint8_t *csr_buf,
			size_t cap, int *csr_n)
{
	mbedtls_pk_context key;
	mbedtls_x509write_csr csr;
	int rc = -1;

	mbedtls_pk_init(&key);
	mbedtls_x509write_csr_init(&csr);
	if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) ||
	    mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
				mbedtls_ctr_drbg_random, &drbg) ||
	    mbedtls_pk_write_key_pem(&key, key_pem, 512))
		goto out;
	mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
	mbedtls_x509write_csr_set_key(&csr, &key);
	if (mbedtls_x509write_csr_set_subject_name(&csr, "CN=opendectcell-cell"))
		goto out;
	*csr_n = mbedtls_x509write_csr_der(&csr, csr_buf, cap,
					   mbedtls_ctr_drbg_random, &drbg);
	rc = *csr_n > 0 ? 0 : -1;
out:
	mbedtls_x509write_csr_free(&csr);
	mbedtls_pk_free(&key);
	return rc;
}

static int enroll(const char *s)
{
	char addr[200], tok[64], pin[64][48], hint[64], sw[64], hw[64];
	uint8_t token[20], csr_buf[1024], body[2048], rx[RX_BUF];
	unsigned char key_pem[512];
	struct dect_cell_tlv t;
	struct dect_cell_frame f;
	struct tls tl;
	const char *q;
	const uint8_t *cert, *ca, *id;
	size_t cert_n = 0, ca_n = 0, id_n = 0;
	uint16_t status = 0xFFFF;
	uint8_t state = 0;
	int csr_n, rc = 1;
	long n = 0, used;
	mbedtls_x509_crt cacrt;
	char capin[48], text[512];

	if (strncmp(s, "opendectcell-enroll:v1?", 18)) {
		fprintf(stderr, "not a opendectcell-enroll:v1 string\n");
		return 2;
	}
	q = s + 18;
	if (query_get(q, "h", addr, sizeof(addr)) ||
	    query_get(q, "t", tok, sizeof(tok)) ||
	    query_get(q, "ca", pin[0], sizeof(pin[0])) ||
	    base32_decode(tok, token, sizeof(token)) != (long)sizeof(token) ||
	    strlen(pin[0]) != 43u || !strchr(addr, ':')) {
		fprintf(stderr, "the enrollment string is malformed\n");
		return 2;
	}
	if (mkdir(dir, 0700) && errno != EEXIST) {
		perror(dir);
		return 1;
	}
	if (make_key_csr(key_pem, csr_buf, sizeof(csr_buf), &csr_n)) {
		fprintf(stderr, "cannot make a key and its request\n");
		goto out;
	}
	if (tls_open(&tl, addr, NULL, NULL, 0, pin, 1)) {
		fprintf(stderr, "no verified connection to %s\n", addr);
		goto out;
	}
	gethostname(hint, sizeof(hint));
	hint[sizeof(hint) - 1] = 0;
	sw_version(sw, sizeof(sw));
	hw_model(hw, sizeof(hw));
	dect_cell_tlv_init(&t, body, sizeof(body));
	dect_cell_tlv_put(&t, 1, token, sizeof(token));
	dect_cell_tlv_put(&t, 2, csr_buf + sizeof(csr_buf) - csr_n,
			  (size_t)csr_n);
	if (hw[0])
		dect_cell_tlv_str(&t, 3, hw);
	dect_cell_tlv_str(&t, 4, sw);
	dect_cell_tlv_str(&t, 5, hint);
	if (t.err || tls_send(&tl, DECT_CELL_ENROLL_REQUEST, 1, body, t.len)) {
		fprintf(stderr, "cannot send the request\n");
		goto close;
	}
	/* the controller may delay a rate-limited source by 30 s (§5.2) */
	for (used = 0; used < 60; used++) {
		n = tls_recv(&tl, rx, (size_t)n, sizeof(rx), 1000);
		if (n < 0 || dect_cell_parse(rx, (size_t)n, &f) != 0)
			break;
	}
	if (n <= 0 || dect_cell_parse(rx, (size_t)n, &f) <= 0 ||
	    f.type != DECT_CELL_ENROLL_RESPONSE ||
	    dect_cell_tlv_check(f.body, f.len) ||
	    dect_cell_tlv_get_u16(f.body, f.len, 1, &status)) {
		fprintf(stderr, "no answer from the controller\n");
		goto close;
	}
	if (status) {
		fprintf(stderr, "enrolment refused: %s\n",
			status == 1 ? "token rejected (unknown, used or "
				      "expired)" :
			status == 2 ? "bad certificate request" :
			status == 3 ? "rate limited, try again later" :
				      "controller error");
		goto close;
	}
	id = dect_cell_tlv_get(f.body, f.len, 2, &id_n);
	cert = dect_cell_tlv_get(f.body, f.len, 3, &cert_n);
	ca = dect_cell_tlv_get(f.body, f.len, 4, &ca_n);
	(void)dect_cell_tlv_get_u8(f.body, f.len, 5, &state);
	mbedtls_x509_crt_init(&cacrt);
	if (!id || id_n != 16u || !cert || !ca ||
	    mbedtls_x509_crt_parse_der(&cacrt, ca, ca_n)) {
		fprintf(stderr, "the controller's answer is incomplete\n");
		mbedtls_x509_crt_free(&cacrt);
		goto close;
	}
	spki_pin(&cacrt, capin);
	mbedtls_x509_crt_free(&cacrt);
	if (strcmp(capin, pin[0])) {
		fprintf(stderr, "the CA certificate does not match the pin\n");
		goto close;
	}
	snprintf(text, sizeof(text), "controller=%s\npin=%s\ncell_id=", addr,
		 pin[0]);
	for (used = 0; used < 16; used++)
		snprintf(text + strlen(text), sizeof(text) - strlen(text),
			 "%02x", id[used]);
	strcat(text, "\n");
	if (file_put("key.pem", key_pem, strlen((char *)key_pem), 0600) ||
	    file_put("cert.der", cert, cert_n, 0600) ||
	    file_put("ca.der", ca, ca_n, 0600) ||
	    file_put("enrollment", text, strlen(text), 0600)) {
		fprintf(stderr, "cannot write %s\n", dir);
		goto close;
	}
	printf("enrolled with %s; the cell is %s\n", addr,
	       state == DECT_CELL_ST_ACTIVE ? "active" :
	       "pending until an admin approves it");
	rc = 0;
close:
	tls_close(&tl);
out:
	mbedtls_platform_zeroize(key_pem, sizeof(key_pem));
	return rc;
}

static int forget(void)
{
	static const char *const names[] = { "key.pem", "cert.der", "ca.der",
					     "enrollment", "key.pem.new",
					     "cert.der.new" };
	unsigned i;
	char p[256];

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		path_of(p, sizeof(p), names[i]);
		(void)unlink(p);
	}
	printf("the cell's credentials are erased\n");
	return 0;
}

/* ---- the running link ------------------------------------------------- */

struct creds {
	char addr[200];
	char pins[1][48];
	char key_pem[512];
	uint8_t cert[DECT_CELL_FRAME_MAX];	/* as the enrolment brought it */
	size_t cert_n;
	char cell_id[33];
	int64_t not_before, not_after;	/* the certificate's, Unix seconds */
};

static int64_t x509_time_s(const mbedtls_x509_time *t)
{
	struct tm tm;

	memset(&tm, 0, sizeof(tm));
	tm.tm_year = t->year - 1900;
	tm.tm_mon = t->mon - 1;
	tm.tm_mday = t->day;
	tm.tm_hour = t->hour;
	tm.tm_min = t->min;
	tm.tm_sec = t->sec;
	return (int64_t)timegm(&tm);
}

/* The key in key_pem is the one cert certifies: 0, else -1. */
static int pair_ok(const uint8_t *cert, size_t cert_n, const char *key_pem,
		   int64_t *from, int64_t *to)
{
	mbedtls_x509_crt crt;
	mbedtls_pk_context key;
	int r;

	mbedtls_x509_crt_init(&crt);
	mbedtls_pk_init(&key);
	r = mbedtls_x509_crt_parse_der(&crt, cert, cert_n) ||
	    mbedtls_pk_parse_key(&key, (const unsigned char *)key_pem,
				 strlen(key_pem) + 1u, NULL, 0,
				 mbedtls_ctr_drbg_random, &drbg) ||
	    mbedtls_pk_check_pair(&crt.pk, &key, mbedtls_ctr_drbg_random,
				  &drbg);
	if (!r && from) {
		*from = x509_time_s(&crt.valid_from);
		*to = x509_time_s(&crt.valid_to);
	}
	mbedtls_pk_free(&key);
	mbedtls_x509_crt_free(&crt);
	return r ? -1 : 0;
}

static int creds_load(struct creds *c)
{
	char text[512], *p;
	long n;

	memset(c, 0, sizeof(*c));
	if (file_get("enrollment", (unsigned char *)text, sizeof(text)) < 0 ||
	    file_get("key.pem", (unsigned char *)c->key_pem,
		     sizeof(c->key_pem)) <= 0 ||
	    (n = file_get("cert.der", c->cert, sizeof(c->cert))) <= 0)
		return -1;
	c->cert_n = (size_t)n;
	/*
	 * A renewal writes key.pem.new and cert.der.new, then renames the
	 * certificate and the key. Cut short between the two, the new
	 * certificate sits beside the old key: the new key finishes it.
	 * Cut short before, the old pair is whole and the new files go.
	 */
	if (pair_ok(c->cert, c->cert_n, c->key_pem, &c->not_before,
		    &c->not_after) == 0) {
		char np[256];

		path_of(np, sizeof(np), "key.pem.new");
		(void)unlink(np);
		path_of(np, sizeof(np), "cert.der.new");
		(void)unlink(np);
	} else {
		char np[256], kp[256];

		if (file_get("key.pem.new", (unsigned char *)c->key_pem,
			     sizeof(c->key_pem)) <= 0 ||
		    pair_ok(c->cert, c->cert_n, c->key_pem, &c->not_before,
			    &c->not_after)) {
			logf_("the key in %s is not the certificate's", dir);
			mbedtls_platform_zeroize(c->key_pem, sizeof(c->key_pem));
			return -1;
		}
		path_of(np, sizeof(np), "key.pem.new");
		path_of(kp, sizeof(kp), "key.pem");
		if (rename(np, kp))
			return -1;
		logf_("a renewal cut short is completed: the new key is in use");
	}
	for (p = strtok(text, "\n"); p; p = strtok(NULL, "\n")) {
		if (!strncmp(p, "controller=", 11))
			snprintf(c->addr, sizeof(c->addr), "%s", p + 11);
		else if (!strncmp(p, "pin=", 4))
			snprintf(c->pins[0], sizeof(c->pins[0]), "%s", p + 4);
		else if (!strncmp(p, "cell_id=", 8))
			snprintf(c->cell_id, sizeof(c->cell_id), "%s", p + 8);
	}
	return c->addr[0] && c->pins[0][0] ? 0 : -1;
}

/*
 * §8 media: a session per MEDIA_OPEN, until MEDIA_CLOSED. The controller's
 * UDP flow on one side (RTP, SRTP when keyed), dect-stubd's audio socket on
 * the other: 20 ms frames of 160 G.722 octets or 160 linear samples at
 * 8 kHz in host order (docs/bridge-interface.md §3, cell-protocol §14.2).
 * The keys stay here; dect-stubd gets the session without them.
 */
#define MEDIA_MAX	3u		/* the chip's voice channels */
#define RTP_MAX		(12u + 320u + DECT_SRTP_OVERHEAD)

struct media {
	int      used;		/* MEDIA_OPEN taken, sent to dect-stubd */
	int      live;		/* dect-stubd took it: the flow runs */
	uint32_t id;
	uint8_t  fmt, ptime, pt, profile, dscp;
	struct sockaddr_storage peer;
	socklen_t peer_len;
	uint32_t ssrc_up, ssrc_down;
	struct dect_srtp tx, rx;
	int      udp, audio;	/* -1 = none */
	uint16_t seq;
	uint32_t ts;
	int      marker;	/* the first packet's */
	uint8_t  half[320];	/* ptime 10: the first half of 20 ms down */
	size_t   half_len;
	uint16_t half_seq;
	uint32_t pkts_up, pkts_down, dropped;
};

struct link {
	struct tls tls;
	int stubd;			/* dect-stubd's local socket, -1 */
	uint8_t rx[RX_BUF];
	size_t rx_len;
	/* from dect-stubd's STATUS */
	int have_status;
	uint8_t radio, chip;
	uint32_t epoch;
	char stub[64];
	/* the session */
	uint8_t cell_state;
	uint16_t heartbeat_ms, dead_ms;
	uint64_t last_rx, last_tx, up_since, next_try;
	uint32_t backoff_ms, reconnects, rtt_ms;
	uint8_t boot_id[16];
	const char *state;		/* for the Monitor */
	struct creds creds;
	int enrolled;
	uint32_t dropped;		/* frames for a controller we lack */
	uint64_t next_stats;		/* §10: STATS every stats_interval_s */
	/*
	 * §6.4 resume: the session's token from HELLO_ACK, offered again in
	 * HELLO while the grace period since the link went down lasts.
	 */
	uint8_t  token[16];
	int      have_token;
	uint64_t lost_at;
	uint16_t grace_ms;
	uint32_t caps;			/* HELLO_ACK tag 10: what both have */
	struct media media[MEDIA_MAX];
	/* §5.4 renewal: the new key until its certificate comes */
	unsigned char renew_key[512];
	int      renewing;
	uint64_t renew_sent, renew_next;	/* monotonic ms */
};

/* §10 stats_interval_s, its default: CONFIG_SET is not applied yet */
#define STATS_INTERVAL_MS	60000u

static void link_state_write(struct link *l)
{
	FILE *f = fopen(LINK_STATE ".tmp", "w");

	if (!f)
		return;
	/* rtt_ms 0: not measured (only our own heartbeats measure it) */
	fprintf(f, "{\"state\":\"%s\",\"controller\":\"%s\",\"cell_id\":\"%s\","
		"\"cell_state\":\"%s\",\"rtt_ms\":%u,\"reconnects\":%u,"
		"\"dect_stubd\":%s,\"dropped\":%u,\"cert_expires\":%lld}\n",
		l->state,
		l->enrolled ? l->creds.addr : "", l->enrolled ?
		l->creds.cell_id : "",
		!l->tls.up ? "" : l->cell_state == DECT_CELL_ST_ACTIVE ?
		"active" : l->cell_state == DECT_CELL_ST_PENDING ? "pending" :
		l->cell_state == DECT_CELL_ST_DISABLED ? "disabled" : "revoked",
		l->rtt_ms, l->reconnects, l->stubd >= 0 ? "true" : "false",
		l->dropped, l->enrolled ? (long long)l->creds.not_after : 0LL);
	if (fclose(f) == 0)
		(void)rename(LINK_STATE ".tmp", LINK_STATE);
}

static void set_state(struct link *l, const char *st)
{
	if (l->state != st)
		logf_("%s", st);
	l->state = st;
	link_state_write(l);
}

static void boot_id(uint8_t out[16])
{
	char s[64];
	FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
	unsigned i = 0;
	const char *p;

	memset(out, 0, 16);
	if (!f)
		return;
	if (fgets(s, sizeof(s), f))
		for (p = s; *p && i < 32u; p++)
			if (hexval(*p) >= 0) {
				out[i / 2u] = (uint8_t)(out[i / 2u] << 4 |
							hexval(*p));
				i++;
			}
	fclose(f);
}

/* One frame to dect-stubd; -1 if it is not there or not reading. */
static int stubd_send(struct link *l, uint16_t type, uint16_t corr,
		      const uint8_t *body, size_t len)
{
	uint8_t f[DECT_CELL_FRAME_MAX];
	size_t n = dect_cell_put(f, sizeof(f), type, corr, body, len);

	if (l->stubd < 0 || !n)
		return -1;
	return send(l->stubd, f, n, MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)n ?
	       0 : -1;
}

/* CELL_STATE to dect-stubd: the controller's state, or the link lost. */
static void stubd_cell_state(struct link *l, uint8_t st, const char *reason)
{
	uint8_t b[32];
	struct dect_cell_tlv t;

	dect_cell_tlv_init(&t, b, sizeof(b));
	dect_cell_tlv_u8(&t, 1, st);
	if (reason)
		dect_cell_tlv_str(&t, 2, reason);
	(void)stubd_send(l, DECT_CELL_CELL_STATE, 0, b, t.len);
}

/* §6.4 back-off: at once, then 250 ms doubling to 30 s, each ±20 % */
static void backoff(struct link *l, uint64_t now)
{
	l->backoff_ms = !l->backoff_ms ? 250u :
			l->backoff_ms >= BACKOFF_MAX_MS / 2u ? BACKOFF_MAX_MS :
			l->backoff_ms * 2u;
	l->next_try = now + l->backoff_ms * (80u + (uint32_t)(rand() % 41)) /
		      100u;
}

static void link_down(struct link *l, const char *why, uint64_t now)
{
	if (!l->tls.up)
		return;
	tls_close(&l->tls);
	l->rx_len = 0;
	/* a link that held a minute starts over; one that flaps backs off */
	if (now - l->up_since >= BACKOFF_RESET_MS) {
		l->backoff_ms = 0;
		l->next_try = now;
	} else {
		backoff(l, now);
	}
	logf_("controller link down: %s", why);
	l->lost_at = now;		/* §6.4: the grace period starts */
	stubd_cell_state(l, l->cell_state, "link-lost");
	set_state(l, "connecting");
}

static void ctl_send(struct link *l, uint16_t type, uint16_t corr,
		     const uint8_t *body, size_t len, uint64_t now)
{
	if (tls_send(&l->tls, type, corr, body, len)) {
		link_down(l, "write failed", now);
		return;
	}
	l->last_tx = now;
}

static void ctl_error(struct link *l, uint16_t code, uint16_t type,
		      uint16_t corr, uint64_t now)
{
	uint8_t b[16];
	struct dect_cell_tlv t;

	dect_cell_tlv_init(&t, b, sizeof(b));
	dect_cell_tlv_u16(&t, 1, code);
	dect_cell_tlv_u16(&t, 2, type);
	dect_cell_tlv_u16(&t, 3, corr);
	ctl_send(l, DECT_CELL_ERROR, 0, b, t.len, now);
}

/* §6.1 HELLO, from what dect-stubd last told us. */
static int hello(struct link *l)
{
	uint8_t b[512];
	const uint8_t ver[2] = { DECT_CELL_PROTO_MAJOR, DECT_CELL_PROTO_MINOR };
	const uint8_t formats[2] = { DECT_CELL_FMT_G722, DECT_CELL_FMT_L16_8K };
	struct dect_cell_tlv t;
	char sw[64], hw[64];
	struct timespec ts;

	sw_version(sw, sizeof(sw));
	hw_model(hw, sizeof(hw));
	clock_gettime(CLOCK_REALTIME, &ts);
	dect_cell_tlv_init(&t, b, sizeof(b));
	dect_cell_tlv_put(&t, 1, ver, sizeof(ver));
	dect_cell_tlv_str(&t, 2, sw);
	dect_cell_tlv_str(&t, 3, l->stub[0] ? l->stub : "unknown");
	if (hw[0])
		dect_cell_tlv_str(&t, 4, hw);
	dect_cell_tlv_put(&t, 5, l->boot_id, sizeof(l->boot_id));
	dect_cell_tlv_u32(&t, 6, l->epoch);
	dect_cell_tlv_u32(&t, 7, DECT_CELL_CAP_MAIL | DECT_CELL_CAP_FLOW |
			  DECT_CELL_CAP_STATS | DECT_CELL_CAP_RESUME |
			  DECT_CELL_CAP_MEDIA | DECT_CELL_CAP_SRTP |
			  (allow_plain ? DECT_CELL_CAP_PLAIN_RTP : 0u));
	dect_cell_tlv_u8(&t, 8, 6);	/* subscription records */
	dect_cell_tlv_u8(&t, 9, 6);	/* radio connection contexts */
	dect_cell_tlv_u8(&t, 10, 3);	/* voice channels [CR A-0124] */
	dect_cell_tlv_u8(&t, 11, 3);
	dect_cell_tlv_put(&t, 12, formats, sizeof(formats));
	/* §6.4: resume the session if its grace period still lasts */
	if (l->have_token && mono_ms() - l->lost_at < l->grace_ms)
		dect_cell_tlv_put(&t, 13, l->token, sizeof(l->token));
	dect_cell_tlv_u8(&t, 14, l->radio);
	dect_cell_tlv_u64(&t, 15, (uint64_t)ts.tv_sec * 1000u +
			  (uint64_t)ts.tv_nsec / 1000000u);
	dect_cell_tlv_put(&t, 16, formats, sizeof(formats));
	if (t.err)
		return -1;
	return tls_send(&l->tls, DECT_CELL_HELLO, 1, b, t.len);
}

/* Connect, verify, HELLO, HELLO_ACK. 0 or -1. */
static int link_up(struct link *l)
{
	struct dect_cell_frame f;
	uint16_t hb = 1000, dead = 3000;
	uint8_t st = DECT_CELL_ST_PENDING, resumed = 0;
	long n = 0;
	uint64_t until;

	/*
	 * §5.4: an expired certificate is not renewed; the cell is enrolled
	 * again with a new token. (A clock before the certificate's start is
	 * not set yet, and proves nothing.)
	 */
	if (l->creds.not_after && wall_s() > l->creds.not_after &&
	    wall_s() >= l->creds.not_before) {
		logf_("our certificate has expired: enrol this cell again");
		l->next_try = UINT64_MAX;
		set_state(l, "stopped: certificate expired, enrol again");
		return -1;
	}
	set_state(l, "connecting");
	if (tls_open(&l->tls, l->creds.addr, l->creds.key_pem, l->creds.cert,
		     l->creds.cert_n, l->creds.pins, 1))
		goto fail;
	if (hello(l))
		goto fail;
	until = mono_ms() + CONNECT_MS;
	while ((n = tls_recv(&l->tls, l->rx, (size_t)n, sizeof(l->rx), 500))
	       >= 0 && dect_cell_parse(l->rx, (size_t)n, &f) == 0 &&
	       mono_ms() < until)
		;
	if (n <= 0 || dect_cell_parse(l->rx, (size_t)n, &f) <= 0) {
		logf_("no HELLO_ACK from %s", l->creds.addr);
		goto fail;
	}
	if (f.type == DECT_CELL_GOODBYE) {
		uint16_t why = 0;

		if (!dect_cell_tlv_check(f.body, f.len))
			(void)dect_cell_tlv_get_u16(f.body, f.len, 1, &why);
		logf_("the controller said goodbye (reason %u)", why);
		if (why == DECT_CELL_BYE_REVOKED) {
			(void)forget();
			l->enrolled = 0;
		}
		if (why == DECT_CELL_BYE_VERSION || why == DECT_CELL_BYE_AUTH ||
		    why == DECT_CELL_BYE_REVOKED)
			l->next_try = UINT64_MAX;	/* until restarted */
		goto fail;
	}
	if (f.type != DECT_CELL_HELLO_ACK || dect_cell_tlv_check(f.body, f.len) ||
	    dect_cell_tlv_get_u8(f.body, f.len, 3, &st)) {
		logf_("unexpected answer to HELLO (type 0x%04x)", f.type);
		goto fail;
	}
	(void)dect_cell_tlv_get_u16(f.body, f.len, 6, &hb);
	(void)dect_cell_tlv_get_u16(f.body, f.len, 7, &dead);
	l->caps = 0;
	(void)dect_cell_tlv_get_u32(f.body, f.len, 10, &l->caps);
	{
		size_t tn = 0;
		const uint8_t *tok = dect_cell_tlv_get(f.body, f.len, 4, &tn);
		uint16_t grace = 0;

		(void)dect_cell_tlv_get_u8(f.body, f.len, 5, &resumed);
		(void)dect_cell_tlv_get_u16(f.body, f.len, 8, &grace);
		l->grace_ms = grace ? grace : 7000u;
		l->have_token = tok && tn == sizeof(l->token);
		if (l->have_token)
			memcpy(l->token, tok, sizeof(l->token));
	}
	/* frames that came with the HELLO_ACK stay for the main loop */
	{
		long used = dect_cell_parse(l->rx, (size_t)n, &f);

		memmove(l->rx, l->rx + used, (size_t)(n - used));
		l->rx_len = (size_t)(n - used);
	}
	l->heartbeat_ms = hb ? hb : 1000;
	l->dead_ms = dead ? dead : 3000;
	l->cell_state = st;
	/*
	 * §10 id 6, reconnects since boot: links re-established, not failed
	 * attempts. Counting the failures, two lost and at once restored
	 * links read 0 (bench N6, 7412, 2026-10-30).
	 */
	if (l->up_since)
		l->reconnects++;
	l->last_rx = l->last_tx = l->up_since = mono_ms();
	stubd_cell_state(l, st, NULL);
	set_state(l, "up");
	if (resumed && l->stubd >= 0)	/* §6.4: the snapshot comes next */
		(void)stubd_send(l, DECT_CELL_STATE_SNAPSHOT, 0, NULL, 0);
	logf_("controller link %s to %s, cell %s",
	      resumed ? "resumed" : "up", l->creds.addr,
	      st == DECT_CELL_ST_ACTIVE ? "active" :
	      st == DECT_CELL_ST_PENDING ? "pending approval" : "disabled");
	return 0;
fail:
	tls_close(&l->tls);
	if (l->next_try != UINT64_MAX)
		backoff(l, mono_ms());
	set_state(l, l->enrolled ? "connecting" : "not enrolled");
	return -1;
}

/* ---- media (§8) ------------------------------------------------------- */

static void media_free(struct media *m)
{
	if (m->udp >= 0)
		close(m->udp);
	if (m->audio >= 0)
		close(m->audio);
	dect_srtp_free(&m->tx);
	dect_srtp_free(&m->rx);
	mbedtls_platform_zeroize(m, sizeof(*m));
	m->udp = m->audio = -1;
}

static struct media *media_of(struct link *l, uint32_t id)
{
	unsigned i;

	for (i = 0; i < MEDIA_MAX; i++)
		if (l->media[i].used && l->media[i].id == id)
			return &l->media[i];
	return NULL;
}

static void media_ack(struct link *l, uint16_t corr, uint32_t id,
		      uint16_t status, uint64_t now)
{
	uint8_t b[16];
	struct dect_cell_tlv t;

	if (!l->tls.up)
		return;
	dect_cell_tlv_init(&t, b, sizeof(b));
	dect_cell_tlv_u32(&t, 1, id);
	dect_cell_tlv_u16(&t, 2, status);
	ctl_send(l, DECT_CELL_MEDIA_OPEN_ACK, corr, b, t.len, now);
}

static void media_closed(struct link *l, uint32_t id, uint8_t reason,
			 uint64_t now)
{
	uint8_t b[16];
	struct dect_cell_tlv t;

	if (!l->tls.up)
		return;
	dect_cell_tlv_init(&t, b, sizeof(b));
	dect_cell_tlv_u32(&t, 1, id);
	dect_cell_tlv_u8(&t, 2, reason);
	ctl_send(l, DECT_CELL_MEDIA_CLOSED, 0, b, t.len, now);
}

/*
 * MEDIA_OPEN from the controller (§8.1): checked and keyed here, then
 * passed to dect-stubd without the keys and the address; the flow starts
 * on its MEDIA_OPEN_ACK.
 */
static void media_open(struct link *l, const struct dect_cell_frame *f,
		       uint64_t now)
{
	const uint8_t *body = f->body, *a, *ku, *kd;
	size_t len = f->len, an = 0, kun = 0, kdn = 0, kl;
	uint32_t id, su, sd;
	uint8_t ch, fmt, fmt_up, pt, prof, ptime = 20, dscp = 46;
	struct media *m = NULL;
	uint8_t b[32];
	struct dect_cell_tlv t;
	unsigned i;

	if (!(l->caps & DECT_CELL_CAP_MEDIA)) {
		ctl_error(l, DECT_CELL_ERR_NOT_ALLOWED, f->type, f->corr, now);
		return;
	}
	if (dect_cell_tlv_check(body, len) ||
	    dect_cell_tlv_get_u32(body, len, 1, &id) ||
	    dect_cell_tlv_get_u8(body, len, 2, &ch) ||
	    dect_cell_tlv_get_u8(body, len, 3, &fmt) ||
	    dect_cell_tlv_get_u8(body, len, 5, &pt) ||
	    !(a = dect_cell_tlv_get(body, len, 6, &an)) ||
	    dect_cell_tlv_get_u32(body, len, 7, &su) ||
	    dect_cell_tlv_get_u32(body, len, 8, &sd) ||
	    dect_cell_tlv_get_u8(body, len, 9, &prof) ||
	    !((an == 7u && a[0] == 4u) || (an == 19u && a[0] == 6u))) {
		ctl_error(l, DECT_CELL_ERR_MALFORMED, f->type, f->corr, now);
		return;
	}
	fmt_up = fmt;
	(void)dect_cell_tlv_get_u8(body, len, 13, &fmt_up);
	(void)dect_cell_tlv_get_u8(body, len, 4, &ptime);
	(void)dect_cell_tlv_get_u8(body, len, 12, &dscp);
	ku = dect_cell_tlv_get(body, len, 10, &kun);
	kd = dect_cell_tlv_get(body, len, 11, &kdn);
	kl = prof == DECT_SRTP_AES_CM_SHA1_80 ? 30u :
	     prof == DECT_SRTP_AEAD_AES_128_GCM ? 28u : 0u;
	if (prof && (!ku || !kd || kun != kl || kdn != kl)) {
		ctl_error(l, DECT_CELL_ERR_MALFORMED, f->type, f->corr, now);
		return;
	}
	if ((fmt != DECT_CELL_FMT_G722 && fmt != DECT_CELL_FMT_L16_8K) ||
	    fmt_up != fmt || (ptime != 10u && ptime != 20u) || pt > 127u ||
	    (prof && !kl)) {
		media_ack(l, f->corr, id, 2, now);
		return;
	}
	/* §8.1: plain RTP only where the admin allowed it and both agreed */
	if (!prof && !(allow_plain && (l->caps & DECT_CELL_CAP_PLAIN_RTP))) {
		logf_("media %u: plain RTP refused (not allowed here)", id);
		media_ack(l, f->corr, id, 4, now);
		return;
	}
	if (media_of(l, id)) {
		media_ack(l, f->corr, id, 3, now);
		return;
	}
	for (i = 0; i < MEDIA_MAX && !m; i++)
		if (!l->media[i].used)
			m = &l->media[i];
	if (!m) {
		media_ack(l, f->corr, id, 3, now);
		return;
	}
	memset(m, 0, sizeof(*m));
	m->udp = m->audio = -1;
	if (prof && (dect_srtp_init(&m->tx, prof, ku, kun) ||
		     dect_srtp_init(&m->rx, prof, kd, kdn))) {
		media_free(m);
		media_ack(l, f->corr, id, 4, now);
		return;
	}
	m->id = id;
	m->fmt = fmt;
	m->ptime = ptime;
	m->pt = pt;
	m->profile = prof;
	m->dscp = dscp;
	m->ssrc_up = su;
	m->ssrc_down = sd;
	if (a[0] == 4u) {
		struct sockaddr_in *sa = (struct sockaddr_in *)&m->peer;

		sa->sin_family = AF_INET;
		memcpy(&sa->sin_addr, a + 1, 4);
		memcpy(&sa->sin_port, a + 5, 2);	/* network order */
		m->peer_len = sizeof(*sa);
	} else {
		struct sockaddr_in6 *sa = (struct sockaddr_in6 *)&m->peer;

		sa->sin6_family = AF_INET6;
		memcpy(&sa->sin6_addr, a + 1, 16);
		memcpy(&sa->sin6_port, a + 17, 2);
		m->peer_len = sizeof(*sa);
	}
	/* to dect-stubd: the session, the channel, the formats; no keys */
	dect_cell_tlv_init(&t, b, sizeof(b));
	dect_cell_tlv_u32(&t, 1, id);
	dect_cell_tlv_u8(&t, 2, ch);
	dect_cell_tlv_u8(&t, 3, fmt);
	dect_cell_tlv_u8(&t, 4, ptime);
	dect_cell_tlv_u8(&t, 13, fmt_up);
	if (stubd_send(l, DECT_CELL_MEDIA_OPEN, f->corr, b, t.len)) {
		media_free(m);
		media_ack(l, f->corr, id, 4, now);
		return;
	}
	m->used = 1;
}

/* dect-stubd took the session: the UDP flow and its audio connection. */
static int media_start(struct media *m)
{
	struct sockaddr_un a;
	char hello[48];
	int tos = m->dscp << 2;
	uint8_t r[6];

	m->udp = socket(m->peer.ss_family, SOCK_DGRAM | SOCK_NONBLOCK |
			SOCK_CLOEXEC, 0);
	if (m->udp < 0)
		return -1;
	if (m->peer.ss_family == AF_INET)
		(void)setsockopt(m->udp, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
	else
		(void)setsockopt(m->udp, IPPROTO_IPV6, IPV6_TCLASS, &tos,
				 sizeof(tos));
	/* connected: the kernel drops what does not come from the peer */
	if (connect(m->udp, (struct sockaddr *)&m->peer, m->peer_len))
		return -1;
	m->audio = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK |
			  SOCK_CLOEXEC, 0);
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", audio_path);
	snprintf(hello, sizeof(hello), "media=%u %s", m->id,
		 m->fmt == DECT_CELL_FMT_G722 ? "codec=g722" : "rate=8000");
	if (m->audio < 0 ||
	    connect(m->audio, (struct sockaddr *)&a, sizeof(a)) ||
	    send(m->audio, hello, strlen(hello), MSG_NOSIGNAL) !=
	    (ssize_t)strlen(hello))
		return -1;
	/* RFC 3550 §5.1: random first sequence number and timestamp */
	if (mbedtls_ctr_drbg_random(&drbg, r, sizeof(r)))
		return -1;
	m->seq = (uint16_t)(r[0] << 8 | r[1]);
	m->ts = (uint32_t)r[2] << 24 | (uint32_t)r[3] << 16 |
		(uint32_t)r[4] << 8 | r[5];
	m->marker = 1;
	m->live = 1;
	return 0;
}

/* MEDIA_OPEN_ACK from dect-stubd: start the flow, then tell the controller. */
static void media_acked(struct link *l, const struct dect_cell_frame *f,
			uint64_t now)
{
	uint32_t id;
	uint16_t st;
	struct media *m;

	if (dect_cell_tlv_check(f->body, f->len) ||
	    dect_cell_tlv_get_u32(f->body, f->len, 1, &id) ||
	    dect_cell_tlv_get_u16(f->body, f->len, 2, &st) ||
	    !(m = media_of(l, id)) || m->live)
		return;
	if (st == 0 && media_start(m)) {
		uint8_t b[8];
		struct dect_cell_tlv t;

		logf_("media %u: no UDP flow or audio socket: %s", id,
		      strerror(errno));
		dect_cell_tlv_init(&t, b, sizeof(b));
		dect_cell_tlv_u32(&t, 1, id);
		(void)stubd_send(l, DECT_CELL_MEDIA_CLOSE, 0, b, t.len);
		st = 4;
	}
	if (st) {
		media_free(m);
	} else {
		logf_("media %u up: %s, %u ms, %s", id,
		      m->fmt == DECT_CELL_FMT_G722 ? "G.722" : "L16 8 kHz",
		      m->ptime, m->profile == DECT_SRTP_AES_CM_SHA1_80 ?
		      "SRTP AES_CM_128_HMAC_SHA1_80" : m->profile ?
		      "SRTP AEAD_AES_128_GCM" : "plain RTP");
	}
	media_ack(l, f->corr, id, st, now);
}

/* MEDIA_CLOSE from the controller: dect-stubd answers MEDIA_CLOSED. */
static void media_close(struct link *l, const struct dect_cell_frame *f)
{
	uint32_t id;
	struct media *m;

	if (!dect_cell_tlv_check(f->body, f->len) &&
	    !dect_cell_tlv_get_u32(f->body, f->len, 1, &id) &&
	    (m = media_of(l, id)))
		media_free(m);
	(void)stubd_send(l, f->type, f->corr, f->body, f->len);
}

/* RTP header (RFC 3550 §5.1) for the next packet up. */
static void rtp_header(struct media *m, uint8_t *p, uint32_t step)
{
	p[0] = 0x80;
	p[1] = (uint8_t)(m->pt | (m->marker ? 0x80u : 0u));
	p[2] = (uint8_t)(m->seq >> 8);
	p[3] = (uint8_t)m->seq;
	p[4] = (uint8_t)(m->ts >> 24);
	p[5] = (uint8_t)(m->ts >> 16);
	p[6] = (uint8_t)(m->ts >> 8);
	p[7] = (uint8_t)m->ts;
	p[8] = (uint8_t)(m->ssrc_up >> 24);
	p[9] = (uint8_t)(m->ssrc_up >> 16);
	p[10] = (uint8_t)(m->ssrc_up >> 8);
	p[11] = (uint8_t)m->ssrc_up;
	m->marker = 0;
	m->seq++;
	m->ts += step;
}

/*
 * 20 ms from dect-stubd, up to the controller: one packet, or two at a
 * ptime of 10. L16 goes in network order. Both formats count 8000 per
 * second on the RTP clock (G.722 too, RFC 3551 §4.5.2).
 */
static void media_up(struct media *m)
{
	uint8_t fr[320], p[RTP_MAX];
	size_t want = m->fmt == DECT_CELL_FMT_G722 ? 160u : 320u, part, k;
	ssize_t n = recv(m->audio, fr, sizeof(fr), MSG_DONTWAIT);
	unsigned i;

	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
		       errno != EINTR)) {
		close(m->audio);	/* dect-stubd closes it with the lane */
		m->audio = -1;
		return;
	}
	if (n != (ssize_t)want)
		return;
	if (m->fmt == DECT_CELL_FMT_L16_8K)
		for (k = 0; k < want; k += 2u) {
			uint16_t v;

			memcpy(&v, fr + k, 2);
			fr[k] = (uint8_t)(v >> 8);
			fr[k + 1u] = (uint8_t)v;
		}
	part = m->ptime == 10u ? want / 2u : want;
	for (i = 0; i < want / part; i++) {
		long len;

		rtp_header(m, p, m->ptime == 10u ? 80u : 160u);
		memcpy(p + 12, fr + i * part, part);
		len = (long)(12u + part);
		if (m->profile)
			len = dect_srtp_protect(&m->tx, p, (size_t)len, sizeof(p));
		if (len > 0 && send(m->udp, p, (size_t)len, MSG_DONTWAIT) == len)
			m->pkts_up++;
		else
			m->dropped++;
	}
}

/* A packet from the controller, down to dect-stubd in 20 ms frames. */
static void media_down(struct media *m)
{
	uint8_t p[RTP_MAX + 64], fr[320];
	size_t want = m->fmt == DECT_CELL_FMT_G722 ? 160u : 320u, part, h, k;
	ssize_t n = recv(m->udp, p, sizeof(p), MSG_DONTWAIT);
	long len = n;
	uint16_t seq;
	const uint8_t *pl;

	if (n <= 0)
		return;
	h = dect_rtp_header_len(p, (size_t)n);
	if (!h || (p[1] & 0x7Fu) != m->pt || ((uint32_t)p[8] << 24 |
	    (uint32_t)p[9] << 16 | (uint32_t)p[10] << 8 | p[11]) !=
	    m->ssrc_down) {
		m->dropped++;
		return;
	}
	if (m->profile)
		len = dect_srtp_unprotect(&m->rx, p, (size_t)n);
	part = m->ptime == 10u ? want / 2u : want;
	if (len < 0 || (size_t)len - h != part) {
		m->dropped++;		/* forged, replayed or the wrong size */
		return;
	}
	m->pkts_down++;
	seq = (uint16_t)(p[2] << 8 | p[3]);
	pl = p + h;
	if (m->ptime == 10u) {
		/* two halves in sequence make a frame; a gap starts over */
		if (!m->half_len || (uint16_t)(m->half_seq + 1u) != seq) {
			memcpy(m->half, pl, part);
			m->half_len = part;
			m->half_seq = seq;
			return;
		}
		memcpy(fr, m->half, part);
		memcpy(fr + part, pl, part);
		m->half_len = 0;
	} else {
		memcpy(fr, pl, part);
	}
	if (m->fmt == DECT_CELL_FMT_L16_8K)
		for (k = 0; k < want; k += 2u) {
			uint16_t v = (uint16_t)(fr[k] << 8 | fr[k + 1u]);

			memcpy(fr + k, &v, 2);
		}
	if (m->audio >= 0)
		(void)send(m->audio, fr, want, MSG_DONTWAIT | MSG_NOSIGNAL);
}

/* dect-stubd gone: its lanes with it, every session closed (reason 2). */
static void media_drop_all(struct link *l, uint64_t now)
{
	unsigned i;

	for (i = 0; i < MEDIA_MAX; i++)
		if (l->media[i].used) {
			uint32_t id = l->media[i].id;

			media_free(&l->media[i]);
			media_closed(l, id, 2, now);
		}
}

/* ---- renewal (§5.4) --------------------------------------------------- */

#define RENEW_RETRY_MS	3600000u	/* §5.4: every hour until it works */
#define RENEW_WAIT_MS	60000u		/* an answer within */

/*
 * Due at two thirds of the certificate's life. A clock before its start
 * is not set yet (no RTC, no NTP): nothing is due then.
 */
static int renew_due(const struct link *l)
{
	int64_t now = wall_s(), from = l->creds.not_before,
		to = l->creds.not_after;

	return to > from && now >= from && now < to &&
	       now >= from + (to - from) * 2 / 3;
}

static void renew_tick(struct link *l, uint64_t now)
{
	uint8_t csr[1024], b[1100];
	struct dect_cell_tlv t;
	int csr_n;

	if (l->renewing && now - l->renew_sent >= RENEW_WAIT_MS) {
		logf_("no answer to the certificate renewal; again in an hour");
		l->renewing = 0;
		mbedtls_platform_zeroize(l->renew_key, sizeof(l->renew_key));
		l->renew_next = now + RENEW_RETRY_MS;
	}
	if (l->renewing || now < l->renew_next || !renew_due(l))
		return;
	l->renew_next = now + RENEW_RETRY_MS;
	if (make_key_csr(l->renew_key, csr, sizeof(csr), &csr_n)) {
		logf_("cannot make a key for the renewal");
		return;
	}
	dect_cell_tlv_init(&t, b, sizeof(b));
	dect_cell_tlv_put(&t, 1, csr + sizeof(csr) - csr_n, (size_t)csr_n);
	ctl_send(l, DECT_CELL_RENEW_REQUEST, 0x5244, b, t.len, now);
	l->renewing = 1;
	l->renew_sent = now;
	logf_("certificate renewal asked (it expires in %lld days)",
	      (long long)((l->creds.not_after - wall_s()) / 86400));
}

/*
 * The cell URI in a certificate's SAN, "urn:opendectcell:cell:<uuid>", into
 * out; 0, or -1 if it has none.
 */
static int cell_uri(const mbedtls_x509_crt *c, char *out, size_t cap)
{
	const mbedtls_x509_sequence *san;

	for (san = &c->subject_alt_names; san && san->buf.p; san = san->next) {
		mbedtls_x509_subject_alternative_name sn;
		int hit;

		if (mbedtls_x509_parse_subject_alt_name(&san->buf, &sn))
			continue;
		hit = sn.type == MBEDTLS_X509_SAN_UNIFORM_RESOURCE_IDENTIFIER &&
		      sn.san.unstructured_name.len > 17u &&
		      sn.san.unstructured_name.len < cap &&
		      !memcmp(sn.san.unstructured_name.p, "urn:opendectcell:cell:",
			      17);
		if (hit) {
			memcpy(out, sn.san.unstructured_name.p,
			       sn.san.unstructured_name.len);
			out[sn.san.unstructured_name.len] = 0;
		}
		mbedtls_x509_free_subject_alt_name(&sn);
		if (hit)
			return 0;
	}
	return -1;
}

/*
 * RENEW_RESPONSE: a certificate for the new key, from our CA, for this
 * cell, for clients. Then the new pair replaces the old on disk (see
 * creds_load() for a cut short) and serves from the next connection on.
 */
static void renew_response(struct link *l, const struct dect_cell_frame *f)
{
	const uint8_t *cert;
	size_t cn = 0;
	uint16_t st = 0xFFFF;
	mbedtls_x509_crt crt, cur, ca;
	uint8_t cab[2048];
	long can;
	uint32_t flags = 0;
	char uri_new[80], uri_cur[80], np[256], p[256];
	int ok;

	if (!l->renewing)
		return;
	l->renewing = 0;
	if (dect_cell_tlv_check(f->body, f->len) ||
	    dect_cell_tlv_get_u16(f->body, f->len, 1, &st) || st ||
	    !(cert = dect_cell_tlv_get(f->body, f->len, 2, &cn))) {
		logf_("certificate renewal refused (status %u); again in an "
		      "hour", st);
		mbedtls_platform_zeroize(l->renew_key, sizeof(l->renew_key));
		return;
	}
	mbedtls_x509_crt_init(&crt);
	mbedtls_x509_crt_init(&cur);
	mbedtls_x509_crt_init(&ca);
	can = file_get("ca.der", cab, sizeof(cab));
	ok = can > 0 && cn < sizeof(l->creds.cert) &&
	     !mbedtls_x509_crt_parse_der(&crt, cert, cn) &&
	     !mbedtls_x509_crt_parse_der(&cur, l->creds.cert,
					 l->creds.cert_n) &&
	     !mbedtls_x509_crt_parse_der(&ca, cab, (size_t)can) &&
	     !mbedtls_x509_crt_verify(&crt, &ca, NULL, NULL, &flags, NULL,
				      NULL) &&
	     !mbedtls_x509_crt_check_extended_key_usage(&crt,
		MBEDTLS_OID_CLIENT_AUTH,
		MBEDTLS_OID_SIZE(MBEDTLS_OID_CLIENT_AUTH)) &&
	     !cell_uri(&crt, uri_new, sizeof(uri_new)) &&
	     !cell_uri(&cur, uri_cur, sizeof(uri_cur)) &&
	     !strcmp(uri_new, uri_cur);
	mbedtls_x509_crt_free(&crt);
	mbedtls_x509_crt_free(&cur);
	mbedtls_x509_crt_free(&ca);
	if (!ok || pair_ok(cert, cn, (const char *)l->renew_key, NULL, NULL)) {
		logf_("the renewed certificate does not fit (flags %08x): "
		      "kept the old one", flags);
		mbedtls_platform_zeroize(l->renew_key, sizeof(l->renew_key));
		return;
	}
	ok = !file_put("cert.der.new", cert, cn, 0600) &&
	     !file_put("key.pem.new", l->renew_key,
		       strlen((const char *)l->renew_key), 0600);
	mbedtls_platform_zeroize(l->renew_key, sizeof(l->renew_key));
	path_of(np, sizeof(np), "cert.der.new");
	path_of(p, sizeof(p), "cert.der");
	ok = ok && !rename(np, p);
	path_of(np, sizeof(np), "key.pem.new");
	path_of(p, sizeof(p), "key.pem");
	ok = ok && !rename(np, p);
	if (!ok || creds_load(&l->creds)) {
		logf_("cannot store the renewed certificate in %s", dir);
		return;
	}
	l->renew_next = 0;
	logf_("certificate renewed: valid until %lld (Unix time), used from "
	      "the next connection", (long long)l->creds.not_after);
	link_state_write(l);
}

/* One STATS counter, tag 1 {id u16, value u64} (§10). */
static void stats_put(struct dect_cell_tlv *t, uint16_t id, uint64_t v)
{
	uint8_t b[10];
	unsigned j;

	b[0] = (uint8_t)(id >> 8);
	b[1] = (uint8_t)id;
	for (j = 0; j < 8u; j++)
		b[2 + j] = (uint8_t)(v >> (56u - 8u * j));
	dect_cell_tlv_put(t, 1, b, sizeof(b));
}

/*
 * STATS to the controller: dect-stubd's counters (body, may be empty when
 * it is not there) and this process's own, the reconnects (§10 id 6).
 */
static void stats_send(struct link *l, uint16_t corr, const uint8_t *body,
		       size_t len, uint64_t now)
{
	uint8_t b[DECT_CELL_FRAME_MAX];
	struct dect_cell_tlv t;

	if (len > sizeof(b) - 16u)
		len = 0;
	if (len)
		memcpy(b, body, len);
	dect_cell_tlv_init(&t, b + len, sizeof(b) - len);
	stats_put(&t, 6, l->reconnects);
	ctl_send(l, DECT_CELL_STATS, corr, b, len + t.len, now);
}

/* A frame from the controller. */
static void from_controller(struct link *l, const struct dect_cell_frame *f,
			    uint64_t now)
{
	size_t max = dect_cell_body_max(f->type);

	l->last_rx = now;
	if (!max && f->type != DECT_CELL_STATUS_REQUEST &&
	    f->type != DECT_CELL_STATS_REQUEST) {
		ctl_error(l, DECT_CELL_ERR_UNKNOWN_TYPE, f->type, f->corr, now);
		return;
	}
	if (f->len > max) {
		ctl_error(l, DECT_CELL_ERR_TOO_LARGE, f->type, f->corr, now);
		return;
	}
	switch (f->type) {
	case DECT_CELL_HEARTBEAT:
		ctl_send(l, DECT_CELL_HEARTBEAT_ECHO, f->corr, f->body, f->len,
			 now);
		break;
	case DECT_CELL_HEARTBEAT_ECHO:
		if (f->len == 4u)
			l->rtt_ms = (uint32_t)now - ((uint32_t)f->body[0] << 24 |
				    (uint32_t)f->body[1] << 16 |
				    (uint32_t)f->body[2] << 8 | f->body[3]);
		break;
	case DECT_CELL_STATS_REQUEST:
		if (l->stubd < 0) {		/* our own counters only */
			stats_send(l, f->corr, NULL, 0, now);
			break;
		}
		(void)stubd_send(l, f->type, f->corr, f->body, f->len);
		break;
	case DECT_CELL_MAIL_DOWN:
	case DECT_CELL_MEDIA_OPEN:
	case DECT_CELL_MEDIA_CLOSE:
		/* §5.3: nothing that touches the radio unless active */
		if (l->cell_state != DECT_CELL_ST_ACTIVE) {
			ctl_error(l, DECT_CELL_ERR_NOT_ALLOWED, f->type,
				  f->corr, now);
			break;
		}
		if (f->type == DECT_CELL_MEDIA_OPEN) {
			media_open(l, f, now);
			break;
		}
		if (f->type == DECT_CELL_MEDIA_CLOSE) {
			media_close(l, f);
			break;
		}
		/* fall through */
	case DECT_CELL_STATUS_REQUEST:
		if (stubd_send(l, f->type, f->corr, f->body, f->len) &&
		    f->type == DECT_CELL_MAIL_DOWN && f->corr) {
			uint8_t b[3] = { (uint8_t)(f->corr >> 8),
					 (uint8_t)f->corr,
					 l->stubd < 0 ? DECT_CELL_REJ_CHIP_DOWN :
					 DECT_CELL_REJ_QUEUE_FULL };

			ctl_send(l, DECT_CELL_MAIL_REJECT, 0, b, 3, now);
		}
		break;
	case DECT_CELL_CELL_STATE: {
		uint8_t st;

		if (dect_cell_tlv_check(f->body, f->len) ||
		    dect_cell_tlv_get_u8(f->body, f->len, 1, &st)) {
			ctl_error(l, DECT_CELL_ERR_MALFORMED, f->type, f->corr,
				  now);
			break;
		}
		l->cell_state = st;
		/*
		 * the state alone: a reason in tag 2 is ours to give
		 * ("link-lost", §14.2), never the controller's
		 */
		stubd_cell_state(l, st, NULL);
		logf_("the controller set the cell %s",
		      st == DECT_CELL_ST_ACTIVE ? "active" :
		      st == DECT_CELL_ST_PENDING ? "pending" :
		      st == DECT_CELL_ST_DISABLED ? "disabled" : "revoked");
		link_state_write(l);
		break;
	}
	case DECT_CELL_CONFIG_SET: {
		/* nothing of §10 is applied yet: "partially applied" */
		uint8_t b[8];
		struct dect_cell_tlv t;

		dect_cell_tlv_init(&t, b, sizeof(b));
		dect_cell_tlv_u16(&t, 1, 1);
		ctl_send(l, DECT_CELL_CONFIG_ACK, f->corr, b, t.len, now);
		break;
	}
	case DECT_CELL_GOODBYE: {
		uint16_t why = 0;

		if (!dect_cell_tlv_check(f->body, f->len))
			(void)dect_cell_tlv_get_u16(f->body, f->len, 1, &why);
		if (why == DECT_CELL_BYE_REVOKED) {
			(void)forget();
			l->enrolled = 0;
			stubd_cell_state(l, DECT_CELL_ST_REVOKED, NULL);
		}
		link_down(l, why == DECT_CELL_BYE_REVOKED ? "revoked" :
			  "goodbye", now);
		if (why == DECT_CELL_BYE_VERSION || why == DECT_CELL_BYE_AUTH ||
		    why == DECT_CELL_BYE_REVOKED) {
			l->next_try = UINT64_MAX;
			set_state(l, why == DECT_CELL_BYE_REVOKED ? "revoked" :
				  why == DECT_CELL_BYE_VERSION ?
				  "stopped: protocol version" :
				  "stopped: certificate refused");
		} else if (why == DECT_CELL_BYE_OVERLOAD) {
			l->next_try = now;
		}
		break;
	}
	case DECT_CELL_RENEW_RESPONSE:
		renew_response(l, f);
		break;
	case DECT_CELL_ERROR: {
		uint16_t code = 0, rt = 0;

		if (!dect_cell_tlv_check(f->body, f->len)) {
			(void)dect_cell_tlv_get_u16(f->body, f->len, 1, &code);
			(void)dect_cell_tlv_get_u16(f->body, f->len, 2, &rt);
		}
		logf_("the controller reports error %u on type 0x%04x", code, rt);
		break;
	}
	default:
		/* renewal, trust, stats: not negotiated by this version */
		ctl_error(l, DECT_CELL_ERR_UNSUPPORTED, f->type, f->corr, now);
	}
}

/* A frame from dect-stubd. */
static void from_stubd(struct link *l, const uint8_t *buf, size_t n,
		       uint64_t now)
{
	struct dect_cell_frame f;

	if (dect_cell_parse(buf, n, &f) != (long)n)
		return;
	if (f.type == DECT_CELL_STATUS && !dect_cell_tlv_check(f.body, f.len)) {
		size_t dn = 0;
		const uint8_t *d = dect_cell_tlv_get(f.body, f.len, 5, &dn);

		(void)dect_cell_tlv_get_u8(f.body, f.len, 1, &l->radio);
		(void)dect_cell_tlv_get_u8(f.body, f.len, 2, &l->chip);
		(void)dect_cell_tlv_get_u32(f.body, f.len, 3, &l->epoch);
		if (d && dn < sizeof(l->stub)) {
			memcpy(l->stub, d, dn);
			l->stub[dn] = 0;
		}
		l->have_status = 1;
	}
	/* §8: the flow starts and ends here, whatever the link does */
	if (f.type == DECT_CELL_MEDIA_OPEN_ACK) {
		media_acked(l, &f, now);
		return;
	}
	if (f.type == DECT_CELL_MEDIA_CLOSED) {
		uint32_t id;
		struct media *m = NULL;

		if (!dect_cell_tlv_check(f.body, f.len) &&
		    !dect_cell_tlv_get_u32(f.body, f.len, 1, &id) &&
		    (m = media_of(l, id)))
			media_free(m);
		/* one we hold, or the answer to a MEDIA_CLOSE */
		if (l->tls.up && (m || f.corr))
			ctl_send(l, f.type, f.corr, f.body, f.len, now);
		return;
	}
	if (!l->tls.up) {
		l->dropped++;
		return;
	}
	/* §5.3: nothing that touches the radio while not active */
	if (f.type == DECT_CELL_MAIL_UP && l->cell_state != DECT_CELL_ST_ACTIVE)
		return;
	if (f.type == DECT_CELL_STATS) {	/* ours added */
		stats_send(l, f.corr, f.body, f.len, now);
		return;
	}
	ctl_send(l, f.type, f.corr, f.body, f.len, now);
}

static void stubd_connect(struct link *l)
{
	struct sockaddr_un a;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", stubd_path);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a))) {
		close(fd);
		return;
	}
	(void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	l->stubd = fd;
	logf_("connected to dect-stubd");
	if (l->tls.up)
		stubd_cell_state(l, l->cell_state, NULL);
	link_state_write(l);
}

static int run(void)
{
	static struct link l;
	uint64_t creds_at = 0, stubd_at = 0;

	memset(&l, 0, sizeof(l));
	l.stubd = -1;
	l.state = "starting";
	boot_id(l.boot_id);
	srand((unsigned)mono_ms());
	while (!stop) {
		uint64_t now = mono_ms();
		struct pollfd p[2u + 2u * MEDIA_MAX];
		struct media *pm[2u + 2u * MEDIA_MAX] = { NULL };
		int np = 0, i;
		unsigned k;

		if (l.stubd < 0 && now >= stubd_at) {
			stubd_connect(&l);
			stubd_at = now + 1000u;
		}
		if (!l.enrolled && now >= creds_at) {
			l.enrolled = creds_load(&l.creds) == 0;
			creds_at = now + CREDS_POLL_MS;
			if (l.enrolled)
				l.next_try = now;
			else
				set_state(&l, "not enrolled");
		}
		/* the chip first: HELLO carries its build and epoch */
		if (l.enrolled && !l.tls.up && l.have_status &&
		    now >= l.next_try) {
			(void)link_up(&l);
			now = mono_ms();	/* it may have taken seconds */
		}
		if (l.tls.up) {
			if (now - l.last_rx >= l.dead_ms)
				link_down(&l, "nothing heard", now);
			else if (now - l.last_tx >= l.heartbeat_ms) {
				uint8_t b[4] = { (uint8_t)(now >> 24),
						 (uint8_t)(now >> 16),
						 (uint8_t)(now >> 8),
						 (uint8_t)now };

				ctl_send(&l, DECT_CELL_HEARTBEAT, 0, b, 4, now);
			}
			if (l.tls.up)
				renew_tick(&l, now);
			/* §10: STATS every stats_interval_s, dect-stubd's first */
			if (l.tls.up && now >= l.next_stats) {
				l.next_stats = now + STATS_INTERVAL_MS;
				if (l.stubd >= 0)
					(void)stubd_send(&l, DECT_CELL_STATS_REQUEST,
						   0, NULL, 0);
				else
					stats_send(&l, 0, NULL, 0, now);
			}
		}
		if (l.stubd >= 0)
			p[np++] = (struct pollfd){ l.stubd, POLLIN, 0 };
		if (l.tls.up)
			p[np++] = (struct pollfd){ l.tls.net.fd, POLLIN, 0 };
		for (k = 0; k < MEDIA_MAX; k++) {
			struct media *m = &l.media[k];

			if (!m->live)
				continue;
			if (m->audio >= 0) {
				pm[np] = m;
				p[np++] = (struct pollfd){ m->audio, POLLIN, 0 };
			}
			pm[np] = m;
			p[np++] = (struct pollfd){ m->udp, POLLIN, 0 };
		}
		/* TLS may hold a record already read: do not wait then */
		if (poll(p, (nfds_t)np, l.tls.up &&
			 mbedtls_ssl_get_bytes_avail(&l.tls.ssl) ? 0 : 100) < 0 &&
		    errno != EINTR)
			break;
		now = mono_ms();
		for (i = 0; i < np; i++) {
			if (p[i].fd == l.stubd && p[i].revents) {
				uint8_t buf[DECT_CELL_FRAME_MAX];
				ssize_t n;

				while ((n = recv(l.stubd, buf, sizeof(buf),
						 MSG_DONTWAIT)) > 0)
					from_stubd(&l, buf, (size_t)n, now);
				if (n == 0 || (n < 0 && errno != EAGAIN &&
					       errno != EWOULDBLOCK)) {
					close(l.stubd);
					l.stubd = -1;
					l.have_status = 0;
					logf_("dect-stubd went away");
					media_drop_all(&l, now);
					link_state_write(&l);
				}
			}
			/* a session; one closed above has its fds at -1 */
			if (pm[i] && p[i].revents && pm[i]->live) {
				if (p[i].fd == pm[i]->audio)
					media_up(pm[i]);
				else if (p[i].fd == pm[i]->udp)
					media_down(pm[i]);
			}
		}
		if (l.tls.up) {
			long n = tls_recv(&l.tls, l.rx, l.rx_len, sizeof(l.rx), 0);
			struct dect_cell_frame f;
			long used = 0;

			if (n < 0) {
				link_down(&l, "closed by the controller", now);
				continue;
			}
			l.rx_len = (size_t)n;
			while (l.tls.up &&
			       (used = dect_cell_parse(l.rx, l.rx_len, &f)) > 0) {
				from_controller(&l, &f, now);
				/*
				 * a GOODBYE or a failed write took the link
				 * down and emptied rx: nothing to move
				 * (review D2, finding 1)
				 */
				if (!l.tls.up)
					break;
				memmove(l.rx, l.rx + used, l.rx_len - (size_t)used);
				l.rx_len -= (size_t)used;
			}
			if (used < 0) {
				uint8_t b[8];
				struct dect_cell_tlv t;

				dect_cell_tlv_init(&t, b, sizeof(b));
				dect_cell_tlv_u16(&t, 1, DECT_CELL_BYE_PROTOCOL);
				(void)tls_send(&l.tls, DECT_CELL_GOODBYE, 0, b,
					       t.len);
				link_down(&l, "framing lost", now);
			}
		}
	}
	if (l.tls.up) {
		uint8_t b[8];
		struct dect_cell_tlv t;

		dect_cell_tlv_init(&t, b, sizeof(b));
		dect_cell_tlv_u16(&t, 1, DECT_CELL_BYE_SHUTDOWN);
		(void)tls_send(&l.tls, DECT_CELL_GOODBYE, 0, b, t.len);
		tls_close(&l.tls);
	}
	if (l.stubd >= 0) {
		stubd_cell_state(&l, l.cell_state, "link-lost");
		close(l.stubd);
	}
	(void)unlink(LINK_STATE);
	return 0;
}

/*
 * The enrollment string: STRING itself, @FILE or @- (standard input). The
 * last two keep its one-time token off the command line, where every local
 * user reads it in /proc while the enrolment runs.
 */
static int enroll_text(const char *arg, char *out, size_t cap)
{
	FILE *f;
	size_t n;

	if (arg[0] != '@') {
		if (strlen(arg) >= cap)
			goto big;
		memcpy(out, arg, strlen(arg) + 1u);
		return 0;
	}
	f = !strcmp(arg + 1, "-") ? stdin : fopen(arg + 1, "r");
	if (!f) {
		perror(arg + 1);
		return -1;
	}
	n = fread(out, 1, cap - 1u, f);
	if (f != stdin)
		fclose(f);
	if (n >= cap - 1u)
		goto big;
	while (n && (out[n - 1u] == '\n' || out[n - 1u] == '\r' ||
		     out[n - 1u] == ' '))
		n--;
	out[n] = '\0';
	return 0;
big:
	fprintf(stderr, "the enrollment string is too long\n");
	return -1;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: dect-cell [-d DIR] [-s SOCKET] [-a SOCKET] [-P] [-v] COMMAND\n"
		"  enroll STRING  enrol with a controller: STRING is its\n"
		"                 opendectcell-enroll:v1?... enrollment string,\n"
		"                 or @FILE / @- (standard input) holding it:\n"
		"                 on the command line, local users can read it\n"
		"  run            keep the link to the controller and relay\n"
		"                 frames to dect-stubd -K\n"
		"  forget         erase this cell's key and certificate\n"
		"  -d DIR     credentials (default " CELL_DIR ")\n"
		"  -s SOCKET  dect-stubd's socket (default " STUBD_SOCK ")\n"
		"  -a SOCKET  dect-stubd's audio socket (default " AUDIO_SOCK ")\n"
		"  -P         allow plain RTP, unencrypted, for media sessions\n"
		"             (an admin's choice: trusted networks only)\n");
}

int main(int argc, char **argv)
{
	int c;

	while ((c = getopt(argc, argv, "d:s:a:Pvh")) != -1) {
		switch (c) {
		case 'd': dir = optarg; break;
		case 's': stubd_path = optarg; break;
		case 'a': audio_path = optarg; break;
		case 'P': allow_plain = 1; break;
		case 'v': verbose = 1; break;
		default: usage(); return c == 'h' ? 0 : 2;
		}
	}
	if (optind >= argc) {
		usage();
		return 2;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);
	if (!strcmp(argv[optind], "forget"))
		return forget();
	if (rng_init()) {
		fprintf(stderr, "no random numbers\n");
		return 1;
	}
	if (!strcmp(argv[optind], "enroll") && optind + 1 < argc) {
		char text[1024];

		if (enroll_text(argv[optind + 1], text, sizeof(text)))
			return 2;
		return enroll(text);
	}
	if (!strcmp(argv[optind], "run"))
		return run();
	usage();
	return 2;
}
