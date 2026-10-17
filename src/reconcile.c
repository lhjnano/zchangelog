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
 * Reconciliation: a full tree walk compared against the inventory
 * table.  Finds anything the live fanotify feed missed and appends
 * corrective records.  Run on collector start, after a queue
 * overflow, or on demand.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ftw.h>
#include <time.h>
#include <sys/stat.h>

#include "zchangelog.h"


static long long	zcl_ts;

static int
reconcile_cb(const char *path, const struct stat *sb, int typeflag,
    struct FTW *ftw)
{
	long long mtime, size, imtime, isize;
	int err, op;

	if (typeflag != FTW_F && typeflag != FTW_D)
		return (0);
	if (getenv("ZCL_DEBUG") != NULL)
		(void) fprintf(stderr, "DBG walk %s type=%d\n", path,
		    typeflag);
	mtime = (long long)sb->st_mtim.tv_sec;
	size = (long long)sb->st_size;
	err = store_inventory_get(path, &imtime, &isize);
	if (err == 0 && imtime == mtime && isize == size) {
		/* Unchanged: still mark as walked, or it looks deleted. */
		(void) store_walked_put(path);
		return (0);
	}
	if (err == 0)
		op = ZCL_MODIFY;
	else if (err == 1)
		op = ZCL_CREATE;
	else
		return (0);
	(void) store_add(zcl_ts, op, path, NULL);
	(void) store_inventory_put(path, mtime, size);
	(void) store_walked_put(path);
	return (0);
}

static void
missing_cb(const char *path, void *arg)
{
	(void) arg;
	(void) store_add(zcl_ts, ZCL_DELETE, path, NULL);
	(void) store_inventory_del(path);
}

int
cmd_reconcile(int argc, char **argv)
{
	if (argc != 1) {
		(void) fprintf(stderr,
		    "usage: zchangelog reconcile <mount>\n");
		return (2);
	}
	if (store_open(zcl_dbfile) != 0)
		return (1);

	zcl_ts = (long long)time(NULL) * 1000000LL;
	(void) store_exec("BEGIN IMMEDIATE");
	(void) store_exec("CREATE TEMP TABLE IF NOT EXISTS walked ("
	    "path TEXT PRIMARY KEY)");
	(void) store_exec("DELETE FROM walked");

	if (nftw(argv[0], reconcile_cb, 16, FTW_PHYS) != 0)
		(void) fprintf(stderr, "zchangelog: walk failed\n");

	(void) store_inventory_del_missing(missing_cb, NULL);
	(void) store_exec("DELETE FROM walked");
	(void) store_exec("COMMIT");
	(void) printf("reconcile done\n");
	store_close();
	return (0);
}
