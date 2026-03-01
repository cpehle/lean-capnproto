#include "rpc_bridge_runtime.h"
#include "rpc_bridge_payload_ref.h"

#include <limits>
#include <stdexcept>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rpc = capnp_lean_rpc;

using rpc::copyByteArray;
using rpc::cppCallOneShot;
using rpc::cppCallPipelinedCapOneShot;
using rpc::cppServeOneShot;
using rpc::cppServeOneShotEx;
using rpc::createRuntime;
using rpc::debugLog;
using rpc::decodeCapTable;
using rpc::decodePipelineOps;
using rpc::describeKjException;
using rpc::getRuntime;
using rpc::isRuntimeAlive;
using rpc::kRuntimeDefaultMaxFdsPerMessage;
using rpc::mkByteArrayCopy;
using rpc::mkIoOkUnit;
using rpc::mkIoUserError;
using rpc::newPromiseCapabilityInline;
using rpc::promiseCapabilityFulfillInline;
using rpc::promiseCapabilityRejectInline;
using rpc::promiseCapabilityReleaseInline;
using rpc::retainByteArrayForQueue;
using rpc::shutdown;
using rpc::unregisterRuntime;
using capnp_lean_rpc_payload_ref::registerRuntimePayloadRef;
using capnp_lean_rpc_payload_ref::registerRuntimePayloadRefFromRawCallResult;
using capnp_lean_rpc_payload_ref::releaseRuntimePayloadRef;
using capnp_lean_rpc_payload_ref::releaseRuntimePayloadRefsForRuntime;
using capnp_lean_rpc_payload_ref::retainRuntimePayloadRef;

