// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crypt32_signer_trio.cpp
/// @brief CryptQueryObject over cabinets, CryptMsgGetAndVerifySigner,
///        CryptHashPublicKeyInfo and CryptDecodeObject replayed against
///        tools/oracle_probe/crypt_signer_trio_probe.c's Windows output
///        (tests/oracle_data/crypt_signer_trio_probe{,.x86}.expected).
///
/// The fixtures are the test-PKI cabinets of tests/winapi/crypt32/fixtures/cab
/// and the bad-signature / bad-digest PEs of fixtures/pe, served from a
/// capsule drive. Three probe rows use a message from CryptMsgOpenToDecode,
/// which Vitreo does not export; they are left out of the replay.

#include "crypt32_api_harness.hpp"

#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>

#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::test;

namespace {

constexpr u32 X509 = 0x1;
constexpr u32 PKCS7 = 0x10000;

std::filesystem::path fixtures() {
    return std::filesystem::path(__FILE__).parent_path() / "fixtures";
}

std::vector<std::string> expected(bool is64) {
    std::ifstream in(std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                     "oracle_data" /
                     (is64 ? "crypt_signer_trio_probe.expected" : "crypt_signer_trio_probe.x86.expected"));
    EXPECT_TRUE(in.good());
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // CryptMsgOpenToDecode messages: not exported by Vitreo.
        if (line.rfind("gav undecoded", 0) == 0 || line.rfind("dataupdate", 0) == 0 ||
            line.rfind("gav data-msg", 0) == 0) {
            continue;
        }
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> replay(bool is64) {
    const std::string dir = (std::filesystem::temp_directory_path() /
                             ("vitreo-signer-trio-" + std::to_string(::getpid()) + (is64 ? "-64" : "-32")))
                                .string();
    std::filesystem::create_directories(dir);
    for (const char* name : {"cab_unsigned.cab", "cab_lead.cab", "cab_tampered.cab", "cab_signed.cab"})
        std::filesystem::copy_file(fixtures() / "cab" / name, std::filesystem::path(dir) / name,
                                   std::filesystem::copy_options::overwrite_existing);
    for (const char* name : {"crypt_wvt_badsig.exe", "crypt_wvt_badmd.exe"})
        std::filesystem::copy_file(fixtures() / "pe" / name, std::filesystem::path(dir) / name,
                                   std::filesystem::copy_options::overwrite_existing);
    capsule::Vfs vfs;
    EXPECT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(dir)).is_ok());
    CryptApi api(is64);
    api.manager().set_vfs(&vfs);
    const u32 ps = api.ptr_size();
    std::vector<std::string> out;
    out.push_back(std::string("bits\t") + (is64 ? "40" : "20"));

    u64 store = 0, msg = 0;
    auto query = [&](const std::string& label, const std::u16string& name) {
        const GuestAddr outs = api.alloc(6 * 8, 8);
        api.write_u32(outs, 0xaaaa);
        api.write_u32(outs + 8, 0xbbbb);
        api.write_u32(outs + 16, 0xcccc);
        api.write_ptr(outs + 24, 1);
        api.write_ptr(outs + 32, 1);
        set_last_error(0xdead);
        const u32 ok = static_cast<u32>(api.call("CryptQueryObject",
            {1, api.put_wstring(u"C:\\" + name), 0x400, 0x2, 0, outs, outs + 8, outs + 16, outs + 24, outs + 32, 0}));
        const u32 err = get_last_error();
        const u64 s = api.ptr_at(outs + 24), mg = api.ptr_at(outs + 32);
        std::string row = "query " + label + "\t" + hex(ok) + "\t" + hex(ok ? 0 : err) + "\t" +
                          hex(api.u32_at(outs)) + "\t" + hex(api.u32_at(outs + 8)) + "\t" +
                          hex(api.u32_at(outs + 16)) + "\t" + hex(s != 0 && s != 1) + "\t" + hex(mg != 0 && mg != 1);
        if (ok) {
            const GuestAddr v = api.alloc(4), sz = api.alloc(4);
            api.write_u32(sz, 4);
            api.call("CryptMsgGetParam", {mg, 5, 0, v, sz});   // CMSG_SIGNER_COUNT_PARAM
            row += "\t" + hex(api.u32_at(v));
            api.write_u32(v, 0);
            api.write_u32(sz, 4);
            api.call("CryptMsgGetParam", {mg, 11, 0, v, sz});  // CMSG_CERT_COUNT_PARAM
            row += "\t" + hex(api.u32_at(v));
            store = s;
            msg = mg;
        }
        out.push_back(row);
        return ok != 0;
    };

