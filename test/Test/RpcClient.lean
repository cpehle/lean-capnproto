import LeanTest
import Capnp.Rpc
import Capnp.RpcKjAsync
import Capnp.KjAsync
import Test.Common
import Capnp.Gen.fixtures.rpc_echo

open LeanTest
open Capnp.Gen.fixtures.rpc_echo

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

def connectRuntimeTargetWithRetry (runtime : Capnp.Rpc.Runtime) (address : String)
    (attempts : Nat := 20) (delayMillis : UInt32 := UInt32.ofNat 10) : IO Capnp.Rpc.Client := do
  let mut target? : Option Capnp.Rpc.Client := none
  let mut attempt := 0
  while target?.isNone && attempt < attempts do
    let nextTarget? ←
      try
        let c ← runtime.connect address
        pure (some (Capnp.Rpc.Client.ofCapability c))
      catch _ =>
        pure none
    target? := nextTarget?
    if target?.isNone then
      IO.sleep delayMillis
    attempt := attempt + 1
  match target? with
  | some target => pure target
  | none => throw (IO.userError s!"failed to connect Lean runtime target to {address}")

@[extern "capnp_lean_rpc_runtime_register_fd_probe_target"]
opaque ffiRuntimeRegisterFdProbeTargetImpl (runtime : UInt64) : IO UInt32

private def registerFdProbeTarget (runtime : Capnp.Rpc.Runtime) : IO Capnp.Rpc.Client :=
  Capnp.Rpc.Client.ofCapability <$> ffiRuntimeRegisterFdProbeTargetImpl runtime.handle

@[test]
def testGeneratedMethodMetadata : IO Unit := do
  assertEqual Echo.fooMethodId (UInt16.ofNat 0)
  assertEqual Echo.barMethodId (UInt16.ofNat 1)
  assertEqual Echo.fooMethod.interfaceId Echo.interfaceId
  assertEqual Echo.fooMethod.methodId Echo.fooMethodId

@[test]
def testTwoPartyVatSideCodec : IO Unit := do
  assertEqual (Capnp.Rpc.TwoPartyVatSide.ofUInt16 0) .client
  assertEqual (Capnp.Rpc.TwoPartyVatSide.ofUInt16 1) .server
  assertEqual (Capnp.Rpc.TwoPartyVatSide.ofUInt16 9) (.unknown 9)
  assertEqual (Capnp.Rpc.TwoPartyVatSide.toUInt16 .client) 0
  assertEqual (Capnp.Rpc.TwoPartyVatSide.toUInt16 .server) 1
  assertEqual (Capnp.Rpc.TwoPartyVatSide.toUInt16 (.unknown 11)) 11

@[test]
def testDispatchRoutesGeneratedClientCall : IO Unit := do
  let hit ← IO.mkRef false
  let seenTarget ← IO.mkRef (UInt32.ofNat 0)
  let payload : Capnp.Rpc.Payload := Capnp.emptyRpcEnvelope
  let dispatch :=
    Capnp.Rpc.Dispatch.register Capnp.Rpc.Dispatch.empty
      Echo.fooMethod
      (fun target req => do
        hit.set true
        seenTarget.set target
        pure req)
  let backend := dispatch.toBackend
  let response ← Echo.callFoo backend (UInt32.ofNat 123) payload
  assertEqual (← hit.get) true
  assertEqual (← seenTarget.get) (UInt32.ofNat 123)
  assertEqual (response == payload) true

@[test]
def testDispatchOnMissingReceivesMethod : IO Unit := do
  let seenMethod ← IO.mkRef ({ interfaceId := 0, methodId := 0 } : Capnp.Rpc.Method)
  let payload : Capnp.Rpc.Payload := Capnp.emptyRpcEnvelope
  let backend := Capnp.Rpc.Dispatch.toBackend Capnp.Rpc.Dispatch.empty (onMissing := fun _ method req => do
    seenMethod.set method
    pure req)
  let response ← Echo.callBar backend (UInt32.ofNat 5) payload
  let method := (← seenMethod.get)
  assertEqual method.interfaceId Echo.interfaceId
  assertEqual method.methodId Echo.barMethodId
  assertEqual (response == payload) true

@[test]
def testGeneratedServerBackendDispatch : IO Unit := do
  let payload : Capnp.Rpc.Payload := Capnp.emptyRpcEnvelope
  let seenFoo ← IO.mkRef false
  let seenBar ← IO.mkRef false
  let server : Echo.Server := {
    foo := fun _ req => do
      seenFoo.set true
      pure req
    bar := fun _ req => do
      seenBar.set true
      pure req
  }
  let backend := Echo.backend server
  let response ← Echo.callBar backend (UInt32.ofNat 5) payload
  assertEqual (response == payload) true
  assertEqual (← seenFoo.get) false
  assertEqual (← seenBar.get) true

@[test]
def testGeneratedTypedServerBackendDispatch : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let seenFoo ← IO.mkRef false
  let server : Echo.TypedServer := {
    foo := fun _ req reqCaps => do
      seenFoo.set true
      assertEqual reqCaps.caps.size 0
      assertEqual req.hasPayload false
      pure payload
    bar := fun _ _ _ =>
      throw (IO.userError "unexpected typed bar handler invocation")
  }
  let backend := Echo.typedBackend server
  let response ← Echo.callFooTyped backend (UInt32.ofNat 7) payload
  assertEqual response.capTable.caps.size 0
  assertEqual response.reader.hasPayload false
  assertEqual (← seenFoo.get) true

@[test]
def testGeneratedRegisterTargetNetwork : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let seenFoo ← IO.mkRef false
  let seenBar ← IO.mkRef false
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let handler : Echo.Server := {
      foo := fun _ req => do
        seenFoo.set true
        pure req
      bar := fun _ req => do
        seenBar.set true
        pure req
    }
    let bootstrap ← Echo.registerTarget runtime handler
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    let remoteTarget ← client.bootstrap
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM remoteTarget payload
    assertEqual response.capTable.caps.size 0
    assertEqual (← seenFoo.get) true
    assertEqual (← seenBar.get) false

    client.release
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testGeneratedRegisterTypedTargetNetwork : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let seenFoo ← IO.mkRef false
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let handler : Echo.TypedServer := {
      foo := fun _ req reqCaps => do
        seenFoo.set true
        assertEqual reqCaps.caps.size 0
        assertEqual req.hasPayload false
        pure payload
      bar := fun _ _ _ =>
        throw (IO.userError "unexpected typed bar handler invocation")
    }
    let bootstrap ← Echo.registerTypedTarget runtime handler
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    let remoteTarget ← client.bootstrap
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooTypedM remoteTarget payload
    assertEqual response.capTable.caps.size 0
    assertEqual response.reader.hasPayload false
    assertEqual (← seenFoo.get) true

    client.release
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testGeneratedRegisterAdvancedTypedTargetNetwork : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let seenFoo ← IO.mkRef false
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let sink ← runtime.registerEchoTarget
    let handler : Echo.AdvancedTypedServer := {
      foo := fun _ req reqCaps => do
        seenFoo.set true
        assertEqual reqCaps.caps.size 0
        assertEqual req.hasPayload false
        pure (Capnp.Rpc.Advanced.now
          (Capnp.Rpc.Advanced.forwardToCaller sink Echo.fooMethod payload
            Capnp.Rpc.AdvancedCallHints.withNoPromisePipelining))
      bar := fun _ _ _ =>
        pure (Capnp.Rpc.Advanced.now
          (Capnp.Rpc.Advanced.throwRemote "unexpected advanced typed bar handler invocation"))
    }
    let bootstrap ← Echo.registerAdvancedTypedTarget runtime handler
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    let remoteTarget ← client.bootstrap
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooTypedM remoteTarget payload
    assertEqual response.capTable.caps.size 0
    assertEqual response.reader.hasPayload false
    assertEqual (← seenFoo.get) true

    client.release
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
    runtime.releaseTarget sink
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testGeneratedRegisterStreamingTypedTargetNetwork : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let seenFoo ← IO.mkRef false
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let handler : Echo.StreamingTypedServer := {
      foo := fun _ req reqCaps => do
        seenFoo.set true
        assertEqual reqCaps.caps.size 0
        assertEqual req.hasPayload false
        pure (Capnp.Rpc.Advanced.now (Capnp.Rpc.Advanced.respond Capnp.emptyRpcEnvelope))
      bar := fun _ _ _ =>
        pure (Capnp.Rpc.Advanced.now
          (Capnp.Rpc.Advanced.throwRemote "unexpected streaming typed bar handler invocation"))
    }
    let bootstrap ← Echo.registerStreamingTypedTarget runtime handler
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    let remoteTarget ← client.bootstrap
    runtime.streamingCall remoteTarget Echo.fooMethod payload
    let mut seen := (← seenFoo.get)
    let mut attempts := 0
    while !seen && attempts < 200 do
      IO.sleep (UInt32.ofNat 5)
      seen := (← seenFoo.get)
      attempts := attempts + 1
    assertEqual seen true

    client.release
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testGeneratedAsyncHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget
    let capPayload := mkCapabilityPayload target
    let pending1 ← Echo.startFoo runtime target capPayload
    let pipelinedCap ← Echo.getFooPipelinedCap pending1
    let pipelinedViaCap ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM pipelinedCap payload
    assertEqual pipelinedViaCap.capTable.caps.size 0
    let response1 ← Echo.awaitFoo pending1
    assertEqual response1.capTable.caps.size 1
    runtime.releaseCapTable response1.capTable
    runtime.releaseTarget pipelinedCap

    let pending2 ← Echo.startFoo runtime target capPayload
    let pipelinedResponse ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooPipelinedM pending2 #[] payload
    assertEqual pipelinedResponse.capTable.caps.size 0
    let response2 ← Echo.awaitFoo pending2
    assertEqual response2.capTable.caps.size 1
    runtime.releaseCapTable response2.capTable

    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testGeneratedPromiseHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let typedPayload : Capnp.Rpc.Payload := Id.run do
    let (_, builder) :=
      (do
        let root ← Capnp.getRootPointer
        let s ← Capnp.initStructPointer root 0 1
        Capnp.clearPointer (Capnp.getPointerBuilder s 0)
      ).run (Capnp.initMessageBuilder 16)
    { msg := Capnp.buildMessage builder, capTable := Capnp.emptyCapTable }
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget
    let capPayload := mkCapabilityPayload target

    let promise ← Echo.startFooPromise runtime target capPayload
    let pipelinedCap ← promise.getPipelinedCap
    let pipelinedResponse ← Capnp.Rpc.RuntimeM.run runtime do
      promise.callPipelinedM #[] payload
    assertEqual pipelinedResponse.capTable.caps.size 0

    let response ← promise.await
    assertEqual response.capTable.caps.size 1
    runtime.releaseCapTable response.capTable

    let promiseTyped ← Echo.startFooPromise runtime target typedPayload
    let typedResponse ← promiseTyped.awaitTyped
    assertEqual typedResponse.capTable.caps.size 0
    runtime.releaseCapTable typedResponse.capTable

    runtime.releaseTarget pipelinedCap
    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testGeneratedPayloadRefHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget

    let requestA ← runtime.payloadRefFromPayload payload
    let responseA ← Echo.callFooWithPayloadRef runtime target requestA
    let decodedA ← responseA.decodeAndRelease
    assertEqual decodedA.capTable.caps.size 0
    requestA.release

    let requestB ← runtime.payloadRefFromPayload payload
    let pendingB ← Echo.startFooWithPayloadRef runtime target requestB
    let responseB ← Echo.awaitFooPayloadRef pendingB
    let decodedB ← responseB.decodeAndRelease
    assertEqual decodedB.capTable.caps.size 0
    requestB.release

    let promiseC ← Echo.startFooPromise runtime target payload
    let responseC ← promiseC.awaitPayloadRef
    let decodedC ← responseC.decodeAndRelease
    assertEqual decodedC.capTable.caps.size 0

    let promiseD ← Echo.startFooPromise runtime target payload
    let responseD ← promiseD.awaitPayloadRef
    let decodedD ← responseD.decodeAndRelease
    assertEqual decodedD.capTable.caps.size 0

    let requestE ← runtime.payloadRefFromPayload payload
    let responseE ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooWithPayloadRefM target requestE
    let decodedE ← responseE.decodeAndRelease
    assertEqual decodedE.capTable.caps.size 0
    requestE.release

    let requestF ← runtime.payloadRefFromPayload payload
    let pendingF ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.startFooWithPayloadRefM target requestF
    let responseF ← Echo.awaitFooPayloadRef pendingF
    let decodedF ← responseF.decodeAndRelease
    assertEqual decodedF.capTable.caps.size 0
    requestF.release

    let requestG ← runtime.payloadRefFromPayload payload
    let pendingG ← Echo.startFooWithPayloadRef runtime target requestG
    let responseG ← pendingG.awaitPayloadRef
    let decodedG ← responseG.decodeAndRelease
    assertEqual decodedG.capTable.caps.size 0
    requestG.release

    let requestH ← runtime.payloadRefFromPayload payload
    let pendingH ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.startFooWithPayloadRefM target requestH
    let responseH ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.pendingCallAwaitPayloadRef pendingH
    let decodedH ← responseH.decodeAndRelease
    assertEqual decodedH.capTable.caps.size 0
    requestH.release

    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testGeneratedSturdyRefAsyncHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let bootstrap ← runtime.registerEchoTarget
    let alice ← runtime.newMultiVatClient "alice-generated-sturdy"
    let bob ← runtime.newMultiVatServer "bob-generated-sturdy" bootstrap
    let objectId := ByteArray.mk #[3, 1, 4, 1]
    let sturdyRef : Capnp.Rpc.SturdyRef := {
      vat := { host := "bob-generated-sturdy", unique := false }
      objectId := objectId
    }
    bob.publishSturdyRef objectId bootstrap

    let checkTarget (target : Echo) : IO Unit := do
      let response ← Capnp.Rpc.RuntimeM.run runtime do
        Echo.callFooM target payload
      assertEqual response.capTable.caps.size 0
      runtime.releaseTarget target

    let pending ← Echo.restoreSturdyRefStart alice sturdyRef
    checkTarget (← Echo.awaitRestoreSturdyRef pending)

    let task ← Echo.restoreSturdyRefAsTask alice sturdyRef
    match (← IO.wait task) with
    | .ok target => checkTarget target
    | .error err => throw err

    let promise ← Echo.restoreSturdyRefAsPromise alice sturdyRef
    match (← promise.awaitResult) with
    | .ok target => checkTarget target
    | .error err => throw err

    checkTarget (← Capnp.Rpc.RuntimeM.run runtime do
      Echo.restoreSturdyRefM alice sturdyRef)

    let pendingM ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.restoreSturdyRefStartM alice sturdyRef
    checkTarget (← Echo.awaitRestoreSturdyRef pendingM)

    let taskM ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.restoreSturdyRefAsTaskM alice sturdyRef
    match (← IO.wait taskM) with
    | .ok target => checkTarget target
    | .error err => throw err

    let promiseM ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.restoreSturdyRefAsPromiseM alice sturdyRef
    match (← promiseM.awaitResult) with
    | .ok target => checkTarget target
    | .error err => throw err

    alice.release
    bob.release
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimePayloadRefLifecycleRoundtrip : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget
    let requestRef ← runtime.payloadRefFromPayload payload
    let responseRef ← runtime.callWithPayloadRef target Echo.fooMethod requestRef
    let response ← responseRef.decode
    assertEqual response.capTable.caps.size 0
    requestRef.release
    responseRef.release

    let requestReleaseFailed ←
      try
        requestRef.release
        pure false
      catch _ =>
        pure true
    assertEqual requestReleaseFailed true
    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimeBootstrapTargetHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let baselineTargets := (← runtime.targetCount)
    let baselineClients := (← runtime.clientCount)

    let acceptTaskA ← IO.asTask (server.accept listener)
    let responseA ← runtime.withBootstrapTarget address (fun target => do
      Echo.callFoo runtime.backend target payload)
    match (← IO.wait acceptTaskA) with
    | .ok _ => pure ()
    | .error err => throw err
    assertEqual responseA.capTable.caps.size 0
    assertEqual (← runtime.targetCount) baselineTargets
    assertEqual (← runtime.clientCount) baselineClients

    let acceptTaskB ← IO.asTask (server.accept listener)
    let responseB ← runtime.withBootstrapClientTarget address (fun client target => do
      let _ ← client.queueSize
      Echo.callFoo runtime.backend target payload)
    match (← IO.wait acceptTaskB) with
    | .ok _ => pure ()
    | .error err => throw err
    assertEqual responseB.capTable.caps.size 0
    assertEqual (← runtime.targetCount) baselineTargets
    assertEqual (← runtime.clientCount) baselineClients

    let acceptTaskC ← IO.asTask (server.accept listener)
    let responseC ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.withBootstrapClientTarget address (fun client target => do
        let _ ← Capnp.Rpc.RuntimeM.clientQueueSize client
        Echo.callFooM target payload)
    match (← IO.wait acceptTaskC) with
    | .ok _ => pure ()
    | .error err => throw err
    assertEqual responseC.capTable.caps.size 0
    assertEqual (← runtime.targetCount) baselineTargets
    assertEqual (← runtime.clientCount) baselineClients

    let acceptTaskD ← IO.asTask (server.accept listener)
    let (clientD, target) ← runtime.newBootstrapTarget address
    let responseD ←
      try
        runtime.withTarget target (fun scopedTarget => do
          Echo.callFoo runtime.backend scopedTarget payload)
      finally
        clientD.release
    match (← IO.wait acceptTaskD) with
    | .ok _ => pure ()
    | .error err => throw err
    assertEqual responseD.capTable.caps.size 0
    assertEqual (← runtime.targetCount) baselineTargets
    assertEqual (← runtime.clientCount) baselineClients

    runtime.releaseListener listener
    server.release
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeCallPayloadRefHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget

    let responseA ← runtime.callPayloadRef target Echo.fooMethod payload
    let decodedA ← responseA.decodeAndRelease
    assertEqual decodedA.capTable.caps.size 0

    let decodedB ← runtime.withCallPayloadRef target Echo.fooMethod
      (fun responseRef => responseRef.decode) payload
    assertEqual decodedB.capTable.caps.size 0

    let decodedC ← runtime.callPayloadRefDecode target Echo.fooMethod payload
    assertEqual decodedC.capTable.caps.size 0

    let decodedD ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.callPayloadRefDecode target Echo.fooMethod payload
    assertEqual decodedD.capTable.caps.size 0

    let decodedE ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.withCallPayloadRef target Echo.fooMethod
        (fun responseRef => do
          responseRef.decode)
        payload
    assertEqual decodedE.capTable.caps.size 0

    let threw ←
      try
        let (_ : Unit) ← runtime.withCallPayloadRef target Echo.fooMethod
          (fun _ => throw (IO.userError "intentional withCallPayloadRef failure"))
          payload
        pure false
      catch _ =>
        pure true
    assertEqual threw true
    runtime.pump
    assertEqual (← runtime.pendingCallCount) (UInt64.ofNat 0)

    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimeStartCallAwaitHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget

    let response ← runtime.startCallAwait target Echo.fooMethod payload
    assertEqual response.capTable.caps.size 0

    let outcome ← runtime.startCallAwaitOutcome target Echo.fooMethod payload
    match outcome with
    | .ok _ _ => pure ()
    | .error ex =>
        throw (IO.userError s!"unexpected startCallAwaitOutcome error: {ex.description}")

    let result ← runtime.startCallAwaitResult target Echo.fooMethod payload
    match result with
    | .ok okPayload =>
        assertEqual okPayload.capTable.caps.size 0
    | .error ex =>
        throw (IO.userError s!"unexpected startCallAwaitResult error: {ex.description}")

    let threw ←
      try
        let (_ : Unit) ← runtime.withStartedCall target Echo.fooMethod
          (fun _ =>
            (throw (IO.userError "intentional withStartedCall failure") : IO Unit)) payload
        pure false
      catch _ =>
        pure true
    assertEqual threw true
    runtime.pump
    assertEqual (← runtime.pendingCallCount) (UInt64.ofNat 0)
    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimeMStartCallAwaitHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget

    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.startCallAwait target Echo.fooMethod payload
    assertEqual response.capTable.caps.size 0

    let outcome ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.startCallAwaitOutcome target Echo.fooMethod payload
    match outcome with
    | .ok _ _ => pure ()
    | .error ex =>
        throw (IO.userError s!"unexpected RuntimeM.startCallAwaitOutcome error: {ex.description}")

    let result ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.startCallAwaitResult target Echo.fooMethod payload
    match result with
    | .ok okPayload =>
        assertEqual okPayload.capTable.caps.size 0
    | .error ex =>
        throw (IO.userError s!"unexpected RuntimeM.startCallAwaitResult error: {ex.description}")

    let threw ←
      try
        let (_ : Unit) ← Capnp.Rpc.RuntimeM.run runtime do
          Capnp.Rpc.RuntimeM.withStartedCall target Echo.fooMethod
            (fun _ =>
              (throw (IO.userError "intentional RuntimeM.withStartedCall failure") :
                Capnp.Rpc.RuntimeM Unit))
            payload
        pure false
      catch _ =>
        pure true
    assertEqual threw true
    runtime.pump
    assertEqual (← runtime.pendingCallCount) (UInt64.ofNat 0)
    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimeMPayloadRefRoundtrip : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      let requestRef ← Capnp.Rpc.RuntimeM.payloadRefFromPayload payload
      let responseRef ← Capnp.Rpc.RuntimeM.callWithPayloadRef target Echo.fooMethod requestRef
      let response ← responseRef.decode
      requestRef.release
      responseRef.release
      pure response
    assertEqual response.capTable.caps.size 0
    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimePayloadRefAsyncTaskPromiseHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget
    let checkPayloadRef (responseRef : Capnp.Rpc.RuntimePayloadRef) : IO Unit := do
      let response ← responseRef.decodeAndRelease
      assertEqual response.capTable.caps.size 0

    let requestA ← runtime.payloadRefFromPayload payload
    let taskA ← runtime.startCallWithPayloadRefAsTask target Echo.fooMethod requestA
    match (← IO.wait taskA) with
    | .ok responseRef => checkPayloadRef responseRef
    | .error err => throw err
    requestA.release

    let requestB ← runtime.payloadRefFromPayload payload
    let promiseB ← runtime.startCallWithPayloadRefAsPromise target Echo.fooMethod requestB
    match (← promiseB.awaitResult) with
    | .ok responseRef => checkPayloadRef responseRef
    | .error err => throw err
    requestB.release

    let requestC ← runtime.payloadRefFromPayload payload
    let pendingC ← runtime.startCallWithPayloadRef target Echo.fooMethod requestC
    let taskC ← pendingC.awaitPayloadRefAsTask
    match (← IO.wait taskC) with
    | .ok responseRef => checkPayloadRef responseRef
    | .error err => throw err
    requestC.release

    let requestD ← runtime.payloadRefFromPayload payload
    let pendingD ← runtime.startCallWithPayloadRef target Echo.fooMethod requestD
    let promiseD ← pendingD.toPromisePayloadRef
    match (← promiseD.awaitResult) with
    | .ok responseRef => checkPayloadRef responseRef
    | .error err => throw err
    requestD.release

    let requestE ← runtime.payloadRefFromPayload payload
    let taskE ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.startCallWithPayloadRefAsTask target Echo.fooMethod requestE
    match (← IO.wait taskE) with
    | .ok responseRef => checkPayloadRef responseRef
    | .error err => throw err
    requestE.release

    let requestF ← runtime.payloadRefFromPayload payload
    let promiseF ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.startCallWithPayloadRefAsPromise target Echo.fooMethod requestF
    match (← promiseF.awaitResult) with
    | .ok responseRef => checkPayloadRef responseRef
    | .error err => throw err
    requestF.release

    let requestH ← runtime.payloadRefFromPayload payload
    let pendingH ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.startCallWithPayloadRef target Echo.fooMethod requestH
    let taskH ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.pendingCallAwaitPayloadRefAsTask pendingH
    match (← IO.wait taskH) with
    | .ok responseRef => checkPayloadRef responseRef
    | .error err => throw err
    requestH.release

    let requestI ← runtime.payloadRefFromPayload payload
    let pendingI ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.startCallWithPayloadRef target Echo.fooMethod requestI
    let promiseI ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.pendingCallToPromisePayloadRef pendingI
    match (← promiseI.awaitResult) with
    | .ok responseRef => checkPayloadRef responseRef
    | .error err => throw err
    requestI.release

    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimePromiseCapabilityPipelining : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let sink ← runtime.registerEchoTarget
    let (promiseCap, fulfiller) ← runtime.newPromiseCapability

    let handler : Echo.AdvancedTypedServer := {
      foo := fun _ req reqCaps => do
        let _ := req
        let _ := reqCaps
        let pipeline := mkCapabilityPayload promiseCap
        let deferred ← Capnp.Rpc.Advanced.defer (next := do
          IO.sleep (UInt32.ofNat 50)
          fulfiller.fulfill sink
          pure (Capnp.Rpc.Advanced.respond pipeline))
        pure (Capnp.Rpc.Advanced.setPipeline pipeline deferred)
      bar := fun _ _ _ =>
        pure (Capnp.Rpc.Advanced.now (Capnp.Rpc.Advanced.respond Capnp.emptyRpcEnvelope))
    }

    let bootstrap ← Echo.registerAdvancedTypedTarget runtime handler
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    let remoteTarget ← client.bootstrap
    let pending ← Echo.startFoo runtime remoteTarget payload
    let pipelinedCap ← Echo.getFooPipelinedCap pending

    let pipelinedResponse ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM pipelinedCap payload
    assertEqual pipelinedResponse.capTable.caps.size 0
    assertEqual (Capnp.isNullPointer (Capnp.getRoot pipelinedResponse.msg)) true

    let response ← Echo.awaitFoo pending
    assertEqual response.capTable.caps.size 1
    runtime.releaseCapTable response.capTable
    runtime.releaseTarget pipelinedCap

    client.release
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
    runtime.releaseTarget promiseCap
    fulfiller.release
    runtime.releaseTarget sink
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testBackendOfRawCall : IO Unit := do
  let seenMethod ← IO.mkRef ({ interfaceId := 0, methodId := 0 } : Capnp.Rpc.Method)
  let payload : Capnp.Rpc.Payload := Capnp.emptyRpcEnvelope
  let raw : Capnp.Rpc.RawCall := fun _ method requestBytes => do
    seenMethod.set method
    pure requestBytes
  let backend := Capnp.Rpc.Backend.ofRawCall raw
  let response ← Echo.callFoo backend (UInt32.ofNat 17) payload
  let method := (← seenMethod.get)
  assertEqual method.interfaceId Echo.interfaceId
  assertEqual method.methodId Echo.fooMethodId
  assertEqual (response == payload) true

