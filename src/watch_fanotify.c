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
 * fanotify based collector in FID reporting mode.  CREATE, DELETE,
 * MOVE and ATTRIB events require FAN_REPORT_DFID_NAME, and locations
 * are reported as file handles that are resolved to paths with the
 * open_by_handle_at syscall.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/fanotify.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/syscall.h>

#include "zchangelog.h"

#ifndef	O_LARGEFILE
#define	O_LARGEFILE	0
#endif

/*
 * struct file_handle is declared by glibc 2.36+ only.  Ubuntu 22.04
 * ships 2.35, so the layout is defined here and the syscall is issued
 * directly.
 */
struct zcl_fh {
	unsigned int	handle_bytes;
	int		handle_type;
	unsigned char	f_handle[];
};

static int	zcl_fanfd = -1;
static unsigned long long	zcl_markmask;
static char	zcl_mountpath[4096];

/*
 * (Re)apply the mark on the current mount.  Marks die with the
 * mount, so this runs again whenever the mount identity changes.
 */
static void
refresh_mark(void)
{
	if (fanotify_mark(zcl_fanfd, FAN_MARK_ADD | FAN_MARK_MOUNT,
	    zcl_markmask, AT_FDCWD, zcl_mountpath) == 0)
		return;
	(void) fanotify_mark(zcl_fanfd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM,
	    zcl_markmask, AT_FDCWD, zcl_mountpath);
}

/*
 * Open the mount root for handle resolution.  The fd lives only for
 * the duration of one resolve: a held mount fd would block unmount
 * and failover.
 */
