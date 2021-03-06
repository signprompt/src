/*	$NetBSD: nist_hash_drbg.c,v 1.3 2019/09/19 18:29:55 riastradh Exp $	*/

/*-
 * Copyright (c) 2019 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Taylor R. Campbell.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This file implements Hash_DRBG, a `deterministic random bit
 * generator' (more commonly known in lay terms and in the cryptography
 * literature as a pseudorandom bit generator or pseudorandom number
 * generator), described in
 *
 *	Elaine Barker and John Kelsey, `Recommendation for Random
 *	Number Generation Using Deterministic Random Bit Generators',
 *	NIST SP800-90A, June 2015.
 *
 * This code is meant to work in userland or in kernel.  For a test
 * program, compile with -DNIST_HASH_DRBG_MAIN to define a `main'
 * function; for verbose debugging output, compile with
 * -DNIST_HASH_DRBG_DEBUG, mainly useful if you need to change
 * something and have to diagnose what's wrong with the known-answer
 * tests.
 */

#ifdef _KERNEL
#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: nist_hash_drbg.c,v 1.3 2019/09/19 18:29:55 riastradh Exp $");
#endif

#include <sys/param.h>
#include <sys/types.h>
#include <sys/sha2.h>

#ifdef _KERNEL
#include <sys/systm.h>			/* memcpy */
#include <lib/libkern/libkern.h>	/* KASSERT */
#define	ASSERT		KASSERT
#else
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define	ASSERT		assert
#define	CTASSERT	__CTASSERT
#endif

#include "nist_hash_drbg.h"

#define	secret	/* must not use in variable-time operations; should zero */
#define	arraycount(A)	(sizeof(A)/sizeof(A[0]))

CTASSERT(0 < NIST_HASH_DRBG_RESEED_INTERVAL);
CTASSERT(NIST_HASH_DRBG_RESEED_INTERVAL <= INT_MAX);
CTASSERT(NIST_HASH_DRBG_RESEED_INTERVAL <= ~(~0ull << 48));

/* Instantiation: SHA-256 */
#define	HASH_LENGTH	SHA256_DIGEST_LENGTH
#define	HASH_CTX	SHA256_CTX
#define	hash_init	SHA256_Init
#define	hash_update	SHA256_Update
#define	hash_final	SHA256_Final

#define	SEEDLEN_BYTES	NIST_HASH_DRBG_SEEDLEN_BYTES

struct hvec {
	const void	*hv_base;
	size_t		hv_len;
};

static void	hashgen(secret uint8_t *, size_t,
		    const secret uint8_t[SEEDLEN_BYTES]);
static void	add8(secret uint8_t *, size_t, const secret uint8_t *, size_t);
static void	hash_df(secret void *, size_t, const struct hvec *, size_t);
static void	hash_df_block(secret void *, uint8_t, uint8_t[4],
		    const struct hvec *, size_t);

/* 10.1.1 Hash_DRBG */

int
nist_hash_drbg_destroy(struct nist_hash_drbg *D)
{

	explicit_memset(D, 0, sizeof(*D));
	D->reseed_counter = UINT_MAX; /* paranoia: make generate fail */

	/* Always return zero for hysterical raisins.  (XXX) */
	return 0;
}

/* 10.1.1.2 Instantiation of Hash_DRBG */

int
nist_hash_drbg_instantiate(secret struct nist_hash_drbg *D,
    const secret void *entropy, size_t entropylen,
    const void *nonce, size_t noncelen,
    const void *personalization, size_t personalizationlen)
{
	/*
	 * 1. seed_material = entropy_input || nonce || personalization_string
	 */
	const struct hvec seed_material[] = {
		{ .hv_base = entropy, .hv_len = entropylen },
		{ .hv_base = nonce, .hv_len = noncelen },
		{ .hv_base = personalization, .hv_len = personalizationlen },
	};

	/*
	 * 2. seed = Hash_df(seed_material, seedlen)
	 * 3. V = seed
	 */
	CTASSERT(sizeof D->V == SEEDLEN_BYTES);
	hash_df(D->V, sizeof D->V, seed_material, arraycount(seed_material));

	/* 4. C = Hash_df((0x00 || V), seedlen) */
	const struct hvec hv[] = {
		{ .hv_base = (const uint8_t[]) {0x00}, .hv_len = 1 },
		{ .hv_base = D->V, .hv_len = sizeof D->V },
	};
	CTASSERT(sizeof D->C == SEEDLEN_BYTES);
	hash_df(D->C, sizeof D->C, hv, arraycount(hv));

	/* 5. reseed_counter = 1 */
	D->reseed_counter = 1;

	/* Always return zero for hysterical raisins.  (XXX) */
	return 0;
}

/* 10.1.1.3 Reseeding a Hash_DRBG Instantiation */

int
nist_hash_drbg_reseed(secret struct nist_hash_drbg *D,
    const secret void *entropy, size_t entropylen,
    const void *additional, size_t additionallen)
{
	/* 1. seed_material = 0x01 || V || entropy_input || additional_input */
	const struct hvec seed_material[] = {
		{ .hv_base = (const uint8_t[]) {0x01}, .hv_len = 1 },
		{ .hv_base = D->V, .hv_len = sizeof D->V },
		{ .hv_base = entropy, .hv_len = entropylen },
		{ .hv_base = additional, .hv_len = additionallen },
	};
	uint8_t seed[SEEDLEN_BYTES];

	/*
	 * 2. seed = Hash_df(seed_material, seedlen)
	 * 3. V = seed
	 */
	CTASSERT(sizeof D->V == SEEDLEN_BYTES);
	hash_df(seed, sizeof seed, seed_material, arraycount(seed_material));
	memcpy(D->V, seed, sizeof D->V);

	/* 3. C = Hash_df((0x00 || V), seedlen) */
	const struct hvec hv[] = {
		{ .hv_base = (const uint8_t[]) {0x00}, .hv_len = 1 },
		{ .hv_base = D->V, .hv_len = sizeof D->V },
	};
	CTASSERT(sizeof D->C == SEEDLEN_BYTES);
	hash_df(D->C, sizeof D->C, hv, arraycount(hv));

	/* 5. reseed_counter = 1 */
	D->reseed_counter = 1;

	/* Always return zero for hysterical raisins.  (XXX) */
	return 0;
}

/* 10.1.1.4 Generating Pseudorandom Bits Using Hash_DRBG */

int
nist_hash_drbg_generate(secret struct nist_hash_drbg *D,
    secret void *output, size_t outputlen,
    const void *additional, size_t additionallen)
{
	secret HASH_CTX ctx;
	secret uint8_t H[HASH_LENGTH];
	uint8_t reseed_counter[4];

	ASSERT(outputlen <= NIST_HASH_DRBG_MAX_REQUEST_BYTES);

	/*
	 * 1. If reseed_counter > reseed_interval, then return an
	 * indication that a reseed is required.
	 */
	if (D->reseed_counter > NIST_HASH_DRBG_RESEED_INTERVAL)
		return 1;

	/* 2. If (additional_input != Null), then do: */
	if (additionallen) {
		/* 2.1 w = Hash(0x02 || V || additional_input) */
		secret uint8_t w[HASH_LENGTH];

		hash_init(&ctx);
		hash_update(&ctx, (const uint8_t[]) {0x02}, 1);
		hash_update(&ctx, D->V, sizeof D->V);
		hash_update(&ctx, additional, additionallen);
		hash_final(w, &ctx);

		/* 2.2 V = (V + w) mod 2^seedlen */
		add8(D->V, sizeof D->V, w, sizeof w);

		explicit_memset(w, 0, sizeof w);
	}

	/* 3. (returned_bits) = Hashgen(requested_number_of_bits, V) */
	hashgen(output, outputlen, D->V);

	/* 4. H = Hash(0x03 || V) */
	hash_init(&ctx);
	hash_update(&ctx, (const uint8_t[]) {0x03}, 1);
	hash_update(&ctx, D->V, sizeof D->V);
	hash_final(H, &ctx);

	/* 5. V = (V + H + C + reseed_counter) mod 2^seedlen */
	be32enc(reseed_counter, D->reseed_counter);
	add8(D->V, sizeof D->V, H, sizeof H);
	add8(D->V, sizeof D->V, D->C, sizeof D->C);
	add8(D->V, sizeof D->V, reseed_counter, sizeof reseed_counter);

	/* 6. reseed_counter = reseed_counter + 1 */
	D->reseed_counter++;

	explicit_memset(&ctx, 0, sizeof ctx);
	explicit_memset(H, 0, sizeof H);

	/* 7. Return SUCCESS, ... */
	return 0;
}