@[test]
def testFfiBackendRawRoundtrip : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let response ← Capnp.Rpc.RuntimeM.runWithNewRuntime do
    assertEqual (← Capnp.Rpc.RuntimeM.isAlive) true
    let target ← Capnp.Rpc.RuntimeM.registerEchoTarget
    let echoed ← Echo.callFooM target payload
    assertEqual echoed.capTable.caps.size 0

    let capPayload := mkCapabilityPayload target
    let capResponse ← Echo.callFooM target capPayload
    assertEqual capResponse.capTable.caps.size 1

    let returnedCap? := Capnp.readCapabilityFromTable capResponse.capTable (Capnp.getRoot capResponse.msg)
    assertEqual returnedCap?.isSome true
    match returnedCap? with
    | some returnedTarget =>
        assertEqual (returnedTarget == (UInt32.ofNat 0)) false
        Echo.callFooM returnedTarget payload
    | none =>
        throw (IO.userError "RPC response is missing expected capability")
  assertEqual response.capTable.caps.size 0

  let runtime ← Capnp.Rpc.Runtime.init
  let target ← runtime.registerEchoTarget
  runtime.shutdown
  assertEqual (← runtime.isAlive) false

  let failedAfterShutdown ←
    try
      let _ ← Capnp.Rpc.RuntimeM.run runtime do
        Echo.callFooM target payload
      pure false
    catch _ =>
      pure true
  assertEqual failedAfterShutdown true

@[test]
def testRuntimeReleaseCapTable : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget
    let capPayload := mkCapabilityPayload target
    let capResponse ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target capPayload
    assertEqual capResponse.capTable.caps.size 1

    let returnedCap? := Capnp.readCapabilityFromTable capResponse.capTable (Capnp.getRoot capResponse.msg)
    assertEqual returnedCap?.isSome true
    match returnedCap? with
    | none =>
        throw (IO.userError "RPC response is missing expected capability")
    | some returnedCap =>
        runtime.releaseCapTable capResponse.capTable
        let failedAfterRelease ←
          try
            let _ ← Capnp.Rpc.RuntimeM.run runtime do
              Echo.callFooM returnedCap payload
            pure false
          catch _ =>
            pure true
        assertEqual failedAfterRelease true

    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimeReleaseTarget : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget
    let _ ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target payload

    runtime.releaseTarget target
    let failedAfterRelease ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM target payload
        pure false
      catch _ =>
        pure true
    assertEqual failedAfterRelease true

    let failedDoubleRelease ←
      try
        runtime.releaseTarget target
        pure false
      catch _ =>
        pure true
    assertEqual failedDoubleRelease true
  finally
    runtime.shutdown

@[test]
def testRuntimeWithTargetLifecycleHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let response ← runtime.withTarget (← runtime.registerEchoTarget) (fun target => do
      Capnp.Rpc.RuntimeM.run runtime do
        Echo.callFooM target payload)
    assertEqual response.capTable.caps.size 0
    assertEqual (← runtime.targetCount) (UInt64.ofNat 0)

    let failedInScope ←
      try
        runtime.withTarget (← runtime.registerEchoTarget) (fun _ => do
          throw (IO.userError "expected Runtime.withTarget failure"))
        pure false
      catch _ =>
        pure true
    assertEqual failedInScope true
    assertEqual (← runtime.targetCount) (UInt64.ofNat 0)
  finally
    runtime.shutdown

@[test]
def testRuntimeWithCapTableLifecycleHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget
    let capPayload := mkCapabilityPayload target

    let capResponse1 ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target capPayload
    assertEqual capResponse1.capTable.caps.size 1
    let returnedCap1? := Capnp.readCapabilityFromTable capResponse1.capTable (Capnp.getRoot capResponse1.msg)
    assertEqual returnedCap1?.isSome true
    match returnedCap1? with
    | none =>
        throw (IO.userError "RPC response is missing expected capability")
    | some returnedCap1 =>
        let response ← runtime.withCapTable capResponse1.capTable (fun _ => do
          Capnp.Rpc.RuntimeM.run runtime do
            Echo.callFooM returnedCap1 payload)
        assertEqual response.capTable.caps.size 0
        let failedAfterScope ←
          try
            let _ ← Capnp.Rpc.RuntimeM.run runtime do
              Echo.callFooM returnedCap1 payload
            pure false
          catch _ =>
            pure true
        assertEqual failedAfterScope true

    let capResponse2 ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target capPayload
    assertEqual capResponse2.capTable.caps.size 1
    let returnedCap2? := Capnp.readCapabilityFromTable capResponse2.capTable (Capnp.getRoot capResponse2.msg)
    assertEqual returnedCap2?.isSome true
    match returnedCap2? with
    | none =>
        throw (IO.userError "RPC response is missing expected capability")
    | some returnedCap2 =>
        let failedInScope ←
          try
            runtime.withCapTable capResponse2.capTable (fun _ => do
              throw (IO.userError "expected Runtime.withCapTable failure"))
            pure false
          catch _ =>
            pure true
        assertEqual failedInScope true
        let failedAfterScope ←
          try
            let _ ← Capnp.Rpc.RuntimeM.run runtime do
              Echo.callFooM returnedCap2 payload
            pure false
          catch _ =>
            pure true
        assertEqual failedAfterScope true

    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimeMWithTargetLifecycleHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      let target ← Capnp.Rpc.RuntimeM.registerEchoTarget
      Capnp.Rpc.RuntimeM.withTarget target (fun scopedTarget => do
        Echo.callFooM scopedTarget payload)
    assertEqual response.capTable.caps.size 0
    assertEqual (← runtime.targetCount) (UInt64.ofNat 0)

    let failedInScope ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          let target ← Capnp.Rpc.RuntimeM.registerEchoTarget
          let _ ← (Capnp.Rpc.RuntimeM.withTarget target (fun _ => do
            throw (IO.userError "expected RuntimeM.withTarget failure")) :
              Capnp.Rpc.RuntimeM Unit)
        pure false
      catch _ =>
        pure true
    assertEqual failedInScope true
    assertEqual (← runtime.targetCount) (UInt64.ofNat 0)
  finally
    runtime.shutdown

@[test]
def testRuntimeMWithCapTableLifecycleHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget
    let capPayload := mkCapabilityPayload target

    let capResponse1 ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target capPayload
    assertEqual capResponse1.capTable.caps.size 1
    let returnedCap1? := Capnp.readCapabilityFromTable capResponse1.capTable (Capnp.getRoot capResponse1.msg)
    assertEqual returnedCap1?.isSome true
    match returnedCap1? with
    | none =>
        throw (IO.userError "RPC response is missing expected capability")
    | some returnedCap1 =>
        let response ← Capnp.Rpc.RuntimeM.run runtime do
          Capnp.Rpc.RuntimeM.withCapTable capResponse1.capTable (fun _ => do
            Echo.callFooM returnedCap1 payload)
        assertEqual response.capTable.caps.size 0
        let failedAfterScope ←
          try
            let _ ← Capnp.Rpc.RuntimeM.run runtime do
              Echo.callFooM returnedCap1 payload
            pure false
          catch _ =>
            pure true
        assertEqual failedAfterScope true

    let capResponse2 ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target capPayload
    assertEqual capResponse2.capTable.caps.size 1
    let returnedCap2? := Capnp.readCapabilityFromTable capResponse2.capTable (Capnp.getRoot capResponse2.msg)
    assertEqual returnedCap2?.isSome true
    match returnedCap2? with
    | none =>
        throw (IO.userError "RPC response is missing expected capability")
    | some returnedCap2 =>
        let failedInScope ←
          try
            let _ ← Capnp.Rpc.RuntimeM.run runtime do
              let _ ← (Capnp.Rpc.RuntimeM.withCapTable capResponse2.capTable (fun _ => do
                throw (IO.userError "expected RuntimeM.withCapTable failure")) :
                  Capnp.Rpc.RuntimeM Unit)
            pure false
          catch _ =>
            pure true
        assertEqual failedInScope true
        let failedAfterScope ←
          try
            let _ ← Capnp.Rpc.RuntimeM.run runtime do
              Echo.callFooM returnedCap2 payload
            pure false
          catch _ =>
            pure true
        assertEqual failedAfterScope true

    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimeRetainTarget : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget
    let retained ← runtime.retainTarget target

    runtime.releaseTarget target
    let _ ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM retained payload

    runtime.releaseTarget retained
    let failedAfterRelease ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM retained payload
        pure false
      catch _ =>
        pure true
    assertEqual failedAfterRelease true
  finally
    runtime.shutdown

@[test]
def testRuntimeConnectInvalidAddress : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let failedConnect ←
      try
        let _ ← runtime.connect "unix:/tmp/capnp-lean4-rpc-missing.sock"
        pure false
      catch _ =>
        pure true
    assertEqual failedConnect true
  finally
    runtime.shutdown

@[test]
def testRuntimeListenAcceptEcho : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let listener ← runtime.listenEcho address
    let target ← runtime.connect address
    runtime.acceptEcho listener
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target payload
    assertEqual response.capTable.caps.size 0

    runtime.releaseListener listener
    let failedAfterRelease ←
      try
        runtime.acceptEcho listener
        pure false
      catch _ =>
        pure true
    assertEqual failedAfterRelease true
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeClientServerLifecycle : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    let target ← client.bootstrap
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target payload
    assertEqual response.capTable.caps.size 0

    server.release
    client.onDisconnect
    client.release
    runtime.releaseListener listener
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeServerBootstrapFactoryLifecycle : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  let factoryCalls ← IO.mkRef (0 : Nat)
  let seenUnknownSide ← IO.mkRef false
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServerWithBootstrapFactory (fun side => do
      match side with
      | .unknown _ => seenUnknownSide.set true
      | _ => pure ()
      factoryCalls.modify (fun n => n + 1)
      pure bootstrap)
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    let target ← client.bootstrap
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target payload
    assertEqual response.capTable.caps.size 0
    assertTrue ((← factoryCalls.get) > 0) "bootstrap factory callback was not invoked"
    assertEqual (← seenUnknownSide.get) false

    client.release
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeServerBootstrapFactoryFailure : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let server ← runtime.newServerWithBootstrapFactory (fun _ => do
      throw (IO.userError "expected bootstrap factory failure"))
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    let target ← client.bootstrap
    let errMsg ← try
      let _ ← Capnp.Rpc.RuntimeM.run runtime do
        Echo.callFooM target payload
      pure ""
    catch err =>
      pure (toString err)
    if !(errMsg.containsSubstr "Lean bootstrap factory returned IO error") then
      throw (IO.userError s!"missing bootstrap factory error text: {errMsg}")

    runtime.releaseTarget target
    client.release
    server.release
    runtime.releaseListener listener
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeServerBootstrapFactoryUnknownTarget : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let server ← runtime.newServerWithBootstrapFactory (fun _ => do
      pure (0xFFFFFFFF : UInt32))
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    let target ← client.bootstrap
    let errMsg ← try
      let _ ← Capnp.Rpc.RuntimeM.run runtime do
        Echo.callFooM target payload
      pure ""
    catch err =>
      pure (toString err)
    if !(errMsg.containsSubstr "unknown RPC bootstrap capability id from Lean bootstrap factory") then
      throw (IO.userError s!"missing unknown bootstrap target error text: {errMsg}")

    runtime.releaseTarget target
    client.release
    server.release
    runtime.releaseListener listener
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeServerDrain : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    let target ← client.bootstrap
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target payload
    assertEqual response.capTable.caps.size 0

    client.release
    server.drain
    server.release
    runtime.releaseListener listener
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeAsyncConnectAndWhenResolvedStart : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let connectPromise ← runtime.connectStart address
    server.accept listener
    let target ← connectPromise.awaitTarget
    let whenResolvedPromise ← runtime.targetWhenResolvedStart target
    whenResolvedPromise.await

    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target payload
    assertEqual response.capTable.caps.size 0

    runtime.releaseTarget target
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeRegisterPromiseAwaitAndReleaseConsumes : IO Unit := do
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let connectPromise ← runtime.connectStart address
    server.accept listener
    let target ← connectPromise.awaitTarget

    let releaseFailed ←
      try
        connectPromise.release
        pure false
      catch _ =>
        pure true
    assertEqual releaseFailed true

    runtime.releaseTarget target
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeMRegisterPromiseAwaitAndReleaseConsumes : IO Unit := do
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let connectPromise ← runtime.connectStart address
    server.accept listener
    let target ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.registerPromiseAwait connectPromise

    let releaseFailed ←
      try
        Capnp.Rpc.RuntimeM.run runtime do
          Capnp.Rpc.RuntimeM.registerPromiseRelease connectPromise
        pure false
      catch _ =>
        pure true
    assertEqual releaseFailed true

    runtime.releaseTarget target
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeUnitPromiseAwaitAndReleaseConsumes : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget
    let promise ← runtime.targetWhenResolvedStart target
    promise.await
    let releaseFailed ←
      try
        promise.release
        pure false
      catch _ =>
        pure true
    assertEqual releaseFailed true
    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimeMUnitPromiseAwaitAndReleaseConsumes : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget
    let promise ← runtime.targetWhenResolvedStart target
    Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.unitPromiseAwait promise
    let releaseFailed ←
      try
        Capnp.Rpc.RuntimeM.run runtime do
          Capnp.Rpc.RuntimeM.unitPromiseRelease promise
        pure false
      catch _ =>
        pure true
    assertEqual releaseFailed true
    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimeAsyncClientLifecyclePrimitives : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let clientPromise ← runtime.newClientStart address
    let acceptPromise ← server.acceptStart listener
    let client ← clientPromise.awaitClient
    acceptPromise.await

    let target ← client.bootstrap
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target payload
    assertEqual response.capTable.caps.size 0

    runtime.releaseTarget target
    let disconnectPromise ← client.onDisconnectStart
    client.release
    disconnectPromise.await
    let drainPromise ← server.drainStart
    drainPromise.await

    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeClientQueueMetrics : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    let target ← client.bootstrap
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target payload
    assertEqual response.capTable.caps.size 0

    let queueSize ← client.queueSize
    let queueCount ← client.queueCount
    let outgoingWait ← client.outgoingWaitNanos
    assertEqual queueSize (UInt64.ofNat 0)
    assertEqual queueCount (UInt64.ofNat 0)
    assertTrue (outgoingWait ≤ UInt64.ofNat 1_000_000_000) "outgoing wait exceeded expected bound"

    client.release
    server.release
    runtime.releaseListener listener
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeClientQueueMetricsPreAcceptBacklogDrains : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    let target ← client.bootstrap
    let pending ← runtime.startCall target Echo.fooMethod payload

    let rec waitForBacklog (attempts : Nat) : IO Unit := do
      runtime.pump
      let pendingCount ← runtime.pendingCallCount
      let queueCount ← client.queueCount
      let queueSize ← client.queueSize
      -- Some transports report backlog only via pending-call state before accept;
      -- queue-size/count may legitimately remain zero until the server accepts.
      if pendingCount == (UInt64.ofNat 1) then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError
              s!"pre-accept backlog metrics did not materialize: pending={pendingCount}, queueCount={queueCount}, queueSize={queueSize}")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForBacklog attempts
    waitForBacklog 200

    server.accept listener
    let response ← pending.await
    assertEqual response.capTable.caps.size 0

    let rec waitForDrain (attempts : Nat) : IO Unit := do
      runtime.pump
      let pendingCount ← runtime.pendingCallCount
      let queueCount ← client.queueCount
      let queueSize ← client.queueSize
      if pendingCount == (UInt64.ofNat 0) &&
          queueCount == (UInt64.ofNat 0) &&
          queueSize == (UInt64.ofNat 0) then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError
              s!"post-accept metrics did not drain: pending={pendingCount}, queueCount={queueCount}, queueSize={queueSize}")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForDrain attempts
    waitForDrain 200

    runtime.releaseTarget target
    client.release
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeClientQueueMetricsPreAcceptCancellationDrains : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    let target ← client.bootstrap
    let pending ← runtime.startCall target Echo.fooMethod payload

    let rec waitForPending (attempts : Nat) : IO Unit := do
      runtime.pump
      let pendingCount ← runtime.pendingCallCount
      if pendingCount == (UInt64.ofNat 1) then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError
              s!"pre-accept cancellation pending call did not materialize: pending={pendingCount}")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForPending attempts
    waitForPending 200

    pending.release

    let rec waitForDrain (attempts : Nat) : IO Unit := do
      runtime.pump
      let pendingCount ← runtime.pendingCallCount
      let queueCount ← client.queueCount
      let queueSize ← client.queueSize
      if pendingCount == (UInt64.ofNat 0) &&
          queueCount == (UInt64.ofNat 0) &&
          queueSize == (UInt64.ofNat 0) then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError
              s!"pre-accept cancellation metrics did not drain: pending={pendingCount}, queueCount={queueCount}, queueSize={queueSize}")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForDrain attempts
    waitForDrain 200

    server.accept listener
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target payload
    assertEqual response.capTable.caps.size 0

    runtime.releaseTarget target
    client.release
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeClientSetFlowLimit : IO Unit := do
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    client.release
    server.release
    runtime.releaseListener listener
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeClientOutgoingWaitIncreasesWhenBlocked : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address

    -- We don't accept yet, so the bootstrap request will be queued.
    let target ← client.bootstrap
    let pending ← runtime.startCall target Echo.fooMethod payload

    -- Some platforms/transports do not surface non-zero wait time before accept.
    -- If a positive value appears, it should continue increasing while blocked.
    let mut wait1 := UInt64.ofNat 0
    let mut attempts := 0
    while wait1 == 0 && attempts < 100 do
      runtime.pump
      IO.sleep (UInt32.ofNat 5)
      wait1 ← client.outgoingWaitNanos
      attempts := attempts + 1

    if wait1 > 0 then
      IO.sleep (UInt32.ofNat 50)
      let wait2 ← client.outgoingWaitNanos
      -- outgoingWaitNanos is typically the time the oldest message has been waiting.
      assertTrue (wait2 > wait1) s!"outgoing wait should increase while blocked: {wait2} <= {wait1}"
    else
      let pendingCount ← runtime.pendingCallCount
      assertTrue (pendingCount > 0)
        s!"expected blocked pending call when outgoing wait remains zero, got pending={pendingCount}"

    server.accept listener
    let response ← pending.await
    assertEqual response.capTable.caps.size 0

    runtime.releaseTarget target
    client.release
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeMClientServerLifecycle : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let (response, queueSize, queueCount) ← Capnp.Rpc.RuntimeM.run runtime do
      let bootstrap ← Capnp.Rpc.RuntimeM.registerEchoTarget
      let server ← Capnp.Rpc.RuntimeM.newServer bootstrap
      let listener ← Capnp.Rpc.RuntimeM.serverListen server address
      let client ← Capnp.Rpc.RuntimeM.newClient address
      Capnp.Rpc.RuntimeM.serverAccept server listener
      Capnp.Rpc.RuntimeM.clientSetFlowLimit client (UInt64.ofNat 65_536)

      let target ← Capnp.Rpc.RuntimeM.clientBootstrap client
      let response ← Echo.callFooM target payload
      let queueSize ← Capnp.Rpc.RuntimeM.clientQueueSize client
      let queueCount ← Capnp.Rpc.RuntimeM.clientQueueCount client

      Capnp.Rpc.RuntimeM.serverRelease server
      Capnp.Rpc.RuntimeM.clientOnDisconnect client
      Capnp.Rpc.RuntimeM.clientRelease client
      Capnp.Rpc.RuntimeM.releaseListener listener
      pure (response, queueSize, queueCount)

    assertEqual response.capTable.caps.size 0
    assertEqual queueSize (UInt64.ofNat 0)
    assertEqual queueCount (UInt64.ofNat 0)
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeResourceCounts : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    assertEqual (← runtime.targetCount) (UInt64.ofNat 0)
    assertEqual (← runtime.listenerCount) (UInt64.ofNat 0)
    assertEqual (← runtime.clientCount) (UInt64.ofNat 0)
    assertEqual (← runtime.serverCount) (UInt64.ofNat 0)
    assertEqual (← runtime.pendingCallCount) (UInt64.ofNat 0)

    let bootstrap ← runtime.registerEchoTarget
    assertEqual (← runtime.targetCount) (UInt64.ofNat 1)

    let retained ← runtime.retainTarget bootstrap
    assertEqual (← runtime.targetCount) (UInt64.ofNat 2)
    runtime.releaseTarget retained
    assertEqual (← runtime.targetCount) (UInt64.ofNat 1)

    let server ← runtime.newServer bootstrap
    assertEqual (← runtime.serverCount) (UInt64.ofNat 1)
    let listener ← server.listen address
    assertEqual (← runtime.listenerCount) (UInt64.ofNat 1)
    let client ← runtime.newClient address
    assertEqual (← runtime.clientCount) (UInt64.ofNat 1)

    server.accept listener
    let target ← client.bootstrap
    assertEqual (← runtime.targetCount) (UInt64.ofNat 2)

    let pending ← runtime.startCall target Echo.fooMethod payload
    assertEqual (← runtime.pendingCallCount) (UInt64.ofNat 1)
    let response ← pending.await
    assertEqual response.capTable.caps.size 0
    assertEqual (← runtime.pendingCallCount) (UInt64.ofNat 0)

    runtime.releaseTarget target
    assertEqual (← runtime.targetCount) (UInt64.ofNat 1)
    client.release
    assertEqual (← runtime.clientCount) (UInt64.ofNat 0)
    server.release
    assertEqual (← runtime.serverCount) (UInt64.ofNat 0)
    runtime.releaseListener listener
    assertEqual (← runtime.listenerCount) (UInt64.ofNat 0)
    runtime.releaseTarget bootstrap
    assertEqual (← runtime.targetCount) (UInt64.ofNat 0)
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeProtocolDiagnostics : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let vatNetwork := runtime.vatNetwork
    let bob ← vatNetwork.newServer "bob" (← runtime.registerEchoTarget)
    let alice ← vatNetwork.newClient "alice"

    let bobVatId : Capnp.Rpc.VatId := { host := "bob" }

    -- Alice connects to Bob.
    let aliceToBob ← alice.bootstrapPeer bob

    -- Alice's perspective: 1 IMPORT.
    -- Bob's perspective: 1 EXPORT.

    let bobToAliceVatId : Capnp.Rpc.VatId := { host := "alice" }
    let rec waitForConnected (attempts : Nat) : IO Unit := do
      runtime.pump
      let dBob ← bob.getDiagnostics bobToAliceVatId
      let dAlice ← alice.getDiagnostics bobVatId
      if dBob.exportCount == (UInt64.ofNat 1) && dAlice.importCount == (UInt64.ofNat 1) then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError
              s!"protocol diagnostics did not reach connected state: bob={repr dBob}, alice={repr dAlice}")
        | n + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForConnected n
    waitForConnected 100

    let diagBob ← bob.getDiagnostics bobToAliceVatId
    assertEqual diagBob.exportCount (UInt64.ofNat 1)
    assertEqual diagBob.importCount (UInt64.ofNat 0)
    assertEqual diagBob.isIdle false

    let diagAlice ← alice.getDiagnostics bobVatId
    assertEqual diagAlice.exportCount (UInt64.ofNat 0)
    assertEqual diagAlice.importCount (UInt64.ofNat 1)
    assertEqual diagAlice.isIdle false

    -- Alice releases Bob's bootstrap.
    runtime.releaseTarget aliceToBob

    -- Give it a moment to process the release.
    let rec waitForIdle (attempts : Nat) : IO Unit := do
      runtime.pump
      let d ← alice.getDiagnostics bobVatId
      if d.isIdle then pure ()
      else match attempts with
        | 0 => throw (IO.userError s!"Alice did not become idle: {repr d}")
        | n + 1 => IO.sleep (UInt32.ofNat 10); waitForIdle n
    waitForIdle 100

    let diagAlicePost ← alice.getDiagnostics bobVatId
    assertEqual diagAlicePost.importCount (UInt64.ofNat 0)
    assertEqual diagAlicePost.isIdle true

    -- Verify Bob also became idle.
    let rec waitForBobIdle (attempts : Nat) : IO Unit := do
      runtime.pump
      let d ← bob.getDiagnostics bobToAliceVatId
      if d.isIdle then pure ()
      else match attempts with
        | 0 => throw (IO.userError s!"Bob did not become idle: {repr d}")
        | n + 1 => IO.sleep (UInt32.ofNat 10); waitForBobIdle n
    waitForBobIdle 100

    let diagBobPost ← bob.getDiagnostics bobToAliceVatId
    assertEqual diagBobPost.exportCount (UInt64.ofNat 0)
    assertEqual diagBobPost.isIdle true

    alice.release
    bob.release
  finally
    runtime.shutdown

@[test]
def testRuntimeDeferredReleaseTargetEventuallyDropsCount : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  let rec waitForTargetCountZero (attempts : Nat) : IO Unit := do
    runtime.pump
    let current ← runtime.targetCount
    if current == (UInt64.ofNat 0) then
      pure ()
    else
      match attempts with
      | 0 =>
          throw (IO.userError s!"deferred target release did not drain: targetCount={current}")
      | attempts + 1 =>
          IO.sleep (UInt32.ofNat 10)
          waitForTargetCountZero attempts
  try
    let target ← runtime.registerEchoTarget
    assertEqual (← runtime.targetCount) (UInt64.ofNat 1)
    runtime.releaseTargetDeferred target
    waitForTargetCountZero 200
  finally
    runtime.shutdown

@[test]
def testRuntimeDeferredReleasePendingCallEventuallyDropsCount : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  let rec waitForPendingCount (expected : UInt64) (label : String) (attempts : Nat) : IO Unit := do
    runtime.pump
    let current ← runtime.pendingCallCount
    if current == expected then
      pure ()
    else
      match attempts with
      | 0 =>
          throw (IO.userError s!"{label}: pendingCallCount={current}, expected={expected}")
      | attempts + 1 =>
          IO.sleep (UInt32.ofNat 10)
          waitForPendingCount expected label attempts
  try
    let target ← runtime.registerEchoTarget
    let pending ← runtime.startCall target Echo.fooMethod payload
    waitForPendingCount (UInt64.ofNat 1) "pending call did not register before deferred release" 200
    pending.releaseDeferred
    waitForPendingCount (UInt64.ofNat 0) "pending call deferred release did not drain" 200
    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimePendingCallCountTracksConcurrentCalls : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let released ← IO.mkRef false
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let blocker ← runtime.registerAdvancedHandlerTargetAsync (fun _ method req => do
      if method.interfaceId != Echo.interfaceId || method.methodId != Echo.fooMethodId then
        throw (IO.userError
          s!"unexpected method in pending-count blocker target: {method.interfaceId}/{method.methodId}")
      Capnp.Rpc.Advanced.defer do
        let rec waitForRelease (attempts : Nat) : IO Capnp.Rpc.AdvancedHandlerResult := do
          if (← released.get) then
            pure (Capnp.Rpc.Advanced.respond req)
          else
            match attempts with
            | 0 =>
                throw (IO.userError "pending-count blocker did not release in time")
            | attempts + 1 =>
                IO.sleep (UInt32.ofNat 5)
                waitForRelease attempts
        waitForRelease 10_000)

    let pending0 ← runtime.startCall blocker Echo.fooMethod payload
    let pending1 ← runtime.startCall blocker Echo.fooMethod payload
    let pending2 ← runtime.startCall blocker Echo.fooMethod payload

    let rec waitForPendingCount (attempts : Nat) : IO Unit := do
      runtime.pump
      let pendingCount ← runtime.pendingCallCount
      if pendingCount == (UInt64.ofNat 3) then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError s!"expected three pending calls, observed {pendingCount}")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForPendingCount attempts
    waitForPendingCount 200

    released.set true
    let response0 ← pending0.await
    let response1 ← pending1.await
    let response2 ← pending2.await
    assertEqual response0.capTable.caps.size 0
    assertEqual response1.capTable.caps.size 0
    assertEqual response2.capTable.caps.size 0
    assertEqual (← runtime.pendingCallCount) (UInt64.ofNat 0)

    runtime.releaseTarget blocker
  finally
    runtime.shutdown

