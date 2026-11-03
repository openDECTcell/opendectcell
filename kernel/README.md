# kernel/: the openDECTcell kernel module

One small Linux module, `opendectcell.ko`, gives userspace the character device
`/dev/dect0`. It owns what only the kernel can do for the DECT chip: pulling
its reset line, holding its boot strap, and (on the 7520/7530 only) the PCM/TDM
audio bus. Everything above that (firmware upload, the DECT stack, calls) is
userspace; see [../docs/hardware-abstraction.md](../docs/hardware-abstraction.md)
for how the models differ and why the split is drawn here.

The source is built in one of two ways, selected by `OPENDECTCELL_LANTIQ` in
[Makefile](Makefile). Both builds produce a module called `opendectcell.ko`; a
target gets one or the other, never both.

## Files

| File | What it is | Box |
|---|---|---|
| [opendectcell_ipq40xx.c](opendectcell_ipq40xx.c) | Module entry, board table, `/dev/dect0` (`file_operations`, ioctls), reset and boot-strap GPIOs, restoring the UART pin after a strap, read-only GPIO capture for diagnostics | 7520 / 7530 (IPQ4019) |
| [opendectcell_pcm.c](opendectcell_pcm.c) | The PCM/TDM path through the IPQ4019 audio block (ADSS): clocks, pins, TDM slave setup, MBOX DMA rings, receive and transmit, the stepwise bring-up | 7520 / 7530 |
| [opendectcell_pcm.h](opendectcell_pcm.h) | Interface between the two files above: `struct dect_pcm_board` (physical addresses, IRQ, pins) and the `dect_pcm_*` functions | 7520 / 7530 |
| [opendectcell_lantiq.c](opendectcell_lantiq.c) | Module entry, board table, `/dev/dect0` for reset and boot strap only; no audio | 7412, 7362 SL (Lantiq VR9) |
| [include/uapi/opendectcell.h](include/uapi/opendectcell.h) | The userspace interface of `/dev/dect0`: ioctl numbers, structures, block format | all |
| [include/uapi/ssc_pcm.h](include/uapi/ssc_pcm.h) | A copy of the uapi of `kmod-lantiq-ssc-pcm` (`/dev/ssc-pcm0`, version 3), which `dect-stubd` uses on the 7412. Not ours to change here | 7412 |

Which files go into the module is decided in `Makefile`: the default is
`opendectcell_ipq40xx.o opendectcell_pcm.o`; with `OPENDECTCELL_LANTIQ` it is
`opendectcell_lantiq.o` alone. `opendectcell.mod.c` and the `*.o`, `*.ko`, `*.mod*` and
`Module.symvers` files that appear after a build are git-ignored debris.

## Board table

Each module matches the running board by device-tree `compatible`
(`of_machine_is_compatible`) against a small table, and takes its pins and
transport from it. On an unknown board the module refuses to load and logs the
model and compatible string it saw; it touches no GPIO it does not know.
Userspace never sees GPIO numbers or register addresses.

| `compatible` | Module build | `transport` in `DECT_IOC_INFO` | Reset / strap / other | PCM |
|---|---|---|---|---|
| `avm,fritzbox-7530` (also the 7520) | `opendectcell_ipq40xx.c` | `ipq40xx-tdm` | reset GPIO 40, strap GPIO 16 (UART RX), PCM pins GPIO 20-27 | this module (`opendectcell_pcm.c`) |
| `avm,fritz7412` | `opendectcell_lantiq.c` | `none` | reset GPIO 33; GPIO 7 (PCM clock enable) held high from load to unload; a strap line (GPIO 8) is in the table but the board profile sets `strap=0`, so it is not used | `kmod-lantiq-ssc-pcm`, `/dev/ssc-pcm0` |
| `avm,fritz7362sl` | `opendectcell_lantiq.c` | `none` | reset GPIO 30, strap on GPIO 11 (the USIF RX pin, handed back afterwards) | `kmod-lantiq-tdm-pcm`, `/dev/tdm-pcm0` |

