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
