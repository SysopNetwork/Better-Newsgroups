<div align="center">

```
                    ██████╗ ███████╗████████╗████████╗███████╗██████╗
                    ██╔══██╗██╔════╝╚══██╔══╝╚══██╔══╝██╔════╝██╔══██╗
                    ██████╔╝█████╗     ██║      ██║   █████╗  ██████╔╝
                    ██╔══██╗██╔══╝     ██║      ██║   ██╔══╝  ██╔══██╗
                    ██████╔╝███████╗   ██║      ██║   ███████╗██║  ██║
                    ╚═════╝ ╚══════╝   ╚═╝      ╚═╝   ╚══════╝╚═╝  ╚═╝

  ███╗   ██╗███████╗██╗    ██╗███████╗ ██████╗ ██████╗  ██████╗ ██╗   ██╗██████╗ ███████╗
  ████╗  ██║██╔════╝██║    ██║██╔════╝██╔════╝ ██╔══██╗██╔═══██╗██║   ██║██╔══██╗██╔════╝
  ██╔██╗ ██║█████╗  ██║ █╗ ██║███████╗██║  ███╗██████╔╝██║   ██║██║   ██║██████╔╝███████╗
  ██║╚██╗██║██╔══╝  ██║███╗██║╚════██║██║   ██║██╔══██╗██║   ██║██║   ██║██╔═══╝ ╚════██║
  ██║ ╚████║███████╗╚███╔███╔╝███████║╚██████╔╝██║  ██║╚██████╔╝╚██████╔╝██║     ███████║
  ╚═╝  ╚═══╝╚══════╝ ╚══╝╚══╝ ╚══════╝ ╚═════╝ ╚═╝  ╚═╝ ╚═════╝  ╚═════╝ ╚═╝     ╚══════╝
```

**A two-way gateway between Usenet newsgroups and the forums of The Major BBS v10.**

