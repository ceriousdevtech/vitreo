// CryptVerifyCertificateSignatureEx over md5WithRSA and OIW sha1RSASign
// certificates (../crypt_legacy_sigalg_certs.h). Prints: case, BOOL, GetLastError.
//   x86_64-w64-mingw32-gcc -O1 -I.. -o sig64.exe crypt_legacy_sigalg_probe.c -lcrypt32
//   i686-w64-mingw32-gcc   -O1 -I.. -o sig32.exe crypt_legacy_sigalg_probe.c -lcrypt32
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>
#include "crypt_legacy_sigalg_certs.h"

static void run(const char* name, const unsigned char* der, DWORD size, int flip, PCERT_PUBLIC_KEY_INFO key) {
    unsigned char copy[4096];
    memcpy(copy, der, size);
    if (flip) copy[size / 3] ^= 1;
    CRYPT_DATA_BLOB blob = {size, copy};
    SetLastError(0xdead);
    BOOL ok = CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING, CRYPT_VERIFY_CERT_SIGN_SUBJECT_BLOB,
                                                &blob, CRYPT_VERIFY_CERT_SIGN_ISSUER_PUBKEY, key, 0, NULL);
    printf("%s\t%d\t%08lx\n", name, ok, ok ? 0ul : GetLastError());
}

int main(void) {
    PCCERT_CONTEXT root = CertCreateCertificateContext(X509_ASN_ENCODING, kMd5RootDer, sizeof kMd5RootDer);
    PCCERT_CONTEXT leaf = CertCreateCertificateContext(X509_ASN_ENCODING, kOiwLeafDer, sizeof kOiwLeafDer);
    if (!root || !leaf) { printf("parse\t%08lx\n", GetLastError()); return 1; }
    PCERT_PUBLIC_KEY_INFO root_key = &root->pCertInfo->SubjectPublicKeyInfo;
    PCERT_PUBLIC_KEY_INFO leaf_key = &leaf->pCertInfo->SubjectPublicKeyInfo;
    run("md5_root_self", kMd5RootDer, sizeof kMd5RootDer, 0, root_key);
    run("md5_root_tampered", kMd5RootDer, sizeof kMd5RootDer, 1, root_key);
    run("oiw_leaf", kOiwLeafDer, sizeof kOiwLeafDer, 0, root_key);
    run("oiw_leaf_tampered", kOiwLeafDer, sizeof kOiwLeafDer, 1, root_key);
    run("oiw_leaf_wrong_key", kOiwLeafDer, sizeof kOiwLeafDer, 0, leaf_key);
    run("md5_root_wrong_key", kMd5RootDer, sizeof kMd5RootDer, 0, leaf_key);
    return 0;
}
