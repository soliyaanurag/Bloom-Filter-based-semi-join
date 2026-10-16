| configuration | original ms | index ms | filter ms | auto ms | auto chose | fastest | regret | correct choice | speedup |
|---|---:|---:|---:|---:|---|---|---:|---|---:|
| E1 n=100 narrow | 1,231.9 | 2.4 | 100.0 | 2.1 | index | index | 0.86 | yes | 590.8x |
| E1 n=1000 narrow | 1,186.6 | 7.6 | 109.4 | 7.9 | index | index | 1.04 | yes | 150.5x |
| E1 n=10000 narrow | 1,203.2 | 56.9 | 206.7 | 53.3 | index | index | 0.94 | yes | 22.6x |
| E1 n=50000 narrow | 1,576.0 | 310.1 | 296.9 | 310.7 | filter/bloom | filter | 1.05 | yes | 5.1x |
| E1 n=150000 narrow | 1,555.8 | 635.3 | 519.1 | 534.2 | filter/bloom | filter | 1.03 | yes | 2.9x |
| E1 n=100 wide | 1,289.2 | 2.1 | 98.8 | 2.5 | index | index | 1.21 | yes | 507.7x |
| E1 n=1000 wide | 1,333.5 | 7.3 | 109.3 | 7.9 | index | index | 1.08 | yes | 168.1x |
| E1 n=10000 wide | 1,358.4 | 54.0 | 198.5 | 52.8 | index | index | 0.98 | yes | 25.7x |
| E1 n=50000 wide | 1,517.3 | 363.0 | 324.7 | 333.9 | filter/bloom | filter | 1.03 | yes | 4.5x |
| E1 n=150000 wide | 1,754.5 | 686.9 | 593.6 | 578.2 | filter/bloom | filter | 0.97 | yes | 3.0x |
| E2 n=1000 s=0.1M | 110.5 | 5.5 | 13.5 | 5.1 | index | index | 0.94 | yes | 21.6x |
| E2 n=10000 s=0.1M | 120.7 | 34.6 | 38.7 | 37.4 | filter/bloom | index | 1.08 | **no** | 3.2x |
| E2 n=50000 s=0.1M | 159.5 | 126.8 | 120.8 | 118.2 | filter/bloom | filter | 0.98 | yes | 1.3x |
| E2 n=1000 s=3M | 3,561.6 | 7.2 | 358.9 | 7.5 | index | index | 1.05 | yes | 473.4x |
| E2 n=10000 s=3M | 3,805.6 | 59.4 | 469.6 | 59.5 | index | index | 1.00 | yes | 63.9x |
| E2 n=50000 s=3M | 4,199.9 | 503.5 | 688.8 | 496.4 | index | index | 0.99 | yes | 8.5x |
| E3 n=1000 match=0.5 | 1,181.6 | 4.8 | 106.3 | 4.9 | index | index | 1.03 | yes | 241.0x |
| E3 n=10000 match=0.5 | 1,227.9 | 37.0 | 175.8 | 48.7 | index | index | 1.32 | yes | 25.2x |
| E3 n=50000 match=0.5 | 1,384.7 | 150.9 | 314.9 | 274.2 | filter/bloom | index | 1.82 | **no** | 5.1x |
| E3 n=1000 match=0.1 | 1,223.3 | 5.0 | 103.7 | 3.0 | index | index | 0.59 | yes | 410.7x |
| E3 n=10000 match=0.1 | 1,197.9 | 17.3 | 148.9 | 18.5 | index | index | 1.07 | yes | 64.8x |
| E3 n=50000 match=0.1 | 1,267.3 | 79.8 | 200.5 | 189.8 | filter/bloom | index | 2.38 | **no** | 6.7x |
| E4 n=1000 skewed | 1,384.6 | 8.3 | 140.3 | 7.7 | index | index | 0.93 | yes | 179.4x |
| E4 n=10000 skewed | 1,386.3 | 46.6 | 192.0 | 45.1 | index | index | 0.97 | yes | 30.8x |
| E4 n=50000 skewed | 1,534.8 | 227.4 | 305.1 | 288.0 | filter/bloom | index | 1.27 | **no** | 5.3x |
| E5 n=100 10Mbit | 50,510.8 | 19.4 | 112.2 | 19.4 | index | index | 1.00 | yes | 2597.2x |
| E5 n=1000 10Mbit | 50,361.8 | 71.9 | 171.4 | 71.7 | index | index | 1.00 | yes | 702.7x |
| E5 n=10000 10Mbit | 50,261.8 | 610.9 | 730.8 | 606.5 | index | index | 0.99 | yes | 82.9x |
| E5 n=50000 10Mbit | 50,594.3 | 2,997.1 | 2,891.8 | 2,919.3 | filter/bloom | filter | 1.01 | yes | 17.3x |
| E5 n=150000 10Mbit | 50,730.6 | 8,346.8 | 7,907.9 | 7,923.2 | filter/bloom | filter | 1.00 | yes | 6.4x |
| E5 n=100 100Mbit | 25,593.5 | 15.3 | 110.3 | 15.8 | index | index | 1.03 | yes | 1620.1x |
| E5 n=1000 100Mbit | 25,444.5 | 41.3 | 144.6 | 40.5 | index | index | 0.98 | yes | 627.8x |
| E5 n=10000 100Mbit | 25,621.3 | 301.4 | 404.7 | 298.3 | index | index | 0.99 | yes | 85.9x |
| E5 n=50000 100Mbit | 25,547.9 | 1,560.4 | 1,530.7 | 1,512.3 | filter/exact | filter | 0.99 | yes | 16.9x |
| E5 n=150000 100Mbit | 25,874.8 | 4,128.1 | 3,967.6 | 3,925.2 | filter/exact | filter | 0.99 | yes | 6.6x |
| E6 n=10000 binary | 1,211.5 | 49.5 | 169.4 | 50.5 | index | index | 1.02 | yes | 24.0x |
| E6 n=50000 binary | 1,384.1 | 338.6 | 305.9 | 311.8 | filter/bloom | filter | 1.02 | yes | 4.4x |
| E6 n=150000 binary | 1,436.6 | 634.0 | 522.7 | 514.1 | filter/bloom | filter | 0.98 | yes | 2.8x |

E5 at 10 Mbit: auto with the network numbers measured under netem, and auto_loopback with the calibrated loopback numbers (1000 Mbit, 0.062 ms):

| configuration | fastest | auto chose | auto ms | regret | auto_loopback chose | auto_loopback ms | regret |
|---|---|---|---:|---:|---|---:|---:|
| E5 n=100 10Mbit | index | index | 19.4 | 1.00 | index | 19.6 | 1.02 |
| E5 n=1000 10Mbit | index | index | 71.7 | 1.00 | index | 71.7 | 1.00 |
| E5 n=10000 10Mbit | index | index | 606.5 | 0.99 | index | 613.8 | 1.00 |
| E5 n=50000 10Mbit | filter | filter/bloom | 2,919.3 | 1.01 | filter | 2,893.9 | 1.00 |
| E5 n=150000 10Mbit | filter | filter/bloom | 7,923.2 | 1.00 | filter | 7,921.2 | 1.00 |
