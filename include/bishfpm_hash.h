#ifndef BISHFPM_HASH_H
#define BISHFPM_HASH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint32_t bishfpm_load32le(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static inline uint64_t bishfpm_load64le(const uint8_t *p) {
  return (uint64_t)bishfpm_load32le(p) | (uint64_t)bishfpm_load32le(p + 4)
                                             << 32;
}

/* rotate left helper */
static inline uint32_t bishfpm_rotl32(uint32_t x, int8_t r) {
  unsigned n = (unsigned)r & 31;
  return (x << n) | (x >> ((32 - n) & 31));
}

static inline uint64_t bishfpm_rotl64(uint64_t x, int8_t r) {
  unsigned n = (unsigned)r & 63;
  return (x << n) | (x >> ((64 - n) & 63));
}

/* 32-bit MurmurHash3 */
static inline uint32_t bishfpm_murmur3_32(const void *key, size_t len,
                                          uint32_t seed) {
  const uint8_t *data = (const uint8_t *)key;
  const size_t nblocks = len / 4;
  uint32_t h1 = seed;

  const uint32_t c1 = 0xcc9e2d51;
  const uint32_t c2 = 0x1b873593;

  /* body */
  for (size_t i = 0; i < nblocks; i++) {
    uint32_t k1;
    k1 = bishfpm_load32le(data + i * 4);

    k1 *= c1;
    k1 = bishfpm_rotl32(k1, 15);
    k1 *= c2;

    h1 ^= k1;
    h1 = bishfpm_rotl32(h1, 13);
    h1 = h1 * 5 + 0xe6546b64;
  }

  /* tail */
  const uint8_t *tail = (const uint8_t *)(data + nblocks * 4);
  uint32_t k1 = 0;

  switch (len & 3) {
  case 3:
    k1 ^= (uint32_t)tail[2] << 16;
    /* fallthrough */
  case 2:
    k1 ^= (uint32_t)tail[1] << 8;
    /* fallthrough */
  case 1:
    k1 ^= (uint32_t)tail[0];
    k1 *= c1;
    k1 = bishfpm_rotl32(k1, 15);
    k1 *= c2;
    h1 ^= k1;
    break;
  default:
    break;
  }

  /* finalization */
  h1 ^= (uint32_t)len;
  h1 ^= h1 >> 16;
  h1 *= 0x85ebca6b;
  h1 ^= h1 >> 13;
  h1 *= 0xc2b2ae35;
  h1 ^= h1 >> 16;

  return h1;
}

/* 64-bit hash derived from MurmurHash3 x64 */
static inline uint64_t bishfpm_murmur3_64(const void *key, size_t len,
                                          uint64_t seed) {
  const uint8_t *data = (const uint8_t *)key;
  const size_t nblocks = len / 16;

  uint64_t h1 = seed;
  uint64_t h2 = seed;

  const uint64_t c1 = 0x87c37b91114253d5ULL;
  const uint64_t c2 = 0x4cf5ad432745937fULL;

  /* body */
  for (size_t i = 0; i < nblocks; i++) {
    uint64_t k1, k2;
    k1 = bishfpm_load64le(data + i * 16);
    k2 = bishfpm_load64le(data + i * 16 + 8);

    k1 *= c1;
    k1 = bishfpm_rotl64(k1, 31);
    k1 *= c2;
    h1 ^= k1;

    h1 = bishfpm_rotl64(h1, 27);
    h1 += h2;
    h1 = h1 * 5 + 0x52dce729;

    k2 *= c2;
    k2 = bishfpm_rotl64(k2, 33);
    k2 *= c1;
    h2 ^= k2;

    h2 = bishfpm_rotl64(h2, 31);
    h2 += h1;
    h2 = h2 * 5 + 0x38495ab5;
  }

  /* tail */
  const uint8_t *tail = (const uint8_t *)(data + nblocks * 16);
  uint64_t k1 = 0;
  uint64_t k2 = 0;

  switch (len & 15) {
  case 15:
    k2 ^= (uint64_t)tail[14] << 48;
    /* fallthrough */
  case 14:
    k2 ^= (uint64_t)tail[13] << 40;
    /* fallthrough */
  case 13:
    k2 ^= (uint64_t)tail[12] << 32;
    /* fallthrough */
  case 12:
    k2 ^= (uint64_t)tail[11] << 24;
    /* fallthrough */
  case 11:
    k2 ^= (uint64_t)tail[10] << 16;
    /* fallthrough */
  case 10:
    k2 ^= (uint64_t)tail[9] << 8;
    /* fallthrough */
  case 9:
    k2 ^= (uint64_t)tail[8];
    k2 *= c2;
    k2 = bishfpm_rotl64(k2, 33);
    k2 *= c1;
    h2 ^= k2;
    /* fallthrough */
  case 8:
    k1 ^= (uint64_t)tail[7] << 56;
    /* fallthrough */
  case 7:
    k1 ^= (uint64_t)tail[6] << 48;
    /* fallthrough */
  case 6:
    k1 ^= (uint64_t)tail[5] << 40;
    /* fallthrough */
  case 5:
    k1 ^= (uint64_t)tail[4] << 32;
    /* fallthrough */
  case 4:
    k1 ^= (uint64_t)tail[3] << 24;
    /* fallthrough */
  case 3:
    k1 ^= (uint64_t)tail[2] << 16;
    /* fallthrough */
  case 2:
    k1 ^= (uint64_t)tail[1] << 8;
    /* fallthrough */
  case 1:
    k1 ^= (uint64_t)tail[0];
    k1 *= c1;
    k1 = bishfpm_rotl64(k1, 31);
    k1 *= c2;
    h1 ^= k1;
    break;
  default:
    break;
  }

  /* finalization */
  h1 ^= (uint64_t)len;
  h2 ^= (uint64_t)len;

  h1 += h2;

  h1 ^= h1 >> 33;
  h1 *= 0xff51afd7ed558ccdULL;
  h1 ^= h1 >> 33;
  h1 *= 0xc4ceb9fe1a85ec53ULL;
  h1 ^= h1 >> 33;

  return h1;
}

/* string hash helpers */
static inline uint64_t bishfpm_hash_str(const char *str, uint64_t seed) {
  return bishfpm_murmur3_64(str, strlen(str), seed);
}

static inline uint32_t bishfpm_hash_str32(const char *str, uint32_t seed) {
  return bishfpm_murmur3_32(str, strlen(str), seed);
}

/* fast pseudo-random generator state */
static inline uint64_t bishfpm_splitmix64(uint64_t *state) {
  uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

#ifdef __cplusplus
}
#endif

#endif /* BISHFPM_HASH_H */
