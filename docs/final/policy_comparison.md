| policy | mean regret | median regret | worst regret (configuration) | within 5% of the best |
|---|---:|---:|---|---:|
| original (plain postgres_fdw) | 231.20 | 24.81 | 2,609.97 (E5 n=100 10Mbit) | 0 of 38 |
| always index | 1.03 | 1.00 | 1.22 (E1 n=150000 narrow) | 32 of 38 |
| always filter | 8.08 | 2.49 | 50.16 (E2 n=1000 s=3M) | 11 of 38 |
| auto (our cost model) | 1.08 | 1.00 | 2.51 (E3 n=50000 match=0.1) | 34 of 38 |
