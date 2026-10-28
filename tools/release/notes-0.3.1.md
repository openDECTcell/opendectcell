**The first public release of openDECTcell: an open-source DECT base station for
FRITZ!Box routers running OpenWrt.**

Many FRITZ!Boxes have a DECT base station built in. Once OpenWrt is
installed, that base goes silent, because its software is part of AVM's
closed firmware. openDECTcell brings it back as free software: register your
cordless handsets with the box, give them a SIP account, and make phone
calls in HD voice, encrypted, all set up in OpenWrt's web interface.

This is an early release. It makes real phone calls on a FRITZ!Box 7520, but
many comfort features are still missing, and so far it has been tested on
one box model with four handset models. Please read *Known limitations*
before you install it, and tell us how it works for you.

## Highlights

- **Handsets on OpenWrt.** GAP and CAT-iq handsets register with a random
  8-digit PIN (6 or 4 digits for handsets that need it). Registration is
  open only for two minutes after you start it in the web page. Up to six
  handsets per box.
- **Phone calls over SIP.** Each handset gets up to two SIP accounts (VoIP
  profiles), for a provider or your own telephone system. Calls go out and
  come in; tested with a FRITZ!Box as the SIP server and a mobile phone on
  the other end.
- **HD voice.** Wideband G.722 audio in both directions, end to end.
- **Encryption.** Every call is encrypted on the air by default, after the
  standard DECT authentication.
- **Caller ID and line names.** The caller's number on the display; on
  CAT-iq handsets also the line ("via Line 1").
- **Calls between handsets** on the same box, and two of them at the same
  time.
- **Handset profiles.** Choose the handset's model before registering, and
  openDECTcell applies what that model needs. Profiles are plain files in
  `/etc/opendectcell/profiles/`; new ones need no new release.
- **A complete web interface.**
  - *Network > DECT*: switch the base on and off, register, rename and
    deregister handsets, assign each handset its Line 1 and Line 2, and
    manage the VoIP profiles (account, server and NAT, media, encryption,
    with "Try to autodetect" for media and encryption).
  - *Status > DECT*: a live monitor in three tabs. **DECT** shows the base
    and every handset with its IPEI, model, signal strength, link quality
    and last contact. **VoIP** shows each SIP registration and every running
    call with codec, data rate, endpoints, packet loss, jitter and
    encryption. **Log** shows the base's and the SIP side's log in one
    place.
- **Signed packages.** Built by GitHub Actions with the official OpenWrt
  SDK and signed with the project key, so they install through the web
  interface's *Upload Package*.

## What works, and what doesn't yet

| | |
|---|---|
| ✅ Registration, authentication, encryption | ✅ Calls in and out over SIP |
| ✅ HD voice (G.722) | ✅ Caller's number, line name |
| ✅ Calls between handsets | ✅ "Busy" when the base is full |
| ✅ Deregistering from the web page | ✅ Handset names, renaming on the handset |
| ✅ Signal and quality per handset | ✅ Fon/DECT LED |
| 🟡 Caller's name: only partly shown by the tested Gigasets | 🟡 Line 2 rings, but outgoing calls always use Line 1 |
| 🟡 Narrowband (non-HD) handsets: calls between handsets only, not yet on SIP | 🟡 FRITZ!Box 7530 and 7412: not yet tested |
| ❌ More than one SIP call at a time | ❌ One SIP account ringing several handsets |
| ❌ Call waiting, hold, switching between calls | ❌ Phonebook and call lists on the handset |

