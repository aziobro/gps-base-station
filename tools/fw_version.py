#!/usr/bin/env python3
"""Print the firmware version embedded in an ESP-IDF application image.

ESP-IDF places an ``esp_app_desc_t`` structure at the start of the first
application segment. Its layout begins with a magic word (0xABCD5432) followed
by ``secure_version`` (u32), 8 reserved bytes, then a 32-byte ``version``
string. That ``version`` is exactly what ``esp_app_get_description()->version``
returns at runtime and what the device serves at ``/status``.

Locating the struct by scanning for the magic word keeps this independent of
header sizes and segment offsets, so it works for both the app image
(gps_base_station.bin) and the merged factory image.

Usage:
    tools/fw_version.py build/gps_base_station.bin
"""
import sys

APP_DESC_MAGIC = b"\x32\x54\xcd\xab"  # 0xABCD5432, little-endian
VERSION_OFFSET = 16  # magic(4) + secure_version(4) + reserved(8)
VERSION_LEN = 32


def extract_version(path: str) -> str:
    with open(path, "rb") as handle:
        # The descriptor lives in the first segment; 64 KiB is ample.
        blob = handle.read(65536)
    index = blob.find(APP_DESC_MAGIC)
    if index < 0:
        raise ValueError(f"no esp_app_desc_t magic word found in {path}")
    start = index + VERSION_OFFSET
    raw = blob[start:start + VERSION_LEN]
    return raw.split(b"\x00", 1)[0].decode("ascii", "replace")


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write("usage: fw_version.py <app-image.bin>\n")
        return 2
    try:
        print(extract_version(sys.argv[1]))
    except (OSError, ValueError) as error:
        sys.stderr.write(f"error: {error}\n")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
