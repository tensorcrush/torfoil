# Torfoil

A native BitTorrent client for the Nintendo Switch (Atmosphère), with a built-in
**Mullvad WireGuard tunnel** and direct `.nsp` / `.xci` installation through
`ncm`/`ns`. Like Tinfoil, except the source is a torrent.

> The interface speaks German, English, Spanish, French, Japanese, Russian and
> Chinese, and picks the console's own language on first launch. Code comments
> are in French.

## Install

Copy `torfoil.nro` to `sdmc:/switch/` and launch it from the homebrew menu.
`torfoil-diag.nro` is optional: it is the text-console diagnostic build, useful
when the main application refuses to start.

Installing games requires `prod.keys` at `sdmc:/switch/prod.keys` (dump it with
Lockpick_RCM). Downloading does not.

## Usage

| Tab | Controls |
|---|---|
| Torrents | **A** open a `.torrent` from the card · **X** paste a magnet · **ZL** import `magnets.txt` · **Y** pause/resume · **–** details · **B** skip verification · **ZR** remove |
| Library | **A** install · **Y** verify package · **X** rescan folder · **B** cancel current operation |
| Remote | **A** start/stop the local server · **Y** restart it |
| VPN | **X** Mullvad account (16 digits) · **A** connect/disconnect · **Y** country |
| Settings | **↑↓** select · **A** or **←→** change · **Y** self-test |

Removing a torrent asks first, and asks the useful question: keep the files
(**A**) or delete them too (**Y**).

**–** on a torrent opens the numbers a single line cannot hold: pieces, rejected
pieces, blocks in flight, connected peers *and how many of them are choking you*,
and the tracker list. Those last two answer "why is this slow?" on their own.

Free space on the card is shown in the top bar at all times, and turns amber below
5 GB. A modern Switch game does not fit under that.

Active torrents **resume automatically on startup**: each magnet link is kept in
`sdmc:/torfoil/downloads/.<hash>.magnet`. Without this, closing the application
silently abandoned the download: the files stayed on the card, half finished,
and nothing ever picked them up again.

## Language

Seven: **German, English, Spanish, French, Japanese, Russian, Chinese**. On first
launch the app reads the console's own setting and follows it; a homebrew that
opens in French on a German console looks broken before it has done anything.
Consoles set to a language not in the list get English, never French.

Change it at the top of the Settings tab — it is the first row precisely because
it is the one you must be able to reach when you cannot read what is below it.
The choice applies immediately and is stored in `settings.cfg` as `language=`
(`auto`, or a two-letter code).

The web page served to your phone is translated too, and carries the matching
`lang` attribute.

### Where the translations live

One file, [`include/ui/strings.def`](include/ui/strings.def): one line per key,
its seven translations side by side.

```c
X(TabRemote,
  "Fernzugriff", "Remote", "Remoto", "À distance", "リモート", "Удалённо", "远程")
```

That file is included twice — once to generate the key enum, once to generate the
table — so a forgotten column does not compile, and a `static_assert` catches any
drift between the two. Adding a language means adding a column and fixing every
line the compiler then points at. Reviewing a phrase means reading seven versions
of it on one screen rather than hunting through seven files.

**What is not translated:** engine diagnostics (storage errno detail, NCA parse
failures, tracker refusals) and the pre-UI fatal console messages. Those exist to
be pasted into a bug report, and they are French like the rest of the code. It is
a deliberate line, not an unfinished job.

### Privacy (Settings tab)

Every checkbox states what it costs. A protection that hides what it takes away
gets enabled by accident, then blamed for making the app slow.

| Toggle | What it actually does |
|---|---|
| Require VPN | No transport is handed to the engine without a tunnel. Nothing can leave, even if the VPN drops mid-download. |
| Encrypted trackers only | Rejects plain `http://` announces, which carry the info_hash in the clear. Fewer usable trackers. |
| Disable DHT | No more plaintext UDP queries. Far fewer peers found. |
| Disable PEX | Incoming messages are ignored *and* support is no longer advertised, since otherwise the peer already knows you take part. |
| Share nothing | Chokes every peer up front. You will be choked back: the protocol rewards reciprocity. |
| Remote access | Serves a web page on the local network. Only private addresses are answered, and nothing of the content passes through it. |

