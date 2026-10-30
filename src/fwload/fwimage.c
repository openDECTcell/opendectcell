// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * STUB upload images. Written from docs/stub-image-format.md; section
 * references below point there.
 */
#include "fwimage.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ---- families (doc §1) ------------------------------------------------ */

static const struct fwi_family families[] = {
	{ "441",     0x10080,    0x0,        0 },
	{ "488",     0x10080,    0xF0000,    0 },
	{ "DA14495", 0x20000000, 0x200000C0, 50 },
	{ "DCX81",   0xA0000,    0x400000,   50 },
};

const struct fwi_family *fwi_family_from_name(const char *name)
{
	for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++)
		if (name && strstr(name, families[i].name))
			return &families[i];
	return &families[1];
}

/* ---- helpers ---------------------------------------------------------- */

static uint32_t get_be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[1] << 8 | p[0];
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

void fwi_image_free(struct fwi_image *img)
{
	free(img->data);
	img->data = NULL;
	img->len = 0;
}

/* ---- Intel HEX reader ------------------------------------------------- */

enum { REC_DATA = 0, REC_EOF = 1, REC_EXT_SEG = 2, REC_START_SEG = 3,
       REC_EXT_LIN = 4, REC_START_LIN = 5 };

struct hex_record {
	uint8_t type;
	uint8_t count;
	uint16_t addr;
	uint8_t data[255];
};

/*
 * Read the next record from *pos. Return 1 for a record, 0 at the end of
 * the text, -EINVAL for a malformed line or a wrong record checksum.
 */
static int hex_next(const char *hex, size_t len, size_t *pos,
		    struct hex_record *rec)
{
	size_t i = *pos;
	uint8_t raw[5 + 255];
	unsigned sum = 0;

	while (i < len && (hex[i] == '\r' || hex[i] == '\n' || hex[i] == ' ' ||
			   hex[i] == '\t'))
		i++;
	if (i >= len)
		return 0;
	if (hex[i++] != ':')
		return -EINVAL;

	/* count, address (2), type, data[count], checksum */
	size_t want = 5;
	for (size_t n = 0; n < want; n++) {
		if (i + 1 >= len)
			return -EINVAL;
		int hi = hexval(hex[i]), lo = hexval(hex[i + 1]);
		if (hi < 0 || lo < 0)
			return -EINVAL;
		raw[n] = (uint8_t)(hi << 4 | lo);
		sum += raw[n];
		i += 2;
		if (n == 0)
			want = 5 + raw[0];
	}
	if ((sum & 0xFF) != 0)
		return -EINVAL;

	rec->count = raw[0];
	rec->addr = (uint16_t)(raw[1] << 8 | raw[2]);
	rec->type = raw[3];
	memcpy(rec->data, raw + 4, rec->count);
	*pos = i;
	return 1;
}

/* Extended-address records update the base (doc §3). */
static int apply_ext(const struct hex_record *rec, uint32_t *base)
{
	if (rec->count != 2)
		return -EINVAL;
	uint32_t v = (uint32_t)rec->data[0] << 8 | rec->data[1];
	*base = rec->type == REC_EXT_SEG ? v << 4 : v << 16;
	return 0;
}

/* Growable byte buffer for image assembly. */
struct buf {
	uint8_t *p;
	size_t len, cap;
};

static int buf_reserve(struct buf *b, size_t need)
{
	if (need <= b->cap)
		return 0;
	size_t cap = b->cap ? b->cap : 4096;
	while (cap < need)
		cap *= 2;
	uint8_t *p = realloc(b->p, cap);
	if (!p)
		return -ENOMEM;
	memset(p + b->cap, 0, cap - b->cap);
	b->p = p;
	b->cap = cap;
	return 0;
}

/* ---- first-level image (doc §2) -------------------------------------- */

int fwi_build_firstlevel(const char *hex, size_t hex_len, uint32_t load_addr,
			 struct fwi_image *out)
{
	struct buf b = { 0 };
	struct hex_record rec;
	uint32_t base = 0;
	size_t pos = 0, end = 0;
	int r, eof = 0;

	while (!eof && (r = hex_next(hex, hex_len, &pos, &rec)) > 0) {
		switch (rec.type) {
		case REC_DATA: {
			uint32_t a = base + rec.addr;
			if (a < load_addr) {
				r = -EINVAL;
				goto fail;
			}
			size_t off = a - load_addr;
			if (off + rec.count > FWI_MAX_DATA) {
				r = -EFBIG;
				goto fail;
			}
			/* 2-byte header in front; gaps stay zero */
			if ((r = buf_reserve(&b, 2 + off + rec.count + 1)))
				goto fail;
			memcpy(b.p + 2 + off, rec.data, rec.count);
			if (off + rec.count > end)
				end = off + rec.count;
			break;
		}
		case REC_EOF:
			eof = 1;
			break;
		case REC_EXT_SEG:
		case REC_EXT_LIN:
			if ((r = apply_ext(&rec, &base)))
				goto fail;
			break;
		case REC_START_SEG:
		case REC_START_LIN:
			break;
		default:
			r = -EINVAL;
			goto fail;
		}
	}
	if (r < 0 || !eof || end > 0xFFFF) {
		r = r < 0 ? r : (!eof ? -EINVAL : -EFBIG);
		goto fail;
	}
	if ((r = buf_reserve(&b, 2 + end + 1)))
		goto fail;

