# Bench card: the first handset on a FRITZ!Box 7412 (written 2026-10-29)

For the maintainer, at the 7412 (`root@192.168.1.1`) with one Gigaset. Two
tests, about 15 minutes. On 2026-10-29 the service ran on the 7412 without a
handset: STUB upload, control channel, bring-up, radio-on and park all worked
([7412-spi-tdm-and-loader.md](7412-spi-tdm-and-loader.md) §1b). Whether the
radio really beacons can only be seen from a handset. The 06.88 STUB needs its
radio switched off before the bring-up; the daemon does that since `143c5fd`
(command check, same doc §1b).

**Before anything:** the box runs openDECTcell `dc28a48` or later. `md5sum
/usr/sbin/dect-stubd` should print `206982939b00b3b54bf0fd9cd6aad2ed`; the
modules `lantiq_ssc_pcm.ko` and `opendectcell.ko` are in `/lib/modules/6.12.112/`.
The 0.3.1 files are kept in `/root/dw-orig/`. The chip is held in reset
through sysfs; release it first:

```
echo 545 > /sys/class/gpio/unexport
```

A Gigaset can be registered to up to four bases, so registering it to the
7412 as an additional base keeps its 7520 registration.

## 1. The handset sees the base and registers (10 minutes)

```
uci set opendectcell.main.enabled=1; uci commit opendectcell
/etc/init.d/opendectcell start
logread -f -e dect-stubd     # wait for "all 5 extra mails sent", then Ctrl-C
uci get opendectcell.main.pin     # the registration PIN
/etc/init.d/opendectcell register # opens registration for 120 s
```

On the handset: Settings, Registration, Register handset, choose a free base
number, enter the PIN.

What each result means (written before the run):

| You see | Meaning | Next |
|---|---|---|
| The handset registers; the log shows the enrolment and `/etc/opendectcell-sub.1` exists | The 7412 is a working base | Test 2 |
| The handset searches and finds no base | The radio does not beacon on 06.88, although the scan report comes alive after the radio-off-first bring-up (`143c5fd`); the levels it shows were not ours | Send `logread -e dect-stubd`; the agent takes it to the clean room (Q-0008) |
| It finds the base but registration fails | The radio works; the registration path differs on 06.88 | Send `logread -e dect-stubd` |

## 2. Location registration after a restart (3 minutes)

Only if test 1 passed.

```
/etc/init.d/opendectcell restart
logread -f -e dect-stubd
```

Press a key on the handset. Expected: it finds the base within a minute and
the log shows a location registration of slot 1. Note: the 06.88 slot table
table is at `0x067F` and the daemon reads the key back 1 s after restoring
it: expect "UAK read-back slot 1 at 0x0067F: MATCHES" and an audit line with
`1=OURS`.

## 3. Does the park silence the base? (3 minutes)

Only if the handset found the base in test 1 or 2. The park (`0x015B 00`)
clears the chip's radio switch, but its beacon-carrier byte keeps its value
afterwards, so the chip alone does not tell us whether it stopped
transmitting (Q-0010).

```
/etc/init.d/opendectcell stop     # parks: close registration, radio off
logread -e dect-stubd | grep "park"   # expect "park verified on the chip"
```

Watch the handset for two minutes, without pressing keys.

| You see | Meaning |
|---|---|
| It loses the base ("no base", searching) within about a minute | The park works on the 7412 |
| It stays in range | The park does not stop the beacon on 06.88; only the chip reset below does. Tell the agent: the service's stop then has to reset the chip on this box |

## After the tests

```
/etc/init.d/opendectcell stop
uci set opendectcell.main.enabled=0; uci commit opendectcell
echo 545 > /sys/class/gpio/export
echo low > /sys/class/gpio/gpio545/direction
cat /sys/class/gpio/gpio545/value   # 0: chip in reset, nothing transmits
```

Tell the agent what you saw; it reads the log and records the result here.

## Results, 2026-10-29 evening (A540)

All three tests pass, after two fixes made at the bench. Logs and the
pre-committed outcomes are in the agent's notes for that session.

