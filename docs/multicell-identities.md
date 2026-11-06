# Multi-cell: DECT identities and inter-cell synchronisation

Research for ROADMAP M11 (openDECTcell) and C9 (opendectcell-controller), 2026-10-29,
by an AI agent from the standards on disk; three load-bearing claims (class
A's 7 cells, GAP table 51 on IPUI type N, -3 6.2.2.1.1 on synchronisation)
were checked against the PDFs afterwards. Standards side only. Nothing here
was measured; chip facts are listed as questions (§10), not answered.

> **Made-up identities are test values.** The EIC and ARI values this project generates for class B multi-cell setups
> are private test values; they are **not allocated by ETSI**. They can collide with a real installation and the resulting
> network is not conformant on identity. Obtain an EIC (manufacturer code) from ETSI before any deployment outside the lab
> or a small group of friends, such as a makerspace.

**Sources on disk** (`opendectcell/docs/specs/`, extracted with `pdftotext -layout`):

| short | document |
|---|---|
| -3 | EN 300 175-3 V2.9.1, MAC |
| -4 | EN 300 175-4 V2.9.1, DLC |
| -5 | EN 300 175-5 V2.9.1, NWK |
| -6 | EN 300 175-6 V2.9.1, identities and addressing |
| GAP | EN 300 444 V2.5.1 |
| CAT1 | TS 102 527-1 V1.5.1 |
| CAT3 | TS 102 527-3 V1.7.1 |

**Not on disk, and needed** (§11): EN 300 175-2 (PHL: slot/frame timing, the
synchronisation port, the inter-RFP timing tolerance) and EN 300 175-7
(security: the handover encryption process, 6.4.7). Every statement below that
would need those is marked as such. Nothing is cited from memory.

Our own notes read first: `docs/journals/multicell-experiments.md`, `docs/capacity.md`,
`docs/stub-image-format.md` §4, `docs/mac-bringup.md` (RFPI section),
`docs/7412-spi-tdm-and-loader.md`, `docs/pcm-tdm-ipq4019.md`;
controller `docs/cell-protocol.md` §7.4, `docs/architecture.md` §8, `ROADMAP.md` C9.

---

## 0. Conclusions

1. **Use ARI class A.** It is the class for "residential and private (PBX)
   single cell FPs and small multi-cell FPs with a maximum of 7 RFPs" (-6 5.1,
   table 4.1). Classes C and D are for public operators, E is PP-to-PP
   (-6 5.3-5.5). Class B is the "system identity" class for larger private
   systems (-6 5.2), but needs an EIC from ETSI, and a class B FP must give
   handsets an IPUI that is **not** type N (GAP table 51). We have neither.
   **Update 2026-10-30:** the maintainer's goal is now a hosted controller for
   dozens to hundreds of cells across makerspaces, which class A cannot carry.
   The route past 7 is a project EIC (class B, §7.7): ETSI sells one for
   EUR 200.
2. **Class A caps one system at 7 cells.** The RPN is 3 bits; RPN 0 means
   "single cell, no intercell handover"; RPN 1-7 separate "a maximum of 7
   different cells" (-6 5.1). The controller's design target of 10 cells does
   not fit one class A FP. §7.6 lists the ways past 7; none is free.
3. **We have no EMC, so we cannot mint a class A ARI.** EMCs come from ETSI
   (-6 5.1, 7.4), FPNs from the EMC holder. Proposal: the controller adopts the
   **factory class A ARI of one enrolled box** (the "donor") as the system
   PARI, forever, and assigns RPN 1-7 to cells. RPNs are explicitly the
   "manufacturer/installer"'s to allocate (-6 5.1).
4. **What makes a handset treat cells as one system** is the standard's
   ordinary machinery, nothing per cell: its PARK{y} includes the PARI
   (-6 4.0, 8.2 rule 1), so it may lock to any RFP broadcasting that PARI; if
   the first LAL bits of PARI+RPN are unchanged it is in the same location
   area and does not re-locate (-6 5.0, GAP 13.2); if only the RPN differs it
   is in the internal handover domain (-6 5.0, figure 5.3).
5. **GAP and CAT-iq handsets must accept any PARI and RPN** (GAP 10.2.1
   table 111: `<PARI>` All, `<RPN>` All; CAT1 repeats it). I found no clause
   that lets a GAP PT refuse a class. The only class rule in GAP is that a
   non-class-A FP shall never send IPUI type N (GAP table 51).
6. **Location areas**: with LAL 36 (what we send today) all cells are one LA,
   handsets do not re-locate between cells (matches M4 on the A540), and every
   page must go out on every cell. LAL 39 makes each cell its own LA: the
   handset then sends `{LOCATE-REQUEST}` on every cell change (GAP 13.2), with
   its old PARI+RPN (GAP table 44). Both are conformant; §6.
7. **The standard assumes one FP is synchronised.** "Both frame and multiframe
   timing shall be synchronized for all RFPs of one DECT FP" (-3 6.2.2.1.1);
   RFPs of one internal handover area share carriers and primary-scan carrier
   at the same time (-3 11.8); the multiframe is synchronised over the whole FP
   area when ciphering is offered (GAP 10.13). The only numeric bound on disk
   is the PP's sync window: ±4 bits to its reference RFP, at least ±10 bits to
   others (GAP 11.7), i.e. about ±8.7 µs at 1 152 kbit/s. The normative
   RFP-to-RFP tolerance is in -2, not on disk.
8. **Unsynchronised cells sharing a PARI break -3 6.2.2.1.1.** It is still the
   only way to give GAP handsets idle roaming without re-registration and
   without our own EMC (§7.6). Mitigation: broadcast "no bearer handover to
   other RFPs" (-3 7.2.4.3.7 info type 0000/0001; mandatory at least every
   10 s for an FT that does not support bearer handover in the whole FT,
   GAP 10.3.4) and connection handover "not supported" (-5 annex F a46).
   Whether the chip lets the host set either is an open question (§10).
9. **Seamless handover across boxes means connection handover, not bearer
   handover.** Bearer handover needs one MBC on the fixed side and the new RFP
   "in the same cluster" (-3 10.6.1 NOTE 1); the MBC lives in the one Cluster
   Control Function (-3 4.1.2). Two chips are two MACs. Connection handover is
   DLC-level: the PT sets up a new MAC connection and the FT's DLC re-routes
   the link (-4 10.5, GAP 9.7; always PT-originated, -3 10.2.0). GAP makes it
   mandatory in the PT, optional in the FT (GAP table 3: M.12; table 2: D.5).
   It needs synchronised cells, a DLC instance and cipher key that can move
   between cells, and a switched voice path.
10. **Without sync, a call ends when the handset leaves its cell.** External
    handover (-5 15.7) is between FPs and is not in GAP. Call survival through
    a link loss is a "may" (-5 17.9 c), not something a GAP handset owes us.

