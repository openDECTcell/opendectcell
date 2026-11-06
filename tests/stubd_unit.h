/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The daemon's source files compiled as one translation unit, for the tests that reach into its
 * internals (test_stubd, test_stubd_cell). Include it where main() is renamed:
 *
 *   #define main stubd_main
 *   #include "stubd_unit.h"
 *   #undef main
 *
 * Add a new file of src/stubd/ here and to STUBD_C in the Makefile.
 */
#include "../src/stubd/dect-stubd.c"
#include "../src/stubd/stubd_link.c"
#include "../src/stubd/stubd_audio.c"
#include "../src/stubd/stubd_host.c"
#include "../src/stubd/stubd_calls.c"
#include "../src/stubd/stubd_handsets.c"
#include "../src/stubd/stubd_ctl.c"
#include "../src/stubd/stubd_cell.c"
#include "../src/stubd/stubd_opts.c"