1. **The radio beacons on 06.88.** The A540 found the base at once and
   opened a link, but nothing reached the daemon: the 06.88 STUB sends
   `0x0203` as 7 bytes `{handle, 01, seg[5]}` (08.25: 13 or 15, with tag
   and handle copy), and the decoder dropped them. Fixed in `d917c9b`.
2. **Registration** then ran through (DSAA, "handset registered"), but the
   handset's location registration was rejected "authentication failed":
   a regression for every fresh registration since `e05fd56`, on both
   boxes. Fixed in `3907ee2`. With both, the A540 registered, the store was
   saved, `UAK read-back slot 1 at 0x0067F: MATCHES`, audit `1=OURS`, and
   it ran a ciphered list session (its name "DECT 1").
3. **Test 2:** after a restart it came back on its own within 6.4 s, the key
   read back MATCHES. **Test 3:** the park was verified on the chip and the
   A540 lost the base within two minutes: the park silences the 7412.

**The control channel slipped (fixed, see below).** In three of six runs the chip went
silent on the SSC link after 23 to 60 s (once before any handset link),
the session died, and the stop's park could not be verified. The radio
kept running (the A540 showed signal, "Synchronisiere"), and restarting
the daemon alone, with no upload, found the control slot again at once:
the chip is alive and the PCM/TDM alignment was lost.

Measured the same evening with the alignment watch (`563b080`): each loss
is the chip's frame moving by whole 16-bit lanes in our stream (8, 12, 20
lanes), against a steady bit clock and with the SSC's overflow flags
clear. At first that read as the chip moving its frame [CR A-0012, A-0013],
but the SSC's flags cannot see words lost in the DMA ring, so it was not
evidence either way.

**Cause: the box, not the chip.** A ring-health check in the driver
(kmod-lantiq-ssc-pcm `0000455`) showed the next loss coming with its 1 ms
DMA collector held off for 27.5 ms, longer than the 24 ms ring: the
receive ring ran full and the send ring ran empty. Soaks with the chip
held in reset (no radio) found a 26-27 ms interrupt blackout every 120 s;
it went away with `ubihealthd` (OpenWrt's periodic flash check) stopped
and came back when it was started again. The blackout lands on whichever
CPU `ubihealthd` runs on, so pinning the timer to one CPU (`6a457d8`)
failed in a full run. The driver now runs one collector per CPU
(`e3c898d`): a 15-minute run with the radio on and `ubihealthd` running
kept its control channel throughout (longest ring gap 1.01 ms, session
never lost, park verified), and a soak shows the blackouts still come and
are covered ("timer on cpu 1 held off 27 ms"). Whether the chip also moves
its frame on its own (Q-0013) is not needed for any loss seen so far; the
clean room's frame-sync facts [CR A-0014] (FSC as EXIN5, interrupt line
42) stay on file for a frame-sync alignment later.

A 7412 whose park could not be verified had to be put in reset by hand
after the stop (the "After the tests" block above). Since 2026-10-30 the
service's stop does it (`park_guard` in the init script, the profile's
`reset_chip`/`reset_line`): dect-stubd leaves `/var/run/opendectcell/parked`
after a confirmed park, and without it the stop holds GPIO33 low and the
next start uploads the firmware again. Untested on the box: test A14.

## First calls, 2026-10-29 evening (A540, SIP through baresip)

Signalling worked both ways at once; audio did not, either way: the daemon
used lane 0 for speech, the 7520's layout, which on the 7412 is whatever
lane the alignment put first, and it used one lane index for both
directions although the control lane sat at different indices in received
and sent frames. Since `d1d2c8f` the receive stream is delayed by whole
lanes so both directions share one numbering, and the speech lane is the
control lane + 1 [CR A-0015]. The next call had clean wideband audio both
ways. During it, lane +1 changed 4000 times a second (a G.722 word held
for two frames, as on the 7520), lanes +2 and +3 stayed idle (one lane per
call), and lane +4 (0xD5D5 when idle) changed about 2000 times a second:
not explained, no effect on the call.

