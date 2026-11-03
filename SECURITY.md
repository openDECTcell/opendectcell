# Security policy

openDECTcell runs as root on routers and handles phone calls, handset keys and SIP credentials, so security reports are
welcome and taken seriously.

## Reporting a vulnerability

**Please do not open a public issue for a security problem.** Report it privately through GitHub: on the
repository's *Security* tab choose *Report a vulnerability* (private vulnerability reporting). Say:

- what you found and where (file, function, option or interface);
- which release or commit, and which box and OpenWrt version if it concerns a running system;
- how to reproduce it, or why you believe it is exploitable;
- whether it needs network access, a handset in radio range, or a local login.

Please keep real phone numbers, SIP credentials, handset identities and keys out of the report; mask them.

You will get an answer, and a fix or a decision, as soon as the maintainers can; this is a small project, so
please allow some days. A fix is released with a note in the release text, and the reporter is named if they wish.

## What is in scope

- The DECT stack (`src/libopendectcell/`), the daemon (`src/stubd/`), the cell link (`src/cell/`), the SIP module
  (`src/baresip/`), the kernel modules, the OpenWrt packages, the LuCI pages and the release process.
- The separate [opendectcell-controller](https://github.com/openDECTcell/opendectcell-controller) has its own report channel.

## Known weaknesses of DECT itself

The DECT standard cipher and authentication are old and weak by today's standards; openDECTcell uses them because
handsets support nothing else. They protect against casual eavesdropping on the radio link, not against a
determined attacker. This is a property of the standard, not a vulnerability in openDECTcell, but a report that makes
it worse or shows a downgrade in our code is in scope.

## Your data

What the box stores about you, where, and how to remove it: [docs/privacy-and-data.md](docs/privacy-and-data.md).

## What we already know

The findings of our own reviews, what was fixed and what is still open, are in
[docs/security-review-2026-11-02.md](docs/security-review-2026-11-02.md).
