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

**Open: the control channel slips.** In three of six runs the chip went
silent on the SSC link after 23 to 60 s (once before any handset link),
the session died, and the stop's park could not be verified. The radio
kept running (the A540 showed signal, "Synchronisiere"), and restarting
the daemon alone, with no upload, found the control slot again at once:
the chip is alive and the PCM/TDM alignment was lost.

Measured the same evening with the alignment watch (`563b080`): the chip
moves its PCM frame by whole 16-bit lanes against a steady bit clock --
by 8 lanes about 10 s after radio-on, which is when the control channel
stopped, and by -2 lanes two minutes later. The driver lost nothing
(overruns, dropped blocks and TX underruns all stayed at 0) and the frame
rate held at 8000/s, so it is not the box losing data and not a clock
change. This fits the clean room's account [CR A-0012] that the chip owns
both the bit clock and a frame sync (FSC, gpio9) and that AVM's host
re-locks to the chip continuously, but not its guess of a single
half-word step. What moves the frame is asked in Q-0013. Until the daemon
re-hunts the slot on silence, a 7412 whose session died must be put in
reset by hand after the stop (the "After the tests" block above); the
service's stop does not reset the chip.
