# Lean4 RPC Backend Plan (C++ Parity)

This plan tracks closure to C++ RPC behavior parity and keeps implementation priorities explicit.

## Goal

Deliver a production-ready Lean RPC stack with:
- C++-shape runtime semantics (two-party first, explicit lifecycle)
- advanced handler control surface for Lean-implemented capabilities
- first-class async composition (`KjAsync` + `Capnp.Async`) without semantic drift
- clear parity matrix against C++ behavior classes

Related roadmap:
- `doc/lean4-kjasync-goals.md` tracks additional RPC-independent KJ async/runtime
  surface goals for Lean.

## Status Snapshot (2026-02-23)

Implemented baseline:
- explicit runtime lifecycle (`Runtime.init`, `Runtime.shutdown`, `RuntimeM.runWithNewRuntime`)
- two-party client/server handles (`newClient`, `newServer`, bootstrap/listen/accept/drain)
- disconnect and lifecycle APIs (`onDisconnect`, release APIs, retain/release semantics)
- pending-call pipelining (`startCall`, `pendingCallGetPipelinedCap`, await/release)
- advanced handler controls:
  - deferred async handler replies
  - `sendResultsTo` caller forwarding
  - call hints (`noPromisePipelining`, `onlyPromisePipeline`)
  - `setPipeline` controls and validation
  - typed remote exceptions with detail payloads
- parity-oriented ordering tests for resolve/disembargo/tail-call classes
- Lean<->C++ interop tests (both client and server directions)
- full parity matrix + parity-critical sync validation in CI
- trace encoder support:
  - enable/disable toggles
  - custom callback (`setTraceEncoder`)
- KJ async networking/runtime surface exposed in Lean (`tcp`, `udp/datagram`, `http`, `websocket`)

## Parity Closure Checklist

Legend: `[x]` done, `[~]` partial, `[ ]` open.

- [x] Core two-party runtime lifecycle and handles.
- [x] Core call/return/cancel/disconnect semantics.
- [x] Promise pipelining (`startCall` + pipelined caps).
- [x] Tail-call forwarding controls.
- [x] Resolve/disembargo behavior classes (Lean parity tests present).
- [x] Advanced call-context controls (`sendResultsTo`, call hints, deferred replies, validation).
- [x] Structured remote exception propagation (type + detail payloads).
- [x] Trace encoder toggles and callback surface.
- [x] Multi-vat basics (bootstrap, sturdy refs, forwarding stats, connection checks).
- [x] Full parity matrix coverage against `rpc-test.c++` / `rpc-twoparty-test.c++` behavior classes tracked in Lean CI.
- [~] Streaming + FD parity coverage across platforms (implemented, but CI/platform gating still needs formal closure).
- [ ] GC-backed foreign object ownership to remove manual release burden in public APIs.
- [ ] Typed promised-capability codegen surface (`Rpc.Promise`-style generated ergonomics).
- [ ] Zero-copy message/data paths in hot RPC/KJ async flows.
- [ ] C++ bridge modularization (split large bridge translation units into focused modules).

## Open Gaps (Priority Order)

1. Lifecycle ergonomics (deep integration track).
- Current public API still requires explicit release patterns in many paths.
- Move toward GC-backed wrappers/foreign objects where practical.
- Keep explicit runtime initialization/shutdown semantics unchanged.

2. Zero-copy/performance track.
- Current RPC/KJ async payload flows are ByteArray-centric.
- Add low-risk zero-copy paths first (borrowing/slice-view style where ownership is clear).
- Add benchmark gates for before/after validation.

3. Bridge maintainability.
- `rpc_bridge_runtime.cpp` and `kj_async_bridge.cpp` remain large and hard to reason about.
- Refactor into modules by concern (runtime lifecycle, advanced handlers, promises, transport).
- Preserve ABI and test behavior during split.

4. Networking scope boundaries and documentation.
- Document exact supported HTTP method surface and transport capabilities.
- Document unsupported/intentional exclusions and expected error modes.

## Networking Scope (Current)

