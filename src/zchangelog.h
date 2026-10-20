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

#ifndef	_ZCHANGELOG_H
#define	_ZCHANGELOG_H

#include <sqlite3.h>
#include <stddef.h>
#include <string.h>

/*
 * glibc gained strlcpy in 2.38; provide it for older glibc (Ubuntu
 * 22.04 ships 2.35).
 */
static inline size_t
zcl_strlcpy(char *dst, const char *src, size_t dstsize)
{
#ifdef	HAVE_STRLCPY
	return (strlcpy(dst, src, dstsize));
#else
	size_t srclen = strlen(src);

	if (dstsize != 0) {
		size_t n = (srclen < dstsize - 1 ? srclen : dstsize - 1);
		(void) memcpy(dst, src, n);
		dst[n] = '\0';
	}
	return (srclen);
#endif
}
/*
 * Ops recorded by the fanotify source.  Values follow the kernel
 * changelog ops where a mapping exists; MODIFY and ATTRIB have no
 * kernel counterpart and are userland only.
 */
#define	ZCL_CREATE	1
#define	ZCL_DELETE	2
#define	ZCL_RENAME	3
#define	ZCL_MODIFY	4
#define	ZCL_ATTRIB	5

typedef struct zcl_record {
	long long	seq;
	long long	ts;		/* epoch microseconds */
	int		op;
	char		path[4096];
	char		path_to[4096];	/* rename destination or "" */
} zcl_record_t;

extern const char	*zcl_dbfile;

/* conf.c */
long long	zcl_tunable(const char *key);
void	zcl_conf_load(void);
int	zcl_include_count(void);
const char	*zcl_include(int i);
int	zcl_exclude_count(void);
const char	*zcl_exclude(int i);
void	zcl_filter_add(int is_exclude, const char *prefix);
void	zcl_filter_owner(long long uid, long long gid);
long long	zcl_filter_owner_uid(void);
long long	zcl_filter_owner_gid(void);
int	zcl_prefix_match(const char *prefix, const char *path);
int	zcl_path_allowed(const char *path);

/* store.c */
int	store_open(const char *dbfile);
void	store_close(void);
int	store_add(long long ts, int op, const char *path, const char *to);
int	store_read(long long since, int max, zcl_record_t *out, int *nout);
int	store_consumer_set(const char *name, long long cursor);
int	store_consumer_get(const char *name, long long *cursorp);
int	store_consumer_del(const char *name);
int	store_consumer_list(int (*cb)(const char *, long long,
    long long, void *), void *arg);
int	store_maint(void (*print)(const char *, void *), void *arg);
int	store_inventory_put(const char *path, long long mtime, long long size);
int	store_inventory_get(const char *path, long long *mtimep,
    long long *sizep);
int	store_inventory_del_missing(void (*cb)(const char *, void *),
    void *arg);
int	store_inventory_del(const char *path);
int	store_walked_put(const char *path);
int	store_exec(const char *sql);
long long	store_min_cursor(void);
long long	store_max_seq(void);
void	store_trim(void);

/* watch_fanotify.c */
int	watch_run(const char *mountpath);
int	watch_inotify_run(const char *mountpath);
int	zcl_skip_path(const char *path);
void	zcl_inv_stat(const char *path);
int	zcl_join_path(const char *dir, const char *name, char *buf,
    size_t buflen);

/* cmd.c */
int	cmd_register(int argc, char **argv);
int	cmd_unregister(int argc, char **argv);
int	cmd_read(int argc, char **argv);
int	cmd_clear(int argc, char **argv);
int	cmd_status(int argc, char **argv);
int	cmd_watch(int argc, char **argv);
int	cmd_reconcile(int argc, char **argv);
int	cmd_maint(int argc, char **argv);

const char	*zcl_opname(int op);

#endif	/* _ZCHANGELOG_H */
