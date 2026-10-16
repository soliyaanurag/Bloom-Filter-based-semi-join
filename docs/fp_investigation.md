<!-- Presentation material: why our Bloom filter had too many false positives for small filters, and how enhanced double hashing fixed most of it. -->

# False-positive investigation: classic vs. enhanced double hashing

## 1. The problem

In the Batch 4 benchmark, `bloom_dynamic` picks a large b (bits per key) for small n, so theory predicts almost no false positives. The measurements disagreed:

| n | b | predicted false positives | measured |
|---:|---:|---:|---:|
| 100 | 26.4 | about 3 | **215** |
| 1,000 | 21.6 | about 31 | **78** |
| 10,000 | 16.8 | about 313 | 335 (matches) |

The prediction uses the standard Bloom filter formula (Broder & Mitzenmacher 2004):

```
predicted_fp = (1 - e^(-k·n/m))^k        n = keys in the filter, m = bits, k = positions per key
```

The gap only shows up when the filter is **small** (m = 2,640 bits for n = 100). Our suspect was the way we turn one hash value into k bit positions.

## 2. How a key gets its k bit positions

We hash each key **once** with PostgreSQL's `hashint4extended` and split the 64-bit result into two 32-bit numbers: `h1` (low half) and `h2` (high half, forced odd). The k positions are then computed from h1 and h2.

### Classic double hashing (our version 1)

```
position_i = (h1 + i · h2) mod m        for i = 0 .. k-1
```

The positions are **evenly spaced**: start at h1, then always take the same step h2. This causes two kinds of trouble when m is small:

- **Repeats.** If the step shares a factor with m, the walk comes back to a bit it already used.
- **Shifted copies.** Two keys with the same step and nearby starting points walk along the same evenly spaced bits, so they share most of their k bits.

Example with m = 20, k = 5, step h2 = 5:

| key | h1 | positions (classic) | set of bits |
|---|---:|---|---|
| A (in the filter) | 3 | 3, 8, 13, 18, **3** | {3, 8, 13, 18}: only 4 different bits |
| B (not in the filter) | 8 | 8, 13, 18, 3, **8** | {3, 8, 13, 18}: **the same bits as A** |

B is a false positive **every time** A is in the filter.

### Enhanced double hashing (our version 2, now the only algorithm)

This is the method of Dillinger & Manolios (FMCAD 2004). PostgreSQL uses it too, in `src/backend/lib/bloomfilter.c` (function `k_hashes`). In simple words: **take a step, then make the next step a little bigger**.

```
x = h1 mod m             (where we are)
y = h2 mod m             (how far the next step goes)
position_0 = x
for i = 1 .. k-1:
    x = (x + y) mod m    take the step
    y = (y + i) mod m    the next step is i bigger
    position_i = x
```

The steps grow by 1, 2, 3, ..., so the positions are **no longer evenly spaced**, and a shifted key no longer follows the same path. The same example (m = 20, k = 5, h2 = 5):

| key | h1 | positions (enhanced) | bits shared with A |
|---|---:|---|---|
| A | 3 | 3, 8, 14, 2, 13 | |
| B | 8 | 8, 13, 19, 7, 18 | only 8 and 13 |

It costs nothing extra: still **one hash call per key**, and k additions instead of k multiplications. PostgreSQL computes "mod m" with a bit mask because its m is always a power of two. Our m can be any number, so we use `%`.

## 3. Results: version 1 vs. version 2

False positives among **1,000,000** keys that are not in the filter. m = ceil(n·b), k = round(0.693·b), so everything except the position formula is identical. Filter keys are 1..n, and the tested keys are n+1 .. n+1,000,000.

| n | b | m | k | predicted (formula) | measured v1 (classic) | measured v2 (enhanced) |
|---:|---:|---:|---:|---:|---:|---:|
| 100 | 26.4 | 2,640 | 18 | 3.1 | 170 | **29** |
| 1,000 | 21.6 | 21,600 | 15 | 31.1 | 51 | **29** |
| 10,000 | 16.8 | 168,000 | 12 | 313.0 | 342 | **355** |
| 10,000 | 10.0 | 100,000 | 7 | 8,193.7 | 7,906 | **8,140** |

The same check with the **real** benchmark keys (the distinct customer_ids of the first n orders, tested against all other customer ids). v1 reproduces the Batch 4 numbers exactly (215 and 78):

| n | b | distinct keys | m | k | predicted | measured v1 | measured v2 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 26.4 | 100 | 2,640 | 18 | 3.1 | 215 | **37** |
| 1,000 | 21.6 | 999 | 21,579 | 15 | 31.1 | 78 | **27** |

And the standard false-positive experiment (`queries/fp_rate.sql`, 10,000 keys, large m) with the new code still matches the formula:

