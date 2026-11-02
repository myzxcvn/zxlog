# Changelog

## Unreleased — v0.7 integration
- Bounded SPSC record slots with validated `max_record_bytes` and `max_queue_bytes`, plus queue memory statistics; no partial records on overflow.
- v3 compact typed wire encoding with canonical ULEB128 and ZigZag; Python and C++ decoders preserve v1/v2 compatibility and CRC checks.
- Public `Sink` abstraction with FileSink retaining existing POSIX FileWriter/PathLease, ConsoleSink and user-supplied mode-specific Sinks.
- Optional shared Dispatcher with controlled drain/detach and no registry lock across Sink I/O. Default dedicated consumer remains unchanged.
- Preprocessor compile-time severity filtering with `zxlog_active_level` (0..6), including elimination of disabled ELF log sites.
- Three-mode GN/Ninja CI regression, enhanced protocol, resource-budget, Sink and shared-lifecycle tests.
- See [docs/v0.7-upgrade.md](docs/v0.7-upgrade.md) for compatibility, memory budgeting, limits and deployment.

## Unreleased (historical v0.4 work)

- Compose Logger from AdmissionState, AsyncRuntime, OutputBackend and Diagnostics, with state and operations owned together. Retain only runtime configuration and remove the unused wall-clock member. Preserve public API, wire formats, ordering and shutdown semantics; consumers must rebuild for the new object layout. Extend rollback and concurrent control/shutdown coverage.

- Derive per-executable IDs from the source fingerprint and linked ELF, finalize a retained runtime identity section, and reject unfinalized or mismatched IDs. Validate image/dictionary hashes during deployment packaging and emit matching manifests; keep existing log/dictionary wire formats readable.

- Add classified loss/error counters, thread-safe last_error(), output_failed(), and rate-limited backend error callbacks with reentrancy/exception isolation. Preserve the original file failure and verify explicit shutdown/recreate recovery using injected I/O failures.

- Flush pending low-volume output every 100 ms by default. Add independent flush/sync intervals and severity triggers, retaining explicit acknowledged flush barriers.

- Bound rendered text with max_text_bytes (64 KiB by default) and the file quota. Count rejected text separately without poisoning the writer or blocking later records.

- Encode each argument once into a bounded destination; reject oversize events without publishing partial records. Enforce matching, supported and noexcept LogAdapter conversions.

- Add `Logger::create()` and dual `ZX_LOG(level, ...)` / `ZX_LOG(logger, level, ...)` forms, accepting logger references and raw/unique/shared pointers. Evaluate logger expressions once and skip arguments for null, disabled or closed instances.
- Isolate queues, workers, sequence barriers, files and statistics per logger. Use bounded per-thread weak bindings keyed by unique instance IDs; release queues at shutdown and combine record admission/closing in one atomic state.
- Reserve normalized output paths and numeric archive families before opening files. Reject overlapping instances, active symlinks/hard-link aliases and RUNTIME internal path collisions. Roll back resource ownership on initialization failure.
- Add multi-logger integration checks in all three modes for isolation, TLS reclamation/capacity, concurrent shutdown, path conflicts, independent rotation, metadata stripping and C++/Python decoding.
- Unify logging calls as `ZX_LOG(level, ...)`, using `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, or `CRITICAL`. Remove the per-level `LOG_*` / `ZXLOG_*` macros and `ZXLOG_NO_SHORT_MACROS` switch. Preserve argument filtering, C++17 compile-time format checks, and build-time LogSite metadata extraction.

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