By **[Sysop Network](https://github.com/SysopNetwork)** — https://github.com/SysopNetwork/Better-Newsgroups

[![Major BBS](https://img.shields.io/badge/The%20Major%20BBS-v10-blue)](https://github.com/SysopNetwork/Better-Newsgroups)
[![License](https://img.shields.io/badge/license-MIT-blue?logo=opensourceinitiative&logoColor=white)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20x86-lightgrey?logo=windows&logoColor=white)]()
[![Release](https://img.shields.io/github/v/release/SysopNetwork/Better-Newsgroups)](https://github.com/SysopNetwork/Better-Newsgroups/releases)

📋 **[See what's new in v1.0.0 →](CHANGELOG.MD)**

</div>

---

Articles in a newsgroup appear as messages in a BBS forum, threaded the way they were on Usenet. Messages your users write in that forum are posted back to the newsgroup. Everything runs inside the BBS as one module (`SNBETNWS`), with no external programs or scheduled tasks to set up.

---

## ✨ Features

| | Feature | Description |
|---|---|---|
| 🔁 | **Import and export** | Each linked newsgroup can bring articles in, send forum messages out, or both |
| 📰 | **Any NNTP server** | Works with any news server, with an AUTHINFO login when the server needs one |
| 🔒 | **Encrypted connections** | TLS (usually port 563) or STARTTLS, with certificate checking |
| 🧵 | **Threading kept both ways** | Usenet replies are filed in the right forum thread; BBS replies carry proper `References` headers |
| 🚫 | **No loops or duplicates** | Imported messages are never posted back, and the module recognizes its own posts when the server sends them back |
| 🔤 | **Readable text** | MIME, quoted-printable, Base64, UTF-8 and other character sets become CP437; HTML-only articles become plain text; long articles are split across messages |
| ✉️ | **Posts look right on Usenet** | UTF-8 text with colour codes removed, the user's BBS mail address, an optional signature line and Organization |
| 📚 | **Backfill** | A new group can start with its last 0–9999999 articles, or ALL the server still has, read over several checks |
| 📤 | **Reliable posting** | An outgoing queue with retries; if a post finally fails, the author gets a BBS e-mail saying why |
| 🛠️ | **All settings online** | Changed from the module's Sysop menu while the board runs, applied at once, with no CNF options |
| ⚡ | **Never slows the board** | All network work happens on a background thread |
| 📜 | **Audit Trail and transcript** | Problems (and, if wanted, normal activity) go to the Audit Trail; an optional server transcript never contains passwords |

---

## 🚀 Installation

### Requirements

- The Major BBS v10 (the module is 32-bit, like every v10 module, and runs on 64-bit Windows)
- An account on an NNTP news server

### Steps

1. Download `Better-Newsgroups-v1.0.0.zip` from [Releases](https://github.com/SysopNetwork/Better-Newsgroups/releases).
2. Copy `SNBETNWS.DLL`, `SNBETNWS.MDF`, `SNBETNWS.MSG` and `SNBETNWS.RLN` to the BBS directory.
3. Add **Better Newsgroups** to a Sysop menu with the Menu Tree Editor.
4. Restart the BBS. The data files (`SNBETNWS.DAT`, `SNBETMID.DAT`) are created on first start.

### Quick start

Enter the module from the menu (the SYSOP key is required).

1. Settings **2–7**: your news server, port, connection security and login.
2. **G**: link each newsgroup to a forum. Create the forums first with the usual forum tools.
3. Setting **1** to **YES**, then **C** to check right away. The status line and the Audit Trail show the result.

Each setting has help: type `?` at its prompt, or `H` and its number at the menu.

---

## ⚙️ Configuration

All settings live in `SNBETNWS.DAT` and are edited online from the module's Sysop menu. A change applies at once; there are no CNF options.

### 🌐 News server

<table>
<tr>
<th width="60">#</th>
<th width="230">Setting</th>
<th>Description</th>
<th width="130">Default</th>
</tr>
<tr><td>1</td><td>Gateway enabled</td><td>Turns all checking and posting on or off</td><td><code>NO</code></td></tr>
<tr><td>2</td><td>News server</td><td>Host name of the NNTP server</td><td>(none)</td></tr>
<tr><td>3</td><td>News server port</td><td>119 for plain or STARTTLS, 563 for TLS</td><td><code>119</code></td></tr>
<tr><td>4</td><td>Connection security</td><td><code>NONE</code>, <code>STARTTLS</code> or <code>TLS</code></td><td><code>NONE</code></td></tr>
<tr><td>5</td><td>Verify TLS certificate</td><td>Check the server's certificate (recommended)</td><td><code>YES</code></td></tr>
<tr><td>6</td><td>Login name</td><td>AUTHINFO login name; <code>-</code> for none</td><td>(none)</td></tr>
<tr><td>7</td><td>Password</td><td>AUTHINFO password, never shown once set; <code>-</code> clears it</td><td>(none)</td></tr>
<tr><td>8</td><td>Check every (minutes)</td><td>How often the server is checked, 1–1440</td><td><code>10</code></td></tr>
<tr><td>9</td><td>Timeout (seconds)</td><td>How long to wait for the server, 10–300</td><td><code>60</code></td></tr>
</table>

### 📥 Import

<table>
<tr>
<th width="60">#</th>
<th width="230">Setting</th>
<th>Description</th>
<th width="130">Default</th>
</tr>
<tr><td>10</td><td>Articles for a new group</td><td>How many existing articles a new link starts with: 0–9999999, or <code>ALL</code></td><td><code>50</code></td></tr>
<tr><td>11</td><td>Most articles per check</td><td>Per newsgroup, 1–1000; a larger backlog continues next check</td><td><code>200</code></td></tr>
<tr><td>12</td><td>Largest article (KB)</td><td>Larger articles are imported as a short notice, 4–1024</td><td><code>64</code></td></tr>
<tr><td>19</td><td>Sender line in messages</td><td>Put the sender's name, address and the date at the top of imported messages</td><td><code>YES</code></td></tr>
<tr><td>20</td><td>Word-wrap column</td><td>Word-wrap imported text at this column, 40–132; 0 leaves lines as sent</td><td><code>79</code></td></tr>
<tr><td>21</td><td>Keep Message-IDs (days)</td><td>How long the BBS remembers which forum message is which article (no duplicates, threading), 7–3650</td><td><code>365</code></td></tr>
</table>

### 📤 Export

<table>
<tr>
<th width="60">#</th>
<th width="230">Setting</th>
<th>Description</th>
<th width="130">Default</th>
</tr>
<tr><td>13</td><td>From name</td><td>Display name on posts; <code>{USER}</code> = User-ID, <code>{BBS}</code> = BBS name</td><td><code>{USER}</code></td></tr>
<tr><td>14</td><td>Message-ID host name</td><td>Host name in each post's Message-ID; set it when the mail host name is not a real domain</td><td>BBS mail host name</td></tr>
<tr><td>15</td><td>Organization</td><td>One-line Organization header</td><td>(none)</td></tr>
<tr><td>16</td><td>Key needed to post</td><td>Limit posting to Usenet to users holding this key</td><td>anyone who can write in the forum</td></tr>
<tr><td>17</td><td>Retry interval (minutes)</td><td>Wait between attempts to send a post, 1–1440</td><td><code>15</code></td></tr>
<tr><td>18</td><td>Give up after (hours)</td><td>A post still unsent after this long is dropped and its author told, 1–720</td><td><code>48</code></td></tr>
</table>

### 📜 Logging

<table>
<tr>
<th width="60">#</th>
<th width="230">Setting</th>
<th>Description</th>
<th width="130">Default</th>
</tr>
<tr><td>22</td><td>Audit normal activity</td><td>Record each check that found articles, and each post sent, in the Audit Trail</td><td><code>NO</code></td></tr>
<tr><td>23</td><td>Audit errors</td><td>Write problems to the Audit Trail</td><td><code>YES</code></td></tr>
<tr><td>24</td><td>Transcript log file</td><td>Record the server conversation in <code>SNBETNWS.LOG</code> (passwords are never written)</td><td><code>NO</code></td></tr>
</table>

### 🔗 Newsgroup links (G)

Each link joins one newsgroup to one forum. Several newsgroups may share one forum; export then posts to all of them at once, as one cross-posted article.

<table>
<tr>
<th width="230">Setting</th>
<th>Description</th>
</tr>
<tr><td>Newsgroup</td><td>The newsgroup's name, exactly as the news server lists it</td></tr>
<tr><td>Forum</td><td>The BBS forum it is linked to</td></tr>
<tr><td>Import articles</td><td>Copy the group's articles into the forum</td></tr>
<tr><td>Export messages</td><td>Post what users write in the forum to the group</td></tr>
<tr><td>Next article number</td><td>The next article to read; 0 starts over with the newest articles (setting 10), without importing anything twice</td></tr>
</table>

### 🧭 Sysop menu

| Key | Action |
|---|---|
| `1`–`24` | Change a setting |
| `N` | Other page of settings |
| `G` | Newsgroup links |
| `C` | Check the news server now |
| `Q` | Posts waiting to be sent |
| `H` | Help (`H 12` explains setting 12) |
| `X` | Exit |

---

## 💬 Usage

Users read and write in the linked forums as they do in any other forum. Forum access settings decide who can read and who can post; setting 16 can further limit posting to Usenet to users who hold a key.

A forum's message lifetime still applies: the BBS's nightly cleanup removes older imported articles like any other forum message.

---

## 📁 Architecture

```
Better-Newsgroups/
├── SRC/
│   ├── SNBETNWS.C        # module: Sysop menu, settings, import, export hook, timer
│   ├── BNWORK.C          # background worker: one news server session per check
│   ├── BNNNTP.C          # NNTP client (AUTHINFO, GROUP, ARTICLE, POST, STARTTLS)
│   ├── BNART.C           # articles in and out (headers, MIME, References)
│   ├── BNSPOOL.C         # incoming spool between the worker and the BBS thread
│   ├── BNVER.H           # the version number
│   ├── SNBETNWS.MSG      # all text the module shows (CP437, Sysop-editable)
│   ├── SNBETNWS.MDF      # module definition
│   ├── SNBETNWS.RLN      # release notes / online manual
│   └── SHARED/           # TLS sockets, MIME and character set code
├── TESTS/                # BNTEST unit tests
├── TOOLS/                # BNFETCH command-line test tool
└── DIST/v1.0.0/          # the deployable files
```

The network work runs on a background thread that never touches the BBS. Articles it fetches are written to `SNBETNWS\IN\` and imported into forums a few at a time by the BBS's own thread. A message written in a linked forum is turned into an article at once, queued in `SNBETNWS\OUT\`, and posted by the worker. `SNBETMID.DAT` maps every Message-ID to its forum message, which is how threads are kept and loops prevented.

See [TECHNICAL.MD](TECHNICAL.MD) for the details and [BUILDING.MD](BUILDING.MD) to build from source.

---

## 🛟 Support

Report problems at https://github.com/SysopNetwork/Better-Newsgroups/issues. Include the Audit Trail lines and, if you can, a transcript from setting 24. The transcript never contains passwords.

---

## 🏆 Credits

Better Newsgroups is developed and maintained by [Mark Laudenbach](https://github.com/laudenbachm) at [Sysop Network](https://github.com/SysopNetwork).

The files in `SRC/SHARED/` are shared with Sysop Network's Better Internet Email.

---

## 💖 Tips / Donations

If this module is useful to you, tips are appreciated:

- Cash App: https://cash.app/$laudenbachm
- Ko-fi: https://ko-fi.com/laudenbachm

---

## 📄 License

MIT — Copyright (c) 2026 Sysop Network. See [LICENSE](LICENSE).

---

Better Newsgroups for The Major BBS v10 by Mark Laudenbach - https://github.com/laudenbachm
