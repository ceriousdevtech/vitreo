#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-only
"""Build Authenticode-signed cabinet fixtures for the cabinet WinVerifyTrust
oracle (crypt_cab_wvt.expected).

Usage: crypt_cab_sign.py <out-dir>

Makes a throwaway test PKI (root + leaf, RSA-2048, SHA-256), a minimal
uncompressed cabinet holding one file, and these variants:

  cab_unsigned.cab   no reserve area                      (no signature)
  cab_signed.cab     leaf + root in the PKCS#7            (root not trusted)
  cab_tampered.cab   signed, one data byte flipped        (digest mismatch)
  cab_reserved1.cab  signed, CFHEADER.reserved1 changed   (excluded from digest)
  cab_reserve12.cab  signed, abReserve[12] changed        (excluded from digest)
  cab_icabinet.cab   signed, iCabinet changed             (covered by digest)
  cab_lead.cab       signed, abReserve lead word changed  (signature not found)
  cab_tail.cab       signed, 16 bytes appended            (covered by digest)

The digest model (CFHEADER without reserved1, cbCFHeader and abReserve, then
everything but the PKCS#7 blob) was measured with WinVerifyTrust on a
Microsoft-signed cabinet; Windows, not this script, is the judge of these
fixtures (crypt_cab_wvt.expected).
"""
import datetime
import hashlib
import os
import struct
import sys

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID


def der_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b


def tlv(tag, content):
    return bytes([tag]) + der_len(len(content)) + content


def seq(*items):
    return tlv(0x30, b"".join(items))


def oid(dotted):
    arcs = [int(a) for a in dotted.split(".")]
    out = bytearray()
    for arc in [arcs[0] * 40 + arcs[1]] + arcs[2:]:
        chunk = [arc & 0x7F]
        arc >>= 7
        while arc:
            chunk.append(0x80 | (arc & 0x7F))
            arc >>= 7
        out += bytes(reversed(chunk))
    return tlv(0x06, bytes(out))


NULL = b"\x05\x00"
SHA256 = seq(oid("2.16.840.1.101.3.4.2.1"), NULL)
RESERVE = 20


def make_pki():
    now = datetime.datetime(2026, 1, 1, tzinfo=datetime.timezone.utc)
    later = now + datetime.timedelta(days=3650)
    root_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    root_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Vitreo Cab Probe Root")])
    root = (x509.CertificateBuilder().subject_name(root_name).issuer_name(root_name)
            .public_key(root_key.public_key()).serial_number(0x5001)
            .not_valid_before(now).not_valid_after(later)
            .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
            .add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False), critical=True)
            .sign(root_key, hashes.SHA256()))
    leaf_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    leaf = (x509.CertificateBuilder()
            .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "vitreo-cab-probe.test")]))
            .issuer_name(root_name).public_key(leaf_key.public_key()).serial_number(0x5002)
            .not_valid_before(now).not_valid_after(later)
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CODE_SIGNING]), critical=False)
            .sign(root_key, hashes.SHA256()))
    return leaf, leaf_key, root


def make_cab(reserve):
    """One uncompressed folder holding 'hello.txt'. With reserve, the CFHEADER
    carries a 20-byte abReserve area (signature fields filled in later)."""
    payload = b"Vitreo cabinet Authenticode fixture.\r\n" * 4
    name = b"hello.txt\x00"
    header = 36 + (4 + RESERVE if reserve else 0)
    folder_at = header
    file_at = folder_at + 8
    data_at = file_at + 16 + len(name)
    total = data_at + 8 + len(payload)
    flags = 0x0004 if reserve else 0
    cab = struct.pack("<4sIIIIIBBHHHHH", b"MSCF", 0, total, 0, file_at, 0, 3, 1, 1, 1, flags, 0x1234, 0)
    if reserve:
        cab += struct.pack("<HBB", RESERVE, 0, 0) + b"\x00" * RESERVE
    cab += struct.pack("<IHH", data_at, 1, 0)                                   # CFFOLDER, no compression
    cab += struct.pack("<IIHHHH", len(payload), 0, 0, 0x5A21, 0x0000, 0x20) + name  # CFFILE
    cab += struct.pack("<IHH", 0, len(payload), len(payload)) + payload       # CFDATA, csum 0
    assert len(cab) == total
    return bytearray(cab)