---

## 1. Starting point in our code

| what | where | state |
|---|---|---|
| RFPI on air | STUB patch TLV tag 1, 5 bytes, from the factory DECT block; placeholder "0270" replaced from the `maca` MAC (`stub-image-format.md` §4 step 5, `fwi_rfpi_from_maca()`) | set **only at chip upload** by `dect-fwload`; no override option exists. `mac-bringup.md` "RFPI / identity" assumes the STUB takes its identity from the patch and reports it |
| RFPI in the host | `dect-stubd` copies it from mail `0xF601` (`dect-stubd.c` `handle_reg_mail()`, `DECT_PRIM_CONTACT_ACK1`) into `mcfg.rfpi` | learned, never set |
| placeholder RFPI | class A (ARC 000), a fixed EMC, FPN = 17 low bits of the MAC, **RPN 0** | decoded from `fwi_rfpi_from_maca()` |
| PARK in `<<FIXED-IDENTITY>>` | `dect_mm_fixed_identity()`: type 32 PARK, length 37 (PLI 36 + 1), the 5 RFPI bytes copied verbatim | copies the RPN bits (and the E bit) into the PARK's non-significant bits; fine for RPN 0 / E 0, see §7.4 |
| LAL in `{LOCATE-ACCEPT}` | `dect_mm.c`: `0x40 | 36` for class A ("stock's value in every capture" [CR A-0127]); other classes echo the handset's | the whole PARI is one LA |
| TPUI | slot-derived today; the controller design (cell-protocol §7.4) moves to one pool, unique per LA | matches -6 6.3.2 NOTE 2 |
| controller docs | `cell-protocol.md`: "The DECT configuration of the chip (RFPI/RPN, radio on/off) is **not** cell configuration: the controller sets it directly by mail" | **no RFPI-setting mail is known**; today the RFPI is fixed at upload. Open question Q1 |

---

## 2. The identity structure

**ARI = ARC + ARD.** In the FP it is the ARI; in the PP it is the PARK. A PARK
can stand for a group of ARIs: PARK{y}, where y = PLI is how many ARC+ARD bits
count; the rest are "don't care" (-6 4.0, figure 4.2; -6 6.0).

**RFPI = E + PARI + RPN, 40 bits**, broadcast in the NT-channel at least once
per multiframe as bits a8-a47 (-6 5.0, figure 5.2). Its three purposes: carry
the PARI, "uniquely identify RFPs geographically", and "show domains for
handover and location areas" (-6 5.0). E = SARI list available (-6 7.1).

**PARI, SARI, TARI** (-6 5.0): the PARI is in every RFPI; SARIs are broadcast
less often in the QT-channel (QH = 0101); TARIs are only answered on request.
"Several FPs may apply the same ARI. However, as a PARI it has to be
geographically unique" (-6 5.0 NOTE).

**IPUI = PUT + PUN.** A PP is identified by its pair PARK{y} + IPUI
(-6 8.1 rule 8). IPUI N is the IPEI, globally unique, "primarily intended to be
used for simple FPs with an ARI class A" (-6 6.2.1).

**TPUI**: 20 bits, for paging, valid only in one location area (-6 6.3.1;
-5 5.6.2). One assigned individual TPUI per IPUI-PARK pair per LA, locally
unique; "assigned TPUIs should be unique within the entire FP rather than
within any of the location areas" (-6 6.3.2 NOTE 2; GAP 13.3 NOTE). The PMID
is the assigned TPUI when there is one (-6 9.1.2, GAP 13.4).

**FMID** = 12 LSBs of the RFPI, "geographically unique for a FP, since the RPN
is geographically unique" (-6 9.1.1). For class A that is the 9 low FPN bits
plus the RPN, so distinct RPNs give distinct FMIDs inside one system.

### 2.1 The five classes

| class | intended for (-6 table 4.1) | ARD | who allocates | RFPI split | cells per PARI | SARI/TARI | IPUI types |
|---|---|---|---|---|---|---|---|
| A (000) | residential and private (PBX), single and small multi-cell | EMC 16 + FPN 17 (ARI 36 bits) | EMC: ETSI or provider authorised by ETSI; FPN: the manufacturer, unique per EMC; RPN: "manufacturer/installer" (-6 5.1) | E1 A3 EMC16 FPN17 **RPN3** | **7** (RPN 1-7); RPN 0 = single cell (-6 5.1) | PARI only, never SARI/TARI (-6 5.1) | N, S |
| B (001) | private (PABX) multi-cell, "a system identity that follows a system" | EIC 16 + FPN 8 + FPS 4 (ARI 31 bits; FPN/FPS border may move, sum 12) | EIC: ETSI or authorised provider, also to big companies; FPN: manufacturer to installers; FPS: operator/installer; RPN: operator/installer (-6 5.2) | E1 B3 EIC16 FPN8 FPS4 **RPN8** | 256 (RPN 0-255), more "through geographical separation" (-6 5.2) | yes | O, S, T |
| C (010) | public | POC 16 + FPN 8 + FPS 4 | POC: ETSI; FPN/FPS/RPN: operator (-6 5.3) | RPN 8, LSB 0 = single cell, 1 = multi-cell | 128 multi-cell RPNs | yes | P, Q, R, S, U |
| D (011) | public DECT to a GSM/UMTS operator | GOP 20 + FPN 8 | GOP = PLMN id; only in networks owned by that operator (-6 5.4) | RPN 8, LSB as class C | 128 | yes | R |
| E (100) | PP-to-PP direct | FIL 16 (fixed 0101 pattern) + FPN 12 | the user, 5 keypad digits; "not globally unique" (-6 5.5) | RPN 8, decimal 01-99 | n/a | PARI only | N |

Uniqueness rules: class A "shall be globally unique" (-6 5.1). Class B "in most
cases" globally unique; inside one owner's network it need only be
geographically unique, "to avoid ambiguity at call set-up and handover"; but
also "FPN + FPS shall be unique for each EIC" (-6 5.2). Class C and D:
geographically unique per operator (-6 5.3, 5.4).

Who needs a code: classes A, B and C each need a 16-bit code "received from
ETSI as a 4-digit hex number" (-6 7.4). **No class except E is free to use
without an allocation**, and E is for PP-to-PP only.

---

## 3. Which class an openDECTcell system can legitimately use

