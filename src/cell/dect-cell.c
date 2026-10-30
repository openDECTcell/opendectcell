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
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>
#include <psa/crypto.h>

#include "dect_cell.h"

#ifndef OPENDECTCELL_VERSION
#define OPENDECTCELL_VERSION "dev"
#endif

#define CELL_DIR	"/etc/opendectcell/cell"
#define STUBD_SOCK	"/var/run/opendectcell-cell.sock"
#define LINK_STATE	"/var/run/opendectcell/cell-link.json"
#define CONNECT_MS	5000u		/* TCP + TLS + HELLO_ACK */
#define BACKOFF_MAX_MS	30000u		/* §6.4 */
#define BACKOFF_RESET_MS 60000u
#define CREDS_POLL_MS	10000u		/* not enrolled: look again */
#define RX_BUF		(2u * DECT_CELL_FRAME_MAX)

static const char *dir = CELL_DIR;
static const char *stubd_path = STUBD_SOCK;
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
	if ((r = mbedtls_net_connect(&t->net, host, port,
				     MBEDTLS_NET_PROTO_TCP))) {
		logf_("cannot reach %s (%d)", addr, r);
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
	mbedtls_ssl_set_bio(&t->ssl, &t->net, mbedtls_net_send, NULL,
			    mbedtls_net_recv_timeout);
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
			return -1;		/* a peer that never frames */
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

static int enroll(const char *s)
{
	char addr[200], tok[64], pin[64][48], hint[64], sw[64], hw[64];
	uint8_t token[20], csr_buf[1024], body[2048], rx[RX_BUF];
	unsigned char key_pem[512];
	mbedtls_pk_context key;
	mbedtls_x509write_csr csr;
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
	/* §5.2: a P-256 key made here, which never leaves the cell */
	mbedtls_pk_init(&key);
	mbedtls_x509write_csr_init(&csr);
	if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) ||
	    mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
				mbedtls_ctr_drbg_random, &drbg) ||
	    mbedtls_pk_write_key_pem(&key, key_pem, sizeof(key_pem))) {
		fprintf(stderr, "cannot make a key\n");
		goto out;
	}
	mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
	mbedtls_x509write_csr_set_key(&csr, &key);
	if (mbedtls_x509write_csr_set_subject_name(&csr, "CN=opendectcell-cell")) {
		fprintf(stderr, "cannot make a request\n");
		goto out;
	}
	csr_n = mbedtls_x509write_csr_der(&csr, csr_buf, sizeof(csr_buf),
					  mbedtls_ctr_drbg_random, &drbg);
	if (csr_n <= 0) {
		fprintf(stderr, "cannot make a request\n");
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
	mbedtls_x509write_csr_free(&csr);
	mbedtls_pk_free(&key);
	mbedtls_platform_zeroize(key_pem, sizeof(key_pem));
	return rc;
}

static int forget(void)
{
	static const char *const names[] = { "key.pem", "cert.der", "ca.der",
					     "enrollment" };
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
	uint8_t cert[2048];
	size_t cert_n;
	char cell_id[33];
};

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
		"\"dect_stubd\":%s,\"dropped\":%u}\n", l->state,
		l->enrolled ? l->creds.addr : "", l->enrolled ?
		l->creds.cell_id : "",
		!l->tls.up ? "" : l->cell_state == DECT_CELL_ST_ACTIVE ?
		"active" : l->cell_state == DECT_CELL_ST_PENDING ? "pending" :
		l->cell_state == DECT_CELL_ST_DISABLED ? "disabled" : "revoked",
		l->rtt_ms, l->reconnects, l->stubd >= 0 ? "true" : "false",
		l->dropped);
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