static int
mount_fd(void)
{
	return (open(zcl_mountpath, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
}

static int
path_from_handle(const struct zcl_fh *fh, char *buf, size_t buflen)
{
	static unsigned char last_bytes[32];
	static size_t last_len = (size_t)-1;
	static char last_path[4096];
	unsigned char fbuf[512];
	size_t n = fh->handle_bytes;
	int mfd, ffd;
	char link[64];

	if (n == 0 || n > sizeof (fbuf))
		return (-1);
	if (last_len == n && memcmp(last_bytes, fh->f_handle, n) == 0) {
		(void) zcl_strlcpy(buf, last_path, buflen);
		return (0);
	}

	mfd = mount_fd();
	if (mfd < 0)
		return (-1);
	unsigned char gbuf[512 + sizeof (struct zcl_fh)];
	struct zcl_fh *g = (struct zcl_fh *)gbuf;

	memcpy(gbuf, fh, sizeof (*fh) + n);
	ffd = (int)syscall(SYS_open_by_handle_at, mfd, g,
	    O_PATH | O_DIRECTORY);
	if (ffd < 0)
		ffd = (int)syscall(SYS_open_by_handle_at, mfd, g,
		    O_PATH);
	if (ffd < 0) {
		(void) close(mfd);
		return (-1);
	}
	(void) snprintf(link, sizeof (link), "/proc/self/fd/%d", ffd);
	if (readlink(link, buf, buflen - 1) < 0) {
		(void) close(ffd);
		(void) close(mfd);
		return (-1);
	}
	buf[buflen - 1] = '\0';
	(void) close(ffd);
	(void) close(mfd);

	memcpy(last_bytes, fh->f_handle, n);
	last_len = n;
	(void) zcl_strlcpy(last_path, buf, sizeof (last_path));
	return (0);
}

/* Join a resolved directory and an event name into a full path. */
static int
join_path(const char *dir, const char *name, char *buf, size_t buflen)
{
	size_t d = strlen(dir);

	if (d == 0 || name == NULL || d + strlen(name) + 2 > buflen)
		return (-1);
	memcpy(buf, dir, d);
	buf[d] = '/';
	(void) zcl_strlcpy(buf + d + 1, name, buflen - d - 1);
	return (0);
}

/* Skip the journal database itself and its WAL side files. */
static int
skip_path(const char *path)
{
	size_t n = strlen(zcl_dbfile);

	if (strncmp(path, zcl_dbfile, n) != 0)
		return (0);
	return (path[n] == '\0' || strcmp(path + n, "-wal") == 0 ||
	    strcmp(path + n, "-shm") == 0);
}

/* Map a fanotify mask to a changelog op. */
static int
event_op(unsigned long long mask)
{
	if ((mask & FAN_MOVED_FROM) && (mask & FAN_MOVED_TO))
		return (ZCL_RENAME);
	/*
	 * A filesystem mark delivers the two halves of a rename as
	 * separate events; surface them as delete and create so
	 * consumers see consistent semantics.
	 */
	if (mask & FAN_MOVED_FROM)
		return (ZCL_DELETE);
	if (mask & FAN_MOVED_TO)
		return (ZCL_CREATE);
	if (mask & FAN_CREATE)
		return (ZCL_CREATE);
	if (mask & FAN_DELETE)
		return (ZCL_DELETE);
	if (mask & FAN_ATTRIB)
		return (ZCL_ATTRIB);
	return (ZCL_MODIFY);
}

/*
 * Extract paths from one event's info records.  DFID_NAME and
 * OLD/NEW_DFID_NAME carry a parent directory handle plus the entry
 * name; plain DFID (modify, attrib) carries the object itself.
 */
static void
event_paths(struct fanotify_event_metadata *meta, char *full,
    size_t fullen, char *oldfull, size_t oldlen)
{
	struct fanotify_event_info_header *info;
	struct zcl_fh *fh;
	struct fanotify_event_info_fid *fid;
	const char *name;
	char dirpath[4096];
	size_t off = sizeof (*meta);
	int got = 0;

	full[0] = '\0';
	oldfull[0] = '\0';
	(void) oldlen;

	while (off < meta->event_len) {
		info = (struct fanotify_event_info_header *)
		    ((char *)meta + off);
		if (info->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME ||
		    info->info_type == FAN_EVENT_INFO_TYPE_DFID ||
		    info->info_type == FAN_EVENT_INFO_TYPE_OLD_DFID_NAME ||
		    info->info_type == FAN_EVENT_INFO_TYPE_NEW_DFID_NAME) {
			fid = (struct fanotify_event_info_fid *)info;
			fh = (struct zcl_fh *)&fid->handle;
			name = (const char *)(fh->f_handle + fh->handle_bytes);
			if (path_from_handle(fh, dirpath,
			    sizeof (dirpath)) == 0 &&
			    (name == NULL || name[0] == '\0' ||
			    join_path(dirpath, name, full, fullen) == 0)) {
				if (info->info_type ==
				    FAN_EVENT_INFO_TYPE_OLD_DFID_NAME) {
					(void) zcl_strlcpy(oldfull, full,
					    oldlen);
					full[0] = '\0';
					got = 0;
				} else {
					got = 1;
				}
			}
		}
		off += info->len;
	}
	(void) got;
}

/* stat() a recorded path into the inventory; drop it if unreachable. */
static void
inv_stat(const char *path)
{
	struct stat st;

	if (stat(path, &st) == 0)
		(void) store_inventory_put(path,
		    (long long)st.st_mtim.tv_sec, (long long)st.st_size);
	else
		(void) store_inventory_del(path);
}

int
watch_run(const char *mountpath)
{
	struct fanotify_event_metadata *meta;
	char buf[65536];
	char full[4096], oldfull[4096];
	long long ts;
	int fd, len, op, rint;

	(void) zcl_strlcpy(zcl_mountpath, mountpath, sizeof (zcl_mountpath));
	zcl_fanfd = fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME,
	    O_RDONLY | O_CLOEXEC | O_LARGEFILE);
	if (zcl_fanfd < 0) {
		(void) fprintf(stderr, "zchangelog: fanotify_init: %s\n",
		    strerror(errno));
		return (1);
	}
	zcl_markmask = FAN_MODIFY | FAN_ATTRIB | FAN_CREATE | FAN_DELETE |
	    FAN_MOVED_FROM | FAN_MOVED_TO;
	refresh_mark();

	(void) printf("watching %s\n", mountpath);
	(void) fflush(stdout);

	for (;;) {
		struct pollfd pfd;
		int pret;

		/*
		 * poll with a timeout so a remounted dataset gets its
		 * marks refreshed without a restart; read only when
		 * the queue actually has events, otherwise the next
		 * refresh would never run.
		 */
		pfd.fd = zcl_fanfd;
		pfd.events = POLLIN;
		rint = (int)zcl_tunable("refresh_interval_ms");
		if (rint <= 0)
			rint = 5000;
		pret = poll(&pfd, 1, rint);
		if (pret < 0) {
			if (errno == EINTR)
				continue;
			(void) fprintf(stderr, "zchangelog: poll: %s\n",
			    strerror(errno));
			return (1);
		}
		if (pret == 0) {
			refresh_mark();
			continue;
		}

		len = read(zcl_fanfd, buf, sizeof (buf));
		if (len < 0) {
			if (errno == EINTR)
				continue;
			(void) fprintf(stderr, "zchangelog: read: %s\n",
			    strerror(errno));
			return (1);
		}

		meta = (struct fanotify_event_metadata *)buf;
		(void) store_exec("BEGIN IMMEDIATE");
		while (FAN_EVENT_OK(meta, len)) {
			if (meta->mask & FAN_Q_OVERFLOW)
				(void) fprintf(stderr, "zchangelog: queue "
				    "overflow, run reconcile\n");

			event_paths(meta, full, sizeof (full), oldfull,
			    sizeof (oldfull));

			if (getenv("ZCL_DEBUG") != NULL) {
				size_t o2 = sizeof (*meta);
				int first = 1;

				(void) fprintf(stderr, "DBG mask=%llx len=%u "
				    "types:", (unsigned long long)meta->mask,
				    meta->event_len);
				while (o2 < meta->event_len) {
					struct fanotify_event_info_header
					    *h = (struct
					    fanotify_event_info_header *)
					    ((char *)meta + o2);

					(void) fprintf(stderr, "%s%d",
					    first ? "" : ",", h->info_type);
					first = 0;
					o2 += h->len;
				}
				(void) fprintf(stderr, " full=%s old=%s "
				    "op=%s\n", full, oldfull, zcl_opname(
				    event_op(meta->mask)));
			}

			if (full[0] == '\0' && oldfull[0] == '\0')
				goto next;

			if (full[0] != '\0' && strncmp(full, zcl_mountpath,
			    strlen(zcl_mountpath)) != 0)
				goto next;
			if (zcl_filter_owner_uid() >= 0 ||
			    zcl_filter_owner_gid() >= 0) {
				const char *p = oldfull[0] ? oldfull : full;
				struct stat st;

				if (getenv("ZCL_DEBUG") != NULL &&
				    stat(p, &st) == 0)
					(void) fprintf(stderr,
					    "DBG attrib stat %s uid=%d "
					    "gid=%d\n", p, (int)st.st_uid,
					    (int)st.st_gid);

				/*
				 * Cannot attribute a vanished object;
				 * drop the event rather than misfile
				 * it under the wrong owner.
				 */
				if (p[0] == '\0' || stat(p, &st) != 0 ||
				    (zcl_filter_owner_uid() >= 0 &&
				    st.st_uid != (uid_t)
				    zcl_filter_owner_uid()) ||
				    (zcl_filter_owner_gid() >= 0 &&
				    st.st_gid != (gid_t)
				    zcl_filter_owner_gid()))
					goto next;
			}

			if (!zcl_path_allowed(full) ||
			    (oldfull[0] != '\0' &&
			    !zcl_path_allowed(oldfull)))
				goto next;

			if (skip_path(full) || skip_path(oldfull)) {
				meta = FAN_EVENT_NEXT(meta, len);
				continue;
			}

			ts = (long long)time(NULL) * 1000000LL;
			op = event_op(meta->mask);
			if (op == ZCL_RENAME) {
				(void) store_add(ts, op,
				    oldfull[0] ? oldfull : full,
				    full[0] ? full : NULL);
				(void) store_inventory_del(
				    oldfull[0] ? oldfull : full);
				if (full[0] != '\0')
					inv_stat(full);
			} else if (op == ZCL_DELETE) {
				(void) store_add(ts, op,
				    oldfull[0] ? oldfull : full, NULL);
				(void) store_inventory_del(
				    oldfull[0] ? oldfull : full);
			} else {
				(void) store_add(ts, op,
				    oldfull[0] ? oldfull : full, NULL);
				if (full[0] != '\0')
					inv_stat(full);
			}

next:
			meta = FAN_EVENT_NEXT(meta, len);
		}
		/*
		 * One transaction per read batch: autocommit commits per
		 * event, which dominates the collector cost at high event
		 * rates.
		 */
		if (store_exec("COMMIT") != 0)
			(void) store_exec("ROLLBACK");
	}
}
