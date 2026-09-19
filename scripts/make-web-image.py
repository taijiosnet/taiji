#!/usr/bin/env python3
import re
import sys
import gzip
import os
import zlib
from pathlib import Path


ROOT_PAYLOAD_FILES = (
    "386/bin/rill",
    "386/bin/ktrem",
    "386/bin/shelf",
    "386/bin/explorer",
    "386/bin/inbe",
    "386/bin/pass",
    "386/bin/rio9",
    "386/bin/linuxrun",
    "386/bin/xbridge",
    "386/bin/q9taskmgr",
    "386/bin/rio",
    "lib/controlpanel/Display",
    "lib/controlpanel/Themes",
    "sys/src/inbe/assets/app/icon.png",
    "sys/src/pass/assets/app/icon.png",
    "lib/rill/applications",
    "lib/rill/associations",
    "lib/rill/icon.map",
    "lib/rill/panel",
    "386/lib/ape/libap.a",
    "386/lib/ape/libbsd.a",
    "rc/bin/ape/linuxcc",
    "rc/bin/ape/taiji-posix-sh",
    "rc/bin/q9hw",
    "sys/include/ape/poll.h",
    "sys/include/ape/sys/epoll.h",
    "sys/include/ape/sys/inotify.h",
    "sys/include/ape/sys/ioctl.h",
    "sys/include/ape/sys/mman.h",
    "sys/include/ape/sys/poll.h",
    "sys/include/ape/sys/types.h",
    "sys/include/ape/termios.h",
    "sys/include/ape/time.h",
    "sys/src/ape/README.taiji-posix",
    "sys/src/ape/compat-smoke.c",
    "sys/src/ape/lib/ap/plan9/clock_gettime.c",
    "sys/src/ape/lib/ap/plan9/epoll.c",
    "sys/src/ape/lib/ap/plan9/inotify.c",
    "sys/src/ape/lib/ap/plan9/mkfile",
    "sys/src/ape/lib/ap/plan9/mman.c",
    "sys/src/ape/lib/ap/plan9/nanosleep.c",
    "sys/src/ape/lib/ap/plan9/poll.c",
    "sys/src/ape/lib/ap/plan9/tcgetattr.c",
    "sys/src/ape/lib/bsd/ioctl.c",
    "usr/glenda/lib/profile",
    "usr/glenda/bin/rc/startwm",
    "usr/glenda/readme.rill",
    "lib/kryon/system-theme",
    "usr/glenda/lib/kryon/theme",
    "usr/glenda/lib/wallpaper",
    "rc/bin/debian-session",
    "rc/bin/linux-app",
    "rc/bin/rill-open",
    "doc/unified-desktop.md",
    "rc/bin/deb/install",
    "debian/rootfs/etc/debian_version",
    "debian/rootfs/etc/group",
    "debian/rootfs/etc/os-release",
    "debian/rootfs/etc/passwd",
    "debian/rootfs/root/README",
    "debian/rootfs/usr/bin/dpkg",
    "debian/rootfs/var/lib/dpkg/status",
)

ROOT_PAYLOAD_DIRS = (
    "386/lib/ape",
    "debian",
    "debian/rootfs",
    "debian/rootfs/etc",
    "debian/rootfs/root",
    "debian/rootfs/usr",
    "debian/rootfs/usr/bin",
    "debian/rootfs/var",
    "debian/rootfs/var/lib",
    "debian/rootfs/var/lib/dpkg",
    "lib/kryon",
    "lib/rill",
    "lib/controlpanel",
    "rc/bin/ape",
    "rc/bin/deb",
    "sys/include/ape/sys",
    "sys/src/ape/lib",
    "sys/src/ape/lib/ap",
    "sys/src/ape/lib/ap/plan9",
    "sys/src/ape/lib/bsd",
    "sys/src/inbe",
    "sys/src/inbe/assets",
    "sys/src/inbe/assets/app",
    "sys/src/pass",
    "sys/src/pass/assets",
    "sys/src/pass/assets/app",
    "usr/glenda/lib/kryon",
)

