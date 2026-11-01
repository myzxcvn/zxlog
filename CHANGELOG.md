# Changelog

## v0.4.0 (2026-11-01)

- Preserve `LOG_*` API and C++17 standard; support TEXT/BINARY/RUNTIME backend policies.
- Replace producer-side mutex MPSC with bounded per-thread SPSC rings, atomic tickets and consumer-ordered control barriers. Recycle retired ring slots after draining.
- Add MIT-licensed {fmt} 12.1.0 for C++17 compile-time format checking and runtime text backend. Support alignment, explicit positional arguments, dynamic width, integer bases, floating-point format specs. Add simple scalar `LogAdapter<T>` interface.
- Introduce v2 wire records with CRC32 per event and CRC32 per 64-byte file header; dictionary remains v1 and Python decoder supports legacy v1.
- Add batched POSIX writer, bounded rotation, archive pruning, explicit `flush(true)` fdatasync, directory metadata sync, torn-record truncation on next startup, and conservative handling for invalid existing files.
- Offline `--recover` mode scans past CRC-corrupted events and skips torn tails; default strict mode fails with diagnostic. Added C++ `zxlog_decode` that reuses the runtime {fmt} formatter for exact reconstruction, alongside the lightweight Python decoder.
- Extend integration verification: corrupted records, legacy migration, torn headers, recovery after restart, thread churn, quotas, four-thread stress, compile-negative format checking, metadata stripping.
- Collect and retain template-local LogSite metadata with `build/zxlog_meta.ld`, including GCC 12's `.rodata.*` fallback. Resolve integration-test executable paths before launching subprocesses. All three GN modes now pass the full `check` target with GCC 12.3.0.

### Intentional compatibility changes

- Binary data protocol bumped `ZXLOG01` → `ZXLOG02` (v1 decoder retained).
- Runtime options `queue_slots` replaced by `max_producers` and `per_thread_slots`, plus file retention/sync options.
- TXT/RUNTIME configurations now depend on bundled fmt headers; Binary-only still has no formatter linked into the deployment image.

### Current limitations

- No guarantees for queued records not persisted before abrupt shutdown; no signal-handler emergency path.
- No multi-process shared file support and no hard realtime determinism.
- Binary custom schemas limited to scalar `LogAdapter<T>` representation; Python formatter covers common scalar {fmt} syntax.
