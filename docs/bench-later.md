# Bench later: every open test on one card

Collected 2026-10-30 while the maintainer is away from the bench
(`docs/tasks-2026-10-30.md`). Newest work first. Each test names its pass
and its fail before the run; results go into the file the test comes from,
or here for the tests written here.

## Before any run

- **Which box:** the 7412 (192.168.1.124) unless a test says the 7520.
  The 7412 is parked with its service stopped since 2026-10-30 09:37 UTC.
- **Packages:** build from the newest commit for the box
  (`OPENWRT=25.12.6 tools/release/build.sh lantiq/xrx200 build/release/out-<name>-lantiq-25126`),
  copy and `apk add --allow-untrusted --force-reinstall` the openDECTcell,
  opendectcell-baresip, opendectcell-carddav and luci-app-opendectcell packages.
- **apk keeps a hand-edited file in /etc** and puts the new one beside it
  as `*.apk-new` (2026-10-30). After an install: `ls /etc/init.d/*.apk-new
  /etc/config/*.apk-new` and move the new one over unless the edit is
  wanted.
- **Logs:** `logread -e dect`; mark a position by time, never by line
  count (logread is a ring buffer).
- **At the end:** stop the service, check `cat /var/run/opendectcell/parked`
  says `verified` (or the hold message in the log).

## 1. New since 2026-10-30 afternoon (desk work)

Added as the tasks land; each with the commit it needs.

| test | needs | pass | fail and what it means |
|---|---|---|---|
| N1. Register a handset afresh (deregister the C430A or the AS690H first), then call out from it, then restart the service and call again | T2 | registration, the first locate, then **one** `sub: cleared chip subscription slots` and, until the restart, no `sub: stored subscription reloaded into the chip` (after the restart it appears, as at every start); both calls go through | the second call is refused or the handset shows "no base" after the restart: the chip lacked the record, so the second install was needed after all; revert T2 |

## 2. Card A, 7412 (`docs/bench-7412.md`, "Card A")

| test | state |
|---|---|
| A1-A12 | not run: shared-line answer, two calls, call waiting, R key, DTMF, voicemail, narrowband, redial, phonebook edit and new entry, call lists after calls, delete a missed call |
| A13 | the load passed on the box; still open: after real calls (A1, A2) restart the service and look at the A540's call lists |
| A14 | passed on the box; still open: watch the A540 lose the base after `kill -9` and the stop |

## 3. Older cards with no recorded result (`docs/bench-next.md`, mostly the 7520)

| test | what |
|---|---|
| 17, lost handset | the fix `0b11de0` (the request only on a link that answered the page with the target's identity) was never run on the air |
| 17b, 17c | deregistration from a handset: needs ciphered list calls (test 20) |
| 20-26 | ciphered list session and the PIN, ring during a list sync, deregistration while another handset connects, the second call ciphered, the internal caller on the A540, the bridge without SIP, line names |
| 29-37 | the IPEI, calls from and to the FRITZ!Box, internal call with list access, Status > DECT, a handset that cannot prove its key, the periodic date and time, a fresh registration |
