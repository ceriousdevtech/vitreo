// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crypt32_contracts.cpp
/// @brief Oracle replays of the crypt32 contract probes.
///
/// Each test runs the logic of a tools/oracle_probe probe against Vitreo's
/// exported thunks at both pointer widths and compares the transcript with
/// the Windows output committed next to the probe. Expected values therefore
/// come from Windows, never from this implementation.

#include "crypt32_api_harness.hpp"

#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>

#include <fcntl.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "../crypt32_test_certs.hpp"
#include "../../oracle_data/crypt_chain_certs.h"
#include "../../oracle_data/crypt_decode_cases.h"

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::test::CryptApi;
using vitreo::winapi::test::bytes_hex;
using vitreo::winapi::test::expect_transcript;
using vitreo::winapi::test::hex;
using vitreo::winapi::test::read_expected;

namespace {

constexpr u32 kX509 = 0x1;
constexpr u32 kPkcs7 = 0x10000;

/// CERT_INFO member offsets (wincrypt.h), per pointer width.
struct InfoLayout {
    u32 ptr, blob, alg, bit_blob, serial, sig_alg, issuer, not_before, not_after,
        subject, spki, pubkey, issuer_uid, subject_uid, c_ext, rg_ext, ext_size;
    explicit InfoLayout(bool is64)
        : ptr(is64 ? 8 : 4), blob(is64 ? 16 : 8), alg(is64 ? 24 : 12),
          bit_blob(is64 ? 24 : 12), ext_size(is64 ? 32 : 16) {
        serial = ptr; sig_alg = serial + blob; issuer = sig_alg + alg;
        not_before = issuer + blob; not_after = not_before + 8; subject = not_after + 8;
        spki = subject + blob; pubkey = spki + alg; issuer_uid = pubkey + bit_blob;
        subject_uid = issuer_uid + bit_blob; c_ext = subject_uid + bit_blob;
        rg_ext = c_ext + ptr;
    }
};

// ---------------------------------------------------------------------------
// tools/oracle_probe/cert_eku_probe.cpp -> cert_eku_probe{32,64}.expected
// ---------------------------------------------------------------------------

std::vector<std::string> replay_eku_probe(bool is64) {
    CryptApi api(is64);
    const InfoLayout l(is64);
    const u32 ps = api.ptr_size();
    std::vector<std::string> out;
    out.push_back("layout\t" + hex(is64 ? 16 : 8) + "\t" + hex(ps));

    const GuestAddr leaf_der = api.put(kLeafDer, sizeof(kLeafDer));
    const GuestAddr empty = api.put("\x30\x00", 2);
    const GuestAddr eku_oid = api.put_string("2.5.29.37");
    const GuestAddr extension = api.alloc(l.ext_size, 8);
    api.write_ptr(extension, eku_oid);
    api.write_u32(extension + ps, 0);
    api.write_u32(extension + 2 * ps, 2);
    api.write_ptr(extension + 3 * ps, empty);
    const GuestAddr server_auth = api.put_string("1.3.6.1.5.5.7.3.1");
    const GuestAddr other = api.put_string("1.2.3.4");
    const GuestAddr ids = api.alloc(3 * ps, 8);
    api.write_ptr(ids, server_auth);
    api.write_ptr(ids + ps, other);
    api.write_ptr(ids + 2 * ps, server_auth);
    const GuestAddr usage_in = api.alloc(2 * ps, 8);
    const GuestAddr buffer = api.alloc(512, 16);
    const GuestAddr size_ptr = api.alloc(4, 4);
    const u32 flags[] = {0, 2, 4, 3, 4, 0xffffffffu};

    for (unsigned extcase = 0; extcase < 3; ++extcase) {
        for (unsigned propcase = 0; propcase < 4; ++propcase) {
            const GuestAddr cert = static_cast<GuestAddr>(
                api.call("CertCreateCertificateContext", {kX509, leaf_der, sizeof(kLeafDer)}));
            EXPECT_NE(cert, 0u);
            const GuestAddr info = static_cast<GuestAddr>(api.ptr_at(cert + 3 * ps));
            const u32 saved_count = api.u32_at(info + l.c_ext);
            const u64 saved_ext = api.ptr_at(info + l.rg_ext);
            if (extcase == 1) api.write_u32(info + l.c_ext, 0);
            if (extcase == 2) {
                api.write_u32(info + l.c_ext, 1);
                api.write_ptr(info + l.rg_ext, extension);
            }
            api.write_u32(usage_in, propcase == 1 ? 0u : propcase == 2 ? 1u : 3u);
            api.write_ptr(usage_in + ps, ids);
            if (propcase) api.call("CertSetEnhancedKeyUsage", {cert, usage_in});
            for (unsigned f = 0; f < 6; ++f) {
                for (unsigned mode = 0; mode < 4; ++mode) {
                    for (u32 i = 0; i < 512; ++i) (void)api.memory().write_u8(buffer + i, 0xcc);
                    const u32 size = mode == 0 ? 0 : mode == 1 ? 1 : mode == 2 ? (is64 ? 16 : 8) : 512;
                    api.write_u32(size_ptr, size);
                    set_last_error(0xdead);
                    const u64 ok = api.call("CertGetEnhancedKeyUsage",
                                            {cert, flags[f], mode == 0 ? 0 : buffer, size_ptr});
                    const u32 err = get_last_error();
                    std::string line = hex(extcase) + "\t" + hex(propcase) + "\t" + hex(flags[f]) +
                                       "\t" + hex(mode) + "\t" + hex(ok) + "\t" + hex(err) + "\t" +
                                       hex(api.u32_at(size_ptr));
                    if (ok && mode) {
                        const u32 count = api.u32_at(buffer);
                        line += "\t" + hex(count);
                        const GuestAddr array = static_cast<GuestAddr>(api.ptr_at(buffer + ps));
                        for (u32 i = 0; i < count; ++i) {
                            line += "\t" + api.cstring_at(
                                static_cast<GuestAddr>(api.ptr_at(array + i * ps)));
                        }
                    }
                    out.push_back(line);
                }
            }
            api.write_u32(info + l.c_ext, saved_count);
            api.write_ptr(info + l.rg_ext, saved_ext);
            api.call("CertFreeCertificateContext", {cert});
        }
    }
    return out;
}

TEST(Crypt32OracleReplay, CertGetEnhancedKeyUsageMatchesWindowsX64) {
    expect_transcript(read_expected("cert_eku_probe64.expected"), replay_eku_probe(true));
}

TEST(Crypt32OracleReplay, CertGetEnhancedKeyUsageMatchesWindowsX86) {
    expect_transcript(read_expected("cert_eku_probe32.expected"), replay_eku_probe(false));
}

// ---------------------------------------------------------------------------
// tools/oracle_probe/crypt_prop_probe.c -> crypt_prop_probe{,.x86}.expected
// ---------------------------------------------------------------------------

std::vector<std::string> replay_prop_probe(bool is64) {
    CryptApi api(is64);
    const InfoLayout l(is64);
    const u32 ps = api.ptr_size();
    std::vector<std::string> out;
    out.push_back("layout\t" + hex(is64 ? 0x28 : 0x14) + "\t" + hex(is64 ? 0xd0 : 0x70) +
                  "\t" + hex(is64 ? 0x30 : 0x1c));
    const GuestAddr size_ptr = api.alloc(4, 4);
    auto create = [&](u32 encoding, const void* der, u32 size) -> GuestAddr {
        const GuestAddr data = der ? api.put(der, size) : 0;
        return static_cast<GuestAddr>(
            api.call("CertCreateCertificateContext", {encoding, data, size}));
    };
    auto create_case = [&](const std::string& name, u32 encoding, const void* der, u32 size) {
        set_last_error(0xdead);
        const GuestAddr c = create(encoding, der, size);
        const u32 e = get_last_error();
        out.push_back("create " + name + "\t" + hex(c != 0) + "\t" + hex(e));
        if (c) api.call("CertFreeCertificateContext", {c});
    };

    std::vector<u8> trailing(kProbeLeafDer, kProbeLeafDer + sizeof(kProbeLeafDer));
    trailing.push_back(0);
    trailing.push_back(0);
    static const u8 minimal[] = {0x30,0x0f,0x30,0x0d,0x02,0x01,0x01,0x30,0x00,
                                 0x30,0x00,0x30,0x00,0x30,0x00,0x30,0x00};
    static const u8 junk[] = {0x04,0x03,0x01,0x02,0x03};
    create_case("valid", kX509, kProbeLeafDer, sizeof(kProbeLeafDer));
    create_case("valid_both", kX509 | kPkcs7, kProbeLeafDer, sizeof(kProbeLeafDer));
    create_case("enc0", 0, kProbeLeafDer, sizeof(kProbeLeafDer));
    create_case("enc_pkcs7", kPkcs7, kProbeLeafDer, sizeof(kProbeLeafDer));
    create_case("enc2", 2, kProbeLeafDer, sizeof(kProbeLeafDer));
    create_case("truncated", kX509, kProbeLeafDer, sizeof(kProbeLeafDer) - 1);
    create_case("trailing", kX509, trailing.data(), static_cast<u32>(trailing.size()));
    create_case("minimal", kX509, minimal, sizeof(minimal));
    create_case("junk", kX509, junk, sizeof(junk));
    create_case("len0", kX509, kProbeLeafDer, 0);
    create_case("null", kX509, nullptr, 0);
    create_case("badsig", kX509, kProbeBadSignatureLeafDer, sizeof(kProbeBadSignatureLeafDer));
    for (const auto& c : kDecodeCases) create_case(c.name, kX509, c.der, c.size);

    const GuestAddr leaf_bytes = api.put(kProbeLeafDer, sizeof(kProbeLeafDer));
    const GuestAddr leaf = static_cast<GuestAddr>(api.call(
        "CertCreateCertificateContext", {kX509 | kPkcs7, leaf_bytes, sizeof(kProbeLeafDer)}));
    const GuestAddr inter =
        create(kX509, kProbeIntermediateDer, sizeof(kProbeIntermediateDer));
    EXPECT_NE(leaf, 0u);
    EXPECT_NE(inter, 0u);
    out.push_back("ctx\t" + hex(api.u32_at(leaf)) + "\t" + hex(api.u32_at(leaf + 2 * ps)) +
                  "\t" + hex(api.ptr_at(leaf + 4 * ps) != 0) + "\t" +
                  hex(api.ptr_at(leaf + ps) != leaf_bytes));
    const GuestAddr info = static_cast<GuestAddr>(api.ptr_at(leaf + 3 * ps));
    auto blob_bytes = [&](GuestAddr blob) {
        return api.bytes_at(static_cast<GuestAddr>(api.ptr_at(blob + ps)), api.u32_at(blob));
    };
    std::string line = "info\t" + hex(api.u32_at(info)) + "\t" + hex(api.u32_at(info + l.serial)) +
        "\t" + bytes_hex(blob_bytes(info + l.serial)) + "\t" +
        api.cstring_at(static_cast<GuestAddr>(api.ptr_at(info + l.sig_alg))) + "\t" +
        hex(api.u32_at(info + l.sig_alg + ps)) + "\t" + hex(api.u32_at(info + l.issuer)) + "\t" +
        hex(api.u32_at(info + l.subject)) + "\t" + hex(api.u32_at(info + l.not_before + 4)) + "\t" +
        hex(api.u32_at(info + l.not_before)) + "\t" + hex(api.u32_at(info + l.not_after + 4)) +
        "\t" + hex(api.u32_at(info + l.not_after)) + "\t" +
        api.cstring_at(static_cast<GuestAddr>(api.ptr_at(info + l.spki))) + "\t" +
        hex(api.u32_at(info + l.pubkey)) + "\t" + hex(api.u32_at(info + l.pubkey + 2 * ps)) +
        "\t" + hex(api.u32_at(info + l.issuer_uid)) + "\t" + hex(api.u32_at(info + l.subject_uid)) +
        "\t" + hex(api.u32_at(info + l.c_ext));
    const GuestAddr extensions = static_cast<GuestAddr>(api.ptr_at(info + l.rg_ext));
    for (u32 i = 0; i < api.u32_at(info + l.c_ext); ++i) {
        const GuestAddr e = extensions + i * l.ext_size;
        line += "\t" + api.cstring_at(static_cast<GuestAddr>(api.ptr_at(e))) + ":" +
                hex(api.u32_at(e + ps)) + ":" + hex(api.u32_at(e + 2 * ps));
    }
    out.push_back(line);

    const GuestAddr fresh = create(kX509, kProbeLeafDer, sizeof(kProbeLeafDer));
    const GuestAddr buf = api.alloc(512, 16);
    auto get = [&](GuestAddr context, u32 id, GuestAddr data, u32 size) -> u64 {
        api.write_u32(size_ptr, size);
        return api.call("CertGetCertificateContextProperty", {context, id, data, size_ptr});
    };
    for (u32 id = 1; id <= 131; ++id) {
        set_last_error(0xdead);
        const u64 ok = get(fresh, id, 0, 0);
        const u32 err = get_last_error();
        const u32 size = api.u32_at(size_ptr);
        if (!ok && err == 0x80092004u && size == 0) continue;
        std::string row = "prop\t" + hex(id) + "\t" + hex(ok) + "\t" + hex(err) + "\t" + hex(size);
        if (ok && size && size <= 512 && id != 1 && id != 5 && id != 78 && id != 79) {
            set_last_error(0xdead);
            const u64 ok2 = get(fresh, id, buf, size);
            const u32 got = api.u32_at(size_ptr);
            row += "\t" + hex(ok2) + "\t" + hex(get_last_error()) + "\t" + hex(got) + "\t" +
                   bytes_hex(api.bytes_at(buf, got < 64 ? got : 64));
        }
        out.push_back(row);
    }
    set_last_error(0xdead);
    u64 ok = get(fresh, 0x8000, 0, 0);
    out.push_back("prop_user\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" +
                  hex(api.u32_at(size_ptr)));
    set_last_error(0xdead);
    ok = get(fresh, 0, 0, 0x77);
    out.push_back("prop_zero\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" +
                  hex(api.u32_at(size_ptr)));
    for (const u32 cap : {0u, 1u, 19u, 20u, 21u, 100u}) {
        for (u32 i = 0; i < 128; ++i) (void)api.memory().write_u8(buf + i, 0xcc);
        set_last_error(0xdead);
        ok = get(fresh, 3, buf, cap);
        out.push_back("sha1cap\t" + hex(cap) + "\t" + hex(ok) + "\t" + hex(get_last_error()) +
                      "\t" + hex(api.u32_at(size_ptr)) + "\t" + hex(api.u8_at(buf)) + "\t" +
                      hex(api.u8_at(buf + 20)));
    }

    auto set = [&](GuestAddr context, u32 id, u32 flags, GuestAddr data) -> u64 {
        return api.call("CertSetCertificateContextProperty", {context, id, flags, data});
    };
    const std::u16string name_text = u"Probe Name";
    const GuestAddr name = api.put_wstring(name_text);
    const GuestAddr name_blob = api.put_blob(name, static_cast<u32>((name_text.size() + 1) * 2));
    set_last_error(0xdead);
    ok = set(fresh, 11, 0, name_blob);
    out.push_back("set11\t" + hex(ok) + "\t" + hex(get_last_error()));
    set_last_error(0xdead);
    ok = get(fresh, 11, buf, 128);
    out.push_back("get11\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" +
                  hex(api.u32_at(size_ptr)) + "\t" +
                  bytes_hex(api.bytes_at(buf, api.u32_at(size_ptr))));
    const GuestAddr dup = static_cast<GuestAddr>(
        api.call("CertDuplicateCertificateContext", {fresh}));
    ok = get(dup, 11, buf, 128);
    out.push_back("dup11\t" + hex(dup == fresh) + "\t" + hex(ok) + "\t" + hex(api.u32_at(size_ptr)));
    api.call("CertFreeCertificateContext", {dup});
    std::vector<u8> forced(20, 0x11);
    const GuestAddr forced_blob = api.put_blob(api.put(forced.data(), 20), 20);
    set_last_error(0xdead);
    ok = set(fresh, 3, 0, forced_blob);
    u64 ok2 = get(fresh, 3, buf, 128);
    out.push_back("set3\t" + hex(ok) + "\t" + hex(ok2) + "\t" + hex(api.u32_at(size_ptr)) + "\t" +
                  bytes_hex(api.bytes_at(buf, api.u32_at(size_ptr))));
    set_last_error(0xdead);
    ok = set(fresh, 11, 0, 0);
    u32 e = get_last_error();
    set_last_error(0xdead);
    ok2 = get(fresh, 11, buf, 128);
    out.push_back("del11\t" + hex(ok) + "\t" + hex(e) + "\t" + hex(ok2) + "\t" + hex(get_last_error()));
    set_last_error(0xdead);
    ok = set(fresh, 13, 0, 0);
    out.push_back("del13\t" + hex(ok) + "\t" + hex(get_last_error()));
    const GuestAddr empty_blob = api.put_blob(0, 0);
    set_last_error(0xdead);
    ok = set(fresh, 0x8001, 0, empty_blob);
    set_last_error(0xdead);
    ok2 = get(fresh, 0x8001, 0, 5);
    out.push_back("empty\t" + hex(ok) + "\t" + hex(ok2) + "\t" + hex(get_last_error()) + "\t" +
                  hex(api.u32_at(size_ptr)));
    ok = set(fresh, 3, 0, 0);
    ok2 = get(fresh, 3, buf, 128);
    out.push_back("del3\t" + hex(ok) + "\t" + hex(ok2) + "\t" + hex(api.u32_at(size_ptr)) + "\t" +
                  bytes_hex(api.bytes_at(buf, api.u32_at(size_ptr))));
    for (const u32 bad : {0u, 14u, 5u, 0x6bu, 0x10000u}) {  // mingw CERT_FIRST_RESERVED_PROP_ID = 0x6b
        set_last_error(0xdead);
        ok = set(fresh, bad, 0, name_blob);
        out.push_back("setbad\t" + hex(bad) + "\t" + hex(ok) + "\t" + hex(get_last_error()));
    }
    // CRYPT_KEY_PROV_INFO
    const u32 kp_size = is64 ? 48 : 28;
    const GuestAddr kp = api.alloc(kp_size, 8);
    api.write_ptr(kp, api.put_wstring(u"cont"));
    api.write_ptr(kp + ps, api.put_wstring(u"prov"));
    api.write_u32(kp + 2 * ps, 1);
    api.write_u32(kp + (is64 ? 40 : 24), 2);
    set_last_error(0xdead);
    ok = set(fresh, 2, 0, kp);
    out.push_back("set2\t" + hex(ok) + "\t" + hex(get_last_error()));
    ok = get(fresh, 2, 0, 0);
    out.push_back("get2size\t" + hex(ok) + "\t" + hex(api.u32_at(size_ptr)));
    ok = get(fresh, 2, buf, 256);
    auto rel = [&](u64 p) { return hex(static_cast<u32>(p - buf)); };
    out.push_back("get2\t" + hex(ok) + "\t" + hex(api.u32_at(size_ptr)) + "\t" +
                  rel(api.ptr_at(buf)) + "\t" + api.wstring_at(static_cast<GuestAddr>(api.ptr_at(buf))) +
                  "\t" + rel(api.ptr_at(buf + ps)) + "\t" +
                  api.wstring_at(static_cast<GuestAddr>(api.ptr_at(buf + ps))) + "\t" +
                  hex(api.u32_at(buf + 2 * ps)) + "\t" + hex(api.u32_at(buf + 2 * ps + 4)) + "\t" +
                  hex(api.u32_at(buf + 2 * ps + 8)) + "\t" + hex(api.ptr_at(buf + (is64 ? 32 : 20)) != 0) +
                  "\t" + hex(api.u32_at(buf + (is64 ? 40 : 24))));
    set_last_error(0xdead);
    ok = get(fresh, 6, 0, 0);
    std::string get6 = "get6\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" + hex(api.u32_at(size_ptr));
    if (ok) {
        get(fresh, 6, buf, 4);
        get6 += "\t" + hex(api.u32_at(buf));
    }
    out.push_back(get6);
    // Provider parameters.
    const u32 param_size = is64 ? 24 : 16;
    const GuestAddr params = api.alloc(2 * param_size, 8);
    const u8 p1[] = {0xa1, 0xa2, 0xa3};
    api.write_u32(params, 7);
    api.write_ptr(params + ps, api.put(p1, 3));
    api.write_u32(params + 2 * ps, 3);
    api.write_u32(params + 2 * ps + 4, 9);
    api.write_u32(params + param_size, 8);
    api.write_u32(kp + 2 * ps + 8, 2);
    api.write_ptr(kp + (is64 ? 32 : 20), params);
    for (int variant = 0; variant < 2; ++variant) {
        api.write_ptr(kp, variant ? api.put_wstring(u"cont") : 0);
        ok = set(fresh, 2, 0, kp);
        for (int shift = 0; shift < 2; ++shift) {
            const GuestAddr kb = buf + shift * 4;
            ok2 = get(fresh, 2, kb, 260);
            const u32 size = api.u32_at(size_ptr);
            get(fresh, 2, 0, 0);
            const u32 query = api.u32_at(size_ptr);
            auto off = [&](u64 p) { return hex(static_cast<u32>(p - kb)); };
            std::string row = "get2p\t" + hex(variant) + "\t" + hex(shift) + "\t" + hex(ok) + "\t" +
                              hex(ok2) + "\t" + hex(query) + "\t" + hex(size) + "\t" +
                              (api.ptr_at(kb) ? off(api.ptr_at(kb)) : hex(0xffffffffu)) + "\t" +
                              off(api.ptr_at(kb + ps)) + "\t" + hex(api.u32_at(kb + 2 * ps + 8)) +
                              "\t" + off(api.ptr_at(kb + (is64 ? 32 : 20)));
            const GuestAddr array = static_cast<GuestAddr>(api.ptr_at(kb + (is64 ? 32 : 20)));
            for (u32 i = 0; i < api.u32_at(kb + 2 * ps + 8); ++i) {
                const GuestAddr p = array + i * param_size;
                const u64 data = api.ptr_at(p + ps);
                row += "\t" + hex(api.u32_at(p)) + ":" + hex(api.u32_at(p + 2 * ps)) + ":" +
                       hex(api.u32_at(p + 2 * ps + 4)) + ":" +
                       (data ? off(data) : hex(0xffffffffu));
                if (data) {
                    row += ":" + bytes_hex(api.bytes_at(static_cast<GuestAddr>(data),
                                                        api.u32_at(p + 2 * ps)));
                }
            }
            out.push_back(row);
        }
    }
    const GuestAddr spec = api.alloc(4, 4);
    api.write_u32(spec, 1);
    set_last_error(0xdead);
    ok = set(fresh, 6, 0, spec);
    set_last_error(0xdead);
    ok2 = get(fresh, 6, buf, 4);
    out.push_back("set6\t" + hex(ok) + "\t" + hex(ok2) + "\t" + hex(get_last_error()) + "\t" +
                  hex(api.u32_at(size_ptr)) + "\t" + hex(api.u32_at(buf)));
    const u8 ftb[8] = {0x44, 0x33, 0x22, 0x11, 0x00, 0x00, 0xd0, 0x01};
    ok = set(fresh, 27, 0, api.put_blob(api.put(ftb, 8), 8));
    ok2 = get(fresh, 27, buf, 16);
    out.push_back("set27\t" + hex(ok) + "\t" + hex(ok2) + "\t" + hex(api.u32_at(size_ptr)) + "\t" +
                  bytes_hex(api.bytes_at(buf, api.u32_at(size_ptr))));
    api.write_u32(buf, 5);
    ok = get(fresh, 14, buf, 4);
    out.push_back("get14\t" + hex(ok) + "\t" + hex(api.u32_at(size_ptr)) + "\t" + hex(api.u32_at(buf)));
    ok = set(fresh, 19, 0, empty_blob);
    set_last_error(0xdead);
    ok2 = get(fresh, 19, 0, 9);
    out.push_back("set19\t" + hex(ok) + "\t" + hex(ok2) + "\t" + hex(get_last_error()) + "\t" +
                  hex(api.u32_at(size_ptr)));
    const u8 eku[] = {0x30,0x0a,0x06,0x08,0x2b,0x06,0x01,0x05,0x05,0x07,0x03,0x02};
    ok = set(fresh, 9, 0, api.put_blob(api.put(eku, sizeof(eku)), sizeof(eku)));
    ok2 = get(fresh, 9, buf, 128);
    out.push_back("set9\t" + hex(ok) + "\t" + hex(ok2) + "\t" + hex(api.u32_at(size_ptr)) + "\t" +
                  bytes_hex(api.bytes_at(buf, api.u32_at(size_ptr))));
    api.call("CertFreeCertificateContext", {fresh});

    for (int which = 0; which < 2; ++which) {
        const GuestAddr c = which == 0
            ? create(kX509, kProbeLegacyNoSkiDer, sizeof(kProbeLegacyNoSkiDer))
            : create(kX509, kProbeEcDer, sizeof(kProbeEcDer));
        std::string row = std::string(which == 0 ? "legacy\t" : "ec\t") + hex(c != 0);
        if (c) {
            const GuestAddr ci = static_cast<GuestAddr>(api.ptr_at(c + 3 * ps));
            row += "\t" + hex(api.u32_at(ci + l.c_ext)) + "\t" + bytes_hex(blob_bytes(ci + l.serial));
            for (const u32 id : {15u, 20u, 25u, 28u, 89u, 92u, 93u, 124u}) {
                ok = get(c, id, buf, 128);
                row += "\t" + hex(id) + ":" + hex(ok) + ":" +
                       bytes_hex(ok ? api.bytes_at(buf, api.u32_at(size_ptr)) : std::vector<u8>{});
            }
            api.call("CertFreeCertificateContext", {c});
        }
        out.push_back(row);
    }

    const GuestAddr leaf_info = info;
    const GuestAddr inter_info = static_cast<GuestAddr>(api.ptr_at(inter + 3 * ps));
    const u8 x[4] = {1, 2, 3, 4};
    const u8 y[4] = {1, 2, 3, 5};
    const GuestAddr bx = api.put_blob(api.put(x, 4), 4);
    const GuestAddr by = api.put_blob(api.put(y, 4), 4);
    const GuestAddr bz = api.put_blob(0, 0);
    const GuestAddr bw = api.put_blob(api.put(x, 4), 0);
    auto cmp = [&](u32 enc, GuestAddr a, GuestAddr b) {
        return hex(api.call("CertCompareCertificateName", {enc, a, b}));
    };
    std::string row = "cmp\t" + cmp(kX509, leaf_info + l.issuer, inter_info + l.subject) + "\t" +
                      cmp(kX509, leaf_info + l.issuer, leaf_info + l.subject) + "\t" +
                      cmp(kX509, bx, by) + "\t" + cmp(kX509, bx, bx) + "\t" + cmp(kX509, bz, bw) +
                      "\t" + cmp(0, leaf_info + l.issuer, inter_info + l.subject) + "\t";
    set_last_error(0xdead);
    row += cmp(kX509, bx, by) + "\t" + hex(get_last_error());
    out.push_back(row);
    api.call("CertFreeCertificateContext", {leaf});
    api.call("CertFreeCertificateContext", {inter});
    return out;
}

TEST(Crypt32OracleReplay, CertificateContextAndPropertiesMatchWindowsX64) {
    expect_transcript(read_expected("crypt_prop_probe.expected"), replay_prop_probe(true));
}

TEST(Crypt32OracleReplay, CertificateContextAndPropertiesMatchWindowsX86) {
    expect_transcript(read_expected("crypt_prop_probe.x86.expected"), replay_prop_probe(false));
}

TEST(Crypt32OracleReplay, PropertyExportsHaveWincryptStdcallSizes) {
    CryptApi api(false);
    const std::pair<const char*, u16> exports[] = {
        {"CertGetCertificateContextProperty", 16}, {"CertSetCertificateContextProperty", 16},
        {"CertGetEnhancedKeyUsage", 16}, {"CertSetEnhancedKeyUsage", 8},
        {"CertCompareCertificateName", 12}, {"CertCreateCertificateContext", 12},
    };
    for (const auto& [name, bytes] : exports) {
        const ExportEntry* entry = api.find(name);
        ASSERT_NE(entry, nullptr) << name;
        EXPECT_EQ(entry->stdcall_bytes, bytes) << name;
    }
}


// ---------------------------------------------------------------------------
// tools/oracle_probe/crypt_chain_probe.c -> crypt_chain_probe{,.x86}.expected
// ---------------------------------------------------------------------------
//
// Every scenario except "bnet": that one chains a real public certificate to
// the host's copy of Amazon Root CA 1, which on Windows carries the Microsoft
// root program's EKU properties and on macOS carries none, so only its root
// line (and the intermediate's usage order derived from it) can differ.

struct ChainReplay {
    CryptApi api;
    u32 ps;
    std::vector<std::string> out;
    std::vector<std::pair<std::string, std::vector<u8>>> known;

