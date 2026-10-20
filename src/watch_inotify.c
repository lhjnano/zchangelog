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
 * inotify based collector for kernels without fanotify FID reporting
 * (Linux 4.x) or with restricted fanotify namespace events.  inotify
 * delivers entry names directly, so records stay path based without
 * handle resolution; watches are per directory, added recursively,
 * with new directories picked up from the event stream itself.  The
 * reconciler remains the safety net for event loss.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "zchangelog.h"

#define	ZCL_IMEVENTS (IN_CREATE | IN_DELETE | IN_MOVED_FROM | \
	    IN_MOVED_TO | IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF | \
	    IN_ISDIR | IN_UNMOUNT)

struct wdent {
	int		wd;
	char		path[4096];
};

static int	zcl_ifd = -1;
static char	zcl_mountpath[4096];
static struct wdent	*wds;
static int	nwds, capwds;

/*
 * Rename cookie and old path of the last unmatched MOVED_FROM.  A
 * rename arrives as an adjacent pair but the pair can straddle two
 * read buffers; keep only the last unmatched one.
 */
static unsigned int	pend_cookie;
static char	pend_old[4096];
static int	have_pend;

static const char *
wd_path(int wd)
{
	int i;

	for (i = 0; i < nwds; i++)
		if (wds[i].wd == wd)
			return (wds[i].path);
	return (NULL);
}

static void
wd_add(int wd, const char *path)
{
	int i;

	for (i = 0; i < nwds; i++)
		if (wds[i].wd == wd) {
			(void) zcl_strlcpy(wds[i].path, path,
			    sizeof (wds[i].path));
			return;
		}
	if (nwds == capwds) {
		capwds = capwds == 0 ? 16 : capwds * 2;
		wds = realloc(wds, capwds * sizeof (*wds));
		if (wds == NULL) {
			(void) fprintf(stderr,
			    "zchangelog: out of memory\n");
			exit(1);
		}
	}
	wds[nwds].wd = wd;
	(void) zcl_strlcpy(wds[nwds].path, path, sizeof (wds[nwds].path));
	nwds++;
}

static void
wd_del(int wd)
{
	int i;

	for (i = 0; i < nwds; i++) {
		if (wds[i].wd == wd) {
			(void) memmove(&wds[i], &wds[i + 1],
			    (nwds - i - 1) * sizeof (*wds));
			nwds--;
			return;
		}
	}
}

static void
clear_watches(void)
{
	int i;

	for (i = 0; i < nwds; i++)
		(void) inotify_rm_watch(zcl_ifd, wds[i].wd);
	free(wds);
	wds = NULL;
	nwds = capwds = 0;
}

static int
add_dir_watch(const char *path)
{
	int wd = inotify_add_watch(zcl_ifd, path, ZCL_IMEVENTS);

	if (wd < 0) {
		(void) fprintf(stderr,
		    "zchangelog: inotify_add_watch(%s): %s\n",
		    path, strerror(errno));
		return (-1);
	}
	wd_add(wd, path);
	return (0);
}

/*
 * Recursively add the directory tree to the watch set.  Partial
 * failures are logged and tolerated; the reconciler covers holes.
 */