BLZ_RAW_CHUNK = 32 * 1024


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: make-web-image.py input.raw plan9.ini output.raw", file=sys.stderr)
        return 2

    src = Path(sys.argv[1])
    ini = Path(sys.argv[2])
    dst = Path(sys.argv[3])
    image = bytearray(src.read_bytes())
    marker = b"[menu]\n"
    start = image.find(marker)
    if start < 0:
        print(f"{src}: embedded plan9.ini marker not found", file=sys.stderr)
        return 1

    end = image.find(b"\0", start)
    if end < 0:
        print(f"{src}: embedded plan9.ini terminator not found", file=sys.stderr)
        return 1

    old_len = end - start
    replacement = ini.read_bytes()
    if not replacement.endswith(b"\n"):
        replacement += b"\n"
    if len(replacement) > old_len:
        print(
            f"{ini}: {len(replacement)} bytes does not fit existing {old_len}-byte plan9.ini slot",
            file=sys.stderr,
        )
        return 1

    image[start : start + old_len] = replacement + b"\0" * (old_len - len(replacement))
    if not expand_partition(image):
        return 1
    if not patch_kernel_payload(image):
        return 1
    if not patch_root_payload(image):
        return 1
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(image)
    return 0


def patch_root_payload(image: bytearray) -> bool:
    marker = b"gzfilesystem "
    start = image.find(marker)
    if start < 0:
        print("embedded gzfilesystem marker not found", file=sys.stderr)
        return False

    line_end = image.find(b"\n", start)
    if line_end < 0:
        print("embedded gzfilesystem header is unterminated", file=sys.stderr)
        return False

    try:
        old_size = int(image[start + len(marker) : line_end])
    except ValueError:
        print("embedded gzfilesystem size is invalid", file=sys.stderr)
        return False

    payload_start = line_end + 1
    payload_limit = partition_end(image)
    if payload_limit <= payload_start:
        print("embedded gzfilesystem payload starts outside the Plan 9 partition", file=sys.stderr)
        return False
    payload_end = payload_start + old_size
    compressed = bytes(image[payload_start:payload_end])
    try:
        payload = gzip.decompress(compressed)
    except gzip.BadGzipFile as exc:
        print(f"could not decompress embedded root payload: {exc}", file=sys.stderr)
        return False

    replacements = {
        b"\tip/ipconfig": b"\t#ipconfig  ",
    }
    for old, new in replacements.items():
        if len(old) != len(new):
            print(f"internal replacement length mismatch for {old!r}", file=sys.stderr)
            return False
        count = payload.count(old)
        if count == 0:
            print(f"embedded root payload did not contain {old!r}", file=sys.stderr)
            return False
        payload = payload.replace(old, new)

    try:
        payload = append_root_payload(payload)
    except ValueError as exc:
        print(f"could not append embedded root payload: {exc}", file=sys.stderr)
        return False

    recompressed = gzip.compress(payload, compresslevel=9, mtime=0)
    new_size = len(recompressed)
    header = b"gzfilesystem " + str(new_size).encode("ascii") + b"\n"
    needed = start + len(header) + new_size
    if needed > payload_limit:
        # The preinstalled apps outgrew the base image: extend the raw
        # image (whole megabytes) and re-expand the partition to match.
        grow = needed - payload_limit
        grow += 1024 * 1024 - grow % (1024 * 1024)
        image += b"\0" * grow
        if not expand_partition(image):
            return False
        payload_limit = partition_end(image)
        if needed > payload_limit:
            print(
                f"patched root payload of {new_size} bytes still does not fit the partition",
                file=sys.stderr,
            )
            return False

    image[start:payload_limit] = header + recompressed + b"\0" * (
        payload_limit - start - len(header) - new_size
    )
    return True


