# Online services

## Index

* [Files](#files)
* [WFC presets](#wfc-presets)
* [The DS's connection slot](#the-dss-connection-slot)
* [3DS on Pretendo Network](#3ds-on-pretendo-network)
* [Tests](#tests)

The DS's and the 3DS's online services through their communities' replacements, in strict ISO C23 on the host's own
sockets: the DS's WFC services and its firmware's connection slot, which `src/nds/hle/pxi.c` gives the game, and the
3DS's login on Pretendo Network, which `src/3ds/services/frd.c` and `soc.c` make. Every call that can fail returns a
negative value and writes why into the caller's buffer, naming the value it was given.

## Files

| File | What it holds |
| --- | --- |
| `wfc.h`, `wfc.c` | the WFC services' presets, the choice of one (`off`, `auto`, `custom` or a preset) and the DS firmware's connection slot |
| `dns.h`, `dns.c`, `resolve.c` | a DNS query and its reply, and the resolver that asks one server over UDP |
| `socket.h`, `socket.c` | the host's UDP sockets the resolver uses (Winsock 2 on Windows) |
| `nasc.h`, `nasc.c` | the console's base64, NASC's LOGIN form and its reply, NEX's DateTime |
| `ctr.h`, `ctr.c` | the 3DS console's files in Azahar's NAND layout, Nimbus's rewrites, `GetGameAuthenticationData`'s bytes |
| `hosts.h`, `hosts.c` | the console maker's service domains |

## WFC presets

A DS reaches its Wi-Fi Connection service by name: it asks its DNS server for `nas.nintendowifi.net`,
`conntest.nintendowifi.net` and the GameSpy names under `gs.nintendowifi.net`. A replacement service runs a DNS server
that answers those names with its own servers and every other name as any resolver does, so a player sets that server
as the DS's DNS and nothing else. `wfc.c` holds the list, in the order issue #2307 gives it.

| id            | Service       | DNS server       | `nas.nintendowifi.net` | `conntest.nintendowifi.net` |
|---------------|---------------|------------------|------------------------|-----------------------------|
| `kaeru`       | Kaeru WFC     | 178.62.43.212    | 178.62.43.212          | 198.62.122.140              |
| `wiilink`     | WiiLink       | 167.235.229.36   | 178.62.43.212          | 167.235.229.36              |
| `wiimmfi`     | Wiimmfi       | 95.217.77.181    | 78.46.231.155          | 188.34.177.217              |
| `wiilink_wfc` | WiiLink WFC   | 5.161.56.11      | 5.161.56.11            | 5.161.56.11                 |
| `altwfc`      | AltWFC        | 172.104.88.237   | 172.104.88.237         | 172.104.88.237              |
| `newwfc`      | NewWFC        | 89.117.58.143    | 89.117.58.143          | 89.117.58.143               |
| `skytemple`   | SkyTemple WFC | 91.107.226.151   | 91.107.226.151         | 91.107.226.151              |
| `diyfi`       | DIYFI         | 3.22.166.69      | 3.22.166.69            | 3.22.166.69                 |
| `sudomemo`    | Sudomemo      | 104.248.0.110    | 104.248.0.110          | 44.229.101.156              |

The two right columns are what each server answered `tests/online_presets_live.c` (`SNX_TEST_ONLINE_LIVE=ON`, then
`ctest -L network`), the resolver here asking each one. WiiLink's server sends the DS names to Kaeru's NAS; Kaeru's
and Wiimmfi's send the GameSpy names to Wiimmfi's servers.

SkyTemple's server puts the EDNS record of a query that carries one in its answer section, which `dig` reports as a
malformed message; a DS sends no EDNS, and neither does `snx_online_dns_query`, so it answers as the others do.

A service is added when its DNS server answers both names with a server of its own; one that stops answering them
leaves the list in the same change that `online_presets_live` shows failing. Addresses a service used before and left
are not listed. The launcher's menu keeps the ids: `nds_board` checks settings.txt's `nds_wfc` against
`snx_online_wfc_preset_at`.

`auto` asks every service's server for `nas.nintendowifi.net` at once and takes the one whose address came back first
(`snx_online_wfc_fastest`), within a time the caller gives; the DS driver says each server's time and the one taken.
It weighs time alone: a service that serves only some titles (Sudomemo serves one DSi application) can be the fastest
one. `custom` takes any IPv4 address but `0.0.0.0` (the DS's "from the network") and `255.255.255.255`; `off` leaves
the DS on the DNS its network gives it.

## The DS's connection slot

A DS game that goes online reads the firmware's three Wi-Fi connection slots, connects through the first that works
and asks the DNS server the slot names (its DHCP's when the slot says `0.0.0.0`). The DS driver gives the game one
configured slot whose DNS servers are the chosen service's, at the firmware's user settings offset less
`SNX_ONLINE_NDS_WFC_BEFORE_USER` (`0x3FA00` in a 256 KiB flash); `snx_online_nds_wfc_slot_empty` fills the other two.
Both DNS fields carry the service's server: a second server that is a public resolver would answer the service's names
with the console maker's own addresses, which no longer serve.

GBATEK's "DS Firmware Wifi Internet Access Points", the fields `snx_online_nds_wfc_slot` writes:

| Offset | Size | Field                                      | Written                    |
|--------|------|--------------------------------------------|----------------------------|
| 0x40   | 32   | SSID                                       | the driver's network       |
| 0xC0   | 4    | IP address (0: DHCP)                       | 0                          |
| 0xC8   | 4    | primary DNS                                | the service's server       |
| 0xCC   | 4    | secondary DNS                              | the service's server       |
| 0xE6   | 1    | WEP mode                                   | 0, open                    |
| 0xE7   | 1    | status (0 normal, 1 AOSS, 0xFF not set)    | 0                          |
| 0xE8   | 1    | SSID length                                | its length                 |
| 0xEF   | 1    | set by the connection setup                | 1                          |
| 0xF0   | 6    | the WFC user ID the NAS gives              | 0: the NAS assigns one     |
| 0xFE   | 2    | CRC-16 of 0x00 to 0xFD, from 0             | computed                   |

A game writes the slot back once the NAS gives the console its WFC ID; a firmware kept between runs keeps that ID. The
game's data then leaves through the DS's wireless, which the driver does not have yet: the ARM7's wireless manager
joining the slot's network and carrying the game's frames, and a network behind it (DHCP, ARP, the game's TCP and UDP
to the host's sockets).

## 3DS on Pretendo Network

[Nimbus](https://github.com/PretendoNetwork/Nimbus) patches the console's system modules, and with a Pretendo account
selected each module goes to Pretendo:

| Module  | Nimbus's patch                                           | Here                             |
|---------|----------------------------------------------------------|----------------------------------|
| friends | NASC at `https://nasc.pretendo.cc/ac/`                   | `SNX_ONLINE_PRETENDO_NASC_URL`   |
| act     | the account server at `https://account.pretendo.cc/v1/api/` | `SNX_ONLINE_PRETENDO_ACCOUNT_URL` |
| http    | the first `nintendowifi.net`, `nintendo.net` and `pokemon-gl.com` of a URL become `pretendo.cc` | `snx_online_pretendo_url` |
| socket  | `nncs1`/`nncs2.app.nintendowifi.net`, the NAT check, become `.app.pretendo.cc` | `snx_online_pretendo_host` |
| ssl     | the console takes Pretendo's certificates                | the host's TLS (the hosts have publicly trusted certificates) |

Nimbus makes the Pretendo account the friends module's local account 2, beside the console's first.

`snx_online_maker_host` names the console maker's service domains (`nintendo.net`, `nintendowifi.net` and the rest,
the list the Switch driver keeps too). `soc:U` resolves those names to no address, after Nimbus's rewrites: the game
hears the name does not resolve, as on a console that cannot reach it, and nothing goes to the maker's servers.

A game that goes online asks the friends module (`frd:u` `RequestGameAuthentication`, command 0x28) to log in to its
game server: the module posts a LOGIN form to NASC and keeps the reply, which the game reads with
`GetGameAuthenticationData` (0x29): the game server's address and port and a token. The game then talks to that server
itself, over the socket service, with the token.

Pretendo's NASC (its account server's `src/middleware/nasc.ts` and `src/services/nasc/routes/ac.ts`) takes a LOGIN only
from a console it knows: `fcdcert` a LocalFriendCodeSeed_B whose signature is the console maker's, `csnum` the serial
the device was made with, `macadr` 12 hex digits under a console vendor's prefix, and `userid` with `uidhmac` the
Pretendo account's principal ID and the code its server gave it. Its replies: `returncd` 001 with `locator`, `token`
and `datetime`; an error's code otherwise (102 an unknown account or device, 110 no server for the title, 121 a
certificate it refuses, 122 a wrong `uidhmac`, 150 a serial that is not the device's, 152 another game server ID).

The console's files come from a folder laid out as Azahar's NAND folder (`ctr_console`). Azahar's Artic Setup Tool
copies the first two from the player's console; the third, the friends save with the Pretendo account, it does not
copy ("Artic Setup Tool does not copy your account or save data from your system", Pretendo's guide for Azahar): Nimbus
makes it when it runs in Azahar:

| File                                                                    | What the login takes                 |
|-------------------------------------------------------------------------|--------------------------------------|
| `rw/sys/LocalFriendCodeSeed_B` (0x110 bytes)                            | the whole file, `fcdcert`            |
| `rw/sys/SecureInfo_A` (0x111 bytes)                                     | the serial at 0x102, the region at 0x100 |
| `data/00000000000000000000000000000000/sysdata/00010032/00000000/2/account` | the principal ID at 0x14, the local friend code at 0x18, the uidhmac at 0x42 (UTF-16, 8 letters) |

The account file's offsets are 3dbrew's FRD Savegame page's and Nimbus's repair, which writes the uidhmac at 0x42; no
account file of a real console has been read by the tests, which build one from those offsets. The Wi-Fi address
(`macadr`, `bssid`) is not in those files: `snx_online_ctr_mac` keeps one per seed under the vendor prefix 40:F4:07.
Pretendo checks the prefix and keeps the device by its seed, not by its address.

No LOGIN has gone to Pretendo's servers from the tests: that takes a player's own console files, and the tests use
none. `fpdver` (the friends module's version) and `gamecd` (the product code's last four letters) are sent as the
fields' descriptions in Pretendo's code give them; its NASC reads neither.

## Tests

| Test | What it checks |
| --- | --- |
| `online_wfc` | the presets' ids and addresses, the choice's errors, IPv4 parsing, the CRC, the DS slot |
| `online_dns` | a query's exact bytes, replies read and refused (another id, cut short, a loop, codes) |
| `online_resolve` | the resolver against a DNS server on 127.0.0.1: an address, no such name, a stray reply passed over, no reply in time; `auto` among a fast server, a slow one and a silent one |
| `online_nasc` | the console's base64, the form, Pretendo's replies (LOGIN, error codes, `null`), NEX's DateTime |
| `online_ctr` | Nimbus's rewrites, the console's files in Azahar's layout, the LOGIN form read back, the game's authentication data |
| `online_presets_live` | each WFC service's DNS server over the internet, and `auto` among them; only with `SNX_TEST_ONLINE_LIVE=ON` (`ctest -L network`), how a service that stopped answering is found |
