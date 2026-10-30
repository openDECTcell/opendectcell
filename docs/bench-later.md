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

N2 needs packages built with S2.3d or later (S2.3, the stack's first
move into the library); run it before anything else on that build.

| test | pass | fail and what it means |
|---|---|---|
| N2a. The service starts with the handsets known | `sub: restored subscription ... (with call identities)` per store, `sub: cleared chip subscription slots other than ...`, `sub: locked-in handset mask 0x017c 00 NN`, the A540 and the C430A show the base within a minute | a handset searches: the install (`dect_base_slot_*`) or the held identities (`dect_base_link_open`) broke; `sub: refusing 0x020A` names a bad slot |
| N2b. The A540 relocates (power cycle it) | `reg: connection up`, `sub: now serving <its store>` if it is not store 0, LOCATE-ACCEPT, `reg: sent the date and time` | LOCATE-REJECT or "IPUI unknown": the identity gate or adoption (`dect_base_main_segment`) |
| N2c. Register a handset afresh (the AS690H or the C430A after a deregistration), PIN from LuCI | access rights, key allocation, the first locate, `sub: identity gate armed for the newly registered handset`, one install, `reg: subscription persisted + registration closed`, the store file written | the handset says "PIN wrong" or registers and then searches: the completion (`dect_base_main_registered`, the install callback) |
| N2d. A call each way on the newly registered handset, then restart the service and call again | both calls ciphered and with audio; after the restart the same | the second call fails: the saved record (`dect_base_changed`) |

N3 is bench B4a (`docs/multicell-identities.md` §10.1 step 2): a handset
gets an assigned IPUI O instead of keeping its IPUI N. Class A may send
any type (GAP table 51), so it runs today. One handset at a time (the
C430A or the AS690H first; the A540 last, it carries card A); each needs
its own number. Afterwards it stays registered with that identity: to go
back, deregister it and register again without the option.

| test | pass | fail and what it means |
|---|---|---|
| N3a. Deregister the handset, set `ipuio=1` in Network > DECT, Extra options (with `-v` for the log), open registration, register with the PIN | `nwk out` shows the ACCESS-RIGHTS-ACCEPT with `05 07 80 a8 10 00 00 00 01`; the handset says registered; its first LOCATE-REQUEST names `80 a8 10 00 00 00 01`; LOCATE-ACCEPT; `reg: subscription persisted` | the handset rejects the accept or keeps searching: it does not take an assigned IPUI; it locates with its IPEI (`80 a8 0x ...`) and gets LOCATE-REJECT: it stored the identity but does not use it |
| N3b. Restart the service; the handset relocates | LOCATE-REQUEST with the IPUI O, accepted | "IPUI unknown": the record or the gate lost the assigned identity |
| N3c. Call it (call-in from SIP), then call out from it | it rings on the page (TPUI from LOCATE-ACCEPT, `00 11 0n`), the call connects ciphered both ways | it does not ring: the page; the call fails at CC-SETUP: the CC identities derived from the record |
| N3d. The same on the other Gigasets (`ipuio=2`, `ipuio=3`) | as above | a model that refuses: note which, B4b must avoid it |

N4 is bench B0 (`docs/multicell-identities.md` §10.1 step 1): does the
06.88 STUB keep a class B RFPI with an RPN above 7? Nothing is transmitted:
in cell role the radio stays parked. No handset needed, so it can run
unattended (T13). The RFPI is made up: ARC 001, EIC 0F42, FPN 01, FPS 1,
RPN 09 = `10f4201109`.

```
uci set opendectcell.main.role=cell; uci set opendectcell.main.rfpi_test=10f4201109; uci commit opendectcell
/etc/init.d/opendectcell stop; rm -f /var/run/opendectcell/stub.second
/etc/init.d/opendectcell start      # uploads with tag 1 = 10f4201109, dect-stubd -K stays parked
logread -e rfpi -e 'control slot'; cat /tmp/dect-fwload.log
# afterwards: uci delete opendectcell.main.rfpi_test; uci set opendectcell.main.role=base; uci commit opendectcell
#             /etc/init.d/opendectcell stop; rm -f /var/run/opendectcell/stub.second; /etc/init.d/opendectcell start
```

| test | pass | fail and what it means |
|---|---|---|
| N4a. The upload with the class B RFPI | `/tmp/dect-fwload.log` counts one more TLV patched; `control slot found`; the chip's periodic mails as usual | no control slot after 3 minutes: a class B tag 1 stops the STUB booting |
| N4b. The read-back | `rfpi: the chip reports the RFPI rfpicheck= names, all 5 octets` | `DIFFERS ... octet(s) 5`: the STUB rewrote the RPN (it keeps 3 bits: class A only); other octets: it rewrote the ARI; no `rfpi:` line: no 0xF601 came (the contact answer) |
| N4c. Back to the factory RFPI (the afterwards lines), then a handset | the base back as before, the A540 finds it | the chip kept the class B RFPI: the reload did not run (`stub.second` still there) |

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