    u64 signer_ctx = 0;
    auto sha1_of = [&](u64 ctx) {
        const GuestAddr buf = api.alloc(32), sz = api.alloc(4);
        api.write_u32(sz, 20);
        if (!api.call("CertGetCertificateContextProperty", {ctx, 3, buf, sz})) return std::string("?");
        return bytes_hex(api.bytes_at(buf, api.u32_at(sz)));
    };
    auto gav = [&](const std::string& label, u64 m, std::vector<u64> stores, u32 flags, bool want_signer,
                   bool want_index, u32 idx_in) {
        const GuestAddr arr = stores.empty() ? 0 : api.alloc(stores.size() * ps, 8);
        for (usize i = 0; i < stores.size(); ++i) api.write_ptr(arr + i * ps, stores[i]);
        const GuestAddr signer = api.alloc(8, 8), idx = api.alloc(4);
        api.write_ptr(signer, 1);
        api.write_u32(idx, idx_in);
        set_last_error(0xdead);
        const u32 ok = static_cast<u32>(api.call("CryptMsgGetAndVerifySigner",
            {m, stores.size(), arr, flags, want_signer ? signer : 0, want_index ? idx : 0}));
        const u32 err = get_last_error();
        std::string row = "gav " + label + "\t" + hex(ok) + "\t" + hex(err) + "\t" +
                          (want_index ? hex(api.u32_at(idx)) : std::string("-")) + "\t";
        const u64 ctx = api.ptr_at(signer);
        if (!want_signer) row += "-";
        else if (ctx == 1) row += "untouched";
        else if (ctx == 0) row += "null";
        else {
            const u64 ctx_store = api.ptr_at(ctx + 4 * ps);
            row += sha1_of(ctx) + "\t" + hex(api.u32_at(ctx)) + "\t" + hex(ctx_store == store) + "\t" +
                   hex(!stores.empty() && ctx_store == stores[0]);
            if (!signer_ctx) signer_ctx = api.call("CertDuplicateCertificateContext", {ctx});
            api.call("CertFreeCertificateContext", {ctx});
        }
        out.push_back(row);
    };

    query("cab_unsigned", u"cab_unsigned.cab");
    query("cab_lead", u"cab_lead.cab");
    query("cab_tampered", u"cab_tampered.cab");
    const u64 tampered = msg;
    if (query("pe_badsig", u"crypt_wvt_badsig.exe")) {
        gav("badsig", msg, {}, 0, true, true, 0x77);
        gav("badsig-only", msg, {}, 2, true, true, 0x77);
        gav("badsig-index0", msg, {}, 4, true, true, 0);
    }
    if (query("pe_badmd", u"crypt_wvt_badmd.exe")) {
        gav("badmd", msg, {}, 0, true, true, 0x77);
        gav("badmd-index0", msg, {}, 4, true, true, 0);
    }
    // As in the probe, the key info hashed below is that of the first signer
    // returned (the bad-signature PE's, through CMSG_SIGNER_ONLY_FLAG).
    if (!query("cab_signed", u"cab_signed.cab")) return out;
    const GuestAddr empty_store = static_cast<GuestAddr>(api.call("CertOpenStore", {2, 0, 0, 0, 0}));
    gav("plain", msg, {}, 0, true, true, 0x77);
    gav("plain-nulls", msg, {}, 0, false, false, 0);
    gav("signer-only", msg, {}, 2, true, true, 0x77);
    gav("index0", msg, {}, 4, true, true, 0);
    gav("index1", msg, {}, 4, true, true, 1);
    gav("index-big", msg, {}, 4, true, true, 0x100);
    gav("index1-only", msg, {}, 6, true, true, 1);
    gav("trusted-none", msg, {}, 1, true, true, 0x77);
    gav("trusted-empty", msg, {empty_store}, 1, true, true, 0x77);
    gav("trusted-query", msg, {store}, 1, true, true, 0x77);
    gav("trusted-both", msg, {empty_store, store}, 1, true, true, 0x77);
    gav("trusted-only-empty", msg, {empty_store}, 3, true, true, 0x77);
    gav("extra-empty", msg, {empty_store}, 0, true, true, 0x77);
    gav("extra-query", msg, {store}, 0, true, true, 0x77);
    gav("flag8", msg, {}, 8, true, true, 0x77);
    gav("flags-all", msg, {}, 0xffffffff, true, true, 0);
    gav("tampered", tampered, {}, 0, true, true, 0x77);

