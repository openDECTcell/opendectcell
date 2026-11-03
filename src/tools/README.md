# src/tools: small C programs for bring-up and analysis

Three stand-alone C programs from the chip bring-up. None of them is needed
to run a base station; they are for looking at the PCM link and at captures.
(Python and shell tools are in [../../tools/](../../tools/README.md).)

## Files

| Tool | Runs on | What it is for | Built by |
|---|---|---|---|
| `dect-pcmcap.c` | the box (live), or a PC with `-S FILE` | Brings up the PCM receive (and with `-T` transmit) path of `/dev/dect0` step by step (`-s STEP`), captures blocks, searches the data for 0xAA-framed STUB mails and checks their checksums, can send mails in the control slot (`-m SRC:DST:PRIM[:HEX]`), analyse tones per slot (`-A`), transmit a test tone (`-G SLOT:FREQ`), and run a command with the open device (`-e CMD`, e.g. `dect-fwload --dect-fd $DECT_FD`). This is what the control-slot search in `dect-stubd` grew out of | `make` (PC), `make cross` (`build/dect-pcmcap.arm`) |
| `dect-maildump.c` | PC | Offline: recovers the complete mail stream from a raw TDM capture written by `dect-stubd -w FILE`, by re-framing the control slot with the library's own decoder (`dect_rx_push()` / `dect_rx_pull()`). Options: `-c` slots per frame (default 8), `-w` control word (default 1), `-p HEX` only one primitive (e.g. `f625`), `-q` one line per mail. Timestamps are relative to the capture start | `make` (`build/dect-maildump`) |
| `dect-sla.c` | the box | One-shot read-only capture of the PCM pins through the kernel module (`DECT_IOC_CAPTURE`): `dect-sla OUT.bin [duration_us 1..8000]`. A bring-up tool, not a PCM receiver | Not built by the `Makefile`; build it with the SDK's compiler if needed |

`dect-pcmcap` uses `../libopendectcell/dect_audio.c` and the kernel interface
header `../../kernel/include/uapi/opendectcell.h`; `dect-maildump` uses
`../libopendectcell/dect_mail.c`. `dect-pcmcap` and `dect-sla` speak the 7520/7530
module's interface (`/dev/dect0`); the 7412 and 7362 SL transports have a
different interface (`ssc_pcm.h`) that these tools do not use. Use
`dect-stubd -v` there.

## Tests

None of the three has a unit test of its own. `dect-maildump` shares its
decoder with `tests/test_mail.c`; `dect-pcmcap` shares the audio helpers with
`tests/test_audio.c`.

## Where to look for...

| You want | Look at |
|---|---|
| The control-channel framing the tools decode | [../../docs/stub-pcm-control-channel.md](../../docs/stub-pcm-control-channel.md), `../libopendectcell/dect_mail.c` |
| The kernel interface they call | `../../kernel/include/uapi/opendectcell.h`, [../../docs/kmod-spec.md](../../docs/kmod-spec.md) |
| Examples of `dect-pcmcap` runs and what they showed | [../../docs/pcm-tdm-ipq4019.md](../../docs/pcm-tdm-ipq4019.md), [../../docs/stub-pcm-control-channel.md](../../docs/stub-pcm-control-channel.md) |
| Reading a capture from a call | `dect-stubd -w`/`-V` in [../stubd/README.md](../stubd/README.md); [../../docs/diagnostics.md](../../docs/diagnostics.md) for what each mail means |
| Other tools (firmware extraction, release, bench, cell test peer) | [../../tools/README.md](../../tools/README.md) |
