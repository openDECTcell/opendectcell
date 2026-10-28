# Bench card: the next session (written 2026-10-28)

For the maintainer, with the A540, C430A and AS690H at hand. Seven tests, about 50 minutes.
Every test checks one change made at the desk on 2026-10-28, none of which has
been on air yet. What each result means is written down in advance; the
technical detail is in `docs/spec-sweep-2026-10-28.md` ("Next bench tests").

**Before anything:** the box runs `bd7ede5` or later (`md5sum /tmp/dect-stubd`
against `build/dect-stubd.arm`; deploy if they differ), the radio is parked. After
a reboot run `SECOND=dectfw_secondlevel_441-07.56.hex ./fwload.sh` first. All
`./ringone.sh` commands run in `work/7520/lab`; the `ssh` ones from anywhere.
Wait for each run to end (it parks the radio) before starting the next. Press a
key on a handset before each test so it finds the base quickly.

Tell the agent after each test what you saw; it reads the log and confirms the
park.

## 1. Caller ID on the C430A: the name as display text (S3, 5 minutes)

Two rings on the **C430A**. Do not answer; read the display while it rings, then
let it ring out.

```
EXTRA=clip=0301234567,cnip=Wednesday,cnipalpha=0,infoie=28095765646e6573646179 HOLD=1 ./ringone.sh c430a
EXTRA=clip=0301234567,cnip=Wednesday,cnipalpha=0,infoie=2811020a1a910202095765646e65736461790d HOLD=1 ./ringone.sh c430a
```

| you see | meaning |
|---|---|
| "Wednesday" in the first ring | the name has to come as display text; plain text is enough |
| "Wednesday" only in the second | it needs the Gigaset's display framing: a Gigaset profile entry |
| the number only, both times | the name comes some other way still; the agent asks the the hardware-analysis step |

If the C430A shows the name, repeat the same ring on the **AS690H**
(`./ringone.sh as690h`).

## 2. The A540's silent ring (S2, 5 minutes)

Two rings on the **A540**. Do not answer; **listen**: aloud or silent?

```
EXTRA=clip=0301234567,lineid=1 HOLD=1 ./ringone.sh a540
EXTRA=clip=0301234567,lineid=1,linetype=1,callid=1,callstatus=1 HOLD=1 ./ringone.sh a540
```

| you hear | meaning |
|---|---|
| silent, then aloud | the A540 wants the full call information, as stock sends it: build it that way |
| silent both times | an A540 quirk; it goes into the A540's profile |
| aloud both times | last week's silence had another cause; the agent re-reads that log |

## 3. Call timer after the call-status change (S6, 5 minutes)

Since `b9f8f8d` only the A540 gets call status (the standard forbids it for the
C430A and AS690H). Check that nothing was lost. On each handset dial **123**
and keep the call for 20 seconds.

```
MAXSECS=90 HOLD=20 ATTACH=600000 EXTRA=playpcm=/tmp/wb16k.pcm ./ringone.sh c430a
MAXSECS=90 HOLD=20 ATTACH=600000 EXTRA=playpcm=/tmp/wb16k.pcm ./ringone.sh a540
```

(Copy the test sound first if the box was rebooted: `scp -O work/audio/wb16k.pcm root@192.168.1.1:/tmp/`.)

| you see | meaning |
|---|---|
| a timer on both, as on 2026-10-28 | the change cost nothing |
| no timer on the C430A any more | it did use our call status; the agent notes it for the Gigaset profile |
| no timer on the A540 | the change misfired; the agent reads the log |

## 4. Encryption, now with the key for the chip (S1, 10 minutes)

A call with encryption on, twice: the key bytes in the two possible orders. On
the **A540** dial **123**, keep the call 20 seconds. Silence is expected (no test
sound); the point is whether the call **stays up**.

```
ssh root@192.168.1.1 '/tmp/dect-stubd -g -P 0000 -s /etc/opendectcell-sub.a540 -s /etc/opendectcell-sub.c430a -s /etc/opendectcell-sub.as690h -p -t 120 -a mask=0x0002,codec=3,status=8000 > /tmp/vS1a-$(date +%H%M%S).log 2>&1'
ssh root@192.168.1.1 '/tmp/dect-stubd -g -P 0000 -s /etc/opendectcell-sub.a540 -s /etc/opendectcell-sub.c430a -s /etc/opendectcell-sub.as690h -p -t 120 -k plain -a mask=0x0002,codec=3,status=8000 > /tmp/vS1b-$(date +%H%M%S).log 2>&1'
```

| you see | meaning |
|---|---|
| the call holds in one of the two | encryption works; that byte order is the right one, and the service can default to encryption again |
| it drops at once in both | the agent compares the mail with stock's byte by byte |

## 5. Internal call ring and a quick redial (S4, S5, 5 minutes)

