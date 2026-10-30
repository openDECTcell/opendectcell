# The handsets' phonebook from a CardDAV address book

Since 2026-10-30. The handsets read the base's Contact List (CAT-iq list
access, TS 102 527-3 V1.7.1 clause 7.4.10.5.7), and the base fills it from
CardDAV address books (RFC 6352) over HTTPS. It works both ways: an edit or
a delete on a handset goes back to the book the contact came from, and a
contact made on a handset goes to the book chosen for new contacts.

**Several books, no merging.** One account usually has several address
books. LuCI's "Fetch address books" lists them, and the user ticks the ones
to use and picks the one for new contacts. Contacts are not merged: one that
is in two books appears twice on the handsets (the maintainer's decision,
2026-10-30). LuCI says so on the Phonebook tab.
The maintainer asked for this on 2026-10-28 (ROADMAP M9b) and approved it on
2026-10-30.

## Call lists

Also since 2026-10-30, and served beside the Contact List: the Missed Calls
List (01H), the Outgoing Calls List (02H) and the Incoming Accepted Calls
List (03H), TS 102 527-3 7.4.10.5.3-5 (`src/libopendectcell/dect_calls.[ch]`).

- **Where the calls come from:** the baresip module knows a SIP call's
  outcome over every handset it rang. When an incoming call ends, it sends
  dect-stubd one `calllog` (missed or accepted); an outgoing call is sent
  when it is placed (`docs/bridge-interface.md`).
- **Storage:** 40 entries per list, in RAM, newest first. A restart of the
  service or the box empties them.
- **Merging:** a call from or to the party of the newest entry merges into
  it (strategy 3). A missed one counts up 'Number of calls' and becomes
  unread again.
- **Per handset:** each handset sees the calls of its own lines, under its
  own line id ("FP virtual list strategy 2").
- **Names:** the Contact List's name comes before the caller's own name, as
  7.4.10.5.1.3 note 1 recommends.
- **The missed call notification** (7.4.1.3) carries the unread count and
  the list change indication. It goes to the handsets the call rang: at
  once on a link, else by a page. It is sent again, as an update, after a
  handset reads or deletes entries and after every location registration.
  Part 3 handsets only: the C430A (Part 1) neither reads the lists nor gets
  the notification.
- **The List of Supported Lists notice:** a handset keeps the List of
  Supported Lists it read once. dect-stubd therefore sends a list change
  indication for it after each location registration, and the handset then
  re-reads it.
- **On the A540, 2026-10-30:**
  - after a power cycle it re-read list 00 ({00,01,02,03,05,06,07,08});
  - an unanswered call from a mobile rang both handsets and gave "1 unread
    of 1 (a new one)";
  - the A540 read the Missed Calls List (number, time, unread, line
    "Fritzbox") and marked the entry read, and the update (0 unread)
    followed.
- **Not yet:** the All Calls and All Incoming Calls lists (optional), a
  search on the call lists (it answers "no match"), and keeping the lists
  across a restart.

## Parts

| part | where | does |
|---|---|---|
| the store | `src/libopendectcell/dect_contacts.[ch]` | up to 500 contacts, sorted by name then first name without case (ä as a, ß as ss), stable entry identifiers, a journal of the handsets' changes, caller-name lookup by number |
| the list | `src/libopendectcell/dect_iwu.c` | Contact List 05H: read, search (by name), query fields, edit, save (existing and new), delete entry, delete list; one store shared by every link, so an entry one handset edits is locked for the others |
| dect-stubd | `-b FILE` | offers the list, re-reads FILE when it changes, appends the handsets' changes to FILE.changes; an incoming call without a SIP name gets the contact's name |
| the sync | `openwrt/net/opendectcell/files/carddav.uc`, package `opendectcell-carddav` | ucode, about 25 KB; finds the account's books (`discover`), reads the chosen ones into FILE and writes FILE.changes back |
| its HTTP client | `openwrt/utils/ucode-mod-opendectcell-uclient` | uclient's ucode module with two patches: sized request bodies and the REPORT method, 25 KB installed (see Limits) |
| LuCI | Network > DECT, Phonebook tab | server, user and password; "Fetch address books" and a row per book (use it, new contacts go here); the last sync per book |

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

- **Discovery** follows RFC 6764 section 6 and RFC 6352 section 7:
  - the URL's own books;
  - else the current-user-principal of the URL or of `/.well-known/carddav`;
  - that principal's addressbook-home-set and the books in it.

  PROPFIND only. The form's password goes to the box in a root-only file,
  which the script deletes; it is never on a command line. DNS SRV lookup
  (RFC 6764 section 3) is not done: give the server's own address.
- **Chunked uploads: fixed in our build.** OpenWrt's HTTP client (uclient)
  sends every request body with `Transfer-Encoding: chunked`.
  - Servers behind nginx, or Apache with mod_php, accept that.
  - A server that cannot (Radicale's built-in server; PHP behind Apache's
    mod_proxy_fcgi) sees an empty body, so handset changes fail there.
  - Since 2026-10-30 the sync uses `opendectcell_uclient` (package
    `ucode-mod-opendectcell-uclient`, `openwrt/utils/`): uclient's ucode module,
    built from the commit the base feed uses, with one patch. A body whose
    length the caller gives in a Content-Length header goes out unframed.
    The system's libuclient is left as it is.
  - Without the package the sync falls back to the stock module, chunked.
  - Tested on the 7412 against `carddav-testserver.py --no-chunked`, which
    reads a chunked body as empty, as WSGI servers do: the stock module's
    edit was refused, and with ours the edit, the delete and the new card
    all arrived.
- **REPORT.** Each sync first asks each book for its CTag. When the CTag
  changed, the sync lists the ETags (PROPFIND Depth 1) and fetches the cards
  that changed with addressbook-multiget, 50 per request. uclient had no
  REPORT; `opendectcell_uclient` adds it.
  - Where REPORT fails (the stock module, a server answering 405), the sync
    fetches card by card for the rest of that round, about 0.3 s per card
    on the 7412.
  - Tested against `carddav-testserver.py`: two books took 6 PROPFINDs and
    2 REPORTs; with `--no-report`, the same result by GET.
- **500 contacts at most**, over all books. More are cut, and the log says
  so. The maintainer expects that to be enough for nearly everyone. The
  handsets never hold the list: they read it from the base in pieces and
  search it there (7.4.10.4.3, 7.4.10.4.8). So the only limit is the
  base's, and a search reaches every contact the base holds.
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
- **Several books, 2026-10-30, 7412 against the test server:**
  - discovery from the server root, from a book URL, from an unknown path
    (via `/.well-known/carddav`) and with a wrong password;
  - a sync of two books, Anna in both shown twice;
  - an edit of the work book's Anna went back to the work book, a new
    contact went to the default book, a delete went to the work book;
  - the Phonebook tab in headless Chrome: the form, "Fetch address books"
    and the book rows render.
- **Not tested yet:**
  - a handset reading or editing the list on the air;
  - a real server (Nextcloud, Baïkal);
  - the LuCI tab in a browser.
