# What data openDECTcell stores, where, and how it is removed

This page lists the personal and secret data that openDECTcell keeps on the box: handset keys, phone numbers, SIP and
CardDAV credentials, call lists, logs. For each kind it says where it lives, whether it survives a reboot and a
sysupgrade, who can read it and how to delete it. Everything below was read from the repository's code and
configuration at the version in `openwrt/net/opendectcell/Makefile`; where something cannot be established from the
repository, the page says "not established here".

openDECTcell runs as root. Every protection below is a file mode or a root-only directory; none of it is encryption at rest
(see [Backups](#3-backups)). The review of 2026-11-02 lists the open security findings:
[security-review-2026-11-02.md](security-review-2026-11-02.md).

## 1. Data inventory

### 1.1 On flash (survives a reboot)

"Sysupgrade" says whether the data is kept across a firmware upgrade because of the packages' keep lists
(`openwrt/net/opendectcell/files/opendectcell.keep`, `opendectcell-cell.keep`) or OpenWrt's own rule for `/etc/config`.

| Data | Why it is needed | Where | Sysupgrade | Mode and readers | Deleted by |
|---|---|---|---|---|---|
| **Handset subscription record**: the handset's authentication key (UAK), its identity (IPUI/IPEI) and the identities a call to it needs | The base must prove itself to the handset and cipher calls; without it the handset must be registered again | `/etc/opendectcell-sub.N`, N = 1..6 (option `stores`) | kept (`/etc/opendectcell-sub.*`) | 0600, written atomically by `dect-stubd` (`fchmod(fd, 0600)`). Daemon only; the LuCI ACL does not grant read of it | Deregistration moves it aside, it does not delete it, see section 2 |
| **Handset side files**: display name (`.name`), model profile id (`.profile`), capability flags (`.cap`) | Names on the handsets' internal list and the web page; per-model behaviour | `/etc/opendectcell-sub.N.name`, `.profile`, `.cap` | kept (same pattern) | The code sets no mode (default, so what the root umask gives). The LuCI ACL may read and write `.profile` files | Deregistration moves them aside, see section 2 |
| **Leftovers of a deregistered handset** | A mistake can be undone by moving them back | `/etc/opendectcell-sub.N.deregistered`, `.name.deregistered`, `.profile.deregistered`, `.cap.deregistered` | kept (same pattern) | The record keeps its 0600 (a rename keeps the mode) | Only by hand, section 2.2 |
| **Registration PIN** | The access code a handset must enter to register | UCI `opendectcell.main.pin` in `/etc/config/opendectcell`; a random one is made at install (`opendectcell.uci-defaults`) | kept | Default (see UCI below). Shown on Network > DECT, Handsets | `uci delete opendectcell.main.pin`; the init script then makes a new random one at the next start |
| **SIP account credentials** (`user`, `authuser`, `password`, `domain`, STUN/TURN user and password) | To register the lines with the provider | UCI `config voip` sections in `/etc/config/opendectcell` | kept | Default (see UCI below) | `uci delete opendectcell.voipN` (section names are in `uci show opendectcell`), then `uci commit opendectcell` |
| **CardDAV credentials** (`url`, `username`, `password`) and the books chosen | To read and write the address book | UCI `config carddav` in `/etc/config/opendectcell` | kept | Default (see UCI below) | `uci delete opendectcell.carddav` |
| **Handset settings and lines** (slot, `line1`, `line2`, `override` list) | Which VoIP profile rings which handset; per-handset options | UCI `config handset` sections | kept | Default (see UCI below) | `/etc/init.d/opendectcell forget N` (removes the section) or the Remove button on a row the base no longer holds |
| **Generated per-handset files**: overrides, line names | The daemon reads them; written afresh at every start from UCI | `/etc/opendectcell/handsetN.override`, `handsetN.lines` | not in the keep lists; rebuilt at the next start | The init script sets no mode (default) | `forget N`, or the next start (`rm -f handset*.override handset*.lines`, then rewritten from UCI) |
| **Cell credentials** (cell mode): the cell's private key `key.pem`, `cert.der`, `ca.der`, the enrolment string `enrollment`, and the controller-set identity `rfpi`, `rfpi.tried` | The TLS link to the controller | `/etc/opendectcell/cell/` | kept (`/etc/opendectcell/cell/`) | Directory 0700 (`mkdir(dir, 0700)`), each file 0600 (`file_put(..., 0600)`) in `src/cell/dect-cell.c` | `/etc/init.d/opendectcell cell_leave`, section 2.3 |
| **Chip firmware** (not personal data; the user's own, never shipped) | The DECT chip boots from it | `/etc/opendectcell/firmware/` (option `firmware_dir`) | kept | 0600 (`firmware.sh` writes `.name.new` with 0600 and moves it) | the Firmware tab's Delete, `firmware.sh delete ROLE` |
| **Handset profiles** (model data, no personal data) | Per-model settings | `/etc/opendectcell/profiles/` | kept (conffiles and keep list) | package defaults | not personal data |

**UCI file permissions.** `/etc/config/opendectcell` is installed with OpenWrt's `INSTALL_CONF` macro
(`openwrt/net/opendectcell/Makefile`); openDECTcell's own code sets no mode on it, and the mode after a `uci commit` is OpenWrt's
business: not established here. Check with `ls -l /etc/config/opendectcell`. The file holds the PIN and all passwords in
clear text. The LuCI ACL (`openwrt/applications/luci-app-opendectcell/root/usr/share/rpcd/acl.d/luci-app-opendectcell.json`)
grants read and write of the whole `opendectcell` UCI config to any rpcd login that has that ACL. Which logins have it is
set by `/etc/config/rpcd`: not established here.

### 1.2 In RAM only (gone at a reboot)

All of these are in `/var/run/opendectcell/`, a directory the init script creates with `chmod 700`
(`opendectcell.init`). A service restart keeps them unless noted. None is in a keep list.

| Data | Why | File | Mode and readers | Deleted by |
|---|---|---|---|---|
| **The PIN, as the daemon reads it** | So the PIN is not on a command line where every local user could read it | `pin` | 0600 (`umask 077`) | reboot; rewritten at each start |
| **Baresip accounts**, with the SIP password in clear | baresip needs them | `baresip/accounts`, `baresip/config` | `accounts` 0600 in a 0700 directory | reboot; rewritten at each start |
| **Line map** (handset, line, SIP address) | The baresip module routes calls with it | `lines` | default, inside the 0700 directory | reboot; rewritten at each start |
| **Call lists**: missed, outgoing, accepted calls, 40 per list, each with number, name, time and the handset lines | The handsets' call lists | `calls` | 0600 (`fopen_private`, `O_NOFOLLOW`) | reboot; or stop the service, `rm /var/run/opendectcell/calls`, start; a handset's "delete list"; older entries drop out at 40 |
| **Contact cache** from CardDAV: name, first name, numbers of up to 500 contacts | The handsets' phonebook | `contacts` (written by `carddav.uc`) | default, inside the 0700 directory | reboot; the next sync rebuilds it |
| **Handset changes not yet sent to the server** | Written back to the CardDAV server | `contacts.changes`, `contacts.changes.work` | `.changes` is opened with 0600 | reboot (the changes are then lost); sent at the next successful sync |
| **Base state** for the web page: uptime, radio state, per handset slot, name, store path, **IPEI**, link state, model | Status > DECT | `state.json` | default, inside the 0700 directory; the LuCI ACL may read it | reboot; rewritten about once a second |
| **VoIP state**: SIP address per line, registration result, per call the codec and **both media endpoints** (IP addresses) | Status > DECT, VoIP tab | `voip.json` | as `state.json` | reboot; removed when baresip stops |
| **CardDAV status** and the last discovery (book URLs) | The Phonebook tab | `carddav.json`, `carddav-books.json` | as `state.json` | reboot |
| **Cell state** | Status > DECT in cell mode | `cell.json`, `cell-link.json` | as `state.json` | reboot |
| **Run markers and logs of the start**: which firmware build is loaded, whether the radio was parked, the firmware loader's output | Start and stop logic | `stub.second`, `parked`, `time-valid`, `dect-fwload.log`, `dect-warm.log` | default, inside the 0700 directory | reboot |
| **Control socket**: events with handset names, dialled and caller numbers; commands | The bridge to baresip and the web page | `/var/run/opendectcell.sock` | 0600 root (`chmod(path, 0600)`); [bridge-interface.md](bridge-interface.md) | removed at daemon exit |
| **Audio socket**: the live speech of calls, not stored | The bridge to baresip | `/var/run/opendectcell-audio.sock` | 0600 (`chmod(AB_PATH, 0600)`) | removed at daemon exit |
| **Cell socket**: the chip's mails, which can carry keys | `dect-stubd -K` to `dect-cell` | `/var/run/opendectcell-cell.sock` | created under `umask 077` | removed when the cell stops |

"Default, inside the 0700 directory" means the code sets no mode on the file itself; other local users are kept out by
the directory. If `carddav.uc` ran before the init script made the directory, it would create it with the default mode
(`fs.mkdir(DIR)`); the init script creates it first.

### 1.3 Temporary files of the web interface (in `/tmp`)

| File | Holds | Mode | Removed |
|---|---|---|---|
| `/tmp/opendectcell-carddav-discover.json` | The server, user name and **password** typed into "Fetch address books" | 0600 (LuCI writes it with mode 0600) | `carddav.uc discover` reads and deletes it. If that command never runs, it stays until a reboot. The review lists this file as still open |
| `/tmp/opendectcell-enroll.<random>` | The cell enrolment string with its one-time token | 0600 | `/etc/init.d/opendectcell enroll` deletes it after the attempt. If the cell package is missing the script returns before it does |
| `/tmp/opendectcell-fw-upload.<role>` | An uploaded chip firmware file | set by LuCI's upload handler (`cgi-io`): not established here | `firmware.sh install` deletes it on success and on every refusal in the code it reads |

Whether `/tmp` is a RAM filesystem is OpenWrt's default but is not set by openDECTcell: not established here.

### 1.4 Logs

| Where | What | Notes |
|---|---|---|
| System log (`logger -t opendectcell` from the init script; the daemons' stdout and stderr, which the init script gives to procd with `stdout 1`, `stderr 1`) | Normal operation: start and stop, registrations, **dialled numbers** (`call: dialled "..."`), **names** given to handsets (`names: DECT 1 is now ...`), the numbers and peers of SIP calls (`call from ... rings`, `DECT 1 calls ... on Line 1`), SIP registration failures | Where the system log lives and how long it is kept is OpenWrt's setting (`logd`): not established here. The Log tab of Status > DECT reads it with `logread` |
| Verbose daemon output (`dect-stubd -v`) | Every chip mail with its type and length; "parameters hidden" | Not used by the init script (it passes neither `-v` nor `-p`), only by a developer starting the daemon by hand |
| `dect-stubd -p` | Also the parameter bytes of every mail: identity data. Key bytes print as `**` even then (`log_mail`, `mail_byte_is_key`) | Never share such a log |
| `dect-stubd -w FILE` | The speech of calls, raw | A developer option; the file holds call audio |

What the daemon masks: with `-v` the parameters are hidden; with `-p` the UAK bytes in the key records are still
printed as `**`. What it does not mask: the review's finding L-8 says `-v` logs list contents and registration
challenges, and the daemon's normal lines carry dialled numbers and names (above).

## 2. Removing a handset and its data

### 2.1 What a deregistration does

Deregister on Network > DECT, Handsets (or a handset deregistering itself) runs `dect_base_dereg_begin` and then
`dect_base_dereg_erase` in `src/libopendectcell/dect_base.c`, and `base_forget` in `src/stubd/dect-stubd.c`:

- The key is cleared **in the chip**: the daemon writes zeros to the chip's key slot and subscription slot.
- The handset is dropped from the daemon's tables and the internal names list; its settings in memory are reset.
- On flash, the subscription store and its `.name`, `.profile` and `.cap` files are **renamed** to `*.deregistered`,
  not deleted, "so a mistake can be undone by moving them back" (comment in `dect-stubd.c`). The key stays on flash.
- The web page then runs `/etc/init.d/opendectcell forget N`, which deletes the handset's UCI section
  (`opendectcell.dectN`), the two generated files `handsetN.override` and `handsetN.lines`, and restarts the service if the
  handset had a line. The VoIP profiles it used stay, free (they hold SIP credentials).

What a deregistration does **not** touch:

- the `*.deregistered` files;
- entries of this handset in the call lists (`/var/run/opendectcell/calls`): the code that erases a handset
  (`dect_base_dereg_erase`) does not call `dect_calls_clear`;
- the VoIP profiles and their SIP passwords;
- contacts the handset added in the CardDAV address book (they are on the server).

### 2.2 Administrator procedure: remove a handset and everything about it

Slot N is the number shown on the page (1..6). Run as root on the box.

1. Deregister the handset on Network > DECT, Handsets, and wait until the page reloads. If the handset is lost or
   switched off, the base gives up after about 30 seconds ([luci-manual.md](luci-manual.md)). If the row says
   *Remove* instead, press it.
2. Delete the leftovers of the key and the side files:
   `rm -f /etc/opendectcell-sub.N.deregistered /etc/opendectcell-sub.N.name.deregistered /etc/opendectcell-sub.N.profile.deregistered /etc/opendectcell-sub.N.cap.deregistered`
   Check with `ls -l /etc/opendectcell-sub.*`: nothing for this N should remain (a store `/etc/opendectcell-sub.N` appears
   again only when another handset registers into that slot).
3. If the handset had its own VoIP profiles, delete them (`uci delete opendectcell.voipN`, `uci commit opendectcell`), and
   change the SIP password at the provider if the handset was not yours.
4. Clear its entries from the call lists: stop the service (`/etc/init.d/opendectcell stop`), `rm /var/run/opendectcell/calls`,
   start it again. This removes the calls of **all** handsets from the lists. (Stop first: the daemon rewrites the
   file while it runs and at its end.)
5. Make a new backup (section 3). Old backups still contain the key (the `.deregistered` files are matched by the
   keep pattern `/etc/opendectcell-sub.*`) and must be destroyed or accepted as a risk.

Removing a file on flash does not guarantee that its bytes are overwritten; whether the storage can be wiped
securely is not established here. For a box that changes hands, see factory reset below.

### 2.3 Leaving cell mode and factory reset

`/etc/init.d/opendectcell cell_leave` (Network > DECT, Leave cell mode) runs `dect-cell forget`, which deletes `key.pem`,
`cert.der`, `ca.der`, `enrollment`, `key.pem.new`, `cert.der.new`, `rfpi` and `rfpi.tried` in `/etc/opendectcell/cell/`,
sets `opendectcell.main.role` to `base`, and deletes `stub.second` so the chip is loaded with the factory identity. The
empty directory stays. It does **not** touch handset stores (in cell mode `dect-stubd -K` holds no handsets: the
controller does; whether the controller keeps a copy is the controller's matter, see section 6), UCI, the call lists
or the chip firmware.

A factory reset of the whole box is OpenWrt's own function (the overlay is wiped, which removes everything under
`/etc` listed above). openDECTcell has no reset command of its own, and what the flash does with the freed blocks is not
established here. `apk del opendectcell ...` removes the programs; whether it removes files created at run time such as
`/etc/opendectcell-sub.*` is not established here (they are not in the package's file list).

## 3. Backups

An OpenWrt configuration backup (`sysupgrade -b`, or System > Backup in LuCI) archives the files that sysupgrade
keeps. The repository declares these for openDECTcell:

- `/etc/config/opendectcell`: PIN, SIP passwords, CardDAV password, handset settings (UCI, and a `conffiles` entry in
  `openwrt/net/opendectcell/Makefile`);
- `/etc/opendectcell-sub.*`: **every handset key**, the side files, and the `.deregistered` leftovers;
- `/etc/opendectcell/cell/`: the cell's TLS **private key**, certificate and enrolment string;
- `/etc/opendectcell/firmware/`: the user's chip firmware (not personal, but not for sharing, see
  [firmware-sourcing.md](firmware-sourcing.md));
- `/etc/opendectcell/profiles/`: model profiles.

Whether `sysupgrade -b` includes exactly the same set as a sysupgrade is OpenWrt's behaviour and is not established
here; list what a backup would contain with `sysupgrade -l` before you store it.

**Protect the backup file like the box itself.** File permissions are not encryption at rest: the archive is plain, and
anyone who has it can read the PIN, the passwords and the handset keys, and could impersonate the base to a handset.
Store it encrypted and off the box, share it with nobody, and do not post it in an issue. Rotate what it holds
(handset registration, SIP and CardDAV passwords) if it leaks. The same holds for the box's flash itself: openDECTcell
does not encrypt anything on it.

## 4. Logs: what to remove before you post one

[CONTRIBUTING.md](../CONTRIBUTING.md) tells reporters to remove phone numbers, SIP user names and server addresses and,
if they prefer, the handset's IPEI before posting a log. In terms of the lists above:

- normal logs: dialled numbers, handset names, caller numbers and names, SIP peers and addresses, the INVITE source
  address;
- the Status > DECT page and `state.json` show the IPEI; the normal log lines do not print it (`dect_ipei_text` is
  used only for the state file);
- a log taken with `-p` holds identity data and must not be posted at all (the daemon's own help says so);
- never post `/etc/config/opendectcell`, a store file, `/var/run/opendectcell/baresip/accounts` or a backup. To share
  settings, use `uci export opendectcell` and delete every `password`, `stunpass`, `pin`, `username` and `url` line first.

A security problem goes through the private channel in [SECURITY.md](../SECURITY.md), not a public issue.

## 5. CardDAV: what a handset can change on the server

The phonebook works both ways ([phonebook.md](phonebook.md)): a handset's edit, delete or new contact is written to the
server's address book (`carddav.uc`, `apply_change`: PUT with `If-Match`, DELETE, PUT with `If-None-Match`). An edit
changes only the name and telephone number fields of the card; a delete removes the card on the server. Anyone with
access to a registered handset can therefore change or delete contacts on the server, with the rights of the
configured account.

- **`allow_clear`** (UCI option of `config carddav`, Network > DECT, Phonebook, *Allow "delete list"*): when `1`, a
  handset's "delete list" sends a DELETE for every card the sync knows in every chosen book. **Default `0`**
  (`opendectcell.config`, `carddav.uc`: `allow_clear: s.allow_clear == '1'`); with `0` the request is skipped.
- **Read-only mode: not available.** No option stops single edits and deletes from reaching the server. The only
  ways to limit the damage are the server side (an account or an app password with read-only rights, which the
  sync would then fail to write with) and a server-side backup. Whether a given server refuses writes cleanly is
  not established here.
- The credentials go to the configured server only; a redirect to another origin is not followed (`carddav.uc`,
  fix M-4 of the review).
- Nothing of the address book is kept on flash: the cache is in RAM. The password is in UCI.

## 6. Not covered here

How long a multi-user or hosted deployment keeps data, who may see it and how a person's data is erased across many
boxes are decisions for the operator and for the controller (<https://github.com/openDECTcell/opendectcell-controller>, a separate
project with its own documents), not for this page. This page does not give legal advice.
