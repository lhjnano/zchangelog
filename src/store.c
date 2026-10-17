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
 * SQLite backed change journal.  Two tables implement the changelog
 * contract: records (the stream, keyed by a sequence number that
 * doubles as the consumer cursor position) and consumers (one cursor
 * per registered consumer).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "zchangelog.h"

static sqlite3	*db;
static sqlite3_stmt	*zcl_add_stmt;

int
store_open(const char *dbfile)
{
	char *err = NULL;

	if (sqlite3_open(dbfile, &db) != SQLITE_OK) {
		fprintf(stderr, "zchangelog: cannot open %s: %s\n", dbfile,
		    db ? sqlite3_errmsg(db) : "out of memory");
		return (-1);
	}

	(void) sqlite3_busy_timeout(db, (int)zcl_tunable("busy_timeout_ms"));
	if (sqlite3_exec(db, "PRAGMA journal_mode=WAL;"
	    "PRAGMA synchronous=NORMAL;"
	    "CREATE TABLE IF NOT EXISTS records ("
	    "seq INTEGER PRIMARY KEY AUTOINCREMENT,"
	    "ts INTEGER NOT NULL,"
	    "op INTEGER NOT NULL,"
	    "path TEXT NOT NULL,"
	    "path_to TEXT);"
	    "CREATE TABLE IF NOT EXISTS consumers ("
	    "name TEXT PRIMARY KEY,"
	    "cursor INTEGER NOT NULL);"
	    "CREATE TABLE IF NOT EXISTS inventory ("
	    "path TEXT PRIMARY KEY,"
	    "mtime INTEGER,"
	    "size INTEGER);",
	    NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "zchangelog: schema: %s\n",
		    err ? err : sqlite3_errmsg(db));
		return (-1);
	}

	/*
	 * Schema migrations, keyed by user_version.  v2 adds the
	 * consumer last-activity timestamp used by the staleness
	 * report.  Setting user_version doubles as the writability
	 * probe, so a root-owned journal fails here with a clear
	 * error instead of deep inside subcommands.
	 */
	if (sqlite3_exec(db, "ALTER TABLE consumers ADD COLUMN "
	    "updated INTEGER NOT NULL DEFAULT 0",
	    NULL, NULL, NULL) != SQLITE_OK &&
	    strstr(sqlite3_errmsg(db), "duplicate") == NULL)
		fprintf(stderr, "zchangelog: consumers migration: %s\n",
		    sqlite3_errmsg(db));
	if (sqlite3_exec(db, "PRAGMA user_version = 2", NULL, NULL,
	    &err) != SQLITE_OK) {
		fprintf(stderr, "zchangelog: %s is not writable by this "
		    "user\n", dbfile);
		return (-1);
	}
	return (0);
}

void
store_close(void)
{
	if (zcl_add_stmt != NULL) {
		sqlite3_finalize(zcl_add_stmt);
		zcl_add_stmt = NULL;
	}
	if (db != NULL)
		sqlite3_close(db);
	db = NULL;
}

int
store_add(long long ts, int op, const char *path, const char *to)
{
	sqlite3_stmt *stmt;
	int rc;

	if (zcl_add_stmt == NULL) {
		if (sqlite3_prepare_v2(db,
		    "INSERT INTO records (ts, op, path, path_to) "
		    "VALUES (?, ?, ?, ?)",
		    -1, &stmt, NULL) != SQLITE_OK)
			return (-1);
		zcl_add_stmt = stmt;
	}
	(void) sqlite3_reset(zcl_add_stmt);
	(void) sqlite3_clear_bindings(zcl_add_stmt);
	(void) sqlite3_bind_int64(zcl_add_stmt, 1, ts);
	(void) sqlite3_bind_int(zcl_add_stmt, 2, op);
	(void) sqlite3_bind_text(zcl_add_stmt, 3, path, -1, SQLITE_STATIC);
	(void) sqlite3_bind_text(zcl_add_stmt, 4, to ? to : "", -1,
	    SQLITE_STATIC);
	rc = sqlite3_step(zcl_add_stmt);
	return (rc == SQLITE_DONE ? 0 : -1);
}

int
store_read(long long since, int max, zcl_record_t *out, int *nout)
{
	sqlite3_stmt *stmt;
	int rc, n = 0;

	if (sqlite3_prepare_v2(db,
	    "SELECT seq, ts, op, path, path_to FROM records "
	    "WHERE seq > ? ORDER BY seq LIMIT ?",
	    -1, &stmt, NULL) != SQLITE_OK)
		return (-1);
	(void) sqlite3_bind_int64(stmt, 1, since);
	(void) sqlite3_bind_int(stmt, 2, max);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && n < max) {
		zcl_record_t *r = &out[n++];

		r->seq = sqlite3_column_int64(stmt, 0);
		r->ts = sqlite3_column_int64(stmt, 1);
		r->op = sqlite3_column_int(stmt, 2);
		(void) snprintf(r->path, sizeof (r->path), "%s",
		    sqlite3_column_text(stmt, 3));
		(void) snprintf(r->path_to, sizeof (r->path_to), "%s",
		    sqlite3_column_text(stmt, 4));
	}
	(void) sqlite3_finalize(stmt);
	*nout = n;
	return (rc == SQLITE_DONE || rc == SQLITE_ROW ? 0 : -1);
}