```
ssh root@192.168.1.1 '/tmp/dect-stubd -g -P 0000 -s /etc/opendectcell-sub.a540 -s /etc/opendectcell-sub.c430a -s /etc/opendectcell-sub.as690h -p -C -t 180 -a mask=0x0002,codec=3,status=8000,twocalls > /tmp/vS45-$(date +%H%M%S).log 2>&1'
```

While it runs (3 minutes):
1. On the **A540**: INT, DECT 2. **Listen to the C430A's ring**: is it different
   from an external call's (internal pattern)? Answer, talk, hang up.
2. On the **A540**: dial 123, hang up, and **redial at once**. Does the second
   call connect?

| you see | meaning |
|---|---|
| a different ring for the internal call | the internal signal works |
| the same ring | the handset ignores it; nothing to fix |
| the redial connects | a second call on the same link works now |
| the redial shows nothing or "not possible" | the agent reads the log |

## 6. Wrong PIN, after the reject change (test 9 again, 5 minutes)

A failed registration now sends the reject the standard asks for first. Check
the handset still says the PIN was wrong. Registration is open for 3 minutes;
on the **AS690H** start a registration and enter the wrong PIN **1111**.

```
PIN=31415926 ./regall.sh as690h
```

| you see | meaning |
|---|---|
| the handset reports a wrong PIN, as before | the new reject is fine |
| it hangs or retries for a long time | the agent reads the log |

Then register the AS690H properly with **31415926** in the same window (or run
the command again), so it keeps working.

## 7. Releasing the link ourselves (nlr, 5 minutes)

A new option makes the base end the radio link right after a call, as stock
does. Off by default; this run decides whether it can be on.

```
ssh root@192.168.1.1 '/tmp/dect-stubd -g -P 0000 -s /etc/opendectcell-sub.a540 -s /etc/opendectcell-sub.c430a -s /etc/opendectcell-sub.as690h -p -C -t 150 -a mask=0x0002,codec=3,status=8000,nlr > /tmp/vNLR-$(date +%H%M%S).log 2>&1'
```

While it runs: on the **A540** dial 123, talk 5 s, hang up. Wait 10 s, then dial
123 again. Then open the A540's **internal list** (INT) and leave it again.

| you see | meaning |
|---|---|
| both calls fine, the list opens and closes normally | nlr is harmless: the agent makes it the default |
| the second call fails, or the handset shows "no base" for a while | releasing the link confuses it: keep nlr off, the agent reads the log |
| the list hangs or shows an error | the DISC after list access is wrong: the agent reads the log |

The log should show `link: released after the call (0x0202 ...)` after each call
and `... after the list session, DISC first ...` after the list.

## Also watch for, in every run

The agent checks the logs for one new line: `lapc: *** ... no acknowledgement
after 3 retransmissions -- link released ***`. If it ever appears during a call
that was working, the new link-loss rule is too strict.

## Results, 2026-10-28 afternoon `[observed on hardware]`

Box at `fdbb0718` (API 38), STUB 07.56, the maintainer at the bench. Logs in
`work/box-tmp/`, passing runs in `work/7520/golden.txt`.

| test | result | log |
|---|---|---|
| 1 caller name, C430A | **the name shows only as base-drawn display text.** Plain {CC-INFO} <<MULTI-DISPLAY>> "Wednesday": a separate text screen without the ringer symbol, then the handset's own ringer screen with the number takes over. Stock's AS690H framing: the same. Stock's C430A two-line framing: the text screen showed the number line, not the name line, for 6-8 s, then the ringer screen again. The standard <<CALLING-PARTY-NAME>> is ignored by this Part 1 handset. A Gigaset manufacturer-profile matter (vendor display framing), not standard work | `vRINGC430A-20261028-095744-1`, `-100*` |
| 2 A540 ring | **solved.** Bare line id: silent (control, as 21c). Stock's full <<CALL-INFORMATION>> (line, line type, call id, CS call setup): **rings aloud and shows "via Line 1 0301234567"**, the first caller number on the A540 | `vRINGA540-20261028-100803-1` |
| 3 call timers | **pass.** C430A without call status (Part 1): its own timer still appears (8-10 s in, already at 00:04). A540 with call status (Part 3): timer at once, CONNECT `... 7e 06 01 00 81 02 01 85` | `vRINGC430A-20261028-101004-1`, `vRINGA540-20261028-101227-1` |
| 4 encryption | **works.** The chip acknowledged `0x0208` with `0x0209 01` in both byte orders and neither call dropped (the link holds while nothing is sent). The discriminator was the handset's release after ciphering: **plain order read `03 4d e2 00` through the cipher**; reversed (A-0028's reading) arrived garbled, and its audio was wind. Plain made the default (`03ae17f`); a ciphered call with the test sound: both tones clear. Service default back to cipher on (`c51a583`) | `vS1a/b/c-20261028-1` |
| 5 internal ring, redial | internal SETUP carried <<SIGNAL>> `e4 40`; whether the C430A's ring pattern differs was not judged. **Redial void:** only one "123" reached the base, and the A540 released its link after every call, so a second call on the same link did not occur (the fix stays covered by its unit test) | `vS45-20261028-1` |
| 6 wrong PIN | **pass.** `05 43 60 01 10` (AUTHENTICATION-REJECT) then `85 47 60 01 10`; the AS690H showed "PIN ungültig!", then registered with the right PIN | `vREGAS690H102454` |
| 7 nlr | **pass.** After each call `0x0202 01`, chip `0x0201 01` 24 ms later; both calls and the list worked. The list-access DISC branch did not run (the A540 ended its list links first). Made the default (`f93e86b`) | `vNLR-20261028-1` |

