# Lean 4 Plugin Code Review Tracker

Last updated: 2026-02-28
Scope: FFI and Runtime Structure Refactoring

Status legend:
- `[ ]` open
- `[~]` in progress
- `[x]` completed

## Working Set (Current)

- [ ] `API-01` Refactor `Capnp.KjAsync` and `Capnp.Rpc` to remove boilerplate method duplication (e.g. `connect`, `connectStart`, `connectAsTask`).
- [ ] `API-02` Implement a Lean `macro` or meta-program to auto-generate standard async variants (Promise, Task, Sync) from a single FFI definition.
- [ ] `GEN-01` Refactor `capnpc-lean4.c++` to extract string-concatenation logic into a separate template builder class.

## Open Issues

### High

- [ ] **Lean API Repetition (`Capnp.KjAsync` & `Capnp.Rpc`):** There are over 500 explicit definition wrappers manually routing calls to the FFI, defining 4-5 variants per method (`Foo`, `FooStart`, `FooAsTask`, `FooAsPromise`, `FooAwait`).
  - *Action:* Execute `API-01` and `API-02`.

### Medium

- [ ] **Compiler Plugin Code Generation Boilerplate:** The `capnpc-lean4.c++` compiler plugin is monolithic and heavily relies on manual string concatenation (over 2000 lines of `out += ...`).
  - *Action:* Introduce a lightweight C++ template engine or AST builder class to cleanly separate Lean 4 syntax generation from Cap'n Proto schema traversal.

### Pending Architectural Re-evaluation

- **FFI Boilerplate (Reverted):** Initial plan to use C++ macros to reduce repetitive `extern "C"` FFI wrappers was reverted, as heavy macro usage obfuscates C++ code and makes it harder to debug.

## Completed (This Pass)

- [x] `FFI-01` Create a dedicated `native/` or `ffi/` directory at the project root for production C++ bridge code.
- [x] `FFI-02` Move `kj_async_bridge.cpp`, `rpc_bridge_runtime.cpp`, and other core FFI bridges out of `test/lean4/c/` to the new production directory.
- [x] `FFI-03` Update `lakefile.lean` to compile the relocated FFI bridges as part of the core library, separating them from the test harness.
- [x] `FFI-04` Update CMake and Bazel configurations across the codebase to reflect the new paths of the FFI bridge files.
- [x] `FFI-05` Design and implement a shared Lean abstraction to encapsulate repetitive `runtimeHandle` and `raw` / `handle` fields.
- [x] `FFI-06` Refactor `lean/Capnp/KjAsync.lean` to replace repetitive structure definitions with the new shared FFI handle abstraction.
- [x] `FFI-07` Refactor `lean/Capnp/Rpc.lean` to adopt the shared FFI handle abstraction.
- [x] `FFI-08` Verify that FFI boundary semantics remain intact after refactoring by running the full parity matrix and test suite.

### Action Plan Refinement

- **Lean API Boilerplate:** Rather than heavy C++ macro usage, the Lean API boilerplate can be reduced by using Lean 4's powerful macro system to automatically derive `...AsTask`, `...AsPromise`, and synchronous wrappers from the base `...Start` function. This keeps the C++ side readable while eliminating Lean repetition.

- **Compiler String Concatenation:** For `capnpc-lean4.c++`, refactoring `std::string out` to use a dedicated `LeanCodeBuilder` class (which manages indentation, blocks, and syntax generation internally) is the safest first step.