Downloads land in `sdmc:/torfoil/downloads`.

## Getting torrents in, from a phone (Remote tab)

This is the main route, and the reason the Remote tab exists. A magnet link is
sixty characters on a virtual keyboard, and a `.torrent` file cannot be opened by
the console at all — the two things you actually have on a phone are exactly the
two things the console was worst at receiving.

**Open the Remote tab and point your camera at the QR code.** iOS offers to open
the page; there is nothing to install, no account, no third-party service. The
address is also printed in full for anyone who would rather type it.

The page is served by the console itself and offers:

- **a `.torrent` file picker** — the iOS picker reaches Files and iCloud Drive, so
  anything Safari saved with *Download Linked File* is one tap away. Several files
  at once are fine;
- **a magnet field**, one link per line, so a list prepared on the phone goes over
  in one go;
- **the live list of what the console is doing**, with pause, resume and remove.

Uploaded `.torrent` files are copied to `sdmc:/torfoil/inbox` so you keep what you
sent. Everything that arrives this way is announced on the console screen too:
otherwise you cannot tell a successful upload from a silent failure without
walking over to check.

### What it is not

The server carries **names of content, never content**. A `.torrent` file and a
magnet link describe what to fetch; the fetching itself still goes through
`net::Transport` and nothing else. The killswitch is untouched — see
`include/net/http_server.hpp`.

Two limits are enforced rather than documented: connections from outside the
private address ranges are closed without a reply, and request bodies are capped.
The whole thing switches off from the Settings tab, or with **A** in the Remote
tab.

### The other three ways in

- **A** in the Torrents tab opens a `.torrent` picker on the card. It lists
  directories and `.torrent` files and nothing else — a Switch SD card holds tens
  of thousands of files, and showing them all would bury what you came for. **A**
  enters or adds, **Y** adds every `.torrent` in the folder at once, **B** goes up
  one level and only closes at the top.
- **`sdmc:/torfoil/watch`** is scanned every five seconds. Drop a `.torrent` in
  it — by FTP, from a card reader, from anywhere — and it is added on its own,
  then moved to `inbox`. Moved, not copied: leaving it there would re-add it
  forever. A file that is rejected is renamed `.refuse`, for the same reason, and
  the reason lands in the log.
- **X** opens the system keyboard. Fine for a short link.
- **`sdmc:/torfoil/magnets.txt`**, one link per line, then **ZL** in the Torrents
  tab. Prepare the file on a PC and everything is added at once. The file is
  created empty on first launch. Accepted links are removed from it; rejected ones
  stay, with the reason as a comment, so re-importing never creates duplicates.

## Why everything is written from scratch

| Component | Why not the obvious solution |
|---|---|
| Torrent engine | `libtorrent` means Boost and 200k lines, with no devkitPro port. qBittorrent is only a GUI on top of it: there is no engine to extract. |
| VPN | No TUN device and no kernel WireGuard on Horizon. → **userspace** WireGuard over the **lwIP** stack (NO_SYS mode). |
| Crypto | devkitPro's mbedtls is built without Everest X25519. → X25519, BLAKE2s and ChaCha20-Poly1305 ported to portable C, which also makes them testable on a PC. |
| QR code | Every library is a build-system dependency for one 25×25 grid. → ~380 lines, byte mode, level M, versions 1 to 6. Stopping at 6 removes the version-information block and the two-block-group case from the spec, and still holds 106 bytes — four times what a LAN URL needs. |

## Where peers come from

Three sources, and this matters: a magnet link carries only an info_hash and
sometimes a few trackers, often dead or filtered. If trackers were the only
source, a perfectly healthy torrent would sit at zero peers.

