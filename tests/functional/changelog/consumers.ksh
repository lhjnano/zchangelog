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
# Multiple consumers: independent cursors, acknowledge advancing only
# the acknowledging consumer, and trim bounded by the slowest one.
#

. tests/include/zcltest.shlib

log_assert "consumer cursors are independent (FS=$FS)"

mount_test_fs || log_fail "cannot mount $FS"

sudo -n $ZCL -d $CLDB watch $CLMNT >$CLDIR/watch.log 2>&1 &
sleep 1
sudo -n $ZCL -d $CLDB register fast
sudo -n $ZCL -d $CLDB register slow

touch $CLMNT/a
touch $CLMNT/b
touch $CLMNT/c
sleep 1
sudo -n pkill -x zchangelog
sleep 0.3

# fast acknowledges everything; slow never reads.
sudo -n $ZCL -d $CLDB read -c fast -a > /dev/null

head=$(sudo -n $ZCL -d $CLDB status | awk 'NR==1 {print $2}')
tail=$(sudo -n $ZCL -d $CLDB status | awk 'NR==1 {print $4}')
[[ $tail -eq 0 ]] || log_fail "tail moved past unacked slow consumer"

out=$(sudo -n $ZCL -d $CLDB read -c slow)
n=$(echo "$out" | grep -c CREATE)
(( n >= 3 )) || log_fail "slow consumer sees $n creates, expected 3"

# After slow acknowledges too, everything is trimmed.
sudo -n $ZCL -d $CLDB clear slow $head
tail=$(sudo -n $ZCL -d $CLDB status | awk 'NR==1 {print $4}')
[[ $tail -eq $head ]] || log_fail "tail $tail did not reach head $head"

log_pass "consumer cursors are independent"