/*
 * p := H(V) || H(V + 1) || H(V + 2) || ...
 */
static void
hashgen(secret uint8_t *p, size_t n, const secret uint8_t V[SEEDLEN_BYTES])
{
	secret uint8_t data[SEEDLEN_BYTES];
	secret HASH_CTX ctx;

	/* Save a copy so that we can increment it.  */
	memcpy(data, V, SEEDLEN_BYTES);

	/* Generate block by block into p directly.  */
	while (HASH_LENGTH <= n) {
		hash_init(&ctx);
		hash_update(&ctx, data, SEEDLEN_BYTES);
		hash_final(p, &ctx);

		p += HASH_LENGTH;
		n -= HASH_LENGTH;
		add8(data, sizeof data, (const uint8_t[]) {1}, 1);
	}

	/*
	 * If any partial block requested, generate a full block and
	 * copy the part we need.
	 */
	if (n) {
		secret uint8_t t[HASH_LENGTH];

		hash_init(&ctx);
		hash_update(&ctx, data, SEEDLEN_BYTES);
		hash_final(t, &ctx);

		memcpy(p, t, n);
		explicit_memset(t, 0, sizeof t);
	}

	explicit_memset(data, 0, sizeof data);
	explicit_memset(&ctx, 0, sizeof ctx);
}

/*
 * s := s + a (big-endian, radix-2^8)
 */
static void
add8(secret uint8_t *s, size_t slen, const secret uint8_t *a, size_t alen)
{
	const size_t smax = slen - 1, amax = alen - 1;
	size_t i;
	secret unsigned c = 0;

	/* 2^8 c + s_i := s_i + a_i + c */
	for (i = 0; i < MIN(slen, alen); i++) {
		c += s[smax - i] + a[amax - i];
		s[smax - i] = c & 0xff;
		c >>= 8;
	}

	/* 2^8 c + s_i := s_i + c */
	for (; i < slen; i++) {
		c += s[smax - i];
		s[smax - i] = c & 0xff;
		c >>= 8;
	}

	explicit_memset(&c, 0, sizeof c);
}

/* 10.4.1 Derivation Function Using a Hash Function (Hash_df) */

static void
hash_df(void *h, size_t hlen, const struct hvec *input, size_t inputlen)
{
	uint8_t *p = h;
	size_t n = hlen;
	uint8_t counter = 1;
	uint8_t hbits[4];

	ASSERT(hlen <= 255*HASH_LENGTH);
	ASSERT(hlen <= UINT32_MAX/8);
	be32enc(hbits, 8*hlen);

	while (HASH_LENGTH <= n) {
		hash_df_block(p, counter++, hbits, input, inputlen);
		p += HASH_LENGTH;
		n -= HASH_LENGTH;
	}

	if (n) {
		secret uint8_t t[HASH_LENGTH];

		hash_df_block(t, counter, hbits, input, inputlen);
		memcpy(p, t, n);

		explicit_memset(t, 0, sizeof t);
	}
}

static void
hash_df_block(secret void *h, uint8_t counter, uint8_t hbits[4],
    const struct hvec *input, size_t inputlen)
{
	secret HASH_CTX ctx;
	size_t i;

	/*
	 * Hash_df Process, step 4.1:
	 * Hash(counter || no_of_bits_to_return || input_string)
	 */
	hash_init(&ctx);
	hash_update(&ctx, &counter, 1);
	hash_update(&ctx, hbits, 4);
	for (i = 0; i < inputlen; i++) {
		if (input[i].hv_len)
			hash_update(&ctx, input[i].hv_base, input[i].hv_len);
	}
	hash_final(h, &ctx);

	explicit_memset(&ctx, 0, sizeof ctx);
}

/*
 * Known-answer test vectors for Hash_DRBG with SHA-256
 */

/* Hash_DRBG.PDF, p. 190 */
static const uint8_t kat_entropy[3][SEEDLEN_BYTES] = {
	[0] = {
		0x00,0x01,0x02,0x03, 0x04,0x05,0x06,0x07,
		0x08,0x09,0x0a,0x0b, 0x0c,0x0d,0x0e,0x0f,
		0x10,0x11,0x12,0x13, 0x14,0x15,0x16,0x17,
		0x18,0x19,0x1a,0x1b, 0x1c,0x1d,0x1e,0x1f,
		0x20,0x21,0x22,0x23, 0x24,0x25,0x26,0x27,
		0x28,0x29,0x2a,0x2b, 0x2c,0x2d,0x2e,0x2f,
		0x30,0x31,0x32,0x33, 0x34,0x35,0x36,
	},
	[1] = {			/* for reseed1 */
		0x80,0x81,0x82,0x83, 0x84,0x85,0x86,0x87,
		0x88,0x89,0x8a,0x8b, 0x8c,0x8d,0x8e,0x8f,
		0x90,0x91,0x92,0x93, 0x94,0x95,0x96,0x97,
		0x98,0x99,0x9a,0x9b, 0x9c,0x9d,0x9e,0x9f,
		0xa0,0xa1,0xa2,0xa3, 0xa4,0xa5,0xa6,0xa7,
		0xa8,0xa9,0xaa,0xab, 0xac,0xad,0xae,0xaf,
		0xb0,0xb1,0xb2,0xb3, 0xb4,0xb5,0xb6,
	},
	[2] = {			/* for reseed2 */
		0xc0,0xc1,0xc2,0xc3, 0xc4,0xc5,0xc6,0xc7,
		0xc8,0xc9,0xca,0xcb, 0xcc,0xcd,0xce,0xcf,
		0xd0,0xd1,0xd2,0xd3, 0xd4,0xd5,0xd6,0xd7,
		0xd8,0xd9,0xda,0xdb, 0xdc,0xdd,0xde,0xdf,
		0xe0,0xe1,0xe2,0xe3, 0xe4,0xe5,0xe6,0xe7,
		0xe8,0xe9,0xea,0xeb, 0xec,0xed,0xee,0xef,
		0xf0,0xf1,0xf2,0xf3, 0xf4,0xf5,0xf6,
	},
};

static const uint8_t kat_nonce[] = {
	0x20,0x21,0x22,0x23, 0x24,0x25,0x26,0x27,
};

static const struct hvec kat_zero = { .hv_base = 0, .hv_len = 0 };

