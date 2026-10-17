# Roadmap ideas

Open extension directions, roughly in order of expected value. The
core contract (journal schema and the register/read/clear consumer
API) is stable; every item below is additive.

## Backends

- kqueue collector (FreeBSD): directory-descriptor watches instead of
  fanotify marks; per-directory registration management is the main
  work.
- kernel changelog backend: if a kernel change stream is accepted
  into OpenZFS, switch the source and gain same-txg immediacy and FID
  identity. The consumer API stays the same.
- eBPF based collector: an alternative source for kernels where
  fanotify is unavailable; same lossiness caveats as any live feed,
  so it would pair with reconciliation.

## Delivery

- Consumer output plugins: webhooks, Kafka, Redis streams, AMQP,
  files, stdout. The journal and cursors stay in zchangelog; plugins
  only transport records.
- A small HTTP API in front of the journal so remote consumers do not
  need filesystem access to it.
- Pool-wide aggregation: one journal covering every dataset of a pool
  instead of one collector per mount.

## Operations

- Prometheus metrics endpoint (records, lag per consumer, gap counts)
  beside the existing kstat-style counters.
- Log rotation and archival of trimmed records for consumers that
  want history beyond the retention bound.
- Per-dataset configuration inside one journal (several mounts, one
  collector process).

## Data model

- Object identity reporting (file handles) in records where the
  backend provides it, so consumers survive renames without
  path-based bookkeeping.
- Hierarchical events (a flag on directory records) so consumers can
  prune whole subtrees cheaply.
