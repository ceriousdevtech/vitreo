#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-only
"""Build Authenticode-signed structured-storage (Windows Installer) fixtures
for WinVerifyTrust, and check the digest rules against real signed packages.

Usage:
  crypt_msi_sign.py <out-dir>                 write fixtures/msi/*
  crypt_msi_sign.py --check <file.msi> ...    recompute each signed file's
                                              digest; exit 1 on any mismatch

The digest rules (authenticode.hpp, storage_digest_input) were established
with --check against the messageDigest Windows wrote into real packages:
SHA-1 without \\x05MsiDigitalSignatureEx (VC++ 2010 vc_red.msi and its
msp_kb2565063.msp) and SHA-256 with it (VC++ 14.x runtime MSIs, LibreOffice
26.8 with ~130 nested storages). The fixtures below are a small MSI-shaped
compound file (CFB v3, mini and regular streams, nested and empty storages,
names whose UTF-16LE byte order differs from code-unit and case-insensitive
order) signed under a throwaway test PKI, then edited:

  msi_unsigned.msi       no \\x05DigitalSignature                  no signature
  msi_sha1.msi           SHA-1, no extended stream                  digest ok
  msi_sha256.msi         SHA-256, no extended stream                digest ok
  msi_sha1_ex.msi        SHA-1 with \\x05MsiDigitalSignatureEx       digest ok
  msi_sha256_ex.msi      SHA-256 with \\x05MsiDigitalSignatureEx     digest ok
  msi_stream_ex.msi      _ex, one byte of a nested stream flipped   bad digest
  msi_big.msi            sha256, one byte of a regular-sector stream bad digest
  msi_meta.msi           sha256, a storage's modified time changed  digest ok
  msi_meta_ex.msi        _ex, the same change                       bad digest
  msi_clsid.msi          sha256, a nested storage's CLSID changed   bad digest
  msi_ex_flip.msi        _ex, one byte of the extended stream flipped bad digest
  msi_ex_only.msi        extended stream but no \\x05DigitalSignature no signature
  msi_garbage_sig.msi    \\x05DigitalSignature that is not PKCS#7    no signature
  msi_corrupt.msi        compound-file signature, unreadable header unknown form

"digest ok" fixtures chain to root.der, which is not trusted by default.
The expectations are in test_crypt32_contracts.cpp; they have not been
replayed on Windows yet (oracle/crypt_cab_wvt_probe.c takes these files as
arguments unchanged).
"""
import functools
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crypt_cab_sign import NULL, SHA256, make_pki, oid, seq, tlv  # noqa: E402

from cryptography.hazmat.primitives import hashes, serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import padding  # noqa: E402

SIG = "\x05DigitalSignature"
EX = "\x05MsiDigitalSignatureEx"
SHA1 = seq(oid("1.3.14.3.2.26"), NULL)
MSI_CLSID = bytes.fromhex("84100c0000000000c000000000000046")      # {000C1084-...} package
TRANSFORM_CLSID = bytes.fromhex("82100c0000000000c000000000000046")  # {000C1082-...} transform
SIP_GUID = bytes.fromhex("f1100c0000000000c000000000000046")         # {000C10F1-...}


class Node:
    def __init__(self, name, data=None, clsid=b"\0" * 16, state=0, ctime=0, mtime=0):
        self.name = name
        self.data = data            # bytes for a stream, None for a storage
        self.children = []
        self.clsid = clsid
        self.state = state
        self.ctime = ctime
        self.mtime = mtime

    def add(self, node):
        self.children.append(node)
        return node

    def child(self, name):
        return next(c for c in self.children if c.name == name)


# --- digest rules ----------------------------------------------------------

def name_bytes(name):
    return name.encode("utf-16le")


def digest_order(storage, skip):
    kids = [c for c in storage.children if c.name not in skip]
    return sorted(kids, key=lambda c: name_bytes(c.name))  # bytes compare == memcmp, prefix first