static void stubd_send(struct link *l, uint16_t type, uint16_t corr,
		       const uint8_t *body, size_t len)
{
	uint8_t f[DECT_CELL_FRAME_MAX];
	size_t n = dect_cell_put(f, sizeof(f), type, corr, body, len);

	if (l->stubd >= 0 && n)
		(void)send(l->stubd, f, n, MSG_DONTWAIT | MSG_NOSIGNAL);
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
	stubd_send(l, DECT_CELL_CELL_STATE, 0, b, t.len);
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
	/* media is not built yet (§14.3): the mail tunnel and FLOW only */
	dect_cell_tlv_u32(&t, 7, DECT_CELL_CAP_MAIL | DECT_CELL_CAP_FLOW |
			  DECT_CELL_CAP_STATS | DECT_CELL_CAP_RESUME);
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
	l->last_rx = l->last_tx = l->up_since = mono_ms();
	stubd_cell_state(l, st, NULL);
	set_state(l, "up");
	if (resumed && l->stubd >= 0)	/* §6.4: the snapshot comes next */
		stubd_send(l, DECT_CELL_STATE_SNAPSHOT, 0, NULL, 0);
	logf_("controller link %s to %s, cell %s",
	      resumed ? "resumed" : "up", l->creds.addr,
	      st == DECT_CELL_ST_ACTIVE ? "active" :
	      st == DECT_CELL_ST_PENDING ? "pending approval" : "disabled");
	return 0;
fail:
	tls_close(&l->tls);
	l->reconnects++;
	if (l->next_try != UINT64_MAX)
		backoff(l, mono_ms());
	set_state(l, l->enrolled ? "connecting" : "not enrolled");
	return -1;
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
		stubd_send(l, f->type, f->corr, f->body, f->len);
		break;
	case DECT_CELL_MAIL_DOWN:
	case DECT_CELL_MEDIA_OPEN:
	case DECT_CELL_MEDIA_CLOSE:
	case DECT_CELL_STATUS_REQUEST:
		if (l->stubd < 0) {
			if (f->type == DECT_CELL_MAIL_DOWN && f->corr) {
				uint8_t b[3] = { (uint8_t)(f->corr >> 8),
						 (uint8_t)f->corr,
						 DECT_CELL_REJ_CHIP_DOWN };

				ctl_send(l, DECT_CELL_MAIL_REJECT, 0, b, 3, now);
			}
			break;
		}
		stubd_send(l, f->type, f->corr, f->body, f->len);
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
		stubd_send(l, f->type, f->corr, f->body, f->len);
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
		struct pollfd p[2];
		int np = 0, i;

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
			/* §10: STATS every stats_interval_s, dect-stubd's first */
			if (l.tls.up && now >= l.next_stats) {
				l.next_stats = now + STATS_INTERVAL_MS;
				if (l.stubd >= 0)
					stubd_send(&l, DECT_CELL_STATS_REQUEST,
						   0, NULL, 0);
				else
					stats_send(&l, 0, NULL, 0, now);
			}
		}
		if (l.stubd >= 0)
			p[np++] = (struct pollfd){ l.stubd, POLLIN, 0 };
		if (l.tls.up)
			p[np++] = (struct pollfd){ l.tls.net.fd, POLLIN, 0 };
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
					link_state_write(&l);
				}
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

static void usage(void)
{
	fprintf(stderr,
		"usage: dect-cell [-d DIR] [-s SOCKET] [-v] COMMAND\n"
		"  enroll STRING  enrol with a controller: STRING is its\n"
		"                 opendectcell-enroll:v1?... enrollment string\n"
		"  run            keep the link to the controller and relay\n"
		"                 frames to dect-stubd -K\n"
		"  forget         erase this cell's key and certificate\n"
		"  -d DIR     credentials (default " CELL_DIR ")\n"
		"  -s SOCKET  dect-stubd's socket (default " STUBD_SOCK ")\n");
}

int main(int argc, char **argv)
{
	int c;

	while ((c = getopt(argc, argv, "d:s:vh")) != -1) {
		switch (c) {
		case 'd': dir = optarg; break;
		case 's': stubd_path = optarg; break;
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
	if (!strcmp(argv[optind], "enroll") && optind + 1 < argc)
		return enroll(argv[optind + 1]);
	if (!strcmp(argv[optind], "run"))
		return run();
	usage();
	return 2;
}
