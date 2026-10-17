// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/fwload/fwimage.c against docs/stub-image-format.md.
 *
 * Synthetic HEX input always runs. The real AVM files are used only if
 * OPENDECTCELL_STUB_HEX / OPENDECTCELL_FL_HEX point to them (they are never in git).
 */
#include "check.h"
#include "../src/fwload/fwimage.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ---- building HEX text ------------------------------------------------ */

static char hexbuf[8192];

static void rec(int type, unsigned addr, const uint8_t *d, unsigned n)
{
	char *p = hexbuf + strlen(hexbuf);
	unsigned sum = n + (addr >> 8) + (addr & 0xFF) + type;

	p += sprintf(p, ":%02X%04X%02X", n, addr & 0xFFFF, type);
	for (unsigned i = 0; i < n; i++) {
		p += sprintf(p, "%02X", d[i]);
		sum += d[i];
	}
	sprintf(p, "%02X\r\n", (0x100 - (sum & 0xFF)) & 0xFF);
}

static void ext_lin(unsigned upper)
{
	uint8_t d[2] = { (uint8_t)(upper >> 8), (uint8_t)upper };
	rec(4, 0, d, 2);
}

static void eof(void)
{
	rec(1, 0, NULL, 0);
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* ---- tests ------------------------------------------------------------ */

static void test_families(void)
{
	CHECK_EQ(fwi_family_from_name("dectfw_secondlevel_441.hex")->patch_addr, 0);
	CHECK_EQ(fwi_family_from_name("dectfw_secondlevel_441.hex")->firstlevel_addr,
		 0x10080);
	CHECK_EQ(fwi_family_from_name("x_488.hex")->patch_addr, 0xF0000);
	CHECK_EQ(fwi_family_from_name("DA14495.hex")->fixed_patch_size, 50);
	CHECK_EQ(fwi_family_from_name("DA14495.hex")->patch_addr, 0x200000C0);
	CHECK_EQ(fwi_family_from_name("DCX81.hex")->patch_addr, 0x400000);
	CHECK_EQ(fwi_family_from_name("other.hex")->patch_addr, 0xF0000);
}

static void test_segments(void)
{
	static const uint8_t a[4] = { 1, 2, 3, 4 }, b[4] = { 5, 6, 7, 8 },
			     c[2] = { 9, 10 }, e[3] = { 11, 12, 13 };
	struct fwi_image img;

	hexbuf[0] = 0;
	rec(0, 0x0000, a, 4);
	rec(0, 0x0004, b, 4); /* contiguous: same segment */
	rec(0, 0x0010, c, 2); /* gap: new segment */
	ext_lin(0x0001);      /* extended address: always a new segment */
	rec(0, 0x0012, e, 3);
	eof();

	CHECK_EQ(fwi_build_secondlevel(hexbuf, strlen(hexbuf), &img), 0);
	CHECK_EQ(fwi_segment_count(&img), 3);
	CHECK_EQ(fwi_payload_len(&img), 3 * 8 + 8 + 2 + 3);
	CHECK_EQ(img.len, 4 + fwi_payload_len(&img) + 1);

	const uint8_t *p = img.data + 4;
	CHECK_EQ(be32(p), 0x0000);
	CHECK_EQ(be32(p + 4), 8);
	CHECK(!memcmp(p + 8, a, 4) && !memcmp(p + 12, b, 4));
	p += 16;
	CHECK_EQ(be32(p), 0x0010);
	CHECK_EQ(be32(p + 4), 2);
	p += 10;
	CHECK_EQ(be32(p), 0x10012);
	CHECK_EQ(be32(p + 4), 3);

	uint8_t x = 0;
	for (uint32_t i = 0; i < fwi_payload_len(&img); i++)
		x ^= img.data[4 + i];
	CHECK_EQ(img.data[img.len - 1], x);

	CHECK(fwi_locate(&img, 0x0002, 4) == img.data + 4 + 8 + 2);
	CHECK(fwi_locate(&img, 0x10012, 3) != NULL);
	CHECK(fwi_locate(&img, 0x0006, 4) == NULL);  /* runs past the end */
	CHECK(fwi_locate(&img, 0x000C, 1) == NULL);  /* in the gap */
	fwi_image_free(&img);
}

static void test_contiguous_across_ext(void)
{
	static const uint8_t a[8] = { 0 };
	struct fwi_image img;

	/* Same 16-bit address continuation, but an extended record between:
	 * still a new segment (doc §3). */
	hexbuf[0] = 0;
	rec(0, 0x0000, a, 8);
	ext_lin(0x0000);
	rec(0, 0x0008, a, 8);
	eof();
	CHECK_EQ(fwi_build_secondlevel(hexbuf, strlen(hexbuf), &img), 0);
	CHECK_EQ(fwi_segment_count(&img), 2);
	fwi_image_free(&img);
}

static void test_bad_hex(void)
{
	static const uint8_t a[2] = { 1, 2 };
	struct fwi_image img;

	hexbuf[0] = 0;
	rec(0, 0, a, 2);
	CHECK_EQ(fwi_build_secondlevel(hexbuf, strlen(hexbuf), &img), -EINVAL);

	eof();
	size_t n = strlen(hexbuf);
	hexbuf[9] = hexbuf[9] == 'F' ? '0' : 'F'; /* corrupt first data byte */
	CHECK_EQ(fwi_build_secondlevel(hexbuf, n, &img), -EINVAL);

	CHECK_EQ(fwi_build_secondlevel("garbage", 7, &img), -EINVAL);
}

static void test_firstlevel(void)
{
	static const uint8_t a[3] = { 0xA1, 0xA2, 0xA3 }, b[2] = { 0xB1, 0xB2 };
	struct fwi_image img;

	hexbuf[0] = 0;
	ext_lin(0x0001);
	rec(0, 0x0080, a, 3);
	rec(0, 0x0085, b, 2); /* 2-byte gap, zero-filled */
	eof();
	CHECK_EQ(fwi_build_firstlevel(hexbuf, strlen(hexbuf), 0x10080, &img), 0);
	CHECK_EQ(img.len, 2 + 7 + 1);
	CHECK_EQ(img.data[0], 7); /* little-endian length */
	CHECK_EQ(img.data[1], 0);
	static const uint8_t want[7] = { 0xA1, 0xA2, 0xA3, 0, 0, 0xB1, 0xB2 };
	CHECK(!memcmp(img.data + 2, want, 7));
	CHECK_EQ(img.data[9], 0xA1 ^ 0xA2 ^ 0xA3 ^ 0xB1 ^ 0xB2);
	fwi_image_free(&img);

	/* data below the load address */
	CHECK_EQ(fwi_build_firstlevel(hexbuf, strlen(hexbuf), 0x10081, &img),
		 -EINVAL);
}

static void test_rfpi(void)
{
	uint8_t r[5];

	/* By hand from doc §4 step 5: d10=3 d12=4 d13=4 d15=5 d16=5
	 * -> v = 0x70BA22A8 -> 02 70 BA 22 A8 */
	CHECK_EQ(fwi_rfpi_from_maca("00:11:22:33:44:55", r), 0);
	static const uint8_t want[5] = { 0x02, 0x70, 0xBA, 0x22, 0xA8 };
	CHECK(!memcmp(r, want, 5));
	CHECK_EQ(fwi_rfpi_from_maca("00:11:22", r), -EINVAL);
	CHECK_EQ(fwi_rfpi_from_maca("00:11:22:33:4G:55", r), -EINVAL);
}

static void test_tlv(void)
{
	uint8_t area[16] = { 1, 2, 0xAA, 0xBB, 7, 1, 0x00, 0 };
	uint8_t v2[2] = { 0x11, 0x22 }, v3[3] = { 1, 2, 3 }, m = 5, len;

	CHECK_EQ(fwi_tlv_replace(area, sizeof(area), 1, v2, 2), FWI_TLV_REPLACED);
	CHECK(area[2] == 0x11 && area[3] == 0x22);
	CHECK_EQ(fwi_tlv_replace(area, sizeof(area), 1, v3, 3), FWI_TLV_SKIPPED);
	CHECK_EQ(fwi_tlv_replace(area, sizeof(area), 9, &m, 1), FWI_TLV_SKIPPED);
	CHECK_EQ(fwi_tlv_replace(area, sizeof(area), 0, &m, 1), FWI_TLV_SKIPPED);
	CHECK_EQ(fwi_tlv_replace(area, sizeof(area), 7, &m, 1), FWI_TLV_REPLACED);
	CHECK_EQ(area[6], 5);
	CHECK(fwi_tlv_find(area, sizeof(area), 7, &len) == area + 6 && len == 1);
	CHECK(fwi_tlv_find(area, sizeof(area), 9, &len) == NULL);

	uint8_t small[4] = { 5, 2, 0, 0 }, big[8] = { 0 };
	CHECK_EQ(fwi_tlv_replace(small, sizeof(small), 5, big, 8),
		 FWI_TLV_NO_SPACE);
}

/* Image with a descriptor at 0 -> patch area at 0x20, 16 bytes. */
static void build_patchable(struct fwi_image *img)
{
	uint8_t seg[0x30] = { 0x20, 0, 0, 0, 16, 0 };
	static const uint8_t tlvs[] = { 1, 5, '0', '2', '7', '0', 0x01,
					7, 1, 0x00, 4, 1, 0x33, 0 };

	memcpy(seg + 0x20, tlvs, sizeof(tlvs));
	hexbuf[0] = 0;
	rec(0, 0x0000, seg, 0x10);
	rec(0, 0x0010, seg + 0x10, 0x10);
	rec(0, 0x0020, seg + 0x20, 0x10);
	eof();
	CHECK_EQ(fwi_build_secondlevel(hexbuf, strlen(hexbuf), img), 0);
}

static void test_patch(void)
{
	const struct fwi_family *fam = fwi_family_from_name("x_441.hex");
	struct fwi_image img;
	struct fwi_patch_report rep;
	uint8_t *area, len;
	size_t size;

	build_patchable(&img);
	CHECK_EQ(fwi_patch_area(&img, fam, &area, &size), 0);
	CHECK_EQ(size, 16);

	/* placeholder RFPI, a tag the area lacks, a changed tag 4 */
	static const uint8_t factory[] = { 1, 5, '0', '2', '7', '0', 0x99,
					   3, 1, 0x44, 4, 1, 0x55 };
	struct fwi_patch_input in = { factory, sizeof(factory),
				      "00:11:22:33:44:55", 0x05 };
	CHECK_EQ(fwi_patch(&img, fam, &in, &rep), 0);
	CHECK_EQ(rep.rfpi_from_maca, 1);
	CHECK_EQ(rep.replaced, 3); /* RFPI, tag 4, mode */
	CHECK_EQ(rep.skipped, 1);  /* tag 3 */

	const uint8_t *v = fwi_tlv_find(area, size, FWI_TAG_RFPI, &len);
	static const uint8_t rfpi[5] = { 0x02, 0x70, 0xBA, 0x22, 0xA8 };
	CHECK(v && len == 5 && !memcmp(v, rfpi, 5));
	v = fwi_tlv_find(area, size, FWI_TAG_MODE, &len);
	CHECK(v && *v == 0x05);
	v = fwi_tlv_find(area, size, 4, &len);
	CHECK(v && *v == 0x55);
	CHECK_EQ(img.data[img.len - 1], fwi_checksum(&img));
	fwi_image_free(&img);

	/* placeholder kept when no MAC is available */
	build_patchable(&img);
	struct fwi_patch_input no_mac = { factory, sizeof(factory), NULL, 0 };
	CHECK_EQ(fwi_patch(&img, fam, &no_mac, &rep), 0);
	CHECK_EQ(rep.rfpi_from_maca, 0);
	fwi_patch_area(&img, fam, &area, &size);
	v = fwi_tlv_find(area, size, FWI_TAG_RFPI, &len);
	CHECK(v && v[4] == 0x99);
	v = fwi_tlv_find(area, size, FWI_TAG_MODE, &len);
	CHECK(v && *v == 0x00); /* mode 0: untouched */
	fwi_image_free(&img);

	/* factory block longer than 64 bytes is ignored entirely */
	build_patchable(&img);
	uint8_t longblk[65] = { 4, 1, 0x77 };
	struct fwi_patch_input too_long = { longblk, sizeof(longblk), NULL, 0 };
	CHECK_EQ(fwi_patch(&img, fam, &too_long, &rep), 0);
	CHECK_EQ(rep.factory_ignored, 1);
	fwi_patch_area(&img, fam, &area, &size);
	v = fwi_tlv_find(area, size, 4, &len);
	CHECK(v && *v == 0x33);
	fwi_image_free(&img);

	/* no descriptor -> no patch area */
	static const uint8_t a[2] = { 0 };
	hexbuf[0] = 0;
	rec(0, 0x0100, a, 2);
	eof();
	CHECK_EQ(fwi_build_secondlevel(hexbuf, strlen(hexbuf), &img), 0);
	CHECK_EQ(fwi_patch(&img, fam, &in, &rep), -ENOENT);
	fwi_image_free(&img);
}

/* ---- real AVM files (optional) ---------------------------------------- */

static char *slurp(const char *path, size_t *len)
{
	FILE *f = path ? fopen(path, "rb") : NULL;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	char *p = malloc(n);
	if (p && fread(p, 1, n, f) != (size_t)n) {
		free(p);
		p = NULL;
	}
	fclose(f);
	*len = n;
	return p;
}

/* Expected values: docs/stub-image-format.md §3-§4, 7520 FRITZ!OS 08.25. */
static void test_real_7520(void)
{
	size_t n;
	char *hex = slurp(getenv("OPENDECTCELL_STUB_HEX"), &n);
	if (!hex) {
		printf("skip: OPENDECTCELL_STUB_HEX not set or unreadable\n");
		return;
	}
	struct fwi_image img;
	const struct fwi_family *fam =
		fwi_family_from_name("dectfw_secondlevel_441.hex");
	uint8_t *area, len;
	size_t size;

	CHECK_EQ(fwi_build_secondlevel(hex, n, &img), 0);
	CHECK_EQ(fwi_payload_len(&img), 54119); /* logged by the stock loader */
	CHECK_EQ(fwi_segment_count(&img), 9);
	CHECK_EQ(fwi_checksum(&img), 0xC7);     /* unpatched */
	CHECK_EQ(fwi_patch_area(&img, fam, &area, &size), 0);
	CHECK_EQ(size, 53);
	CHECK(area == fwi_locate(&img, 0xA0, 53));
	static const uint8_t tags[] = { 1, 2, 3, 4, 6, 7, 10 };
	for (size_t i = 0; i < sizeof(tags); i++)
		CHECK(fwi_tlv_find(area, size, tags[i], &len) != NULL);
	CHECK(fwi_tlv_find(area, size, FWI_TAG_STUB_VERSION, &len) == NULL);
	fwi_image_free(&img);
	free(hex);

	hex = slurp(getenv("OPENDECTCELL_FL_HEX"), &n);
	if (!hex) {
		printf("skip: OPENDECTCELL_FL_HEX not set or unreadable\n");
		return;
	}
	CHECK_EQ(fwi_build_firstlevel(hex, n, fam->firstlevel_addr, &img), 0);
	CHECK_EQ(img.len, 618 + 3);
	CHECK_EQ(img.data[0] | img.data[1] << 8, 618);
	fwi_image_free(&img);
	free(hex);
}

int main(void)
{
	test_families();
	test_segments();
	test_contiguous_across_ext();
	test_bad_hex();
	test_firstlevel();
	test_rfpi();
	test_tlv();
	test_patch();
	test_real_7520();
	return CHECK_DONE();
}