Lean `KjAsync` currently exposes:
- TCP stream connect/listen/read/write helpers.
- UDP datagram bind/send/receive (including promise/task forms).
- HTTP client/server helpers (including streaming request/response bodies).
- WebSocket client/server messaging helpers.

HTTP method enum currently includes:
- `GET`, `HEAD`, `POST`, `PUT`, `DELETE`, `PATCH`, `PURGE`, `OPTIONS`, `TRACE`
- `COPY`, `LOCK`, `MKCOL`, `MOVE`, `PROPFIND`, `PROPPATCH`, `SEARCH`, `UNLOCK`, `ACL`
- `REPORT`, `MKACTIVITY`, `CHECKOUT`, `MERGE`, `MSEARCH`, `NOTIFY`, `SUBSCRIBE`
- `UNSUBSCRIBE`, `QUERY`, `BAN`

Out-of-scope or not yet explicitly planned for parity:
- `CONNECT` handling (KJ models this separately from `HttpMethod`)
- protocol features not represented in the current KJ bridge surface

## Milestones and Exit Criteria

### M0: Plan Hygiene (Immediate)

- Update this plan on every parity-affecting merge.
- Add a machine-readable parity checklist artifact in `test/lean4/`.

Exit criteria:
- every parity claim in this document links to concrete Lean test coverage.

### M1: Parity Matrix Closure

- Enumerate all targeted behavior classes from `rpc-test.c++` / `rpc-twoparty-test.c++`.
- Mark each class as implemented/tested/blocked.
- Add missing parity tests for uncovered critical classes.

Exit criteria:
- all P0/P1 behavior classes mapped and passing in Lean CI.

M1 seed matrix status (`2026-02-23`):
- [x] Explicit behavior-class matrix exists in `test/lean4/parity_matrix.json`.
- [x] Core call/return + release/cancel class is `covered`.
- [x] Ordering-sensitive class is `covered`.
- [x] Transport connect/listen/disconnect class is `covered`.
- [x] Reliability abort/failure-propagation class is `covered`.
- [x] Flow control + trace observability class is `covered`.
- [x] Streaming + FD transfer class is `covered`.

Behavior-class mapping table:

