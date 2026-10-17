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
# Burst load: many rapid creations.  The live feed may overflow its
# queue; reconcile must close the gap so the final state is complete.
#

. tests/include/zcltest.shlib

log_assert "burst load ends with complete state (FS=$FS)"

mount_test_fs || log_fail "cannot mount $FS"

sudo -n $ZCL -d $CLDB watch $CLMNT >$CLDIR/watch.log 2>&1 &
sleep 1

typeset -i i=1
while (( i <= 500 )); do
	echo $i > $CLMNT/f$i
	(( i += 1 ))
done

sleep 2
sudo -n pkill -x zchangelog
sleep 0.3

# Read the whole stream by following the meta cursor in batches.
typeset all=""
typeset cursor=0
while :; do
	out=$(sudo -n $ZCL -d $CLDB read -s $cursor -n 256 -j)
	all="$all$(echo "$out" | grep '"type":"rec"')"
	typeset -i next=$(echo "$out" | tail -1 |
	    sed -n 's/.*"cursor":\([0-9]*\).*/\1/p')
	[[ $next -eq $cursor ]] && break
	cursor=$next
done
log_note "stream holds $(( $(echo "$all" | wc -l) )) records"
sudo -n $ZCL -d $CLDB reconcile $CLMNT

typeset -i missing=0
i=1
while (( i <= 500 )); do
	echo "$all" | grep -q "\"path\":\"$CLMNT/f$i\"" || (( missing += 1 ))
	(( i += 1 ))
done
(( missing == 0 )) || log_fail "$missing creations missing after reconcile"

log_pass "burst load ends with complete state"
