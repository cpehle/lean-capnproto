# Deep Integration Plan: Lean 4 & Cap'n Proto

This document outlines the roadmap for moving the Lean 4 Cap'n Proto backend from a raw FFI handle-based system to a "Deep Integration" model. This model prioritizes ergonomic, safe, and idiomatic Lean usage by leveraging the Garbage Collector for lifetime management and bridging Cap'n Proto's `kj::Promise` with Lean's async runtime.

## Goals

1.  **Safety & Ergonomics**: Remove manual `release` calls. Use Lean's GC to manage C++ object lifecycles automatically.
2.  **Async Efficiency**: Replace blocking FFI calls with non-blocking integrations that play nicely with Lean's `IO.Promise` and task scheduler.
3.  **Explicit Pipelining**: Provide a distinct type system representation for "Promised Capabilities" to enable type-safe, eager pipelining.

## Phase 1: Native Object Lifecycle (The "Foreign Object" Layer)

Currently, capabilities are tracked by integer handles (`UInt32`). We will replace this with Lean's `External` object interface.

### 1.1 C++ Wrappers
*   Define `ClientWrapper` struct in C++ that holds `kj::Own<capnp::ClientHook>`.
*   Implement `Client_finalize` function that runs when the Lean object is garbage collected. This function will trigger the reference count decrement (and eventual release) in the C++ `RuntimeLoop`.

### 1.2 Lean Runtime Updates
*   Change the definition of `Client` in `Capnp.Rpc` from a simple type alias to an opaque non-scalar type backed by the external class.
*   Remove `Runtime.releaseTarget` from the public API.
*   Update `Runtime` methods to accept and return these opaque objects instead of `UInt32` IDs.

## Phase 2: Async Runtime Integration (`kj::Promise` <-> `IO.Promise`)

Currently, the FFI uses `std::condition_variable` to block the calling thread until the C++ worker completes a task. This blocks a Lean worker thread. We will move to a callback-based approach.

### 2.1 The Promise Bridge
*   Instead of blocking, FFI calls will immediately return a Lean `IO.Promise`.
*   We will register a `.then()` continuation on the underlying `kj::Promise`.
*   When the `kj::Promise` resolves (success or exception), the C++ worker thread will invoke a thread-safe helper to resolve the Lean `IO.Promise`.

### 2.2 Execution Model
*   **Lean Side**: `let task <- IO.asTask (ffiCall ...)` becomes non-blocking. The Lean scheduler can park the task until the `IO.Promise` is resolved.
*   **C++ Side**: The `RuntimeLoop` continues to process events. When a result is ready, it pushes the resolution back to Lean.

## Phase 3: Pipelining & The `Promise` Type

We cannot treat a Lean `Task` transparently as a value. We will solve this by introducing a dedicated type for pipelining.

### 3.1 The `Rpc.Promise` Type
*   Define `structure Rpc.Promise (α : Type)`.
*   This structure holds a reference to the **Pending Call** (itself an External Object) and a **Pipeline Path** (list of ops).

### 3.2 Pipelined Method Calls
*   Generated code will produce instances of `Rpc.Promise Client`.
*   Invoking a method on `Rpc.Promise Client` will **not** wait for the promise to resolve. Instead, it creates a new `Call` message that targets the *pipeline* of the original call.
*   This maps directly to `pendingCallGetPipelinedCap` but is hidden behind the monadic interface.

## Phase 4: Implementation Sequence

1.  **Refactor `Client` to External Object**: Update `rpc_bridge_runtime` to register the external class and use it for `Client` objects. (High Priority)
2.  **Async Bridge**: Implement the `IO.Promise` resolution mechanism in C++. (High Priority)
3.  **Update Codegen**: Modify the compiler plugin to generate `Rpc.Promise` types and pipelining-aware stubs.
4.  **Refine `RuntimeM`**: Polish the monadic interface to make using these new features seamless.

## Next Steps

We will begin with **Phase 1**, converting the `Client` handle system to use Lean External Objects. This is the foundational change required for safe memory management in the subsequent async phases.
