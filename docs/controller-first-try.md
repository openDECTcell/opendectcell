# The controller's first try with a real 7412

Prepared 2026-10-31 night for the maintainer's first run of the controller
with a real cell (the controller repo's "step 3"). What is ready, what to do
in what order, what to watch, and how to get back. The controller side is
`opendectcell-controller` (`docs/deployment.md`, the maintainers' notes); the cell side is
[luci-manual.md](luci-manual.md) section 4.

## What is ready

| | State |
|---|---|
| Controller image | Builds with `LIBOPENDECTCELL=1` in Docker (first time, 2026-10-31 night: `docker compose build --build-arg LIBOPENDECTCELL=1`, exit 0) and starts: migrations applied, SIP, web on 8080, cells on 7300. The stack was torn down again (`down -v`); nothing is left running or stored |
| Library in the controller | Submodule at openDECTcell `e0c38f0` (API 142), `cdect.h` requires 142 (controller `28d04ed`). Not compiled with Go on the machine I worked on (no Go toolchain); the controller's CI builds it with the submodule |
| 7412 | Running as a standalone base with a test setup from 2026-10-31 (see "Before you start"). It must be put back to a clean state and given the cell packages |
| Cell packages | Built by the "Packages" workflow on `main` (lantiq/xrx200 for the 7412), dispatched 2026-10-31 night; see below for where they are |
| What the chip allows | A standby handset needs no chip slot (more-than-six.md section 6): the controller may keep every key and give the chip only what a call needs, with one key always in the chip's table |

## Before you start (the 7412)

1. The 7412 still has test options (`holdback`, `tpuioffset`), PIN `1111`,
   DECT 1's line removed and a hand-assigned line for DECT 3. The old PIN is
   in `/root/pin.before-1111`, the old line in `/root/dect1.line1.before-run7`
   (both on the box). Restore both, or leave them: in cell mode this box's own
   handsets, PIN and lines are saved but unused.
2. Have the Tasmota socket at hand. The stop of the DECT service on the 7412
   can hang (rare; none in about ten stops on 2026-10-31). The socket also
   powers the lab 7520, so a cycle takes both.
3. Install the new packages (the `opendectcell`, `opendectcell-cell` and the LuCI app
   from the Packages run), then check the daemon says API 142.

## The run

1. On the PC: `docker compose up -d` in the controller repo with the image
   built `LIBOPENDECTCELL=1`, create an admin user, **back up the master key**
   (`docs/deployment.md`).
2. In the controller's web interface, take the cell **enrollment string**
   (`opendectcell-enroll:v1?...`; it holds the controller's address, so the
   7412 must reach the PC on port 7300). Approve the cell if asked.
3. On the 7412: Network > DECT, *Make this box a cell...*, paste, *Enrol*
   (up to a minute). The link should read *up* and the cell *active*.
4. Open a registration window for your user at the controller (a PIN per
   window, ADR 0005). Register a handset with that PIN. Expected: a handset
   record at the controller, keys on the controller; the 7412 keeps none.
5. The handset should show the base and locate. Then calls, once the
   controller has a SIP account: out, in, and (after the controller's media
   step) audio. Audio through the controller is the controller's C3 step 2 and
   may not be there yet; signalling and registration are the first goal.

## What to watch, and what is known to be wrong

- LuCI on the 7412 sometimes says the base is off while it is up, and its
  handset list has the wrong order, "Last Contact" and the type column miss
  values (tasks L2 to L5). In cell mode LuCI shows only the cell page, so
  watch the cell's own log (`logread -e dect-cell -e dect-stubd`) as well.
- A handset that does not answer can have stopped other handsets ringing; the
  fix (L6) is in this build and has not been on air yet.
- Registering a handset over a record the box already holds was not saved
  before API 140; the fix is in this build and not yet seen on air.
- Keep one key in the chip's table at all times; an empty table silenced the
  base on 07.56.

## Getting back

*Leave cell mode...* on the 7412 erases the controller's key and
certificate and runs it as its own base again (luci-manual.md). To be a cell
again it needs a new enrollment string. The handsets registered through the
controller are not known to the box afterwards and must be registered again.
