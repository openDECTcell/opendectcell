# How the clean room works, and what the citations mean

openDECTcell drives a DECT radio chip whose firmware and interface are not documented. To write an independent implementation
that only interoperates with the hardware, the work is split in two, and only facts cross the split.

**The two sides.**

- *The analysis side* has the manufacturer's material (the stock firmware image of the box, the manufacturer's published GPL
  source drops, captures of the stock software working). It answers written questions with **facts only**: a mail number,
  a field layout, a register, an ordering, a timing. It never sends code, listings or the manufacturer's symbol names.
- *The implementation side* is this repository and the people and AI coding agents who write it. It never opens the
  manufacturer's material. When the standards do not answer a question, it writes the question down. Everything that goes over
  the air is written from the public ETSI standards (EN 300 175, EN 300 444 GAP, TS 102 527 CAT-iq), not from the chip.

**The record.** Questions and answers are numbered files in a record kept outside this repository. In the code and the
documents, **`[CR A-0124]`** means "answer 124 of that record": the fact it gives is cited so a reader can tell where a
statement about the chip comes from. Other labels (`[CR 7362SL/A-0002]` for the 7362 SL, `[CR COMMAND_SET §8.2]` and similar)
cite a section of an answer. The record is not published; the statements it supports are written out in this repository's
documents ([docs/stub-api.md](stub-api.md) and the others), in our own words.

**What is deliberately not here.** No manufacturer firmware, binaries, kernel modules, device-tree source or calibration data
in this repository; no analysed code; no manufacturer symbol names (they were replaced by role phrases before publication).
The chip firmware is taken by each user from the official image of their own box ([firmware-sourcing.md](firmware-sourcing.md)).
The kernel-module repositories for the 7412 and the 7362 SL are separate, and their READMEs say where their code came from.

**Legal basis claimed:** EU Directive 2009/24/EC Art. 5(3) and 6 (observation and interoperability), in Germany §§ 69d and
69e UrhG. This is the project's reading, not a legal opinion; see also [licensing.md](licensing.md).

**AI agents.** The code and documents are largely written by AI coding agents under the rules in [AGENTS.md](../AGENTS.md),
reviewed and directed by the maintainers.