The GPIO numbers come from the tables in the two `.c` files. The device node
is created with mode 0600 (root only).

## Userspace interface

Header: [include/uapi/opendectcell.h](include/uapi/opendectcell.h), magic `'D'`,
`DECT_IOC_VERSION` 2. `INFO` returns the board, transport and a feature
bitmask (`DECT_FEATURE_RESET`, `_STRAP`, `_GPIO_CAPTURE`, `_PCM_RX`).

| Ioctl | Does | 7520/7530 | 7412, 7362 SL | Used by |
|---|---|---|---|---|
| `DECT_IOC_INFO` | Read `struct dect_ioc_info` (version, stage, features, board, transport) | yes | yes | `dect-stubd`, `dect-pcmcap` |
| `DECT_IOC_RESET` | Write `__u32`: 1 asserts reset (line low), 0 releases; other values `-EINVAL` | yes | yes | `dect-fwload` |
| `DECT_IOC_STRAP` | Write `__u32`: 1 drives the boot strap low, 0 restores the UART pin | yes | yes (`-EOPNOTSUPP` if the strap line cannot be claimed); the 7412 profile never calls it | `dect-fwload` |
| `DECT_IOC_CAPTURE` | Read-only polling of the PCM pins, for diagnosis (`struct dect_ioc_capture`) | yes | `-EOPNOTSUPP` | `dect-sla` |
| `DECT_IOC_PCM_STEP` | Bring the PCM path up one numbered step at a time (1 clock ... 6 DMA); 0 undoes all | yes | `-EOPNOTSUPP` | `dect-stubd`, `dect-pcmcap` |
| `DECT_IOC_PCM_CONFIG` | Slots per frame, delays, block size, descriptors, TX flag; only at step 0 | yes | `-EOPNOTSUPP` | `dect-stubd`, `dect-pcmcap` |
| `DECT_IOC_PCM_REGS`, `DECT_IOC_PCM_STATS` | Register dump and counters, for bring-up and the monitor | yes | `-EOPNOTSUPP` | `dect-pcmcap`, `dect-stubd` (stats) |
| `DECT_IOC_PCM_TX_FRAME` | Set the frame sent when the write queue is empty | yes | `-EOPNOTSUPP` | `dect-pcmcap` |

Besides the ioctls, the 7520/7530 device supports `read()` (whole
`struct dect_pcm_block` records: header plus raw DMA payload), `write()`
(whole transmit frames, only with `DECT_PCM_CF_TX` at step 6) and `poll()`.
The Lantiq device has none of these: its PCM bus is the other modules'
device (`ssc_pcm.h` for `/dev/ssc-pcm0`; the 7362 SL's `/dev/tdm-pcm0` has the
same interface, per its package description). Closing the device or unloading
the module undoes the PCM bring-up.

## How the packages build it

The OpenWrt feed lives in [../openwrt/](../openwrt/) (see its
[README](../openwrt/README.md)). Both kernel packages are defined in
[../openwrt/net/opendectcell/Makefile](../openwrt/net/opendectcell/Makefile), next to
the userspace package, and build from the same source archive:

| Package | Target | Built how |
|---|---|---|
| `kmod-opendectcell-ipq40xx` | `ipq40xx` | `$(KERNEL_MAKE) M=.../kernel modules`; installs `kernel/opendectcell.ko` |
| `kmod-opendectcell-lantiq` | `lantiq/xrx200` | the same with `OPENDECTCELL_LANTIQ=1` (set when `CONFIG_TARGET_lantiq_xrx200`) |

`opendectcell` depends on its target's module. Neither is loaded at boot: the
`opendectcell` init script loads it when the service is enabled. The module only
fits the kernel it was built against.

The Lantiq PCM bus and the 7362 SL's firmware-load UART are not in this
directory. They are separate repositories, each a package in
[../openwrt/kernel/](../openwrt/kernel/):

