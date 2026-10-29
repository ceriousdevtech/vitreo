// Generated from cert_usage_probe Windows output; do not edit.
struct UsageLayout { u32 count, extensions, stride, blob, data; };
constexpr UsageLayout usage_layout_32{0x68u,0x6cu,0x10u,0x8u,0x4u};
constexpr UsageLayout usage_layout_64{0xc0u,0xc8u,0x20u,0x10u,0x8u};
constexpr u32 asn_eod = 0x80093102u;
constexpr u32 asn_corrupt = 0x80093103u;
constexpr u32 asn_badtag = 0x8009310bu;
constexpr u32 asn_large = 0x80093104u;
constexpr u32 encoding_not_found = 0x2u;
constexpr u32 cert_encoding_mask = 0xffffu;
constexpr u32 msg_encoding_mask = 0xffff0000u;
constexpr u32 x509_encoding = 0x1u;
constexpr u32 pkcs7_encoding = 0x10000u;
constexpr u32 bad_encode = 0x80092002u;
constexpr const char* key_usage_oid = "2.5.29.15";
constexpr const char* key_attributes_oid = "2.5.29.2";
constexpr u32 access_violation_status = 0xc0000005u;