static int
watch_tree(const char *root)
{
	DIR *d;
	struct dirent *de;
	struct stat st;
	char child[4096];

	if (add_dir_watch(root) != 0)
		return (-1);

	d = opendir(root);
	if (d == NULL) {
		(void) fprintf(stderr, "zchangelog: opendir(%s): %s\n",
		    root, strerror(errno));
		return (-1);
	}
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		if (zcl_join_path(root, de->d_name, child,
		    sizeof (child)) != 0)
			continue;
		if (lstat(child, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			(void) watch_tree(child);
	}
	closedir(d);
	return (0);
}

/* Record an inventory entry, dropping it when unreachable. */
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
watch_inotify_run(const char *mountpath)
{
	struct inotify_event *iev;
	char buf[65536];
	char full[4096], oldfull[4096];
	long long ts;
	int len, ofs, op, rint;

	(void) zcl_strlcpy(zcl_mountpath, mountpath, sizeof (zcl_mountpath));
	zcl_ifd = inotify_init();
	if (zcl_ifd < 0) {
		(void) fprintf(stderr, "zchangelog: inotify_init: %s\n",
		    strerror(errno));
		return (1);
	}
	if (watch_tree(mountpath) != 0) {
		(void) fprintf(stderr, "zchangelog: initial watch failed\n");
		(void) close(zcl_ifd);
		return (1);
	}
	(void) printf("watching %s (inotify backend)\n", mountpath);
	(void) fflush(stdout);

	for (;;) {
		struct pollfd pfd;
		int pret;

		/*
		 * Poll with a timeout so a remounted dataset gets its
		 * watches rebuilt without a restart; the inotify queue
		 * is read only when it actually has events.
		 */
		pfd.fd = zcl_ifd;
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
			if (nwds == 0)
				(void) watch_tree(zcl_mountpath);
			continue;
		}

		len = read(zcl_ifd, buf, sizeof (buf));
		if (len < 0) {
			if (errno == EINTR)
				continue;
			(void) fprintf(stderr, "zchangelog: read: %s\n",
			    strerror(errno));
			return (1);
		}

		(void) store_exec("BEGIN IMMEDIATE");
		ofs = 0;
		while (ofs < len) {
			iev = (struct inotify_event *)(buf + ofs);
			const char *dirpath = wd_path(iev->wd);
			int skip = 0;

			if (iev->mask & IN_UNMOUNT) {
				clear_watches();
				break;
			}
			if (iev->mask & IN_Q_OVERFLOW)
				(void) fprintf(stderr, "zchangelog: inotify "
				    "queue overflow, run reconcile\n");
			if (dirpath == NULL || iev->wd == 0) {
				ofs += sizeof (*iev) + iev->len;
				continue;
			}
			if (iev->mask & IN_IGNORED) {
				wd_del(iev->wd);
				ofs += sizeof (*iev) + iev->len;
				continue;
			}
			if (iev->mask & IN_DELETE_SELF) {
				wd_del(iev->wd);
				ofs += sizeof (*iev) + iev->len;
				continue;
			}
			if (iev->len == 0 || dirpath == NULL) {
				ofs += sizeof (*iev);
				continue;
			}

			if (zcl_join_path(dirpath,
			    (char *)iev->name, full, sizeof (full)) != 0) {
				ofs += sizeof (*iev) + iev->len;
				continue;
			}
			if (strncmp(full, zcl_mountpath,
			    strlen(zcl_mountpath)) != 0)
				skip = 1;
			if (!zcl_path_allowed(full))
				skip = 1;
			if (zcl_skip_path(full))
				skip = 1;
			if (skip) {
				ofs += sizeof (*iev) + iev->len;
				continue;
			}

			if (iev->mask & IN_MOVED_FROM) {
				pend_cookie = iev->cookie;
				(void) zcl_strlcpy(pend_old, full,
				    sizeof (pend_old));
				have_pend = 1;
				ofs += sizeof (*iev) + iev->len;
				continue;
			}
			if (iev->mask & IN_MOVED_TO) {
				if (have_pend && pend_cookie == iev->cookie) {
					op = ZCL_RENAME;
					(void) zcl_strlcpy(oldfull, pend_old,
					    sizeof (oldfull));
				} else {
					op = ZCL_CREATE;
					oldfull[0] = '\0';
				}
				have_pend = 0;
			}
			ts = (long long)time(NULL) * 1000000LL;
			if (iev->mask & IN_MOVED_TO && op == ZCL_RENAME) {
				(void) store_add(ts, op, oldfull, full);
				(void) store_inventory_del(oldfull);
				(void) zcl_inv_stat(full);
			} else if (iev->mask & IN_MOVED_TO) {
				(void) store_add(ts, op, full, NULL);
				(void) zcl_inv_stat(full);
			} else if (iev->mask & IN_CREATE) {
				(void) store_add(ts, ZCL_CREATE, full, NULL);
				if (iev->mask & IN_ISDIR)
					(void) watch_tree(full);
				(void) zcl_inv_stat(full);
			} else if (iev->mask & IN_DELETE) {
				(void) store_add(ts, ZCL_DELETE, full, NULL);
				(void) store_inventory_del(full);
			} else if (iev->mask & IN_MODIFY) {
				(void) store_add(ts, ZCL_MODIFY, full, NULL);
				(void) zcl_inv_stat(full);
			} else if (iev->mask & IN_ATTRIB) {
				(void) store_add(ts, ZCL_ATTRIB, full, NULL);
				(void) zcl_inv_stat(full);
			}
			ofs += sizeof (*iev) + iev->len;
		}
		if (store_exec("COMMIT") != 0)
			(void) store_exec("ROLLBACK");
		have_pend = 0;
	}
}
