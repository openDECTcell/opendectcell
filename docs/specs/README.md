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

Fetch them with:

```sh
UA="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36"
for p in 03 04 05; do
  curl -sL -A "$UA" -o docs/specs/en_300175${p}v020901p.pdf \
    "https://www.etsi.org/deliver/etsi_en/300100_300199/300175${p}/02.09.01_60/en_300175${p}v020901p.pdf"
done
curl -sL -A "$UA" -o docs/specs/ts_10252701v010501p.pdf \
  "https://www.etsi.org/deliver/etsi_ts/102500_102599/10252701/01.05.01_60/ts_10252701v010501p.pdf"
```

`etsi.org` returns 403 without a browser `User-Agent`; that is what `-A` is for.
To search them, `pdftotext -layout FILE -` and grep the text.

**Why these four.** Parts 3/4/5 are the DECT stack we implement the host side
of; TS 102 527-1 is CAT-iq part 1, which defines the wideband codec negotiation
(`<<CODEC-LIST>>`) that a wideband call depends on. Clause references in
`call-g7.md`, `registration-g6.md` and `spec-conformance.md` point into these
versions specifically.