| b | m | k | measured | predicted |
|---:|---:|---:|---:|---:|
| 4 | 40,000 | 3 | 0.144824 | 0.146892 |
| 8 | 80,000 | 6 | 0.021121 | 0.021577 |
| 12 | 120,000 | 8 | 0.003147 | 0.003142 |
| 16 | 160,000 | 11 | 0.000453 | 0.000459 |
| 22 | 220,000 | 15 | 0.000023 | 0.000026 |

How to read the counts: false positives are random, so a count c varies by about ±2·√c between experiments (for example 29 ± 11, or 355 ± 38). Within that noise, v2 matches the formula at n = 1,000 and n = 10,000. The large-m differences between v1 and v2 (342 vs 355, 7,906 vs 8,140) are noise.

**Summary:** enhanced double hashing removes most of the excess. At n = 100 it goes from about 55× the prediction down to about 9× (from 69× to 12× with the real keys). At n = 1,000 it removes the excess completely.

## 4. Remaining gap at small m: a known limitation

At n = 100 (m = 2,640) we still measure about 29 false positives where the formula says 3. The cause is that **both** versions compute all k positions from just two numbers, h1 mod m and h2 mod m:

- There are only about m² such pairs. Since h2 is forced odd and our m is even here, h2 mod m is always odd, so there are only m²/2 = 3.5 million pairs.
- A non-member whose pair equals a member's pair gets **exactly the same k positions**, so it is **always** a false positive. Making k bigger does not help.
- Expected number of such keys: 1,000,000 × n / (m²/2) = 1,000,000 × 100 / 3,484,800 ≈ **29**.

We counted these keys directly from `hashint4extended` (Step 3 of `queries/fp_cases.sql`):

| n | m | non-members with exactly a member's positions | expected |
|---:|---:|---:|---:|
| 100 | 2,640 | 25 | 28.7 |
| 1,000 | 21,600 | 3 | 4.3 |

So at n = 100, 25 of the 29 false positives come from this effect, and the rest is the ~3 the formula predicts. This floor shrinks like n/m², which is why it disappears for larger filters.

This is a known property of double hashing. Kirsch & Mitzenmacher (2006) prove that a Bloom filter that derives its k positions from two hash values reaches the standard false-positive rate only **asymptotically**, as the filter grows. For small m some extra false positives remain.

**Decision.** Our false-positive model stays ONLY the standard formula. We document the small-m gap as a limitation and do not model it, because:

- it only matters when the formula predicts very few false positives (tiny filters with a large b);
- even then the cost is small: at n = 100, about 26 extra rows ≈ 440 bytes of narrow-row text. A wrong b would cost far more;
- the final join result is always exactly correct, whatever the false-positive rate.

Possible future improvements (**not done**, both would change a fixed design decision):

- not forcing h2 to be odd, which would halve the floor when m is even (we counted 13 instead of 25 at n = 100);
- taking more hash bits per key, for example a second `hashint4extended` call with another seed.

## 5. Compatibility: filter version 2

Site A builds the filter and site B tests keys against it, so both **must** compute the same positions. A filter built by the old code but read with the new positions could silently drop matching rows (false negatives), and the join result would be wrong. So the filter header's version went from 1 to 2 (`SJB_VERSION` in `sjbloom/sjb_filter.h`). A filter with any other version is refused:

```
ERROR:  sjbloom: filter has version 1, but this sjbloom understands only version 2
HINT:  Site A and site B must run the same sjbloom build. Install it on both sites and rebuild the filter.
```

## 6. How to reproduce

```bash
source setup/env.example.sh
make -C sjbloom install
psql -d site_a -c "DROP EXTENSION sjbloom CASCADE; CREATE EXTENSION sjbloom;"
psql -d site_b -c "DROP EXTENSION sjbloom CASCADE; CREATE EXTENSION sjbloom;"
psql -X -d site_a -f queries/fp_rate.sql     # standard experiment, large m
psql -X -d site_a -f queries/fp_cases.sql    # the small-m cases, real keys, and the pair count
```

The version 1 numbers come from the branch `fp-investigation` (commit `cc29986`, file `queries/fp_v1_v2.sql`). That branch had both versions side by side; master has only version 2.

## References

- P. C. Dillinger and P. Manolios. *Bloom Filters in Probabilistic Verification.* Formal Methods in Computer-Aided Design (FMCAD 2004), LNCS 3312, pp. 367–381. Introduces enhanced double hashing.
- A. Broder and M. Mitzenmacher. *Network Applications of Bloom Filters: A Survey.* Internet Mathematics 1(4), 2004, pp. 485–509. The standard false-positive formula.
- A. Kirsch and M. Mitzenmacher. *Less Hashing, Same Performance: Building a Better Bloom Filter.* European Symposium on Algorithms (ESA 2006), LNCS 4168, pp. 456–467. Double hashing reaches the standard rate asymptotically.
- PostgreSQL 18 source, `src/backend/lib/bloomfilter.c`, function `k_hashes`. Enhanced double hashing in PostgreSQL itself.
