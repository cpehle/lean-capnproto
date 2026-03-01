import LeanTest
import Capnp.Rpc
import Capnp.RpcKjAsync
import Test.Common
import Capnp.Gen.test.lean4.fixtures.rpc_echo

open LeanTest
open Capnp.Gen.test.lean4.fixtures.rpc_echo

private def waitTaskWithin {α : Type} (runtime : Capnp.Rpc.Runtime) (label : String)
    (task : Task (Except IO.Error α))
    (timeoutMillis : UInt32 := UInt32.ofNat 3000) : IO (Option (Except IO.Error α)) := do
  let timeoutTask ← runtime.sleepMillisAsTask timeoutMillis
  let wrappedTask : Task (Except IO.Error (Option (Except IO.Error α))) :=
    Task.map (fun result => Except.ok (some result)) task
  let wrappedTimeout : Task (Except IO.Error (Option (Except IO.Error α))) := Task.map
    (fun result =>
      match result with
      | .ok _ => Except.ok none
      | .error err => Except.error err)
    timeoutTask
  match (← IO.waitAny [wrappedTask, wrappedTimeout]) with
  | .ok outcome =>
      pure outcome
  | .error err =>
      throw (IO.userError s!"{label}: {err}")

private def awaitTaskWithin {α : Type} (runtime : Capnp.Rpc.Runtime) (label : String)
    (task : Task (Except IO.Error α)) (timeoutMillis : UInt32 := UInt32.ofNat 3000) : IO α := do
  match (← waitTaskWithin runtime label task timeoutMillis) with
  | some (.ok value) =>
      pure value
  | some (.error err) =>
      throw (IO.userError s!"{label}: {err}")
  | none =>
      throw (IO.userError s!"{label}: timeout")

private def assertTaskPendingWithin {α : Type} (runtime : Capnp.Rpc.Runtime) (label : String)
    (task : Task (Except IO.Error α))
    (timeoutMillis : UInt32 := UInt32.ofNat 150) : IO Unit := do
  match (← waitTaskWithin runtime label task timeoutMillis) with
  | none =>
      pure ()
  | some (.ok _) =>
      throw (IO.userError s!"{label}: completed before expected failure")
  | some (.error err) =>
      throw (IO.userError s!"{label}: failed before expected failure: {err}")

private def waitForPendingCount (runtime : Capnp.Rpc.Runtime) (label : String)
    (expected : UInt64) (attempts : Nat := 200) : IO Unit := do
  let rec loop (remaining : Nat) : IO Unit := do
    runtime.pump
    let observed ← runtime.pendingCallCount
    if observed == expected then
      pure ()
    else
      match remaining with
      | 0 =>
          throw (IO.userError s!"{label}: expected pending={expected}, observed {observed}")
      | remaining + 1 =>
          IO.sleep (UInt32.ofNat 5)
          loop remaining
  loop attempts

private def assertDisconnectedWithDetail (label : String) (messageNeedle : String)
    (detail : ByteArray) (outcome : Capnp.Rpc.RawCallOutcome) : IO Unit := do
  match outcome with
  | .ok _ _ =>
      throw (IO.userError s!"{label}: expected call to fail")
  | .error ex =>
      assertEqual ex.type .disconnected
      if !(ex.description.containsSubstr messageNeedle) then
        throw (IO.userError s!"{label}: missing disconnect message in: {ex.description}")
      assertEqual ex.detail detail

private def assertDisconnectedResultWithDetail (label : String) (messageNeedle : String)
    (detail : ByteArray) (result : Except Capnp.Rpc.RemoteException Capnp.Rpc.Payload) :
    IO Unit := do
  match result with
  | .ok _ =>
      throw (IO.userError s!"{label}: expected call to fail")
  | .error ex =>
      assertEqual ex.type .disconnected
      if !(ex.description.containsSubstr messageNeedle) then
        throw (IO.userError s!"{label}: missing disconnect message in: {ex.description}")
      assertEqual ex.detail detail

