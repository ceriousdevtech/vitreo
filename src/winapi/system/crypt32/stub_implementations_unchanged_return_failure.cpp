// SPDX-License-Identifier: LGPL-2.1-only
/// @file stub_implementations_unchanged_return_failure.cpp
/// @brief Host X.509 certificate verification for CRYPT32.dll.

#include "crypt32.hpp"
#include "winapi/system/crypt32/crypt32_internal.hpp"
#include "x509_parse.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CommonCrypto/CommonDigest.h>
#endif

/// @file stub_implementations_unchanged_return_failure.cpp
/// @brief Stub Implementations (unchanged — return failure / no-op)
///
/// Closed cut from crypt32.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Stub Implementations (unchanged — return failure / no-op)
// =============================================================================

// Windows CERT_TRUST error flags
static constexpr u32 CERT_TRUST_IS_NOT_TIME_VALID        = 0x00000001;
static constexpr u32 CERT_TRUST_IS_UNTRUSTED_ROOT        = 0x00000020;
static constexpr u32 CERT_TRUST_IS_PARTIAL_CHAIN         = 0x00010000;

u32 Crypt32Manager::host_verify_cert_chain(
    const std::vector<u8>& leaf_der,
    const std::vector<std::vector<u8>>& chain_ders,
    const std::vector<std::vector<u8>>& trust_anchors) {

#ifdef __APPLE__
    // Create SecCertificateRef for leaf
    CFDataRef leaf_data = CFDataCreate(kCFAllocatorDefault, leaf_der.data(),
                                       static_cast<CFIndex>(leaf_der.size()));
    if (!leaf_data) {
        VITREO_TRACE("[crypt32] SecTrust: CFDataCreate failed for leaf cert\n");
        return CERT_TRUST_IS_PARTIAL_CHAIN;
    }

    SecCertificateRef leaf_cert = SecCertificateCreateWithData(kCFAllocatorDefault, leaf_data);
    CFRelease(leaf_data);
    if (!leaf_cert) {
        VITREO_TRACE("[crypt32] SecTrust: SecCertificateCreateWithData failed for leaf\n");
        return CERT_TRUST_IS_PARTIAL_CHAIN;
    }

    // Build cert array: leaf + intermediates
    CFMutableArrayRef cert_array = CFArrayCreateMutable(kCFAllocatorDefault,
        1 + static_cast<CFIndex>(chain_ders.size()), &kCFTypeArrayCallBacks);
    CFArrayAppendValue(cert_array, leaf_cert);

    for (const auto& der : chain_ders) {
        CFDataRef der_data = CFDataCreate(kCFAllocatorDefault, der.data(),
                                           static_cast<CFIndex>(der.size()));
        if (der_data) {
            SecCertificateRef cert = SecCertificateCreateWithData(kCFAllocatorDefault, der_data);
            CFRelease(der_data);
            if (cert) {
                CFArrayAppendValue(cert_array, cert);
                CFRelease(cert);
            }
        }
    }

    // CertGetCertificateChain builds a policy-neutral X.509 chain. Purpose-
    // specific checks are performed later by CertVerifyCertificateChainPolicy.
    SecPolicyRef policy = SecPolicyCreateBasicX509();

    // Create trust object
    SecTrustRef trust = nullptr;
    OSStatus status = SecTrustCreateWithCertificates(cert_array, policy, &trust);
    CFRelease(cert_array);
    CFRelease(leaf_cert);
    CFRelease(policy);

    if (status != errSecSuccess || !trust) {
        VITREO_TRACE("[crypt32] SecTrust: SecTrustCreateWithCertificates failed: %d\n",
                     static_cast<int>(status));
        return CERT_TRUST_IS_PARTIAL_CHAIN;
    }

    bool has_capsule_anchors = false;
    if (!trust_anchors.empty()) {
        CFMutableArrayRef anchors = CFArrayCreateMutable(
            kCFAllocatorDefault,
            static_cast<CFIndex>(trust_anchors.size()),
            &kCFTypeArrayCallBacks);
        if (!anchors) {
            CFRelease(trust);
            return CERT_TRUST_IS_PARTIAL_CHAIN;
        }
        for (const auto& der : trust_anchors) {
            CFDataRef anchor_data = CFDataCreate(
                kCFAllocatorDefault, der.data(),
                static_cast<CFIndex>(der.size()));
            SecCertificateRef anchor = anchor_data
                ? SecCertificateCreateWithData(kCFAllocatorDefault, anchor_data)
                : nullptr;
            if (anchor_data) CFRelease(anchor_data);
            if (anchor) {
                CFArrayAppendValue(anchors, anchor);
                CFRelease(anchor);
            }
        }
        if (CFArrayGetCount(anchors) != 0) {
            (void)SecTrustSetAnchorCertificates(trust, anchors);
            (void)SecTrustSetAnchorCertificatesOnly(trust, true);
            has_capsule_anchors = true;
        }
        CFRelease(anchors);
    }

    // A Windows ROOT store includes OS-managed roots plus roots installed by
    // applications. Evaluate the capsule overlay first so its self-signed
    // roots retain full anchor semantics. If that path does not validate,
    // restore Security.framework's platform roots and evaluate that namespace.
    // This models the union without making a non-empty capsule store replace
    // every public CA, which surfaces as a certificate-authority failure.
    CFErrorRef error = nullptr;
    bool trusted = SecTrustEvaluateWithError(trust, &error);
    if (!trusted && has_capsule_anchors) {
        if (error) {
            CFRelease(error);
            error = nullptr;
        }
        if (SecTrustSetAnchorCertificates(trust, nullptr) == errSecSuccess) {
            trusted = SecTrustEvaluateWithError(trust, &error);
        }
    }

    u32 result = 0;
    if (!trusted) {
        // Map SecTrust error to Windows CERT_TRUST flags
        if (error) {
            CFIndex err_code = CFErrorGetCode(error);
            VITREO_TRACE("[crypt32] SecTrust: evaluation failed, error code=%ld\n",
                         static_cast<long>(err_code));
            CFRelease(error);

            // errSecNotTrusted / errSecCreateChainFailed → untrusted root
            // errSecCertificateExpired → not time valid
            if (err_code == errSecCertificateExpired) {
                result = CERT_TRUST_IS_NOT_TIME_VALID;
            } else {
                result = CERT_TRUST_IS_UNTRUSTED_ROOT;
            }
        } else {
            result = CERT_TRUST_IS_UNTRUSTED_ROOT;
        }
    } else {
        VITREO_TRACE("[crypt32] SecTrust: certificate chain TRUSTED\n");
    }

    CFRelease(trust);
    return result;
#else
    static const bool reported = [] {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED crypt32!CertGetCertificateChain: host certificate verifier unavailable\n");
        return true;
    }();
    (void)reported;
    return CERT_TRUST_IS_PARTIAL_CHAIN;
#endif
}

} // namespace vitreo::winapi
