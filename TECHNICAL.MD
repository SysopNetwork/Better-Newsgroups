# Better Newsgroups — Technical Notes

## Architecture

```
 news server (NNTP)
        ^  |
        |  v
  BNWORK worker thread            no SDK calls
  - BNNNTP: connect, TLS, AUTHINFO, POST, GROUP, ARTICLE
  - IEMSG/IETEXT: MIME -> CP437 text      -> SNBETNWS\IN\xxxxxxxx.TXT/.RDY
  - posts SNBETNWS\OUT\xxxxxxxx.ART
        ^  |   job / result (critical section, one slot)
        |  v
  SNBETNWS.C main thread           rtkick timer every second
  - dispatch(): copy settings, groups, due posts into a BNJOB
  - collect(): post outcomes, group positions, new spool ids
  - importsome(): a few impmsg() steps per tick
  - GME new-message hook -> bnartBuild() -> SNBETNWS\OUT
```

| File | Thread | Purpose |
|---|---|---|
| `SNBETNWS.C` | main | Init/shutdown, timer, import into GME, export hook, outgoing queue, Message-ID cross-reference, settings and newsgroup records, Sysop editor |
| `BNWORK.C` | worker | Worker thread, job slot, one session (`bnwRun`), article spooling |
| `BNNNTP.C` | worker | NNTP client (RFC 3977/4643/4642), dot-stuffing both ways |
| `BNART.C` | both | Outgoing article building, RFC 2047 words, References and Date parsing |
| `BNSPOOL.C` | both | Incoming spool files |
| `SHARED/*` | both | From Better Internet Email: Winsock+Schannel (`SBNET`), MIME decoding (`IEMSG`/`IETEXT`), CP437/UTF-8 (`SBMAIL`), helpers (`SBUTIL`) |

`BNWORK`, `BNNNTP`, `BNART`, `BNSPOOL` and `SHARED` include no SDK headers, so they also build into `BNFETCH.EXE` and `BNTEST.EXE`.

## Data files

**SNBETNWS.DAT:** 1024-byte packed records, one 96-byte string key.
- `!SETTINGS`: `struct bncfg` (a `!` cannot start a newsgroup name)
- one `struct bngrp` per newsgroup link, keyed by the lower-case name: forum, import/export flags, next article number (0 = start with backfill), counters, last problem

**SNBETMID.DAT:** `struct bnmid`, 512 bytes, one per forum message that is also a Usenet article.

| Key | Segments | Use |
|---|---|---|
| 0 unique | Message-ID (248) + forum (2) | skip duplicates; find the parent forum message when importing |
| 1 dup | GME global id (sysid, msgid) | Message-ID of the message a user replied to, for `References` |
| 2 dup | time added | oldest-first purge after "Keep Message-IDs" days (25 records per tick) |

Lookups use `dfaAcqEQ` with a full-width, zero-padded key in writable memory. Purge deletes directly after `dfaAcqLO`.

## Import

1. The worker opens each import group with `GROUP`. It starts at `next`; a new group (`next` = 0) or one whose server numbering went down starts at `last - backfill + 1`, or at the server's first article when backfill is ALL (and never before the first article). It reads with `ARTICLE n` up to "Most articles per check" (and at most four times that many numbers tried). An article over the size limit keeps only its header. A large backfill is therefore read over many checks; the saved `next` carries it on. Imported messages keep the article's date, so a forum with a message lifetime deletes articles older than that at GME's cleanup.
2. `bnwSpoolArticle` decodes with `iemailParse` and writes `.TXT`, then `.RDY` (by rename).
3. `collect()` saves each group's new `next` and queues the spool ids.
4. `loadnext()`: the link and forum must still exist; an article already in the forum (Message-ID + forum) is dropped; the parent is the nearest References entry found in the same forum.
5. `step()`: `impmsg()` with `rplto` = parent, so GME files it in the parent's thread (`GMECORE.C:4290`). Imports are NOECHO unless `gmeForceEcho()` is called (`GMECORE.C:4303`), so other exporters never see them. Long text is split into up to 8 parts; parts 2+ reply to part 1.
6. The Message-ID record is written when part 1 goes in; the spool is removed after the last part. A restart in between cannot import twice.

From is GALSMTP's form (`IN:user@host`, via `smtpCvtFrom`) so "reply by e-mail" works. The date is the article's own date in local time (packed DOS format) unless it is unreadable or in the future.

## Export

`hook_gme(GMEHOOK_NOT_NEWMSG)` sees every new message. Only `NEWMSG_WRITE`/`NEWMSG_REPLY` in a forum linked for export, with the gateway on (and the post key, if set), are queued. The article is built at once (`bnartBuild`): user address by GALSMTP's rules (`xltGME2Int`, else alias@`smtpHost()`), References from the cross-reference (thread root + parent), signature from `TAGLINE`, UTF-8 8bit body, no `Path`. The Message-ID is recorded immediately.

Post outcomes: `240` OK; `441` naming a duplicate counts as OK (a retry after a dropped reply); other `441` is permanent (the author gets BBS e-mail); everything else is retried every "Retry interval" until "Give up after".

## Message file

`SNBETNWS.MSG` is CP437 with CRLF line endings and real ESC/0x01 bytes, with one `LEVEL6` block (message 1 = `SYSONLY`). Text variables are `SNBN_*`. Append new messages at the end. Hook-time and timer-time text uses `getmsg()`, never `prfmsg()`, so a user's output buffer is never touched.