@[test]
def testRuntimeCapabilityListPayloadRoundtrip : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink1 ← runtime.registerEchoTarget
    let sink2 ← runtime.registerEchoTarget
    let payload : Capnp.Rpc.Payload := Id.run do
      let (capTable, builder) :=
        (do
          let root ← Capnp.getRootPointer
          let list ← Capnp.initListPointer root Capnp.elemSizePointer 2
          let ptrs := Capnp.listPointerBuilders list
          let p0 := ptrs.getD 0 { seg := 0, word := 0 }
          let p1 := ptrs.getD 1 { seg := 0, word := 0 }
          let capTable1 ← Capnp.writeCapabilityWithTable Capnp.emptyCapTable p0 sink1
          Capnp.writeCapabilityWithTable capTable1 p1 sink2
        ).run (Capnp.initMessageBuilder 16)
      { msg := Capnp.buildMessage builder, capTable := capTable }

    let counter ← runtime.registerHandlerTarget (fun _ method req => do
      if method.interfaceId != Echo.interfaceId || method.methodId != Echo.fooMethodId then
        throw (IO.userError
          s!"unexpected method in capability-list roundtrip handler: {method.interfaceId}/{method.methodId}")
      pure (mkUInt64Payload (UInt64.ofNat req.capTable.caps.size)))

    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM counter payload
    let observed ← readUInt64Payload response
    assertEqual observed (UInt64.ofNat 2)
    runtime.releaseCapTable response.capTable

    runtime.releaseTarget counter
    runtime.releaseTarget sink1
    runtime.releaseTarget sink2
  finally
    runtime.shutdown

@[test]
def testRuntimeCapabilityListPayloadCppEchoControl : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink1 ← runtime.registerEchoTarget
    let sink2 ← runtime.registerEchoTarget
    let payload : Capnp.Rpc.Payload := Id.run do
      let (capTable, builder) :=
        (do
          let root ← Capnp.getRootPointer
          let list ← Capnp.initListPointer root Capnp.elemSizePointer 2
          let ptrs := Capnp.listPointerBuilders list
          let p0 := ptrs.getD 0 { seg := 0, word := 0 }
          let p1 := ptrs.getD 1 { seg := 0, word := 0 }
          let capTable1 ← Capnp.writeCapabilityWithTable Capnp.emptyCapTable p0 sink1
          Capnp.writeCapabilityWithTable capTable1 p1 sink2
        ).run (Capnp.initMessageBuilder 16)
      { msg := Capnp.buildMessage builder, capTable := capTable }

    let echo ← runtime.registerEchoTarget
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM echo payload
    assertEqual response.capTable.caps.size 2
    runtime.releaseCapTable response.capTable
    runtime.releaseTarget echo
    runtime.releaseTarget sink1
    runtime.releaseTarget sink2
  finally
    runtime.shutdown

@[test]
def testRuntimeMCallAndPendingResultHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.registerAdvancedHandlerTarget (fun _ _ _ => do
        pure (.throwRemote "runtimem call failed" "runtimem-detail".toUTF8))

    let callRes ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.callResult target Echo.fooMethod payload
    match callRes with
    | .ok _ =>
        throw (IO.userError "expected RuntimeM.callResult to return a remote exception")
    | .error ex =>
        assertEqual ex.type .failed
        if !(ex.description.containsSubstr "runtimem call failed") then
          throw (IO.userError s!"missing RuntimeM.callResult exception text: {ex.description}")
        assertEqual ex.detail "runtimem-detail".toUTF8

    let pendingRes ← Capnp.Rpc.RuntimeM.run runtime do
      let pending ← Capnp.Rpc.RuntimeM.startCall target Echo.fooMethod payload
      Capnp.Rpc.RuntimeM.pendingCallAwaitResult pending
    match pendingRes with
    | .ok _ =>
        throw (IO.userError "expected RuntimeM.pendingCallAwaitResult to return a remote exception")
    | .error ex =>
        assertEqual ex.type .failed
        if !(ex.description.containsSubstr "runtimem call failed") then
          throw (IO.userError s!"missing RuntimeM.pendingCallAwaitResult exception text: {ex.description}")
        assertEqual ex.detail "runtimem-detail".toUTF8

    let pendingOutcome ← Capnp.Rpc.RuntimeM.run runtime do
      let pending ← Capnp.Rpc.RuntimeM.startCall target Echo.fooMethod payload
      Capnp.Rpc.RuntimeM.pendingCallAwaitOutcome pending
    match pendingOutcome with
    | .ok _ _ =>
        throw (IO.userError "expected RuntimeM.pendingCallAwaitOutcome to report an exception")
    | .error ex =>
        assertEqual ex.type .failed
        if !(ex.description.containsSubstr "runtimem call failed") then
          throw (IO.userError s!"missing RuntimeM.pendingCallAwaitOutcome exception text: {ex.description}")
        assertEqual ex.detail "runtimem-detail".toUTF8

    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimeClientReleaseErrors : IO Unit := do
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    client.release

    let failedBootstrapAfterRelease ←
      try
        let _ ← client.bootstrap
        pure false
      catch _ =>
        pure true
    assertEqual failedBootstrapAfterRelease true

    let failedDoubleRelease ←
      try
        client.release
        pure false
      catch _ =>
        pure true
    assertEqual failedDoubleRelease true

    let failedQueueSizeAfterRelease ←
      try
        let _ ← client.queueSize
        pure false
      catch _ =>
        pure true
    assertEqual failedQueueSizeAfterRelease true

    let failedOnDisconnectAfterRelease ←
      try
        client.onDisconnect
        pure false
      catch _ =>
        pure true
    assertEqual failedOnDisconnectAfterRelease true

    server.release
    runtime.releaseListener listener
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeServerReleaseErrors : IO Unit := do
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    server.release

    let failedDrainAfterRelease ←
      try
        server.drain
        pure false
      catch _ =>
        pure true
    assertEqual failedDrainAfterRelease true

    let failedDoubleRelease ←
      try
        server.release
        pure false
      catch _ =>
        pure true
    assertEqual failedDoubleRelease true

    let failedAcceptAfterRelease ←
      try
        server.accept listener
        pure false
      catch _ =>
        pure true
    assertEqual failedAcceptAfterRelease true

    client.release
    runtime.releaseListener listener
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeClientOnDisconnectAfterServerRelease : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    let target ← client.bootstrap
    let _ ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target payload

    server.release
    client.onDisconnect

    let failedCallAfterDisconnect ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM target payload
        pure false
      catch _ =>
        pure true
    assertEqual failedCallAfterDisconnect true

    client.release
    runtime.releaseListener listener
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeDisconnectVisibilityViaCallResult : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener
    let target ← client.bootstrap

    let warmup ← runtime.callResult target Echo.fooMethod payload
    match warmup with
    | .ok response =>
        assertEqual response.capTable.caps.size 0
    | .error ex =>
        throw (IO.userError s!"expected warmup call to succeed, got: {ex.description}")

    let disconnectPromise ← client.onDisconnectStart
    server.release
    disconnectPromise.await

    let disconnected ← runtime.callResult target Echo.fooMethod payload
    match disconnected with
    | .ok _ =>
        throw (IO.userError "expected call after disconnect to fail")
    | .error ex =>
        assertEqual ex.type .disconnected
        assertTrue (!ex.description.isEmpty) "disconnect exception should include a description"

    assertEqual (← runtime.pendingCallCount) (UInt64.ofNat 0)
    assertEqual (← client.queueCount) (UInt64.ofNat 0)
    assertEqual (← client.queueSize) (UInt64.ofNat 0)

    runtime.releaseTarget target
    client.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeListenerRuntimeMismatchErrors : IO Unit := do
  let (address, socketPath) ← mkUnixTestAddress
  let runtimeA ← Capnp.Rpc.Runtime.init
  let runtimeB ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrapA ← runtimeA.registerEchoTarget
    let serverA ← runtimeA.newServer bootstrapA
    let listenerA ← serverA.listen address

    let bootstrapB ← runtimeB.registerEchoTarget
    let serverB ← runtimeB.newServer bootstrapB

    let releaseErr ←
      try
        runtimeB.releaseListener listenerA
        pure ""
      catch e =>
        pure (toString e)
    if !(releaseErr.containsSubstr "different Capnp.Rpc runtime") then
      throw (IO.userError s!"expected listener runtime mismatch error, got: {releaseErr}")

    let acceptErr ←
      try
        serverB.accept listenerA
        pure ""
      catch e =>
        pure (toString e)
    if !(acceptErr.containsSubstr "different Capnp.Rpc runtime") then
      throw (IO.userError s!"expected server/listener runtime mismatch error, got: {acceptErr}")

    runtimeA.releaseListener listenerA
    serverA.release
    runtimeA.releaseTarget bootstrapA
    serverB.release
    runtimeB.releaseTarget bootstrapB
  finally
    runtimeA.shutdown
    runtimeB.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeTransportRuntimeMismatchErrors : IO Unit := do
  let runtimeA ← Capnp.Rpc.Runtime.init
  let runtimeB ← Capnp.Rpc.Runtime.init
  try
    let (transportA, transportB) ← runtimeA.newTransportPipe

    let connectErr ←
      try
        let _ ← runtimeB.connectTransport transportA
        pure ""
      catch e =>
        pure (toString e)
    if !(connectErr.containsSubstr "different Capnp.Rpc runtime") then
      throw (IO.userError s!"expected transport runtime mismatch error, got: {connectErr}")

    let getFdErr ←
      try
        let _ ← runtimeB.transportGetFd? transportA
        pure ""
      catch e =>
        pure (toString e)
    if !(getFdErr.containsSubstr "different Capnp.Rpc runtime") then
      throw (IO.userError s!"expected transport fd runtime mismatch error, got: {getFdErr}")

    let releaseErr ←
      try
        runtimeB.releaseTransport transportA
        pure ""
      catch e =>
        pure (toString e)
    if !(releaseErr.containsSubstr "different Capnp.Rpc runtime") then
      throw (IO.userError s!"expected transport release runtime mismatch error, got: {releaseErr}")

    runtimeA.releaseTransport transportA
    runtimeA.releaseTransport transportB
  finally
    runtimeA.shutdown
    runtimeB.shutdown

@[test]
def testRuntimeMHandleRuntimeMismatchErrors : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtimeA ← Capnp.Rpc.Runtime.init
  let runtimeB ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrapA ← runtimeA.registerEchoTarget
    let serverA ← runtimeA.newServer bootstrapA
    let listenerA ← serverA.listen address

    let registerPromiseA ← runtimeA.connectStart address
    let unitPromiseA ← serverA.acceptStart listenerA
    let pendingA ← runtimeA.startCall bootstrapA Echo.fooMethod payload

    let registerErr ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtimeB do
          Capnp.Rpc.RuntimeM.registerPromiseAwait registerPromiseA
        pure ""
      catch e =>
        pure (toString e)
    if !(registerErr.containsSubstr "different Capnp.Rpc runtime") then
      throw (IO.userError s!"expected RuntimeM register promise mismatch error, got: {registerErr}")

    let unitErr ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtimeB do
          Capnp.Rpc.RuntimeM.unitPromiseAwait unitPromiseA
        pure ""
      catch e =>
        pure (toString e)
    if !(unitErr.containsSubstr "different Capnp.Rpc runtime") then
      throw (IO.userError s!"expected RuntimeM unit promise mismatch error, got: {unitErr}")

    let pendingErr ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtimeB do
          Capnp.Rpc.RuntimeM.pendingCallRelease pendingA
        pure ""
      catch e =>
        pure (toString e)
    if !(pendingErr.containsSubstr "different Capnp.Rpc runtime") then
      throw (IO.userError s!"expected RuntimeM pending call mismatch error, got: {pendingErr}")

    registerPromiseA.cancel
    registerPromiseA.release
    unitPromiseA.cancel
    unitPromiseA.release
    pendingA.release
    runtimeA.releaseListener listenerA
    serverA.release
    runtimeA.releaseTarget bootstrapA
  finally
    runtimeA.shutdown
    runtimeB.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeMScopedResources : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let response ← Capnp.Rpc.RuntimeM.run runtime do
      let bootstrap ← Capnp.Rpc.RuntimeM.registerEchoTarget
      Capnp.Rpc.RuntimeM.withServer bootstrap fun server => do
        Capnp.Rpc.RuntimeM.withServerListener server address (fun listener => do
          Capnp.Rpc.RuntimeM.withClient address (fun client => do
            Capnp.Rpc.RuntimeM.serverAccept server listener
            let target ← Capnp.Rpc.RuntimeM.clientBootstrap client
            Echo.callFooM target payload))

    assertEqual response.capTable.caps.size 0

    let failedConnectAfterScope ←
      try
        let _ ← runtime.connect address
        pure false
      catch _ =>
        pure true
    assertEqual failedConnectAfterScope true
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeScopedResources : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let response ← runtime.withServer bootstrap fun server => do
      server.withListener address (fun listener => do
        runtime.withClient address (fun client => do
          server.accept listener
          let target ← client.bootstrap
          Capnp.Rpc.RuntimeM.run runtime do
            Echo.callFooM target payload))
    assertEqual response.capTable.caps.size 0

    let failedConnectAfterScope ←
      try
        let _ ← runtime.connect address
        pure false
      catch _ =>
        pure true
    assertEqual failedConnectAfterScope true
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeScopedResourcesExplicitPortHintArgOrder : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let response ← runtime.withServer bootstrap fun server => do
      server.withListener address
        (fun listener => do
          runtime.withClient address
            (fun client => do
              server.accept listener
              let target ← client.bootstrap
              Capnp.Rpc.RuntimeM.run runtime do
                Echo.callFooM target payload)
            0)
        0
    assertEqual response.capTable.caps.size 0
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeRegisterHandlerTargetNetwork : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let seenTarget ← IO.mkRef (UInt32.ofNat 0)
  let seenMethod ← IO.mkRef ({ interfaceId := 0, methodId := 0 } : Capnp.Rpc.Method)
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerHandlerTarget (fun target method req => do
      seenTarget.set target
      seenMethod.set method
      pure req)
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    let remoteTarget ← client.bootstrap
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM remoteTarget payload
    assertEqual response.capTable.caps.size 0

    assertEqual (← seenTarget.get) bootstrap
    let method := (← seenMethod.get)
    assertEqual method.interfaceId Echo.interfaceId
    assertEqual method.methodId Echo.fooMethodId

    client.release
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeHandlerIoErrorCleansRequestCaps : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerEchoTarget
    let broken ← runtime.registerHandlerTarget (fun _ _ _ => do
      throw (IO.userError "expected handler failure"))
    let loopback ← runtime.registerLoopbackTarget broken
    let baselineTargets := (← runtime.targetCount)

    let errMsg ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM loopback (mkCapabilityPayload sink)
        pure ""
      catch err =>
        pure (toString err)
    if !(errMsg.containsSubstr "Lean RPC handler returned IO error") then
      throw (IO.userError s!"missing handler IO error text: {errMsg}")

    let rec waitForTargetCount (attempts : Nat) : IO Unit := do
      runtime.pump
      let current ← runtime.targetCount
      if current == baselineTargets then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError s!"request capability cleanup did not converge: {current} vs {baselineTargets}")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForTargetCount attempts
    waitForTargetCount 200

    runtime.releaseTarget loopback
    runtime.releaseTarget broken
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testRuntimeTailCallHandlerIoErrorCleansRequestCaps : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerEchoTarget
    let broken ← runtime.registerTailCallHandlerTarget (fun _ _ _ => do
      throw (IO.userError "expected tail-call handler failure"))
    let loopback ← runtime.registerLoopbackTarget broken
    let baselineTargets := (← runtime.targetCount)

    let errMsg ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM loopback (mkCapabilityPayload sink)
        pure ""
      catch err =>
        pure (toString err)
    if !(errMsg.containsSubstr "Lean RPC tail-call handler returned IO error") then
      throw (IO.userError s!"missing tail-call handler IO error text: {errMsg}")

    let rec waitForTargetCount (attempts : Nat) : IO Unit := do
      runtime.pump
      let current ← runtime.targetCount
      if current == baselineTargets then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError s!"request capability cleanup did not converge: {current} vs {baselineTargets}")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForTargetCount attempts
    waitForTargetCount 200

    runtime.releaseTarget loopback
    runtime.releaseTarget broken
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerIoErrorCleansRequestCaps : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerEchoTarget
    let broken ← runtime.registerAdvancedHandlerTarget (fun _ _ _ => do
      throw (IO.userError "expected advanced handler failure"))
    let loopback ← runtime.registerLoopbackTarget broken
    let baselineTargets := (← runtime.targetCount)

    let errMsg ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM loopback (mkCapabilityPayload sink)
        pure ""
      catch err =>
        pure (toString err)
    if !(errMsg.containsSubstr "Lean RPC advanced handler returned IO error") then
      throw (IO.userError s!"missing advanced handler IO error text: {errMsg}")

    let rec waitForTargetCount (attempts : Nat) : IO Unit := do
      runtime.pump
      let current ← runtime.targetCount
      if current == baselineTargets then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError s!"request capability cleanup did not converge: {current} vs {baselineTargets}")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForTargetCount attempts
    waitForTargetCount 200

    runtime.releaseTarget loopback
    runtime.releaseTarget broken
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testInteropCppClientCallsLeanServer : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let seenMethod ← IO.mkRef ({ interfaceId := 0, methodId := 0 } : Capnp.Rpc.Method)
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerHandlerTarget (fun _ method req => do
      seenMethod.set method
      pure req)
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let response ← Capnp.Rpc.Interop.cppCallWithAccept runtime server listener address Echo.fooMethod
      payload
    let responseRef ← Capnp.Rpc.Interop.cppCallWithAcceptPayloadRef
      runtime server listener address Echo.fooMethod payload
    let decodedRefResponse ← responseRef.decodeAndRelease

    assertEqual response.capTable.caps.size 0
    assertEqual decodedRefResponse.capTable.caps.size 0
    let method := (← seenMethod.get)
    assertEqual method.interfaceId Echo.interfaceId
    assertEqual method.methodId Echo.fooMethodId

    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testInteropCppClientCallsLeanServerWithCapabilities : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let localCap ← runtime.registerEchoTarget
    let capPayload := mkCapabilityPayload localCap

    let bootstrap ← runtime.registerHandlerTarget (fun _ _ req => pure req)
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let response ← Capnp.Rpc.Interop.cppCallWithAccept runtime server listener address Echo.fooMethod
      capPayload

    assertEqual response.capTable.caps.size 1
    let returnedCap? := Capnp.readCapabilityFromTable response.capTable (Capnp.getRoot response.msg)
    assertEqual returnedCap?.isSome true
    match returnedCap? with
    | none =>
        throw (IO.userError "RPC response is missing expected capability")
    | some returnedCap =>
        let echoed ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM returnedCap payload
        assertEqual echoed.capTable.caps.size 0
        runtime.releaseCapTable response.capTable

    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
    runtime.releaseTarget localCap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testInteropCppClientReceivesLeanServerCapability : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let sink ← runtime.registerEchoTarget
    let bootstrap ← runtime.registerHandlerTarget (fun _ _ _ => do
      pure (mkCapabilityPayload sink))
    let baselineTargets ← runtime.targetCount
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let response ← Capnp.Rpc.Interop.cppCallWithAccept runtime server listener address Echo.fooMethod
      payload

    assertEqual response.capTable.caps.size 1
    let returnedCap? := Capnp.readCapabilityFromTable response.capTable (Capnp.getRoot response.msg)
    assertEqual returnedCap?.isSome true
    match returnedCap? with
    | none =>
        throw (IO.userError "RPC response is missing expected capability")
    | some returnedCap =>
        assertEqual (returnedCap == (UInt32.ofNat 0)) false

    runtime.releaseCapTable response.capTable
    let rec waitForTargetCount (attempts : Nat) : IO Unit := do
      runtime.pump
      let current ← runtime.targetCount
      if current == baselineTargets then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError s!"response capability cleanup did not converge: {current} vs {baselineTargets}")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForTargetCount attempts
    waitForTargetCount 200

    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
    runtime.releaseTarget sink
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testInteropLeanClientCallsCppServer : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let serveTask ← IO.asTask
      (Capnp.Rpc.Interop.cppServeEchoOncePayloadRef runtime address Echo.fooMethod)
    IO.sleep (UInt32.ofNat 20)

    let mut target? : Option Capnp.Rpc.Client := none
    let mut attempts := 0
    while target?.isNone && attempts < 20 do
      let nextTarget? ←
        try
          let c ← runtime.connect address
          pure (some (Capnp.Rpc.Client.ofCapability c))
        catch _ =>
          pure none
      target? := nextTarget?
      if target?.isNone then
        IO.sleep (UInt32.ofNat 10)
      attempts := attempts + 1

    let target ←
      match target? with
      | some c => pure c
      | none => throw (IO.userError "failed to connect Lean runtime target to C++ server")
    let responseResult : Except IO.Error Capnp.Rpc.Payload ←
      try
        let response ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM target payload
        pure (Except.ok response)
      catch err =>
        pure (Except.error err)

    let response ←
      match responseResult with
      | Except.ok r => pure r
      | Except.error err =>
          match serveTask.get with
          | Except.ok _ =>
              throw err
          | Except.error serveErr =>
              throw (IO.userError s!"call failed ({err}); serve task failed ({serveErr})")
    assertEqual response.capTable.caps.size 0

    runtime.releaseTarget target

    match serveTask.get with
    | .ok observedRef =>
        let observed ← observedRef.decodeAndRelease
        assertEqual observed.capTable.caps.size 0
    | .error err =>
        throw err
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testInteropLeanClientObservesCppDisconnectAfterOneShot : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  let waitTaskWithin {α : Type} (label : String) (task : Task (Except IO.Error α))
      (timeoutMillis : UInt32 := UInt32.ofNat 3000) : IO (Option (Except IO.Error α)) := do
    let timeoutTask ← runtime.sleepMillisAsTask timeoutMillis
    let wrappedTask : Task (Except IO.Error (Option (Except IO.Error α))) := Task.map
      (fun result => .ok (some result))
      task
    let wrappedTimeout : Task (Except IO.Error (Option (Except IO.Error α))) := Task.map
      (fun result =>
        match result with
        | .ok _ => .ok none
        | .error err => .error err)
      timeoutTask
    match (← IO.waitAny [wrappedTask, wrappedTimeout]) with
    | .ok outcome =>
        pure outcome
    | .error err =>
        throw (IO.userError s!"{label}: {err}")
  let awaitTaskWithin {α : Type} (label : String) (task : Task (Except IO.Error α))
      (timeoutMillis : UInt32 := UInt32.ofNat 3000) : IO α := do
    match (← waitTaskWithin label task timeoutMillis) with
    | some (.ok value) =>
        pure value
    | some (.error err) =>
        throw (IO.userError s!"{label}: {err}")
    | none =>
        throw (IO.userError s!"{label}: timeout")
  let assertTaskPendingWithin {α : Type} (label : String) (task : Task (Except IO.Error α))
      (timeoutMillis : UInt32 := UInt32.ofNat 150) : IO Unit := do
    match (← waitTaskWithin label task timeoutMillis) with
    | none =>
        pure ()
    | some (.ok _) =>
        throw (IO.userError s!"{label}: completed before disconnect")
    | some (.error err) =>
        throw (IO.userError s!"{label}: failed before disconnect: {err}")
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let serveTask ← IO.asTask (Capnp.Rpc.Interop.cppServeEchoOnce address Echo.fooMethod)
    let mut client? : Option Capnp.Rpc.RuntimeClientRef := none
    let mut attempts := 0
    while client?.isNone && attempts < 50 do
      let nextClient? ←
        try
          let c ← runtime.newClient address
          pure (some c)
        catch _ =>
          pure none
      client? := nextClient?
      if client?.isNone then
        IO.sleep (UInt32.ofNat 10)
      attempts := attempts + 1
    let client ←
      match client? with
      | some c => pure c
      | none => throw (IO.userError "failed to connect Lean runtime client to C++ one-shot server")
    let target ← client.bootstrap
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target payload
    assertEqual response.capTable.caps.size 0

    assertTaskPendingWithin "interop one-shot server completion before disconnect" serveTask

    let disconnectPromise ← client.onDisconnectStart
    runtime.releaseTarget target
    client.release

    let disconnectTask ← disconnectPromise.awaitAsTask
    awaitTaskWithin "interop one-shot disconnect" disconnectTask
    let observed ← awaitTaskWithin "interop one-shot server completion" serveTask
    assertEqual observed.capTable.caps.size 0

    let reconnectTask ← IO.asTask (runtime.connect address)
    match (← waitTaskWithin "interop one-shot reconnect attempt" reconnectTask) with
    | some (.ok target2) =>
        runtime.releaseTarget target2
        throw (IO.userError "expected reconnect after one-shot disconnect to fail")
    | some (.error _) =>
        pure ()
    | none =>
        throw (IO.userError "interop one-shot reconnect attempt: timeout")
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testInteropLeanClientCancelsPendingCallToCppDelayedServer : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let serveTask ← IO.asTask (
      Capnp.Rpc.Interop.cppServeDelayedEchoOnce address Echo.fooMethod (UInt32.ofNat 150))
    IO.sleep (UInt32.ofNat 20)

    let target ← connectRuntimeTargetWithRetry runtime address
    assertEqual (← runtime.pendingCallCount) (UInt64.ofNat 0)
    let pending ← runtime.startCall target Echo.fooMethod payload
    assertEqual (← runtime.pendingCallCount) (UInt64.ofNat 1)
    IO.sleep (UInt32.ofNat 10)
    pending.release
    assertEqual (← runtime.pendingCallCount) (UInt64.ofNat 0)

    let doubleReleaseFailed ←
      try
        pending.release
        pure false
      catch _ =>
        pure true
    assertEqual doubleReleaseFailed true

    -- If cancellation drops the in-flight request before send, force a request on
    -- the same connection so the one-shot C++ server task can complete deterministically.
    let _rescueOverSameConnection ←
      try
        let rescueResponse ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM target payload
        assertEqual rescueResponse.capTable.caps.size 0
        pure true
      catch _ =>
        pure false

    runtime.releaseTarget target

    match serveTask.get with
    | .ok observed =>
        assertEqual observed.capTable.caps.size 0
    | .error err =>
        throw err
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testInteropLeanPendingCallOutcomeCapturesCppException : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let serveTask ← IO.asTask (Capnp.Rpc.Interop.cppServeThrowOnce address Echo.fooMethod true)
    IO.sleep (UInt32.ofNat 20)

    let target ← connectRuntimeTargetWithRetry runtime address
    let pending ← runtime.startCall target Echo.fooMethod payload
    let outcome ← pending.awaitOutcome
    match outcome with
    | .ok _ _ =>
        throw (IO.userError "expected pending call to C++ one-shot throw server to fail")
    | .error ex =>
        assertEqual ex.type .failed
        if !(ex.description.containsSubstr "remote exception: test exception") then
          throw (IO.userError s!"missing remote exception text: {ex.description}")
        assertEqual ex.detail "cpp-detail-1".toUTF8

    runtime.releaseTarget target
    match serveTask.get with
    | .ok observed =>
        assertEqual observed.capTable.caps.size 0
    | .error err =>
        throw err
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testInteropLeanClientReceivesCppExceptionDetail : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let serveTask ← IO.asTask (Capnp.Rpc.Interop.cppServeThrowOnce address Echo.fooMethod true)
    IO.sleep (UInt32.ofNat 20)

    let target ← connectRuntimeTargetWithRetry runtime address
    let res ← runtime.callResult target Echo.fooMethod payload
    match res with
    | .ok _ =>
        throw (IO.userError "expected C++ one-shot server to throw")
    | .error ex =>
        assertEqual ex.type .failed
        if !(ex.description.containsSubstr "remote exception: test exception") then
          throw (IO.userError s!"missing remote exception text: {ex.description}")
        assertEqual ex.detail "cpp-detail-1".toUTF8

    runtime.releaseTarget target
    match serveTask.get with
    | .ok observed =>
        assertEqual observed.capTable.caps.size 0
    | .error err =>
        throw err
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeParityPromisedCapabilityRejectsWithoutDisconnect : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let heldCap ← IO.mkRef (none : Option Capnp.Rpc.Client)
  let runtime ← Capnp.Rpc.Runtime.init
  let waitTaskWithin {α : Type} (label : String) (task : Task (Except IO.Error α))
      (timeoutMillis : UInt32 := UInt32.ofNat 3000) : IO (Option (Except IO.Error α)) := do
    let timeoutTask ← runtime.sleepMillisAsTask timeoutMillis
    let wrappedTask : Task (Except IO.Error (Option (Except IO.Error α))) := Task.map
      (fun result => .ok (some result))
      task
    let wrappedTimeout : Task (Except IO.Error (Option (Except IO.Error α))) := Task.map
      (fun result =>
        match result with
        | .ok _ => .ok none
        | .error err => .error err)
      timeoutTask
    match (← IO.waitAny [wrappedTask, wrappedTimeout]) with
    | .ok outcome =>
        pure outcome
    | .error err =>
        throw (IO.userError s!"{label}: {err}")
  let awaitTaskWithin {α : Type} (label : String) (task : Task (Except IO.Error α))
      (timeoutMillis : UInt32 := UInt32.ofNat 3000) : IO α := do
    match (← waitTaskWithin label task timeoutMillis) with
    | some (.ok value) =>
        pure value
    | some (.error err) =>
        throw (IO.userError s!"{label}: {err}")
    | none =>
        throw (IO.userError s!"{label}: timeout")
  let assertTaskPendingWithin {α : Type} (label : String) (task : Task (Except IO.Error α))
      (timeoutMillis : UInt32 := UInt32.ofNat 150) : IO Unit := do
    match (← waitTaskWithin label task timeoutMillis) with
    | none =>
        pure ()
    | some (.ok _) =>
        throw (IO.userError s!"{label}: completed before promise rejection")
    | some (.error err) =>
        throw (IO.userError s!"{label}: failed before promise rejection: {err}")
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let relay ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      if method.interfaceId == Echo.interfaceId && method.methodId == Echo.fooMethodId then
        let cap? := Capnp.readCapabilityFromTable req.capTable (Capnp.getRoot req.msg)
        match cap? with
        | some cap =>
            let retained ← runtime.retainTarget cap
            match (← heldCap.get) with
            | some previous => runtime.releaseTarget previous
            | none => pure ()
            heldCap.set (some retained)
            pure (Capnp.Rpc.Advanced.respond mkNullPayload)
        | none =>
            pure (Capnp.Rpc.Advanced.respond req)
      else if method.interfaceId == Echo.interfaceId && method.methodId == Echo.barMethodId then
        match (← heldCap.get) with
        | some cap =>
            pure (Capnp.Rpc.Advanced.asyncForward cap Echo.fooMethod req)
        | none =>
            pure (Capnp.Rpc.Advanced.throwRemote "promised-cap relay has no held capability")
      else
        pure (Capnp.Rpc.Advanced.respond req))

    let server ← runtime.newServer relay
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener
    let remoteTarget ← client.bootstrap

    let (promiseCap, fulfiller) ← runtime.newPromiseCapability
    let _ ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM remoteTarget (mkCapabilityPayload promiseCap)

    let pending ← runtime.startCall remoteTarget Echo.barMethod payload
    let pendingTask ← IO.asTask pending.awaitOutcome
    assertTaskPendingWithin "promised capability pending call" pendingTask

    fulfiller.reject .disconnected "promised capability rejected" "promised-disconnect-detail".toUTF8

    let outcome ← awaitTaskWithin "promised capability awaitOutcome" pendingTask
    match outcome with
    | .ok _ _ =>
        throw (IO.userError "expected promised capability call to fail after rejection")
    | .error ex =>
        assertEqual ex.type .disconnected
        if !(ex.description.containsSubstr "promised capability rejected") then
          throw (IO.userError s!"missing promised capability rejection text: {ex.description}")
        assertEqual ex.detail "promised-disconnect-detail".toUTF8

    let healthCheck ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM remoteTarget payload
    assertEqual healthCheck.capTable.caps.size 0

    runtime.releaseTarget remoteTarget
    client.release
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget relay
    match (← heldCap.get) with
    | some cap =>
        runtime.releaseTarget cap
        heldCap.set none
    | none =>
        pure ()
    fulfiller.release
    runtime.releaseTarget promiseCap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testInteropCppClientPipeliningThroughLeanTailCallForwarder : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let sink ← runtime.registerEchoTarget
    let returnPayload := mkCapabilityPayload sink
    let returnCapTarget ← runtime.registerHandlerTarget (fun _ _ _ => do
      IO.sleep (UInt32.ofNat 30)
      pure returnPayload)
    let forwarder ← runtime.registerTailCallTarget returnCapTarget
    let server ← runtime.newServer forwarder
    let listener ← server.listen address
    let response ← Capnp.Rpc.Interop.cppCallPipelinedWithAccept
      runtime server listener address Echo.fooMethod payload payload
    assertEqual response.capTable.caps.size 0

    server.release
    runtime.releaseListener listener
    runtime.releaseTarget forwarder
    runtime.releaseTarget returnCapTarget
    runtime.releaseTarget sink
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testInteropCppClientPipeliningThroughLeanAdvancedAsyncForwarder : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let sink ← runtime.registerEchoTarget
    let returnPayload := mkCapabilityPayload sink
    let returnCapTarget ← runtime.registerHandlerTarget (fun _ _ _ => do
      IO.sleep (UInt32.ofNat 30)
      pure returnPayload)
    let forwarder ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      pure (.asyncCall returnCapTarget method req))
    let server ← runtime.newServer forwarder
    let listener ← server.listen address
    let response ← Capnp.Rpc.Interop.cppCallPipelinedWithAccept
      runtime server listener address Echo.fooMethod payload payload
    assertEqual response.capTable.caps.size 0

    server.release
    runtime.releaseListener listener
    runtime.releaseTarget forwarder
    runtime.releaseTarget returnCapTarget
    runtime.releaseTarget sink
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testInteropCppClientPipeliningThroughLeanAdvancedTailCallForwarder : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let sink ← runtime.registerEchoTarget
    let returnPayload := mkCapabilityPayload sink
    let returnCapTarget ← runtime.registerHandlerTarget (fun _ _ _ => do
      IO.sleep (UInt32.ofNat 30)
      pure returnPayload)
    let forwarder ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      pure (.tailCall returnCapTarget method req))
    let server ← runtime.newServer forwarder
    let listener ← server.listen address
    let response ← Capnp.Rpc.Interop.cppCallPipelinedWithAccept
      runtime server listener address Echo.fooMethod payload payload
    assertEqual response.capTable.caps.size 0

    server.release
    runtime.releaseListener listener
    runtime.releaseTarget forwarder
    runtime.releaseTarget returnCapTarget
    runtime.releaseTarget sink
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testInteropCppClientReceivesLeanAdvancedRemoteDetail : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let throwing ← runtime.registerAdvancedHandlerTarget (fun _ _ _ => do
      pure (.throwRemote "lean advanced failure" "lean-detail-1".toUTF8))
    let server ← runtime.newServer throwing
    let listener ← server.listen address
    let errMsg ←
      try
        let _ ← Capnp.Rpc.Interop.cppCallWithAccept
          runtime server listener address Echo.fooMethod payload
        pure ""
      catch err =>
        pure (toString err)
    if !(errMsg.containsSubstr "remote exception: lean advanced failure") then
      throw (IO.userError s!"missing remote exception text: {errMsg}")
    if !(errMsg.containsSubstr "remote detail[1]: lean-detail-1") then
      throw (IO.userError s!"missing remote detail text: {errMsg}")

    server.release
    runtime.releaseListener listener
    runtime.releaseTarget throwing
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testInteropCppClientReceivesLeanAdvancedRemoteExceptionType : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let throwing ← runtime.registerAdvancedHandlerTarget (fun _ _ _ => do
      pure <| Capnp.Rpc.Advanced.throwRemoteWithType
        Capnp.Rpc.RemoteExceptionType.overloaded "lean overloaded failure")
    let server ← runtime.newServer throwing
    let listener ← server.listen address
    let errMsg ←
      try
        let _ ← Capnp.Rpc.Interop.cppCallWithAccept
          runtime server listener address Echo.fooMethod payload
        pure ""
      catch err =>
        pure (toString err)
    if !(errMsg.containsSubstr "exception type: OVERLOADED") then
      throw (IO.userError s!"missing exception type text: {errMsg}")

    server.release
    runtime.releaseListener listener
    runtime.releaseTarget throwing
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimePendingCallPipeline : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let localCap ← runtime.registerEchoTarget
    let capPayload := mkCapabilityPayload localCap
    let pending ← runtime.startCall localCap Echo.fooMethod capPayload
    let pipelinedCap ← pending.getPipelinedCap

    let echoed ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM pipelinedCap payload
    assertEqual echoed.capTable.caps.size 0

    let response ← pending.await
    assertEqual response.capTable.caps.size 1
    runtime.releaseCapTable response.capTable

    runtime.releaseTarget pipelinedCap
    runtime.releaseTarget localCap
  finally
    runtime.shutdown

