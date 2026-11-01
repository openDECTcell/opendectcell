# Bench card: gates G6 and G7 on a FRITZ!Box 7362 SL (2026-11-01)

Gate G6 (`ROADMAP.md`): **a handset registers.** Gate G7: **it rings, is
answered, and audio passes** (tone or echo). The radio transmits: run with
the maintainer's agreement and a handset at hand (given 2026-11-01). Run
as `/tmp/g6.sh` and `/tmp/g7.sh` on the box after G5
([bench-7362sl-g5.md](bench-7362sl-g5.md)), same modules and firmware,
`dect-stubd` from `d697936` (MIPS build), in base mode (`-g`), which sends
stock's bring-up. Before the daemon each script records the STUB build in
`/var/run/opendectcell/stub.second`, as the service does, so the daemon knows
it runs 07.18 (it then skips the key read-back and slot audit, whose
addresses are known only for 08.25 and 06.88). The handset store is a new
file in `/tmp` (it holds the handset's key; never shared, gone at reboot).

## G6: registration

    dect-stubd -d /dev/tdm-pcm0 -c 8 -v -g -t 300 -P PIN -s /tmp/g6-sub \
        -a regopen=300,led=/sys/class/leds/green:dect/brightness

A random 4-digit PIN, given to the maintainer, who started registration on
the handset. *Observed on hardware:* 11 s after the radio came on the
handset's access-rights request arrived, it proposed DSAA with the
authentication code, the base ran DSAA from the PIN, and "handset
registered"; the subscription went to chip slot 1 and into the store, the
other chip slots were cleared. Window closed after 300 s, the daemon
parked on exit. **G6 passed.**

## G7: ring and echo

    dect-stubd -d /dev/tdm-pcm0 -c 8 -v -g -t 240 -s /tmp/g6-sub \
        -a ringattach=5000,loopback,led=/sys/class/leds/green:dect/brightness

Registration closed; the stored handset reloaded into the chip.
*Observed on hardware:*

| Step | What happened |
|---|---|
| attach | the handset's location registration ~39 s after start, then the link released |
| ring | 5 s later: page, page response, CC-SETUP; the handset was authenticated and the link ciphered before the call went on; the handset rang |
| answer | connected; codec G.722, the wideband bind (`0x0230` flags `0x41`), audio connect on channel 1 |
| speech | the uplink in lane 1 (control lane + 1, as the daemon assumes for call 1), echoed back by `loopback`; **the maintainer heard a clear echo** |
| downlink | 0 of 8000 frames a second idle-filled |
| hang-up | audio disconnected (`0x0233`), link released |

The control channel had no sequence gaps or checksum errors; the transport
lost no frame in 1.9 million. **G7 passed.**

Every run ended with the park, then the chip held in reset and the
modules unloaded.

## Not yet run

- The service (`/etc/init.d/opendectcell`) with the new packages: these runs
  used the room's module files and a hand-built `dect-stubd`.
- A SIP call through baresip, the second handset, call lists, and the
  rest of [bench-next.md](bench-next.md) on this box.