static const struct hvec kat_personalization = {
	.hv_len = 55,
	.hv_base = (const void *)(const uint8_t[]) { /* p. 208 */
		0x40,0x41,0x42,0x43, 0x44,0x45,0x46,0x47,
		0x48,0x49,0x4a,0x4b, 0x4c,0x4d,0x4e,0x4f,
		0x50,0x51,0x52,0x53, 0x54,0x55,0x56,0x57,
		0x58,0x59,0x5a,0x5b, 0x5c,0x5d,0x5e,0x5f,
		0x60,0x61,0x62,0x63, 0x64,0x65,0x66,0x67,
		0x68,0x69,0x6a,0x6b, 0x6c,0x6d,0x6e,0x6f,
		0x70,0x71,0x72,0x73, 0x74,0x75,0x76,
	},
};

static const struct hvec *const kat_no_additional[] = {
	[0] = &kat_zero,
	[1] = &kat_zero,
};

static const struct hvec *const kat_additional[] = {
	[0] = &(const struct hvec) {
		.hv_len = 55,
		.hv_base = (const void *)(const uint8_t[]) {
			0x60,0x61,0x62,0x63, 0x64,0x65,0x66,0x67,
			0x68,0x69,0x6a,0x6b, 0x6c,0x6d,0x6e,0x6f,
			0x70,0x71,0x72,0x73, 0x74,0x75,0x76,0x77,
			0x78,0x79,0x7a,0x7b, 0x7c,0x7d,0x7e,0x7f,
			0x80,0x81,0x82,0x83, 0x84,0x85,0x86,0x87,
			0x88,0x89,0x8a,0x8b, 0x8c,0x8d,0x8e,0x8f,
			0x90,0x91,0x92,0x93, 0x94,0x95,0x96,
		},
	},
	[1] = &(const struct hvec) {
		.hv_len = 55,
		.hv_base = (const void *)(const uint8_t[]) {
			0xa0,0xa1,0xa2,0xa3, 0xa4,0xa5,0xa6,0xa7,
			0xa8,0xa9,0xaa,0xab, 0xac,0xad,0xae,0xaf,
			0xb0,0xb1,0xb2,0xb3, 0xb4,0xb5,0xb6,0xb7,
			0xb8,0xb9,0xba,0xbb, 0xbc,0xbd,0xbe,0xbf,
			0xc0,0xc1,0xc2,0xc3, 0xc4,0xc5,0xc6,0xc7,
			0xc8,0xc9,0xca,0xcb, 0xcc,0xcd,0xce,0xcf,
			0xd0,0xd1,0xd2,0xd3, 0xd4,0xd5,0xd6,
		},
	},
};