	uint8_t x = 0;
	for (size_t i = 0; i < end; i++)
		x ^= b.p[2 + i];
	b.p[0] = (uint8_t)end; /* little-endian length */
	b.p[1] = (uint8_t)(end >> 8);
	b.p[2 + end] = x;
	out->data = b.p;
	out->len = 2 + end + 1;
	return 0;
fail:
	free(b.p);
	return r;
}

/* ---- second-level image (doc §3) ------------------------------------- */

int fwi_build_secondlevel(const char *hex, size_t hex_len,
			  struct fwi_image *out)
{
	struct buf b = { 0 };
	struct hex_record rec;
	uint32_t base = 0, next_addr = 0;
	size_t pos = 0, seg = 0; /* offset of the open segment's header */
	size_t data_total = 0;
	int r, eof = 0, open = 0;

	/* room for the length header */
	if ((r = buf_reserve(&b, 4)))
		goto fail;
	b.len = 4;

	while (!eof && (r = hex_next(hex, hex_len, &pos, &rec)) > 0) {
		switch (rec.type) {
		case REC_DATA:
			/* compare the 16-bit address field only (doc §3) */
			if (!open || rec.addr != next_addr) {
				if ((r = buf_reserve(&b, b.len + 8)))
					goto fail;
				seg = b.len;
				put_be32(b.p + seg, base + rec.addr);
				put_be32(b.p + seg + 4, 0);
				b.len += 8;
				open = 1;
			}
			data_total += rec.count;
			if (data_total > FWI_MAX_DATA) {
				r = -EFBIG;
				goto fail;
			}
			if ((r = buf_reserve(&b, b.len + rec.count)))
				goto fail;
			memcpy(b.p + b.len, rec.data, rec.count);
			b.len += rec.count;
			put_be32(b.p + seg + 4,
				 get_be32(b.p + seg + 4) + rec.count);
			next_addr = (uint32_t)rec.addr + rec.count;
			break;
		case REC_EOF:
			eof = 1;
			open = 0;
			break;
		case REC_EXT_SEG:
		case REC_EXT_LIN:
			if ((r = apply_ext(&rec, &base)))
				goto fail;
			open = 0;
			break;
		case REC_START_SEG:
		case REC_START_LIN:
			break;
		default:
			r = -EINVAL;
			goto fail;
		}
	}
	if (r < 0 || !eof) {
		r = r < 0 ? r : -EINVAL;
		goto fail;
	}
	if ((r = buf_reserve(&b, b.len + 1)))
		goto fail;
	put_be32(b.p, (uint32_t)(b.len - 4));
	out->data = b.p;
	out->len = b.len + 1;
	fwi_update_checksum(out);
	return 0;
fail:
	free(b.p);
	return r;
}

uint32_t fwi_payload_len(const struct fwi_image *img)
{
	return img->len >= 5 ? get_be32(img->data) : 0;
}

/*
 * Walk the segments. With `count`, count them; otherwise return a pointer
 * to the first segment holding all of [addr, addr + size).
 */
static uint8_t *walk(const struct fwi_image *img, uint32_t addr, size_t size,
		     size_t *count)
{
	uint32_t plen = fwi_payload_len(img);
	uint8_t *p = img->data + 4;
	size_t left = plen;

	if (count)
		*count = 0;
	while (left >= 8) {
		uint32_t sa = get_be32(p), sl = get_be32(p + 4);
		if (sl > left - 8)
			break;
		if (count)
			(*count)++;
		else if (sa <= addr && size <= sl &&
			 (uint64_t)addr + size <= (uint64_t)sa + sl)
			return p + 8 + (addr - sa);
		p += 8 + sl;
		left -= 8 + sl;
	}
	return NULL;
}

size_t fwi_segment_count(const struct fwi_image *img)
{
	size_t n;

	walk(img, 0, 0, &n);
	return n;
}

uint8_t *fwi_locate(const struct fwi_image *img, uint32_t addr, size_t size)
{
	return walk(img, addr, size, NULL);
}

/* ---- patching (doc §4) ------------------------------------------------ */

int fwi_patch_area(const struct fwi_image *img, const struct fwi_family *fam,
		   uint8_t **area, size_t *size)
{
	uint32_t addr = fam->patch_addr;
	size_t sz = fam->fixed_patch_size;

	if (!sz) {
		const uint8_t *d = fwi_locate(img, fam->patch_addr, 6);
		if (!d)
			return -ENOENT;
		addr = get_le32(d);
		sz = (size_t)d[4] | (size_t)d[5] << 8;
	}
	*area = fwi_locate(img, addr, sz);
	if (!*area)
		return -ENOENT;
	*size = sz;
	return 0;
}

