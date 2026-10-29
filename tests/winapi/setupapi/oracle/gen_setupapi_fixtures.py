#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-only
"""Write the cabinet the SetupAPI INF probe and tests use.

Usage: gen_setupapi_fixtures.py <out-dir>

probe.cab: one uncompressed folder holding incab1.txt and incab2.txt with a
fixed DOS date/time (2010-02-04 09:04:00) and the archive attribute, so the
SPFILENOTIFY_FILEINCABINET fields in setupapi_inf_probe*.expected are
deterministic. flag.cab holds flagcab.txt and flag10.txt the same way
(the SourceDisksNames flags cases). Layout per [MS-CAB].
"""
import struct
import sys

FILES = [(b"incab1.txt", b"first file in the cabinet\r\n"),
         (b"incab2.txt", b"second file, a little longer than the first\r\n")]
DOS_DATE = ((2010 - 1980) << 9) | (2 << 5) | 4
DOS_TIME = (9 << 11) | (4 << 5)


def make_cab(files=None):
    files = FILES if files is None else files
    payload = b"".join(data for _, data in files)
    header = 36
    folder_at = header
    files_at = folder_at + 8
    entries = b""
    offset = 0
    for name, data in files:
        entries += struct.pack("<IIHHHH", len(data), offset, 0, DOS_DATE, DOS_TIME, 0x20) + name + b"\0"
        offset += len(data)
    data_at = files_at + len(entries)
    total = data_at + 8 + len(payload)
    cab = struct.pack("<4sIIIIIBBHHHHH", b"MSCF", 0, total, 0, files_at, 0, 3, 1, 1, len(files), 0, 0x5150, 0)
    cab += struct.pack("<IHH", data_at, 1, 0)
    cab += entries
    cab += struct.pack("<IHH", 0, len(payload), len(payload)) + payload
    assert len(cab) == total
    return cab


FLAG_FILES = [(b"flagcab.txt", b"flag cab file\r\n"), (b"flag10.txt", b"flag 0x10 file\r\n")]

if __name__ == "__main__":
    with open(sys.argv[1] + "/probe.cab", "wb") as f:
        f.write(make_cab())
    with open(sys.argv[1] + "/flag.cab", "wb") as f:
        f.write(make_cab(FLAG_FILES))