@[test]
def testRuntimeParityPromisedCapabilityDelayedRejectPropagatesToPendingCalls : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let rejectMessage := "promised capability delayed rejection"
  let rejectDetail := "promised-delayed-reject-detail".toUTF8
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let (promiseCap, fulfiller) ← runtime.newPromiseCapability
    let pendingA ← runtime.startCall promiseCap Echo.fooMethod payload
    let pendingB ← runtime.startCall promiseCap Echo.fooMethod payload
    assertEqual (← runtime.pendingCallCount) (UInt64.ofNat 2)

    let taskA ← IO.asTask pendingA.awaitOutcome
    let taskB ← IO.asTask pendingB.awaitOutcome
    assertTaskPendingWithin runtime "delayed rejection pending A" taskA
    assertTaskPendingWithin runtime "delayed rejection pending B" taskB

    IO.sleep (UInt32.ofNat 20)
    fulfiller.reject .disconnected rejectMessage rejectDetail

    let outcomeA ← awaitTaskWithin runtime "delayed rejection outcome A" taskA
    let outcomeB ← awaitTaskWithin runtime "delayed rejection outcome B" taskB
    assertDisconnectedWithDetail "delayed rejection outcome A" rejectMessage rejectDetail outcomeA
    assertDisconnectedWithDetail "delayed rejection outcome B" rejectMessage rejectDetail outcomeB
    assertEqual (← runtime.pendingCallCount) (UInt64.ofNat 0)

    let postRejectOutcome ← runtime.callResult promiseCap Echo.fooMethod payload
    assertDisconnectedResultWithDetail
      "post-reject callResult"
      rejectMessage
      rejectDetail
      postRejectOutcome

    fulfiller.release
    runtime.releaseTarget promiseCap
  finally
    runtime.shutdown

@[test]
def testRuntimeParityPromisedCapabilityCancelBeforeRejectSequencing : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let rejectMessage := "promised capability cancel-before-reject sequencing"
  let rejectDetail := "promised-cancel-before-reject-detail".toUTF8
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let (promiseCap, fulfiller) ← runtime.newPromiseCapability
    let canceledPending ← runtime.startCall promiseCap Echo.fooMethod payload
    let survivingPending ← runtime.startCall promiseCap Echo.fooMethod payload
    assertEqual (← runtime.pendingCallCount) (UInt64.ofNat 2)

    canceledPending.release
    waitForPendingCount runtime "cancel-before-reject pending count after release" (UInt64.ofNat 1)

    let canceledDoubleReleaseFailed ←
      try
        canceledPending.release
        pure false
      catch _ =>
        pure true
    assertEqual canceledDoubleReleaseFailed true

    let survivingTask ← IO.asTask survivingPending.awaitOutcome
    assertTaskPendingWithin runtime "cancel-before-reject surviving call pending" survivingTask

    IO.sleep (UInt32.ofNat 20)
    fulfiller.reject .disconnected rejectMessage rejectDetail

    let survivingOutcome ← awaitTaskWithin runtime "cancel-before-reject surviving outcome" survivingTask
    assertDisconnectedWithDetail
      "cancel-before-reject surviving outcome"
      rejectMessage
      rejectDetail
      survivingOutcome
    assertEqual (← runtime.pendingCallCount) (UInt64.ofNat 0)

    let afterRejectOutcome ← runtime.callResult promiseCap Echo.fooMethod payload
    assertDisconnectedResultWithDetail
      "cancel-before-reject callResult"
      rejectMessage
      rejectDetail
      afterRejectOutcome

    fulfiller.release
    runtime.releaseTarget promiseCap
  finally
    runtime.shutdown

@[test]
def testRuntimeTransportAbortPendingCall : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (fd1, fd2) ← ffiNewSocketPairImpl
  let runtime ← Capnp.Rpc.Runtime.init
  try
    -- Give fd1 to the runtime for the client, taking ownership.
    let transport1 ← runtime.newTransportFromFdTake fd1
    let client ← runtime.connectTransport transport1

    -- Start a call. It should be queued or attempting to send.
    let pending ← runtime.startCall client Echo.fooMethod payload

    -- Now close fd2 (the other end of the socket pair) without ever accepting it.
    -- This should cause the client's connection on fd1 to be aborted.
    ffiCloseFdImpl fd2

    let outcome ← pending.awaitOutcome
    match outcome with
    | .ok _ _ =>
        throw (IO.userError "expected call to fail after transport abort")
    | .error ex =>
        assertEqual ex.type .disconnected

    runtime.releaseTarget client
  finally
    runtime.shutdown

@[test]
def testRuntimeProtocolDisconnectDetail : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let vatNetwork := runtime.vatNetwork
    let alice ← vatNetwork.newClient "alice"
    let bobBootstrap ← runtime.registerEchoTarget
    let bob ← vatNetwork.newServer "bob" bobBootstrap

    let aliceToBob ← alice.bootstrapPeer bob

    let disconnectMessage := "custom disconnect message"
    let disconnectDetail := "custom-disconnect-detail".toUTF8

    -- Alice disconnects her connection to Bob.
    alice.disconnectConnectionTo bob .disconnected disconnectMessage disconnectDetail

    let outcome ← runtime.callOutcome aliceToBob Echo.fooMethod payload
    assertDisconnectedWithDetail "aliceToBob after disconnect" disconnectMessage disconnectDetail outcome

    runtime.releaseTarget aliceToBob
    alice.release
    bob.release
    runtime.releaseTarget bobBootstrap
  finally
    runtime.shutdown