    explicit ChainReplay(bool is64) : api(is64), ps(api.ptr_size()) {
        known = {{"root", {kProbeRootDer, kProbeRootDer + sizeof(kProbeRootDer)}},
                 {"inter", {kProbeIntermediateDer, kProbeIntermediateDer + sizeof(kProbeIntermediateDer)}},
                 {"leaf", {kProbeLeafDer, kProbeLeafDer + sizeof(kProbeLeafDer)}},
                 {"expired", {kProbeExpiredLeafDer, kProbeExpiredLeafDer + sizeof(kProbeExpiredLeafDer)}},
                 {"codesign", {kProbeCodeSignLeafDer, kProbeCodeSignLeafDer + sizeof(kProbeCodeSignLeafDer)}},
                 {"badsig", {kProbeBadSignatureLeafDer, kProbeBadSignatureLeafDer + sizeof(kProbeBadSignatureLeafDer)}},
                 {"self", {kProbeSelfSignedDer, kProbeSelfSignedDer + sizeof(kProbeSelfSignedDer)}}};
    }

    std::string name_of(GuestAddr context) {
        if (!context) return "null";
        const auto der = api.bytes_at(static_cast<GuestAddr>(api.ptr_at(context + ps)),
                                      api.u32_at(context + 2 * ps));
        for (const auto& [name, bytes] : known) if (bytes == der) return name;
        return "other";
    }
    std::string usage(GuestAddr u) {
        if (!u) return "-";
        const u32 count = api.u32_at(u);
        std::string text = hex(count);
        const GuestAddr array = static_cast<GuestAddr>(api.ptr_at(u + ps));
        for (u32 i = 0; i < count; ++i) {
            text += "," + api.cstring_at(static_cast<GuestAddr>(api.ptr_at(array + i * ps)));
        }
        return text;
    }
    GuestAddr store() {
        return static_cast<GuestAddr>(api.call("CertOpenStore", {2, 0, 0, 0, 0}));
    }
    GuestAddr add(GuestAddr store, const u8* der, u32 size) {
        const GuestAddr data = api.put(der, size);
        const GuestAddr out_ptr = api.alloc(8, 8);
        api.call("CertAddEncodedCertificateToStore", {store, 1, data, size, 4, out_ptr});
        return static_cast<GuestAddr>(api.ptr_at(out_ptr));
    }
    GuestAddr create(const u8* der, u32 size) {
        return static_cast<GuestAddr>(
            api.call("CertCreateCertificateContext", {1, api.put(der, size), size}));
    }
    GuestAddr filetime(u32 low, u32 high) {
        const GuestAddr at = api.alloc(8, 8);
        api.write_u32(at, low);
        api.write_u32(at + 4, high);
        return at;
    }
    std::string policy(const char* label, GuestAddr chain, u32 oid) {
        const GuestAddr para = api.alloc(16, 8);
        api.write_u32(para, api.is64() ? 16 : 12);
        const GuestAddr status = api.alloc(api.is64() ? 24 : 20, 8);
        for (u32 i = 0; i < 16; i += 4) api.write_u32(status + i, 0xcccccccc);
        api.write_u32(status, api.is64() ? 24 : 20);
        const u64 ok = api.call("CertVerifyCertificateChainPolicy", {oid, chain, para, status});
        return std::string(label) + hex(ok) + ":" + hex(api.u32_at(status + 4)) + ":" +
               hex(api.u32_at(status + 8)) + ":" + hex(api.u32_at(status + 12));
    }
    void run(const char* name, GuestAddr engine, GuestAddr cert, GuestAddr when,
             GuestAddr extra, const char* want, u32 usage_type, u32 flags) {
        const GuestAddr para = api.alloc(api.is64() ? 32 : 16, 8);
        api.write_u32(para, api.is64() ? 32 : 16);
        if (want) {
            const GuestAddr ids = api.alloc(ps, 8);
            api.write_ptr(ids, api.put_string(want));
            api.write_u32(para + ps, usage_type);
            api.write_u32(para + 2 * ps, 1);
            api.write_ptr(para + 3 * ps, ids);
        }
        const GuestAddr out_ptr = api.alloc(8, 8);
        api.write_ptr(out_ptr, 1);
        set_last_error(0xdead);
        const u64 ok = api.call("CertGetCertificateChain",
                                {engine, cert, when, extra, para, flags, 0, out_ptr});
        const u32 err = get_last_error();
        const GuestAddr chain = static_cast<GuestAddr>(api.ptr_at(out_ptr));
        std::string line = std::string("chain ") + name + "\t" + hex(ok) + "\t" + hex(err);
        if (!ok) {
            out.push_back(line + "\t" + hex(chain == 0));
            return;
        }
        line += "\t" + hex(api.u32_at(chain)) + "\t" + hex(api.u32_at(chain + 4)) + "\t" +
                hex(api.u32_at(chain + 8)) + "\t" + hex(api.u32_at(chain + 12)) + "\t" +
                hex(api.u32_at(chain + (api.is64() ? 24 : 20))) + "\t" +
                hex(api.u32_at(chain + (api.is64() ? 40 : 28))) + "\t" +
                hex(api.u32_at(chain + (api.is64() ? 48 : 36)));
        out.push_back(line);
        const GuestAddr chains = static_cast<GuestAddr>(api.ptr_at(chain + 16));
        for (u32 si = 0; si < api.u32_at(chain + 12); ++si) {
            const GuestAddr simple = static_cast<GuestAddr>(api.ptr_at(chains + si * ps));
            out.push_back("  simple " + hex(si) + "\t" + hex(api.u32_at(simple)) + "\t" +
                          hex(api.u32_at(simple + 4)) + "\t" + hex(api.u32_at(simple + 8)) +
                          "\t" + hex(api.u32_at(simple + 12)) + "\t" +
                          hex(api.ptr_at(simple + (api.is64() ? 24 : 20)) != 0) + "\t" +
                          hex(api.u32_at(simple + (api.is64() ? 32 : 24))));
            const GuestAddr elements = static_cast<GuestAddr>(api.ptr_at(simple + 16));
            for (u32 ei = 0; ei < api.u32_at(simple + 12); ++ei) {
                const GuestAddr el = static_cast<GuestAddr>(api.ptr_at(elements + ei * ps));
                const GuestAddr context = static_cast<GuestAddr>(api.ptr_at(el + ps));
                const u32 rev_field = 2 * ps + 8;
                const GuestAddr rev = static_cast<GuestAddr>(api.ptr_at(el + rev_field));
                std::string row = "    elem " + hex(ei) + "\t" + hex(api.u32_at(el)) + "\t" +
                                  name_of(context) + "\t" + hex(context == cert) + "\t" +
                                  hex(api.u32_at(el + 2 * ps)) + "\t" +
                                  hex(api.u32_at(el + 2 * ps + 4)) + "\t";
                if (rev) {
                    row += "rev:" + hex(api.u32_at(rev)) + ":" + hex(api.u32_at(rev + 4)) + ":" +
                           hex(api.ptr_at(rev + 8) != 0) + ":" +
                           hex(api.ptr_at(rev + (api.is64() ? 32 : 24)) != 0);
                } else {
                    row += "rev:-";
                }
                row += "\tiss:" + usage(static_cast<GuestAddr>(api.ptr_at(el + rev_field + ps))) +
                       "\tapp:" + usage(static_cast<GuestAddr>(api.ptr_at(el + rev_field + 2 * ps))) +
                       "\text:" + (api.ptr_at(el + rev_field + 3 * ps) ? "?" : "-");
                out.push_back(row);
            }
        }
        out.push_back("  " + policy("base:", chain, 1) + "\t" + policy("ssl:", chain, 4) +
                      "\t" + policy("auth:", chain, 2));
        const u64 dup = api.call("CertDuplicateCertificateChain", {chain});
        out.push_back("  dup\t" + hex(dup == chain));
        api.call("CertFreeCertificateChain", {dup});
        api.call("CertFreeCertificateChain", {chain});
    }
};

std::vector<std::string> replay_chain_probe(bool is64) {
    ChainReplay r(is64);
    auto& api = r.api;
    const u32 ps = r.ps;
    r.out.push_back(std::string("layout\t") + (is64 ? "48\t28\t38\t20\t28\t58\t18"
                                                    : "38\t20\t20\t10\t1c\t34\t14"));
    const GuestAddr loose = r.store();
    const GuestAddr leaf = r.add(loose, kProbeLeafDer, sizeof(kProbeLeafDer));
    r.add(loose, kProbeExpiredLeafDer, sizeof(kProbeExpiredLeafDer));
    r.add(loose, kProbeCodeSignLeafDer, sizeof(kProbeCodeSignLeafDer));
    const GuestAddr badsig = r.add(loose, kProbeBadSignatureLeafDer, sizeof(kProbeBadSignatureLeafDer));
    r.add(loose, kProbeSelfSignedDer, sizeof(kProbeSelfSignedDer));
    r.add(loose, kProbeRootDer, sizeof(kProbeRootDer));
    r.add(loose, kProbeIntermediateDer, sizeof(kProbeIntermediateDer));
    const GuestAddr inter_only = r.store();
    r.add(inter_only, kProbeIntermediateDer, sizeof(kProbeIntermediateDer));
    const GuestAddr full = r.store();
    r.add(full, kProbeIntermediateDer, sizeof(kProbeIntermediateDer));
    r.add(full, kProbeRootDer, sizeof(kProbeRootDer));
    const GuestAddr roots = r.store();
    r.add(roots, kProbeRootDer, sizeof(kProbeRootDer));
    const GuestAddr now2025 = r.filetime(0x1e1a8000, 0x01dbd288);
    const GuestAddr early2020 = r.filetime(0x97cf0000, 0x01d637a7);
    const GuestAddr lone = r.create(kProbeLeafDer, sizeof(kProbeLeafDer));
    const GuestAddr lone_bad = r.create(kProbeBadSignatureLeafDer, sizeof(kProbeBadSignatureLeafDer));
    const GuestAddr lone_expired = r.create(kProbeExpiredLeafDer, sizeof(kProbeExpiredLeafDer));
    const GuestAddr lone_code = r.create(kProbeCodeSignLeafDer, sizeof(kProbeCodeSignLeafDer));
    const GuestAddr lone_self = r.create(kProbeSelfSignedDer, sizeof(kProbeSelfSignedDer));
    const GuestAddr lone_root = r.create(kProbeRootDer, sizeof(kProbeRootDer));
    const char* server = "1.3.6.1.5.5.7.3.1";
    const char* client = "1.3.6.1.5.5.7.3.2";
    r.run("leaf_alone", 0, lone, now2025, 0, nullptr, 0, 0);
    r.run("leaf_inter", 0, lone, now2025, inter_only, nullptr, 0, 0);
    r.run("leaf_full", 0, lone, now2025, full, nullptr, 0, 0);
    r.run("leaf_full_nulltime", 0, lone, 0, full, nullptr, 0, 0);
    r.run("badsig_full", 0, lone_bad, now2025, full, nullptr, 0, 0);
    r.run("expired_full", 0, lone_expired, now2025, full, nullptr, 0, 0);
    r.run("leaf_full_2020", 0, lone, early2020, full, nullptr, 0, 0);
    r.run("usage_server", 0, lone, now2025, full, server, 0, 0);
    r.run("usage_client", 0, lone, now2025, full, client, 0, 0);
    r.run("usage_client_or", 0, lone, now2025, full, client, 1, 0);
    r.run("codesign_server", 0, lone_code, now2025, full, server, 0, 0);
    r.run("self_alone", 0, lone_self, now2025, 0, nullptr, 0, 0);
    r.run("root_alone", 0, lone_root, now2025, 0, nullptr, 0, 0);
    r.run("rev_chain", 0, lone, now2025, full, nullptr, 0, 0x20000000);
    r.run("rev_exclude_root", 0, lone, now2025, full, nullptr, 0, 0x40000000);
    r.run("rev_end_cert", 0, lone, now2025, full, nullptr, 0, 0x10000000);
    r.run("rev_cache_only", 0, lone, now2025, full, nullptr, 0, 0xc0000000);
    r.run("store_leaf", 0, leaf, now2025, 0, nullptr, 0, 0);
    r.run("store_badsig", 0, badsig, now2025, 0, nullptr, 0, 0);

    const u32 config_size = is64 ? 0x58 : 0x34;
    const GuestAddr config = api.alloc(0x80, 8);
    api.write_u32(config, config_size);
    api.write_ptr(config + (is64 ? 0x40 : 0x28), roots);
    const GuestAddr engine_out = api.alloc(8, 8);
    set_last_error(0xdead);
    const u64 made = api.call("CertCreateCertificateChainEngine", {config, engine_out});
    const GuestAddr engine = static_cast<GuestAddr>(api.ptr_at(engine_out));
    r.out.push_back("engine\t" + hex(made) + "\t" + hex(made ? 0 : get_last_error()) + "\t" +
                    hex(engine != 0));
    r.run("excl_leaf_inter", engine, lone, now2025, inter_only, nullptr, 0, 0);
    r.run("excl_leaf_alone", engine, lone, now2025, 0, nullptr, 0, 0);
    r.run("excl_expired", engine, lone_expired, now2025, inter_only, nullptr, 0, 0);
    r.run("excl_rev", engine, lone, now2025, inter_only, nullptr, 0, 0x40000000);
    r.run("excl_usage_client", engine, lone, now2025, inter_only, client, 0, 0);
    {
        const GuestAddr para = api.alloc(32, 8);
        api.write_u32(para, is64 ? 32 : 16);
        const GuestAddr out_ptr = api.alloc(8, 8);
        api.call("CertGetCertificateChain", {engine, lone, now2025, inter_only, para, 0, 0, out_ptr});
        const GuestAddr chain = static_cast<GuestAddr>(api.ptr_at(out_ptr));
        const std::u16string names[] = {u"vitrum-probe.test", u"VITRUM-PROBE.TEST", u"other.test",
                                        u"x.vitrum-probe.test", u""};
        for (int i = 0; i < 5; ++i) {
            const GuestAddr ssl = api.alloc(24, 8);
            api.write_u32(ssl, is64 ? 24 : 16);
            api.write_u32(ssl + 4, 2);  // AUTHTYPE_SERVER
            api.write_ptr(ssl + (is64 ? 16 : 12), api.put_wstring(names[i]));
            const GuestAddr pp = api.alloc(16, 8);
            api.write_u32(pp, is64 ? 16 : 12);
            api.write_ptr(pp + 8, ssl);
            const GuestAddr st = api.alloc(24, 8);
            api.write_u32(st, is64 ? 24 : 20);
            const u64 ok = api.call("CertVerifyCertificateChainPolicy", {4, chain, pp, st});
            r.out.push_back("sslname\t" + hex(i) + "\t" + hex(ok) + "\t" + hex(api.u32_at(st + 4)) +
                            "\t" + hex(api.u32_at(st + 8)) + "\t" + hex(api.u32_at(st + 12)));
        }
        api.call("CertFreeCertificateChain", {chain});
    }
    api.call("CertFreeCertificateChainEngine", {engine});
    const u32 sizes64[] = {4, 0x40, 0x50, 0x58, 0x60};
    const u32 sizes32[] = {4, 0x28, 0x30, 0x34, 0x38};
    for (int i = 0; i < 5; ++i) {
        api.write_u32(config, is64 ? sizes64[i] : sizes32[i]);
        set_last_error(0xdead);
        const u64 ok = api.call("CertCreateCertificateChainEngine", {config, engine_out});
        r.out.push_back("engine_cb\t" + hex(i) + "\t" + hex(ok) + "\t" + hex(ok ? 0 : get_last_error()));
        if (ok) api.call("CertFreeCertificateChainEngine", {api.ptr_at(engine_out)});
    }
    {
        const GuestAddr out_ptr = api.alloc(8, 8);
        api.write_ptr(out_ptr, 1);
        set_last_error(0xdead);
        u64 ok = api.call("CertGetCertificateChain", {0, lone, 0, 0, 0, 0, 0, out_ptr});
        r.out.push_back("nullpara\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" +
                        hex(api.ptr_at(out_ptr) == 0));
        const GuestAddr para = api.alloc(32, 8);
        api.write_ptr(out_ptr, 1);
        set_last_error(0xdead);
        ok = api.call("CertGetCertificateChain", {0, lone, 0, 0, para, 0, 0, out_ptr});
        r.out.push_back("cbsize0\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" +
                        hex(api.ptr_at(out_ptr) == 0));
        if (ok) api.call("CertFreeCertificateChain", {api.ptr_at(out_ptr)});
    }
    (void)ps;
    return r.out;
}

/// The Windows transcript minus the "bnet" block (see ChainReplay above) and
/// the lines after it, which the replay covers separately.
std::vector<std::string> chain_expected(const char* file) {
    std::vector<std::string> kept;
    bool in_bnet = false;
    for (const auto& line : read_expected(file)) {
        if (line.rfind("chain bnet", 0) == 0) { in_bnet = true; continue; }
        if (in_bnet && line.rfind("  ", 0) == 0) continue;
        in_bnet = false;
        if (line.rfind("after", 0) == 0 || line.rfind("exception", 0) == 0 ||
            line.rfind("nullcert", 0) == 0) {
            continue;
        }
        kept.push_back(line);
    }
    return kept;
}

TEST(Crypt32OracleReplay, CertificateChainMatchesWindowsX64) {
    expect_transcript(chain_expected("crypt_chain_probe.expected"), replay_chain_probe(true));
}

TEST(Crypt32OracleReplay, CertificateChainMatchesWindowsX86) {
    expect_transcript(chain_expected("crypt_chain_probe.x86.expected"), replay_chain_probe(false));
}

TEST(Crypt32OracleReplay, CertGetCertificateChainNullContextFaults) {
    for (const bool is64 : {false, true}) {
        CryptApi api(is64);
        const GuestAddr para = api.alloc(32, 8);
        api.write_u32(para, is64 ? 32 : 16);
        const GuestAddr out_ptr = api.alloc(8, 8);
        api.cpu().pending_raise = {};
        api.call("CertGetCertificateChain", {0, 0, 0, 0, para, 0, 0, out_ptr});
        // crypt_chain_probe "exception c0000005" (Windows dereferences it).
        EXPECT_TRUE(api.cpu().pending_raise.active);
        EXPECT_EQ(api.cpu().pending_raise.code, 0xC0000005u);
    }
}

TEST(Crypt32OracleReplay, ChainFreeReleasesItsMemory) {
    // CertFreeCertificateChain must release the chain: thousands of
    // verifications never run out of memory (the old 64 KiB scratch arena
    // failed after ~350).
    CryptApi api(true);
    const GuestAddr der = api.put(kProbeLeafDer, sizeof(kProbeLeafDer));
    const GuestAddr leaf = static_cast<GuestAddr>(
        api.call("CertCreateCertificateContext", {1, der, sizeof(kProbeLeafDer)}));
    ASSERT_NE(leaf, 0u);
    const GuestAddr para = api.alloc(32, 8);
    api.write_u32(para, 32);
    const GuestAddr out_ptr = api.alloc(8, 8);
    for (int i = 0; i < 2000; ++i) {
        ASSERT_EQ(api.call("CertGetCertificateChain", {0, leaf, 0, 0, para, 0, 0, out_ptr}), 1u) << i;
        api.call("CertFreeCertificateChain", {api.ptr_at(out_ptr)});
    }
    api.call("CertFreeCertificateContext", {leaf});
}


// ---------------------------------------------------------------------------
// tools/oracle_probe/crypt_wvt_probe.c -> crypt_wvt_probe{,.x86}.expected
// ---------------------------------------------------------------------------
//
// The fixtures (crypt_wvt_*.exe, made by crypt_authenticode_sign.py) are
// served from a capsule drive. The "trusted" case (a copy of a publicly
// trusted, vendor-signed binary that is not committed) and DRIVER_ACTION_VERIFY
// (a policy Vitreo does not implement; it fails loudly) are left out.

std::vector<std::string> replay_wvt_probe(bool is64) {
    const std::string directory =
        (std::filesystem::temp_directory_path() / ("vitreo-wvt-" + std::to_string(::getpid()) +
                                                   (is64 ? "-64" : "-32"))).string();
    std::filesystem::create_directories(directory);
    for (const char* name : {"crypt_wvt_unsigned.exe", "crypt_wvt_signed.exe",
                             "crypt_wvt_tampered.exe", "crypt_wvt_badsig.exe",
                             "crypt_wvt_badmd.exe", "crypt_wvt_withroot.exe",
                             "crypt_wvt_text.txt"}) {
        std::filesystem::copy_file(std::string(VITREO_ORACLE_PROBE_DIR) + "/" + name,
                                   directory + "/" + name,
                                   std::filesystem::copy_options::overwrite_existing);
    }
    capsule::Vfs vfs;
    EXPECT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(directory)).is_ok());
    CryptApi api(is64);
    api.manager().set_vfs(&vfs);
    const u32 ps = api.ptr_size();
    std::vector<std::string> out;
    out.push_back(std::string("layout\t") + (is64 ? "50\t20\te0\t40\t58" : "30\t10\t80\t2c\t3c"));
    const u8 v2_bytes[16] = {0x6b, 0xc5, 0xaa, 0x00, 0x44, 0xcd, 0xd0, 0x11,
                             0x8c, 0xc2, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee};
    const GuestAddr v2 = api.put(v2_bytes, 16);
    const u32 wd_size = is64 ? 0x50 : 0x30;
    const u32 off_ui = is64 ? 24 : 12;
    const u32 off_union = is64 ? 40 : 24;
    const u32 off_state = is64 ? 56 : 32;
    auto verify = [&](GuestAddr action, const std::u16string& name, u32 state_action,
                      u32 revocation, GuestAddr* state, u32 cb) -> u32 {
        const GuestAddr file = api.alloc(4 * ps, 8);
        api.write_u32(file, 4 * ps);
        api.write_ptr(file + ps, api.put_wstring(u"C:\\" + name));
        const GuestAddr wd = api.alloc(0x60, 8);
        api.write_u32(wd, cb);
        api.write_u32(wd + off_ui, 2);                  // WTD_UI_NONE
        api.write_u32(wd + off_ui + 4, revocation);
        api.write_u32(wd + off_ui + 8, 1);              // WTD_CHOICE_FILE
        api.write_ptr(wd + off_union, file);
        api.write_u32(wd + off_union + ps, state_action);
        api.write_ptr(wd + off_state, state ? *state : 0);
        api.write_u32(wd + off_state + 2 * ps, 0x1000); // WTD_CACHE_ONLY_URL_RETRIEVAL
        const u32 r = static_cast<u32>(api.call("WinVerifyTrust", {~0ull, action, wd}));
        if (state) *state = static_cast<GuestAddr>(api.ptr_at(wd + off_state));
        return r;
    };
    auto provider = [&](GuestAddr state) {
        const GuestAddr pd = static_cast<GuestAddr>(api.call("WTHelperProvDataFromStateData", {state}));
        std::string row = "  provider\t" + hex(pd != 0);
        if (!pd) { out.push_back(row); return; }
        row += "\t" + hex(api.u32_at(pd)) + "\t" + hex(api.u32_at(pd + (is64 ? 48 : 24))) + "\t" +
               hex(api.u32_at(pd + (is64 ? 204 : 112))) + "\t" +
               hex(api.u32_at(pd + (is64 ? 120 : 64))) + "\t" +
               hex(api.u32_at(pd + (is64 ? 200 : 108))) + "\t" +
               hex(api.u32_at(pd + (is64 ? 16 : 8)));
        const GuestAddr sg = static_cast<GuestAddr>(
            api.call("WTHelperGetProvSignerFromChain", {pd, 0, 0, 0}));
        row += "\t" + hex(sg != 0);
        if (sg) {
            row += "\t" + hex(api.u32_at(sg)) + "\t" + hex(api.u32_at(sg + 12)) + "\t" +
                   hex(api.u32_at(sg + (is64 ? 24 : 20))) + "\t" +
                   hex(api.u32_at(sg + (is64 ? 44 : 32))) + "\t" +
                   hex(api.u32_at(sg + (is64 ? 40 : 28))) + "\t" +
                   hex(api.ptr_at(sg + (is64 ? 56 : 40)) != 0) + "\t" +
                   hex(api.ptr_at(sg + (is64 ? 32 : 24)) != 0);
            for (u32 i = 0; i < api.u32_at(sg + 12) && i < 4; ++i) {
                const GuestAddr pc = static_cast<GuestAddr>(
                    api.call("WTHelperGetProvCertFromChain", {sg, i}));
                row += "\tcert" + hex(i) + ":";
                const GuestAddr cert = pc ? static_cast<GuestAddr>(api.ptr_at(pc + ps)) : 0;
                if (!cert) { row += "-"; continue; }
                const GuestAddr name = api.alloc(96, 8);
                api.call("CertGetNameStringA", {cert, 4, 0, 0, name, 96});
                row += api.cstring_at(name) + ":";
                const GuestAddr hash = api.alloc(20, 8);
                const GuestAddr size = api.alloc(4, 4);
                api.write_u32(size, 20);
                if (api.call("CertGetCertificateContextProperty", {cert, 3, hash, size})) {
                    row += bytes_hex(api.bytes_at(hash, 4));
                }
                row += ":" + hex(api.u32_at(pc)) + ":" + hex(api.u32_at(pc + 2 * ps + 8)) + ":" +
                       hex(api.u32_at(pc + 2 * ps + 4)) + ":" + hex(api.u32_at(pc + 2 * ps + 24)) +
                       ":" + hex(api.u32_at(pc + 2 * ps)) + ":" + hex(api.u32_at(pc + 2 * ps + 20)) +
                       ":" + hex(api.ptr_at(pc + (is64 ? 80 : 56)) != 0);
            }
        }
        out.push_back(row);
    };
    auto file_case = [&](const char* label, const std::u16string& name) {
        GuestAddr state = 0;
        set_last_error(0xdead);
        u32 r = verify(v2, name, 1, 0, &state, wd_size);
        out.push_back(std::string("file ") + label + "\t" + hex(r) + "\t" + hex(get_last_error()) +
                      "\t" + hex(state != 0));
        if (state) provider(state);
        set_last_error(0xdead);
        r = verify(v2, name, 2, 0, &state, wd_size);
        out.push_back("  close\t" + hex(r) + "\t" + hex(get_last_error()) + "\t" + hex(state != 0));
        set_last_error(0xdead);
        r = verify(v2, name, 0, 0, nullptr, wd_size);
        out.push_back("  ignore\t" + hex(r) + "\t" + hex(get_last_error()));
        set_last_error(0xdead);
        r = verify(v2, name, 0, 1, nullptr, wd_size);
        out.push_back("  revoke\t" + hex(r) + "\t" + hex(get_last_error()));
    };
    file_case("unsigned", u"crypt_wvt_unsigned.exe");
    file_case("signed", u"crypt_wvt_signed.exe");
    file_case("tampered", u"crypt_wvt_tampered.exe");
    file_case("missing", u"crypt_wvt_missing.exe");
    file_case("badsig", u"crypt_wvt_badsig.exe");
    file_case("badmd", u"crypt_wvt_badmd.exe");
    file_case("withroot", u"crypt_wvt_withroot.exe");
    file_case("text", u"crypt_wvt_text.txt");
    const u8 unknown_bytes[16] = {0x67, 0x45, 0x23, 0x01, 0xab, 0x89, 0xef, 0xcd,
                                  0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    const u8 chain_bytes[16] = {0x16, 0x1c, 0x45, 0xfc, 0x75, 0xac, 0xd1, 0x11,
                                0xb4, 0xb8, 0x00, 0xc0, 0x4f, 0xb6, 0x6e, 0xa0};
    const std::pair<const char*, const u8*> actions[] = {{"unknown", unknown_bytes},
                                                         {"chain_verify", chain_bytes}};
    for (const auto& [name, bytes] : actions) {
        set_last_error(0xdead);
        const u32 r = verify(api.put(bytes, 16), u"crypt_wvt_signed.exe", 0, 0, nullptr, wd_size);
        out.push_back(std::string("action ") + name + "\t" + hex(r) + "\t" + hex(get_last_error()));
    }
    const u32 sizes[] = {0, 4, 8, 0xc, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30};
    for (const u32 size : sizes) {
        set_last_error(0xdead);
        const u32 r = verify(v2, u"crypt_wvt_signed.exe", 0, 0, nullptr, size);
        out.push_back("cbstruct\t" + hex(size) + "\t" + hex(r) + "\t" + hex(get_last_error()));
    }
    api.cpu().pending_raise = {};
    api.call("WinVerifyTrust", {~0ull, v2, 0});
    if (api.cpu().pending_raise.active) {
        out.push_back("exception\t" + hex(api.cpu().pending_raise.code));
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return out;
}

std::vector<std::string> wvt_expected(const char* file) {
    std::vector<std::string> kept;
    bool skipping = false;
    for (const auto& line : read_expected(file)) {
        if (line.rfind("file trusted", 0) == 0) { skipping = true; continue; }
        if (skipping && line.rfind("  ", 0) == 0) continue;
        skipping = false;
        if (line.rfind("action driver", 0) == 0) continue;
        kept.push_back(line);
    }
    return kept;
}

TEST(Crypt32OracleReplay, WinVerifyTrustMatchesWindowsX64) {
    expect_transcript(wvt_expected("crypt_wvt_probe.expected"), replay_wvt_probe(true));
}

TEST(Crypt32OracleReplay, WinVerifyTrustMatchesWindowsX86) {
    expect_transcript(wvt_expected("crypt_wvt_probe.x86.expected"), replay_wvt_probe(false));
}

// ---------------------------------------------------------------------------
// Cabinet subjects: oracle/crypt_cab_wvt_probe.c over fixtures/cab/*.cab
// (made by oracle/crypt_cab_sign.py) -> oracle/crypt_cab_wvt.expected
// ---------------------------------------------------------------------------

std::vector<std::string> replay_cab_wvt(bool is64, std::vector<std::string>& expected) {
    const std::filesystem::path here = std::filesystem::path(__FILE__).parent_path();
    const std::string directory =
        (std::filesystem::temp_directory_path() / ("vitreo-cabwvt-" + std::to_string(::getpid()) +
                                                   (is64 ? "-64" : "-32"))).string();
    std::filesystem::create_directories(directory);
    std::ifstream in(here / "oracle" / "crypt_cab_wvt.expected");
    std::vector<std::string> names;
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) continue;
        expected.push_back(line);
        names.push_back(line.substr(0, line.find('\t')));
        std::filesystem::copy_file(here / "fixtures" / "cab" / names.back(), directory + "/" + names.back(),
                                   std::filesystem::copy_options::overwrite_existing);
    }
    capsule::Vfs vfs;
    EXPECT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(directory)).is_ok());
    CryptApi api(is64);
    api.manager().set_vfs(&vfs);
    const u32 ps = api.ptr_size();
    const u8 v2_bytes[16] = {0x6b, 0xc5, 0xaa, 0x00, 0x44, 0xcd, 0xd0, 0x11,
                             0x8c, 0xc2, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee};
    const GuestAddr v2 = api.put(v2_bytes, 16);
    std::vector<std::string> out;
    for (const auto& name : names) {
        const GuestAddr file = api.alloc(4 * ps, 8);
        api.write_u32(file, 4 * ps);
        api.write_ptr(file + ps, api.put_wstring(u"C:\\" + std::u16string(name.begin(), name.end())));
        const GuestAddr wd = api.alloc(0x60, 8);
        api.write_u32(wd, is64 ? 0x50 : 0x30);
        api.write_u32(wd + (is64 ? 24 : 12), 2);      // WTD_UI_NONE
        api.write_u32(wd + (is64 ? 24 : 12) + 4, 0);  // WTD_REVOKE_NONE
        api.write_u32(wd + (is64 ? 24 : 12) + 8, 1);  // WTD_CHOICE_FILE
        api.write_ptr(wd + (is64 ? 40 : 24), file);
        const u32 r = static_cast<u32>(api.call("WinVerifyTrust", {~0ull, v2, wd}));
        char row[16];
        std::snprintf(row, sizeof(row), "%08x", r);
        out.push_back(name + "\t" + row);
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return out;
}

