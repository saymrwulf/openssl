/*
 * Copyright 2025 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <string.h>
#include <openssl/byteorder.h>
#include <openssl/err.h>
#include <openssl/proverr.h>
#include <openssl/x509.h>
#include <openssl/core_names.h>
#include "internal/encoder.h"
#include "ml_dsa_codecs.h"

/*-
 * Tables describing supported ASN.1 input/output formats.
 * For each parameter set we support a few PKCS#8 input formats, three
 * corresponding to the "either or both" variants of:
 *
 *  ML-DSA-PrivateKey ::= CHOICE {
 *    seed [0] IMPLICIT OCTET STRING SIZE (32),
 *    expandedKey OCTET STRING SIZE (2560 | 4032 | 4896)
 *    both SEQUENCE {
 *      seed OCTET STRING SIZE (32),
 *      expandedKey OCTET STRING SIZE (2560 | 4032 | 4896) } }
 *
 * one more for a historical OQS encoding:
 *
 * - OQS private + public key: OCTET STRING
 *   (The public key is ignored, just as with PKCS#8 v2.)
 *
 * and two more that are the minimal IETF non-ASN.1 seed encoding:
 *
 * - Bare seed (just the 32 bytes)
 * - Bare priv (just the key bytes)
 *
 * A length of zero means that particular field is absent.
 *
 * The p8_shift is 0 when the top-level tag+length occupy four bytes, 2 when
 * they occupy two by†es, and 4 when no tag is used at all.
 *
 * On output the PKCS8 info table order is important:
 * - When we have a seed we'll use the first entry with a non-zero seed offset.
 * - Otherwise, the first entry with a zero seed offset.
 *
 * As written, when possible, we prefer to output both the seed and private
 * key, otherwise, just the private key ([1] IMPLICIT OCTET STRING form).
 *
 * The various lengths in the PKCS#8 tag/len fields could have been left
 * zeroed, and filled in on the fly from the algorithm parameters, but that
 * makes the code more complex, so a choice was made to embed them directly
 * into the tables.  Had they been zeroed, one table could cover all three
 * ML-DSA parameter sets.
 */
#define NUM_PKCS8_FORMATS   6

/*-
 * ML-DSA-44:
 * Public key bytes:  1312 (0x0520)
 * Private key bytes: 2560 (0x0a00)
 */
static const ML_DSA_SPKI_FMT ml_dsa_44_spkifmt = {
    { 0x30, 0x82, 0x05, 0x32, 0x30, 0x0b, 0x06, 0x09, 0x60, 0x86, 0x48,
      0x01, 0x65, 0x03, 0x04, 0x03, 0x11, 0x03, 0x82, 0x05, 0x21, 0x00, }
};
static const ML_DSA_PKCS8_FMT ml_dsa_44_p8fmt[NUM_PKCS8_FORMATS] = {
    { "seed-priv",  0x0a2a, 0, 0x30820a26, 0x0420, 6, 0x20, 0x04820a00, 0x2a, 0x0a00, 0,      0,     },
    { "priv-only",  0x0a04, 0, 0x04820a00, 0,      0, 0,    0,          0x04, 0x0a00, 0,      0,     },
    { "oqskeypair", 0x0f24, 0, 0x04820f20, 0,      0, 0,    0,          0x04, 0x0a00, 0x0a04, 0x0520 },
    { "seed-only",  0x0022, 2, 0x8020,     0,      2, 0x20, 0,          0,    0,      0,      0,     },
    { "bare-priv",  0x0a00, 4, 0,          0,      0, 0,    0,          0,    0x0a00, 0,      0,     },
    { "bare-seed",  0x0020, 4, 0,          0,      0, 0x20, 0,          0,    0,      0,      0,     },
};

/*
 * ML-DSA-65:
 * Public key bytes:  1952 (0x07a0)
 * Private key bytes: 4032 (0x0fc0)
 */
