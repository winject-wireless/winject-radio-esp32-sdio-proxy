#!/usr/bin/env python3
"""Update a P4 app or its C6 radio over udp_ota (components/udp_ota/core/wire.hpp).

  udp_ota.py <host:port> status
  udp_ota.py <host:port> reboot
  udp_ota.py <host:port> flash <app.bin> [--reboot] [--window N]
  udp_ota.py <host:port> flash-c6 <winject-esp32c6.bin> [--window N]
  udp_ota.py <host:port> c6-reset | c6-download
  udp_ota.py <host:port> fs-get <name> [<local file>|-]
  udp_ota.py <host:port> fs-put <local file> <name>

Rover over Ethernet: <rover IP>:3232. Rover over the air: <ground IP>:2430
(ground L3 upstream 9 -> air -> rover upstream 9 -> 127.0.0.1:3232).
flash sends the image as 1024-byte fragments with up to --window in flight
and retransmits unanswered ones; the reply to the fragment that completes the
image says whether the P4 accepted it (it rejects an image of another
project). With --reboot it then boots the image and waits until the P4 runs it
with state=valid, and checks again 10 s later (no rollback).
fs-get / fs-put read and write one file in the P4's config partition
(/winject, e.g. winject.conf); fs-put overwrites the file, truncates it to
the new length, closes it and reads it back to verify. A changed config takes
effect on the next boot (reboot). Python stdlib only.
"""

import argparse
import os
import socket
import struct
import sys
import time

FRAGMENT = 1024
START, FRAGMENT_REQ, REBOOT, STATUS = 0x01, 0x02, 0x03, 0x04
FS_OPEN, FS_CLOSE, FS_READ, FS_WRITE, FS_TRUNCATE = 0x05, 0x06, 0x07, 0x08, 0x09
C6_RESET, C6_DOWNLOAD = 0x0A, 0x0B
TARGET_P4, TARGET_C6 = 0, 1
ESP_CHIP_ID_ESP32C6 = 0x000D
RESP = 0x80
STATUS_NAMES = ["OK", "EINPROGRESS", "EINVAL", "ENOENT", "EIO", "EBADMSG", "EFBIG", "ENOSPC"]


def status_name(code):
    return STATUS_NAMES[code] if code < len(STATUS_NAMES) else "E%d" % code


class OtaError(Exception):
    pass


def parse_addr(s):
    host, _, port = s.rpartition(":")
    if not host or not port.isdigit():
        raise SystemExit("address must be host:port, got %r" % s)
    return (socket.gethostbyname(host), int(port))


def app_elf_sha(image):
    """First 8 bytes of app_elf_sha256 from the image's esp_app_desc_t, as
    esp_app_get_elf_sha256(buf, 17) prints it; None if not an app image."""
    off = 24 + 8  # image header + first segment header
    if len(image) < off + 176 or struct.unpack_from("<I", image, off)[0] != 0xABCD5432:
        return None
    return image[off + 144 : off + 152].hex()


def image_chip_id(image):
    """chip_id of an ESP app image header, or None if it is not one."""
    if len(image) < 24 or image[0] != 0xE9:
        return None
    return struct.unpack_from("<H", image, 12)[0]


def app_desc(image):
    off = 32
    if len(image) < off + 176 or struct.unpack_from("<I", image, off)[0] != 0xABCD5432:
        return None

    def cstr(a, b):
        return image[off + a : off + b].split(b"\0", 1)[0].decode(errors="replace")

    return {
        "version": cstr(16, 48),
        "project": cstr(48, 80),
        "built": "%s %s" % (cstr(96, 112), cstr(80, 96)),
    }


