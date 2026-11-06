# Glossary

The words and abbreviations the code and the documents use. DECT terms follow the ETSI standards
([specs/README.md](specs/README.md) says which and where to get them); the project's own words are in the last
section. Where a term has a home in the code, the file is named.

## Parts of a DECT system

| Term | Meaning |
|---|---|
| **FP**, fixed part | The base station side. A FRITZ!Box running openDECTcell is a fixed part. |
| **PP**, portable part | A handset. |
| **RFP**, radio fixed part | One radio unit of a fixed part: one box with one DECT chip is one RFP, in this project also called a **cell** when it is run for a controller. |
| **GAP** | Generic Access Profile (EN 300 444): the baseline every handset and base speaks (registration, calls, caller number). |
| **NG-DECT**, **CAT-iq** | The newer profiles (ETSI TS 102 527, parts 1 to 5): wideband audio, line names, list access (phonebook, call lists), call waiting. **Part 1** handsets (many Gigasets) get wideband and little else; **Part 3** handsets (for example the Gigaset A540) get the full set. |
| **Cell** | Used by the controller project: a box that is run by a controller instead of on its own. See [bridge-interface.md](bridge-interface.md) for the standalone box and [hosting-dect-base.md](hosting-dect-base.md) for a host that runs the stack for a cell. |

## Identities

| Term | Meaning |
|---|---|
| **IPEI** | International Portable Equipment Identity: the handset's own serial identity, printed under its battery. |
| **IPUI** | International Portable User Identity: how a base knows a registered handset. Type N is made from the IPEI. |
| **TPUI** | Temporary Portable User Identity: the short number a base pages a handset by. Which values the 06.88 chip can hear is a hardware detail ([more-than-six.md](more-than-six.md), `dect_tpui_hearable()` in `src/libopendectcell/dect_mm.c`). |
| **ARI**, **PARI** | Access Rights Identity: what identifies a DECT system; PARI is the primary one a handset stores when it registers. **Class A** ARIs belong to a manufacturer's code (EIC) and allow up to seven cells; **class B** is for large systems ([multicell-identities.md](multicell-identities.md)). |
| **RFPI** | Radio Fixed Part Identity: what a cell broadcasts: the ARI plus the cell's number. |
| **RPN** | Radio fixed Part Number: the cell's number inside the system. |
| **EIC** | Equipment Installer's Code: the 16-bit manufacturer part of a class A ARI. |
| **FMID** | The low 12 bits of the RFPI. The chip's MAC layer checks it in every burst, so cells that hear each other must differ in it. |
| **PARK** | Portable Access Rights Key: the ARI plus its length; a handset stores it at registration to recognise the system. |

## Protocol layers

From the radio up. `docs/stack-in-library.md` and the library README ([../src/libopendectcell/README.md](../src/libopendectcell/README.md))
show where each layer lives.

| Term | Meaning | Code |
|---|---|---|
| **PHY**, **MAC** | Radio and medium access. Done by the DECT chip's firmware, not by us. | the chip |
| **DLC**, **Lc**, **LAPC** | Data link control. Lc frames the signalling channel; LAPC is the acknowledged link protocol on top of it (a LAPD relative). | `dect_lc.c`, `dect_lapc.c`, `dect_link.c` |
| **C-plane**, **U-plane** | Control (signalling) plane and user (speech) plane. | |
| **NWK** | The network layer: everything a handset and base say to each other above the link. | `dect_nwk.c` |
| **MM** | Mobility Management: access rights (registration), authentication, location registration, TPUI assignment, identity. | `dect_mm.c`, `dect_dsaa.c` |
| **CC** | Call Control: setting up, answering and releasing a call. | `dect_cc.c`, `dect_calls.c` |
| **CS** | Call-related supplementary services, for example call information and call waiting. | `dect_cs.c`, `dect_callinfo.c` |
| **IWU** | Interworking information: the part of a call message that carries extra data; CAT-iq list access travels in it, and this is where the list access responder lives. | `dect_iwu.c` |
| **CLIP**, **CNIP** | Calling line identity / name presentation: the caller's number and name on the handset. | `dect_clip.c` |
| **MWI** | Message waiting indication: the voicemail count on the handset. | |
| **List access** (LiA) | The service a Part 3 handset uses to read the base's phonebook, call lists and the names of other handsets (the Internal Names list). | `dect_iwu.c` (the responder), `dect_contacts.c`, `dect_calls.c` |
| **G.722** | The wideband speech codec; **G.711** (A-law, µ-law) is the narrowband one. | `dect_g722.c`, `dect_audio.c` |

