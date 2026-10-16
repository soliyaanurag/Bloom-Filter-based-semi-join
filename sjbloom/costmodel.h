/*-------------------------------------------------------------------------
 *
 * costmodel.h
 *
 * The time-based cost model of the sjbloom extension (Batch 5).
 *
 * Site A can ask site B for the matching rows of S in two ways (PATHS):
 *
 *   index path : B gets "customer_id = ANY(key list)" and does one index
 *                lookup per key.
 *   filter path: B gets "sjb_contains(filter, customer_id)" and checks
 *                ALL rows of S. The filter has one of three MODES:
 *                BLOOM, EXACT or PASS (see sjb_filter.h).
 *
 * For each path and mode we estimate the TIME the query costs, with the
 * total-time formula of Ozsu & Valduriez ("Principles of Distributed
 * Database Systems", chapter on query optimization, cost functions):
 *
 *   time = T_CPU * work  +  T_MSG * messages  +  T_TR * bytes
 *
 *   T_CPU * work     work at site B (checking rows or index lookups),
 *                    work at site A (building the filter), and the work
 *                    per returned row (text out at B, text in at A)
 *   T_MSG * messages one cost per message round trip: postgres_fdw sends
 *                    the query, then fetches fetch_size rows per FETCH
 *   T_TR * bytes     bytes moved / bandwidth
 *
 * The cheapest one wins.
 *
 * costmodel.c only does arithmetic on doubles. It knows nothing about SQL
 * values; sjbloom.c turns SQL arguments into an SjbCostInputs and calls it.
 *
 * Every .c file must include "postgres.h" BEFORE this file.
 *
 *-------------------------------------------------------------------------
 */
#ifndef SJB_COSTMODEL_H
#define SJB_COSTMODEL_H

/*
 * Default settings. The SQL functions use the same numbers as defaults.
 *
 * WIRE_FACTOR: postgres_fdw sends a bytea parameter as hex text, so every
 *   filter byte becomes 2 bytes on the wire.
 * MAX_FILTER_BYTES: a BLOOM or EXACT filter bigger than this (header
 *   included) is never built (1 MB).
 * KEY_TEXT_LEN: average length of one key in the text form of the key list
 *   "{12,345,...}": the digits plus one comma. Our customer_ids go up to
 *   1,000,000, so most keys have 6 digits + 1 comma = 7 bytes.
 */
#define SJB_DEFAULT_WIRE_FACTOR 2.0
#define SJB_DEFAULT_MAX_FILTER_BYTES 1048576.0
#define SJB_DEFAULT_KEY_TEXT_LEN 7.0

/*
 * Everything the cost model needs to know. All values are doubles, so the
 * formulas can be written exactly as on paper.
 */
typedef struct SjbCostInputs
{
	double		num_keys;		/* n: distinct join keys at site A */
	double		s_rows;			/* number of rows of S at site B */
	double		rows_per_key;	/* rows of S per distinct key value */
	double		row_width;		/* bytes of one returned row ON THE WIRE */
	double		network_mbit;	/* network speed in megabits per second */
	double		t_check_ns;		/* site B: ns to scan + check one row */
	double		t_lookup_us;	/* site B: us for one index lookup */
	double		max_filter_bytes;	/* largest BLOOM/EXACT filter allowed */
	double		wire_factor;	/* wire bytes per filter byte (2 = hex) */
	double		key_text_len;	/* bytes per key in the key list text */
	double		per_message_ms;	/* T_MSG: ms per message round trip */
	double		per_row_us;		/* us per returned row (text out + in) */
	double		t_build_us;		/* site A: us per key to build a filter */
	double		fetch_size;		/* rows per postgres_fdw FETCH (100) */
} SjbCostInputs;

/*
 * The estimate for ONE way of running the query: the index path, or the
 * filter path with one mode.
 */
typedef struct SjbCostEstimate
{
	const char *name;			/* "index", "bloom", "exact" or "pass" */
	bool		allowed;		/* false if this mode was skipped */
	const char *skip_reason;	/* why it was skipped (NULL if allowed) */
	double		bits_per_key;	/* BLOOM only: b */
	double		m;				/* BLOOM only: number of bits */
	double		k;				/* BLOOM only: hash positions per key */
	double		fp_rate;		/* BLOOM only: predicted false-positive rate */
	double		filter_size;	/* the filter value itself (header included) */
	double		bytes_a_to_b;	/* filter or key list, as text on the wire */
	double		rows_back;		/* rows site B sends back */
	double		bytes_b_to_a;	/* rows_back * row_width */
	double		network_seconds;	/* (bytes_a_to_b + bytes_b_to_a) / bandwidth */
	double		messages;		/* 1 + ceil(rows_back / fetch_size) */
	double		message_seconds;	/* messages * per_message_ms */
	double		row_seconds;	/* rows_back * per_row_us */
	double		work_at_a_seconds;	/* building the filter at site A */
	double		work_at_b_seconds;	/* checks or index lookups at site B */
	double		total_seconds;	/* the sum of the five times above */
} SjbCostEstimate;

/* The estimates for all four ways, and the two decisions. */
typedef struct SjbChoice
{
	double		matching_rows;	/* M = rows of S that really match */
	SjbCostEstimate index_path;
	SjbCostEstimate bloom;
	SjbCostEstimate exact;
	SjbCostEstimate pass;
	int			best_filter_mode;	/* SJB_MODE_BLOOM, _EXACT or _PASS */
	bool		index_path_wins;	/* true: index path; false: filter path */
} SjbChoice;

/* Single formulas (costmodel.c explains each one). */
extern double sjb_fp_rate(double n, double m, double k);
extern double sjb_best_bits(double n, double s_rows, double matches,
							double row_width, double wire_factor);
extern double sjb_bloom_bits_for(double n, double bits_per_key);
extern double sjb_bloom_hashes_for(double bits_per_key);
extern double sjb_matching_rows(double n, double s_rows, double rows_per_key);
extern double sjb_bloom_filter_bytes(double m, double wire_factor);
extern double sjb_exact_filter_bytes(double n, double wire_factor);
extern double sjb_pass_filter_bytes(double wire_factor);
extern double sjb_key_list_bytes(double n, double key_text_len);
extern double sjb_bandwidth_bytes_per_sec(double network_mbit);
extern double sjb_message_count(double rows_back, double fetch_size);
extern double sjb_message_seconds(double messages, double per_message_ms);
extern double sjb_returned_row_seconds(double rows_back, double per_row_us);
extern double sjb_index_lookup_seconds(double n, double t_lookup_us);
extern double sjb_filter_build_seconds(double n, double t_build_us);

/* The whole model: fills in all four estimates and both decisions. */
extern void sjb_estimate_all(const SjbCostInputs *inputs, SjbChoice *choice);

/* The estimate of one filter mode (SJB_MODE_...) inside a choice. */
extern const SjbCostEstimate *sjb_estimate_for_mode(const SjbChoice *choice,
													int mode);

#endif							/* SJB_COSTMODEL_H */
