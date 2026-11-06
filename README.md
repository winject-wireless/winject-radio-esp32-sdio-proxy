# winject-radio-esp32-sdio-proxy

ESP32-P4 side of [winject-radio-esp32c6](https://github.com/winject-wireless/winject-radio-esp32c6): it drives the C6 radio over SDIO and exposes it on the P4's network as an ordinary winject radio, m-plane on UDP 22 and d-plane on UDP 9000. winject-manager (winject-l3) talks to it as it does to an Ethernet radio, over the LAN or from the same P4.

```
 winject-manager ──UDP 22 / 9000──▶ winject_proxy ──SDIO (plane + len header)──▶ C6 radio ─▶ air
                 ◀─────────────────    (P4 lwIP)   ◀──────────────────────────────           ◀─ air
```

The SDIO message format, bring-up and proxy behaviour are specified in winject-radio-esp32c6's [docs/sdio-link.md](https://github.com/winject-wireless/winject-radio-esp32c6/blob/main/docs/sdio-link.md). The encoder/parser (`sdio_msg`) stays in that repo, next to the C6 firmware, and is fetched from there so both ends build from one copy.

## Layout

```
components/sdio_link_host/   SDIO host link: resets the C6 (CHIP_PU, boot strap), receive task, send, link recovery
components/winject_proxy/    UDP 22 / 9000 <-> SDIO; starts udp_ota
components/udp_ota/          UDP 3232: P4 app and C6 firmware update, C6 reset / ROM downloader, config files
tools/udp_ota.py             udp_ota client (Python stdlib only)
test/run.sh                  Host tests (mplane routing, udp_ota core) and udp_ota_host_server
cmake/winject_sdio_proxy.cmake  Fetches sdio_msg; sets WINJECT_SDIO_PROXY_COMPONENT_DIRS
app/                         Standalone proxy image: Ethernet (IP101, DHCP) + winject_proxy
```

## Use in an ESP-IDF project

```cmake
cmake_minimum_required(VERSION 3.16)
include(FetchContent)
FetchContent_Declare(winject_sdio_proxy
    GIT_REPOSITORY https://github.com/winject-wireless/winject-radio-esp32-sdio-proxy.git
    GIT_TAG <commit>)
FetchContent_GetProperties(winject_sdio_proxy)
if(NOT winject_sdio_proxy_POPULATED)
    FetchContent_Populate(winject_sdio_proxy)
endif()
include(${winject_sdio_proxy_SOURCE_DIR}/cmake/winject_sdio_proxy.cmake)
list(APPEND EXTRA_COMPONENT_DIRS ${WINJECT_SDIO_PROXY_COMPONENT_DIRS})
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
```

Then `PRIV_REQUIRES winject_proxy` and call `winject_proxy_start(&cfg)` with `WINJECT_PROXY_CONFIG_DEFAULT()` once the network is up.

### Queue d-plane

When winject-l3 runs on the same P4, the d-plane can skip lwIP: set `cfg.dplane_tx_queue`, `cfg.dplane_rx_queue` and `cfg.dplane_reactor` and the proxy uses two in-process bfc queues instead of UDP 9000 (the m-plane stays on UDP 22). The host registers both `bfc::reactive_circular_lossy_queue<bfc::shared_sized_buffer>` in `bfc::cv_reactor_circular_lossy_queue_registry`, a `bfc::cv_reactor<>` in `bfc::cv_reactor_registry`, runs the reactor, then starts the proxy and winject-l3 with the same names (`radio.dplane_tx`, `radio.dplane_rx`, `radio.reactor`). The proxy consumes `dplane_tx_queue` (inject MPDUs and registrations, sent to the C6 by the d-plane task, never on the reactor thread) and pushes every C6 frame to `dplane_rx_queue`. Each queue's producer counts what a full queue evicts: the proxy as `dplane_up_evicted` (`dup_evict`), winject-l3 as `radio_tx_drops`. The binding is for the life of the program. bfc is pinned in `cmake/BfcFetch.cmake`; FetchContent keeps the first declaration, so keep it equal to winject-l3's and vstreamer's. P4 updates over udp_ota need a two-slot partition table with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` (see `app/`). To build against local checkouts, pass `-DFETCHCONTENT_SOURCE_DIR_WINJECT_SDIO_PROXY=<dir>` and `-DFETCHCONTENT_SOURCE_DIR_WINJECT_RADIO_ESP32C6=<dir>`.

## Standalone image

Native ESP-IDF v5.5 (`. ~/esp/esp-idf-v5.5.5/export.sh`), Waveshare ESP32-P4-WIFI6-DEV-KIT (P4 USB-UART on the Type-C "UART" port):

```bash
cd app && idf.py set-target esp32p4 build && idf.py -p /dev/ttyACM0 flash
```

The P4 resets the C6 at boot, gets an address by DHCP (hostname `winject-esp32c6`, printed as `WINJECT_P4_IP ip=…`) and serves the radio on UDP 22 / 9000. Point winject-manager at it like an ESP32 radio (`winject.device = <P4 IP>`, `console = 22`, `dplane_port = 9000`, `radio_fcs = signal`). Counters print every 10 s (`WINJECT_P4_STATS`).

## Firmware updates (udp_ota)

There is no HTTP server. `components/udp_ota` serves UDP 3232 (`ota_port`; 0 = off), one request per datagram with every reply to its sender, so it works over Ethernet and, through an L3 relay upstream, over the air (rov-esp32p4 upstream 9, ground UDP 2430). Each chip keeps its previous image until the new one proves itself:

```bash
tools/udp_ota.py <P4 IP>:3232 status                                      # P4 image, C6 update state, link
tools/udp_ota.py <P4 IP>:3232 flash app/build/winject-p4-proxy.bin --reboot   # kept once reached over the network
tools/udp_ota.py <P4 IP>:3232 flash-c6 winject-esp32c6.bin                # kept once the C6 answers the P4
tools/udp_ota.py <P4 IP>:3232 fs-get winject.conf | fs-put <file> <name>  # with ota_fs_dir set
```

- **P4 app**: 1024-byte fragments in any order, retransmitted by the tool, written straight into the inactive slot (each 4 KB sector erased on its first write). The completing fragment's reply carries the verdict (`esp_ota_end`, and the project name must match the running app). `--reboot` boots it and waits for `state=valid`, then checks again 10 s later.
- **C6**: the same transfer with target 1 is staged raw in the P4's inactive slot, so air loss never leaves the C6 half-written. Once the header checks out (ESP32-C6 app), a task writes it to the C6 over the SDIO OTA plane (1400 B chunks in order, `BEGIN` / `DATA` / `END`), resets the C6 and checks the new image answers; `status` shows `c6=writing c6_written=…`, then `c6=ok` or `c6=failed c6_err=…`. `on_ota_active` is held true meanwhile (e.g. to pause video). The C6 confirms its new image on its first link message; one that never answers is rolled back by a second reset. Not while a freshly updated P4 image is unconfirmed (its rollback image is in that slot).
- **Files**: `ota_fs_*` read and write one file at a time in `ota_fs_dir` (see `components/udp_ota/core/wire.hpp`).

Wire and status codes: [`components/udp_ota/core/wire.hpp`](components/udp_ota/core/wire.hpp). Host tests and a lossy host server: `test/run.sh`, then `build_test/udp_ota_host_server 47232 /tmp/out.bin 20 10` and point the tool at `127.0.0.1:47232`.

To reflash the C6 over its UART (first flash, no bootable image, or its bootloader), the P4 resets it into its ROM downloader by holding its GPIO9 strap low (P4 GPIO45) through the CHIP_PU pulse:

```bash
tools/udp_ota.py <P4 IP>:3232 c6-download
python -m esptool --chip esp32c6 -p <C6 UART> --before no_reset --after no_reset write_flash @flash_args
tools/udp_ota.py <P4 IP>:3232 c6-reset      # normal boot, link up again
```

The strap pin depends on the board (`sdio_link_host_config_t`):

| Board | C6 `GPIO9` | `boot_gpio_ldo` | C6 `U0RXD` / `U0TXD` |
|---|---|---|---|
| Guition JC-ESP32P4-M3-DEV | GPIO45 (JP1 pin 24 jumper), the default | 4 (LDO_VO4) | GPIO32 / GPIO33 (JP1 20 / 22 jumpers) |
| Waveshare ESP32-P4-WIFI6-DEV-KIT | GPIO22 | -1 | GPIO33 / GPIO32 |

GPIO45 is in the P4's GPIO39–48 IO domain, powered by the on-chip LDO_VO4; `sdio_link_host` enables it at 3.3 V and drives GPIO45 high, otherwise the strap reads low and the C6 boots into the downloader on every reset. A silent C6 is otherwise reset only after 30 s (at most 3 times), so it can also be reflashed over its UART while the P4 runs.

## Open issues

- **Proxy addressing:** any 1–23 byte datagram on UDP 9000 re-targets where received frames go. This matches the Ethernet radio, but the proxy accepts it from anywhere on the LAN, as it does m-plane requests.

## m-plane clients

Several clients can use UDP 22 at once (for example winject-l3 on the same P4 and an operator over an L3 upstream). The C6 runs one request line at a time and tags each reply line with the request's `cmd:<u8>`, so the proxy routes by tag ([`mplane_route.c`](components/winject_proxy/mplane_route.c)): it gives every command line an unused id (`cmd:<id> `, 1–255), remembers the sender and the sender's own tag, and on each `OK:<id>` / `NOK:<id>` reply line puts that tag back and sends the line to that sender. An id stays reserved until the ordered C6 reply stream advances to the next id; a request is dropped rather than overwrite an outstanding route if all ids are busy. Clients that tag get their own ids back unchanged. Untagged clients get untagged replies, but every reply line then starts with `OK` or `NOK` (the C6 decorates body lines of tagged requests that way). Blank and `#` lines and lines with a malformed `cmd:` prefix go to the C6 unchanged, and its untagged reply goes to the most recent sender. Run the host sanitizer test with [`test/run.sh`](test/run.sh).
- **Network settings:** DHCP only; the address cannot be set over the m-plane (`network` replies `NOK ENOTSUP` on the C6) and there is no static fallback.
