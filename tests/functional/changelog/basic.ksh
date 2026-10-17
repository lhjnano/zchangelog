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
# Basic smoke test: namespace operations on the watched filesystem are
# recorded and read back by a consumer.
#

. tests/include/zcltest.shlib

log_assert "namespace ops are recorded (FS=$FS)"

mount_test_fs || log_fail "cannot mount $FS"

sudo -n $ZCL -d $CLDB watch $CLMNT >$CLDIR/watch.log 2>&1 &
sleep 1

touch $CLMNT/a
mkdir $CLMNT/d
mv $CLMNT/a $CLMNT/b
rm $CLMNT/b

sleep 1
sudo -n pkill -x zchangelog
sleep 0.3

out=$(sudo -n $ZCL -d $CLDB read)
echo "$out"

echo "$out" | grep -q CREATE || log_fail "no CREATE record"
echo "$out" | grep -q REMOVE || log_fail "no REMOVE record"

log_pass "namespace ops are recorded"
