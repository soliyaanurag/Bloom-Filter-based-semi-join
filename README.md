# Cost-Based Adaptive Semi-Join Filtering for postgres_fdw

CS631 course project, IIT Bombay.

**Team:** Anurag Soliya, Aman Patel, Dhaivat Patel, Nitin Saini

## The idea

When a query at site A joins a local table R (orders) with a remote table S (customers) at site B through postgres_fdw, PostgreSQL normally pulls all of S across the network even if only a few of its rows will match. In our project, site A first collects the distinct join keys of the filtered local rows and packs them into a compact filter: a Bloom filter, an exact sorted key list, or a "pass everything" filter. Site A sends this filter to site B as a query parameter, and B returns only the rows of S whose key passes the filter (a semijoin). Site A then runs the normal join, which removes any Bloom false positives, so the answer is always exactly correct. A cost model picks the filter type and the Bloom filter size separately for each query, so we only pay for a filter when it actually saves network traffic.

See [CLAUDE.md](CLAUDE.md) for the fixed design decisions and [docs/WALKTHROUGH.md](docs/WALKTHROUGH.md) for a step-by-step explanation of what has been built so far.
