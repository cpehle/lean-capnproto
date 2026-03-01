import Capnp.Async
import Capnp.Runtime
import Capnp.Ffi

namespace Capnp
namespace Rpc

abbrev Payload := Capnp.RpcEnvelope

structure Method where
  interfaceId : UInt64
  methodId : UInt16
  deriving Inhabited, BEq, Repr

abbrev Client := Capnp.Capability

structure Listener extends Capnp.FfiRuntimeHandle UInt64
  deriving Inhabited, BEq, Repr

structure RuntimeClient extends Capnp.FfiHandle
  deriving Inhabited, BEq, Repr

structure RuntimeServer extends Capnp.FfiHandle
  deriving Inhabited, BEq, Repr

structure RuntimePendingCall extends Capnp.FfiHandle
  deriving Inhabited, BEq, Repr

structure RuntimeTransport extends Capnp.FfiRuntimeHandle UInt64
  deriving Inhabited, BEq, Repr

structure RuntimeVatPeer extends Capnp.FfiHandle
  deriving Inhabited, BEq, Repr

structure VatId where
  host : String
  unique : Bool := false
  deriving Inhabited, BEq, Repr

namespace VatId

@[inline] def ofHost (host : String) (unique : Bool := false) : VatId :=
  { host := host, unique := unique }

end VatId

inductive TwoPartyVatSide where
  | client
  | server
  | unknown (raw : UInt16)
  deriving Inhabited, BEq, Repr

@[inline] def TwoPartyVatSide.ofUInt16 : UInt16 -> TwoPartyVatSide
  | 0 => .client
  | 1 => .server
  | raw => .unknown raw

@[inline] def TwoPartyVatSide.toUInt16 : TwoPartyVatSide -> UInt16
  | .client => 0
  | .server => 1
  | .unknown raw => raw

instance : ToString TwoPartyVatSide where
  toString
    | .client => "client"
    | .server => "server"
    | .unknown raw => s!"unknown({raw})"

structure SturdyRef where
  vat : VatId
  objectId : ByteArray := ByteArray.empty
  deriving Inhabited, BEq

namespace SturdyRef

@[inline] def ofVat (vat : VatId) (objectId : ByteArray := ByteArray.empty) : SturdyRef :=
  { vat := vat, objectId := objectId }

@[inline] def ofHost (host : String) (objectId : ByteArray := ByteArray.empty)
    (unique : Bool := false) : SturdyRef :=
  { vat := VatId.ofHost host unique, objectId := objectId }

@[inline] def withObjectId (sturdyRef : SturdyRef) (objectId : ByteArray) : SturdyRef :=
  { sturdyRef with objectId := objectId }

end SturdyRef

structure MultiVatStats where
  forwardCount : UInt64
  deniedForwardCount : UInt64
  thirdPartyTokenCount : UInt64
  deriving Inhabited, BEq, Repr

structure RpcDiagnostics where
  questionCount : UInt64
  answerCount : UInt64
  exportCount : UInt64
  importCount : UInt64
  embargoCount : UInt64
  isIdle : Bool
  deriving Inhabited, BEq, Repr

structure ProtocolMessageCounts where
  resolveCount : UInt64
  disembargoCount : UInt64
  deriving Inhabited, BEq, Repr

inductive ProtocolMessageTraceTag where
  | resolve
  | disembargo
  | unknown (raw : UInt16)
  deriving Inhabited, BEq, Repr

@[inline] def ProtocolMessageTraceTag.toUInt16 : ProtocolMessageTraceTag -> UInt16
  | .resolve => 1
  | .disembargo => 2
  | .unknown raw => raw

@[inline] def ProtocolMessageTraceTag.ofUInt16 : UInt16 -> ProtocolMessageTraceTag
  | 1 => .resolve
  | 2 => .disembargo
  | raw => .unknown raw

instance : ToString ProtocolMessageTraceTag where
  toString
    | .resolve => "resolve"
    | .disembargo => "disembargo"
    | .unknown raw => s!"unknown({raw})"

@[inline] def Client.ofCapability (cap : Capnp.Capability) : Client := cap

@[inline] def Client.toCapability (client : Client) : Capnp.Capability := client

structure Backend where
  call : Client -> Method -> Payload -> IO Payload

