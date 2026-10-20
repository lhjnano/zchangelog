# zchangelog

A change stream for ZFS datasets. zchangelog watches a dataset mount,
appends a record for every namespace operation to a SQLite journal
with sequence-number cursors, and serves multiple named consumers
with register/read/clear semantics.

Copies of a dataset, search indexes, vector stores, replicas,
dashboards, need to know what changed without walking the whole tree
or polling. zchangelog turns filesystem events into a durable,
resumable stream: every consumer resumes exactly where it stopped,
acknowledges what it processed, and old records are trimmed once
every consumer passed them.

## Features

- Two collection backends sharing one journal and cursor model:
  fanotify FID reporting (the default; Linux 5.13+) and an inotify
  backend covering Linux 4.x and newer kernels that restrict
  fanotify namespace events
- SQLite journal with per-consumer cursors, acknowledge and trim
- Collector survives unmounts: it does not pin the dataset, and
  refreshes its mark automatically after a remount, which is what
  makes cluster failovers possible without stopping anything first
- Reconciliation: a full tree walk that catches anything the live
  feed missed
- JSON output and follow mode for pipeline integration
- Tunables through /etc/zchangelog.conf

## Requirements

- Linux 4.x or newer: collection runs on fanotify FID reporting
  (Linux 5.13+, the default backend) or on the inotify backend
  (any kernel with inotify, selected with watch -b inotify)
  (FAN_REPORT_DFID_NAME, FAN_CREATE/DELETE/MOVE/ATTRIB) and
  open_by_handle_at, none of which exist in older kernels
- fanotify is Linux only; a FreeBSD port would need a kqueue based
  collector

## Install

### From packages

