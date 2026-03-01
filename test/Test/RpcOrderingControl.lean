import LeanTest
import Capnp.Rpc
import Test.Common
import Capnp.Gen.test.lean4.fixtures.rpc_echo

open LeanTest
open Capnp.Gen.test.lean4.fixtures.rpc_echo

private def countTraceTag (trace : Array Capnp.Rpc.ProtocolMessageTraceTag)
    (tag : Capnp.Rpc.ProtocolMessageTraceTag) : Nat :=
  trace.foldl (fun acc entry => if entry == tag then acc + 1 else acc) 0

private def hasDisembargoBeforeResolve (trace : Array Capnp.Rpc.ProtocolMessageTraceTag) : Bool :=
  let (_, seenOutOfOrder) :=
    trace.foldl
      (fun (state : Bool × Bool) entry =>
        let seenResolve := state.fst
        let outOfOrder := state.snd
        match entry with
        | .resolve => (true, outOfOrder)
        | .disembargo => (seenResolve, outOfOrder || !seenResolve)
        | .unknown _ => (seenResolve, outOfOrder))
      (false, false)
  seenOutOfOrder

private def registerEchoFooCallOrderTarget (runtime : Capnp.Rpc.Runtime) :
    IO (Capnp.Rpc.Client × IO UInt64) := do
  let nextExpected ← IO.mkRef (UInt64.ofNat 0)
  let target ← runtime.registerHandlerTarget (fun _ method req => do
    if method.interfaceId != Echo.interfaceId || method.methodId != Echo.fooMethodId then
      throw (IO.userError
        s!"unexpected method in call-order target: {method.interfaceId}/{method.methodId}")
    let expected ← readUInt64Payload req
    let current ← nextExpected.get
    if expected != current then
      throw (IO.userError s!"call-order mismatch: expected {current}, got {expected}")
    nextExpected.set (current + (UInt64.ofNat 1))
    pure (mkUInt64Payload current))
  pure (target, nextExpected.get)

private def pumpUntil (runtime : Capnp.Rpc.Runtime) (label : String) (attempts : Nat)
    (check : IO Bool) : IO Unit := do
  let mut remaining := attempts
  while remaining > 0 do
    runtime.pump
    if (← check) then
      return ()
    remaining := remaining - 1
    IO.sleep (UInt32.ofNat 5)
  throw (IO.userError s!"{label}: timeout")

@[test]
def testRuntimeOrderingResolveHoldControlsDisembargo : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let (callOrder, getNextExpected) ← registerEchoFooCallOrderTarget runtime
    let (promiseCap, fulfiller) ← runtime.newPromiseCapability
    let promisedPayload := mkCapabilityPayload promiseCap
    let responder ← runtime.registerAdvancedHandlerTargetAsync (fun _ method _ => do
      if method.interfaceId != Echo.interfaceId || method.methodId != Echo.fooMethodId then
        throw (IO.userError
          s!"unexpected method in ordering-control target: {method.interfaceId}/{method.methodId}")
      let deferred ← Capnp.Rpc.Advanced.defer do
        pure (Capnp.Rpc.Advanced.respond promisedPayload)
      pure (Capnp.Rpc.Advanced.setPipeline promisedPayload deferred))

    runtime.orderingSetResolveHold true
    assertEqual (bubble (← runtime.orderingHeldResolveCount)) (UInt64.ofNat 0)

    let pending ← runtime.startCall responder Echo.fooMethod payload
    let pipelineCap ← pending.getPipelinedCap

    let call0Pending ← runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 0))
    let call1Pending ← runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 1))

    fulfiller.fulfill callOrder
    assertEqual (bubble (← runtime.orderingHeldResolveCount)) (UInt64.ofNat 1)

    assertEqual (bubble (← runtime.orderingFlushResolves)) (UInt64.ofNat 1)
    assertEqual (bubble (← runtime.orderingHeldResolveCount)) (UInt64.ofNat 0)
    pumpUntil runtime "pipelined capability resolve/disembargo" 128 do
      runtime.targetWhenResolvedPoll pipelineCap
    assertEqual (← runtime.targetWhenResolvedPoll pipelineCap) true

    let call0Response ← call0Pending.await
    let call1Response ← call1Pending.await
    assertEqual (← readUInt64Payload call0Response) (UInt64.ofNat 0)
    assertEqual (← readUInt64Payload call1Response) (UInt64.ofNat 1)
    assertEqual (← getNextExpected) (UInt64.ofNat 2)

    let response ← pending.await
    runtime.releaseCapTable response.capTable
    runtime.releaseTarget pipelineCap
    runtime.releaseTarget responder
    fulfiller.release
    runtime.releaseTarget promiseCap
    runtime.releaseTarget callOrder
    runtime.orderingSetResolveHold false
  finally
    runtime.shutdown
  where bubble (x : UInt64) := x

