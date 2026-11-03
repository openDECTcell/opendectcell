# 7412 stop-hang campaigns

Scripts for the experiments in [docs/7412-stop-hang.md](../../docs/7412-stop-hang.md). `run.sh` runs on the
PC and drives one of the on-box scripts, power-cycling the box (a Tasmota socket) when it goes silent and
continuing with the next cycle; the cycle numbers come from the box's syslog, which must go to
`tools/syslog-sink.py` first.

| Script (on the box) | What one cycle does | Tests |
|---|---|---|
| `module-only.sh` | the chip held in reset, `ssc-pcm-test slots=36 hold=2` starts and stops the SSC engine | H2: the module alone |
| `chip-reset.sh` | start the service, wait for the radio, hold the chip in reset (GPIO 545) while the engine runs, stop | H1: the chip vanishes under an armed DMA |
| `real-stop.sh` | start the service, wait for the radio, stop it 1 to 6 s later (the park mails are sent) | the real stop, as a cell or as a base |

Run `real-stop.sh` once as a cell and once as a base (`uci set opendectcell.main.role=base`, then back), and
once with traffic to and from the box. Switch the service's autostart off for `module-only.sh`
(`/etc/init.d/opendectcell disable`) and on again afterwards. The box needs `/root/netguard.sh` if the state of
a network stall is to be kept (it copies the kernel log and the NIC counters to `/root/hangevid`).