    {
        // PKCS7_SIGNER_INFO. The encoded signer comes from Windows'
        // CryptMsgGetParam(CMSG_ENCODED_SIGNER) row (a parameter Vitreo does
        // not serve); its decoded size must equal CMSG_SIGNER_INFO_PARAM's.
        std::vector<u8> enc;
        std::string enc_row;
        for (const auto& line : expected(is64))
            if (line.rfind("encsigner\t1\t", 0) == 0) enc_row = line;
        EXPECT_FALSE(enc_row.empty());
        for (usize i = std::string("encsigner\t1\t").size(); i + 1 < enc_row.size(); i += 2)
            enc.push_back(static_cast<u8>(std::stoul(enc_row.substr(i, 2), nullptr, 16)));
        out.push_back(enc_row);
        const GuestAddr pn = api.alloc(4);
        api.call("CryptMsgGetParam", {msg, 6, 0, 0, pn});
        out.push_back("siparam\t" + hex(api.u32_at(pn)));
        const GuestAddr data = api.put(enc.data(), enc.size());
        const u64 kType = 500;
        for (u32 mode = 0; mode < 4; ++mode) {
            const GuestAddr buf = api.alloc(4096, 8), sz = api.alloc(4), alloc_ptr = api.alloc(8, 8);
            api.write_u32(sz, mode == 1 ? 0 : (mode == 2 ? 16 : 16384));
            set_last_error(0xdead);
            const u32 ok = static_cast<u32>(mode == 3
                ? api.call("CryptDecodeObjectEx", {X509 | PKCS7, kType, data, enc.size(), 0x8000, 0, alloc_ptr, sz})
                : api.call("CryptDecodeObject", {X509 | PKCS7, kType, data, enc.size(), 0, mode == 1 ? 0 : buf, sz}));
            std::string row = "si " + hex(mode) + "\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" +
                              hex(api.u32_at(sz));
            const GuestAddr si = mode == 3 ? static_cast<GuestAddr>(api.ptr_at(alloc_ptr)) : buf;
            if (ok && si && mode != 1) {
                // CMSG_SIGNER_INFO: dwVersion, Issuer, SerialNumber,
                // HashAlgorithm, HashEncryptionAlgorithm, EncryptedHash,
                // AuthAttrs, UnauthAttrs.
                const u32 issuer = ps, serial = issuer + 2 * ps, hash_alg = serial + 2 * ps,
                          enc_alg = hash_alg + 3 * ps, enc_hash = enc_alg + 3 * ps,
                          auth = enc_hash + 2 * ps, unauth = auth + 2 * ps;
                row += "\t" + hex(api.u32_at(si)) + "\t" + hex(api.u32_at(si + issuer)) + "\t" +
                       bytes_hex(api.bytes_at(static_cast<GuestAddr>(api.ptr_at(si + serial + ps)),
                                              api.u32_at(si + serial))) +
                       "\t" + api.cstring_at(static_cast<GuestAddr>(api.ptr_at(si + hash_alg))) + "\t" +
                       hex(api.u32_at(si + hash_alg + ps)) + "\t" +
                       api.cstring_at(static_cast<GuestAddr>(api.ptr_at(si + enc_alg))) + "\t" +
                       hex(api.u32_at(si + enc_alg + ps)) + "\t" + hex(api.u32_at(si + enc_hash)) + "\t" +
                       hex(api.u32_at(si + auth));
                const GuestAddr attrs = static_cast<GuestAddr>(api.ptr_at(si + auth + ps));
                for (u32 i = 0; i < api.u32_at(si + auth); ++i) {
                    const GuestAddr a = attrs + i * 3 * ps;
                    const GuestAddr values = static_cast<GuestAddr>(api.ptr_at(a + 2 * ps));
                    row += "\t" + api.cstring_at(static_cast<GuestAddr>(api.ptr_at(a))) + "/" +
                           hex(api.u32_at(a + ps)) + "/" + hex(api.u32_at(values));
                }
                row += "\t" + hex(api.u32_at(si + unauth));
            }
            out.push_back(row);
        }
        const u8 bad[] = {0x31, 0x00}, seq_empty[] = {0x30, 0x00}, seq_int[] = {0x30, 0x03, 0x02, 0x01, 0x01};
        const std::vector<std::vector<u8>> cases = {{bad, bad + 2}, {seq_empty, seq_empty + 2},
                                                    {seq_int, seq_int + 5}, {enc.begin(), enc.begin() + 5}};
        for (u32 i = 0; i < cases.size(); ++i) {
            const GuestAddr d = api.put(cases[i].data(), cases[i].size());
            const GuestAddr buf = api.alloc(4096, 8), sz = api.alloc(4);
            api.write_u32(sz, 16384);
            set_last_error(0xdead);
            const u32 ok = static_cast<u32>(
                api.call("CryptDecodeObject", {X509 | PKCS7, kType, d, cases[i].size(), 0, buf, sz}));
            out.push_back("sibad " + hex(i) + "\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" +
                          hex(api.u32_at(sz)));
        }
    }