@[test]
def testRuntimeOrderingResolveHooksTrackHeldCount : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let echo ← runtime.registerEchoTarget
    let (promiseCap, fulfiller) ← runtime.newPromiseCapability
    runtime.orderingSetResolveHold true
    assertEqual (bubble (← runtime.orderingHeldResolveCount)) (UInt64.ofNat 0)
    assertEqual (← runtime.targetWhenResolvedPoll promiseCap) false

    fulfiller.fulfill echo
    assertEqual (bubble (← runtime.orderingHeldResolveCount)) (UInt64.ofNat 1)
    assertEqual (← runtime.targetWhenResolvedPoll promiseCap) false

    assertEqual (bubble (← runtime.orderingFlushResolves)) (UInt64.ofNat 1)
    assertEqual (bubble (bubble (← runtime.orderingHeldResolveCount))) (UInt64.ofNat 0)
    pumpUntil runtime "promise capability resolve poll" 64 do
      runtime.targetWhenResolvedPoll promiseCap
    assertEqual (← runtime.targetWhenResolvedPoll promiseCap) true

    fulfiller.release
    runtime.releaseTarget promiseCap
    runtime.releaseTarget echo
    runtime.orderingSetResolveHold false
  finally
    runtime.shutdown
  where bubble (x : UInt64) := x