| Source | Role |
|---|---|
| HTTP / HTTPS / UDP trackers | Fast start, when they answer |
| **DHT** (BEP 5) | Distributed Kademlia table: finds peers with no tracker at all |
| **PEX** (BEP 11) | Connected peers trade their address books |

The DHT follows the active transport: when the VPN comes up or drops, its
queries take the same path as everything else. No UDP leaking alongside the
tunnel.

## Architecture

```
        ┌───────────────────────────────────────────┐
        │  UI (SDL2 + system font)                  │
        ├───────────────────────────────────────────┤
        │  Session · Peer · PiecePicker · Tracker    │  BitTorrent engine
        ├───────────────────────────────────────────┤
        │  net::Transport            ← interface     │
        │   ├── BsdTransport      libnx sockets      │
        │   └── Tunnel            lwIP ⇄ WireGuard   │
        ├───────────────────────────────────────────┤
        │  Storage (SD)  ·  Installer (ncm/ns)       │
        └───────────────────────────────────────────┘

        ┌───────────────────────────────────────────┐
        │  HttpServer (LAN)  →  Session commands     │  phone import
        └───────────────────────────────────────────┘
```

The phone server is drawn apart on purpose: it is the one socket in the program
that does not come from `net::Transport`, because it does not go out. It listens
on the console's own Wi-Fi interface and pushes into the same command queue the
UI uses. Routing it through the tunnel would send it to Mullvad instead of to the
living room.

**The killswitch is structural.** The engine never opens a socket itself; it asks
`net::Transport` for one. In VPN mode the tunnel is the only transport ever
instantiated, and if it drops, `ready()` returns `false` and no connection can be
created. There is no cleartext path to "remember to close": it does not exist.

## Tests

The core depends on neither libnx nor the hardware, and builds with g++:

```bash
bash tests/run.sh
```

194 assertions under AddressSanitizer and UBSan: SHA-1, bencode, magnet, picker,
storage, multi-file boundaries, PFS0/NCA/CNMT parsing, plus the crypto checked
against the **RFC 7693** (BLAKE2s), **RFC 8439** (ChaCha20-Poly1305) and
**RFC 7748** (X25519) vectors, plus a full WireGuard handshake between two
instances, replay and tampering included.

Eight of them guard the translation table, and they are worth more than their
count suggests: 1,449 strings are checked for emptiness, and every key's format
specifiers are compared across all seven languages. That second check is the one
that matters — `trf()` passes the same arguments whatever the language, so a `%s`
that became `%d` in a single column reads a pointer as an integer. It would
misbehave *only* for the users of that language, which is exactly the kind of bug
nobody finds by looking. The test was verified against a deliberately sabotaged
table before being trusted.

The phone import brings 31 more. Two of them matter more than the rest:

- the QR Reed-Solomon block is checked against the published reference vector,
  which is the only part of the encoding no one can judge by eye;
- every version is checked to leave *exactly* the number of free modules the
  standard specifies. A misplaced service pattern shifts the whole bit stream by
  one module and produces a code that is perfectly plausible on screen and
  unreadable by every camera — the failure mode with no error message.

The multipart parser is tested with two files and a text field in one submission,
binary payloads with embedded zero bytes included: that is literally what an
iPhone sends when several files are picked at once.

### Testing the VPN for real

The tests above only talk to themselves. To check the tunnel against Mullvad's
real infrastructure, with no console involved:

```bash
bash tests/vpn_live.sh 1234567890123456 France
```

The country is optional. These are **the same source files** that ship inside the
`.nro` (Mullvad API, WireGuard, lwIP), rebuilt for the PC; only the socket layer
differs. The program runs the whole chain: reference public IP → login → device
registration → relay list → handshake → **a real HTTPS request through the tunnel
to `am.i.mullvad.net`** → internal DNS at 10.64.0.1 → killswitch. The question
"am I really inside the tunnel?" is answered by Mullvad, not by this code.

