/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal test helpers: no framework, plain C11. */
#ifndef OPENDECTCELL_CHECK_H
#define OPENDECTCELL_CHECK_H

#include <stdio.h>

static int check_failures;
static int check_count;

#define CHECK(cond)                                                       \
	do {                                                              \
		check_count++;                                            \
		if (!(cond)) {                                            \
			check_failures++;                                 \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n",      \
				__FILE__, __LINE__, #cond);               \
		}                                                         \
	} while (0)

#define CHECK_EQ(a, b)                                                    \
	do {                                                              \
		long long check_a_ = (long long)(a);                      \
		long long check_b_ = (long long)(b);                      \
		check_count++;                                            \
		if (check_a_ != check_b_) {                               \
			check_failures++;                                 \
			fprintf(stderr, "%s:%d: %s == %s failed: "        \
				"%lld != %lld (0x%llx != 0x%llx)\n",      \
				__FILE__, __LINE__, #a, #b, check_a_,     \
				check_b_, check_a_, check_b_);            \
		}                                                         \
	} while (0)

#define CHECK_DONE()                                                      \
	(printf("%s: %d checks, %d failed\n", __FILE__, check_count,     \
		check_failures),                                          \
	 check_failures ? 1 : 0)

#endif
