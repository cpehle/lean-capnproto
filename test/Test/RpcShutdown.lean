import LeanTest
import Capnp.Rpc
import Capnp.KjAsync
import Test.Common
import Capnp.Gen.fixtures.rpc_echo

open LeanTest
open Capnp.Gen.fixtures.rpc_echo

@[test]
def testRuntimeAdvancedHandlerRejectsRpcShutdownOnWorkerThread : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerEchoTarget
    let seenError ← IO.mkRef ""
    let forwarder ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      let errMsg ←
        try
          runtime.shutdown
          pure ""
        catch err =>
          pure (toString err)
      seenError.set errMsg
      pure (Capnp.Rpc.Advanced.forward sink method req))
    let _ ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM forwarder payload
    let errMsg ← seenError.get
    if !(errMsg.containsSubstr "not allowed from the Capnp.Rpc worker thread") then
      throw (IO.userError s!"missing worker-thread shutdown rejection text: {errMsg}")
    assertEqual (← runtime.isAlive) true
    runtime.releaseTarget forwarder
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerRejectsKjAsyncShutdownOnWorkerThread : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerEchoTarget
    let seenError ← IO.mkRef ""
    let forwarder ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      let kjRuntime : Capnp.KjAsync.Runtime := { handle := runtime.handle }
      let errMsg ←
        try
          kjRuntime.shutdown
          pure ""
        catch err =>
          pure (toString err)
      seenError.set errMsg
      pure (Capnp.Rpc.Advanced.forward sink method req))
    let _ ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM forwarder payload
    let errMsg ← seenError.get
    if !(errMsg.containsSubstr "not allowed from the Capnp.Rpc worker thread") then
      throw (IO.userError s!"missing KjAsync worker-thread shutdown rejection text: {errMsg}")
    assertEqual (← runtime.isAlive) true
    runtime.releaseTarget forwarder
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testKjAsyncShutdownReleasesRpcRuntimeHandle : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  let kjRuntime : Capnp.KjAsync.Runtime := { handle := runtime.handle }
  assertEqual (← runtime.isAlive) true
  kjRuntime.shutdown
  assertEqual (← runtime.isAlive) false
  runtime.shutdown

/-- Start both native event-loop implementations concurrently and reuse their
storage repeatedly. The worker must not observe members before construction
finishes, and each initialized loop must execute queued work and stop cleanly. -/
@[test]
def testRuntimeConcurrentStartupAndShutdown : IO Unit := do
  let mut workers := #[]
  try
    for _ in [:4] do
      let worker ← IO.asTask (prio := .dedicated) do
        for _ in [:64] do
          let rpc ← Capnp.Rpc.Runtime.init
          try
            assertEqual (← rpc.isAlive) true
            rpc.pump
          finally rpc.shutdown
          let kj ← Capnp.KjAsync.Runtime.init
          try
            assertEqual (← kj.isAlive) true
            kj.sleepMillis 0
          finally kj.shutdown
      workers := workers.push worker
    for worker in workers do IO.ofExcept (← IO.wait worker)
  finally
    for worker in workers do let _ ← IO.wait worker