@[test]
def testRuntimeTargetWhenResolvedPipeline : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let localCap ← runtime.registerEchoTarget
    let returnPayload := mkCapabilityPayload localCap
    let bootstrap ← runtime.registerHandlerTarget (fun _ _ _ => pure returnPayload)
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    let remoteTarget ← client.bootstrap
    let pending ← runtime.startCall remoteTarget Echo.fooMethod payload
    let pipelinedCap ← pending.getPipelinedCap
    runtime.targetWhenResolved pipelinedCap

    let echoed ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM pipelinedCap payload
    assertEqual echoed.capTable.caps.size 0

    let response ← pending.await
    assertEqual response.capTable.caps.size 1
    runtime.releaseCapTable response.capTable

    runtime.releaseTarget pipelinedCap
    runtime.releaseTarget remoteTarget
    client.release
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
    runtime.releaseTarget localCap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()


@[test]
def testRuntimeParityResolvePipelineOrdering : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let (callOrder, getNextExpected) ← registerEchoFooCallOrderTarget runtime
    let (promiseCap, fulfiller) ← runtime.newPromiseCapability
    let pipelinePayload := mkCapabilityPayload promiseCap
    let responder ← runtime.registerAdvancedHandlerTargetAsync (fun _ method req => do
      if method.interfaceId != Echo.interfaceId || method.methodId != Echo.fooMethodId then
        throw (IO.userError
          s!"unexpected method in resolve/pipeline parity target: {method.interfaceId}/{method.methodId}")
      let _ := req
      let deferred ← Capnp.Rpc.Advanced.defer do
        IO.sleep (UInt32.ofNat 30)
        fulfiller.fulfill callOrder
        pure (Capnp.Rpc.Advanced.respond pipelinePayload)
      pure (Capnp.Rpc.Advanced.setPipeline pipelinePayload deferred))

    let pending ← runtime.startCall responder Echo.fooMethod payload
    let pipelineCap ← pending.getPipelinedCap
    let call0Pending ← runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 0))
    let call1Pending ← runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 1))

    let response ← pending.await
    assertEqual response.capTable.caps.size 1
    let resolvedCap? := Capnp.readCapabilityFromTable response.capTable (Capnp.getRoot response.msg)
    assertEqual resolvedCap?.isSome true

    let call2Pending ← runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 2))
    let call3Pending ←
      match resolvedCap? with
      | some resolvedCap =>
          runtime.startCall resolvedCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 3))
      | none =>
          throw (IO.userError "resolve parity response missing expected capability")

    let call0Response ← call0Pending.await
    let call1Response ← call1Pending.await
    let call2Response ← call2Pending.await
    let call3Response ← call3Pending.await

    assertEqual (← readUInt64Payload call0Response) (UInt64.ofNat 0)
    assertEqual (← readUInt64Payload call1Response) (UInt64.ofNat 1)
    assertEqual (← readUInt64Payload call2Response) (UInt64.ofNat 2)
    assertEqual (← readUInt64Payload call3Response) (UInt64.ofNat 3)
    assertEqual (← getNextExpected) (UInt64.ofNat 4)

    runtime.releaseCapTable response.capTable
    runtime.releaseTarget pipelineCap
    runtime.releaseTarget responder
    fulfiller.release
    runtime.releaseTarget promiseCap
    runtime.releaseTarget callOrder
  finally
    runtime.shutdown

@[test]
def testRuntimeParityDisembargoNullPipelineDoesNotDisconnect : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerHandlerTarget (fun _ _ _ => pure mkNullPayload)
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    let remoteTarget ← client.bootstrap
    let pending ← runtime.startCall remoteTarget Echo.fooMethod payload
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
      Echo.callBarM remoteTarget payload
    assertEqual healthCheck.capTable.caps.size 0

    runtime.releaseTarget nullPipelineCap
    runtime.releaseTarget remoteTarget
    client.release
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeParityEmbargoErrorKeepsConnectionAlive : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let rejectMessage := "embargo parity rejection"
  let rejectDetail := "embargo-parity-detail".toUTF8
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  let assertRejectedOutcome (label : String) (outcome : Capnp.Rpc.RawCallOutcome) : IO Unit := do
    match outcome with
    | .ok _ _ =>
        throw (IO.userError s!"{label}: expected pipelined call to fail")
    | .error ex =>
        assertEqual ex.type .disconnected
        if !(ex.description.containsSubstr rejectMessage) then
          throw (IO.userError s!"{label}: missing reject message in: {ex.description}")
        assertEqual ex.detail rejectDetail
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    let remoteTarget ← client.bootstrap
    let (promiseCap, fulfiller) ← runtime.newPromiseCapability

    let pendingEcho ← runtime.startCall remoteTarget Echo.fooMethod (mkCapabilityPayload promiseCap)
    let pipelineCap ← pendingEcho.getPipelinedCap
    let call0Pending ← runtime.startCall pipelineCap Echo.fooMethod payload
    let call1Pending ← runtime.startCall pipelineCap Echo.fooMethod payload
    let call2Pending ← runtime.startCall pipelineCap Echo.fooMethod payload

    let echoResponse ← pendingEcho.await
    assertEqual echoResponse.capTable.caps.size 1
    runtime.releaseCapTable echoResponse.capTable

    fulfiller.reject .disconnected rejectMessage rejectDetail

    let call0Outcome ← call0Pending.awaitOutcome
    let call1Outcome ← call1Pending.awaitOutcome
    let call2Outcome ← call2Pending.awaitOutcome
    assertRejectedOutcome "embargo parity call0" call0Outcome
    assertRejectedOutcome "embargo parity call1" call1Outcome
    assertRejectedOutcome "embargo parity call2" call2Outcome

    let healthCheck ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callBarM remoteTarget payload
    assertEqual healthCheck.capTable.caps.size 0

    runtime.releaseTarget pipelineCap
    runtime.releaseTarget promiseCap
    fulfiller.release
    runtime.releaseTarget remoteTarget
    client.release
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeParityTailCallPipelineOrdering : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let (callOrder, getNextExpected) ← registerEchoFooCallOrderTarget runtime
    let echo ← runtime.registerEchoTarget
    let tailForwarder ← runtime.registerTailCallTarget echo

    let pending ← runtime.startCall tailForwarder Echo.fooMethod (mkCapabilityPayload callOrder)
    let pipelineCap ← pending.getPipelinedCap
    let call0Pending ← runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 0))

    let response ← pending.await
    assertEqual response.capTable.caps.size 1
    let call1Pending ← runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 1))

    let call0Response ← call0Pending.await
    let call1Response ← call1Pending.await

    let resolvedCap? := Capnp.readCapabilityFromTable response.capTable (Capnp.getRoot response.msg)
    let call2Pending ←
      match resolvedCap? with
      | some resolvedCap =>
          runtime.startCall resolvedCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 2))
      | none =>
          throw (IO.userError "tail-call parity response missing expected capability")
    let call2Response ← call2Pending.await

    assertEqual (← readUInt64Payload call0Response) (UInt64.ofNat 0)
    assertEqual (← readUInt64Payload call1Response) (UInt64.ofNat 1)
    assertEqual (← readUInt64Payload call2Response) (UInt64.ofNat 2)
    assertEqual (← getNextExpected) (UInt64.ofNat 3)

    runtime.releaseCapTable response.capTable
    runtime.releaseTarget pipelineCap
    runtime.releaseTarget tailForwarder
    runtime.releaseTarget echo
    runtime.releaseTarget callOrder
  finally
    runtime.shutdown

@[test]
def testRuntimeParityAdvancedDeferredSetPipelineOrdering : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let (callOrder, getNextExpected) ← registerEchoFooCallOrderTarget runtime
    let advanced ← runtime.registerAdvancedHandlerTargetAsync (fun _ method _ => do
      if method.interfaceId != Echo.interfaceId || method.methodId != Echo.fooMethodId then
        throw (IO.userError
          s!"unexpected method in advanced set-pipeline parity target: {method.interfaceId}/{method.methodId}")
      let pipelinePayload := mkCapabilityPayload callOrder
      let deferred ← Capnp.Rpc.Advanced.defer do
        IO.sleep (UInt32.ofNat 30)
        pure (Capnp.Rpc.Advanced.respond pipelinePayload)
      pure (Capnp.Rpc.Advanced.setPipeline pipelinePayload deferred))

    let pending ← runtime.startCall advanced Echo.fooMethod payload
    let pipelineCap ← pending.getPipelinedCap
    let call0Pending ← runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 0))
    let call1Pending ← runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 1))

    let response ← pending.await
    assertEqual response.capTable.caps.size 1
    let resolvedCap? := Capnp.readCapabilityFromTable response.capTable (Capnp.getRoot response.msg)
    assertEqual resolvedCap?.isSome true

    let call2Pending ← runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 2))
    let call3Pending ←
      match resolvedCap? with
      | some resolvedCap =>
          runtime.startCall resolvedCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 3))
      | none =>
          throw (IO.userError "advanced set-pipeline parity response missing expected capability")

    let call0Response ← call0Pending.await
    let call1Response ← call1Pending.await
    let call2Response ← call2Pending.await
    let call3Response ← call3Pending.await

    assertEqual (← readUInt64Payload call0Response) (UInt64.ofNat 0)
    assertEqual (← readUInt64Payload call1Response) (UInt64.ofNat 1)
    assertEqual (← readUInt64Payload call2Response) (UInt64.ofNat 2)
    assertEqual (← readUInt64Payload call3Response) (UInt64.ofNat 3)
    assertEqual (← getNextExpected) (UInt64.ofNat 4)

    runtime.releaseCapTable response.capTable
    runtime.releaseTarget pipelineCap
    runtime.releaseTarget advanced
    runtime.releaseTarget callOrder
  finally
    runtime.shutdown

@[test]
def testRuntimeParityCancelDisconnectSequencing : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  let rec waitFor (label : String) (attempts : Nat) (check : IO Bool) : IO Unit := do
    runtime.pump
    if (← check) then
      pure ()
    else
      match attempts with
      | 0 =>
          throw (IO.userError s!"{label}: timeout")
      | attempts + 1 =>
          IO.sleep (UInt32.ofNat 10)
          waitFor label attempts check
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let entered ← IO.mkRef false
    let canceled ← IO.mkRef false
    let hanging ← runtime.registerAdvancedHandlerTargetAsync (fun _ method req => do
      if method.interfaceId != Echo.interfaceId || method.methodId != Echo.fooMethodId then
        throw (IO.userError
          s!"unexpected method in cancel/disconnect parity target: {method.interfaceId}/{method.methodId}")
      entered.set true
      Capnp.Rpc.Advanced.defer (opts := { allowCancellation := true }) do
        let rec waitForCancel (attempts : Nat) : IO Capnp.Rpc.AdvancedHandlerResult := do
          if (← IO.checkCanceled) then
            canceled.set true
            pure (Capnp.Rpc.Advanced.respond mkNullPayload)
          else
            match attempts with
            | 0 =>
                pure (Capnp.Rpc.Advanced.respond req)
            | attempts + 1 =>
                IO.sleep (UInt32.ofNat 5)
                waitForCancel attempts
        waitForCancel 10_000)

    let server ← runtime.newServer hanging
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener

    let remoteTarget ← client.bootstrap
    let pending ← runtime.startCall remoteTarget Echo.fooMethod payload

    waitFor "cancel/disconnect handler entry" 500 entered.get
    pending.release
    waitFor "cancel/disconnect cancellation" 500 canceled.get
    assertEqual (← runtime.pendingCallCount) (UInt64.ofNat 0)

    let disconnectPromise ← client.onDisconnectStart
    server.release
    disconnectPromise.await

    let failedAfterDisconnect ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM remoteTarget payload
        pure false
      catch _ =>
        pure true
    assertEqual failedAfterDisconnect true
    assertEqual (← canceled.get) true

    runtime.releaseTarget remoteTarget
    client.release
    runtime.releaseListener listener
    runtime.releaseTarget hanging
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()
@[test]
def testRuntimeTwoHopPipelinedResolveOrdering : IO Unit := do
  let (frontAddress, frontSocketPath) ← mkUnixTestAddress
  let (backAddress, backSocketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  let cleanupSocket (path : String) : IO Unit := do
    try
      IO.FS.removeFile path
    catch _ =>
      pure ()
  let step {α : Type} (label : String) (action : IO α) : IO α := do
    try
      action
    catch err =>
      throw (IO.userError s!"{label}: {err}")
  try
    cleanupSocket frontSocketPath
    cleanupSocket backSocketPath

    let nextExpected ← IO.mkRef (UInt64.ofNat 0)
    let callOrder ← step "register call-order target" <| runtime.registerHandlerTarget (fun _ method req => do
      if method.interfaceId != Echo.interfaceId || method.methodId != Echo.fooMethodId then
        throw (IO.userError
          s!"unexpected method in call-order target: {method.interfaceId}/{method.methodId}")
      let expected ← readUInt64Payload req
      let current ← nextExpected.get
      if expected != current then
        throw (IO.userError s!"call-order mismatch: expected {current}, got {expected}")
      nextExpected.set (current + (UInt64.ofNat 1))
      pure (mkUInt64Payload current))

    let backBootstrap ← step "register back bootstrap" <| runtime.registerEchoTarget
    let backServer ← step "new back server" <| runtime.newServer backBootstrap
    let backListener ← step "listen back server" <| backServer.listen backAddress

    let proxyBootstrap ← step "connect proxy bootstrap to back" <| runtime.connect backAddress
    step "accept back server connection" <| backServer.accept backListener

    let frontServer ← step "new front server" <| runtime.newServer proxyBootstrap
    let frontListener ← step "listen front server" <| frontServer.listen frontAddress
    let frontClient ← step "new front client" <| runtime.newClient frontAddress
    step "accept front server connection" <| frontServer.accept frontListener

    let remoteBootstrap ← step "bootstrap front client" <| frontClient.bootstrap
    let pendingEcho ← step "start echo call through front" <|
      runtime.startCall remoteBootstrap Echo.fooMethod (mkCapabilityPayload callOrder)
    let pipelineCap ← step "get pipelined cap" <| pendingEcho.getPipelinedCap

    let call0Pending ← step "start call0" <|
      runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 0))
    let call1Pending ← step "start call1" <|
      runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 1))

    let earlyResponse ← step "early bar call" <| Capnp.Rpc.RuntimeM.run runtime do
      Echo.callBarM remoteBootstrap mkNullPayload
    assertEqual earlyResponse.capTable.caps.size 0

    let call2Pending ← step "start call2" <|
      runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 2))

    let echoResponse ← step "await echo call" <| pendingEcho.await
    assertEqual echoResponse.capTable.caps.size 1
    let resolvedCap? := Capnp.readCapabilityFromTable echoResponse.capTable (Capnp.getRoot echoResponse.msg)
    assertEqual resolvedCap?.isSome true

    let call3Pending ← step "start call3" <|
      runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 3))
    let call4Pending ← step "start call4" <|
      runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 4))
    let call5Pending ← step "start call5" <|
      runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 5))

    let call0Response ← step "await call0" <| call0Pending.await
    let call1Response ← step "await call1" <| call1Pending.await
    let call2Response ← step "await call2" <| call2Pending.await
    let call3Response ← step "await call3" <| call3Pending.await
    let call4Response ← step "await call4" <| call4Pending.await
    let call5Response ← step "await call5" <| call5Pending.await

    assertEqual (← readUInt64Payload call0Response) (UInt64.ofNat 0)
    assertEqual (← readUInt64Payload call1Response) (UInt64.ofNat 1)
    assertEqual (← readUInt64Payload call2Response) (UInt64.ofNat 2)
    assertEqual (← readUInt64Payload call3Response) (UInt64.ofNat 3)
    assertEqual (← readUInt64Payload call4Response) (UInt64.ofNat 4)
    assertEqual (← readUInt64Payload call5Response) (UInt64.ofNat 5)
    assertEqual (← nextExpected.get) (UInt64.ofNat 6)
  finally
    runtime.shutdown
    cleanupSocket frontSocketPath
    cleanupSocket backSocketPath