static const ML_DSA_SPKI_FMT ml_dsa_65_spkifmt = {
    { 0x30, 0x82, 0x07, 0xb2, 0x30, 0x0b, 0x06, 0x09, 0x60, 0x86, 0x48,
      0x01, 0x65, 0x03, 0x04, 0x03, 0x12, 0x03, 0x82, 0x07, 0xa1, 0x00, }
};
static const ML_DSA_PKCS8_FMT ml_dsa_65_p8fmt[NUM_PKCS8_FORMATS] = {
    { "seed-priv",  0x0fea, 0, 0x30820fe6, 0x0420, 6, 0x20, 0x04820fc0, 0x2a, 0x0fc0, 0,      0,     },
    { "priv-only",  0x0fc4, 0, 0x04820fc0, 0,      0, 0,    0,          0x04, 0x0fc0, 0,      0,     },
    { "oqskeypair", 0x1764, 0, 0x04821760, 0,      0, 0,    0,          0x04, 0x0fc0, 0x0fc4, 0x07a0 },
    { "seed-only",  0x0022, 2, 0x8020,     0,      2, 0x20, 0,          0,    0,      0,      0,     },
    { "bare-priv",  0x0fc0, 4, 0,          0,      0, 0,    0,          0,    0x0fc0, 0,      0,     },
    { "bare-seed",  0x0020, 4, 0,          0,      0, 0x20, 0,          0,    0,      0,      0,     },
};

/*-
 * ML-DSA-87:
 * Public key bytes:  2592 (0x0a20)
 * Private key bytes: 4896 (0x1320)
 */
static const ML_DSA_SPKI_FMT ml_dsa_87_spkifmt = {
    { 0x30, 0x82, 0x0a, 0x32, 0x30, 0x0b, 0x06, 0x09, 0x60, 0x86, 0x48,
      0x01, 0x65, 0x03, 0x04, 0x03, 0x13, 0x03, 0x82, 0x0a, 0x21, 0x00, }
};
static const ML_DSA_PKCS8_FMT ml_dsa_87_p8fmt[NUM_PKCS8_FORMATS] = {
    { "seed-priv",  0x134a, 0, 0x30821346, 0x0420, 6, 0x20, 0x04821320, 0x2a, 0x1320, 0,      0,     },
    { "priv-only",  0x1324, 0, 0x04821320, 0,      0, 0,    0,          0x04, 0x1320, 0,      0,     },
    { "oqskeypair", 0x1d44, 0, 0x04821d40, 0,      0, 0,    0,          0x04, 0x1320, 0x1324, 0x0a20 },
    { "seed-only",  0x0022, 2, 0x8020,     0,      2, 0x20, 0,          0,    0,      0,      0,     },
    { "bare-priv",  0x1320, 4, 0,          0,      0, 0,    0,          0,    0x1320, 0,      0,     },
    { "bare-seed",  0x0020, 4, 0,          0,      0, 0x20, 0,          0,    0,      0,      0,     },
};

/* Indices of slots in the codec table below */
#define ML_DSA_44_CODEC    0
#define ML_DSA_65_CODEC    1
#define ML_DSA_87_CODEC    2

/*
 * Per-variant fixed parameters
 */
static const ML_DSA_CODEC codecs[3] = {
    { &ml_dsa_44_spkifmt, ml_dsa_44_p8fmt },
    { &ml_dsa_65_spkifmt, ml_dsa_65_p8fmt },
    { &ml_dsa_87_spkifmt, ml_dsa_87_p8fmt }
};

/* Retrieve the parameters of one of the ML-DSA variants */
static const ML_DSA_CODEC *ml_dsa_get_codec(int evp_type)
{
    switch (evp_type) {
    case EVP_PKEY_ML_DSA_44:
        return &codecs[ML_DSA_44_CODEC];
    case EVP_PKEY_ML_DSA_65:
        return &codecs[ML_DSA_65_CODEC];
    case EVP_PKEY_ML_DSA_87:
        return &codecs[ML_DSA_87_CODEC];
    }
    return NULL;
}

static int pref_cmp(const void *va, const void *vb)
{
    const ML_DSA_PKCS8_FMT_PREF *a = va;
    const ML_DSA_PKCS8_FMT_PREF *b = vb;

    /*
     * Zeros sort last, otherwise the sort is in increasing order.
     *
     * The preferences are small enough to ensure the comparison is transitive
     * as required by qsort(3).  When overflow or underflow is possible, the
     * correct transitive comparison would be: (b < a) - (a < b).
     */
    if (a->pref > 0 && b->pref > 0)
        return a->pref - b->pref;
    /* A preference of 0 is "larger" than (sorts after) any nonzero value. */
    return b->pref - a->pref;
}

