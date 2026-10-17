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

#include <stdio.h>
#include <string.h>

#include "zchangelog.h"

const char *zcl_dbfile = "zchangelog.db";

static void
usage(void)
{
	(void) fprintf(stderr,
	    "usage: zchangelog [-d dbfile] <register|unregister|read|"
	    "clear|status|watch|reconcile|maint> ...\n");
}

int
main(int argc, char **argv)
{
	zcl_conf_load();

	int i = 1;

	if (argc > 2 && strcmp(argv[1], "-d") == 0) {
		zcl_dbfile = argv[2];
		i = 3;
	}

	if (argc <= i) {
		usage();
		return (2);
	}

	if (strcmp(argv[i], "register") == 0)
		return (cmd_register(argc - i - 1, argv + i + 1));
	if (strcmp(argv[i], "unregister") == 0)
		return (cmd_unregister(argc - i - 1, argv + i + 1));
	if (strcmp(argv[i], "read") == 0)
		return (cmd_read(argc - i, argv + i));
	if (strcmp(argv[i], "clear") == 0)
		return (cmd_clear(argc - i - 1, argv + i + 1));
	if (strcmp(argv[i], "status") == 0)
		return (cmd_status(argc - i - 1, argv + i + 1));
	if (strcmp(argv[i], "watch") == 0)
		return (cmd_watch(argc - i - 1, argv + i + 1));
	if (strcmp(argv[i], "reconcile") == 0)
		return (cmd_reconcile(argc - i - 1, argv + i + 1));
	if (strcmp(argv[i], "maint") == 0)
		return (cmd_maint(argc - i - 1, argv + i + 1));

	usage();
	return (2);
}