static const struct {
	const struct hvec *personalization;
	const struct hvec *const *additional;
	bool reseed;
	uint8_t C[SEEDLEN_BYTES];
	uint8_t V[3][SEEDLEN_BYTES];
	uint8_t rnd_val[2][64];
} kat[] = {
	[0] = {			/* Hash_DRBG.pdf, p. 190 */
		.personalization = &kat_zero,
		.additional = kat_no_additional,
		.reseed = false,
		.C = {		/* p. 193 */
			0xe1,0x5d,0xe4,0xa8, 0xe3,0xb1,0x41,0x9b,
			0x61,0xd5,0x34,0xf1, 0x5d,0xbd,0x31,0xee,
			0x19,0xec,0x59,0x5f, 0x8b,0x98,0x11,0x1a,
			0x94,0xf5,0x22,0x37, 0xad,0x5d,0x66,0xf0,
			0xcf,0xaa,0xfd,0xdc, 0x90,0x19,0x59,0x02,
			0xe9,0x79,0xf7,0x9b, 0x65,0x35,0x7f,0xea,
			0x85,0x99,0x8e,0x4e, 0x37,0xd2,0xc1,
		},
		.V = {
			[0] = {		/* p. 192 */
				0xab,0x41,0xcd,0xe4, 0x37,0xab,0x8b,0x09,
				0x1c,0xa7,0xc5,0x75, 0x5d,0x10,0xf0,0x11,
				0x0c,0x1d,0xbd,0x46, 0x2f,0x22,0x6c,0xfd,
				0xab,0xfb,0xb0,0x4a, 0x8b,0xcd,0xef,0x95,
				0x16,0x7d,0x84,0xaf, 0x64,0x12,0x8c,0x0d,
				0x71,0xf4,0xd5,0xb8, 0xc0,0xed,0xfb,0xbe,
				0x3d,0xf4,0x04,0x48, 0xd2,0xd8,0xe1,
			},
			[1] = {		/* p. 195 */
				0x8c,0x9f,0xb2,0x8d, 0x1b,0x5c,0xcc,0xa4,
				0x7e,0x7c,0xfa,0x66, 0xba,0xce,0x21,0xff,
				0x26,0x0a,0x16,0xa5, 0xba,0xba,0x7f,0x14,
				0x4e,0x75,0x79,0x36, 0x8e,0x99,0x55,0xbe,
				0xfb,0xe7,0x00,0xee, 0xf8,0x72,0x77,0x6b,
				0x17,0xae,0xff,0xd5, 0x3d,0x76,0xf4,0xe3,
				0xbe,0x65,0xe8,0xc9, 0x4b,0x70,0x8f,
			},
			[2] = {		/* p. 197 */
				0x6d,0xfd,0x97,0x35, 0xff,0x0e,0x0e,0x3f,
				0xe0,0x52,0x2f,0x58, 0x18,0x8b,0x53,0xed,
				0x3f,0xf6,0x70,0x05, 0x46,0x52,0x90,0x44,
				0xb6,0x2b,0xe1,0x7d, 0x1b,0x1c,0x21,0xd0,
				0x91,0xb0,0x89,0xb1, 0x77,0x47,0x95,0xdb,
				0x14,0x22,0xa8,0x6c, 0x95,0x46,0x34,0x80,
				0x76,0xb4,0xb6,0x21, 0xc7,0x2f,0x91,
			},
		},
		.rnd_val = {
			[0] = {
				0x77,0xe0,0x5a,0x0e, 0x7d,0xc7,0x8a,0xb5,
				0xd8,0x93,0x4d,0x5e, 0x93,0xe8,0x2c,0x06,
				0xa0,0x7c,0x04,0xce, 0xe6,0xc9,0xc5,0x30,
				0x45,0xee,0xb4,0x85, 0x87,0x27,0x77,0xcf,
				0x3b,0x3e,0x35,0xc4, 0x74,0xf9,0x76,0xb8,
				0x94,0xbf,0x30,0x1a, 0x86,0xfa,0x65,0x1f,
				0x46,0x39,0x70,0xe8, 0x9d,0x4a,0x05,0x34,
				0xb2,0xec,0xad,0x29, 0xec,0x04,0x4e,0x7e,
			},
			{
				0x5f,0xf4,0xba,0x49, 0x3c,0x40,0xcf,0xff,
				0x3b,0x01,0xe4,0x72, 0xc5,0x75,0x66,0x8c,
				0xce,0x38,0x80,0xb9, 0x29,0x0b,0x05,0xbf,
				0xed,0xe5,0xec,0x96, 0xed,0x5e,0x9b,0x28,
				0x98,0x50,0x8b,0x09, 0xbc,0x80,0x0e,0xee,
				0x09,0x9a,0x3c,0x90, 0x60,0x2a,0xbd,0x4b,
				0x1d,0x4f,0x34,0x3d, 0x49,0x7c,0x60,0x55,
				0xc8,0x7b,0xb9,0x56, 0xd5,0x3b,0xf3,0x51,
			},
		},
	},

	[1] = {			/* Hash_DRBG.pdf, p. 198 */
		.personalization = &kat_zero,
		.additional = kat_additional,
		.reseed = false,
		.C = {		/* p. 201 */
			0xe1,0x5d,0xe4,0xa8, 0xe3,0xb1,0x41,0x9b,
			0x61,0xd5,0x34,0xf1, 0x5d,0xbd,0x31,0xee,
			0x19,0xec,0x59,0x5f, 0x8b,0x98,0x11,0x1a,
			0x94,0xf5,0x22,0x37, 0xad,0x5d,0x66,0xf0,
			0xcf,0xaa,0xfd,0xdc, 0x90,0x19,0x59,0x02,
			0xe9,0x79,0xf7,0x9b, 0x65,0x35,0x7f,0xea,
			0x85,0x99,0x8e,0x4e, 0x37,0xd2,0xc1,
		},
		.V = {
			[0] = {	/* p. 200 */
				0xab,0x41,0xcd,0xe4, 0x37,0xab,0x8b,0x09,
				0x1c,0xa7,0xc5,0x75, 0x5d,0x10,0xf0,0x11,
				0x0c,0x1d,0xbd,0x46, 0x2f,0x22,0x6c,0xfd,
				0xab,0xfb,0xb0,0x4a, 0x8b,0xcd,0xef,0x95,
				0x16,0x7d,0x84,0xaf, 0x64,0x12,0x8c,0x0d,
				0x71,0xf4,0xd5,0xb8, 0xc0,0xed,0xfb,0xbe,
				0x3d,0xf4,0x04,0x48, 0xd2,0xd8,0xe1,
			},
			[1] = {	/* p. 204 */
				0x8c,0x9f,0xb2,0x8d, 0x1b,0x5c,0xcc,0xa4,
				0x7e,0x7c,0xfa,0x66, 0xba,0xce,0x21,0xff,
				0x26,0x0a,0x16,0xa5, 0xba,0xba,0x7f,0x1f,
				0xd3,0x3b,0x30,0x79, 0x8f,0xb2,0x9a,0x0f,
				0xba,0x66,0x65,0x02, 0x7d,0x7f,0x10,0x58,
				0x71,0xbf,0xb4,0x40, 0xdf,0xbe,0xde,0x81,
				0xd0,0x4d,0x22,0xdf, 0xf7,0x89,0xe1,
			},
			[2] = {	/* p. 207 */
				0x6d,0xfd,0x97,0x35, 0xff,0x0e,0x0e,0x3f,
				0xe0,0x52,0x2f,0x58, 0x18,0x8b,0x53,0xed,
				0x3f,0xf6,0x70,0x05, 0x46,0x52,0x90,0xe1,
				0x7c,0x5a,0xd8,0x2d, 0xa9,0x2a,0x05,0x01,
				0xaa,0x66,0x3a,0xa6, 0x9f,0xa5,0xa0,0xb0,
				0x81,0x2b,0x4b,0x4f, 0xaf,0xf3,0xfe,0xce,
				0x79,0xcc,0xf6,0xaa, 0xde,0xc1,0xd0,
			},
		},
		.rnd_val = {
			[0] = {	/* p. 203 */
				0x51,0x07,0x24,0xb9, 0x3a,0xe9,0xa1,0x82,
				0x70,0xe4,0x84,0x73, 0x71,0x1d,0x88,0x24,
				0x63,0x1b,0xaa,0x7f, 0x1d,0x9a,0xc9,0x28,
				0x4e,0x7e,0xc8,0xf3, 0x63,0x7f,0x7a,0x74,
				0x3b,0x36,0x44,0xeb, 0x96,0xc9,0x86,0x27,
				0xc8,0xfd,0x40,0x5a, 0x7a,0x46,0x03,0xf3,
				0x8c,0xff,0x7c,0x89, 0xe9,0xc1,0x33,0xf5,
				0x85,0x1f,0x40,0xe9, 0x20,0x30,0xfe,0xa2,
			},
			[1] = {	/* p. 206 */
				0x62,0x53,0xda,0x3a, 0xae,0x8b,0x88,0xa3,
				0xb7,0x46,0xe4,0xc8, 0xb2,0x63,0x5c,0x54,
				0x0f,0x6e,0x9e,0xa7, 0x15,0x7e,0xe6,0x9d,
				0xd7,0x1e,0xfb,0x2e, 0x8f,0xf7,0xbb,0xe1,
				0xe3,0x33,0x68,0x88, 0x38,0xdd,0x7d,0xe4,
				0x9c,0xc8,0x89,0x90, 0x30,0x9c,0x96,0xcd,
				0xb2,0xab,0x92,0x95, 0x74,0x36,0xbf,0x83,
				0xd1,0xbd,0x83,0x08, 0x19,0xc7,0x48,0xca,
			},
		},
	},

	[2] = {			/* Hash_DRBG.pdf, p. 208 */
		.personalization = &kat_personalization,
		.additional = kat_no_additional,
		.reseed = false,
		.C = {		/* p. 211 */
			0x44,0x74,0x8a,0x78, 0xb1,0x6e,0x75,0x55,
			0x9f,0x88,0x1d,0x51, 0xc1,0x5d,0xfe,0x6c,
			0x52,0xcf,0xb0,0xbb, 0x71,0x62,0x01,0x69,
			0xc7,0x93,0x34,0x27, 0x67,0xe7,0xf8,0x87,
			0x5f,0x42,0xcb,0x6a, 0x20,0xc8,0x9d,0x7c,
			0x6e,0xf3,0xdc,0x61, 0x0d,0x8f,0xf2,0x03,
			0xd6,0x76,0x6c,0xed, 0x19,0x19,0xd0,
		},
		.V = {
			[0] = {	/* p. 210 */
				0xa3,0xe9,0x4e,0x39, 0x26,0xfd,0xa1,0x69,
				0xc3,0x03,0xd6,0x64, 0x38,0x39,0x05,0xe0,
				0xd7,0x99,0x62,0xd1, 0x65,0x44,0x6d,0x63,
				0xbd,0xa6,0x54,0xd1, 0x32,0xf7,0x2d,0xb4,
				0x71,0x56,0x4b,0x45, 0x6f,0xf2,0xee,0xc8,
				0x36,0x42,0x2a,0xcc, 0x5a,0x02,0x99,0x35,
				0xa7,0x99,0x29,0x90, 0x94,0xa1,0xca,
			},
			[1] = {	/* p. 213 */
				0xe8,0x5d,0xd8,0xb1, 0xd8,0x6c,0x16,0xbf,
				0x62,0x8b,0xf3,0xb5, 0xf9,0x97,0x04,0x4d,
				0x2a,0x69,0x13,0x8c, 0xd6,0xa6,0x6e,0xe7,
				0x36,0xdb,0xaa,0x3b, 0xf1,0xd0,0x28,0x3b,
				0x71,0x7b,0x33,0x6e, 0xb3,0xae,0x5b,0xdd,
				0x04,0x17,0x2e,0xa2, 0x6e,0x5a,0x48,0xf3,
				0xb3,0xfb,0xab,0xf8, 0x2f,0x76,0x79,
			},
			[2] = {	/* p. 215 */
				0x2c,0xd2,0x63,0x2a, 0x89,0xda,0x8c,0x15,
				0x02,0x14,0x11,0x07, 0xba,0xf5,0x02,0xb9,
				0x7d,0x38,0xc4,0x48, 0x48,0x08,0x71,0x0a,
				0x66,0xf8,0x40,0x11, 0xd7,0x02,0x8d,0x14,
				0xd3,0x15,0x5a,0x73, 0x79,0xad,0xd5,0x3c,
				0xc8,0xea,0x84,0xd0, 0xfc,0x64,0x1d,0xfc,
				0x62,0x9e,0x06,0x19, 0x1f,0x5f,0x6d,
			},
		},
		.rnd_val = {
			[0] = {	/* p. 213 */
				0x4a,0x62,0x66,0x4f, 0x26,0x6e,0xe5,0x37,
				0xb9,0x0d,0x64,0xb0, 0x5e,0x1d,0x81,0x3d,
				0x28,0xb1,0x59,0xa9, 0x79,0xf1,0x50,0x9d,
				0xde,0x31,0xb7,0x1d, 0xa4,0x3d,0x54,0x6e,
				0xe8,0xe7,0x86,0x78, 0x20,0x2d,0xc2,0x37,
				0xad,0x4a,0xfe,0x7d, 0xf3,0x10,0xc9,0xa4,
				0x13,0xe3,0x8a,0xaf, 0x41,0x7d,0x2d,0x22,
				0x5a,0xa3,0x65,0xec, 0x4a,0x7d,0x29,0x96,
			},
			[1] = {	/* p. 215 */
				0x59,0x58,0x3d,0x3c, 0x0a,0xc3,0x71,0x30,
				0xc4,0x78,0x9a,0x83, 0x11,0xb8,0xca,0x8f,
				0x98,0x5e,0xf1,0xe8, 0xf9,0x4d,0x95,0x4e,
				0x32,0xe3,0x44,0xa6, 0x21,0xc2,0x4b,0x2f,
				0x37,0x1d,0xa9,0xba, 0x3c,0x33,0x15,0x3f,
				0x09,0xe5,0x51,0x45, 0xe7,0x62,0x92,0x6b,
				0x73,0xac,0x14,0x7a, 0x1e,0x86,0x31,0xd1,
				0xcc,0xd0,0x85,0x67, 0xcf,0x67,0x7c,0x72,
			},
		},
	},

	[3] = {			/* Hash_DRBG.pdf, p. 215 */
		.personalization = &kat_personalization,
		.additional = kat_additional,
		.reseed = false,
		.C = {		/* p. 220 */
			0x44,0x74,0x8a,0x78, 0xb1,0x6e,0x75,0x55,
			0x9f,0x88,0x1d,0x51, 0xc1,0x5d,0xfe,0x6c,
			0x52,0xcf,0xb0,0xbb, 0x71,0x62,0x01,0x69,
			0xc7,0x93,0x34,0x27, 0x67,0xe7,0xf8,0x87,
			0x5f,0x42,0xcb,0x6a, 0x20,0xc8,0x9d,0x7c,
			0x6e,0xf3,0xdc,0x61, 0x0d,0x8f,0xf2,0x03,
			0xd6,0x76,0x6c,0xed, 0x19,0x19,0xd0,
		},
		.V = {
			[0] = {	/* p. 218 */
				0xa3,0xe9,0x4e,0x39, 0x26,0xfd,0xa1,0x69,
				0xc3,0x03,0xd6,0x64, 0x38,0x39,0x05,0xe0,
				0xd7,0x99,0x62,0xd1, 0x65,0x44,0x6d,0x63,
				0xbd,0xa6,0x54,0xd1, 0x32,0xf7,0x2d,0xb4,
				0x71,0x56,0x4b,0x45, 0x6f,0xf2,0xee,0xc8,
				0x36,0x42,0x2a,0xcc, 0x5a,0x02,0x99,0x35,
				0xa7,0x99,0x29,0x90, 0x94,0xa1,0xca,
			},
			[1] = {	/* p. 222 */
				0xe8,0x5d,0xd8,0xb1, 0xd8,0x6c,0x16,0xbf,
				0x62,0x8b,0xf3,0xb5, 0xf9,0x97,0x04,0x4d,
				0x2a,0x69,0x13,0x8c, 0xd6,0xa6,0x6f,0x8c,
				0xa8,0x7b,0x87,0x43, 0x50,0x20,0x2e,0x1d,
				0x8a,0xb0,0xb5,0xad, 0x47,0xac,0xc2,0x75,
				0x40,0x28,0x9f,0xe3, 0xa8,0xe3,0x1f,0x7b,
				0x56,0x58,0xdd,0xd1, 0x96,0x94,0x89,
			},
			[2] = {	/* p. 225 */
				0x2c,0xd2,0x63,0x2a, 0x89,0xda,0x8c,0x15,
				0x02,0x14,0x11,0x07, 0xba,0xf5,0x02,0xb9,
				0x7d,0x38,0xc4,0x48, 0x48,0x08,0x71,0xb2,
				0x77,0xae,0xc7,0xff, 0x8d,0xa2,0x3c,0x71,
				0xef,0xf5,0x9d,0xc2, 0x4e,0x5e,0x4c,0x7f,
				0x58,0x47,0xb0,0xc1, 0x2f,0x6a,0x59,0x9e,
				0x6b,0x2e,0xda,0xc0, 0x30,0x6b,0xcd,
			},
		},
		.rnd_val = {	/* p. 222 */
			[0] = {
				0xe0,0xb9,0x7c,0x82, 0x12,0x68,0xfd,0x3b,
				0xb2,0xca,0xbf,0xd1, 0xf9,0x54,0x84,0x78,
				0xae,0x8a,0x60,0x41, 0x7f,0x7b,0x09,0x4a,
				0x26,0x13,0x95,0x46, 0x06,0x2b,0x52,0x1c,
				0xfd,0x33,0xe4,0xe3, 0x9b,0x9d,0xcd,0x0a,
				0x3d,0xa1,0x52,0x09, 0xc7,0x2a,0xdb,0xe5,
				0x8c,0x20,0xab,0x34, 0x07,0x02,0x69,0x51,
				0x29,0x7a,0xd2,0x54, 0x30,0x75,0x53,0xa5,
			},
			[1] = {	/* p. 225 */
				0xc1,0xac,0xd3,0xad, 0xa4,0xc8,0xc4,0x95,
				0xbf,0x17,0x9d,0xb5, 0x98,0x22,0xc3,0x51,
				0xbc,0x47,0x9a,0xbe, 0x4e,0xb2,0x8f,0x84,
				0x39,0x57,0xb1,0x1e, 0x3c,0x2b,0xc0,0x48,
				0x83,0x96,0x42,0x97, 0x97,0x5b,0xd7,0x2d,
				0x10,0x24,0xab,0xcf, 0x6f,0x66,0x15,0xd7,
				0xf5,0xb4,0xfd,0x1e, 0x40,0xa6,0x4e,0xeb,
				0x45,0xba,0x21,0x81, 0xb8,0x39,0x37,0xed,
			},
		},
	},

	[4] = {			/* Hash_DRBG.pdf, p. 225 */
		.personalization = &kat_zero,
		.additional = kat_no_additional,
		.reseed = true,
		.C = {		/* p. 229 */
			0xe1,0x5d,0xe4,0xa8, 0xe3,0xb1,0x41,0x9b,
			0x61,0xd5,0x34,0xf1, 0x5d,0xbd,0x31,0xee,
			0x19,0xec,0x59,0x5f, 0x8b,0x98,0x11,0x1a,
			0x94,0xf5,0x22,0x37, 0xad,0x5d,0x66,0xf0,
			0xcf,0xaa,0xfd,0xdc, 0x90,0x19,0x59,0x02,
			0xe9,0x79,0xf7,0x9b, 0x65,0x35,0x7f,0xea,
			0x85,0x99,0x8e,0x4e, 0x37,0xd2,0xc1,
		},
		.V = {
			[0] = {	/* p. 227 */
				0xab,0x41,0xcd,0xe4, 0x37,0xab,0x8b,0x09,
				0x1c,0xa7,0xc5,0x75, 0x5d,0x10,0xf0,0x11,
				0x0c,0x1d,0xbd,0x46, 0x2f,0x22,0x6c,0xfd,
				0xab,0xfb,0xb0,0x4a, 0x8b,0xcd,0xef,0x95,
				0x16,0x7d,0x84,0xaf, 0x64,0x12,0x8c,0x0d,
				0x71,0xf4,0xd5,0xb8, 0xc0,0xed,0xfb,0xbe,
				0x3d,0xf4,0x04,0x48, 0xd2,0xd8,0xe1,
			},
			[1] = {	/* p. 234 */
				0x23,0x97,0x6c,0x61, 0x63,0xd7,0xe2,0x4a,
				0x1a,0x03,0x8f,0x2b, 0x2b,0x64,0x67,0x97,
				0x50,0xca,0x9e,0xd8, 0xd1,0x40,0x69,0x8d,
				0x64,0x22,0x39,0x7b, 0x02,0x96,0x9e,0x6e,
				0xcd,0xd2,0x9d,0xac, 0xc5,0x76,0x7e,0x2c,
				0xc2,0xd0,0xa1,0x56, 0xc8,0x7a,0xd0,0xb3,
				0x57,0x89,0x05,0x07, 0xe0,0x37,0x77,
			},
			[2] = {	/* p. 239 */
				0x92,0xfb,0x0e,0x48, 0x0e,0x86,0x99,0x13,
				0xc7,0xad,0x45,0xc7, 0xe3,0xfd,0x46,0x10,
				0x17,0xe5,0xa6,0xb7, 0x70,0xf3,0x3b,0x31,
				0x3c,0x38,0x83,0xf1, 0xcc,0x56,0x71,0x89,
				0x45,0x21,0xf5,0xed, 0xe6,0x2e,0xaa,0xb0,
				0x83,0xb1,0x41,0xa7, 0x5b,0x5c,0xc0,0x22,
				0x60,0x5a,0x8a,0x3d, 0xc7,0x1b,0xa7,
			},
		},
		.rnd_val = {
			[0] = {	/* p. 234 */
				0x92,0x27,0x55,0x23, 0xc7,0x0e,0x56,0x7b,
				0xcf,0x9b,0x35,0xec, 0x50,0xb9,0x33,0xf8,
				0x12,0x61,0x6d,0xf5, 0x86,0xb7,0xf7,0x2e,
				0xe1,0xbc,0x77,0x35, 0xa5,0xc2,0x65,0x43,
				0x73,0xcb,0xbc,0x72, 0x31,0x6d,0xff,0x84,
				0x20,0xa3,0x3b,0xf0, 0x2b,0x97,0xac,0x8d,
				0x19,0x52,0x58,0x3f, 0x27,0x0a,0xcd,0x70,
				0x05,0xcc,0x02,0x7f, 0x4c,0xf1,0x18,0x7e,
			},
			[1] = {	/* p. 239 */
				0x68,0x1a,0x46,0xb2, 0xaa,0x86,0x94,0xa0,
				0xfe,0x4d,0xee,0xa7, 0x20,0x92,0x7a,0x84,
				0xea,0xaa,0x98,0x5e, 0x59,0xc1,0x9f,0x8b,
				0xe0,0x98,0x4d,0x8c, 0xbe,0xf8,0xc6,0x9b,
				0x75,0x41,0x67,0x64, 0x19,0x46,0xe0,0x40,
				0xee,0x20,0x43,0xe1, 0xcc,0xb2,0x9d,0xcf,
				0x06,0x3c,0x0a,0x50, 0x83,0x0e,0x42,0x8e,
				0x6d,0xca,0x26,0x2e, 0xcd,0x77,0xc5,0x42,
			},
		},
	},

	[5] = {			/* Hash_DRBG.pdf, p. 239 */
		.personalization = &kat_zero,
		.additional = kat_additional,
		.reseed = true,
		.C = {		/* p. 243 */
			0xe1,0x5d,0xe4,0xa8, 0xe3,0xb1,0x41,0x9b,
			0x61,0xd5,0x34,0xf1, 0x5d,0xbd,0x31,0xee,
			0x19,0xec,0x59,0x5f, 0x8b,0x98,0x11,0x1a,
			0x94,0xf5,0x22,0x37, 0xad,0x5d,0x66,0xf0,
			0xcf,0xaa,0xfd,0xdc, 0x90,0x19,0x59,0x02,
			0xe9,0x79,0xf7,0x9b, 0x65,0x35,0x7f,0xea,
			0x85,0x99,0x8e,0x4e, 0x37,0xd2,0xc1,
		},
		.V = {
			[0] = {	/* p. 242 */
				0xab,0x41,0xcd,0xe4, 0x37,0xab,0x8b,0x09,
				0x1c,0xa7,0xc5,0x75, 0x5d,0x10,0xf0,0x11,
				0x0c,0x1d,0xbd,0x46, 0x2f,0x22,0x6c,0xfd,
				0xab,0xfb,0xb0,0x4a, 0x8b,0xcd,0xef,0x95,
				0x16,0x7d,0x84,0xaf, 0x64,0x12,0x8c,0x0d,
				0x71,0xf4,0xd5,0xb8, 0xc0,0xed,0xfb,0xbe,
				0x3d,0xf4,0x04,0x48, 0xd2,0xd8,0xe1,
			},
			[1] = {	/* p. 249 */
				0xb3,0x74,0x95,0x46, 0x81,0xcf,0xc9,0x5b,
				0x8d,0xb8,0x39,0x52, 0x8c,0x71,0x08,0x83,
				0x5e,0xb4,0xf3,0x0a, 0xd9,0x1c,0xbe,0x9e,
				0xa0,0xd5,0x45,0xcc, 0xfd,0x18,0x13,0x2a,
				0xf1,0xd3,0x76,0x8f, 0x47,0x02,0x77,0x2b,
				0x69,0x15,0x9f,0x2c, 0xc0,0x7f,0x48,0x74,
				0x1e,0xb5,0xb2,0xb1, 0x22,0x11,0x25,
			},
			[2] = {	/* p. 254 */
				0xbf,0xe3,0xd6,0x81, 0xa2,0x0f,0xbe,0x39,
				0x03,0x8f,0x4d,0x66, 0x77,0x7c,0x1b,0xe5,
				0x79,0xee,0xb4,0x85, 0x7b,0x42,0xf2,0x1c,
				0x3f,0x59,0x8b,0x59, 0x62,0xb7,0xaa,0x48,
				0x0e,0xa5,0x65,0xfe, 0xea,0xbd,0xfb,0xd6,
				0xa7,0xec,0xcb,0x96, 0x02,0xc1,0x4b,0xfa,
				0x30,0xf0,0xf9,0x81, 0x90,0x0c,0xd0,
			},
		},
		.rnd_val = {
			[0] = {	/* p. 249 */
				0x11,0x60,0x1b,0x72, 0xca,0x60,0x89,0x73,
				0x6b,0x20,0x47,0x44, 0xb2,0x9d,0xa1,0xaa,
				0xaf,0xba,0xca,0xa5, 0x28,0x8f,0x06,0xbe,
				0x48,0x45,0x69,0xcc, 0xed,0xbe,0xce,0x03,
				0xe8,0x22,0xea,0xa5, 0xb1,0x4f,0x0e,0x04,
				0x94,0x8c,0x05,0xcd, 0x3c,0xc2,0xe2,0x88,
				0x9a,0x89,0xfa,0x03, 0xd6,0x5d,0x4d,0x74,
				0xac,0x50,0xff,0x6b, 0xd8,0x56,0xe5,0x79,
			},
			[1] = {	/* p. 255 */
				0x05,0x5b,0xc1,0x28, 0xcc,0x2d,0x0e,0x25,
				0x0f,0x47,0xe4,0xe4, 0xf5,0x82,0x37,0x5d,
				0xe3,0xee,0x5e,0x9f, 0xe8,0x31,0x68,0x74,
				0x97,0xe5,0xaf,0x1e, 0x7c,0xb6,0x9e,0xfd,
				0xeb,0xd2,0xfd,0x31, 0xc7,0xce,0x2b,0xba,
				0x0d,0xbc,0x6c,0x74, 0xc8,0xa2,0x0a,0x7d,
				0x72,0xf6,0x0e,0x6d, 0x9f,0x63,0xed,0x50,
				0x9e,0x96,0x3e,0x54, 0xa6,0x9e,0x90,0x48,
			},
		},
	},

	[6] = {			/* Hash_DRBG.pdf, p. 255 */
		.personalization = &kat_personalization,
		.additional = kat_no_additional,
		.reseed = true,
		.C = {		/* p. 259 */
			0x44,0x74,0x8a,0x78, 0xb1,0x6e,0x75,0x55,
			0x9f,0x88,0x1d,0x51, 0xc1,0x5d,0xfe,0x6c,
			0x52,0xcf,0xb0,0xbb, 0x71,0x62,0x01,0x69,
			0xc7,0x93,0x34,0x27, 0x67,0xe7,0xf8,0x87,
			0x5f,0x42,0xcb,0x6a, 0x20,0xc8,0x9d,0x7c,
			0x6e,0xf3,0xdc,0x61, 0x0d,0x8f,0xf2,0x03,
			0xd6,0x76,0x6c,0xed, 0x19,0x19,0xd0,
		},
		.V = {
			[0] = {	/* p. 257 */
				0xa3,0xe9,0x4e,0x39, 0x26,0xfd,0xa1,0x69,
				0xc3,0x03,0xd6,0x64, 0x38,0x39,0x05,0xe0,
				0xd7,0x99,0x62,0xd1, 0x65,0x44,0x6d,0x63,
				0xbd,0xa6,0x54,0xd1, 0x32,0xf7,0x2d,0xb4,
				0x71,0x56,0x4b,0x45, 0x6f,0xf2,0xee,0xc8,
				0x36,0x42,0x2a,0xcc, 0x5a,0x02,0x99,0x35,
				0xa7,0x99,0x29,0x90, 0x94,0xa1,0xca,
			},
			[1] = {	/* p. 264 */
				0xaa,0x11,0x1b,0x0e, 0xd5,0x6c,0xf4,0xa6,
				0xcc,0xe4,0xad,0xe7, 0xf1,0x1b,0x06,0x10,
				0x45,0xbf,0x10,0x92, 0xcb,0xb3,0x8f,0xf3,
				0x23,0x95,0xea,0x62, 0xd2,0x6b,0x27,0xc8,
				0x86,0x89,0x45,0xc5, 0x93,0xba,0x70,0xc3,
				0x84,0xad,0xad,0x45, 0x77,0x1c,0x93,0xb0,
				0x9c,0x27,0x69,0x07, 0x52,0xd1,0xd8,
			},
			[2] = {	/* p. 269 */
				0x5f,0x0f,0xd4,0x0c, 0x8c,0x82,0xef,0x41,
				0x03,0x14,0xb8,0x30, 0xc2,0x0f,0xcc,0xea,
				0x71,0x59,0x18,0x9a, 0xea,0x13,0xe8,0x48,
				0x75,0x68,0x68,0x18, 0xcd,0x4f,0x12,0xb9,
				0xde,0xa8,0x82,0x58, 0x16,0xa4,0x13,0xa2,
				0x95,0x72,0x5e,0xb3, 0x3e,0x33,0xb9,0xad,
				0xfe,0xe0,0xb1,0xc2, 0x34,0x0a,0xe0,
			},
		},
		.rnd_val = {
			[0] = {	/* p. 264 */
				0x7a,0x33,0xd3,0x90, 0x33,0xf8,0x60,0x58,
				0x9f,0x37,0x5e,0x73, 0x35,0x30,0x75,0x52,
				0x96,0x58,0xbb,0xed, 0x99,0xc8,0xa0,0xef,
				0x5e,0x28,0xb3,0x51, 0xb2,0xdf,0x33,0x58,
				0xb3,0xd8,0x9b,0xac, 0x72,0x25,0xdf,0x9e,
				0x3b,0xcd,0x08,0x36, 0xb9,0x9b,0x5d,0xbf,
				0x36,0x3a,0x17,0x0c, 0x7b,0xb9,0xbe,0x41,
				0xa4,0xaa,0x97,0x44, 0x5e,0xce,0xe4,0x1e,
			},
			[1] = {	/* p. 269 */
				0x04,0x1a,0xbd,0x94, 0x07,0x9a,0x05,0x71,
				0x88,0x5f,0x16,0x65, 0x94,0x4e,0x0e,0x7f,
				0x1b,0xfa,0xcd,0xea, 0xea,0xe9,0xd4,0x4e,
				0xed,0xc1,0x1f,0xad, 0xd8,0x4c,0x34,0xc7,
				0xca,0xa7,0x3d,0x09, 0xa0,0x19,0x31,0x93,
				0xfa,0x40,0xa1,0x9f, 0x64,0x4f,0x04,0x8d,
				0x2a,0x54,0x17,0x04, 0x25,0x53,0xdf,0x52,
				0x51,0x74,0x1b,0x40, 0xea,0xcf,0xeb,0x98,
			},
		},
	},

	[7] = {			/* Hash_DRBG.pdf, p. 269 */
		.personalization = &kat_personalization,
		.additional = kat_additional,
		.reseed = true,
		.C = {		/* p. 274 */
			0x44,0x74,0x8a,0x78, 0xb1,0x6e,0x75,0x55,
			0x9f,0x88,0x1d,0x51, 0xc1,0x5d,0xfe,0x6c,
			0x52,0xcf,0xb0,0xbb, 0x71,0x62,0x01,0x69,
			0xc7,0x93,0x34,0x27, 0x67,0xe7,0xf8,0x87,
			0x5f,0x42,0xcb,0x6a, 0x20,0xc8,0x9d,0x7c,
			0x6e,0xf3,0xdc,0x61, 0x0d,0x8f,0xf2,0x03,
			0xd6,0x76,0x6c,0xed, 0x19,0x19,0xd0,
		},
		.V = {
			[0] = {	/* p. 272 */
				0xa3,0xe9,0x4e,0x39, 0x26,0xfd,0xa1,0x69,
				0xc3,0x03,0xd6,0x64, 0x38,0x39,0x05,0xe0,
				0xd7,0x99,0x62,0xd1, 0x65,0x44,0x6d,0x63,
				0xbd,0xa6,0x54,0xd1, 0x32,0xf7,0x2d,0xb4,
				0x71,0x56,0x4b,0x45, 0x6f,0xf2,0xee,0xc8,
				0x36,0x42,0x2a,0xcc, 0x5a,0x02,0x99,0x35,
				0xa7,0x99,0x29,0x90, 0x94,0xa1,0xca,
			},
			[1] = {	/* p. 279 */
				0xaa,0xf6,0xb9,0x9b, 0x7f,0x84,0xb0,0x36,
				0xe1,0xcc,0xbc,0x9d, 0x57,0x3a,0x36,0xb8,
				0xbd,0xd4,0x7c,0x35, 0x8b,0xb5,0xf3,0xc1,
				0xd6,0xe7,0x90,0x3a, 0xaa,0x29,0xf1,0xc8,
				0x7a,0xe6,0x66,0xb8, 0x86,0x93,0xbe,0xf4,
				0x6c,0x51,0xc2,0x4c, 0x47,0xbe,0xfe,0x4b,
				0x35,0x75,0x4d,0xcb, 0xfa,0x1e,0x7d,
			},
			[2] = {	/* p. 285 */
				0x0c,0x75,0x77,0x4d, 0x61,0x02,0x69,0xad,
				0x5b,0xb4,0xab,0xbb, 0x14,0x83,0x23,0xc9,
				0x78,0x9f,0x8f,0x76, 0x25,0xcc,0x34,0x33,
				0x7c,0x03,0x47,0x2d, 0x9a,0x0c,0x4f,0xac,
				0x30,0xbe,0xd2,0xdd, 0xde,0x64,0xb8,0x7a,
				0x2c,0x70,0x67,0x52, 0xc2,0x1a,0xc0,0x11,
				0x27,0x43,0x59,0x2c, 0x4f,0xdf,0x67,
			},
		},
		.rnd_val = {	/* p. 279 */
			[0] = {
				0x88,0x97,0x32,0x97, 0x5b,0x36,0xe8,0xe2,
				0xe7,0xb7,0x40,0x50, 0xae,0xa1,0x71,0x39,
				0xda,0x2b,0x86,0x34, 0xdc,0xe2,0x13,0x3b,
				0x06,0x34,0x74,0x3f, 0x47,0x75,0x57,0xab,
				0x7b,0x84,0x4e,0xd3, 0xf2,0xa4,0x6c,0xc6,
				0x3e,0xb2,0x32,0x86, 0x46,0x4c,0x51,0xd5,
				0xd7,0x69,0x71,0xc4, 0x7b,0xc5,0xb5,0x5f,
				0xed,0x72,0xa8,0x04, 0x3c,0xbf,0x66,0x4f,
			},
			[1] = {
				0xbf,0x49,0xb8,0x89, 0xba,0x98,0x4d,0x34,
				0x63,0x87,0xe8,0x64, 0x7e,0x98,0xbb,0x99,
				0xcd,0x41,0xa3,0x2f, 0xbe,0xc1,0xfc,0xb3,
				0xb6,0xa1,0xb7,0xd9, 0x93,0x2b,0xa7,0xe1,
				0x1e,0xe6,0xbb,0xd9, 0x24,0x40,0x5a,0x2c,
				0x7f,0xca,0x89,0x0a, 0x5e,0x9a,0x8d,0xea,
				0x66,0xac,0x0c,0xac, 0xa0,0xca,0x7b,0xc1,
				0x8d,0x74,0xfb,0xc0, 0x2a,0x11,0xe4,0x53,
			},
		},
	},
};