def expand_partition(image: bytearray) -> bool:
    if len(image) < 512 or image[510:512] != b"\x55\xaa":
        print("raw image does not contain an MBR signature", file=sys.stderr)
        return False

    entry = 446
    start_lba = int.from_bytes(image[entry + 8 : entry + 12], "little")
    if start_lba <= 0:
        print("raw image has an invalid Plan 9 partition start", file=sys.stderr)
        return False

    sectors = len(image) // 512 - start_lba
    if sectors <= 0:
        print("raw image is too small for its Plan 9 partition", file=sys.stderr)
        return False

    image[entry + 5 : entry + 8] = b"\xfe\xff\xff"
    image[entry + 12 : entry + 16] = sectors.to_bytes(4, "little")

    # Rewrite the in-partition map line wholesale: the sector count can
    # gain digits, so the replacement is allowed to change length as long
    # as the map sector is rebuilt and NUL-padded to 512 bytes.
    match = re.search(
        rb"part gzroot 4096 \d+\n",
        bytes(image[start_lba * 512 : (start_lba + 4096) * 512]),
    )
    if match is None:
        print("raw image Plan 9 gzroot partition map entry not found", file=sys.stderr)
        return False
    map_pos = start_lba * 512 + match.start()
    sector_base = map_pos - map_pos % 512
    relative = map_pos - sector_base
    sector = bytearray(image[sector_base : sector_base + 512])
    sector[relative : relative + len(match.group(0))] = f"part gzroot 4096 {sectors}\n".encode("ascii")
    image[sector_base : sector_base + 512] = sector
    return True


def partition_end(image: bytes | bytearray) -> int:
    entry = 446
    start_lba = int.from_bytes(image[entry + 8 : entry + 12], "little")
    sectors = int.from_bytes(image[entry + 12 : entry + 16], "little")
    return min(len(image), (start_lba + sectors) * 512)


def append_root_payload(blz: bytes) -> bytes:
    length, descriptors = parse_bflz(blz)
    archive = unbflz_from_descriptors(length, descriptors)
    terminator = find_mkfs_terminator(archive)
    additions = make_mkfs_records()

    new_descriptors = []
    produced = 0
    for descriptor in descriptors:
        if produced >= terminator:
            break
        kind, size, data_or_offset = descriptor
        take = min(size, terminator - produced)
        if take <= 0:
            break
        if kind == "raw":
            new_descriptors.append(("raw", take, data_or_offset[:take]))
        else:
            new_descriptors.append(("ref", take, data_or_offset))
        produced += take

    if produced != terminator:
        raise ValueError("could not truncate bflz stream at mkfs terminator")

    new_descriptors.append(("raw", len(additions), additions))
    return write_bflz(terminator + len(additions), new_descriptors)


def make_mkfs_records() -> bytes:
    out = bytearray()
    for path in ROOT_PAYLOAD_DIRS:
        if not Path(path).exists():
            continue
        out += f"/{path} 20000000755 rsc staff 4294967295 0\n".encode("ascii")
    for path in ROOT_PAYLOAD_FILES:
        src = Path(path)
        if not src.exists():
            print(f"skipping optional embedded root payload: {src} does not exist", file=sys.stderr)
            continue
        data = src.read_bytes()
        mode = "775" if os.access(src, os.X_OK) else "664"
        out += f"/{path} {mode} rsc staff 4294967295 {len(data)}\n".encode("ascii")
        out += data
    out += b"end of archive\n"
    return bytes(out)


def find_mkfs_terminator(archive: bytes) -> int:
    pos = 0
    while True:
        line_end = archive.find(b"\n", pos)
        if line_end < 0:
            raise ValueError("mkfs archive terminator not found")
        line = archive[pos:line_end]
        if line == b"end of archive":
            return pos
        fields = line.split()
        if len(fields) != 6:
            raise ValueError(f"bad mkfs header at byte {pos}")
        try:
            size = int(fields[5])
        except ValueError as exc:
            raise ValueError(f"bad mkfs size at byte {pos}") from exc
        pos = line_end + 1 + size