TEST(Crypt32OracleReplay, WinVerifyTrustCabinetMatchesWindows) {
    for (bool is64 : {true, false}) {
        std::vector<std::string> expected;
        const auto actual = replay_cab_wvt(is64, expected);
        ASSERT_EQ(expected.size(), 8u);
        EXPECT_EQ(actual, expected) << (is64 ? "x64" : "x86");
    }
}

// ---------------------------------------------------------------------------
// Structured-storage (Windows Installer) subjects: fixtures/msi (made by
// oracle/crypt_msi_sign.py). The digest rules were established against the
// messageDigest Windows wrote into 17 real signed packages (SHA-1 without
// and SHA-256 with \x05MsiDigitalSignatureEx; crypt_msi_sign.py --check).
// The rows follow the documented WinVerifyTrust results; they have not been
// replayed on winoracle yet (crypt_cab_wvt_probe.c takes these files as-is).
// ---------------------------------------------------------------------------

constexpr u32 kMsiUntrustedRoot = 0x800B0109u;  // digest and signature good, test root
constexpr u32 kMsiBadDigest = 0x80096010u;
constexpr u32 kMsiNoSignature = 0x800B0100u;
constexpr u32 kMsiSubjectFormUnknown = 0x800B0003u;

struct MsiWvt {
    const char* name;
    u32 result;
};
constexpr MsiWvt kMsiWvtRows[] = {
    {"msi_unsigned.msi", kMsiNoSignature},
    {"msi_sha1.msi", kMsiUntrustedRoot},
    {"msi_sha256.msi", kMsiUntrustedRoot},
    {"msi_sha1_ex.msi", kMsiUntrustedRoot},
    {"msi_sha256_ex.msi", kMsiUntrustedRoot},
    {"msi_stream_ex.msi", kMsiBadDigest},      // nested stream byte
    {"msi_big.msi", kMsiBadDigest},            // regular-sector stream byte
    {"msi_meta.msi", kMsiUntrustedRoot},       // storage time: not covered without Ex
    {"msi_meta_ex.msi", kMsiBadDigest},        // ... covered with Ex
    {"msi_clsid.msi", kMsiBadDigest},          // nested storage CLSID
    {"msi_ex_flip.msi", kMsiBadDigest},        // stored Ex != recomputed (unmeasured on Windows)
    {"msi_ex_only.msi", kMsiNoSignature},
    {"msi_garbage_sig.msi", kMsiNoSignature},
    {"msi_corrupt.msi", kMsiSubjectFormUnknown},
};