The test device is deleted automatically at the end (Mullvad caps accounts at 5);
`TORFOIL_KEEP_DEVICE=1` keeps it. The account number is never printed in full and
never written to disk.

Requires `sudo apt-get install -y libmbedtls-dev build-essential rsync`.

### Testing peer discovery

When a torrent sits at 0 peers, the question is: the torrent, the network, or the
code? This settles it, with no console:

```bash
bash tests/dht_live.sh dd8255ecdc7ca55fb0bbf81323d87062db1f6d1c
```

The info_hash alone is enough: the 40 characters after `btih:` in the magnet,
which is all the DHT needs. **Prefer this form from PowerShell**, as it contains
no special characters.

To pass a full magnet from PowerShell, use the form below; the `&` in the link
breaks a direct `wsl … bash script "magnet:…"` call, which then exits without a
word:

```bash
wsl -u root -e bash -lc "cd /mnt/c/path/to/torfoil && bash tests/dht_live.sh 'magnet:?xt=urn:btih:…'"
```

It queries the real DHT using the shipped code and prints the peers found, plus
counters (queries sent, replies matched, nodes returned). Reference measurement
on Big Buck Bunny: **489 peers in 55 s, with no tracker at all**.

### Testing a full download

Finding peers does not prove you can talk to them. This exercises the entire
chain: DHT, connection, handshake, metadata, piece selection, SHA-1
verification, disk writes:

```bash
bash tests/leech_live.sh my.torrent 90
```

Reference measurement: **2.44 GB in 5 minutes, 8.1 MB/s average and 10.6 MB/s
peak**, every piece SHA-1 verified, zero corruption. Running the same command
again exercises resume: the download picks up where it stopped, without
re-reading everything.

The display shows how many peers are **choking us** and how many blocks are in
flight. Those two numbers explain almost every throughput anomaly on their own: a
choking peer serves nothing, and blocks-in-flight divided by 64 gives the number
of peers actually working for you.

## Build

```bash
bash build.sh
```

Requires devkitA64 + libnx (`DEVKITPRO=/opt/devkitpro`). `build.sh` copies the
sources into the Linux filesystem before compiling, because under WSL building
directly from `/mnt/c` is very slow. Set `TORFOIL_SRC` to override the source
location.

`make` alone works too, from any shell that has the toolchain on its `PATH`, and
produces `torfoil.nro`. `make -f Makefile.diag` builds the text-console variant.

The homebrew menu icon is generated, not committed as an opaque binary:

```bash
python tools/make_icon.py   # écrit icon.jpg, repris automatiquement par le Makefile
```

## Known constraints

- **Console sleeps → download stops.** `appletSetAutoSleepDisabled` is forced
  during transfers, so the screen must stay on.
- **Remote access needs the same network.** The console and the phone must be on
  the same Wi-Fi; the server answers private addresses only. Guest networks and
  "client isolation" on the router block it, and there is nothing the console can
  do about that. Sleep also takes the server down with the rest of the network,
  and no transfer is running to keep the console awake — so keep the Remote tab in
  view while you send.
- **Plain HTTP on the LAN.** No certificate can be issued for `192.168.x.y`, so
  the page is unencrypted. Consequence worth knowing: Safari treats it as an
  insecure context and the clipboard API is unavailable, which is why the page has
  no *Paste* button — long-press and paste into the field works normally.
- **Applet mode gives ~448 MB of heap.** The engine streams (16 KB blocks, SHA-1
  in 64 KB chunks). An NSP forwarder grants full RAM and much better throughput.
- **Mullvad dropped port forwarding** in 2023: downloads are normal, seeding is
  weak (no incoming connections).
- **NSZ/XCZ cannot be installed** yet: decompress them to `.nsp`/`.xci` first.
  They appear greyed out in the library.
- **FAT32 SD cards** are handled. Files over 4 GB are created as Horizon
  *concatenation files*, the mechanism built for exactly this. No need to
  reformat to exFAT.