| Behavior class | C++ RPC references | Lean tests | Status | Notes |
| --- | --- | --- | --- | --- |
| Core call/return and release/cancel semantics | `rpc-test.c++`: `basics`, `release capability`, `release capabilities when canceled during return`, `cancellation` | `testRpcReleaseMessageRoundtrip`, `testRpcReturnCanceled`, `testRuntimePendingCallRelease`, `testRuntimeParityAdvancedDeferredReleaseWithoutAllowCancellation` | `covered` | Message-level and runtime-level release/cancel semantics are exercised. |
| Ordering-sensitive pipelining/resolve/disembargo/tail-call | `rpc-test.c++`: `pipelining`, `resolve promise`, `embargo`, `embargo error`, `don't embargo null capability`, `tail call`; `rpc-twoparty-test.c++`: `Two-hop embargo` | `testRuntimeParityResolvePipelineOrdering`, `testRuntimeParityDisembargoNullPipelineDoesNotDisconnect`, `testRuntimeParityEmbargoErrorKeepsConnectionAlive`, `testRuntimeParityTailCallPipelineOrdering`, `testRuntimeParityAdvancedDeferredSetPipelineOrdering`, `testRuntimeTwoHopPipelinedResolveOrdering`, `testRuntimeTwoHopPipelinedResolveOrderingWithNestedPromise`, `testRuntimeProtocolResolveDisembargoMessageCounters`, `testRuntimeProtocolNullPipelineDoesNotEmitDisembargo` | `covered` | Ordering-sensitive classes are covered with runtime and protocol-trace assertions, including null-pipeline and embargo-error paths. |
| Transport connect/listen/disconnect lifecycle | `rpc-test.c++`: `loopback bootstrap()`, `clean connection shutdown`, `connections set idle when appropriate` | `testRuntimeAsyncClientLifecyclePrimitives`, `testRuntimeClientOnDisconnectAfterServerRelease`, `testRuntimeDisconnectVisibilityViaCallResult`, `testInteropLeanClientObservesCppDisconnectAfterOneShot` | `covered` | Lean runtime and interop tests cover connect/bootstrap/disconnect visibility and cleanup. |
| Reliability abort and failure propagation | `rpc-test.c++`: `abort`, `call promise that later rejects`, `handles exceptions thrown during disconnect`, `disconnection exception retains details`, `method throws exception with detail` | `testRuntimeParityCancelDisconnectSequencing`, `testRuntimeParityPromisedCapabilityDelayedRejectPropagatesToPendingCalls`, `testRuntimeParityPromisedCapabilityCancelBeforeRejectSequencing`, `testRuntimeTransportAbortPendingCall`, `testRuntimeProtocolDisconnectDetail`, `testInteropLeanClientCancelsPendingCallToCppDelayedServer`, `testInteropLeanPendingCallOutcomeCapturesCppException`, `testInteropLeanClientReceivesCppExceptionDetail` | `covered` | Abort/cancel/failure propagation and remote exception detail retention are covered in both Lean runtime tests and Lean<->C++ interop tests. |
| Flow control and trace observability | `rpc-test.c++`: `connections set idle when appropriate`, `method throws exception with trace encoder` | `testRuntimeClientQueueMetrics`, `testRuntimeClientQueueMetricsPreAcceptBacklogDrains`, `testRuntimeProtocolDiagnostics`, `testRuntimeClientSetFlowLimit`, `testRuntimeTraceEncoderToggle`, `testRuntimeSetTraceEncoderOnExistingConnection`, `testRuntimeTraceEncoderCallResultVisibility` | `covered` | Queue/flow/trace and internal diagnostics surfaces are covered with direct runtime checks. |
| Advanced streaming and FD transfer | `rpc-twoparty-test.c++`: `Streaming over RPC`, `Streaming over RPC no premature cancellation when client dropped`, `send FD over RPC`, `FD per message limit` | `testRuntimeStreamingCall`, `testRuntimeRegisterStreamingHandlerTarget`, `testRuntimeStreamingCancellation`, `testRuntimeStreamingNoPrematureCancellationWhenTargetDropped`, `testRuntimeStreamingForwardedAcrossMultiVatNoPrematureCancellation`, `testRuntimeStreamingChainedBackpressure`, `testRuntimeFdPassingOverNetwork`, `testRuntimeFdPerMessageLimitDropsExcessFds`, `testRuntimeFdTargetLocalGetFd` | `covered` | Streaming semantics (including no-premature-cancel and forwarding/backpressure) plus FD transfer and per-message limits are covered. |

### M2: Lifecycle Ergonomics

- Prototype GC-backed wrapper path for selected handles (`Client` first).
- Reduce required manual release calls in high-level APIs.
- Preserve explicit runtime ownership boundaries.

Exit criteria:
- representative app paths run without manual release calls for common capability usage.

### M3: Zero-Copy + Perf

- Identify top copy hotspots in RPC/KJ async bridging.
- Add low-risk zero-copy variants with clear ownership semantics.
- Benchmark and regress-test these paths.

Exit criteria:
- measurable copy reduction and latency/throughput improvement on benchmark scenarios.

### M4: Bridge Modularization

- Split C++ bridge code by subsystem with unchanged external ABI.
- Keep Lean extern signatures stable during refactor.

Exit criteria:
- bridge code is modular, parity tests still pass, no ABI regressions.

### M5: CI and Docs Lock-In

- Gate parity-critical tests in CI for Linux and macOS.
- Publish user-facing docs for recommended Lean RPC/KjAsync patterns.
- Deterministic parity command: `cd test/lean4 && lake test -- --parity-critical`.

Exit criteria:
- deterministic parity CI and documented supported feature matrix.

## Immediate Next Slice

1. Continue M2 with a narrow `Client` lifecycle ergonomics prototype while keeping runtime lifecycle explicit.
2. Push M3 zero-copy paths deeper into RPC payload and hot HTTP/WebSocket read/write paths.
3. Split bridge implementation units by subsystem while preserving extern ABI and passing parity suite.
