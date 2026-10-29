#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-only
"""Build Authenticode PE fixtures with RFC 3161 timestamps for the
WinVerifyTrust timestamp tests (test_crypt32_contracts.cpp).

Usage: crypt_rfc3161_sign.py <out-dir>

Makes a throwaway test PKI (RSA-2048, SHA-256):

  root.der         self-signed root, 2000-01-01 .. 2099-12-31
  leaf             code signing, 2020-01-01 .. 2021-01-01 (expired now)
  tsa              time stamping (critical EKU), 2019-01-01 .. 2040-01-01

and a minimal PE32+ image signed by the leaf in these variants:

  ts_none.exe        no timestamp
  ts_valid.exe       RFC 3161 token, genTime 2020-06-01T12:00:00Z
  ts_fraction.exe    same, genTime with fractional seconds (...00.5Z)
  ts_badimprint.exe  token whose messageImprint is not the signature's hash

The token layout follows RFC 3161 section 2.4.2 and the Authenticode
timestamp (unsigned attribute 1.3.6.1.4.1.311.3.3.1 holding a ContentInfo
SignedData whose eContent is the TSTInfo, imprinting the hash of the
primary signer's encryptedDigest). Windows checks a timestamped signer's
certificate at genTime, not at the current time ("Time Stamping Authenticode
Signatures", Microsoft Learn).
"""
import datetime
import hashlib
import os
import struct
import sys

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID


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


def integer(value):
    return tlv(0x02, value.to_bytes((value.bit_length() + 8) // 8, "big"))


NULL = b"\x05\x00"
SHA256 = seq(oid("2.16.840.1.101.3.4.2.1"), NULL)
RSA = seq(oid("1.2.840.113549.1.1.1"), NULL)


def utc(year, month, day):
    return datetime.datetime(year, month, day, tzinfo=datetime.timezone.utc)


def make_cert(subject, issuer_name, issuer_key, key, not_before, not_after, serial, ca, eku):
    builder = (x509.CertificateBuilder()
               .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, subject)]))
               .issuer_name(issuer_name)
               .public_key(key.public_key())
               .serial_number(serial)
               .not_valid_before(not_before)
               .not_valid_after(not_after)
               .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
    if eku is not None:
        builder = builder.add_extension(x509.ExtendedKeyUsage([eku]),
                                        critical=eku == ExtendedKeyUsageOID.TIME_STAMPING)
    return builder.sign(issuer_key, hashes.SHA256())