def cab_digest(cab, sig_offset, sig_size):
    h = hashlib.sha256()
    h.update(cab[0:4])
    h.update(cab[8:36])
    h.update(cab[38:40])
    h.update(cab[40 + RESERVE:sig_offset])
    h.update(cab[sig_offset + sig_size:])
    return h.digest()


def sign(cab, leaf, leaf_key, extra):
    """Append the PKCS#7 and point abReserve at it."""
    sig_offset = len(cab)
    # The PKCS#7 length must be known before the digest (abReserve is not
    # hashed, but the blob is excluded by size); build twice.
    pkcs7 = b""
    for _ in range(2):
        struct.pack_into("<III", cab, 40, 0x00100000, sig_offset, len(pkcs7))
        digest = cab_digest(bytes(cab) + pkcs7, sig_offset, len(pkcs7))
        cab_data = seq(tlv(0x03, b"\x00"), tlv(0xA0, tlv(0x80, b"")))  # SpcLink, empty
        indirect = seq(seq(oid("1.3.6.1.4.1.311.2.1.25"), cab_data), seq(SHA256, tlv(0x04, digest)))
        value = indirect[2:] if indirect[1] < 0x80 else indirect[2 + (indirect[1] & 0x7F):]
        attributes = sorted([
            seq(oid("1.2.840.113549.1.9.3"), tlv(0x31, oid("1.3.6.1.4.1.311.2.1.4"))),
            seq(oid("1.3.6.1.4.1.311.2.1.12"), tlv(0x31, seq())),
            seq(oid("1.2.840.113549.1.9.4"), tlv(0x31, tlv(0x04, hashlib.sha256(value).digest()))),
        ])
        signature = leaf_key.sign(tlv(0x31, b"".join(attributes)), padding.PKCS1v15(), hashes.SHA256())
        issuer = leaf.issuer.public_bytes()
        serial = leaf.serial_number.to_bytes((leaf.serial_number.bit_length() + 8) // 8, "big")
        signer_info = seq(tlv(0x02, b"\x01"), seq(issuer, tlv(0x02, serial)), SHA256,
                          tlv(0xA0, b"".join(attributes)), seq(oid("1.2.840.113549.1.1.1"), NULL),
                          tlv(0x04, signature))
        certs = b"".join(c.public_bytes(serialization.Encoding.DER) for c in [leaf] + extra)
        signed_data = seq(tlv(0x02, b"\x01"), tlv(0x31, SHA256),
                          seq(oid("1.3.6.1.4.1.311.2.1.4"), tlv(0xA0, indirect)),
                          tlv(0xA0, certs), tlv(0x31, signer_info))
        new = seq(oid("1.2.840.113549.1.7.2"), tlv(0xA0, signed_data))
        if len(new) == len(pkcs7):
            pkcs7 = new
            break
        pkcs7 = new
    struct.pack_into("<III", cab, 40, 0x00100000, sig_offset, len(pkcs7))
    return bytearray(bytes(cab) + pkcs7)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    leaf, leaf_key, root = make_pki()
    open(f"{out}/cab_unsigned.cab", "wb").write(bytes(make_cab(False)))
    signed = sign(make_cab(True), leaf, leaf_key, [root])
    variants = {"cab_signed": [], "cab_tampered": [(len(signed) - 1 - struct.unpack_from("<I", signed, 48)[0], None)],
                "cab_reserved1": [(4, 0x55)], "cab_reserve12": [(52, 0x55)], "cab_icabinet": [(34, 1)],
                "cab_lead": [(40, 0x55)]}
    for name, edits in variants.items():
        x = bytearray(signed)
        for off, val in edits:
            x[off] = (x[off] ^ 1) if val is None else val
        open(f"{out}/{name}.cab", "wb").write(bytes(x))
    open(f"{out}/cab_tail.cab", "wb").write(bytes(signed) + b"\x00" * 16)


if __name__ == "__main__":
    main()