Also: the test file `wb16k.pcm` has its voice at an amplitude of 3-22 (inaudible);
the voice is in `wb16k-voice.pcm`. The 40 s ring delay of `ringone.sh` is now
10 s after first contact (maintainer).

**Follow-ups:** send the full call information by default to Part 3 handsets
on every external incoming call (test 2); a Gigaset display profile for the
caller name (test 1); internal-call CLIP (the C430A showed no caller on test 5).

## Next round (written 2026-10-28 evening, after more desk work)

Three changes since the bench need a handset. About 15 minutes. Box at `9a13f74`
or later.

### 8. The A540 rings properly by default (5 minutes)

Since `2c76ace` a Part 3 handset gets the full call information without any
option. Ring the **A540** with only a number; **answer** it after a few rings,
then hang up after 10 s.

```
EXTRA=clip=0301234567 HOLD=10 ./ringone.sh a540
```

| you see / hear | meaning |
|---|---|
| it rings aloud, shows "via Line 1 0301234567", and the timer runs after you answer | the default works; the log should show `ring: full call information for a Part 3 handset` and `call: CS call connect sent in CC-INFO (Part 3)` |
| it rings silently | the log says why: look for `terminal capability is not known yet` |
| the call drops when you answer | the CS-call-connect CC-INFO upsets it: the agent reads the log |

Since the ring guard (task list item 16) the ring waits while the A540 has a
list session open: the log may show `ring: the link carries a list session or
an MM procedure -- waiting for it to end`, then `the link is free after N ms`.
A few seconds' delay is expected; `still in use after 30000 ms` is not (the
agent reads the log).

### 9. The A540 dials out: line named, timer unchanged (3 minutes)

Since `8d128d1` our answer names the line the A540 is calling on (it asks for
"None", we answer line 1). On the **A540** dial **123**, keep it 15 s.

```
MAXSECS=90 HOLD=15 ATTACH=600000 EXTRA=playpcm=/tmp/wb16k.pcm ./ringone.sh a540
```

| you see | meaning |
|---|---|
| the timer at once, as before; maybe "Line 1" on the display | fine; the log's CALL-PROC carries `7e 0c 00 00 81 00 05 81 ...` |
| no timer, or the call drops | naming the line upset it: the agent reads the log |

### 10. Two calls at once, now with their own call ids (5 minutes)

Test 18 again, since `9a13f74` gave each call its own id.

```
MAXSECS=200 HOLD=90 CAP=1 EXTRA=playpcm=/tmp/wb16k.pcm,twocalls ./ringone.sh a540
```

Answer the **A540** when it rings; while that call runs, dial **123** on the
**C430A**. Both should hear the test sound as on 2026-10-28.

| you see | meaning |
|---|---|
| both calls work as before | fine; the log should show call id 1 for the A540 and 2 for the C430A's call (no call status goes to the C430A, so only its log line shows it) |
| the second call fails | the agent reads the log |

### 11. Internal call shows the caller (3 minutes)

Since `f69b9cf` an internal call carries the caller's handset number and name.

```
ssh root@192.168.1.1 '/tmp/dect-stubd -g -P 0000 -s /etc/opendectcell-sub.a540 -s /etc/opendectcell-sub.c430a -s /etc/opendectcell-sub.as690h -p -t 150 -a mask=0x0002,codec=3,status=8000,twocalls > /tmp/vINT11-$(date +%H%M%S).log 2>&1'
```

On the **A540**: INT, DECT 2. Look at the **C430A** while it rings: does it show
"DECT 1" (or "1")? Then the other way round: on the **C430A** INT, type 1; does
the **A540** show "DECT 2"?

| you see | meaning |
|---|---|
| the caller's name or number | internal CLIP/CNIP work |
| nothing, as before | the log shows what went out (`intern: caller shown as ...`); the agent reads it |

### 12. "Busy" when the base is full (3 minutes)

Since `9f3e5df` the A540 (Part 3) hears a busy tone when the base cannot take
its call, instead of an immediate refusal. Run **without** two calls, so the
second call cannot get a channel:

