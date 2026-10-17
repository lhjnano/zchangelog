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
 * Tunables.  Loaded from a config file (default /etc/zchangelog.conf,
 * override with the ZCHANGELOG_CONF environment variable).  Lines of
 * the form "key = value"; unknown keys are ignored and missing keys
 * keep the defaults below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zchangelog.h"

struct zcl_tunable {
	const char	*key;
	long long	value;
};

static struct zcl_tunable zcl_tunables[] = {
	{ "busy_timeout_ms",	5000 },	/* sqlite lock wait */
	{ "read_batch",		1024 },	/* records fetched per read */
	{ "follow_interval_ms",	500 },	/* follow mode poll interval */
	{ "refresh_interval_ms", 5000 },	/* remount mark refresh check */
	{ "retention_records",	0 },	/* 0 = keep until acknowledged */
};

#define	ZCL_TUNABLE_COUNT \
	((int)(sizeof (zcl_tunables) / sizeof (*zcl_tunables)))

long long
zcl_tunable(const char *key)
{
	for (int i = 0; i < ZCL_TUNABLE_COUNT; i++) {
		if (strcmp(zcl_tunables[i].key, key) == 0)
			return (zcl_tunables[i].value);
	}
	return (0);
}

static void
zcl_tunable_set(const char *key, long long value)
{
	for (int i = 0; i < ZCL_TUNABLE_COUNT; i++) {
		if (strcmp(zcl_tunables[i].key, key) == 0) {
			zcl_tunables[i].value = value;
			return;
		}
	}
}

void
zcl_conf_load(void)
{
	const char *path = getenv("ZCHANGELOG_CONF");
	FILE *fp;
	char line[512];

	if (path == NULL || path[0] == '\0')
		path = "/etc/zchangelog.conf";
	fp = fopen(path, "r");
	if (fp == NULL)
		return;

	while (fgets(line, sizeof (line), fp) != NULL) {
		char *key = line, *value, *end;

		while (*key == ' ' || *key == '\t')
			key++;
		if (*key == '#' || *key == '\n' || *key == '\0')
			continue;
		value = strchr(key, '=');
		if (value == NULL)
			continue;
		*value = '\0';
		value++;
		end = key + strlen(key);
		while (end > key && (end[-1] == ' ' || end[-1] == '\t'))
			*--end = '\0';
		while (*value == ' ' || *value == '\t')
			value++;
		end = value + strlen(value);
		while (end > value && (end[-1] == ' ' || end[-1] == '\t' ||
		    end[-1] == '\n'))
			*--end = '\0';
		zcl_tunable_set(key, atoll(value));
	}
	(void) fclose(fp);
}

/*
 * Watch filters.  Empty lists mean everything is collected.  Prefix
 * matching requires a path or directory boundary, so /pool/data does
 * not match /pool/database.
 */
#define	ZCL_MAX_FILTERS	64

static char	*zcl_includes[ZCL_MAX_FILTERS];
static int	zcl_ninc;
static char	*zcl_excludes[ZCL_MAX_FILTERS];
static int	zcl_nexc;
static long long	zcl_owner_uid = -1;
static long long	zcl_owner_gid = -1;

int
zcl_include_count(void)
{
	return (zcl_ninc);
}

const char *
zcl_include(int i)
{
	return (zcl_includes[i]);
}

int
zcl_exclude_count(void)
{
	return (zcl_nexc);
}

const char *
zcl_exclude(int i)
{
	return (zcl_excludes[i]);
}

void
zcl_filter_add(int is_exclude, const char *prefix)
{
	char **list = is_exclude ? zcl_excludes : zcl_includes;
	int *n = is_exclude ? &zcl_nexc : &zcl_ninc;

	if (*n >= ZCL_MAX_FILTERS || prefix == NULL || prefix[0] == '\0')
		return;
	list[*n] = strdup(prefix);
	if (list[*n] != NULL)
		(*n)++;
}

void
zcl_filter_owner(long long uid, long long gid)
{
	if (uid >= 0)
		zcl_owner_uid = uid;
	if (gid >= 0)
		zcl_owner_gid = gid;
}

long long
zcl_filter_owner_uid(void)
{
	return (zcl_owner_uid);
}

long long
zcl_filter_owner_gid(void)
{
	return (zcl_owner_gid);
}

/*
 * True when path is prefix itself or lives below it.
 */
int
zcl_prefix_match(const char *prefix, const char *path)
{
	size_t n = strlen(prefix);

	if (n == 0)
		return (1);
	return (strncmp(path, prefix, n) == 0 &&
	    (path[n] == '\0' || path[n] == '/'));
}

/*
 * Apply the configured filters to a path.
 */
int
zcl_path_allowed(const char *path)
{
	int i;

	for (i = 0; i < zcl_nexc; i++)
		if (zcl_prefix_match(zcl_excludes[i], path))
			return (0);
	if (zcl_ninc == 0)
		return (1);
	for (i = 0; i < zcl_ninc; i++)
		if (zcl_prefix_match(zcl_includes[i], path))
			return (1);
	return (0);
}