static
ML_DSA_PKCS8_FMT_PREF *vp8_order(const char *algorithm_name,
                                 const ML_DSA_PKCS8_FMT *p8fmt,
                                 const char *direction, const char *formats)
{
    ML_DSA_PKCS8_FMT_PREF *ret;
    int i, count = 0;
    const char *fmt = formats, *end;
    const char *sep = "\t ,";

    /* Reserve an extra terminal slot with fmt == NULL */
    if ((ret = OPENSSL_zalloc((NUM_PKCS8_FORMATS + 1) * sizeof(*ret))) == NULL)
        return NULL;

    /* Entries that match a format will get a non-zero preference. */
    for (i = 0; i < NUM_PKCS8_FORMATS; ++i) {
        ret[i].fmt = &p8fmt[i];
        ret[i].pref = 0;
    }

    /* Default to compile-time table order when none specified. */
    if (formats == NULL)
        return ret;

    /*
     * Formats are case-insensitive, separated by spaces, tabs or commas.
     * Duplicate formats are allowed, the first occurence determines the order.
     */
    do {
        if (*(fmt += strspn(fmt, sep)) == '\0')
            break;
        end = fmt + strcspn(fmt, sep);
        for (i = 0; i < NUM_PKCS8_FORMATS; ++i) {
            /* Skip slots already selected or with a different name. */
            if (ret[i].pref > 0
                || OPENSSL_strncasecmp(ret[i].fmt->p8_name,
                                       fmt, (end - fmt)) != 0)
                continue;
            /* First time match */
            ret[i].pref = ++count;
            break;
        }
        fmt = end;
    } while (count < NUM_PKCS8_FORMATS);

    /* No formats matched, raise an error */
    if (count == 0) {
        OPENSSL_free(ret);
        ERR_raise_data(ERR_LIB_PROV, PROV_R_ML_DSA_NO_FORMAT,
                       "no %s private key %s formats are enabled",
                       algorithm_name, direction);
        return NULL;
    }
    /* Sort by preference, with 0's last */
    qsort(ret, NUM_PKCS8_FORMATS, sizeof(*ret), pref_cmp);
    /* Terminate the list at first unselected entry, perhaps reserved slot. */
    ret[count].fmt = NULL;
    return ret;
}

ML_DSA_KEY *
ossl_ml_dsa_d2i_PUBKEY(const uint8_t *pk, int pk_len, int evp_type,
                       PROV_CTX *provctx, const char *propq)
{
    OSSL_LIB_CTX *libctx = PROV_LIBCTX_OF(provctx);
    const ML_DSA_CODEC *codec;
    const ML_DSA_PARAMS *params;
    ML_DSA_KEY *ret;

    if ((params = ossl_ml_dsa_params_get(evp_type)) == NULL
        || (codec = ml_dsa_get_codec(evp_type)) == NULL)
        return NULL;
    if (pk_len != ML_DSA_SPKI_OVERHEAD + (ossl_ssize_t) params->pk_len
        || memcmp(pk, codec->spkifmt->asn1_prefix, ML_DSA_SPKI_OVERHEAD) != 0)
        return NULL;
    pk_len -= ML_DSA_SPKI_OVERHEAD;
    pk += ML_DSA_SPKI_OVERHEAD;

    if ((ret = ossl_ml_dsa_key_new(libctx, propq, evp_type)) == NULL)
        return NULL;

    if (!ossl_ml_dsa_pk_decode(ret, pk, (size_t) pk_len)) {
        ERR_raise_data(ERR_LIB_PROV, PROV_R_BAD_ENCODING,
                       "errror parsing %s public key from input SPKI",
                       params->alg);
        ossl_ml_dsa_key_free(ret);
        return NULL;
    }

    return ret;
}

