#include "rpc_bridge_common.h"
#include <cstring>
#include <stdexcept>

namespace capnp_lean_rpc {

std::atomic<uint64_t> gNextRuntimeId{1};

lean_obj_res mkByteArrayCopy(const uint8_t* data, size_t size) {
  lean_object* out = lean_alloc_sarray(1, size, size);
  if (size != 0) {
    std::memcpy(lean_sarray_cptr(out), data, size);
  }
  lean_sarray_set_size(out, size);
  return out;
}

lean_obj_res mkIoUserError(const std::string& message) {
  lean_object* msg = lean_mk_string(message.c_str());
  lean_object* err = lean_mk_io_user_error(msg);
  return lean_io_result_mk_error(err);
}

void mkIoOkUnit(lean_obj_res& out) { out = lean_io_result_mk_ok(lean_box(0)); }

const char* kjExceptionTypeName(kj::Exception::Type type) {
  switch (type) {
    case kj::Exception::Type::FAILED:
      return "FAILED";
    case kj::Exception::Type::OVERLOADED:
      return "OVERLOADED";
    case kj::Exception::Type::DISCONNECTED:
      return "DISCONNECTED";
    case kj::Exception::Type::UNIMPLEMENTED:
      return "UNIMPLEMENTED";
    default:
      return "OTHER";
  }
}

std::string describeKjException(const kj::Exception& e) {
  std::string message(e.getDescription().cStr());
  message += "\nexception type: ";
  message += kjExceptionTypeName(e.getType());
  auto remoteTrace = e.getRemoteTrace();
  if (remoteTrace != nullptr) {
    message += "\nremote trace: ";
    message += remoteTrace.cStr();
  }
  KJ_IF_SOME(detail, e.getDetail(1)) {
    message += "\nremote detail[1]: ";
    message.append(reinterpret_cast<const char*>(detail.begin()), detail.size());
  }
  return message;
}

uint32_t readUint32Le(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

uint16_t readUint16Le(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

uint64_t readUint64Le(const uint8_t* data) {
  return static_cast<uint64_t>(data[0]) |
         (static_cast<uint64_t>(data[1]) << 8) |
         (static_cast<uint64_t>(data[2]) << 16) |
         (static_cast<uint64_t>(data[3]) << 24) |
         (static_cast<uint64_t>(data[4]) << 32) |
         (static_cast<uint64_t>(data[5]) << 40) |
         (static_cast<uint64_t>(data[6]) << 48) |
         (static_cast<uint64_t>(data[7]) << 56);
}

void appendUint32Le(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

void appendUint16Le(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void appendUint64Le(std::vector<uint8_t>& out, uint64_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 32) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 40) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 48) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 56) & 0xff));
}

std::vector<uint32_t> decodeCapTable(const uint8_t* data, size_t size) {
  if ((size % 4) != 0) {
    throw std::runtime_error("RPC capability table payload must be a multiple of 4 bytes");
  }
  std::vector<uint32_t> caps;
  caps.reserve(size / 4);
  for (size_t i = 0; i < size; i += 4) {
    caps.push_back(readUint32Le(data + i));
  }
  return caps;
}

std::vector<uint32_t> decodeCapTable(b_lean_obj_arg bytes) {
  const auto size = lean_sarray_size(bytes);
  const auto* data =
      reinterpret_cast<const uint8_t*>(lean_sarray_cptr(const_cast<lean_object*>(bytes)));
  return decodeCapTable(data, size);
}

std::vector<uint16_t> decodePipelineOps(const uint8_t* data, size_t size) {
  if ((size % 2) != 0) {
    throw std::runtime_error("RPC pipeline ops payload must be a multiple of 2 bytes");
  }
  std::vector<uint16_t> ops;
  ops.reserve(size / 2);
  for (size_t i = 0; i < size; i += 2) {
    ops.push_back(readUint16Le(data + i));
  }
  return ops;
}

