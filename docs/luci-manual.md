# The openDECTcell web pages: a manual

`luci-app-opendectcell` adds two pages to OpenWrt's web interface (LuCI):

| Page | What it is for |
|---|---|
| **Network > DECT** | Settings: the base, its handsets, the phone lines (VoIP profiles), the phonebook, the chip |
| **Status > DECT** | The DECT Monitor: live figures of the base, the handsets, the radio and the phone lines, and the log |

This manual walks through both pages, field by field. Installing the
packages and the DECT chip firmware is described in
[install.md](install.md); this manual starts where that guide's step 3
ends.

Contents:

1. [From nothing to the first call](#1-from-nothing-to-the-first-call)
2. [Network > DECT](#2-network--dect)
   - [General](#21-general)
   - [Handsets](#22-handsets)
   - [VoIP Profiles](#23-voip-profiles)
   - [Phonebook](#24-phonebook)
   - [Advanced](#25-advanced)
3. [Status > DECT: the DECT Monitor](#3-status--dect-the-dect-monitor)
4. [Cell mode: the box as a controller's cell](#4-cell-mode-the-box-as-a-controllers-cell)
5. [When something doesn't work](#5-when-something-doesnt-work)
6. [Where the settings are kept](#6-where-the-settings-are-kept)

## How saving works

Network > DECT is an ordinary LuCI settings page: **Save** keeps your
changes, **Save & Apply** puts them into effect. Applying restarts the DECT
service with the new settings, so **a call in progress ends**. Apply when
nobody is on the phone.

A few things take effect at once, without Save & Apply, because the page
sends them straight to the running base. That applies to a handset's
**name** and **model**, **Deregister** and **Register a handset**. These
need the DECT service running; while it is stopped, those fields are
greyed out.

## 1. From nothing to the first call

Once the packages and the chip firmware are installed
([install.md](install.md) steps 1-3):

1. **Switch the base on.** Network > DECT, tab *General*: tick *Enable the
   DECT base*, then *Save & Apply*. Status > DECT should now say *Radio: on*.
2. **Register a handset.** Network > DECT, tab *Handsets*, below the list:
   - pick the handset's manufacturer and model if listed, otherwise keep
     *Generic DECT GAP/CAT-iq handset*;
   - press *Register a handset*;
   - on the handset, start the registration (on Gigaset handsets usually
     *Settings > Registration > Register handset*) and enter the
     *Registration PIN* shown on the page.

   The handset appears in the list as *DECT 1* within a few seconds.
3. **Add a phone line** (needs the `opendectcell-baresip` package). Tab
   *VoIP Profiles*, *Add a profile*: a profile name, the *User name*,
   *Password* and *Registrar* your SIP provider or telephone system gives
   you. Save the pop-up.
4. **Give the handset the line.** Tab *Handsets*, *Edit* on the handset,
   tab *General*: choose the profile as *Line 1*. Save the pop-up, then
   *Save & Apply*.
5. **Check.** Status > DECT, tab *VoIP*: the line shows *registered*. Call
   out from the handset, and call its number from outside.

## 2. Network > DECT

The page has up to six tabs: *General*, *Handsets*, *VoIP Profiles*,
*Phonebook* (only with the `opendectcell-carddav` package), *Firmware* and *Advanced*. If
the box is a cell of a controller, the page looks different: see
[section 4](#4-cell-mode-the-box-as-a-controllers-cell).

### 2.1 General

| Field | Default | What it does |
|---|---|---|
| **Enable the DECT base** | off | Boots the DECT chip and puts the base on the air. Needs the chip firmware (tab *Firmware*, [install.md](install.md) step 3). Off: the radio is parked and nothing transmits |
| **Encrypt calls** | on | DECT standard encryption on every call and every list session (phonebook, call lists). Switch off only to debug audio |
| **Check handsets when they report in** | on | Each handset proves it holds its key whenever it reports in to the base; a handset that cannot is turned away |
| **Wideband audio (HD)** | on | G.722 (HD voice) when the handset offers it. Off keeps every call narrowband |
| **Several calls** | on | Up to three calls at once: other handsets may call and be called while one is in a call. Off: one call at a time, and a second caller hears "busy" |
| **Collect dialled digits** | on | The base waits until the number is complete before it connects the call. Needed for handsets that send the digits one by one as you press them |
| **Dialling pause** | 3000 ms | Shown when *Collect dialled digits* is on: how long after the last digit the number counts as complete. Range 1000-10000 ms |
| **Time update interval** | 24 h | Hours between sending the date and time to all handsets. Each handset also gets it whenever it reports in; 0 sends it only then. The box's own clock must be right (NTP) |
| **Controller** | - | A *Make this box a cell…* button, only with the `opendectcell-cell` package: see [section 4](#4-cell-mode-the-box-as-a-controllers-cell) |

### 2.2 Handsets

#### The handset list

One row per handset the base holds:

| Column | Meaning |
|---|---|
| **No.** | The handset's number on this base, 1-6 (*DECT 1* … *DECT 6*) |
| **Name** | What every handset shows for this one. *not registered* when the base no longer holds it; *unknown: the service is off* while the service is stopped |
| **IPEI** | The handset's own identity, as printed on its label or shown in its service menu |
| **Type** | *CAT-iq* for a handset that reports the newer DECT features (call waiting, line names, lists), *GAP* for a plain one, *-* until it has reported |
| **Model** | The model profile in use, *standard* if none |
| **Line 1**, **Line 2** | The VoIP profiles the handset has as its lines |
| **Last contact** | *connected now*, or how long ago it was last heard; *base off* while the radio is parked |

Each row has two buttons:

- **Edit** opens the handset's settings (below).
- **Deregister** erases the handset's registration on the base. If the
  handset is in range, the base tells it, and the handset says it is no
  longer registered. A handset that is switched off or lost is erased after
  about 30 seconds without its confirmation. The page waits up to 40
  seconds, then removes the handset's settings and reloads. The VoIP
  profiles it had stay, free for another handset. To use the handset again,
  register it anew.

  On a row whose handset the base no longer holds, the button reads
  **Remove** instead: it deletes the leftover settings and frees the row's
  VoIP profiles.

#### Editing a handset

*Edit* opens a pop-up with four tabs. Everything except *Name* and *Model*
is saved with the page and takes effect at the next *Save & Apply*.

**General**

| Field | What it does |
|---|---|
| **Name** | What every handset shows for this one, at most 15 characters (fewer with umlauts and other non-ASCII letters, which take two or three of the 15 bytes). Empty restores *DECT n*. Changed at once when the pop-up is saved; needs the service running |
| **Model** | The handset's model profile: the known quirks of that model, so it works without hand-tuning. *automatic (standard)* uses none. A manufacturer's entry, e.g. *Gigaset (other models)*, covers that make's unlisted models. Takes effect at once when the pop-up is saved |
| **Line 1** | A VoIP profile. Calls to it ring this handset, and the handset's calls go out on it unless the handset picks Line 2. The search field above the menu filters the profiles by name |
| **Line 2** | An optional second VoIP profile, which rings this handset too. A CAT-iq handset can pick it for a call it makes. Needs Line 1 first, and cannot be the same profile as Line 1 |

The model profiles shipped with openDECTcell are *Gigaset A540*, *Gigaset
AS690H*, *Gigaset C430A* and *Gigaset (other models)*. What a profile
contains, and how to write one for another model:
[handset-profiles.md](handset-profiles.md).

**Calls**, **Radio link and lists**, **Capabilities and features**

These three tabs are the handset's own DECT settings: one field per
setting a model profile can make. Each field starts empty, which means
*as the model says*, and the menu names the model's value in brackets.
Fill a field in only to override the model for this one handset, for
example when a feature misbehaves on it.

| Tab | Setting | Values |
|---|---|---|
| Calls | Call status in call messages | auto (on for CAT-iq handsets), on, off |
| | Call information when it rings | auto, full, line, none |
| | "Call connected" status after answering | auto, on, off |
| | Audio offered on an incoming call | auto (wideband if it can), wideband, basic |
| | Ring pattern, external calls | 0x40-0x48 (64-72); the model's default is 0x41 |
| | Ring pattern, internal calls | 0x40-0x48; the model's default is 0x40 |
| | Switch the audio on before connecting | no, yes |
| | How the caller's name is shown | cnip (the standard way), display (as display text), both, none |
| | Display text framing | hex: bytes around the name, `{text}` standing for it |
| | Alphabet of the caller's name | auto, utf8, dect |
| | Who draws the call timer | handset, base |
| | How the caller's number is shown | clip (the standard way), display, both |
| Radio link and lists | End the radio link after each call | no, yes |
| | It tries PIN 0000 first when registering | no, yes |
| | Offer it the base's lists (names, lines, settings) | no, yes |
| | How it picks a line | auto, fp, hash |
| | Send it notifications (list changes) | auto, on, off |
| Capabilities and features: *What the handset reports* | Believe what it reports about itself | yes, no |
| | Its CAT-iq level, if it reports it wrongly | from the handset, gap, part1, part3 |
| Capabilities and features: *Features* | Call waiting | on, off |
| | Call lists | on, off |
| | Contacts | on, off |

The details of each setting, and what the standard says about it, are in
[handset-profiles.md](handset-profiles.md).

#### Registering a new handset

Below the list:

| Field | What it does |
|---|---|
| **Manufacturer**, **Model** | Of the handset you are about to register. A known model gets the settings it needs from the start; *Generic DECT GAP/CAT-iq handset* uses the standard ones. Not saved: it applies to the next registration only |
| **Registration PIN** | The PIN a handset asks for when it registers. Random, made by your browser |
| **New PIN** | Makes another random PIN |
| **PIN length** | 8 digits (default), 6 or 4. 8 digits are the most DECT allows and the hardest to guess; choose fewer only for a handset that cannot take 8. Changing it makes a new PIN of that length |
| **Register a handset** | Opens registration for the *Registration window*. Then start the registration on the handset and enter the PIN |
| **Registration window** | How long *Register a handset* keeps registration open, 10-3600 s; default 120 |
| **Keep registration open** | Registration stays open for as long as the base runs. Not recommended: anyone nearby who knows the PIN can register a handset |

The base registers with the PIN it was **started** with. After changing
the PIN, press *Save & Apply* first, then *Register a handset*. The page
reminds you if you forget ("The PIN shown is not in use yet").

Registration closes by itself after one handset has registered, or when
the window runs out. The base holds up to six handsets.

### 2.3 VoIP Profiles

A VoIP profile is one SIP account: a phone number at a provider, or an
extension of your own telephone system (a FRITZ!Box with an "IP phone"
set up, Asterisk, FreeSWITCH, …). Profiles become handsets' lines under
*Handsets*, *Edit*, *Line 1* / *Line 2*. This tab needs the
`opendectcell-baresip` package to have any effect.

- One profile per SIP account: the page refuses two profiles with the same
  user name and registrar.
- A profile may be a line of several handsets: a call to it rings them
  all, and the first to answer takes it.
- A profile cannot be both lines of one handset.
- Deleting a profile frees every line that held it.

#### The profile list

| Column | Meaning |
|---|---|
| **Name** | The profile's name, as the handsets' Line menus show it |
| **Account** | user@registrar |
| **Line of** | The handsets and lines that use it, or *free* |
| **Registration** | As on Status > DECT, tab VoIP: *registered*, *registering…*, *failed* with the server's reason, or *not in use (yet)* |

*Add a profile* opens an empty profile; *Edit* an existing one. The
pop-up has five tabs.

#### Account

| Field | What it does |
|---|---|
| **Profile name** | How the handsets' Line menus name this profile. Required, and unique |
| **Name on the handset** | What CAT-iq handsets show for this line, at most 15 characters (fewer with umlauts). Empty: the profile name, if it is that short |
| **Display name** | The name the called party sees |
| **User name** | The SIP user, often the phone number. Required |
| **Login name** | Only if the provider wants a login other than the user name |
| **Password** | The SIP password |
| **Registrar** | The provider's SIP server, `host` or `host:port`, e.g. `sip.example.net`, or `192.168.178.1` for a FRITZ!Box. Required |
| **Register** | On (default): the base registers with the server. Off for a peer that calls this line directly by address |
| **Registration interval** | Seconds between registrations, 60-86400; default 3600 |

User name, login name and password cannot contain `;`, `"`, `<`, `>` or
spaces (the SIP client cannot take them).

#### Server and NAT

| Field | What it does |
|---|---|
| **Transport** | UDP (default), TCP or **TLS (encrypted signalling)**. Use TLS if your provider supports it: your password and the numbers you call then do not cross the internet in the clear. The provider names the port, often 5061 (`sip.example.net:5061`) |
| **Outbound proxy**, **Second outbound proxy** | Only when the provider names one |
| **SIP outbound (RFC 5626)** | Keeps the connection to the server open through NAT, if the provider supports it |
| **NAT traversal for media** | none (default), STUN, TURN or ICE |
| **STUN/TURN server** | `host` or `host:port`, when one is chosen above |
| **TURN user**, **TURN password** | For TURN |
| **Keep media open** | Packets that keep a NAT binding open while one side is silent: off (default), empty RTP packets, STUN, dynamic, RTCP |

Behind an ordinary home router with the provider on the internet, the
defaults usually work. If calls connect but one side hears nothing, try
STUN with your provider's STUN server.

#### Media

| Field | What it does |
|---|---|
| **Try to autodetect** | On (default): offers every codec, HD first, and sends key tones the way the other side asks. Right for nearly every provider. Untick to set the fields below by hand |
| **Codecs** | G.722 (HD), G.711 A-law, G.711 µ-law, offered in this order. G.722 keeps the handset's HD voice end to end |
| **Packet time** | Milliseconds of audio per RTP packet: 20 (default), 10, 30, 40 |
| **Key tones (DTMF)** | RTP events (RFC 4733, default), SIP INFO or automatic. Needed for voicemail and other menus you control by pressing keys |
| **Reliable provisional responses (100rel)** | when the other side supports it (default), never, required |
| **Voicemail notification (MWI)** | On: CAT-iq handsets show the message indicator when the provider's mailbox has new messages |
| **Allow call transfer** | On (default) |

The base does not convert speech when it does not have to: an HD call
goes through as G.722 untouched, a narrowband one as G.711. The phones do
the coding.

#### Encryption

| Field | What it does |
|---|---|
| **Try to autodetect** | On (default): offers encrypted speech (SRTP) and falls back to plain speech when the other side does not take it |
| **Media encryption** | With autodetect off: none, SRTP when offered, SRTP required, SRTP required (in the offer), DTLS-SRTP |

Encrypted speech is only private if the signalling is encrypted too
(*Transport: TLS*): otherwise the keys travel in the clear.

#### Advanced

| Field | What it does |
|---|---|
| **Further account parameters** | Appended to the account as is, in baresip's syntax `;key=value;key=value`. For a provider that needs something the other tabs do not offer |

#### Settings for all profiles

Below the profile list:

| Field | Default | What it does |
|---|---|---|
| **SIP port** | 5060 | The local port for SIP signalling (UDP and TCP; TLS uses the next port) |
| **RTP port range** | 10000-20000 | Local ports for the speech, first-last, 1024-65535 |
| **Network interface** | automatic | The network **device** SIP and speech go out on, e.g. `br-lan` or `pppoe-wan`, not an interface name like `wan`. Empty: the default route |
| **DNS server** | the system's | An IP address, optionally with `:port`, for looking up the SIP servers' names |
| **Speech priority (DSCP)** | EF (46) | Marks the speech packets so routers can prefer them: EF (46), AF41 (34) or none |
| **Signalling priority (DSCP)** | CS3 (24) | The same for SIP: CS3 (24), AF31 (26) or none |
| **Jitter buffer** | 20-160 | Milliseconds of speech held back to smooth out the network, min-max, up to 1000 |
| **Check the server's certificate** | on | For TLS lines. Off accepts any certificate, which defeats the encryption |
| **Certificate authorities** | the system's | A PEM file of the CAs to trust, e.g. for your own telephone system's CA |

### 2.4 Phonebook

Shown only with the `opendectcell-carddav` package. The handsets' phonebook
(their *Contacts* list) then comes from CardDAV address books, such as
Nextcloud, Baïkal or a mail provider's. It works both ways:

- What a handset changes or deletes is written back to the book the
  contact came from.
- A contact made on a handset goes to the book chosen for new contacts.
- Contacts are not merged: one that is in two books appears twice on the
  handsets.

| Field | What it does |
|---|---|
| **Use address books** | Switches the phonebook on. Off keeps the settings below for later |
| **Server** | Any `https://` address of the account: the server (`https://cloud.example.org`) or an address book's own URL |
| **User name**, **Password** | The account's login. Use an app password where the server offers one |
| **Address books** | *Fetch address books* asks the server for the account's books and lists them. Tick **Use** for each book the handsets should see, and pick the one **New contacts** go to. At least one is needed |
| **Check every** | Minutes between checks for changes on the server, 1-1440; default 15. Changes made on a handset are written back at once |
| **CA certificate** | Only for a server with its own certificate authority: a PEM file on the box. Empty: the usual public CAs |
| **Allow "delete list"** | A handset's "delete list" empties every address book used here, **on the server**. Off (default): the base refuses it |
| **Last sync** | The last sync's result: the number of contacts and when, per book, or the error |

More about the phonebook: [phonebook.md](phonebook.md).

### 2.5 Firmware

The two files the DECT chip boots from. openDECTcell cannot ship them: take them
from the FRITZ!OS image of your box with `tools/dect-fw-extract.py` on your
PC ([install.md](install.md) step 3), then upload the two `.hex` files here.
One row per file:

| Column | What it is |
|---|---|
| **File**, **Name** | First or second stage, and the file name the box keeps it under |
| **Expected SHA-256** | The build this box's profile pins (the first 12 digits; hover for all of it). On a 7520 the profile pins nothing and any build openDECTcell knows is taken, under the name that build belongs to |
| **State** | *verified*; *missing*; *wrong build* (not the pinned one); *unknown build* (not one openDECTcell knows); *known build, kept under another build's name* (set the file name under Advanced to the one it names, save, upload again) |
| **Upload…**, **Delete** | Pick the file on your PC; the box checks it before it replaces anything and keeps the old file if the check fails. A file over 400 000 bytes is refused in the browser. The new file is used the next time the chip boots (a restart of the DECT service) |

In cell mode the same table is on the page's only tab. The files stay on the
box across OpenWrt upgrades.

### 2.6 Advanced

Settings for unusual set-ups and for developers. Every field shows in grey
the value the service uses when it is left empty, taken from the box's
board profile.

| Field | What it is |
|---|---|
| **Chip firmware directory** | Where the two DECT chip firmware files are kept; default `/etc/opendectcell/firmware` |
| **First-stage firmware**, **Second-stage firmware** | The two files' names, if they differ from the board's defaults |
| **Allow unknown firmware** | Runs a chip firmware build openDECTcell does not know. For developers: an unknown build may behave differently in ways openDECTcell cannot check |
| **DECT UART** | The serial port to the DECT chip |
| **Factory partition**, **Factory offset** | Where the box keeps its DECT identity and radio calibration |
| **Chip mode word** | The chip's start-up mode |
| **Handset stores** | The files that hold the registered handsets, one each; default `/etc/opendectcell-sub.*` |
| **Extra options** | Further `dect-stubd -a` options, comma-separated, for developers. They come after the service's own, so a key set here wins |

On a FRITZ!Box 7520, 7530 or 7412, leave all of these empty.

## 3. Status > DECT: the DECT Monitor

The Monitor shows what the base is doing, refreshed every 2 seconds. It
has three tabs: *DECT*, *VoIP* and *Log*. Only the tab in view is
refreshed. Nothing on this page changes a setting.

### 3.1 DECT

**Service.** Whether the DECT service runs, and if not, why:

| Shown | Meaning |
|---|---|
| running | The base is on the air |
| running, the radio is starting or parked | The service runs but the radio is not on yet: the chip is booting (up to a minute after a start) |
| switched off (Network > DECT, General: Enable) | *Enable the DECT base* is off |
| stopped after repeated failures: see the Log tab | The service crashed several times and OpenWrt gave up. The Log tab says why |
| not running although switched on: see the Log tab | It did not start: most often the chip firmware is missing |

**Base.**

| Row | Meaning |
|---|---|
| Radio | *on*, or *parked* (not transmitting) |
| Registration | *closed*, or *open* with the seconds left |
| Encryption, Handset check on reporting in, Wideband audio, Several calls | The settings of the same names on Network > DECT, General, as the running base has them |
| Chip firmware | The DECT chip firmware build loaded |
| Running for | Time since the service started |
| openDECTcell interface | The version of openDECTcell's internal interface (for bug reports) |

**Handsets.** One row per handset: name, number, IPEI, type, model, state
(*idle*, *connected*, or the call's state), last contact, and for a handset
in a call the radio figures: carrier, field strength and quality.

**Connection.** For each handset with a connection, a section of its own:

| Row | Meaning |
|---|---|
| Call | *idle*, *setting up*, *connecting*, *in a call*, *ringing*, *being called*, *dialling*, *call ended*; incoming or from the handset |
| Codec | *G.722, wideband (HD)* or *G.726, narrowband* on the radio; *list access, no speech* while the handset reads the phonebook or call lists |
| Encryption | on or off, for this connection |
| Carrier | Which of the ten DECT carriers, with its frequency (carrier 0 is 1897.344 MHz, each next one 1.728 MHz lower) |
| Time slot | The time slot in the DECT frame |
| Field strength | How strongly the base hears the handset, in dBm, with five bars: above -40 dBm five bars, then one fewer per 10 dB. Anything above -70 dBm is fine |
| Quality | The base's quality figure, 0-100 % |
| Connection time, Frames | How long the connection has been up, and in DECT frames (100 per second) |

The radio figures exist only during a speech call (*Radio figures: only
during a speech call* otherwise).

Below it:

- **Errors in the last interval**: the chip's eight error counters for this
  connection: A-, X- and Z-field CRC errors, Q1 and Q2 bit errors,
  handover requests, slide and sync errors. A few now and then are
  normal. Many CRC errors at a weak field strength mean the handset is
  too far away. Many at a good field strength mean interference on that
  carrier.
- **Level per carrier**: the level the chip measures on each of the ten
  carriers, the one in use in dark blue. It shows which part of the band is
  busy (neighbours' DECT, baby monitors) and which is quiet.
- **Level and quality over time**: the last five minutes, drawn in your
  browser while the page is open. Reloading the page starts it afresh.

### 3.2 VoIP

**Service.** Whether the SIP client (baresip) runs:

| Shown | Meaning |
|---|---|
| running | It runs; *Bridge to the DECT base* says whether it is connected to the base |
| not running: it runs with the DECT service | The DECT service is stopped |
| not running: no handset has a VoIP profile as a line | No handset has a Line 1 or Line 2 yet: Network > DECT, Handsets, Edit |
| stopped after repeated failures: see the Log tab | It crashed several times |
| not running: is the package opendectcell-baresip installed? | The SIP package is missing |

**Lines.** One row per handset line: the handset, the line and its
profile, the account, the registration state (*registered*,
*registering…*, *failed* with the server's reason, *signing off*,
*off: calls by address only* when *Register* is off, *no account loaded*)
and the registration interval.

**Calls.** For each SIP call in progress:

| Row | Meaning |
|---|---|
| Direction, Other party, State, Duration | The call itself |
| Codec | The codec on the internet side, with its sample rate (16 kHz is HD) |
| Signalling | UDP, TCP or TLS |
| Media, this end / other end | The addresses the speech goes between |
| Data rate, Packets, Packets lost, Jitter, Round trip | The network's view of the speech. Lost packets and jitter above about 30 ms are heard as gaps or robotic speech |
| Encryption | *on (SRTP …)*, *… asked for, not in use*, or *off* |

### 3.3 Log

The newest 1000 lines of the system log from the DECT base, the SIP client
and the service, refreshed every 2 seconds. *Show* picks *DECT and VoIP*,
*DECT only* or *VoIP only*. The view stays where you scrolled to, so you
can select and copy lines; scrolled to the bottom, it follows new lines.

When you report a problem, copy the lines from around the time it
happened. The log never contains keys, PINs or passwords, but it does
contain phone numbers and handset names. Look it over before posting it
publicly.

## 4. Cell mode: the box as a controller's cell

With the `opendectcell-cell` package, a box can become a **cell** of a
[openDECTcell controller](notes-for-the-controller.md): a central server that
runs the DECT base for several boxes, so handsets roam between them. The
controller then has the handsets, the calls and the phone lines. This
box's own handsets, VoIP profiles and settings stay saved but unused, and
its SIP client does not run.

**Becoming a cell.** Network > DECT, tab *General*, *Make this box a
cell…*. Paste the **enrollment string** from the controller's web
interface (it starts with `opendectcell-enroll:v1?`) and press *Enrol*. This can
take up to a minute. The page then reloads in cell mode. An administrator
may have to approve the cell on the controller first.

**In cell mode** Network > DECT shows only:

| Field | Meaning |
|---|---|
| Enable the cell | Boots the DECT chip and connects to the controller. The controller turns the radio on |
| Controller | The controller's address, or *not enrolled* |
| Link | *up*, *connecting*, *not running* |
| This cell at the controller | *active*, *waiting for an admin to approve it*, *disabled*, *revoked* |
| Enrolment | *Enrol…*, while the box is not enrolled |
| Cell mode | *Leave cell mode…* |

**Leaving cell mode** erases the controller's key and certificate on this
box and runs it as its own DECT base again, with its own handsets and
settings. To be a cell again it needs a new enrollment string.

**Status > DECT in cell mode** has two tabs. *Cell* shows the chip
(dect-stubd) and controller link (dect-cell) services, the controller, the
link's state, round trip and reconnects, the date the cell's certificate
runs out, and the chip side: radio, chip firmware, radio links, calls with
audio, and the counts of messages ("mails") to and from the controller.
*Log* is as in [3.3](#33-log).

**The certificate renews itself.** The controller's certificate for the
cell is valid for 90 days; at two thirds of that the cell asks the
controller for a new one over its link, and keeps asking every hour until
it gets one. The box needs the right date for that (NTP). A certificate
that has run out cannot be renewed: the cell stops and has to be enrolled
again with a new enrollment string.

**Calls' speech** goes between the cell and the controller encrypted
(SRTP). Unencrypted RTP is possible only if you allow it on the command
line (`uci set opendectcell.main.cell_plain_rtp=1`), for a network you trust
end to end.

## 5. When something doesn't work

The *Log* tab of Status > DECT shows what the base and the SIP client say.
More causes and fixes are in [install.md](install.md#when-something-doesnt-work).

| What you see | What it means, and what to do |
|---|---|
| Status > DECT: *switched off* | Network > DECT, General: tick *Enable the DECT base*, *Save & Apply* |
| *not running although switched on*; Log: "chip firmware missing" | The chip firmware is not on the box, or not where *Advanced* says: [install.md](install.md) step 3 |
| Log: "not a firmware build openDECTcell knows" | The firmware is from a FRITZ!OS version openDECTcell does not know: use one listed in [firmware-sourcing.md](firmware-sourcing.md) |
| *Register a handset*, *Name* or *Deregister* are greyed out | They need the DECT service running |
| "The PIN shown is not in use yet: press Save & Apply first, then register" | The PIN was changed on the page but the base still has the old one. *Save & Apply*, then register |
| The handset says the PIN is wrong | Enter the PIN shown on the page now. Some handsets try 0000 by themselves first: the model profile or *It tries PIN 0000 first* covers that |
| The handset does not find the base when registering | The window is over (default two minutes): *Register a handset* again |
| "Not done: the base is busy with other requests" | Try again in a moment |
| "Not done: the name is too long" | At most 15 bytes: umlauts and other non-ASCII letters count double or more |
| "DECT n is still registered after 40 s" | The base did not let the handset go. See the Log tab; try *Deregister* again |
| VoIP: *failed: 401 Unauthorized* / *403 Forbidden* | User name, login name or password in the profile are wrong |
| VoIP: *failed* with a timeout | The registrar's address or port is wrong, or a firewall blocks SIP |
| VoIP: *not running: no handset has a VoIP profile as a line* | Give a handset the profile as *Line 1*, then *Save & Apply* |
| Cell: *stopped: this cell's certificate expired* | The cell was offline past its certificate's end (or the box's date is wrong: check NTP). Get a new enrollment string from the controller and enrol again |
| Calls connect but one side hears nothing | NAT: try *NAT traversal for media: STUN* in the profile, or check *Network interface* |
| A call is not HD | Both ends need G.722: the handset (Status: *G.722, wideband*), the profile's codecs, and the other party's phone |
| Phonebook: *Last sync: failed* | The reason follows. Check the server address (https only), user name and app password, and that at least one book is ticked |
| A handset searches for its base after a restart | Handsets wait longer and longer between searches. Give it half a minute, or press a key on it |

If you are stuck, please open an issue: [CONTRIBUTING.md](../CONTRIBUTING.md)
says what to include.

## 6. Where the settings are kept

Everything on Network > DECT is stored in `/etc/config/opendectcell` (OpenWrt's
UCI), except the handsets' names and models, which the base keeps next to
each handset's registration (`/etc/opendectcell-sub.*`). Both survive a
reboot and a firmware upgrade (sysupgrade keeps them). For the command line:

| Page field | UCI option |
|---|---|
| General | `opendectcell.main.enabled`, `.cipher`, `.locauth`, `.wideband`, `.twocalls`, `.overlap`, `.dialwait`, `.time_update` |
| Registration | `opendectcell.main.pin`, `.pin_length`, `.regwindow`, `.registration` |
| A handset's lines and settings | `config handset 'dect<N>'`: `option slot`, `option line1`, `option line2`, `list override 'key=value'` |
| A VoIP profile | `config voip 'voip<N>'`: `name`, `line_name`, `display`, `user`, `authuser`, `password`, `domain`, `register`, `regint`, `transport`, `outbound`, `outbound2`, `sipnat`, `medianat`, `stunserver`, `stunuser`, `stunpass`, `rtpkeep`, `media_auto`, `codec`, `ptime`, `dtmf`, `rel100`, `mwi`, `transfer`, `enc_auto`, `mediaenc`, `extra` |
| Settings for all profiles | `opendectcell.main.sip_port`, `.rtp_ports`, `.net_interface`, `.dns_server`, `.rtp_dscp`, `.sip_dscp`, `.jitter`, `.tls_verify`, `.tls_cafile` |
| Phonebook | `config carddav 'carddav'`: `enabled`, `url`, `username`, `password`, `list book`, `default`, `interval`, `ca_file`, `allow_clear` |
| Advanced | `opendectcell.main.firmware_dir`, `.first`, `.second`, `.fw_allow_unverified`, `.uart`, `.factory_mtd`, `.factory_offset`, `.mode`, `.stores`, `.options` |
| Cell mode | `opendectcell.main.role` (`cell`), `.cell_plain_rtp` |

After a change on the command line: `uci commit opendectcell` and
`/etc/init.d/opendectcell restart`. Some settings exist only on the command
line, e.g. `sip_any_source` ([install.md](install.md#5-phone-lines-sip)).

The live files the Monitor reads, `/var/run/opendectcell/state.json` and
`voip.json`, are described in [diagnostics.md](diagnostics.md).