```
MAXSECS=150 HOLD=120 ATTACH=600000 EXTRA=playpcm=/tmp/wb16k.pcm ./ringone.sh c430a
```

On the **C430A** dial 123 and stay in the call. While it runs, dial **123** on
the **A540**.

| you hear on the A540 | meaning |
|---|---|
| a busy tone (maybe "busy" on the display), then the call ends by itself after ~5 s | the busy notification works |
| the call ends at once, no tone | the old refusal: the A540's call came on a link that has no call status, the agent reads the log |
| the A540 hangs or shows an error | the agent reads the log |

### 13. A key the base cannot serve during a call (3 minutes)

Since `305b1bd` a call-control key pressed during a call (toggle, consult, hold,
INT) is never taken for dialled digits; a Part 3 handset gets the standard
"not possible" answer (TS 102 527-3 §7.4.3.4). On the **A540** dial **123**;
during the call press **INT** (or the "R"/consult key, whatever the menu
offers) and pick DECT 2. Stay in the call 10 s more, then hang up.

```
MAXSECS=90 HOLD=60 ATTACH=600000 EXTRA=playpcm=/tmp/wb16k.pcm ./ringone.sh a540
```

| you see / hear on the A540 | meaning |
|---|---|
| a short error tone or "not possible", and the call goes on | the negative acknowledgement works; the log shows `nwk out` with `e4 09` |
| nothing, the call goes on | the handset ignores the tone; the log must still show `e4 09` and no `handset dialled` line for the key |
| the call drops | the negative acknowledgement upsets it: the agent reads the log |

The same on the **C430A** (Part 1, `./ringone.sh c430a`): it gets no answer; the
log must show no `handset dialled: \x17` any more.

### 14. The A540 sees the system settings list (3 minutes, any run)

Since `a037fd0` the base names the DECT System Settings List among its lists,
and the A540 reads that list when it syncs. Nothing to do but watch: after
registration and the 10 s wait, leave the A540 idle for a minute (any of the
runs above will do), then open its base settings menu if it has one
(Settings, Base, or "Base info").

| you see | meaning |
|---|---|
| the A540 stays idle and normal; a base menu may show "openDECTcell" as the firmware | the list works |
| the A540 keeps showing "please wait" or resyncs every few seconds | it dislikes the new list: the log shows sessions on list 07; the agent reads it |

### 15. Rename a handset from the handset (3 minutes, any run)

Since `c9c2afd` and the daemon commit after it, the Internal Names List takes
a new name. On the **A540**, open the internal list (INT, or the handset list
in the menu), pick a handset and rename it (e.g. "Kueche"); save.

| you see | meaning |
|---|---|
| the new name stays in the list, and the log shows `names: DECT n is now "Kueche"` | renaming works; `/etc/opendectcell-sub.<model>.name` holds it, and it survives a restart |
| "not possible" or the old name comes back | the log shows the edit and save records (`c3 08`, `c3 0a`, `c3 13/14`) and our answer; the agent reads it |

Other handsets learn the new name the next time they read the list; the
notification that makes them read it at once is not built yet.

### 16. List change notices to the A540 (watch only, any run)

Since the daemon commit after `569b09a`, the A540 (Part 3 only) gets a list
change notice for the Line Settings List after every location registration,
and one for the Internal Names List when a handset is renamed (test 15) or
registers. Look for `reg: list change notice sent for line 1` and
`names: list change notice sent to DECT n` in the log.

| you see | meaning |
|---|---|
| the A540 behaves as before; after a rename, it shows the new name without being switched off and on | the notices work |
| the A540 resyncs its lists right after each notice ("please wait"), once | expected: that is what a notice is for |
| it resyncs again and again, or drops off the base | it dislikes the notice: the agent reads the `nwk out --> 64 62 7d` lines |

### 17. Deregistering a handset (10 minutes; **erases a registration**)

Since the deregistration commit after `a08ea6b`. Uses the **AS690H**, which is
re-registered at the end. Before: `cp /etc/opendectcell-sub.as690h /root/` on the box.
Start any run with all three stores (e.g. the test 13 command with
`./ringone.sh a540`), wait until the AS690H is idle on the base, then:

```
ssh root@192.168.1.1 'echo 3 > /tmp/opendectcell-deregister; kill -USR2 $(pidof dect-stubd)'
```

(3 = the AS690H's slot as the log's `sub:` lines name it; use the slot shown.
Several at once: `echo "1 3" > /tmp/opendectcell-deregister`; they queue.)

| you see | meaning |
|---|---|
| the AS690H says it is no longer registered; the log shows `dereg: ... the handset confirmed` | it works as stock does |
| the log shows `not reachable (paged 10 times)` after ~30 s | the page did not reach it: the handset kept its registration, the base erased it; the agent reads the page lines |
| the A540's handset list loses the AS690H (test 16's notice) | the list change notice works |

