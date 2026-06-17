# Lean4 Plugin Layer Stack

This document maps the current Lean4 backend/plugin stack by implementation layer.

| Layer | Primary files | Responsibility | Depends on |
| --- | --- | --- | --- |
| 0. Cap'n Proto plugin frontend | `c++/src/capnp/compiler/capnpc-lean4.c++` | Reads `schema::CodeGeneratorRequest`, resolves schema graph, emits Lean source files under `Capnp/Gen/...`. | Cap'n Proto schema loader and compiler internals |
| 1. Generated Lean schema modules | `test/out/Capnp/Gen/...` (optional golden files under `test/expected/...`) | Typed schema accessors/builders/method metadata generated from `.capnp` definitions. | `Capnp.Runtime`, `Capnp.Rpc` (for capability schemas) |
| 2. Core wire/runtime library | `lean/Capnp/Runtime.lean` | Pure Lean message/segment model, reader/builder types, packing/unpacking helpers, cap-table envelope primitives. | Lean stdlib |
| 3. Generic async abstraction | `lean/Capnp/Async.lean` | Backend-agnostic async traits (`Awaitable`, `Cancelable`, `Releasable`) and combinator helpers (`Promise`, `withRelease`, etc.). | Lean `Task` / `IO.Promise` |
| 4. KJ async/network binding layer | `lean/Capnp/KjAsync.lean`, `ffi/kj_async_bridge.cpp` | Lean surface for KJ runtime loop, promises, TCP/UDP, HTTP/WebSocket, stream/payload refs, timeout/cancel/release controls. | KJ async + compat HTTP C++ APIs, Layer 3 |
| 5. RPC binding layer | `lean/Capnp/Rpc.lean`, `ffi/rpc_bridge_runtime.cpp` | Lean surface for Cap'n Proto RPC runtime, client/server handles, pending calls, pipelining, advanced handler controls, multi-vat primitives. | Cap'n Proto RPC C++ runtime, Layer 2, Layer 3 |
| 6. RPC/KJ runtime bridge | `lean/Capnp/RpcKjAsync.lean` | Shared-runtime helpers so RPC and KJ async operations compose on one runtime handle/event loop. | Layer 4, Layer 5 |
| 7. Validation and parity harness | `test/Test/*.lean`, `test/TestDriverRpc.lean`, `test/parity_matrix.json`, `test/scripts/validate_parity_matrix.py` | Regression tests, interop tests, parity-critical selection, and matrix validation against C++ behavior classes. | All runtime layers |