enum fwi_tlv_result fwi_tlv_replace(uint8_t *area, size_t size, uint8_t tag,
				    const uint8_t *val, uint8_t len)
{
	size_t i = 0;

	if (tag == 0)
		return FWI_TLV_SKIPPED;
	while (size - i >= 2 && area[i] != 0) {
		size_t room = size - i - 2;
		if (area[i] == tag) {
			if (len > room)
				return FWI_TLV_NO_SPACE;
			if (area[i + 1] != len)
				return FWI_TLV_SKIPPED;
			memcpy(area + i + 2, val, len);
			return FWI_TLV_REPLACED;
		}
		if (area[i + 1] > room)
			return FWI_TLV_SKIPPED; /* list runs past the area */
		i += 2 + area[i + 1];
	}
	return FWI_TLV_SKIPPED;
}

const uint8_t *fwi_tlv_find(const uint8_t *area, size_t size, uint8_t tag,
			    uint8_t *len)
{
	size_t i = 0;

	while (size - i >= 2 && area[i] != 0) {
		if (area[i + 1] > size - i - 2)
			return NULL;
		if (area[i] == tag) {
			*len = area[i + 1];
			return area + i + 2;
		}
		i += 2 + area[i + 1];
	}
	return NULL;
}

int fwi_rfpi_from_maca(const char *maca, uint8_t rfpi[FWI_RFPI_LEN])
{
	static const int pos[] = { 10, 12, 13, 15, 16 };
	int d[17];

	if (!maca || strlen(maca) < 17)
		return -EINVAL;
	for (size_t i = 0; i < 5; i++) {
		d[pos[i]] = hexval(maca[pos[i]]);
		if (d[pos[i]] < 0)
			return -EINVAL;
	}
	uint32_t v = 0x70B00000u | (uint32_t)d[16] << 3 |
		     (uint32_t)d[15] << 7 | (uint32_t)d[13] << 11 |
		     (uint32_t)d[12] << 15 | (((uint32_t)d[10] << 19) & 0x80000);
	rfpi[0] = 0x02;
	rfpi[1] = 0x70;
	rfpi[2] = (uint8_t)(v >> 16);
	rfpi[3] = (uint8_t)(v >> 8);
	rfpi[4] = (uint8_t)v;
	return 0;
}

uint8_t fwi_checksum(const struct fwi_image *img)
{
	uint32_t plen = fwi_payload_len(img);
	uint8_t x = 0;

	for (uint32_t i = 0; i < plen; i++)
		x ^= img->data[4 + i];
	return x;
}

void fwi_update_checksum(struct fwi_image *img)
{
	img->data[4 + fwi_payload_len(img)] = fwi_checksum(img);
}

static void count(struct fwi_patch_report *rep, enum fwi_tlv_result r)
{
	if (rep && r == FWI_TLV_REPLACED)
		rep->replaced++;
	else if (rep)
		rep->skipped++;
}

int fwi_patch(struct fwi_image *img, const struct fwi_family *fam,
	      const struct fwi_patch_input *in, struct fwi_patch_report *rep)
{
	static const uint8_t placeholder[4] = { '0', '2', '7', '0' };
	uint8_t *area;
	size_t size;

	if (rep)
		memset(rep, 0, sizeof(*rep));
	if (fwi_patch_area(img, fam, &area, &size))
		return -ENOENT;

	if (in->factory && in->factory_len > FWI_FACTORY_MAX) {
		if (rep)
			rep->factory_ignored = 1;
	} else if (in->factory) {
		const uint8_t *f = in->factory;
		size_t left = in->factory_len;

		/* Unlike the stock loader, a truncated last TLV is dropped. */
		while (left >= 2 && (size_t)f[1] + 2 <= left) {
			uint8_t tag = f[0], len = f[1];
			const uint8_t *val = f + 2;
			uint8_t rfpi[FWI_RFPI_LEN];

			if (tag == FWI_TAG_RFPI && len == FWI_RFPI_LEN &&
			    !memcmp(val, placeholder, 4) && in->maca &&
			    !fwi_rfpi_from_maca(in->maca, rfpi)) {
				val = rfpi;
				if (rep)
					rep->rfpi_from_maca = 1;
			}
			count(rep, fwi_tlv_replace(area, size, tag, val, len));
			f += 2 + len;
			left -= 2 + len;
		}
	}
	if (in->mode)
		count(rep, fwi_tlv_replace(area, size, FWI_TAG_MODE,
					   &in->mode, 1));
	if (in->rfpi)
		count(rep, fwi_tlv_replace(area, size, FWI_TAG_RFPI,
					   in->rfpi, FWI_RFPI_LEN));
	fwi_update_checksum(img);
	return 0;
}