std::vector<uint16_t> decodePipelineOps(b_lean_obj_arg bytes) {
  const auto size = lean_sarray_size(bytes);
  const auto* data =
      reinterpret_cast<const uint8_t*>(lean_sarray_cptr(const_cast<lean_object*>(bytes)));
  return decodePipelineOps(data, size);
}

std::vector<uint8_t> copyByteArray(b_lean_obj_arg bytes) {
  const auto size = lean_sarray_size(bytes);
  const auto* data =
      reinterpret_cast<const uint8_t*>(lean_sarray_cptr(const_cast<lean_object*>(bytes)));
  std::vector<uint8_t> out(size);
  if (size != 0) {
    std::memcpy(out.data(), data, size);
  }
  return out;
}

DeferredLeanTaskState::DeferredLeanTaskState(kj::Own<DeferredLeanTask>&& waitTask,
                                             kj::Own<DeferredLeanTask>&& cancelTask,
                                             bool allowCancellation)
    : waitTask(kj::mv(waitTask)),
      cancelTask(kj::mv(cancelTask)),
      allowCancellation(allowCancellation) {}

bool DeferredLeanTaskState::requestCancellation() {
  if (!allowCancellation || completed.load(std::memory_order_acquire)) {
    return false;
  }
  if (cancellationRequested.exchange(true, std::memory_order_acq_rel)) {
    return false;
  }
  lean_io_cancel_core(cancelTask->task);
  if (waitTask.get() != cancelTask.get()) {
    lean_io_cancel_core(waitTask->task);
  }
  return true;
}

DeferredLeanTaskState::~DeferredLeanTaskState() { requestCancellation(); }

