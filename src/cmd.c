// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 *
 * Copyright (c) 2026 by HeonJe Lee
 */

/*
 * Subcommand implementations.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>

#include "zchangelog.h"

static const char *const zcl_opnames[] = {
	NULL,		/* 0 unused */
	"CREATE", "REMOVE", "RENAME", "MODIFY", "ATTRIB"
};

const char *
zcl_opname(int op)
{
	if (op < 1 || op >= (int)(sizeof (zcl_opnames) / sizeof (*zcl_opnames)))
		return ("UNKNOWN");
	return (zcl_opnames[op]);
}

static void
fmt_time(long long ts, char *buf, size_t buflen)
{
	time_t t = (time_t)(ts / 1000000);
	struct tm tm;

	(void) localtime_r(&t, &tm);
	(void) strftime(buf, buflen, "%Y-%m-%d %H:%M:%S", &tm);
}

static void
print_rec(const zcl_record_t *r)
{
	char tbuf[32];

	fmt_time(r->ts, tbuf, sizeof (tbuf));
	(void) printf("%-10lld %-8s %s %s", r->seq, zcl_opname(r->op), tbuf,
	    r->path);
	if (r->path_to[0] != '\0')
		(void) printf(" -> %s", r->path_to);
	(void) printf("\n");
}

static void
print_rec_json(const zcl_record_t *r)
{
	(void) printf("{\"type\":\"rec\",\"seq\":%lld,\"ts\":%lld,"
	    "\"op\":\"%s\",\"path\":\"%s\",\"path_to\":\"%s\"}\n",
	    r->seq, r->ts, zcl_opname(r->op), r->path, r->path_to);
}

int
cmd_register(int argc, char **argv)
{
	long long head;
	int err;

	if (argc != 1) {
		(void) fprintf(stderr,
		    "usage: zchangelog register <consumer>\n");
		return (2);
	}
	if (store_open(zcl_dbfile) != 0)
		return (1);

	err = store_consumer_get(argv[0], NULL);
	if (err == 0) {
		(void) fprintf(stderr, "zchangelog: consumer '%s' already "
		    "registered\n", argv[0]);
		store_close();
		return (1);
	}
	head = store_max_seq();
	if (store_consumer_set(argv[0], head) != 0) {
		store_close();
		return (1);
	}
	(void) printf("registered '%s' at seq %lld\n", argv[0], head);
	store_close();
	return (0);
}

int
cmd_unregister(int argc, char **argv)
{
	long long cursor;
	int err;

	if (argc != 1) {
		(void) fprintf(stderr,
		    "usage: zchangelog unregister <consumer>\n");
		return (2);
	}
	if (store_open(zcl_dbfile) != 0)
		return (1);

	err = store_consumer_get(argv[0], &cursor);
	if (err != 0) {
		(void) fprintf(stderr, "zchangelog: unknown consumer '%s'\n",
		    argv[0]);
		store_close();
		return (1);
	}
	(void) printf("unregistered '%s' at seq %lld\n", argv[0], cursor);
	(void) store_consumer_del(argv[0]);
	store_trim();
	store_close();
	return (0);
}

static int
status_cb(const char *name, long long cursor, long long updated, void *arg)
{
	long long age = (long long)time(NULL) - updated;

	(void) printf("  %-24s cursor %lld  idle %llds\n", name, cursor,
	    updated > 0 ? age : -1);
	return (0);
}

int
cmd_status(int argc, char **argv)
{
	long long head, tail;

	if (argc != 0) {
		(void) fprintf(stderr, "usage: zchangelog status\n");
		return (2);
	}
	if (store_open(zcl_dbfile) != 0)
		return (1);

	head = store_max_seq();
	tail = store_min_cursor();
	(void) printf("head %lld  tail %lld  pending %lld\n",
	    head, tail, head - tail);
	(void) store_consumer_list(status_cb, NULL);
	store_close();
	return (0);
}

int
cmd_clear(int argc, char **argv)
{
	long long cursor, head, want;
	int err;

	if (argc != 2) {
		(void) fprintf(stderr,
		    "usage: zchangelog clear <consumer> <seq>\n");
		return (2);
	}
	if (store_open(zcl_dbfile) != 0)
		return (1);

	err = store_consumer_get(argv[0], &cursor);
	if (err != 0) {
		(void) fprintf(stderr, "zchangelog: unknown consumer '%s'\n",
		    argv[0]);
		store_close();
		return (1);
	}
	head = store_max_seq();
	want = atoll(argv[1]);
	cursor = (want > head) ? head : want;
	(void) store_consumer_set(argv[0], cursor);
	store_trim();
	(void) printf("cleared '%s' to seq %lld\n", argv[0], cursor);
	store_close();
	return (0);
}