The full list: [docs/status.md](https://github.com/openDECTcell/opendectcell/blob/v{VERSION}/docs/status.md).

## Known limitations

- **One SIP call at a time.** A second incoming SIP call is refused as
  busy while one is running.
- **A VoIP profile belongs to one handset.** Ringing several handsets for
  one account comes in a later release.
- **Outgoing calls use Line 1.** Calls to Line 2 ring the handset, but you
  cannot yet choose Line 2 for a call you make.
- **SIP calls need an HD handset.** The speech path to the SIP side carries
  wideband audio only so far. All handsets tested so far are HD handsets.
- **Only OpenWrt {OPENWRT}.** The kernel module loads only into the kernel
  it was built for.
- **Firmware upload on the command line.** The DECT chip firmware is copied
  with `scp`; there is no upload button in the web page yet.
- **`dect-fw-extract.py` is new.** It has been tested on synthetic images
  only. If it does not find the firmware in your FRITZ!OS image, please open
  an issue with the image's file name.
- **The serial console is taken.** On these boxes the DECT chip shares the
  router's serial console port, so openDECTcell switches the serial console off.

## Supported hardware

| Box | Packages | State |
|---|---|---|
| FRITZ!Box 7520 | `kmod-opendectcell-ipq40xx`, `opendectcell` (`arm_cortex-a7_neon-vfpv4`) | Tested: the development box |
| FRITZ!Box 7530 | the same | Same hardware and OpenWrt image as the 7520; not yet tested |
| FRITZ!Box 7412 | `kmod-opendectcell-lantiq`, `opendectcell` (`mips_24kc`) | Built, but not yet run on a 7412 |

Both architectures also need `luci-app-opendectcell` (the web pages) and, for
phone lines, `opendectcell-baresip`.

**Handsets tested:** Gigaset A540 CAT (CAT-iq: everything above, including
SIP calls both ways), Gigaset C430A and AS690H (GAP: registration, calls,
HD voice, encryption, caller's number), AVM FRITZ!Fon C4 (registered and
called during development; not tested with this release). openDECTcell follows
the DECT standards rather than one maker's habits, so other GAP and CAT-iq
handsets should work too.

## Installing

You need the box with OpenWrt {OPENWRT}, internet access on it while
installing, a PC with Python 3, and the official FRITZ!OS image for your box
(openDECTcell is tested with the 7520's FRITZ!OS 07.56).

1. **Trust the project's key, once:** System > Administration, tab *Repo
   Public Keys*: paste the content of `opendectcell-signing-key.pem` from this
   release and press *Add key*.
2. **Install the packages:** System > Software, *Update lists…*, then
   *Upload Package…* for each file, in this order: the kernel module for
   your box, `opendectcell`, `luci-app-opendectcell`, and `opendectcell-baresip` for SIP.
3. **Supply the DECT chip firmware.** openDECTcell does not and cannot ship AVM's
   chip firmware. Extract it from the FRITZ!OS image on your PC with
   `python3 tools/dect-fw-extract.py FRITZ.Box_7520-07.56.image` (from the
   source code of this release) and run the commands it prints.
4. **Switch on:** Network > DECT, *Enable the DECT base*, *Save & Apply*.
5. **Register a handset:** tab *Handsets*, choose the model, *Register a
   handset*, then start the registration on the handset and enter the PIN
   from the web page.
6. **Add a phone line:** tab *VoIP Profiles*, add your SIP account; then
   *Edit* the handset and choose the profile as *Line 1*.

Step by step, with the command line and troubleshooting:
[docs/install.md](https://github.com/openDECTcell/opendectcell/blob/v{VERSION}/docs/install.md).

To check the files: `sha256sum -c SHA256SUMS --ignore-missing`.

## How it works

The DECT chip in these boxes (a Dialog SC14448) only does the radio work,
running AVM's chip firmware, which the router uploads at every start.
Everything above the radio used to run inside AVM's closed kernel module.
openDECTcell replaces that part with free software:

- `dect-fwload` uploads the chip firmware;
- `kmod-opendectcell` drives the bus between the router and the chip, which
  carries control messages and speech;
- `dect-stubd` with `libopendectcell` is the DECT protocol stack (link layer,
  registration and authentication, encryption, call control, G.722),
  written from the ETSI standards;
- the `opendectcell-baresip` module connects calls to SIP through baresip;
- `luci-app-opendectcell` is the web interface.

openDECTcell is a clean-room implementation: no AVM code is in the repository,
and AVM's chip firmware is never shipped, only referred to by its checksum.
Details: [README](https://github.com/openDECTcell/opendectcell/blob/v{VERSION}/README.md#clean-room).

## Be careful with the radio

A DECT base transmits on the licence-free DECT band. Stopping the service
(`/etc/init.d/opendectcell stop`, or *Enable the DECT base* off) switches the
radio off.

## About

openDECTcell was created by [ctandi](https://github.com/ctandi), partly in free
time and partly at work, as a research project on reverse engineering with
AI. It was made possible by the technology magazine c't /
[heise](https://heise.de). It builds on a lot of public work, from the ETSI
standards to OsmoDECT, Erik Tews' DECT security research, OpenWrt and
baresip; the full list is in the
[README](https://github.com/openDECTcell/opendectcell/blob/v{VERSION}/README.md#credits).

openDECTcell is not affiliated with or endorsed by AVM. Licence: GPL-2.0.

**Reports welcome:** which handsets and boxes work for you, and which don't.
See [CONTRIBUTING.md](https://github.com/openDECTcell/opendectcell/blob/v{VERSION}/CONTRIBUTING.md).
