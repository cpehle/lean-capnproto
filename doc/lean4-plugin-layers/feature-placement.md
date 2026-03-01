# Lean4 Plugin Feature Placement Guide

Use this mapping when deciding where a change belongs.

- New schema-level syntax mapping:
  - Layer 0 (`capnpc-lean4`) and golden outputs in Layer 1 tests.
- New wire-format utility:
  - Layer 2 (`Capnp.Runtime`).
- Shared async helper or lifecycle combinator:
  - Layer 3 (`Capnp.Async`).
- New network primitive (TCP/UDP/HTTP/WebSocket):
  - Layer 4 (`Capnp.KjAsync` + `test/lean4/c/kj_async_bridge.cpp`) + Layer 7 tests.
- New RPC primitive (handlers/pipelining/topology):
  - Layer 5 (`Capnp.Rpc` + `test/lean4/c/rpc_bridge_runtime.cpp`) + Layer 7 tests.
- Mixed RPC + KJ ergonomics:
  - Layer 6 (`Capnp.RpcKjAsync`).
