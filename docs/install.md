# Installing openDECTcell

Each release on GitHub has ready-built, signed packages for OpenWrt. This
guide takes you from a FRITZ!Box running OpenWrt to a handset making calls.

## Before you start

- **A FRITZ!Box 7520 or 7530 with OpenWrt**, of exactly the version named in
  the release (the kernel module only loads into the kernel it was built
  for). Installing OpenWrt itself is described on the
  [OpenWrt wiki](https://openwrt.org/toh/avm/avm_fritz_box_7530).
- **Internet access on the box** while installing.
- **A PC with Python 3** ([python.org](https://www.python.org/downloads/);
  Linux usually has it) and this repository, or at least the files
  `tools/dect-fw-extract.py` and `tools/sqfs_be.py`.
- **The official FRITZ!OS image for your box** from AVM (step 3).
- **Your DECT handsets.** If a handset is still registered with another
  base, it can usually register with a second one; otherwise reset it as
  its manual describes.

Good to know: on these boxes the DECT chip is connected to the same serial
port as the router's console. openDECTcell switches the serial console off so
the chip can use it. This matters only if you have soldered on a serial
cable.

## 1. Which files

| File | What it is | Needed |
|---|---|---|
| `kmod-opendectcell-ipq40xx-…_ipq40xx-generic.apk` | kernel module, FRITZ!Box 7520/7530 | yes, on a 7520/7530 |
| `kmod-opendectcell-lantiq-…_lantiq-xrx200.apk` | kernel module, FRITZ!Box 7412 | yes, on a 7412 (experimental, see below) |
| `opendectcell-…_<arch>.apk` | the DECT base: firmware loader, daemon, service | yes |
| `luci-app-opendectcell-…_all.apk` | web pages: Network > DECT and Status > DECT ([luci-manual.md](luci-manual.md)) | recommended |
| `opendectcell-baresip-…_<arch>.apk` | SIP: connects the handsets' lines to a SIP provider or telephone system | for phone calls outside the house |

`<arch>` is `arm_cortex-a7_neon-vfpv4` for the 7520/7530 and
`mips_24kc` for the 7412. `SHA256SUMS` lists every file's checksum, and
`opendectcell-signing-key.pem` is the key the packages are signed with (the same
as [openwrt/opendectcell-signing-key.pem](../openwrt/opendectcell-signing-key.pem) in
the repository).

### FRITZ!Box 7412 (experimental)

The 7412 runs the base since 2026-10-29: handsets register, and SIP calls in
and out work with HD voice ([7412-spi-tdm-and-loader.md](7412-spi-tdm-and-loader.md) §1b,
[bench-7412.md](bench-7412.md)). Its PCM bus needs a second kernel module
that is **not part of the release yet**: `kmod-lantiq-ssc-pcm`, the generic
Lantiq SSC PCM transport, from its own repository. Build it with the OpenWrt
SDK for `lantiq/xrx200` (its `tools/build.sh`) and install the package, or
copy `lantiq_ssc_pcm.ko` to `/lib/modules/$(uname -r)/`. The service loads
it before the chip boots; the board profile names it (`pcm_kmod`).

The 7412 uses its **own** chip firmware, from `FRITZ.Box_7412.137.06.88.image`
(step 3), under the names the board profile expects:
`dectfw_firstlevel_441-06.88-7412.hex` and
`dectfw_secondlevel_441-06.88-7412.hex`. `tools/dect-fw-extract.py` knows both
stages by their hash and saves them under these names; running it on the 7412
image has not been tested yet. The 7520's firmware does not drive the 7412's
bus.

## 2. Install

The box needs internet access while installing: OpenWrt's package manager
fetches what openDECTcell builds on, such as baresip, from OpenWrt.

### In the web interface

1. **Once: trust the project's key.** The packages are signed with the
   openDECTcell key, not OpenWrt's. Open System > Administration, tab
   *Repo Public Keys*, paste the content of `opendectcell-signing-key.pem` from
   the release and press *Add key*. Without this, the upload below ends in
   "UNTRUSTED signature".
2. Open System > Software and press *Update lists…*.
3. Press *Upload Package…* and install the files one by one, in this order:
   the kernel module, `opendectcell`, `luci-app-opendectcell`, and for SIP
   `opendectcell-baresip`.

### On the command line

Copy the files and the key to the box, for example into `/tmp`:

```sh
scp -O *.apk SHA256SUMS opendectcell-signing-key.pem root@192.168.1.1:/tmp/
```

On the box (`ssh root@192.168.1.1`):

```sh
cd /tmp
sha256sum -c SHA256SUMS --ignore-missing
cp opendectcell-signing-key.pem /etc/apk/keys/
apk update
apk add ./kmod-opendectcell-*.apk ./opendectcell-[0-9]*.apk ./luci-app-opendectcell-*.apk
```

For SIP add `./opendectcell-baresip-*.apk` to the same line (or install it later the
same way).

## 3. The DECT chip firmware

The DECT chip needs AVM's firmware, which the router uploads at every start.
openDECTcell may not ship it, so you take it once from the official FRITZ!OS
image for your box, on your own PC. Why, and which builds are known:
[firmware-sourcing.md](firmware-sourcing.md).

1. Download the FRITZ!OS image for your model from AVM (download.avm.de,
   the folder of your box, `fritz.os`). openDECTcell is tested with
   **FRITZ!OS 07.56** of the 7520 (`FRITZ.Box_7520-07.56.image`); 08.25 is
   known as well. A copy from elsewhere works too: the tool checks the
   firmware inside it.
2. On the PC:
   ```sh
   python3 tools/dect-fw-extract.py FRITZ.Box_7520-07.56.image
   ```
   It finds the two firmware files, checks them, saves them to
   `./opendectcell-firmware/` and prints the commands for the next step, for
   example:
   ```sh
   scp -O opendectcell-firmware/*.hex root@192.168.1.1:/etc/opendectcell/firmware/
   ```
   and an `ssh … uci set …` line that tells the box which build it got.
   Run them. They use OpenWrt's default address 192.168.1.1; for another
   one, add `--box root@ADDRESS` to the first command.

   Instead of `scp`, you can upload the two `.hex` files in the web
   interface: **Network > DECT**, tab *Firmware*, *Upload…* for each. The
   box checks them against the build it needs before it keeps them
   ([luci-manual.md](luci-manual.md) section 2.5).

The files stay on the box across OpenWrt upgrades.

## 4. Switch on and register

In the web interface, open **Network > DECT**, tick *Enable the DECT base*
and press *Save & Apply*. On the command line:

```sh
uci set opendectcell.main.enabled=1; uci commit opendectcell
/etc/init.d/opendectcell start
```

Status > DECT now shows the base as on the air. Then, in Network > DECT, tab
**Handsets**, below the list:

1. Choose the handset's manufacturer and model, if it is listed; otherwise
   keep "Generic DECT GAP/CAT-iq handset".
2. Press *Register a handset*, then start the registration on the handset
   (often under Settings > Registration, "Register handset") and enter the
   PIN shown in the web page: 8 random digits. *New PIN* makes another one;
   *PIN length* allows 6 or 4 digits for a handset that cannot take 8.

Registration is open only while you are registering: it closes after one
handset has registered, or after two minutes. The handset then appears in
the list as *DECT 1*, *DECT 2* and so on; *Edit* renames it. Every field of
the web pages is described in [luci-manual.md](luci-manual.md).

## 5. Phone lines (SIP)

With `opendectcell-baresip` installed, a handset can make and take calls through
a SIP provider or your own telephone system, e.g. a FRITZ!Box with an "IP
phone" set up for it.

1. Network > DECT, tab **VoIP Profiles**, *Add a profile*: a name for it, the
   user name, password and registrar (the SIP server) your provider gives.
   One profile per SIP account. The defaults ("Try to autodetect" for media
   and encryption) suit nearly every provider; if your provider supports
   TLS, choose it under Server and NAT.
2. Tab **Handsets**, *Edit* the handset, tab General: pick the profile as
   **Line 1**. Calls to that account ring the handset, and its calls go out
   on Line 1. Line 2 is an optional second account that rings it too; a
   CAT-iq handset can pick Line 2 for a call it makes. A profile may be the
   line of several handsets: a call to it rings them all, and the first to
   answer takes it.
3. *Save & Apply*. Status > DECT, tab VoIP shows whether the line is
   registered, and the codec and encryption of each call.

Up to three calls run at once, over SIP or between handsets (*Several
calls*, tab General). Handsets without HD voice call over SIP too. The
base picks the codecs so that the phones do the coding: an HD call goes
through as G.722 untouched, a narrowband one as G.711.

A CAT-iq handset (e.g. a Gigaset A540) also gets call waiting during a
call and can hold, switch between and end two calls; other handsets give
the caller "busy" while they are in a call. Keys pressed during a call go
to the other end as DTMF, for voicemail and other menus (*DTMF* in the
profile's Media tab, if your provider needs a particular mode).

**Voicemail.** Switch on *Voicemail notification (MWI)* in the VoIP
profile: when your provider's mailbox has new messages, CAT-iq handsets
show the message indicator. Whether calls go to the mailbox (no answer,
busy) is set at your provider, not on the base. To listen, call the
mailbox number your provider gives; Gigaset handsets can store it as the
"network mailbox" of the line and call it with a long press on 1.

The base takes a SIP call only from the server the account registers with.
If your provider's calls come from other addresses (the Log says "is not
from the account's server -- 403"), allow them on the command line:

```sh
uci set opendectcell.main.sip_any_source=1; uci commit opendectcell
/etc/init.d/opendectcell restart
```

## Upgrading and removing

A newer release installs the same way, over the old one; your settings in
`/etc/config/opendectcell` stay. `apk del opendectcell-baresip luci-app-opendectcell openDECTcell
kmod-opendectcell-ipq40xx` removes everything again.

## When something doesn't work

The *Log* tab of Status > DECT shows what the base and the SIP side say.

| What you see | What it means, and what to do |
|---|---|
| "UNTRUSTED signature" when uploading a package | The project's key is not added yet: step 2, point 1 |
| The kernel module does not load | The box runs a different OpenWrt version than the release was built for. Install the matching OpenWrt version, or build the packages yourself |
| Log: "chip firmware missing" | Step 3 is not done, or the files are in another folder than `/etc/opendectcell/firmware/` |
| Log: "not a firmware build openDECTcell knows" | The image is from a FRITZ!OS version openDECTcell does not know. Use one listed in [firmware-sourcing.md](firmware-sourcing.md) |
| The handset searches for its base after the box restarted | Handsets wait longer and longer between searches. Give it half a minute, or press a key on it |
| The handset says the PIN is wrong | Enter the PIN shown in the web page right now (*New PIN* changes it). Some handsets first try 0000 by themselves |
| Registration does not start | Press *Register a handset* again: the window closes after two minutes |
| The VoIP tab shows a line as not registered | The reason is shown next to it. Check user name, password and registrar in the VoIP profile |
| A call is "busy" right away | The handset is already in a call or ringing, or three calls are up (the chip's voice channels; one if *Several calls* is off) |

If you are stuck, please open an issue: [CONTRIBUTING.md](../CONTRIBUTING.md)
says what to include.