An incoming call (19:09) had audio both ways as well. A C430A registered
as DECT 2 (located and saved: `3907ee2` holds for a second model). An
internal call A540 -> C430A first rang for a moment and stopped: the
target's link never adopted the paged handset, so the authentication
every call now needs found no key (`4b08aff` fixes it; the 7520 had the
same fault). With `-a twocalls` and the side lanes counted from the
control lane (`03d4107`), the next internal call had audio both ways:
lane +1 (A540, channel 1) and +2 (C430A, channel 2) each changed 4000
times a second, so call *n* is in control lane + *n* [CR A-0015].

Open after this run: two external calls at once (the SIP bridge carried
one call), narrowband over SIP (the bridge was wideband only), the A540's
call timer on the internal call. The timer: stock gives a Part 3 caller
the line id and type on an internal call too, from CALL-PROC on, and the
A540 times a call only with them [CR 7412 A-0016, capture 14]; we sent
the call id and status alone (`e9e767b` adds the line). Base-drawn
timers are for Part 1 handsets only. The other two are built in
`07a57dc`; card below.

## Card: several calls, shared line, narrowband (2026-10-29 evening)

Box at `07a57dc` (dect-stubd md5 `67d4b1e4`, `opendectcell.so` md5 `696ccc03`,
hand-copied) with the init script of `ead1a36`; `voip1` is Line 1 of
DECT 1 (A540) and DECT 2 (C430A). Outcomes written before the run:

| test | pass | fail and what it means |
|---|---|---|
| 1. Call the number from outside | both handsets ring; answer on one, the other stops at once; audio both ways | only one rings: the bridge's second leg (`opendectcell: call from ... rings 2 handset(s)` missing) or the side ring (`bridge: paged DECT n`); the other keeps ringing: `remote-released` for the losing leg |
| 2. While 1 is up, the other handset dials a number outside | second SIP call connects, audio both ways on both calls | `bridge: 3 calls already` or `busy`: the call table; no audio on one: the lane (`audio: call N's speech on ... lane 2`) or the device name (`audio source "..." is no call of ours`) |
| 3. A540 in a call, call the number again from a second phone | the C430A rings (side ring) and can take it | busy at once: `why=channels` or `in-call` in the log |
| 4. An internal call A540 -> C430A | the A540 shows its call timer | no timer: the line in CONNECT (`7e 0c ... 02 01 85` with `-p`) |
| 5. Wideband off (Network > DECT, Wideband audio), one call out | a clear narrowband call both ways | silence or noise: the linear lane's format on the 7412 (A-0015 says 16-bit linear, one sample a frame) |
| 6. Redial from the A540's list | the call goes out over SIP | a silent local call: `number_complete()` |
| 7. A G.711 call: G.722 off in the VoIP profile's codecs (Media), or a far end without G.722; one call each way | audio both ways | silence: baresip dropping 16 kHz frames for an 8 kHz codec ("Use module auresamp!" in the log); fixed in `2b8e268`, every earlier SIP test negotiated G.722 |

Since the card was written: the code review's fixes, the cleanup of
lab leftovers and the codec matching are on the box (`9aeca07`;
dect-stubd md5 `b3f969f0`, `opendectcell.so` md5 `65a96417`, init script,
LuCI views and ACL of the same commit), and `baresip-mod-auresamp` is
installed. What tests 5 and 7 should now show in the log (`logread`):

| call | expected |
|---|---|
| 5, wideband off, call out | `call-out ... codec=nb`, the INVITE offers G.711 only, `audio: the bridge connected for call N at 8000 Hz -- narrowband, passed as it is` |
| 7, G.722 off in the profile, call out from the A540 | `answered by the far end -- narrowband, G.726 to the handset`, then `at 8000 Hz -- narrowband, passed as it is` |
| 7, a caller without G.722, call in | the page without the wideband flag, `call: codec ... narrowband`, `at 8000 Hz -- narrowband, passed as it is` |
| any G.722 call (1-4) | `at 16000 Hz -- G.722 coded here` (pass-through without coding not built yet) |

