import Capnp.Rpc
import Capnp.KjAsync

namespace Capnp

namespace Rpc
namespace Runtime

/-- Borrow an RPC runtime handle as a `Capnp.KjAsync.Runtime`. -/
@[inline] def asKjAsyncRuntime (runtime : Capnp.Rpc.Runtime) : Capnp.KjAsync.Runtime :=
  { handle := runtime.handle }

/--
Run an `IO` action using a borrowed `Capnp.KjAsync.Runtime` view of this RPC runtime.
The runtime ownership remains with `Capnp.Rpc.Runtime`.
-/
@[inline] def withKjAsyncRuntime (runtime : Capnp.Rpc.Runtime)
    (action : Capnp.KjAsync.Runtime -> IO α) : IO α :=
  action runtime.asKjAsyncRuntime

/--
Run a `Capnp.KjAsync.RuntimeM` action on this RPC runtime handle.
The runtime ownership remains with `Capnp.Rpc.Runtime`.
-/
@[inline] def runKjAsync (runtime : Capnp.Rpc.Runtime)
    (action : Capnp.KjAsync.RuntimeM α) : IO α :=
  Capnp.KjAsync.RuntimeM.run runtime.asKjAsyncRuntime action

/--
Start a KJ timer on this RPC runtime and expose it as a Lean task.
This keeps orchestration in KJ while still fitting Lean-side async APIs.
-/
@[inline] def sleepNanosAsTask (runtime : Capnp.Rpc.Runtime)
    (delayNanos : UInt64) : IO (Task (Except IO.Error Unit)) :=
  runtime.runKjAsync do
    let promise ← Capnp.KjAsync.RuntimeM.sleepNanosStart delayNanos
    promise.awaitAsTask

/-- Millisecond variant of `sleepNanosAsTask`. -/
@[inline] def sleepMillisAsTask (runtime : Capnp.Rpc.Runtime)
    (delayMillis : UInt32) : IO (Task (Except IO.Error Unit)) :=
  runtime.runKjAsync do
    let promise ← Capnp.KjAsync.RuntimeM.sleepMillisStart delayMillis
    promise.awaitAsTask

/-- Promise variant of `sleepNanosAsTask`. -/
@[inline] def sleepNanosAsPromise (runtime : Capnp.Rpc.Runtime)
    (delayNanos : UInt64) : IO (Capnp.Async.Promise Unit) := do
  pure (Capnp.Async.Promise.ofTask (← runtime.sleepNanosAsTask delayNanos))

/-- Promise variant of `sleepMillisAsTask`. -/
@[inline] def sleepMillisAsPromise (runtime : Capnp.Rpc.Runtime)
    (delayMillis : UInt32) : IO (Capnp.Async.Promise Unit) := do
  pure (Capnp.Async.Promise.ofTask (← runtime.sleepMillisAsTask delayMillis))

end Runtime

namespace RuntimeM

/-- Borrow the current RPC runtime handle as `Capnp.KjAsync.Runtime`. -/
@[inline] def kjAsyncRuntime : Capnp.Rpc.RuntimeM Capnp.KjAsync.Runtime := do
  return (← Capnp.Rpc.RuntimeM.runtime).asKjAsyncRuntime

/--
Run an `IO` action using a borrowed `Capnp.KjAsync.Runtime` view of the current RPC runtime.
The runtime ownership remains with the outer `Capnp.Rpc.Runtime`.
-/
@[inline] def withKjAsyncRuntime
    (action : Capnp.KjAsync.Runtime -> IO α) : Capnp.Rpc.RuntimeM α := do
  action (← kjAsyncRuntime)

/--
Run a `Capnp.KjAsync.RuntimeM` action on the current RPC runtime handle.
The runtime ownership remains with the outer `Capnp.Rpc.Runtime`.
-/
@[inline] def runKjAsync (action : Capnp.KjAsync.RuntimeM α) : Capnp.Rpc.RuntimeM α :=
  withKjAsyncRuntime fun runtime => Capnp.KjAsync.RuntimeM.run runtime action

/-- RuntimeM variant of `Capnp.Rpc.Runtime.sleepNanosAsTask`. -/
@[inline] def sleepNanosAsTask
    (delayNanos : UInt64) : Capnp.Rpc.RuntimeM (Task (Except IO.Error Unit)) := do
  let runtime ← Capnp.Rpc.RuntimeM.runtime
  runtime.sleepNanosAsTask delayNanos

/-- RuntimeM variant of `Capnp.Rpc.Runtime.sleepMillisAsTask`. -/
@[inline] def sleepMillisAsTask
    (delayMillis : UInt32) : Capnp.Rpc.RuntimeM (Task (Except IO.Error Unit)) := do
  let runtime ← Capnp.Rpc.RuntimeM.runtime
  runtime.sleepMillisAsTask delayMillis

/-- RuntimeM variant of `Capnp.Rpc.Runtime.sleepNanosAsPromise`. -/
@[inline] def sleepNanosAsPromise
    (delayNanos : UInt64) : Capnp.Rpc.RuntimeM (Capnp.Async.Promise Unit) := do
  let runtime ← Capnp.Rpc.RuntimeM.runtime
  runtime.sleepNanosAsPromise delayNanos

/-- RuntimeM variant of `Capnp.Rpc.Runtime.sleepMillisAsPromise`. -/
@[inline] def sleepMillisAsPromise
    (delayMillis : UInt32) : Capnp.Rpc.RuntimeM (Capnp.Async.Promise Unit) := do
  let runtime ← Capnp.Rpc.RuntimeM.runtime
  runtime.sleepMillisAsPromise delayMillis

end RuntimeM
end Rpc

end Capnp
