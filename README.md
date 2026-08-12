# Torfoil

A native BitTorrent client for the Nintendo Switch (Atmosphère), with a built-in
**Mullvad WireGuard tunnel**.

## Torfoil does not install anything

Torfoil is a downloader, and only that. It never reads Nintendo keys, never
decrypts game content, and never writes to system storage. Its job ends when the
file is on the SD card.

To install content you are legally entitled to (your own dumps, homebrew), use a
separate title manager such as [DBI](https://github.com/rashevskyv/dbi).

The interface speaks **German, English, Spanish, French, Japanese, Russian and
Chinese**. The first launch asks which one; it can be changed at any time from
the first row of Settings. The non-Latin translations are best effort and have
not been proofread by native speakers, so corrections are welcome.

> Code comments and log messages remain in French. They go into `torfoil.log`
> and serve bug reports rather than daily use.

## Setup

Copy `torfoil.nro` to `sdmc:/switch/` and launch it from the homebrew menu.
`torfoil-diag.nro` is optional: it is the text-console diagnostic build, useful
when the main application refuses to start.

## Usage

| Tab | Controls |
|---|---|
| Torrents | **↑↓** select · **A** actions on the torrent under the cursor · **X** paste a magnet · **Y** search · **ZL** import `magnets.txt` · **ZR** orphan files |
| Downloads | What is running right now, then the queue in order |
| Phone | **A** open/close the access point · **Y** once the phone has joined |
| VPN | **X** Mullvad account (16 digits) · **A** connect/disconnect · **Y** country |
| Settings | **↑↓** select · **A** toggle · **←→** change value · **Y** self-test |

The **Torrents** tab is a dense list, one row per torrent: an icon for the kind
of content, the name, the state, and the percentage on the right. Nothing else
is shown there — everything else is one **A** away.

| Action | What it does |
|---|---|
| Location | Browses the torrent's own files, and only those. Split files (a folder of `00`, `01`… slices) appear as the single file they are. |
| Extract archive | Only for a finished `.zip`. Unpacks it next to the archive, in a folder named after it, in the background. |
| Information | Pieces held and rejected, piece length, file count, peers, blocks in flight, rates, ETA, trackers, private flag, hash, path. |
| Pause / Resume | Drops the peers, keeps the resume point. |
| Remove | Keeping the files, or deleting them — the second asks for confirmation and names the torrent. |

The **Downloads** tab shows what is actually moving: the running torrents as
large cards with rate, ETA and peers, then the numbered queue below.

Torfoil runs **two torrents at a time** by default. The rest wait in the queue
and start on their own as slots free up. `max_active` in the Settings tab sets
the limit from 1 to 8, or `0` for no limit at all.

Active torrents **resume automatically on startup**: each magnet link is kept in
`sdmc:/torfoil/downloads/.<hash>.magnet`. Without this, closing the application
silently abandoned the download: the files stayed on the card, half finished,
and nothing ever picked them up again.

### Searching

**Y** on the Torrents tab opens a search box; results are listed with size,
seeder count and source, sorted by seeders, and **A** starts the download.

No source ships with the application. `sdmc:/torfoil/search.json` is created
empty on first launch and it is up to you to declare what gets queried:

```json
{
  "providers": [
    {
      "name": "My indexer",
      "kind": "torznab",
      "url": "http://192.168.1.10:9117/api/v2.0/indexers/all/results/torznab/api",
      "api_key": "...",
      "enabled": true
    }
  ]
}
```

`kind` is either `torznab`, the protocol every indexer manager speaks (Jackett,
Prowlarr), or `json` for any API returning a list, in which case `list_key`,
`name_key`, `size_key`, `seeders_key`, `magnet_key` and `hash_key` say where to
read each field. Searches go through the active transport, so they ride the
tunnel when the VPN is up.

### Driving it from a computer

Turn on **Remote access** in the Settings tab. The console then serves a page on
the network it is already connected to, so downloads keep running while you use
it, and the Settings tab shows the address and the key to type in.

The page lists the torrents with their progress, rate, peers and ETA, refreshes
every two seconds, and can add a magnet or a `.torrent`, pause, resume and
remove. Only private addresses are served, and every API call needs the key, so
a neighbour on the same Wi-Fi cannot delete your files.

### Extracting archives

A finished `.zip` gets an **Extract archive** entry in its action menu. The
files are written next to the archive, in a folder named after it, while a
progress panel shows where it is; **B** puts that panel away and the extraction
carries on. Entries that try to write outside the destination are refused, and
outputs over 4 GB use the same concatenation-file trick as downloads. Only ZIP
is supported: RAR and 7z need libraries heavier than the rest of the
application.

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

Downloads land in `sdmc:/torfoil/downloads`.

### Importing from a phone

The console raises **its own Wi-Fi network** and shows a QR code, the same way
the Album's "send to smartphone" does. Point the phone camera at it and the
phone joins; a second QR code carries the address of a page served by the
console, where you paste magnet links or upload `.torrent` files. Nothing to
type, nothing to install, and no router to convince: this works at a friend's
place, in a hotel, and on networks that isolate their clients from each other.

The service behind it is `lp2p`, the one the Album itself uses. Not `ldn`: that
is console-to-console local play and speaks Nintendo frames no phone will ever
understand. `lp2p` creates a real infrastructure-mode access point with DHCP,
and since firmware 11.0.0 it accepts standard WPA2-PSK.

Two things worth knowing. The console has one radio, so while the access point
is up it has no internet and downloads stop; that is true of the Album too, and
the tab says so before you open it. And `member_count_max` must be **1** in
WPA2 mode: at four, `lp2pCreateGroup` refuses the network with 2231-0261, an
error nobody has documented. The probe in `torfoil-diag.nro` found it by running
the whole matrix and changing one variable at a time.

### Adding magnet links

Typing a magnet on a virtual keyboard is painful, so there are three other ways
in: the phone import above, and the three below.

- **X** opens the system keyboard. Fine for a short link, or if you can paste it
  from the console browser.
- **`sdmc:/torfoil/magnets.txt`**, one link per line, then **ZL** in the Torrents
  tab. This is the normal route: prepare the file on a PC (SD card in a reader,
  or over FTP with `ftpd`/sys-ftpd to keep the console running) and everything is
  added at once. The file is created empty on first launch. Accepted links are
  removed from it; rejected ones stay, with the reason as a comment, so
  re-importing never creates duplicates.
- **`sdmc:/torfoil/inbox`**, for `.torrent` files. Drop them in the folder and
  they are picked up at launch, and again on every **ZL**. A file that is
  accepted is deleted from the inbox, so the folder is also the list of what has
  not been taken yet.

## Why everything is written from scratch

| Component | Why not the obvious solution |
|---|---|
| Torrent engine | `libtorrent` means Boost and 200k lines, with no devkitPro port. qBittorrent is only a GUI on top of it: there is no engine to extract. |
| VPN | No TUN device and no kernel WireGuard on Horizon. → **userspace** WireGuard over the **lwIP** stack (NO_SYS mode). |
| Crypto | devkitPro's mbedtls is built without Everest X25519. → X25519, BLAKE2s and ChaCha20-Poly1305 ported to portable C, which also makes them testable on a PC. |

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
        │  Storage (SD)                              │
        └───────────────────────────────────────────┘
```

**The killswitch is structural.** The engine never opens a socket itself; it asks
`net::Transport` for one. In VPN mode the tunnel is the only transport ever
instantiated, and if it drops, `ready()` returns `false` and no connection can be
created. There is no cleartext path to "remember to close": it does not exist.

## Tests

The core depends on neither libnx nor the hardware, and builds with g++:

```bash
bash tests/run.sh
```

157 assertions under AddressSanitizer and UBSan: SHA-1, bencode, magnet, picker,
storage, multi-file boundaries, the QR encoder against published Reed-Solomon
vectors, the HTTP and multipart parsers, the seven translation columns, plus the
crypto checked
against the **RFC 7693** (BLAKE2s), **RFC 8439** (ChaCha20-Poly1305) and
**RFC 7748** (X25519) vectors, plus a full WireGuard handshake between two
instances, replay and tampering included.

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

### Running it on a PC

```bash
bash host/run.sh --script "R,R,shot,quit"
```

This builds and runs **the real program** (same engine, same UI, same source
files) on a development machine. It is not a mock: `host/include/switch.h`
fakes the handful of Horizon calls the program makes and sits first on the
include path, so no application file needs an `#ifdef`. The sockets were already
plain POSIX and the display was already plain SDL2.

Without `--script` it opens a window and the keyboard acts as the pad. With one,
no display is needed at all: the actions are played in order and `shot` saves the
screen to `host-shots/`, which is how the screens in this project get checked
without a console in hand. Not testable there: the lp2p access point,
concatenation files, and the console socket pool sizing.

Needs `libsdl2-dev libsdl2-ttf-dev libmbedtls-dev fonts-dejavu-core`.

## Known constraints

- **Console sleeps → download stops.** `appletSetAutoSleepDisabled` is forced
  during transfers, so the screen must stay on.
- **Applet mode gives ~448 MB of heap.** The engine streams (16 KB blocks, SHA-1
  in 64 KB chunks). Launching from a game title grants full RAM and much better
  throughput.
- **Mullvad dropped port forwarding** in 2023: downloads are normal, seeding is
  weak (no incoming connections).
- **BitTorrent v2 (BEP 52) is not supported.** The engine is v1: SHA-1 pieces, no
  merkle tree. Hybrid torrents work through their v1 half; v2-only ones do not.
  Almost everything in circulation is still v1 or hybrid.
- **Everything in a torrent is downloaded.** There is no per-file selection.
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
| Crypto, bencode, magnet, picker, storage, QR, HTTP, translations | 157 tests under ASan/UBSan |
| Multi-file torrents, piece boundary mid-file | Proven: exact bytes on both sides |
| Peer discovery (DHT) | Proven on PC: 489 peers, no tracker |
| Full download | Proven on PC: 2.44 GB at 8.1 MB/s, SHA-1 verified |
| Throughput on console | 3 MB/s without the VPN, measured on hardware |
| Resume after shutdown | Proven on PC: resumes without re-reading everything |
| Startup, UI, keyboard, `magnets.txt` | Verified on console |
| libnx sockets | Verified on console |
| Mullvad login, relays, WireGuard tunnel | Verified on console, tunnel up |
| Files > 4 GB (FAT32) | Verified on console: a 23 GB and a 12.5 GB file written to a FAT32 card |
| A >4 GB file on a real card | Not exercised here, **checkable via the self-test (Y)** |
| Traffic actually leaving through the VPN | Not exercised here, **checkable via the self-test (Y)** |
| Wi-Fi access point and the two QR codes | Verified on console: network created at 192.168.0.1, phone joined, page served, torrents received |
| Interface in seven languages | Table verified by test; rendering verified on the PC build |

Every build in the table above was run on real hardware, and each fix moved the
number: 70 kB/s, then 200 kB/s over the VPN, then 800 kB/s without it, and
**3 MB/s** once the receive window went from 16 kB to 64 kB. That last jump is
almost exactly the factor of four the window ratio predicts.

Two things are still open. Throughput over the VPN on this build has not been
measured, and 3 MB/s on a 60 Mbps line leaves headroom: the next candidates are
the socket pool settings (`sb_efficiency`, `num_bsd_sessions`) and the cost of
software ChaCha20-Poly1305 on every tunnelled packet.

### Fixed after testing on hardware

| Symptom | Actual cause |
|---|---|
| "SD write failed" | A large download exceeds 4 GB, and such a file cannot exist on a FAT32 card. Switched to Horizon *concatenation files*, which present a split directory as a single file. |
| "SD write failed", again | The fix above only applied to missing files. Ordinary files left by the previous version stayed ordinary (still capped at 4 GB) and opening them reported nothing. The code now tests whether an existing file can reach its final size, and recreates it otherwise. |
| A flat file list nobody could use | The Library tab listed every file on the card side by side, with no way to tell which torrent had written what. It is gone: the file view now opens from a torrent and is scoped to that torrent's content, and what no torrent claims is counted in a single footer line. |
| Library working "halfway" | In-progress downloads looked finished. They are now greyed out with their progress, and the recursive scan no longer stalls the display every 10 s. |
| A download permanently stuck | Torrents were not resumed on restart: the download simply stopped, and the file stayed forever incomplete. The magnet is now kept and the torrent restarted automatically. |
| Only three peers with the VPN on | The tunnel's lwIP stack had 96 buffers shared across every connection. The ceiling was not the number of peers found but that pool: raised to 2048, with 96 TCP connections available. |
| Ridiculous throughput | A block that was never served stayed counted as "in flight" forever: the peer looked saturated and was never sent another request. Peers died one by one. |
| Few peers | The network driver allocates a shared pool; 32 KB of receive buffer per socket exhausted it after about thirty connections, with no error reported. |
| Slow SD writes | Every piece was read back from the card to be verified. It is now assembled in memory, hashed there, then written in one go. |
| Throughput collapsing after two minutes | We waited indefinitely on peers that were choking us. After 40 s without a byte, they are dropped in favour of others, worth 1 to 8 MB/s. |
| Endless verification at startup | Files are preallocated to their final size, so "the file exists" no longer proved anything: 35 GB were re-hashed on launch. The resume point is now written empty at creation, and **B** skips a legitimate re-scan. |
| Peers capped at 3–5 | The socket ceiling was a one-way ratchet armed by an `errno` that was not its own; lwIP never sets `errno`, so a stale value from an unrelated UDP send was read as "out of sockets". |
| ~130 KB/s per peer | A TCP connection cannot exceed its receive window divided by the round-trip time. At 16 KB and ~125 ms, that is exactly 131 KB/s. Raised to 64 KB, the libnx default. |
| Window collapsing on every completed piece | SHA-1 over 2 MiB plus the SD write ran on the network loop: for tens of milliseconds nobody read the sockets, they overflowed, and TCP read those losses as congestion. Piece completion now runs on a dedicated thread behind a bounded queue. |

### Diagnostic log

`sdmc:/torfoil/torfoil.log` records startup, service status, storage errors and
VPN activity. This is the file to attach when something goes wrong on hardware.

A write failure now appears with its full context: system cause, target file,
piece, absolute offset:

```
[ 12345 ms] ÉCHEC écriture : écriture SD : fichier trop gros pour ce système de
            fichiers (carte en FAT32 ? la limite y est de 4 Go) — sdmc:/torfoil/
            downloads/big.iso | pièce 2048, décalage 0, 16384 octets,
            position absolue 4294967296
```

Without that context, "write failed" points at nothing: it is what sent us
looking at a full SD card while the real problem was FAT32's size limit.

### Self-test: what only the console can settle

Two things cannot be checked anywhere else: that an SD card accepts a file over
4 GB, and that traffic really leaves through the tunnel. Rather than leave them
in the dark, the app tests itself: **Settings tab, Y**.

| Check | What it actually does |
|---|---|
| SD card | Creates a split file, writes 64 KB **at 4 GB + 4 KB**, reads it back, compares, deletes |
| VPN | Queries `am.i.mullvad.net` through the active transport, so Mullvad answers, not us |

Nothing is kept, and every result also goes to the log. A
failure arrives with its system error code, which is usually enough to identify
the cause with no further round trip.

**`torfoil-diag.nro`** does the same in a text console, without SDL or system
fonts. Use it when the main application will not start: it isolates the engine
from everything display-related. Results land in `sdmc:/torfoil/diagnostic.log`.

### Testing on an emulator

Torfoil runs unmodified in a headless Switch emulator, and the whole run is
scripted: a fresh SD card is seeded with five `.torrent` files and their data,
the app is driven by a sequence of button presses, screenshots are taken, and
the test then reads back what the guest wrote to that card (log, settings,
resume points, emptied inbox). One command, about a minute, exit code 0 or 1.

```powershell
powershell -File tools\emu\Test-TorfoilEmu.ps1
```

Firmware and keys must be dumped from your own console. Setup, the layout
independent input handling, the two emulator patches needed for the GPU free
Linux variant, and the known limitations are documented in
[tools/emu/README.md](tools/emu/README.md).

An earlier attempt with yuzu had gone nowhere: it crashed (0xC0000005) on a
twelve-line hello world before any guest code ran. That dead end is what
motivated the built-in self-test, which remains the fastest check on real
hardware.

## Prior art

[nxTransmission](https://github.com/t-flo/nxTransmission) is a port of
Transmission 2.94 to the Switch, abandoned in 2020. It is a daemon driven from a
browser on another device, with no on-console UI. Its socket tuning was a useful
reference.

## Licence

MIT, see [LICENSE](LICENSE). Bundled lwIP is BSD-3-Clause.

## Disclaimer

This tool provides no content. It downloads what you give it.