class MsiWvtHarness {
public:
    explicit MsiWvtHarness(bool is64) : m_api(is64) {
        const std::filesystem::path fixtures =
            std::filesystem::path(__FILE__).parent_path() / "fixtures" / "msi";
        m_directory = (std::filesystem::temp_directory_path() /
                       ("vitreo-msiwvt-" + std::to_string(::getpid()) + (is64 ? "-64" : "-32"))).string();
        std::filesystem::create_directories(m_directory);
        for (const auto& entry : std::filesystem::directory_iterator(fixtures)) {
            std::filesystem::copy_file(entry.path(), m_directory + "/" + entry.path().filename().string(),
                                       std::filesystem::copy_options::overwrite_existing);
        }
        EXPECT_TRUE(m_vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(m_directory)).is_ok());
        m_api.manager().set_vfs(&m_vfs);
        const u8 v2_bytes[16] = {0x6b, 0xc5, 0xaa, 0x00, 0x44, 0xcd, 0xd0, 0x11,
                                 0x8c, 0xc2, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee};
        m_v2 = m_api.put(v2_bytes, 16);
    }
    ~MsiWvtHarness() {
        std::error_code error;
        std::filesystem::remove_all(m_directory, error);
    }

    CryptApi& api() { return m_api; }
    std::string path(const char* name) const { return m_directory + "/" + name; }

    /// WinVerifyTrust(GENERIC_VERIFY_V2, no UI, no revocation) with the
    /// given state action; returns the result and leaves `wd` for CLOSE.
    u32 verify(const char* name, u32 state_action, GuestAddr& wd) {
        const u32 ps = m_api.ptr_size();
        const bool is64 = ps == 8;
        const GuestAddr file = m_api.alloc(4 * ps, 8);
        m_api.write_u32(file, 4 * ps);
        m_api.write_ptr(file + ps, m_api.put_wstring(u"C:\\" + std::u16string(name, name + std::strlen(name))));
        wd = m_api.alloc(0x60, 8);
        m_api.write_u32(wd, is64 ? 0x50 : 0x30);
        m_api.write_u32(wd + (is64 ? 24 : 12), 2);      // WTD_UI_NONE
        m_api.write_u32(wd + (is64 ? 24 : 12) + 4, 0);  // WTD_REVOKE_NONE
        m_api.write_u32(wd + (is64 ? 24 : 12) + 8, 1);  // WTD_CHOICE_FILE
        m_api.write_ptr(wd + (is64 ? 40 : 24), file);
        m_api.write_u32(wd + (is64 ? 40 : 24) + ps, state_action);
        return static_cast<u32>(m_api.call("WinVerifyTrust", {~0ull, m_v2, wd}));
    }
    u32 verify(const char* name) {
        GuestAddr wd = 0;
        return verify(name, 0, wd);  // WTD_STATEACTION_IGNORE
    }
    void close(GuestAddr wd) {
        m_api.write_u32(wd + (m_api.ptr_size() == 8 ? 48 : 28), 2);  // WTD_STATEACTION_CLOSE
        EXPECT_EQ(m_api.call("WinVerifyTrust", {~0ull, m_v2, wd}), 0u);
    }
    void trust_root() {
        std::ifstream root_file(path("root.der"), std::ios::binary);
        const std::vector<u8> root_der{std::istreambuf_iterator<char>(root_file), {}};
        ASSERT_FALSE(root_der.empty());
        const u64 root_store = m_api.call("CertOpenStore", {10, 0, 0, 0x10000, m_api.put_wstring(u"ROOT")});
        ASSERT_NE(root_store, 0u);
        ASSERT_EQ(m_api.call("CertAddEncodedCertificateToStore",
                             {root_store, 1, m_api.put(root_der.data(), root_der.size()), root_der.size(), 4, 0}),
                  1u);
    }