int
store_consumer_set(const char *name, long long cursor)
{
	sqlite3_stmt *stmt;
	int rc;

	if (sqlite3_prepare_v2(db,
	    "INSERT INTO consumers (name, cursor, updated) VALUES (?, ?, ?) "
	    "ON CONFLICT(name) DO UPDATE SET cursor = excluded.cursor, "
	    "updated = excluded.updated",
	    -1, &stmt, NULL) != SQLITE_OK)
		return (-1);
	(void) sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
	(void) sqlite3_bind_int64(stmt, 2, cursor);
	(void) sqlite3_bind_int64(stmt, 3, (long long)time(NULL));
	rc = sqlite3_step(stmt);
	(void) sqlite3_finalize(stmt);
	return (rc == SQLITE_DONE ? 0 : -1);
}

int
store_consumer_get(const char *name, long long *cursorp)
{
	sqlite3_stmt *stmt;
	int rc;

	if (sqlite3_prepare_v2(db,
	    "SELECT cursor FROM consumers WHERE name = ?",
	    -1, &stmt, NULL) != SQLITE_OK)
		return (-1);
	(void) sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW && cursorp != NULL)
		*cursorp = sqlite3_column_int64(stmt, 0);
	(void) sqlite3_finalize(stmt);
	return (rc == SQLITE_ROW ? 0 : (rc == SQLITE_DONE ? 1 : -1));
}

int
store_consumer_del(const char *name)
{
	sqlite3_stmt *stmt;
	int rc;

	if (sqlite3_prepare_v2(db,
	    "DELETE FROM consumers WHERE name = ?",
	    -1, &stmt, NULL) != SQLITE_OK)
		return (-1);
	(void) sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
	rc = sqlite3_step(stmt);
	(void) sqlite3_finalize(stmt);
	return (rc == SQLITE_DONE ? 0 : -1);
}

long long
store_min_cursor(void)
{
	sqlite3_stmt *stmt;
	long long v = 0;

	if (sqlite3_prepare_v2(db,
	    "SELECT MIN(cursor) FROM consumers", -1, &stmt, NULL) != SQLITE_OK)
		return (0);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		v = sqlite3_column_int64(stmt, 0);
	(void) sqlite3_finalize(stmt);
	return (v);
}

long long
store_max_seq(void)
{
	sqlite3_stmt *stmt;
	long long v = 0;

	if (sqlite3_prepare_v2(db,
	    "SELECT COALESCE(MAX(seq), 0) FROM records", -1, &stmt, NULL) !=
	    SQLITE_OK)
		return (0);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		v = sqlite3_column_int64(stmt, 0);
	(void) sqlite3_finalize(stmt);
	return (v);
}

void
store_trim(void)
{
	sqlite3_stmt *stmt;
	long long min, head, cap;

	min = store_min_cursor();
	if (min > 0) {
		if (sqlite3_prepare_v2(db,
		    "DELETE FROM records WHERE seq <= ?",
		    -1, &stmt, NULL) == SQLITE_OK) {
			(void) sqlite3_bind_int64(stmt, 1, min);
			(void) sqlite3_step(stmt);
			(void) sqlite3_finalize(stmt);
		}
	}

	/*
	 * Retention cap: records older than this are dropped even
	 * without consumer acknowledgement, so a dead consumer cannot
	 * make the journal grow forever.
	 */
	cap = zcl_tunable("retention_records");
	if (cap <= 0)
		return;
	head = store_max_seq();
	if (head - min <= cap)
		return;
	if (sqlite3_prepare_v2(db, "DELETE FROM records WHERE seq <= ?",
	    -1, &stmt, NULL) == SQLITE_OK) {
		(void) sqlite3_bind_int64(stmt, 1, head - cap);
		(void) sqlite3_step(stmt);
		(void) sqlite3_finalize(stmt);
	}
}

int
store_consumer_list(int (*cb)(const char *name, long long cursor,
    long long updated, void *arg), void *arg)
{
	sqlite3_stmt *stmt;
	int rc;

	if (sqlite3_prepare_v2(db,
	    "SELECT name, cursor, updated FROM consumers ORDER BY name",
	    -1, &stmt, NULL) != SQLITE_OK)
		return (-1);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		if (cb((const char *)sqlite3_column_text(stmt, 0),
		    sqlite3_column_int64(stmt, 1),
		    sqlite3_column_int64(stmt, 2), arg) != 0)
			break;
	}
	(void) sqlite3_finalize(stmt);
	return (rc == SQLITE_DONE || rc == SQLITE_ROW ? 0 : -1);
}