Second, the lost handset: re-register the AS690H, **switch it off**, and send
the same deregistration. Expect `not reachable (paged 10 times): deleted
WITHOUT its confirmation` after about 30 s.

Third, the handset's own menu: re-register the AS690H once more and
deregister it from the handset (Settings, Registration, De-register). Since
`b867058` the base first challenges it with its key: the log shows an
{AUTHENTICATION-REQUEST} (`nwk out --> 05 40`), the handset's reply
(`nwk in  <-- 85 41`), then the ACCEPT and `the handset deregistered itself
(accepted)`. A REJECT there with `10` (authentication failed) means the
handset's key and ours differ: the agent reads the log.

Undo at any point: stop the daemon, `mv /etc/opendectcell-sub.as690h.deregistered
/etc/opendectcell-sub.as690h` (or copy back the one in `/root/`), and re-register.

### Watch in every run from now: the handset mask

Since `9504236` the base sends `0x017c 00 <mask>` computed from the slots it
holds ([CR A-0129]) instead of a fixed `09`. With the A540, C430A and AS690H
in slots 1-3 the log shows `sub: locked-in handset mask 0x017c 00 07`.

| you see | meaning |
|---|---|
| all three ring and attach as before | as expected: they rang with `09` too, so the mask does not gate our paging |
| a handset that rang before no longer rings, or no longer attaches | the mask matters after all: re-run with `EXTRA=slotmask=0x09` to confirm, and the agent reads both logs |

### 18. A handset profile takes effect (5 minutes)

Since round 2 (R2) the daemon applies a handset's profile. Copy the example
files from the repo's `profiles/` to the box (the package installs them too)
and pick the C430A's model:

```
ssh root@192.168.1.1 'mkdir -p /etc/opendectcell/profiles'
scp -O profiles/*.profile root@192.168.1.1:/etc/opendectcell/profiles/
ssh root@192.168.1.1 'echo gigaset-c430a > /etc/opendectcell-sub.c430a.profile; /tmp/dect-stubd --check-profile /etc/opendectcell/profiles/gigaset-c430a.profile'
EXTRA=clip=0301234567,cnip=Wednesday HOLD=1 ./ringone.sh c430a
```

| you see | meaning |
|---|---|
| the log shows `profile: DECT 2 = gigaset-c430a (via gigaset): cc.name.method=both ...` and `call: profile display text sent`; the C430A shows what test 1 showed with stock's framing | the profile path works end to end; what the display shows is the framing question of test 1 |
| `profile: ... unusable -- standard defaults` | the file did not parse: `--check-profile` above names the line |

Afterwards `rm /etc/opendectcell-sub.c430a.profile` returns the C430A to the
standard defaults.

**17b. Deleting a handset from another handset's list** (since R3): on the
A540, open the handset list, pick the AS690H and delete it. The A540 should
ask for the base PIN (it then sends it on the ciphered link). Expect, in
order: the PIN check (`c3 0a` on list 07, our `c3 0b`), the delete
(`c3 0c`, our `c3 0d`), then `dereg: DECT 3, asked by DECT 1's handset list
(PIN given)` and the deregistration as in 17. Without ciphering (`-C`) the
PIN is refused ("procedure not allowed"), as the standard wants.

### 19. Authentication on location registration (5 minutes, task 36)

Since R4, `-a locauth` makes the base challenge each subscribed handset with
its key inside its location registration (EN 300 175-5 annex H.2). Off by
default until this test. Switch each handset off and on (that makes it
locate) with:

```
EXTRA=locauth HOLD=1 ./ringone.sh a540
```

| you see, per handset | meaning |
|---|---|
| it comes back to "registered" as usual; the log shows `nwk out --> 05 40` then `nwk in <-- 85 41` then the LOCATE-ACCEPT | it answers the challenge: locauth can become the default |
| it shows "not registered" or searches; the log has a LOCATE-REJECT with `10` | its RES did not match (wrong stored key, or it answers differently): the agent reads the log |
| it takes clearly longer to attach | note how long; a few hundred ms is the expected cost |

## Results, 2026-10-28 evening `[observed on hardware]`

Box at `05b71bb` (API 78), STUB 07.56, the maintainer at the bench, call
ciphering on in every run (`CIPHER=1`, new in `ringone.sh`). Logs in
`work/box-tmp/`.