## Security

| Term | Meaning |
|---|---|
| **AC** | Authentication Code: the PIN a user types at the handset to register. |
| **UAK** | User Authentication Key: the secret a handset and base share after registration. |
| **DSAA** | DECT Standard Authentication Algorithm: how the UAK and the session keys are derived (`dect_dsaa.c`). |
| **DSC** | DECT Standard Cipher: the air-interface encryption. It is weak by today's standards; the project uses it because handsets support nothing else. |
| **Locate authentication** | The base asks the handset to prove its UAK in a location registration, so a handset that merely knows an identity cannot attach (`-a locauth`). |

## The chip and the box

| Term | Meaning |
|---|---|
| **DECT chip** | The radio part of the FRITZ!Box: a Dialog SC144xx. It runs its own firmware. |
| **STUB** | Our name for the firmware on the DECT chip (it is uploaded at every start). It does PHY and MAC and speaks a mail protocol with the host. Builds are named by number, for example 06.88 (7412), 07.56 and 08.25 (7520). |
| **Mail** | A message between host and chip, framed and checksummed ([stub-api.md](stub-api.md) is the catalogue, `dect_mail.c` the framing). |
| **Control slot** | The one PCM time slot on which mails travel; found by probing because its position moves between starts ([stub-pcm-control-channel.md](stub-pcm-control-channel.md)). |
| **PCM / TDM** | The serial bus between the router's SoC and the chip; it carries mails and speech in fixed slots. On the 7520 it is the IPQ4019's TDM block, on the 7412 the VR9's SSC in SPI-slave mode, on the 7362 SL the VR9's TDM block. |
| **Lane** | One speech channel on the PCM bus (up to three at once per chip). |
| **Subscription slot** | One of the six places in the chip that hold a registered handset's key. More than six handsets work by treating the slots as a cache ([more-than-six.md](more-than-six.md)). |
| **Park** | Switching the radio off cleanly: close registration, radio off, then check on the chip that it took. The service does it at every stop. |
| **Board profile** | The file in `boards/` that tells the init script what a particular FRITZ!Box model needs. |

## openDECTcell's own words

| Term | Meaning |
|---|---|
| **libopendectcell** | The DECT protocol library in `src/libopendectcell/`. Hardware-free; hosted by `dect-stubd` on a box and by the controller through cgo. |
| **dect-stubd** | The daemon that runs the base on a box (`src/stubd/`). |
| **Bridge** | The control and audio sockets between `dect-stubd` and the SIP side ([bridge-interface.md](bridge-interface.md)). |
| **Host** | Whoever runs the stack: `dect-stubd`, or the controller. The library calls the host's functions (a table of callbacks) to send mails, store records and time things. |
| **Record** | The host's data for one registered handset (identity, key, name, lines). The chip's slots are a cache of records. |
| **Handset profile** | A file in `profiles/` with per-model settings (a Gigaset wants this display text, an A540 that call information); [handset-profiles.md](handset-profiles.md). |
| **VoIP profile** | A SIP account and its settings as the web interface shows them (Network > DECT, VoIP Profiles). Not related to the handset profile. |
| **Golden trace** | A recorded exchange of mails between host and chip that the tests replay against the library and compare, line by line ([../tests/README.md](../tests/README.md)). |
| **API version** | The number in `dect_proto.h` that changes whenever the library's public interface does; every change is described in [api-versions.md](api-versions.md). |
| **Controller** | The separate project (`opendectcell-controller`) that runs many cells as one DECT system, with one web interface and database. |
| **Evidence labels** | `[observed]`, `[analysed]`, `[CR A-NNNN]`, *assumed*: how sure a statement in the documents is ([README.md](README.md#labels-used-in-the-documents)). |
