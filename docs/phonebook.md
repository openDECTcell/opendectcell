# The handsets' phonebook from a CardDAV address book

Since 2026-10-30. The handsets read the base's Contact List (CAT-iq list
access, TS 102 527-3 V1.7.1 clause 7.4.10.5.7), and the base fills it from
one CardDAV address book (RFC 6352) over HTTPS. It works both ways: a
contact a handset adds, edits or deletes is written back to the server.
The maintainer asked for this on 2026-10-28 (ROADMAP M9b) and approved it on
2026-10-30.

## Parts

| part | where | does |
|---|---|---|
| the store | `src/libopendectcell/dect_contacts.[ch]` | up to 500 contacts, sorted by name then first name without case (ä as a, ß as ss), stable entry identifiers, a journal of the handsets' changes, caller-name lookup by number |
| the list | `src/libopendectcell/dect_iwu.c` | Contact List 05H: read, search (by name), query fields, edit, save (existing and new), delete entry, delete list; one store shared by every link, so an entry one handset edits is locked for the others |
| dect-stubd | `-b FILE` | offers the list, re-reads FILE when it changes, appends the handsets' changes to FILE.changes; an incoming call without a SIP name gets the contact's name |
| the sync | `openwrt/net/opendectcell/files/carddav.uc`, package `opendectcell-carddav` | ucode, about 20 KB; reads the address book into FILE and writes FILE.changes back |
| LuCI | Network > DECT, Phonebook tab | the settings and the last sync's result |

File formats: the comment at the top of `dect_contacts.h`.

## What a handset sees

- Name (the vCard's N family name; a card without N shows FN, then ORG),
  first name, up to three numbers and the line.
- The numbers are the first three telephone numbers of the card that are
  not fax or pager numbers: CELL is "mobile", WORK "work", anything else
  "fixed". Digits only; a leading + becomes 00.
- The line is "all lines" unless a handset set one, kept as
  X-OPENDECTCELL-LINE on the card.
- No per-contact melody (the optional 'Associated melody' field is not
  offered).

## What goes back to the server

- **An edit:** the card is fetched again with its ETag, and only N, FN and
  the telephone numbers the handset showed are changed. Everything else on
  the card (photo, addresses, e-mail, fax numbers, notes) stays. The PUT
  carries If-Match. If the card changed on the server in between, it is
  fetched and changed again, once.
- **A new contact:** a vCard 3.0 under a new UID, PUT with If-None-Match.
- **A delete:** DELETE with If-Match.
- **"Delete list":** refused by default. With `allow_clear '1'` it deletes
  every card in the address book.
- **When the server is unreachable**, the changes stay in FILE.changes.work
  and are retried. Until the server has them, the handsets see their own
  version.

## Limits

- **One address book, its URL configured.** No discovery from a server
  name.
- **Chunked uploads.** OpenWrt's HTTP client (uclient) sends every request
  body with `Transfer-Encoding: chunked` and has no other mode.
  - Servers behind nginx, or Apache with mod_php, accept that.
  - A server that cannot (Radicale's built-in server; PHP behind Apache's
    mod_proxy_fcgi) sees an empty body. Reading still works there, but
    handset changes fail.
  - The fix would be a small uclient change that sends Content-Length when
    the body is known. That is an upstream change; not done.
- **No REPORT** (uclient has no such method): each sync first asks for the
  collection's CTag. When the CTag changed, it lists the ETags and fetches
  only the cards that changed.
  - The first sync of a large address book takes about 0.3 s per card on
    the 7412 (500 cards: about 2.5 minutes).
- **500 contacts at most**; more are cut, and the log says so.
- **Nothing on flash.** The cache lives in the sync's memory and the files
  under /var/run. After a reboot the first sync reads the address book
  again.

## Tested

- **Unit tests:** `tests/test_contacts.c`, 148 checks.
  - The store's order and reloads.
  - Changes surviving reloads until the sync has them.
  - Deletions.
  - Caller-number matching.
  - The list commands byte for byte against the standard's tables,
    including partial delivery and locks across two links.
- **On the 7412, 2026-10-30,** against `tools/carddav-testserver.py`, which
  has its own CA and seed cards in vCard 3.0 and 4.0, a company card, a fax
  number, a photo and folded lines:
  - the first sync gave the four expected entries;
  - three journal lines (an edit, a delete, a new contact) arrived on the
    server as described above, with the photo, e-mail and fax kept;
  - the contacts file came back with seq=3 and the new entry's identifier
    hint.
- **Integration on the 7412, the same day:** with the service started,
  dect-stubd loaded the four contacts, and a journal line went to the
  server and back into dect-stubd within seconds.
- **Stopping:** the service stop first left the sync running until procd
  killed it 5 s later. Under ucode's uloop a SIGTERM was caught and
  dropped. The sync now exits from its own handler. Timed stop, with the
  server unreachable and after real requests: under 1 s in both cases.
- **Not tested yet:**
  - a handset reading or editing the list on the air;
  - a real server (Nextcloud, Baïkal);
  - the LuCI tab in a browser.
