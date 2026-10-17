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
# Collection filters: an include prefix keeps only the matching
# subtree, and read -p applies the same constraint on the consumer
# side.
#

. tests/include/zcltest.shlib

log_assert "include and read path filters (FS=$FS)"

mount_test_fs || log_fail "cannot mount $FS"

sudo -n $ZCL -d $CLDB watch $CLMNT -i $CLMNT/keep >$CLDIR/watch.log 2>&1 &
sleep 1

mkdir $CLMNT/keep
mkdir $CLMNT/drop
touch $CLMNT/keep/k1
touch $CLMNT/drop/d1

sleep 1
sudo -n pkill -x zchangelog
sleep 0.3

out=$(sudo -n $ZCL -d $CLDB read -j)
echo "$out" | grep -q "keep/k1" || log_fail "k1 record missing"
echo "$out" | grep -q "drop/d1" && log_fail "d1 was not filtered"

# The consumer side can constrain by path independently: a fresh
# collector without include filters records both subtrees, and -p
# narrows the read to one.
sudo -n $ZCL -d $CLDB watch $CLMNT >$CLDIR/watch2.log 2>&1 &
sleep 1
touch $CLMNT/plain1
touch $CLMNT/plain2
sleep 1
sudo -n pkill -x zchangelog
sleep 0.3

pout=$(sudo -n $ZCL -d $CLDB read -j -p $CLMNT/keep)
echo "$pout" | grep -q "keep/k1" || log_fail "read -p missed k1"
echo "$pout" | grep -q "plain1" && log_fail "read -p leaked plain1"

log_pass "include and read path filters"