- **Mullvad allows 5 devices.** The app registers one and reuses it (key stored
  in `vpn.cfg`). Reformatting the card creates a new one; at the sixth, remove
  one at mullvad.net.
- **China / Russia**: bare WireGuard is recognisable by DPI. The transport layer
  is designed to host udp2tcp, but that is not wired up yet.
- The WireGuard private key is stored in cleartext in `sdmc:/torfoil/vpn.cfg`, as
  the console offers homebrew no keystore. If the card is lost, revoke the device
  from the Mullvad site.

## Verification status

| Component | Status |
|---|---|
| Crypto, bencode, magnet, picker, storage, PFS0, NCA, CNMT | 194 tests under ASan/UBSan |
| QR encoding, HTTP and multipart parsing | Covered by those tests; **the remote import as a whole has not been run on hardware** |
| Translation table, all 7 languages | 1,449 strings checked empty-free and format-consistent |
| Console language detection, settings redesign | Compiles and builds; **not yet seen on hardware** |
| Multi-file torrents, piece boundary mid-file | Proven: exact bytes on both sides |
| Peer discovery (DHT) | Proven on PC: 489 peers, no tracker |
| Full download | Proven on PC: 2.44 GB at 8.1 MB/s, SHA-1 verified |
| Throughput on console | 3 MB/s without the VPN, measured on hardware |
| Resume after shutdown | Proven on PC: resumes without re-reading everything |
| Startup, UI, keyboard, `magnets.txt` | Verified on console |
| `torfoil.nro` and `torfoil-diag.nro` build | Both link clean with devkitA64, no warnings |
| `.torrent` picker, watch folder, details panel | Compiled and reviewed; **not yet run on hardware** |
| libnx sockets | Verified on console |
| Mullvad login, relays, WireGuard tunnel | Verified on console, tunnel up |
| Files > 4 GB (FAT32) | Verified on console: a 23 GB and a 12.5 GB file written to a FAT32 card |
| Reading an NSP: encrypted NCA → CNMT → contents | Proven on PC with a forged, encrypted package |
| Package verification (**Y**) | Proven: a single flipped bit or a truncated file is caught |
| `ncm` writes to system memory | Not exercised here, **checkable via the self-test (Y)** |
| A >4 GB file on a real card | Not exercised here, **checkable via the self-test (Y)** |
| Traffic actually leaving through the VPN | Not exercised here, **checkable via the self-test (Y)** |

Every build in the table above was run on real hardware, and each fix moved the
number: 70 kB/s, then 200 kB/s over the VPN, then 800 kB/s without it, and
**3 MB/s** once the receive window went from 16 kB to 64 kB. That last jump is
almost exactly the factor of four the window ratio predicts.

Two things are still open. Throughput over the VPN on this build has not been
measured, and 3 MB/s on a 60 Mbps line leaves headroom: the next candidates are
the socket pool settings (`sb_efficiency`, `num_bsd_sessions`) and the cost of
software ChaCha20-Poly1305 on every tunnelled packet.

### Fixed by reading the code

These were found by inspection, not by a crash. Each one is the kind that makes
the program merely *look* wrong, which is why none of them had been reported.

