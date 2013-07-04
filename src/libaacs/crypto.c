/*
 * This file is part of libaacs
 * Copyright (C) 2009-2010  Obliter0n
 * Copyright (C) 2010-2013  npzacs
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include "crypto.h"
#include "util/strutl.h"
#include "util/macro.h"
#include "util/logging.h"

#include <string.h>
#include <stdio.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <gcrypt.h>
#ifdef HAVE_PTHREAD_H
#include <pthread.h>
#endif
#include <errno.h>

/* elliptic curve from AACS specs */
#define AACS_EC_p   "9DC9D81355ECCEB560BDB09EF9EAE7C479A7D7DF"
#define AACS_EC_a   "9DC9D81355ECCEB560BDB09EF9EAE7C479A7D7DC"
#define AACS_EC_b   "402DAD3EC1CBCD165248D68E1245E0C4DAACB1D8"
#define AACS_EC_n   "9DC9D81355ECCEB560BDC44F54817B2C7F5AB017"
#define AACS_EC_G_x "2E64FC22578351E6F4CCA7EB81D0A4BDC54CCEC6"
#define AACS_EC_G_y "0914A25DD05442889DB455C7F23C9A0707F5CBB9"

/* Set this in CFLAGS to debug gcrypt MPIs and S-expressions */
#ifndef GCRYPT_DEBUG
#define GCRYPT_DEBUG 0
#endif

/* Use pthread in libgcrypt */
#ifdef HAVE_PTHREAD_H
GCRY_THREAD_OPTION_PTHREAD_IMPL;
#endif

/* include some elliptic curve utils from libgcrypt */
#include "ec.c"

typedef struct {
    gcry_mpi_t  p, a, b, n;
    mpi_point_t G;
} elliptic_curve_t;

static void _aacs_curve_init(elliptic_curve_t *c)
{
    /* elliptic curve from AACS specs */
    const uint8_t p[20]   = { 0x9D,0xC9,0xD8,0x13,0x55,0xEC,0xCE,0xB5,0x60,0xBD,
                              0xB0,0x9E,0xF9,0xEA,0xE7,0xC4,0x79,0xA7,0xD7,0xDF };
    const uint8_t a[20]   = { 0x9D,0xC9,0xD8,0x13,0x55,0xEC,0xCE,0xB5,0x60,0xBD,
                              0xB0,0x9E,0xF9,0xEA,0xE7,0xC4,0x79,0xA7,0xD7,0xDC };
    const uint8_t b[20]   = { 0x40,0x2D,0xAD,0x3E,0xC1,0xCB,0xCD,0x16,0x52,0x48,
                              0xD6,0x8E,0x12,0x45,0xE0,0xC4,0xDA,0xAC,0xB1,0xD8 };
    const uint8_t n[20]   = { 0x9D,0xC9,0xD8,0x13,0x55,0xEC,0xCE,0xB5,0x60,0xBD,
                              0xC4,0x4F,0x54,0x81,0x7B,0x2C,0x7F,0x5A,0xB0,0x17 };
    const uint8_t G_x[20] = { 0x2E,0x64,0xFC,0x22,0x57,0x83,0x51,0xE6,0xF4,0xCC,
                              0xA7,0xEB,0x81,0xD0,0xA4,0xBD,0xC5,0x4C,0xCE,0xC6 };
    const uint8_t G_y[20] = { 0x09,0x14,0xA2,0x5D,0xD0,0x54,0x42,0x88,0x9D,0xB4,
                              0x55,0xC7,0xF2,0x3C,0x9A,0x07,0x07,0xF5,0xCB,0xB9 };

    memset(c, 0, sizeof(*c));

    gcry_mpi_scan (&c->p,   GCRYMPI_FMT_USG, p,   20, NULL);
    gcry_mpi_scan (&c->a,   GCRYMPI_FMT_USG, a,   20, NULL);
    gcry_mpi_scan (&c->b,   GCRYMPI_FMT_USG, b,   20, NULL);
    gcry_mpi_scan (&c->n,   GCRYMPI_FMT_USG, n,   20, NULL);
    gcry_mpi_scan (&c->G.x, GCRYMPI_FMT_USG, G_x, 20, NULL);
    gcry_mpi_scan (&c->G.y, GCRYMPI_FMT_USG, G_y, 20, NULL);
    c->G.z = mpi_alloc_set_ui(1);
}

