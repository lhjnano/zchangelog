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
 * Helpers shared by the collection backends.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "zchangelog.h"

/* Skip the journal database itself and its WAL side files. */
int
zcl_skip_path(const char *path)
{
	size_t n = strlen(zcl_dbfile);

	if (strncmp(path, zcl_dbfile, n) != 0)
		return (0);
	return (path[n] == '\0' || strcmp(path + n, "-wal") == 0 ||
	    strcmp(path + n, "-shm") == 0);
}

/* stat() a recorded path into the inventory; drop it if unreachable. */
void
zcl_inv_stat(const char *path)
{
	struct stat st;

	if (stat(path, &st) == 0)
		(void) store_inventory_put(path,
		    (long long)st.st_mtim.tv_sec, (long long)st.st_size);
	else
		(void) store_inventory_del(path);
}

/* Join a resolved directory and an event name into a full path. */
int
zcl_join_path(const char *dir, const char *name, char *buf, size_t buflen)
{
	size_t d = strlen(dir);

	if (d == 0 || name == NULL || d + strlen(name) + 2 > buflen)
		return (-1);
	memcpy(buf, dir, d);
	buf[d] = '/';
	(void) zcl_strlcpy(buf + d + 1, name, buflen - d - 1);
	return (0);
}
