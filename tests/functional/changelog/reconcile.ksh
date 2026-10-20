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
# Reconciliation: changes made while the collector is down are caught
# by reconcile, and a no-change reconcile adds no records.
#

. tests/include/zcltest.shlib

log_assert "reconcile catches collector downtime (FS=$FS)"

mount_test_fs || log_fail "cannot mount $FS"

sudo -n $ZCL -d $CLDB watch $ZCL_WATCHFLAGS $CLMNT >$CLDIR/watch.log 2>&1 &
sleep 1
touch $CLMNT/seen1
touch $CLMNT/seen2
sleep 1
sudo -n pkill -x zchangelog
sleep 0.3

# Collector is down: these are missed by the live feed.
rm $CLMNT/seen1
touch $CLMNT/unseen

before=$(sudo -n $ZCL -d $CLDB status | awk 'NR==1 {print $4}')
sudo -n $ZCL -d $CLDB reconcile $CLMNT
out=$(sudo -n $ZCL -d $CLDB read)

echo "$out"
echo "$out" | grep -q REMOVE || log_fail "missed deletion of seen1"
echo "$out" | grep unseen | grep -q CREATE || log_fail "missed unseen"

after=$(sudo -n $ZCL -d $CLDB status | awk 'NR==1 {print $4}')
sudo -n $ZCL -d $CLDB reconcile $CLMNT
final=$(sudo -n $ZCL -d $CLDB status | awk 'NR==1 {print $4}')

[[ $after -eq $final ]] || log_fail "no-change reconcile added records"

log_pass "reconcile catches collector downtime"