#ifdef NIST_HASH_DRBG_DEBUG
#define	DPRINTF(fmt, ...)						      \
	printf("%s:%d: " fmt, __func__, __LINE__, ##__VA_ARGS__)
#define	DUSE(v)
#else
#define	DPRINTF(fmt, ...)
#define	DUSE(v)			(void)(v)
#endif

#define	CHECK(i, name, actual, expected, n) do				      \
{									      \
	CTASSERT(sizeof(actual) == (n));				      \
	ok &= check(i, name, actual, expected, (n));			      \
} while (0)

static bool
check(unsigned katno, const char *name, const uint8_t *actual,
    const uint8_t *expected, size_t n)
{
	bool ok = true;
	size_t i;

	DUSE(katno);
	DUSE(name);

	for (i = 0; i < n; i++) {
		if (actual[i] != expected[i]) {
			DPRINTF("KAT %u %s[%zu] = %02x, expected %02x\n",
			    katno, name, i, actual[i], expected[i]);
			ok = false;
		}
	}

	return ok;
}

#ifdef NIST_HASH_DRBG_MAIN
int
main(void)
{
	int ret;

	ret = nist_hash_drbg_initialize();

	fflush(stdout);
	return ret || ferror(stdout);
}
#endif

int
nist_hash_drbg_initialize(void)
{
	const unsigned truncation[] = { 0, 1, 32, 63 };
	bool ok = true;
	size_t i, j;

	for (i = 0; i < arraycount(kat); i++) {
		for (j = 0; j < arraycount(truncation); j++) {
			const unsigned trunc = truncation[j];
			struct nist_hash_drbg drbg, *D = &drbg;
			uint8_t rnd_val[64];
			unsigned reseed_counter;

			nist_hash_drbg_instantiate(D,
			    kat_entropy[0], sizeof kat_entropy[0],
			    kat_nonce, sizeof kat_nonce,
			    kat[i].personalization->hv_base,
			    kat[i].personalization->hv_len);
			reseed_counter = 1;
			CHECK(i, "C", D->C, kat[i].C, SEEDLEN_BYTES);
			CHECK(i, "V[0]", D->V, kat[i].V[0], SEEDLEN_BYTES);
			if (D->reseed_counter != reseed_counter) {
				DPRINTF("bad reseed counter: %u, expected %u",
				    D->reseed_counter, reseed_counter);
				ok = false;
			}

			if (kat[i].reseed) {
				nist_hash_drbg_reseed(D,
				    kat_entropy[1], sizeof kat_entropy[1],
				    kat[i].additional[0]->hv_base,
				    kat[i].additional[0]->hv_len);
			}

			nist_hash_drbg_generate(D, rnd_val,
			    sizeof(rnd_val) - trunc,
			    kat[i].reseed ? 0 : kat[i].additional[0]->hv_base,
			    kat[i].reseed ? 0 : kat[i].additional[0]->hv_len);
			reseed_counter++;
			CHECK(i, "V[1]", D->V, kat[i].V[1], SEEDLEN_BYTES);
			ASSERT(sizeof(kat[i].rnd_val[0]) - trunc <=
			    sizeof rnd_val);
			check(i, "rnd_val[0]", rnd_val, kat[i].rnd_val[0],
			    sizeof(kat[i].rnd_val[0]) - trunc);
			if (D->reseed_counter != reseed_counter) {
				DPRINTF("bad reseed counter: %u, expected %u",
				    D->reseed_counter, reseed_counter);
				ok = false;
			}

			if (kat[i].reseed) {
				nist_hash_drbg_reseed(D,
				    kat_entropy[2], sizeof kat_entropy[2],
				    kat[i].additional[1]->hv_base,
				    kat[i].additional[1]->hv_len);
				reseed_counter = 1;
			}

			nist_hash_drbg_generate(D, rnd_val,
			    sizeof(rnd_val) - trunc,
			    kat[i].reseed ? 0 : kat[i].additional[1]->hv_base,
			    kat[i].reseed ? 0 : kat[i].additional[1]->hv_len);
			reseed_counter++;
			CHECK(i, "V[2]", D->V, kat[i].V[2], SEEDLEN_BYTES);
			ASSERT(sizeof(kat[i].rnd_val[1]) - trunc <=
			    sizeof rnd_val);
			check(i, "rnd_val[1]", rnd_val, kat[i].rnd_val[1],
			    sizeof(kat[i].rnd_val[1]) - trunc);
			if (D->reseed_counter != reseed_counter) {
				DPRINTF("bad reseed counter: %u, expected %u",
				    D->reseed_counter, reseed_counter);
				ok = false;
			}

			nist_hash_drbg_destroy(D);
		}
	}

	if (!ok)
		return 1;
	return 0;
}