ML_DSA_KEY *
ossl_ml_dsa_d2i_PKCS8(const uint8_t *prvenc, int prvlen,
                      int evp_type, PROV_CTX *provctx,
                      const char *propq)
{
    OSSL_LIB_CTX *libctx = PROV_LIBCTX_OF(provctx);
    const ML_DSA_PARAMS *v;
    const ML_DSA_CODEC *codec;
    ML_DSA_PKCS8_FMT_PREF *vp8_alloc = NULL, *slot;
    const ML_DSA_PKCS8_FMT *p8fmt;
    ML_DSA_KEY *key = NULL, *ret = NULL;
    PKCS8_PRIV_KEY_INFO *p8inf = NULL;
    const uint8_t *buf, *pos;
    const X509_ALGOR *alg = NULL;
    const char *formats;
    int len, ptype, retain, prefer;
    uint32_t magic;
    uint16_t seed_magic;
    const uint8_t *seed = NULL;
    const uint8_t *priv = NULL;

    /* Which ML-DSA variant? */
    if ((v = ossl_ml_dsa_params_get(evp_type)) == NULL
        || (codec = ml_dsa_get_codec(evp_type)) == NULL)
        return 0;

    /* Extract the key OID and any parameters. */
    if ((p8inf = d2i_PKCS8_PRIV_KEY_INFO(NULL, &prvenc, prvlen)) == NULL)
        return 0;
    /* Shortest prefix is 4 bytes: seq tag/len  + octet string tag/len */
    if (!PKCS8_pkey_get0(NULL, &buf, &len, &alg, p8inf))
        goto end;
    /* Bail out early if this is some other key type. */
    if (OBJ_obj2nid(alg->algorithm) != evp_type)
        goto end;

    /* Get the list of enabled decoders. Their order is not important here. */
    formats = ossl_prov_ctx_get_param(
        provctx, OSSL_PKEY_PARAM_ML_DSA_INPUT_FORMATS, NULL);
    vp8_alloc = vp8_order(v->alg, codec->p8fmt, "input", formats);
    if (vp8_alloc == NULL)
        goto end;

    /* Parameters must be absent. */
    X509_ALGOR_get0(NULL, &ptype, NULL, alg);
    if (ptype != V_ASN1_UNDEF) {
        ERR_raise_data(ERR_LIB_PROV, PROV_R_UNEXPECTED_KEY_PARAMETERS,
                       "unexpected parameters with a PKCS#8 %s private key",
                       v->alg);
        goto end;
    }
    if ((ossl_ssize_t)len < (ossl_ssize_t)sizeof(magic))
        goto end;

    /* Find the matching p8 info slot, that also has the expected length. */
    pos = OPENSSL_load_u32_be(&magic, buf);
    for (slot = vp8_alloc; (p8fmt = slot->fmt) != NULL; ++slot) {
        if (len != (ossl_ssize_t)p8fmt->p8_bytes)
            continue;
        if (p8fmt->p8_shift == sizeof(magic)
            || (magic >> (p8fmt->p8_shift * 8)) == p8fmt->p8_magic) {
            pos -= p8fmt->p8_shift;
            break;
        }
    }
    if (p8fmt == NULL
        || (p8fmt->seed_length > 0 && p8fmt->seed_length != ML_DSA_SEED_BYTES)
        || (p8fmt->priv_length > 0 && p8fmt->priv_length != v->sk_len)
        || (p8fmt->pub_length > 0 && p8fmt->pub_length != v->pk_len)) {
        ERR_raise_data(ERR_LIB_PROV, PROV_R_ML_DSA_NO_FORMAT,
                       "no matching enabled %s private key input formats",
                       v->alg);
        goto end;
    }

    if (p8fmt->seed_length > 0) {
        /* Check |seed| tag/len, if not subsumed by |magic|. */
        if (pos + sizeof(uint16_t) == buf + p8fmt->seed_offset) {
            pos = OPENSSL_load_u16_be(&seed_magic, pos);
            if (seed_magic != p8fmt->seed_magic)
                goto end;
        } else if (pos != buf + p8fmt->seed_offset) {
            goto end;
        }
        pos += ML_DSA_SEED_BYTES;
    }
    if (p8fmt->priv_length > 0) {
        /* Check |priv| tag/len */
        if (pos + sizeof(uint32_t) == buf + p8fmt->priv_offset) {
            pos = OPENSSL_load_u32_be(&magic, pos);
            if (magic != p8fmt->priv_magic)
                goto end;
        } else if (pos != buf + p8fmt->priv_offset) {
            goto end;
        }
        pos += v->sk_len;
    }
    if (p8fmt->pub_length > 0) {
        if (pos != buf + p8fmt->pub_offset)
            goto end;
        pos += v->pk_len;
    }
    if (pos != buf + len)
        goto end;

    /*
     * Collect the seed and/or key into a "decoded" private key object,
     * to be turned into a real key on provider "load" or "import".
     */
    if ((key = ossl_ml_dsa_key_new(libctx, propq, evp_type)) == NULL)
        goto end;
    if (p8fmt->seed_length > 0)
        seed = buf + p8fmt->seed_offset;
    if (p8fmt->priv_length > 0)
        priv = buf + p8fmt->priv_offset;
    /* Any OQS public key content is ignored */

    /*
     * If the key ends up "loaded" into the same provider, these are the
     * correct config settings, otherwise, new values will be assigned on
     * import into a different provider.  The "load" API does not pass along
     * the provider context.
     */
    retain =
        ossl_prov_ctx_get_bool_param(
            provctx, OSSL_PKEY_PARAM_ML_DSA_RETAIN_SEED, 1);
    prefer =
        ossl_prov_ctx_get_bool_param(
            provctx, OSSL_PKEY_PARAM_ML_DSA_PREFER_SEED, 1);
    if (ossl_ml_dsa_set_prekey(key, prefer, retain,
                               seed, ML_DSA_SEED_BYTES, priv, v->sk_len))
        ret = key;

  end:
    OPENSSL_free(vp8_alloc);
    PKCS8_PRIV_KEY_INFO_free(p8inf);
    if (ret == NULL)
        ossl_ml_dsa_key_free(key);
    return ret;
}