def minimal_pe():
    """A header-only PE32+ image: DOS header, PE signature, file header and
    an optional header with 16 data directories, padded to 0x200."""
    image = bytearray(0x200)
    struct.pack_into("<H", image, 0, 0x5A4D)
    struct.pack_into("<I", image, 0x3C, 0x80)
    image[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", image, 0x84, 0x8664, 0, 0, 0, 0, 0xF0, 0x0022)
    opt = 0x98
    struct.pack_into("<H", image, opt, 0x20B)
    struct.pack_into("<I", image, opt + 32, 0x1000)      # SectionAlignment
    struct.pack_into("<I", image, opt + 36, 0x200)       # FileAlignment
    struct.pack_into("<I", image, opt + 56, 0x1000)      # SizeOfImage
    struct.pack_into("<I", image, opt + 60, 0x200)       # SizeOfHeaders
    struct.pack_into("<H", image, opt + 68, 3)           # Subsystem: console
    struct.pack_into("<I", image, opt + 108, 16)         # NumberOfRvaAndSizes
    return image


def pe_digest(image):
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    opt = pe + 24
    checksum = opt + 64
    certdir = opt + 144
    h = hashlib.sha256()
    h.update(image[:checksum])
    h.update(image[checksum + 4:certdir])
    h.update(image[certdir + 8:])
    return h.digest(), certdir


def signer_info(cert, attributes, signature, unsigned=b""):
    serial = cert.serial_number.to_bytes((cert.serial_number.bit_length() + 8) // 8, "big")
    return seq(
        tlv(0x02, b"\x01"),
        seq(cert.issuer.public_bytes(), tlv(0x02, serial)),
        SHA256,
        tlv(0xA0, b"".join(attributes)),
        RSA,
        tlv(0x04, signature),
        unsigned,
    )


def sign_attributes(key, attributes):
    return key.sign(tlv(0x31, b"".join(attributes)), padding.PKCS1v15(), hashes.SHA256())


def timestamp_token(tsa, tsa_key, imprinted, gen_time):
    tst_info = seq(
        integer(1),
        oid("1.2.3.4.1"),                                   # TSA policy
        seq(SHA256, tlv(0x04, hashlib.sha256(imprinted).digest())),
        integer(0x5151),
        tlv(0x18, gen_time.encode()),
    )
    attributes = sorted([
        seq(oid("1.2.840.113549.1.9.3"), tlv(0x31, oid("1.2.840.113549.1.9.16.1.4"))),
        seq(oid("1.2.840.113549.1.9.4"), tlv(0x31, tlv(0x04, hashlib.sha256(tst_info).digest()))),
    ])
    signed_data = seq(
        integer(3),
        tlv(0x31, SHA256),
        seq(oid("1.2.840.113549.1.9.16.1.4"), tlv(0xA0, tlv(0x04, tst_info))),
        tlv(0xA0, tsa.public_bytes(serialization.Encoding.DER)),
        tlv(0x31, signer_info(tsa, attributes, sign_attributes(tsa_key, attributes))),
    )
    return seq(oid("1.2.840.113549.1.7.2"), tlv(0xA0, signed_data))


def sign_pe(leaf, leaf_key, tsa, tsa_key, gen_time, bad_imprint):
    image = minimal_pe()
    digest, certdir = pe_digest(bytes(image))
    obsolete = "<<<Obsolete>>>".encode("utf-16-be")
    indirect = seq(seq(oid("1.3.6.1.4.1.311.2.1.15"),
                       seq(tlv(0x03, b"\x00"), tlv(0xA0, tlv(0xA2, tlv(0x80, obsolete))))),
                   seq(SHA256, tlv(0x04, digest)))
    indirect_value = indirect[2:] if indirect[1] < 0x80 else indirect[2 + (indirect[1] & 0x7F):]
    attributes = sorted([
        seq(oid("1.2.840.113549.1.9.3"), tlv(0x31, oid("1.3.6.1.4.1.311.2.1.4"))),
        seq(oid("1.3.6.1.4.1.311.2.1.12"), tlv(0x31, seq())),
        seq(oid("1.2.840.113549.1.9.4"), tlv(0x31, tlv(0x04, hashlib.sha256(indirect_value).digest()))),
    ])
    signature = sign_attributes(leaf_key, attributes)
    unsigned = b""
    if gen_time is not None:
        imprinted = signature if not bad_imprint else bytes([signature[0] ^ 1]) + signature[1:]
        token = timestamp_token(tsa, tsa_key, imprinted, gen_time)
        unsigned = tlv(0xA1, seq(oid("1.3.6.1.4.1.311.3.3.1"), tlv(0x31, token)))
    signed_data = seq(
        integer(1),
        tlv(0x31, SHA256),
        seq(oid("1.3.6.1.4.1.311.2.1.4"), tlv(0xA0, indirect)),
        tlv(0xA0, leaf.public_bytes(serialization.Encoding.DER)),
        tlv(0x31, signer_info(leaf, attributes, signature, unsigned)),
    )
    pkcs7 = seq(oid("1.2.840.113549.1.7.2"), tlv(0xA0, signed_data))
    length = 8 + len(pkcs7)
    padded = (length + 7) & ~7
    struct.pack_into("<II", image, certdir, len(image), padded)
    return bytes(image) + struct.pack("<IHH", padded, 0x0200, 0x0002) + pkcs7 + b"\x00" * (padded - length)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    root_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    leaf_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    tsa_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    root_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Vitreo RFC3161 Test Root")])
    root = make_cert("Vitreo RFC3161 Test Root", root_name, root_key, root_key,
                     utc(2000, 1, 1), utc(2099, 12, 31), 1, True, None)
    leaf = make_cert("Vitreo RFC3161 Test Signer", root_name, root_key, leaf_key,
                     utc(2020, 1, 1), utc(2021, 1, 1), 2, False, ExtendedKeyUsageOID.CODE_SIGNING)
    tsa = make_cert("Vitreo RFC3161 Test TSA", root_name, root_key, tsa_key,
                    utc(2019, 1, 1), utc(2040, 1, 1), 3, False, ExtendedKeyUsageOID.TIME_STAMPING)
    open(os.path.join(out, "root.der"), "wb").write(root.public_bytes(serialization.Encoding.DER))
    variants = {
        "ts_none.exe": (None, False),
        "ts_valid.exe": ("20200601120000Z", False),
        "ts_fraction.exe": ("20200601120000.5Z", False),
        "ts_badimprint.exe": ("20200601120000Z", True),
    }
    for name, (gen_time, bad_imprint) in variants.items():
        open(os.path.join(out, name), "wb").write(
            sign_pe(leaf, leaf_key, tsa, tsa_key, gen_time, bad_imprint))


if __name__ == "__main__":
    main()