    if (signer_ctx) {
        // CERT_CONTEXT.pCertInfo -> CERT_INFO.SubjectPublicKeyInfo.
        const u64 info = api.ptr_at(signer_ctx + 3 * ps);
        const GuestAddr spki = static_cast<GuestAddr>(info + (is64 ? 96 : 56));
        const u32 spki_size = 6 * ps;
        auto hpki = [&](const std::string& label, u32 alg, u32 flags, u32 enc, GuestAddr key_info, bool null_buf,
                        u32 size_in) {
            const GuestAddr buf = api.alloc(80), sz = api.alloc(4);
            for (u32 i = 0; i < 80; ++i) EXPECT_TRUE(api.memory().write_u8(buf + i, 0x5a).is_ok());
            api.write_u32(sz, size_in);
            set_last_error(0xdead);
            const u32 ok = static_cast<u32>(api.call("CryptHashPublicKeyInfo",
                {0, alg, flags, enc, key_info, null_buf ? 0 : buf, sz}));
            const u32 err = get_last_error();
            const u32 size = api.u32_at(sz);
            std::string row = "hpki " + label + "\t" + hex(ok) + "\t" + hex(err) + "\t" + hex(size) + "\t";
            if (ok && !null_buf) {
                row += bytes_hex(api.bytes_at(buf, size));
            } else {
                u32 touched = 0;
                for (u32 i = 0; i < 80; ++i) touched += api.u8_at(buf + i) != 0x5a;
                row += "touched=" + hex(touched);
            }
            out.push_back(row);
        };
        auto edit = [&](auto&& change) {
            const GuestAddr copy = api.put(api.bytes_at(spki, spki_size).data(), spki_size, 8);
            change(copy);
            return copy;
        };
        hpki("default", 0, 0, X509, spki, false, 64);
        hpki("md5", 0x8003, 0, X509, spki, false, 64);
        hpki("sha1", 0x8004, 0, X509, spki, false, 64);
        hpki("sha256", 0x800c, 0, X509, spki, false, 64);
        hpki("sha384", 0x800d, 0, X509, spki, false, 64);
        hpki("sha512", 0x800e, 0, X509, spki, false, 64);
        hpki("md2", 0x8001, 0, X509, spki, false, 64);
        hpki("md4", 0x8002, 0, X509, spki, false, 64);
        hpki("mac", 0x8005, 0, X509, spki, false, 64);
        hpki("hmac", 0x8009, 0, X509, spki, false, 64);
        hpki("ssl3shamd5", 0x8008, 0, X509, spki, false, 64);
        hpki("rsasign", 0x2400, 0, X509, spki, false, 64);
        hpki("bogus", 0x1234, 0, X509, spki, false, 64);
        hpki("enc-both", 0x8004, 0, X509 | PKCS7, spki, false, 64);
        hpki("enc-pkcs7", 0x8004, 0, PKCS7, spki, false, 64);
        hpki("enc-zero", 0x8004, 0, 0, spki, false, 64);
        hpki("enc-2", 0x8004, 0, 2, spki, false, 64);
        hpki("sizequery", 0x8004, 0, X509, spki, true, 0);
        hpki("sizequery-big", 0x8004, 0, X509, spki, true, 64);
        hpki("exact", 0x8004, 0, X509, spki, false, 20);
        hpki("small", 0x8004, 0, X509, spki, false, 19);
        hpki("zero", 0x8004, 0, X509, spki, false, 0);
        hpki("flags1", 0x8004, 1, X509, spki, false, 64);
        hpki("flagsff", 0x8004, 0xffffffff, X509, spki, false, 64);
        hpki("unused3", 0x8004, 0, X509, edit([&](GuestAddr c) { api.write_u32(c + 5 * ps, 3); }), false, 64);
        hpki("noparams", 0x8004, 0, X509, edit([&](GuestAddr c) {
            api.write_u32(c + ps, 0);
            api.write_ptr(c + 2 * ps, 0);
        }), false, 64);
        hpki("oid1234", 0x8004, 0, X509, edit([&](GuestAddr c) { api.write_ptr(c, api.put_string("1.2.3.4")); }),
             false, 64);
        hpki("oidbogus", 0x8004, 0, X509, edit([&](GuestAddr c) { api.write_ptr(c, api.put_string("bogus")); }),
             false, 64);
        hpki("oidempty", 0x8004, 0, X509, edit([&](GuestAddr c) { api.write_ptr(c, api.put_string("")); }),
             false, 64);
        hpki("emptykey", 0x8004, 0, X509, edit([&](GuestAddr c) { api.write_u32(c + 3 * ps, 0); }), false, 64);
        const u8 bad_params[] = {0x30, 0x05};
        hpki("badparams", 0x8004, 0, X509, edit([&](GuestAddr c) {
            api.write_u32(c + ps, 2);
            api.write_ptr(c + 2 * ps, api.put(bad_params, 2));
        }), false, 64);
        std::vector<u8> big(300);
        for (u32 i = 0; i < 300; ++i) big[i] = static_cast<u8>(i);
        hpki("bigkey", 0x8004, 0, X509, edit([&](GuestAddr c) {
            api.write_u32(c + 3 * ps, 300);
            api.write_ptr(c + 4 * ps, api.put(big.data(), big.size()));
            api.write_u32(c + 5 * ps, 0);
        }), false, 64);
        api.call("CertFreeCertificateContext", {signer_ctx});
    }

