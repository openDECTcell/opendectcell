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

(3 = the AS690H's slot as the log's `sub:` lines name it; use the slot shown.)

| you see | meaning |
|---|---|
| the AS690H says it is no longer registered; the log shows `dereg: ... the handset confirmed` | it works as stock does |
| the log shows `not reachable (paged 10 times)` after ~30 s | the page did not reach it: the handset kept its registration, the base erased it; the agent reads the page lines |
| the A540's handset list loses the AS690H (test 16's notice) | the list change notice works |

Second, the lost handset: re-register the AS690H, **switch it off**, and send
the same deregistration. Expect `not reachable (paged 10 times): deleted
WITHOUT its confirmation` after about 30 s.

Third, the handset's own menu: re-register the AS690H once more and
deregister it from the handset (Settings, Registration, De-register). Expect
`the handset deregistered itself (accepted)`.

Undo at any point: stop the daemon, `mv /etc/opendectcell-sub.as690h.deregistered
/etc/opendectcell-sub.as690h` (or copy back the one in `/root/`), and re-register.