@[test]
def testRuntimeProtocolResolveDisembargoMessageCounters : IO Unit := do
  let payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    runtime.orderingSetResolveHold true
    try
      let (callOrder, getNextExpected) ← registerEchoFooCallOrderTarget runtime
      let (promiseCap, fulfiller) ← runtime.newPromiseCapability
      let pipelinePayload := mkCapabilityPayload promiseCap
      let bobBootstrap ← runtime.registerAdvancedHandlerTargetAsync (fun _ method req => do
        if method.interfaceId != Echo.interfaceId || method.methodId != Echo.fooMethodId then
          throw (IO.userError
            s!"unexpected method in protocol counter target: {method.interfaceId}/{method.methodId}")
        let _ := req
        let deferred ← Capnp.Rpc.Advanced.defer do
          IO.sleep (UInt32.ofNat 20)
          fulfiller.fulfill callOrder
          pure (Capnp.Rpc.Advanced.respond pipelinePayload)
        pure (Capnp.Rpc.Advanced.setPipeline pipelinePayload deferred))

      let vatNetwork := runtime.vatNetwork
      let alice ← vatNetwork.newClient "alice-protocol-counts"
      let bob ← vatNetwork.newServer "bob-protocol-counts" bobBootstrap
      let aliceToBob ← alice.bootstrapPeer bob

      alice.resetResolveDisembargoTraceTo bob
      bob.resetResolveDisembargoTraceTo alice

      let pending ← runtime.startCall aliceToBob Echo.fooMethod payload
      let pipelineCap ← pending.getPipelinedCap
      let call0Pending ← runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 0))
      let call1Pending ← runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 1))

      let response ← pending.await
      runtime.releaseCapTable response.capTable

      let rec waitForHeldResolve (attempts : Nat) : IO Unit := do
        runtime.pump
        if (← runtime.orderingHeldResolveCount) > 0 then
          pure ()
        else
          match attempts with
          | 0 =>
              throw (IO.userError "expected held Resolve before flush in protocol counter test")
          | attempts + 1 =>
              IO.sleep (UInt32.ofNat 5)
              waitForHeldResolve attempts
      waitForHeldResolve 400

      assertTrue (← alice.resolveDisembargoTraceTo bob).isEmpty
        "expected empty resolve/disembargo trace while Resolve messages are held (alice -> bob)"
      assertTrue (← bob.resolveDisembargoTraceTo alice).isEmpty
        "expected empty resolve/disembargo trace while Resolve messages are held (bob -> alice)"

      let flushed ← runtime.orderingFlushResolves
      if flushed == 0 then
        throw (IO.userError "expected at least one held Resolve to flush")

      let call0Response ← call0Pending.await
      let call1Response ← call1Pending.await
      assertEqual (← readUInt64Payload call0Response) (UInt64.ofNat 0)
      assertEqual (← readUInt64Payload call1Response) (UInt64.ofNat 1)
      assertEqual (← getNextExpected) (UInt64.ofNat 2)

      let countsAliceToBob ← alice.resolveDisembargoCountsTo bob
      let countsBobToAlice ← bob.resolveDisembargoCountsTo alice
      let totalResolve := countsAliceToBob.resolveCount + countsBobToAlice.resolveCount
      let totalDisembargo := countsAliceToBob.disembargoCount + countsBobToAlice.disembargoCount
      if totalResolve == 0 then
        throw (IO.userError
          s!"expected at least one Resolve message, got alice->bob={repr countsAliceToBob}, bob->alice={repr countsBobToAlice}")

      let traceAliceToBob ← alice.resolveDisembargoTraceTo bob
      let traceBobToAlice ← bob.resolveDisembargoTraceTo alice
      assertEqual (hasDisembargoBeforeResolve traceAliceToBob) false
      assertEqual (hasDisembargoBeforeResolve traceBobToAlice) false
      let traceResolveCount :=
        UInt64.ofNat
          ((countTraceTag traceAliceToBob .resolve) + (countTraceTag traceBobToAlice .resolve))
      let traceDisembargoCount :=
        UInt64.ofNat
          ((countTraceTag traceAliceToBob .disembargo) + (countTraceTag traceBobToAlice .disembargo))
      assertEqual traceResolveCount totalResolve
      assertEqual traceDisembargoCount totalDisembargo

      alice.resetResolveDisembargoTraceTo bob
      bob.resetResolveDisembargoTraceTo alice
      assertTrue (← alice.resolveDisembargoTraceTo bob).isEmpty
        "expected empty resolve/disembargo trace after reset (alice -> bob)"
      assertTrue (← bob.resolveDisembargoTraceTo alice).isEmpty
        "expected empty resolve/disembargo trace after reset (bob -> alice)"

      runtime.releaseTarget pipelineCap
      runtime.releaseTarget aliceToBob
      alice.release
      bob.release
      runtime.releaseTarget bobBootstrap
      fulfiller.release
      runtime.releaseTarget promiseCap
      runtime.releaseTarget callOrder
    finally
      runtime.orderingSetResolveHold false
  finally
    runtime.shutdown

