# Bench card: the next session (written 2026-10-28)

For the maintainer, with the A540, C430A and AS690H at hand. About 45 minutes.
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