| test | result | log |
|---|---|---|
| basics | **pass.** All three located (3 LOCATE-ACCEPT, 0 reject); the A540 took a call, A540, AS690H and C430A each called out; every call ciphered (`0x0209`), no <MM_cipher.1> or <MM_auth.1> expiry, no link loss. A second call during a call was refused with 32H (two calls off) | `vRINGA540-20261028-131902-1` |
| ring during a list sync | **defect.** The Line Settings change notice after the A540's location registration made it start a list session; its LiA {CC-SETUP} crossed our ring. We released its call (32H) and re-presented; the A540 released ours with 32H and opened its list call again at once (it does not wait P<CC.06>), and the ring was lost. Re-run with `ATTACH=30000`: fine | `vRINGA540-20261028-132419-1` |
| 8 A540 rings by default | **pass.** Rings aloud, "via Line 1 0301234567", timer at once | `vRINGA540-20261028-132654-1` |
| 9 A540 dials out | **pass.** CALL-PROC names line 1, timer at once | same |
| 13 key during a call | **pass.** A540: INT, DECT 2 arrives as `2c 02 17 32`; our `e4 09` shows "Code unbekannt", the call goes on (twice). C430A: three `17` keypads, no answer, no dialled-digit line | same |
| 14 system settings list | **pass.** "Basisversion" shows "openDECTcell" (field 09). The A540 also reads field 0C, which we do not list, and gets an empty entry; no visible effect | same, `vS11-20261028-133714` |
| 11 internal call shows the caller | **half.** C430A (Part 1) shows "1". A540 (Part 3) shows "via Intern DECT" instead of the caller although the SETUP carries `6c 03 39 81 32`, `6d 07 01 "DECT 2"` and the call information | `vS11-20261028-133714` |
| line settings edit | as built: "Verbind.-Name" and "Zugeordnete Mobilteile" answer "Nicht möglich" (fields not editable); the A540 then saves an empty data packet | same |
| 10 two calls | **pass.** A540 call id 1, C430A call id 2, both heard the voice | `vRINGA540-20261028-134338-1` |
| 15/16 rename | **pass.** INT, DECT 3, Optionen, Name ändern: `names: DECT 3 is now "Kueche"`; "Gespeichert", no resync loop; the name shows in LuCI | same |
| internal call to the AS690H | the AS690H rang 29 s and sent nothing until the A540 hung up: its red key only silences the ringer | same |
| 12 busy when full | **pass.** `e4 04` + CS call disconnecting, then tones off and release; the A540 shows "Nicht möglich" with the busy tone. The C430A's first 123 was refused with 32H while the A540 held a list session (one call without `twocalls`) | `vRINGC430A-20261028-134920-1` |
| 18 profile | **pass.** `profile: DECT 2 = gigaset-c430a (via gigaset)`, display text sent on ALERTING; "Wednesday" full screen, after 10-15 s the ringer screen with the number (as test 1) | `vRINGC430A-20261028-135257-1` |
| 19 locauth | **pass, all three.** {AUTHENTICATION-REQUEST} inside the location registration, the reply, then LOCATE-ACCEPT; about 280 ms on the A540. Visible: a short sync, then the idle screen | `vRINGA540-20261028-135440-1` |
| 17 deregister from the base | **pass, A540 and AS690H** (both switched on): page, TERMINATE-REQUEST, `85 49`, slot cleared, store moved aside, mask `07` -> `06` -> `02`. The A540 shows "MT anmelden" | `vDEREG-20261028-135945`, `vDEREG3-20261028-140255` |
| 17 lost handset | **defect, the timeout path did not run.** Old AS690H store restored (the handset had forgotten the base). After the third page the C430A happened to connect for a location registration; we took that link for the AS690H and sent the TERMINATE-REQUEST, with the AS690H's IPUI, on the C430A's link. The C430A ignored it; "link went down before it answered: deleted WITHOUT its confirmation". We must wait for the target's {LCE-PAGE-RESPONSE} before sending | `vDEREGLOST-20261028-140403` |
| 17b / 17c from a handset | **blocked.** The AS690H and C430A only offer a handset reset. The A540's "Mobilteil abmelden" is a list delete of its own Internal Names entry (`c3 0c 81 81`); we answer PIN required (`c3 12 81 0b`), it edits the PIN field of list 07 (`c3 08 82 81 01`) and we refuse it on the unciphered list call (`c3 12 82 07`): "Nicht möglich". List calls must be ciphered for this | `vS17-20261028-141132` |

Lab-script defect found on the way: `others()` in `ringone.sh`, `regall.sh`
and `ringall.sh` loaded every `/etc/opendectcell-sub.*` file as a store,
`.deregistered` and `.name` files included, so the A540's re-registration
was saved under `/etc/opendectcell-sub.a540.deregistered` (renamed by hand).
Fixed in the scripts: only `/etc/opendectcell-sub.<name>` is loaded.

Box afterwards: all three handsets registered again (A540 slot 1 with a new
key, C430A slot 2, AS690H slot 3 with a new key, its name back to "DECT 3").

## Next round (written 2026-10-28 late, after round 3 at the desk)

Box at `37d9b22` or later. No park between runs (maintainer, 2026-10-28):
the lab scripts only stop the daemon, which parks itself; `./park.sh` at the
end of the session parks and checks. `CIPHER=1` keeps call ciphering on in
`ringone.sh`, as the service does.

### 20. A ciphered list session and the PIN (5 minutes; **deregisters the A540**)