    // CryptDecodeObject beside CryptDecodeObjectEx (X509_ENHANCED_KEY_USAGE = 36).
    const u8 eku[] = {0x30, 0x0c, 0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x0a, 0x03, 0x01};
    const u8 eku2[] = {0x30, 0x14, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x03,
                       0x06, 0x08, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01};
    const u8 badtag[] = {0x31, 0x00};
    const u8 trunc[] = {0x30, 0x0c, 0x06, 0x0a, 0x2b};
    const GuestAddr eku_oid = api.put_string("2.5.29.37");
    auto dec = [&](const std::string& label, u32 enc, u64 type, const u8* der, u32 len, u32 flags, bool null_buf,
                   u32 size_in) {
        const GuestAddr data = api.put(der, len ? len : 1);
        for (int which = 0; which < 2; ++which) {
            const GuestAddr buf = api.alloc(256, 8), sz = api.alloc(4), alloc_ptr = api.alloc(8, 8);
            for (u32 i = 0; i < 256; ++i) EXPECT_TRUE(api.memory().write_u8(buf + i, 0x5a).is_ok());
            api.write_u32(sz, size_in);
            set_last_error(0xdead);
            const bool alloc = (flags & 0x8000) != 0;
            const u32 ok = static_cast<u32>(which == 0
                ? api.call("CryptDecodeObject", {enc, type, data, len, flags, null_buf ? 0 : buf, sz})
                : api.call("CryptDecodeObjectEx",
                           {enc, type, data, len, flags, 0, alloc ? alloc_ptr : (null_buf ? 0 : buf), sz}));
            const u32 err = get_last_error();
            std::string row = std::string(which == 0 ? "dec " : "decex ") + label + "\t" + hex(ok) + "\t" +
                              hex(err) + "\t" + hex(api.u32_at(sz));
            const GuestAddr result = which == 1 && alloc ? static_cast<GuestAddr>(api.ptr_at(alloc_ptr)) : buf;
            auto touched = [&] {
                u32 n = 0;
                for (u32 i = 0; i < 256; ++i) n += api.u8_at(buf + i) != 0x5a;
                return "\ttouched=" + hex(n);
            };
            if (ok && result && !null_buf) {
                if (which == 0 && alloc) {
                    row += touched();
                } else {
                    const u32 count = api.u32_at(result);
                    row += "\t" + hex(count);
                    const u64 array = api.ptr_at(result + ps);
                    for (u32 i = 0; i < count; ++i)
                        row += "\t" + api.cstring_at(static_cast<GuestAddr>(api.ptr_at(array + i * ps)));
                }
            } else {
                row += touched();
            }
            out.push_back(row);
        }
    };
    dec("eku", X509, 36, eku, sizeof eku, 0, false, 256);
    dec("eku2", X509, 36, eku2, sizeof eku2, 0, false, 256);
    dec("eku-oid", X509, eku_oid, eku2, sizeof eku2, 0, false, 256);
    dec("eku-both", X509 | PKCS7, 36, eku, sizeof eku, 0, false, 256);
    dec("eku-pkcs7", PKCS7, 36, eku, sizeof eku, 0, false, 256);
    dec("eku-enc0", 0, 36, eku, sizeof eku, 0, false, 256);
    dec("eku-size", X509, 36, eku, sizeof eku, 0, true, 0);
    dec("eku-small", X509, 36, eku, sizeof eku, 0, false, 4);
    dec("eku-alloc", X509, 36, eku, sizeof eku, 0x8000, false, 256);
    dec("eku-nocopy", X509, 36, eku, sizeof eku, 0x8, false, 256);
    dec("badtag", X509, 36, badtag, sizeof badtag, 0, false, 256);
    dec("trunc", X509, 36, trunc, sizeof trunc, 0, false, 256);
    dec("empty", X509, 36, eku, 0, 0, false, 256);
    {
        // szOID_RSA_signingTime / PKCS_UTC_TIME, the probe's cases in order.
        static const char* times[] = {
            "170d3131303631313031323334355a", "170d3439313233313233353935395a",
            "170d3530303130313030303030305a", "170b313130363131303132335a",
            "17113131303631313031323334352b30313030", "17113131303631313031323334352d30313330",
            "170c3131303631313031323334", "180f32303530303130313030303030305a",
            "180f31393439313233313233353935395a", "180f32303131303631313031323334355a",
            "181332303131303631313031323334352e3132335a", "170d3131313333313031323334355a",
            "170d3131303233303031323334355a", "170d3131303631313235323334355a", "0400", "170d313130363131", "",
            "170d3131303631313031323334355a00", "17113131303631313031323334352d30303030",
            "17113131303631313031323334352d30313030", "17113131303631313031323334352d30303330",
            "17113131303631313031323334352b30303330", "17113131303631313031323334352b30313330",
            "17113131303631313031323334352d30303031", "17113131303631313031323334352b30303031",
            "17113131303631313031323334352b31323030", "17113131303631313031323334352b32353030",
            "17113131303631313031323334352b30303630", "170f313130363131303132332b30313030",
        };
        const GuestAddr signing_time = api.put_string("1.2.840.113549.1.9.5");
        for (u32 t = 0; t < std::size(times); ++t) {
            std::vector<u8> der;
            for (const char* h = times[t]; h[0] && h[1]; h += 2)
                der.push_back(static_cast<u8>(std::stoul(std::string(h, 2), nullptr, 16)));
            const GuestAddr d = api.put(der.data(), der.size());
            for (int ty = 0; ty < 2; ++ty) {
                const GuestAddr ft = api.alloc(64, 8), sz = api.alloc(4);
                api.write_u32(ft, 0x5a5a5a5a);
                api.write_u32(ft + 4, 0x5a5a5a5a);
                api.write_u32(sz, ty == 0 ? 8 : 64);
                set_last_error(0xdead);
                const u32 ok = static_cast<u32>(api.call("CryptDecodeObject",
                    {X509 | PKCS7, ty == 0 ? signing_time : 17, d, der.size(), 0, ft, sz}));
                out.push_back("time " + hex(t) + (ty == 0 ? " oid" : " num") + "\t" + hex(ok) + "\t" +
                              hex(get_last_error()) + "\t" + hex(api.u32_at(sz)) + "\t" + hex(api.u32_at(ft + 4)) +
                              ":" + hex(api.u32_at(ft)));
            }
        }
        const u8 ut[] = {0x17, 0x0d, '1', '1', '0', '6', '1', '1', '0', '1', '2', '3', '4', '5', 'Z'};
        const GuestAddr d = api.put(ut, sizeof ut);
        const GuestAddr buf = api.alloc(16, 8), sz = api.alloc(4), ap = api.alloc(8, 8);
        api.write_u32(sz, 0);
        set_last_error(0xdead);
        u32 ok = static_cast<u32>(api.call("CryptDecodeObject", {X509, signing_time, d, sizeof ut, 0, 0, sz}));
        out.push_back("timesize\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" + hex(api.u32_at(sz)));
        api.write_u32(sz, 4);
        set_last_error(0xdead);
        ok = static_cast<u32>(api.call("CryptDecodeObject", {X509, signing_time, d, sizeof ut, 0, buf, sz}));
        out.push_back("timesmall\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" + hex(api.u32_at(sz)));
        api.write_u32(sz, 0);
        set_last_error(0xdead);
        ok = static_cast<u32>(api.call("CryptDecodeObjectEx", {X509, signing_time, d, sizeof ut, 0x8000, 0, ap, sz}));
        std::string row = "timealloc\t" + hex(ok) + "\t" + hex(get_last_error()) + "\t" + hex(api.u32_at(sz));
        if (ok) {
            const GuestAddr f = static_cast<GuestAddr>(api.ptr_at(ap));
            row += "\t" + hex(api.u32_at(f + 4)) + ":" + hex(api.u32_at(f));
        }
        out.push_back(row);
    }
    std::filesystem::remove_all(dir);
    return out;
}