int
cmd_read(int argc, char **argv)
{
	zcl_record_t *recs;
	long long cursor = 0, ms = 0;
	const char *consumer = NULL, *prefix = NULL;
	int nrecs, ncount, use_cursor = 0, ack = 0, follow = 0,
	    json = 0, coalesce = 0, err, ch;

	ncount = (int)zcl_tunable("read_batch");
	recs = malloc((size_t)ncount * sizeof (*recs));
	if (recs == NULL)
		return (1);

	optind = 1;
	while ((ch = getopt(argc, argv, "c:s:n:w:afjCp:")) != -1) {
		switch (ch) {
		case 'C':
			coalesce = 1;
			break;
		case 'c':
			consumer = optarg;
			break;
		case 's':
			cursor = atoll(optarg);
			use_cursor = 1;
			break;
		case 'n':
			ncount = atoi(optarg);
			break;
		case 'w':
			ms = atoll(optarg);
			break;
		case 'a':
			ack = 1;
			break;
		case 'f':
			follow = 1;
			break;
		case 'j':
			json = 1;
			break;
		case 'p':
			prefix = optarg;
			break;
		default:
			(void) fprintf(stderr, "usage: zchangelog read "
			    "[-c consumer] [-s seq] [-n count] [-w ms] "
			    "[-afj]\n");
			return (2);
		}
	}
	if (store_open(zcl_dbfile) != 0)
		return (1);

	if (consumer != NULL && !use_cursor) {
		err = store_consumer_get(consumer, &cursor);
		if (err != 0) {
			(void) fprintf(stderr, "zchangelog: unknown consumer "
			    "'%s'\n", consumer);
			store_close();
			return (1);
		}
	} else if (consumer == NULL && !use_cursor) {
		cursor = 0;
	}
read_loop:
	err = store_read(cursor, ncount, recs, &nrecs);
	if (err != 0) {
		(void) fprintf(stderr, "zchangelog: read failed\n");
		store_close();
		return (1);
	}
	/*
	 * Coalesce: keep only the last record per path, so a consumer
	 * applying the result sees the final state of each entry.  The
	 * cursor still advances past everything that was read.
	 */
	if (coalesce && nrecs > 1) {
		zcl_record_t *kept = malloc((size_t)nrecs * sizeof (*kept));
		int nk = 0;

		if (kept == NULL) {
			free(recs);
			store_close();
			return (1);
		}
		for (int i = nrecs - 1; i >= 0; i--) {
			int dup = 0;

			for (int j = i + 1; j < nrecs; j++) {
				if (strcmp(recs[j].path, recs[i].path) == 0) {
					dup = 1;
					break;
				}
			}
			if (!dup)
				kept[nk++] = recs[i];
		}
		for (int i = nk - 1; i >= 0; i--)
			recs[nk - 1 - i] = kept[i];
		nrecs = nk;
		free(kept);
	}

	cursor = (nrecs > 0) ? recs[nrecs - 1].seq : cursor;

	if (prefix != NULL) {
		int n = 0;

		for (int i = 0; i < nrecs; i++)
			if (zcl_prefix_match(prefix, recs[i].path))
				recs[n++] = recs[i];
		nrecs = n;
	}

	for (int i = 0; i < nrecs; i++) {
		if (json)
			print_rec_json(&recs[i]);
		else
			print_rec(&recs[i]);
	}

	if (getenv("ZCL_DEBUG") != NULL)
		(void) fprintf(stderr, "DBG read nrecs=%d cursor=%lld ack=%d "
		    "consumer=%s coalesce=%d\n", nrecs, cursor, ack,
		    consumer ? consumer : "(none)", coalesce);

	if (ack && consumer != NULL)
		(void) store_consumer_set(consumer, cursor);

	free(recs);
	if (follow) {
		if (nrecs > 0)
			goto read_loop;
		(void) usleep((useconds_t)(ms > 0 ? ms * 1000 :
		    zcl_tunable("follow_interval_ms") * 1000));
		goto read_loop;
	}
	if (json)
		(void) printf("{\"type\":\"meta\",\"cursor\":%lld,"
		    "\"head\":%lld}\n", cursor, store_max_seq());
	store_close();
	return (0);
}

int
cmd_watch(int argc, char **argv)
{
	const char *mount = NULL;
	int i;

	/*
	 * Parsed by hand: getopt permutes non-options, which loses
	 * track of the mount when flags follow it.
	 */
	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
			zcl_filter_add(0, argv[++i]);
		} else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
			zcl_filter_add(1, argv[++i]);
		} else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
			zcl_filter_owner((long long)atoll(argv[++i]), -1);
		} else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
			zcl_filter_owner(-1, (long long)atoll(argv[++i]));
		} else if (mount == NULL) {
			mount = argv[i];
		} else {
			mount = NULL;
			break;
		}
	}

	if (mount == NULL) {
		(void) fprintf(stderr, "usage: zchangelog watch "
		    "[-i include-prefix] [-e exclude-prefix] "
		    "[-u owner-uid] [-g owner-gid] <mount>\n");
		return (2);
	}
	if (store_open(zcl_dbfile) != 0)
		return (1);
	return (watch_run(mount));
}

static void
maint_print(const char *line, void *arg)
{
	(void) arg;
	(void) printf("%s\n", line);
}

int
cmd_maint(int argc, char **argv)
{
	if (argc != 0) {
		(void) fprintf(stderr, "usage: zchangelog maint\n");
		return (2);
	}
	if (store_open(zcl_dbfile) != 0)
		return (1);

	if (store_maint(maint_print, NULL) != 0) {
		store_close();
		return (1);
	}
	(void) store_consumer_list(status_cb, NULL);
	store_close();
	return (0);
}