| class | usable? | why |
|---|---|---|
| A | **yes, ≤ 7 cells** | fits the environment (-6 table 4.1, 5.1). Needs an ARI we did not allocate: borrow a box's factory ARI (§7) |
| B | **later, if the project obtains an EIC** | the right class conceptually (-6 5.2: system identity, 256 RPNs, PARK{y} over a whole FPN). Costs: an EIC from ETSI or an authorised provider; and every handset needs an assigned IPUI O/S/T, because "All ARI equipment classes other than Class A equipment, shall never send IPUI type N" in `{ACCESS-RIGHTS-ACCEPT}` (GAP table 51). That changes our handset identity model (IPUI N = IPEI today) and is unproven on Gigasets |
| C, D | no | public operator classes; POC or PLMN id required (-6 5.3, 5.4) |
| E | no | PP-to-PP direct communication only (-6 5.5) |

---

## 4. RFPI, RPN, PARK: how a handset decides that two cells are one system

The decisions a PP makes, in order, and the clause behind each:

1. **May I use this FP at all?** "A PP is only allowed to access a FP if one of
   its PARKs includes one of the ARIs of the FP, i.e. the PARI, a SARI or a
   TARI" (-6 4.0; 8.1 rule 6; 8.2 step 1). The PP finds the PARI in the NT
   message while scanning (-3 11.3.2: the management entity accepts a PARI
   "only if it identifies an acceptable ARI").
2. **Which RFP?** It "should then ... select the RFP (that transmits the above
   PARI) with the strongest signal strength" (-3 11.3.2). In Idle_Locked it
   "shall maintain frame and multiframe synchronism with the FP and may
   occasionally scan for RFPs with a stronger signal strength. If a stronger
   RFP is found, then the PP may lock to this RFP instead" (-3 11.3.3.0).
   That sentence assumes the RFPs share frame and multiframe timing (§8.2).
3. **Is it a new location area?** The LA is the first LAL bits of PARI+RPN;
   "As soon as any of these x bits change the PP has entered into a new LA and
   should do a location update" (-6 5.0, figures 5.5, 5.6). Without a LAL the
   default LA is the PARI (-6 5.0, figure 5.6). GAP: location registration "upon
   change of LA" and "upon power-up and after the first lock", if broadcast
   attribute a38 is 1 (GAP 13.2).
4. **May I hand over to it?** Internal handover (bearer and connection) is
   possible "between RFPs that have the same PARI in their RFPIs, i.e. only
   have changes in the RPN" (-6 5.0, figure 5.3). The connection handover
   domain "is always identical to the internal handover domain"; the bearer
   handover domain is the cluster, which the PP takes to be the whole internal
   handover domain unless the FP broadcasts "Bearer handover information"
   (-6 5.0; -3 7.2.4.3.7).

So **nothing per cell is registered**. A subscription is the pair IPUI +
PARK{y} (GAP 13.7 table 128 stores IPUI, PARK, PLI, LAL, last ARI, last RPN,
UAK). Any RFP whose PARI the PARK includes is the same system to the handset.
Registration (`{ACCESS-RIGHTS-REQUEST}`) happens once; moving between cells at
most causes a location registration, and with one LA not even that. This is
what M4 saw on the A540 (`docs/journals/multicell-experiments.md`).

**PLI.** With class A and PLI 36 the PARK names exactly one ARI. A smaller PLI
widens access to every ARI sharing the first y bits (-6 6.0, 6.1.0). With a
borrowed ARI, a PLI below 36 would give our handsets access rights to other
manufacturers' boxes with the same EMC prefix. **Always PLI 36 for class A.**

**SARI is no help for class A**: a class A ARI may only be a PARI (-6 5.1),
and the PP needs Idle_Locked to read SARIs (-6 8.2).

---

## 5. What GAP and CAT-iq require a handset to accept

| item | requirement | clause |
|---|---|---|
| PARI and RPN in the NT message | FT shall send, PT shall receive and process; values "All" | GAP 10.2.1 table 111; CAT1 (same table, `<PARI>` All, `<RPN>` All) |
| ARC+ARD of the PARK in `{ACCESS-RIGHTS-ACCEPT}` | "All", whole PARK including non-significant bits, length PLI+1 | GAP table 51 |
| IPUI type in `{ACCESS-RIGHTS-ACCEPT}` | non-class-A FPs "shall never send IPUI type N" | GAP table 51 |
| SARI list | PT shall receive and process if broadcast; TARI not required | GAP 10.2.4 table 116; GAP table 3 M.13 (PT M) |
| intercell handover | PT mandatory: bearer handover inter-cell (M.10), connection handover inter-cell (M.12), intercell voluntary connection handover (D.5); FT optional. "The PT is required to be able to support handover between RFPs. The invocation of the feature is however optional to the operator" | GAP tables 2, 3; CAT1 and CAT3 repeat them |
| bearer handover information | PT shall support info types 0000-0011; FT not supporting bearer handover in the whole FT shall send it at least every 10 s | GAP 10.3.4; CAT1 7.6.4.5 |
| LAL in `{LOCATE-ACCEPT}` | LI-type 1, LAL 0-39, "even if default LAL" | GAP table 45 |
| store at power-down | IPUI, PARK (complete), PLI, LAL, ARI, RPN, UAK | GAP 13.7 table 128 |
| at least two subscriptions | yes, but one "active IPUI/PARK pair" is used to lock | GAP 14.1 |
| manual PARK entry | optional; PARK length 01-36 | GAP 14.3; CAT3 feature table (GAP.A.3, PT O) |
| external handover | not listed in GAP at all (no hit for "external h" in GAP, CAT1, CAT3) | — |
| access rights modification by the FT | in the base standard (-5 13.5.3), **not** in GAP, CAT1 or CAT3 | — |

**Any class a GAP handset may refuse?** None found. The profile tables say
"All" for PARI, RPN and ARC+ARD, and GAP covers residential, business and
public environments (the R/B and P columns). What a handset firmware actually
does with class B or an assigned IPUI O is a bench question, not a standards
one.

---

## 6. Location areas, paging, location registration