class CryptSignerTrioTest : public ::testing::TestWithParam<bool> {};

TEST_P(CryptSignerTrioTest, EveryProbeRowMatchesWindows) {
    // "dec eku-alloc" reports how many bytes of pvStructInfo changed when
    // CryptDecodeObject stored the allocated pointer there; a pointer byte can
    // equal the 0x5a fill by chance, so the row is compared as "a pointer was
    // stored" (pointer-size or pointer-size - 1 bytes changed).
    auto normalize = [](std::vector<std::string> rows, bool is64) {
        const std::string ps = is64 ? "8" : "4", ps1 = is64 ? "7" : "3";
        for (auto& r : rows) {
            if (r.rfind("dec eku-alloc\t", 0) != 0) continue;
            for (const auto& n : {ps, ps1}) {
                const auto at = r.rfind("touched=" + n);
                if (at != std::string::npos && at + 8 + n.size() == r.size()) r = r.substr(0, at) + "touched=ptr";
            }
        }
        return rows;
    };
    const auto got = normalize(replay(GetParam()), GetParam());
    const auto want = normalize(expected(GetParam()), GetParam());
    EXPECT_EQ(got.size(), want.size());
    for (usize i = 0; i < std::min(got.size(), want.size()); ++i) EXPECT_EQ(got[i], want[i]) << "row " << i;
}

INSTANTIATE_TEST_SUITE_P(Bitness, CryptSignerTrioTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

} // namespace