private:
    capsule::Vfs m_vfs;
    CryptApi m_api;
    std::string m_directory;
    GuestAddr m_v2{0};
};

TEST(Crypt32Authenticode, WinVerifyTrustStructuredStorageDigest) {
    for (bool is64 : {true, false}) {
        MsiWvtHarness h(is64);
        for (const auto& row : kMsiWvtRows) {
            EXPECT_EQ(h.verify(row.name), row.result) << row.name << (is64 ? " x64" : " x86");
            EXPECT_EQ(get_last_error(), row.result) << row.name << " (the result is also the last error)";
        }
    }
}

TEST(Crypt32Authenticode, WinVerifyTrustStructuredStorageTrustedRootAndState) {
    for (bool is64 : {true, false}) {
        MsiWvtHarness h(is64);
        h.trust_root();
        for (const char* name : {"msi_sha1.msi", "msi_sha256.msi", "msi_sha1_ex.msi", "msi_sha256_ex.msi",
                                 "msi_meta.msi"}) {
            EXPECT_EQ(h.verify(name), 0u) << name << (is64 ? " x64" : " x86");
        }
        EXPECT_EQ(h.verify("msi_meta_ex.msi"), kMsiBadDigest);

        // VERIFY hands out provider state with the package's signer; CLOSE frees it.
        GuestAddr wd = 0;
        EXPECT_EQ(h.verify("msi_sha256_ex.msi", 1, wd), 0u);  // WTD_STATEACTION_VERIFY
        CryptApi& api = h.api();
        const u64 state = api.ptr_at(wd + (is64 ? 56 : 32));
        ASSERT_NE(state, 0u);
        const GuestAddr pd = static_cast<GuestAddr>(api.call("WTHelperProvDataFromStateData", {state}));
        ASSERT_NE(pd, 0u);
        EXPECT_NE(api.call("WTHelperGetProvSignerFromChain", {pd, 0, 0, 0}), 0u);
        h.close(wd);
        EXPECT_EQ(api.ptr_at(wd + (is64 ? 56 : 32)), 0u);

        // A failed verification still creates state (crypt_wvt_probe).
        EXPECT_EQ(h.verify("msi_stream_ex.msi", 1, wd), kMsiBadDigest);
        EXPECT_NE(api.ptr_at(wd + (is64 ? 56 : 32)), 0u);
        h.close(wd);
    }
}

TEST(Crypt32Authenticode, CryptQueryObjectFindsStructuredStorageSignature) {
    for (bool is64 : {true, false}) {
        MsiWvtHarness h(is64);
        CryptApi& api = h.api();
        auto query = [&](const char* name, u32& content_type) {
            const GuestAddr outs = api.alloc(64, 8);
            const u64 ok = api.call("CryptQueryObject",
                                    {1, api.put_wstring(u"C:\\" + std::u16string(name, name + std::strlen(name))),
                                     0x400, 0x2, 0, outs, outs + 4, outs + 8, outs + 16, outs + 24, 0});
            content_type = api.u32_at(outs + 4);
            return ok;
        };
        u32 content_type = 0;
        EXPECT_EQ(query("msi_sha256_ex.msi", content_type), 1u) << (is64 ? "x64" : "x86");
        EXPECT_EQ(content_type, 10u);  // CERT_QUERY_CONTENT_PKCS7_SIGNED_EMBED
        EXPECT_EQ(query("msi_unsigned.msi", content_type), 0u);
        EXPECT_EQ(get_last_error(), 0x80092009u);  // CRYPT_E_NO_MATCH
        EXPECT_EQ(content_type, 0u);
    }
}

// ---------------------------------------------------------------------------
// RFC 3161 timestamps: fixtures/rfc3161 (made by oracle/crypt_rfc3161_sign.py)
// signed by a leaf that expired on 2021-01-01, under a test root trusted here.
// A valid timestamp moves the leaf's validity check to its genTime ("Time
// Stamping Authenticode Signatures", Learn); the TSTInfo is the full SEQUENCE
// inside the token's eContent OCTET STRING (RFC 3161 section 2.4.2).
// ---------------------------------------------------------------------------

