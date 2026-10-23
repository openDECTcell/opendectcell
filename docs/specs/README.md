# Specifications

The PDFs themselves are **not committed** (see `.gitignore`): they are large,
and ETSI publishes them free, so a fetch command is better than a copy that
goes stale.

| file | standard |
|---|---|
| `en_30017503v020901p.pdf` | EN 300 175-3 V2.9.1 — MAC layer |
| `en_30017504v020901p.pdf` | EN 300 175-4 V2.9.1 — DLC layer |
| `en_30017505v020901p.pdf` | EN 300 175-5 V2.9.1 — Network (NWK) layer |
| `ts_10252701v010501p.pdf` | TS 102 527-1 V1.5.1 — CAT-iq, New Generation DECT part 1 |
| `en_300444v020501p.pdf` | **EN 300 444 V2.5.1 — Generic Access Profile (GAP)** |

Fetch them with:

```sh
UA="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36"
for p in 03 04 05; do
  curl -sL -A "$UA" -o docs/specs/en_300175${p}v020901p.pdf \
    "https://www.etsi.org/deliver/etsi_en/300100_300199/300175${p}/02.09.01_60/en_300175${p}v020901p.pdf"
done
curl -sL -A "$UA" -o docs/specs/ts_10252701v010501p.pdf \
  "https://www.etsi.org/deliver/etsi_ts/102500_102599/10252701/01.05.01_60/ts_10252701v010501p.pdf"
curl -sL -A "$UA" -o docs/specs/en_300444v020501p.pdf \
  "https://www.etsi.org/deliver/etsi_en/300400_300499/300444/02.05.01_60/en_300444v020501p.pdf"
```

`etsi.org` returns 403 without a browser `User-Agent`; that is what `-A` is for.

**Reading them: `-layout` for prose, `-raw` for tables.** `pdftotext -layout`
is right for clause text, and **wrong for the coding tables** — it emits them
column-wise, so all the "bit 8" values come out in one block, then all the
"bit 7" values, and reassembling them by hand is guesswork. On 2026-10-23 that
produced three confident, wrong answers in a row (`<<MULTI-KEYPAD>>` as `0x32`,
`<<DURATION>>` as `0x6E`, and an MM table in which not one of ten known values
survived calibration).

`pdftotext -raw -f PAGE -l PAGE FILE -` renders the same tables correctly, one
element per line with its bits and its clause reference:

```
Multi-Keypad               0 0 1 0 1 1 0 0   7.7.27
Calling Party Number       0 1 1 0 1 1 0 0   7.7.9
```

**Calibrate anyway.** Whatever the method, check the result against identifiers
already verified on the wire (`0x05` portable identity, `0x7C` codec list,
`0x77` IWU-to-IWU) before trusting a new one. That check is what caught all
three errors.

**GAP was missing until 2026-10-23**, which is worth recording: the project's
standing rule is "GAP and CAT-iq are the product", and we had been inferring
GAP's requirements from EN 300 175-5 without the profile document that
actually states which procedures are mandatory. The first audit against it
found a mandatory feature we had not implemented. **Hold the profile, not
just the base standard.**

**Why these five.** Parts 3/4/5 are the DECT stack we implement the host side
of; TS 102 527-1 is CAT-iq part 1, which defines the wideband codec negotiation
(`<<CODEC-LIST>>`) that a wideband call depends on. Clause references in
`call-g7.md`, `registration-g6.md` and `spec-conformance.md` point into these
versions specifically.