| Symptom | Actual cause |
|---|---|
| **`make` failing after everything compiled and linked** | `ROMFS := romfs` pointed at a directory that does not exist and never did — the program embeds no assets on purpose. Every source built, the ELF linked, and then `build_romfs` failed on the very last step with "Failed to open .../romfs!". |
| No way to add a `.torrent` at all | The console could parse them — `add_torrent_file` existed and worked — but nothing in the interface ever called it. The one format you actually download from a browser had no door. |
| French that read like a translation of English | *Choke* had been rendered literally as *étrangler* throughout, which is not what a French torrent client says and reads as violence rather than protocol. "vous serez étranglé plus souvent" became "les autres vous serviront moins"; "%u pairs (%u refusent)" became "(%u bloquent)"; "Vérification passée" — ambiguous between *passed* and *skipped* — became "Vérification ignorée". Plus a plain grammar error, "du essaim" for "de l'essaim". |
| Settings that looked like a terminal | `[x]` and `[ ]` in a monospace column, one flat list running off the bottom of the screen. Now: sliding switches whose position reads at a glance, section headers, and card rows whose background changes with focus while nothing moves. |
| The whole library greyed out as "in progress" | The match was on the torrent's save path — which is the shared download folder, identical for every torrent. One unfinished download therefore claimed every file on the card, including finished, installable packages. Now matched on the torrent's own file or folder. |
| **B** silently skipping a verification that mattered | The "skip check" flag was cleared only when a scan ended. Pressing **B** outside any verification left it armed forever, and it fired on the next torrent that legitimately needed re-reading. Cleared on entry now. |
| A dead tracker staying dead for 30 minutes | A failed announce was rescheduled 30 minutes out, like a successful one. And the interval the tracker asks for was parsed, then ignored. Failure now retries in 5 minutes; success follows the tracker's own interval. |
| Trackers seeing a brand-new client every half hour | Every announce carried `event=started`, and `completed` was never sent at all. Each is now sent once, when it means something. |
| "Torrent added" when nothing was added | A duplicate is dropped by the engine, but the call still reported success — so `magnets.txt` import counted it, and the phone page would too. Both entry points now check first. |
| Losing your place in a list by glancing at another tab | One scroll position shared by every tab, reset on each switch. One per tab now. |
| Self-test results invisible | The Settings tab stacked everything down one column and ran past the bottom of the screen at the fifth checkbox. The diagnostic output — the thing you press **Y** for — fell off the edge. Two columns now. |
| Long errors unreadable | The toast sized itself to its text with no ceiling, so the longest messages, the ones you need most, ran off both edges of the screen. |
| A torrent gone by accident | **ZR** removed immediately, with no confirmation and no way to delete the files. It now asks, and offers both outcomes. |
| CPU burnt in the piece picker | Choosing the next piece scanned all 17 000 pieces of a 35 GB torrent, per peer, per tick — a million iterations to pick a number a 256-piece sample gives just as well. And the pending-block list was a vector scanned end to end on every 16 KB block received. |

### Fixed after testing on hardware

| Symptom | Actual cause |
|---|---|
| "SD write failed" | A Switch game exceeds 4 GB, and such a file cannot exist on a FAT32 card. Switched to Horizon *concatenation files*, which present a split directory as a single file. |
| "SD write failed", again | The fix above only applied to missing files. Ordinary files left by the previous version stayed ordinary (still capped at 4 GB) and opening them reported nothing. The code now tests whether an existing file can reach its final size, and recreates it otherwise. |
| Library working "halfway" | In-progress downloads showed up as installable. They are now greyed out with their progress, and the recursive scan no longer stalls the display every 10 s. |
| One file accepted, the other "not an NSP" | A torrent downloads its pieces out of order: a file can reach its final size before its header has arrived. The library now reads the first bytes and says so, instead of blaming the file. |
| Install freezing the whole app | It was a modal dialog with no way to switch tabs. It is now a banner: installation runs in the background like a download, **B** cancels. |
| A game permanently unreadable | Torrents were not resumed on restart: the download simply stopped, and the file stayed forever without its header. The magnet is now kept and the torrent restarted automatically. |
| Only three peers with the VPN on | The tunnel's lwIP stack had 96 buffers shared across every connection. The ceiling was not the number of peers found but that pool: raised to 2048, with 96 TCP connections available. |
| Ridiculous throughput | A block that was never served stayed counted as "in flight" forever: the peer looked saturated and was never sent another request. Peers died one by one. |
| Few peers | The network driver allocates a shared pool; 32 KB of receive buffer per socket exhausted it after about thirty connections, with no error reported. |
| Slow SD writes | Every piece was read back from the card to be verified. It is now assembled in memory, hashed there, then written in one go. |
| Throughput collapsing after two minutes | We waited indefinitely on peers that were choking us. After 40 s without a byte, they are dropped in favour of others, worth 1 to 8 MB/s. |
| Endless verification at startup | Files are preallocated to their final size, so "the file exists" no longer proved anything: 35 GB were re-hashed on launch. The resume point is now written empty at creation, and **B** skips a legitimate re-scan. |
| Peers capped at 3–5 | The socket ceiling was a one-way ratchet armed by an `errno` that was not its own; lwIP never sets `errno`, so a stale value from an unrelated UDP send was read as "out of sockets". |
| ~130 KB/s per peer | A TCP connection cannot exceed its receive window divided by the round-trip time. At 16 KB and ~125 ms, that is exactly 131 KB/s. Raised to 64 KB, the libnx default. |
| Window collapsing on every completed piece | SHA-1 over 2 MiB plus the SD write ran on the network loop: for tens of milliseconds nobody read the sockets, they overflowed, and TCP read those losses as congestion. Piece completion now runs on a dedicated thread behind a bounded queue. |