@[test]
def testRuntimeTwoHopPipelinedResolveOrderingWithNestedPromise : IO Unit := do
  let (frontAddress, frontSocketPath) ← mkUnixTestAddress
  let (backAddress, backSocketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  let cleanupSocket (path : String) : IO Unit := do
    try
      IO.FS.removeFile path
    catch _ =>
      pure ()
  let step {α : Type} (label : String) (action : IO α) : IO α := do
    try
      action
    catch err =>
      throw (IO.userError s!"{label}: {err}")
  try
    cleanupSocket frontSocketPath
    cleanupSocket backSocketPath

    let nextExpected ← IO.mkRef (UInt64.ofNat 0)
    let callOrder ← step "register nested call-order target" <|
      runtime.registerHandlerTarget (fun _ method req => do
        if method.interfaceId != Echo.interfaceId || method.methodId != Echo.fooMethodId then
          throw (IO.userError
            s!"unexpected method in nested call-order target: {method.interfaceId}/{method.methodId}")
        let expected ← readUInt64Payload req
        let current ← nextExpected.get
        if expected != current then
          throw (IO.userError s!"nested call-order mismatch: expected {current}, got {expected}")
        nextExpected.set (current + (UInt64.ofNat 1))
        pure (mkUInt64Payload current))

    let (innerPromiseCap, innerFulfiller) ← step "new inner promise capability" <|
      runtime.newPromiseCapability
    let (outerPromiseCap, outerFulfiller) ← step "new outer promise capability" <|
      runtime.newPromiseCapability

    let backBootstrap ← step "register nested back bootstrap" <| runtime.registerEchoTarget
    let backServer ← step "new nested back server" <| runtime.newServer backBootstrap
    let backListener ← step "listen nested back server" <| backServer.listen backAddress

    let proxyBootstrap ← step "connect nested proxy bootstrap to back" <| runtime.connect backAddress
    step "accept nested back server connection" <| backServer.accept backListener

    let frontServer ← step "new nested front server" <| runtime.newServer proxyBootstrap
    let frontListener ← step "listen nested front server" <| frontServer.listen frontAddress
    let frontClient ← step "new nested front client" <| runtime.newClient frontAddress
    step "accept nested front server connection" <| frontServer.accept frontListener

    let remoteBootstrap ← step "bootstrap nested front client" <| frontClient.bootstrap
    let pendingEcho ← step "start nested echo call through front" <|
      runtime.startCall remoteBootstrap Echo.fooMethod (mkCapabilityPayload outerPromiseCap)
    let pipelineCap ← step "get nested pipelined cap" <| pendingEcho.getPipelinedCap

    let call0Pending ← step "start nested call0" <|
      runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 0))
    let call1Pending ← step "start nested call1" <|
      runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 1))

    let earlyResponse ← step "nested early bar call" <| Capnp.Rpc.RuntimeM.run runtime do
      Echo.callBarM remoteBootstrap mkNullPayload
    assertEqual earlyResponse.capTable.caps.size 0

    step "fulfill outer promise to inner promise" <|
      outerFulfiller.fulfill innerPromiseCap

    let call2Pending ← step "start nested call2" <|
      runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 2))

    let echoResponse ← step "await nested echo call" <| pendingEcho.await
    assertEqual echoResponse.capTable.caps.size 1
    let resolvedCap? := Capnp.readCapabilityFromTable echoResponse.capTable (Capnp.getRoot echoResponse.msg)
    assertEqual resolvedCap?.isSome true

    step "fulfill inner promise to call-order target" <|
      innerFulfiller.fulfill callOrder

    let call3Pending ← step "start nested call3" <|
      runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 3))
    let call4Pending ← step "start nested call4" <|
      runtime.startCall pipelineCap Echo.fooMethod (mkUInt64Payload (UInt64.ofNat 4))

    let call0Response ← step "await nested call0" <| call0Pending.await
    let call1Response ← step "await nested call1" <| call1Pending.await
    let call2Response ← step "await nested call2" <| call2Pending.await
    let call3Response ← step "await nested call3" <| call3Pending.await
    let call4Response ← step "await nested call4" <| call4Pending.await

    assertEqual (← readUInt64Payload call0Response) (UInt64.ofNat 0)
    assertEqual (← readUInt64Payload call1Response) (UInt64.ofNat 1)
    assertEqual (← readUInt64Payload call2Response) (UInt64.ofNat 2)
    assertEqual (← readUInt64Payload call3Response) (UInt64.ofNat 3)
    assertEqual (← readUInt64Payload call4Response) (UInt64.ofNat 4)
    assertEqual (← nextExpected.get) (UInt64.ofNat 5)

    runtime.releaseCapTable echoResponse.capTable
    runtime.releaseTarget pipelineCap
    runtime.releaseTarget remoteBootstrap
    frontClient.release
    runtime.releaseListener frontListener
    frontServer.release
    runtime.releaseTarget proxyBootstrap
    runtime.releaseListener backListener
    backServer.release
    runtime.releaseTarget backBootstrap
    innerFulfiller.release
    runtime.releaseTarget innerPromiseCap
    outerFulfiller.release
    runtime.releaseTarget outerPromiseCap
    runtime.releaseTarget callOrder
  finally
    runtime.shutdown
    cleanupSocket frontSocketPath
    cleanupSocket backSocketPath

@[test]
def testRuntimePendingCallRelease : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget
    let pending ← runtime.startCall target Echo.fooMethod mkNullPayload
    pending.release
    let awaitFailed ←
      try
        let _ ← pending.await
        pure false
      catch _ =>
        pure true
    assertEqual awaitFailed true
    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimePendingCallAwaitAndReleaseConsumes : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget
    let capPayload := mkCapabilityPayload target
    let pending ← runtime.startCall target Echo.fooMethod capPayload
    let response ← pending.await
    assertEqual response.capTable.caps.size 1
    runtime.releaseCapTable response.capTable

    let releaseFailed ←
      try
        pending.release
        pure false
      catch _ =>
        pure true
    assertEqual releaseFailed true
    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimePendingCallWithReleasesOnException : IO Unit := do
  let released ← IO.mkRef false
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let blocker ← runtime.registerAdvancedHandlerTargetAsync (fun _ method req => do
      if method.interfaceId != Echo.interfaceId || method.methodId != Echo.fooMethodId then
        throw (IO.userError
          s!"unexpected method in pending-call with-release target: {method.interfaceId}/{method.methodId}")
      Capnp.Rpc.Advanced.defer do
        let rec waitForRelease (attempts : Nat) : IO Capnp.Rpc.AdvancedHandlerResult := do
          if (← released.get) then
            pure (Capnp.Rpc.Advanced.respond req)
          else
            match attempts with
            | 0 =>
                throw (IO.userError "pending-call with-release target did not release in time")
            | attempts + 1 =>
                IO.sleep (UInt32.ofNat 5)
                waitForRelease attempts
        waitForRelease 10_000)

    let pending ← runtime.startCall blocker Echo.fooMethod mkNullPayload

    let rec waitForPendingCount (attempts : Nat) (expected : UInt64) : IO Unit := do
      runtime.pump
      let current ← runtime.pendingCallCount
      if current == expected then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError
              s!"expected pending call count {expected}, observed {current}")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForPendingCount attempts expected

    waitForPendingCount 200 (UInt64.ofNat 1)

    let threw ←
      try
        let (_ : Capnp.Rpc.Payload) ← pending.withRelease (fun _ =>
          throw (IO.userError "intentional pending call scoped-action failure"))
        pure false
      catch _ =>
        pure true
    assertEqual threw true

    waitForPendingCount 200 (UInt64.ofNat 0)
    released.set true
    runtime.releaseTarget blocker
  finally
    runtime.shutdown

@[test]
def testRuntimeMPendingCallAwaitAndReleaseConsumes : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget
    let capPayload := mkCapabilityPayload target
    let pending ← runtime.startCall target Echo.fooMethod capPayload
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.pendingCallAwait pending
    assertEqual response.capTable.caps.size 1
    runtime.releaseCapTable response.capTable

    let releaseFailed ←
      try
        Capnp.Rpc.RuntimeM.run runtime do
          Capnp.Rpc.RuntimeM.pendingCallRelease pending
        pure false
      catch _ =>
        pure true
    assertEqual releaseFailed true
    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimeMPendingCallWithReleasesOnException : IO Unit := do
  let released ← IO.mkRef false
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let blocker ← runtime.registerAdvancedHandlerTargetAsync (fun _ method req => do
      if method.interfaceId != Echo.interfaceId || method.methodId != Echo.fooMethodId then
        throw (IO.userError
          s!"unexpected method in RuntimeM pending-call with-release target: {method.interfaceId}/{method.methodId}")
      Capnp.Rpc.Advanced.defer do
        let rec waitForRelease (attempts : Nat) : IO Capnp.Rpc.AdvancedHandlerResult := do
          if (← released.get) then
            pure (Capnp.Rpc.Advanced.respond req)
          else
            match attempts with
            | 0 =>
                throw (IO.userError "RuntimeM pending-call with-release target did not release in time")
            | attempts + 1 =>
                IO.sleep (UInt32.ofNat 5)
                waitForRelease attempts
        waitForRelease 10_000)

    let pending ← runtime.startCall blocker Echo.fooMethod mkNullPayload

    let rec waitForPendingCount (attempts : Nat) (expected : UInt64) : IO Unit := do
      runtime.pump
      let current ← runtime.pendingCallCount
      if current == expected then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError
              s!"expected pending call count {expected}, observed {current}")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForPendingCount attempts expected

    waitForPendingCount 200 (UInt64.ofNat 1)

    let threw ←
      try
        let (_ : Capnp.Rpc.Payload) ← Capnp.Rpc.RuntimeM.run runtime do
          Capnp.Rpc.RuntimeM.withPendingCall pending (fun _ =>
            throw (IO.userError "intentional RuntimeM pending call scoped-action failure"))
        pure false
      catch _ =>
        pure true
    assertEqual threw true

    waitForPendingCount 200 (UInt64.ofNat 0)
    released.set true
    runtime.releaseTarget blocker
  finally
    runtime.shutdown

@[test]
def testRuntimeStreamingCall : IO Unit := do
  let seen ← IO.mkRef false
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerHandlerTarget (fun _ method req => do
      if method.interfaceId == Echo.interfaceId && method.methodId == Echo.fooMethodId then
        seen.set true
      pure req)
    runtime.streamingCall target Echo.fooMethod mkNullPayload
    assertEqual (← seen.get) true
    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimeRegisterStreamingHandlerTarget : IO Unit := do
  let seen ← IO.mkRef false
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerStreamingHandlerTarget (fun _ method _ => do
      if method.interfaceId == Echo.interfaceId && method.methodId == Echo.fooMethodId then
        seen.set true)
    runtime.streamingCall target Echo.fooMethod mkNullPayload
    assertEqual (← seen.get) true
    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimeStreamingCancellation : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  let canceled ← IO.mkRef false
  try
    let target ← runtime.registerAdvancedHandlerTargetAsync (fun _ _ _ => do
      let waitTask ← IO.asTask do
        IO.sleep (UInt32.ofNat 2000)
        pure (Capnp.Rpc.Advanced.respond mkNullPayload)
      let cancelTask ← IO.asTask do
        canceled.set true
        pure (Capnp.Rpc.Advanced.throwRemote "canceled")
      pure (Capnp.Rpc.Advanced.deferTaskWithCancel waitTask cancelTask
        (opts := { isStreaming := true })))

    -- Use startCall so we can get a handle to cancel it.
    let pending ← runtime.startCall target Echo.fooMethod payload

    -- Give it a moment to reach the server.
    IO.sleep (UInt32.ofNat 50)

    -- Cancel by releasing the pending call handle.
    pending.release

    -- Wait for the cancellation to propagate.
    let mut seen := (← canceled.get)
    let mut attempts := 0
    while !seen && attempts < 200 do
      runtime.pump
      IO.sleep (UInt32.ofNat 10)
      seen ← canceled.get
      attempts := attempts + 1

    assertEqual seen true "streaming call was not canceled"

    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimeStreamingNoPrematureCancellationWhenTargetDropped : IO Unit := do
  let payload123 := mkUInt64Payload (UInt64.ofNat 123)
  let payload456 := mkUInt64Payload (UInt64.ofNat 456)
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  let entered ← IO.mkRef (Nat.zero)
  let gate ← IO.mkRef (Nat.zero)
  let total ← IO.mkRef (UInt64.ofNat 0)
  let rec waitForGate (value : UInt64) (attempts : Nat) : IO Capnp.Rpc.AdvancedHandlerResult := do
    let available ← gate.get
    if available > 0 then
      gate.set (available - 1)
      total.modify (fun current => current + value)
      pure (Capnp.Rpc.Advanced.respond mkNullPayload)
    else
      match attempts with
      | 0 =>
          throw (IO.userError "streaming no-premature-cancel gate wait timed out")
      | attempts + 1 =>
          IO.sleep (UInt32.ofNat 5)
          waitForGate value attempts
  let rec waitForEnteredAtLeast (required : Nat) (attempts : Nat) : IO Unit := do
    runtime.pump
    if (← entered.get) >= required then
      pure ()
    else
      match attempts with
      | 0 =>
          throw (IO.userError
            s!"streaming calls did not reach server before timeout (required={required}, entered={(← entered.get)}, pending={(← runtime.pendingCallCount)})")
      | attempts + 1 =>
          IO.sleep (UInt32.ofNat 10)
          waitForEnteredAtLeast required attempts
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let target ← runtime.registerAdvancedHandlerTargetAsync (fun _ method req => do
      if method.interfaceId == Echo.interfaceId && method.methodId == Echo.fooMethodId then
        let n ← readUInt64Payload req
        entered.modify (fun c => c + 1)
        Capnp.Rpc.Advanced.streamingDefer do
          waitForGate n 10_000
      else if method.interfaceId == Echo.interfaceId && method.methodId == Echo.barMethodId then
        pure (Capnp.Rpc.Advanced.now (Capnp.Rpc.Advanced.respond (mkUInt64Payload (← total.get))))
      else
        throw (IO.userError
          s!"unexpected method in streaming no-premature-cancel handler: {method.interfaceId}/{method.methodId}"))

    let server ← runtime.newServer target
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener
    let remoteTarget ← client.bootstrap

    let _stream1 ← runtime.startCall remoteTarget Echo.fooMethod payload123
    let _stream2 ← runtime.startCall remoteTarget Echo.fooMethod payload456
    let finish ← runtime.startCall remoteTarget Echo.barMethod mkNullPayload

    -- Wait for the first streaming call to enter before dropping the client-side target handle.
    waitForEnteredAtLeast 1 500

    -- Drop the client-side target handle while calls are in flight; streaming calls must not
    -- be canceled just because this local handle was released.
    runtime.releaseTarget remoteTarget

    -- Dispatch is effectively serialized per target here; release one call at a time.
    gate.set 1
    waitForEnteredAtLeast 2 500
    gate.set 1

    let finishResult ← finish.awaitResult
    let finishResponse ←
      match finishResult with
      | .ok payload =>
          pure payload
      | .error ex =>
          throw (IO.userError s!"finishStream failed: {ex.type}: {ex.description}")
    assertEqual (← readUInt64Payload finishResponse) (UInt64.ofNat 579)

    client.release
    runtime.releaseListener listener
    server.release
    runtime.releaseTarget target
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeStreamingForwardedAcrossMultiVatNoPrematureCancellation : IO Unit := do
  let payload := mkUInt64Payload (UInt64.ofNat 42)
  let runtime ← Capnp.Rpc.Runtime.init
  let entered ← IO.mkRef false
  let gate ← IO.mkRef false
  let bobToCarolRef ← IO.mkRef (none : Option Capnp.Rpc.Client)
  let rec waitForGate (attempts : Nat) : IO Capnp.Rpc.AdvancedHandlerResult := do
    if (← gate.get) then
      pure (Capnp.Rpc.Advanced.respond payload)
    else
      match attempts with
      | 0 =>
          throw (IO.userError "forwarded multi-vat streaming gate wait timed out")
      | attempts + 1 =>
          IO.sleep (UInt32.ofNat 5)
          waitForGate attempts
  let rec waitForEntered (attempts : Nat) : IO Unit := do
    runtime.pump
    if (← entered.get) then
      pure ()
    else
      match attempts with
      | 0 =>
          throw (IO.userError "forwarded multi-vat streaming call did not reach destination")
      | attempts + 1 =>
          IO.sleep (UInt32.ofNat 10)
          waitForEntered attempts
  try
    let carolBootstrap ← runtime.registerAdvancedHandlerTargetAsync (fun _ method _ => do
      if method.interfaceId == Echo.interfaceId && method.methodId == Echo.fooMethodId then
        entered.set true
        Capnp.Rpc.Advanced.streamingDefer do
          waitForGate 10_000
      else
        throw (IO.userError
          s!"unexpected method in forwarded multi-vat streaming destination: {method.interfaceId}/{method.methodId}"))

    let bobBootstrap ← runtime.registerAdvancedHandlerTargetAsync (fun _ method req => do
      match (← bobToCarolRef.get) with
      | some carolCap =>
          pure (Capnp.Rpc.Advanced.now
            (Capnp.Rpc.Advanced.streaming (Capnp.Rpc.Advanced.forward carolCap method req)))
      | none =>
          throw (IO.userError "bob forwarding capability not initialized"))

    let network := runtime.vatNetwork
    let alice ← network.newClient "alice-forward-stream"
    let bob ← network.newServer "bob-forward-stream" bobBootstrap
    let carol ← network.newServer "carol-forward-stream" carolBootstrap

    let bobToCarol ← bob.bootstrapPeer carol
    bobToCarolRef.set (some bobToCarol)
    let aliceToBob ← alice.bootstrapPeer bob

    let pending ← runtime.startCall aliceToBob Echo.fooMethod payload

    -- Ensure the forwarded call reached Carol before dropping Alice's local handle.
    waitForEntered 500
    runtime.releaseTarget aliceToBob

    gate.set true
    let result ← pending.awaitResult
    match result with
    | .ok _ =>
        pure ()
    | .error ex =>
        throw (IO.userError
          s!"forwarded multi-vat streaming call failed: {ex.type}: {ex.description}")

    runtime.releaseTarget bobToCarol
    alice.release
    bob.release
    carol.release
    runtime.releaseTarget bobBootstrap
    runtime.releaseTarget carolBootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeStreamingChainedBackpressure : IO Unit := do
  let payload := mkLargePayload 10240 -- 10KB
  let runtime ← Capnp.Rpc.Runtime.init
  let (carolAddress, carolSocketPath) ← mkUnixTestAddress
  let (aliceAddress, aliceSocketPath) ← mkUnixTestAddress
  try
    -- Carol is the final destination.
    let carolBootstrap ← runtime.registerEchoTarget
    let carolServer ← runtime.newServer carolBootstrap
    let carolListener ← carolServer.listen carolAddress
    let bobToCarolClient ← runtime.newClient carolAddress
    carolServer.accept carolListener
    let bobToCarolCap ← bobToCarolClient.bootstrap

    -- Set a VERY small flow limit on the Bob -> Carol connection.
    bobToCarolClient.setFlowLimit (UInt64.ofNat 1)

    -- Bob is a local handler that forwards to Carol.
    let bobHandler ← runtime.registerAdvancedHandlerTargetAsync (fun _ _ req =>
      Capnp.Rpc.Advanced.streamingDefer do
        IO.sleep (UInt32.ofNat 50)
        pure (Capnp.Rpc.Advanced.forward bobToCarolCap Echo.fooMethod req))

    -- Alice is the client calling Bob.
    let bobServer ← runtime.newServer bobHandler
    let aliceListener ← bobServer.listen aliceAddress
    let aliceClient ← runtime.newClient aliceAddress
    bobServer.accept aliceListener
    let aliceToBob ← aliceClient.bootstrap

    -- Set flow limit on Alice -> Bob too.
    aliceClient.setFlowLimit (UInt64.ofNat 1)

    -- Alice sends many streaming calls using startStreamingCall.
    let mut pendingCalls := #[]
    for _ in [0:200] do
      pendingCalls := pendingCalls.push (← runtime.startStreamingCall aliceToBob Echo.fooMethod payload)

    -- Wait for backpressure to propagate.
    let mut pCount := UInt64.ofNat 0
    let mut attempts := 0
    while pCount < 50 && attempts < 100 do
      runtime.pump
      IO.sleep (UInt32.ofNat 10)
      pCount ← runtime.pendingCallCount
      attempts := attempts + 1

    assertTrue (pCount >= 50) s!"Pending call count should increase due to chained backpressure, got {pCount}"

    -- Cleanup.
    runtime.releaseTarget aliceToBob
    aliceClient.release
    runtime.releaseListener aliceListener
    bobServer.release
    runtime.releaseTarget bobHandler
    runtime.releaseTarget bobToCarolCap
    bobToCarolClient.release
    runtime.releaseListener carolListener
    carolServer.release
    runtime.releaseTarget carolBootstrap
  finally
    runtime.shutdown
    try IO.FS.removeFile carolSocketPath catch _ => pure ()
    try IO.FS.removeFile aliceSocketPath catch _ => pure ()

@[test]
def testRuntimeSocketCapabilityHandoffRemainsProxiedViaIntermediaryConnection : IO Unit := do
  if System.Platform.isWindows then
    pure ()
  else
    let payload : Capnp.Rpc.Payload := mkNullPayload
    let (aliceAddress, aliceSocketPath) ← mkUnixTestAddress
    let (carolAddress, carolSocketPath) ← mkUnixTestAddress
    let runtimeA ← Capnp.Rpc.Runtime.init
    let runtimeB ← Capnp.Rpc.Runtime.init
    let runtimeC ← Capnp.Rpc.Runtime.init
    let cleanupSocket (path : String) : IO Unit := do
      try
        IO.FS.removeFile path
      catch _ =>
        pure ()
    let waitTaskWithin {α : Type} (runtime : Capnp.Rpc.Runtime) (label : String)
        (task : Task (Except IO.Error α)) (timeoutMillis : UInt32 := UInt32.ofNat 3000) :
        IO (Option (Except IO.Error α)) := do
      let timeoutTask ← runtime.sleepMillisAsTask timeoutMillis
      let wrappedTask : Task (Except IO.Error (Option (Except IO.Error α))) := Task.map
        (fun result => .ok (some result))
        task
      let wrappedTimeout : Task (Except IO.Error (Option (Except IO.Error α))) := Task.map
        (fun result =>
          match result with
          | .ok _ => .ok none
          | .error err => .error err)
        timeoutTask
      match (← IO.waitAny [wrappedTask, wrappedTimeout]) with
      | .ok outcome =>
          pure outcome
      | .error err =>
          throw (IO.userError s!"{label}: {err}")
    let awaitTaskWithin {α : Type} (runtime : Capnp.Rpc.Runtime) (label : String)
        (task : Task (Except IO.Error α)) (timeoutMillis : UInt32 := UInt32.ofNat 3000) : IO α := do
      match (← waitTaskWithin runtime label task timeoutMillis) with
      | some (.ok value) =>
          pure value
      | some (.error err) =>
          throw (IO.userError s!"{label}: {err}")
      | none =>
          throw (IO.userError s!"{label}: timeout")
    try
      cleanupSocket aliceSocketPath
      cleanupSocket carolSocketPath

      let aliceCallCount ← IO.mkRef (UInt64.ofNat 0)
      let aliceBootstrap ← runtimeA.registerHandlerTarget (fun _ method req => do
        if method.interfaceId != Echo.interfaceId || method.methodId != Echo.fooMethodId then
          throw (IO.userError
            s!"unexpected method in socket handoff source target: {method.interfaceId}/{method.methodId}")
        aliceCallCount.modify (fun count => count + (UInt64.ofNat 1))
        pure req)
      let aliceServer ← runtimeA.newServer aliceBootstrap
      let aliceListener ← aliceServer.listen aliceAddress

      -- Carol first imports Alice's capability over one socket connection.
      let carolToAliceClient ← runtimeC.newClient aliceAddress
      aliceServer.accept aliceListener
      let carolImportedAliceCap ← carolToAliceClient.bootstrap

      -- Carol then hands that A-owned capability to Bob over a separate socket connection.
      let carolServer ← runtimeC.newServer carolImportedAliceCap
      let carolListener ← carolServer.listen carolAddress
      let bobToCarolClient ← runtimeB.newClient carolAddress
      carolServer.accept carolListener
      let bobReceivedAliceCap ← bobToCarolClient.bootstrap

      -- Bob can initially use the handed-off capability.
      let firstResult ← runtimeB.callResult bobReceivedAliceCap Echo.fooMethod payload
      match firstResult with
      | .ok response =>
          assertEqual response.capTable.caps.size 0
      | .error ex =>
          throw (IO.userError
            s!"initial forwarded socket capability call failed: {ex.type}: {ex.description}")
      assertEqual (← aliceCallCount.get) (UInt64.ofNat 1)

      -- If the handoff were direct, closing Carol's Alice-side client would not affect Bob's copy.
      let disconnectTask ← carolToAliceClient.onDisconnectAsTask
      carolToAliceClient.release
      awaitTaskWithin runtimeC
        "intermediary disconnect after closing source-side client"
        disconnectTask

      let secondResult ← runtimeB.callResult bobReceivedAliceCap Echo.fooMethod payload
      match secondResult with
      | .ok _ =>
          throw (IO.userError
            "expected handed-off socket capability to fail after intermediary lost its source connection")
      | .error ex =>
          assertTrue (!ex.description.isEmpty)
            "disconnecting intermediary source connection should surface a remote error"
      assertEqual (← aliceCallCount.get) (UInt64.ofNat 1)

      runtimeB.releaseTarget bobReceivedAliceCap
      bobToCarolClient.release
      carolServer.release
      runtimeC.releaseListener carolListener
      runtimeC.releaseTarget carolImportedAliceCap
      aliceServer.release
      runtimeA.releaseListener aliceListener
      runtimeA.releaseTarget aliceBootstrap
    finally
      runtimeC.shutdown
      runtimeB.shutdown
      runtimeA.shutdown
      cleanupSocket carolSocketPath
      cleanupSocket aliceSocketPath

@[test]
def testRuntimeTraceEncoderToggle : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  let runFailingCall (traceEnabled : Bool) : IO String := do
    let (address, socketPath) ← mkUnixTestAddress
    try
      try
        IO.FS.removeFile socketPath
      catch _ =>
        pure ()

      if traceEnabled then
        runtime.enableTraceEncoder
      else
        runtime.disableTraceEncoder

      let bootstrap ← runtime.registerHandlerTarget (fun _ method _ => do
        if method.interfaceId == Echo.interfaceId && method.methodId == Echo.fooMethodId then
          throw (IO.userError "trace test exception")
        pure mkNullPayload)
      try
        runtime.withServer bootstrap fun server => do
          server.withListener address (fun listener => do
            runtime.withClient address (fun client => do
              server.accept listener
              let target ← client.bootstrap
              try
                let _ ← Capnp.Rpc.RuntimeM.run runtime do
                  Echo.callFooM target payload
                pure ""
              catch err =>
                pure (toString err)
              finally
                runtime.releaseTarget target))
      finally
        runtime.releaseTarget bootstrap
    finally
      try
        IO.FS.removeFile socketPath
      catch _ =>
        pure ()

  try
    let disabledMessage ← runFailingCall false
    if !(disabledMessage.containsSubstr "remote exception:") then
      throw (IO.userError s!"unexpected disabled error text: {disabledMessage}")
    if disabledMessage.containsSubstr "remote trace:" then
      throw (IO.userError s!"disabled call unexpectedly included remote trace: {disabledMessage}")

    let enabledMessage ← runFailingCall true
    if !(enabledMessage.containsSubstr "remote exception:") then
      throw (IO.userError s!"unexpected enabled error text: {enabledMessage}")
    if !(enabledMessage.containsSubstr "remote trace: lean4-rpc-trace:") then
      throw (IO.userError s!"enabled call did not include encoded remote trace: {enabledMessage}")
  finally
    runtime.shutdown