/* Same as ossl_ml_dsa_encode_pubkey, but allocates the output buffer. */
int ossl_ml_dsa_i2d_pubkey(const ML_DSA_KEY *key, unsigned char **out)
{
    const ML_DSA_PARAMS *params = ossl_ml_dsa_key_params(key);
    const uint8_t *pk = ossl_ml_dsa_key_get_pub(key);

    if (pk == NULL) {
        ERR_raise_data(ERR_LIB_PROV, PROV_R_NOT_A_PUBLIC_KEY,
                       "no %s public key data available", params->alg);
        return 0;
    }
    if (out != NULL
        && (*out = OPENSSL_memdup(pk, params->pk_len)) == NULL)
        return 0;
    return (int)params->pk_len;
}

/* Allocate and encode PKCS#8 private key payload. */
int ossl_ml_dsa_i2d_prvkey(const ML_DSA_KEY *key, uint8_t **out,
                           PROV_CTX *provctx)
{
    const ML_DSA_PARAMS *params = ossl_ml_dsa_key_params(key);
    const ML_DSA_CODEC *codec;
    ML_DSA_PKCS8_FMT_PREF *vp8_alloc, *slot;
    const ML_DSA_PKCS8_FMT *p8fmt;
    uint8_t *buf = NULL, *pos;
    const char *formats;
    int len = ML_DSA_SEED_BYTES;
    int ret = 0;
    const uint8_t *seed = ossl_ml_dsa_key_get_seed(key);
    const uint8_t *sk = ossl_ml_dsa_key_get_priv(key);

    /* Not ours to handle */
    if ((codec = ml_dsa_get_codec(params->evp_type)) == NULL)
        return 0;

    if (sk == NULL) {
        ERR_raise_data(ERR_LIB_PROV, PROV_R_NOT_A_PRIVATE_KEY,
                       "no %s private key data available",
                       params->alg);
        return 0;
    }

    formats = ossl_prov_ctx_get_param(
        provctx, OSSL_PKEY_PARAM_ML_DSA_OUTPUT_FORMATS, NULL);
    vp8_alloc = vp8_order(params->alg, codec->p8fmt, "output", formats);
    if (vp8_alloc == NULL)
        return 0;

    /* If we don't have a seed, skip seedful entries */
    for (slot = vp8_alloc; (p8fmt = slot->fmt) != NULL; ++slot)
        if (seed != NULL || p8fmt->seed_length == 0)
            break;
    /* No matching table entries, give up */
    if (p8fmt == NULL
        || (p8fmt->seed_length > 0 && p8fmt->seed_length != ML_DSA_SEED_BYTES)
        || (p8fmt->priv_length > 0 && p8fmt->priv_length != params->sk_len)
        || (p8fmt->pub_length > 0 && p8fmt->pub_length != params->pk_len)) {
        ERR_raise_data(ERR_LIB_PROV, PROV_R_ML_DSA_NO_FORMAT,
                       "no matching enabled %s private key output formats",
                       params->alg);
        goto end;
    }
    len = p8fmt->p8_bytes;

    if (out == NULL) {
        ret = len;
        goto end;
    }

    if ((pos = buf = OPENSSL_malloc((size_t) len)) == NULL)
        goto end;

    switch (p8fmt->p8_shift) {
    case 0:
        pos = OPENSSL_store_u32_be(pos, p8fmt->p8_magic);
        break;
    case 2:
        pos = OPENSSL_store_u16_be(pos, (uint16_t)p8fmt->p8_magic);
        break;
    case 4:
        break;
    default:
        ERR_raise_data(ERR_LIB_PROV, ERR_R_INTERNAL_ERROR,
                       "error encoding %s private key", params->alg);
        goto end;
    }

    if (p8fmt->seed_length != 0) {
        /*
         * Either the tag/len were already included in |magic| or they require
         * us to write two bytes now.
         */
        if (pos + sizeof(uint16_t) == buf + p8fmt->seed_offset)
            pos = OPENSSL_store_u16_be(pos, p8fmt->seed_magic);
        if (pos != buf + p8fmt->seed_offset) {
            ERR_raise_data(ERR_LIB_PROV, ERR_R_INTERNAL_ERROR,
                           "error encoding %s private key", params->alg);
            goto end;
        }
        memcpy(pos, seed, ML_DSA_SEED_BYTES);
        pos += ML_DSA_SEED_BYTES;
    }
    if (p8fmt->priv_length != 0) {
        if (pos + sizeof(uint32_t) == buf + p8fmt->priv_offset)
            pos = OPENSSL_store_u32_be(pos, p8fmt->priv_magic);
        if (pos != buf + p8fmt->priv_offset) {
            ERR_raise_data(ERR_LIB_PROV, ERR_R_INTERNAL_ERROR,
                           "error encoding %s private key", params->alg);
            goto end;
        }
        memcpy(pos, sk, params->sk_len);
        pos += params->sk_len;
    }
    /* OQS form output with tacked-on public key */
    if (p8fmt->pub_length != 0) {
        /* The OQS pubkey is never separately DER-wrapped */
        if (pos != buf + p8fmt->pub_offset) {
            ERR_raise_data(ERR_LIB_PROV, ERR_R_INTERNAL_ERROR,
                           "error encoding %s private key", params->alg);
            goto end;
        }
        memcpy(pos, ossl_ml_dsa_key_get_pub(key), params->pk_len);
        pos += params->pk_len;
    }

    if (pos == buf + len) {
        *out = buf;
        ret = len;
    }

  end:
    OPENSSL_free(vp8_alloc);
    if (ret == 0)
        OPENSSL_free(buf);
    return ret;
}