Grab a deb or rpm from the
[releases page](https://github.com/lhjnano/zchangelog/releases) and
install it with the system package manager:

    apt install ./zchangelog_0.1.0-1_amd64.deb      # Debian, Ubuntu
    dnf install zchangelog-0.1.0-1.fc44.x86_64.rpm  # Fedora, RHEL family

### From source

Build dependencies differ per distribution:

Ubuntu, Debian:

    apt install build-essential autoconf automake pkg-config libsqlite3-dev

Fedora:

    dnf install gcc make autoconf automake pkgconf-pkg-config sqlite-devel

RHEL, AlmaLinux, Rocky (9 or newer):

    dnf install gcc make autoconf automake pkgconfig sqlite-devel

Alpine, other musl based distributions: the same autotools build
should work with `build-base sqlite-dev`; it is not covered by CI
yet.

Then:

    ./autogen.sh
    ./configure
    make
    sudo make install

For the test suite add ksh, e2fsprogs, xfsprogs (Debian, Ubuntu:
`apt install ksh e2fsprogs xfsprogs`; Fedora, RHEL family:
`dnf install ksh e2fsprogs xfsprogs`). The tests create their own
filesystems and need root.

## Usage

### Collect

Run the collector on the storage node (fanotify needs root):

    sudo zchangelog -d /var/lib/zchangelog/pool.db watch /pool/data

Every namespace operation on the dataset now appends a record to the
journal. The collector selects fanotify FID reporting when the kernel
offers it and falls back to the inotify backend otherwise; pass
`-b inotify` to force it, which is what 4.x kernels take.

### Consume

Register a consumer once; it starts at the current head of the
stream:

    zchangelog -d /var/lib/zchangelog/pool.db register indexer
    zchangelog -d /var/lib/zchangelog/pool.db read -c indexer -j

Read with `-a` to acknowledge what was processed, `-f` to follow, or
`-s` to read from an explicit position. `-p` restricts the output to
a path prefix, and `-C` coalesces records so each path appears once
with its final state, which suits debounce style workers. Records
are trimmed once every consumer acknowledged them.

### Filter

The collector can constrain what it records. The default is
everything; include prefixes, exclude prefixes and owner or group
ids are all optional and combine:

    sudo zchangelog -d pool.db watch /pool/data \
        -i /pool/data/keep -e /pool/data/keep/tmp -u 0

Filters are evaluated at collection time (before the journal
write), so filtered out events cost no journal space. Owner and
group are attributed when the event arrives: a record keeps the
owner the file had at creation. Note that a chown arrives as its
own attrib event, and attrib events race with the change they
describe, so owner filtering is best effort rather than a strict
guarantee. When a pool hosts many datasets, prefer include
prefixes over running one collector per dataset.

### Reconcile

The live feed can miss events (queue overflow, collector downtime).
Reconcile walks the dataset and appends corrective records:

    zchangelog -d /var/lib/zchangelog/pool.db reconcile /pool/data

Run it at collector start, after a queue overflow warning, or
periodically. The journal keeps a per-path inventory, so a
no-change reconcile adds no records.

### Pipelines

The consumer contract is three calls: register once, read from a
cursor, acknowledge. A vector store can, for example, run read -j in
a loop and apply each record to its index, acknowledging after
commit. Multiple consumers, an index, a search cache, a replica 
share one journal at independent positions.

## Operational notes

- Run the collector and every consumer as the **same user**, and keep
  the journal out of sticky world writable directories (/tmp is
  subject to fs.protected_regular on modern distributions and rejects
  cross-user opens). /var/lib/zchangelog works well.
- The journal is per-mount: watching an NFS export from the client is
  not possible; run the collector on the storage node.
- A rollback, receive or destroy shows up as bulk changes; run
  reconcile afterwards and expect consumers to rescan.
- The collector does not hold the mount open and does not pin it: an
  unmount succeeds while it runs. During the millisecond a fanotify
  event is being resolved, a racing unmount can still report EBUSY,
  so retry once if a scripted failover hits it. After a remount the
  marks are refreshed automatically, within refresh_interval_ms
  (default 5s), with no restart and no lost state; the events that
  happened while the dataset was unmounted do not exist for the
  collector, run reconcile after mount if that window matters.

## Journal maintenance

    zchangelog -d /var/lib/zchangelog/pool.db maint

reports an integrity check, runs a WAL checkpoint (TRUNCATE) and
prints journal size and free space, plus per-consumer idle time.
A consumer that never acknowledges pins old records; the
retention_records tunable bounds that, and `unregister` releases a
dead consumer explicitly. The schema version (PRAGMA user_version)
drives automatic migrations on open.

## Tunables

/etc/zchangelog.conf (lines of key = value):

| key | default | description |
|-----|---------|-------------|
| busy_timeout_ms | 5000 | sqlite lock wait |
| read_batch | 1024 | records fetched per read |
| follow_interval_ms | 500 | follow mode poll interval |
| refresh_interval_ms | 5000 | how often an idle collector re-arms the fanotify mark after a remount |
| retention_records | 0 | force-drop oldest records beyond this; 0 keeps everything until acknowledged |

## Performance

Measured on a tmpfs workload, one collector, default config:

- Sustained rate: about 5,500 events per second (one event per
  namespace operation; a single touch of a fresh file produces one
  create plus one attrib, so user visible operations count double).
- Burst absorption: the kernel fanotify queue holds 16,384 events.
  A 150,000 operation burst arriving in 0.7s fills it, the collector
  logs "queue overflow, run reconcile" once, and the queue keeps
  accepting events as the collector drains it.
- Overflow recovery: files that still exist get their create or
  modify corrected by reconcile. A file that was both created and
  deleted inside the overflow window leaves no trace anywhere, so no
  changelog can recover it; it is gone from the journal too. If that
  window matters, slow the writer down, or run the consumers from a
  full scan baseline.

Reconcile is the intended safety net: it compares the tree against
the inventory and appends corrective create, modify and remove
records. Run it after every overflow warning.

## Tests

The suite runs the functional tests against tmpfs, ext4, xfs and
zfs, with a CI matrix over ZFS feature variants (plain, dedup,
encryption, quota, raidz pool layout, separate slog). See
docs/zfs-integration.md for how the stream interacts with ZFS
features, and docs/roadmap.md for extension directions:

    make check

## Packaging

Debian and RPM packaging live in debian/ and packaging/rpm/. A
release workflow publishes source tarballs (make dist) and packages
on version tags.

## License

CDDL-1.0, matching OpenZFS.
