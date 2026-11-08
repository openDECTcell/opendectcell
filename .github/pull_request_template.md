## What and why

## Checklist

- [ ] `make check` passes
- [ ] Every commit is signed off (`Signed-off-by:`, `git commit -s`); AI assistance, if any, is stated above
- [ ] New source files start with an `SPDX-License-Identifier` line (GPL-2.0-or-later; `kernel/` is GPL-2.0-only)
- [ ] No material from a vendor's firmware, drivers, disassembly or device trees ([AGENTS.md](../AGENTS.md) §2); no real handset identities, keys, phone numbers or addresses
- [ ] A change to shared code (`src/libopendectcell/`, `dect-stubd`, the init script, packaging) says here what it does on the other boxes and how to bench it (AGENTS.md §1b)