static void _curve_free(elliptic_curve_t *c)
{
    gcry_mpi_release(c->p); c->p = NULL;
    gcry_mpi_release(c->a); c->a = NULL;
    gcry_mpi_release(c->b); c->b = NULL;
    gcry_mpi_release(c->n); c->n = NULL;
    point_free(&c->G);
}

static void _aesg3(const uint8_t *src_key, uint8_t *dst_key, uint8_t inc)
{
    int a;
    gcry_cipher_hd_t gcry_h;
    uint8_t seed[16] = { 0x7B, 0x10, 0x3C, 0x5D, 0xCB, 0x08, 0xC4, 0xE5,
                         0x1A, 0x27, 0xB0, 0x17, 0x99, 0x05, 0x3B, 0xD9 };
    seed[15] += inc;

    gcry_cipher_open(&gcry_h, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_ECB, 0);
    gcry_cipher_setkey(gcry_h, src_key, 16);
    gcry_cipher_decrypt (gcry_h, dst_key, 16, seed, 16);
    gcry_cipher_close(gcry_h);

    for (a = 0; a < 16; a++) {
        dst_key[a] ^= seed[a];
    }
}

/* Initializes libgcrypt */
int crypto_init()
{
    static int crypto_init_check = 0;

    if (!crypto_init_check) {
        crypto_init_check = 1;
#ifdef HAVE_PTHREAD_H
        gcry_control(GCRYCTL_SET_THREAD_CBS, &gcry_threads_pthread);
#endif
        if (!gcry_check_version(GCRYPT_VERSION)) {
            crypto_init_check = 0;
        }
    }

    return crypto_init_check;
}

void crypto_aesg3(const uint8_t *D, uint8_t *lsubk, uint8_t* rsubk, uint8_t *pk)
{
    if (lsubk) {
        _aesg3(D, lsubk, 0);
    }

    if (pk) {
        _aesg3(D, pk, 1);
    }

    if (rsubk) {
        _aesg3(D, rsubk, 2);
    }
}

/*
 * AES CMAC
 */

static void _shl_128(unsigned char *dst, const unsigned char *src)
{
    uint8_t overflow = 0;
    int i;

    for (i = 15; i >= 0; i--) {
        dst[i] = (src[i] << 1) | overflow;
	overflow = src[i] >> 7;
    }
}

