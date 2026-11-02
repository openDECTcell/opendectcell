# Bench later: every open test on one card

Collected 2026-10-30 while the maintainer is away from the bench
(`docs/tasks-2026-10-30.md`). Newest work first. Each test names its pass
and its fail before the run; results go into the file the test comes from,
or here for the tests written here.

## Before any run

- **Which box:** the 7412 (192.0.2.124) unless a test says the 7520.
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

## 0. Bench day results, 2026-10-31 afternoon (the bench day of 2026-11-01 moved up; maintainer at the bench)

| test | result |
|---|---|
| V2b (lab 7520, 08.25) | **pass**: after a restart all three handsets located (`connection up`, `sent the date and time`), no LOCATE-REJECT |
| T-B (06.88, 07.56, 08.25) | **06.88: world A** (a keyless handset is admitted, AS690H and C430A fully located, empty table). **07.56: world A with a non-empty table** (the A540 connected with its key held back); an empty table silences the base. **08.25: open** (the first reading, world B, was an empty table on a cold chip; retracted). `more-than-six.md` §4b |
| T-C (06.88) | **pass**, the form `{00,11,N}`: LOCATE-ACCEPT with `0x1143`, ASSIGN-ACK, page answered, ciphered call. A handset takes a new TPUI only at its next location registration. `more-than-six.md` §4b |
| 4b, registration open during a call (7412, 06.88, AS690H) | **pass**: window opened 15:39:48 (`0x022B`) and closed 15:41:48 (`0x022C`, window elapsed) during one ciphered call; no click or dropout heard at either, the call stayed up until the maintainer hung up; starvation 0.00 %, radio error counters 0 at the two moments (a few single CRC errors 36-60 s after the opening). One handset, no registration attempt during the call |
| fresh registration (N2c/N2d/card 37, partly) | the three handsets registered on the 7520 and the AS690H on the 7412 (fresh stores): saved, keys landed, located after restarts, a ring-in call ciphered on the 7412. Two defects found: a new handset takes the first free record in list order, not the lowest slot (the A540 landed in slot 3 on the 7520); re-registering a handset the base already holds, without a deregistration, is not saved (C430A on the 7412). Also: after a registration on 08.25 the key read-back runs at once and reports a false `READ-BACK MISMATCH` (the default `keycheckms` is 0; 50 ms is enough) |

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
logread -e rfpi -e 'control slot'; cat /var/run/opendectcell/dect-fwload.log
# afterwards: uci delete opendectcell.main.rfpi_test; uci set opendectcell.main.role=base; uci commit opendectcell
#             /etc/init.d/opendectcell stop; rm -f /var/run/opendectcell/stub.second; /etc/init.d/opendectcell start
```

| test | pass | fail and what it means |
|---|---|---|
| N4a. The upload with the class B RFPI | `/var/run/opendectcell/dect-fwload.log` counts one more TLV patched; `control slot found`; the chip's periodic mails as usual | no control slot after 3 minutes: a class B tag 1 stops the STUB booting |
| N4b. The read-back | `rfpi: the chip reports the RFPI rfpicheck= names, all 5 octets` | `DIFFERS ... octet(s) 5`: the STUB rewrote the RPN (it keeps 3 bits: class A only); other octets: it rewrote the ARI; no `rfpi:` line: no 0xF601 came (the contact answer) |
| N4c. Back to the factory RFPI (the afterwards lines), then a handset | the base back as before, the A540 finds it | the chip kept the class B RFPI: the reload did not run (`stub.second` still there) |

| test | needs | pass | fail and what it means |
|---|---|---|---|
| N5. After calls in and out, open the A540's call lists; and with `-v`, see whether it ever asks for list 09 (`c3 00 09`) | API 100 | the lists as before; if the A540 opens 09, missed and accepted calls together | an empty or garbled list: the view or its fields; nobody asks for 09: nothing to see, the list is optional |

| N6. STATS: the 7412 as a cell of the Go controller in Docker (as on 2026-10-30, `docs/notes-for-the-controller.md`), radio parked | T11 | within a minute of the link coming up the controller's cell page lists counters: `mails_up`, `mails_down`, `stub_csum_errors` ... `stub_resyncs` (0 when healthy), `chip_queue_hwm`, `reconnects`; again every 60 s | no counters: dect-cell did not announce CAP_STATS or the request did not reach dect-stubd (`logread -e dect-cell`); `reconnects` only: dect-stubd did not answer STATS_REQUEST |

| N7. Resume: the 7412 as an active cell of the Go controller; cut its link for 4 s (`iptables -I OUTPUT -d <controller> -j DROP; sleep 4; iptables -D OUTPUT -d <controller> -j DROP`), then for 10 s | T11 | 4 s: `controller link resumed` in `logread -e dect-cell`, the controller logs `cell connected resumed=true`, no calls or links released; 10 s: a fresh start (`link up`, `resumed=false`), the cell released its links at 7 s | a fresh start after 4 s: the token or the grace (HELLO tag 13, HELLO_ACK tags 4, 5, 8); the controller rejects the snapshot (ERROR): its layout |

## 1a. T13: unattended runs after the stack move (written 2026-10-30, before the runs)

The maintainer asked for unattended runs on the 7412 and the 7520 once the
stack is in the library (`docs/stack-in-library.md`, S0-S6c, API 136).
Nobody is at the handsets: these runs check that the moved stack starts,
restores the handsets into the chip, holds up and stops cleanly. They
cannot check anything on the air beyond the base's own mails. Packages
from `a30793a`. Each box ends parked with the park verified.

| test | pass | fail and what it means |
|---|---|---|
| U1. 7412: install, start, five minutes running | `control slot found` within 90 s; `sub: restored subscription` per store; `sub: cleared chip subscription slots other than`; `sub: locked-in handset mask 0x017c`; the same dect-stubd pid after five minutes; no `LOST`, no `***`, no `ASSERT`/`AddressSanitizer`; `/var/run/opendectcell/state.json` rewritten | no control slot: bring-up or transport (not moved; compare `f3a2f4f`); a new pid or a procd respawn: a crash in the moved stack, logread before it says where; no restore lines: the start-up restore (`sub_restored`, `dect_base_slot_*`) |
| U1b. If a handset happens to locate meanwhile | `reg: connection up`, `LOCATE-ACCEPT` in its `nwk out` when -v, `reg: sent the date and time` (or the clock-not-right line) | `LOCATE-REJECT`/`IPUI unknown` for a handset that located on `f3a2f4f`: the identity gate or `dect_base_main_segment()`/`dect_base_serve()` |
| U2. 7412: bench N4 (class B RFPI, radio parked), as written above | N4a and N4b's pass | N4a/N4b's fail columns |
| U3. 7412: bench N6 and N7 (STATS, resume) against the Go controller in Docker on the desk PC, radio parked, as written above; then the cell credentials forgotten, role base again | N6's and N7's pass | N6's and N7's fail columns |
| U4. 7412: stop | `/var/run/opendectcell/parked` says `verified` | anything else: the park (not moved) |

U1 transmits for five minutes (the base beacons, as at any start); U2
and U3 do not (cell role, radio parked). The 7520 runs follow once the
maintainer says what that box may be used for: it runs 0.3.0 with the
radio on and may be the household's working VoIP base.

### T13 results, 7412, 2026-10-30 13:04-13:16 UTC (unattended)

Packages from `a30793a` (`build/release/out-a30793a-lantiq-25126`); apk
kept the box's own `/etc/config/opendectcell` (its SIP accounts and PIN) and
put the packaged default beside it as `opendectcell.apk-new`, left there.
Logs on the box: `/root/t13-u1-20261030-130434.log`,
`/root/t13-u2-20261030-131114.log`; the controller's in the session's
scratchpad.

| test | result |
|---|---|
| U1 | **pass**: control slot 56 s after the start; `sub: restored subscription` for slots 1 and 2 (with call identities), the other chip slots cleared, mask `00 03`; the same pid after five minutes; no trouble lines, no respawn; `state.json` rewritten. No handset located meanwhile, so U1b saw nothing |
| U2 = N4a, N4b | **pass**: 6 TLV patched (a normal upload: 5), control slot found, and `rfpi: the chip reports the RFPI rfpicheck= names, all 5 octets (0xF601)`: the 06.88 STUB keeps a class B RFPI with RPN 09 (made up, `10f4201109`), radio parked throughout. N4c (a handset finds the base again) stays for the bench |
| U3 = N6 | **pass, with two bugs found**: the controller's cell page listed `chip_queue_hwm=28 mails_down=0 mails_rejected=0 mails_up=50 reconnects=0 stub_csum_errors=0 stub_framing_errors=0 stub_queue_drops=0 stub_seq_gaps=0`. Missing: `stub_resyncs` -- the STATS buffer held 12 octets per counter where a TLV takes 13, so the ninth never fitted (fixed, `a84dfa8`). And `reconnects` stayed 0 after the two cuts below: dect-cell counted failed attempts, not links re-established (fixed, `6262998`). Both fixes not yet on a box |
| U3 = N7 | **pass**: a 4 s cut (nftables drop of the box's packets to the controller, as the card's iptables lines): `controller link down: nothing heard` after 3 s, `controller link resumed` 2 s later, the controller `resumed=true`; a 10 s cut: the controller's grace expired at 7 s, a fresh `link up`, `resumed=false`. No links to release (radio parked) |
| U4 | **pass**: after `cell_leave` and the stop, role base, no credentials, nothing running, `parked` says `verified` |

Left for a box: the two fixes above (N6 again: `stub_resyncs` listed,
`reconnects` 2 after two cuts). The 7520 runs wait for the maintainer's
word on that box.

**U5, S2.2b on the 7412** (written 2026-10-30, before the run): the
read-back and the audit's answers are judged by the library since
`0cad99e`, which no trace covers (the traces carry the reads, not the
answers). The `0cad99e` dect-stubd over the packaged one (kept as
`/root/dect-stubd.a30793a`), a service start, the control slot, ten
seconds, the stop, the packaged binary back. Pass: the same three lines
as U1, from the chip's real answers, about a second after the slots are
written: `UAK read-back slot 1 at 0x0067F: MATCHES (16 bytes, flag 00)`,
`chip slot audit: 1=OURS 2=ours 3=- 4=- 5=- 6=-`, `slot table is as we
intend it`; no `***` line; `parked` says `verified` at the end. Fail: no
read-back or audit line (the delay timer never fired: the tick), a
MISMATCH or a foreign slot that U1 did not show (the answer parsed
wrong), or a different address (the table not handed over).

**U5 result, 2026-10-30 13:54-13:55 UTC: pass.** Control slot at 14:54:44
(box time), the slots cleared, mask `00 03`; at 14:54:45, one second
later as the 06.88 delay wants: `UAK read-back slot 1 at 0x0067F:
MATCHES (16 bytes, flag 00)`, `chip slot audit: 1=OURS 2=ours 3=- 4=-
5=- 6=-`, `slot table is as we intend it`; no `***` line. Stopped,
`parked` says `verified`, the packaged binary back (md5 as before). Log:
`/root/t13-u5-20261030-135437.log`.

**U6, the 7412 on the current packages** (written 2026-10-30, before the
run): U1 again, on packages from `37ec832`
(`build/release/out-37ec832-lantiq-25126`; openDECTcell, opendectcell-baresip,
opendectcell-carddav, luci-app-opendectcell; the kernel modules unchanged since
`a30793a`, so not reinstalled). Install, start, five minutes, the stop.
Pass: as U1 (control slot, `sub: restored subscription` for slots 1 and
2, the others cleared, mask `00 03`, the same pid after five minutes, no
trouble line, no respawn) and as U5 (`MATCHES` at `0x0067F`, the audit
`1=OURS 2=ours`, `slot table is as we intend it`); `chip: STUB second
stage dectfw_secondlevel_441-06.88-7412.hex` named (the box's init
script is the packaged one, so no `.apk-new` for it); LuCI's two views
installed (`/www/luci-static/resources/view/opendectcell/settings.js`,
`monitor.js`); at the end `parked` says `verified`. Fail: anything U1 or
U5 did not show; a `.apk-new` for the init script means the packaged one
was edited on the box.

**U6 result, 2026-10-30 14:03-14:11 UTC: pass on the second start, two
findings.** apk kept `/etc/config/opendectcell` only (`.apk-new`), the init
script was replaced. **The first start failed its control-slot search:**
it measured lane bit offset 9 (1533 idle lanes, 1022 at 1), tried 9 and
1 alternately for ten passes and exited with `control slot not found`
after 68 s; procd started it again, which measured offset 3 and found
the slot 17 s later (`tx 25, rx 6`). The SSC transport is unchanged
since `a30793a`, and the seven starts before it that day all found the
slot: an old weak spot met once, not a regression. The script's own
checks followed the first pid, so they read "control slot: 0" and "pid
14779 (was 14594)". From the second start, all as predicted: slots 1
and 2 restored, the others cleared, mask `00 03`, `UAK read-back slot 1
at 0x0067F: MATCHES`, audit `1=OURS 2=ours`, `slot table is as we
intend it`, no `***` line in six minutes, `state.json` with `"api":137`,
the stop's park read back (`parked`: `verified`). **The LuCI package
shipped `settings.js.o`:** jsmin failed on the view (a regex after
`return`) and luci.mk kept its partial output beside the original;
fixed in `8530657`, with a check in `make check`. Log:
`/root/t13-u6-20261030-140323.log` (the first pid's lines).

**U7, the slot search measures again** (written 2026-10-30, before any
run): since the commit after `8530657`, a 7412 start that has tried both
offsets twice measures the lanes again before going on (passes 4 and 8).
It changes nothing on a start that finds the slot. Pass, when a search
fails again: `ssc: no slot at offsets A and B; measuring the lanes
again`, then `ssc: lane bit offset C`; with C not A or B the slot found
in that same start (the offset had moved: the cause found), or with C
the same as before still none found (the offset did not move: the cause
is elsewhere, the chip not answering). Unattended starts on the 7412
cover it, whenever one fails.

**U6b, the fixed packages on the 7412** (written 2026-10-30, before the
run): packages from `e1a4cbf` (openDECTcell, opendectcell-baresip, opendectcell-carddav,
luci-app-opendectcell), installed as in U6, a start, the control slot, the
read-back and audit lines, the stop. Pass: no `settings.js.o` in the
views and `settings.js` minified (no comment lines left in it); the
control slot found (in the first start, or after `measuring the lanes
again`, which then is U7's result); `MATCHES`, `slot table is as we
intend it`, no `***` line; `parked` says `verified` at the end. Fail: a
`.o` file still (the package build did not take the fix), or anything
U6's second start did not show.

**U6b and U7 results, 2026-10-30 14:16-14:21 UTC.** U6b **pass**: the
views are `monitor.js` and `settings.js` only, settings.js minified
(43856 bytes, 92 lines, no comment lines); the slot found, `MATCHES`,
`slot table is as we intend it`, no `***` line, park read back. (The
script's own log cut came out empty: it marked the system log by line
count, which a full ring buffer does not move; read by time instead.)
**U7 answered its question the other way:** the start measured offset 6
(1533 idle lanes), found nothing at 6 or 14 in four passes, measured
again and got 0 (1024 idle lanes, 512 at 8: the chip's output still
changing), then 14 (1033), and found the slot at pass 9 **at offset 6,
the first measurement**, 66 s after the start. The offset had not
moved; the chip answered late. Measuring again while the chip starts
only picks wrong offsets, so `e1a4cbf` is reverted, and the SSC search
gets twenty passes (~140 s) instead of ten. Start-to-slot times on the
7412 that day: 58 s (first start), 57 s (U1, after an install), 17, 19,
2 s (U2-U3 restarts), 6 s (U5), more than 68 s (U6, after an install),
17 s (its restart), 66 s (U6b, after an install). The slow ones are the
first start after an install or a long pause; why the chip then takes a
minute is open.

### U8, the 7412 on `639fa67` (written 2026-10-30 night, before the run; D5)

Unattended, as T13. The packages from `639fa67` (lantiq, OpenWrt
25.12.6): dect-stubd with the twenty-pass slot search (`ed07be3`), the
cell fixes (`8aa6685`), the cell's media (`6c910b1`) and renewal
(`639fa67`). The box starts parked, service stopped, role base.

| step | pass | fail and what it means |
|---|---|---|
| U8a. Install openDECTcell, opendectcell-baresip, opendectcell-cell, opendectcell-carddav, luci-app-opendectcell; `ls /etc/init.d/*.apk-new` | installed; no `opendectcell.apk-new` for the init script (the box's init is the packaged one) | an init `.apk-new`: the kept script is not the packaged one; move it over before U8b |
| U8b. The first start after the install (the slow case `ed07be3` is for) | `control slot found` within the search; the seconds from start to slot written down (57-68 s expected after an install) | `gave up after 20 passes`: the chip took longer than ~140 s, or the offset moved after all |
| U8c. Five minutes in base mode, then stop | no resync storm, no `***` line; `parked` says `verified` | a resync storm: the SSC collectors (kmod `e3c898d`) are not loaded; park not verified: the hold message instead |
| U8d. Cell mode against `tools/cell-testpeer.py` on the PC (192.0.2.146:7300), `--mail-test`, two minutes | enrolled; the peer logs `HELLO` with caps `0x77` (MAIL, MEDIA, SRTP, STATS, FLOW, RESUME) and formats `[6, 1]`; heartbeats with no reconnect; the memory read answered (`MAIL_UP 0xFFF1`); `cell-link.json` has `cert_expires` about 90 days ahead; `cell.json` has `"radio":"parked"` and `"media":0` | no link: the connect path of `8aa6685` (own TCP connect, handshake reads) fails on the box: then the old binary's behaviour is the comparison; caps without MEDIA: the old dect-cell is still installed; a reconnect within the two minutes: the heartbeat path |
| U8e. Leave cell mode (`/etc/init.d/opendectcell cell_leave`, role base), stop | the credentials gone, role `base`, service stopped, `parked` `verified` | anything else: put the box back by hand and say so |

**U8 results, 2026-10-30 19:59-20:09 CET (box time).**
- **U8a pass:** installed; no init `.apk-new`. apk started the service itself (pid 21697, 19:59:50).
- **U8b pass:** that first start after the install found the slot on pass 3, 18 s after the daemon started. This was not the slow case: the chip answered at once, so `ed07be3`'s longer search was not exercised.
- **U8c pass:** 5 min 40 s in base mode; no resync, no `***` line, `MATCHES`, slot table as intended. Stopped, `parked` `verified`.
- **U8d pass:**
  - Enrolled against the test peer at 20:06:13 and connected 5 s later.
  - `HELLO` showed caps `0x77` and formats `[6, 1]`.
  - The memory read was forwarded and answered (`MAIL_UP 0xfff1`).
  - Over two minutes: no reconnect; `cert_expires` 90.0 days ahead; `"radio":"parked"`, `"media":0`.
- **U8e FAILED: the box hung.**
  - `cell_leave` ran, logged "cell mode left" and "SIP: 1 line(s)", and restarted the service in base mode.
  - About 4 s later the script's `stop` ran. From then on the box answered neither SSH nor ping nor ARP; the 7520 and the router did.
  - It had not come back after more than 6 minutes, so this was no reboot. Its log, in RAM, is lost with the power cycle.
  - What the radio did: the chip was parked in cell mode (at start and by the peer's `0x015B 00`), and the base start reuses the running chip. Restarts find the slot in 2-19 s and the radio goes on only after that, so the hang more likely came before radio-on. That is not verified.
  - **Open, for the bench:** reproduce a base start followed by a stop within a few seconds on the 7412, with `logread -f` on the serial-less box streamed to the PC. Suspects: a stop while the SSC slot search runs (`park_guard` holding the chip in reset under live DMA), or the cell-to-base restart without a chip reset. The maintainer reboots the box.
  - **After the reboot (2026-10-30, about 23:02 CET, by the maintainer):** the box came back and the base started by itself (enabled); left running. What the box itself rules out:
    - **Not memory or logs.** The system log is a fixed 128 KB ring (`log_size`), `/tmp` holds 1.2 MB of 58 MB, 53 MB free with the base running. A userspace out-of-memory would also kill a process and leave ping and ARP answered.
    - **Not a kernel crash, not a full freeze.** `panic_on_oops=1` and `panic=3` reboot the box 3 s after an oops; the hardware watchdog (`watchdog0`, 30 s, fed by procd) reboots it when everything stops. Neither happened in three hours, so the kernel and procd ran on while the network was gone.
    - **So:** a driver stuck in a way that took the Ethernet path down while the rest ran (ours, stopped in the middle of its start-up, or the switch's), or something that brought the interface down. A guess, not a finding.
    - **No record survives:** no pstore on the 7412, the log only in RAM. The boot log starts at 20:08:55, the mtime of the newest file in `/etc` (OpenWrt's `sysfixtime`, no clock battery): `cell_leave`'s config write, so nothing in `/etc` was written after it.
  - **To investigate later (U8f, attended or not):** first point the box's log at the PC (`uci set system.@system[0].log_ip=<PC>; uci set system.@system[0].log_proto=udp; uci commit system; /etc/init.d/log restart`, a listener on the PC writing to a file with the run's time in its name), so the log outlives a hang. Then, outcomes written before each: (1) `cell_leave` and a stop 4 s later, as U8e; (2) a plain base start and a stop at 2, 4 and 8 s, without cell mode; (3) the same with the radio already on. Hang in (1) only: the cell-to-base path; in (2): a stop during the slot search; the last lines before the silence name the step. Restore `log_ip` afterwards.

  - **U8f prepared (2026-10-31, nothing run yet).** The 7412's log goes to
    the PC since 2026-10-31 00:07 CET (`log_ip` 192.0.2.146, `log_port`
    5514, udp), collected by `tools/syslog-sink.py` into
    `u8f/7412-syslog-<start time>.log` in the session's scratch folder
    (a new file per start, never reopened). The script is
    `tools/bench-stop-race.sh HOST KNOWN_HOSTS DELAY...`. It needs the
    maintainer at the box (a hang needs a power cycle) and stops at the
    first silence. **Outcomes, written before the run:**

    | run | pass (the box answers after every stop) | hang, and what it names |
    |---|---|---|
    | U8f-1: `bench-stop-race.sh ... 2 4 8` (base start, stop after 2, 4, 8 s; the chip was parked by the stop before) | answers after each; `parked` present at each | silence at 2 or 4 s: a stop during the slot search (`park_guard` holding the chip in reset while the search is running, or the daemon's stop path in that phase); the sink's last lines show which step; silence at 8 s only: the radio-on or the first mails |
    | U8f-2: `RADIO_ON=1 ... 2 5` (radio already up, then stop) | answers (this is U1's stop, passed on 2026-10-30) | silence: the stop under traffic is the trigger; the lines before it say what the daemon was doing |
    | U8f-3: `cell_leave` then a stop 4 s later, as U8e (needs the peer, `tools/cell-testpeer.py`) | answers | silence only here: the cell-to-base path without a chip reset |

    Falsifying first: if U8f-1 and U8f-2 both pass ten times (five each),
    "a stop during the slot search" is out and U8f-3 is the only open case.
    The park guard's reset line (`gpio-xway` 33) has held the chip in reset
    on this box before (A14, 2026-10-28), so that path alone does not
    explain a hang.

    **U8f results (2026-10-31, 09:25-09:33 CET, 7412, maintainer at the box;
    run files `u8f/run-U8f-1*.txt`, `u8f/run-U8f-2*.txt` in the session scratch
    folder, log in the sink file):**

    | run | result |
    |---|---|
    | U8f-1 (`2 4 8`, then `2 3 2 4 3`) | **8 stops, the box answered after every one.** The parked marker was absent after every one (the stop came while the chip was being brought up; the daemon logged "the park was not confirmed: the DECT chip is held in reset (GPIO 545)"). My table expected `parked` present; that expectation was wrong for a stop before the radio is up, and is by design (the next start uploads the firmware again) |
    | U8f-2 (`RADIO_ON=1`: `2 5`, then `2 2 5`) | **5 stops, answered after every one, `parked` present after every one** |
    | falsifier | **superseded by U8f-1c below**: both reached five passes, but only three of those stops were early; the early series then hung |

    **Weak point of the instrument:** the script counts the delay from the
    return of `/etc/init.d/opendectcell start`. After an unparked stop that call
    takes 13-17 s (firmware upload), so only the first stop of each U8f-1
    batch (marker present before, start returned at once) landed 3-4 s after
    the start began, in the slot search. The other six landed about 3-4 s
    after the chip was up. Two early stops are not a series.

    **U8f-1c: HANG reproduced (2026-10-31, 09:36:14 CET, 7412).** Outcomes
    written before the run: pass = the box answers after each early stop;
    hang = a stop 2 s after a quick start (parked marker present before the
    start, so `start` returns at once and the stop lands in the slot search)
    is the trigger. Run file `u8f/run-U8f-1c-20261031-093431.txt`.

    | quick-start stops (marker present before) | result |
    |---|---|
    | U8f-1 delay 2 (09:25:50), U8f-1b delay 2 (09:29:21), U8f-1c cycle 1 (09:34:43) | answered; "control slot not found", then "park not confirmed" 2-3 s after the stop |
    | U8f-1c cycle 2 (start 09:36:09, stop 09:36:13) | **hang: 1 of 4 quick-start stops** |

    What the sink shows for the hang (pid 19062): the last daemon line is
    `ssc: lane bit offset 14` (09:36:11); baresip logged "terminated by signal
    15 / ua: stop all" at 09:36:14.73, the last line of any kind. Missing, which
    every passing early stop logged within 1-3 s: `control slot not found`, the
    kernel's `DMA: longest gap between ring runs ...` (printed by
    `ssc_engine_stop()` after `hrtimer_cancel()` and before `ssc_dma_stop()`),
    and the init's `park was not confirmed`. The box then did not answer ping
    (`No route to host`, i.e. no ARP answer) for six minutes, until the
    maintainer's power cycle (first boot line 09:42:20 by the PC's clock). **The
    procd watchdog (`ubus call system watchdog`: running, timeout 30 s,
    frequency 5) did not reset it.**

    Not the discriminator: the lane offset. Of the 18 starts in the log, offset
    14 also occurred in two passing ones (09:30:19, 09:30:59); the offsets are
    scattered over 0-15.

    The signature is U8e's (network gone for 6+ minutes, no reboot, watchdog
    silent), so the U8e analysis above stands: the kernel and procd ran on
    while the Ethernet path was gone, not memory. What is new: U8e's first
    suspect, **a stop while the SSC slot search runs, is reproduced** (1 of 4
    quick-start stops; one hang is not a rate), and **the cell-to-base path is
    not needed** (no cell mode here). U8f-3 stays as a check, no longer as the
    only explanation.

    Still open:
    - **What takes the network down.** The kernel's line from `ssc_engine_stop()`
      (after `hrtimer_cancel()`, before `ssc_dma_stop()`) is missing; the box's
      own log cannot say more, since a log line leaves over the interface that
      is going. Paper check of `kmod-lantiq-ssc-pcm/src/ssc_pcm_hw.c`:
      `ssc_engine_stop()` is not called under `engine_lock`, so there is no
      plain two-CPU deadlock between the stop and the ring timers. Candidate
      to look at next: `ssc_dma_stop()` closing the two `ltq_dma` channels and
      freeing the rings two frame times later, on the DMA controller the
      Ethernet driver also uses.
    - **The U8e conclusion assumes the hardware watchdog works on this
      board; that was never shown.** W-1 shows it: service stopped and parked,
      `ubus call system watchdog '{"stop":true}'`, expect a reboot by itself
      within about 60 s. If it does not reboot, the box needs a power cycle,
      so the maintainer should be there.
    - **Next series (maintainer present):** the same quick-start stops with
      `ping -D` running on the PC (does ping stop before or after the last
      log line?) and a step marker in the module's stop (`dev_info` before and
      after `hrtimer_cancel`, the DMA register write, `ltq_dma_close` and
      `ssc_dma_free`), so the last line that arrives names the step.
    - The box was parked after the hang (stop from radio up, marker present),
      log forwarding to the PC left on.

    **U8f-1d: the hang is after the park mails, not in the module's stop
    (2026-10-31, 11:38-12:16 CET, unattended, the 7412 power-cycled through
    the Tasmota socket).** Outcomes written before the runs: last line
    inside a `dma_stop:` marker = the death is in `ssc_dma_stop`; at
    `teardown: done` / `release done` = after the module; silence before
    `engine_stop: enter` = not the module's stop at all.

    | what | result |
    |---|---|
    | the instrument | `kmod-lantiq-ssc-pcm` with `u8f:` printks in start, stop, DMA stop, teardown and release (built with the lantiq SDK in the kmod repo, not committed; the box's original module is `/root/lantiq_ssc_pcm.ko.e3c898d.orig`) |
    | a second hang, on the original module, an ordinary stop from radio up (11:38:52) | the last daemon line is `reg: radio parked -- park mails on the wire 1 ms after queueing`; no `park verified`; the box silent, no power cycle for 3 minutes |
    | campaign, 15 cycles, 30 stops (A: stop from radio up, B: quick start and a stop 2-4 s later) | A 15 pass; B 14 pass, **1 hang** (10:59:36.76 UTC). In the hang the last daemon line is again `radio parked`, and **no kernel marker follows** (no `release enter`, no `teardown`, no `engine_stop`): the module's teardown was never reached. A passing stop logs `park verified on the chip` 65 ms after `radio parked` and the teardown 20 ms after that |
    | ping (`-i 0.5`, PC) | last reply 36.44, the `radio parked` line still reached the sink at 36.76, unanswered from about 36.94: the network died within 180 ms of the park mails, while the SSC engine was still running |
    | `radio parked ... N ms after queueing` | not the discriminator: 1 ms in seven stops (six passed), 2 ms in four (three passed); the hang is only the missing `park verified` |
    | **W-1, the hardware watchdog** (`ubus call system watchdog '{"stop":true}'`, service stopped) | **works**: the box vanished after 30 s and was back by itself after 106 s (uptime 58 s) |

    **Reading.** The watchdog works, so in the hangs procd kept feeding it
    for six minutes: the kernel and userspace ran on and **the network path
    was dead**, as the U8e analysis said. The earlier "H1, a freeze inside
    `ssc_engine_stop()`" is **out**: the teardown was never reached. The
    start of the death is between the park mails and the first chip read
    after them, with the engine running. What is not known is what in that
    window takes the Ethernet down (`xrx200_net` on IRQ 72/73 and the same
    DMA controller as the SSC rings).

    The first hang (09:36, a stop 3 s after a quick start) died before the
    radio was parked, so there may be more than one entry into the same
    state; one sample each.

    **Later the same day (unattended).**

    | run | result |
    |---|---|
    | campaign v2 (`netguard.sh` on the box, 10 cycles) | **hang 4**: an ordinary stop from radio up (12:34:45). The guard's `eth0` down/up and `network restart` did not bring the network back in 8 minutes; its RAM evidence was lost with the power cycle (the guard now copies it to `/root/hangevid` before the remedies) |
    | campaign v3 (the same, evidence to flash, 40 cycles, 80 stops: A 40 pass, B 40 pass) | **no hang** |
    | total | four hangs in about 190 stops (two ordinary stops from radio up, two quick-start stops), none in the last 80. One hang is not a rate; nothing explains why the last 80 passed (the diagnostic module and the guard ran in v2 as well). The `ssh` poller that ran in the earlier runs was off from 12:17 |

    The cause is still open. What is established: the watchdog works and did
    not fire, the module's teardown is not reached, the network dies within
    about 180 ms of the radio park mails, and `eth0` down/up does not revive
    it. A run with the maintainer watching the 7412's LEDs, and the evidence
    file from the next hang, are the next steps.

    **T-A on the 7412 (06.88, 14:23-14:38).** Same method (keys of the two
    stores swapped through slot 2, `keycheckms=N`, two repeats each way, no
    void): N = 0: no answer, 4 of 4; N = 20 to 200, 1000, 1500: MATCHES, 24 of
    24; N = 400 and 800: MATCHES 6 of 8, no answer 2 of 8; never a MISMATCH. A
    key write is effective within 20 ms on 06.88 as on 08.25; a read-back may
    occasionally get no answer and should be repeated once. The 7412 runs the
    keycheck daemon now (`/root/dect-stubd.pre-keycheck-20261031` is the
    packaged one) and is stopped.

    **T-A, key-write delay (lab 7520, 08.25, chip warm).** Method after two
    void designs (identical rewrites; a store glob the init rejected): two
    different handsets' keys swapped through slot 2 (`subslot=2`), the
    read-back after `keycheckms=N`. N = 0: no answer, 6 of 6. N = 20, 50,
    100, 200, 400, 800: MATCHES, 36 of 36 (three repeats, both directions,
    no void). A key write is effective within about 20 ms; the first daemon
    run after a fresh chip upload reads back nothing. A slot clear leaves the
    key bytes readable 1 s and 5 s later (Q-0073).

### T13 on the lab 7520 (written 2026-10-30, before the run)

The maintainer's word (2026-10-30): test the 7520 and keep the new build
running. It ran the 0.3.0 packages with the radio on; its handsets will
meet the moved stack when it restarts. Packages from the newest commit
(the T13 fixes included), ipq40xx, OpenWrt 25.12.5.

| test | pass | fail and what it means |
|---|---|---|
| V1. Install, start, ten minutes running | as U1: control slot, a `sub: restored subscription` per store, the slots cleared, the mask; the same pid after ten minutes; no trouble lines, no respawn; baresip registered as before (`logread -e baresip`) | as U1; baresip not registered: the bridge's start (not moved) or the config kept from 0.3.0 |
| V2. The handsets come back by themselves | within the ten minutes each handset that was registered relocates: `reg: connection up`, its `sub: now serving` if not record 0, no `LOCATE-REJECT`; `cap: record N reports ...` once proven | a handset that located on 0.3.0 now gets `LOCATE-REJECT` or keeps searching: the identity gate, `dect_base_main_segment()` or the restore; then put 0.3.0 back at once |
| V3. Left running | dect-stubd up at the end, radio on, as the maintainer asked; not parked | -- |

### T13 results, lab 7520, 2026-10-30 13:26-13:36 UTC (unattended)

Packages from `d1cdd37` (`build/release/out-d1cdd37-ipq40xx`); the 0.3.0
daemon is kept as `/root/dect-stubd.0.3.0`. apk kept two edited files:
`/etc/config/opendectcell` and **`/etc/init.d/opendectcell`** (both beside their
packaged versions as `.apk-new`). The kept init script predates
`1c081ee` and records the loaded STUB in `/tmp/dect-stub.second`. Log on
the box: `/root/t13-v-20261030-132618.log`.

| test | result |
|---|---|
| V1 | **pass, with one false alarm explained**: the apk post-upgrade restarted the service, then the script did; control slot at once (the kept init found its STUB record and did not boot the chip again); `sub: restored subscription` for slots 1, 3, 2, the others cleared, mask `00 07`; the same pid after ten minutes; no respawn; baresip `200 OK [1 binding]`. Two trouble lines: `UAK READ-BACK MISMATCH slot 1` and `3 FOREIGN SLOT(S) STILL HELD`, both with `STUB second stage unknown (no /var/run/opendectcell/stub.second)`. Since `1c081ee` the daemon reads only the new path, so it guessed 08.25 and read that build's table address on a 07.56 chip, where 0.3.0 had logged `slot audit skipped -- STUB dectfw_secondlevel_441-07.56.hex`. The audit reads only, so nothing on the chip changed. Not a stack-move regression: an upgrade gap of `1c081ee` |
| V2 | **not shown**: no handset located in the ten minutes, and none had to. The chip kept running across the restart, so the handsets stayed locked and never searched. No `LOCATE-REJECT`. A handset's own action (power-cycle, a call) still has to show it: V2 below |
| V3 | **pass**: dect-stubd up at the end (pid 23227), radio on, not parked, as asked |

Left for the bench (added to the attended list): **V2b**: switch a 7520
handset off and on, or call out on it. Pass: `reg: connection up` and
the call works. Fail: `LOCATE-REJECT` or no service; then put 0.3.0 back
(`/root/dect-stubd.0.3.0` over `/usr/sbin/dect-stubd`, restart).

**V1b** (written before the run): the daemon also reads the old record,
read-only and root-owned only, so a kept old init script still tells it
the build. Its binary goes over `/usr/sbin/dect-stubd` (the `d1cdd37`
one is kept as `/root/dect-stubd.d1cdd37`), then a service restart.
Pass: `chip: STUB second stage dectfw_secondlevel_441-07.56.hex` and
`slot audit skipped`, with no MISMATCH or FOREIGN lines; the same
restore lines as V1; still running five minutes later. Fail: `unknown`
again means the fallback never reached the file (owner or mode); trouble
lines with the build named mean an audit gap other than the record.

**V1b result, 2026-10-30 13:40-13:45 UTC: pass.** `chip: STUB second
stage dectfw_secondlevel_441-07.56.hex`, `slot audit skipped`, no
`***` line; slots 1, 3, 2 restored, the others cleared, mask `00 07`;
baresip `200 OK [1 binding]`; the same pid five minutes later. Log:
`/root/t13-v1b-20261030-143958.log`. The box is left running the
`51a5f95` dect-stubd (over the `d1cdd37` package's binary; the next apk
upgrade replaces it), radio on, as asked.

The kept `/etc/init.d/opendectcell` on the 7520 is byte for byte the
committed `00741ff` version, not a hand edit: apk kept it only because
it had been copied over the 0.3.0 package's file. Taking
`/etc/init.d/opendectcell.apk-new` loses nothing and brings the park guard
(`1c081ee`) and role `cell`; its first start boots the chip afresh (its
record is not in `/var/run/opendectcell` yet). Left for the maintainer, as
it reboots the radio.

### V1c: the lab 7520 gets the packaged init script (written 2026-10-31, before the run)

The box has run the daemon of `d1cdd37` under its kept 2026-10-28 init
script (`/etc/init.d/opendectcell`; the package's own is
`opendectcell.apk-new`, from 2026-10-30 14:22). The maintainer's word: swap them
("the 7520 is an experimental box as well"); the radio goes away about a
minute while the chip reboots. Only the init script changes; the daemon
stays `d1cdd37`. The kept script is copied to
`/root/opendectcell.init.2026-10-28.kept` first (never overwritten).

| test | pass | fail and what it means |
|---|---|---|
| V1c-1. The new init starts the daemon | after `restart`: a dect-stubd with a new pid, the chip firmware loaded, the control slot found, one `sub: restored subscription` per store, the slots cleared, the mask, the radio on within three minutes | the daemon dies or refuses an option in the log: the init passes an option `d1cdd37` does not know (a daemon older than the init); roll back at once: copy the kept script back, `restart` |
| V1c-2. The same options as before | the command line still has `-P`, the five `-s` stores, `dialwait=3000,regwindow=120` and the LED; the new script may add more options and the daemon accepts all | an option missing: the uci keys are read differently now; compare with the kept script before accepting |
| V1c-3. The handsets come back | each handset that was registered relocates by itself within ten minutes (`reg: connection up`, no `LOCATE-REJECT`) | `LOCATE-REJECT`: the record store path or the STUB record; roll back |
| V1c-4. Left running | dect-stubd up at the end, radio on | -- |

**V1c results, 2026-10-30 23:58-00:08 CET (box time), by hand, lab 7520.**
The old init is kept as `/root/opendectcell.init.2026-10-28.kept` and the
packaged one as `/root/opendectcell.init.apk-new.2026-10-30`.
- **V1c-1 pass:** new pid 1915 at 23:58:44; the chip's first pass missed
  the control slot, the second found it (23:58:47); three
  `sub: restored subscription`, the other slots cleared, the mask
  `00 07`; `state.json` says `"radio":"up"`.
- **V1c-2 pass, with changes:** `-P @/var/run/opendectcell/pin` (the PIN no
  longer on the command line), `-a mask=0x0002,codec=3,status=8000,dialwait=3000,regwindow=120,timeupdate=1440,led=...`.
  Five `-s` stores as before. Baresip registered (`200 OK`) and
  connected to the bridge.
- **V1c-3 not shown:** no handset connected in the eight minutes watched
  (`link:false` for all three), and none had connected since 2026-10-29
  16:09 (the oldest line in the log), so none had to. Needs a handset's own
  action: V2b.
- **V1c-4 pass:** dect-stubd running, radio up, left on.
- The daemon is still `d1cdd37` (API 136); only the init changed.

## 1b. M1: the cell's media on air (written 2026-10-30, needs C3)

The controller places or takes a call through a cell (the 7412 in cell
mode, `dect-cell` enrolled) once its C3 host drives calls. Built and tested
at the desk only (`tests/test_stubd_cell.c`, `make check-cell`); the
sources are opendectcell-controller cell-protocol.md §8 and
[notes-for-the-controller.md](notes-for-the-controller.md), "The cell's
media is built".

| test | pass | fail and what it means |
|---|---|---|
| M1a. A wideband call, handset to the controller's SIP peer | the cell's log: `cell: media N: channel 1 on handle H, lane 0 (TDM word W, G.722), G.722 on the socket`, then `media N up: G.722, 20 ms, SRTP AEAD_AES_128_GCM` (or the profile the controller picked); speech both ways, clean | `no lane for channel C (no long 0x0242 ...)`: MEDIA_OPEN came before the chip's answer to the bind, or named another nibble; `(its bind's mask names none)`: the 0x0242 mask is not one bit 1-3. Speech one way only: the 7412's lane numbering (control lane + k) is wrong for the cell's channel; noise: the lane is narrowband but the bind said G.722, or the reverse (the 0x0242's bit 6) |
| M1b. A narrowband call (`-a narrowband` on the controller's side, or a G.711-only peer) | `... (TDM word W, narrowband), 8 kHz linear on the socket`; speech both ways | noise: byte order on the 7412 (big-endian MIPS: host order on the socket, network order on the wire, `media_up()`/`media_down()`) |
| M1c. Hang up at either end | `cell: media N closed (requested)` or `(the chip released the channel)`, and the controller's MEDIA_CLOSED; `"media":0` in `/var/run/opendectcell/cell.json` | a session left open: the release path missed `0x0201` on the handle |

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