class Client:
    def __init__(self, addr):
        self.addr = addr
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", 0))

    def recv(self, timeout):
        self.sock.settimeout(max(timeout, 0.001))
        try:
            data, src = self.sock.recvfrom(2048)
        except socket.timeout:
            return None
        return data if src == self.addr else None

    def request(self, msg, want_type, tries=10, timeout=1.0):
        """Sends msg until a reply of want_type arrives; returns it."""
        for _ in range(tries):
            self.sock.sendto(msg, self.addr)
            deadline = time.monotonic() + timeout
            while True:
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                data = self.recv(left)
                if data and data[0] == want_type:
                    return data
        raise OtaError("no reply from %s:%d" % self.addr)

    def status(self, tries=5, timeout=1.0):
        r = self.request(bytes([STATUS]), STATUS | RESP, tries, timeout)
        if len(r) < 2 or r[1] != 0:
            raise OtaError("status: %s" % status_name(r[1] if len(r) > 1 else 255))
        return r[2:].decode(errors="replace")

    def reboot(self):
        r = self.request(bytes([REBOOT]), REBOOT | RESP, tries=5)
        if r[1] != 0:
            raise OtaError("reboot: %s" % status_name(r[1]))

    # ota_fs_*: one request at a time, each resent until its reply arrives
    # (all are idempotent). Replies are matched by type and offset.
    def _fs(self, msg, want_type, match=None, tries=20, timeout=1.0):
        for _ in range(tries):
            self.sock.sendto(msg, self.addr)
            deadline = time.monotonic() + timeout
            while True:
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                data = self.recv(left)
                if data and data[0] == want_type and (match is None or match(data)):
                    return data
        raise OtaError("no reply from %s:%d" % self.addr)

    def fs_open(self, name):
        r = self._fs(bytes([FS_OPEN]) + name.encode(), FS_OPEN | RESP)
        if len(r) < 6 or r[1] != 0:
            raise OtaError("open %s: %s" % (name, status_name(r[1] if len(r) > 1 else 255)))
        return struct.unpack_from("<I", r, 2)[0]

    def fs_close(self):
        r = self._fs(bytes([FS_CLOSE]), FS_CLOSE | RESP)
        if r[1] != 0:
            raise OtaError("close: %s" % status_name(r[1]))

    def fs_read_all(self, size):
        out = bytearray()
        while len(out) < size:
            off = len(out)
            want = min(FRAGMENT, size - off)
            r = self._fs(struct.pack("<BIH", FS_READ, off, want), FS_READ | RESP,
                         lambda d, off=off: len(d) >= 6 and struct.unpack_from("<I", d, 1)[0] == off)
            if r[5] != 0:
                raise OtaError("read at %d: %s" % (off, status_name(r[5])))
            if len(r) == 6:
                break  # the file got shorter
            out += r[6:]
        return bytes(out)

    def fs_write(self, off, data):
        r = self._fs(struct.pack("<BIH", FS_WRITE, off, len(data)) + data, FS_WRITE | RESP,
                     lambda d: len(d) >= 12 and struct.unpack_from("<I", d, 1)[0] == off)
        if r[7] != 0:
            raise OtaError("write at %d: %s" % (off, status_name(r[7])))

    def fs_truncate(self, size):
        r = self._fs(struct.pack("<BI", FS_TRUNCATE, size), FS_TRUNCATE | RESP)
        if r[1] != 0:
            raise OtaError("truncate to %d: %s" % (size, status_name(r[1])))

    def fs_get(self, name):
        size = self.fs_open(name)
        try:
            return self.fs_read_all(size)
        finally:
            self.fs_close()

    def fs_put(self, name, data, log=print):
        old = self.fs_open(name)
        try:
            # In order: a write may start at most at the current end.
            for off in range(0, len(data), FRAGMENT):
                self.fs_write(off, data[off : off + FRAGMENT])
            if old > len(data):
                self.fs_truncate(len(data))
            back = self.fs_read_all(len(data))
        finally:
            self.fs_close()
        if back != data:
            raise OtaError("%s: read-back differs from what was written" % name)
        log("%s: %d B written (was %d B), verified" % (name, len(data), old))

    def simple(self, msg_type, what):
        r = self.request(bytes([msg_type]), msg_type | RESP, tries=5)
        if len(r) < 2 or r[1] != 0:
            raise OtaError("%s: %s" % (what, status_name(r[1] if len(r) > 1 else 255)))

    def wait_c6(self, timeout_s=120, log=print):
        """After a C6 image is staged: follows the P4 writing it to the C6."""
        deadline = time.monotonic() + timeout_s
        last = None
        while time.monotonic() < deadline:
            try:
                st = self.status(tries=1, timeout=1.0)
            except OtaError:
                continue  # the P4 is busy, or the air link is down during the C6 reset
            state = status_field(st, "c6")
            shown = "%s %s" % (state, status_field(st, "c6_written") or "")
            if shown != last:
                log("  c6=%s" % shown.strip())
                last = shown
            if state == "ok":
                return st
            if state == "failed":
                raise OtaError("C6 update failed: %s" % (status_field(st, "c6_err") or st))
            time.sleep(0.5)
        raise OtaError("C6 update did not finish within %d s" % timeout_s)

    def flash(self, image, window, log=print, target=TARGET_P4):
        count = (len(image) + FRAGMENT - 1) // FRAGMENT
        if count == 0 or count > 0xFFFF:
            raise OtaError("image of %d bytes does not fit u16 fragments" % len(image))
        t0 = time.monotonic()
        # The target byte only for the C6: older servers take the 3-byte start.
        start = struct.pack("<BH", START, count) + (bytes([target]) if target != TARGET_P4 else b"")
        r = self.request(start, START | RESP, tries=30, timeout=1.0)
        if len(r) < 4 or struct.unpack_from("<H", r, 1)[0] != count:
            raise OtaError("bad start response %r" % r)
        if r[3] != 0:
            raise OtaError("start: %s" % status_name(r[3]))

        frags = [image[i * FRAGMENT : (i + 1) * FRAGMENT] for i in range(count)]
        acked = [False] * count
        sent_at = {}  # fragment -> monotonic time of its last send
        tries = [0] * count
        nxt = 0  # next never-sent fragment
        done = 0
        srtt = None
        retransmits = 0
        last_log = t0

        def send(i):
            self.sock.sendto(struct.pack("<BH", FRAGMENT_REQ, i) + frags[i], self.addr)
            sent_at[i] = time.monotonic()
            tries[i] += 1

        while done < count:
            now = time.monotonic()
            rto = max(0.2, 3 * srtt) if srtt else 1.0
            for i, t in list(sent_at.items()):
                if now - t >= rto:
                    if tries[i] >= 20:
                        raise OtaError("fragment %d: no reply after %d tries" % (i, tries[i]))
                    retransmits += 1
                    send(i)
            while nxt < count and len(sent_at) < window:
                send(nxt)
                nxt += 1
            wait = min(sent_at.values()) + rto - time.monotonic() if sent_at else 0.05
            data = self.recv(wait)
            if not data or data[0] != FRAGMENT_REQ | RESP or len(data) < 4:
                continue
            i, st = struct.unpack_from("<H", data, 1)[0], data[3]
            if i >= count or acked[i]:
                continue
            if st != 0:
                raise OtaError("fragment %d: %s" % (i, status_name(st)))
            if i in sent_at:
                if tries[i] == 1:  # Karn: time only first sends
                    rtt = time.monotonic() - sent_at[i]
                    srtt = rtt if srtt is None else 0.875 * srtt + 0.125 * rtt
                del sent_at[i]
            acked[i] = True
            done += 1
            if time.monotonic() - last_log >= 2:
                last_log = time.monotonic()
                log("  %d/%d fragments, %d retransmits" % (done, count, retransmits))
        dt = time.monotonic() - t0
        log(
            "image accepted: %d B in %.1f s (%.0f kB/s), %d fragments, %d retransmits, rtt %.0f ms"
            % (len(image), dt, len(image) / dt / 1000, count, retransmits, (srtt or 0) * 1000)
        )