Since `521682e` a list-access call is ciphered like a speech call
(TS 102 527-3 C.7.2), so the PIN can travel. Start any run with all three
stores (e.g. `CIPHER=1 ATTACH=600000 MAXSECS=300 HOLD=200 ./ringone.sh c430a`).

1. On the A540 browse the handset list and the base menu as usual.
2. Then Einstellungen, Anmeldung, **Mobilteil abmelden**, PIN 0000.

| you see | meaning |
|---|---|
| browsing works as before; the log shows `call: list-access call -- ciphering it` and `the chip reports link 1 ciphered (0x0209)` for each list session | ciphered list calls are fine |
| the A540 says it is no longer registered; the log shows `c3 0c 81 81`, our PIN-required NACK, the PIN save on list 07 accepted, then `dereg: DECT 1, asked by DECT 1's handset list (PIN given)` and the deregistration | the PIN path works end to end; re-register the A540 (`./regall.sh a540`) |
| "Nicht möglich" again | the agent reads the log: whether the cipher came up before the PIN edit |
| list sessions now fail or take long | ciphering list calls upsets it: the agent reads the log (`-a` has no switch for it yet) |

### 21. A ring during the A540's list sync (5 minutes, repeat 2-3 times)

Since `d8ead39` a ring lost to the handset's own call is rung again. Recreate
the evening's loss: the ring 10 s after first contact, which is when the A540
starts its list sync after the list change notice.

```
CIPHER=1 ATTACH=10000 HOLD=5 ./ringone.sh a540
```

Press a key on the A540 (or switch it off and on), let it ring, answer.

| you see | meaning |
|---|---|
| it rings every time, perhaps a few seconds late; the log may show `ring: our call was lost to the handset's own call (collision, 9.5.2.3) -- ringing again when the link is free (1 of 2)` | the retry works (when that line is absent, the collision did not happen that time: repeat) |
| no ring, and `giving up after 2 new attempts` | the A540 keeps crossing us: the agent reads the log |

### 22. Deregistration while another handset connects (5 minutes)

Since `cde5cdb` a deregistration waits for the target's own page response.
The agent installs an invented handset in slot 4 that nobody answers (as at
the desk, `vDEREGQ-20261028-142353`) and asks for its deregistration; while
the base pages it (30 s), **press a key on the C430A** so it connects.

| you see | meaning |
|---|---|
| the C430A behaves normally; the log shows no `05 48` on its link, 10 pages, then `not reachable (paged 10 times): deleted WITHOUT its confirmation` | fixed |
| `05 48` goes out on the C430A's link | not fixed: the agent reads the log |

### 23. The second call ciphered (5 minutes)

Card 10's second call (the C430A on a side link) ran in the clear: side
links never started ciphering. Since the commit after `a776cf7` they do, for
a speech call once connected and a list-access call at once. Card 10 again:

```
CIPHER=1 MAXSECS=300 HOLD=120 EXTRA=playpcm=/tmp/wb16k-voice.pcm,twocalls ./ringone.sh a540
```

Answer the A540; during its call dial 123 on the C430A, listen, hang up both.

| you hear / the log shows | meaning |
|---|---|
| both calls clear; `side: cipher: authenticating the handset on handle 2`, `call key to the chip (0x0208, link 2 ...)`, `side: cipher: the chip reports link 2 ciphered (0x0209)` | the side link ciphers |
| the C430A's sound turns to noise ("wind") after a second | the chip's key for link 2 is wrong, or 0x0208 is per main link only: the agent reads the log; `-C` is the way back |
| `side: cipher: authentication FAILED` | the side link used the wrong UAK: the agent reads the log |

### 24. The internal caller on the A540 (3 minutes)

Card 11 showed "via Intern DECT" on the A540. Stock sends the internal
caller's number in the SETUP and the name afterwards in a {CC-INFO}
(`6c 03 39 81 3x`, `6d LL 05 <name>`, `7e 03 01 00 81`) [CR A-0130 §1]; since
the commit after `57aaf33` we do the same for a Part 3 target. Any run with
all three stores; on the **C430A** INT, 1.

