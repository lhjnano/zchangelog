#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# https://opensource.org/license/CDDL-1.0.
#
# Copyright (c) 2026 by HeonJe Lee
#

#
#
# Maintenance: the maint command serves the integrity check, the wal
# checkpoint and the consumer listing.
#

. tests/include/zcltest.shlib

log_assert "maint reports integrity and consumers (FS=$FS, backend=$ZCL_BACKEND)"

mount_test_fs || log_fail "cannot mount $FS"

sudo -n $ZCL -d $CLDB watch $ZCL_WATCHFLAGS $CLMNT >$CLDIR/watch.log 2>&1 &
sleep 1
touch $CLMNT/m
sleep 1
sudo -n pkill -x zchangelog
sleep 0.3

sudo -n $ZCL -d $CLDB register m1 > /dev/null

out=$(sudo -n $ZCL -d $CLDB maint)
echo "$out"

echo "$out" | grep -q "integrity: ok" || log_fail "integrity check failed"
echo "$out" | grep -q "m1" || log_fail "consumer m1 missing from listing"

log_pass "maint reports integrity and consumers"
