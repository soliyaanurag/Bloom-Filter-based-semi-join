/*-------------------------------------------------------------------------
 *
 * sjb_filter.h
 *
 * Shared definitions for the sjbloom extension.
 *
 * A "filter" is a bytea value that site A builds from its join keys and
 * sends to site B. Every filter starts with the same 24-byte header
 * (SjbHeader below), followed by a payload whose meaning depends on the
 * mode:
 *
 *   SJB_MODE_BLOOM : payload = ceil(m/8) bytes of Bloom filter bits
 *   SJB_MODE_EXACT : payload = sorted, unique int4 keys    (Batch 5)
 *   SJB_MODE_PASS  : no payload, every key passes          (Batch 5)
 *
 * This header also declares the pure Bloom filter functions in bloom.c.
 *
 * Every .c file must include "postgres.h" BEFORE this file, because we use
 * PostgreSQL's integer types (uint8, uint16, uint32, uint64, int32).
 *
 *-------------------------------------------------------------------------
 */
#ifndef SJB_FILTER_H
#define SJB_FILTER_H

/* Version number stored in every filter header. */
#define SJB_VERSION 1

/* The three filter modes (stored in the "mode" field of the header). */
#define SJB_MODE_BLOOM 1
#define SJB_MODE_EXACT 2
#define SJB_MODE_PASS 3

/*
 * Seed for the hash function. Site A and site B must use the same seed,
 * so it is stored in the header of every filter.
 * UINT64CONST is a PostgreSQL macro that writes a 64-bit unsigned constant
 * in a way every C compiler accepts.
 */
#define SJB_SEED UINT64CONST(0x5EED5EED)

/* Allowed range for the Bloom filter size m (in bits): 8 bits to 64 MB. */
#define SJB_MIN_BITS 8
#define SJB_MAX_BITS (8 * 64 * 1024 * 1024)

/* Allowed range for k, the number of hash positions per key. */
#define SJB_MIN_K 1
#define SJB_MAX_K 32

/*
 * The 24-byte header at the start of every filter.
 *
 *   offset  size  field
 *   0       1     version  (always SJB_VERSION)
 *   1       1     mode     (SJB_MODE_BLOOM, SJB_MODE_EXACT or SJB_MODE_PASS)
 *   2       2     k        (number of hash positions per key)
 *   4       4     nkeys    (number of keys that were added)
 *   8       8     m        (number of bits in the Bloom filter)
 *   16      8     seed     (hash seed)
 *
 * The fields are ordered so that no padding is needed. Bytes inside a
 * bytea may not be aligned for 8-byte reads, so we always copy the header
 * in and out with memcpy instead of casting a pointer to SjbHeader *.
 */
typedef struct SjbHeader
{
	uint8		version;
	uint8		mode;
	uint16		k;
	uint32		nkeys;
	uint64		m;
	uint64		seed;
} SjbHeader;

/*
 * StaticAssertDecl is a PostgreSQL macro that checks a condition while
 * compiling. If the header is ever not exactly 24 bytes, the build fails.
 */
StaticAssertDecl(sizeof(SjbHeader) == 24, "SjbHeader must be exactly 24 bytes");

/* Bloom filter functions (bloom.c). */
extern void sjb_bloom_add(uint8 *bits, uint64 m, int k, uint64 seed, int32 key);
extern bool sjb_bloom_test(const uint8 *bits, uint64 m, int k, uint64 seed, int32 key);

#endif							/* SJB_FILTER_H */