@[inline] def call (backend : Backend) (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO Payload :=
  backend.call target method payload

abbrev RawCall := Client -> Method -> ByteArray -> IO ByteArray
abbrev RawHandlerCall := Client -> UInt64 -> UInt16 -> ByteArray -> ByteArray ->
    IO (ByteArray × ByteArray)
abbrev RawTailCallHandlerCall := Client -> UInt64 -> UInt16 -> ByteArray -> ByteArray ->
    IO UInt32

structure AdvancedHandlerControl where
  releaseParams : Bool := false
  allowCancellation : Bool := false
  isStreaming : Bool := false
  deriving Inhabited, BEq, Repr

inductive AdvancedSendResultsTo where
  | yourself
  | caller
  deriving Inhabited, BEq, Repr

structure AdvancedCallHints where
  noPromisePipelining : Bool := false
  onlyPromisePipeline : Bool := false
  deriving Inhabited, BEq, Repr

structure AdvancedForwardOptions where
  sendResultsTo : AdvancedSendResultsTo := .yourself
  callHints : AdvancedCallHints := {}
  deriving Inhabited, BEq, Repr

inductive RemoteExceptionType where
  | failed
  | overloaded
  | disconnected
  | unimplemented
  deriving Inhabited, BEq, Repr

@[inline] def RemoteExceptionType.toUInt8 : RemoteExceptionType -> UInt8
  | .failed => 0
  | .overloaded => 1
  | .disconnected => 2
  | .unimplemented => 3

@[inline] def RemoteExceptionType.ofUInt8 : UInt8 -> RemoteExceptionType
  | 0 => .failed
  | 1 => .overloaded
  | 2 => .disconnected
  | 3 => .unimplemented
  | _ => .failed

instance : ToString RemoteExceptionType where
  toString
    | .failed => "failed"
    | .overloaded => "overloaded"
    | .disconnected => "disconnected"
    | .unimplemented => "unimplemented"

structure RemoteException where
  description : String
  remoteTrace : String
  detail : ByteArray
  fileName : String
  lineNumber : UInt32
  -- `kj::Exception::Type` as a `UInt8` (same encoding as `RemoteExceptionType.toUInt8`).
  typeTag : UInt8
  deriving Inhabited, BEq

@[inline] def RemoteException.type (e : RemoteException) : RemoteExceptionType :=
  RemoteExceptionType.ofUInt8 e.typeTag

inductive RawCallOutcome where
  | ok (response : ByteArray) (responseCaps : ByteArray)
  | error (ex : RemoteException)
  deriving Inhabited, BEq

inductive AdvancedHandlerResult where
  | respond (payload : Payload)
  | asyncCall (target : Client) (method : Method)
      (payload : Payload := Capnp.emptyRpcEnvelope)
  | forwardCall (target : Client) (method : Method)
      (payload : Payload := Capnp.emptyRpcEnvelope)
      (options : AdvancedForwardOptions := {})
  | tailCall (target : Client) (method : Method)
      (payload : Payload := Capnp.emptyRpcEnvelope)
  | throwRemote (message : String) (detail : ByteArray := ByteArray.empty)
  | throwRemoteWithType (type : RemoteExceptionType) (message : String)
      (detail : ByteArray := ByteArray.empty)
  | control (opts : AdvancedHandlerControl) (next : AdvancedHandlerResult)

inductive AdvancedHandlerReply where
  | now (result : AdvancedHandlerResult)
  | deferred (task : Task (Except IO.Error AdvancedHandlerResult))
  | deferredWithCancel (task : Task (Except IO.Error AdvancedHandlerResult))
      (cancelTask : Task (Except IO.Error AdvancedHandlerResult))
  | control (opts : AdvancedHandlerControl) (next : AdvancedHandlerReply)
  | pipeline (pipeline : Payload) (next : AdvancedHandlerReply)

@[inline] def AdvancedHandlerControl.hasAny (opts : AdvancedHandlerControl) : Bool :=
  opts.releaseParams || opts.allowCancellation || opts.isStreaming

@[inline] def AdvancedHandlerControl.merge
    (first : AdvancedHandlerControl) (second : AdvancedHandlerControl) :
    AdvancedHandlerControl :=
  { releaseParams := first.releaseParams || second.releaseParams
    allowCancellation := first.allowCancellation || second.allowCancellation
    isStreaming := first.isStreaming || second.isStreaming
  }

@[inline] private def applyAdvancedHandlerResultControl
    (opts : AdvancedHandlerControl) (next : AdvancedHandlerResult) :
    AdvancedHandlerResult :=
  if opts.hasAny then
    match next with
    | .control nested nestedNext => .control (nested.merge opts) nestedNext
    | _ => .control opts next
  else
    next

@[inline] private def applyAdvancedHandlerReplyControl
    (opts : AdvancedHandlerControl) (next : AdvancedHandlerReply) :
    AdvancedHandlerReply :=
  if opts.hasAny then
    match next with
    | .control nested nestedNext => .control (nested.merge opts) nestedNext
    | _ => .control opts next
  else
    next

@[inline] def AdvancedHandlerReply.deferTask
    (task : Task (Except IO.Error AdvancedHandlerResult))
    (opts : AdvancedHandlerControl := {}) : AdvancedHandlerReply :=
  applyAdvancedHandlerReplyControl opts (.deferred task)

@[inline] def AdvancedHandlerReply.deferTaskWithCancel
    (task : Task (Except IO.Error AdvancedHandlerResult))
    (cancelTask : Task (Except IO.Error AdvancedHandlerResult))
    (opts : AdvancedHandlerControl := {}) : AdvancedHandlerReply :=
  applyAdvancedHandlerReplyControl { opts with allowCancellation := true }
    (.deferredWithCancel task cancelTask)

@[inline] def AdvancedHandlerReply.deferPromise
    (promise : Capnp.Async.Promise AdvancedHandlerResult)
    (opts : AdvancedHandlerControl := {}) : AdvancedHandlerReply :=
  AdvancedHandlerReply.deferTask promise.toTask opts

@[inline] def AdvancedHandlerReply.deferPromiseWithCancel
    (promise : Capnp.Async.Promise AdvancedHandlerResult)
    (cancelPromise : Capnp.Async.Promise AdvancedHandlerResult)
    (opts : AdvancedHandlerControl := {}) : AdvancedHandlerReply :=
  AdvancedHandlerReply.deferTaskWithCancel promise.toTask cancelPromise.toTask opts

@[inline] def AdvancedHandlerReply.defer
    (next : IO AdvancedHandlerResult) (opts : AdvancedHandlerControl := {}) :
    IO AdvancedHandlerReply := do
  let task ← IO.asTask next
  return AdvancedHandlerReply.deferTask task opts

@[inline] def AdvancedHandlerResult.withControl
    (opts : AdvancedHandlerControl) (next : AdvancedHandlerResult) : AdvancedHandlerResult :=
  applyAdvancedHandlerResultControl opts next

@[inline] def AdvancedHandlerResult.releaseParams
    (next : AdvancedHandlerResult) : AdvancedHandlerResult :=
  AdvancedHandlerResult.withControl { releaseParams := true } next

@[inline] def AdvancedHandlerResult.allowCancellation
    (next : AdvancedHandlerResult) : AdvancedHandlerResult :=
  AdvancedHandlerResult.withControl { allowCancellation := true } next

@[inline] def AdvancedHandlerResult.streaming
    (next : AdvancedHandlerResult) : AdvancedHandlerResult :=
  AdvancedHandlerResult.withControl { isStreaming := true } next

@[inline] def AdvancedHandlerReply.withControl
    (opts : AdvancedHandlerControl) (next : AdvancedHandlerReply) : AdvancedHandlerReply :=
  applyAdvancedHandlerReplyControl opts next

@[inline] def AdvancedHandlerReply.withPipeline
    (pipeline : Payload) (next : AdvancedHandlerReply) : AdvancedHandlerReply :=
  .pipeline pipeline next

@[inline] def AdvancedHandlerReply.releaseParams
    (next : AdvancedHandlerReply) : AdvancedHandlerReply :=
  AdvancedHandlerReply.withControl { releaseParams := true } next

@[inline] def AdvancedHandlerReply.allowCancellation
    (next : AdvancedHandlerReply) : AdvancedHandlerReply :=
  AdvancedHandlerReply.withControl { allowCancellation := true } next

@[inline] def AdvancedHandlerReply.streaming
    (next : AdvancedHandlerReply) : AdvancedHandlerReply :=
  AdvancedHandlerReply.withControl { isStreaming := true } next

inductive RawAdvancedHandlerResult where
  | returnPayload (response : ByteArray) (responseCaps : ByteArray)
  | asyncCall (target : UInt32) (interfaceId : UInt64) (methodId : UInt16)
      (request : ByteArray) (requestCaps : ByteArray)
  | tailCall (target : UInt32) (interfaceId : UInt64) (methodId : UInt16)
      (request : ByteArray) (requestCaps : ByteArray)
  | throwRemote (message : String) (detail : ByteArray)
  | control (releaseParams : Bool) (allowCancellation : Bool) (isStreaming : Bool)
      (next : RawAdvancedHandlerResult)
  | awaitTask (task : Task (Except IO.Error RawAdvancedHandlerResult))
      (cancelTask : Task (Except IO.Error AdvancedHandlerResult))
  | sendResultsToCaller (next : RawAdvancedHandlerResult)
  | callHints (noPromisePipelining : Bool) (onlyPromisePipeline : Bool)
      (next : RawAdvancedHandlerResult)
  | exceptionType (type : UInt8) (next : RawAdvancedHandlerResult)
  | setPipeline (pipeline : ByteArray) (pipelineCaps : ByteArray) (next : RawAdvancedHandlerResult)

abbrev RawAdvancedHandlerCall := Client -> UInt64 -> UInt16 -> ByteArray -> ByteArray ->
    IO RawAdvancedHandlerResult
abbrev RawBootstrapFactoryCall := UInt16 -> IO UInt32
abbrev RawVatBootstrapFactoryCall := String -> Bool -> IO UInt32
abbrev RawVatRestorerCall := String -> Bool -> ByteArray -> IO UInt32
abbrev RawTraceEncoder := String -> IO String

@[inline] def AdvancedForwardOptions.toCaller
    (callHints : AdvancedCallHints := {}) : AdvancedForwardOptions :=
  { sendResultsTo := .caller, callHints := callHints }

@[inline] def AdvancedForwardOptions.toYourself
    (callHints : AdvancedCallHints := {}) : AdvancedForwardOptions :=
  { sendResultsTo := .yourself, callHints := callHints }

@[inline] def AdvancedCallHints.withNoPromisePipelining : AdvancedCallHints :=
  { noPromisePipelining := true }

@[inline] def AdvancedCallHints.withOnlyPromisePipeline : AdvancedCallHints :=
  { onlyPromisePipeline := true }

@[inline] def AdvancedCallHints.setNoPromisePipelining
    (hints : AdvancedCallHints := {}) : AdvancedCallHints :=
  { hints with noPromisePipelining := true }

@[inline] def AdvancedCallHints.setOnlyPromisePipeline
    (hints : AdvancedCallHints := {}) : AdvancedCallHints :=
  { hints with onlyPromisePipeline := true }

@[inline] def AdvancedForwardOptions.setNoPromisePipelining
    (opts : AdvancedForwardOptions := {}) : AdvancedForwardOptions :=
  { opts with callHints := AdvancedCallHints.setNoPromisePipelining opts.callHints }

@[inline] def AdvancedForwardOptions.setOnlyPromisePipeline
    (opts : AdvancedForwardOptions := {}) : AdvancedForwardOptions :=
  { opts with callHints := AdvancedCallHints.setOnlyPromisePipeline opts.callHints }

@[inline] def AdvancedForwardOptions.setSendResultsToCaller
    (opts : AdvancedForwardOptions := {}) : AdvancedForwardOptions :=
  { opts with sendResultsTo := .caller }

namespace Advanced

@[inline] def respond (payload : Payload) : AdvancedHandlerResult :=
  .respond payload

@[inline] def asyncForward (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : AdvancedHandlerResult :=
  .asyncCall target method payload

@[inline] def forward (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope)
    (opts : AdvancedForwardOptions := {}) : AdvancedHandlerResult :=
  .forwardCall target method payload opts

@[inline] def forwardToCaller (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope)
    (callHints : AdvancedCallHints := {}) : AdvancedHandlerResult :=
  .forwardCall target method payload (AdvancedForwardOptions.toCaller callHints)

@[inline] def tailCall (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope)
    (opts : AdvancedHandlerControl := {}) : AdvancedHandlerResult :=
  AdvancedHandlerResult.withControl opts (.tailCall target method payload)

@[inline] def throwRemote (message : String)
    (detail : ByteArray := ByteArray.empty) : AdvancedHandlerResult :=
  .throwRemote message detail

@[inline] def throwRemoteWithType (type : RemoteExceptionType) (message : String)
    (detail : ByteArray := ByteArray.empty) : AdvancedHandlerResult :=
  .throwRemoteWithType type message detail

@[inline] def streaming (next : AdvancedHandlerResult) : AdvancedHandlerResult :=
  AdvancedHandlerResult.streaming next

@[inline] def now (result : AdvancedHandlerResult) : AdvancedHandlerReply :=
  .now result

@[inline] def defer
    (next : IO AdvancedHandlerResult) (opts : AdvancedHandlerControl := {}) :
    IO AdvancedHandlerReply :=
  AdvancedHandlerReply.defer next opts

@[inline] def deferTask
    (task : Task (Except IO.Error AdvancedHandlerResult))
    (opts : AdvancedHandlerControl := {}) : AdvancedHandlerReply :=
  AdvancedHandlerReply.deferTask task opts

@[inline] def deferTaskWithCancel
    (task : Task (Except IO.Error AdvancedHandlerResult))
    (cancelTask : Task (Except IO.Error AdvancedHandlerResult))
    (opts : AdvancedHandlerControl := {}) : AdvancedHandlerReply :=
  AdvancedHandlerReply.deferTaskWithCancel task cancelTask opts

@[inline] def deferPromise
    (promise : Capnp.Async.Promise AdvancedHandlerResult)
    (opts : AdvancedHandlerControl := {}) : AdvancedHandlerReply :=
  AdvancedHandlerReply.deferPromise promise opts

@[inline] def deferPromiseWithCancel
    (promise : Capnp.Async.Promise AdvancedHandlerResult)
    (cancelPromise : Capnp.Async.Promise AdvancedHandlerResult)
    (opts : AdvancedHandlerControl := {}) : AdvancedHandlerReply :=
  AdvancedHandlerReply.deferPromiseWithCancel promise cancelPromise opts

@[inline] def streamingDefer
    (next : IO AdvancedHandlerResult) (opts : AdvancedHandlerControl := {}) :
    IO AdvancedHandlerReply := do
  return AdvancedHandlerReply.streaming (← defer next opts)

@[inline] def setPipeline (pipeline : Payload) (next : AdvancedHandlerReply) : AdvancedHandlerReply :=
  AdvancedHandlerReply.withPipeline pipeline next

end Advanced

@[inline] def Payload.toBytes (payload : Payload) : ByteArray :=
  Capnp.writeMessage payload.msg

@[inline] def Payload.ofBytes (bytes : ByteArray) : Payload :=
  { msg := Capnp.readMessage bytes, capTable := Capnp.emptyCapTable }

def Backend.ofRawCall (rawCall : RawCall) : Backend where
  call := fun target method payload => do
    let requestBytes := payload.toBytes
    let responseBytes ← rawCall target method requestBytes
    return Payload.ofBytes responseBytes

@[extern "capnp_lean_rpc_raw_call_on_runtime"]
opaque ffiRawCallOnRuntimeImpl
    (runtime : UInt64) (target : UInt32) (interfaceId : UInt64) (methodId : UInt16)
    (request : @& ByteArray) : IO ByteArray

@[extern "capnp_lean_rpc_raw_call_with_caps_on_runtime"]
opaque ffiRawCallWithCapsOnRuntimeImpl
    (runtime : UInt64) (target : UInt32) (interfaceId : UInt64) (methodId : UInt16)
    (request : @& ByteArray) (requestCaps : @& ByteArray) : IO (ByteArray × ByteArray)

@[extern "capnp_lean_rpc_raw_call_with_caps_on_runtime_outcome"]
opaque ffiRawCallWithCapsOnRuntimeOutcomeImpl
    (runtime : UInt64) (target : UInt32) (interfaceId : UInt64) (methodId : UInt16)
    (request : @& ByteArray) (requestCaps : @& ByteArray) : IO RawCallOutcome

@[extern "capnp_lean_rpc_runtime_start_call_with_caps"]
opaque ffiRuntimeStartCallWithCapsImpl
    (runtime : UInt64) (target : UInt32) (interfaceId : UInt64) (methodId : UInt16)
    (request : @& ByteArray) (requestCaps : @& ByteArray) : IO UInt32

@[extern "capnp_lean_rpc_runtime_start_streaming_call_with_caps"]
opaque ffiRuntimeStartStreamingCallWithCapsImpl
    (runtime : UInt64) (target : UInt32) (interfaceId : UInt64) (methodId : UInt16)
    (request : @& ByteArray) (requestCaps : @& ByteArray) : IO UInt32

@[extern "capnp_lean_rpc_runtime_payload_ref_from_bytes"]
opaque ffiRuntimePayloadRefFromBytesImpl
    (runtime : UInt64) (request : @& ByteArray) (requestCaps : @& ByteArray) : IO UInt32

@[extern "capnp_lean_rpc_runtime_payload_ref_to_bytes"]
opaque ffiRuntimePayloadRefToBytesImpl
    (runtime : UInt64) (payloadRef : UInt32) : IO (ByteArray × ByteArray)

@[extern "capnp_lean_rpc_runtime_payload_ref_release"]
opaque ffiRuntimePayloadRefReleaseImpl (runtime : UInt64) (payloadRef : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_call_with_payload_ref"]
opaque ffiRuntimeCallWithPayloadRefImpl
    (runtime : UInt64) (target : UInt32) (interfaceId : UInt64) (methodId : UInt16)
    (payloadRef : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_start_call_with_payload_ref"]
opaque ffiRuntimeStartCallWithPayloadRefImpl
    (runtime : UInt64) (target : UInt32) (interfaceId : UInt64) (methodId : UInt16)
    (payloadRef : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_start_streaming_call_with_payload_ref"]
opaque ffiRuntimeStartStreamingCallWithPayloadRefImpl
    (runtime : UInt64) (target : UInt32) (interfaceId : UInt64) (methodId : UInt16)
    (payloadRef : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_pending_call_await"]
opaque ffiRuntimePendingCallAwaitImpl
    (runtime : UInt64) (pendingCall : UInt32) : IO (ByteArray × ByteArray)

@[extern "capnp_lean_rpc_runtime_pending_call_await_payload_ref"]
opaque ffiRuntimePendingCallAwaitPayloadRefImpl
    (runtime : UInt64) (pendingCall : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_pending_call_await_outcome"]
opaque ffiRuntimePendingCallAwaitOutcomeImpl
    (runtime : UInt64) (pendingCall : UInt32) : IO RawCallOutcome

@[extern "capnp_lean_rpc_runtime_pending_call_release"]
opaque ffiRuntimePendingCallReleaseImpl (runtime : UInt64) (pendingCall : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_pending_call_release_deferred"]
opaque ffiRuntimePendingCallReleaseDeferredImpl
    (runtime : UInt64) (pendingCall : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_pending_call_get_pipelined_cap"]
opaque ffiRuntimePendingCallGetPipelinedCapImpl
    (runtime : UInt64) (pendingCall : UInt32) (pipelineOps : @& ByteArray) : IO UInt32

@[extern "capnp_lean_rpc_runtime_register_promise_await"]
opaque ffiRuntimeRegisterPromiseAwaitImpl
    (runtime : UInt64) (promise : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_register_promise_cancel"]
opaque ffiRuntimeRegisterPromiseCancelImpl
    (runtime : UInt64) (promise : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_register_promise_release"]
opaque ffiRuntimeRegisterPromiseReleaseImpl
    (runtime : UInt64) (promise : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_unit_promise_await"]
opaque ffiRuntimeUnitPromiseAwaitImpl
    (runtime : UInt64) (promise : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_unit_promise_cancel"]
opaque ffiRuntimeUnitPromiseCancelImpl
    (runtime : UInt64) (promise : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_unit_promise_release"]
opaque ffiRuntimeUnitPromiseReleaseImpl
    (runtime : UInt64) (promise : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_pump"]
opaque ffiRuntimePumpImpl (runtime : UInt64) : IO Unit

@[extern "capnp_lean_rpc_runtime_pump_async"]
opaque ffiRuntimePumpAsyncImpl (runtime : UInt64) (promise : @& IO.Promise (Except IO.Error Unit)) : IO Unit

@[extern "capnp_lean_rpc_runtime_streaming_call_with_caps"]
opaque ffiRuntimeStreamingCallWithCapsImpl
    (runtime : UInt64) (target : UInt32) (interfaceId : UInt64) (methodId : UInt16)
    (request : @& ByteArray) (requestCaps : @& ByteArray) : IO Unit

@[extern "capnp_lean_rpc_runtime_target_get_fd"]
opaque ffiRuntimeTargetGetFdImpl (runtime : UInt64) (target : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_target_when_resolved"]
opaque ffiRuntimeTargetWhenResolvedImpl (runtime : UInt64) (target : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_target_when_resolved_start"]
opaque ffiRuntimeTargetWhenResolvedStartImpl (runtime : UInt64) (target : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_target_when_resolved_poll"]
opaque ffiRuntimeTargetWhenResolvedPollImpl (runtime : UInt64) (target : UInt32) : IO Bool

@[extern "capnp_lean_rpc_runtime_ordering_set_resolve_hold"]
opaque ffiRuntimeOrderingSetResolveHoldImpl (runtime : UInt64) (enabled : UInt8) : IO Unit

@[extern "capnp_lean_rpc_runtime_ordering_flush_resolves"]
opaque ffiRuntimeOrderingFlushResolvesImpl (runtime : UInt64) : IO UInt64

@[extern "capnp_lean_rpc_runtime_ordering_held_resolve_count"]
opaque ffiRuntimeOrderingHeldResolveCountImpl (runtime : UInt64) : IO UInt64

@[extern "capnp_lean_rpc_runtime_enable_trace_encoder"]
opaque ffiRuntimeEnableTraceEncoderImpl (runtime : UInt64) : IO Unit

@[extern "capnp_lean_rpc_runtime_disable_trace_encoder"]
opaque ffiRuntimeDisableTraceEncoderImpl (runtime : UInt64) : IO Unit

@[extern "capnp_lean_rpc_runtime_set_trace_encoder"]
opaque ffiRuntimeSetTraceEncoderImpl (runtime : UInt64) (encoder : @& RawTraceEncoder) : IO Unit

@[extern "capnp_lean_rpc_runtime_new"]
opaque ffiRuntimeNewImpl : IO UInt64

@[extern "capnp_lean_rpc_runtime_new_with_fd_limit"]
opaque ffiRuntimeNewWithFdLimitImpl (maxFdsPerMessage : UInt32) : IO UInt64

@[extern "capnp_lean_rpc_runtime_release"]
opaque ffiRuntimeReleaseImpl (runtime : UInt64) : IO Unit

@[extern "capnp_lean_rpc_runtime_is_alive"]
opaque ffiRuntimeIsAliveImpl (runtime : UInt64) : IO Bool

@[extern "capnp_lean_rpc_runtime_register_echo_target"]
opaque ffiRuntimeRegisterEchoTargetImpl (runtime : UInt64) : IO UInt32

@[extern "capnp_lean_rpc_runtime_register_handler_target"]
opaque ffiRuntimeRegisterHandlerTargetImpl (runtime : UInt64) (handler : @& RawHandlerCall) :
    IO UInt32

@[extern "capnp_lean_rpc_runtime_register_advanced_handler_target"]
opaque ffiRuntimeRegisterAdvancedHandlerTargetImpl
    (runtime : UInt64) (handler : @& RawAdvancedHandlerCall) : IO UInt32

@[extern "capnp_lean_rpc_runtime_register_tailcall_handler_target"]
opaque ffiRuntimeRegisterTailCallHandlerTargetImpl
    (runtime : UInt64) (handler : @& RawTailCallHandlerCall) : IO UInt32

@[extern "capnp_lean_rpc_runtime_register_loopback_target"]
opaque ffiRuntimeRegisterLoopbackTargetImpl (runtime : UInt64) (bootstrap : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_register_tailcall_target"]
opaque ffiRuntimeRegisterTailCallTargetImpl (runtime : UInt64) (target : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_register_fd_target"]
opaque ffiRuntimeRegisterFdTargetImpl (runtime : UInt64) (fd : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_release_target"]
opaque ffiRuntimeReleaseTargetImpl (runtime : UInt64) (target : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_release_target_deferred"]
opaque ffiRuntimeReleaseTargetDeferredImpl (runtime : UInt64) (target : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_release_targets"]
opaque ffiRuntimeReleaseTargetsImpl (runtime : UInt64) (targets : @& ByteArray) : IO Unit

@[extern "capnp_lean_rpc_runtime_retain_target"]
opaque ffiRuntimeRetainTargetImpl (runtime : UInt64) (target : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_new_promise_capability"]
opaque ffiRuntimeNewPromiseCapabilityImpl (runtime : UInt64) : IO (UInt32 × UInt32)

@[extern "capnp_lean_rpc_runtime_promise_capability_fulfill"]
opaque ffiRuntimePromiseCapabilityFulfillImpl (runtime : UInt64) (fulfiller : UInt32)
    (target : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_promise_capability_reject"]
opaque ffiRuntimePromiseCapabilityRejectImpl (runtime : UInt64) (fulfiller : UInt32) (typeTag : UInt8)
    (message : @& String) (detail : @& ByteArray) : IO Unit

@[extern "capnp_lean_rpc_runtime_promise_capability_release"]
opaque ffiRuntimePromiseCapabilityReleaseImpl (runtime : UInt64) (fulfiller : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_connect"]
opaque ffiRuntimeConnectImpl (runtime : UInt64) (address : @& String) (portHint : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_connect_start"]
opaque ffiRuntimeConnectStartImpl
    (runtime : UInt64) (address : @& String) (portHint : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_connect_fd"]
opaque ffiRuntimeConnectFdImpl (runtime : UInt64) (fd : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_new_transport_pipe"]
opaque ffiRuntimeNewTransportPipeImpl (runtime : UInt64) : IO (UInt32 × UInt32)

@[extern "capnp_lean_rpc_runtime_new_transport_from_fd"]
opaque ffiRuntimeNewTransportFromFdImpl (runtime : UInt64) (fd : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_new_transport_from_fd_take"]
opaque ffiRuntimeNewTransportFromFdTakeImpl (runtime : UInt64) (fd : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_release_transport"]
opaque ffiRuntimeReleaseTransportImpl (runtime : UInt64) (transport : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_transport_get_fd"]
opaque ffiRuntimeTransportGetFdImpl (runtime : UInt64) (transport : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_connect_transport"]
opaque ffiRuntimeConnectTransportImpl (runtime : UInt64) (transport : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_listen_echo"]
opaque ffiRuntimeListenEchoImpl (runtime : UInt64) (address : @& String) (portHint : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_accept_echo"]
opaque ffiRuntimeAcceptEchoImpl (runtime : UInt64) (listener : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_release_listener"]
opaque ffiRuntimeReleaseListenerImpl (runtime : UInt64) (listener : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_new_client"]
opaque ffiRuntimeNewClientImpl
    (runtime : UInt64) (address : @& String) (portHint : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_new_client_start"]
opaque ffiRuntimeNewClientStartImpl
    (runtime : UInt64) (address : @& String) (portHint : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_release_client"]
opaque ffiRuntimeReleaseClientImpl (runtime : UInt64) (client : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_client_bootstrap"]
opaque ffiRuntimeClientBootstrapImpl (runtime : UInt64) (client : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_client_on_disconnect"]
opaque ffiRuntimeClientOnDisconnectImpl (runtime : UInt64) (client : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_client_on_disconnect_start"]
opaque ffiRuntimeClientOnDisconnectStartImpl (runtime : UInt64) (client : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_client_set_flow_limit"]
opaque ffiRuntimeClientSetFlowLimitImpl
    (runtime : UInt64) (client : UInt32) (words : UInt64) : IO Unit

@[extern "capnp_lean_rpc_runtime_client_queue_size"]
opaque ffiRuntimeClientQueueSizeImpl (runtime : UInt64) (client : UInt32) : IO UInt64

@[extern "capnp_lean_rpc_runtime_client_queue_count"]
opaque ffiRuntimeClientQueueCountImpl (runtime : UInt64) (client : UInt32) : IO UInt64

@[extern "capnp_lean_rpc_runtime_client_outgoing_wait_nanos"]
opaque ffiRuntimeClientOutgoingWaitNanosImpl (runtime : UInt64) (client : UInt32) : IO UInt64

@[extern "capnp_lean_rpc_runtime_target_count"]
opaque ffiRuntimeTargetCountImpl (runtime : UInt64) : IO UInt64

@[extern "capnp_lean_rpc_runtime_listener_count"]
opaque ffiRuntimeListenerCountImpl (runtime : UInt64) : IO UInt64

@[extern "capnp_lean_rpc_runtime_client_count"]
opaque ffiRuntimeClientCountImpl (runtime : UInt64) : IO UInt64

@[extern "capnp_lean_rpc_runtime_server_count"]
opaque ffiRuntimeServerCountImpl (runtime : UInt64) : IO UInt64

@[extern "capnp_lean_rpc_runtime_pending_call_count"]
opaque ffiRuntimePendingCallCountImpl (runtime : UInt64) : IO UInt64

@[extern "capnp_lean_rpc_runtime_new_server"]
opaque ffiRuntimeNewServerImpl (runtime : UInt64) (bootstrap : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_new_server_with_bootstrap_factory"]
opaque ffiRuntimeNewServerWithBootstrapFactoryImpl
    (runtime : UInt64) (bootstrapFactory : @& RawBootstrapFactoryCall) : IO UInt32

@[extern "capnp_lean_rpc_runtime_release_server"]
opaque ffiRuntimeReleaseServerImpl (runtime : UInt64) (server : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_server_listen"]
opaque ffiRuntimeServerListenImpl
    (runtime : UInt64) (server : UInt32) (address : @& String) (portHint : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_server_accept"]
opaque ffiRuntimeServerAcceptImpl (runtime : UInt64) (server : UInt32) (listener : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_server_accept_start"]
opaque ffiRuntimeServerAcceptStartImpl
    (runtime : UInt64) (server : UInt32) (listener : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_server_accept_fd"]
opaque ffiRuntimeServerAcceptFdImpl
    (runtime : UInt64) (server : UInt32) (fd : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_server_accept_transport"]
opaque ffiRuntimeServerAcceptTransportImpl
    (runtime : UInt64) (server : UInt32) (transport : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_server_drain"]
opaque ffiRuntimeServerDrainImpl (runtime : UInt64) (server : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_server_drain_start"]
opaque ffiRuntimeServerDrainStartImpl (runtime : UInt64) (server : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_multivat_new_client"]
opaque ffiRuntimeMultiVatNewClientImpl
    (runtime : UInt64) (name : @& String) : IO UInt32

@[extern "capnp_lean_rpc_runtime_multivat_new_server"]
opaque ffiRuntimeMultiVatNewServerImpl
    (runtime : UInt64) (name : @& String) (bootstrap : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_multivat_new_server_with_bootstrap_factory"]
opaque ffiRuntimeMultiVatNewServerWithBootstrapFactoryImpl
    (runtime : UInt64) (name : @& String) (bootstrapFactory : @& RawVatBootstrapFactoryCall) :
    IO UInt32

@[extern "capnp_lean_rpc_runtime_multivat_release_peer"]
opaque ffiRuntimeMultiVatReleasePeerImpl
    (runtime : UInt64) (peer : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_multivat_bootstrap"]
opaque ffiRuntimeMultiVatBootstrapImpl
    (runtime : UInt64) (sourcePeer : UInt32) (host : @& String) (unique : UInt8) : IO UInt32

@[extern "capnp_lean_rpc_runtime_multivat_bootstrap_peer"]
opaque ffiRuntimeMultiVatBootstrapPeerImpl
    (runtime : UInt64) (sourcePeer : UInt32) (targetPeer : UInt32) (unique : UInt8) : IO UInt32

@[extern "capnp_lean_rpc_runtime_multivat_set_forwarding_enabled"]
opaque ffiRuntimeMultiVatSetForwardingEnabledImpl
    (runtime : UInt64) (enabled : UInt8) : IO Unit

@[extern "capnp_lean_rpc_runtime_multivat_reset_forwarding_stats"]
opaque ffiRuntimeMultiVatResetForwardingStatsImpl (runtime : UInt64) : IO Unit

@[extern "capnp_lean_rpc_runtime_multivat_forward_count"]
opaque ffiRuntimeMultiVatForwardCountImpl (runtime : UInt64) : IO UInt64

@[extern "capnp_lean_rpc_runtime_multivat_third_party_token_count"]
opaque ffiRuntimeMultiVatThirdPartyTokenCountImpl (runtime : UInt64) : IO UInt64

@[extern "capnp_lean_rpc_runtime_multivat_denied_forward_count"]
opaque ffiRuntimeMultiVatDeniedForwardCountImpl (runtime : UInt64) : IO UInt64

@[extern "capnp_lean_rpc_runtime_multivat_has_connection"]
opaque ffiRuntimeMultiVatHasConnectionImpl
    (runtime : UInt64) (fromPeer : UInt32) (toPeer : UInt32) : IO Bool

@[extern "capnp_lean_rpc_runtime_multivat_set_restorer"]
opaque ffiRuntimeMultiVatSetRestorerImpl
    (runtime : UInt64) (peer : UInt32) (restorer : @& RawVatRestorerCall) : IO Unit

@[extern "capnp_lean_rpc_runtime_multivat_clear_restorer"]
opaque ffiRuntimeMultiVatClearRestorerImpl
    (runtime : UInt64) (peer : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_multivat_publish_sturdy_ref"]
opaque ffiRuntimeMultiVatPublishSturdyRefImpl
    (runtime : UInt64) (hostPeer : UInt32) (objectId : @& ByteArray) (target : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_multivat_publish_sturdy_ref_start"]
opaque ffiRuntimeMultiVatPublishSturdyRefStartImpl
    (runtime : UInt64) (hostPeer : UInt32) (objectId : @& ByteArray) (target : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_multivat_unpublish_sturdy_ref"]
opaque ffiRuntimeMultiVatUnpublishSturdyRefImpl
    (runtime : UInt64) (hostPeer : UInt32) (objectId : @& ByteArray) : IO Unit

@[extern "capnp_lean_rpc_runtime_multivat_unpublish_sturdy_ref_start"]
opaque ffiRuntimeMultiVatUnpublishSturdyRefStartImpl
    (runtime : UInt64) (hostPeer : UInt32) (objectId : @& ByteArray) : IO UInt32

@[extern "capnp_lean_rpc_runtime_multivat_clear_published_sturdy_refs"]
opaque ffiRuntimeMultiVatClearPublishedSturdyRefsImpl
    (runtime : UInt64) (hostPeer : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_multivat_clear_published_sturdy_refs_start"]
opaque ffiRuntimeMultiVatClearPublishedSturdyRefsStartImpl
    (runtime : UInt64) (hostPeer : UInt32) : IO UInt32

@[extern "capnp_lean_rpc_runtime_multivat_published_sturdy_ref_count"]
opaque ffiRuntimeMultiVatPublishedSturdyRefCountImpl
    (runtime : UInt64) (hostPeer : UInt32) : IO UInt64

@[extern "capnp_lean_rpc_runtime_multivat_restore_sturdy_ref"]
opaque ffiRuntimeMultiVatRestoreSturdyRefImpl
    (runtime : UInt64) (sourcePeer : UInt32) (host : @& String) (unique : UInt8)
    (objectId : @& ByteArray) : IO UInt32

@[extern "capnp_lean_rpc_runtime_multivat_restore_sturdy_ref_start"]
opaque ffiRuntimeMultiVatRestoreSturdyRefStartImpl
    (runtime : UInt64) (sourcePeer : UInt32) (host : @& String) (unique : UInt8)
    (objectId : @& ByteArray) : IO UInt32

@[extern "capnp_lean_rpc_runtime_multivat_connection_block"]
opaque ffiRuntimeMultiVatConnectionBlockImpl
    (runtime : UInt64) (fromPeer : UInt32) (toPeer : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_multivat_connection_unblock"]
opaque ffiRuntimeMultiVatConnectionUnblockImpl
    (runtime : UInt64) (fromPeer : UInt32) (toPeer : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_multivat_connection_disconnect"]
opaque ffiRuntimeMultiVatConnectionDisconnectImpl
    (runtime : UInt64) (fromPeer : UInt32) (toPeer : UInt32) (exceptionTypeTag : UInt8)
    (message : @& String) (detailBytes : @& ByteArray) : IO Unit

@[extern "capnp_lean_rpc_runtime_multivat_connection_resolve_disembargo_counts"]
opaque ffiRuntimeMultiVatConnectionResolveDisembargoCountsImpl
    (runtime : UInt64) (fromPeer : UInt32) (toPeer : UInt32) : IO ProtocolMessageCounts

@[extern "capnp_lean_rpc_runtime_multivat_connection_resolve_disembargo_trace"]
opaque ffiRuntimeMultiVatConnectionResolveDisembargoTraceImpl
    (runtime : UInt64) (fromPeer : UInt32) (toPeer : UInt32) : IO ByteArray

@[extern "capnp_lean_rpc_runtime_multivat_connection_reset_resolve_disembargo_trace"]
opaque ffiRuntimeMultiVatConnectionResetResolveDisembargoTraceImpl
    (runtime : UInt64) (fromPeer : UInt32) (toPeer : UInt32) : IO Unit

@[extern "capnp_lean_rpc_runtime_multivat_get_diagnostics"]
opaque ffiRuntimeMultiVatGetDiagnosticsImpl
    (runtime : UInt64) (peerId : UInt32) (targetVatId : @& VatId) : IO RpcDiagnostics

@[extern "capnp_lean_rpc_cpp_call_one_shot"]
opaque ffiCppCallOneShotImpl
    (address : @& String) (portHint : UInt32) (interfaceId : UInt64) (methodId : UInt16)
    (request : @& ByteArray) (requestCaps : @& ByteArray) : IO (ByteArray × ByteArray)

@[extern "capnp_lean_rpc_runtime_cpp_call_with_accept"]
opaque ffiRuntimeCppCallWithAcceptImpl
    (runtime : UInt64) (server : UInt32) (listener : UInt32)
    (address : @& String) (portHint : UInt32) (interfaceId : UInt64) (methodId : UInt16)
    (request : @& ByteArray) (requestCaps : @& ByteArray) : IO (ByteArray × ByteArray)

@[extern "capnp_lean_rpc_runtime_cpp_call_pipelined_with_accept"]
opaque ffiRuntimeCppCallPipelinedWithAcceptImpl
    (runtime : UInt64) (server : UInt32) (listener : UInt32)
    (address : @& String) (portHint : UInt32) (interfaceId : UInt64) (methodId : UInt16)
    (request : @& ByteArray) (requestCaps : @& ByteArray)
    (pipelinedRequest : @& ByteArray) (pipelinedRequestCaps : @& ByteArray) :
    IO (ByteArray × ByteArray)

@[extern "capnp_lean_rpc_cpp_serve_echo_once"]
opaque ffiCppServeEchoOnceImpl
    (address : @& String) (portHint : UInt32) (interfaceId : UInt64) (methodId : UInt16) :
    IO (ByteArray × ByteArray)

@[extern "capnp_lean_rpc_cpp_serve_throw_once"]
opaque ffiCppServeThrowOnceImpl
    (address : @& String) (portHint : UInt32) (interfaceId : UInt64) (methodId : UInt16)
    (withDetail : UInt8) : IO (ByteArray × ByteArray)

@[extern "capnp_lean_rpc_cpp_serve_delayed_echo_once"]
opaque ffiCppServeDelayedEchoOnceImpl
    (address : @& String) (portHint : UInt32) (interfaceId : UInt64) (methodId : UInt16)
    (delayMillis : UInt32) : IO (ByteArray × ByteArray)

@[extern "capnp_lean_rpc_cpp_call_pipelined_cap_one_shot"]
opaque ffiCppCallPipelinedCapOneShotImpl
    (address : @& String) (portHint : UInt32) (interfaceId : UInt64) (methodId : UInt16)
    (request : @& ByteArray) (requestCaps : @& ByteArray)
    (pipelinedRequest : @& ByteArray) (pipelinedRequestCaps : @& ByteArray) :
    IO (ByteArray × ByteArray)

structure Runtime where
  handle : UInt64
  deriving Inhabited, BEq, Repr

structure RuntimeClientRef where
  runtime : Runtime
  handle : RuntimeClient
  deriving Inhabited, BEq, Repr

structure RuntimeServerRef where
  runtime : Runtime
  handle : RuntimeServer
  deriving Inhabited, BEq, Repr

structure RuntimeVatPeerRef where
  runtime : Runtime
  handle : RuntimeVatPeer
  deriving Inhabited, BEq, Repr

structure VatNetwork where
  runtime : Runtime
  deriving Inhabited, BEq, Repr

structure RuntimePendingCallRef where
  runtime : Runtime
  handle : RuntimePendingCall
  deriving Inhabited, BEq, Repr

structure RuntimePayloadRef where
  runtime : Runtime
  handle : UInt32
  deriving Inhabited, BEq, Repr

structure Promise (α : Type) where
  pendingCall : RuntimePendingCallRef
  deriving Inhabited, BEq, Repr

structure TypedPayload (α : Type) where
  reader : α
  capTable : Capnp.CapTable
  deriving Inhabited, BEq

namespace TypedPayload

@[inline] def toPair (payload : TypedPayload α) : α × Capnp.CapTable :=
  (payload.reader, payload.capTable)

@[inline] def mapReader (f : α -> β) (payload : TypedPayload α) : TypedPayload β :=
  { reader := f payload.reader, capTable := payload.capTable }

end TypedPayload

structure RuntimePromiseRef (α : Type) where
  runtime : Runtime
  handle : UInt32
  deriving Inhabited, BEq, Repr

abbrev RuntimeRegisterPromiseRef := RuntimePromiseRef UInt32
abbrev RuntimeUnitPromiseRef := RuntimePromiseRef Unit

structure RuntimePromiseCapabilityFulfillerRef where
  runtime : Runtime
  handle : UInt32
  deriving Inhabited, BEq, Repr

@[inline] private def ensureSameRuntimeHandle
    (runtime : Runtime) (ownerHandle : UInt64) (resource : String) : IO Unit := do
  if runtime.handle != ownerHandle then
    throw (IO.userError s!"{resource} belongs to a different Capnp.Rpc runtime")

@[inline] private def ensureSameRuntime
    (runtime : Runtime) (owner : Runtime) (resource : String) : IO Unit :=
  ensureSameRuntimeHandle runtime owner.handle resource

namespace PipelinePath

@[inline] def toBytes (ops : Array UInt16) : ByteArray := Id.run do
  let mut out := ByteArray.emptyWithCapacity (ops.size * 2)
  for op in ops do
    out := out.push op.toUInt8
    out := out.push (op >>> 8).toUInt8
  return out

@[inline] def ofBytes (bytes : ByteArray) : Array UInt16 := Id.run do
  let mut out : Array UInt16 := Array.emptyWithCapacity (bytes.size / 2)
  let mut i := 0
  while i + 2 ≤ bytes.size do
    let lo := bytes.get! i |>.toUInt16
    let hi := bytes.get! (i + 1) |>.toUInt16
    out := out.push (lo + (hi <<< 8))
    i := i + 2
  return out

@[inline] def ofBytesChecked (bytes : ByteArray) : Except String (Array UInt16) :=
  if bytes.size % 2 != 0 then
    Except.error "pipeline op payload must be a multiple of 2 bytes"
  else
    Except.ok (ofBytes bytes)

end PipelinePath

namespace CapTable

@[inline] def toBytes (t : Capnp.CapTable) : ByteArray := Id.run do
  let mut out := ByteArray.emptyWithCapacity (t.caps.size * 4)
  for cap in t.caps do
    out := Capnp.appendUInt32LE out cap
  return out

@[inline] def ofBytes (bytes : ByteArray) : Capnp.CapTable := Id.run do
  let mut caps : Array Capnp.Capability := Array.emptyWithCapacity (bytes.size / 4)
  let mut i := 0
  while i + 4 ≤ bytes.size do
    caps := caps.push (Capnp.readUInt32LE bytes i)
    i := i + 4
  return { caps := caps }

@[inline] def ofBytesChecked (bytes : ByteArray) : Except String Capnp.CapTable :=
  if bytes.size % 4 != 0 then
    Except.error "capability table payload must be a multiple of 4 bytes"
  else
    Except.ok (CapTable.ofBytes bytes)

end CapTable

namespace Payload

@[inline] def capTableBytes (payload : Payload) : ByteArray :=
  CapTable.toBytes payload.capTable

end Payload

@[inline] private def expectChecked (what : String) (value : Except String α) : IO α :=
  match value with
  | Except.ok v => pure v
  | Except.error e => throw (IO.userError s!"{what}: {e}")

@[inline] private def decodePayloadChecked (msgBytes capBytes : ByteArray) : IO Payload := do
  let opts : Capnp.ReaderOptions := {}
  let msg ← expectChecked "invalid RPC message" (Capnp.readMessageChecked opts msgBytes)
  let capTable ← expectChecked "invalid RPC capability table" (CapTable.ofBytesChecked capBytes)
  return { msg := msg, capTable := capTable }

@[inline] private def toRawHandlerCall
    (handler : Client -> Method -> Payload -> IO Payload) : RawHandlerCall :=
  fun target interfaceId methodId requestBytes requestCaps => do
    let request ← decodePayloadChecked requestBytes requestCaps
    let response ← handler target { interfaceId := interfaceId, methodId := methodId } request
    return (response.toBytes, response.capTableBytes)

@[inline] private def toRawTailCallHandlerCall
    (handler : Client -> Method -> Payload -> IO Client) : RawTailCallHandlerCall :=
  fun target interfaceId methodId requestBytes requestCaps => do
    let request ← decodePayloadChecked requestBytes requestCaps
    handler target { interfaceId := interfaceId, methodId := methodId } request

@[inline] private def toRawAdvancedHandlerResult
    (result : AdvancedHandlerResult) : RawAdvancedHandlerResult :=
  match result with
  | .respond response =>
      .returnPayload response.toBytes response.capTableBytes
  | .asyncCall nextTarget nextMethod nextPayload =>
      .asyncCall nextTarget nextMethod.interfaceId nextMethod.methodId
        nextPayload.toBytes nextPayload.capTableBytes
  | .forwardCall nextTarget nextMethod nextPayload options =>
      let base : RawAdvancedHandlerResult := .asyncCall
        nextTarget nextMethod.interfaceId nextMethod.methodId
        nextPayload.toBytes nextPayload.capTableBytes
      let withHints : RawAdvancedHandlerResult :=
        .callHints options.callHints.noPromisePipelining options.callHints.onlyPromisePipeline base
      let withSend : RawAdvancedHandlerResult :=
        match options.sendResultsTo with
        | .yourself => withHints
        | .caller => .sendResultsToCaller withHints
      withSend
  | .tailCall nextTarget nextMethod nextPayload =>
      .tailCall nextTarget nextMethod.interfaceId nextMethod.methodId
        nextPayload.toBytes nextPayload.capTableBytes
  | .throwRemote message detail =>
      .throwRemote message detail
  | .throwRemoteWithType type message detail =>
      .exceptionType type.toUInt8 (.throwRemote message detail)
  | .control opts next =>
      .control opts.releaseParams opts.allowCancellation opts.isStreaming
        (toRawAdvancedHandlerResult next)

@[inline] private def mapDeferredAdvancedHandlerTask
    (task : Task (Except IO.Error AdvancedHandlerResult)) :
    Task (Except IO.Error RawAdvancedHandlerResult) :=
  Task.map (fun result => result.map toRawAdvancedHandlerResult) task

@[inline] private def toRawAdvancedHandlerReply
    (reply : AdvancedHandlerReply) : RawAdvancedHandlerResult :=
  match reply with
  | .now result =>
      toRawAdvancedHandlerResult result
  | .deferred task =>
      .awaitTask (mapDeferredAdvancedHandlerTask task) task
  | .deferredWithCancel task cancelTask =>
      .awaitTask (mapDeferredAdvancedHandlerTask task) cancelTask
  | .control opts next =>
      .control opts.releaseParams opts.allowCancellation opts.isStreaming
        (toRawAdvancedHandlerReply next)
  | .pipeline pipeline next =>
      .setPipeline pipeline.toBytes pipeline.capTableBytes
        (toRawAdvancedHandlerReply next)

@[inline] private def toRawAdvancedHandlerCallAsync
    (handler : Client -> Method -> Payload -> IO AdvancedHandlerReply) : RawAdvancedHandlerCall :=
  fun target interfaceId methodId requestBytes requestCaps => do
    let request ← decodePayloadChecked requestBytes requestCaps
    let method : Method := { interfaceId := interfaceId, methodId := methodId }
    pure (toRawAdvancedHandlerReply (← handler target method request))

namespace Runtime

@[inline] def init : IO Runtime := do
  return { handle := (← ffiRuntimeNewImpl) }

@[inline] def initWithFdLimit (maxFdsPerMessage : UInt32) : IO Runtime := do
  return { handle := (← ffiRuntimeNewWithFdLimitImpl maxFdsPerMessage) }

@[inline] def shutdown (runtime : Runtime) : IO Unit :=
  ffiRuntimeReleaseImpl runtime.handle

@[inline] def isAlive (runtime : Runtime) : IO Bool :=
  ffiRuntimeIsAliveImpl runtime.handle

@[inline] def registerEchoTarget (runtime : Runtime) : IO Client :=
  ffiRuntimeRegisterEchoTargetImpl runtime.handle

@[inline] def registerLoopbackTarget (runtime : Runtime) (bootstrap : Client) : IO Client :=
  ffiRuntimeRegisterLoopbackTargetImpl runtime.handle bootstrap

@[inline] def registerHandlerTarget (runtime : Runtime)
    (handler : Client -> Method -> Payload -> IO Payload) : IO Client :=
  ffiRuntimeRegisterHandlerTargetImpl runtime.handle (toRawHandlerCall handler)

@[inline] def registerAdvancedHandlerTarget (runtime : Runtime)
    (handler : Client -> Method -> Payload -> IO AdvancedHandlerResult) : IO Client :=
  ffiRuntimeRegisterAdvancedHandlerTargetImpl runtime.handle
    (toRawAdvancedHandlerCallAsync fun target method payload => do
      pure (.now (← handler target method payload)))

@[inline] def registerAdvancedHandlerTargetAsync (runtime : Runtime)
    (handler : Client -> Method -> Payload -> IO AdvancedHandlerReply) : IO Client :=
  ffiRuntimeRegisterAdvancedHandlerTargetImpl runtime.handle
    (toRawAdvancedHandlerCallAsync handler)

@[inline] def registerTailCallHandlerTarget (runtime : Runtime)
    (handler : Client -> Method -> Payload -> IO Client) : IO Client :=
  ffiRuntimeRegisterTailCallHandlerTargetImpl runtime.handle (toRawTailCallHandlerCall handler)

@[inline] def registerStreamingHandlerTarget (runtime : Runtime)
    (handler : Client -> Method -> Payload -> IO Unit) : IO Client :=
  runtime.registerAdvancedHandlerTargetAsync (fun target method payload => do
    handler target method payload
    pure <| AdvancedHandlerReply.streaming <| .now (.respond Capnp.emptyRpcEnvelope))

@[inline] def registerStreamingHandlerTargetAsync (runtime : Runtime)
    (handler : Client -> Method -> Payload -> IO AdvancedHandlerReply) : IO Client :=
  runtime.registerAdvancedHandlerTargetAsync (fun target method payload => do
    return AdvancedHandlerReply.streaming (← handler target method payload))

@[inline] def registerTailCallTarget (runtime : Runtime) (target : Client) : IO Client :=
  ffiRuntimeRegisterTailCallTargetImpl runtime.handle target

@[inline] def registerFdTarget (runtime : Runtime) (fd : UInt32) : IO Client :=
  ffiRuntimeRegisterFdTargetImpl runtime.handle fd

@[inline] def releaseTarget (runtime : Runtime) (target : Client) : IO Unit :=
  ffiRuntimeReleaseTargetImpl runtime.handle target

@[inline] def releaseTargetDeferred (runtime : Runtime) (target : Client) : IO Unit :=
  ffiRuntimeReleaseTargetDeferredImpl runtime.handle target

@[inline] def retainTarget (runtime : Runtime) (target : Client) : IO Client :=
  ffiRuntimeRetainTargetImpl runtime.handle target

@[inline] def withTarget (runtime : Runtime) (target : Client)
    (action : Client -> IO α) : IO α := do
  try
    action target
  finally
    runtime.releaseTarget target

@[inline] def withRetainedTarget (runtime : Runtime) (target : Client)
    (action : Client -> IO α) : IO α := do
  let retained ← runtime.retainTarget target
  runtime.withTarget retained action

@[inline] def newPromiseCapability (runtime : Runtime) :
    IO (Client × RuntimePromiseCapabilityFulfillerRef) := do
  let (promiseTarget, fulfiller) ← ffiRuntimeNewPromiseCapabilityImpl runtime.handle
  return (
    promiseTarget,
    { runtime := runtime, handle := fulfiller }
  )

@[inline] def promiseCapabilityFulfill (fulfiller : RuntimePromiseCapabilityFulfillerRef)
    (target : Client) : IO Unit := do
  ffiRuntimePromiseCapabilityFulfillImpl fulfiller.runtime.handle fulfiller.handle target

@[inline] def promiseCapabilityReject (fulfiller : RuntimePromiseCapabilityFulfillerRef)
    (type : RemoteExceptionType) (message : String)
    (detail : ByteArray := ByteArray.empty) : IO Unit := do
  ffiRuntimePromiseCapabilityRejectImpl fulfiller.runtime.handle fulfiller.handle type.toUInt8
    message detail

@[inline] def promiseCapabilityRelease (fulfiller : RuntimePromiseCapabilityFulfillerRef) : IO Unit := do
  ffiRuntimePromiseCapabilityReleaseImpl fulfiller.runtime.handle fulfiller.handle

@[inline] def releaseCapTable (runtime : Runtime) (capTable : Capnp.CapTable) : IO Unit := do
  if capTable.caps.isEmpty then
    pure ()
  else
    ffiRuntimeReleaseTargetsImpl runtime.handle (CapTable.toBytes capTable)

@[inline] def withCapTable (runtime : Runtime) (capTable : Capnp.CapTable)
    (action : Capnp.CapTable -> IO α) : IO α := do
  try
    action capTable
  finally
    runtime.releaseCapTable capTable

@[inline] def connect (runtime : Runtime) (address : String) (portHint : UInt32 := 0) : IO Client :=
  ffiRuntimeConnectImpl runtime.handle address portHint

@[inline] def connectStart (runtime : Runtime) (address : String) (portHint : UInt32 := 0) :
    IO RuntimeRegisterPromiseRef := do
  return {
    runtime := runtime
    handle := (← ffiRuntimeConnectStartImpl runtime.handle address portHint)
  }

@[inline] def connectAsTask (runtime : Runtime) (address : String) (portHint : UInt32 := 0) :
    IO (Task (Except IO.Error Client)) := do
  let pending ← runtime.connectStart address portHint
  IO.asTask (ffiRuntimeRegisterPromiseAwaitImpl runtime.handle pending.handle)

@[inline] def connectAsPromise (runtime : Runtime) (address : String) (portHint : UInt32 := 0) :
    IO (Capnp.Async.Promise Client) := do
  pure (Capnp.Async.Promise.ofTask (← runtime.connectAsTask address portHint))

@[inline] def connectFd (runtime : Runtime) (fd : UInt32) : IO Client :=
  ffiRuntimeConnectFdImpl runtime.handle fd

@[inline] def newTransportPipe (runtime : Runtime) : IO (RuntimeTransport × RuntimeTransport) := do
  let (first, second) ← ffiRuntimeNewTransportPipeImpl runtime.handle
  return (
    { runtime := runtime.handle, handle := first },
    { runtime := runtime.handle, handle := second }
  )

@[inline] def newTransportFromFd (runtime : Runtime) (fd : UInt32) : IO RuntimeTransport :=
  return { runtime := runtime.handle, handle := (← ffiRuntimeNewTransportFromFdImpl runtime.handle fd) }

/-- Create a transport from `fd` and take ownership of it on the runtime side.

This differs from `newTransportFromFd`, which duplicates the fd. -/
@[inline] def newTransportFromFdTake (runtime : Runtime) (fd : UInt32) : IO RuntimeTransport :=
  return { runtime := runtime.handle, handle := (← ffiRuntimeNewTransportFromFdTakeImpl runtime.handle fd) }

@[inline] def releaseTransport (runtime : Runtime) (transport : RuntimeTransport) : IO Unit := do
  ensureSameRuntimeHandle runtime transport.runtime "RuntimeTransport"
  ffiRuntimeReleaseTransportImpl runtime.handle transport.handle

@[inline] def transportGetFd? (runtime : Runtime) (transport : RuntimeTransport) :
    IO (Option UInt32) := do
  ensureSameRuntimeHandle runtime transport.runtime "RuntimeTransport"
  let noneSentinel : UInt32 := 0xFFFFFFFF
  let fd ← ffiRuntimeTransportGetFdImpl runtime.handle transport.handle
  if fd == noneSentinel then
    return none
  else
    return some fd

@[inline] def connectTransport (runtime : Runtime) (transport : RuntimeTransport) : IO Client := do
  ensureSameRuntimeHandle runtime transport.runtime "RuntimeTransport"
  ffiRuntimeConnectTransportImpl runtime.handle transport.handle

@[inline] def connectTransportFd (runtime : Runtime) (fd : UInt32) : IO Client := do
  let transport ← runtime.newTransportFromFd fd
  runtime.connectTransport transport

@[inline] def listenEcho (runtime : Runtime) (address : String) (portHint : UInt32 := 0) :
    IO Listener :=
  return { runtime := runtime.handle, handle := (← ffiRuntimeListenEchoImpl runtime.handle address portHint) }

@[inline] def acceptEcho (runtime : Runtime) (listener : Listener) : IO Unit := do
  ensureSameRuntimeHandle runtime listener.runtime "Listener"
  ffiRuntimeAcceptEchoImpl runtime.handle listener.handle

@[inline] def releaseListener (runtime : Runtime) (listener : Listener) : IO Unit := do
  ensureSameRuntimeHandle runtime listener.runtime "Listener"
  ffiRuntimeReleaseListenerImpl runtime.handle listener.handle

@[inline] def newClient (runtime : Runtime) (address : String) (portHint : UInt32 := 0) :
    IO RuntimeClientRef := do
  return {
    runtime := runtime
    handle := { handle := (← ffiRuntimeNewClientImpl runtime.handle address portHint) }
  }

@[inline] def newClientStart (runtime : Runtime) (address : String) (portHint : UInt32 := 0) :
    IO RuntimeRegisterPromiseRef := do
  return {
    runtime := runtime
    handle := (← ffiRuntimeNewClientStartImpl runtime.handle address portHint)
  }

@[inline] def newClientAsTask (runtime : Runtime) (address : String) (portHint : UInt32 := 0) :
    IO (Task (Except IO.Error RuntimeClientRef)) := do
  let pending ← runtime.newClientStart address portHint
  IO.asTask do
    return {
      runtime := runtime
      handle := { handle := (← ffiRuntimeRegisterPromiseAwaitImpl runtime.handle pending.handle) }
    }

@[inline] def newClientAsPromise (runtime : Runtime) (address : String) (portHint : UInt32 := 0) :
    IO (Capnp.Async.Promise RuntimeClientRef) := do
  pure (Capnp.Async.Promise.ofTask (← runtime.newClientAsTask address portHint))

@[inline] def newServer (runtime : Runtime) (bootstrap : Client) : IO RuntimeServerRef := do
  return { runtime := runtime, handle := { handle := (← ffiRuntimeNewServerImpl runtime.handle bootstrap) } }

@[inline] def newServerWithBootstrapFactory (runtime : Runtime)
    (bootstrapFactory : TwoPartyVatSide -> IO Client) : IO RuntimeServerRef := do
  let rawFactory : RawBootstrapFactoryCall := fun sideRaw =>
    bootstrapFactory (TwoPartyVatSide.ofUInt16 sideRaw)
  return {
    runtime := runtime
    handle := {
      handle := (← ffiRuntimeNewServerWithBootstrapFactoryImpl runtime.handle rawFactory)
    }
  }

@[inline] private def boolToUInt8 (b : Bool) : UInt8 :=
  if b then (1 : UInt8) else (0 : UInt8)

@[inline] def newMultiVatClient (runtime : Runtime) (name : String) : IO RuntimeVatPeerRef := do
  return {
    runtime := runtime
    handle := { handle := (← ffiRuntimeMultiVatNewClientImpl runtime.handle name) }
  }

@[inline] def newMultiVatServer (runtime : Runtime) (name : String) (bootstrap : Client) :
    IO RuntimeVatPeerRef := do
  return {
    runtime := runtime
    handle := { handle := (← ffiRuntimeMultiVatNewServerImpl runtime.handle name bootstrap) }
  }

@[inline] def newMultiVatServerWithBootstrapFactory (runtime : Runtime) (name : String)
    (bootstrapFactory : VatId -> IO Client) : IO RuntimeVatPeerRef := do
  let rawFactory : RawVatBootstrapFactoryCall := fun host unique =>
    bootstrapFactory { host := host, unique := unique }
  return {
    runtime := runtime
    handle := {
      handle := (← ffiRuntimeMultiVatNewServerWithBootstrapFactoryImpl
        runtime.handle name rawFactory)
    }
  }

@[inline] def releaseMultiVatPeer (peer : RuntimeVatPeerRef) : IO Unit :=
  ffiRuntimeMultiVatReleasePeerImpl peer.runtime.handle peer.handle.handle

@[inline] def multiVatBootstrap (peer : RuntimeVatPeerRef) (vatId : VatId) : IO Client :=
  ffiRuntimeMultiVatBootstrapImpl peer.runtime.handle peer.handle.handle vatId.host
    (boolToUInt8 vatId.unique)

@[inline] def multiVatBootstrapPeer (runtime : Runtime)
    (sourcePeer : RuntimeVatPeerRef) (targetPeer : RuntimeVatPeerRef)
    (unique : Bool := false) : IO Client := do
  ensureSameRuntime runtime sourcePeer.runtime "RuntimeVatPeerRef"
  ensureSameRuntime runtime targetPeer.runtime "RuntimeVatPeerRef"
  ffiRuntimeMultiVatBootstrapPeerImpl runtime.handle sourcePeer.handle.handle targetPeer.handle.handle
    (boolToUInt8 unique)

@[inline] def multiVatSetForwardingEnabled (runtime : Runtime) (enabled : Bool) : IO Unit :=
  ffiRuntimeMultiVatSetForwardingEnabledImpl runtime.handle (boolToUInt8 enabled)

@[inline] def multiVatResetForwardingStats (runtime : Runtime) : IO Unit :=
  ffiRuntimeMultiVatResetForwardingStatsImpl runtime.handle

@[inline] def multiVatForwardCount (runtime : Runtime) : IO UInt64 :=
  ffiRuntimeMultiVatForwardCountImpl runtime.handle

@[inline] def multiVatThirdPartyTokenCount (runtime : Runtime) : IO UInt64 :=
  ffiRuntimeMultiVatThirdPartyTokenCountImpl runtime.handle

@[inline] def multiVatDeniedForwardCount (runtime : Runtime) : IO UInt64 :=
  ffiRuntimeMultiVatDeniedForwardCountImpl runtime.handle

@[inline] def multiVatHasConnection (runtime : Runtime)
    (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef) : IO Bool := do
  ensureSameRuntime runtime fromPeer.runtime "RuntimeVatPeerRef"
  ensureSameRuntime runtime toPeer.runtime "RuntimeVatPeerRef"
  ffiRuntimeMultiVatHasConnectionImpl runtime.handle fromPeer.handle.handle toPeer.handle.handle

@[inline] def multiVatGetDiagnostics (runtime : Runtime)
    (peer : RuntimeVatPeerRef) (targetVatId : VatId) : IO RpcDiagnostics := do
  ensureSameRuntime runtime peer.runtime "RuntimeVatPeerRef"
  ffiRuntimeMultiVatGetDiagnosticsImpl runtime.handle peer.handle.handle targetVatId

@[inline] def multiVatConnectionBlock (runtime : Runtime)
    (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef) : IO Unit := do
  ensureSameRuntime runtime fromPeer.runtime "RuntimeVatPeerRef"
  ensureSameRuntime runtime toPeer.runtime "RuntimeVatPeerRef"
  ffiRuntimeMultiVatConnectionBlockImpl runtime.handle fromPeer.handle.handle toPeer.handle.handle

@[inline] def multiVatConnectionUnblock (runtime : Runtime)
    (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef) : IO Unit := do
  ensureSameRuntime runtime fromPeer.runtime "RuntimeVatPeerRef"
  ensureSameRuntime runtime toPeer.runtime "RuntimeVatPeerRef"
  ffiRuntimeMultiVatConnectionUnblockImpl runtime.handle fromPeer.handle.handle toPeer.handle.handle

@[inline] def multiVatConnectionDisconnect (runtime : Runtime)
    (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef)
    (type : RemoteExceptionType) (message : String) (detail : ByteArray := ByteArray.empty) : IO Unit := do
  ensureSameRuntime runtime fromPeer.runtime "RuntimeVatPeerRef"
  ensureSameRuntime runtime toPeer.runtime "RuntimeVatPeerRef"
  ffiRuntimeMultiVatConnectionDisconnectImpl runtime.handle fromPeer.handle.handle toPeer.handle.handle
    type.toUInt8 message detail

@[inline] def multiVatConnectionResolveDisembargoCounts (runtime : Runtime)
    (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef) : IO ProtocolMessageCounts := do
  ensureSameRuntime runtime fromPeer.runtime "RuntimeVatPeerRef"
  ensureSameRuntime runtime toPeer.runtime "RuntimeVatPeerRef"
  ffiRuntimeMultiVatConnectionResolveDisembargoCountsImpl
    runtime.handle fromPeer.handle.handle toPeer.handle.handle

@[inline] def multiVatConnectionResolveDisembargoTrace (runtime : Runtime)
    (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef) :
    IO (Array ProtocolMessageTraceTag) := do
  ensureSameRuntime runtime fromPeer.runtime "RuntimeVatPeerRef"
  ensureSameRuntime runtime toPeer.runtime "RuntimeVatPeerRef"
  let bytes ← ffiRuntimeMultiVatConnectionResolveDisembargoTraceImpl
    runtime.handle fromPeer.handle.handle toPeer.handle.handle
  return (PipelinePath.ofBytes bytes).map ProtocolMessageTraceTag.ofUInt16

@[inline] def multiVatConnectionResetResolveDisembargoTrace (runtime : Runtime)
    (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef) : IO Unit := do
  ensureSameRuntime runtime fromPeer.runtime "RuntimeVatPeerRef"
  ensureSameRuntime runtime toPeer.runtime "RuntimeVatPeerRef"
  ffiRuntimeMultiVatConnectionResetResolveDisembargoTraceImpl
    runtime.handle fromPeer.handle.handle toPeer.handle.handle

@[inline] def multiVatSetRestorer (peer : RuntimeVatPeerRef)
    (restorer : VatId -> ByteArray -> IO Client) : IO Unit :=
  ffiRuntimeMultiVatSetRestorerImpl peer.runtime.handle peer.handle.handle
    (fun host unique objectId =>
      restorer { host := host, unique := unique } objectId)

@[inline] def multiVatClearRestorer (peer : RuntimeVatPeerRef) : IO Unit :=
  ffiRuntimeMultiVatClearRestorerImpl peer.runtime.handle peer.handle.handle

@[inline] def multiVatPublishSturdyRef (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client) : IO Unit :=
  ffiRuntimeMultiVatPublishSturdyRefImpl peer.runtime.handle peer.handle.handle objectId target

@[inline] def multiVatPublishSturdyRefStart (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client) : IO RuntimeUnitPromiseRef := do
  return {
    runtime := peer.runtime
    handle := (← ffiRuntimeMultiVatPublishSturdyRefStartImpl
      peer.runtime.handle peer.handle.handle objectId target)
  }

@[inline] def multiVatPublishSturdyRefAsTask (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client) : IO (Task (Except IO.Error Unit)) := do
  let pending ← multiVatPublishSturdyRefStart peer objectId target
  IO.asTask (ffiRuntimeUnitPromiseAwaitImpl peer.runtime.handle pending.handle)

@[inline] def multiVatPublishSturdyRefAsPromise (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client) : IO (Capnp.Async.Promise Unit) := do
  pure (Capnp.Async.Promise.ofTask (← multiVatPublishSturdyRefAsTask peer objectId target))

@[inline] def multiVatUnpublishSturdyRef (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) : IO Unit :=
  ffiRuntimeMultiVatUnpublishSturdyRefImpl peer.runtime.handle peer.handle.handle objectId

@[inline] def multiVatUnpublishSturdyRefStart (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) : IO RuntimeUnitPromiseRef := do
  return {
    runtime := peer.runtime
    handle := (← ffiRuntimeMultiVatUnpublishSturdyRefStartImpl
      peer.runtime.handle peer.handle.handle objectId)
  }

@[inline] def multiVatUnpublishSturdyRefAsTask (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) : IO (Task (Except IO.Error Unit)) := do
  let pending ← multiVatUnpublishSturdyRefStart peer objectId
  IO.asTask (ffiRuntimeUnitPromiseAwaitImpl peer.runtime.handle pending.handle)

@[inline] def multiVatUnpublishSturdyRefAsPromise (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) : IO (Capnp.Async.Promise Unit) := do
  pure (Capnp.Async.Promise.ofTask (← multiVatUnpublishSturdyRefAsTask peer objectId))

@[inline] def multiVatClearPublishedSturdyRefs (peer : RuntimeVatPeerRef) : IO Unit :=
  ffiRuntimeMultiVatClearPublishedSturdyRefsImpl peer.runtime.handle peer.handle.handle

@[inline] def multiVatClearPublishedSturdyRefsStart (peer : RuntimeVatPeerRef) :
    IO RuntimeUnitPromiseRef := do
  return {
    runtime := peer.runtime
    handle := (← ffiRuntimeMultiVatClearPublishedSturdyRefsStartImpl
      peer.runtime.handle peer.handle.handle)
  }

@[inline] def multiVatClearPublishedSturdyRefsAsTask (peer : RuntimeVatPeerRef) :
    IO (Task (Except IO.Error Unit)) := do
  let pending ← multiVatClearPublishedSturdyRefsStart peer
  IO.asTask (ffiRuntimeUnitPromiseAwaitImpl peer.runtime.handle pending.handle)

@[inline] def multiVatClearPublishedSturdyRefsAsPromise (peer : RuntimeVatPeerRef) :
    IO (Capnp.Async.Promise Unit) := do
  pure (Capnp.Async.Promise.ofTask (← multiVatClearPublishedSturdyRefsAsTask peer))

@[inline] def multiVatPublishedSturdyRefCount (peer : RuntimeVatPeerRef) : IO UInt64 :=
  ffiRuntimeMultiVatPublishedSturdyRefCountImpl peer.runtime.handle peer.handle.handle

@[inline] def multiVatRestoreSturdyRef (peer : RuntimeVatPeerRef)
    (sturdyRef : SturdyRef) : IO Client :=
  ffiRuntimeMultiVatRestoreSturdyRefImpl peer.runtime.handle peer.handle.handle
    sturdyRef.vat.host (boolToUInt8 sturdyRef.vat.unique) sturdyRef.objectId

@[inline] def multiVatRestoreSturdyRefStart (peer : RuntimeVatPeerRef)
    (sturdyRef : SturdyRef) : IO RuntimeRegisterPromiseRef := do
  return {
    runtime := peer.runtime
    handle := (← ffiRuntimeMultiVatRestoreSturdyRefStartImpl peer.runtime.handle peer.handle.handle
      sturdyRef.vat.host (boolToUInt8 sturdyRef.vat.unique) sturdyRef.objectId)
  }

@[inline] def multiVatRestoreSturdyRefAsTask (peer : RuntimeVatPeerRef)
    (sturdyRef : SturdyRef) : IO (Task (Except IO.Error Client)) := do
  let pending ← multiVatRestoreSturdyRefStart peer sturdyRef
  IO.asTask (ffiRuntimeRegisterPromiseAwaitImpl peer.runtime.handle pending.handle)

@[inline] def multiVatRestoreSturdyRefAsPromise (peer : RuntimeVatPeerRef)
    (sturdyRef : SturdyRef) : IO (Capnp.Async.Promise Client) := do
  pure (Capnp.Async.Promise.ofTask (← multiVatRestoreSturdyRefAsTask peer sturdyRef))

@[inline] def multiVatStats (runtime : Runtime) : IO MultiVatStats := do
  return {
    forwardCount := (← runtime.multiVatForwardCount)
    deniedForwardCount := (← runtime.multiVatDeniedForwardCount)
    thirdPartyTokenCount := (← runtime.multiVatThirdPartyTokenCount)
  }

@[inline] def vatNetwork (runtime : Runtime) : VatNetwork :=
  { runtime := runtime }

@[inline] def withClient (runtime : Runtime) (address : String)
    (action : RuntimeClientRef -> IO α) (portHint : UInt32 := 0) : IO α := do
  let client ← runtime.newClient address portHint
  try
    action client
  finally
    ffiRuntimeReleaseClientImpl runtime.handle client.handle.handle

@[inline] def newBootstrapTarget (runtime : Runtime) (address : String)
    (portHint : UInt32 := 0) : IO (RuntimeClientRef × Client) := do
  let client ← runtime.newClient address portHint
  let target ← ffiRuntimeClientBootstrapImpl runtime.handle client.handle.handle
  pure (client, target)

@[inline] def withBootstrapClientTarget (runtime : Runtime) (address : String)
    (action : RuntimeClientRef -> Client -> IO α) (portHint : UInt32 := 0) : IO α := do
  runtime.withClient address (fun client => do
    let target ← ffiRuntimeClientBootstrapImpl runtime.handle client.handle.handle
    runtime.withTarget target (fun scopedTarget => action client scopedTarget)
  ) portHint

@[inline] def withBootstrapTarget (runtime : Runtime) (address : String)
    (action : Client -> IO α) (portHint : UInt32 := 0) : IO α := do
  runtime.withBootstrapClientTarget address (fun _ target => action target) portHint

@[inline] def withServer (runtime : Runtime) (bootstrap : Client)
    (action : RuntimeServerRef -> IO α) : IO α := do
  let server ← runtime.newServer bootstrap
  try
    action server
  finally
    ffiRuntimeReleaseServerImpl runtime.handle server.handle.handle

@[inline] def withServerWithBootstrapFactory (runtime : Runtime)
    (bootstrapFactory : TwoPartyVatSide -> IO Client)
    (action : RuntimeServerRef -> IO α) : IO α := do
  let server ← runtime.newServerWithBootstrapFactory bootstrapFactory
  try
    action server
  finally
    ffiRuntimeReleaseServerImpl runtime.handle server.handle.handle

@[inline] def rawCall (runtime : Runtime) : RawCall :=
  fun target method request =>
    ffiRawCallOnRuntimeImpl runtime.handle target method.interfaceId method.methodId request

@[inline] def backend (runtime : Runtime) : Backend where
  call := fun target method payload => do
    let requestBytes := payload.toBytes
    let requestCaps := payload.capTableBytes
    let (responseBytes, responseCaps) ← ffiRawCallWithCapsOnRuntimeImpl
      runtime.handle target method.interfaceId method.methodId requestBytes requestCaps
    decodePayloadChecked responseBytes responseCaps

@[inline] def payloadRefFromBytes (runtime : Runtime)
    (request : ByteArray) (requestCaps : ByteArray := ByteArray.empty) : IO RuntimePayloadRef := do
  return {
    runtime := runtime
    handle := (← ffiRuntimePayloadRefFromBytesImpl runtime.handle request requestCaps)
  }

@[inline] def payloadRefFromBytesCopy (runtime : Runtime)
    (request : ByteArray) (requestCaps : ByteArray := ByteArray.empty) : IO RuntimePayloadRef :=
  runtime.payloadRefFromBytes request requestCaps

@[inline] def payloadRefFromPayload (runtime : Runtime)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO RuntimePayloadRef := do
  runtime.payloadRefFromBytes payload.toBytes payload.capTableBytes

@[inline] def payloadRefToBytes (payloadRef : RuntimePayloadRef) : IO (ByteArray × ByteArray) :=
  ffiRuntimePayloadRefToBytesImpl payloadRef.runtime.handle payloadRef.handle

@[inline] def payloadRefToBytesCopy (payloadRef : RuntimePayloadRef) : IO (ByteArray × ByteArray) :=
  payloadRefToBytes payloadRef

@[inline] def payloadRefDecode (payloadRef : RuntimePayloadRef) : IO Payload := do
  let (responseBytes, responseCaps) ← payloadRefToBytes payloadRef
  decodePayloadChecked responseBytes responseCaps

@[inline] def payloadRefRelease (payloadRef : RuntimePayloadRef) : IO Unit :=
  ffiRuntimePayloadRefReleaseImpl payloadRef.runtime.handle payloadRef.handle

@[inline] def withPayloadRef (runtime : Runtime) (payloadRef : RuntimePayloadRef)
    (action : RuntimePayloadRef -> IO α) : IO α := do
  ensureSameRuntime runtime payloadRef.runtime "RuntimePayloadRef"
  try
    action payloadRef
  finally
    Runtime.payloadRefRelease payloadRef

@[inline] def callWithPayloadRef (runtime : Runtime) (target : Client) (method : Method)
    (payloadRef : RuntimePayloadRef) : IO RuntimePayloadRef := do
  ensureSameRuntime runtime payloadRef.runtime "RuntimePayloadRef"
  return {
    runtime := runtime
    handle := (← ffiRuntimeCallWithPayloadRefImpl runtime.handle target
      method.interfaceId method.methodId payloadRef.handle)
  }

@[inline] def callPayloadRef (runtime : Runtime) (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO RuntimePayloadRef := do
  let requestRef ← runtime.payloadRefFromPayload payload
  try
    runtime.callWithPayloadRef target method requestRef
  finally
    Runtime.payloadRefRelease requestRef

@[inline] def withCallPayloadRef (runtime : Runtime) (target : Client) (method : Method)
    (action : RuntimePayloadRef -> IO α)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO α := do
  let responseRef ← runtime.callPayloadRef target method payload
  try
    action responseRef
  finally
    Runtime.payloadRefRelease responseRef

@[inline] def callPayloadRefDecode (runtime : Runtime) (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO Payload := do
  runtime.withCallPayloadRef target method Runtime.payloadRefDecode payload

@[inline] def callOutcome (runtime : Runtime) (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO RawCallOutcome := do
  let requestBytes := payload.toBytes
  let requestCaps := payload.capTableBytes
  ffiRawCallWithCapsOnRuntimeOutcomeImpl runtime.handle target method.interfaceId method.methodId
    requestBytes requestCaps

@[inline] def callOutcomeCopy (runtime : Runtime) (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO RawCallOutcome :=
  runtime.callOutcome target method payload

@[inline] def callResult (runtime : Runtime) (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO (Except RemoteException Payload) := do
  match (← runtime.callOutcome target method payload) with
  | .ok responseBytes responseCaps =>
      return .ok (← decodePayloadChecked responseBytes responseCaps)
  | .error ex =>
      return .error ex

@[inline] def callResultCopy (runtime : Runtime) (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) :
    IO (Except RemoteException Payload) :=
  runtime.callResult target method payload

@[inline] def startCall (runtime : Runtime) (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO RuntimePendingCallRef := do
  let requestBytes := payload.toBytes
  let requestCaps := payload.capTableBytes
  return {
    runtime := runtime
    handle := {
      handle := (← ffiRuntimeStartCallWithCapsImpl runtime.handle target method.interfaceId
        method.methodId requestBytes requestCaps)
    }
  }

@[inline] def startCallCopy (runtime : Runtime) (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO RuntimePendingCallRef :=
  runtime.startCall target method payload

@[inline] def startCallWithPayloadRef (runtime : Runtime) (target : Client) (method : Method)
    (payloadRef : RuntimePayloadRef) : IO RuntimePendingCallRef := do
  ensureSameRuntime runtime payloadRef.runtime "RuntimePayloadRef"
  return {
    runtime := runtime
    handle := {
      handle := (← ffiRuntimeStartCallWithPayloadRefImpl runtime.handle target
        method.interfaceId method.methodId payloadRef.handle)
    }
  }

@[inline] def startStreamingCall (runtime : Runtime) (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO RuntimePendingCallRef := do
  let requestBytes := payload.toBytes
  let requestCaps := payload.capTableBytes
  return {
    runtime := runtime
    handle := {
      handle := (← ffiRuntimeStartStreamingCallWithCapsImpl runtime.handle target method.interfaceId
        method.methodId requestBytes requestCaps)
    }
  }

@[inline] def startStreamingCallCopy (runtime : Runtime) (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO RuntimePendingCallRef :=
  runtime.startStreamingCall target method payload

@[inline] def startStreamingCallWithPayloadRef
    (runtime : Runtime) (target : Client) (method : Method)
    (payloadRef : RuntimePayloadRef) : IO RuntimePendingCallRef := do
  ensureSameRuntime runtime payloadRef.runtime "RuntimePayloadRef"
  return {
    runtime := runtime
    handle := {
      handle := (← ffiRuntimeStartStreamingCallWithPayloadRefImpl runtime.handle target
        method.interfaceId method.methodId payloadRef.handle)
    }
  }

@[inline] def startCallAsTask (runtime : Runtime) (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO (Task (Except IO.Error Payload)) := do
  let pending ← runtime.startCall target method payload
  IO.asTask do
    let (responseBytes, responseCaps) ←
      ffiRuntimePendingCallAwaitImpl runtime.handle pending.handle.handle
    decodePayloadChecked responseBytes responseCaps

@[inline] def startCallAsPromise (runtime : Runtime) (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO (Capnp.Async.Promise Payload) := do
  pure (Capnp.Async.Promise.ofTask (← runtime.startCallAsTask target method payload))

@[inline] def startCallWithPayloadRefAsTask (runtime : Runtime) (target : Client) (method : Method)
    (payloadRef : RuntimePayloadRef) : IO (Task (Except IO.Error RuntimePayloadRef)) := do
  let pending ← runtime.startCallWithPayloadRef target method payloadRef
  IO.asTask do
    return {
      runtime := runtime
      handle := (← ffiRuntimePendingCallAwaitPayloadRefImpl runtime.handle pending.handle.handle)
    }

@[inline] def startCallWithPayloadRefAsPromise
    (runtime : Runtime) (target : Client) (method : Method)
    (payloadRef : RuntimePayloadRef) : IO (Capnp.Async.Promise RuntimePayloadRef) := do
  pure (Capnp.Async.Promise.ofTask (← runtime.startCallWithPayloadRefAsTask target method payloadRef))

@[inline] def withStartedCall (runtime : Runtime) (target : Client) (method : Method)
    (action : RuntimePendingCallRef -> IO α)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO α := do
  let pending ← runtime.startCall target method payload
  try
    action pending
  finally
    ffiRuntimePendingCallReleaseImpl runtime.handle pending.handle.handle

@[inline] def startCallAwait (runtime : Runtime) (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO Payload := do
  let pending ← runtime.startCall target method payload
  let (responseBytes, responseCaps) ←
    ffiRuntimePendingCallAwaitImpl runtime.handle pending.handle.handle
  decodePayloadChecked responseBytes responseCaps

@[inline] def startCallAwaitPayloadRef (runtime : Runtime) (target : Client) (method : Method)
    (payloadRef : RuntimePayloadRef) : IO RuntimePayloadRef := do
  let pending ← runtime.startCallWithPayloadRef target method payloadRef
  return {
    runtime := runtime
    handle := (← ffiRuntimePendingCallAwaitPayloadRefImpl runtime.handle pending.handle.handle)
  }

@[inline] def startCallAwaitOutcome (runtime : Runtime) (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO RawCallOutcome := do
  let pending ← runtime.startCall target method payload
  ffiRuntimePendingCallAwaitOutcomeImpl runtime.handle pending.handle.handle

@[inline] def startCallAwaitResult (runtime : Runtime) (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO (Except RemoteException Payload) := do
  match (← runtime.startCallAwaitOutcome target method payload) with
  | .ok responseBytes responseCaps =>
      return .ok (← decodePayloadChecked responseBytes responseCaps)
  | .error ex =>
      return .error ex

@[inline] def pendingCallAwait (pendingCall : RuntimePendingCallRef) : IO Payload := do
  let (responseBytes, responseCaps) ←
    ffiRuntimePendingCallAwaitImpl pendingCall.runtime.handle pendingCall.handle.handle
  decodePayloadChecked responseBytes responseCaps

@[inline] def pendingCallAwaitPayloadRef (pendingCall : RuntimePendingCallRef) :
    IO RuntimePayloadRef := do
  return {
    runtime := pendingCall.runtime
    handle := (← ffiRuntimePendingCallAwaitPayloadRefImpl
      pendingCall.runtime.handle pendingCall.handle.handle)
  }

@[inline] def pendingCallAwaitOutcome (pendingCall : RuntimePendingCallRef) : IO RawCallOutcome :=
  ffiRuntimePendingCallAwaitOutcomeImpl pendingCall.runtime.handle pendingCall.handle.handle

@[inline] def pendingCallAwaitResult (pendingCall : RuntimePendingCallRef) :
    IO (Except RemoteException Payload) := do
  match (← pendingCallAwaitOutcome pendingCall) with
  | .ok responseBytes responseCaps =>
      return .ok (← decodePayloadChecked responseBytes responseCaps)
  | .error ex =>
      return .error ex

@[inline] def pendingCallRelease (pendingCall : RuntimePendingCallRef) : IO Unit :=
  ffiRuntimePendingCallReleaseImpl pendingCall.runtime.handle pendingCall.handle.handle

@[inline] def pendingCallReleaseDeferred (pendingCall : RuntimePendingCallRef) : IO Unit :=
  ffiRuntimePendingCallReleaseDeferredImpl pendingCall.runtime.handle pendingCall.handle.handle

@[inline] def pendingCallGetPipelinedCap (pendingCall : RuntimePendingCallRef)
    (pointerPath : Array UInt16 := #[]) : IO Client := do
  ffiRuntimePendingCallGetPipelinedCapImpl pendingCall.runtime.handle pendingCall.handle.handle
    (PipelinePath.toBytes pointerPath)

@[inline] def registerPromiseAwait (promise : RuntimeRegisterPromiseRef) : IO UInt32 :=
  ffiRuntimeRegisterPromiseAwaitImpl promise.runtime.handle promise.handle

@[inline] def registerPromiseCancel (promise : RuntimeRegisterPromiseRef) : IO Unit :=
  ffiRuntimeRegisterPromiseCancelImpl promise.runtime.handle promise.handle

@[inline] def registerPromiseRelease (promise : RuntimeRegisterPromiseRef) : IO Unit :=
  ffiRuntimeRegisterPromiseReleaseImpl promise.runtime.handle promise.handle

@[inline] def unitPromiseAwait (promise : RuntimeUnitPromiseRef) : IO Unit :=
  ffiRuntimeUnitPromiseAwaitImpl promise.runtime.handle promise.handle

@[inline] def unitPromiseCancel (promise : RuntimeUnitPromiseRef) : IO Unit :=
  ffiRuntimeUnitPromiseCancelImpl promise.runtime.handle promise.handle

@[inline] def unitPromiseRelease (promise : RuntimeUnitPromiseRef) : IO Unit :=
  ffiRuntimeUnitPromiseReleaseImpl promise.runtime.handle promise.handle

@[inline] def streamingCall (runtime : Runtime) (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : IO Unit := do
  let requestBytes := payload.toBytes
  let requestCaps := payload.capTableBytes
  ffiRuntimeStreamingCallWithCapsImpl runtime.handle target method.interfaceId method.methodId
    requestBytes requestCaps

@[inline] def targetGetFd? (runtime : Runtime) (target : Client) : IO (Option UInt32) := do
  let noneSentinel : UInt32 := 0xFFFFFFFF
  let fd ← ffiRuntimeTargetGetFdImpl runtime.handle target
  if fd == noneSentinel then
    return none
  else
    return some fd

@[inline] def targetWhenResolved (runtime : Runtime) (target : Client) : IO Unit :=
  ffiRuntimeTargetWhenResolvedImpl runtime.handle target

@[inline] def targetWhenResolvedStart (runtime : Runtime) (target : Client) :
    IO RuntimeUnitPromiseRef := do
  return {
    runtime := runtime
    handle := (← ffiRuntimeTargetWhenResolvedStartImpl runtime.handle target)
  }

@[inline] def targetWhenResolvedPoll (runtime : Runtime) (target : Client) : IO Bool :=
  ffiRuntimeTargetWhenResolvedPollImpl runtime.handle target

@[inline] def orderingSetResolveHold (runtime : Runtime) (enabled : Bool) : IO Unit :=
  ffiRuntimeOrderingSetResolveHoldImpl runtime.handle (if enabled then 1 else 0)

@[inline] def orderingFlushResolves (runtime : Runtime) : IO UInt64 :=
  ffiRuntimeOrderingFlushResolvesImpl runtime.handle

@[inline] def orderingHeldResolveCount (runtime : Runtime) : IO UInt64 :=
  ffiRuntimeOrderingHeldResolveCountImpl runtime.handle

@[inline] def targetWhenResolvedAsTask (runtime : Runtime) (target : Client) :
    IO (Task (Except IO.Error Unit)) := do
  let pending ← runtime.targetWhenResolvedStart target
  IO.asTask (ffiRuntimeUnitPromiseAwaitImpl runtime.handle pending.handle)

@[inline] def targetWhenResolvedAsPromise (runtime : Runtime) (target : Client) :
    IO (Capnp.Async.Promise Unit) := do
  pure (Capnp.Async.Promise.ofTask (← runtime.targetWhenResolvedAsTask target))

@[inline] def pump (runtime : Runtime) : IO Unit :=
  ffiRuntimePumpImpl runtime.handle

@[inline] def pumpAsTask (runtime : Runtime) : IO (Task (Except IO.Error Unit)) := do
  IO.asTask (ffiRuntimePumpImpl runtime.handle)

@[inline] def pumpAsPromise (runtime : Runtime) : IO (Capnp.Async.Promise Unit) := do
  pure (Capnp.Async.Promise.ofTask (← runtime.pumpAsTask))

@[inline] def enableTraceEncoder (runtime : Runtime) : IO Unit :=
  ffiRuntimeEnableTraceEncoderImpl runtime.handle

@[inline] def disableTraceEncoder (runtime : Runtime) : IO Unit :=
  ffiRuntimeDisableTraceEncoderImpl runtime.handle

@[inline] def setTraceEncoder (runtime : Runtime) (encoder : RawTraceEncoder) : IO Unit :=
  ffiRuntimeSetTraceEncoderImpl runtime.handle encoder

@[inline] def targetCount (runtime : Runtime) : IO UInt64 :=
  ffiRuntimeTargetCountImpl runtime.handle

@[inline] def listenerCount (runtime : Runtime) : IO UInt64 :=
  ffiRuntimeListenerCountImpl runtime.handle

@[inline] def clientCount (runtime : Runtime) : IO UInt64 :=
  ffiRuntimeClientCountImpl runtime.handle

@[inline] def serverCount (runtime : Runtime) : IO UInt64 :=
  ffiRuntimeServerCountImpl runtime.handle

@[inline] def pendingCallCount (runtime : Runtime) : IO UInt64 :=
  ffiRuntimePendingCallCountImpl runtime.handle

def withRuntime (action : Runtime -> IO α) : IO α := do
  let runtime ← init
  try
    action runtime
  finally
    runtime.shutdown

def withRuntimeWithFdLimit (maxFdsPerMessage : UInt32) (action : Runtime -> IO α) : IO α := do
  let runtime ← initWithFdLimit maxFdsPerMessage
  try
    action runtime
  finally
    runtime.shutdown

end Runtime

namespace RuntimeClientRef

@[inline] def release (client : RuntimeClientRef) : IO Unit :=
  ffiRuntimeReleaseClientImpl client.runtime.handle client.handle.handle

@[inline] def withRelease (client : RuntimeClientRef) (action : RuntimeClientRef -> IO α) : IO α := do
  try
    action client
  finally
    client.release

@[inline] def bootstrap (client : RuntimeClientRef) : IO Client :=
  ffiRuntimeClientBootstrapImpl client.runtime.handle client.handle.handle

@[inline] def onDisconnect (client : RuntimeClientRef) : IO Unit :=
  ffiRuntimeClientOnDisconnectImpl client.runtime.handle client.handle.handle

@[inline] def onDisconnectStart (client : RuntimeClientRef) : IO RuntimeUnitPromiseRef := do
  return {
    runtime := client.runtime
    handle := (← ffiRuntimeClientOnDisconnectStartImpl client.runtime.handle client.handle.handle)
  }

@[inline] def onDisconnectAsTask (client : RuntimeClientRef) :
    IO (Task (Except IO.Error Unit)) := do
  let pending ← client.onDisconnectStart
  IO.asTask (ffiRuntimeUnitPromiseAwaitImpl client.runtime.handle pending.handle)

@[inline] def onDisconnectAsPromise (client : RuntimeClientRef) :
    IO (Capnp.Async.Promise Unit) := do
  pure (Capnp.Async.Promise.ofTask (← client.onDisconnectAsTask))

@[inline] def setFlowLimit (client : RuntimeClientRef) (words : UInt64) : IO Unit :=
  ffiRuntimeClientSetFlowLimitImpl client.runtime.handle client.handle.handle words

@[inline] def queueSize (client : RuntimeClientRef) : IO UInt64 :=
  ffiRuntimeClientQueueSizeImpl client.runtime.handle client.handle.handle

@[inline] def queueCount (client : RuntimeClientRef) : IO UInt64 :=
  ffiRuntimeClientQueueCountImpl client.runtime.handle client.handle.handle

@[inline] def outgoingWaitNanos (client : RuntimeClientRef) : IO UInt64 :=
  ffiRuntimeClientOutgoingWaitNanosImpl client.runtime.handle client.handle.handle

instance : Capnp.Async.Releasable RuntimeClientRef where
  release := RuntimeClientRef.release

end RuntimeClientRef

namespace RuntimeServerRef

@[inline] def release (server : RuntimeServerRef) : IO Unit :=
  ffiRuntimeReleaseServerImpl server.runtime.handle server.handle.handle

@[inline] def withRelease (server : RuntimeServerRef) (action : RuntimeServerRef -> IO α) : IO α := do
  try
    action server
  finally
    server.release

@[inline] def listen (server : RuntimeServerRef) (address : String) (portHint : UInt32 := 0) :
    IO Listener :=
  return {
    runtime := server.runtime.handle
    handle := (← ffiRuntimeServerListenImpl server.runtime.handle server.handle.handle address portHint)
  }

@[inline] def accept (server : RuntimeServerRef) (listener : Listener) : IO Unit := do
  ensureSameRuntimeHandle server.runtime listener.runtime "Listener"
  ffiRuntimeServerAcceptImpl server.runtime.handle server.handle.handle listener.handle

@[inline] def acceptStart (server : RuntimeServerRef) (listener : Listener) :
    IO RuntimeUnitPromiseRef := do
  ensureSameRuntimeHandle server.runtime listener.runtime "Listener"
  return {
    runtime := server.runtime
    handle := (← ffiRuntimeServerAcceptStartImpl
      server.runtime.handle server.handle.handle listener.handle)
  }

@[inline] def acceptAsTask (server : RuntimeServerRef) (listener : Listener) :
    IO (Task (Except IO.Error Unit)) := do
  let pending ← server.acceptStart listener
  IO.asTask (ffiRuntimeUnitPromiseAwaitImpl server.runtime.handle pending.handle)

@[inline] def acceptAsPromise (server : RuntimeServerRef) (listener : Listener) :
    IO (Capnp.Async.Promise Unit) := do
  pure (Capnp.Async.Promise.ofTask (← server.acceptAsTask listener))

@[inline] def acceptFd (server : RuntimeServerRef) (fd : UInt32) : IO Unit :=
  ffiRuntimeServerAcceptFdImpl server.runtime.handle server.handle.handle fd

@[inline] def acceptTransport (server : RuntimeServerRef) (transport : RuntimeTransport) : IO Unit := do
  ensureSameRuntimeHandle server.runtime transport.runtime "RuntimeTransport"
  ffiRuntimeServerAcceptTransportImpl server.runtime.handle server.handle.handle transport.handle

@[inline] def acceptTransportFd (server : RuntimeServerRef) (fd : UInt32) : IO Unit := do
  let transport ← Runtime.newTransportFromFd server.runtime fd
  server.acceptTransport transport

@[inline] def drain (server : RuntimeServerRef) : IO Unit :=
  ffiRuntimeServerDrainImpl server.runtime.handle server.handle.handle

@[inline] def drainStart (server : RuntimeServerRef) : IO RuntimeUnitPromiseRef := do
  return {
    runtime := server.runtime
    handle := (← ffiRuntimeServerDrainStartImpl server.runtime.handle server.handle.handle)
  }

@[inline] def drainAsTask (server : RuntimeServerRef) :
    IO (Task (Except IO.Error Unit)) := do
  let pending ← server.drainStart
  IO.asTask (ffiRuntimeUnitPromiseAwaitImpl server.runtime.handle pending.handle)

@[inline] def drainAsPromise (server : RuntimeServerRef) :
    IO (Capnp.Async.Promise Unit) := do
  pure (Capnp.Async.Promise.ofTask (← server.drainAsTask))

@[inline] def withListener (server : RuntimeServerRef) (address : String)
    (action : Listener -> IO α) (portHint : UInt32 := 0) : IO α := do
  let listener ← server.listen address portHint
  try
    action listener
  finally
    server.runtime.releaseListener listener

instance : Capnp.Async.Releasable RuntimeServerRef where
  release := RuntimeServerRef.release

end RuntimeServerRef

namespace RuntimeVatPeerRef

@[inline] def release (peer : RuntimeVatPeerRef) : IO Unit :=
  Runtime.releaseMultiVatPeer peer

@[inline] def withRelease (peer : RuntimeVatPeerRef) (action : RuntimeVatPeerRef -> IO α) : IO α := do
  try
    action peer
  finally
    peer.release

@[inline] def bootstrap (peer : RuntimeVatPeerRef) (vatId : VatId) : IO Client :=
  Runtime.multiVatBootstrap peer vatId

@[inline] def bootstrapPeer (peer : RuntimeVatPeerRef) (targetPeer : RuntimeVatPeerRef)
    (unique : Bool := false) : IO Client :=
  Runtime.multiVatBootstrapPeer peer.runtime peer targetPeer unique

@[inline] def getDiagnostics (peer : RuntimeVatPeerRef) (targetVatId : VatId) : IO RpcDiagnostics :=
  Runtime.multiVatGetDiagnostics peer.runtime peer targetVatId

@[inline] def blockConnectionTo (peer : RuntimeVatPeerRef) (targetPeer : RuntimeVatPeerRef) : IO Unit :=
  Runtime.multiVatConnectionBlock peer.runtime peer targetPeer

@[inline] def unblockConnectionTo (peer : RuntimeVatPeerRef) (targetPeer : RuntimeVatPeerRef) : IO Unit :=
  Runtime.multiVatConnectionUnblock peer.runtime peer targetPeer

@[inline] def disconnectConnectionTo (peer : RuntimeVatPeerRef) (targetPeer : RuntimeVatPeerRef)
    (type : RemoteExceptionType) (message : String) (detail : ByteArray := ByteArray.empty) : IO Unit :=
  Runtime.multiVatConnectionDisconnect peer.runtime peer targetPeer type message detail

@[inline] def resolveDisembargoCountsTo (peer : RuntimeVatPeerRef)
    (targetPeer : RuntimeVatPeerRef) : IO ProtocolMessageCounts :=
  Runtime.multiVatConnectionResolveDisembargoCounts peer.runtime peer targetPeer

@[inline] def resolveDisembargoTraceTo (peer : RuntimeVatPeerRef)
    (targetPeer : RuntimeVatPeerRef) : IO (Array ProtocolMessageTraceTag) :=
  Runtime.multiVatConnectionResolveDisembargoTrace peer.runtime peer targetPeer

@[inline] def resetResolveDisembargoTraceTo (peer : RuntimeVatPeerRef)
    (targetPeer : RuntimeVatPeerRef) : IO Unit :=
  Runtime.multiVatConnectionResetResolveDisembargoTrace peer.runtime peer targetPeer

@[inline] def setRestorer (peer : RuntimeVatPeerRef)
    (restorer : VatId -> ByteArray -> IO Client) : IO Unit :=
  Runtime.multiVatSetRestorer peer restorer

@[inline] def clearRestorer (peer : RuntimeVatPeerRef) : IO Unit :=
  Runtime.multiVatClearRestorer peer

@[inline] def withRestorer (peer : RuntimeVatPeerRef)
    (restorer : VatId -> ByteArray -> IO Client)
    (action : RuntimeVatPeerRef -> IO α) : IO α := do
  peer.setRestorer restorer
  try
    action peer
  finally
    peer.clearRestorer

@[inline] def publishSturdyRef (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client) : IO Unit :=
  Runtime.multiVatPublishSturdyRef peer objectId target

@[inline] def publishSturdyRefStart (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client) : IO RuntimeUnitPromiseRef :=
  Runtime.multiVatPublishSturdyRefStart peer objectId target

@[inline] def publishSturdyRefAsTask (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client) : IO (Task (Except IO.Error Unit)) :=
  Runtime.multiVatPublishSturdyRefAsTask peer objectId target

@[inline] def publishSturdyRefAsPromise (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client) : IO (Capnp.Async.Promise Unit) :=
  Runtime.multiVatPublishSturdyRefAsPromise peer objectId target

@[inline] def withPublishedSturdyRef (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client)
    (action : RuntimeVatPeerRef -> IO α) : IO α := do
  peer.publishSturdyRef objectId target
  try
    action peer
  finally
    Runtime.multiVatUnpublishSturdyRef peer objectId

@[inline] def unpublishSturdyRef (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) : IO Unit :=
  Runtime.multiVatUnpublishSturdyRef peer objectId

@[inline] def unpublishSturdyRefStart (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) : IO RuntimeUnitPromiseRef :=
  Runtime.multiVatUnpublishSturdyRefStart peer objectId

@[inline] def unpublishSturdyRefAsTask (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) : IO (Task (Except IO.Error Unit)) :=
  Runtime.multiVatUnpublishSturdyRefAsTask peer objectId

@[inline] def unpublishSturdyRefAsPromise (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) : IO (Capnp.Async.Promise Unit) :=
  Runtime.multiVatUnpublishSturdyRefAsPromise peer objectId

@[inline] def clearPublishedSturdyRefs (peer : RuntimeVatPeerRef) : IO Unit :=
  Runtime.multiVatClearPublishedSturdyRefs peer

@[inline] def clearPublishedSturdyRefsStart (peer : RuntimeVatPeerRef) :
    IO RuntimeUnitPromiseRef :=
  Runtime.multiVatClearPublishedSturdyRefsStart peer

@[inline] def clearPublishedSturdyRefsAsTask (peer : RuntimeVatPeerRef) :
    IO (Task (Except IO.Error Unit)) :=
  Runtime.multiVatClearPublishedSturdyRefsAsTask peer

@[inline] def clearPublishedSturdyRefsAsPromise (peer : RuntimeVatPeerRef) :
    IO (Capnp.Async.Promise Unit) :=
  Runtime.multiVatClearPublishedSturdyRefsAsPromise peer

@[inline] def publishedSturdyRefCount (peer : RuntimeVatPeerRef) : IO UInt64 :=
  Runtime.multiVatPublishedSturdyRefCount peer

@[inline] def restoreSturdyRef (peer : RuntimeVatPeerRef)
    (sturdyRef : SturdyRef) : IO Client :=
  Runtime.multiVatRestoreSturdyRef peer sturdyRef

@[inline] def restoreSturdyRefAt (peer : RuntimeVatPeerRef)
    (host : String) (objectId : ByteArray) (unique : Bool := false) : IO Client :=
  peer.restoreSturdyRef (SturdyRef.ofHost host objectId unique)

@[inline] def restoreSturdyRefStart (peer : RuntimeVatPeerRef)
    (sturdyRef : SturdyRef) : IO RuntimeRegisterPromiseRef :=
  Runtime.multiVatRestoreSturdyRefStart peer sturdyRef

@[inline] def restoreSturdyRefStartAt (peer : RuntimeVatPeerRef)
    (host : String) (objectId : ByteArray) (unique : Bool := false) :
    IO RuntimeRegisterPromiseRef :=
  peer.restoreSturdyRefStart (SturdyRef.ofHost host objectId unique)

@[inline] def restoreSturdyRefAsTask (peer : RuntimeVatPeerRef)
    (sturdyRef : SturdyRef) : IO (Task (Except IO.Error Client)) :=
  Runtime.multiVatRestoreSturdyRefAsTask peer sturdyRef

@[inline] def restoreSturdyRefAsTaskAt (peer : RuntimeVatPeerRef)
    (host : String) (objectId : ByteArray) (unique : Bool := false) :
    IO (Task (Except IO.Error Client)) :=
  peer.restoreSturdyRefAsTask (SturdyRef.ofHost host objectId unique)

@[inline] def restoreSturdyRefAsPromise (peer : RuntimeVatPeerRef)
    (sturdyRef : SturdyRef) : IO (Capnp.Async.Promise Client) :=
  Runtime.multiVatRestoreSturdyRefAsPromise peer sturdyRef

@[inline] def restoreSturdyRefAsPromiseAt (peer : RuntimeVatPeerRef)
    (host : String) (objectId : ByteArray) (unique : Bool := false) :
    IO (Capnp.Async.Promise Client) :=
  peer.restoreSturdyRefAsPromise (SturdyRef.ofHost host objectId unique)

instance : Capnp.Async.Releasable RuntimeVatPeerRef where
  release := RuntimeVatPeerRef.release

end RuntimeVatPeerRef

namespace VatNetwork

@[inline] private def ensurePeerRuntime
    (network : VatNetwork) (peer : RuntimeVatPeerRef) (op : String) : IO Unit :=
  if peer.runtime == network.runtime then
    pure ()
  else
    throw (IO.userError s!"VatNetwork.{op}: peer belongs to a different runtime")

@[inline] def newClient (network : VatNetwork) (name : String) : IO RuntimeVatPeerRef :=
  Runtime.newMultiVatClient network.runtime name

@[inline] def newServer (network : VatNetwork) (name : String) (bootstrap : Client) :
    IO RuntimeVatPeerRef :=
  Runtime.newMultiVatServer network.runtime name bootstrap

@[inline] def newServerWithBootstrapFactory (network : VatNetwork) (name : String)
    (bootstrapFactory : VatId -> IO Client) : IO RuntimeVatPeerRef :=
  Runtime.newMultiVatServerWithBootstrapFactory network.runtime name bootstrapFactory

@[inline] def bootstrap (network : VatNetwork) (sourcePeer : RuntimeVatPeerRef)
    (targetPeer : RuntimeVatPeerRef) (unique : Bool := false) : IO Client := do
  ensurePeerRuntime network sourcePeer "bootstrap"
  ensurePeerRuntime network targetPeer "bootstrap"
  Runtime.multiVatBootstrapPeer network.runtime sourcePeer targetPeer unique

@[inline] def setRestorer (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (restorer : VatId -> ByteArray -> IO Client) : IO Unit := do
  ensurePeerRuntime network peer "setRestorer"
  Runtime.multiVatSetRestorer peer restorer

@[inline] def clearRestorer (network : VatNetwork) (peer : RuntimeVatPeerRef) : IO Unit := do
  ensurePeerRuntime network peer "clearRestorer"
  Runtime.multiVatClearRestorer peer

@[inline] def withRestorer (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (restorer : VatId -> ByteArray -> IO Client)
    (action : RuntimeVatPeerRef -> IO α) : IO α := do
  ensurePeerRuntime network peer "withRestorer"
  network.setRestorer peer restorer
  try
    action peer
  finally
    network.clearRestorer peer

@[inline] def publishSturdyRef (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client) : IO Unit := do
  ensurePeerRuntime network peer "publishSturdyRef"
  Runtime.multiVatPublishSturdyRef peer objectId target

@[inline] def publishSturdyRefStart (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client) : IO RuntimeUnitPromiseRef := do
  ensurePeerRuntime network peer "publishSturdyRefStart"
  Runtime.multiVatPublishSturdyRefStart peer objectId target

@[inline] def publishSturdyRefAsTask (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client) : IO (Task (Except IO.Error Unit)) := do
  ensurePeerRuntime network peer "publishSturdyRefAsTask"
  Runtime.multiVatPublishSturdyRefAsTask peer objectId target

@[inline] def publishSturdyRefAsPromise (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client) : IO (Capnp.Async.Promise Unit) := do
  ensurePeerRuntime network peer "publishSturdyRefAsPromise"
  Runtime.multiVatPublishSturdyRefAsPromise peer objectId target

@[inline] def withPublishedSturdyRef (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client)
    (action : RuntimeVatPeerRef -> IO α) : IO α := do
  ensurePeerRuntime network peer "withPublishedSturdyRef"
  Runtime.multiVatPublishSturdyRef peer objectId target
  try
    action peer
  finally
    Runtime.multiVatUnpublishSturdyRef peer objectId

@[inline] def unpublishSturdyRef (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) : IO Unit := do
  ensurePeerRuntime network peer "unpublishSturdyRef"
  Runtime.multiVatUnpublishSturdyRef peer objectId

@[inline] def unpublishSturdyRefStart (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) : IO RuntimeUnitPromiseRef := do
  ensurePeerRuntime network peer "unpublishSturdyRefStart"
  Runtime.multiVatUnpublishSturdyRefStart peer objectId

@[inline] def unpublishSturdyRefAsTask (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) : IO (Task (Except IO.Error Unit)) := do
  ensurePeerRuntime network peer "unpublishSturdyRefAsTask"
  Runtime.multiVatUnpublishSturdyRefAsTask peer objectId

@[inline] def unpublishSturdyRefAsPromise (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) : IO (Capnp.Async.Promise Unit) := do
  ensurePeerRuntime network peer "unpublishSturdyRefAsPromise"
  Runtime.multiVatUnpublishSturdyRefAsPromise peer objectId

@[inline] def clearPublishedSturdyRefs (network : VatNetwork) (peer : RuntimeVatPeerRef) : IO Unit := do
  ensurePeerRuntime network peer "clearPublishedSturdyRefs"
  Runtime.multiVatClearPublishedSturdyRefs peer

@[inline] def clearPublishedSturdyRefsStart (network : VatNetwork) (peer : RuntimeVatPeerRef) :
    IO RuntimeUnitPromiseRef := do
  ensurePeerRuntime network peer "clearPublishedSturdyRefsStart"
  Runtime.multiVatClearPublishedSturdyRefsStart peer

@[inline] def clearPublishedSturdyRefsAsTask (network : VatNetwork) (peer : RuntimeVatPeerRef) :
    IO (Task (Except IO.Error Unit)) := do
  ensurePeerRuntime network peer "clearPublishedSturdyRefsAsTask"
  Runtime.multiVatClearPublishedSturdyRefsAsTask peer

@[inline] def clearPublishedSturdyRefsAsPromise (network : VatNetwork) (peer : RuntimeVatPeerRef) :
    IO (Capnp.Async.Promise Unit) := do
  ensurePeerRuntime network peer "clearPublishedSturdyRefsAsPromise"
  Runtime.multiVatClearPublishedSturdyRefsAsPromise peer

@[inline] def publishedSturdyRefCount (network : VatNetwork)
    (peer : RuntimeVatPeerRef) : IO UInt64 := do
  ensurePeerRuntime network peer "publishedSturdyRefCount"
  Runtime.multiVatPublishedSturdyRefCount peer

@[inline] def restoreSturdyRef (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (sturdyRef : SturdyRef) : IO Client := do
  ensurePeerRuntime network peer "restoreSturdyRef"
  Runtime.multiVatRestoreSturdyRef peer sturdyRef

@[inline] def restoreSturdyRefAt (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (host : String) (objectId : ByteArray) (unique : Bool := false) : IO Client := do
  ensurePeerRuntime network peer "restoreSturdyRefAt"
  Runtime.multiVatRestoreSturdyRef peer (SturdyRef.ofHost host objectId unique)

@[inline] def restoreSturdyRefStart (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (sturdyRef : SturdyRef) : IO RuntimeRegisterPromiseRef := do
  ensurePeerRuntime network peer "restoreSturdyRefStart"
  Runtime.multiVatRestoreSturdyRefStart peer sturdyRef

@[inline] def restoreSturdyRefStartAt (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (host : String) (objectId : ByteArray) (unique : Bool := false) :
    IO RuntimeRegisterPromiseRef := do
  ensurePeerRuntime network peer "restoreSturdyRefStartAt"
  Runtime.multiVatRestoreSturdyRefStart peer (SturdyRef.ofHost host objectId unique)

@[inline] def restoreSturdyRefAsTask (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (sturdyRef : SturdyRef) : IO (Task (Except IO.Error Client)) := do
  ensurePeerRuntime network peer "restoreSturdyRefAsTask"
  Runtime.multiVatRestoreSturdyRefAsTask peer sturdyRef

@[inline] def restoreSturdyRefAsTaskAt (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (host : String) (objectId : ByteArray) (unique : Bool := false) :
    IO (Task (Except IO.Error Client)) := do
  ensurePeerRuntime network peer "restoreSturdyRefAsTaskAt"
  Runtime.multiVatRestoreSturdyRefAsTask peer (SturdyRef.ofHost host objectId unique)

@[inline] def restoreSturdyRefAsPromise (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (sturdyRef : SturdyRef) : IO (Capnp.Async.Promise Client) := do
  ensurePeerRuntime network peer "restoreSturdyRefAsPromise"
  Runtime.multiVatRestoreSturdyRefAsPromise peer sturdyRef

@[inline] def restoreSturdyRefAsPromiseAt (network : VatNetwork) (peer : RuntimeVatPeerRef)
    (host : String) (objectId : ByteArray) (unique : Bool := false) :
    IO (Capnp.Async.Promise Client) := do
  ensurePeerRuntime network peer "restoreSturdyRefAsPromiseAt"
  Runtime.multiVatRestoreSturdyRefAsPromise peer (SturdyRef.ofHost host objectId unique)

@[inline] def setForwardingEnabled (network : VatNetwork) (enabled : Bool) : IO Unit :=
  Runtime.multiVatSetForwardingEnabled network.runtime enabled

@[inline] def resetForwardingStats (network : VatNetwork) : IO Unit :=
  Runtime.multiVatResetForwardingStats network.runtime

@[inline] def stats (network : VatNetwork) : IO MultiVatStats :=
  Runtime.multiVatStats network.runtime

@[inline] def hasConnection (network : VatNetwork)
    (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef) : IO Bool := do
  ensurePeerRuntime network fromPeer "hasConnection"
  ensurePeerRuntime network toPeer "hasConnection"
  Runtime.multiVatHasConnection network.runtime fromPeer toPeer

@[inline] def getDiagnostics (network : VatNetwork)
    (peer : RuntimeVatPeerRef) (targetVatId : VatId) : IO RpcDiagnostics := do
  ensurePeerRuntime network peer "getDiagnostics"
  Runtime.multiVatGetDiagnostics network.runtime peer targetVatId

@[inline] def blockConnection (network : VatNetwork)
    (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef) : IO Unit := do
  ensurePeerRuntime network fromPeer "blockConnection"
  ensurePeerRuntime network toPeer "blockConnection"
  Runtime.multiVatConnectionBlock network.runtime fromPeer toPeer

@[inline] def unblockConnection (network : VatNetwork)
    (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef) : IO Unit := do
  ensurePeerRuntime network fromPeer "unblockConnection"
  ensurePeerRuntime network toPeer "unblockConnection"
  Runtime.multiVatConnectionUnblock network.runtime fromPeer toPeer

@[inline] def disconnectConnection (network : VatNetwork)
    (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef)
    (type : RemoteExceptionType) (message : String) (detail : ByteArray := ByteArray.empty) : IO Unit := do
  ensurePeerRuntime network fromPeer "disconnectConnection"
  ensurePeerRuntime network toPeer "disconnectConnection"
  Runtime.multiVatConnectionDisconnect network.runtime fromPeer toPeer type message detail

@[inline] def connectionResolveDisembargoCounts (network : VatNetwork)
    (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef) : IO ProtocolMessageCounts := do
  ensurePeerRuntime network fromPeer "connectionResolveDisembargoCounts"
  ensurePeerRuntime network toPeer "connectionResolveDisembargoCounts"
  Runtime.multiVatConnectionResolveDisembargoCounts network.runtime fromPeer toPeer

@[inline] def connectionResolveDisembargoTrace (network : VatNetwork)
    (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef) :
    IO (Array ProtocolMessageTraceTag) := do
  ensurePeerRuntime network fromPeer "connectionResolveDisembargoTrace"
  ensurePeerRuntime network toPeer "connectionResolveDisembargoTrace"
  Runtime.multiVatConnectionResolveDisembargoTrace network.runtime fromPeer toPeer

@[inline] def resetConnectionResolveDisembargoTrace (network : VatNetwork)
    (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef) : IO Unit := do
  ensurePeerRuntime network fromPeer "resetConnectionResolveDisembargoTrace"
  ensurePeerRuntime network toPeer "resetConnectionResolveDisembargoTrace"
  Runtime.multiVatConnectionResetResolveDisembargoTrace network.runtime fromPeer toPeer

@[inline] def releasePeer (network : VatNetwork) (peer : RuntimeVatPeerRef) : IO Unit := do
  ensurePeerRuntime network peer "releasePeer"
  Runtime.releaseMultiVatPeer peer

end VatNetwork

namespace RuntimePayloadRef

@[inline] def toBytes (payloadRef : RuntimePayloadRef) : IO (ByteArray × ByteArray) :=
  Runtime.payloadRefToBytes payloadRef

@[inline] def toBytesCopy (payloadRef : RuntimePayloadRef) : IO (ByteArray × ByteArray) :=
  Runtime.payloadRefToBytesCopy payloadRef

@[inline] def decode (payloadRef : RuntimePayloadRef) : IO Payload :=
  Runtime.payloadRefDecode payloadRef

@[inline] def release (payloadRef : RuntimePayloadRef) : IO Unit :=
  Runtime.payloadRefRelease payloadRef

@[inline] def withRelease (payloadRef : RuntimePayloadRef)
    (action : RuntimePayloadRef -> IO α) : IO α := do
  try
    action payloadRef
  finally
    payloadRef.release

@[inline] def decodeAndRelease (payloadRef : RuntimePayloadRef) : IO Payload := do
  let payload ← payloadRef.decode
  payloadRef.release
  pure payload

instance : Capnp.Async.Releasable RuntimePayloadRef where
  release := RuntimePayloadRef.release

end RuntimePayloadRef

namespace RuntimePendingCallRef

@[inline] def await (pendingCall : RuntimePendingCallRef) : IO Payload :=
  Runtime.pendingCallAwait pendingCall

@[inline] def awaitPayloadRef (pendingCall : RuntimePendingCallRef) : IO RuntimePayloadRef :=
  Runtime.pendingCallAwaitPayloadRef pendingCall

@[inline] def awaitOutcome (pendingCall : RuntimePendingCallRef) : IO RawCallOutcome :=
  Runtime.pendingCallAwaitOutcome pendingCall

@[inline] def awaitResult (pendingCall : RuntimePendingCallRef) :
    IO (Except RemoteException Payload) :=
  Runtime.pendingCallAwaitResult pendingCall

@[inline] def release (pendingCall : RuntimePendingCallRef) : IO Unit :=
  Runtime.pendingCallRelease pendingCall

@[inline] def releaseDeferred (pendingCall : RuntimePendingCallRef) : IO Unit :=
  Runtime.pendingCallReleaseDeferred pendingCall

@[inline] def withRelease (pendingCall : RuntimePendingCallRef)
    (action : RuntimePendingCallRef -> IO α) : IO α := do
  try
    action pendingCall
  finally
    pendingCall.release

@[inline] def getPipelinedCap (pendingCall : RuntimePendingCallRef)
    (pointerPath : Array UInt16 := #[]) : IO Client :=
  Runtime.pendingCallGetPipelinedCap pendingCall pointerPath

instance : Capnp.Async.Awaitable RuntimePendingCallRef Payload where
  await := RuntimePendingCallRef.await

instance : Capnp.Async.Releasable RuntimePendingCallRef where
  release := RuntimePendingCallRef.release

@[inline] def awaitAsTask (pendingCall : RuntimePendingCallRef) :
    IO (Task (Except IO.Error Payload)) :=
  Capnp.Async.awaitAsTask pendingCall

@[inline] def awaitPayloadRefAsTask (pendingCall : RuntimePendingCallRef) :
    IO (Task (Except IO.Error RuntimePayloadRef)) :=
  IO.asTask do
    pendingCall.awaitPayloadRef

@[inline] def awaitOutcomeAsTask (pendingCall : RuntimePendingCallRef) :
    IO (Task (Except IO.Error RawCallOutcome)) :=
  IO.asTask do
    pendingCall.awaitOutcome

@[inline] def awaitResultAsTask (pendingCall : RuntimePendingCallRef) :
    IO (Task (Except IO.Error (Except RemoteException Payload))) :=
  IO.asTask do
    pendingCall.awaitResult

@[inline] def toPromise (pendingCall : RuntimePendingCallRef) :
    IO (Capnp.Async.Promise Payload) := do
  pure (Capnp.Async.Promise.ofTask (← pendingCall.awaitAsTask))

@[inline] def toPromisePayloadRef (pendingCall : RuntimePendingCallRef) :
    IO (Capnp.Async.Promise RuntimePayloadRef) := do
  pure (Capnp.Async.Promise.ofTask (← pendingCall.awaitPayloadRefAsTask))

@[inline] def toPromiseOutcome (pendingCall : RuntimePendingCallRef) :
    IO (Capnp.Async.Promise RawCallOutcome) := do
  pure (Capnp.Async.Promise.ofTask (← pendingCall.awaitOutcomeAsTask))

@[inline] def toPromiseResult (pendingCall : RuntimePendingCallRef) :
    IO (Capnp.Async.Promise (Except RemoteException Payload)) := do
  pure (Capnp.Async.Promise.ofTask (← pendingCall.awaitResultAsTask))

def toIOPromise (pendingCall : RuntimePendingCallRef) :
    IO (IO.Promise (Except String Payload)) := do
  Capnp.Async.toIOPromise pendingCall

end RuntimePendingCallRef

namespace RuntimeRegisterPromiseRef

@[inline] def await (promise : RuntimeRegisterPromiseRef) : IO UInt32 :=
  Runtime.registerPromiseAwait promise

@[inline] def cancel (promise : RuntimeRegisterPromiseRef) : IO Unit :=
  Runtime.registerPromiseCancel promise

@[inline] def release (promise : RuntimeRegisterPromiseRef) : IO Unit :=
  Runtime.registerPromiseRelease promise

@[inline] def withRelease (promise : RuntimeRegisterPromiseRef)
    (action : RuntimeRegisterPromiseRef -> IO α) : IO α := do
  try
    action promise
  finally
    promise.release

@[inline] def awaitTarget (promise : RuntimeRegisterPromiseRef) : IO Client :=
  promise.await

@[inline] def awaitClient (promise : RuntimeRegisterPromiseRef) : IO RuntimeClientRef := do
  return {
    runtime := promise.runtime
    handle := { handle := (← promise.await) }
  }

@[inline] def awaitListener (promise : RuntimeRegisterPromiseRef) : IO Listener := do
  return { runtime := promise.runtime.handle, handle := (← promise.await) }

@[inline] def awaitServer (promise : RuntimeRegisterPromiseRef) : IO RuntimeServerRef := do
  return {
    runtime := promise.runtime
    handle := { handle := (← promise.await) }
  }

instance : Capnp.Async.Awaitable RuntimeRegisterPromiseRef UInt32 where
  await := RuntimeRegisterPromiseRef.await

instance : Capnp.Async.Cancelable RuntimeRegisterPromiseRef where
  cancel := RuntimeRegisterPromiseRef.cancel

instance : Capnp.Async.Releasable RuntimeRegisterPromiseRef where
  release := RuntimeRegisterPromiseRef.release

@[inline] def awaitAsTask (promise : RuntimeRegisterPromiseRef) :
    IO (Task (Except IO.Error UInt32)) :=
  Capnp.Async.awaitAsTask promise

@[inline] def toPromise (promise : RuntimeRegisterPromiseRef) :
    IO (Capnp.Async.Promise UInt32) := do
  pure (Capnp.Async.Promise.ofTask (← promise.awaitAsTask))

def toIOPromise (promise : RuntimeRegisterPromiseRef) :
    IO (IO.Promise (Except String UInt32)) := do
  Capnp.Async.toIOPromise promise

end RuntimeRegisterPromiseRef

namespace RuntimeUnitPromiseRef

@[inline] def await (promise : RuntimeUnitPromiseRef) : IO Unit :=
  Runtime.unitPromiseAwait promise

@[inline] def cancel (promise : RuntimeUnitPromiseRef) : IO Unit :=
  Runtime.unitPromiseCancel promise

@[inline] def release (promise : RuntimeUnitPromiseRef) : IO Unit :=
  Runtime.unitPromiseRelease promise

@[inline] def withRelease (promise : RuntimeUnitPromiseRef)
    (action : RuntimeUnitPromiseRef -> IO α) : IO α := do
  try
    action promise
  finally
    promise.release

instance : Capnp.Async.Awaitable RuntimeUnitPromiseRef Unit where
  await := RuntimeUnitPromiseRef.await

instance : Capnp.Async.Cancelable RuntimeUnitPromiseRef where
  cancel := RuntimeUnitPromiseRef.cancel

instance : Capnp.Async.Releasable RuntimeUnitPromiseRef where
  release := RuntimeUnitPromiseRef.release

@[inline] def awaitAsTask (promise : RuntimeUnitPromiseRef) :
    IO (Task (Except IO.Error Unit)) :=
  Capnp.Async.awaitAsTask promise

@[inline] def toPromise (promise : RuntimeUnitPromiseRef) :
    IO (Capnp.Async.Promise Unit) := do
  pure (Capnp.Async.Promise.ofTask (← promise.awaitAsTask))

def toIOPromise (promise : RuntimeUnitPromiseRef) :
    IO (IO.Promise (Except String Unit)) := do
  Capnp.Async.toIOPromise promise

end RuntimeUnitPromiseRef

namespace RuntimePromiseCapabilityFulfillerRef

@[inline] def fulfill (fulfiller : RuntimePromiseCapabilityFulfillerRef) (target : Client) : IO Unit :=
  Runtime.promiseCapabilityFulfill fulfiller target

@[inline] def reject (fulfiller : RuntimePromiseCapabilityFulfillerRef) (type : RemoteExceptionType)
    (message : String) (detail : ByteArray := ByteArray.empty) : IO Unit :=
  Runtime.promiseCapabilityReject fulfiller type message detail

@[inline] def release (fulfiller : RuntimePromiseCapabilityFulfillerRef) : IO Unit :=
  Runtime.promiseCapabilityRelease fulfiller

@[inline] def withRelease (fulfiller : RuntimePromiseCapabilityFulfillerRef)
    (action : RuntimePromiseCapabilityFulfillerRef -> IO α) : IO α := do
  try
    action fulfiller
  finally
    fulfiller.release

end RuntimePromiseCapabilityFulfillerRef

abbrev RuntimeM := ReaderT Runtime IO

namespace RuntimeM

@[inline] def run (runtime : Runtime) (action : RuntimeM α) : IO α :=
  action runtime

@[inline] def runWithNewRuntime (action : RuntimeM α) : IO α :=
  Runtime.withRuntime fun runtime => action runtime

@[inline] def runWithNewRuntimeWithFdLimit
    (maxFdsPerMessage : UInt32) (action : RuntimeM α) : IO α :=
  Runtime.withRuntimeWithFdLimit maxFdsPerMessage fun runtime => action runtime

@[inline] def runtime : RuntimeM Runtime := read

@[inline] private def ensureCurrentRuntime
    (owner : Runtime) (resource : String) : RuntimeM Unit := do
  ensureSameRuntime (← runtime) owner resource

@[inline] private def ensureCurrentRuntimeHandle
    (ownerHandle : UInt64) (resource : String) : RuntimeM Unit := do
  ensureSameRuntimeHandle (← runtime) ownerHandle resource

@[inline] def backend : RuntimeM Backend := do
  return Runtime.backend (← runtime)

@[inline] def isAlive : RuntimeM Bool := do
  Runtime.isAlive (← runtime)

@[inline] def registerEchoTarget : RuntimeM Client := do
  Runtime.registerEchoTarget (← runtime)

@[inline] def registerLoopbackTarget (bootstrap : Client) : RuntimeM Client := do
  Runtime.registerLoopbackTarget (← runtime) bootstrap

@[inline] def registerHandlerTarget
    (handler : Client -> Method -> Payload -> IO Payload) : RuntimeM Client := do
  Runtime.registerHandlerTarget (← runtime) handler

@[inline] def registerAdvancedHandlerTarget
    (handler : Client -> Method -> Payload -> IO AdvancedHandlerResult) : RuntimeM Client := do
  Runtime.registerAdvancedHandlerTarget (← runtime) handler

@[inline] def registerAdvancedHandlerTargetAsync
    (handler : Client -> Method -> Payload -> IO AdvancedHandlerReply) : RuntimeM Client := do
  Runtime.registerAdvancedHandlerTargetAsync (← runtime) handler

@[inline] def registerTailCallHandlerTarget
    (handler : Client -> Method -> Payload -> IO Client) : RuntimeM Client := do
  Runtime.registerTailCallHandlerTarget (← runtime) handler

@[inline] def registerStreamingHandlerTarget
    (handler : Client -> Method -> Payload -> IO Unit) : RuntimeM Client := do
  Runtime.registerStreamingHandlerTarget (← runtime) handler

@[inline] def registerStreamingHandlerTargetAsync
    (handler : Client -> Method -> Payload -> IO AdvancedHandlerReply) : RuntimeM Client := do
  Runtime.registerStreamingHandlerTargetAsync (← runtime) handler

@[inline] def registerTailCallTarget (target : Client) : RuntimeM Client := do
  Runtime.registerTailCallTarget (← runtime) target

@[inline] def registerFdTarget (fd : UInt32) : RuntimeM Client := do
  Runtime.registerFdTarget (← runtime) fd

@[inline] def releaseTarget (target : Client) : RuntimeM Unit := do
  Runtime.releaseTarget (← runtime) target

@[inline] def releaseTargetDeferred (target : Client) : RuntimeM Unit := do
  Runtime.releaseTargetDeferred (← runtime) target

@[inline] def retainTarget (target : Client) : RuntimeM Client := do
  Runtime.retainTarget (← runtime) target

@[inline] def withTarget (target : Client) (action : Client -> RuntimeM α) : RuntimeM α := do
  try
    action target
  finally
    releaseTarget target

@[inline] def withRetainedTarget (target : Client)
    (action : Client -> RuntimeM α) : RuntimeM α := do
  let retained ← retainTarget target
  withTarget retained action

@[inline] def newPromiseCapability : RuntimeM (Client × RuntimePromiseCapabilityFulfillerRef) := do
  Runtime.newPromiseCapability (← runtime)

@[inline] def promiseCapabilityFulfill (fulfiller : RuntimePromiseCapabilityFulfillerRef)
    (target : Client) : RuntimeM Unit := do
  ensureCurrentRuntime fulfiller.runtime "RuntimePromiseCapabilityFulfillerRef"
  Runtime.promiseCapabilityFulfill fulfiller target

@[inline] def promiseCapabilityReject (fulfiller : RuntimePromiseCapabilityFulfillerRef)
    (type : RemoteExceptionType) (message : String)
    (detail : ByteArray := ByteArray.empty) : RuntimeM Unit := do
  ensureCurrentRuntime fulfiller.runtime "RuntimePromiseCapabilityFulfillerRef"
  Runtime.promiseCapabilityReject fulfiller type message detail

@[inline] def promiseCapabilityRelease (fulfiller : RuntimePromiseCapabilityFulfillerRef) : RuntimeM Unit := do
  ensureCurrentRuntime fulfiller.runtime "RuntimePromiseCapabilityFulfillerRef"
  Runtime.promiseCapabilityRelease fulfiller

@[inline] def releaseCapTable (capTable : Capnp.CapTable) : RuntimeM Unit := do
  Runtime.releaseCapTable (← runtime) capTable

@[inline] def withCapTable (capTable : Capnp.CapTable)
    (action : Capnp.CapTable -> RuntimeM α) : RuntimeM α := do
  try
    action capTable
  finally
    releaseCapTable capTable

@[inline] def connect (address : String) (portHint : UInt32 := 0) : RuntimeM Client := do
  Runtime.connect (← runtime) address portHint

@[inline] def connectStart (address : String) (portHint : UInt32 := 0) :
    RuntimeM RuntimeRegisterPromiseRef := do
  Runtime.connectStart (← runtime) address portHint

@[inline] def connectAsTask (address : String) (portHint : UInt32 := 0) :
    RuntimeM (Task (Except IO.Error Client)) := do
  Runtime.connectAsTask (← runtime) address portHint

@[inline] def connectAsPromise (address : String) (portHint : UInt32 := 0) :
    RuntimeM (Capnp.Async.Promise Client) := do
  Runtime.connectAsPromise (← runtime) address portHint

@[inline] def connectFd (fd : UInt32) : RuntimeM Client := do
  Runtime.connectFd (← runtime) fd

@[inline] def newTransportPipe : RuntimeM (RuntimeTransport × RuntimeTransport) := do
  Runtime.newTransportPipe (← runtime)

@[inline] def newTransportFromFd (fd : UInt32) : RuntimeM RuntimeTransport := do
  Runtime.newTransportFromFd (← runtime) fd

@[inline] def newTransportFromFdTake (fd : UInt32) : RuntimeM RuntimeTransport := do
  Runtime.newTransportFromFdTake (← runtime) fd

@[inline] def releaseTransport (transport : RuntimeTransport) : RuntimeM Unit := do
  Runtime.releaseTransport (← runtime) transport

@[inline] def transportGetFd? (transport : RuntimeTransport) : RuntimeM (Option UInt32) := do
  Runtime.transportGetFd? (← runtime) transport

@[inline] def connectTransport (transport : RuntimeTransport) : RuntimeM Client := do
  Runtime.connectTransport (← runtime) transport

@[inline] def connectTransportFd (fd : UInt32) : RuntimeM Client := do
  Runtime.connectTransportFd (← runtime) fd

@[inline] def listenEcho (address : String) (portHint : UInt32 := 0) : RuntimeM Listener := do
  Runtime.listenEcho (← runtime) address portHint

@[inline] def acceptEcho (listener : Listener) : RuntimeM Unit := do
  Runtime.acceptEcho (← runtime) listener

@[inline] def releaseListener (listener : Listener) : RuntimeM Unit := do
  Runtime.releaseListener (← runtime) listener

@[inline] def newClient (address : String) (portHint : UInt32 := 0) : RuntimeM RuntimeClientRef := do
  Runtime.newClient (← runtime) address portHint

@[inline] def newClientStart (address : String) (portHint : UInt32 := 0) :
    RuntimeM RuntimeRegisterPromiseRef := do
  Runtime.newClientStart (← runtime) address portHint

@[inline] def newClientAsTask (address : String) (portHint : UInt32 := 0) :
    RuntimeM (Task (Except IO.Error RuntimeClientRef)) := do
  Runtime.newClientAsTask (← runtime) address portHint

@[inline] def newClientAsPromise (address : String) (portHint : UInt32 := 0) :
    RuntimeM (Capnp.Async.Promise RuntimeClientRef) := do
  Runtime.newClientAsPromise (← runtime) address portHint

@[inline] def newServer (bootstrap : Client) : RuntimeM RuntimeServerRef := do
  Runtime.newServer (← runtime) bootstrap

@[inline] def newServerWithBootstrapFactory
    (bootstrapFactory : TwoPartyVatSide -> IO Client) : RuntimeM RuntimeServerRef := do
  Runtime.newServerWithBootstrapFactory (← runtime) bootstrapFactory

@[inline] def newMultiVatClient (name : String) : RuntimeM RuntimeVatPeerRef := do
  Runtime.newMultiVatClient (← runtime) name

@[inline] def newMultiVatServer (name : String) (bootstrap : Client) :
    RuntimeM RuntimeVatPeerRef := do
  Runtime.newMultiVatServer (← runtime) name bootstrap

@[inline] def newMultiVatServerWithBootstrapFactory (name : String)
    (bootstrapFactory : VatId -> IO Client) : RuntimeM RuntimeVatPeerRef := do
  Runtime.newMultiVatServerWithBootstrapFactory (← runtime) name bootstrapFactory

@[inline] def releaseMultiVatPeer (peer : RuntimeVatPeerRef) : RuntimeM Unit := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.releaseMultiVatPeer peer

@[inline] def multiVatBootstrap (peer : RuntimeVatPeerRef) (vatId : VatId) :
    RuntimeM Client := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatBootstrap peer vatId

@[inline] def multiVatBootstrapPeer (sourcePeer : RuntimeVatPeerRef) (targetPeer : RuntimeVatPeerRef)
    (unique : Bool := false) : RuntimeM Client := do
  Runtime.multiVatBootstrapPeer (← runtime) sourcePeer targetPeer unique

@[inline] def multiVatSetForwardingEnabled (enabled : Bool) : RuntimeM Unit := do
  Runtime.multiVatSetForwardingEnabled (← runtime) enabled

@[inline] def multiVatResetForwardingStats : RuntimeM Unit := do
  Runtime.multiVatResetForwardingStats (← runtime)

@[inline] def multiVatForwardCount : RuntimeM UInt64 := do
  Runtime.multiVatForwardCount (← runtime)

@[inline] def multiVatThirdPartyTokenCount : RuntimeM UInt64 := do
  Runtime.multiVatThirdPartyTokenCount (← runtime)

@[inline] def multiVatDeniedForwardCount : RuntimeM UInt64 := do
  Runtime.multiVatDeniedForwardCount (← runtime)

@[inline] def multiVatStats : RuntimeM MultiVatStats := do
  Runtime.multiVatStats (← runtime)

@[inline] def multiVatHasConnection (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef) :
    RuntimeM Bool := do
  Runtime.multiVatHasConnection (← runtime) fromPeer toPeer

@[inline] def multiVatConnectionResolveDisembargoCounts
    (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef) :
    RuntimeM ProtocolMessageCounts := do
  Runtime.multiVatConnectionResolveDisembargoCounts (← runtime) fromPeer toPeer

@[inline] def multiVatConnectionResolveDisembargoTrace
    (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef) :
    RuntimeM (Array ProtocolMessageTraceTag) := do
  Runtime.multiVatConnectionResolveDisembargoTrace (← runtime) fromPeer toPeer

@[inline] def multiVatConnectionResetResolveDisembargoTrace
    (fromPeer : RuntimeVatPeerRef) (toPeer : RuntimeVatPeerRef) :
    RuntimeM Unit := do
  Runtime.multiVatConnectionResetResolveDisembargoTrace (← runtime) fromPeer toPeer

@[inline] def multiVatSetRestorer (peer : RuntimeVatPeerRef)
    (restorer : VatId -> ByteArray -> IO Client) : RuntimeM Unit := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatSetRestorer peer restorer

@[inline] def multiVatClearRestorer (peer : RuntimeVatPeerRef) : RuntimeM Unit := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatClearRestorer peer

@[inline] def multiVatWithRestorer (peer : RuntimeVatPeerRef)
    (restorer : VatId -> ByteArray -> IO Client)
    (action : RuntimeVatPeerRef -> RuntimeM α) : RuntimeM α := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatSetRestorer peer restorer
  try
    action peer
  finally
    Runtime.multiVatClearRestorer peer

@[inline] def multiVatPublishSturdyRef (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client) : RuntimeM Unit := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatPublishSturdyRef peer objectId target

@[inline] def multiVatPublishSturdyRefStart (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client) : RuntimeM RuntimeUnitPromiseRef := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatPublishSturdyRefStart peer objectId target

@[inline] def multiVatPublishSturdyRefAsTask (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client) : RuntimeM (Task (Except IO.Error Unit)) := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatPublishSturdyRefAsTask peer objectId target

@[inline] def multiVatPublishSturdyRefAsPromise (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client) : RuntimeM (Capnp.Async.Promise Unit) := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatPublishSturdyRefAsPromise peer objectId target

@[inline] def multiVatWithPublishedSturdyRef (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) (target : Client)
    (action : RuntimeVatPeerRef -> RuntimeM α) : RuntimeM α := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatPublishSturdyRef peer objectId target
  try
    action peer
  finally
    Runtime.multiVatUnpublishSturdyRef peer objectId

@[inline] def multiVatUnpublishSturdyRef (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) : RuntimeM Unit := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatUnpublishSturdyRef peer objectId

@[inline] def multiVatUnpublishSturdyRefStart (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) : RuntimeM RuntimeUnitPromiseRef := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatUnpublishSturdyRefStart peer objectId

@[inline] def multiVatUnpublishSturdyRefAsTask (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) : RuntimeM (Task (Except IO.Error Unit)) := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatUnpublishSturdyRefAsTask peer objectId

@[inline] def multiVatUnpublishSturdyRefAsPromise (peer : RuntimeVatPeerRef)
    (objectId : ByteArray) : RuntimeM (Capnp.Async.Promise Unit) := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatUnpublishSturdyRefAsPromise peer objectId

@[inline] def multiVatClearPublishedSturdyRefs (peer : RuntimeVatPeerRef) : RuntimeM Unit := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatClearPublishedSturdyRefs peer

@[inline] def multiVatClearPublishedSturdyRefsStart (peer : RuntimeVatPeerRef) :
    RuntimeM RuntimeUnitPromiseRef := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatClearPublishedSturdyRefsStart peer

@[inline] def multiVatClearPublishedSturdyRefsAsTask (peer : RuntimeVatPeerRef) :
    RuntimeM (Task (Except IO.Error Unit)) := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatClearPublishedSturdyRefsAsTask peer

@[inline] def multiVatClearPublishedSturdyRefsAsPromise (peer : RuntimeVatPeerRef) :
    RuntimeM (Capnp.Async.Promise Unit) := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatClearPublishedSturdyRefsAsPromise peer

@[inline] def multiVatPublishedSturdyRefCount (peer : RuntimeVatPeerRef) : RuntimeM UInt64 := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatPublishedSturdyRefCount peer

@[inline] def multiVatRestoreSturdyRef (peer : RuntimeVatPeerRef)
    (sturdyRef : SturdyRef) : RuntimeM Client := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatRestoreSturdyRef peer sturdyRef

@[inline] def multiVatRestoreSturdyRefAt (peer : RuntimeVatPeerRef)
    (host : String) (objectId : ByteArray) (unique : Bool := false) : RuntimeM Client := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatRestoreSturdyRef peer (SturdyRef.ofHost host objectId unique)

@[inline] def multiVatRestoreSturdyRefStart (peer : RuntimeVatPeerRef)
    (sturdyRef : SturdyRef) : RuntimeM RuntimeRegisterPromiseRef := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatRestoreSturdyRefStart peer sturdyRef

@[inline] def multiVatRestoreSturdyRefStartAt (peer : RuntimeVatPeerRef)
    (host : String) (objectId : ByteArray) (unique : Bool := false) :
    RuntimeM RuntimeRegisterPromiseRef := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatRestoreSturdyRefStart peer (SturdyRef.ofHost host objectId unique)

@[inline] def multiVatRestoreSturdyRefAsTask (peer : RuntimeVatPeerRef)
    (sturdyRef : SturdyRef) : RuntimeM (Task (Except IO.Error Client)) := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatRestoreSturdyRefAsTask peer sturdyRef

@[inline] def multiVatRestoreSturdyRefAsTaskAt (peer : RuntimeVatPeerRef)
    (host : String) (objectId : ByteArray) (unique : Bool := false) :
    RuntimeM (Task (Except IO.Error Client)) := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatRestoreSturdyRefAsTask peer (SturdyRef.ofHost host objectId unique)

@[inline] def multiVatRestoreSturdyRefAsPromise (peer : RuntimeVatPeerRef)
    (sturdyRef : SturdyRef) : RuntimeM (Capnp.Async.Promise Client) := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatRestoreSturdyRefAsPromise peer sturdyRef

@[inline] def multiVatRestoreSturdyRefAsPromiseAt (peer : RuntimeVatPeerRef)
    (host : String) (objectId : ByteArray) (unique : Bool := false) :
    RuntimeM (Capnp.Async.Promise Client) := do
  ensureCurrentRuntime peer.runtime "RuntimeVatPeerRef"
  Runtime.multiVatRestoreSturdyRefAsPromise peer (SturdyRef.ofHost host objectId unique)

@[inline] def vatNetwork : RuntimeM VatNetwork := do
  return Runtime.vatNetwork (← runtime)

@[inline] def clientRelease (client : RuntimeClientRef) : RuntimeM Unit := do
  ensureCurrentRuntime client.runtime "RuntimeClientRef"
  client.release

@[inline] def clientBootstrap (client : RuntimeClientRef) : RuntimeM Client := do
  ensureCurrentRuntime client.runtime "RuntimeClientRef"
  client.bootstrap

@[inline] def clientOnDisconnect (client : RuntimeClientRef) : RuntimeM Unit := do
  ensureCurrentRuntime client.runtime "RuntimeClientRef"
  client.onDisconnect

@[inline] def clientOnDisconnectStart (client : RuntimeClientRef) :
    RuntimeM RuntimeUnitPromiseRef := do
  ensureCurrentRuntime client.runtime "RuntimeClientRef"
  client.onDisconnectStart

@[inline] def clientOnDisconnectAsTask (client : RuntimeClientRef) :
    RuntimeM (Task (Except IO.Error Unit)) := do
  let pending ← clientOnDisconnectStart client
  pending.awaitAsTask

@[inline] def clientOnDisconnectAsPromise (client : RuntimeClientRef) :
    RuntimeM (Capnp.Async.Promise Unit) := do
  let pending ← clientOnDisconnectStart client
  pending.toPromise

@[inline] def clientSetFlowLimit (client : RuntimeClientRef) (words : UInt64) : RuntimeM Unit := do
  ensureCurrentRuntime client.runtime "RuntimeClientRef"
  client.setFlowLimit words

@[inline] def clientQueueSize (client : RuntimeClientRef) : RuntimeM UInt64 := do
  ensureCurrentRuntime client.runtime "RuntimeClientRef"
  client.queueSize

@[inline] def clientQueueCount (client : RuntimeClientRef) : RuntimeM UInt64 := do
  ensureCurrentRuntime client.runtime "RuntimeClientRef"
  client.queueCount

@[inline] def clientOutgoingWaitNanos (client : RuntimeClientRef) : RuntimeM UInt64 := do
  ensureCurrentRuntime client.runtime "RuntimeClientRef"
  client.outgoingWaitNanos

@[inline] def targetCount : RuntimeM UInt64 := do
  Runtime.targetCount (← runtime)

@[inline] def listenerCount : RuntimeM UInt64 := do
  Runtime.listenerCount (← runtime)

@[inline] def clientCount : RuntimeM UInt64 := do
  Runtime.clientCount (← runtime)

@[inline] def serverCount : RuntimeM UInt64 := do
  Runtime.serverCount (← runtime)

@[inline] def pendingCallCount : RuntimeM UInt64 := do
  Runtime.pendingCallCount (← runtime)

@[inline] def serverRelease (server : RuntimeServerRef) : RuntimeM Unit := do
  ensureCurrentRuntime server.runtime "RuntimeServerRef"
  server.release

@[inline] def serverListen (server : RuntimeServerRef) (address : String)
    (portHint : UInt32 := 0) : RuntimeM Listener := do
  ensureCurrentRuntime server.runtime "RuntimeServerRef"
  server.listen address portHint

@[inline] def serverAccept (server : RuntimeServerRef) (listener : Listener) : RuntimeM Unit := do
  ensureCurrentRuntime server.runtime "RuntimeServerRef"
  ensureCurrentRuntimeHandle listener.runtime "Listener"
  server.accept listener

@[inline] def serverAcceptStart (server : RuntimeServerRef) (listener : Listener) :
    RuntimeM RuntimeUnitPromiseRef := do
  ensureCurrentRuntime server.runtime "RuntimeServerRef"
  ensureCurrentRuntimeHandle listener.runtime "Listener"
  server.acceptStart listener

@[inline] def serverAcceptAsTask (server : RuntimeServerRef) (listener : Listener) :
    RuntimeM (Task (Except IO.Error Unit)) := do
  let pending ← serverAcceptStart server listener
  pending.awaitAsTask

@[inline] def serverAcceptAsPromise (server : RuntimeServerRef) (listener : Listener) :
    RuntimeM (Capnp.Async.Promise Unit) := do
  let pending ← serverAcceptStart server listener
  pending.toPromise

@[inline] def serverAcceptFd (server : RuntimeServerRef) (fd : UInt32) : RuntimeM Unit := do
  ensureCurrentRuntime server.runtime "RuntimeServerRef"
  server.acceptFd fd

@[inline] def serverAcceptTransport (server : RuntimeServerRef)
    (transport : RuntimeTransport) : RuntimeM Unit := do
  ensureCurrentRuntime server.runtime "RuntimeServerRef"
  ensureCurrentRuntimeHandle transport.runtime "RuntimeTransport"
  server.acceptTransport transport

@[inline] def serverAcceptTransportFd (server : RuntimeServerRef) (fd : UInt32) : RuntimeM Unit := do
  ensureCurrentRuntime server.runtime "RuntimeServerRef"
  server.acceptTransportFd fd

@[inline] def serverDrain (server : RuntimeServerRef) : RuntimeM Unit := do
  ensureCurrentRuntime server.runtime "RuntimeServerRef"
  server.drain

@[inline] def serverDrainStart (server : RuntimeServerRef) : RuntimeM RuntimeUnitPromiseRef := do
  ensureCurrentRuntime server.runtime "RuntimeServerRef"
  server.drainStart

@[inline] def serverDrainAsTask (server : RuntimeServerRef) :
    RuntimeM (Task (Except IO.Error Unit)) := do
  let pending ← serverDrainStart server
  pending.awaitAsTask

@[inline] def serverDrainAsPromise (server : RuntimeServerRef) :
    RuntimeM (Capnp.Async.Promise Unit) := do
  let pending ← serverDrainStart server
  pending.toPromise

@[inline] def withClient (address : String)
    (action : RuntimeClientRef -> RuntimeM α) (portHint : UInt32 := 0) : RuntimeM α := do
  let client ← newClient address portHint
  try
    action client
  finally
    client.release

@[inline] def newBootstrapTarget (address : String) (portHint : UInt32 := 0) :
    RuntimeM (RuntimeClientRef × Client) := do
  Runtime.newBootstrapTarget (← runtime) address portHint

@[inline] def withBootstrapClientTarget (address : String)
    (action : RuntimeClientRef -> Client -> RuntimeM α)
    (portHint : UInt32 := 0) : RuntimeM α := do
  withClient address (fun client => do
    let target ← clientBootstrap client
    withTarget target (fun scopedTarget => action client scopedTarget)
  ) portHint

@[inline] def withBootstrapTarget (address : String)
    (action : Client -> RuntimeM α) (portHint : UInt32 := 0) : RuntimeM α := do
  withBootstrapClientTarget address (fun _ target => action target) portHint

@[inline] def withServer (bootstrap : Client)
    (action : RuntimeServerRef -> RuntimeM α) : RuntimeM α := do
  let server ← newServer bootstrap
  try
    action server
  finally
    server.release

@[inline] def withServerWithBootstrapFactory
    (bootstrapFactory : TwoPartyVatSide -> IO Client)
    (action : RuntimeServerRef -> RuntimeM α) : RuntimeM α := do
  let server ← newServerWithBootstrapFactory bootstrapFactory
  try
    action server
  finally
    server.release

@[inline] def withServerListener (server : RuntimeServerRef) (address : String)
    (action : Listener -> RuntimeM α) (portHint : UInt32 := 0) : RuntimeM α := do
  let listener ← serverListen server address portHint
  try
    action listener
  finally
    releaseListener listener

@[inline] def call (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : RuntimeM Payload := do
  Capnp.Rpc.call (← backend) target method payload

@[inline] def callOutcome (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : RuntimeM RawCallOutcome := do
  Runtime.callOutcome (← runtime) target method payload

@[inline] def callOutcomeCopy (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : RuntimeM RawCallOutcome := do
  Runtime.callOutcomeCopy (← runtime) target method payload

@[inline] def callResult (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : RuntimeM (Except RemoteException Payload) := do
  Runtime.callResult (← runtime) target method payload

@[inline] def callResultCopy (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : RuntimeM (Except RemoteException Payload) := do
  Runtime.callResultCopy (← runtime) target method payload

@[inline] def payloadRefFromBytes
    (request : ByteArray) (requestCaps : ByteArray := ByteArray.empty) :
    RuntimeM RuntimePayloadRef := do
  Runtime.payloadRefFromBytes (← runtime) request requestCaps

@[inline] def payloadRefFromBytesCopy
    (request : ByteArray) (requestCaps : ByteArray := ByteArray.empty) :
    RuntimeM RuntimePayloadRef := do
  Runtime.payloadRefFromBytesCopy (← runtime) request requestCaps

@[inline] def payloadRefFromPayload
    (payload : Payload := Capnp.emptyRpcEnvelope) : RuntimeM RuntimePayloadRef := do
  Runtime.payloadRefFromPayload (← runtime) payload

@[inline] def payloadRefToBytes (payloadRef : RuntimePayloadRef) : RuntimeM (ByteArray × ByteArray) := do
  ensureCurrentRuntime payloadRef.runtime "RuntimePayloadRef"
  Runtime.payloadRefToBytes payloadRef

@[inline] def payloadRefToBytesCopy
    (payloadRef : RuntimePayloadRef) : RuntimeM (ByteArray × ByteArray) := do
  ensureCurrentRuntime payloadRef.runtime "RuntimePayloadRef"
  Runtime.payloadRefToBytesCopy payloadRef

@[inline] def payloadRefDecode (payloadRef : RuntimePayloadRef) : RuntimeM Payload := do
  ensureCurrentRuntime payloadRef.runtime "RuntimePayloadRef"
  Runtime.payloadRefDecode payloadRef

@[inline] def payloadRefRelease (payloadRef : RuntimePayloadRef) : RuntimeM Unit := do
  ensureCurrentRuntime payloadRef.runtime "RuntimePayloadRef"
  Runtime.payloadRefRelease payloadRef

@[inline] def withPayloadRef (payloadRef : RuntimePayloadRef)
    (action : RuntimePayloadRef -> RuntimeM α) : RuntimeM α := do
  ensureCurrentRuntime payloadRef.runtime "RuntimePayloadRef"
  try
    action payloadRef
  finally
    Runtime.payloadRefRelease payloadRef

@[inline] def callWithPayloadRef (target : Client) (method : Method)
    (payloadRef : RuntimePayloadRef) : RuntimeM RuntimePayloadRef := do
  Runtime.callWithPayloadRef (← runtime) target method payloadRef

@[inline] def callPayloadRef (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : RuntimeM RuntimePayloadRef := do
  Runtime.callPayloadRef (← runtime) target method payload

@[inline] def withCallPayloadRef (target : Client) (method : Method)
    (action : RuntimePayloadRef -> RuntimeM α)
    (payload : Payload := Capnp.emptyRpcEnvelope) : RuntimeM α := do
  let responseRef ← callPayloadRef target method payload
  try
    action responseRef
  finally
    Runtime.payloadRefRelease responseRef

@[inline] def callPayloadRefDecode (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : RuntimeM Payload := do
  Runtime.callPayloadRefDecode (← runtime) target method payload

@[inline] def startCall (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : RuntimeM RuntimePendingCallRef := do
  Runtime.startCall (← runtime) target method payload

@[inline] def startCallCopy (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : RuntimeM RuntimePendingCallRef := do
  Runtime.startCallCopy (← runtime) target method payload

@[inline] def startCallWithPayloadRef (target : Client) (method : Method)
    (payloadRef : RuntimePayloadRef) : RuntimeM RuntimePendingCallRef := do
  Runtime.startCallWithPayloadRef (← runtime) target method payloadRef

@[inline] def startStreamingCallWithPayloadRef (target : Client) (method : Method)
    (payloadRef : RuntimePayloadRef) : RuntimeM RuntimePendingCallRef := do
  Runtime.startStreamingCallWithPayloadRef (← runtime) target method payloadRef

@[inline] def startCallAsTask (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) :
    RuntimeM (Task (Except IO.Error Payload)) := do
  let pending ← startCall target method payload
  pending.awaitAsTask

@[inline] def startCallAsPromise (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) :
    RuntimeM (Capnp.Async.Promise Payload) := do
  let pending ← startCall target method payload
  pending.toPromise

@[inline] def startCallWithPayloadRefAsTask (target : Client) (method : Method)
    (payloadRef : RuntimePayloadRef) :
    RuntimeM (Task (Except IO.Error RuntimePayloadRef)) := do
  let pending ← startCallWithPayloadRef target method payloadRef
  pending.awaitPayloadRefAsTask

@[inline] def startCallWithPayloadRefAsPromise (target : Client) (method : Method)
    (payloadRef : RuntimePayloadRef) :
    RuntimeM (Capnp.Async.Promise RuntimePayloadRef) := do
  let pending ← startCallWithPayloadRef target method payloadRef
  pending.toPromisePayloadRef

@[inline] def withStartedCall (target : Client) (method : Method)
    (action : RuntimePendingCallRef -> RuntimeM α)
    (payload : Payload := Capnp.emptyRpcEnvelope) : RuntimeM α := do
  let pending ← startCall target method payload
  try
    action pending
  finally
    Runtime.pendingCallRelease pending

@[inline] def startCallAwait (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : RuntimeM Payload := do
  let pending ← startCall target method payload
  Runtime.pendingCallAwait pending

@[inline] def startCallAwaitPayloadRef (target : Client) (method : Method)
    (payloadRef : RuntimePayloadRef) : RuntimeM RuntimePayloadRef := do
  let pending ← startCallWithPayloadRef target method payloadRef
  Runtime.pendingCallAwaitPayloadRef pending

@[inline] def startCallAwaitOutcome (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : RuntimeM RawCallOutcome := do
  let pending ← startCall target method payload
  Runtime.pendingCallAwaitOutcome pending

@[inline] def startCallAwaitResult (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) :
    RuntimeM (Except RemoteException Payload) := do
  let pending ← startCall target method payload
  Runtime.pendingCallAwaitResult pending

@[inline] def pendingCallAwait (pendingCall : RuntimePendingCallRef) : RuntimeM Payload := do
  ensureCurrentRuntime pendingCall.runtime "RuntimePendingCallRef"
  Runtime.pendingCallAwait pendingCall

@[inline] def pendingCallAwaitPayloadRef (pendingCall : RuntimePendingCallRef) :
    RuntimeM RuntimePayloadRef := do
  ensureCurrentRuntime pendingCall.runtime "RuntimePendingCallRef"
  Runtime.pendingCallAwaitPayloadRef pendingCall

@[inline] def pendingCallAwaitOutcome (pendingCall : RuntimePendingCallRef) :
    RuntimeM RawCallOutcome := do
  ensureCurrentRuntime pendingCall.runtime "RuntimePendingCallRef"
  Runtime.pendingCallAwaitOutcome pendingCall

@[inline] def pendingCallAwaitResult (pendingCall : RuntimePendingCallRef) :
    RuntimeM (Except RemoteException Payload) := do
  ensureCurrentRuntime pendingCall.runtime "RuntimePendingCallRef"
  Runtime.pendingCallAwaitResult pendingCall

@[inline] def pendingCallAwaitOutcomeAsTask (pendingCall : RuntimePendingCallRef) :
    RuntimeM (Task (Except IO.Error RawCallOutcome)) := do
  ensureCurrentRuntime pendingCall.runtime "RuntimePendingCallRef"
  pendingCall.awaitOutcomeAsTask

@[inline] def pendingCallAwaitPayloadRefAsTask (pendingCall : RuntimePendingCallRef) :
    RuntimeM (Task (Except IO.Error RuntimePayloadRef)) := do
  ensureCurrentRuntime pendingCall.runtime "RuntimePendingCallRef"
  pendingCall.awaitPayloadRefAsTask

@[inline] def pendingCallAwaitResultAsTask (pendingCall : RuntimePendingCallRef) :
    RuntimeM (Task (Except IO.Error (Except RemoteException Payload))) := do
  ensureCurrentRuntime pendingCall.runtime "RuntimePendingCallRef"
  pendingCall.awaitResultAsTask

@[inline] def pendingCallToPromiseOutcome (pendingCall : RuntimePendingCallRef) :
    RuntimeM (Capnp.Async.Promise RawCallOutcome) := do
  ensureCurrentRuntime pendingCall.runtime "RuntimePendingCallRef"
  pendingCall.toPromiseOutcome

@[inline] def pendingCallToPromisePayloadRef (pendingCall : RuntimePendingCallRef) :
    RuntimeM (Capnp.Async.Promise RuntimePayloadRef) := do
  ensureCurrentRuntime pendingCall.runtime "RuntimePendingCallRef"
  pendingCall.toPromisePayloadRef

@[inline] def pendingCallToPromiseResult (pendingCall : RuntimePendingCallRef) :
    RuntimeM (Capnp.Async.Promise (Except RemoteException Payload)) := do
  ensureCurrentRuntime pendingCall.runtime "RuntimePendingCallRef"
  pendingCall.toPromiseResult

@[inline] def pendingCallRelease (pendingCall : RuntimePendingCallRef) : RuntimeM Unit := do
  ensureCurrentRuntime pendingCall.runtime "RuntimePendingCallRef"
  Runtime.pendingCallRelease pendingCall

@[inline] def pendingCallReleaseDeferred (pendingCall : RuntimePendingCallRef) : RuntimeM Unit := do
  ensureCurrentRuntime pendingCall.runtime "RuntimePendingCallRef"
  Runtime.pendingCallReleaseDeferred pendingCall

@[inline] def withPendingCall (pendingCall : RuntimePendingCallRef)
    (action : RuntimePendingCallRef -> RuntimeM α) : RuntimeM α := do
  ensureCurrentRuntime pendingCall.runtime "RuntimePendingCallRef"
  try
    action pendingCall
  finally
    Runtime.pendingCallRelease pendingCall

@[inline] def pendingCallGetPipelinedCap (pendingCall : RuntimePendingCallRef)
    (pointerPath : Array UInt16 := #[]) : RuntimeM Client := do
  ensureCurrentRuntime pendingCall.runtime "RuntimePendingCallRef"
  Runtime.pendingCallGetPipelinedCap pendingCall pointerPath

@[inline] def registerPromiseAwait (promise : RuntimeRegisterPromiseRef) : RuntimeM UInt32 := do
  ensureCurrentRuntime promise.runtime "RuntimeRegisterPromiseRef"
  Runtime.registerPromiseAwait promise

@[inline] def registerPromiseCancel (promise : RuntimeRegisterPromiseRef) : RuntimeM Unit := do
  ensureCurrentRuntime promise.runtime "RuntimeRegisterPromiseRef"
  Runtime.registerPromiseCancel promise

@[inline] def registerPromiseRelease (promise : RuntimeRegisterPromiseRef) : RuntimeM Unit := do
  ensureCurrentRuntime promise.runtime "RuntimeRegisterPromiseRef"
  Runtime.registerPromiseRelease promise

@[inline] def withRegisterPromise (promise : RuntimeRegisterPromiseRef)
    (action : RuntimeRegisterPromiseRef -> RuntimeM α) : RuntimeM α := do
  ensureCurrentRuntime promise.runtime "RuntimeRegisterPromiseRef"
  try
    action promise
  finally
    Runtime.registerPromiseRelease promise

@[inline] def unitPromiseAwait (promise : RuntimeUnitPromiseRef) : RuntimeM Unit := do
  ensureCurrentRuntime promise.runtime "RuntimeUnitPromiseRef"
  Runtime.unitPromiseAwait promise

@[inline] def unitPromiseCancel (promise : RuntimeUnitPromiseRef) : RuntimeM Unit := do
  ensureCurrentRuntime promise.runtime "RuntimeUnitPromiseRef"
  Runtime.unitPromiseCancel promise

@[inline] def unitPromiseRelease (promise : RuntimeUnitPromiseRef) : RuntimeM Unit := do
  ensureCurrentRuntime promise.runtime "RuntimeUnitPromiseRef"
  Runtime.unitPromiseRelease promise

@[inline] def withUnitPromise (promise : RuntimeUnitPromiseRef)
    (action : RuntimeUnitPromiseRef -> RuntimeM α) : RuntimeM α := do
  ensureCurrentRuntime promise.runtime "RuntimeUnitPromiseRef"
  try
    action promise
  finally
    Runtime.unitPromiseRelease promise

@[inline] def streamingCall (target : Client) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) : RuntimeM Unit := do
  Runtime.streamingCall (← runtime) target method payload

@[inline] def targetGetFd? (target : Client) : RuntimeM (Option UInt32) := do
  Runtime.targetGetFd? (← runtime) target

@[inline] def targetWhenResolved (target : Client) : RuntimeM Unit := do
  Runtime.targetWhenResolved (← runtime) target

@[inline] def targetWhenResolvedStart (target : Client) : RuntimeM RuntimeUnitPromiseRef := do
  Runtime.targetWhenResolvedStart (← runtime) target

@[inline] def targetWhenResolvedPoll (target : Client) : RuntimeM Bool := do
  Runtime.targetWhenResolvedPoll (← runtime) target

@[inline] def orderingSetResolveHold (enabled : Bool) : RuntimeM Unit := do
  Runtime.orderingSetResolveHold (← runtime) enabled

@[inline] def orderingFlushResolves : RuntimeM UInt64 := do
  Runtime.orderingFlushResolves (← runtime)

@[inline] def orderingHeldResolveCount : RuntimeM UInt64 := do
  Runtime.orderingHeldResolveCount (← runtime)

@[inline] def targetWhenResolvedAsTask (target : Client) :
    RuntimeM (Task (Except IO.Error Unit)) := do
  let pending ← targetWhenResolvedStart target
  pending.awaitAsTask

@[inline] def targetWhenResolvedAsPromise (target : Client) :
    RuntimeM (Capnp.Async.Promise Unit) := do
  let pending ← targetWhenResolvedStart target
  pending.toPromise

@[inline] def pump : RuntimeM Unit := do
  Runtime.pump (← runtime)

@[inline] def enableTraceEncoder : RuntimeM Unit := do
  Runtime.enableTraceEncoder (← runtime)

@[inline] def disableTraceEncoder : RuntimeM Unit := do
  Runtime.disableTraceEncoder (← runtime)

@[inline] def setTraceEncoder (encoder : RawTraceEncoder) : RuntimeM Unit := do
  Runtime.setTraceEncoder (← runtime) encoder

@[inline] def withBackend (f : Backend -> IO α) : RuntimeM α := do
  f (← backend)

end RuntimeM

@[inline] def ffiBackend (runtime : Runtime) : Backend :=
  runtime.backend

abbrev Handler := Client -> Payload -> IO Payload

structure Route where
  method : Method
  handler : Handler

structure Dispatch where
  routes : Array Route := #[]

@[inline] def Dispatch.empty : Dispatch := { routes := #[] }

@[inline] def Dispatch.register (d : Dispatch) (method : Method) (handler : Handler) : Dispatch :=
  { routes := d.routes.push { method := method, handler := handler } }

def Dispatch.findHandler? (d : Dispatch) (method : Method) : Option Handler := Id.run do
  for route in d.routes do
    if route.method == method then
      return some route.handler
  return none

def Dispatch.toBackend (d : Dispatch)
    (onMissing : Client -> Method -> Payload -> IO Payload := fun _ _ _ => pure Capnp.emptyRpcEnvelope) :
    Backend where
  call := fun target method payload =>
    match d.findHandler? method with
    | some handler => handler target payload
    | none => onMissing target method payload

namespace Runtime

@[inline] def registerBackendTarget (runtime : Runtime) (backend : Backend) : IO Client :=
  runtime.registerHandlerTarget (fun target method payload => backend.call target method payload)

@[inline] def registerDispatchTarget (runtime : Runtime) (dispatch : Dispatch)
    (onMissing : Client -> Method -> Payload -> IO Payload := fun _ _ _ => pure Capnp.emptyRpcEnvelope) :
    IO Client :=
  runtime.registerBackendTarget (dispatch.toBackend (onMissing := onMissing))

end Runtime

namespace RuntimeM

@[inline] def registerBackendTarget (backend : Backend) : RuntimeM Client := do
  Runtime.registerBackendTarget (← runtime) backend

@[inline] def registerDispatchTarget (dispatch : Dispatch)
    (onMissing : Client -> Method -> Payload -> IO Payload := fun _ _ _ => pure Capnp.emptyRpcEnvelope) :
    RuntimeM Client := do
  Runtime.registerDispatchTarget (← runtime) dispatch (onMissing := onMissing)

end RuntimeM

namespace Interop

@[inline] def cppCallPayloadRef (runtime : Runtime) (address : String) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) (portHint : UInt32 := 0) :
    IO RuntimePayloadRef := do
  let requestBytes := payload.toBytes
  let requestCaps := payload.capTableBytes
  let (responseBytes, responseCaps) ←
    ffiCppCallOneShotImpl address portHint method.interfaceId method.methodId requestBytes requestCaps
  runtime.payloadRefFromBytes responseBytes responseCaps

@[inline] def cppCall (address : String) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) (portHint : UInt32 := 0) : IO Payload := do
  let requestBytes := payload.toBytes
  let requestCaps := payload.capTableBytes
  let (responseBytes, responseCaps) ←
    ffiCppCallOneShotImpl address portHint method.interfaceId method.methodId requestBytes requestCaps
  decodePayloadChecked responseBytes responseCaps

@[inline] def cppCallWithAcceptPayloadRef (runtime : Runtime) (server : RuntimeServerRef)
    (listener : Listener) (address : String) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) (portHint : UInt32 := 0) :
    IO RuntimePayloadRef := do
  ensureSameRuntime runtime server.runtime "RuntimeServerRef"
  ensureSameRuntimeHandle runtime listener.runtime "Listener"
  let requestBytes := payload.toBytes
  let requestCaps := payload.capTableBytes
  let (responseBytes, responseCaps) ←
    ffiRuntimeCppCallWithAcceptImpl runtime.handle server.handle.handle listener.handle
      address portHint method.interfaceId method.methodId requestBytes requestCaps
  runtime.payloadRefFromBytes responseBytes responseCaps

@[inline] def cppCallWithAccept (runtime : Runtime) (server : RuntimeServerRef) (listener : Listener)
    (address : String) (method : Method)
    (payload : Payload := Capnp.emptyRpcEnvelope) (portHint : UInt32 := 0) : IO Payload := do
  let responseRef ← cppCallWithAcceptPayloadRef runtime server listener address method payload portHint
  responseRef.decodeAndRelease

@[inline] def cppCallPipelinedWithAcceptPayloadRef (runtime : Runtime) (server : RuntimeServerRef)
    (listener : Listener) (address : String) (method : Method)
    (request : Payload := Capnp.emptyRpcEnvelope)
    (pipelinedRequest : Payload := Capnp.emptyRpcEnvelope) (portHint : UInt32 := 0) :
    IO RuntimePayloadRef := do
  ensureSameRuntime runtime server.runtime "RuntimeServerRef"
  ensureSameRuntimeHandle runtime listener.runtime "Listener"
  let requestBytes := request.toBytes
  let requestCaps := request.capTableBytes
  let pipelinedRequestBytes := pipelinedRequest.toBytes
  let pipelinedRequestCaps := pipelinedRequest.capTableBytes
  let (responseBytes, responseCaps) ← ffiRuntimeCppCallPipelinedWithAcceptImpl
    runtime.handle server.handle.handle listener.handle address portHint method.interfaceId method.methodId
    requestBytes requestCaps pipelinedRequestBytes pipelinedRequestCaps
  runtime.payloadRefFromBytes responseBytes responseCaps

@[inline] def cppCallPipelinedWithAccept (runtime : Runtime) (server : RuntimeServerRef)
    (listener : Listener) (address : String) (method : Method)
    (request : Payload := Capnp.emptyRpcEnvelope)
    (pipelinedRequest : Payload := Capnp.emptyRpcEnvelope) (portHint : UInt32 := 0) :
    IO Payload := do
  let responseRef ← cppCallPipelinedWithAcceptPayloadRef
    runtime server listener address method request pipelinedRequest portHint
  responseRef.decodeAndRelease

@[inline] def cppServeEchoOncePayloadRef (runtime : Runtime) (address : String) (method : Method)
    (portHint : UInt32 := 0) : IO RuntimePayloadRef := do
  let (requestBytes, requestCaps) ←
    ffiCppServeEchoOnceImpl address portHint method.interfaceId method.methodId
  runtime.payloadRefFromBytes requestBytes requestCaps

@[inline] def cppServeEchoOnce (address : String) (method : Method)
    (portHint : UInt32 := 0) : IO Payload := do
  let (requestBytes, requestCaps) ←
    ffiCppServeEchoOnceImpl address portHint method.interfaceId method.methodId
  decodePayloadChecked requestBytes requestCaps

@[inline] def cppServeThrowOncePayloadRef (runtime : Runtime) (address : String) (method : Method)
    (withDetail : Bool := false) (portHint : UInt32 := 0) : IO RuntimePayloadRef := do
  let detailFlag : UInt8 := if withDetail then 1 else 0
  let (requestBytes, requestCaps) ←
    ffiCppServeThrowOnceImpl address portHint method.interfaceId method.methodId detailFlag
  runtime.payloadRefFromBytes requestBytes requestCaps

@[inline] def cppServeThrowOnce (address : String) (method : Method)
    (withDetail : Bool := false) (portHint : UInt32 := 0) : IO Payload := do
  let detailFlag : UInt8 := if withDetail then 1 else 0
  let (requestBytes, requestCaps) ←
    ffiCppServeThrowOnceImpl address portHint method.interfaceId method.methodId detailFlag
  decodePayloadChecked requestBytes requestCaps

@[inline] def cppServeDelayedEchoOncePayloadRef (runtime : Runtime) (address : String) (method : Method)
    (delayMillis : UInt32) (portHint : UInt32 := 0) : IO RuntimePayloadRef := do
  let (requestBytes, requestCaps) ← ffiCppServeDelayedEchoOnceImpl
    address portHint method.interfaceId method.methodId delayMillis
  runtime.payloadRefFromBytes requestBytes requestCaps

@[inline] def cppServeDelayedEchoOnce (address : String) (method : Method)
    (delayMillis : UInt32) (portHint : UInt32 := 0) : IO Payload := do
  let (requestBytes, requestCaps) ← ffiCppServeDelayedEchoOnceImpl
    address portHint method.interfaceId method.methodId delayMillis
  decodePayloadChecked requestBytes requestCaps

@[inline] def cppCallPipelinedCapOneShotPayloadRef (runtime : Runtime) (address : String)
    (method : Method) (request : Payload := Capnp.emptyRpcEnvelope)
    (pipelinedRequest : Payload := Capnp.emptyRpcEnvelope)
    (portHint : UInt32 := 0) : IO RuntimePayloadRef := do
  let requestBytes := request.toBytes
  let requestCaps := request.capTableBytes
  let pipelinedRequestBytes := pipelinedRequest.toBytes
  let pipelinedRequestCaps := pipelinedRequest.capTableBytes
  let (responseBytes, responseCaps) ← ffiCppCallPipelinedCapOneShotImpl
    address portHint method.interfaceId method.methodId
    requestBytes requestCaps pipelinedRequestBytes pipelinedRequestCaps
  runtime.payloadRefFromBytes responseBytes responseCaps

@[inline] def cppCallPipelinedCapOneShot (address : String) (method : Method)
    (request : Payload := Capnp.emptyRpcEnvelope)
    (pipelinedRequest : Payload := Capnp.emptyRpcEnvelope)
    (portHint : UInt32 := 0) : IO Payload := do
  let requestBytes := request.toBytes
  let requestCaps := request.capTableBytes
  let pipelinedRequestBytes := pipelinedRequest.toBytes
  let pipelinedRequestCaps := pipelinedRequest.capTableBytes
  let (responseBytes, responseCaps) ← ffiCppCallPipelinedCapOneShotImpl
    address portHint method.interfaceId method.methodId
    requestBytes requestCaps pipelinedRequestBytes pipelinedRequestCaps
  decodePayloadChecked responseBytes responseCaps

end Interop

def echoBackend : Backend where
  call := fun _ _ payload => pure payload

def emptyBackend : Backend where
  call := fun _ _ _ => pure Capnp.emptyRpcEnvelope

end Rpc
end Capnp