def content(storage, h, skip=()):
    for c in digest_order(storage, skip):
        if c.data is None:
            content(c, h)
        else:
            h.update(c.data)
    h.update(storage.clsid)


def metadata(storage, h, skip=()):
    for c in digest_order(storage, skip):
        h.update(name_bytes(c.name))
        h.update(c.clsid if c.data is None else struct.pack("<I", len(c.data)))
        h.update(struct.pack("<IQQ", c.state, c.ctime, c.mtime))
        if c.data is None:
            metadata(c, h)


def subject_digest(root, alg, extended):
    skip = (SIG, EX)
    h = hashlib.new(alg)
    if extended:
        pre = hashlib.new(alg)
        pre.update(root.clsid + struct.pack("<I", root.state))
        metadata(root, pre, skip)
        h.update(pre.digest())
    content(root, h, skip)
    return h.digest()


def prehash(root, alg):
    pre = hashlib.new(alg)
    pre.update(root.clsid + struct.pack("<I", root.state))
    metadata(root, pre, (SIG, EX))
    return pre.digest()


# --- compound file reader (for --check) --------------------------------------

def read_cfb(data):
    assert data[:8] == bytes.fromhex("d0cf11e0a1b11ae1"), "not a compound file"
    ss = 1 << struct.unpack_from("<H", data, 30)[0]
    nfat, dirstart, _, cutoff, minifat, nminifat, difat, ndifat = struct.unpack_from("<IIIIIIII", data, 44)
    sector = lambda n: data[(n + 1) * ss:(n + 2) * ss]
    fatsecs = list(struct.unpack_from("<109I", data, 76))
    for _ in range(ndifat):
        v = struct.unpack_from("<%dI" % (ss // 4), sector(difat))
        fatsecs += v[:-1]
        difat = v[-1]
    fat = []
    for f in fatsecs[:nfat]:
        fat += struct.unpack_from("<%dI" % (ss // 4), sector(f))

    def chain(n):
        out = bytearray()
        while n < 0xFFFFFFFA:
            out += sector(n)
            n = fat[n]
        return bytes(out)

    d = chain(dirstart)
    entries = []
    for i in range(len(d) // 128):
        e = d[i * 128:(i + 1) * 128]
        nl = struct.unpack_from("<H", e, 64)[0]
        typ = e[66]
        l, r, c = struct.unpack_from("<III", e, 68)
        state, ct, mt, start, size = struct.unpack_from("<IQQIQ", e, 96)
        if ss == 512:
            size &= 0xFFFFFFFF
        entries.append((e[:max(nl - 2, 0)].decode("utf-16le", "surrogatepass"), typ, l, r, c,
                        e[80:96], state, ct, mt, start, size))
    mini = chain(entries[0][9])
    mfat = []
    if nminifat:
        m = chain(minifat)
        mfat = list(struct.unpack_from("<%dI" % (len(m) // 4), m))

    def stream(start, size):
        if size >= cutoff:
            return chain(start)[:size]
        out = bytearray()
        n = start
        while n < 0xFFFFFFFA and len(out) < size:
            out += mini[n * 64:(n + 1) * 64]
            n = mfat[n]
        return bytes(out[:size])

    def build(i):
        name, typ, l, r, c, clsid, state, ct, mt, start, size = entries[i]
        node = Node(name, None if typ in (1, 5) else stream(start, size), clsid, state, ct, mt)
        if typ in (1, 5):
            def walk(j):
                if j >= len(entries):
                    return
                walk(entries[j][2])
                node.children.append(build(j))
                walk(entries[j][3])
            walk(c)
        return node

    return build(0)


def der(b, p=0):
    t, n = b[p], b[p + 1]
    p += 2
    if n & 0x80:
        k = n & 0x7F
        n = int.from_bytes(b[p:p + k], "big")
        p += k
    return t, b[p:p + n], p + n


def items(b):
    p, out = 0, []
    while p < len(b):
        t, v, p = der(b, p)
        out.append((t, v))
    return out


def signed_digest(pkcs7):
    """SpcIndirectDataContent.messageDigest -> (hashlib name, digest)."""
    _, ci, _ = der(pkcs7)
    _, sd, _ = der(items(ci)[1][1])
    eci = items(items(sd)[2][1])
    _, spc, _ = der(eci[1][1])
    info = items(items(spc)[1][1])
    alg = items(info[0][1])[0][1]
    names = {bytes.fromhex("2b0e03021a"): "sha1", bytes.fromhex("608648016503040201"): "sha256",
             bytes.fromhex("608648016503040202"): "sha384", bytes.fromhex("608648016503040203"): "sha512"}
    return names[alg], info[1][1]


def check(paths):
    bad = 0
    for path in paths:
        try:
            root = read_cfb(open(path, "rb").read())
            names = {c.name: c for c in root.children}
            if SIG not in names:
                print(f"unsigned  {path}")
                continue
            alg, want = signed_digest(names[SIG].data)
        except (AssertionError, IndexError, KeyError, ValueError, struct.error) as error:
            print(f"unreadable {path}: {error!r}")
            continue
        extended = EX in names
        got = subject_digest(root, alg, extended)
        ok = got == want and (not extended or names[EX].data == prehash(root, alg))
        bad += not ok
        print(f"{'match' if ok else 'MISMATCH':9} {alg:6} {'ex' if extended else '--'} {path}")
    return 1 if bad else 0


# --- compound file writer ----------------------------------------------------

def cfb_less(a, b):
    """[MS-CFB] 2.6.4 sibling order: shorter name first, then uppercase compare."""
    ua, ub = a.name.upper(), b.name.upper()
    if len(ua) != len(ub):
        return -1 if len(ua) < len(ub) else 1
    return (ua > ub) - (ua < ub)


def write_cfb(root):
    ss, cutoff = 512, 4096
    entries = []

    def flatten(node):
        idx = len(entries)
        entries.append([node, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF])
        if node.data is None:
            kids = [flatten(c) for c in sorted(node.children, key=functools.cmp_to_key(cfb_less))]

            def tree(lo, hi):
                if lo >= hi:
                    return 0xFFFFFFFF
                mid = (lo + hi) // 2
                entries[kids[mid]][1] = tree(lo, mid)
                entries[kids[mid]][2] = tree(mid + 1, hi)
                return kids[mid]
            entries[idx][3] = tree(0, len(kids))
        return idx

    flatten(root)
    mini, minifat, big = bytearray(), [], []
    starts = {}
    for i, (node, *_rest) in enumerate(entries):
        if node.data is None or not node.data:
            continue
        if len(node.data) < cutoff:
            first = len(mini) // 64
            count = (len(node.data) + 63) // 64
            mini += node.data + b"\0" * (count * 64 - len(node.data))
            minifat += [first + k + 1 for k in range(count - 1)] + [0xFFFFFFFE]
            starts[i] = ("mini", first)
        else:
            big.append(i)
    dirbytes = bytearray()
    blobs = []                                    # (kind, bytes) laid out after the FAT
    minifat_bytes = struct.pack("<%dI" % len(minifat), *minifat)
    blobs.append(("dir", None))
    blobs.append(("minifat", minifat_bytes))
    blobs.append(("ministream", bytes(mini)))
    for i in big:
        blobs.append((i, entries[i][0].data))
    sizes = {}
    for kind, data in blobs:
        n = len(data) if data is not None else len(entries) * 128
        sizes[kind] = (n + ss - 1) // ss
    data_sectors = sum(sizes.values())
    nfat = 1
    while nfat * 128 < data_sectors + nfat:
        nfat += 1
    assert nfat <= 109
    fat = [0xFFFFFFFD] * nfat
    first = {}
    for kind, _ in blobs:
        count = sizes[kind]
        first[kind] = len(fat) if count else 0xFFFFFFFE
        fat += [len(fat) + k + 1 for k in range(count - 1)] + ([0xFFFFFFFE] if count else [])
    fat += [0xFFFFFFFF] * (nfat * 128 - len(fat))
    for i, (node, left, right, child) in enumerate(entries):
        name = name_bytes("Root Entry" if i == 0 else node.name)
        assert len(name) <= 62
        typ = 5 if i == 0 else (1 if node.data is None else 2)
        if i == 0:
            start, size = first["ministream"], len(mini)
        elif node.data is None or not node.data:
            start, size = (0 if node.data is None else 0xFFFFFFFE), 0
        elif i in starts:
            start, size = starts[i][1], len(node.data)
        else:
            start, size = first[i], len(node.data)
        e = name + b"\0" * (64 - len(name))
        e += struct.pack("<HBBIII", len(name) + 2, typ, 1, left, right, child)
        e += node.clsid + struct.pack("<IQQIQ", node.state, node.ctime, node.mtime, start, size)
        dirbytes += e
    blob_data = {"dir": bytes(dirbytes)}
    header = bytearray(512)
    header[0:8] = bytes.fromhex("d0cf11e0a1b11ae1")
    struct.pack_into("<HHHHH", header, 24, 0x3E, 3, 0xFFFE, 9, 6)
    struct.pack_into("<IIIIIIIII", header, 40, 0, nfat, first["dir"], 0, cutoff,
                     first["minifat"] if minifat else 0xFFFFFFFE, sizes["minifat"], 0xFFFFFFFE, 0)
    difat = list(range(nfat)) + [0xFFFFFFFF] * (109 - nfat)
    struct.pack_into("<109I", header, 76, *difat)
    out = bytearray(header)
    out += struct.pack("<%dI" % len(fat), *fat)
    for kind, data in blobs:
        data = blob_data.get(kind, data)
        out += data + b"\0" * (sizes[kind] * ss - len(data))
    return bytes(out)


# --- fixtures ------------------------------------------------------------------

def package():
    root = Node("Root Entry", clsid=MSI_CLSID, mtime=0x01DD567A522B2120)
    root.add(Node("\x05SummaryInformation", b"\xfe\xff\x00\x00" + bytes(range(44))))
    # MSI-encoded table names, and pairs whose UTF-16LE byte order differs
    # from code-unit order (Ł / ɀ) and from case-insensitive order.
    root.add(Node("䡀㽿䅤䈯䠶", b"string pool " * 9))
    root.add(Node("Łx", b"code unit 0141"))
    root.add(Node("ɀx", b"code unit 0240"))
    root.add(Node("a1", b"lower a"))
    root.add(Node("B1", b"upper B"))
    root.add(Node("Ab", b"prefix"))
    root.add(Node("Abc", b"prefix plus"))
    root.add(Node("Empty", b""))
    root.add(Node("Binary.big", bytes((i * 7) & 0xFF for i in range(6000))))
    sub = root.add(Node("1033", clsid=TRANSFORM_CLSID, state=0, ctime=0x01DD567A522B2120,
                        mtime=0x01DD567A522B2130))
    sub.add(Node("䡀㬿䏲䐸䖱", b"nested transform stream " * 3))
    sub.add(Node("inner", clsid=bytes(range(16)), state=7, ctime=1, mtime=2))
    return root


def spc_indirect(alg_seq, digest, version):
    sip_info = seq(tlv(0x02, bytes([version])), tlv(0x04, SIP_GUID),
                   *[tlv(0x02, b"\x00") for _ in range(5)])
    return seq(seq(oid("1.3.6.1.4.1.311.2.1.30"), sip_info), seq(alg_seq, tlv(0x04, digest)))


def pkcs7_for(indirect, leaf, leaf_key, extra):
    value = indirect[2:] if indirect[1] < 0x80 else indirect[2 + (indirect[1] & 0x7F):]
    attributes = sorted([
        seq(oid("1.2.840.113549.1.9.3"), tlv(0x31, oid("1.3.6.1.4.1.311.2.1.4"))),
        seq(oid("1.3.6.1.4.1.311.2.1.12"), tlv(0x31, seq())),
        seq(oid("1.2.840.113549.1.9.4"), tlv(0x31, tlv(0x04, hashlib.sha256(value).digest()))),
    ])
    signature = leaf_key.sign(tlv(0x31, b"".join(attributes)), padding.PKCS1v15(), hashes.SHA256())
    serial = leaf.serial_number.to_bytes((leaf.serial_number.bit_length() + 8) // 8, "big")
    signer_info = seq(tlv(0x02, b"\x01"), seq(leaf.issuer.public_bytes(), tlv(0x02, serial)), SHA256,
                      tlv(0xA0, b"".join(attributes)), seq(oid("1.2.840.113549.1.1.1"), NULL),
                      tlv(0x04, signature))
    certs = b"".join(c.public_bytes(serialization.Encoding.DER) for c in [leaf] + extra)
    signed_data = seq(tlv(0x02, b"\x01"), tlv(0x31, SHA256),
                      seq(oid("1.3.6.1.4.1.311.2.1.4"), tlv(0xA0, indirect)),
                      tlv(0xA0, certs), tlv(0x31, signer_info))
    return seq(oid("1.2.840.113549.1.7.2"), tlv(0xA0, signed_data))


def signed(alg, extended, pki):
    leaf, leaf_key, root_cert = pki
    root = package()
    if extended:
        root.add(Node(EX, prehash(root, alg)))
    alg_seq = SHA1 if alg == "sha1" else SHA256
    indirect = spc_indirect(alg_seq, subject_digest(root, alg, extended), 1 if alg == "sha1" else 2)
    root.add(Node(SIG, pkcs7_for(indirect, leaf, leaf_key, [root_cert])))
    return root


def flip(data, at):
    b = bytearray(data)
    b[at] ^= 1
    return bytes(b)


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--check":
        sys.exit(check(sys.argv[2:]))
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    pki = make_pki()
    files = {"msi_unsigned": package()}
    for alg in ("sha1", "sha256"):
        files[f"msi_{alg}"] = signed(alg, False, pki)
        files[f"msi_{alg}_ex"] = signed(alg, True, pki)

    t = signed("sha256", True, pki)
    stream = t.child("1033").child("䡀㬿䏲䐸䖱")
    stream.data = flip(stream.data, 5)
    files["msi_stream_ex"] = t

    t = signed("sha256", False, pki)
    t.child("Binary.big").data = flip(t.child("Binary.big").data, 4500)
    files["msi_big"] = t

    for name, extended in (("msi_meta", False), ("msi_meta_ex", True)):
        t = signed("sha256", extended, pki)
        t.child("1033").mtime += 10000000
        files[name] = t

    t = signed("sha256", False, pki)
    t.child("1033").child("inner").clsid = bytes(range(1, 17))
    files["msi_clsid"] = t

    t = signed("sha256", True, pki)
    t.child(EX).data = flip(t.child(EX).data, 0)
    files["msi_ex_flip"] = t

    t = signed("sha256", True, pki)
    t.children.remove(t.child(SIG))
    files["msi_ex_only"] = t

    t = signed("sha256", False, pki)
    t.child(SIG).data = b"\x30\x03\x02\x01\x01" + b"\0" * 32
    files["msi_garbage_sig"] = t

    for name, tree in files.items():
        open(os.path.join(out, name + ".msi"), "wb").write(write_cfb(tree))
    open(os.path.join(out, "msi_corrupt.msi"), "wb").write(
        bytes.fromhex("d0cf11e0a1b11ae1") + b"\0" * 16 + b"\xff" * 488)
    open(os.path.join(out, "root.der"), "wb").write(pki[2].public_bytes(serialization.Encoding.DER))


if __name__ == "__main__":
    main()
