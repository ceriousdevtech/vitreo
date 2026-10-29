// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_auth_root.cpp
/// @brief The authroot.stl CTL format (Learn CTL_INFO / CTL_ENTRY;
///        wincrypt.h property ids). The CTL here is built in the test to
///        that grammar -- Microsoft's list is not committed -- with each
///        property the automatic root update reads.

#include <gtest/gtest.h>

#include "winapi/system/crypt32/auth_root.hpp"

#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi::crypt32;

namespace {

std::vector<u8> tlv(u8 tag, const std::vector<u8>& body) {
    std::vector<u8> out{tag};
    if (body.size() < 0x80) {
        out.push_back(static_cast<u8>(body.size()));
    } else {
        out.push_back(0x82);
        out.push_back(static_cast<u8>(body.size() >> 8));
        out.push_back(static_cast<u8>(body.size()));
    }
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::vector<u8> cat(std::initializer_list<std::vector<u8>> parts) {
    std::vector<u8> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

std::vector<u8> oid(const std::string& dotted) {
    std::vector<unsigned long> arcs;
    for (usize at = 0; at < dotted.size();) {
        const usize dot = dotted.find('.', at);
        arcs.push_back(std::stoul(dotted.substr(at, dot - at)));
        at = dot == std::string::npos ? dotted.size() : dot + 1;
    }
    std::vector<u8> body{static_cast<u8>(arcs[0] * 40 + arcs[1])};
    for (usize i = 2; i < arcs.size(); ++i) {
        std::vector<u8> chunk{static_cast<u8>(arcs[i] & 0x7F)};
        for (unsigned long v = arcs[i] >> 7; v != 0; v >>= 7) chunk.insert(chunk.begin(), static_cast<u8>(0x80 | (v & 0x7F)));
        body.insert(body.end(), chunk.begin(), chunk.end());
    }
    return tlv(0x06, body);
}

/// Attribute { szOID_CERT_PROP_ID_PREFIX + id, SET { OCTET STRING value } }.
std::vector<u8> prop(int id, const std::vector<u8>& value) {
    return tlv(0x30, cat({oid("1.3.6.1.4.1.311.10.11." + std::to_string(id)), tlv(0x31, tlv(0x04, value))}));
}

std::vector<u8> eku(std::initializer_list<const char*> oids) {
    std::vector<u8> body;
    for (const char* o : oids) {
        const auto e = oid(o);
        body.insert(body.end(), e.begin(), e.end());
    }
    return tlv(0x30, body);
}

std::vector<u8> ctl(const char* usage, const std::vector<u8>& subjects) {
    return tlv(0x30, cat({tlv(0x30, oid(usage)),                     // subjectUsage
                          tlv(0x02, {0x01}),                          // sequenceNumber
                          tlv(0x17, std::vector<u8>{'2', '6', '1', '0', '2', '7', '0', '0', '0', '0', '0', '0', 'Z'}),
                          tlv(0x30, cat({oid("1.3.14.3.2.26"), tlv(0x05, {})})),  // subjectAlgorithm SHA-1
                          tlv(0x30, subjects)}));
}

}  // namespace

TEST(AuthRootCtl, ParsesEveryPropertyTheUpdateUses) {
    const std::vector<u8> sha1_a(20, 0xA1), sha1_b(20, 0xB2), key_id(20, 0x4A), md5(16, 0x6C);
    // FILETIME 0x01D39A266FDC0000 (2018-01-31), little-endian.
    const std::vector<u8> filetime{0x00, 0x00, 0xDC, 0x6F, 0x26, 0x9A, 0xD3, 0x01};
    const auto entry_a = tlv(0x30, cat({tlv(0x04, sha1_a),
                                        tlv(0x31, cat({prop(104, filetime),
                                                       prop(105, eku({"1.3.6.1.4.1.311.60.3.2"})),
                                                       prop(29, md5), prop(20, key_id),
                                                       prop(9, eku({"1.3.6.1.5.5.7.3.3"})),
                                                       prop(122, eku({"1.3.6.1.5.5.7.3.1"}))}))}));
    const auto entry_b = tlv(0x30, cat({tlv(0x04, sha1_b), tlv(0x31, prop(84, {0x30, 0x00}))}));
    const auto entries = parse_auth_root_ctl(ctl("1.3.6.1.4.1.311.10.3.9", cat({entry_a, entry_b})));
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 2u);
    const auto& a = (*entries)[0];
    EXPECT_TRUE(std::equal(a.sha1.begin(), a.sha1.end(), sha1_a.begin()));
    EXPECT_EQ(a.key_id, key_id);
    EXPECT_EQ(a.subject_name_md5, md5);
    ASSERT_TRUE(a.disallowed_filetime.has_value());
    EXPECT_EQ(*a.disallowed_filetime, 0x01D39A266FDC0000ull);
    EXPECT_EQ(a.eku, eku({"1.3.6.1.5.5.7.3.3"}));
    EXPECT_EQ(a.disallowed_eku, eku({"1.3.6.1.5.5.7.3.1"}));
    EXPECT_FALSE(a.name_constraints);
    EXPECT_EQ(a.der, entry_a);
    EXPECT_TRUE((*entries)[1].name_constraints);

    // The entry kept beside an installed root parses back to the same thing.
    const auto again = parse_auth_root_entry(a.der);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->disallowed_filetime, a.disallowed_filetime);
    EXPECT_EQ(again->eku, a.eku);
}

TEST(AuthRootCtl, RejectsAListForAnotherUsage) {
    const auto entry = tlv(0x30, tlv(0x04, std::vector<u8>(20, 1)));
    EXPECT_FALSE(parse_auth_root_ctl(ctl("1.3.6.1.4.1.311.10.3.1", entry)).has_value());
    EXPECT_FALSE(parse_auth_root_ctl(std::vector<u8>{0x04, 0x00}).has_value());
    EXPECT_FALSE(parse_auth_root_entry(tlv(0x30, tlv(0x04, std::vector<u8>(19, 1)))).has_value());
}
