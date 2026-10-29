# Bench card: the first handset on a FRITZ!Box 7412 (written 2026-10-29)

For the maintainer, at the 7412 (`root@192.168.1.1`) with one Gigaset. Two
tests, about 15 minutes. On 2026-10-29 the service ran on the 7412 without a
handset: STUB upload, control channel, bring-up, radio-on and park all worked
([7412-spi-tdm-and-loader.md](7412-spi-tdm-and-loader.md) §1b). Whether the
radio really beacons can only be seen from a handset. The 06.88 STUB needs its
radio switched off before the bring-up; the daemon does that since `143c5fd`
(command check, same doc §1b).

**Before anything:** the box runs openDECTcell `c9d16f6` or later. `md5sum
/usr/sbin/dect-stubd` should print `ed610ef2e1fc990c67e9737d2f3df7a7`; the
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