int
store_inventory_put(const char *path, long long mtime, long long size)
{
	sqlite3_stmt *stmt;
	int rc;

	if (sqlite3_prepare_v2(db,
	    "INSERT OR REPLACE INTO inventory (path, mtime, size) "
	    "VALUES (?, ?, ?)", -1, &stmt, NULL) != SQLITE_OK)
		return (-1);
	(void) sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
	(void) sqlite3_bind_int64(stmt, 2, mtime);
	(void) sqlite3_bind_int64(stmt, 3, size);
	rc = sqlite3_step(stmt);
	(void) sqlite3_finalize(stmt);
	return (rc == SQLITE_DONE ? 0 : -1);
}

int
store_inventory_get(const char *path, long long *mtimep, long long *sizep)
{
	sqlite3_stmt *stmt;
	int rc;

	if (sqlite3_prepare_v2(db,
	    "SELECT mtime, size FROM inventory WHERE path = ?",
	    -1, &stmt, NULL) != SQLITE_OK)
		return (-1);
	(void) sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		*mtimep = sqlite3_column_int64(stmt, 0);
		*sizep = sqlite3_column_int64(stmt, 1);
	}
	(void) sqlite3_finalize(stmt);
	return (rc == SQLITE_ROW ? 0 : (rc == SQLITE_DONE ? 1 : -1));
}

int
store_inventory_del_missing(void (*cb)(const char *path, void *arg),
    void *arg)
{
	sqlite3_stmt *stmt;
	int rc;

	if (sqlite3_exec(db,
	    "CREATE TEMP TABLE IF NOT EXISTS walked ("
	    "path TEXT PRIMARY KEY)", NULL, NULL, NULL) != SQLITE_OK)
		return (-1);
	if (sqlite3_prepare_v2(db,
	    "SELECT path FROM inventory WHERE path NOT IN "
	    "(SELECT path FROM walked)", -1, &stmt, NULL) != SQLITE_OK)
		return (-1);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		cb((const char *)sqlite3_column_text(stmt, 0), arg);
	}
	(void) sqlite3_finalize(stmt);
	return (0);
}

int
store_exec(const char *sql)
{
	char *err = NULL;

	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "zchangelog: exec: %s\n",
		    err ? err : sqlite3_errmsg(db));
		return (-1);
	}
	return (0);
}

int
store_walked_put(const char *path)
{
	sqlite3_stmt *stmt;
	int rc;

	if (sqlite3_prepare_v2(db,
	    "INSERT OR IGNORE INTO walked (path) VALUES (?)",
	    -1, &stmt, NULL) != SQLITE_OK)
		return (-1);
	(void) sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
	rc = sqlite3_step(stmt);
	(void) sqlite3_finalize(stmt);
	return (rc == SQLITE_DONE ? 0 : -1);
}

int
store_inventory_del(const char *path)
{
	sqlite3_stmt *stmt;
	int rc;

	if (sqlite3_prepare_v2(db, "DELETE FROM inventory WHERE path = ?",
	    -1, &stmt, NULL) != SQLITE_OK)
		return (-1);
	(void) sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
	rc = sqlite3_step(stmt);
	(void) sqlite3_finalize(stmt);
	return (rc == SQLITE_DONE ? 0 : -1);
}

int
store_maint(void (*print)(const char *line, void *arg), void *arg)
{
	sqlite3_stmt *stmt;
	int rc;
	char line[256];

	if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &stmt,
	    NULL) != SQLITE_OK)
		return (-1);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		(void) snprintf(line, sizeof (line), "integrity: %s",
		    sqlite3_column_text(stmt, 0));
		print(line, arg);
	}
	(void) sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return (-1);

	(void) snprintf(line, sizeof (line),
	    "wal checkpoint: %d", sqlite3_wal_checkpoint_v2(db, NULL,
	    SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL));
	print(line, arg);

	if (sqlite3_prepare_v2(db,
	    "SELECT page_count * page_size, freelist_count * page_size "
	    "FROM pragma_page_count(), pragma_freelist_count(), "
	    "pragma_page_size()", -1, &stmt, NULL) != SQLITE_OK)
		return (0);
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		(void) snprintf(line, sizeof (line),
		    "size %lld bytes, free %lld bytes",
		    sqlite3_column_int64(stmt, 0),
		    sqlite3_column_int64(stmt, 1));
		print(line, arg);
	}
	(void) sqlite3_finalize(stmt);
	return (0);
}