TEST(Crypt32Authenticode, Rfc3161TimestampMovesTheValidityCheckToGenTime) {
    const std::filesystem::path fixtures =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / "rfc3161";
    for (bool is64 : {true, false}) {
        const std::string directory =
            (std::filesystem::temp_directory_path() / ("vitreo-rfc3161-" + std::to_string(::getpid()) +
                                                       (is64 ? "-64" : "-32"))).string();
        std::filesystem::create_directories(directory);
        for (const char* name : {"ts_none.exe", "ts_valid.exe", "ts_fraction.exe", "ts_badimprint.exe"}) {
            std::filesystem::copy_file(fixtures / name, directory + "/" + name,
                                       std::filesystem::copy_options::overwrite_existing);
        }
        capsule::Vfs vfs;
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(directory)).is_ok());
        CryptApi api(is64);
        api.manager().set_vfs(&vfs);
        const u32 ps = api.ptr_size();

        // Trust the test root: CertOpenStore(CERT_STORE_PROV_SYSTEM_W,
        // CERT_SYSTEM_STORE_CURRENT_USER, L"ROOT") + CERT_STORE_ADD_ALWAYS.
        std::ifstream root_file(fixtures / "root.der", std::ios::binary);
        const std::vector<u8> root_der{std::istreambuf_iterator<char>(root_file), {}};
        ASSERT_FALSE(root_der.empty());
        const u64 root_store = api.call("CertOpenStore", {10, 0, 0, 0x10000, api.put_wstring(u"ROOT")});
        ASSERT_NE(root_store, 0u);
        ASSERT_EQ(api.call("CertAddEncodedCertificateToStore",
                           {root_store, 1, api.put(root_der.data(), root_der.size()), root_der.size(), 4, 0}),
                  1u);

        const u8 v2_bytes[16] = {0x6b, 0xc5, 0xaa, 0x00, 0x44, 0xcd, 0xd0, 0x11,
                                 0x8c, 0xc2, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee};
        const GuestAddr v2 = api.put(v2_bytes, 16);
        // Returns WinVerifyTrust's result and, through the provider state,
        // the signer's sftVerifyAsOf and its counter-signer count.
        struct Outcome { u32 result; u64 verify_as_of; u32 counter_signers; };
        auto verify = [&](const char* name) {
            const GuestAddr file = api.alloc(4 * ps, 8);
            api.write_u32(file, 4 * ps);
            api.write_ptr(file + ps, api.put_wstring(u"C:\\" + std::u16string(name, name + std::strlen(name))));
            const GuestAddr wd = api.alloc(0x60, 8);
            const u32 off_union = is64 ? 40 : 24;
            api.write_u32(wd, is64 ? 0x50 : 0x30);
            api.write_u32(wd + (is64 ? 24 : 12), 2);      // WTD_UI_NONE
            api.write_u32(wd + (is64 ? 24 : 12) + 8, 1);  // WTD_CHOICE_FILE
            api.write_ptr(wd + off_union, file);
            api.write_u32(wd + off_union + ps, 1);        // WTD_STATEACTION_VERIFY
            Outcome o{static_cast<u32>(api.call("WinVerifyTrust", {~0ull, v2, wd})), 0, 0};
            const u64 state = api.ptr_at(wd + (is64 ? 56 : 32));
            const GuestAddr pd = static_cast<GuestAddr>(api.call("WTHelperProvDataFromStateData", {state}));
            const GuestAddr sg = pd ? static_cast<GuestAddr>(
                                          api.call("WTHelperGetProvSignerFromChain", {pd, 0, 0, 0}))
                                    : 0;
            if (sg) {
                o.verify_as_of = static_cast<u64>(api.u32_at(sg + 4)) |
                                 (static_cast<u64>(api.u32_at(sg + 8)) << 32);
                o.counter_signers = api.u32_at(sg + (is64 ? 44 : 32));
            }
            api.write_u32(wd + off_union + ps, 2);        // WTD_STATEACTION_CLOSE
            (void)api.call("WinVerifyTrust", {~0ull, v2, wd});
            return o;
        };
        constexpr u32 kCertEExpired = 0x800B0101u;
        constexpr u64 kGenTime = 132354864000000000ull;  // 2020-06-01T12:00:00Z

        const Outcome none = verify("ts_none.exe");
        EXPECT_EQ(none.result, kCertEExpired) << (is64 ? "x64" : "x86");
        EXPECT_EQ(none.counter_signers, 0u);

        const Outcome valid = verify("ts_valid.exe");
        EXPECT_EQ(valid.result, 0u) << (is64 ? "x64" : "x86");
        EXPECT_EQ(valid.verify_as_of, kGenTime);
        EXPECT_EQ(valid.counter_signers, 1u);

        EXPECT_EQ(verify("ts_fraction.exe").result, 0u) << (is64 ? "x64" : "x86");

        // An imprint that is not the hash of the signature does not timestamp
        // it, so the leaf is checked now and has expired.
        const Outcome bad = verify("ts_badimprint.exe");
        EXPECT_EQ(bad.result, kCertEExpired) << (is64 ? "x64" : "x86");
        EXPECT_NE(bad.verify_as_of, kGenTime);

        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
}


// ---------------------------------------------------------------------------
// tools/oracle_probe/crypt_msg_probe.c -> crypt_msg_probe{,.x86}.expected
// ---------------------------------------------------------------------------

std::vector<std::string> replay_msg_probe(bool is64) {
    const std::string directory =
        (std::filesystem::temp_directory_path() / ("vitreo-msg-" + std::to_string(::getpid()) +
                                                   (is64 ? "-64" : "-32"))).string();
    std::filesystem::create_directories(directory);
    for (const char* name : {"crypt_wvt_unsigned.exe", "crypt_wvt_signed.exe", "crypt_wvt_text.txt"}) {
        std::filesystem::copy_file(std::string(VITREO_ORACLE_PROBE_DIR) + "/" + name,
                                   directory + "/" + name,
                                   std::filesystem::copy_options::overwrite_existing);
    }
    capsule::Vfs vfs;
    EXPECT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(directory)).is_ok());
    CryptApi api(is64);
    api.manager().set_vfs(&vfs);
    const u32 ps = api.ptr_size();
    std::vector<std::string> out;
    out.push_back(std::string("layout\t") + (is64 ? "88\t18" : "44\tc"));
    const GuestAddr buf = api.alloc(8192, 16);
    const GuestAddr size_ptr = api.alloc(4, 4);
    auto get = [&](GuestAddr msg, u32 type, u32 index, GuestAddr data, u32 size) -> u64 {
        api.write_u32(size_ptr, size);
        return api.call("CryptMsgGetParam", {msg, type, index, data, size_ptr});
    };
    auto param = [&](GuestAddr msg, const char* label, u32 type, u32 index) {
        set_last_error(0xdead);
        u64 ok = get(msg, type, index, 0, 0);
        const u32 err = get_last_error();
        const u32 size = api.u32_at(size_ptr);
        std::string row = std::string("param ") + label + "\t" + hex(ok) + "\t" + hex(ok ? 0 : err) +
                          "\t" + hex(size != 0);
        if (ok && size) {
            set_last_error(0xdead);
            const u64 ok_small = get(msg, type, index, buf, 1);
            row += "\t" + hex(ok_small) + "\t" + hex(get_last_error()) + "\t" +
                   hex(api.u32_at(size_ptr) == size);
            set_last_error(0xdead);
            const u64 ok2 = get(msg, type, index, buf, 8192);
            const u32 got = api.u32_at(size_ptr);
            row += "\t" + hex(ok2) + "\t" + hex(get_last_error()) + "\t" + hex(got == size);
            if (type == 1 || type == 5 || type == 11) {
                row += "\t" + hex(api.u32_at(buf));
            } else if (type == 4) {
                row += "\t" + api.cstring_at(buf);
            } else if (type == 6) {
                auto blob = [&](GuestAddr at) {
                    return api.bytes_at(static_cast<GuestAddr>(api.ptr_at(at + ps)), api.u32_at(at));
                };
                row += "\t" + hex(api.u32_at(buf)) + "\t" + hex(api.u32_at(buf + ps)) + "\t" +
                       bytes_hex(blob(buf + 3 * ps)) + "\t" +
                       api.cstring_at(static_cast<GuestAddr>(api.ptr_at(buf + 5 * ps))) + "\t" +
                       hex(api.u32_at(buf + 6 * ps)) + "\t" +
                       api.cstring_at(static_cast<GuestAddr>(api.ptr_at(buf + 8 * ps))) + "\t" +
                       hex(api.u32_at(buf + 9 * ps)) + "\t" + hex(api.u32_at(buf + 11 * ps)) + "\t" +
                       hex(api.u32_at(buf + 13 * ps));
                const GuestAddr attrs = static_cast<GuestAddr>(api.ptr_at(buf + 14 * ps));
                for (u32 i = 0; i < api.u32_at(buf + 13 * ps); ++i) {
                    const GuestAddr a = attrs + i * 3 * ps;
                    const GuestAddr values = static_cast<GuestAddr>(api.ptr_at(a + 2 * ps));
                    row += "," + api.cstring_at(static_cast<GuestAddr>(api.ptr_at(a))) + ":" +
                           hex(api.u32_at(a + ps)) + ":" + hex(api.u32_at(values));
                }
                row += "\t" + hex(api.u32_at(buf + 15 * ps));
            } else if (type == 12) {
                row += "\t" + hex(api.u8_at(buf)) + "\t" + hex(got);
            } else if (type == 7) {
                const InfoLayout l(is64);
                row += "\t" + hex(api.u32_at(buf + l.issuer)) + "\t" +
                       bytes_hex(api.bytes_at(static_cast<GuestAddr>(api.ptr_at(buf + l.serial + ps)),
                                              api.u32_at(buf + l.serial)));
            } else {
                row += "\t" + hex(got);
            }
        }
        out.push_back(row);
    };
    auto query = [&](const char* label, const std::u16string& name) {
        const GuestAddr path = api.put_wstring(u"C:\\" + name);
        const GuestAddr outs = api.alloc(64, 8);
        api.write_u32(outs, 0xaaaa);
        api.write_u32(outs + 4, 0xbbbb);
        api.write_u32(outs + 8, 0xcccc);
        api.write_ptr(outs + 16, 1);
        api.write_ptr(outs + 24, 1);
        set_last_error(0xdead);
        const u64 ok = api.call("CryptQueryObject", {1, path, 0x400, 0x2, 0, outs, outs + 4, outs + 8,
                                                     outs + 16, outs + 24, 0});
        const u32 err = get_last_error();
        const u64 store = api.ptr_at(outs + 16);
        const u64 msg = api.ptr_at(outs + 24);
        out.push_back(std::string("query ") + label + "\t" + hex(ok) + "\t" + hex(ok ? 0 : err) + "\t" +
                      hex(api.u32_at(outs)) + "\t" + hex(api.u32_at(outs + 4)) + "\t" +
                      hex(api.u32_at(outs + 8)) + "\t" + hex(store != 0 && store != 1) + "\t" +
                      hex(msg != 0 && msg != 1));
        if (!ok) return;
        const GuestAddr m = static_cast<GuestAddr>(msg);
        param(m, "type", 1, 0);
        param(m, "inner", 4, 0);
        param(m, "signers", 5, 0);
        param(m, "signer0", 6, 0);
        param(m, "signer1", 6, 1);
        param(m, "certinfo0", 7, 0);
        param(m, "certs", 11, 0);
        param(m, "cert0", 12, 0);
        param(m, "cert1", 12, 1);
        param(m, "cert9", 12, 9);
        param(m, "content", 2, 0);
        param(m, "bogus", 0x99, 0);
        if (get(m, 6, 0, buf, 8192)) {
            const InfoLayout l(is64);
            const GuestAddr ci = api.alloc(0xd0, 8);
            api.write_u32(ci + l.issuer, api.u32_at(buf + ps));
            api.write_ptr(ci + l.issuer + ps, api.ptr_at(buf + 2 * ps));
            api.write_u32(ci + l.serial, api.u32_at(buf + 3 * ps));
            api.write_ptr(ci + l.serial + ps, api.ptr_at(buf + 4 * ps));
            set_last_error(0xdead);
            const GuestAddr c = static_cast<GuestAddr>(api.call(
                "CertFindCertificateInStore", {store, 0x10001, 0, 0x000B0000, ci, 0}));
            std::string row = "find\t" + hex(c != 0);
            if (c) {
                const GuestAddr name = api.alloc(96, 8);
                api.call("CertGetNameStringA", {c, 4, 0, 0, name, 96});
                row += "\t" + api.cstring_at(name) + "\t" + hex(api.u32_at(c)) + "\t" +
                       hex(api.ptr_at(c + 4 * ps) == store);
                api.call("CertFreeCertificateContext", {c});
            } else {
                row += "\t" + hex(get_last_error());
            }
            out.push_back(row);
        }
        set_last_error(0xdead);
        out.push_back("msgclose\t" + hex(api.call("CryptMsgClose", {m})) + "\t" + hex(get_last_error()));
        set_last_error(0xdead);
        out.push_back("storeclose\t" + hex(api.call("CertCloseStore", {store, 0})) + "\t" +
                      hex(get_last_error()));
    };
    query("signed", u"crypt_wvt_signed.exe");
    query("unsigned", u"crypt_wvt_unsigned.exe");
    query("text", u"crypt_wvt_text.txt");
    query("missing", u"crypt_wvt_missing.exe");
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return out;
}

TEST(Crypt32OracleReplay, CryptQueryObjectAndMsgGetParamMatchWindowsX64) {
    expect_transcript(read_expected("crypt_msg_probe.expected"), replay_msg_probe(true));
}

TEST(Crypt32OracleReplay, CryptQueryObjectAndMsgGetParamMatchWindowsX86) {
    expect_transcript(read_expected("crypt_msg_probe.x86.expected"), replay_msg_probe(false));
}

// ---------------------------------------------------------------------------
// tools/oracle_probe/crypt_misc_probe.c -> crypt_misc_probe{,.x86}.expected
// ---------------------------------------------------------------------------