int ossl_ml_dsa_key_to_text(BIO *out, const ML_DSA_KEY *key, int selection)
{
    const ML_DSA_PARAMS *params;
    const uint8_t *seed, *sk, *pk;

    if (out == NULL || key == NULL) {
        ERR_raise(ERR_LIB_PROV, ERR_R_PASSED_NULL_PARAMETER);
        return 0;
    }
    params = ossl_ml_dsa_key_params(key);
    pk = ossl_ml_dsa_key_get_pub(key);
    sk = ossl_ml_dsa_key_get_priv(key);
    seed = ossl_ml_dsa_key_get_seed(key);

    if (pk == NULL) {
        /* Regardless of the |selection|, there must be a public key */
        ERR_raise_data(ERR_LIB_PROV, PROV_R_MISSING_KEY,
                       "no %s key material available", params->alg);
        return 0;
    }

    if ((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) != 0) {
        if (sk == NULL) {
            ERR_raise_data(ERR_LIB_PROV, PROV_R_MISSING_KEY,
                           "no %s key material available", params->alg);
            return 0;
        }
        if (BIO_printf(out, "%s Private-Key:\n", params->alg) <= 0)
            return 0;
        if (seed != NULL && !ossl_bio_print_labeled_buf(out, "seed:", seed,
                                                        ML_DSA_SEED_BYTES))
            return 0;
        if (!ossl_bio_print_labeled_buf(out, "priv:", sk, params->sk_len))
            return 0;
    } else if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) != 0) {
        if (BIO_printf(out, "%s Public-Key:\n", params->alg) <= 0)
            return 0;
    }

    if (!ossl_bio_print_labeled_buf(out, "pub:", pk, params->pk_len))
        return 0;

    return 1;
}
