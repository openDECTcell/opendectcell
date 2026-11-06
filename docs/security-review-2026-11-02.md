# Security review before publication, 2026-11-02

An adversarial review of this repository (and of the controller, `docs/security-review-2026-11-02.md` there): the
air-facing code of libopendectcell, the control protocol, dect-stubd, dect-cell, the kernel module, the OpenWrt packages and the
LuCI app. Method: code reading, `make check` (ASAN/UBSAN), clang libFuzzer with ASAN and UBSAN over the cell and control
codecs, list access, the link/MM/CC stack, the base with control verbs, the file loaders and the firmware image builder (about
120 million executions, seeded from the golden traces), `gcc -fanalyzer`. `dect-cell` and the SRTP code were read by hand
only (no mbedTLS headers on the review machine). Nothing was tried on hardware.

## Fixed (libopendectcell API 154 and 155 and the OpenWrt files)

| Finding | What was wrong | What changed |
|---|---|---|
| **H-1 (high)** list access before authentication | A peer that knew a handset's IPUI (sent in the clear) could read and change the phonebook and the call lists, and rename handsets, while the base's authentication request went unanswered | `struct dect_link.auth_required`: the handset's {CC-IWU-INFO} is held until `mm.authenticated` (`dect_link_tick`), dropped if the call ends first. Traces: `list-access.trace` (the handset answers, then the list is served), `list-access-unauthenticated.trace` (it never does: nothing is served) |
| **M-1 (medium)** out-of-bounds read | `dect_contacts_fold_cmp` read past a 65-byte table for any 2-byte UTF-8 character above U+00FF (Cyrillic, Greek, Polish names; reachable from the air through SEARCH ENTRIES) | bounds check; test in `test_contacts.c` |
| **M-3 (medium)** dial string into a SIP URI | the keypad octets a handset sent went to the bridge, and from there into a SIP URI, unfiltered | `dect_base_call_out` accepts `0-9 * # + A-D` only. Test in `test_base.c`. The baresip module still builds the URI with `account_uri_complete_strdup`: build it explicitly |
| **M-4 (medium)** CardDAV credentials | the Basic credentials went to every redirect target and every absolute href the server named | sent to the configured origin only; a cross-origin redirect is not followed (`carddav.uc`) |
| **L-1** weak random fallback | `gen_rand` fell back to a clock-seeded generator without a word | the kernel's `getrandom`, and the daemon stops if it cannot give numbers |
| **L-2** fixed `/tmp` names | the init script wrote `/tmp/dect-fwload.log` and `/tmp/dect-warm.log` as root | `/var/run/opendectcell/` (0700). (closed 2026-11-03, see L-3) the LuCI discover file (`/tmp/opendectcell-carddav-discover.json`, holds the CardDAV password) and the firmware upload file |
| **M-6 (medium)** "every call is encrypted" | the documents promised more than the code does: a call whose cipher request is rejected or never completes goes on unciphered, and the DECT cipher and authentication are weak | wording in `README.md`, `docs/status.md`, `docs/luci-manual.md` |
| **L-8** `-v` logs list contents and challenges | `-v` printed every NWK message in hex: RAND/RES/RS, phonebook and call-list contents, dialled digits; normal lines showed dialled numbers whole | `-v` prints NWK messages without those elements (`dect_nwk_format_redacted`, API 156); `-p` is the explicit full dump; dialled numbers in normal lines lose their middle digits (`dect_mask_number`). Tests in `test_nwk.c`, `test_clip.c` |
| **L-3** open-ended rpcd ACL, hand-over files in `/tmp` | `carddav.uc discover *`, `enroll *`, `logread -l *`, `--ctl *` took any argument; the credentials, the enrolment string and the firmware upload were handed over in predictable `/tmp` names | every command names its verb and its slot or file (`logread -l 2000`, `forget [1-6]`, `--ctl deregister hs=[1-6]`, ...); the hand-over files are in `/var/run/opendectcell-ui` (root-only, made by the service before each handover, `ui_dir`); `carddav.uc discover` takes only that file; `tests/test_luci_acl.py` fails on an open-ended or /tmp entry. Checked on the lab 7412 at the command line. **The browser flows did not work** (found 2026-11-03: "Permission denied"): rpcd matches a file ACL against the resolved path and `/var/run` is `/tmp/run` on the box, so each hand-over file is now listed under both names (`tests/test_luci_acl.py` fails without the pair). Replayed through a web session on the 7412 (write the token file, `enroll_start`, read the status): works. Not yet clicked in a browser |
| **L-4** `strcpy` into `sun_path` | the developer subcommand `--bridge-echo` copied its socket path unchecked | the length is checked first (2026-11-03) |
| **L-5** no PIN attempt limit, default PIN | a PIN could be guessed at the radio's speed; `dect-stubd -g` without `-P` ran with `0000` | a window closes itself after 10 wrong PINs (`dect_base_tick`, API 155); `-g` without `-P` is refused. Tests: `test_base.c`, `test_stubd.c` |
| **L-6** FT-authentication oracle, {DETACH} adoption | a peer claiming a held identity could draw unlimited RES2 values for challenges it chose; an unauthenticated {DETACH} moved the connection onto another handset's identity | at most 3 answers per link (`DECT_MM_FT_AUTH_MAX`); a {DETACH} names, it does not adopt. Tests in `test_mm.c`. Not done: a limit across links |
| **L-7** keys never wiped | session keys and the PIN copy stayed in the link after release | `dect_mm_wipe()` on every release (volatile writes). Test in `test_mm.c`. The stored subscription keys stay by design |

## Open

| Finding | Summary | Suggested fix |
|---|---|---|
| **M-2 (medium)** release signing | the key is a plain repository secret and is in the SDK build tree while unpinned feeds compile; the only copy is the secret; users fetch the trust anchor from the release page | a protected environment with required reviewers; sign in a last step; pin feeds; keep an offline copy; publish the fingerprint in the README **Built 2026-11-06:** the `sign` job of `packages.yml` is the only one that sees the key (docs/releasing.md); the environment settings are set when the repository moves to its organisation. |
| **M-5 (medium)** everything runs as root | `dect-stubd` and `dect-cell` still run as root next to the keys. **Done 2026-11-03:** baresip runs as the user `opendectcell` in `/var/run/opendectcell-sip` and listens on the LAN address (`sip_bind`); an account that does not register takes calls only from `list sip_peer`; the bridge sockets belong to the group `opendectcell`. Tested on the lab 7412: baresip as `opendectcell`, registered, bridge connected; an unsolicited INVITE gets 403, from a listed peer it is taken | **Decision 2026-11-03: `dect-stubd` and `dect-cell` stay root for now (accepted risk).** A drop would need a small privileged helper for the chip's reset-line hold (sysfs GPIO), the LED and the handset stores under `/etc`, and hardware testing on both boxes. Reopen if a daemon bug is found that gives code execution from the air or the LAN |
| info | kernel module, cell pinning and enrolment, SRTP (by hand), the init script, `firmware.sh` and the LuCI JS showed no exploitable bug | |

The full reports are kept by the maintainer outside the repository while findings are open.