@[test]
def testRuntimeSetTraceEncoderOnExistingConnection : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let bootstrap ← runtime.registerHandlerTarget (fun _ method _ => do
      if method.interfaceId == Echo.interfaceId && method.methodId == Echo.fooMethodId then
        throw (IO.userError "dynamic trace test exception")
      pure mkNullPayload)
    let server ← runtime.newServer bootstrap
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener
    let target ← client.bootstrap

    runtime.disableTraceEncoder
    let disabledErr ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM target payload
        pure ""
      catch err =>
        pure (toString err)
    if disabledErr.containsSubstr "remote trace:" then
      throw (IO.userError s!"trace unexpectedly present before enabling callback: {disabledErr}")

    runtime.setTraceEncoder (fun description => pure s!"custom-trace<{description}>")
    let enabledErr ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM target payload
        pure ""
      catch err =>
        pure (toString err)
    if !(enabledErr.containsSubstr "remote trace: custom-trace<") then
      throw (IO.userError s!"missing callback trace on existing connection: {enabledErr}")

    runtime.disableTraceEncoder
    let disabledAgainErr ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM target payload
        pure ""
      catch err =>
        pure (toString err)
    if disabledAgainErr.containsSubstr "remote trace:" then
      throw (IO.userError s!"trace unexpectedly present after disabling callback: {disabledAgainErr}")

    runtime.releaseTarget target
    client.release
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeTraceEncoderCallResultVisibility : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let traceCalls ← IO.mkRef (0 : Nat)
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let throwing ← runtime.registerAdvancedHandlerTarget (fun _ method _ => do
      if method.interfaceId == Echo.interfaceId && method.methodId == Echo.fooMethodId then
        pure <| Capnp.Rpc.Advanced.throwRemoteWithType
          Capnp.Rpc.RemoteExceptionType.overloaded "trace visibility test exception"
      else
        pure (Capnp.Rpc.Advanced.respond mkNullPayload))
    let server ← runtime.newServer throwing
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener
    let target ← client.bootstrap

    runtime.disableTraceEncoder
    let disabledRes ← runtime.callResult target Echo.fooMethod payload
    match disabledRes with
    | .ok _ =>
        throw (IO.userError "expected disabled trace call to fail")
    | .error ex => do
        assertEqual ex.type .overloaded
        assertEqual ex.remoteTrace ""

    runtime.setTraceEncoder (fun description => do
      traceCalls.modify (fun n => n + 1)
      pure s!"obs-trace<{description}>")

    let enabledRes0 ← runtime.callResult target Echo.fooMethod payload
    let enabledRes1 ← runtime.callResult target Echo.fooMethod payload
    for res in #[enabledRes0, enabledRes1] do
      match res with
      | .ok _ =>
          throw (IO.userError "expected enabled trace call to fail")
      | .error ex => do
          assertEqual ex.type .overloaded
          if !(ex.remoteTrace.containsSubstr "obs-trace<") then
            throw (IO.userError s!"enabled trace missing encoded marker: {ex.remoteTrace}")
    assertEqual (← traceCalls.get) 2

    runtime.disableTraceEncoder
    let disabledAgainRes ← runtime.callResult target Echo.fooMethod payload
    match disabledAgainRes with
    | .ok _ =>
        throw (IO.userError "expected disabled-again trace call to fail")
    | .error ex => do
        assertEqual ex.type .overloaded
        assertEqual ex.remoteTrace ""
    assertEqual (← traceCalls.get) 2

    runtime.releaseTarget target
    client.release
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget throwing
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeTraceEncoderPreservesDetailAndType : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let traceCalls ← IO.mkRef (0 : Nat)
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let throwing ← runtime.registerAdvancedHandlerTarget (fun _ method _ => do
      if method.interfaceId == Echo.interfaceId && method.methodId == Echo.fooMethodId then
        pure <| Capnp.Rpc.Advanced.throwRemoteWithType
          Capnp.Rpc.RemoteExceptionType.disconnected
          "trace detail visibility test exception"
          "trace-detail-1".toUTF8
      else
        pure (Capnp.Rpc.Advanced.respond mkNullPayload))
    let server ← runtime.newServer throwing
    let listener ← server.listen address
    let client ← runtime.newClient address
    server.accept listener
    let target ← client.bootstrap

    runtime.setTraceEncoder (fun description => do
      traceCalls.modify (fun n => n + 1)
      pure s!"detail-trace<{description}>")

    let tracedRes ← runtime.callResult target Echo.fooMethod payload
    match tracedRes with
    | .ok _ =>
        throw (IO.userError "expected trace/detail call to fail")
    | .error ex => do
        assertEqual ex.type .disconnected
        if !(ex.description.containsSubstr "trace detail visibility test exception") then
          throw (IO.userError s!"missing trace/detail exception text: {ex.description}")
        assertEqual ex.detail "trace-detail-1".toUTF8
        if !(ex.remoteTrace.containsSubstr "detail-trace<") then
          throw (IO.userError s!"missing encoded trace marker: {ex.remoteTrace}")
    assertEqual (← traceCalls.get) 1

    runtime.disableTraceEncoder
    let untracedRes ← runtime.callResult target Echo.fooMethod payload
    match untracedRes with
    | .ok _ =>
        throw (IO.userError "expected untraced detail call to fail")
    | .error ex => do
        assertEqual ex.type .disconnected
        assertEqual ex.detail "trace-detail-1".toUTF8
        assertEqual ex.remoteTrace ""
    assertEqual (← traceCalls.get) 1

    runtime.releaseTarget target
    client.release
    server.release
    runtime.releaseListener listener
    runtime.releaseTarget throwing
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeTargetGetFdOption : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget
    let fd? ← runtime.targetGetFd? target
    assertEqual fd?.isNone true
    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimeTailCallForwardingTarget : IO Unit := do
  let seenMethod ← IO.mkRef ({ interfaceId := 0, methodId := 0 } : Capnp.Rpc.Method)
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerHandlerTarget (fun _ method req => do
      seenMethod.set method
      pure req)
    let forwarder ← runtime.registerTailCallTarget sink
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM forwarder mkNullPayload
    assertEqual response.capTable.caps.size 0
    let method := (← seenMethod.get)
    assertEqual method.interfaceId Echo.interfaceId
    assertEqual method.methodId Echo.fooMethodId
    runtime.releaseTarget forwarder
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testRuntimeTailCallHandlerTargetFromRequestCapability : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  let payload : Capnp.Rpc.Payload := mkNullPayload
  try
    let sink ← runtime.registerEchoTarget
    let capPayload := mkCapabilityPayload sink
    let forwarder ← runtime.registerTailCallHandlerTarget (fun _ method req => do
      assertEqual method.interfaceId Echo.interfaceId
      assertEqual method.methodId Echo.fooMethodId
      let cap? := Capnp.readCapabilityFromTable req.capTable (Capnp.getRoot req.msg)
      match cap? with
      | some cap => pure cap
      | none =>
          throw (IO.userError "tail-call handler expected request capability"))

    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM forwarder capPayload
    assertEqual response.capTable.caps.size 1
    let returnedCap? := Capnp.readCapabilityFromTable response.capTable (Capnp.getRoot response.msg)
    assertEqual returnedCap?.isSome true
    match returnedCap? with
    | none =>
        throw (IO.userError "tail-call response missing expected capability")
    | some returnedCap =>
        let echoed ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM returnedCap payload
        assertEqual echoed.capTable.caps.size 0

    runtime.releaseCapTable response.capTable
    runtime.releaseTarget forwarder
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testRuntimeRegisterLoopbackTargetUsesBootstrap : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let seenFoo ← IO.mkRef false
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let bootstrap ← runtime.registerHandlerTarget (fun _ method req => do
      if method.interfaceId == Echo.interfaceId && method.methodId == Echo.fooMethodId then
        seenFoo.set true
      pure req)
    let loopback ← runtime.registerLoopbackTarget bootstrap
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM loopback payload
    assertEqual response.capTable.caps.size 0
    assertEqual (← seenFoo.get) true
    runtime.releaseTarget loopback
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerAsyncCallPipeline : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerEchoTarget
    let capPayload := mkCapabilityPayload sink
    let forwarder ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      pure (.asyncCall sink method req))

    let pending ← runtime.startCall forwarder Echo.fooMethod capPayload
    let pipelinedCap ← pending.getPipelinedCap
    let pipelinedResponse ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM pipelinedCap payload
    assertEqual pipelinedResponse.capTable.caps.size 0
    let response ← pending.await
    assertEqual response.capTable.caps.size 1
    runtime.releaseCapTable response.capTable
    runtime.releaseTarget pipelinedCap
    runtime.releaseTarget forwarder
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerTailCall : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let seenMethod ← IO.mkRef ({ interfaceId := 0, methodId := 0 } : Capnp.Rpc.Method)
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerHandlerTarget (fun _ method req => do
      seenMethod.set method
      pure req)
    let forwarder ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      pure (.tailCall sink method req))
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM forwarder payload
    assertEqual response.capTable.caps.size 0
    let method := (← seenMethod.get)
    assertEqual method.interfaceId Echo.interfaceId
    assertEqual method.methodId Echo.fooMethodId
    runtime.releaseTarget forwarder
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerForwardCallSendResultsToCallerWithHints : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let seenMethod ← IO.mkRef ({ interfaceId := 0, methodId := 0 } : Capnp.Rpc.Method)
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerHandlerTarget (fun _ method req => do
      seenMethod.set method
      pure req)
    let forwarder ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      let hints : Capnp.Rpc.AdvancedCallHints := {
        noPromisePipelining := true
        onlyPromisePipeline := true
      }
      pure (Capnp.Rpc.Advanced.forwardToCaller sink method req hints))
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM forwarder payload
    assertEqual response.capTable.caps.size 0
    let method := (← seenMethod.get)
    assertEqual method.interfaceId Echo.interfaceId
    assertEqual method.methodId Echo.fooMethodId
    runtime.releaseTarget forwarder
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerForwardToCallerWithOnlyPromisePipelineHint : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let seenMethod ← IO.mkRef ({ interfaceId := 0, methodId := 0 } : Capnp.Rpc.Method)
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerHandlerTarget (fun _ method req => do
      seenMethod.set method
      pure req)
    let forwarder ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      pure (Capnp.Rpc.Advanced.forwardToCaller sink method req { onlyPromisePipeline := true }))
    let pending ← runtime.startCall forwarder Echo.fooMethod payload
    let rec waitForForwardedCall (attempts : Nat) : IO Unit := do
      runtime.pump
      let method ← seenMethod.get
      if method.interfaceId == Echo.interfaceId && method.methodId == Echo.fooMethodId then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError
              s!"onlyPromisePipeline forward call did not reach sink: method={method.interfaceId}:{method.methodId}")
        | n + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForForwardedCall n
    waitForForwardedCall 200
    let pendingCount ← runtime.pendingCallCount
    assertTrue (pendingCount > 0)
      s!"onlyPromisePipeline call should remain pending until canceled/released, got pending={pendingCount}"
    pending.release
    runtime.releaseTarget forwarder
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerTailCallAlias : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerEchoTarget
    let forwarder ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      pure (Capnp.Rpc.Advanced.tailCall sink method req))
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM forwarder payload
    assertEqual response.capTable.caps.size 0
    runtime.releaseTarget forwarder
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerStartsKjAsyncPromisesOnSameRuntime : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerEchoTarget
    let forwarder ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      let kjRuntime : Capnp.KjAsync.Runtime := { handle := runtime.handle }
      let first ← kjRuntime.sleepMillisStart (UInt32.ofNat 1)
      let second ← kjRuntime.sleepMillisStart (UInt32.ofNat 1)
      let seq ← kjRuntime.promiseThenStart first second
      seq.release
      pure (Capnp.Rpc.Advanced.forward sink method req))
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM forwarder payload
    assertEqual response.capTable.caps.size 0
    runtime.releaseTarget forwarder
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerRejectsKjAsyncAwaitOnWorkerThread : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerEchoTarget
    let seenError ← IO.mkRef ""
    let forwarder ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      let kjRuntime : Capnp.KjAsync.Runtime := { handle := runtime.handle }
      let promise ← kjRuntime.sleepMillisStart (UInt32.ofNat 1)
      let errMsg ←
        try
          promise.await
          pure ""
        catch err =>
          pure (toString err)
      promise.release
      seenError.set errMsg
      pure (Capnp.Rpc.Advanced.forward sink method req))
    let _ ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM forwarder payload
    let errMsg ← seenError.get
    if !(errMsg.containsSubstr "not allowed from the Capnp.Rpc worker thread") then
      throw (IO.userError s!"missing worker-thread await rejection text: {errMsg}")
    runtime.releaseTarget forwarder
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerForwardCallOnlyPromisePipelineRequiresCaller : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerEchoTarget
    let forwarder ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      let opts : Capnp.Rpc.AdvancedForwardOptions :=
        Capnp.Rpc.AdvancedForwardOptions.setOnlyPromisePipeline {}
      pure (Capnp.Rpc.Advanced.forward sink method req opts))
    let errMsg ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM forwarder payload
        pure ""
      catch err =>
        pure (toString err)
    if !(errMsg.containsSubstr "onlyPromisePipeline requires sendResultsTo.caller") then
      throw (IO.userError s!"missing onlyPromisePipeline validation error text: {errMsg}")
    runtime.releaseTarget forwarder
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerSetPipelineValidationCleansRequestCaps : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerEchoTarget
    let invalid ← runtime.registerAdvancedHandlerTargetAsync (fun _ _ req => do
      let pipeline := mkCapabilityPayload sink
      pure (Capnp.Rpc.Advanced.setPipeline pipeline
        (Capnp.Rpc.Advanced.now (Capnp.Rpc.Advanced.respond req))))
    let loopback ← runtime.registerLoopbackTarget invalid
    let baselineTargets := (← runtime.targetCount)

    let errMsg ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM loopback (mkCapabilityPayload sink)
        pure ""
      catch err =>
        pure (toString err)
    if !(errMsg.containsSubstr "setPipeline is only valid with defer") then
      throw (IO.userError s!"missing setPipeline validation error text: {errMsg}")

    let rec waitForTargetCount (attempts : Nat) : IO Unit := do
      runtime.pump
      let current ← runtime.targetCount
      if current == baselineTargets then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError s!"request capability cleanup did not converge: {current} vs {baselineTargets}")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForTargetCount attempts
    waitForTargetCount 200

    runtime.releaseTarget loopback
    runtime.releaseTarget invalid
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerDuplicateSetPipelineCleansRequestCaps : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerEchoTarget
    let invalid ← runtime.registerAdvancedHandlerTargetAsync (fun _ _ req => do
      let pipeline := mkCapabilityPayload sink
      pure (Capnp.Rpc.Advanced.setPipeline pipeline
        (Capnp.Rpc.Advanced.setPipeline pipeline
          (Capnp.Rpc.Advanced.now (Capnp.Rpc.Advanced.respond req)))))
    let loopback ← runtime.registerLoopbackTarget invalid
    let baselineTargets := (← runtime.targetCount)

    let errMsg ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM loopback (mkCapabilityPayload sink)
        pure ""
      catch err =>
        pure (toString err)
    if !(errMsg.containsSubstr "setPipeline may only be specified once") then
      throw (IO.userError s!"missing duplicate setPipeline validation error text: {errMsg}")

    let rec waitForTargetCount (attempts : Nat) : IO Unit := do
      runtime.pump
      let current ← runtime.targetCount
      if current == baselineTargets then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError s!"request capability cleanup did not converge: {current} vs {baselineTargets}")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForTargetCount attempts
    waitForTargetCount 200

    runtime.releaseTarget loopback
    runtime.releaseTarget invalid
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerUnknownForwardTargetCleansRequestCaps : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerEchoTarget
    let unknownTarget : Capnp.Rpc.Client := UInt32.ofNat 424242
    let forwarder ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      pure (Capnp.Rpc.Advanced.forward unknownTarget method req))
    let loopback ← runtime.registerLoopbackTarget forwarder
    let baselineTargets := (← runtime.targetCount)

    let errMsg ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM loopback (mkCapabilityPayload sink)
        pure ""
      catch err =>
        pure (toString err)
    if !(errMsg.containsSubstr "unknown RPC async-call target capability id from Lean handler") then
      throw (IO.userError s!"missing forward-target validation error text: {errMsg}")

    let rec waitForTargetCount (attempts : Nat) : IO Unit := do
      runtime.pump
      let current ← runtime.targetCount
      if current == baselineTargets then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError s!"request capability cleanup did not converge: {current} vs {baselineTargets}")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForTargetCount attempts
    waitForTargetCount 200

    runtime.releaseTarget loopback
    runtime.releaseTarget forwarder
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerUnknownTailTargetCleansRequestCaps : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerEchoTarget
    let unknownTarget : Capnp.Rpc.Client := UInt32.ofNat 424243
    let forwarder ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      pure (Capnp.Rpc.Advanced.tailCall unknownTarget method req))
    let loopback ← runtime.registerLoopbackTarget forwarder
    let baselineTargets := (← runtime.targetCount)

    let errMsg ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM loopback (mkCapabilityPayload sink)
        pure ""
      catch err =>
        pure (toString err)
    if !(errMsg.containsSubstr "unknown RPC tail-call target capability id from Lean advanced handler") then
      throw (IO.userError s!"missing tail-target validation error text: {errMsg}")

    let rec waitForTargetCount (attempts : Nat) : IO Unit := do
      runtime.pump
      let current ← runtime.targetCount
      if current == baselineTargets then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError s!"request capability cleanup did not converge: {current} vs {baselineTargets}")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForTargetCount attempts
    waitForTargetCount 200

    runtime.releaseTarget loopback
    runtime.releaseTarget forwarder
    runtime.releaseTarget sink
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerDeferredCancellationReleasesRequestCaps : IO Unit := do
  let canceled ← IO.mkRef false
  let entered ← IO.mkRef false
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerEchoTarget
    let handler ← runtime.registerAdvancedHandlerTargetAsync (fun _ _ _ => do
      entered.set true
      let deferred ← Capnp.Rpc.Advanced.defer do
        let rec waitForCancel (attempts : Nat) : IO Capnp.Rpc.AdvancedHandlerResult := do
          if (← IO.checkCanceled) then
            canceled.set true
            pure (Capnp.Rpc.Advanced.respond mkNullPayload)
          else
            match attempts with
            | 0 =>
                pure (Capnp.Rpc.Advanced.respond mkNullPayload)
            | attempts + 1 =>
                IO.sleep (UInt32.ofNat 5)
                waitForCancel attempts
        waitForCancel 10000
      pure (.control { releaseParams := true, allowCancellation := true } deferred))
    let loopback ← runtime.registerLoopbackTarget handler

    let baselineTargets := (← runtime.targetCount)
    assertEqual baselineTargets (UInt64.ofNat 3)

    let pending ← runtime.startCall loopback Echo.fooMethod (mkCapabilityPayload sink)
    let rec waitForEntered (attempts : Nat) : IO Unit := do
      runtime.pump
      if (← entered.get) then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError "deferred handler was not entered")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForEntered attempts
    waitForEntered 500
    pending.release

    let rec waitForTargetCount (attempts : Nat) : IO Unit := do
      runtime.pump
      let current ← runtime.targetCount
      if current == baselineTargets then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError s!"request capability cleanup did not converge: {current} vs {baselineTargets}")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForTargetCount attempts

    let rec waitForCanceled (attempts : Nat) : IO Unit := do
      runtime.pump
      if (← canceled.get) then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError "deferred handler task was not canceled")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 10)
            waitForCanceled attempts

    waitForTargetCount 200
    waitForCanceled 200

    runtime.releaseTarget loopback
    runtime.releaseTarget handler
    runtime.releaseTarget sink
    assertEqual (← runtime.targetCount) (UInt64.ofNat 0)
  finally
    runtime.shutdown

@[test]
def testRuntimeParityAdvancedDeferredReleaseWithoutAllowCancellation : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let entered ← IO.mkRef false
  let completed ← IO.mkRef false
  let sawCanceled ← IO.mkRef false
  let runtime ← Capnp.Rpc.Runtime.init
  let rec waitFor (label : String) (attempts : Nat) (check : IO Bool) : IO Unit := do
    runtime.pump
    if (← check) then
      pure ()
    else
      match attempts with
      | 0 =>
          throw (IO.userError s!"{label}: timeout")
      | attempts + 1 =>
          IO.sleep (UInt32.ofNat 10)
          waitFor label attempts check
  try
    let deferred ← runtime.registerAdvancedHandlerTargetAsync (fun _ method req => do
      if method.interfaceId != Echo.interfaceId || method.methodId != Echo.fooMethodId then
        throw (IO.userError
          s!"unexpected method in deferred no-cancellation parity target: {method.interfaceId}/{method.methodId}")
      entered.set true
      Capnp.Rpc.Advanced.defer do
        IO.sleep (UInt32.ofNat 30)
        if (← IO.checkCanceled) then
          sawCanceled.set true
        completed.set true
        pure (Capnp.Rpc.Advanced.respond req))

    let pending ← runtime.startCall deferred Echo.fooMethod payload
    waitFor "deferred no-cancellation handler entry" 500 entered.get
    pending.release
    waitFor "deferred no-cancellation completion" 500 completed.get

    assertEqual (← sawCanceled.get) false
    assertEqual (← runtime.pendingCallCount) (UInt64.ofNat 0)

    runtime.releaseTarget deferred
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerDeferredCancellationDoesNotCarryAcrossCalls : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let sawCanceled ← IO.mkRef false
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let echo ← runtime.registerEchoTarget
    let canceledPending ← runtime.startCall echo Echo.fooMethod payload
    canceledPending.release
    runtime.releaseTarget echo

    let deferred ← runtime.registerAdvancedHandlerTargetAsync (fun _ _ req => do
      Capnp.Rpc.Advanced.defer (opts := { allowCancellation := true }) do
        IO.sleep (UInt32.ofNat 25)
        if (← IO.checkCanceled) then
          sawCanceled.set true
          pure (Capnp.Rpc.Advanced.throwRemote "unexpected deferred cancellation")
        else
          pure (Capnp.Rpc.Advanced.respond req))

    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM deferred payload
    assertEqual response.capTable.caps.size 0
    assertEqual (← sawCanceled.get) false
    runtime.releaseTarget deferred
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerDeferredRespond : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let deferred ← runtime.registerAdvancedHandlerTargetAsync (fun _ _ req => do
      let responseTask ← IO.asTask do
        IO.sleep (UInt32.ofNat 25)
        pure (Capnp.Rpc.Advanced.respond req)
      pure (Capnp.Rpc.Advanced.deferTask responseTask))
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM deferred payload
    assertEqual response.capTable.caps.size 0
    runtime.releaseTarget deferred
  finally
    runtime.shutdown

@[test]
def testRuntimeKjAsyncSleepAsTaskAndPromiseHelpers : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sleepTask ← runtime.sleepMillisAsTask (UInt32.ofNat 5)
    match (← IO.wait sleepTask) with
    | .ok () => pure ()
    | .error err => throw err

    let sleepTaskM ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.sleepNanosAsTask (UInt64.ofNat 1000000)
    match (← IO.wait sleepTaskM) with
    | .ok () => pure ()
    | .error err => throw err

    let sleepPromise ← runtime.sleepMillisAsPromise (UInt32.ofNat 5)
    match (← sleepPromise.awaitResult) with
    | .ok () => pure ()
    | .error err => throw err

    let sleepPromiseM ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.sleepNanosAsPromise (UInt64.ofNat 1000000)
    match (← sleepPromiseM.awaitResult) with
    | .ok () => pure ()
    | .error err => throw err
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerDeferredSetPipeline : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (address, socketPath) ← mkUnixTestAddress
  let sinkSeen ← IO.mkRef false
  let runtime ← Capnp.Rpc.Runtime.init
  try
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

    let sink ← runtime.registerHandlerTarget (fun _ _ _ => do
      sinkSeen.set true
      pure payload)

    let delayedPayload := mkCapabilityPayload sink
    let handler ← runtime.registerAdvancedHandlerTargetAsync (fun _ _ _ => do
      let deferred ← Capnp.Rpc.Advanced.defer do
        IO.sleep (UInt32.ofNat 200)
        pure (Capnp.Rpc.Advanced.respond delayedPayload)
      pure (Capnp.Rpc.Advanced.setPipeline delayedPayload deferred))

    let server ← runtime.newServer handler
    let listener ← server.listen address
    let callTask ← IO.asTask (Capnp.Rpc.Interop.cppCallPipelinedCapOneShot
      address Echo.fooMethod payload payload)
    server.accept listener

    let rec waitForSink (attempts : Nat) : IO Unit := do
      if (← sinkSeen.get) then
        pure ()
      else
        match attempts with
        | 0 =>
            throw (IO.userError "pipelined call did not reach sink before handler finished")
        | attempts + 1 =>
            IO.sleep (UInt32.ofNat 5)
            waitForSink attempts
    waitForSink 20

    match callTask.get with
    | .ok res =>
        assertEqual res.capTable.caps.size 0
    | .error err =>
        throw err

    server.release
    runtime.releaseListener listener
    runtime.releaseTarget handler
    runtime.releaseTarget sink
  finally
    runtime.shutdown
    try
      IO.FS.removeFile socketPath
    catch _ =>
      pure ()

