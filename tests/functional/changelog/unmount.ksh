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
# The collector must not pin the dataset mount.  Unmounting while it
# runs has to succeed, the collector has to survive, and after a
# remount it has to pick up new events on its own.  Together these
# are what make a cluster failover possible.
#

. tests/include/zcltest.shlib

log_assert "unmount while collecting, resume after remount (FS=$FS)"

mkdir -p $CLDIR
printf 'refresh_interval_ms = 1000\n' > $CLDIR/zcl.conf
CONF=$CLDIR/zcl.conf

mount_test_fs || log_fail "cannot mount $FS"

sudo -n env ZCHANGELOG_CONF=$CONF $ZCL -d $CLDB watch $ZCL_WATCHFLAGS $CLMNT \
    >$CLDIR/watch.log 2>&1 &
sleep 1

touch $CLMNT/before
sleep 1
sudo -n $ZCL -d $CLDB read | grep -q before ||
    log_fail "event before unmount not recorded"

log_must sudo -n umount $CLMNT
sleep 0.5
pgrep -x zchangelog >/dev/null ||
    log_fail "collector died across unmount"

remount_test_fs || log_fail "cannot remount $FS"

# Give the idle collector one refresh_interval to re-arm the mark,
# then generate an event: only events after the re-arm arrive.
sleep 2
touch $CLMNT/after
sleep 1.5
sudo -n $ZCL -d $CLDB read | grep -q after ||
    log_fail "no events after remount (mark not refreshed)"

log_pass "unmount while collecting, resume after remount"
