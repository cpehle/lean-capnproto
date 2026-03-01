import Capnp.Rpc
import LeanTest

open LeanTest

@[test]
def testRuntimeAsyncPump : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let task ← runtime.pumpAsTask
    let result ← IO.wait task
    match result with
    | .ok () => pure ()
    | .error err => throw err
  finally
    runtime.shutdown