Anything "resampled here" or "decoded and resampled" in tests 5 and 7
means the codecs were not matched.


## Card: pass-through, call waiting, DTMF, voicemail, date (for 2026-10-30)

Box at `45b0264` (dect-stubd md5 `f6814ca6`, `opendectcell.so` md5 `618fd5f7`; since 2026-10-30 00:00 dect-stubd from `d174295` (md5 `d3f8953d`, the same base mode plus `-K`), the init script from `b2e1a22` and LuCI from `bfe2f6e`;
`baresip-mod-mwi` installed, `/etc/hotplug.d/ntp/25-opendectcell` in place).
Outcomes written before the run:

| test | pass | fail and what it means |
|---|---|---|
| 8. Any HD call over SIP (the A540, G.722 at the far end) | clean audio both ways; log `connected for call N (G.722) -- G.722 passed as it is` | noise or silence: the octet carrier in the bridge (two samples per octet) or the gap marker; `G.722 decoded here` means baresip did not pick our codec |
| 9. A540 in a SIP call, call the number again | the A540 shows the second caller with the waiting tone; accept: the first goes on hold (log `on hold`), talk to the second; toggle back and forth; end one, the other stays | the second caller gets busy: `waiting_party()` (the call must be bridged and connected, Part 3); the A540 shows nothing: our call waiting {CC-INFO} |
| 10. A540 in a call: second call out (the handset's "external call" / R key, then a number) | the first goes on hold, the second rings and connects | a negative tone: the 1CH 15H path, `par_control()` |
| 11. Call a voicemail or any IVR, press keys | the menu reacts | nothing: `digit` events in the log? if yes, the profile's DTMF mode |
| 12. Voicemail notification on in the profile, leave a message | the A540 shows the message indicator (count); after listening it goes | nothing: `mwi:` lines in the log? the provider must send message-summary NOTIFYs |
| 13. Switch the A540 and the C430A off and on | the A540 shows the date and time; the C430A does not (a Part 1 handset: date and time is Part 3, TS 102 527-3 §7.4.20; the base sends it anyway, log `sent the date and time` for both) | `the clock is not known to be right`: /var/run/opendectcell/time-valid missing or stale |

## Results, 2026-10-30 (A540, C430A; the maintainer at the bench)

The box on the packages of `db173c8` (dect-stubd with verbose logging for the
day, by a hand edit of the init script), the Nextcloud address book of the
maintainer's (388 contacts) as the phonebook.

| test | result |
|---|---|
| 13, date and time | **pass as expected**: the A540 shows the date and time after a reboot, the C430A does not (Part 1; it has no setting for it either). The box ran on UTC until the maintainer set the time zone |
| 1, a call to the shared line (partly) | both handsets rang (the C430A on a side link while the A540's link was busy); hung up before answering, so answering on one and the other stopping is still open |
| Phonebook | **pass on the A540 after a fix.** The A540 kept the List of Supported Lists it had read before the base had a Contact List and re-read only 06 and 08 on reconnect, so it offered no base phonebook. Since `490cf02` a location registration brings a list change indication for list 00; after a power cycle it re-read 00, opened the Contact List (388 entries), and reading worked, a few entries a read, one number per contact requested. Editing on the handset not tried yet |
| Call lists | **pass on the A540** (`db173c8`): a missed call from a mobile rang both handsets; "1 unread of 1 (a new one)"; the message key opened the Missed Calls List (number, time, line "Fritzbox"), read it with "mark read", the update "0 unread" followed. The message key lists only new messages: pressing it again shows "Keine Einträge" while the entry stays. Accepted and outgoing lists not checked on the air |
| C430A and the lists | as expected for a Part 1 handset: no base phonebook, its message key shows its own internal list |
| Save & Apply after a hand-made stop | **defect, fixed** (`cc89757`): with the service stopped by `/etc/init.d/opendectcell stop`, saving in LuCI did nothing, procd had no trigger left; LuCI now starts the service after an apply if procd does not know it |


## Card A: the features of 2026-10-29/30 (written 2026-10-30, not run yet)

Box at `db173c8` or later, A540 (DECT 1) and C430A (DECT 2) with `voip1` as
Line 1 of both, the phonebook on. Gathers what the two cards above left
untested and adds the day's features. Log: `logread -e dect` (calls:,
missed:, bridge:, audio: lines); verbose (`-v`) only for list access.
Outcomes written before the run:

| test | pass | fail and what it means |
|---|---|---|
| A1. Call the number from outside, answer on the C430A | the A540 stops ringing at once; audio both ways; `calls: accepted call from ...` when it ends | the A540 keeps ringing: `remote-released` for the losing leg missing (the module's "answered" handling) |
| A2. While A1 is up, the A540 dials out | a second SIP call, audio both ways on both; `calls: outgoing call to ...` | `bridge: 3 calls already` or `busy`: the call table; silence on one: the lane |
| A3. A540 in a SIP call, call the number again (test 9) | waiting tone and the second caller on the A540; accept, toggle, end one, the other stays | busy at once: `waiting_party()`; nothing shown: our call waiting {CC-INFO} |
| A4. A540 in a call, R key and a number (test 10) | the first on hold, the second connects | a negative tone: the 1CH 15H path, `par_control()` |
| A5. DTMF into a voicemail menu (test 11) | the menu reacts | no `digit` lines: the keypad path; digits but no reaction: the profile's DTMF mode |
| A6. Voicemail indicator (test 12; MWI on in the profile, leave a message) | the A540 shows the count; it goes after listening | no `mwi:` lines: no message-summary NOTIFY from the provider |
| A7. Wideband off, one call out (test 5) | clear narrowband both ways; `at 8000 Hz -- narrowband, passed as it is` | silence or noise: the linear lane's format on the 7412 |
| A8. Redial from the A540 (test 6) | goes out over SIP; `calls: outgoing call to ...` | a silent local call: `number_complete()` |
| A9. Phonebook: change a contact's first name on the A540 | Nextcloud shows it within seconds; the first line of `/var/run/opendectcell/contacts` counts `seq=` up | `carddav: change N (put) not written`: the server refused (`docs/phonebook.md`, Limits); nothing at all: the save was refused on the air (`c3 12` with `-v`) |
| A10. Phonebook: a new contact, then delete it, on the A540 | it appears in the default book, then is gone | as A9 |
| A11. Call lists: open the A540's call list menu after A1/A2 | accepted and outgoing calls listed with time and line | an empty list: the lists the A540 asks for (`c3 00 0x` with `-v`); the All Calls List (04) is served since API 88: a box older than that does not offer it |
| A12. Delete the missed call on the A540 | gone on the A540; `missed: DECT 1, line 1: 0 unread of 0 (update)` | a refusal (`c3 12`) or no update line |
| A13. Box at `0d9ffb2` or later: with entries in the lists, `/etc/init.d/opendectcell restart`, then open the call lists on the A540 | the same entries; `calls: N missed, N outgoing, N accepted from /var/run/opendectcell/calls` at the start | empty lists and no `calls:` line: the file was not written (`ls -l /var/run/opendectcell/calls`, mode 0600) |
| A14. Box with the park guard: `kill -9 $(pidof dect-stubd)`, at once `/etc/init.d/opendectcell stop` | `logread`: "the park was not confirmed: the DECT chip is held in reset (GPIO 545)"; `cat /sys/class/gpio/gpio545/value` 0; the A540 loses the base within two minutes; a `start` afterwards uploads the firmware and the A540 finds the base again | "RADIO MAY STILL TRANSMIT": the reset line not found (`cat /sys/class/gpio/gpiochip*/label`); the A540 keeps the base with value 0: GPIO33 is not the reset; procd respawned the daemon before the stop: run the stop again (the park record then decides) |

After the run: the radio parked and read back, the hand edit of the init
script undone (verbose off).
