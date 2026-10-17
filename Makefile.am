ACLOCAL_AMFLAGS = -I m4
bin_PROGRAMS = zchangelog
zchangelog_SOURCES = \
	src/main.c \
	src/cmd.c \
	src/store.c \
	src/watch_fanotify.c \
	src/reconcile.c \
	src/conf.c \
	src/zchangelog.h

zchangelog_CFLAGS = -D_GNU_SOURCE $(sqlite3_CFLAGS)
zchangelog_LDADD = $(sqlite3_LIBS)

CHECKS = cstyle spdxcheck
.PHONY: $(CHECKS) checkstyle

cstyle:
	$(AM_V_at)find $(top_srcdir)/src -type f -name '*.[ch]' | xargs $(top_srcdir)/scripts/cstyle.pl

spdxcheck:
	$(AM_V_at)for f in $(top_srcdir)/src/*.[ch]; do \
	    grep -q 'SPDX-License-Identifier: CDDL-1.0' $$f || \
	    { echo "missing SPDX tag: $$f"; exit 1; }; \
	done

checkstyle: $(CHECKS)

TESTS = \
	tests/functional/changelog/basic.ksh \
	tests/functional/changelog/consumers.ksh \
	tests/functional/changelog/filters.ksh \
	tests/functional/changelog/owner.ksh \
	tests/functional/changelog/reconcile.ksh \
	tests/functional/changelog/stress.ksh \
	tests/functional/changelog/unmount.ksh
EXTRA_DIST = tests scripts