| the A540 shows | meaning |
|---|---|
| "DECT 2" (or the C430A's name) while it rings; the log shows `intern: caller's name "DECT 2" sent in CC-INFO` | fixed |
| "via Intern DECT" still | the agent compares our CC-INFO with A-0130's; stock sends it about 520 ms after the SETUP, after authentication, we send it on ALERTING |

### 25. The bridge without SIP: a call through the control and audio sockets (5 minutes)

The MVP's DECT half (bridge-interface.md §5 steps 2-3), with the test bridge
standing in for SIP. One run, all three stores, then in a second shell:

```
ssh root@192.168.1.1 '/tmp/dect-stubd -g -P 0000 -s /etc/opendectcell-sub.a540 -s /etc/opendectcell-sub.c430a -s /etc/opendectcell-sub.as690h -p -t 300 -a mask=0x0002,codec=3,status=8000 > /tmp/vECHO-$(date +%H%M%S).log 2>&1'
ssh root@192.168.1.1 '/tmp/dect-stubd --bridge-echo'
```

1. On the A540 dial any number (e.g. 123) and speak.
2. Then stop the echo bridge, start it as `--bridge-echo ring`: the A540
   rings as "Echo test 0301234567"; answer and speak.

| you hear | meaning |
|---|---|
| ring-back, then after 1.5 s the call connects and your voice comes back half a second late, clear | signalling and both audio directions through the sockets work: the SIP side only has to fill them |
| it connects but stays silent | the log's `audio:` lines and the echo's `audio connected` say which half is missing; the agent reads them |
| your voice comes back garbled or choppy | the G.722 coding or the frame timing: the `audio: socket closed` line counts underruns and drops |

### 26. A handset's own line names (2 minutes, any run)

Since round 11 a handset's lines are VoIP profiles (Network > DECT, VoIP
Profiles), each with a "Name on the handset"; assign them under Handsets, Edit,
General, Line 1 / Line 2. Set Line 1 of DECT 1 to
"Festnetz" and switch on Line 2 named "Büro", Save & Apply, start the base,
then on the A540 open the base menu, Verbindungen.

| you see | meaning |
|---|---|
| "Festnetz" and "Büro" | the per-handset Line Settings List works |
| only "Line 1" | the agent reads the log's `sub: DECT 1's lines:` and the list session |

### 27. The first SIP call (15 minutes; needs a SIP account or a SIP phone/app)

The MVP: baresip on the box with the openDECTcell module (task list M3). Needs a
SIP peer: a softphone on the PC or a phone app calling the box directly
(Line 1 with "Register" off, registrar = the box, 192.168.1.1), or an IP
phone account on the a FRITZ!Box (Telefonie > Telefoniegeräte >
IP-Telefon; registrar 192.168.178.1).

1. Network > DECT, Handsets, Edit DECT 1, Line 1: on, the account; Save & Apply.
2. Call the PC/app from the A540, talk, hang up. Then call the A540 from it.

| you hear / see | meaning |
|---|---|
| both calls ring through and you hear each other | the MVP works |
| no ring on the far end | the agent reads `logread -e baresip` (registration, the account line) |
| it rings but no sound | the audio socket: the log's `audio:` lines and baresip's |

During the call, Status > DECT, tab VoIP shows the line's registration and
the call's codec, endpoints, data rates and encryption (round 9); a
screenshot of it is worth keeping. "Try to autodetect" is on for Media and
Encryption by default: if the far end refuses the call (488), untick
Encryption's and try again.

### 28. Registering with a chosen model and an 8-digit PIN (5 minutes; **deregisters the A540**)

Round 9: the PIN is random, 8 digits by default, and the model picked before
registering becomes the new handset's profile.

1. Network > DECT, Handsets: Deregister DECT 1 (the A540), or on the A540
   "Mobilteil abmelden".
2. Below the list: PIN length 8 digits, *New PIN*, Save & Apply. Then
   Manufacturer *Gigaset*, Model *Gigaset A540*, *Register a handset*.
3. On the A540 register to the base and enter the 8 digits.

| you see | meaning |
|---|---|
| the A540 registers, and Status > DECT lists it with the profile gigaset-a540 | 8 digits and the model choice both work |
| it registers but the profile reads "standard" | the PIN works; the agent reads the log's `profile: DECT n registered as` |
| the A540 takes only 4 digits, or refuses the PIN | set PIN length 4 digits, New PIN, and register again; the agent notes which handsets need fewer digits |

### 29. The IPEI against the handset's own (2 minutes, any run)

Status > DECT now lists each handset's IPEI (EN 300 175-6 annex C:
"EEEEE PPPPPPP C"), computed from the identity the handset gave at
registration. Compare it with the label under the battery or the handset's
service display.

| you see | meaning |
|---|---|
| the same digits | the IPEI is right |
| other digits | the agent checks the identity bytes against the standard |

## Results, 2026-10-28 evening, at home `[observed on hardware]`

The box on the the local network (192.168.1.1), Line 1 of DECT 1 as an IP phone of
the a FRITZ!Box.

| card | result |
|---|---|
| 27 | **passed**: A540 -> mobile and mobile -> A540, G.722 both ways, ciphered, hang-up from either side. On the way: the A540's stored key had been wrong since the lab repair of 2026-10-28 14:05 (fixed by re-registering), LuCI's commands were refused while baresip held the control socket (`0bce429`), a deregistered handset stayed listed (`0706e9b`), a new registration was hidden (`ecaaebc`), and an incoming call carried no number and line after a restart (capability now remembered) |
| 28 | **passed**: the A540 took the 8-digit PIN, and the model picked in LuCI became its profile at enrolment |