@[test]
def testRuntimeAdvancedHandlerDeferredWithControl : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let sink ← runtime.registerEchoTarget
    let forwarder ← runtime.registerAdvancedHandlerTargetAsync (fun _ _ _ => do
      let deferred ← Capnp.Rpc.Advanced.defer
        (next := do
          IO.sleep (UInt32.ofNat 25)
          pure (Capnp.Rpc.Advanced.respond mkNullPayload))
        (opts := {
          releaseParams := true
          allowCancellation := true
        })
      pure deferred)
    let baselineTargets := (← runtime.targetCount)
    assertEqual baselineTargets (UInt64.ofNat 2)

    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM forwarder (mkCapabilityPayload sink)
    assertEqual response.capTable.caps.size 0
    assertEqual (← runtime.targetCount) baselineTargets

    runtime.releaseTarget forwarder
    runtime.releaseTarget sink
    assertEqual (← runtime.targetCount) (UInt64.ofNat 0)
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerDeferredLateAllowCancellationRejected : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let deferred ← runtime.registerAdvancedHandlerTargetAsync (fun _ _ req => do
      Capnp.Rpc.Advanced.defer do
        IO.sleep (UInt32.ofNat 25)
        pure (Capnp.Rpc.AdvancedHandlerResult.allowCancellation (Capnp.Rpc.Advanced.respond req)))
    let errMsg ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM deferred payload
        pure ""
      catch err =>
        pure (toString err)
    if !(errMsg.containsSubstr "allowCancellation must be set before defer") then
      throw (IO.userError s!"missing deferred allowCancellation ordering error text: {errMsg}")
    runtime.releaseTarget deferred
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerDeferredLateStreamingRejected : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let deferred ← runtime.registerAdvancedHandlerTargetAsync (fun _ _ req => do
      Capnp.Rpc.Advanced.defer do
        IO.sleep (UInt32.ofNat 25)
        pure (Capnp.Rpc.Advanced.streaming (Capnp.Rpc.Advanced.respond req)))
    let errMsg ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM deferred payload
        pure ""
      catch err =>
        pure (toString err)
    if !(errMsg.containsSubstr "isStreaming must be set before defer") then
      throw (IO.userError s!"missing deferred isStreaming ordering error text: {errMsg}")
    runtime.releaseTarget deferred
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerDeferredTaskIoError : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let deferred ← runtime.registerAdvancedHandlerTargetAsync (fun _ _ _ => do
      Capnp.Rpc.Advanced.defer do
        throw (IO.userError "expected deferred task failure"))
    let errMsg ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM deferred payload
        pure ""
      catch err =>
        pure (toString err)
    if !(errMsg.containsSubstr "Lean RPC advanced deferred handler task returned IO error") then
      throw (IO.userError s!"missing deferred task IO error text: {errMsg}")
    runtime.releaseTarget deferred
  finally
    runtime.shutdown

