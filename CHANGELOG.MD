# Changelog

All notable changes to Better Newsgroups.  Versions follow Semantic Versioning.

## [1.0.0] — 2026-10-27

First release.

### Added
- Two-way newsgroup/forum gateway as a Major BBS v10 module (`SNBETNWS`).
- NNTP client with AUTHINFO login, TLS and STARTTLS, run on a background thread.
- Import of articles into forums: MIME and character set decoding to CP437, threading by References, the article's own date, long articles split into parts, too-large articles as a notice.
- Export of forum messages as UTF-8 articles with References, signature, Organization, and optional post key.
- Message-ID cross-reference (`SNBETMID.DAT`) so nothing is imported twice and the BBS's own posts are recognized.
- Outgoing queue with retries, give-up time, and a BBS e-mail notice to the author when a post fails.
- Online Sysop menu: settings, newsgroup links, check now, outgoing posts list.
- "Articles for a new group" accepts up to 9999999, or ALL for every article the server still has; a large backlog is read over several checks.
- `BNFETCH` test tool and `BNTEST` unit tests.