std::vector<std::string> replay_misc_probe(bool is64) {
    CryptApi api(is64);
    const u32 ps = api.ptr_size();
    std::vector<std::string> out;
    const GuestAddr leaf = api.put(kProbeLeafDer, sizeof(kProbeLeafDer));
    const GuestAddr expired = api.put(kProbeExpiredLeafDer, sizeof(kProbeExpiredLeafDer));
    const u32 leaf_size = sizeof(kProbeLeafDer);
    const GuestAddr size = api.alloc(4, 4);
    const GuestAddr got = api.alloc(4, 4);
    const GuestAddr buf = api.alloc(64, 8);
    auto open_memory = [&] { return api.call("CertOpenStore", {2, 0, 0, 0, 0}); };
    auto count_store = [&](u64 store) {
        u32 n = 0;
        u64 c = 0;
        while ((c = api.call("CertEnumCertificatesInStore", {store, c})) != 0) ++n;
        return n;
    };

    const char16_t* algs[] = {u"SHA1", u"SHA256", u"SHA384", u"SHA512", u"MD5", u"sha256",
                              u"MD4", u"MD2", u"BOGUS"};
    for (const char16_t* alg : algs) {
        const std::u16string name(alg);
        const GuestAddr name_ptr = api.put_wstring(name);
        api.write_u32(size, 0);
        set_last_error(0xdead);
        const u64 ok = api.call("CryptHashCertificate2", {name_ptr, 0, 0, leaf, leaf_size, 0, size});
        const u32 err = get_last_error();
        api.write_u32(got, 64);
        set_last_error(0xdead);
        const u64 ok2 = api.call("CryptHashCertificate2", {name_ptr, 0, 0, leaf, leaf_size, buf, got});
        const u32 err2 = get_last_error();
        const u32 n = api.u32_at(got);
        out.push_back("hash " + std::string(name.begin(), name.end()) + "\t" + hex(ok) + "\t" +
                      hex(err) + "\t" + hex(api.u32_at(size)) + "\t" + hex(ok2) + "\t" + hex(err2) +
                      "\t" + hex(n) + "\t" + (ok2 ? bytes_hex(api.bytes_at(buf, n < 8 ? n : 8)) : ""));
    }
    {
        const GuestAddr sha1 = api.put_wstring(u"SHA1");
        api.write_u32(got, 4);
        set_last_error(0xdead);
        u64 ok = api.call("CryptHashCertificate2", {sha1, 0, 0, leaf, leaf_size, buf, got});
        out.push_back("hash_small\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" + hex(api.u32_at(got)));
        api.write_u32(got, 64);
        set_last_error(0xdead);
        ok = api.call("CryptHashCertificate2", {sha1, 1, 0, leaf, leaf_size, buf, got});
        out.push_back("hash_flags\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" + hex(api.u32_at(got)));
        api.write_u32(got, 64);
        set_last_error(0xdead);
        ok = api.call("CryptHashCertificate2", {sha1, 0, 0, 0, 0, buf, got});
        out.push_back("hash_empty\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" + hex(api.u32_at(got)) +
                      "\t" + (ok ? bytes_hex(api.bytes_at(buf, 4)) : ""));
    }

    const std::u16string friendly_text(u"first");
    const GuestAddr friendly_data = api.put(friendly_text.c_str(), (friendly_text.size() + 1) * 2, 2);
    const GuestAddr friendly_blob = api.put_blob(friendly_data, static_cast<u32>((friendly_text.size() + 1) * 2));
    const GuestAddr first_out = api.alloc(8, 8);
    const GuestAddr second_out = api.alloc(8, 8);
    for (const u32 disposition : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 0u, 8u}) {
        const u64 store = open_memory();
        api.write_ptr(first_out, 0);
        api.write_ptr(second_out, 1);
        api.call("CertAddEncodedCertificateToStore", {store, kX509, leaf, leaf_size, 4, first_out});
        const u64 first = api.ptr_at(first_out);
        api.call("CertSetCertificateContextProperty", {first, 11, 0, friendly_blob});
        set_last_error(0xdead);
        const u64 ok = api.call("CertAddEncodedCertificateToStore",
                                {store, kX509, leaf, leaf_size, disposition, second_out});
        const u32 err = get_last_error();
        const u64 second = api.ptr_at(second_out);
        u64 friendly = 0;
        if (ok && second) friendly = api.call("CertGetCertificateContextProperty", {second, 11, 0, size});
        out.push_back("disp_encoded\t" + hex(disposition) + "\t" + hex(ok) + "\t" + hex(err) + "\t" +
                      hex(second == 0) + "\t" + hex(ok && second == first) + "\t" +
                      hex(count_store(store)) + "\t" + hex(friendly));
        if (ok && second && second != 1) api.call("CertFreeCertificateContext", {second});
        api.call("CertFreeCertificateContext", {first});
        api.call("CertCloseStore", {store, 0});
    }
    for (int newer = 0; newer < 2; ++newer) {
        for (u32 d = 6; d <= 7; ++d) {
            const u64 store = open_memory();
            api.call("CertAddEncodedCertificateToStore", {store, kX509, leaf, leaf_size, 4, 0});
            const u64 src = api.call("CertCreateCertificateContext",
                                     {kX509, newer ? leaf : expired,
                                      newer ? leaf_size : sizeof(kProbeExpiredLeafDer)});
            api.write_ptr(second_out, 0);
            set_last_error(0xdead);
            const u64 ok = api.call("CertAddCertificateContextToStore", {store, src, d, second_out});
            out.push_back("disp_context\t" + hex(newer) + "\t" + hex(d) + "\t" + hex(ok) + "\t" +
                          hex(get_last_error()) + "\t" + hex(count_store(store)));
            if (api.ptr_at(second_out)) api.call("CertFreeCertificateContext", {api.ptr_at(second_out)});
            api.call("CertFreeCertificateContext", {src});
            api.call("CertCloseStore", {store, 0});
        }
    }
    {
        u64 store = open_memory();
        api.write_ptr(first_out, 0);
        api.call("CertAddEncodedCertificateToStore", {store, kX509, leaf, leaf_size, 4, first_out});
        const u64 c = api.ptr_at(first_out);
        set_last_error(0xdead);
        u64 ok = api.call("CertCloseStore", {store, 2});
        out.push_back("close_check_pending\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" +
                      hex(api.u32_at(static_cast<GuestAddr>(c) + 2 * ps)));
        api.call("CertFreeCertificateContext", {c});
        store = open_memory();
        set_last_error(0xdead);
        ok = api.call("CertCloseStore", {store, 2});
        out.push_back("close_check_clean\t" + hex(ok) + "\t" + hex(get_last_error()));
        store = open_memory();
        set_last_error(0xdead);
        ok = api.call("CertCloseStore", {store, 0x80});
        out.push_back("close_flag80\t" + hex(ok) + "\t" + hex(get_last_error()));
        set_last_error(0xdead);
        ok = api.call("CertCloseStore", {0, 0});
        out.push_back("close_null\t" + hex(ok) + "\t" + hex(get_last_error()));
    }
    {
        const u64 c = api.call("CertCreateCertificateContext", {kX509, leaf, leaf_size});
        const GuestAddr key = api.alloc(8, 8);
        const GuestAddr spec = api.alloc(4, 4);
        const GuestAddr free_key = api.alloc(4, 4);
        const u64 minus_one = is64 ? ~0ull : 0xffffffffull;
        api.write_ptr(key, minus_one);
        api.write_u32(spec, 0x77);
        api.write_u32(free_key, 0x77);
        set_last_error(0xdead);
        u64 ok = api.call("CryptAcquireCertificatePrivateKey", {c, 0, 0, key, spec, free_key});
        out.push_back("acquire\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" +
                      hex(api.ptr_at(key) == 0) + "\t" + hex(api.ptr_at(key) == minus_one) + "\t" +
                      hex(api.u32_at(spec)) + "\t" + hex(api.u32_at(free_key)));
        set_last_error(0xdead);
        ok = api.call("CryptAcquireCertificatePrivateKey", {c, 0x40 | 0x40000, 0, key, spec, free_key});
        out.push_back("acquire_ncrypt\t" + hex(ok) + "\t" + hex(get_last_error()));
        const u64 store = open_memory();
        for (const u32 control : {1u, 2u, 3u, 4u, 99u}) {
            set_last_error(0xdead);
            const u64 r = api.call("CertControlStore", {store, 0, control, 0});
            out.push_back("control\t" + hex(control) + "\t" + hex(r) + "\t" + hex(get_last_error()));
        }
        api.call("CertAddEncodedCertificateToStore", {store, kX509, leaf, leaf_size, 4, 0});
        const u32 para_size = is64 ? 56 : 32;   // CERT_CHAIN_FIND_BY_ISSUER_PARA
        const GuestAddr para = api.alloc(para_size, 8);
        api.write_u32(para, para_size);
        set_last_error(0xdead);
        const u64 chain = api.call("CertFindChainInStore", {store, kX509, 0, 1, para, 0});
        out.push_back("findchain\t" + hex(chain != 0) + "\t" + hex(get_last_error()));
        if (chain) api.call("CertFreeCertificateChain", {chain});
        api.call("CertCloseStore", {store, 0});
        api.call("CertFreeCertificateContext", {c});
    }
    {
        static const u8 san[] = {
            0x30, 0x2f,
            0x82, 0x06, 'a', '.', 't', 'e', 's', 't',
            0x81, 0x08, 'x', '@', 'y', '.', 't', 'e', 's', 't',
            0x86, 0x0e, 'h', 't', 't', 'p', ':', '/', '/', 'u', '.', 't', 'e', 's', 't', '/',
            0x87, 0x04, 127, 0, 0, 1,
            0x82, 0x05, 'b', '.', 't', 'e', 'x',
        };
        const GuestAddr san_ptr = api.put(san, sizeof(san));
        const GuestAddr out_buf = api.alloc(1024, 8);
        api.write_u32(size, 0);
        set_last_error(0xdead);
        u64 ok = api.call("CryptDecodeObjectEx", {kX509, 12, san_ptr, sizeof(san), 0, 0, 0, size});
        out.push_back("decode_size\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" + hex(api.u32_at(size)));
        api.write_u32(got, 1024);
        set_last_error(0xdead);
        ok = api.call("CryptDecodeObjectEx", {kX509, 12, san_ptr, sizeof(san), 0, 0, out_buf, got});
        const u32 entry_size = is64 ? 24 : 12;
        const u32 count = api.u32_at(out_buf);
        std::string row = "decode\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" +
                          hex(api.u32_at(got)) + "\t" + hex(entry_size) + "\t" + hex(count);
        const GuestAddr entries = static_cast<GuestAddr>(api.ptr_at(out_buf + ps));
        for (u32 i = 0; ok && i < count; ++i) {
            const GuestAddr e = entries + i * entry_size;
            const u32 choice = api.u32_at(e);
            row += "\t" + hex(choice) + ":";
            if (choice == 8) {
                row += bytes_hex(api.bytes_at(static_cast<GuestAddr>(api.ptr_at(e + 2 * ps)),
                                              api.u32_at(e + ps)));
            } else {
                row += api.wstring_at(static_cast<GuestAddr>(api.ptr_at(e + ps)));
            }
        }
        out.push_back(row);
        const GuestAddr oid = api.put_string("2.5.29.17");
        const GuestAddr alloc_out = api.alloc(8, 8);
        const GuestAddr asize = api.alloc(4, 4);
        set_last_error(0xdead);
        ok = api.call("CryptDecodeObjectEx", {kX509, oid, san_ptr, sizeof(san), 0x8000, 0, alloc_out, asize});
        const u32 alloc_err = get_last_error();
        const u64 allocated = api.ptr_at(alloc_out);
        const bool readable = allocated != 0 &&
            api.local_size(static_cast<GuestAddr>(allocated)) >= api.u32_at(asize);
        out.push_back("decode_alloc\t" + hex(ok) + "\t" + hex(alloc_err) + "\t" + hex(api.u32_at(asize)) +
                      "\t" + hex(readable));
        api.write_u32(got, 4);
        set_last_error(0xdead);
        ok = api.call("CryptDecodeObjectEx", {kX509, 12, san_ptr, sizeof(san), 0, 0, out_buf, got});
        out.push_back("decode_small\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" + hex(api.u32_at(got)));
        static const u8 junk[] = {0x04, 0x02, 0x01, 0x02};
        const GuestAddr junk_ptr = api.put(junk, sizeof(junk));
        api.write_u32(got, 1024);
        set_last_error(0xdead);
        ok = api.call("CryptDecodeObjectEx", {kX509, 12, junk_ptr, sizeof(junk), 0, 0, out_buf, got});
        out.push_back("decode_junk\t" + hex(ok) + "\t" + hex(get_last_error()));
        static const u8 eku[] = {0x30, 0x0a, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01};
        const GuestAddr eku_ptr = api.put(eku, sizeof(eku));
        api.write_u32(got, 1024);
        set_last_error(0xdead);
        ok = api.call("CryptDecodeObjectEx", {kX509, 36, eku_ptr, sizeof(eku), 0, 0, out_buf, got});
        row = "decode_eku\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" + hex(api.u32_at(got)) + "\t";
        if (ok) {
            row += hex(api.u32_at(out_buf)) + "\t" +
                   api.cstring_at(static_cast<GuestAddr>(api.ptr_at(static_cast<GuestAddr>(api.ptr_at(out_buf + ps)))));
        }
        out.push_back(row);
    }
    return out;
}

TEST(Crypt32OracleReplay, MiscContractsMatchWindowsX64) {
    expect_transcript(read_expected("crypt_misc_probe.expected"), replay_misc_probe(true));
}

TEST(Crypt32OracleReplay, MiscContractsMatchWindowsX86) {
    expect_transcript(read_expected("crypt_misc_probe.x86.expected"), replay_misc_probe(false));
}

// ---------------------------------------------------------------------------
// tools/oracle_probe/crypt_dpapi_probe.c -> crypt_dpapi_probe{,.x86}.expected
// ---------------------------------------------------------------------------