def parse_bflz(blob: bytes) -> tuple[int, list[tuple[str, int, bytes | int]]]:
    if not blob.startswith(b"BLZ\n"):
        raise ValueError("embedded root is not a BLZ payload")

    length = read_be32(blob, 4)
    pos = 8
    total = 0
    descriptors: list[tuple[str, int, bytes | int]] = []
    while total < length:
        block = read_be32(blob, pos)
        pos += 4
        if block & 0x80000000:
            size = block & 0x7FFFFFFF
            descriptors.append(("raw", size, b""))
        else:
            size = block
            offset = read_be32(blob, pos)
            pos += 4
            descriptors.append(("ref", size, offset))
        total += size
    if total != length:
        raise ValueError("bad BLZ descriptor lengths")

    raw_pos = pos
    hydrated = []
    for kind, size, data_or_offset in descriptors:
        if kind == "raw":
            raw = blob[raw_pos : raw_pos + size]
            if len(raw) != size:
                raise ValueError("short BLZ raw data")
            hydrated.append((kind, size, raw))
            raw_pos += size
        else:
            hydrated.append((kind, size, data_or_offset))
    return length, hydrated


def unbflz_from_descriptors(length: int, descriptors: list[tuple[str, int, bytes | int]]) -> bytes:
    out = bytearray()
    for kind, size, data_or_offset in descriptors:
        if kind == "raw":
            out += data_or_offset
        else:
            offset = int(data_or_offset)
            for i in range(size):
                out.append(out[offset + i])
    if len(out) != length:
        raise ValueError("bad BLZ expansion length")
    return bytes(out)


def write_bflz(length: int, descriptors: list[tuple[str, int, bytes | int]]) -> bytes:
    out = bytearray(b"BLZ\n")
    out += write_be32(length)
    raw = bytearray()
    total = 0
    for kind, size, data_or_offset in descriptors:
        if kind == "raw":
            data = bytes(data_or_offset)
            if len(data) != size:
                raise ValueError("bad BLZ raw output length")
            for offset in range(0, size, BLZ_RAW_CHUNK):
                chunk = data[offset : offset + BLZ_RAW_CHUNK]
                out += write_be32(0x80000000 | len(chunk))
                raw += chunk
                total += len(chunk)
        else:
            out += write_be32(size)
            out += write_be32(int(data_or_offset))
            total += size
    if total != length:
        raise ValueError("bad BLZ output length")
    return bytes(out + raw)


def read_be32(data: bytes, offset: int) -> int:
    if offset + 4 > len(data):
        raise ValueError("short BLZ integer")
    return int.from_bytes(data[offset : offset + 4], "big")


def write_be32(value: int) -> bytes:
    return value.to_bytes(4, "big")


def patch_kernel_payload(image: bytearray) -> bool:
    gzip_magic = b"\x1f\x8b\x08"
    root_marker = b"gzfilesystem "
    kernel_start = image.find(gzip_magic)
    root_start = image.find(root_marker)
    if kernel_start < 0 or root_start < 0 or kernel_start >= root_start:
        print("could not locate embedded kernel gzip stream", file=sys.stderr)
        return False

    kernel_region = bytes(image[kernel_start:root_start])
    decompressor = zlib.decompressobj(16 + zlib.MAX_WBITS)
    try:
        payload = bytearray(decompressor.decompress(kernel_region))
    except zlib.error as exc:
        print(f"could not decompress embedded kernel payload: {exc}", file=sys.stderr)
        return False
    if not decompressor.eof:
        print("embedded kernel gzip stream did not terminate", file=sys.stderr)
        return False
    old_size = len(kernel_region) - len(decompressor.unused_data)
    compressed = kernel_region[:old_size]

    old = b"can't open /srv/usb"
    new = b"web boot has no usb"
    if len(old) != len(new):
        print("internal kernel replacement length mismatch", file=sys.stderr)
        return False
    if payload.count(old) == 0:
        print("embedded kernel payload did not contain USB warning string", file=sys.stderr)
        return False
    payload[:] = payload.replace(old, new)

    recompressed = gzip.compress(bytes(payload), compresslevel=9, mtime=0)
    if len(recompressed) > old_size:
        print(
            f"patched kernel payload grew from {old_size} to {len(recompressed)} bytes",
            file=sys.stderr,
        )
        return False

    image[kernel_start : kernel_start + old_size] = recompressed + b"\0" * (old_size - len(recompressed))
    return True


if __name__ == "__main__":
    raise SystemExit(main())