**Definitions.** An LA is "the domain in which a PP may receive (and/or make)
calls as a result of a single location registration" (GAP 3.1). It "consists of
part of one or several DECT systems" (-5 13.4.1). LAL counts "how many bits of
the PARI + RPN are relevant ... starting with the MSB" (-5 7.7.25). For class
A: LAL 36 = the whole PARI, all RPNs one LA; LAL 39 = PARI+RPN, one LA per
cell. -5 13.4.1 NOTE 5 even names the one-cell LA ("for fast set up ... the
size of the location area is one cell").

**When a GAP handset locates** (GAP 13.2, a38 = 1): on change of LA, at the
latest on entering the CC null state; on power-up and first lock; right after
a successful access rights procedure. Also on `{MM-INFO-SUGGEST}` "locate
suggest", sent twice (GAP 8.29), even with a38 = 0. Expiry of a
<<DURATION>> validity is a further trigger (-5 13.4.1; our M6 showed the
AS690H honours it).

**What it sends** when the LA changed: the old ARI (PARI+RPN, type 1) in
<<FIXED-IDENTITY>> and the old LAL in <<LOCATION-AREA>> (-5 13.4.1; GAP
table 44). On entering a new LA it deletes its assigned TPUI first and uses
the default TPUI until a new one is assigned (GAP 13.3).

**Paging.** The LCE pages with the assigned individual TPUI if there is one,
else the default TPUI or part of the IPUI (-5 14.2.3); `<LCE.03>` resubmits
the page after 3 s (-5 annex A). The standard does not say which RFPs page:
that is the FT's business. Consequence for us: **the controller pages on every
cell of the handset's LA**. With LAL 36 that is every cell.

**The two choices:**

| | LAL 36 (today) | LAL 39 |
|---|---|---|
| LA | the whole system | one cell |
| handset moves between cells | no location registration (M4, A540) | `{LOCATE-REQUEST}` on each move, with the old PARI+RPN |
| who learns the roamer first | whatever it sends first: page response, `{CC-SETUP}`, list access (M4, M5) | the new cell, from the LOCATE; gives the 20 s `<MM_locate.1>` budget and the IPUI before any call |
| paging | every cell | the last cell; fall back to all if no answer |
| TPUI | one assignment survives moves | re-assigned on every move (the pool can hand back the same value) |
| cost | page traffic on all cells | a link setup per cell change; ping-pong at cell borders |

Both are conformant. LAL 36 is what works today and what the controller's
"push records to all cells" plan assumes. LAL 39 is worth one bench run: it
would give the controller location tracking without <<DURATION>> polling
(bench question B2, §10.2).

---

## 7. Proposal: system ARI and RPNs

### 7.1 The identity

| field | value | rule |
|---|---|---|
| class | A | §3 |
| system ARI (= PARI, = PARK) | the 36-bit class A ARI of one enrolled box's **factory** RFPI (the donor) | globally unique by its manufacturer's allocation (-6 5.1) |
| PLI | 36 | §4 |
| PARK bytes | ARI, E/spare bit 0, all 4 trailing bits (RPN position) 0 | GAP tables 10, 21, 51: "the whole PARK including the non-significant bits"; every cell must send identical bytes |
| RPN | 1-7, one per cell, unique in the system | -6 5.1 |
| LAL | 36 (option: 39) | §6 |
| E bit | 0 (no SARIs) | -6 7.1 |

Why a donor ARI and not a random one: a class A FPN is the EMC holder's to
allocate (-6 5.1). We hold no EMC. A random value would collide by
construction with someone's real system sooner or later; the donor's ARI was
allocated once, to equipment that is now part of this system. The other
boxes' own ARIs simply go unused while they serve as cells.

Why not each cell's own ARI: different PARIs are different FPs. A handset's
PARK{36} matches one of them, so it would have no access rights at the others
(-6 8.2) and would need a separate subscription per cell, used one at a time
(GAP 14.1). That is not roaming.

### 7.2 First run

1. The controller starts with no system identity.
2. Each cell reports its **factory** RFPI in HELLO (new tag; today only
   `dect-fwload` sees it). The controller checks it: class A (ARC 000), E = 0,
   RPN 0. Anything else: refuse it as a donor and ask the admin.
3. The first accepted cell becomes the donor (or the admin picks one). Its ARI
   becomes the system ARI and is written to the database **before** any handset
   registers. From then on it is never derived again.
4. Every cell, the donor included, gets an RPN from 1 upwards and is loaded
   with RFPI = system ARI + RPN (§7.4).
5. An existing standalone box adopted as the donor: its handsets keep their
   subscriptions. Its PARI does not change; only RPN 0 becomes 1..7, and with
   LAL 36 the LA does not change either. -6 5.1 NOTE describes exactly this
   when a repeater is added ("the RPN of the FT should change from 0 to a value
   in range 1 to 7"). A standalone box adopted as a non-donor cell changes
   PARI: its handsets must re-register.

### 7.3 Adding and removing cells

- **Add:** lowest free RPN 1-7; load the cell with the system RFPI; push
  records (cell-protocol §7.4); then radio on. An eighth cell is refused (§7.6).
- **Remove:** release calls, park, free the RPN. Hold the freed RPN back for a
  while (a day) so logs and handsets' "last RPN" (GAP table 128) do not mix two
  cells. With LAL 36 the handsets do not care.
- **Remove the donor:** the system ARI stays; it is in the database, not in
  the box. But **that box must never again broadcast its factory RFPI** near
  this installation: standalone openDECTcell or AVM firmware would put the system
  PARI on air with RPN 0, every system handset's PARK would match it
  (-6 8.2), and handsets would idle on a base that does not know them. The
  controller records the donor; the cell package should store a "identity
  donated" flag in UCI and refuse to go on air with the factory RFPI while it
  is set.
- **Uniqueness check before radio-on:** if the chip can report RFPIs it hears
  (Q8), refuse to go on air when the system PARI is already on air from a box
  that is not ours.

### 7.4 What the cell (`dect-fwload` / `dect-stubd`) must be told

| item | today | needed |
|---|---|---|
| RFPI = system ARI + RPN | factory TLV tag 1 at upload; no override | a controller-set RFPI patched as tag 1 at upload (a UCI value plus a chip reload), or a runtime mail if one exists (Q1). Fix `cell-protocol.md`'s "sets it directly by mail" until Q1 is answered |
| PARK bytes for <<FIXED-IDENTITY>> | copied from `cfg.rfpi`, RPN and E bits included | a separate `cfg.park`, from the controller, RPN bits zeroed, identical on every cell (`dect_mm_fixed_identity()`; also used in FP-originated `{CC-SETUP}`, GAP table 21) |
| LAL | hard-coded 36 for class A | a parameter (36 or 39) |
| TPUI | slot-derived | from the controller's pool (cell-protocol §7.4, change request L10) |
| bearer handover information, a46 | whatever the STUB broadcasts | unsync: info type 0000/0001 and a46 = 0; sync with connection handover: 0001 and a46 = 1 (§8.5). Q2 |
| the factory RFPI | read by `dect-fwload` only | reported to the controller in HELLO, for §7.2 |

### 7.5 What must stay stable forever

| stable | why |
|---|---|
| the system ARI (36 bits) | every subscription is an IPUI/PARK pair (GAP 13.7); keys belong to the pair (GAP table 50, "Auth key number ... associated to IPUI/PARK pair"). A new ARI means every handset registers again: the FT-initiated modification of access rights (-5 13.5.3) is not in GAP or CAT-iq |
| PLI and the PARK bytes | stored in the handset as given (GAP table 128: "the complete PARK, including non-significant bits") |
| the handsets' IPUIs and UAKs | the other half of each subscription |
| controller database backups | the system ARI exists only there once the donor is gone |

May change: a cell's RPN (with LAL ≤ 36), the LAL (sent in every
`{LOCATE-ACCEPT}`), TPUIs (re-assigned at location registration), the set of
cells up to 7.

### 7.6 More than 7 cells

| way | cost | verdict |
|---|---|---|
| class B with a project EIC | EUR 200 and an ETSI contract (§7.7); FPN+FPS unique per EIC (12 bits); assigned IPUI O for every handset (GAP table 51); IPUI-keyed records change; unproven on Gigasets | **the chosen route, §7.7**; room Q10-Q11 and bench B4a first |
| two class A systems (two donors) | a handset holds one active subscription at a time (GAP 14.1); no automatic roaming between the two | groups of ≤ 7 only |
| a project EMC with blocks of adjacent FPNs, PARK{y < 36} covering a block | ETSI allocation plus a registry of blocks; each cell its own single-cell FP (RPN 0, no handover expected, -6 5.1), PARK{33} covers 8 FPNs, LAL 33 joins them into one LA | conformant **for unsynchronised cells**, which a shared PARI is not (§8.2). Worth considering if sync never comes |
| reuse RPNs on far-apart cells | -6 allows "geographical separation" for classes B, C, D only; class A RPN is "used to separate a maximum of 7 different cells" (-6 5.1) | not for class A |
| derive a second FPN by flipping a bit of the donor's | that FPN belongs to some other box | no |

Until one of the first three is decided, **the controller caps a system at 7
cells.** The 10-cell target in `architecture.md` needs a note.

### 7.7 Past 7 cells: a project EIC, for a hosted network

The maintainer's goal (2026-10-30): a hosted controller for dozens to
hundreds of cells across makerspaces in Germany. Class A cannot carry it:
7 cells per system, no RPN reuse (-6 5.1). The standard's own pattern for
this is class B. Its example for a "business, large multi-cell" system is an
FP broadcasting RFPI B1 and handsets holding "PARK B1 + IPUI O" (-6 annex A,
figure A.5).

**What one EIC gives** (-6 5.2, 6.1.2):

| level | field | count | ours to allocate |
|---|---|---|---|
| project | EIC, 16 bits | 1 | bought from ETSI |
| system (a site, e.g. one makerspace) | FPN 8 + FPS 4 bits, "unique for each EIC" | 3 825 per EIC (ETSI's EIC form) | yes: "FPN is distributed together with the EIC by the manufacturer to authorized installers"; FPS by "the system operator or installer" |
| cell | RPN, 8 bits | 256 per system, "larger than 256 through geographical separation" | yes, "by the operator or installer" |

So a site gets one ARI B (= PARI = its handover and location domain) and up to
256 cells. The project hands out FPNs: per site, or per operator, as the EIC
holder.

**One subscription for every site.** "A PP may be given access rights to all
FPs with the same FPN, by use of a PARK{y}, where y includes only the FPN"
(-6 5.2). With PLI 19 (ARC + EIC) one PARK covers every site under the
project's EIC, and a handset subscribed once has access rights at every
makerspace. Unlike class A with a borrowed ARI (§4), a short PLI is safe here:
every ARI it covers is ours. Choosing between PARK{19} (every site) and
PARK{27} (one FPN's 15 sub-numbers) is a policy decision for the controller.

**Where the handset is.** Sites have different PARIs, so moving between them
is a new location area and the handset sends `{LOCATE-REQUEST}` (-6 5.0,
GAP 13.2). The hosted controller learns the site and pages only there.
Inside a site, LAL picks the location areas as in §6.

**Handset identity.** A class B FP "shall never send IPUI type N" (GAP
table 51), so `{ACCESS-RIGHTS-ACCEPT}` assigns an IPUI O ("locally unique ...
specified by the operator/owner", -6 6.2.3, "used in pair with PARK B"). With
one controller for all sites, controller-wide uniqueness suffices. IPUI T
(EIC + up to 11 BCD digits, "to support roaming between private DECT networks
run by the same owner", -6 6.2.4) is the alternative if sites ever get
separate controllers. The handset side is obliged: GAP lists `<PUT>` "All" in
`{ACCESS-RIGHTS-ACCEPT}` (table 51), and a PT derives its default TPUI from
the allocated IPUI (GAP 13.3). Chip limit to check: the chip's
subscription table holds 5-byte identities [CR A-0007]. An IPUI O with a
number of at most 36 bits fits; whether the chip reads the PUT nibble is
room question Q11.

**Handover** is unchanged by the class. It works inside one PARI (one site),
and only with synchronised cells (§8). Sites never need handover between
them.

**Cost and conditions** (ETSI, 2026-10-30; check before applying):

- EUR 200 per code, VAT excluded, administrative fees included; a signed web
  form plus payment ("Algorithms and codes" page, etsi.org).
- ETSI's EIC undertaking form:
  - the applicant states it is "a manufacturer of DECT fixed parts";
  - it uses the EIC by EN 300 175-6;
  - it uses most of the EIC's 3 825 class B ARIs before asking for a second;
  - it returns the EIC if unused within 5 years;
  - it indemnifies ETSI for breaches;
  - disputes go to the ICC in Paris, under French law.
- So the applicant is a legal person that can sign as a fixed-part
  manufacturer: the project's publisher, an association, or the hosting
  operator. That is the maintainer's decision, and an outward-facing one.
- An EMC (also EUR 200, class A, 2^17 FPNs) does not help here: every class A
  system still stops at 7 cells.

**Gigaset** sells the N870 with 60 base stations per DECT manager and up to
240 per system (Gigaset's N870 manual and datasheet), far past class A's 7.
Which ARI class it uses was not found.

**What the room answered** (Q-0070, A-0131, both builds; the host facts
were read on the 7520 host driver and matched against the 06.88 build):

- The chip owns only the air side. It broadcasts the 40-bit tag 1 RFPI,
  takes its FMID from the RFPI's 12 low bits (class-agnostic), and matches
  handsets by FMID and assigned TPUI. [CR A-0131]
- The host owns all the rest: the identity table (6 x 5 bytes, indexed by
  link; the chip never reads it), TPUI assignment (not derived from the
  IPUI), and every NWK identity match (access rights, locate, page
  response). A page (0x0300) carries the assigned TPUI, not the IPUI.
  [CR A-0131] An IPUI O, and its TPUI, are therefore dect-stubd's (or the
  controller's) business alone.
- There is no runtime mail that sets the RFPI or the RPN. A change means
  re-patching tag 1 and uploading the STUB again; a park keeps the patched
  RFPI. [CR A-0131]
- The STUB reports the RFPI it holds in the contact answer `0xF601`: 5
  bytes after the primitive, nothing else; a new `0xF600` asks again. Stock
  learns its base RFPI from exactly these bytes, and dect-stubd already
  does (`DECT_PRIM_CONTACT_ACK1`). RFPI_IND, which A-0131 named, is another
  mail: a report of RFPIs heard on the air (own and neighbours, with a type
  and flags), not usable with the radio parked. Its primitive is not
  known. [CR A-0132]
- **Open:** whether the STUB reads the ARC bits or treats the RPN as 3 bits
  (class A). Static analysis could not settle it. [CR A-0131]

**What stands between us and a class B system**, in order:

1. ~~The room (Q10, Q11).~~ Answered, A-0131. Left over: the RPN width.
   Bench B0 (no transmission, no EIC needed): patch tag 1 with a class B
   RFPI whose RPN is 8, upload, keep the radio parked, and read the RFPI
   back from `0xF601` [CR A-0132]. *Built 2026-10-30:* uci `rfpi_test`
   (`dect-fwload --rfpi`, `dect-stubd -a rfpicheck=`); card N4 in
   `docs/journals/bench-later.md`. Pass: the 5 bytes come back unchanged
   and the chip runs as usual. Falsified if the RPN comes back altered or
   the STUB does not come up. A pass shows that the STUB stores tag 1
   verbatim. That the beacon carries the same bytes is the room's
   derivation (one stored RFPI), so the beacon itself still has to be
   checked on the air: step 4.
   **Passed 2026-10-30** (7412, 06.88 STUB, observed on hardware; T13 U2
   in `docs/journals/bench-later.md`): with tag 1 = `10 f4 20 11 09` (made up:
   class B, EIC 0F42, FPN 01, FPS 1, RPN 09) the STUB booted and the
   contact answer `0xF601` carried all 5 octets unchanged. So the STUB
   keeps an RPN above 7 as given; the beacon is still to be checked.
2. Bench B4a, possible today with class A: give a handset an assigned IPUI O
   in `{ACCESS-RIGHTS-ACCEPT}` (class A may send any type, GAP table 51) and
   check that the A540, C430A and AS690H accept, store and use it (locate,
   page by the derived TPUI, a call). *Built 2026-10-30:* `dect-stubd -a
   ipuio=N` assigns IPUI O number N (`dect_mm_cfg.assign_id`), shown at the
   desk by `tests/stubd-traces/register-ipuio.trace`; the card is N3 in
   `docs/journals/bench-later.md`.
3. The EIC. *Revised 2026-10-30 (maintainer):* this is experimental, and
   the controller's admin may make one up (opendectcell-controller d20a930:
   "Make one up…" on the Cells page, saved only after the risks are
   shown). There is no legal obligation to hold an EIC that we could find:
   - the German general authorisation for DECT (BNetzA Vfg. Nr. 102/2024)
     asks for 1880-1900 MHz, 250 mW ERP, the 1.728 MHz raster, modulation
     per EN 301 406, conformity with the FuAG, no permanent channel
     occupation and no drone control, and nothing about identities;
   - the EIC is numbering in a voluntary standard plus a contract with
     ETSI, which publishes no list of allocated codes.

   The risk is practical: a made-up EIC may be a real system's, and within
   radio range the two systems' handsets may try each other's bases. With
   class B systems rare and a FRITZ!Box's range small, the maintainer
   expects no harm.
4. Bench B4b with the chosen EIC: class B RFPI, PARK{19}, IPUI O; the same
   handsets.
5. Controller: `system_identity` becomes {class, EIC, FPN+FPS, PLI, LAL}, and
   `cell.rpn` becomes 0-255. The cell is told its RFPI (§7.4); dect-stubd's
   MM sends the assigned IPUI and the class B PARK.

**Bench 2026-11-01: the RPN stays 0 (07.x/08.25 boxes).** One class B PARI with
the cell number in the RPN did not work on the lab 7520 (08.25): the A540 never
reached the chip under RPN 1 or 2 (10 of 10 power cycles, no mail, no LED;
room: A-0143, a refusal at bearer admission before any mail), and attached 5
of 5 under RPN 0; the 7412 (06.88) attaches under RPN 2. The controller
therefore gives every cell an ARI of its own and RPN 0: cell 1 is the identity
(EIC, FPN, FPS), cell n has the FPN counted up by n-1 (wrapping at 256, 255
cells) and the FPS going round 1-15, and the handsets' PARK stays PLI 19
(ARC + EIC), so one registration covers every cell. Moving from one cell to the
next, a handset sees another PARI and locates again (benched with two boxes,
5 of 5; pulling one box's plug moved the A540 to the other within seconds).
The FPS goes round because the FMID, the low 12 bits of the RFPI (RPN 8 + FPS
4, the FPN is above them), is a MAC identity that both builds check in every
burst (A-0145): with RPN 0 only the FPS tells cells apart, so it is a 15-colour
reuse plan, cells that overlap on air need different ones.

---

## 8. Handover and synchronisation

### 8.1 Kinds of handover

| kind | what | domain | clause |
|---|---|---|---|
| intra-cell bearer handover | MAC moves a bearer to another channel of the same RFP | cell | GAP 5.2 M.9; -3 10.6 |
| inter-cell bearer handover | MAC moves the bearer to another RFP "while maintaining the service to the DLC layer" | the cluster (one MBC) | GAP 5.2 M.10; -3 10.6.1 NOTE 1 |
| connection handover (intra/inter-cell) | DLC re-routes the link from one MAC connection to a new one | the internal handover domain: same PARI | GAP 5.1 D.4/D.5, 9.7; -4 10.5; -6 5.0 |
| external handover | between two FPs, PP-initiated, via handover candidate and reference procedures; needs "a common management entity above the two fixed parts" | different PARIs | -5 15.7.0; not in GAP |

**Bearer handover procedure** (-3 10.6.2.1): PT-initiated; the PT's MBC sets
up a new bearer to the wanted FMID; both bearers may run in parallel; the FT
releases one within T203 = 16 frames; at most N201 = 15 reattempts in
T202 = 3 s (-3 annex A). CAT-iq: "The procedure is equivalent for intra- and
inter-cell handover"; the FT should not release the old bearer within 10 ms
(CAT1 7.6.11).

**Connection handover procedure** (-4 10.5; GAP 9.7): always decided by the PT
(-4 10.5); "All setup attempts for connection handover are PT_originated"
(-3 10.2.0); the MAC_CON-req carries the new and the old MCEI (-3 10.2.4.1). GAP
needs only voluntary handover: the PT's LAPC enters "handover pending", a new
MAC connection is set up, then one of the two is released; a failed attempt
leaves the old connection up (GAP 9.7.1). After success, or after N251 = 3
failures, no new attempt for `<DL.06>` = 4 s (GAP 9.7.2.1; -4 annex A).
Ciphered connections: the new connection starts in clear and the PT turns
ciphering on with the stored key (GAP 9.8.2.2); the LLME stores the key for
"a possible later connection handover" (-4 10.6.1.1). The handover encryption
process itself is -7 6.4.7 (GAP 10.15), not on disk.

### 8.2 The synchronisation the standard assumes

| requirement | clause | normative? |
|---|---|---|
| "Both frame and multiframe timing shall be synchronized for all RFPs of one DECT FP" | -3 6.2.2.1.1 | shall |
| frame and multiframe synchronised at all RFPs in a cluster, "maintained across a complete fixed radio termination" | -3 annex D | informative |
| all RFPs of one internal handover area "shall operate on the same set of RF carriers" and "shall have their primary scans on the same RF carrier at the same time" | -3 11.8 | shall |
| "The multiframe shall be synchronized between the RFPs in the whole FP area" (when the FT offers encryption; the multiframe number is broadcast) | GAP 10.13 | shall |
| PP sync window "at least ±4 bits for bearers to the RFP to which the reference timer is synchronized, and at least ±10 bits for other bearers" | GAP 11.7 | shall (PP) |
| public systems: GPS sync or a class 1/2 synchronisation port | GAP annex C | shall, public only |
| idle PP resynchronises at least every T216 = 8 multiframes | -3 11.3.3.0, annex A | shall (PP) |

**What the numbers mean.** A multiframe is 16 frames (-3 6.2.2.1.1) of
0.16 s (-3 7.2.4.3.7: "254 × 0,16 s"), so a frame is 10 ms. At 1 152 kbit/s
(-3 annex I.2 NOTE, informative; normative in -2) one bit is about 0.87 µs, so
the ±10-bit window is about ±8.7 µs. That window covers the RFP-to-RFP offset
**plus** the difference in propagation delay, so cells must be held well
inside it. The RFP-to-RFP tolerance and the synchronisation port classes are
in -2, not on disk (§11).

**Why sync is needed for any internal handover, in plain terms.** A PP keeps
one timing reference. During either handover it runs the old and the new
bearer in the same TDMA frame, in different slots (-3 10.6.2.1; GAP 9.7.1:
two MAC connections exist until one is released). If cell B's frame is not
aligned with cell A's within the window, the PP cannot place B's slots. If B's
multiframe number differs, the cipher stream on B starts from the wrong state
(GAP 10.13). If B's primary scan is on another carrier, the PP's setup attempt
goes where B is not listening (-3 11.8). The first sentence is an inference
from GAP 11.7 and -3 10.6.2.1; the other two are what the cited clauses
require.

### 8.3 What our architecture can reach, given sync

- **Inter-cell bearer handover: no.** It "requires that an MBC for the
  connection exists on both sides ... and that the new selected RFP at the
  fixed side belongs to the same cluster" (-3 10.6.1 NOTE 1). A cluster has
  one CCF, and the MBC is in the CCF (-3 4.1.2). Each FRITZ!Box chip is its
  own MAC. GAP does not require the FT to offer it (table 3, M.10 FT O).
- **Inter-cell connection handover: possible in principle.** It is optional
  for the FT and mandatory for the PT (GAP tables 2, 3). The FT side must:
  accept a PT-originated connection on cell B that is a handover of a
  connection on cell A (Q5); join it to the **same** DLC link, whose C-plane
  LAPC runs on the host (libopendectcell), so cell A's link state, sequence numbers
  and the NWK transactions behind it must reach cell B's host or live in the
  controller; put the stored cipher key on B's new connection (GAP 9.8.2.2;
  Q6); and switch the voice path from A's PCM channel to B's, which the
  controller's media engine can do. Rule 8 of -4 10.5: C-plane on the new
  connection starts at a frame boundary.
- **Prerequisite for both:** §8.2 sync, chip-side (Q3, Q4).

### 8.4 Without sync

| | works? | how / what the user sees |
|---|---|---|
| idle roaming, shared PARI | **yes in practice, with a conformance gap** | the handset loses cell A (no A-field for T207 = 5 s or no NT for T208 = 20 s, -3 11.3.3.0, annex A), scans, finds the same PARI on B, locks. No re-registration, no location registration with LAL 36. Pages missed during the gap; `<LCE.03>` resubmits every 3 s. The gap breaks -3 6.2.2.1.1 and -3 11.8 for "one FP" |
| idle roaming, separate FPs (§7.6, EMC route) | yes, conformant | each cell a single-cell FP, RPN 0; the handset may still look for a while before it switches |
| call moving between cells | **no** | the call continues while cell A still reaches the handset, then drops; the user redials on cell B. With a shared PARI a handset may first try a bearer or connection handover to B and fail; GAP keeps the old connection on a failed attempt (GAP 9.7.1) |
| link re-establishment after loss | **not to be counted on** | -5 17.9 c: an ACTIVE call "may be maintained" after an abnormal link release and the LCE "should request link re-establishment"; `<LCE.04>` 5 s. The NOTE allows clearing circuit-mode calls. If a handset did re-establish on cell B, B's host would need the call state. A bench question (B5), not a design basis |
| external handover | no | not in GAP; between FPs; needs PP support of -5 15.7 |
| a product trick | maybe | the controller keeps the SIP leg up for a few seconds after a radio drop, and offers it to the same handset's next `{CC-SETUP}` on any cell. Non-standard; the user still sees a dropped call and redials |

### 8.5 Broadcast settings for each mode

| mode | RPN | bearer handover info (-3 7.2.4.3.7) | a46 connection handover (-5 annex F) |
|---|---|---|---|
| single cell (today) | 0 | 0000 or 0001 ("Single cell DECT FPs ... shall not broadcast other ... than info type 0000 and 0001") | as now |
| multi-cell, unsynchronised | 1-7 | **0000 or 0001** ("no bearer handover/replacement to other RFPs"); at least every 10 s (GAP 10.3.4) | **0** |
| multi-cell, synchronised, connection handover built | 1-7 | 0001 (no bearer handover to other RFPs, §8.3) | 1 |

I found no clause telling a PT what to do when a46 = 0; it is an attribute, not
a command (-5 annex F). The "handover disable" bit in -3 is test mode only
(-3 7.2.5.4.2, 12.3.1). So with a shared PARI, the bearer handover information is
the only normative brake, and it covers bearer handover only. A handset that
attempts connection handover into an unsynchronised cell fails and keeps the
old connection (GAP 9.7.1).

The RFP status field also has "asynchronous FP available: a PP can expect to
find an alternative FP not synchronized to its actual FP and which the PP has
access rights to" (-3 7.2.4.3.8 table 7.20 NOTE 3). The standard's model of an
unsynchronised neighbour is **another FP**, which supports reading §7.6's EMC
route as the conformant shape of an unsynchronised system.

---

## 9. Recommendations, in order

1. Controller: add `system_identity` (ARI, PLI 36, LAL, donor cell, created)
   and `cell.rpn` (1-7, unique); refuse an eighth cell; record donors (§7).
2. Cell: report the factory RFPI in HELLO; accept RFPI, PARK, LAL and TPUI
   from the controller; build <<FIXED-IDENTITY>> from the PARK, not the RFPI
   (§7.4). The RFPI path is a STUB reload until Q1 says otherwise.
3. Correct `cell-protocol.md` ("sets it directly by mail") and
   `architecture.md` (10 cells vs class A's 7).
4. Ask the room Q1-Q4 before any multi-cell bench run.
5. Bench, two cells (lab 7520 and the 7412), after the room's answers: B1-B3.
6. Fetch EN 300 175-2 and -7 into `docs/specs/` before any sync design.
7. ~~Decide later, with the maintainer: apply for an EIC or EMC (§7.6).~~
   Since 2026-10-30, class B (§7.7) is the route for systems past 7 cells,
   with a made-up EIC while this is experimental; no application for now
   (maintainer). Bench B0 and B4a come first.

---

## 10. Open questions

### 10.1 For the clean room (chip interface facts)

Q1, Q9, Q10 and Q11 were posted as Q-0070 on 2026-10-30 and answered in
A-0131 the same day (§7.7). Q1: no runtime RFPI mail; Q9: a park keeps the
RFPI, the reload time is a bench number.

- **Q1 RFPI.** Is the RFPI only the STUB patch TLV tag 1 at upload, or can a
  mail set it (and the RPN) at runtime? Does the chip broadcast tag 1
  verbatim as the NT RFPI, and derive its FMID from the 12 LSBs (-6 9.1.1)?
  Does anything in the STUB assume RPN 0 or E = 0?
- **Q2 Broadcast.** Can the host set the higher layer attributes (a38, a44,
  a46; -5 annex F) and the Pt "bearer handover information" (info types
  0000-0011; -3 7.2.4.3.7)? What does the STUB broadcast for each by default?
  Is the RFP status field (-3 table 7.20, "asynchronous FP available")
  settable?
- **Q3 Timing source.** Which mode or input sets the chip's TDMA frame timing?
  Is there an external frame-sync input, a master/slave mode, a sync output,
  or sync to another RFP's dummy bearer over the air? On the 7520 the chip is
  the PCM clock master (`pcm-tdm-ipq4019.md`); on the 7412 who drives DCL/FSC
  is still open (`7412-spi-tdm-and-loader.md`). How is the PCM FSC related to
  the DECT frame, if at all?
- **Q4 Counters.** Can the host read or set the multiframe number (GAP 10.13),
  the frame number within the multiframe, and the primary receiver scan
  carrier (PSCN, -3 7.2.3.2.12, 11.8)?
- **Q5 Connection handover, FT side.** Does the STUB accept a PT-originated
  connection setup that is a connection handover (old MCEI, -3 10.2.4.1), and
  which mail tells the host? Does it answer bearer handover requests itself
  (our diagnostics already have a HANDOVER counter [CR A-0123])?
- **Q6 Cipher key and U-plane.** Where does the voice DLC (LU1) run? Can the
  host load a cipher key (DCK) into a connection the chip did not
  authenticate, and switch it to "crypted" (GAP 9.8.2.2)?
- **Q7 Paging.** Can a pending page (`0x0300`, 40-entry list, `capacity.md`)
  be withdrawn on the cells where the handset did not answer?
- **Q8 Neighbours.** Can the chip report the RFPIs and RSSI of RFPs it hears
  (uniqueness check §7.3, site survey, over-the-air sync)?
- **Q9 Reload cost.** How long from STUB reload to beacon, and does a park
  keep the patched RFPI? (Changing a cell's RPN costs one reload today.)

- **Q10 Class B RFPI.** Does the STUB accept a tag 1 RFPI whose ARC is B
  (001), and broadcast it verbatim? Does it take the RPN as the last 3 bits
  (class A) or by class (8 bits for B), anywhere: FMID, handover, a check?
  Does anything reject or rewrite a non-class-A RFPI?
- **Q11 Assigned IPUI in the slots.** The subscription table holds 5-byte
  identities [CR A-0007]. Does the chip read their PUT nibble (type N versus
  O)? Does it derive a paging identity (TPUI) from a slot's identity, and
  how? Does it compare a handset's identity in NWK messages with the slots,
  or is that only the host's?

### 10.2 For the bench (after the room; write the outcomes first)

- **B1** two unsynchronised cells, same PARI, RPN 1 and 2, LAL 36: the A540
  moves between them with no `{LOCATE-REQUEST}` and answers a page sent on
  both. Falsified if it re-registers, shows "no base" for minutes, or answers
  on neither.
- **B2** the same with LAL 39: each move brings a `{LOCATE-REQUEST}` carrying
  the old PARI+RPN (GAP table 44). Falsified if it does not locate.
- **B3** a call while walking from cell 1 to cell 2, unsynchronised: the call
  drops; record whether the handset tries a handover (chip counters, a second
  link from the same IPUI on cell 2) and whether that depends on Q2's
  settings.
- **B4a** (class A, possible now) the base assigns an IPUI O (a number of at
  most 36 bits) in `{ACCESS-RIGHTS-ACCEPT}`. Pass: the A540, C430A and
  AS690H store it; after a restart they locate with it; a page by its
  derived TPUI rings them; a call works. Falsified if any of them rejects the
  accept, re-registers, or answers only to its IPEI.
- **B4b** (needs the EIC) the same with a class B RFPI and PARK{19}.
- **B5** a short radio loss during a call (park one cell for 2 s): does the
  handset keep the call and re-establish (-5 17.9 c)?

GAP 8.22 lets the FT ask a handset which RFP it is locked to (`{IDENTITY-REQUEST}`
for PARI + RPN, "taken from the RFP to which the PT is currently locked"): a
cheap way to check B1-B2 from the host side.

---

## 11. Missing documents

| document | needed for | fetch (same pattern as `docs/specs/README.md`; check the version exists) |
|---|---|---|
| EN 300 175-2 (PHL) | slot and frame timing, the 1 152 kbit/s rate, the synchronisation port classes 1/2 (GAP annex C), the RFP-to-RFP timing tolerance, T216's basis | `.../300175/30017502/02.09.01_60/en_30017502v020901p.pdf` |
| EN 300 175-7 (security) | handover encryption (6.4.7, GAP 10.15), cipher initialisation from the multiframe number (6.4.4, 6.4.5, GAP 10.13) | `.../300175/30017507/02.09.01_60/en_30017507v020901p.pdf` |

Until they are on disk, any statement about sync accuracy beyond GAP 11.7, or
about how ciphering survives a handover, would be from memory.