namespace {

lean_obj_res mkIoOkRawCallResult(const rpc::RawCallResult& result) {
  auto responseObj = mkByteArrayCopy(result.responseData(), result.responseSize());
  auto responseCapsObj = mkByteArrayCopy(result.responseCaps.data(), result.responseCaps.size());
  auto out = lean_alloc_ctor(0, 2, 0);
  lean_ctor_set(out, 0, responseObj);
  lean_ctor_set(out, 1, responseCapsObj);
  return lean_io_result_mk_ok(out);
}

lean_object* mkRpcDiagnosticsObj(const capnp::_::RpcSystemBase::RpcDiagnostics& diag) {
  // Mirror `Capnp.Rpc.RpcDiagnostics` (see `lean/Capnp/Rpc.lean`).
  // questionCount, answerCount, exportCount, importCount, embargoCount : UInt64
  // isIdle : Bool
  constexpr unsigned kObjFields = 0;
  constexpr unsigned kScalarBytes = 48; // 5 * 8 + 1, rounded up to 8-byte alignment

  auto out = lean_alloc_ctor(0, kObjFields, kScalarBytes);
  lean_ctor_set_uint64(out, 0, diag.questionCount);
  lean_ctor_set_uint64(out, 8, diag.answerCount);
  lean_ctor_set_uint64(out, 16, diag.exportCount);
  lean_ctor_set_uint64(out, 24, diag.importCount);
  lean_ctor_set_uint64(out, 32, diag.embargoCount);
  lean_ctor_set_uint8(out, 40, diag.isIdle ? 1 : 0);
  return out;
}

lean_object* mkProtocolMessageCountsObj(const rpc::ProtocolMessageCounts& counts) {
  // Mirror `Capnp.Rpc.ProtocolMessageCounts` (see `lean/Capnp/Rpc.lean`).
  constexpr unsigned kObjFields = 0;
  constexpr unsigned kScalarBytes = 16;

  auto out = lean_alloc_ctor(0, kObjFields, kScalarBytes);
  lean_ctor_set_uint64(out, 0, counts.resolveCount);
  lean_ctor_set_uint64(out, 8, counts.disembargoCount);
  return out;
}

lean_object* mkRemoteExceptionObj(const rpc::RawCallCompletion& completion) {
  // Mirror `Capnp.Rpc.RemoteException` (see `lean/Capnp/Rpc.lean`).
  constexpr unsigned kObjFields = 4;  // description, remoteTrace, detail, fileName
  constexpr unsigned kScalarBytes = 8;  // 4 (line) + 1 (tag) + padding
  constexpr unsigned kScalarStart = sizeof(void*) * kObjFields;

  auto ex = lean_alloc_ctor(0, kObjFields, kScalarBytes);
  lean_ctor_set(ex, 0, lean_mk_string(completion.exceptionDescription.c_str()));
  lean_ctor_set(ex, 1, lean_mk_string(completion.remoteTrace.c_str()));
  const auto detailSize = completion.detailBytes.size();
  const auto* detailData = detailSize == 0 ? nullptr : completion.detailBytes.data();
  lean_ctor_set(ex, 2, mkByteArrayCopy(detailData, detailSize));
  lean_ctor_set(ex, 3, lean_mk_string(completion.fileName.c_str()));

  lean_ctor_set_uint32(ex, kScalarStart, completion.lineNumber);
  lean_ctor_set_uint8(ex, kScalarStart + 4, completion.exceptionTypeTag);
  return ex;
}

lean_object* mkRawCallOutcomeOkObj(const rpc::RawCallResult& result) {
  // Mirror `Capnp.Rpc.RawCallOutcome.ok`.
  auto responseObj = mkByteArrayCopy(result.responseData(), result.responseSize());
  auto responseCapsObj = mkByteArrayCopy(result.responseCaps.data(), result.responseCaps.size());
  auto out = lean_alloc_ctor(0, 2, 0);
  lean_ctor_set(out, 0, responseObj);
  lean_ctor_set(out, 1, responseCapsObj);
  return out;
}

lean_object* mkRawCallOutcomeErrorObj(const rpc::RawCallCompletion& completion) {
  // Mirror `Capnp.Rpc.RawCallOutcome.error`.
  auto ex = mkRemoteExceptionObj(completion);
  auto out = lean_alloc_ctor(1, 1, 0);
  lean_ctor_set(out, 0, ex);
  return out;
}

}  // namespace
extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_raw_call_on_runtime(
    uint64_t runtimeId, uint32_t target, uint64_t interfaceId, uint16_t methodId,
    b_lean_obj_arg request) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueRawCall(*runtime, target, interfaceId, methodId,
                                          retainByteArrayForQueue(request), {});

    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }

    const auto responseSize = completion->result.responseSize();
    const auto* responseData = completion->result.responseData();
    return lean_io_result_mk_ok(mkByteArrayCopy(responseData, responseSize));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_raw_call_on_runtime");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_raw_call_with_caps_on_runtime(
    uint64_t runtimeId, uint32_t target, uint64_t interfaceId, uint16_t methodId,
    b_lean_obj_arg request, b_lean_obj_arg requestCaps) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    debugLog(
        "ffi.rawcall_with_caps.enter",
        "runtime=" + std::to_string(runtimeId) + " target=" + std::to_string(target) +
            " interfaceId=" + std::to_string(interfaceId) + " methodId=" +
            std::to_string(methodId));

    const auto requestCapsSize = lean_sarray_size(requestCaps);
    const auto* requestCapsData = lean_sarray_cptr(const_cast<lean_object*>(requestCaps));
    auto requestCapIds = decodeCapTable(requestCapsData, requestCapsSize);

    auto completion =
        rpc::enqueueRawCall(*runtime, target, interfaceId, methodId,
                            retainByteArrayForQueue(request), std::move(requestCapIds));
    debugLog("ffi.rawcall_with_caps.enqueued", "waiting");
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    debugLog("ffi.rawcall_with_caps.done", "ok");

    const auto responseSize = completion->result.responseSize();
    const auto* responseData = completion->result.responseData();
    const auto responseCapsSize = completion->result.responseCaps.size();
    const auto* responseCapsData =
        responseCapsSize == 0 ? nullptr : completion->result.responseCaps.data();

    lean_object* resultTuple = lean_alloc_ctor(0, 2, 0);
    lean_ctor_set(resultTuple, 0, mkByteArrayCopy(responseData, responseSize));
    lean_ctor_set(resultTuple, 1, mkByteArrayCopy(responseCapsData, responseCapsSize));
    return lean_io_result_mk_ok(resultTuple);
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_raw_call_with_caps_on_runtime");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_raw_call_with_caps_on_runtime_outcome(
    uint64_t runtimeId, uint32_t target, uint64_t interfaceId, uint16_t methodId,
    b_lean_obj_arg request, b_lean_obj_arg requestCaps) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    const auto requestCapsSize = lean_sarray_size(requestCaps);
    const auto* requestCapsData = lean_sarray_cptr(const_cast<lean_object*>(requestCaps));
    auto requestCapIds = decodeCapTable(requestCapsData, requestCapsSize);

    auto completion =
        rpc::enqueueRawCall(*runtime, target, interfaceId, methodId,
                            retainByteArrayForQueue(request), std::move(requestCapIds));
    lean_object* outcomeObj = nullptr;
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (completion->ok) {
        outcomeObj = mkRawCallOutcomeOkObj(completion->result);
      } else {
        outcomeObj = mkRawCallOutcomeErrorObj(*completion);
      }
    }
    return lean_io_result_mk_ok(outcomeObj);
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_raw_call_with_caps_on_runtime_outcome");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_start_call_with_caps(
    uint64_t runtimeId, uint32_t target, uint64_t interfaceId, uint16_t methodId,
    b_lean_obj_arg request, b_lean_obj_arg requestCaps) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto requestCapIds = decodeCapTable(requestCaps);
    auto completion =
        rpc::enqueueStartPendingCall(*runtime, target, interfaceId, methodId,
                                     retainByteArrayForQueue(request), std::move(requestCapIds));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_start_call_with_caps");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_start_streaming_call_with_caps(
    uint64_t runtimeId, uint32_t target, uint64_t interfaceId, uint16_t methodId,
    b_lean_obj_arg request, b_lean_obj_arg requestCaps) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto requestCapIds = decodeCapTable(requestCaps);
    auto completion =
        rpc::enqueueStartStreamingPendingCall(*runtime, target, interfaceId, methodId,
                                              retainByteArrayForQueue(request), std::move(requestCapIds));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_start_streaming_call_with_caps");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_payload_ref_from_bytes(
    uint64_t runtimeId, b_lean_obj_arg request, b_lean_obj_arg requestCaps) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    uint32_t payloadRefId =
        registerRuntimePayloadRef(runtimeId, const_cast<lean_object*>(request),
                                  const_cast<lean_object*>(requestCaps), true);
    return lean_io_result_mk_ok(lean_box_uint32(payloadRefId));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_payload_ref_from_bytes");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_payload_ref_to_bytes(
    uint64_t runtimeId, uint32_t payloadRefId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto retained = retainRuntimePayloadRef(payloadRefId);
    KJ_IF_SOME(payloadRef, retained) {
      if (payloadRef.runtimeId != runtimeId) {
        return mkIoUserError("runtime payload ref belongs to a different Capnp.Rpc runtime");
      }

      lean_object* out = lean_alloc_ctor(0, 2, 0);
      if (payloadRef.hasLeanBytes()) {
        lean_ctor_set(out, 0, payloadRef.takeMessageBytes());
        lean_ctor_set(out, 1, payloadRef.takeCapBytes());
      } else {
        lean_ctor_set(out, 0, mkByteArrayCopy(payloadRef.messageData(), payloadRef.messageSize()));
        lean_ctor_set(out, 1, mkByteArrayCopy(payloadRef.capData(), payloadRef.capSize()));
      }
      return lean_io_result_mk_ok(out);
    } else {
      return mkIoUserError("unknown runtime payload ref id");
    }
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_payload_ref_to_bytes");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_payload_ref_release(
    uint64_t runtimeId, uint32_t payloadRefId) {
  try {
    std::string error;
    if (!releaseRuntimePayloadRef(runtimeId, payloadRefId, error)) {
      return mkIoUserError(error);
    }
    return mkIoOkUnit();
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_payload_ref_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_call_with_payload_ref(
    uint64_t runtimeId, uint32_t target, uint64_t interfaceId, uint16_t methodId,
    uint32_t payloadRefId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto retained = retainRuntimePayloadRef(payloadRefId);
    KJ_IF_SOME(payloadRef, retained) {
      if (payloadRef.runtimeId != runtimeId) {
        return mkIoUserError("runtime payload ref belongs to a different Capnp.Rpc runtime");
      }

      auto requestCaps = decodeCapTable(payloadRef.capData(), payloadRef.capSize());
      std::shared_ptr<rpc::RawCallCompletion> completion;
      if (payloadRef.hasLeanBytes()) {
        completion = rpc::enqueueRawCall(*runtime, target, interfaceId, methodId,
                                         retainByteArrayForQueue(payloadRef.messageBytes),
                                         std::move(requestCaps));
      } else if (payloadRef.hasRawBytes()) {
        completion = rpc::enqueueRawCallData(*runtime, target, interfaceId, methodId,
                                             payloadRef.messageData(), payloadRef.messageSize(),
                                             payloadRef.rawOwner(), std::move(requestCaps));
      } else {
        return mkIoUserError("runtime payload ref has no payload bytes");
      }
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        uint32_t responseRefId =
            registerRuntimePayloadRefFromRawCallResult(runtimeId, std::move(completion->result));
        return lean_io_result_mk_ok(lean_box_uint32(responseRefId));
      }
    } else {
      return mkIoUserError("unknown runtime payload ref id");
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_call_with_payload_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_start_call_with_payload_ref(
    uint64_t runtimeId, uint32_t target, uint64_t interfaceId, uint16_t methodId,
    uint32_t payloadRefId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto retained = retainRuntimePayloadRef(payloadRefId);
    KJ_IF_SOME(payloadRef, retained) {
      if (payloadRef.runtimeId != runtimeId) {
        return mkIoUserError("runtime payload ref belongs to a different Capnp.Rpc runtime");
      }

      auto requestCaps = decodeCapTable(payloadRef.capData(), payloadRef.capSize());
      std::shared_ptr<rpc::RegisterTargetCompletion> completion;
      if (payloadRef.hasLeanBytes()) {
        completion = rpc::enqueueStartPendingCall(
            *runtime, target, interfaceId, methodId,
            retainByteArrayForQueue(payloadRef.messageBytes), std::move(requestCaps));
      } else if (payloadRef.hasRawBytes()) {
        completion = rpc::enqueueStartPendingCallData(
            *runtime, target, interfaceId, methodId, payloadRef.messageData(),
            payloadRef.messageSize(), payloadRef.rawOwner(), std::move(requestCaps));
      } else {
        return mkIoUserError("runtime payload ref has no payload bytes");
      }
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
      }
    } else {
      return mkIoUserError("unknown runtime payload ref id");
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_start_call_with_payload_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_start_streaming_call_with_payload_ref(
    uint64_t runtimeId, uint32_t target, uint64_t interfaceId, uint16_t methodId,
    uint32_t payloadRefId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto retained = retainRuntimePayloadRef(payloadRefId);
    KJ_IF_SOME(payloadRef, retained) {
      if (payloadRef.runtimeId != runtimeId) {
        return mkIoUserError("runtime payload ref belongs to a different Capnp.Rpc runtime");
      }

      auto requestCaps = decodeCapTable(payloadRef.capData(), payloadRef.capSize());
      std::shared_ptr<rpc::RegisterTargetCompletion> completion;
      if (payloadRef.hasLeanBytes()) {
        completion = rpc::enqueueStartStreamingPendingCall(
            *runtime, target, interfaceId, methodId,
            retainByteArrayForQueue(payloadRef.messageBytes), std::move(requestCaps));
      } else if (payloadRef.hasRawBytes()) {
        completion = rpc::enqueueStartStreamingPendingCallData(
            *runtime, target, interfaceId, methodId, payloadRef.messageData(),
            payloadRef.messageSize(), payloadRef.rawOwner(), std::move(requestCaps));
      } else {
        return mkIoUserError("runtime payload ref has no payload bytes");
      }
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
      }
    } else {
      return mkIoUserError("unknown runtime payload ref id");
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_start_streaming_call_with_payload_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_pending_call_await(
    uint64_t runtimeId, uint32_t pendingCallId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueAwaitPendingCall(*runtime, pendingCallId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    return mkIoOkRawCallResult(completion->result);
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_pending_call_await");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_pending_call_await_payload_ref(
    uint64_t runtimeId, uint32_t pendingCallId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueAwaitPendingCall(*runtime, pendingCallId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      uint32_t payloadRefId =
          registerRuntimePayloadRefFromRawCallResult(runtimeId, std::move(completion->result));
      return lean_io_result_mk_ok(lean_box_uint32(payloadRefId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_pending_call_await_payload_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_pending_call_await_outcome(
    uint64_t runtimeId, uint32_t pendingCallId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueAwaitPendingCall(*runtime, pendingCallId);
    lean_object* outcomeObj = nullptr;
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (completion->ok) {
        outcomeObj = mkRawCallOutcomeOkObj(completion->result);
      } else {
        outcomeObj = mkRawCallOutcomeErrorObj(*completion);
      }
    }
    return lean_io_result_mk_ok(outcomeObj);
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_pending_call_await_outcome");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_pending_call_release(
    uint64_t runtimeId, uint32_t pendingCallId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueReleasePendingCall(*runtime, pendingCallId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_pending_call_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_pending_call_get_pipelined_cap(
    uint64_t runtimeId, uint32_t pendingCallId, b_lean_obj_arg pipelineOps) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto pointerPath = decodePipelineOps(pipelineOps);
    auto completion =
        rpc::enqueueGetPipelinedCap(*runtime, pendingCallId, std::move(pointerPath));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_pending_call_get_pipelined_cap");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_register_promise_await(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueAwaitRegisterPromise(*runtime, promiseId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_register_promise_await");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_register_promise_cancel(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueCancelRegisterPromise(*runtime, promiseId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_register_promise_cancel");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_register_promise_release(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueReleaseRegisterPromise(*runtime, promiseId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_register_promise_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_unit_promise_await(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueAwaitUnitPromise(*runtime, promiseId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_unit_promise_await");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_unit_promise_cancel(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueCancelUnitPromise(*runtime, promiseId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_unit_promise_cancel");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_unit_promise_release(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueReleaseUnitPromise(*runtime, promiseId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_unit_promise_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_pump(uint64_t runtimeId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueuePump(*runtime);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_pump");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_pump_async(uint64_t runtimeId,
                                                                      b_lean_obj_arg promise) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto* prom = const_cast<lean_object*>(promise);
    lean_mark_mt(prom);
    lean_inc(prom);
    rpc::enqueuePumpAsync(*runtime, prom);
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_pump_async");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_streaming_call_with_caps(
    uint64_t runtimeId, uint32_t target, uint64_t interfaceId, uint16_t methodId,
    b_lean_obj_arg request, b_lean_obj_arg requestCaps) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto requestCapIds = decodeCapTable(requestCaps);
    auto completion =
        rpc::enqueueStreamingCall(*runtime, target, interfaceId, methodId,
                                  retainByteArrayForQueue(request), std::move(requestCapIds));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_streaming_call_with_caps");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_target_get_fd(
    uint64_t runtimeId, uint32_t target) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueTargetGetFd(*runtime, target);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      uint32_t out = completion->value < 0 ? std::numeric_limits<uint32_t>::max()
                                           : static_cast<uint32_t>(completion->value);
      return lean_io_result_mk_ok(lean_box_uint32(out));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_target_get_fd");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_target_when_resolved(
    uint64_t runtimeId, uint32_t target) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueTargetWhenResolved(*runtime, target);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_target_when_resolved");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_target_when_resolved_start(
    uint64_t runtimeId, uint32_t target) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueTargetWhenResolvedStart(*runtime, target);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_target_when_resolved_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_enable_trace_encoder(
    uint64_t runtimeId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueEnableTraceEncoder(*runtime);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_enable_trace_encoder");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_disable_trace_encoder(
    uint64_t runtimeId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueDisableTraceEncoder(*runtime);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_disable_trace_encoder");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_set_trace_encoder(
    uint64_t runtimeId, b_lean_obj_arg encoder) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueSetTraceEncoder(*runtime, encoder);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_set_trace_encoder");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_release_target(
    uint64_t runtimeId, uint32_t target) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    if (rpc::isWorkerThread(*runtime)) {
      rpc::releaseTargetInline(*runtime, target);
      lean_obj_res ok;
      mkIoOkUnit(ok);
      return ok;
    }
    auto completion = rpc::enqueueReleaseTarget(*runtime, target);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }

    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_release_target");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_release_targets(
    uint64_t runtimeId, b_lean_obj_arg targets) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    if (lean_sarray_size(targets) == 0) {
      lean_obj_res ok;
      mkIoOkUnit(ok);
      return ok;
    }
    auto targetIds = decodeCapTable(targets);
    if (rpc::isWorkerThread(*runtime)) {
      rpc::releaseTargetsInline(*runtime, targetIds);
      lean_obj_res ok;
      mkIoOkUnit(ok);
      return ok;
    }
    auto completion = rpc::enqueueReleaseTargets(*runtime, std::move(targetIds));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }

    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_release_targets");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_retain_target(
    uint64_t runtimeId, uint32_t target) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    if (rpc::isWorkerThread(*runtime)) {
      return lean_io_result_mk_ok(lean_box_uint32(rpc::retainTargetInline(*runtime, target)));
    }
    auto completion = rpc::enqueueRetainTarget(*runtime, target);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_retain_target");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_new_promise_capability(
    uint64_t runtimeId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    if (rpc::isWorkerThread(*runtime)) {
      auto ids = newPromiseCapabilityInline(*runtime);
      lean_object* resultTuple = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(resultTuple, 0, lean_box_uint32(ids.first));
      lean_ctor_set(resultTuple, 1, lean_box_uint32(ids.second));
      return lean_io_result_mk_ok(resultTuple);
    }
    auto completion = rpc::enqueueNewPromiseCapability(*runtime);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      lean_object* resultTuple = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(resultTuple, 0, lean_box_uint32(completion->first));
      lean_ctor_set(resultTuple, 1, lean_box_uint32(completion->second));
      return lean_io_result_mk_ok(resultTuple);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_new_promise_capability");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_promise_capability_fulfill(
    uint64_t runtimeId, uint32_t fulfillerId, uint32_t target) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    debugLog("ffi.promise_cap.fulfill.enter",
             "runtime=" + std::to_string(runtimeId) + " fulfiller=" + std::to_string(fulfillerId) +
                 " target=" + std::to_string(target));
    if (rpc::isWorkerThread(*runtime)) {
      promiseCapabilityFulfillInline(*runtime, fulfillerId, target);
      lean_obj_res ok;
      mkIoOkUnit(ok);
      debugLog("ffi.promise_cap.fulfill.done", "inline ok");
      return ok;
    }
    auto completion = rpc::enqueuePromiseCapabilityFulfill(*runtime, fulfillerId, target);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      lean_obj_res ok;
      mkIoOkUnit(ok);
      debugLog("ffi.promise_cap.fulfill.done", "queued ok");
      return ok;
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_promise_capability_fulfill");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_promise_capability_reject(
    uint64_t runtimeId, uint32_t fulfillerId, uint8_t exceptionTypeTag, b_lean_obj_arg message,
    b_lean_obj_arg detail) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    std::string messageCopy = lean_string_cstr(message);
    if (rpc::isWorkerThread(*runtime)) {
      auto detailBytes = copyByteArray(detail);
      promiseCapabilityRejectInline(*runtime, fulfillerId, exceptionTypeTag, std::move(messageCopy),
                                    std::move(detailBytes));
      lean_obj_res ok;
      mkIoOkUnit(ok);
      return ok;
    }
    auto completion = rpc::enqueuePromiseCapabilityReject(
        *runtime, fulfillerId, exceptionTypeTag, std::move(messageCopy),
        retainByteArrayForQueue(detail));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      lean_obj_res ok;
      mkIoOkUnit(ok);
      return ok;
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_promise_capability_reject");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_promise_capability_release(
    uint64_t runtimeId, uint32_t fulfillerId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    if (rpc::isWorkerThread(*runtime)) {
      promiseCapabilityReleaseInline(*runtime, fulfillerId);
      lean_obj_res ok;
      mkIoOkUnit(ok);
      return ok;
    }
    auto completion = rpc::enqueuePromiseCapabilityRelease(*runtime, fulfillerId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      lean_obj_res ok;
      mkIoOkUnit(ok);
      return ok;
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_promise_capability_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_connect(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    std::string addressCopy = lean_string_cstr(address);
    auto completion = rpc::enqueueConnectTarget(*runtime, std::move(addressCopy), portHint);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_connect");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_connect_start(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    std::string addressCopy = lean_string_cstr(address);
    auto completion = rpc::enqueueConnectTargetStart(*runtime, std::move(addressCopy), portHint);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_connect_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_connect_fd(
    uint64_t runtimeId, uint32_t fd) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueConnectTargetFd(*runtime, fd);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_connect_fd");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_new_transport_pipe(uint64_t runtimeId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueNewTransportPipe(*runtime);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      lean_object* resultTuple = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(resultTuple, 0, lean_box_uint32(completion->first));
      lean_ctor_set(resultTuple, 1, lean_box_uint32(completion->second));
      return lean_io_result_mk_ok(resultTuple);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_new_transport_pipe");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_new_transport_from_fd(
    uint64_t runtimeId, uint32_t fd) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueNewTransportFromFd(*runtime, fd);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_new_transport_from_fd");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_new_transport_from_fd_take(
    uint64_t runtimeId, uint32_t fd) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueNewTransportFromFdTake(*runtime, fd);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_new_transport_from_fd_take");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_release_transport(
    uint64_t runtimeId, uint32_t transportId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueReleaseTransport(*runtime, transportId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_release_transport");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_transport_get_fd(
    uint64_t runtimeId, uint32_t transportId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueTransportGetFd(*runtime, transportId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      uint32_t out = completion->value < 0 ? std::numeric_limits<uint32_t>::max()
                                           : static_cast<uint32_t>(completion->value);
      return lean_io_result_mk_ok(lean_box_uint32(out));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_transport_get_fd");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_connect_transport(
    uint64_t runtimeId, uint32_t transportId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueConnectTargetTransport(*runtime, transportId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_connect_transport");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_listen_echo(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    std::string addressCopy = lean_string_cstr(address);
    auto completion = rpc::enqueueListenLoopback(*runtime, std::move(addressCopy), portHint);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_listen_echo");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_accept_echo(
    uint64_t runtimeId, uint32_t listenerId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueAcceptLoopback(*runtime, listenerId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }

    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_accept_echo");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_release_listener(
    uint64_t runtimeId, uint32_t listenerId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueReleaseListener(*runtime, listenerId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }

    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_release_listener");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_new_client(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    std::string addressCopy = lean_string_cstr(address);
    auto completion = rpc::enqueueNewClient(*runtime, std::move(addressCopy), portHint);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_new_client");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_new_client_start(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    std::string addressCopy = lean_string_cstr(address);
    auto completion = rpc::enqueueNewClientStart(*runtime, std::move(addressCopy), portHint);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_new_client_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_release_client(
    uint64_t runtimeId, uint32_t clientId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueReleaseClient(*runtime, clientId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }

    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_release_client");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_client_bootstrap(
    uint64_t runtimeId, uint32_t clientId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueClientBootstrap(*runtime, clientId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_client_bootstrap");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_client_on_disconnect(
    uint64_t runtimeId, uint32_t clientId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueClientOnDisconnect(*runtime, clientId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }

    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_client_on_disconnect");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_client_on_disconnect_start(
    uint64_t runtimeId, uint32_t clientId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueClientOnDisconnectStart(*runtime, clientId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_client_on_disconnect_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_client_set_flow_limit(
    uint64_t runtimeId, uint32_t clientId, uint64_t words) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueClientSetFlowLimit(*runtime, clientId, words);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }

    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_client_set_flow_limit");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_client_queue_size(
    uint64_t runtimeId, uint32_t clientId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueClientQueueSize(*runtime, clientId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint64(completion->value));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_client_queue_size");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_client_queue_count(
    uint64_t runtimeId, uint32_t clientId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueClientQueueCount(*runtime, clientId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint64(completion->value));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_client_queue_count");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_client_outgoing_wait_nanos(
    uint64_t runtimeId, uint32_t clientId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueClientOutgoingWaitNanos(*runtime, clientId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint64(completion->value));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_client_outgoing_wait_nanos");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_target_count(uint64_t runtimeId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueTargetCount(*runtime);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint64(completion->value));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_target_count");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_listener_count(uint64_t runtimeId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueListenerCount(*runtime);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint64(completion->value));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_listener_count");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_client_count(uint64_t runtimeId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueClientCount(*runtime);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint64(completion->value));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_client_count");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_server_count(uint64_t runtimeId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueServerCount(*runtime);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint64(completion->value));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_server_count");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_pending_call_count(
    uint64_t runtimeId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueuePendingCallCount(*runtime);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint64(completion->value));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_pending_call_count");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_new_server(
    uint64_t runtimeId, uint32_t bootstrapTarget) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueNewServer(*runtime, bootstrapTarget);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_new_server");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_new_server_with_bootstrap_factory(
    uint64_t runtimeId, b_lean_obj_arg bootstrapFactory) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueNewServerWithBootstrapFactory(*runtime, bootstrapFactory);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_new_server_with_bootstrap_factory");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_release_server(
    uint64_t runtimeId, uint32_t serverId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueReleaseServer(*runtime, serverId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }

    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_release_server");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_server_listen(
    uint64_t runtimeId, uint32_t serverId, b_lean_obj_arg address, uint32_t portHint) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    std::string addressCopy = lean_string_cstr(address);
    auto completion = rpc::enqueueServerListen(*runtime, serverId, std::move(addressCopy), portHint);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_server_listen");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_server_accept(
    uint64_t runtimeId, uint32_t serverId, uint32_t listenerId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueServerAccept(*runtime, serverId, listenerId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }

    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_server_accept");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_server_accept_start(
    uint64_t runtimeId, uint32_t serverId, uint32_t listenerId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueServerAcceptStart(*runtime, serverId, listenerId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_server_accept_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_server_accept_fd(
    uint64_t runtimeId, uint32_t serverId, uint32_t fd) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueServerAcceptFd(*runtime, serverId, fd);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_server_accept_fd");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_server_accept_transport(
    uint64_t runtimeId, uint32_t serverId, uint32_t transportId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueServerAcceptTransport(*runtime, serverId, transportId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_server_accept_transport");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_server_drain(
    uint64_t runtimeId, uint32_t serverId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueServerDrain(*runtime, serverId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }

    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_server_drain");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_server_drain_start(
    uint64_t runtimeId, uint32_t serverId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueServerDrainStart(*runtime, serverId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_server_drain_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_new_with_fd_limit(
    uint32_t maxFdsPerMessage) {
  try {
    uint64_t runtimeId = createRuntime(maxFdsPerMessage);
    return lean_io_result_mk_ok(lean_box_uint64(runtimeId));
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_new_with_fd_limit");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_new() {
  return capnp_lean_rpc_runtime_new_with_fd_limit(kRuntimeDefaultMaxFdsPerMessage);
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_release(uint64_t runtimeId) {
  try {
    auto runtime = getRuntime(runtimeId);
    if (runtime && rpc::isWorkerThread(*runtime)) {
      return mkIoUserError(
          "Capnp.Rpc runtime shutdown is not allowed from the Capnp.Rpc worker thread");
    }

    auto unregisteredRuntime = unregisterRuntime(runtimeId);
    if (unregisteredRuntime) {
      rpc::shutdown(*unregisteredRuntime);
    }
    releaseRuntimePayloadRefsForRuntime(runtimeId);

    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_is_alive(uint64_t runtimeId) {
  return lean_io_result_mk_ok(lean_box(isRuntimeAlive(runtimeId) ? 1 : 0));
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_register_echo_target(
    uint64_t runtimeId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueRegisterLoopbackTarget(*runtime);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_register_echo_target");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_register_loopback_target(
    uint64_t runtimeId, uint32_t bootstrapTarget) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueRegisterLoopbackTarget(*runtime, bootstrapTarget);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_register_loopback_target");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_register_handler_target(
    uint64_t runtimeId, b_lean_obj_arg handler) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueRegisterHandlerTarget(*runtime, handler);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_register_handler_target");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_register_advanced_handler_target(
    uint64_t runtimeId, b_lean_obj_arg handler) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueRegisterAdvancedHandlerTarget(*runtime, handler);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_register_advanced_handler_target");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_register_tailcall_handler_target(
    uint64_t runtimeId, b_lean_obj_arg handler) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueRegisterTailCallHandlerTarget(*runtime, handler);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_register_tailcall_handler_target");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_register_tailcall_target(
    uint64_t runtimeId, uint32_t target) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueRegisterTailCallTarget(*runtime, target);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_register_tailcall_target");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_register_fd_target(
    uint64_t runtimeId, uint32_t fd) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = rpc::enqueueRegisterFdTarget(*runtime, fd);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_register_fd_target");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_new_client(
    uint64_t runtimeId, b_lean_obj_arg name) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatNewClient(*runtime, std::string(lean_string_cstr(name)));
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_multivat_new_client");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_new_server(
    uint64_t runtimeId, b_lean_obj_arg name, uint32_t bootstrapTarget) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatNewServer(*runtime, std::string(lean_string_cstr(name)),
                                                        bootstrapTarget);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_multivat_new_server");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_rpc_runtime_multivat_new_server_with_bootstrap_factory(
    uint64_t runtimeId, b_lean_obj_arg name, b_lean_obj_arg bootstrapFactory) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatNewServerWithBootstrapFactory(*runtime, 
        std::string(lean_string_cstr(name)), bootstrapFactory);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_multivat_new_server_with_bootstrap_factory");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_release_peer(
    uint64_t runtimeId, uint32_t peerId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatReleasePeer(*runtime, peerId);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_multivat_release_peer");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_bootstrap(
    uint64_t runtimeId, uint32_t sourcePeerId, b_lean_obj_arg host, uint8_t unique) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatBootstrap(*runtime, 
        sourcePeerId, std::string(lean_string_cstr(host)), unique != 0);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_multivat_bootstrap");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_bootstrap_peer(
    uint64_t runtimeId, uint32_t sourcePeerId, uint32_t targetPeerId, uint8_t unique) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion =
        rpc::enqueueMultiVatBootstrapPeer(*runtime, sourcePeerId, targetPeerId, unique != 0);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_multivat_bootstrap_peer");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_set_forwarding_enabled(
    uint64_t runtimeId, uint8_t enabled) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatSetForwardingEnabled(*runtime, enabled != 0);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_multivat_set_forwarding_enabled");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_reset_forwarding_stats(
    uint64_t runtimeId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatResetForwardingStats(*runtime);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_multivat_reset_forwarding_stats");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_forward_count(
    uint64_t runtimeId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatForwardCount(*runtime);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    return lean_io_result_mk_ok(lean_box_uint64(completion->value));
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_multivat_forward_count");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_third_party_token_count(
    uint64_t runtimeId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatThirdPartyTokenCount(*runtime);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    return lean_io_result_mk_ok(lean_box_uint64(completion->value));
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_multivat_third_party_token_count");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_denied_forward_count(
    uint64_t runtimeId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatDeniedForwardCount(*runtime);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    return lean_io_result_mk_ok(lean_box_uint64(completion->value));
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_multivat_denied_forward_count");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_has_connection(
    uint64_t runtimeId, uint32_t fromPeerId, uint32_t toPeerId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatHasConnection(*runtime, fromPeerId, toPeerId);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    return lean_io_result_mk_ok(lean_box(completion->value != 0 ? 1 : 0));
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_multivat_has_connection");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_set_restorer(
    uint64_t runtimeId, uint32_t peerId, b_lean_obj_arg restorer) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatSetRestorer(*runtime, peerId, restorer);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_multivat_set_restorer");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_clear_restorer(
    uint64_t runtimeId, uint32_t peerId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatClearRestorer(*runtime, peerId);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_multivat_clear_restorer");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_publish_sturdy_ref(
    uint64_t runtimeId, uint32_t hostPeerId, b_lean_obj_arg objectId, uint32_t targetId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatPublishSturdyRef(
        *runtime, hostPeerId, retainByteArrayForQueue(objectId), targetId);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_multivat_publish_sturdy_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_publish_sturdy_ref_start(
    uint64_t runtimeId, uint32_t hostPeerId, b_lean_obj_arg objectId, uint32_t targetId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatPublishSturdyRefStart(
        *runtime, hostPeerId, retainByteArrayForQueue(objectId), targetId);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_multivat_publish_sturdy_ref_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_unpublish_sturdy_ref(
    uint64_t runtimeId, uint32_t hostPeerId, b_lean_obj_arg objectId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatUnpublishSturdyRef(
        *runtime, hostPeerId, retainByteArrayForQueue(objectId));
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_multivat_unpublish_sturdy_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_unpublish_sturdy_ref_start(
    uint64_t runtimeId, uint32_t hostPeerId, b_lean_obj_arg objectId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatUnpublishSturdyRefStart(
        *runtime, hostPeerId, retainByteArrayForQueue(objectId));
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_multivat_unpublish_sturdy_ref_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_rpc_runtime_multivat_clear_published_sturdy_refs(uint64_t runtimeId,
                                                             uint32_t hostPeerId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatClearPublishedSturdyRefs(*runtime, hostPeerId);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_multivat_clear_published_sturdy_refs");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_rpc_runtime_multivat_clear_published_sturdy_refs_start(uint64_t runtimeId,
                                                                   uint32_t hostPeerId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatClearPublishedSturdyRefsStart(*runtime, hostPeerId);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_multivat_clear_published_sturdy_refs_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_rpc_runtime_multivat_published_sturdy_ref_count(uint64_t runtimeId,
                                                            uint32_t hostPeerId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatPublishedSturdyRefCount(*runtime, hostPeerId);
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    return lean_io_result_mk_ok(lean_box_uint64(completion->value));
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_multivat_published_sturdy_ref_count");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_restore_sturdy_ref(
    uint64_t runtimeId, uint32_t sourcePeerId, b_lean_obj_arg host, uint8_t unique,
    b_lean_obj_arg objectId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatRestoreSturdyRef(
        *runtime, sourcePeerId, std::string(lean_string_cstr(host)), unique != 0,
        retainByteArrayForQueue(objectId));
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_multivat_restore_sturdy_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_restore_sturdy_ref_start(
    uint64_t runtimeId, uint32_t sourcePeerId, b_lean_obj_arg host, uint8_t unique,
    b_lean_obj_arg objectId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }
  try {
    auto completion = rpc::enqueueMultiVatRestoreSturdyRefStart(
        *runtime, sourcePeerId, std::string(lean_string_cstr(host)), unique != 0,
        retainByteArrayForQueue(objectId));
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion]() { return completion->done; });
    if (!completion->ok) {
      return mkIoUserError(completion->error);
    }
    return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_multivat_restore_sturdy_ref_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_cpp_call_one_shot(
    b_lean_obj_arg address, uint32_t portHint, uint64_t interfaceId, uint16_t methodId,
    b_lean_obj_arg request, b_lean_obj_arg requestCaps) {
  try {
    auto addressCopy = std::string(lean_string_cstr(address));
    auto requestBytes = copyByteArray(request);
    auto requestCapIds = decodeCapTable(requestCaps);
    auto result =
        cppCallOneShot(addressCopy, portHint, interfaceId, methodId, requestBytes, requestCapIds);
    return mkIoOkRawCallResult(result);
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_cpp_call_one_shot");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_cpp_serve_echo_once(
    b_lean_obj_arg address, uint32_t portHint, uint64_t interfaceId, uint16_t methodId) {
  try {
    auto result = cppServeOneShot(std::string(lean_string_cstr(address)), portHint, interfaceId,
                                   methodId);
    return mkIoOkRawCallResult(result);
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_cpp_serve_echo_once");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_cpp_serve_throw_once(
    b_lean_obj_arg address, uint32_t portHint, uint64_t interfaceId, uint16_t methodId,
    uint8_t withDetail) {
  try {
    auto result = cppServeOneShotEx(std::string(lean_string_cstr(address)), portHint, interfaceId,
                                    methodId, 0, true, withDetail != 0, true);
    return mkIoOkRawCallResult(result);
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_cpp_serve_throw_once");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_cpp_serve_delayed_echo_once(
    b_lean_obj_arg address, uint32_t portHint, uint64_t interfaceId, uint16_t methodId,
    uint32_t delayMillis) {
  try {
    auto result = cppServeOneShotEx(std::string(lean_string_cstr(address)), portHint, interfaceId,
                                    methodId, delayMillis, false, false, false);
    return mkIoOkRawCallResult(result);
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_cpp_serve_delayed_echo_once");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_cpp_call_pipelined_cap_one_shot(
    b_lean_obj_arg address, uint32_t portHint, uint64_t interfaceId, uint16_t methodId,
    b_lean_obj_arg request, b_lean_obj_arg requestCaps, b_lean_obj_arg pipelinedRequest,
    b_lean_obj_arg pipelinedRequestCaps) {
  try {
    auto addressCopy = std::string(lean_string_cstr(address));
    auto requestBytes = copyByteArray(request);
    auto requestCapIds = decodeCapTable(requestCaps);
    auto pipelinedRequestBytes = copyByteArray(pipelinedRequest);
    auto pipelinedRequestCapIds = decodeCapTable(pipelinedRequestCaps);
    auto result = cppCallPipelinedCapOneShot(
        addressCopy, portHint, interfaceId, methodId, requestBytes, requestCapIds,
        pipelinedRequestBytes, pipelinedRequestCapIds);
    return mkIoOkRawCallResult(result);
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_cpp_call_pipelined_cap_one_shot");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_cpp_call_with_accept(
    uint64_t runtimeId, uint32_t serverId, uint32_t listenerId, b_lean_obj_arg address,
    uint32_t portHint, uint64_t interfaceId, uint16_t methodId, b_lean_obj_arg request,
    b_lean_obj_arg requestCaps) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto addressCopy = std::string(lean_string_cstr(address));
    auto requestCapIds = decodeCapTable(requestCaps);
    auto completion = rpc::enqueueCppCallWithAccept(
        *runtime, serverId, listenerId, std::move(addressCopy), portHint, interfaceId, methodId,
        retainByteArrayForQueue(request), std::move(requestCapIds));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    return mkIoOkRawCallResult(completion->result);
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_runtime_cpp_call_with_accept");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_cpp_call_pipelined_with_accept(
    uint64_t runtimeId, uint32_t serverId, uint32_t listenerId, b_lean_obj_arg address,
    uint32_t portHint, uint64_t interfaceId, uint16_t methodId, b_lean_obj_arg request,
    b_lean_obj_arg requestCaps, b_lean_obj_arg pipelinedRequest,
    b_lean_obj_arg pipelinedRequestCaps) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto addressCopy = std::string(lean_string_cstr(address));
    auto requestCapIds = decodeCapTable(requestCaps);
    auto pipelinedRequestCapIds = decodeCapTable(pipelinedRequestCaps);
    auto completion = rpc::enqueueCppCallPipelinedWithAccept(
        *runtime, serverId, listenerId, std::move(addressCopy), portHint, interfaceId, methodId,
        retainByteArrayForQueue(request), std::move(requestCapIds),
        retainByteArrayForQueue(pipelinedRequest), std::move(pipelinedRequestCapIds));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
    }
    return mkIoOkRawCallResult(completion->result);
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_cpp_call_pipelined_with_accept");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_test_new_socketpair() {
#if defined(_WIN32)
  return mkIoUserError("capnp_lean_rpc_test_new_socketpair is not supported on Windows");
#else
  try {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
      throw std::runtime_error("socketpair() failed in capnp_lean_rpc_test_new_socketpair");
    }
    auto pair = lean_alloc_ctor(0, 2, 0);
    lean_ctor_set(pair, 0, lean_box_uint32(static_cast<uint32_t>(fds[0])));
    lean_ctor_set(pair, 1, lean_box_uint32(static_cast<uint32_t>(fds[1])));
    return lean_io_result_mk_ok(pair);
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_test_new_socketpair");
  }
#endif
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_test_new_listen_socket_fd() {
#if defined(_WIN32)
  return mkIoUserError("capnp_lean_rpc_test_new_listen_socket_fd is not supported on Windows");
#else
  int fd = -1;
  try {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      throw std::runtime_error("socket() failed in capnp_lean_rpc_test_new_listen_socket_fd");
    }

    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      throw std::runtime_error("bind() failed in capnp_lean_rpc_test_new_listen_socket_fd");
    }
    if (listen(fd, SOMAXCONN) != 0) {
      throw std::runtime_error("listen() failed in capnp_lean_rpc_test_new_listen_socket_fd");
    }

    sockaddr_in bound;
    std::memset(&bound, 0, sizeof(bound));
    socklen_t boundLen = sizeof(bound);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
      throw std::runtime_error("getsockname() failed in capnp_lean_rpc_test_new_listen_socket_fd");
    }

    auto pair = lean_alloc_ctor(0, 2, 0);
    lean_ctor_set(pair, 0, lean_box_uint32(static_cast<uint32_t>(fd)));
    lean_ctor_set(pair, 1, lean_box_uint32(static_cast<uint32_t>(ntohs(bound.sin_port))));
    return lean_io_result_mk_ok(pair);
  } catch (const std::exception& e) {
    if (fd >= 0) {
      close(fd);
    }
    return mkIoUserError(e.what());
  } catch (...) {
    if (fd >= 0) {
      close(fd);
    }
    return mkIoUserError("unknown exception in capnp_lean_rpc_test_new_listen_socket_fd");
  }
#endif
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_test_new_datagram_socket_fd() {
#if defined(_WIN32)
  return mkIoUserError("capnp_lean_rpc_test_new_datagram_socket_fd is not supported on Windows");
#else
  int fd = -1;
  try {
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
      throw std::runtime_error("socket() failed in capnp_lean_rpc_test_new_datagram_socket_fd");
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      throw std::runtime_error("bind() failed in capnp_lean_rpc_test_new_datagram_socket_fd");
    }

    sockaddr_in bound;
    std::memset(&bound, 0, sizeof(bound));
    socklen_t boundLen = sizeof(bound);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
      throw std::runtime_error("getsockname() failed in capnp_lean_rpc_test_new_datagram_socket_fd");
    }

    auto pair = lean_alloc_ctor(0, 2, 0);
    lean_ctor_set(pair, 0, lean_box_uint32(static_cast<uint32_t>(fd)));
    lean_ctor_set(pair, 1, lean_box_uint32(static_cast<uint32_t>(ntohs(bound.sin_port))));
    return lean_io_result_mk_ok(pair);
  } catch (const std::exception& e) {
    if (fd >= 0) {
      close(fd);
    }
    return mkIoUserError(e.what());
  } catch (...) {
    if (fd >= 0) {
      close(fd);
    }
    return mkIoUserError("unknown exception in capnp_lean_rpc_test_new_datagram_socket_fd");
  }
#endif
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_connection_block(
    uint64_t runtimeId, uint32_t fromPeerId, uint32_t toPeerId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) return mkIoUserError("Capnp.Rpc runtime handle is invalid");
  try {
    auto completion = rpc::enqueueMultiVatConnectionBlock(*runtime, fromPeerId, toPeerId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) return mkIoUserError(completion->error);
      return mkIoOkUnit();
    }
  } catch (...) { return mkIoUserError("unknown exception in multiVatConnectionBlock"); }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_connection_unblock(
    uint64_t runtimeId, uint32_t fromPeerId, uint32_t toPeerId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) return mkIoUserError("Capnp.Rpc runtime handle is invalid");
  try {
    auto completion = rpc::enqueueMultiVatConnectionUnblock(*runtime, fromPeerId, toPeerId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) return mkIoUserError(completion->error);
      return mkIoOkUnit();
    }
  } catch (...) { return mkIoUserError("unknown exception in multiVatConnectionUnblock"); }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_connection_disconnect(
    uint64_t runtimeId, uint32_t fromPeerId, uint32_t toPeerId, uint8_t exceptionTypeTag,
    b_lean_obj_arg message, b_lean_obj_arg detailBytes) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) return mkIoUserError("Capnp.Rpc runtime handle is invalid");
  try {
    auto completion = rpc::enqueueMultiVatConnectionDisconnect(
        *runtime, fromPeerId, toPeerId, exceptionTypeTag,
        std::string(lean_string_cstr(message)),
        rpc::copyByteArray(detailBytes));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) return mkIoUserError(completion->error);
      return mkIoOkUnit();
    }
  } catch (...) { return mkIoUserError("unknown exception in multiVatConnectionDisconnect"); }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_rpc_runtime_multivat_connection_resolve_disembargo_counts(
    uint64_t runtimeId, uint32_t fromPeerId, uint32_t toPeerId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) return mkIoUserError("Capnp.Rpc runtime handle is invalid");
  try {
    auto completion =
        rpc::enqueueMultiVatConnectionResolveDisembargoCounts(*runtime, fromPeerId, toPeerId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) return mkIoUserError(completion->error);
      return lean_io_result_mk_ok(mkProtocolMessageCountsObj(completion->value));
    }
  } catch (...) {
    return mkIoUserError("unknown exception in multiVatConnectionResolveDisembargoCounts");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_rpc_runtime_multivat_connection_resolve_disembargo_trace(
    uint64_t runtimeId, uint32_t fromPeerId, uint32_t toPeerId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) return mkIoUserError("Capnp.Rpc runtime handle is invalid");
  try {
    auto completion =
        rpc::enqueueMultiVatConnectionResolveDisembargoTrace(*runtime, fromPeerId, toPeerId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) return mkIoUserError(completion->error);
      return lean_io_result_mk_ok(mkByteArrayCopy(completion->value.data(), completion->value.size()));
    }
  } catch (...) {
    return mkIoUserError("unknown exception in multiVatConnectionResolveDisembargoTrace");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_rpc_runtime_multivat_connection_reset_resolve_disembargo_trace(
    uint64_t runtimeId, uint32_t fromPeerId, uint32_t toPeerId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) return mkIoUserError("Capnp.Rpc runtime handle is invalid");
  try {
    auto completion = rpc::enqueueMultiVatConnectionResetResolveDisembargoTrace(
        *runtime, fromPeerId, toPeerId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) return mkIoUserError(completion->error);
      return mkIoOkUnit();
    }
  } catch (...) {
    return mkIoUserError("unknown exception in multiVatConnectionResetResolveDisembargoTrace");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_multivat_get_diagnostics(
    uint64_t runtimeId, uint32_t peerId, b_lean_obj_arg targetVatId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) return mkIoUserError("Capnp.Rpc runtime handle is invalid");
  try {
    auto completion = rpc::enqueueMultiVatGetDiagnostics(*runtime, peerId, targetVatId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) return mkIoUserError(completion->error);
      return lean_io_result_mk_ok(mkRpcDiagnosticsObj(completion->value));
    }
  } catch (...) { return mkIoUserError("unknown exception in multiVatGetDiagnostics"); }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_register_fd_probe_target(
    uint64_t runtimeId) {
  auto runtime = getRuntime(runtimeId);
  if (!runtime) return mkIoUserError("Capnp.Rpc runtime handle is invalid");
  try {
    auto completion = rpc::enqueueRegisterFdProbeTarget(*runtime);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) return mkIoUserError(completion->error);
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }
  } catch (...) { return mkIoUserError("unknown exception in registerFdProbeTarget"); }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_test_verify_rpc_diagnostics_layout(b_lean_obj_arg d) {
  uint64_t questionCount = lean_ctor_get_uint64(d, 0);
  uint64_t answerCount = lean_ctor_get_uint64(d, 8);
  uint64_t exportCount = lean_ctor_get_uint64(d, 16);
  uint64_t importCount = lean_ctor_get_uint64(d, 24);
  uint64_t embargoCount = lean_ctor_get_uint64(d, 32);
  uint8_t isIdle = lean_ctor_get_uint8(d, 40);

  bool ok = true;
  ok &= (questionCount == 0x0101010101010101);
  ok &= (answerCount   == 0x0202020202020202);
  ok &= (exportCount   == 0x0303030303030303);
  ok &= (importCount   == 0x0404040404040404);
  ok &= (embargoCount  == 0x0505050505050505);
  ok &= (isIdle        == 1);

  return lean_io_result_mk_ok(lean_box(ok));
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_test_verify_remote_exception_layout(b_lean_obj_arg e) {
  constexpr unsigned kObjFields = 4;
  constexpr unsigned kScalarStart = sizeof(void*) * kObjFields;

  lean_object* description = lean_ctor_get(e, 0);
  lean_object* remoteTrace = lean_ctor_get(e, 1);
  lean_object* detail = lean_ctor_get(e, 2);
  lean_object* fileName = lean_ctor_get(e, 3);
  
  uint32_t lineNumber = lean_ctor_get_uint32(e, kScalarStart);
  uint8_t type = lean_ctor_get_uint8(e, kScalarStart + 4);

  bool ok = true;
  ok &= (std::string(lean_string_cstr(description)) == "description");
  ok &= (std::string(lean_string_cstr(remoteTrace)) == "trace");
  ok &= (lean_sarray_size(detail) == 3);
  if (ok) ok &= (lean_sarray_cptr(detail)[0] == 1);
  ok &= (std::string(lean_string_cstr(fileName)) == "file.cpp");
  ok &= (lineNumber == 123);
  ok &= (type == 2); 

  return lean_io_result_mk_ok(lean_box(ok));
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_test_close_fd(uint32_t fd) {
#if defined(_WIN32)
  (void)fd;
  return mkIoUserError("capnp_lean_rpc_test_close_fd is not supported on Windows");
#else
  try {
    constexpr uint32_t maxInt = static_cast<uint32_t>(std::numeric_limits<int>::max());
    if (fd > maxInt) {
      throw std::runtime_error("fd exceeds platform int range");
    }
    if (close(static_cast<int>(fd)) != 0) {
      throw std::runtime_error("close() failed in capnp_lean_rpc_test_close_fd");
    }
    lean_obj_res ok;
    mkIoOkUnit(ok);
    return ok;
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_rpc_test_close_fd");
  }
#endif
}
