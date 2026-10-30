/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * STUB upload images: Intel HEX to upload image, patch-area lookup,
 * TLV patching and checksum. Specification: docs/stub-image-format.md.
 *
 * Pure functions on memory buffers: no file or device I/O, so the same
 * code runs on the box and in host tests.
 */
#ifndef OPENDECTCELL_FWIMAGE_H
#define OPENDECTCELL_FWIMAGE_H

#include <stddef.h>
#include <stdint.h>

/* Largest amount of image data the loader accepts (doc §3). */
#define FWI_MAX_DATA 0x2FFFFu

/* Longest factory TLV block the loader uses (doc §4, step 4). */
#define FWI_FACTORY_MAX 64u

/* TLV tags with a known meaning (doc §4). */
#define FWI_TAG_RFPI         1
#define FWI_TAG_MODE         7
#define FWI_TAG_STUB_VERSION 9
#define FWI_RFPI_LEN         5
#define FWI_STUB_VERSION_LEN 22

/* Firmware family, chosen from the second-level file name (doc §1). */
struct fwi_family {
	const char *name;          /* substring that selects it */
	uint32_t firstlevel_addr;  /* first-level load address */
	uint32_t patch_addr;       /* descriptor address, or fixed area */
	uint16_t fixed_patch_size; /* 0: area comes from a descriptor */
};

/* Never returns NULL: names that match nothing get the `488` rules. */
const struct fwi_family *fwi_family_from_name(const char *secondlevel_name);

/* An upload image, owned by the caller; release with fwi_image_free(). */
struct fwi_image {
	uint8_t *data;
	size_t len;
};

void fwi_image_free(struct fwi_image *img);

/*
 * Build images from Intel HEX text. Return 0, or a negative errno:
 * -EINVAL for malformed HEX (syntax, record checksum, missing EOF,
 * data below the load address), -EFBIG above FWI_MAX_DATA, -ENOMEM.
 */
int fwi_build_firstlevel(const char *hex, size_t hex_len, uint32_t load_addr,
			 struct fwi_image *out);
int fwi_build_secondlevel(const char *hex, size_t hex_len,
			  struct fwi_image *out);

/* Second-level images only. */
uint32_t fwi_payload_len(const struct fwi_image *img);
size_t fwi_segment_count(const struct fwi_image *img);

/*
 * Pointer to chip range [addr, addr + size) inside a second-level image,
 * or NULL if no single segment contains all of it.
 */
uint8_t *fwi_locate(const struct fwi_image *img, uint32_t addr, size_t size);

/* Find the STUB's patch area. Return 0 or -ENOENT. */
int fwi_patch_area(const struct fwi_image *img, const struct fwi_family *fam,
		   uint8_t **area, size_t *size);

/* Result of fwi_tlv_replace(). */
enum fwi_tlv_result {
	FWI_TLV_REPLACED = 0,
	FWI_TLV_SKIPPED = 1, /* tag absent, length differs, tag 0, bad list */
	FWI_TLV_NO_SPACE = 2,
};

/*
 * Overwrite the value of an existing TLV with the same tag and length.
 * Never appends (doc §4, step 3).
 */
enum fwi_tlv_result fwi_tlv_replace(uint8_t *area, size_t size, uint8_t tag,
				    const uint8_t *val, uint8_t len);

/* Find a TLV in an area. Return its value or NULL. */
const uint8_t *fwi_tlv_find(const uint8_t *area, size_t size, uint8_t tag,
			    uint8_t *len);

/*
 * RFPI from MAC text "xx:xx:xx:xx:xx:xx" (doc §4, step 5).
 * Return 0 or -EINVAL.
 */
int fwi_rfpi_from_maca(const char *maca, uint8_t rfpi[FWI_RFPI_LEN]);

/* XOR over the payload, stored after it (doc §3 and §4, step 8). */
void fwi_update_checksum(struct fwi_image *img);
uint8_t fwi_checksum(const struct fwi_image *img);

/* Inputs for fwi_patch(). */
struct fwi_patch_input {
	const uint8_t *factory;  /* factory TLV block, or NULL */
	size_t factory_len;
	const char *maca;        /* for the RFPI placeholder, or NULL */
	uint8_t mode;            /* low byte of the mode word; 0 = leave */
	/*
	 * Tag 1 set to these 5 bytes whatever the factory block says, or NULL
	 * (bench B0, docs/multicell-identities.md: a class B RFPI).
	 */
	const uint8_t *rfpi;
};

/* Counts from fwi_patch(), for logging. */
struct fwi_patch_report {
	unsigned replaced;
	unsigned skipped;
	int rfpi_from_maca;      /* 1 if the placeholder was replaced */
	int factory_ignored;     /* 1 if the block exceeded 64 bytes */
};

/*
 * Apply doc §4, steps 1-8, to a second-level image. `report` may be NULL.
 * Return 0, or -ENOENT if there is no patch area.
 */
int fwi_patch(struct fwi_image *img, const struct fwi_family *fam,
	      const struct fwi_patch_input *in, struct fwi_patch_report *report);

#endif
