# Lean4 KJ Async Exposure Goals

This document tracks KJ features worth exposing in Lean, beyond the current
`KjAsync` surface, with an emphasis on RPC parity and production usability.

## Scope

Goals here are RPC-independent KJ primitives and transport/runtime surfaces.
RPC-specific parity items remain tracked in `doc/lean4-rpc-plan.md`.

## Current Baseline (Implemented)

Lean already exposes:
- promise composition (`then`, `catch`, `all`, `race`)
- runtime lifecycle (`init`, `shutdown`) and scheduling helpers
- TCP stream helpers, UDP datagram helpers
- HTTP client/server and WebSocket helpers
- ByteView / payload-ref paths used by recent zero-copy passes

## Priority Roadmap

Legend: `[x]` done, `[~]` partial, `[ ]` open.

### P0: Core Async Model Parity

- [~] Event-loop / wait-scope / executor control surface.
  - Why: C++ code commonly composes RPC with non-RPC async work in one KJ loop.
  - Implemented now: explicit runtime-loop yielding/deadline helpers
    (`yieldNow`, `yieldNowStart`, `pumpNanos`, `sleepUntilMonoNanos`).
  - Remaining: explicit wait-scope/executor ownership handles.
- [~] Promise ergonomics as first-class Lean API.
  - Why: make `KjAsync.PromiseRef` the common abstraction, not ad-hoc task glue.
  - Target: keep combinators total and cancellation-aware; add typed helpers where safe.
- [ ] Cancellation and lifetime semantics doc + tests.
  - Why: parity depends on precise cancellation propagation and resource teardown.
  - Target: cancellation matrix tests for mixed RPC/non-RPC promise chains.

### P1: Low-Level Network and I/O Parity

- [~] Async stream interfaces (`AsyncInputStream`/`AsyncOutputStream`/`AsyncIoStream`).
  - Why: needed for parity with C++ code that works below HTTP/WebSocket helpers.
  - Implemented now: generic Lean stream abstractions (`Stream.ReadableRef`,
    `Stream.WritableRef`) with instances for `Connection`, `HttpRequestBody`,
    `HttpResponseBody`, `HttpServerRequestBody`, and `HttpServerResponseBody`.
  - Remaining: explicit `shutdown`/`end` harmonization surface and additional stream kinds.
- [ ] Address/provider surfaces (`Network`, `NetworkAddress`, parse/restrict features).
  - Why: RPC and raw networking both rely on precise address-level control.
  - Target: expose address parsing, protocol family selection, and policy controls.
- [~] FD/descriptor wrappers for attaching existing sockets/streams.
  - Why: enables integration with externally-created sockets and advanced embeddings.
  - Implemented now: `Runtime.wrapSocketFd` and `Runtime.wrapSocketFdTake`.
  - Remaining: listener/datagram fd wrappers and explicit low-level flags.

### P2: Runtime Breadth

- [ ] Unix signal and child-exit integration (`UnixEventPort` class features).
  - Why: needed for production runtime orchestration from Lean.
  - Target: minimal safe API for signal subscriptions and subprocess completion events.
- [ ] Rich timer/deadline primitives.
  - Why: many protocol deadlines should use KJ timers directly rather than ad-hoc sleep.
  - Target: deadline/cancel/timeout helpers with explicit ownership.

### P3: Compat Layer Expansion

- [ ] TLS identity/certificate callback parity for advanced deployments.
- [ ] HTTP CONNECT and protocol corners currently omitted in Lean method surface.
- [ ] Optional future adapters (filesystem/compression/encoding) only after P0-P2.

## Zero-Copy Policy

- Prefer borrow/view-based APIs over `ByteArray` in hot paths.
- Avoid caching-based approaches; prioritize explicit ownership and predictable lifetimes.
- Add zero-copy variants only where semantics are clear and testable.

## Test and Acceptance Gates

For each new surface:
- API tests in `test/lean4/Test/KjAsync.lean` (unit shape + error behavior)
- integration tests with C++ bridge where lifetimes or ownership cross FFI
- parity notes linked back to the relevant C++ header surface (`kj/async.h`,
  `kj/async-io.h`, `kj/compat/http.h`, platform-specific runtime pieces)

Release gate for this roadmap:
- Linux + macOS passing for the added KjAsync test matrix
- no unresolved lifetime leaks in bridge tests
- explicit docs for ownership and cancellation semantics for every new primitive
