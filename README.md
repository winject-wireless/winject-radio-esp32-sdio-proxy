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
components/winject_proxy/    UDP 22 / 9000 <-> SDIO; HTTP GET /, POST /update/c6, /update/p4, /c6/download, /c6/reset
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

Then `PRIV_REQUIRES winject_proxy` and call `winject_proxy_start(&cfg)` with `WINJECT_PROXY_CONFIG_DEFAULT()` once the network is up. `POST /update/p4` needs a two-slot partition table with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` (see `app/`). To build against local checkouts, pass `-DFETCHCONTENT_SOURCE_DIR_WINJECT_SDIO_PROXY=<dir>` and `-DFETCHCONTENT_SOURCE_DIR_WINJECT_RADIO_ESP32C6=<dir>`.

## Standalone image

Native ESP-IDF v5.5 (`. ~/esp/esp-idf-v5.5.5/export.sh`), Waveshare ESP32-P4-WIFI6-DEV-KIT (P4 USB-UART on the Type-C "UART" port):

```bash
cd app && idf.py set-target esp32p4 build && idf.py -p /dev/ttyACM0 flash
```

The P4 resets the C6 at boot, gets an address by DHCP (hostname `winject-esp32c6`, printed as `WINJECT_P4_IP ip=…`) and serves the radio on UDP 22 / 9000. Point winject-manager at it like an ESP32 radio (`winject.device = <P4 IP>`, `console = 22`, `dplane_port = 9000`, `radio_fcs = signal`). Counters print every 10 s (`WINJECT_P4_STATS`).

Firmware updates over HTTP (raw image as the body); each chip keeps its previous image until the new one proves itself:

```bash
curl --data-binary @winject-esp32c6.bin http://<P4 IP>/update/c6        # kept once the C6 answers the P4 (untested on hardware)
curl --data-binary @app/build/winject-p4-proxy.bin http://<P4 IP>/update/p4   # kept once reached (e.g. curl http://<P4 IP>/)
```

To reflash the C6 over its UART (first flash, or no bootable image), the P4 resets it into its ROM downloader by holding its GPIO9 strap low (P4 GPIO45) through the CHIP_PU pulse:

```bash
curl -X POST http://<P4 IP>/c6/download
python -m esptool --chip esp32c6 -p <C6 UART> --before no_reset --after no_reset write_flash @flash_args
curl -X POST http://<P4 IP>/c6/reset      # normal boot, link up again
```

GPIO45 is in the P4's GPIO39–48 IO domain, powered by the on-chip LDO_VO4; `sdio_link_host` enables it at 3.3 V and drives GPIO45 high, otherwise the strap reads low and the C6 boots into the downloader on every reset. A silent C6 is otherwise reset only after 30 s (at most 3 times), so it can also be reflashed over its UART while the P4 runs.

## Open issues

- **Proxy addressing:** m-plane replies go to the source of the most recent request (one m-plane client at a time), and any 1–23 byte datagram on UDP 9000 re-targets where received frames go. Both match the Ethernet radio, but the proxy accepts them from anywhere on the LAN.
- **Network settings:** DHCP only; the address cannot be set over the m-plane (`network` replies `NOK ENOTSUP` on the C6) and there is no static fallback.
