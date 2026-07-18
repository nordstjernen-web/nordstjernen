/* Nordstjernen — SubtleCrypto primitives implemented over OpenSSL libcrypto.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "webcrypto.h"

#include <string.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#include <openssl/params.h>
#include <openssl/kdf.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/x509.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>

static void *
ns_crypto_err(char **err, const char *prefix)
{
    if (err && !*err) {
        unsigned long e = ERR_peek_last_error();
        char buf[256];
        if (e) {
            ERR_error_string_n(e, buf, sizeof buf);
            *err = g_strdup_printf("%s: %s", prefix, buf);
        } else {
            *err = g_strdup(prefix);
        }
    }
    ERR_clear_error();
    return NULL;
}

void
ns_crypto_key_unref(ns_crypto_key *k)
{
    if (!k) return;
    if (--k->refcount > 0) return;
    g_free(k->algo);
    g_free(k->hash);
    g_free(k->curve);
    if (k->raw) {
        OPENSSL_cleanse(k->raw, k->raw_len);
        g_free(k->raw);
    }
    if (k->pkey) EVP_PKEY_free(k->pkey);
    g_free(k);
}

static ns_crypto_key *
ns_crypto_key_new(ns_ck_type type, const char *algo, const char *hash,
                  const char *curve, int bits, gboolean extractable,
                  guint32 usages)
{
    ns_crypto_key *k = g_new0(ns_crypto_key, 1);
    k->type = type;
    k->algo = g_strdup(algo);
    k->hash = hash ? g_strdup(hash) : NULL;
    k->curve = curve ? g_strdup(curve) : NULL;
    k->bits = bits;
    k->extractable = extractable;
    k->usages = usages;
    k->refcount = 1;
    return k;
}

static const EVP_MD *
ns_crypto_md(const char *hash)
{
    if (!hash) return NULL;
    if (!g_ascii_strcasecmp(hash, "SHA-1"))   return EVP_sha1();
    if (!g_ascii_strcasecmp(hash, "SHA-256")) return EVP_sha256();
    if (!g_ascii_strcasecmp(hash, "SHA-384")) return EVP_sha384();
    if (!g_ascii_strcasecmp(hash, "SHA-512")) return EVP_sha512();
    if (!g_ascii_strcasecmp(hash, "SHA3-256")) return EVP_sha3_256();
    if (!g_ascii_strcasecmp(hash, "SHA3-384")) return EVP_sha3_384();
    if (!g_ascii_strcasecmp(hash, "SHA3-512")) return EVP_sha3_512();
    return NULL;
}

guint8 *
ns_crypto_digest(const char *hash, const guint8 *data, gsize len, gsize *out_len)
{
    const EVP_MD *md = ns_crypto_md(hash);
    if (!md) return NULL;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return NULL;
    guint8 *out = g_malloc((gsize)EVP_MD_get_size(md));
    unsigned int n = 0;
    if (EVP_DigestInit_ex(ctx, md, NULL) != 1 ||
        EVP_DigestUpdate(ctx, data ? data : (const guint8 *)"", len) != 1 ||
        EVP_DigestFinal_ex(ctx, out, &n) != 1) {
        EVP_MD_CTX_free(ctx);
        g_free(out);
        return NULL;
    }
    EVP_MD_CTX_free(ctx);
    if (out_len) *out_len = n;
    return out;
}

static const char *
ns_crypto_md_name(const char *hash)
{
    if (!hash) return NULL;
    if (!g_ascii_strcasecmp(hash, "SHA-1"))   return "SHA1";
    if (!g_ascii_strcasecmp(hash, "SHA-256")) return "SHA256";
    if (!g_ascii_strcasecmp(hash, "SHA-384")) return "SHA384";
    if (!g_ascii_strcasecmp(hash, "SHA-512")) return "SHA512";
    return NULL;
}

static const char *
ns_crypto_curve_group(const char *curve)
{
    if (!curve) return NULL;
    if (!g_ascii_strcasecmp(curve, "P-256")) return "prime256v1";
    if (!g_ascii_strcasecmp(curve, "P-384")) return "secp384r1";
    if (!g_ascii_strcasecmp(curve, "P-521")) return "secp521r1";
    return NULL;
}

static int
ns_crypto_curve_order_bytes(const char *curve)
{
    if (!curve) return 0;
    if (!g_ascii_strcasecmp(curve, "P-256")) return 32;
    if (!g_ascii_strcasecmp(curve, "P-384")) return 48;
    if (!g_ascii_strcasecmp(curve, "P-521")) return 66;
    return 0;
}

static const char *
ns_crypto_okp_name(const char *algo)
{
    if (!algo) return NULL;
    if (!g_ascii_strcasecmp(algo, "Ed25519")) return "ED25519";
    if (!g_ascii_strcasecmp(algo, "X25519"))  return "X25519";
    return NULL;
}

ns_crypto_key *
ns_crypto_generate_secret(const char *algo, const char *hash, int length_bits,
                          gboolean extractable, guint32 usages, char **err)
{
    if (length_bits <= 0 || length_bits % 8 != 0 || length_bits > 4096) {
        if (err) *err = g_strdup("OperationError: invalid key length");
        return NULL;
    }
    gsize len = (gsize)length_bits / 8;
    guint8 *buf = g_malloc(len);
    if (RAND_bytes(buf, (int)len) != 1) {
        OPENSSL_cleanse(buf, len);
        g_free(buf);
        return ns_crypto_err(err, "OperationError: RNG failure");
    }
    ns_crypto_key *k = ns_crypto_key_new(NS_CK_SECRET, algo, hash, NULL,
                                         length_bits, extractable, usages);
    k->raw = buf;
    k->raw_len = len;
    return k;
}

gboolean
ns_crypto_generate_keypair(const char *algo, const char *hash, const char *curve,
                           int modulus_bits, guint32 pubexp, gboolean extractable,
                           guint32 usages, ns_crypto_key **pub, ns_crypto_key **priv,
                           char **err)
{
    *pub = NULL;
    *priv = NULL;
    EVP_PKEY *pkey = NULL;
    gboolean is_ec = !g_ascii_strcasecmp(algo, "ECDSA") ||
                     !g_ascii_strcasecmp(algo, "ECDH");
    const char *okp = ns_crypto_okp_name(algo);

    if (okp) {
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, okp, NULL);
        if (!ctx || EVP_PKEY_keygen_init(ctx) <= 0 ||
            EVP_PKEY_generate(ctx, &pkey) <= 0) {
            EVP_PKEY_CTX_free(ctx);
            return ns_crypto_err(err, "OperationError: keygen"), FALSE;
        }
        EVP_PKEY_CTX_free(ctx);
    } else if (is_ec) {
        const char *group = ns_crypto_curve_group(curve);
        if (!group) { if (err) *err = g_strdup("NotSupportedError: curve"); return FALSE; }
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
        if (!ctx || EVP_PKEY_keygen_init(ctx) <= 0 ||
            EVP_PKEY_CTX_set_group_name(ctx, group) <= 0 ||
            EVP_PKEY_generate(ctx, &pkey) <= 0) {
            EVP_PKEY_CTX_free(ctx);
            return ns_crypto_err(err, "OperationError: EC keygen"), FALSE;
        }
        EVP_PKEY_CTX_free(ctx);
    } else {
        if (modulus_bits < 256 || modulus_bits > 16384) {
            if (err) *err = g_strdup("OperationError: invalid modulus length");
            return FALSE;
        }
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
        BIGNUM *e = BN_new();
        if (!e || !BN_set_word(e, pubexp ? pubexp : 65537)) {
            BN_free(e);
            EVP_PKEY_CTX_free(ctx);
            return ns_crypto_err(err, "OperationError: RSA keygen"), FALSE;
        }
        if (!ctx || EVP_PKEY_keygen_init(ctx) <= 0 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, modulus_bits) <= 0 ||
            EVP_PKEY_CTX_set1_rsa_keygen_pubexp(ctx, e) <= 0 ||
            EVP_PKEY_generate(ctx, &pkey) <= 0) {
            BN_free(e);
            EVP_PKEY_CTX_free(ctx);
            return ns_crypto_err(err, "OperationError: RSA keygen"), FALSE;
        }
        BN_free(e);
        EVP_PKEY_CTX_free(ctx);
    }

    int bits = EVP_PKEY_get_bits(pkey);
    ns_crypto_key *pk = ns_crypto_key_new(NS_CK_PUBLIC, algo, hash, curve, bits,
                                          TRUE, usages);
    ns_crypto_key *sk = ns_crypto_key_new(NS_CK_PRIVATE, algo, hash, curve, bits,
                                          extractable, usages);
    pk->pkey = pkey;
    sk->pkey = EVP_PKEY_dup(pkey);
    if (!sk->pkey) {
        ns_crypto_key_unref(pk);
        ns_crypto_key_unref(sk);
        return ns_crypto_err(err, "OperationError: key duplication"), FALSE;
    }
    *pub = pk;
    *priv = sk;
    return TRUE;
}

static EVP_PKEY *
ns_crypto_pkey_from_der(const char *format, const guint8 *data, gsize len)
{
    const unsigned char *p = data;
    if (!g_strcmp0(format, "spki")) {
        return d2i_PUBKEY(NULL, &p, (long)len);
    }
    if (!g_strcmp0(format, "pkcs8")) {
        PKCS8_PRIV_KEY_INFO *p8 = d2i_PKCS8_PRIV_KEY_INFO(NULL, &p, (long)len);
        if (!p8) return NULL;
        EVP_PKEY *pk = EVP_PKCS82PKEY(p8);
        PKCS8_PRIV_KEY_INFO_free(p8);
        return pk;
    }
    return NULL;
}

ns_crypto_key *
ns_crypto_import_raw(const char *format, const guint8 *data, gsize len,
                     const char *algo, const char *hash, const char *curve,
                     gboolean extractable, guint32 usages, char **err)
{
    gboolean symmetric = !g_ascii_strcasecmp(algo, "HMAC") ||
                         !g_ascii_strncasecmp(algo, "AES", 3) ||
                         !g_ascii_strcasecmp(algo, "PBKDF2") ||
                         !g_ascii_strcasecmp(algo, "HKDF");

    if (!g_strcmp0(format, "raw") && symmetric) {
        if (!g_ascii_strncasecmp(algo, "AES", 3) &&
            len != 16 && len != 24 && len != 32) {
            if (err) *err = g_strdup("DataError: invalid AES key length");
            return NULL;
        }
        ns_crypto_key *k = ns_crypto_key_new(NS_CK_SECRET, algo, hash, NULL,
                                             (int)len * 8, extractable, usages);
        k->raw = g_memdup2(data, len);
        k->raw_len = len;
        return k;
    }

    if (!g_strcmp0(format, "raw") &&
        (!g_ascii_strcasecmp(algo, "ECDSA") || !g_ascii_strcasecmp(algo, "ECDH"))) {
        const char *group = ns_crypto_curve_group(curve);
        if (!group) { if (err) *err = g_strdup("NotSupportedError: curve"); return NULL; }
        OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
        OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME,
                                        group, 0);
        OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, data, len);
        OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(bld);
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
        EVP_PKEY *pkey = NULL;
        if (!ctx || EVP_PKEY_fromdata_init(ctx) <= 0 ||
            EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
            OSSL_PARAM_free(params);
            OSSL_PARAM_BLD_free(bld);
            EVP_PKEY_CTX_free(ctx);
            return ns_crypto_err(err, "DataError: EC raw import");
        }
        OSSL_PARAM_free(params);
        OSSL_PARAM_BLD_free(bld);
        EVP_PKEY_CTX_free(ctx);
        ns_crypto_key *k = ns_crypto_key_new(NS_CK_PUBLIC, algo, hash, curve,
                                             EVP_PKEY_get_bits(pkey), extractable,
                                             usages);
        k->pkey = pkey;
        return k;
    }

    if (!g_strcmp0(format, "raw") && ns_crypto_okp_name(algo)) {
        EVP_PKEY *okp_pkey = EVP_PKEY_new_raw_public_key_ex(
            NULL, ns_crypto_okp_name(algo), NULL, data, len);
        if (!okp_pkey) return ns_crypto_err(err, "DataError: raw import");
        ns_crypto_key *k = ns_crypto_key_new(NS_CK_PUBLIC, algo, hash, NULL,
                                             EVP_PKEY_get_bits(okp_pkey),
                                             extractable, usages);
        k->pkey = okp_pkey;
        return k;
    }

    EVP_PKEY *pkey = ns_crypto_pkey_from_der(format, data, len);
    if (!pkey) return ns_crypto_err(err, "DataError: key import");
    ns_ck_type t = !g_strcmp0(format, "pkcs8") ? NS_CK_PRIVATE : NS_CK_PUBLIC;
    ns_crypto_key *k = ns_crypto_key_new(t, algo, hash, curve,
                                         EVP_PKEY_get_bits(pkey), extractable,
                                         usages);
    k->pkey = pkey;
    return k;
}

static BIGNUM *
ns_crypto_bn(const guint8 *b, gsize len)
{
    return (b && len && len <= (gsize)G_MAXINT) ? BN_bin2bn(b, (int)len, NULL)
                                                : NULL;
}

ns_crypto_key *
ns_crypto_import_rsa_jwk(const guint8 *n, gsize n_len, const guint8 *e, gsize e_len,
                         const guint8 *d, gsize d_len, const guint8 *p, gsize p_len,
                         const guint8 *q, gsize q_len, const guint8 *dp, gsize dp_len,
                         const guint8 *dq, gsize dq_len, const guint8 *qi, gsize qi_len,
                         const char *algo, const char *hash, gboolean extractable,
                         guint32 usages, char **err)
{
    gboolean private = d && d_len;
    OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
    BIGNUM *bn_n = ns_crypto_bn(n, n_len), *bn_e = ns_crypto_bn(e, e_len);
    BIGNUM *bn_d = NULL, *bn_p = NULL, *bn_q = NULL, *bn_dp = NULL,
           *bn_dq = NULL, *bn_qi = NULL;
    OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, bn_n);
    OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, bn_e);
    if (private) {
        bn_d = ns_crypto_bn(d, d_len);
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_D, bn_d);
        if (p && q) {
            bn_p = ns_crypto_bn(p, p_len);
            bn_q = ns_crypto_bn(q, q_len);
            OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_FACTOR1, bn_p);
            OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_FACTOR2, bn_q);
            if (dp && dq && qi) {
                bn_dp = ns_crypto_bn(dp, dp_len);
                bn_dq = ns_crypto_bn(dq, dq_len);
                bn_qi = ns_crypto_bn(qi, qi_len);
                OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_EXPONENT1, bn_dp);
                OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_EXPONENT2, bn_dq);
                OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_COEFFICIENT1, bn_qi);
            }
        }
    }
    OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(bld);
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    EVP_PKEY *pkey = NULL;
    int selection = private ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY;
    ns_crypto_key *k = NULL;
    if (ctx && EVP_PKEY_fromdata_init(ctx) > 0 &&
        EVP_PKEY_fromdata(ctx, &pkey, selection, params) > 0) {
        k = ns_crypto_key_new(private ? NS_CK_PRIVATE : NS_CK_PUBLIC, algo, hash,
                              NULL, EVP_PKEY_get_bits(pkey), extractable, usages);
        k->pkey = pkey;
    } else {
        ns_crypto_err(err, "DataError: RSA JWK import");
    }
    if (!k && pkey) EVP_PKEY_free(pkey);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    EVP_PKEY_CTX_free(ctx);
    BN_free(bn_n); BN_free(bn_e); BN_free(bn_d);
    BN_free(bn_p); BN_free(bn_q);
    BN_free(bn_dp); BN_free(bn_dq); BN_free(bn_qi);
    return k;
}

ns_crypto_key *
ns_crypto_import_ec_jwk(const char *curve, const guint8 *x, gsize x_len,
                        const guint8 *y, gsize y_len, const guint8 *d, gsize d_len,
                        const char *algo, gboolean extractable, guint32 usages,
                        char **err)
{
    const char *group = ns_crypto_curve_group(curve);
    int order = ns_crypto_curve_order_bytes(curve);
    if (!group || !order || !x || !y) {
        if (err) *err = g_strdup("DataError: EC JWK");
        return NULL;
    }
    if (x_len > (gsize)order || y_len > (gsize)order) {
        if (err) *err = g_strdup("DataError: EC JWK coordinate too large");
        return NULL;
    }
    gsize plen = 1 + 2 * (gsize)order;
    guint8 *point = g_malloc0(plen);
    point[0] = 0x04;
    memcpy(point + 1 + order - x_len, x, x_len);
    memcpy(point + 1 + 2 * order - y_len, y, y_len);

    OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
    BIGNUM *bn_d = NULL;
    OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, group, 0);
    OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, point, plen);
    gboolean private = d && d_len;
    if (private) {
        bn_d = ns_crypto_bn(d, d_len);
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, bn_d);
    }
    OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(bld);
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    EVP_PKEY *pkey = NULL;
    int selection = private ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY;
    ns_crypto_key *k = NULL;
    if (ctx && EVP_PKEY_fromdata_init(ctx) > 0 &&
        EVP_PKEY_fromdata(ctx, &pkey, selection, params) > 0) {
        k = ns_crypto_key_new(private ? NS_CK_PRIVATE : NS_CK_PUBLIC, algo, NULL,
                              curve, EVP_PKEY_get_bits(pkey), extractable, usages);
        k->pkey = pkey;
    } else {
        ns_crypto_err(err, "DataError: EC JWK import");
    }
    if (!k && pkey) EVP_PKEY_free(pkey);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    EVP_PKEY_CTX_free(ctx);
    BN_free(bn_d);
    g_free(point);
    return k;
}

ns_crypto_key *
ns_crypto_import_okp_jwk(const char *curve, const guint8 *x, gsize x_len,
                         const guint8 *d, gsize d_len, const char *algo,
                         gboolean extractable, guint32 usages, char **err)
{
    const char *okp = ns_crypto_okp_name(curve);
    gboolean private = d && d_len;
    if (!okp || (!private && (!x || !x_len))) {
        if (err) *err = g_strdup("DataError: OKP JWK");
        return NULL;
    }
    EVP_PKEY *pkey = private
        ? EVP_PKEY_new_raw_private_key_ex(NULL, okp, NULL, d, d_len)
        : EVP_PKEY_new_raw_public_key_ex(NULL, okp, NULL, x, x_len);
    if (!pkey) return ns_crypto_err(err, "DataError: OKP JWK import");
    ns_crypto_key *k = ns_crypto_key_new(private ? NS_CK_PRIVATE : NS_CK_PUBLIC,
                                         algo, NULL, NULL,
                                         EVP_PKEY_get_bits(pkey), extractable,
                                         usages);
    k->pkey = pkey;
    return k;
}

gboolean
ns_crypto_export_okp_jwk(const ns_crypto_key *k, guint8 **x, gsize *x_len,
                         guint8 **d, gsize *d_len, char **err)
{
    *x = *d = NULL;
    *x_len = *d_len = 0;
    if (!k->pkey || !ns_crypto_okp_name(k->algo)) {
        if (err) *err = g_strdup("OperationError: OKP export");
        return FALSE;
    }
    size_t n = 0;
    if (EVP_PKEY_get_raw_public_key(k->pkey, NULL, &n) <= 0 || !n) {
        ns_crypto_err(err, "OperationError: OKP export");
        return FALSE;
    }
    *x = g_malloc(n);
    if (EVP_PKEY_get_raw_public_key(k->pkey, *x, &n) <= 0) {
        g_clear_pointer(x, g_free);
        ns_crypto_err(err, "OperationError: OKP export");
        return FALSE;
    }
    *x_len = n;
    if (k->type == NS_CK_PRIVATE) {
        size_t m = 0;
        guint8 *priv = NULL;
        if (EVP_PKEY_get_raw_private_key(k->pkey, NULL, &m) > 0 && m) {
            priv = g_malloc(m);
            if (EVP_PKEY_get_raw_private_key(k->pkey, priv, &m) <= 0)
                g_clear_pointer(&priv, g_free);
        }
        if (!priv) {
            g_clear_pointer(x, g_free);
            *x_len = 0;
            ns_crypto_err(err, "OperationError: OKP export");
            return FALSE;
        }
        *d = priv;
        *d_len = m;
    }
    return TRUE;
}

guint8 *
ns_crypto_export_raw(const char *format, const ns_crypto_key *k, gsize *out_len,
                     char **err)
{
    if (!g_strcmp0(format, "raw") && k->raw) {
        *out_len = k->raw_len;
        return g_memdup2(k->raw, k->raw_len);
    }
    if (!k->pkey) { if (err) *err = g_strdup("NotSupportedError: export"); return NULL; }

    if (!g_strcmp0(format, "raw") && ns_crypto_okp_name(k->algo)) {
        size_t n = 0;
        if (EVP_PKEY_get_raw_public_key(k->pkey, NULL, &n) <= 0 || !n)
            return ns_crypto_err(err, "OperationError: raw export");
        guint8 *out = g_malloc(n);
        if (EVP_PKEY_get_raw_public_key(k->pkey, out, &n) <= 0) {
            g_free(out);
            return ns_crypto_err(err, "OperationError: raw export");
        }
        *out_len = n;
        return out;
    }
    if (!g_strcmp0(format, "raw") &&
        (!g_strcmp0(k->algo, "ECDSA") || !g_strcmp0(k->algo, "ECDH"))) {
        guint8 *buf = NULL;
        gsize n = EVP_PKEY_get1_encoded_public_key(k->pkey, &buf);
        if (!n) return ns_crypto_err(err, "OperationError: raw export");
        guint8 *out = g_memdup2(buf, n);
        OPENSSL_free(buf);
        *out_len = n;
        return out;
    }
    if (!g_strcmp0(format, "spki")) {
        unsigned char *der = NULL;
        int n = i2d_PUBKEY(k->pkey, &der);
        if (n <= 0) return ns_crypto_err(err, "OperationError: spki export");
        guint8 *out = g_memdup2(der, n);
        OPENSSL_free(der);
        *out_len = n;
        return out;
    }
    if (!g_strcmp0(format, "pkcs8")) {
        PKCS8_PRIV_KEY_INFO *p8 = EVP_PKEY2PKCS8(k->pkey);
        if (!p8) return ns_crypto_err(err, "OperationError: pkcs8 export");
        unsigned char *der = NULL;
        int n = i2d_PKCS8_PRIV_KEY_INFO(p8, &der);
        PKCS8_PRIV_KEY_INFO_free(p8);
        if (n <= 0) return ns_crypto_err(err, "OperationError: pkcs8 export");
        guint8 *out = g_memdup2(der, n);
        OPENSSL_free(der);
        *out_len = n;
        return out;
    }
    if (err) *err = g_strdup("NotSupportedError: export format");
    return NULL;
}

static guint8 *
ns_crypto_bn_export(const EVP_PKEY *pkey, const char *param, gsize *out_len)
{
    BIGNUM *bn = NULL;
    if (EVP_PKEY_get_bn_param(pkey, param, &bn) <= 0 || !bn) return NULL;
    int n = BN_num_bytes(bn);
    guint8 *out = g_malloc(n ? n : 1);
    BN_bn2bin(bn, out);
    *out_len = n;
    BN_clear_free(bn);
    return out;
}

gboolean
ns_crypto_export_rsa_jwk(const ns_crypto_key *k, guint8 **n, gsize *n_len,
                         guint8 **e, gsize *e_len, guint8 **d, gsize *d_len,
                         guint8 **p, gsize *p_len, guint8 **q, gsize *q_len,
                         guint8 **dp, gsize *dp_len, guint8 **dq, gsize *dq_len,
                         guint8 **qi, gsize *qi_len, char **err)
{
    if (!k->pkey) { if (err) *err = g_strdup("export"); return FALSE; }
    *n = ns_crypto_bn_export(k->pkey, OSSL_PKEY_PARAM_RSA_N, n_len);
    *e = ns_crypto_bn_export(k->pkey, OSSL_PKEY_PARAM_RSA_E, e_len);
    *d = *p = *q = *dp = *dq = *qi = NULL;
    *d_len = *p_len = *q_len = *dp_len = *dq_len = *qi_len = 0;
    if (k->type == NS_CK_PRIVATE) {
        *d = ns_crypto_bn_export(k->pkey, OSSL_PKEY_PARAM_RSA_D, d_len);
        *p = ns_crypto_bn_export(k->pkey, OSSL_PKEY_PARAM_RSA_FACTOR1, p_len);
        *q = ns_crypto_bn_export(k->pkey, OSSL_PKEY_PARAM_RSA_FACTOR2, q_len);
        *dp = ns_crypto_bn_export(k->pkey, OSSL_PKEY_PARAM_RSA_EXPONENT1, dp_len);
        *dq = ns_crypto_bn_export(k->pkey, OSSL_PKEY_PARAM_RSA_EXPONENT2, dq_len);
        *qi = ns_crypto_bn_export(k->pkey, OSSL_PKEY_PARAM_RSA_COEFFICIENT1, qi_len);
    }
    if (*n && *e)
        return TRUE;
    g_clear_pointer(n, g_free);
    g_clear_pointer(e, g_free);
    g_clear_pointer(d, g_free);
    g_clear_pointer(p, g_free);
    g_clear_pointer(q, g_free);
    g_clear_pointer(dp, g_free);
    g_clear_pointer(dq, g_free);
    g_clear_pointer(qi, g_free);
    if (err) *err = g_strdup("export");
    return FALSE;
}

gboolean
ns_crypto_export_ec_jwk(const ns_crypto_key *k, guint8 **x, gsize *x_len,
                        guint8 **y, gsize *y_len, guint8 **d, gsize *d_len,
                        char **err)
{
    if (!k->pkey) { if (err) *err = g_strdup("export"); return FALSE; }
    int order = ns_crypto_curve_order_bytes(k->curve);
    if (order <= 0) { if (err) *err = g_strdup("OperationError: EC export"); return FALSE; }
    guint8 *enc = NULL;
    gsize enc_len = EVP_PKEY_get1_encoded_public_key(k->pkey, &enc);
    *x = *y = *d = NULL;
    *x_len = *y_len = *d_len = 0;
    if (enc && enc_len == 1 + 2 * (gsize)order && enc[0] == 0x04) {
        *x = g_memdup2(enc + 1, order);
        *y = g_memdup2(enc + 1 + order, order);
        *x_len = *y_len = order;
    }
    if (enc) OPENSSL_free(enc);
    if (!*x) { if (err) *err = g_strdup("OperationError: EC export"); return FALSE; }
    if (k->type == NS_CK_PRIVATE) {
        guint8 *raw = ns_crypto_bn_export(k->pkey, OSSL_PKEY_PARAM_PRIV_KEY, d_len);
        if (!raw) {
            g_free(*x); g_free(*y);
            *x = *y = NULL; *x_len = *y_len = 0;
            if (err) *err = g_strdup("OperationError: EC export");
            return FALSE;
        }
        if (*d_len < (gsize)order) {
            guint8 *pad = g_malloc0(order);
            memcpy(pad + order - *d_len, raw, *d_len);
            g_free(raw);
            raw = pad;
            *d_len = order;
        }
        *d = raw;
    }
    return TRUE;
}

static guint8 *
ns_crypto_hmac(const ns_crypto_key *k, const guint8 *data, gsize len,
               gsize *out_len, char **err)
{
    const char *md = ns_crypto_md_name(k->hash);
    if (!md) { if (err) *err = g_strdup("NotSupportedError: hash"); return NULL; }
    EVP_MAC *mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
    EVP_MAC_CTX *ctx = mac ? EVP_MAC_CTX_new(mac) : NULL;
    OSSL_PARAM params[2] = {
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, (char *)md, 0),
        OSSL_PARAM_construct_end(),
    };
    guint8 *out = NULL;
    size_t n = 0;
    if (ctx && EVP_MAC_init(ctx, k->raw, k->raw_len, params) &&
        EVP_MAC_update(ctx, data, len) &&
        EVP_MAC_final(ctx, NULL, &n, 0)) {
        out = g_malloc(n ? n : 1);
        if (EVP_MAC_final(ctx, out, &n, n)) {
            *out_len = n;
        } else {
            g_free(out);
            out = NULL;
            ns_crypto_err(err, "OperationError: HMAC");
        }
    } else {
        ns_crypto_err(err, "OperationError: HMAC");
    }
    EVP_MAC_CTX_free(ctx);
    EVP_MAC_free(mac);
    return out;
}

static guint8 *
ns_crypto_ecdsa_der_to_raw(const guint8 *der, gsize der_len, int order,
                           gsize *out_len)
{
    const unsigned char *p = der;
    ECDSA_SIG *sig = d2i_ECDSA_SIG(NULL, &p, (long)der_len);
    if (!sig) return NULL;
    const BIGNUM *r, *s;
    ECDSA_SIG_get0(sig, &r, &s);
    if (!r || !s) { ECDSA_SIG_free(sig); return NULL; }
    guint8 *out = g_malloc0((gsize)order * 2);
    if (BN_bn2binpad(r, out, order) < 0 ||
        BN_bn2binpad(s, out + order, order) < 0) {
        g_free(out);
        ECDSA_SIG_free(sig);
        return NULL;
    }
    ECDSA_SIG_free(sig);
    *out_len = (gsize)order * 2;
    return out;
}

static guint8 *
ns_crypto_ecdsa_raw_to_der(const guint8 *raw, gsize raw_len, int order,
                           gsize *out_len)
{
    if (raw_len != (gsize)order * 2) return NULL;
    ECDSA_SIG *sig = ECDSA_SIG_new();
    BIGNUM *r = BN_bin2bn(raw, order, NULL);
    BIGNUM *s = BN_bin2bn(raw + order, order, NULL);
    if (!sig || !r || !s) {
        BN_free(r);
        BN_free(s);
        ECDSA_SIG_free(sig);
        return NULL;
    }
    ECDSA_SIG_set0(sig, r, s);
    unsigned char *der = NULL;
    int n = i2d_ECDSA_SIG(sig, &der);
    ECDSA_SIG_free(sig);
    if (n <= 0) return NULL;
    guint8 *out = g_memdup2(der, n);
    OPENSSL_free(der);
    *out_len = n;
    return out;
}

static int
ns_crypto_pkey_sign_setup(EVP_PKEY_CTX *pctx, const ns_crypto_key *k,
                          const ns_crypto_params *p)
{
    if (!g_strcmp0(k->algo, "RSA-PSS")) {
        if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) <= 0)
            return 0;
        int salt = p->pss_salt_len;
        if (salt < 0) {
            const EVP_MD *pss_md = ns_crypto_md(p->sign_hash ? p->sign_hash
                                                             : k->hash);
            if (!pss_md) return 0;
            salt = EVP_MD_get_size(pss_md);
        }
        if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, salt) <= 0) return 0;
    } else if (!g_strcmp0(k->algo, "RSASSA-PKCS1-v1_5")) {
        if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING) <= 0) return 0;
    }
    return 1;
}

guint8 *
ns_crypto_sign(const ns_crypto_key *k, const ns_crypto_params *p, const guint8 *data,
               gsize len, gsize *out_len, char **err)
{
    if (!g_strcmp0(k->algo, "HMAC"))
        return ns_crypto_hmac(k, data, len, out_len, err);

    if (!g_strcmp0(k->algo, "Ed25519")) {
        if (!k->pkey) { if (err) *err = g_strdup("NotSupportedError: sign"); return NULL; }
        EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
        size_t n = 0;
        if (!mdctx ||
            EVP_DigestSignInit(mdctx, NULL, NULL, NULL, k->pkey) <= 0 ||
            EVP_DigestSign(mdctx, NULL, &n, data, len) <= 0) {
            EVP_MD_CTX_free(mdctx);
            return ns_crypto_err(err, "OperationError: sign");
        }
        guint8 *out = g_malloc(n ? n : 1);
        if (EVP_DigestSign(mdctx, out, &n, data, len) <= 0) {
            g_free(out);
            EVP_MD_CTX_free(mdctx);
            return ns_crypto_err(err, "OperationError: sign");
        }
        EVP_MD_CTX_free(mdctx);
        *out_len = n;
        return out;
    }

    const EVP_MD *md = ns_crypto_md(p->sign_hash ? p->sign_hash : k->hash);
    if (!md || !k->pkey) { if (err) *err = g_strdup("NotSupportedError: sign"); return NULL; }

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx) return ns_crypto_err(err, "OperationError: sign");
    EVP_PKEY_CTX *pctx = NULL;
    guint8 *out = NULL;
    size_t n = 0;
    if (EVP_DigestSignInit(mdctx, &pctx, md, NULL, k->pkey) <= 0 ||
        !ns_crypto_pkey_sign_setup(pctx, k, p) ||
        EVP_DigestSign(mdctx, NULL, &n, data, len) <= 0) {
        EVP_MD_CTX_free(mdctx);
        return ns_crypto_err(err, "OperationError: sign");
    }
    guint8 *der = g_malloc(n ? n : 1);
    if (EVP_DigestSign(mdctx, der, &n, data, len) <= 0) {
        g_free(der);
        EVP_MD_CTX_free(mdctx);
        return ns_crypto_err(err, "OperationError: sign");
    }
    EVP_MD_CTX_free(mdctx);

    if (!g_strcmp0(k->algo, "ECDSA")) {
        int order = ns_crypto_curve_order_bytes(k->curve);
        if (order <= 0) {
            g_free(der);
            return ns_crypto_err(err, "OperationError: unsupported ECDSA curve");
        }
        out = ns_crypto_ecdsa_der_to_raw(der, n, order, out_len);
        g_free(der);
        if (!out) return ns_crypto_err(err, "OperationError: ECDSA encode");
        return out;
    }
    *out_len = n;
    return der;
}

int
ns_crypto_verify(const ns_crypto_key *k, const ns_crypto_params *p, const guint8 *sig,
                 gsize sig_len, const guint8 *data, gsize len, char **err)
{
    if (!g_strcmp0(k->algo, "HMAC")) {
        gsize mlen = 0;
        guint8 *mac = ns_crypto_hmac(k, data, len, &mlen, err);
        if (!mac) return -1;
        int ok = (mlen == sig_len) && CRYPTO_memcmp(mac, sig, mlen) == 0;
        OPENSSL_cleanse(mac, mlen);
        g_free(mac);
        return ok ? 1 : 0;
    }

    if (!g_strcmp0(k->algo, "Ed25519")) {
        if (!k->pkey) { if (err) *err = g_strdup("NotSupportedError: verify"); return -1; }
        EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
        int rc = -1;
        if (mdctx && EVP_DigestVerifyInit(mdctx, NULL, NULL, NULL, k->pkey) > 0) {
            int v = EVP_DigestVerify(mdctx, sig, sig_len, data, len);
            rc = v == 1 ? 1 : 0;
            if (v < 0) { ERR_clear_error(); rc = 0; }
        } else {
            ns_crypto_err(err, "OperationError: verify");
        }
        EVP_MD_CTX_free(mdctx);
        return rc;
    }

    const EVP_MD *md = ns_crypto_md(p->sign_hash ? p->sign_hash : k->hash);
    if (!md || !k->pkey) { if (err) *err = g_strdup("NotSupportedError: verify"); return -1; }

    guint8 *der = NULL;
    gsize der_len = 0;
    if (!g_strcmp0(k->algo, "ECDSA")) {
        int order = ns_crypto_curve_order_bytes(k->curve);
        if (order <= 0) { if (err) *err = g_strdup("OperationError: unsupported ECDSA curve"); return -1; }
        der = ns_crypto_ecdsa_raw_to_der(sig, sig_len, order, &der_len);
        if (!der) { if (err) *err = g_strdup("OperationError: ECDSA decode"); return -1; }
        sig = der;
        sig_len = der_len;
    }

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        g_free(der);
        if (err) *err = g_strdup("OperationError: verify");
        return -1;
    }
    EVP_PKEY_CTX *pctx = NULL;
    int rc = -1;
    if (EVP_DigestVerifyInit(mdctx, &pctx, md, NULL, k->pkey) > 0 &&
        ns_crypto_pkey_sign_setup(pctx, k, p)) {
        int v = EVP_DigestVerify(mdctx, sig, sig_len, data, len);
        rc = v == 1 ? 1 : 0;
        if (v < 0) { ERR_clear_error(); rc = 0; }
    } else {
        ns_crypto_err(err, "OperationError: verify");
    }
    EVP_MD_CTX_free(mdctx);
    g_free(der);
    return rc;
}

static gboolean
ns_crypto_is_aes(const char *algo)
{
    return algo && !g_ascii_strncasecmp(algo, "AES-", 4);
}

static const EVP_CIPHER *
ns_crypto_aes_cipher(const char *algo, int bits)
{
    if (bits != 128 && bits != 192 && bits != 256)
        return NULL;
    if (!g_strcmp0(algo, "AES-GCM"))
        return bits == 128 ? EVP_aes_128_gcm()
             : bits == 192 ? EVP_aes_192_gcm() : EVP_aes_256_gcm();
    if (!g_strcmp0(algo, "AES-CBC"))
        return bits == 128 ? EVP_aes_128_cbc()
             : bits == 192 ? EVP_aes_192_cbc() : EVP_aes_256_cbc();
    if (!g_strcmp0(algo, "AES-CTR"))
        return bits == 128 ? EVP_aes_128_ctr()
             : bits == 192 ? EVP_aes_192_ctr() : EVP_aes_256_ctr();
    if (!g_strcmp0(algo, "AES-KW"))
        return bits == 128 ? EVP_aes_128_wrap()
             : bits == 192 ? EVP_aes_192_wrap() : EVP_aes_256_wrap();
    return NULL;
}

static guint8 *
ns_crypto_aes(const ns_crypto_key *k, const ns_crypto_params *p, const guint8 *data,
              gsize len, gboolean enc, gsize *out_len, char **err)
{
    const EVP_CIPHER *cipher = ns_crypto_aes_cipher(k->algo, k->bits);
    if (!cipher || !k->raw) { if (err) *err = g_strdup("NotSupportedError: AES"); return NULL; }
    gboolean gcm = !g_strcmp0(k->algo, "AES-GCM");
    gboolean kw = !g_strcmp0(k->algo, "AES-KW");
    if (gcm) {
        if (!p->iv || p->iv_len == 0) {
            if (err) *err = g_strdup("OperationError: invalid AES-GCM IV");
            return NULL;
        }
    } else if (kw) {
        if (len % 8 != 0 || len < (gsize)(enc ? 16 : 24)) {
            if (err) *err = g_strdup("OperationError: invalid AES-KW length");
            return NULL;
        }
    } else {
        if (!p->iv || p->iv_len != 16) {
            if (err) *err = g_strdup("OperationError: invalid AES IV length");
            return NULL;
        }
    }
    int tag_bits = gcm ? (p->tag_bits > 0 ? p->tag_bits : 128) : 0;
    int tag_len = tag_bits / 8;
    if (gcm) {
        switch (tag_bits) {
        case 32: case 64: case 96: case 104: case 112: case 120: case 128:
            break;
        default:
            if (err) *err = g_strdup("OperationError: invalid AES-GCM tag length");
            return NULL;
        }
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int outl = 0, finl = 0;
    guint8 *out = NULL;
    gsize produced = 0;
    const guint8 *ct = data;
    gsize ct_len = len;
    guint8 tagbuf[16];

    if (!ctx || len > (gsize)G_MAXINT - 64 || p->aad_len > (gsize)G_MAXINT ||
        p->iv_len > (gsize)G_MAXINT)
        goto fail;
    out = g_malloc(len + 32 + (gsize)tag_len);

    if (kw) EVP_CIPHER_CTX_set_flags(ctx, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
    if (!EVP_CipherInit_ex(ctx, cipher, NULL, NULL, NULL, enc)) goto fail;
    if (gcm) {
        if (!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)p->iv_len, NULL))
            goto fail;
    }
    if (!EVP_CipherInit_ex(ctx, NULL, NULL, k->raw, kw ? NULL : p->iv, enc))
        goto fail;

    if (gcm && !enc) {
        if (ct_len < (gsize)tag_len) goto fail;
        ct_len -= tag_len;
        memcpy(tagbuf, data + ct_len, tag_len);
    }
    if (gcm && p->aad_len) {
        if (!EVP_CipherUpdate(ctx, NULL, &outl, p->aad, (int)p->aad_len)) goto fail;
    }
    if (ct_len) {
        if (!EVP_CipherUpdate(ctx, out, &outl, ct, (int)ct_len)) goto fail;
        produced = outl;
    }
    if (gcm && !enc) {
        if (!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag_len, tagbuf))
            goto fail;
    }
    if (!EVP_CipherFinal_ex(ctx, out + produced, &finl)) goto fail;
    produced += finl;
    if (gcm && enc) {
        if (!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, tag_len, out + produced))
            goto fail;
        produced += tag_len;
    }
    EVP_CIPHER_CTX_free(ctx);
    *out_len = produced;
    return out;
fail:
    EVP_CIPHER_CTX_free(ctx);
    g_free(out);
    return ns_crypto_err(err, enc ? "OperationError: encrypt"
                                  : "OperationError: decrypt");
}

static guint8 *
ns_crypto_rsa_oaep(const ns_crypto_key *k, const ns_crypto_params *p,
                   const guint8 *data, gsize len, gboolean enc, gsize *out_len,
                   char **err)
{
    const EVP_MD *md = ns_crypto_md(k->hash);
    if (!md || !k->pkey) { if (err) *err = g_strdup("NotSupportedError: RSA-OAEP"); return NULL; }
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_pkey(NULL, k->pkey, NULL);
    guint8 *out = NULL;
    size_t n = 0;
    int ok = ctx && (enc ? EVP_PKEY_encrypt_init(ctx) : EVP_PKEY_decrypt_init(ctx)) > 0 &&
             EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) > 0 &&
             EVP_PKEY_CTX_set_rsa_oaep_md(ctx, md) > 0 &&
             EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, md) > 0;
    if (ok && p->label && p->label_len) {
        guint8 *lbl = p->label_len <= (gsize)G_MAXINT
                          ? OPENSSL_memdup(p->label, p->label_len)
                          : NULL;
        if (!lbl ||
            EVP_PKEY_CTX_set0_rsa_oaep_label(ctx, lbl, (int)p->label_len) <= 0) {
            OPENSSL_free(lbl);
            ok = 0;
        }
    }
    if (ok) {
        int rc = enc ? EVP_PKEY_encrypt(ctx, NULL, &n, data, len)
                     : EVP_PKEY_decrypt(ctx, NULL, &n, data, len);
        if (rc > 0) {
            out = g_malloc(n ? n : 1);
            rc = enc ? EVP_PKEY_encrypt(ctx, out, &n, data, len)
                     : EVP_PKEY_decrypt(ctx, out, &n, data, len);
            if (rc <= 0) { g_free(out); out = NULL; }
        }
    }
    EVP_PKEY_CTX_free(ctx);
    if (!out) return ns_crypto_err(err, enc ? "OperationError: encrypt"
                                            : "OperationError: decrypt");
    *out_len = n;
    return out;
}

guint8 *
ns_crypto_encrypt(const ns_crypto_key *k, const ns_crypto_params *p,
                  const guint8 *data, gsize len, gsize *out_len, char **err)
{
    if (ns_crypto_is_aes(k->algo))
        return ns_crypto_aes(k, p, data, len, TRUE, out_len, err);
    if (!g_strcmp0(k->algo, "RSA-OAEP"))
        return ns_crypto_rsa_oaep(k, p, data, len, TRUE, out_len, err);
    if (err) *err = g_strdup("NotSupportedError: encrypt");
    return NULL;
}

guint8 *
ns_crypto_decrypt(const ns_crypto_key *k, const ns_crypto_params *p,
                  const guint8 *data, gsize len, gsize *out_len, char **err)
{
    if (ns_crypto_is_aes(k->algo))
        return ns_crypto_aes(k, p, data, len, FALSE, out_len, err);
    if (!g_strcmp0(k->algo, "RSA-OAEP"))
        return ns_crypto_rsa_oaep(k, p, data, len, FALSE, out_len, err);
    if (err) *err = g_strdup("NotSupportedError: decrypt");
    return NULL;
}

static guint8 *
ns_crypto_ecdh(const ns_crypto_key *k, const ns_crypto_params *p, int length_bits,
               gsize *out_len, char **err)
{
    if (!k->pkey || !p->peer || !p->peer->pkey) {
        if (err) *err = g_strdup("InvalidAccessError: ECDH");
        return NULL;
    }
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_pkey(NULL, k->pkey, NULL);
    guint8 *out = NULL;
    size_t n = 0;
    if (ctx && EVP_PKEY_derive_init(ctx) > 0 &&
        EVP_PKEY_derive_set_peer(ctx, p->peer->pkey) > 0 &&
        EVP_PKEY_derive(ctx, NULL, &n) > 0) {
        size_t cap = n ? n : 1;
        out = g_malloc(cap);
        if (EVP_PKEY_derive(ctx, out, &n) <= 0) {
            OPENSSL_cleanse(out, cap);
            g_free(out);
            out = NULL;
        }
    }
    EVP_PKEY_CTX_free(ctx);
    if (!out) return ns_crypto_err(err, "OperationError: ECDH");
    if (length_bits > 0) {
        gsize want = (gsize)length_bits / 8;
        if (want > n) {
            OPENSSL_cleanse(out, n);
            g_free(out);
            return ns_crypto_err(err, "OperationError: ECDH length");
        }
        n = want;
    }
    *out_len = n;
    return out;
}

static guint8 *
ns_crypto_pbkdf2(const ns_crypto_key *k, const ns_crypto_params *p, int length_bits,
                 gsize *out_len, char **err)
{
    const EVP_MD *md = ns_crypto_md(p->kdf_hash);
    if (!md || length_bits <= 0) { if (err) *err = g_strdup("OperationError: PBKDF2"); return NULL; }
    if (p->iterations <= 0) {
        if (err) *err = g_strdup("OperationError: PBKDF2 iterations must be positive");
        return NULL;
    }
    if (k->raw_len > (gsize)G_MAXINT || p->salt_len > (gsize)G_MAXINT) {
        if (err) *err = g_strdup("OperationError: PBKDF2 input too large");
        return NULL;
    }
    gsize n = (gsize)length_bits / 8;
    guint8 *out = g_malloc(n ? n : 1);
    if (PKCS5_PBKDF2_HMAC((const char *)k->raw, (int)k->raw_len, p->salt,
                          (int)p->salt_len, p->iterations, md, (int)n, out) != 1) {
        OPENSSL_cleanse(out, n ? n : 1);
        g_free(out);
        return ns_crypto_err(err, "OperationError: PBKDF2");
    }
    *out_len = n;
    return out;
}

static guint8 *
ns_crypto_hkdf(const ns_crypto_key *k, const ns_crypto_params *p, int length_bits,
               gsize *out_len, char **err)
{
    const char *md = ns_crypto_md_name(p->kdf_hash);
    if (!md || length_bits <= 0) { if (err) *err = g_strdup("OperationError: HKDF"); return NULL; }
    gsize n = (gsize)length_bits / 8;
    guint8 *out = g_malloc(n ? n : 1);
    EVP_KDF *kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
    EVP_KDF_CTX *ctx = kdf ? EVP_KDF_CTX_new(kdf) : NULL;
    OSSL_PARAM params[5];
    int i = 0;
    params[i++] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
                                                   (char *)md, 0);
    params[i++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,
                                                    k->raw, k->raw_len);
    params[i++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                                    (void *)(p->salt ? p->salt : (const guint8 *)""),
                                                    p->salt_len);
    params[i++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO,
                                                    (void *)(p->info ? p->info : (const guint8 *)""),
                                                    p->info_len);
    params[i] = OSSL_PARAM_construct_end();
    int ok = ctx && EVP_KDF_derive(ctx, out, n, params) > 0;
    EVP_KDF_CTX_free(ctx);
    EVP_KDF_free(kdf);
    if (!ok) {
        OPENSSL_cleanse(out, n ? n : 1);
        g_free(out);
        return ns_crypto_err(err, "OperationError: HKDF");
    }
    *out_len = n;
    return out;
}

guint8 *
ns_crypto_derive_bits(const ns_crypto_key *k, const ns_crypto_params *p,
                      int length_bits, gsize *out_len, char **err)
{
    if (length_bits > (1 << 20)) {
        if (err) *err = g_strdup("OperationError: deriveBits length too large");
        return NULL;
    }
    if (!g_strcmp0(k->algo, "ECDH") || !g_strcmp0(k->algo, "X25519"))
        return ns_crypto_ecdh(k, p, length_bits, out_len, err);
    if (!g_strcmp0(k->algo, "PBKDF2"))
        return ns_crypto_pbkdf2(k, p, length_bits, out_len, err);
    if (!g_strcmp0(k->algo, "HKDF"))
        return ns_crypto_hkdf(k, p, length_bits, out_len, err);
    if (err) *err = g_strdup("NotSupportedError: deriveBits");
    return NULL;
}