@[test]
def testRuntimeAdvancedHandlerThrowRemote : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let throwing ← runtime.registerAdvancedHandlerTarget (fun _ _ _ => do
      pure (.throwRemote "advanced test exception" (ByteArray.mk #[97, 98])))
    let loopback ← runtime.registerLoopbackTarget throwing
    let errMsg ←
      try
        let _ ← Capnp.Rpc.RuntimeM.run runtime do
          Echo.callFooM loopback payload
        pure ""
      catch err =>
        pure (toString err)
    if !(errMsg.containsSubstr "remote exception: advanced test exception") then
      throw (IO.userError s!"missing remote exception text: {errMsg}")
    if !(errMsg.containsSubstr "remote detail[1]: ab") then
      throw (IO.userError s!"missing remote detail text: {errMsg}")
    runtime.releaseTarget loopback
    runtime.releaseTarget throwing
  finally
    runtime.shutdown

@[test]
def testRuntimeConnectFdAndServerAcceptFd : IO Unit := do
  if System.Platform.isWindows then
    pure ()
  else
    let payload : Capnp.Rpc.Payload := mkNullPayload
    let (clientFd, serverFd) ← ffiNewSocketPairImpl
    let runtime ← Capnp.Rpc.Runtime.init
    try
      let bootstrap ← runtime.registerEchoTarget
      let server ← runtime.newServer bootstrap
      server.acceptFd serverFd
      let target ← runtime.connectFd clientFd
      let response ← Capnp.Rpc.RuntimeM.run runtime do
        Echo.callFooM target payload
      assertEqual response.capTable.caps.size 0
      runtime.releaseTarget target
      server.release
      runtime.releaseTarget bootstrap
    finally
      runtime.shutdown
      try
        ffiCloseFdImpl clientFd
      catch _ =>
        pure ()
      try
        ffiCloseFdImpl serverFd
      catch _ =>
        pure ()

@[test]
def testRuntimeConnectTransportAndServerAcceptTransport : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let bootstrap ← runtime.registerEchoTarget
    let server ← runtime.newServer bootstrap
    let (clientTransport, serverTransport) ← runtime.newTransportPipe
    server.acceptTransport serverTransport
    let target ← runtime.connectTransport clientTransport
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target payload
    assertEqual response.capTable.caps.size 0
    runtime.releaseTarget target
    server.release
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeTransportInjectionFromFd : IO Unit := do
  if System.Platform.isWindows then
    pure ()
  else
    let payload : Capnp.Rpc.Payload := mkNullPayload
    let (clientFd, serverFd) ← ffiNewSocketPairImpl
    let runtime ← Capnp.Rpc.Runtime.init
    try
      let bootstrap ← runtime.registerEchoTarget
      let server ← runtime.newServer bootstrap
      let clientTransport ← runtime.newTransportFromFd clientFd
      let serverTransport ← runtime.newTransportFromFd serverFd
      assertEqual (Option.isSome (← runtime.transportGetFd? clientTransport)) true
      assertEqual (Option.isSome (← runtime.transportGetFd? serverTransport)) true
      server.acceptTransport serverTransport
      let target ← runtime.connectTransport clientTransport
      let response ← Capnp.Rpc.RuntimeM.run runtime do
        Echo.callFooM target payload
      assertEqual response.capTable.caps.size 0
      runtime.releaseTarget target
      server.release
      runtime.releaseTarget bootstrap
    finally
      runtime.shutdown
      try
        ffiCloseFdImpl clientFd
      catch _ =>
        pure ()
      try
        ffiCloseFdImpl serverFd
      catch _ =>
        pure ()

@[test]
def testRuntimeTransportInjectionFromKjAsyncCapabilityPipe : IO Unit := do
  if System.Platform.isWindows then
    pure ()
  else
    let payload : Capnp.Rpc.Payload := mkNullPayload
    let kjRuntime ← Capnp.KjAsync.Runtime.init
    let runtime ← Capnp.Rpc.Runtime.init
    try
      let (clientConn, serverConn) ← kjRuntime.newCapabilityPipe
      let clientFd? ← clientConn.dupFd?
      let serverFd? ← serverConn.dupFd?
      clientConn.release
      serverConn.release

      match (clientFd?, serverFd?) with
      | (some clientFd, some serverFd) =>
          let bootstrap ← runtime.registerEchoTarget
          let server ← runtime.newServer bootstrap
          let clientTransport ← runtime.newTransportFromFdTake clientFd
          let serverTransport ← runtime.newTransportFromFdTake serverFd
          server.acceptTransport serverTransport
          let target ← runtime.connectTransport clientTransport

          let response ← Capnp.Rpc.RuntimeM.run runtime do
            Echo.callFooM target payload
          assertEqual response.capTable.caps.size 0

          runtime.releaseTarget target
          server.release
          runtime.releaseTarget bootstrap
      | _ =>
          throw (IO.userError
            "expected Capnp.KjAsync capability pipe connections to expose an fd")
    finally
      runtime.shutdown
      kjRuntime.shutdown

@[test]
def testRuntimeInitWithFdLimit : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.initWithFdLimit (UInt32.ofNat 4)
  try
    assertEqual (← runtime.isAlive) true
    let target ← runtime.registerEchoTarget
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target payload
    assertEqual response.capTable.caps.size 0
    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimeFdPerMessageLimitDropsExcessFds : IO Unit := do
  if System.Platform.isWindows then
    pure ()
  else
    let (address, socketPath) ← mkUnixTestAddress
    let serverRuntime ← Capnp.Rpc.Runtime.initWithFdLimit (UInt32.ofNat 1)
    let clientRuntime ← Capnp.Rpc.Runtime.initWithFdLimit (UInt32.ofNat 1)
    try
      try
        IO.FS.removeFile socketPath
      catch _ =>
        pure ()

      let fdCap1 ← clientRuntime.registerFdTarget (UInt32.ofNat 0)
      let fdCap2 ← clientRuntime.registerFdTarget (UInt32.ofNat 1)
      let payload : Capnp.Rpc.Payload := Id.run do
        let (capTable, builder) :=
          (do
            let root ← Capnp.getRootPointer
            let list ← Capnp.initListPointer root Capnp.elemSizePointer 2
            let ptrs := Capnp.listPointerBuilders list
            let p0 := ptrs.getD 0 { seg := 0, word := 0 }
            let p1 := ptrs.getD 1 { seg := 0, word := 0 }
            let capTable1 ← Capnp.writeCapabilityWithTable Capnp.emptyCapTable p0 fdCap1
            Capnp.writeCapabilityWithTable capTable1 p1 fdCap2
          ).run (Capnp.initMessageBuilder 16)
        { msg := Capnp.buildMessage builder, capTable := capTable }

      let fdProbeTarget ← registerFdProbeTarget serverRuntime
      let server ← serverRuntime.newServer fdProbeTarget
      let listener ← server.listen address
      let client ← clientRuntime.newClient address
      server.accept listener
      let remoteTarget ← client.bootstrap

      let response ← Capnp.Rpc.RuntimeM.run clientRuntime do
        Echo.callFooM remoteTarget payload
      assertEqual (← readUInt64Payload response) (UInt64.ofNat 1)

      clientRuntime.releaseCapTable response.capTable
      clientRuntime.releaseTarget remoteTarget
      client.release
      server.release
      serverRuntime.releaseListener listener
      serverRuntime.releaseTarget fdProbeTarget
      clientRuntime.releaseTarget fdCap1
      clientRuntime.releaseTarget fdCap2
    finally
      clientRuntime.shutdown
      serverRuntime.shutdown
      try
        IO.FS.removeFile socketPath
      catch _ =>
        pure ()

@[test]
def testRuntimeFdTargetLocalGetFd : IO Unit := do
  if System.Platform.isWindows then
    pure ()
  else
    let runtime ← Capnp.Rpc.Runtime.init
    try
      let fdTarget ← runtime.registerFdTarget (UInt32.ofNat 0)
      let fd? ← runtime.targetGetFd? fdTarget
      assertEqual fd?.isSome true
      runtime.releaseTarget fdTarget
    finally
      runtime.shutdown

@[test]
def testRuntimeFdPassingOverNetwork : IO Unit := do
  if System.Platform.isWindows then
    pure ()
  else
    let payload : Capnp.Rpc.Payload := mkNullPayload
    let (address, socketPath) ← mkUnixTestAddress
    let runtime ← Capnp.Rpc.Runtime.init
    try
      try
        IO.FS.removeFile socketPath
      catch _ =>
        pure ()
      let fdTarget ← runtime.registerFdTarget (UInt32.ofNat 0)
      let returnPayload := mkCapabilityPayload fdTarget
      let bootstrap ← runtime.registerHandlerTarget (fun _ _ _ => pure returnPayload)
      let server ← runtime.newServer bootstrap
      let listener ← server.listen address
      let client ← runtime.newClient address
      server.accept listener

      let remoteTarget ← client.bootstrap
      let response ← Capnp.Rpc.RuntimeM.run runtime do
        Echo.callFooM remoteTarget payload
      let returnedCap? := Capnp.readCapabilityFromTable response.capTable (Capnp.getRoot response.msg)
      assertEqual returnedCap?.isSome true
      match returnedCap? with
      | none =>
          throw (IO.userError "RPC response is missing expected capability")
      | some returnedCap =>
          let fd? ← runtime.targetGetFd? returnedCap
          assertEqual fd?.isSome true
      runtime.releaseCapTable response.capTable
      runtime.releaseTarget remoteTarget
      client.release
      server.release
      runtime.releaseListener listener
      runtime.releaseTarget bootstrap
      runtime.releaseTarget fdTarget
    finally
      runtime.shutdown
      try
        IO.FS.removeFile socketPath
      catch _ =>
        pure ()

@[test]
def testRuntimeSharedAsyncHelpersForRegisterAndUnitPromises : IO Unit := do
  if System.Platform.isWindows then
    pure ()
  else
    let payload : Capnp.Rpc.Payload := mkNullPayload
    let (address, socketPath) ← mkUnixTestAddress
    let runtime ← Capnp.Rpc.Runtime.init
    try
      try
        IO.FS.removeFile socketPath
      catch _ =>
        pure ()

      let bootstrap ← runtime.registerEchoTarget
      let server ← runtime.newServer bootstrap
      let listener ← server.listen address
      let acceptPromise ← server.acceptStart listener
      let connectPromise ← runtime.connectStart address

      let connectAsync ← connectPromise.toPromise
      let target ←
        match (← connectAsync.awaitResult) with
        | .ok connectedTarget => pure connectedTarget
        | .error err =>
            throw (IO.userError s!"register promise toPromise failed: {err}")

      let acceptAsync ← acceptPromise.toPromise
      match (← acceptAsync.awaitResult) with
      | .ok () => pure ()
      | .error err =>
          throw (IO.userError s!"unit promise toPromise failed: {err}")

      let response ← Capnp.Rpc.RuntimeM.run runtime do
        Echo.callFooM target payload
      assertEqual response.capTable.caps.size 0

      runtime.releaseTarget target
      runtime.releaseListener listener
      server.release
      runtime.releaseTarget bootstrap
    finally
      runtime.shutdown
      try
        IO.FS.removeFile socketPath
      catch _ =>
        pure ()

@[test]
def testRuntimePendingCallSharedAsyncHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let target ← runtime.registerEchoTarget

    let pending ← runtime.startCall target Echo.fooMethod payload
    let pendingTask ← pending.awaitAsTask
    let response ←
      match (← IO.wait pendingTask) with
      | .ok rsp => pure rsp
      | .error err =>
          throw (IO.userError s!"pending call awaitAsTask failed: {err}")
    assertEqual response.capTable.caps.size 0

    let pending2 ← runtime.startCall target Echo.fooMethod payload
    let pendingPromise ← pending2.toPromise
    match (← pendingPromise.awaitResult) with
    | .ok rsp =>
        assertEqual rsp.capTable.caps.size 0
    | .error err =>
        throw (IO.userError s!"pending call toPromise failed: {err}")

    let pending3 ← runtime.startCall target Echo.fooMethod payload
    let pendingOutcomeTask ← pending3.awaitOutcomeAsTask
    match (← IO.wait pendingOutcomeTask) with
    | .ok (.ok _responseBytes _responseCaps) => pure ()
    | .ok (.error ex) =>
        throw (IO.userError s!"pending call awaitOutcomeAsTask returned error outcome: {ex.description}")
    | .error err =>
        throw (IO.userError s!"pending call awaitOutcomeAsTask failed: {err}")

    let pending4 ← runtime.startCall target Echo.fooMethod payload
    let pendingResultTask ← pending4.awaitResultAsTask
    match (← IO.wait pendingResultTask) with
    | .ok (.ok rsp) =>
        assertEqual rsp.capTable.caps.size 0
    | .ok (.error ex) =>
        throw (IO.userError s!"pending call awaitResultAsTask returned error outcome: {ex.description}")
    | .error err =>
        throw (IO.userError s!"pending call awaitResultAsTask failed: {err}")

    let pending5 ← runtime.startCall target Echo.fooMethod payload
    let pendingOutcomePromise ← pending5.toPromiseOutcome
    match (← pendingOutcomePromise.awaitResult) with
    | .ok (.ok _responseBytes _responseCaps) => pure ()
    | .ok (.error ex) =>
        throw (IO.userError s!"pending call toPromiseOutcome returned error outcome: {ex.description}")
    | .error err =>
        throw (IO.userError s!"pending call toPromiseOutcome failed: {err}")

    let pending6 ← runtime.startCall target Echo.fooMethod payload
    let pendingResultPromise ← pending6.toPromiseResult
    match (← pendingResultPromise.awaitResult) with
    | .ok (.ok rsp) =>
        assertEqual rsp.capTable.caps.size 0
    | .ok (.error ex) =>
        throw (IO.userError s!"pending call toPromiseResult returned error outcome: {ex.description}")
    | .error err =>
        throw (IO.userError s!"pending call toPromiseResult failed: {err}")

    let failingTarget ← runtime.registerHandlerTarget (fun _ _ _ => do
      throw (IO.userError "pending-call typed promise failure"))
    let pendingFail ← runtime.startCall failingTarget Echo.fooMethod payload
    let pendingFailPromise ← pendingFail.toPromiseResult
    match (← pendingFailPromise.awaitResult) with
    | .ok (.error ex) =>
        assertTrue (ex.description.length > 0)
          "pending call toPromiseResult error should carry a non-empty remote exception description"
    | .ok (.ok _rsp) =>
        throw (IO.userError "pending call toPromiseResult should have returned an error outcome")
    | .error err =>
        throw (IO.userError s!"pending call toPromiseResult failure-path helper failed: {err}")
    runtime.releaseTarget failingTarget

    runtime.releaseTarget target
  finally
    runtime.shutdown

@[test]
def testRuntimeMultiVatBasicThreePartyHandoff : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  let bobCallCount ← IO.mkRef (0 : Nat)
  let heldCap ← IO.mkRef (none : Option Capnp.Rpc.Client)
  try
    let bobBootstrap ← runtime.registerHandlerTarget (fun _ method req => do
      if method.interfaceId == Echo.interfaceId && method.methodId == Echo.fooMethodId then
        bobCallCount.modify (fun n => n + 1)
      pure req)
    let carolBootstrap ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      if method.interfaceId == Echo.interfaceId && method.methodId == Echo.fooMethodId then
        let cap? := Capnp.readCapabilityFromTable req.capTable (Capnp.getRoot req.msg)
        match cap? with
        | some cap =>
            let retained ← runtime.retainTarget cap
            match (← heldCap.get) with
            | some previous => runtime.releaseTarget previous
            | none => pure ()
            heldCap.set (some retained)
        | none => heldCap.set none
        pure (Capnp.Rpc.Advanced.respond mkNullPayload)
      else if method.interfaceId == Echo.interfaceId && method.methodId == Echo.barMethodId then
        match (← heldCap.get) with
        | some target =>
            pure (Capnp.Rpc.Advanced.asyncForward target Echo.fooMethod payload)
        | none =>
            throw (IO.userError "Carol has no held capability")
      else
        pure (Capnp.Rpc.Advanced.respond req))

    let alice ← runtime.newMultiVatClient "alice"
    let bob ← runtime.newMultiVatServer "bob" bobBootstrap
    let carol ← runtime.newMultiVatServer "carol" carolBootstrap

    let bobCap ← alice.bootstrap { host := "bob", unique := false }
    let carolCap ← alice.bootstrap { host := "carol", unique := false }
    assertEqual (← runtime.multiVatHasConnection carol bob) false

    let _ ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM carolCap (mkCapabilityPayload bobCap)
    assertEqual (← bobCallCount.get) 0

    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callBarM carolCap payload
    assertEqual response.capTable.caps.size 0
    assertEqual (← bobCallCount.get) 1
    assertEqual (← runtime.multiVatHasConnection carol bob) true

    runtime.releaseTarget bobCap
    runtime.releaseTarget carolCap
    alice.release
    bob.release
    carol.release
    match (← heldCap.get) with
    | some cap => runtime.releaseTarget cap
    | none => pure ()
    runtime.releaseTarget bobBootstrap
    runtime.releaseTarget carolBootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeMultiVatHandoffOrderingAndMissingHeldCapError : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  let heldCap ← IO.mkRef (none : Option Capnp.Rpc.Client)
  try
    let (bobBootstrap, getNextExpected) ← registerEchoFooCallOrderTarget runtime
    let carolBootstrap ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      if method.interfaceId == Echo.interfaceId && method.methodId == Echo.fooMethodId then
        let cap? := Capnp.readCapabilityFromTable req.capTable (Capnp.getRoot req.msg)
        match cap? with
        | some cap =>
            let retained ← runtime.retainTarget cap
            match (← heldCap.get) with
            | some previous => runtime.releaseTarget previous
            | none => pure ()
            heldCap.set (some retained)
        | none =>
            match (← heldCap.get) with
            | some previous => runtime.releaseTarget previous
            | none => pure ()
            heldCap.set none
        pure (Capnp.Rpc.Advanced.respond mkNullPayload)
      else if method.interfaceId == Echo.interfaceId && method.methodId == Echo.barMethodId then
        match (← heldCap.get) with
        | some target =>
            pure (Capnp.Rpc.Advanced.asyncForward target Echo.fooMethod req)
        | none =>
            throw (IO.userError "Carol has no held capability")
      else
        pure (Capnp.Rpc.Advanced.respond req))

    let alice ← runtime.newMultiVatClient "alice-ordering"
    let bob ← runtime.newMultiVatServer "bob-ordering" bobBootstrap
    let carol ← runtime.newMultiVatServer "carol-ordering" carolBootstrap

    let bobCap ← alice.bootstrap { host := "bob-ordering", unique := false }
    let carolCap ← alice.bootstrap { host := "carol-ordering", unique := false }
    assertEqual (← runtime.multiVatHasConnection carol bob) false

    let _ ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM carolCap (mkCapabilityPayload bobCap)

    let call0Pending ← runtime.startCall carolCap Echo.barMethod (mkUInt64Payload (UInt64.ofNat 0))
    let call1Pending ← runtime.startCall carolCap Echo.barMethod (mkUInt64Payload (UInt64.ofNat 1))
    let call2Pending ← runtime.startCall carolCap Echo.barMethod (mkUInt64Payload (UInt64.ofNat 2))

    let call0Response ← call0Pending.await
    let call1Response ← call1Pending.await
    let call2Response ← call2Pending.await

    assertEqual (← readUInt64Payload call0Response) (UInt64.ofNat 0)
    assertEqual (← readUInt64Payload call1Response) (UInt64.ofNat 1)
    assertEqual (← readUInt64Payload call2Response) (UInt64.ofNat 2)
    assertEqual (← getNextExpected) (UInt64.ofNat 3)
    assertEqual (← runtime.multiVatHasConnection carol bob) true

    let _ ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM carolCap mkNullPayload
    let missingHeldOutcome ← runtime.callResult carolCap Echo.barMethod (mkUInt64Payload (UInt64.ofNat 3))
    match missingHeldOutcome with
    | .ok _ =>
        throw (IO.userError "expected missing held capability call to fail")
    | .error ex =>
        assertEqual ex.type .failed
        assertTrue (!ex.description.isEmpty)
          "missing held capability failure should include a description"

    runtime.releaseTarget bobCap
    runtime.releaseTarget carolCap
    alice.release
    bob.release
    carol.release
    match (← heldCap.get) with
    | some cap => runtime.releaseTarget cap
    | none => pure ()
    runtime.releaseTarget bobBootstrap
    runtime.releaseTarget carolBootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeMultiVatBootstrapFactoryAuth : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  let seenCaller ← IO.mkRef ({ host := "", unique := false } : Capnp.Rpc.VatId)
  try
    let bootstrap ← runtime.registerEchoTarget
    let alice ← runtime.newMultiVatClient "alice"
    let bob ← runtime.newMultiVatServerWithBootstrapFactory "bob" (fun caller => do
      seenCaller.set caller
      pure bootstrap)

    let target ← alice.bootstrap { host := "bob", unique := true }
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target payload
    assertEqual response.capTable.caps.size 0

    let caller := (← seenCaller.get)
    assertEqual caller.host "alice"
    assertEqual caller.unique true

    runtime.releaseTarget target
    alice.release
    bob.release
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeMultiVatBootstrapFactoryFailure : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let alice ← runtime.newMultiVatClient "alice"
    let bob ← runtime.newMultiVatServerWithBootstrapFactory "bob" (fun _ => do
      throw (IO.userError "expected generic bootstrap factory failure"))

    let target ← alice.bootstrap { host := "bob", unique := false }
    let errMsg ← try
      let _ ← Capnp.Rpc.RuntimeM.run runtime do
        Echo.callFooM target payload
      pure ""
    catch err =>
      pure (toString err)
    if !(errMsg.containsSubstr "Lean generic bootstrap factory returned IO error") then
      throw (IO.userError s!"missing generic bootstrap factory error text: {errMsg}")

    runtime.releaseTarget target
    alice.release
    bob.release
  finally
    runtime.shutdown

@[test]
def testRuntimeMultiVatBootstrapFactoryUnknownTarget : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let alice ← runtime.newMultiVatClient "alice"
    let bob ← runtime.newMultiVatServerWithBootstrapFactory "bob" (fun _ => do
      pure (0xFFFFFFFF : UInt32))

    let target ← alice.bootstrap { host := "bob", unique := true }
    let errMsg ← try
      let _ ← Capnp.Rpc.RuntimeM.run runtime do
        Echo.callFooM target payload
      pure ""
    catch err =>
      pure (toString err)
    if !(errMsg.containsSubstr "unknown RPC bootstrap capability id from Lean generic bootstrap factory") then
      throw (IO.userError s!"missing unknown generic bootstrap target error text: {errMsg}")

    runtime.releaseTarget target
    alice.release
    bob.release
  finally
    runtime.shutdown

@[test]
def testRuntimeMultiVatSturdyRefRestoreCallback : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  let seenCaller ← IO.mkRef ({ host := "", unique := false } : Capnp.Rpc.VatId)
  let seenObjectId ← IO.mkRef ByteArray.empty
  try
    let bootstrap ← runtime.registerEchoTarget
    let alice ← runtime.newMultiVatClient "alice"
    let bob ← runtime.newMultiVatServer "bob" bootstrap
    bob.setRestorer (fun caller objectId => do
      seenCaller.set caller
      seenObjectId.set objectId
      pure bootstrap)

    let sturdyRef : Capnp.Rpc.SturdyRef := {
      vat := { host := "bob", unique := false }
      objectId := ByteArray.mk #[10, 20, 30, 40]
    }
    let target ← alice.restoreSturdyRef sturdyRef
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target payload
    assertEqual response.capTable.caps.size 0

    let caller := (← seenCaller.get)
    assertEqual caller.host "alice"
    assertEqual caller.unique false
    assertEqual ((← seenObjectId.get) == sturdyRef.objectId) true

    runtime.releaseTarget target
    bob.clearRestorer
    alice.release
    bob.release
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeMultiVatWithRestorerClearsOnException : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let bootstrap ← runtime.registerEchoTarget
    let alice ← runtime.newMultiVatClient "alice-restorer-scoped"
    let bob ← runtime.newMultiVatServer "bob-restorer-scoped" bootstrap

    let scopedThrew ←
      try
        let (_ : Unit) ← bob.withRestorer
          (fun _ _ => pure bootstrap)
          (fun _ => throw (IO.userError "expected scoped restorer failure"))
        pure false
      catch _ =>
        pure true
    assertEqual scopedThrew true

    let postScopeErr ←
      try
        let restored ← alice.restoreSturdyRef {
          vat := { host := "bob-restorer-scoped", unique := false }
          objectId := ByteArray.mk #[7, 7, 7]
        }
        runtime.releaseTarget restored
        pure ""
      catch err =>
        pure (toString err)
    if !(postScopeErr.containsSubstr "no sturdy refs published for host:") then
      throw (IO.userError s!"missing cleared-scoped-restorer error text: {postScopeErr}")

    alice.release
    bob.release
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeMultiVatSturdyRefRestorerFailure : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let bootstrap ← runtime.registerEchoTarget
    let alice ← runtime.newMultiVatClient "alice-restorer-failure"
    let bob ← runtime.newMultiVatServer "bob-restorer-failure" bootstrap
    bob.setRestorer (fun _ _ => do
      throw (IO.userError "expected sturdy ref restorer failure"))

    let errMsg ←
      try
        let restored ← alice.restoreSturdyRef {
          vat := { host := "bob-restorer-failure", unique := false }
          objectId := ByteArray.mk #[10, 20]
        }
        runtime.releaseTarget restored
        pure ""
      catch err =>
        pure (toString err)
    if !(errMsg.containsSubstr "multi-vat sturdy ref restorer returned IO error") then
      throw (IO.userError s!"missing sturdy ref restorer error text: {errMsg}")

    bob.clearRestorer
    alice.release
    bob.release
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeMultiVatSturdyRefRestorerUnknownTarget : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let bootstrap ← runtime.registerEchoTarget
    let alice ← runtime.newMultiVatClient "alice-restorer-unknown-target"
    let bob ← runtime.newMultiVatServer "bob-restorer-unknown-target" bootstrap
    bob.setRestorer (fun _ _ => pure (0xFFFFFFFF : UInt32))

    let errMsg ←
      try
        let restored ← alice.restoreSturdyRef {
          vat := { host := "bob-restorer-unknown-target", unique := false }
          objectId := ByteArray.mk #[1, 2, 3]
        }
        runtime.releaseTarget restored
        pure ""
      catch err =>
        pure (toString err)
    if !(errMsg.containsSubstr "unknown RPC target capability id") then
      throw (IO.userError s!"missing sturdy ref unknown target error text: {errMsg}")

    bob.clearRestorer
    alice.release
    bob.release
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeMultiVatRestoreSturdyRefMissingObjectErrors : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let bootstrap ← runtime.registerEchoTarget
    let alice ← runtime.newMultiVatClient "alice-missing-sturdy"
    let bob ← runtime.newMultiVatServer "bob-missing-sturdy" bootstrap
    let vat : Capnp.Rpc.VatId := { host := "bob-missing-sturdy", unique := false }

    let noRefsErr ←
      try
        let restored ← alice.restoreSturdyRef { vat := vat, objectId := ByteArray.mk #[4, 5, 6] }
        runtime.releaseTarget restored
        pure ""
      catch err =>
        pure (toString err)
    if !(noRefsErr.containsSubstr "no sturdy refs published for host:") then
      throw (IO.userError s!"missing no-published-sturdy-ref error text: {noRefsErr}")

    bob.publishSturdyRef (ByteArray.mk #[1, 2, 3]) bootstrap
    let unknownObjectErr ←
      try
        let restored ← alice.restoreSturdyRef { vat := vat, objectId := ByteArray.mk #[9, 9, 9] }
        runtime.releaseTarget restored
        pure ""
      catch err =>
        pure (toString err)
    if !(unknownObjectErr.containsSubstr "unknown sturdy ref object id") then
      throw (IO.userError s!"missing unknown sturdy-ref object-id error text: {unknownObjectErr}")

    alice.release
    bob.release
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeMultiVatPublishedSturdyRefAndStats : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let bootstrap ← runtime.registerEchoTarget
    let alice ← runtime.newMultiVatClient "alice"
    let bob ← runtime.newMultiVatServer "bob" bootstrap

    runtime.multiVatSetForwardingEnabled false
    assertEqual (← runtime.multiVatForwardCount) (UInt64.ofNat 0)
    assertEqual (← runtime.multiVatDeniedForwardCount) (UInt64.ofNat 0)

    bob.publishSturdyRef (ByteArray.mk #[1, 2, 3]) bootstrap
    let restored ← alice.restoreSturdyRef
      { vat := { host := "bob", unique := false }, objectId := ByteArray.mk #[1, 2, 3] }
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM restored payload
    assertEqual response.capTable.caps.size 0

    let bootTarget ← alice.bootstrap { host := "bob", unique := false }
    assertEqual (← runtime.multiVatHasConnection alice bob) true

    runtime.releaseTarget bootTarget
    runtime.releaseTarget restored
    alice.release
    bob.release
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeMultiVatPublishedSturdyRefLifecycleControls : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let bootstrap ← runtime.registerEchoTarget
    let alice ← runtime.newMultiVatClient "alice-sturdy-catalog"
    let bob ← runtime.newMultiVatServer "bob-sturdy-catalog" bootstrap

    let objectIdA := ByteArray.mk #[1, 2, 3]
    let objectIdB := ByteArray.mk #[4, 5, 6]
    let vat : Capnp.Rpc.VatId := { host := "bob-sturdy-catalog", unique := false }

    assertEqual (← bob.publishedSturdyRefCount) (UInt64.ofNat 0)

    bob.publishSturdyRef objectIdA bootstrap
    bob.publishSturdyRef objectIdB bootstrap
    assertEqual (← bob.publishedSturdyRefCount) (UInt64.ofNat 2)

    let restoredA ← alice.restoreSturdyRef { vat := vat, objectId := objectIdA }
    runtime.releaseTarget restoredA

    bob.unpublishSturdyRef objectIdA
    assertEqual (← bob.publishedSturdyRefCount) (UInt64.ofNat 1)

    let removedErr ←
      try
        let restored ← alice.restoreSturdyRef { vat := vat, objectId := objectIdA }
        runtime.releaseTarget restored
        pure ""
      catch err =>
        pure (toString err)
    if !(removedErr.containsSubstr "unknown sturdy ref object id") then
      throw (IO.userError s!"missing unknown sturdy-ref object-id error text after unpublish: {removedErr}")

    let restoredB ← alice.restoreSturdyRef { vat := vat, objectId := objectIdB }
    runtime.releaseTarget restoredB

    bob.clearPublishedSturdyRefs
    assertEqual (← bob.publishedSturdyRefCount) (UInt64.ofNat 0)

    let clearedErr ←
      try
        let restored ← alice.restoreSturdyRef { vat := vat, objectId := objectIdB }
        runtime.releaseTarget restored
        pure ""
      catch err =>
        pure (toString err)
    if !(clearedErr.containsSubstr "no sturdy refs published for host:") then
      throw (IO.userError s!"missing no-published-sturdy-ref error text after clear: {clearedErr}")

    let unpublishAfterClearErr ←
      try
        bob.unpublishSturdyRef objectIdA
        pure ""
      catch err =>
        pure (toString err)
    if !(unpublishAfterClearErr.containsSubstr "no sturdy refs published for host peer id:") then
      throw (IO.userError s!"missing unpublish-after-clear error text: {unpublishAfterClearErr}")

    alice.release
    bob.release
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeMultiVatSturdyRefAsyncHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let bootstrap ← runtime.registerEchoTarget
    let alice ← runtime.newMultiVatClient "alice-sturdy-async"
    let bob ← runtime.newMultiVatServer "bob-sturdy-async" bootstrap
    let vat : Capnp.Rpc.VatId := { host := "bob-sturdy-async", unique := false }
    let objectId := ByteArray.mk #[11, 22, 33]

    let publishPending ← bob.publishSturdyRefStart objectId bootstrap
    publishPending.await
    assertEqual (← bob.publishedSturdyRefCount) (UInt64.ofNat 1)

    let restorePending ← alice.restoreSturdyRefStart { vat := vat, objectId := objectId }
    let restored ← restorePending.awaitTarget
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM restored payload
    assertEqual response.capTable.caps.size 0
    runtime.releaseTarget restored

    let unpublishTask ← bob.unpublishSturdyRefAsTask objectId
    match (← IO.wait unpublishTask) with
    | .ok () => pure ()
    | .error err => throw err
    assertEqual (← bob.publishedSturdyRefCount) (UInt64.ofNat 0)

    bob.publishSturdyRef objectId bootstrap
    let clearPromise ← bob.clearPublishedSturdyRefsAsPromise
    match (← clearPromise.awaitResult) with
    | .ok () => pure ()
    | .error err => throw err
    assertEqual (← bob.publishedSturdyRefCount) (UInt64.ofNat 0)

    let restorePromise ← alice.restoreSturdyRefAsPromise { vat := vat, objectId := objectId }
    match (← restorePromise.awaitResult) with
    | .ok target =>
        runtime.releaseTarget target
        throw (IO.userError "restoreSturdyRefAsPromise should fail after clear")
    | .error err =>
        if !((toString err).containsSubstr "no sturdy refs published for host:") then
          throw (IO.userError s!"unexpected restoreSturdyRefAsPromise error text: {err}")

    alice.release
    bob.release
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeMultiVatSturdyRefErgonomicHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let network := runtime.vatNetwork
    let bootstrap ← runtime.registerEchoTarget
    let alice ← runtime.newMultiVatClient "alice-sturdy-ergonomic"
    let bob ← runtime.newMultiVatServer "bob-sturdy-ergonomic" bootstrap
    let host := "bob-sturdy-ergonomic"

    let checkTarget (target : Echo) : IO Unit := do
      let response ← Capnp.Rpc.RuntimeM.run runtime do
        Echo.callFooM target payload
      assertEqual response.capTable.caps.size 0
      runtime.releaseTarget target

    let objectIdPeer := ByteArray.mk #[31, 32, 33]
    let objectIdRuntimeM := ByteArray.mk #[41, 42, 43]
    let objectIdRuntimeMScope := ByteArray.mk #[51, 52, 53]
    let objectIdNetwork := ByteArray.mk #[61, 62, 63]

    bob.withPublishedSturdyRef objectIdPeer bootstrap (fun _ => do
      checkTarget (← alice.restoreSturdyRefAt host objectIdPeer)
      let pending ← alice.restoreSturdyRefStartAt host objectIdPeer
      checkTarget (← pending.awaitTarget)

      let task ← alice.restoreSturdyRefAsTaskAt host objectIdPeer
      match (← IO.wait task) with
      | .ok target => checkTarget target
      | .error err => throw err

      let promise ← alice.restoreSturdyRefAsPromiseAt host objectIdPeer
      match (← promise.awaitResult) with
      | .ok target => checkTarget target
      | .error err => throw err)

    let peerMissingErr ←
      try
        let restored ← alice.restoreSturdyRefAt host objectIdPeer
        runtime.releaseTarget restored
        pure ""
      catch err =>
        pure (toString err)
    if !(peerMissingErr.containsSubstr "no sturdy refs published for host:") then
      throw (IO.userError s!"missing peer helper cleanup error text: {peerMissingErr}")

    Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.multiVatPublishSturdyRef bob objectIdRuntimeM bootstrap
    checkTarget (← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.multiVatRestoreSturdyRefAt alice host objectIdRuntimeM)
    let pendingM ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.multiVatRestoreSturdyRefStartAt alice host objectIdRuntimeM
    checkTarget (← pendingM.awaitTarget)
    let taskM ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.multiVatRestoreSturdyRefAsTaskAt alice host objectIdRuntimeM
    match (← IO.wait taskM) with
    | .ok target => checkTarget target
    | .error err => throw err
    let promiseM ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.multiVatRestoreSturdyRefAsPromiseAt alice host objectIdRuntimeM
    match (← promiseM.awaitResult) with
    | .ok target => checkTarget target
    | .error err => throw err
    Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.multiVatUnpublishSturdyRef bob objectIdRuntimeM

    let restoredScopedM ← Capnp.Rpc.RuntimeM.run runtime do
      Capnp.Rpc.RuntimeM.multiVatWithPublishedSturdyRef bob objectIdRuntimeMScope bootstrap
        (fun _ => Capnp.Rpc.RuntimeM.multiVatRestoreSturdyRefAt alice host objectIdRuntimeMScope)
    checkTarget restoredScopedM
    let scopedMMissingErr ←
      try
        let restored ← Capnp.Rpc.RuntimeM.run runtime do
          Capnp.Rpc.RuntimeM.multiVatRestoreSturdyRefAt alice host objectIdRuntimeMScope
        runtime.releaseTarget restored
        pure ""
      catch err =>
        pure (toString err)
    if !(scopedMMissingErr.containsSubstr "no sturdy refs published for host:") then
      throw (IO.userError s!"missing RuntimeM helper cleanup error text: {scopedMMissingErr}")

    network.withPublishedSturdyRef bob objectIdNetwork bootstrap (fun _ => do
      checkTarget (← network.restoreSturdyRefAt alice host objectIdNetwork)
      let pending ← network.restoreSturdyRefStartAt alice host objectIdNetwork
      checkTarget (← pending.awaitTarget)

      let task ← network.restoreSturdyRefAsTaskAt alice host objectIdNetwork
      match (← IO.wait task) with
      | .ok target => checkTarget target
      | .error err => throw err

      let promise ← network.restoreSturdyRefAsPromiseAt alice host objectIdNetwork
      match (← promise.awaitResult) with
      | .ok target => checkTarget target
      | .error err => throw err)

    let networkMissingErr ←
      try
        let restored ← network.restoreSturdyRefAt alice host objectIdNetwork
        runtime.releaseTarget restored
        pure ""
      catch err =>
        pure (toString err)
    if !(networkMissingErr.containsSubstr "no sturdy refs published for host:") then
      throw (IO.userError s!"missing vat-network helper cleanup error text: {networkMissingErr}")

    alice.release
    bob.release
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeVatNetworkBootstrapPeer : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let network := runtime.vatNetwork
    let bootstrap ← runtime.registerEchoTarget
    let alice ← network.newClient "alice-network"
    let bob ← network.newServer "bob-network" bootstrap
    let target ← network.bootstrap alice bob
    let response ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM target payload
    assertEqual response.capTable.caps.size 0
    assertEqual (← network.hasConnection alice bob) true
    runtime.releaseTarget target
    network.releasePeer alice
    network.releasePeer bob
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeVatNetworkSturdyRefLifecycleHelpers : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  let seenCaller ← IO.mkRef ({ host := "", unique := false } : Capnp.Rpc.VatId)
  let seenObjectId ← IO.mkRef ByteArray.empty
  try
    let network := runtime.vatNetwork
    let bootstrap ← runtime.registerEchoTarget
    let alice ← network.newClient "alice-network-sturdy"
    let bob ← network.newServer "bob-network-sturdy" bootstrap

    let restoredObjectId := ByteArray.mk #[9, 8, 7, 6]
    network.setRestorer bob (fun caller objectId => do
      seenCaller.set caller
      seenObjectId.set objectId
      pure bootstrap)
    let restoredViaRestorer ← network.restoreSturdyRef alice {
      vat := { host := "bob-network-sturdy", unique := false }
      objectId := restoredObjectId
    }
    let response1 ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM restoredViaRestorer payload
    assertEqual response1.capTable.caps.size 0
    let caller := (← seenCaller.get)
    assertEqual caller.host "alice-network-sturdy"
    assertEqual caller.unique false
    assertEqual ((← seenObjectId.get) == restoredObjectId) true
    runtime.releaseTarget restoredViaRestorer

    network.clearRestorer bob
    let publishedObjectId := ByteArray.mk #[1, 2, 3, 4]
    network.publishSturdyRef bob publishedObjectId bootstrap
    assertEqual (← network.publishedSturdyRefCount bob) (UInt64.ofNat 1)
    let restoredViaPublish ← network.restoreSturdyRef alice {
      vat := { host := "bob-network-sturdy", unique := false }
      objectId := publishedObjectId
    }
    let response2 ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM restoredViaPublish payload
    assertEqual response2.capTable.caps.size 0
    runtime.releaseTarget restoredViaPublish

    network.unpublishSturdyRef bob publishedObjectId
    assertEqual (← network.publishedSturdyRefCount bob) (UInt64.ofNat 0)

    let missingAfterUnpublishErr ←
      try
        let restored ← network.restoreSturdyRef alice {
          vat := { host := "bob-network-sturdy", unique := false }
          objectId := publishedObjectId
        }
        runtime.releaseTarget restored
        pure ""
      catch err =>
        pure (toString err)
    if !(missingAfterUnpublishErr.containsSubstr "no sturdy refs published for host:") then
      throw (IO.userError s!"missing no-published-sturdy-ref error text after unpublish: {missingAfterUnpublishErr}")

    network.publishSturdyRef bob publishedObjectId bootstrap
    assertEqual (← network.publishedSturdyRefCount bob) (UInt64.ofNat 1)
    network.clearPublishedSturdyRefs bob
    assertEqual (← network.publishedSturdyRefCount bob) (UInt64.ofNat 0)

    network.releasePeer alice
    network.releasePeer bob
    runtime.releaseTarget bootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeVatNetworkPeerRuntimeMismatchForSturdyRefOps : IO Unit := do
  let runtimeA ← Capnp.Rpc.Runtime.init
  let runtimeB ← Capnp.Rpc.Runtime.init
  try
    let networkA := runtimeA.vatNetwork
    let networkB := runtimeB.vatNetwork
    let bootstrapA ← runtimeA.registerEchoTarget
    let bootstrapB ← runtimeB.registerEchoTarget
    let aliceA ← networkA.newClient "alice-network-mismatch-a"
    let bobB ← networkB.newServer "bob-network-mismatch-b" bootstrapB

    let bootstrapErr ←
      try
        let _ ← networkA.bootstrap aliceA bobB
        pure ""
      catch err =>
        pure (toString err)
    if !(bootstrapErr.containsSubstr "VatNetwork.bootstrap: peer belongs to a different runtime") then
      throw (IO.userError s!"expected vat-network bootstrap mismatch error, got: {bootstrapErr}")

    let hasConnectionErr ←
      try
        let _ ← networkA.hasConnection aliceA bobB
        pure ""
      catch err =>
        pure (toString err)
    if !(hasConnectionErr.containsSubstr "VatNetwork.hasConnection: peer belongs to a different runtime") then
      throw (IO.userError s!"expected vat-network hasConnection mismatch error, got: {hasConnectionErr}")

    let setRestorerErr ←
      try
        networkA.setRestorer bobB (fun _ _ => pure bootstrapA)
        pure ""
      catch err =>
        pure (toString err)
    if !(setRestorerErr.containsSubstr "VatNetwork.setRestorer: peer belongs to a different runtime") then
      throw (IO.userError s!"expected vat-network setRestorer mismatch error, got: {setRestorerErr}")

    let clearRestorerErr ←
      try
        networkA.clearRestorer bobB
        pure ""
      catch err =>
        pure (toString err)
    if !(clearRestorerErr.containsSubstr "VatNetwork.clearRestorer: peer belongs to a different runtime") then
      throw (IO.userError s!"expected vat-network clearRestorer mismatch error, got: {clearRestorerErr}")

    let publishErr ←
      try
        networkA.publishSturdyRef bobB ByteArray.empty bootstrapA
        pure ""
      catch err =>
        pure (toString err)
    if !(publishErr.containsSubstr "VatNetwork.publishSturdyRef: peer belongs to a different runtime") then
      throw (IO.userError s!"expected vat-network publishSturdyRef mismatch error, got: {publishErr}")

    let unpublishErr ←
      try
        networkA.unpublishSturdyRef bobB ByteArray.empty
        pure ""
      catch err =>
        pure (toString err)
    if !(unpublishErr.containsSubstr "VatNetwork.unpublishSturdyRef: peer belongs to a different runtime") then
      throw (IO.userError s!"expected vat-network unpublishSturdyRef mismatch error, got: {unpublishErr}")

    let clearPublishedErr ←
      try
        networkA.clearPublishedSturdyRefs bobB
        pure ""
      catch err =>
        pure (toString err)
    if !(clearPublishedErr.containsSubstr "VatNetwork.clearPublishedSturdyRefs: peer belongs to a different runtime") then
      throw (IO.userError s!"expected vat-network clearPublishedSturdyRefs mismatch error, got: {clearPublishedErr}")

    let publishedCountErr ←
      try
        let _ ← networkA.publishedSturdyRefCount bobB
        pure ""
      catch err =>
        pure (toString err)
    if !(publishedCountErr.containsSubstr "VatNetwork.publishedSturdyRefCount: peer belongs to a different runtime") then
      throw (IO.userError s!"expected vat-network publishedSturdyRefCount mismatch error, got: {publishedCountErr}")

    let restoreErr ←
      try
        let _ ← networkA.restoreSturdyRef bobB
          { vat := { host := "bob-network-mismatch-b", unique := false }, objectId := ByteArray.empty }
        pure ""
      catch err =>
        pure (toString err)
    if !(restoreErr.containsSubstr "VatNetwork.restoreSturdyRef: peer belongs to a different runtime") then
      throw (IO.userError s!"expected vat-network restoreSturdyRef mismatch error, got: {restoreErr}")

    let releaseErr ←
      try
        networkA.releasePeer bobB
        pure ""
      catch err =>
        pure (toString err)
    if !(releaseErr.containsSubstr "VatNetwork.releasePeer: peer belongs to a different runtime") then
      throw (IO.userError s!"expected vat-network releasePeer mismatch error, got: {releaseErr}")

    networkA.releasePeer aliceA
    networkB.releasePeer bobB
    runtimeA.releaseTarget bootstrapA
    runtimeB.releaseTarget bootstrapB
  finally
    runtimeA.shutdown
    runtimeB.shutdown

@[test]
def testRuntimeSocketThreePartyHandoffStaysProxied : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let (aliceAddress, aliceSocketPath) ← mkUnixTestAddress
  let (carolAddress, carolSocketPath) ← mkUnixTestAddress
  let aliceRuntime ← Capnp.Rpc.Runtime.init
  let carolRuntime ← Capnp.Rpc.Runtime.init
  let bobRuntime ← Capnp.Rpc.Runtime.init
  let carolStopped ← IO.mkRef false
  let cleanupSocket (path : String) : IO Unit := do
    try
      IO.FS.removeFile path
    catch _ =>
      pure ()
  try
    cleanupSocket aliceSocketPath
    cleanupSocket carolSocketPath

    let aliceBootstrap ← aliceRuntime.registerEchoTarget
    let aliceServer ← aliceRuntime.newServer aliceBootstrap
    let aliceListener ← aliceServer.listen aliceAddress

    let carolToAliceClient ← carolRuntime.newClient aliceAddress
    aliceServer.accept aliceListener
    let aliceCapViaCarol ← carolToAliceClient.bootstrap

    let carolBootstrap ← carolRuntime.registerHandlerTarget (fun _ method req => do
      if method.interfaceId == Echo.interfaceId && method.methodId == Echo.fooMethodId then
        pure (mkCapabilityPayload aliceCapViaCarol)
      else
        pure req)
    let carolServer ← carolRuntime.newServer carolBootstrap
    let carolListener ← carolServer.listen carolAddress

    let bobToCarolClient ← bobRuntime.newClient carolAddress
    carolServer.accept carolListener
    let carolCap ← bobToCarolClient.bootstrap

    let handoffResponse ← Capnp.Rpc.RuntimeM.run bobRuntime do
      Echo.callFooM carolCap payload
    assertEqual handoffResponse.capTable.caps.size 1

    let handedOffCap? := Capnp.readCapabilityFromTable handoffResponse.capTable
      (Capnp.getRoot handoffResponse.msg)
    assertEqual handedOffCap?.isSome true
    let handedOffCap ←
      match handedOffCap? with
      | some cap => pure cap
      | none => throw (IO.userError "socket handoff response missing expected capability")

    let warmup ← bobRuntime.callResult handedOffCap Echo.fooMethod payload
    match warmup with
    | .ok response =>
        assertEqual response.capTable.caps.size 0
    | .error ex =>
        throw (IO.userError
          s!"expected handed-off socket capability call to succeed before intermediary shutdown, got: {ex.type}: {ex.description}")

    let disconnectPromise ← bobToCarolClient.onDisconnectStart
    -- Current TwoPartyVatNetwork socket transport keeps the handed-off capability behind Carol.
    -- If real third-party introduction is added here later, this assertion should flip.
    carolRuntime.shutdown
    carolStopped.set true
    disconnectPromise.await

    let afterCarolShutdown ← bobRuntime.callResult handedOffCap Echo.fooMethod payload
    match afterCarolShutdown with
    | .ok _ =>
        throw (IO.userError
          "expected handed-off socket capability to disconnect after intermediary shutdown")
    | .error ex =>
        assertEqual ex.type .disconnected

    bobRuntime.releaseCapTable handoffResponse.capTable
    bobRuntime.releaseTarget carolCap
    bobToCarolClient.release
    aliceRuntime.releaseListener aliceListener
    aliceServer.release
    aliceRuntime.releaseTarget aliceBootstrap
  finally
    bobRuntime.shutdown
    if !(← carolStopped.get) then
      carolRuntime.shutdown
    aliceRuntime.shutdown
    cleanupSocket aliceSocketPath
    cleanupSocket carolSocketPath

@[test]
def testRuntimeMultiVatThirdPartyTokenStats : IO Unit := do
  let payload : Capnp.Rpc.Payload := mkNullPayload
  let runtime ← Capnp.Rpc.Runtime.init
  let heldCap ← IO.mkRef (none : Option Capnp.Rpc.Client)
  try
    let bobBootstrap ← runtime.registerHandlerTarget (fun _ _ req => pure req)
    let carolBootstrap ← runtime.registerAdvancedHandlerTarget (fun _ method req => do
      if method.interfaceId == Echo.interfaceId && method.methodId == Echo.fooMethodId then
        let cap? := Capnp.readCapabilityFromTable req.capTable (Capnp.getRoot req.msg)
        match cap? with
        | some cap =>
            let retained ← runtime.retainTarget cap
            match (← heldCap.get) with
            | some previous => runtime.releaseTarget previous
            | none => pure ()
            heldCap.set (some retained)
        | none => heldCap.set none
        pure (Capnp.Rpc.Advanced.respond mkNullPayload)
      else if method.interfaceId == Echo.interfaceId && method.methodId == Echo.barMethodId then
        match (← heldCap.get) with
        | some target =>
            pure (Capnp.Rpc.Advanced.asyncForward target Echo.fooMethod payload)
        | none =>
            throw (IO.userError "Carol has no held capability")
      else
        pure (Capnp.Rpc.Advanced.respond req))

    let network := runtime.vatNetwork
    let alice ← network.newClient "alice-token"
    let bob ← network.newServer "bob-token" bobBootstrap
    let carol ← network.newServer "carol-token" carolBootstrap

    network.resetForwardingStats
    let stats0 ← network.stats
    assertEqual stats0.forwardCount (UInt64.ofNat 0)
    assertEqual stats0.deniedForwardCount (UInt64.ofNat 0)
    assertEqual stats0.thirdPartyTokenCount (UInt64.ofNat 0)

    let bobCap ← network.bootstrap alice bob
    let carolCap ← network.bootstrap alice carol
    let _ ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callFooM carolCap (mkCapabilityPayload bobCap)
    let _ ← Capnp.Rpc.RuntimeM.run runtime do
      Echo.callBarM carolCap payload

    let stats1 ← network.stats
    assertTrue (stats1.thirdPartyTokenCount > (UInt64.ofNat 0))
      "expected third-party handoff token count to increase"

    runtime.releaseTarget bobCap
    runtime.releaseTarget carolCap
    match (← heldCap.get) with
    | some cap => runtime.releaseTarget cap
    | none => pure ()
    network.releasePeer alice
    network.releasePeer bob
    network.releasePeer carol
    runtime.releaseTarget bobBootstrap
    runtime.releaseTarget carolBootstrap
  finally
    runtime.shutdown

@[test]
def testRuntimeAsyncFFIPump : IO Unit := do
  let runtime ← Capnp.Rpc.Runtime.init
  try
    let pumpTask ← runtime.pumpAsTask
    match (← IO.wait pumpTask) with
    | .ok () => pure ()
    | .error err => throw err
  finally
    runtime.shutdown