void completeSuccess(const std::shared_ptr<RawCallCompletion>& completion, RawCallResult result) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->result = std::move(result);
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeFailure(const std::shared_ptr<RawCallCompletion>& completion, std::string message) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = false;
    completion->error = std::move(message);
    completion->exceptionTypeTag = static_cast<uint8_t>(kj::Exception::Type::FAILED);
    completion->exceptionDescription = completion->error;
    completion->remoteTrace.clear();
    completion->detailBytes.clear();
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeFailureKj(const std::shared_ptr<RawCallCompletion>& completion,
                       const kj::Exception& e) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = false;
    completion->error = describeKjException(e);
    completion->exceptionTypeTag = static_cast<uint8_t>(e.getType());
    completion->exceptionDescription = std::string(e.getDescription().cStr());
    auto remoteTrace = e.getRemoteTrace();
    if (remoteTrace != nullptr) {
      completion->remoteTrace = std::string(remoteTrace.cStr());
    } else {
      completion->remoteTrace.clear();
    }
    completion->detailBytes.clear();
    KJ_IF_SOME(detail, e.getDetail(1)) {
      completion->detailBytes.assign(detail.begin(), detail.end());
    }
    completion->fileName = e.getFile();
    completion->lineNumber = e.getLine();
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeRegisterSuccess(const std::shared_ptr<RegisterTargetCompletion>& completion,
                             uint32_t targetId) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->targetId = targetId;
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeRegisterFailure(const std::shared_ptr<RegisterTargetCompletion>& completion,
                             std::string message) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = false;
    completion->error = std::move(message);
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeUnitSuccess(const std::shared_ptr<UnitCompletion>& completion) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeUnitFailure(const std::shared_ptr<UnitCompletion>& completion, std::string message) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = false;
    completion->error = std::move(message);
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeUInt64Success(const std::shared_ptr<UInt64Completion>& completion, uint64_t value) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->value = value;
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeUInt64Failure(const std::shared_ptr<UInt64Completion>& completion,
                           std::string message) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = false;
    completion->error = std::move(message);
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeBoolSuccess(const std::shared_ptr<BoolCompletion>& completion, bool value) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->value = value;
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeBoolFailure(const std::shared_ptr<BoolCompletion>& completion,
                         std::string message) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = false;
    completion->error = std::move(message);
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeOptionalStringSuccess(const std::shared_ptr<OptionalStringCompletion>& completion,
                                   kj::Maybe<std::string> value) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    KJ_IF_SOME(v, value) {
      completion->hasValue = true;
      completion->value = v;
    } else {
      completion->hasValue = false;
      completion->value.clear();
    }
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeOptionalStringFailure(const std::shared_ptr<OptionalStringCompletion>& completion,
                                   std::string message) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = false;
    completion->error = std::move(message);
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeInt64Success(const std::shared_ptr<Int64Completion>& completion, int64_t value) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->value = value;
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeInt64Failure(const std::shared_ptr<Int64Completion>& completion,
                          std::string message) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = false;
    completion->error = std::move(message);
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeByteArraySuccess(const std::shared_ptr<ByteArrayCompletion>& completion,
                              std::vector<uint8_t> value) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->value = std::move(value);
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeByteArrayFailure(const std::shared_ptr<ByteArrayCompletion>& completion,
                              std::string message) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = false;
    completion->error = std::move(message);
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeRegisterPairSuccess(const std::shared_ptr<RegisterPairCompletion>& completion,
                                 uint32_t first, uint32_t second) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->first = first;
    completion->second = second;
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeRegisterPairFailure(const std::shared_ptr<RegisterPairCompletion>& completion,
                                 std::string message) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = false;
    completion->error = std::move(message);
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeKjPromiseIdSuccess(const std::shared_ptr<KjPromiseIdCompletion>& completion,
                                uint32_t promiseId) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->promiseId = promiseId;
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeKjPromiseIdFailure(const std::shared_ptr<KjPromiseIdCompletion>& completion,
                                std::string message) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = false;
    completion->error = std::move(message);
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeDiagnosticsSuccess(const std::shared_ptr<DiagnosticsCompletion>& completion,
                                capnp::_::RpcSystemBase::RpcDiagnostics value) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->value = value;
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeDiagnosticsFailure(const std::shared_ptr<DiagnosticsCompletion>& completion,
                                std::string message) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = false;
    completion->error = std::move(message);
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeProtocolMessageCountsSuccess(
    const std::shared_ptr<ProtocolMessageCountsCompletion>& completion,
    ProtocolMessageCounts value) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->value = value;
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeProtocolMessageCountsFailure(
    const std::shared_ptr<ProtocolMessageCountsCompletion>& completion,
    std::string message) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = false;
    completion->error = std::move(message);
    completion->done = true;
  }
  completion->cv.notify_one();
}

extern "C" lean_obj_res lean_io_promise_resolve(lean_obj_arg, lean_obj_arg);

void completeAsyncUnitSuccess(const std::shared_ptr<AsyncUnitCompletion>& completion) {
  // Except.ok ()
  lean_object* except = lean_alloc_ctor(0, 1, 0);
  lean_ctor_set(except, 0, lean_box(0));
  lean_io_promise_resolve(except, completion->promise);
}

void completeAsyncUnitFailure(const std::shared_ptr<AsyncUnitCompletion>& completion, std::string message) {
  // Resolve with Except.error (handled by Task completion in Lean)
  // Actually, IO.Promise expects the type α.
  // If the promise is IO.Promise (Except IO.Error Unit), we need to resolve with the Except.
  // I will use lean_mk_io_user_error + lean_io_result_mk_error pattern if needed, 
  // but Lean Promises usually resolve with the raw value.
  
  // Assuming Promise (Except IO.Error Unit):
  // .error is constructor 1 of Except.
  lean_object* msg = lean_mk_string(message.c_str());
  lean_object* err = lean_mk_io_user_error(msg);
  lean_object* except = lean_alloc_ctor(1, 1, 0);
  lean_ctor_set(except, 0, err);
  
  lean_io_promise_resolve(except, completion->promise);
}

} // namespace capnp_lean_rpc