| Package | Repository (`PKG_SOURCE_URL`) | For |
|---|---|---|
| `kmod-lantiq-ssc-pcm` ([Makefile](../openwrt/kernel/lantiq-ssc-pcm/Makefile)) | `openDECTcell/kmod-lantiq-ssc-pcm` | 7412 PCM, pulled in by `opendectcell` |
| `kmod-lantiq-tdm-pcm` ([Makefile](../openwrt/kernel/lantiq-tdm-pcm/Makefile)) | `openDECTcell/kmod-lantiq-tdm-pcm` | 7362 SL PCM, via the `opendectcell-fritz7362sl` package |
| `kmod-lantiq-usif-uart` ([Makefile](../openwrt/kernel/lantiq-usif-uart/Makefile)) | `openDECTcell/kmod-lantiq-usif-uart` | 7362 SL firmware UART, via `opendectcell-fritz7362sl` |

[../tools/release/build.sh](../tools/release/build.sh) builds the packages
with the OpenWrt SDK. For a `lantiq/*` target it checks (`git ls-remote`)
whether each of these three repositories is reachable. A reachable one is
built and packaged; an unreachable one (a private repository in CI) is dropped
from the feed and from `opendectcell`'s dependencies, with a warning.

## Building out of tree

You need a configured kernel tree (or an OpenWrt SDK's kernel build directory)
for the target. [Makefile](Makefile) defaults `KDIR` to
`/lib/modules/$(uname -r)/build` and `ARCH` to `arm`, and passes
`CROSS_COMPILE` through:

```
# 7520 / 7530 (IPQ4019)
make -C kernel KDIR=/path/to/linux ARCH=arm CROSS_COMPILE=arm-openwrt-linux-muslgnueabi-

# 7412 / 7362 SL: the Lantiq variant (set ARCH and CROSS_COMPILE for MIPS)
make -C kernel KDIR=/path/to/linux ARCH=mips CROSS_COMPILE=<mips toolchain>- OPENDECTCELL_LANTIQ=1
```

`make -C kernel clean` removes the build output. The top-level `make` and
`make check` do not build the kernel module; `make cross` builds only the
ARM userspace programs. The kernel sources include the uapi header from
`include/uapi` (`ccflags-y`), and `dect-stubd` and the tools compile against
the same header (`-Ikernel/include/uapi`).

## Where the hardware is documented

| Topic | Document |
|---|---|
| How the models differ, the board table idea | [../docs/hardware-abstraction.md](../docs/hardware-abstraction.md) |
| IPQ4019 PCM/TDM registers, clocks, DMA (7520/7530) | [../docs/pcm-tdm-ipq4019.md](../docs/pcm-tdm-ipq4019.md) |
| The module's original task brief (Phases A and B, history) | [../docs/kmod-spec.md](../docs/kmod-spec.md) |
| 7412: SSC as PCM, loader, GPIOs | [../docs/7412-spi-tdm-and-loader.md](../docs/7412-spi-tdm-and-loader.md), [../docs/models/7412-DECT-analysis.md](../docs/models/7412-DECT-analysis.md) |
| 7362 SL: TDM block, USIF UART | [../docs/models/7362SL-DECT-analysis.md](../docs/models/7362SL-DECT-analysis.md) |

## Rules

- Kernel code is `GPL-2.0-only` (SPDX header); everything else in the project
  is `GPL-2.0-or-later` ([../AGENTS.md](../AGENTS.md) section 5).
- Per-model differences go into the board table here, never into userspace.
  A change for one model must not alter the others
  ([../AGENTS.md](../AGENTS.md) section 1b).
- A new box means a new board-table entry; a new PCM block means a new
  transport driver first ([../docs/hardware-abstraction.md](../docs/hardware-abstraction.md)
  section 5, and the checklist in [../boards/README.md](../boards/README.md)).