static void _cmac_key(const unsigned char *aes_key, unsigned char *k1, unsigned char *k2)
{
    uint8_t key[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    gcry_cipher_hd_t gcry_h;

    gcry_cipher_open(&gcry_h, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_ECB, 0);
    gcry_cipher_setkey(gcry_h, aes_key, 16);
    gcry_cipher_encrypt (gcry_h, key, 16, NULL, 0);
    gcry_cipher_close(gcry_h);

    _shl_128(k1, key);
    if (key[0] & 0x80) {
        k1[15] ^= 0x87;
    }

    _shl_128(k2, k1);
    if (k1[0] & 0x80) {
        k2[15] ^= 0x87;
    }
}

void crypto_aes_cmac_16(const unsigned char *data, const unsigned char *aes_key, unsigned char *cmac)
{
    gcry_cipher_hd_t gcry_h;
    uint8_t k1[16], k2[16];
    unsigned ii;

    /*
     * Somplified version of AES CMAC. Spports only 16-byte input data.
     */

    /* generate CMAC keys */
    _cmac_key(aes_key, k1, k2);

    memcpy(cmac, data, 16);
    for (ii = 0; ii < 16; ii++) {
        cmac[ii] ^= k1[ii];
    }
 
    gcry_cipher_open(&gcry_h, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_ECB, 0);
    gcry_cipher_setkey(gcry_h, aes_key, 16);
    gcry_cipher_encrypt (gcry_h, cmac, 16, 0, 16);
    gcry_cipher_close(gcry_h);
}


#if defined(HAVE_STRERROR_R) && defined(HAVE_LIBGPG_ERROR)
#define LOG_GCRY_ERROR(msg, func, err)                                  \
  char errstr[100] = {0};                                               \
  gpg_strerror_r(err, errstr, sizeof(errstr));                          \
  DEBUG(DBG_AACS|DBG_CRIT, "%s: %s failed. error was: %s\n", func, msg, errstr);
#else
#define LOG_GCRY_ERROR(msg, func, err)                                  \
  DEBUG(DBG_AACS|DBG_CRIT, "%s: %s failed. error was: %s\n", func, msg, gcry_strerror(err));
#endif

#define GCRY_VERIFY(msg, op)                                \
    if ((err = (op))) {                                     \
        LOG_GCRY_ERROR(msg, __PRETTY_FUNCTION__, err);      \
        goto error;                                         \
    }

/*
 * build S-expressions
 */

static gcry_error_t _aacs_sexp_key(gcry_sexp_t *p_sexp_key,
                                   const uint8_t *q_x, const uint8_t *q_y,
                                   const uint8_t *priv_key)
{
    gcry_mpi_t    mpi_d = NULL;
    unsigned char Q[41];
    char          str_Q[sizeof(Q) * 2 + 1];
    gcry_error_t  err;

    /* Assign MPI values for ECDSA parameters Q and d.
     * Values are:
     *   Q.x = c[12]..c[31]
     *   Q.y = c[32]..c[51]
     *   d = priv_key
     *
     * Note: The MPI values for Q are in the form "<format>||Q.x||Q.y".
     */
    memcpy(&Q[0],  "\x04", 1); // format
    memcpy(&Q[1],  q_x, 20);   // Q.x
    memcpy(&Q[21], q_y, 20);   // Q.y
    if (priv_key) {
        gcry_mpi_scan(&mpi_d, GCRYMPI_FMT_USG, priv_key, 20, NULL);
    }

    /* Show the values of the MPIs Q.x, Q.y, and d when debugging */
    if (GCRYPT_DEBUG) {
        gcry_mpi_t mpi_Q_x, mpi_Q_y;
        gcry_mpi_scan(&mpi_Q_x, GCRYMPI_FMT_USG, q_x, 20, NULL);
        gcry_mpi_scan(&mpi_Q_y, GCRYMPI_FMT_USG, q_y, 20, NULL);
        gcry_mpi_dump(mpi_Q_x);
        printf("\n");
        gcry_mpi_dump(mpi_Q_y);
        printf("\n");
        if (mpi_d) {
            gcry_mpi_dump(mpi_d);
            printf("\n");
        }
    }

    /* Build the s-expression for the ecdsa private key
     * Constant values are:
     *   p = 900812823637587646514106462588455890498729007071
     *   a = -3
     *   b = 366394034647231750324370400222002566844354703832
     *   G.x = 264865613959729647018113670854605162895977008838
     *   G.y = 51841075954883162510413392745168936296187808697
     *   n = 900812823637587646514106555566573588779770753047
     *
     * Note: Here a = -3 mod p
     */

    /* Points are currently only supported in standard format, so get a
     * hexstring out of Q.
     */
    hex_array_to_hexstring(str_Q, Q, sizeof(Q));

    char *strfmt = str_printf(
      "(%s"
      "(ecdsa"
      "(p #"AACS_EC_p"#)"
      "(a #"AACS_EC_a"#)"
      "(b #"AACS_EC_b"#)"
      "(g #04"
          AACS_EC_G_x
          AACS_EC_G_y
          "#)"
      "(n #"AACS_EC_n"#)"
      "(q #%s#)"
      "%s))",
      mpi_d ? "private-key" : "public-key",
      str_Q,
      mpi_d ? "(d %m)" : ""
      );

    /* Now build the S-expression */
    GCRY_VERIFY("gcry_sexp_build",
                gcry_sexp_build(p_sexp_key, NULL, strfmt, mpi_d));

    /* Dump information about the key s-expression when debugging */
    if (GCRYPT_DEBUG) {
        gcry_sexp_dump(*p_sexp_key);
    }

error:
    X_FREE(strfmt);

    if (mpi_d) {
        gcry_mpi_release(mpi_d);
    }

    return err;
}

static gcry_error_t _aacs_sexp_sha1(gcry_sexp_t *p_sexp_data,
                                    const uint8_t *block, uint32_t len)
{
    gcry_mpi_t   mpi_md = NULL;
    uint8_t      md[20];
    gcry_error_t err;

    gcry_md_hash_buffer(GCRY_MD_SHA1, md, block, len);
    gcry_mpi_scan(&mpi_md, GCRYMPI_FMT_USG, md, sizeof(md), NULL);

    /* Dump information about the md MPI when debugging */
    if (GCRYPT_DEBUG) {
        fprintf(stderr, "SHA1: ");
        gcry_mpi_dump(mpi_md);
        fprintf(stderr, "\n");
    }

    /* Build an s-expression for the hash */
    GCRY_VERIFY("gcry_sexp_build",
                gcry_sexp_build(p_sexp_data, NULL,
                                "(data"
                                "  (flags raw)"
                                "  (value %m))",
                                mpi_md
                                ));

    /* Dump information about the data s-expression when debugging */
    if (GCRYPT_DEBUG) {
        gcry_sexp_dump(*p_sexp_data);
    }

 error:

    gcry_mpi_release(mpi_md);

    return err;
}

static gcry_error_t _aacs_sexp_signature(gcry_sexp_t *p_sexp_sign,
                                         const uint8_t *signature)
{
    gcry_mpi_t   mpi_r = NULL;
    gcry_mpi_t   mpi_s = NULL;
    gcry_error_t err;

    gcry_mpi_scan(&mpi_r, GCRYMPI_FMT_USG, signature,      20, NULL);
    gcry_mpi_scan(&mpi_s, GCRYMPI_FMT_USG, signature + 20, 20, NULL);

    /* Dump information about the md MPI when debugging */
    if (GCRYPT_DEBUG) {
        fprintf(stderr, "signature: ");
        gcry_mpi_dump(mpi_r);
        gcry_mpi_dump(mpi_s);
        fprintf(stderr, "\n");
    }

    /* Build an s-expression for the signature */
    GCRY_VERIFY("gcry_sexp_build",
                gcry_sexp_build(p_sexp_sign, NULL,
                               "(sig-val"
                               "  (ecdsa"
                               "    (r %m) (s %m)))",
                               mpi_r, mpi_s));

    /* Dump information about the data s-expression when debugging */
    if (GCRYPT_DEBUG) {
        gcry_sexp_dump(*p_sexp_sign);
    }

error:

    gcry_mpi_release(mpi_r);
    gcry_mpi_release(mpi_s);

    return err;
}

/*
 *
 */

void crypto_aacs_sign(const uint8_t *cert, const uint8_t *priv_key, uint8_t *signature,
                      const uint8_t *nonce, const uint8_t *point)
{
    gcry_sexp_t sexp_key = NULL, sexp_data = NULL, sexp_sig = NULL, sexp_r = NULL, sexp_s = NULL;
    gcry_mpi_t mpi_r = NULL, mpi_s = NULL;
    unsigned char block[60];
    gcry_error_t err;

    GCRY_VERIFY("_aacs_sexp_key",
                _aacs_sexp_key(&sexp_key, cert + 12, cert + 32, priv_key));

    /* Calculate the sha1 hash from the nonce and host key point and covert
     * the hash into an MPI.
     */
    memcpy(&block[0], nonce, 20);
    memcpy(&block[20], point, 40);

    GCRY_VERIFY("_aacs_sexp_sha1",
                _aacs_sexp_sha1(&sexp_data, block, sizeof(block)));

    /* Sign the hash with the ECDSA key. The resulting s-expression should be
     * in the form:
     * (sig-val
     *   (dsa
     *     (r r-mpi)
     *     (s s-mpi)))
     */
    GCRY_VERIFY("gcry_pk_sign",
                gcry_pk_sign(&sexp_sig, sexp_data, sexp_key));

    /* Dump information about the signature s-expression when debugging */
    if (GCRYPT_DEBUG) {
        gcry_sexp_dump(sexp_sig);
    }

    /* Get the resulting s-expressions for 'r' and 's' */
    sexp_r = gcry_sexp_find_token(sexp_sig, "r", 0);
    sexp_s = gcry_sexp_find_token(sexp_sig, "s", 0);

    /* Dump information about 'r' and 's' values when debugging */
    if (GCRYPT_DEBUG) {
        gcry_sexp_dump(sexp_r);
        gcry_sexp_dump(sexp_s);
    }

    /* Finally concatenate 'r' and 's' to get the ECDSA signature */
    mpi_r = gcry_sexp_nth_mpi (sexp_r, 1, GCRYMPI_FMT_USG);
    mpi_s = gcry_sexp_nth_mpi (sexp_s, 1, GCRYMPI_FMT_USG);
    gcry_mpi_print (GCRYMPI_FMT_USG, signature,      20, NULL, mpi_r);
    gcry_mpi_print (GCRYMPI_FMT_USG, signature + 20, 20, NULL, mpi_s);

 error:

    /* Free allocated memory */
    gcry_sexp_release(sexp_key);
    gcry_sexp_release(sexp_data);
    gcry_sexp_release(sexp_sig);
    gcry_sexp_release(sexp_r);
    gcry_sexp_release(sexp_s);
    gcry_mpi_release(mpi_r);
    gcry_mpi_release(mpi_s);
}

static int _aacs_verify(const uint8_t *signature,
                        const uint8_t *q_x, const uint8_t *q_y,
                        const uint8_t *data, uint32_t len)
{
    gcry_sexp_t  sexp_key  = NULL;
    gcry_sexp_t  sexp_sig  = NULL;
    gcry_sexp_t  sexp_data = NULL;
    gcry_error_t err;

    GCRY_VERIFY("_aacs_sexp_key",
                _aacs_sexp_key(&sexp_key, q_x, q_y, NULL));

    GCRY_VERIFY("_aacs_sexp_sha1",
                _aacs_sexp_sha1(&sexp_data, data, len));

    GCRY_VERIFY("_aacs_sexp_signature",
                _aacs_sexp_signature(&sexp_sig, signature));

    GCRY_VERIFY("gcry_pk_verify",
                gcry_pk_verify(sexp_sig, sexp_data, sexp_key));

 error:
    gcry_sexp_release(sexp_sig);
    gcry_sexp_release(sexp_data);
    gcry_sexp_release(sexp_key);

    return err;
}

int crypto_aacs_verify(const uint8_t *cert, const uint8_t *signature, const uint8_t *data, uint32_t len)
{
    return !_aacs_verify(signature, cert + 12, cert + 32, data, len);
}

int  crypto_aacs_verify_aacsla(const uint8_t *signature, const uint8_t *data, uint32_t len)
{
    static const uint8_t aacs_la_pubkey_x[] = {0x63, 0xC2, 0x1D, 0xFF, 0xB2, 0xB2, 0x79, 0x8A, 0x13, 0xB5,
                                               0x8D, 0x61, 0x16, 0x6C, 0x4E, 0x4A, 0xAC, 0x8A, 0x07, 0x72 };
    static const uint8_t aacs_la_pubkey_y[] = {0x13, 0x7E, 0xC6, 0x38, 0x81, 0x8F, 0xD9, 0x8F, 0xA4, 0xC3,
                                               0x0B, 0x99, 0x67, 0x28, 0xBF, 0x4B, 0x91, 0x7F, 0x6A, 0x27 };

    return !_aacs_verify(signature, aacs_la_pubkey_x, aacs_la_pubkey_y, data, len);
}

int crypto_aacs_verify_cert(const uint8_t *cert)
{
    if (MKINT_BE16(cert+2) != 0x5c) {
        DEBUG(DBG_AACS, "Certificate length is invalid (0x%04x), expected 0x005c\n",
              MKINT_BE16(cert+2));
        return 0;
    }

    return crypto_aacs_verify_aacsla(cert + 52, cert, 52);
}

int crypto_aacs_verify_host_cert(const uint8_t *cert)
{
    if (cert[0] != 0x02) {
        DEBUG(DBG_AACS, "Host certificate type is invalid (0x%02x), expected 0x01\n", cert[0]);
        return 0;
    }

    if (!crypto_aacs_verify_cert(cert)) {
        DEBUG(DBG_AACS, "Host certificate signature is invalid\n");
        return 0;
    }

    return 1;
}

int crypto_aacs_verify_drive_cert(const uint8_t *cert)
{
    if (cert[0] != 0x01) {
        DEBUG(DBG_AACS, "Drive certificate type is invalid (0x%02x), expected 0x01\n", cert[0]);
        return 0;
    }

    if (!crypto_aacs_verify_cert(cert)) {
        DEBUG(DBG_AACS, "Drive certificate signature is invalid\n");
        return 0;
    }

    return 1;
}

void crypto_aacs_title_hash(const uint8_t *ukf, uint64_t len, uint8_t *hash)
{
    gcry_md_hash_buffer(GCRY_MD_SHA1, hash, ukf, len);
}

void crypto_create_nonce(uint8_t *buf, size_t len)
{
    gcry_create_nonce(buf, len);
}

void crypto_create_bus_key(const uint8_t *priv_key, const uint8_t *drive_key_point, unsigned char *bus_key)
{
    /* init AACS curve */

    elliptic_curve_t ec;
    _aacs_curve_init(&ec);

    /* init ec context */

    mpi_ec_t ctx = _gcry_mpi_ec_init (ec.p, ec.a);

    /* parse input data */

    gcry_mpi_t mpi_priv_key = NULL;
    gcry_mpi_scan (&mpi_priv_key, GCRYMPI_FMT_USG, priv_key, 20, NULL);

    mpi_point_t Q;
    point_init (&Q);
    gcry_mpi_scan (&Q.x, GCRYMPI_FMT_USG, drive_key_point,      20, NULL);
    gcry_mpi_scan (&Q.y, GCRYMPI_FMT_USG, drive_key_point + 20, 20, NULL);
    Q.z = mpi_alloc_set_ui(1);

    /* calculate bus key point: multiply drive key point with private key */

    mpi_point_t bus_key_point;
    point_init (&bus_key_point);
    _gcry_mpi_ec_mul_point (&bus_key_point, mpi_priv_key, &Q, ctx);

    /* bus key is lowest 128 bits of bus_key_point x-coordinate */

    /* get affine coordinates (Hv) */
    gcry_mpi_t q_x = mpi_new(0);
    gcry_mpi_t q_y = mpi_new(0);
    _gcry_mpi_ec_get_affine (q_x, q_y, &bus_key_point, ctx);

    /* convert to binary */
    uint8_t q_x_bin[100];
    size_t n = 0;
    gcry_mpi_print (GCRYMPI_FMT_USG, q_x_bin, sizeof(q_x_bin), &n, q_x);

    memcpy(bus_key, q_x_bin + n - 16, 16);

    /* cleanup */

    _gcry_mpi_ec_free (ctx);
    _curve_free(&ec);
    mpi_free(mpi_priv_key);
    point_free(&Q);
    point_free(&bus_key_point);
    mpi_free(q_x);
    mpi_free(q_y);
}

void crypto_create_host_key_pair(uint8_t *host_key, uint8_t *host_key_point)
{
    /*
     * AACS spec, section 4.3, steps 23-24
     */

    /* generate random number Hk (host_key) */

    gcry_mpi_t d;
    gcry_randomize(host_key, 20, 1);
    gcry_mpi_scan(&d, GCRYMPI_FMT_USG, host_key, 20, NULL);

    /* init AACS curve */

    elliptic_curve_t ec;
    _aacs_curve_init(&ec);

    /* init ec context */

    mpi_ec_t ctx = _gcry_mpi_ec_init (ec.p, ec.a);

    /* Compute point (Q) */

    mpi_point_t Q;
    point_init (&Q);
    _gcry_mpi_ec_mul_point (&Q, d, &ec.G, ctx);

    /* get affine coordinates (Hv) */

    gcry_mpi_t q_x = mpi_new(0);
    gcry_mpi_t q_y = mpi_new(0);
    _gcry_mpi_ec_get_affine (q_x, q_y, &Q, ctx);

    gcry_mpi_print (GCRYMPI_FMT_USG, host_key_point,      20, NULL, q_x);
    gcry_mpi_print (GCRYMPI_FMT_USG, host_key_point + 20, 20, NULL, q_y);

    /* cleanup */

    _gcry_mpi_ec_free (ctx);
    _curve_free(&ec);

    mpi_free(d);
    mpi_free(q_x);
    mpi_free(q_y);
    point_free(&Q);
}
