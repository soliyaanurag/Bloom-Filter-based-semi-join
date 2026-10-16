#!/usr/bin/env bash
#
# make_data.sh: Batch 8. Regenerates the data of both sites for one
# experiment, without touching anything else (extensions, server options,
# user mapping and the foreign table all stay):
#   1. data_customers.sql (site_b): customers with ids 1..s_rows
#   2. data_orders.sql    (site_a): 200,000 orders whose keys come from
#      1..(s_rows / match_fraction), uniform or skewed; then ANALYZE of
#      orders AND of the foreign table customers at site_a
#
# Usage (from the project folder):
#     source setup/env.example.sh
#     setup/make_data.sh S_ROWS MATCH_FRACTION SKEW
# for example
#     setup/make_data.sh 1000000 1.0 uniform     <- the default data
#     setup/make_data.sh 3000000 1.0 uniform
#     setup/make_data.sh 1000000 0.5 uniform
#     setup/make_data.sh 1000000 1.0 skewed
#
# "setup/make_data.sh 1000000 1.0 uniform" gives exactly the same rows as
# setup_all.sh.

# Stop at the first command that fails.
set -e

SETUP_DIR="$(cd "$(dirname "$0")" && pwd)"
PSQL="psql -X -q -v ON_ERROR_STOP=1"

if [ "$#" -ne 3 ]; then
    echo "usage: $0 S_ROWS MATCH_FRACTION uniform|skewed" >&2
    exit 1
fi

S_ROWS="$1"
MATCH_FRACTION="$2"
SKEW="$3"

# Check the arguments, so a typo cannot end up inside the SQL.
if ! [[ "$S_ROWS" =~ ^[0-9]+$ ]]; then
    echo "S_ROWS must be a whole number, got: $S_ROWS" >&2
    exit 1
fi
if ! [[ "$MATCH_FRACTION" =~ ^(0\.[0-9]+|1|1\.0+)$ ]]; then
    echo "MATCH_FRACTION must be a number in (0, 1], got: $MATCH_FRACTION" >&2
    exit 1
fi
if [ "$SKEW" = "uniform" ]; then
    SKEWED=false
elif [ "$SKEW" = "skewed" ]; then
    SKEWED=true
else
    echo "SKEW must be uniform or skewed, got: $SKEW" >&2
    exit 1
fi

echo "=== make_data: s_rows=$S_ROWS match_fraction=$MATCH_FRACTION skew=$SKEW ==="

echo "--- 1/2: customers at site_b ---"
$PSQL -v s_rows="$S_ROWS" -d site_b -f "$SETUP_DIR/data_customers.sql"

echo "--- 2/2: orders at site_a, then ANALYZE at site_a ---"
$PSQL -v s_rows="$S_ROWS" -v match_fraction="$MATCH_FRACTION" -v skewed="$SKEWED" \
      -d site_a -f "$SETUP_DIR/data_orders.sql"
