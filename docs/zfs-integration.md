# zchangelog and ZFS

This document explains which ZFS mechanisms zchangelog builds on, how
it behaves alongside ZFS features (dedup, encryption, slog, raidz,
snapshots, send/recv), and how it fits AI-oriented data pipelines.
It is aimed at users who already run ZFS and want their AI tooling
to see dataset changes efficiently.

## What zchangelog uses

zchangelog is a userland program. It depends on two kernel
mechanisms, not on ZFS internals:

1. fanotify FID reporting (Linux 5.1+): the collector marks the
   dataset mount and receives one event per namespace operation
   (create, delete, rename, attribute change, content modify), with
   the parent directory file handle and entry name attached.
2. open_by_handle_at: file handles are resolved back to paths. ZFS
   supports file handles through its NFS export operations, so this
   works on ZFS datasets without any configuration.

Everything else is self contained: a SQLite journal with
sequence-number cursors, named consumers with acknowledge and trim,
and a full tree walk (reconcile) that catches events the live feed
missed. No ZFS kernel module changes, no kernel parameters.

ZFS commands involved on the user side are the ordinary ones:

    zfs create -o changelog... (not needed; watch any dataset)
    zfs create pool/data            # the dataset to watch
    zfs set mountpoint=/pool/data   # mount it, point the collector at it

The collector only needs the mountpoint path.

## How it behaves with ZFS features

| ZFS feature | Interaction with the change stream |
|---|---|
| Compression | Transparent. Records describe namespace operations, not data blocks. |
| raidz, mirrors, draid | Transparent. vdev layout is below the ZPL where events originate. |
| slog (log vdev) | Affects transaction commit latency only; unrelated to namespace events. |
| L2ARC, special vdev | Transparent. |
| Dedup (dedup=on, fast dedup) | Transparent. A write to a deduped file still produces a modify event; dedup tables are not part of the stream. |
| Native encryption | The ZPL hands out decrypted names inside the kernel, so records contain plain paths while the dataset is mounted and the key loaded. If the key is unloaded the dataset cannot be mounted and no events are produced. |
| Quotas, reservations | Transparent. |
| Snapshots | Snapshot creation and access to .zfs/snapshot are not namespace operations on the live tree. Snapshots do not interfere with the stream. |
| Rollback, receive | These rewrite large parts of the tree. Events may be coalesced or missed; run reconcile afterwards. Consumers should treat it as a bulk change. |
| Clones | Each clone has its own mount; point a collector at each mount you care about. |
| Unmount, remount (cluster failover) | The collector does not pin the mount: unmounting succeeds while it runs, and after a remount the mark is re-armed automatically within refresh_interval_ms. Events during the unmounted window do not exist for the stream; reconcile after mount closes the gap. |
| NFS, SMB exports | Client-side writes reach the server kernel as ordinary syscalls and are collected. The collector must run on the server; fanotify cannot watch an NFS client mount. |

## Using it from AI pipelines

The typical shape: an AI sandbox keeps its data space in a ZFS
dataset, and derived state (a vector store, a search index, a feature
cache) must track it.

    # on the storage node
    sudo zchangelog -d /var/lib/zchangelog/data.db watch /pool/sandbox

    # in the indexing worker (can be a different host, via the journal
    # on shared storage or an API layer in front of it)
    zchangelog -d data.db register embeddings
    zchangelog -d data.db read -c embeddings -a -j

Each record carries op, path, timestamp and a monotonically
increasing seq. The worker applies the change to its index, then
acknowledges. After a crash it resumes from its cursor, and if the
journal declares a gap it resyncs, so the index never silently
diverges.

Worker policy for hot files: a file edited repeatedly produces a
modify event per write, and a write can still be in progress when
the event arrives. Read with `-C` to coalesce the batch to the last
state per path, apply a debounce window in the worker (embed a path
only after it stops producing events, and verify size and mtime are
stable before reading), and make the embedding an idempotent
upsert, since a crash between apply and acknowledge redelivers the
last records.

Feature interactions that matter for this use:

- Encrypted datasets: indexing works while the key is loaded (names
  are plain in the stream). Take care that the index does not leak
  content if that is a concern.
- Dedup and compression do not change the event shape, so pipelines
  behave identically across pools with different data settings.
- Send/recv based DR: the replica site runs its own collector on the
  received dataset; a full reindex after receive is the equivalent of
  a gap rescan.

## Known limits

- Directory creation is not always delivered on filesystem marks;
  reconcile closes the gap.
- mtime-based reconciliation cannot see writes that later reset
  mtime (restore tools). Use the fanotify stream as the primary
  source.
- NFS client mounts cannot be watched; run the collector on the
  server.

## Extension directions

- A kqueue based collector for FreeBSD (the journal, consumer API and
  CLI are backend agnostic).
- A backend that consumes the kernel changelog if such a primitive is
  accepted into OpenZFS.
- Output plugins per consumer (webhooks, Kafka, Redis streams,
  Prometheus metrics) beside the existing CLI and JSON.
- Pool-wide aggregation: one journal covering every dataset of a
  pool instead of one collector per mount.