@[test]
def testRuntimeProtocolNullPipelineDoesNotEmitDisembargo : IO Unit := do
  let payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let vatNetwork := runtime.vatNetwork
    let bobBootstrap ← runtime.registerHandlerTarget (fun _ _ _ => pure mkNullPayload)
    let bob ← vatNetwork.newServer "bob-null-disembargo" bobBootstrap
    let alice ← vatNetwork.newClient "alice-null-disembargo"
    let aliceToBob ← alice.bootstrapPeer bob

    let baselineAliceToBob ← alice.resolveDisembargoCountsTo bob
    let baselineBobToAlice ← bob.resolveDisembargoCountsTo alice

    alice.resetResolveDisembargoTraceTo bob
    bob.resetResolveDisembargoTraceTo alice

    let pending ← runtime.startCall aliceToBob Echo.fooMethod payload
    let nullPipelineCap ← pending.getPipelinedCap

    let firstResult ← runtime.callResult nullPipelineCap Echo.fooMethod payload
    match firstResult with
    | .ok _ =>
        throw (IO.userError "expected first null pipelined call to fail")
    | .error _ =>
        pure ()

    let response ← pending.await
    assertEqual (Capnp.isNullPointer (Capnp.getRoot response.msg)) true
    assertEqual response.capTable.caps.size 0

    let secondResult ← runtime.callResult nullPipelineCap Echo.fooMethod payload
    match secondResult with
    | .ok _ =>
        throw (IO.userError "expected second null pipelined call to fail")
    | .error _ =>
        pure ()

    let healthCheck ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callBarM aliceToBob payload
    assertEqual healthCheck.capTable.caps.size 0

    runtime.pump
    runtime.pump

    let afterAliceToBob ← alice.resolveDisembargoCountsTo bob
    let afterBobToAlice ← bob.resolveDisembargoCountsTo alice
    let baselineDisembargo := baselineAliceToBob.disembargoCount + baselineBobToAlice.disembargoCount
    let afterDisembargo := afterAliceToBob.disembargoCount + afterBobToAlice.disembargoCount
    assertEqual afterDisembargo baselineDisembargo

    let traceAliceToBob ← alice.resolveDisembargoTraceTo bob
    let traceBobToAlice ← bob.resolveDisembargoTraceTo alice
    assertEqual (countTraceTag traceAliceToBob .disembargo) 0
    assertEqual (countTraceTag traceBobToAlice .disembargo) 0

    runtime.releaseTarget nullPipelineCap
    runtime.releaseTarget aliceToBob
    alice.release
    bob.release
    runtime.releaseTarget bobBootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeProtocolBlockingOrdering : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let vatNetwork := runtime.vatNetwork
    
    -- Setup callOrder target locally.
    let (callOrder, getNextExpected) ← registerEchoFooCallOrderTarget runtime

    -- Bob is a server that uses callOrder as its bootstrap.
    let bob ← vatNetwork.newServer "bob" callOrder

    -- Alice is a client.
    let alice ← vatNetwork.newClient "alice"
    let aliceToBob ← alice.bootstrapPeer bob

    -- Block Alice -> Bob.
    alice.blockConnectionTo bob

    -- Alice starts calls to Bob.
    let call0Pending ← runtime.startCall aliceToBob Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 0))
    let call1Pending ← runtime.startCall aliceToBob Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 1))

    -- Verify they are indeed blocked.
    let mut attempts := 0
    while (bubble (← getNextExpected)) == 0 && attempts < 50 do
      runtime.pump
      IO.sleep (UInt32.ofNat 5)
      attempts := attempts + 1
    assertEqual (bubble (← getNextExpected)) (UInt64.ofNat 0)

    -- Now unblock Alice -> Bob.
    alice.unblockConnectionTo bob

    -- Wait for the calls to reach Bob and trigger the callOrder handler.
    pumpUntil runtime "wait for blocked calls after unblock" 200 do
      return (bubble (← getNextExpected)) == 2

    let res0 ← call0Pending.await
    let res1 ← call1Pending.await
    assertEqual (bubble (← readUInt64Payload res0)) (UInt64.ofNat 0)
    assertEqual (bubble (bubble (← readUInt64Payload res1))) (UInt64.ofNat 1)

    runtime.releaseTarget aliceToBob
    runtime.releaseTarget callOrder
    alice.release
    bob.release
  finally
    runtime.shutdown
  where bubble (x : UInt64) := x