std::vector<std::string> replay_dpapi_probe(bool is64, const std::string& key_file) {
    CryptApi api(is64);
    api.manager().set_dpapi_key_file(key_file);
    const u32 ps = api.ptr_size();
    std::vector<std::string> out;
    out.push_back("layout\t10\t" + hex(2 * ps) + "\t" + hex(is64 ? 0x18 : 0x10));

    const GuestAddr data = api.alloc(64, 16);
    auto memory_case = [&](const char* label, u32 size, u32 protect_flags, u32 unprotect_flags) {
        std::vector<u8> orig(64);
        for (int i = 0; i < 64; ++i) orig[i] = static_cast<u8>(i * 7 + 1);
        EXPECT_TRUE(api.memory().write_bytes(data, orig.data(), orig.size()).is_ok());
        set_last_error(0xdead);
        const u64 ok = api.call("CryptProtectMemory", {data, size, protect_flags});
        const u32 err = get_last_error();
        const auto after = api.bytes_at(data, 64);
        const u32 n = size ? size : 1;
        const bool changed = !std::equal(after.begin(), after.begin() + n, orig.begin());
        const bool tail = std::equal(after.begin() + size, after.end(), orig.begin() + size);
        set_last_error(0xdead);
        const u64 ok2 = api.call("CryptUnprotectMemory", {data, size, unprotect_flags});
        const u32 err2 = get_last_error();
        out.push_back(std::string("mem ") + label + "\t" + hex(ok) + "\t" + hex(err) + "\t" +
                      hex(changed) + "\t" + hex(tail) + "\t" + hex(ok2) + "\t" + hex(err2) + "\t" +
                      hex(api.bytes_at(data, 64) == orig));
    };
    memory_case("same16", 16, 0, 0);
    memory_case("same32", 32, 0, 0);
    memory_case("cross", 16, 1, 1);
    memory_case("logon", 16, 2, 2);
    memory_case("mixed", 16, 0, 1);
    memory_case("size15", 15, 0, 0);
    memory_case("size17", 17, 0, 0);
    memory_case("size0", 0, 0, 0);
    memory_case("flag3", 16, 3, 3);
    memory_case("flag4", 16, 4, 4);
    memory_case("flag80", 16, 0x80, 0x80);
    {
        const std::vector<u8> fill(32, 0x41);
        const GuestAddr a = api.put(fill.data(), 32);
        const GuestAddr b = api.put(fill.data(), 32);
        api.call("CryptProtectMemory", {a, 32, 0});
        api.call("CryptProtectMemory", {b, 32, 0});
        const auto ca = api.bytes_at(a, 32);
        out.push_back("mem_det\t" + hex(ca == api.bytes_at(b, 32)) + "\t" +
                      hex(std::equal(ca.begin(), ca.begin() + 16, ca.begin() + 16)));
        const GuestAddr c = api.put(fill.data(), 16);
        const GuestAddr d = api.put(fill.data(), 16);
        api.call("CryptProtectMemory", {c, 16, 1});
        api.call("CryptProtectMemory", {d, 16, 0});
        out.push_back("mem_scope\t" + hex(api.bytes_at(c, 16) == api.bytes_at(d, 16)));
    }

    static const char kPlain[] = "Hello world of data protection.";
    const u32 plain_size = sizeof(kPlain);
    const std::vector<u8> plain(kPlain, kPlain + plain_size);
    const GuestAddr in = api.put_blob(api.put(kPlain, plain_size), plain_size);
    auto blob_out = [&] { return api.put_blob(0, 0); };
    auto blob_data = [&](GuestAddr blob) { return static_cast<GuestAddr>(api.ptr_at(blob + ps)); };
    auto blob_size = [&](GuestAddr blob) { return api.u32_at(blob); };
    const GuestAddr descr_text = api.put_wstring(u"Probe description");
    const GuestAddr protected_blob = blob_out();
    set_last_error(0xdead);
    u64 ok = api.call("CryptProtectData", {in, descr_text, 0, 0, 0, 0, protected_blob});
    // Column 4 (LocalSize(pbData) == cbData) is not replayed: Windows
    // allocates more than cbData, Vitreo exactly cbData; callers only
    // LocalFree the buffer.
    out.push_back("protect\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" +
                  hex(blob_size(protected_blob) > plain_size) + "\t-\t" +
                  hex(ok && api.bytes_at(blob_data(protected_blob), 8) !=
                                std::vector<u8>(plain.begin(), plain.begin() + 8)));
    {
        const GuestAddr clear = blob_out();
        const GuestAddr descr = api.alloc(8, 8);
        api.write_ptr(descr, 1);
        set_last_error(0xdead);
        u64 ok2 = api.call("CryptUnprotectData", {protected_blob, descr, 0, 0, 0, 0, clear});
        const u64 text = api.ptr_at(descr);
        std::string row = "unprotect\t" + hex(ok2) + "\t" + hex(get_last_error()) + "\t" +
                          hex(blob_size(clear)) + "\t" +
                          hex(ok2 && api.bytes_at(blob_data(clear), plain_size) == plain) + "\t" +
                          hex(ok2 && api.local_size(blob_data(clear)) >= blob_size(clear)) + "\t" +
                          hex(text != 0 && text != 1) + "\t";
        if (text != 0 && text != 1) {
            row += api.wstring_at(static_cast<GuestAddr>(text)) + "\t" +
                   hex(api.local_size(static_cast<GuestAddr>(text)) >= 36);
        }
        out.push_back(row);
        const GuestAddr clear2 = blob_out();
        set_last_error(0xdead);
        ok2 = api.call("CryptUnprotectData", {protected_blob, 0, 0, 0, 0, 0, clear2});
        out.push_back("unprotect_nodescr\t" + hex(ok2) + "\t" + hex(get_last_error()) + "\t" +
                      hex(blob_size(clear2)));
    }
    {
        const GuestAddr o2 = blob_out();
        const GuestAddr clear = blob_out();
        api.call("CryptProtectData", {in, 0, 0, 0, 0, 0, o2});
        const GuestAddr descr = api.alloc(8, 8);
        api.write_ptr(descr, 1);
        const u64 ok2 = api.call("CryptUnprotectData", {o2, descr, 0, 0, 0, 0, clear});
        const u64 text = api.ptr_at(descr);
        std::string row = "nodescr\t" + hex(ok2) + "\t" + hex(text == 0) + "\t" + hex(text == 1);
        if (text != 0 && text != 1) {
            row += "\t" + hex(api.memory().read_u16(static_cast<GuestAddr>(text)).value_or(0xffff));
        }
        out.push_back(row);
    }
    {
        static const char e1[] = "entropy-one";
        static const char e2[] = "entropy-two";
        const GuestAddr ent1 = api.put_blob(api.put(e1, sizeof(e1)), sizeof(e1));
        const GuestAddr ent2 = api.put_blob(api.put(e2, sizeof(e2)), sizeof(e2));
        const GuestAddr o3 = blob_out();
        const u64 p = api.call("CryptProtectData", {in, 0, ent1, 0, 0, 0, o3});
        GuestAddr clear = blob_out();
        set_last_error(0xdead);
        const u64 a = api.call("CryptUnprotectData", {o3, 0, ent1, 0, 0, 0, clear});
        const u32 ea = get_last_error();
        clear = api.put_blob(1, 0);
        set_last_error(0xdead);
        const u64 b = api.call("CryptUnprotectData", {o3, 0, 0, 0, 0, 0, clear});
        const u32 eb = get_last_error();
        const bool b_out = blob_data(clear) == 0;
        clear = blob_out();
        set_last_error(0xdead);
        const u64 c = api.call("CryptUnprotectData", {o3, 0, ent2, 0, 0, 0, clear});
        const u32 ec = get_last_error();
        out.push_back("entropy\t" + hex(p) + "\t" + hex(a) + "\t" + hex(ea) + "\t" + hex(b) + "\t" +
                      hex(eb) + "\t" + hex(b_out) + "\t" + hex(c) + "\t" + hex(ec));
        const GuestAddr o4 = blob_out();
        api.call("CryptProtectData", {in, 0, 0, 0, 0, 0, o4});
        set_last_error(0xdead);
        const u64 d = api.call("CryptUnprotectData", {o4, 0, ent1, 0, 0, 0, blob_out()});
        out.push_back("entropy_extra\t" + hex(d) + "\t" + hex(get_last_error()));
    }
    {
        const GuestAddr o5 = blob_out();
        api.call("CryptProtectData", {in, 0, 0, 0, 0, 0, o5});
        const u32 n = blob_size(o5);
        const GuestAddr bytes = blob_data(o5);
        const u32 offsets[] = {0, 20, 60, n - 1};
        for (u32 i = 0; i < 4; ++i) {
            const GuestAddr at = bytes + offsets[i];
            (void)api.memory().write_u8(at, api.u8_at(at) ^ 1);
            set_last_error(0xdead);
            const u64 t = api.call("CryptUnprotectData", {o5, 0, 0, 0, 0, 0, blob_out()});
            out.push_back("tamper\t" + hex(i) + "\t" + hex(t) + "\t" + hex(get_last_error()));
            (void)api.memory().write_u8(at, api.u8_at(at) ^ 1);
        }
        u8 junk[40];
        for (int i = 0; i < 40; ++i) junk[i] = static_cast<u8>(i);
        const GuestAddr junk_ptr = api.put(junk, 40);
        set_last_error(0xdead);
        u64 t = api.call("CryptUnprotectData", {api.put_blob(junk_ptr, 40), 0, 0, 0, 0, 0, blob_out()});
        out.push_back("garbage\t" + hex(t) + "\t" + hex(get_last_error()));
        set_last_error(0xdead);
        t = api.call("CryptUnprotectData", {api.put_blob(junk_ptr, 0), 0, 0, 0, 0, 0, blob_out()});
        out.push_back("garbage_empty\t" + hex(t) + "\t" + hex(get_last_error()));
    }
    {
        const GuestAddr empty = api.put_blob(api.put(kPlain, 1), 0);
        const GuestAddr o6 = blob_out();
        set_last_error(0xdead);
        const u64 p = api.call("CryptProtectData", {empty, 0, 0, 0, 0, 0, o6});
        const u32 ep = get_last_error();
        const GuestAddr clear = api.put_blob(1, 0);
        set_last_error(0xdead);
        const u64 u = p ? api.call("CryptUnprotectData", {o6, 0, 0, 0, 0, 0, clear}) : 0;
        out.push_back("empty\t" + hex(p) + "\t" + hex(ep) + "\t" + hex(u) + "\t" + hex(get_last_error()) +
                      "\t" + hex(blob_size(clear)) + "\t" + hex(blob_data(clear) == 0));
    }
    for (const u32 flag : {0x1u, 0x4u, 0x10u, 0x40u, 0x80000000u, 0x8u, 0x20u}) {
        const GuestAddr o7 = blob_out();
        set_last_error(0xdead);
        const u64 p = api.call("CryptProtectData", {in, 0, 0, 0, 0, flag, o7});
        const u32 ep = get_last_error();
        set_last_error(0xdead);
        const u64 u = p ? api.call("CryptUnprotectData", {o7, 0, 0, 0, 0, flag, blob_out()}) : 0;
        out.push_back("flags\t" + hex(flag) + "\t" + hex(p) + "\t" + hex(ep) + "\t" + hex(u) + "\t" +
                      hex(get_last_error()));
    }
    {
        const GuestAddr o8 = blob_out();
        api.call("CryptProtectData", {in, 0, 0, 0, 0, 4, o8});
        set_last_error(0xdead);
        const u64 u = api.call("CryptUnprotectData", {o8, 0, 0, 0, 0, 0, blob_out()});
        out.push_back("machine_to_user\t" + hex(u) + "\t" + hex(get_last_error()));
    }
    {
        const u32 prompt_size = is64 ? 0x18 : 0x10;
        const GuestAddr prompt = api.alloc(prompt_size, 8);
        api.write_u32(prompt, prompt_size);
        set_last_error(0xdead);
        u64 p = api.call("CryptProtectData", {in, 0, 0, 0, prompt, 0, blob_out()});
        out.push_back("prompt0\t" + hex(p) + "\t" + hex(get_last_error()));
        api.write_u32(prompt + 4, 2);   // CRYPTPROTECT_PROMPT_ON_PROTECT
        set_last_error(0xdead);
        p = api.call("CryptProtectData", {in, 0, 0, 0, prompt, 1, blob_out()});
        out.push_back("prompt_forbidden\t" + hex(p) + "\t" + hex(get_last_error()));
    }
    {
        set_last_error(0xdead);
        u64 p = api.call("CryptProtectData", {0, 0, 0, 0, 0, 0, blob_out()});
        out.push_back("null_in\t" + hex(p) + "\t" + hex(get_last_error()));
        set_last_error(0xdead);
        p = api.call("CryptProtectData", {in, 0, 0, 0, 0, 0, 0});
        out.push_back("null_out\t" + hex(p) + "\t" + hex(get_last_error()));
        set_last_error(0xdead);
        p = api.call("CryptUnprotectData", {0, 0, 0, 0, 0, 0, blob_out()});
        out.push_back("unull_in\t" + hex(p) + "\t" + hex(get_last_error()));
        const GuestAddr o10 = blob_out();
        api.call("CryptProtectData", {in, 0, 0, 0, 0, 0, o10});
        set_last_error(0xdead);
        p = api.call("CryptUnprotectData", {o10, 0, 0, 0, 0, 0, 0});
        out.push_back("unull_out\t" + hex(p) + "\t" + hex(get_last_error()));
        set_last_error(0xdead);
        p = api.call("CryptProtectData", {in, 0, 0, 1, 0, 0, blob_out()});
        out.push_back("reserved\t" + hex(p) + "\t" + hex(get_last_error()));
    }
    return out;
}

/// The Windows transcript with the "protect" row's LocalSize column (see
/// replay_dpapi_probe) masked as "-" on both sides: it is not compared.
std::vector<std::string> dpapi_expected(const char* name) {
    auto lines = read_expected(name);
    for (auto& line : lines) {
        if (line.rfind("protect\t", 0) != 0) continue;
        std::vector<std::string> cols;
        std::string col;
        for (const char ch : line + "\t") {
            if (ch == '\t') { cols.push_back(col); col.clear(); } else { col += ch; }
        }
        if (cols.size() > 4) cols[4] = "-";
        line.clear();
        for (usize i = 0; i < cols.size(); ++i) line += (i ? "\t" : "") + cols[i];
    }
    return lines;
}

class DpapiReplay : public ::testing::Test {
protected:
    // The key file must exist (32 bytes, owner-only): a missing file makes
    // ensure_dpapi_key fall back to the login Keychain, which can block on
    // an access prompt in an unattended test run.
    void SetUp() override {
        char path[] = "/tmp/vitreo-dpapi-replay-XXXXXX";
        ASSERT_NE(::mkdtemp(path), nullptr);
        m_dir = path;
        const int fd = ::open(key_file().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        ASSERT_GE(fd, 0);
        u8 key[32];
        for (int i = 0; i < 32; ++i) key[i] = static_cast<u8>(0x5a ^ (i * 29));
        ASSERT_EQ(::write(fd, key, sizeof(key)), static_cast<ssize_t>(sizeof(key)));
        ::close(fd);
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(m_dir, error);
    }
    std::string key_file() const { return m_dir + "/dpapi.key"; }
    std::string m_dir;
};

TEST_F(DpapiReplay, CryptProtectDataAndMemoryMatchWindowsX64) {
    expect_transcript(dpapi_expected("crypt_dpapi_probe.expected"), replay_dpapi_probe(true, key_file()));
}

TEST_F(DpapiReplay, CryptProtectDataAndMemoryMatchWindowsX86) {
    expect_transcript(dpapi_expected("crypt_dpapi_probe.x86.expected"), replay_dpapi_probe(false, key_file()));
}

} // namespace