def status_field(text, key):
    """Value of key=value in a status line, or None."""
    for word in text.split():
        if word.startswith(key + "="):
            return word[len(key) + 1 :]
    return None


def wait_for_image(client, elf, timeout_s, settle_s=10, log=print):
    """Waits until the P4 runs the image with this ELF SHA and has confirmed it
    (state=valid), then checks again after settle_s: about half of P4 OTAs
    used to reset at first boot and roll back, and a pending image that
    resets is rolled back by the bootloader."""
    deadline = time.monotonic() + timeout_s
    time.sleep(2)
    last = None
    while time.monotonic() < deadline:
        try:
            st = client.status(tries=1, timeout=1.0)
        except OtaError:
            continue
        if st != last:
            log("status: " + st)
            last = st
        if elf is not None and status_field(st, "elf") != elf:
            raise OtaError("P4 answers with another image (rolled back?), want elf=%s" % elf)
        if status_field(st, "state") != "valid":
            time.sleep(0.5)
            continue
        time.sleep(settle_s)
        st = client.status()
        if elf is not None and status_field(st, "elf") != elf:
            raise OtaError("image was running but the P4 rolled back: " + st)
        log("confirmed: " + st)
        return st
    raise OtaError("P4 did not confirm the image within %d s after reboot (last: %s)" % (timeout_s, last))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addr", help="host:port of the udp_ota server (or the ground relay)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    sub.add_parser("reboot")
    fl = sub.add_parser("flash")
    fl.add_argument("image")
    fl.add_argument("--window", type=int, default=8, help="fragments in flight (default 8)")
    fl.add_argument("--reboot", action="store_true", help="boot the new image and wait for it")
    fl.add_argument("--boot-timeout", type=int, default=90)
    fl.add_argument("--force", action="store_true", help="flash even if the project names differ")
    fc = sub.add_parser("flash-c6", help="update the C6 radio (staged on the P4, then written over SDIO)")
    fc.add_argument("image")
    fc.add_argument("--window", type=int, default=8, help="fragments in flight (default 8)")
    fc.add_argument("--timeout", type=int, default=120, help="seconds for the P4 to write the C6")
    sub.add_parser("c6-reset", help="reboot the C6 normally (also leaves its ROM downloader)")
    sub.add_parser("c6-download", help="hold the C6 in its ROM downloader for a UART reflash")
    fg = sub.add_parser("fs-get", help="read a file from the config partition")
    fg.add_argument("name")
    fg.add_argument("local", nargs="?", default="-", help="output file, - = stdout (default)")
    fp = sub.add_parser("fs-put", help="replace a file in the config partition")
    fp.add_argument("local")
    fp.add_argument("name")
    args = ap.parse_args()

    client = Client(parse_addr(args.addr))
    try:
        if args.cmd == "status":
            print(client.status())
        elif args.cmd == "reboot":
            client.reboot()
            print("rebooting")
        elif args.cmd == "flash-c6":
            with open(args.image, "rb") as f:
                image = f.read()
            if image_chip_id(image) != ESP_CHIP_ID_ESP32C6:
                raise OtaError("%s is not an ESP32-C6 app image" % args.image)
            print("image: C6 %d B" % len(image))
            print("before: " + client.status())
            client.flash(image, max(1, args.window), target=TARGET_C6)
            st = client.wait_c6(args.timeout)
            print("C6 updated, new image answering: " + st)
        elif args.cmd == "c6-reset":
            client.simple(C6_RESET, "c6-reset")
            print("C6 reset, link up")
        elif args.cmd == "c6-download":
            client.simple(C6_DOWNLOAD, "c6-download")
            print(
                "C6 in its ROM downloader: flash it over its UART "
                "(esptool --before no_reset --after no_reset), then c6-reset"
            )
        elif args.cmd == "fs-get":
            data = client.fs_get(args.name)
            if args.local == "-":
                sys.stdout.buffer.write(data)
            else:
                with open(args.local, "wb") as f:
                    f.write(data)
                print("%s: %d B" % (args.name, len(data)), file=sys.stderr)
        elif args.cmd == "fs-put":
            with open(args.local, "rb") as f:
                client.fs_put(args.name, f.read())
        else:
            with open(args.image, "rb") as f:
                image = f.read()
            desc = app_desc(image)
            elf = app_elf_sha(image)
            if desc:
                print(
                    "image: %s %s built %s elf=%s, %d B"
                    % (desc["project"], desc["version"], desc["built"], elf, len(image))
                )
            else:
                print("image: %d B (no esp_app_desc_t found)" % len(image))
            before = client.status()
            print("before: " + before)
            running = status_field(before, "project")
            if desc and running and desc["project"] != running and not args.force:
                raise OtaError(
                    "image is project %s but the P4 runs %s (the P4 rejects it too; --force skips this check)"
                    % (desc["project"], running)
                )
            client.flash(image, max(1, args.window))
            if args.reboot:
                client.reboot()
                print("rebooting")
                wait_for_image(client, elf, args.boot_timeout)
    except OtaError as e:
        print("ERROR " + str(e), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
