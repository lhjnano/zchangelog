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
# Owner filtering: events are attributed at the moment they arrive,
# so a record keeps the owner the file had at creation, and a later
# chown arrives as an attrib record that the filter rejects.
#

. tests/include/zcltest.shlib

log_assert "owner filter by uid (FS=$FS)"

mount_test_fs || log_fail "cannot mount $FS"

sudo -n $ZCL -d $CLDB watch $CLMNT -u 0 >$CLDIR/watch.log 2>&1 &
sleep 1

sudo -n touch $CLMNT/rootfile
sleep 1
sudo -n touch $CLMNT/otherfile
sudo -n chown nobody $CLMNT/otherfile
sleep 1

sudo -n pkill -x zchangelog
sleep 0.3

out=$(sudo -n $ZCL -d $CLDB read)
echo "$out"

echo "$out" | grep -q "rootfile" || log_fail "rootfile record missing"
echo "$out" | grep otherfile | grep -q ATTRIB &&
    log_fail "attrib of other-owner file was not filtered"

log_pass "owner filter by uid"