### Before installing: verify

**Y** in the library replays the whole installation chain *without writing
anything*: keys, container, meta NCA decryption, CNMT, presence and size of each
content, then a full SHA-256 recomputation of each. An incomplete or damaged
package is caught before anything touches system memory; and if an install then
fails despite a successful verification, the problem is in the installation, not
in the package.

### Diagnostic log

`sdmc:/torfoil/torfoil.log` records startup, service status, storage errors, VPN
activity, phone imports and installs. This is the file to attach when something goes wrong on
hardware.

A write failure now appears with its full context: system cause, target file,
piece, absolute offset:

```
[ 12345 ms] ÉCHEC écriture : écriture SD : fichier trop gros pour ce système de
            fichiers (carte en FAT32 ? la limite y est de 4 Go) — sdmc:/torfoil/
            downloads/FC26/data.nsp | pièce 2048, décalage 0, 16384 octets,
            position absolue 4294967296
```

Without that context, "write failed" points at nothing: it is what sent us
looking at a full SD card while the real problem was FAT32's size limit.

### Self-test: what only the console can settle

Three things cannot be checked anywhere else: that an SD card accepts a file over
4 GB, that the installation services agree to write, and that traffic really
leaves through the tunnel. Rather than leave them in the dark, the app tests
itself: **Settings tab, Y**.

| Check | What it actually does |
|---|---|
| SD card | Creates a split file, writes 64 KB **at 4 GB + 4 KB**, reads it back, compares, deletes |
| Installation | Reserves an `ncm` placeholder, writes 128 KB, checks the size, deletes |
| VPN | Queries `am.i.mullvad.net` through the active transport, so Mullvad answers, not us |

Nothing is installed, nothing is kept, and every result also goes to the log. A
failure arrives with its system error code, which is usually enough to identify
the cause with no further round trip.

**`torfoil-diag.nro`** does the same in a text console, without SDL or system
fonts. Use it when the main application will not start: it isolates the engine
from everything display-related. Results land in `sdmc:/torfoil/diagnostic.log`.

### Why not an emulator

Tested, and the answer was unambiguous: on the development machine, yuzu crashes
(0xC0000005) on a **twelve-line hello world** just as it does on Torfoil. The log
shows the NRO loaded, then the emulator synthesising font archives, then the
exit, before any guest code runs. No homebrew runs at all, so no emulator can
validate anything here. That is what motivated the built-in self-test.

## Prior art

[nxTransmission](https://github.com/t-flo/nxTransmission) is a port of
Transmission 2.94 to the Switch, abandoned in 2020. It is a daemon driven from a
browser on another device, with no on-console UI and no installer. Its socket
tuning was a useful reference.

## Licence

MIT, see [LICENSE](LICENSE). Bundled lwIP is BSD-3-Clause.

## Disclaimer

This tool provides no content. It downloads what you give it.
