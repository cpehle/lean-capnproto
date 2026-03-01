#pragma once

#include <lean/lean.h>
#include <kj/common.h>
#include <kj/exception.h>
#include <capnp/rpc-prelude.h>
#include <vector>
#include <string>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>

namespace capnp_lean_rpc {

inline constexpr uint32_t kRuntimeDefaultMaxFdsPerMessage = 16;

inline bool debugEnabled() {
  static bool enabled = []() {
    const char* flag = std::getenv("CAPNP_LEAN_RPC_DEBUG");
    return flag != nullptr && flag[0] != '\0';
  }();
  return enabled;
}

inline void debugLog(const char* label, const std::string& message) {
  if (!debugEnabled()) return;
  std::fprintf(stderr, "[capnp-lean-rpc] %s: %s\n", label, message.c_str());
  std::fflush(stderr);
}

// Lean object helpers
lean_obj_res mkByteArrayCopy(const uint8_t* data, size_t size);
lean_obj_res mkIoUserError(const std::string& message);
void mkIoOkUnit(lean_obj_res& out);
inline lean_obj_res mkIoOkUnit() { lean_obj_res out; mkIoOkUnit(out); return out; }

// KJ Exception helpers
const char* kjExceptionTypeName(kj::Exception::Type type);
std::string describeKjException(const kj::Exception& e);

// Serialization helpers
uint32_t readUint32Le(const uint8_t* data);
uint16_t readUint16Le(const uint8_t* data);
uint64_t readUint64Le(const uint8_t* data);
void appendUint16Le(std::vector<uint8_t>& out, uint16_t value);
void appendUint32Le(std::vector<uint8_t>& out, uint32_t value);
void appendUint64Le(std::vector<uint8_t>& out, uint64_t value);

// CapTable and PipelineOps decoding
std::vector<uint32_t> decodeCapTable(const uint8_t* data, size_t size);
std::vector<uint32_t> decodeCapTable(b_lean_obj_arg bytes);
std::vector<uint16_t> decodePipelineOps(const uint8_t* data, size_t size);
std::vector<uint16_t> decodePipelineOps(b_lean_obj_arg bytes);
std::vector<uint8_t> copyByteArray(b_lean_obj_arg bytes);

// Move-only retained Lean ByteArray reference.
struct LeanByteArrayRef {
  LeanByteArrayRef() = default;

  explicit LeanByteArrayRef(lean_object* bytes): bytes_(bytes) {
    if (bytes_ != nullptr) {
      lean_inc(bytes_);
    }
  }

  LeanByteArrayRef(const LeanByteArrayRef&) = delete;
  LeanByteArrayRef& operator=(const LeanByteArrayRef&) = delete;

  LeanByteArrayRef(LeanByteArrayRef&& other) noexcept : bytes_(other.bytes_) {
    other.bytes_ = nullptr;
  }

  LeanByteArrayRef& operator=(LeanByteArrayRef&& other) noexcept {
    if (this != &other) {
      reset();
      bytes_ = other.bytes_;
      other.bytes_ = nullptr;
    }
    return *this;
  }

  ~LeanByteArrayRef() { reset(); }

  void reset() {
    if (bytes_ != nullptr) {
      lean_dec(bytes_);
      bytes_ = nullptr;
    }
  }

  size_t size() const {
    if (bytes_ == nullptr) {
      return 0;
    }
    return lean_sarray_size(bytes_);
  }

  bool empty() const { return size() == 0; }

  const uint8_t* data() const {
    if (bytes_ == nullptr) {
      return nullptr;
    }
    return reinterpret_cast<const uint8_t*>(lean_sarray_cptr(bytes_));
  }

 private:
  lean_object* bytes_ = nullptr;
};

inline LeanByteArrayRef retainByteArrayForQueue(b_lean_obj_arg bytes) {
  auto* bytesObj = const_cast<lean_object*>(bytes);
  if (bytesObj != nullptr) {
    lean_mark_mt(bytesObj);
  }
  return LeanByteArrayRef(bytesObj);
}

// Deferred Task structures
struct DeferredLeanTask {
  explicit DeferredLeanTask(lean_object* task): task(task) {}
  ~DeferredLeanTask() { lean_dec(task); }
  lean_object* task;
};

struct DeferredLeanTaskState {
  DeferredLeanTaskState(kj::Own<DeferredLeanTask>&& waitTask,
                        kj::Own<DeferredLeanTask>&& cancelTask,
                        bool allowCancellation);
  ~DeferredLeanTaskState();

  bool requestCancellation();

  kj::Own<DeferredLeanTask> waitTask;
  kj::Own<DeferredLeanTask> cancelTask;
  bool allowCancellation;
  std::atomic<bool> completed = false;
  std::atomic<bool> cancellationRequested = false;
};

// Advanced handler structures
struct LeanAdvancedHandlerAction {
  enum class Kind : uint8_t {
    RETURN_PAYLOAD = 0,
    ASYNC_CALL = 1,
    TAIL_CALL = 2,
    THROW_REMOTE = 3,
    AWAIT_TASK = 4
  };

  Kind kind = Kind::RETURN_PAYLOAD;
  bool releaseParams = false;
  bool allowCancellation = false;
  bool isStreaming = false;
  bool sendResultsToCaller = false;
  bool noPromisePipelining = false;
  bool onlyPromisePipeline = false;
  bool hasPipeline = false;
  uint32_t target = 0;
  uint64_t interfaceId = 0;
  uint16_t methodId = 0;
  LeanByteArrayRef pipelineBytes;
  LeanByteArrayRef pipelineCaps;
  LeanByteArrayRef payloadBytes;
  LeanByteArrayRef payloadCaps;
  std::string message;
  kj::Exception::Type remoteExceptionType = kj::Exception::Type::FAILED;
  LeanByteArrayRef detailBytes;
  kj::Own<DeferredLeanTask> deferredWaitTask;
  kj::Own<DeferredLeanTask> deferredCancelTask;
};

// Completion structures used for synchronization between worker and Lean threads
struct RawCallResult {
  kj::Array<capnp::word> responseWords;
  std::vector<uint8_t> responseCaps;

  inline kj::ArrayPtr<const kj::byte> responseBytes() const { return responseWords.asBytes(); }

  inline size_t responseSize() const { return responseWords.asBytes().size(); }

  inline const uint8_t* responseData() const {
    auto bytes = responseWords.asBytes();
    return bytes.size() == 0 ? nullptr : reinterpret_cast<const uint8_t*>(bytes.begin());
  }
};

struct RawCallCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string error;
  uint8_t exceptionTypeTag = 0;
  std::string exceptionDescription;
  std::string remoteTrace;
  std::vector<uint8_t> detailBytes;
  std::string fileName;
  uint32_t lineNumber = 0;
  RawCallResult result;
};

struct RegisterTargetCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string error;
  uint32_t targetId = 0;
};

struct UnitCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string error;
};

struct UInt64Completion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string error;
  uint64_t value = 0;
};

struct BoolCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string error;
  bool value = false;
};

struct OptionalStringCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string error;
  bool hasValue = false;
  std::string value;
};

struct Int64Completion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string error;
  int64_t value = 0;
};

struct ByteArrayCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string error;
  std::vector<uint8_t> value;
};

struct RegisterPairCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string error;
  uint32_t first = 0;
  uint32_t second = 0;
};

struct KjPromiseIdCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string error;
  uint32_t promiseId = 0;
};

struct DiagnosticsCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string error;
  capnp::_::RpcSystemBase::RpcDiagnostics value;
};

struct ProtocolMessageCounts {
  uint64_t resolveCount = 0;
  uint64_t disembargoCount = 0;
};

struct ProtocolMessageCountsCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string error;
  ProtocolMessageCounts value;
};

struct AsyncUnitCompletion {
  explicit AsyncUnitCompletion(lean_object* promise): promise(promise) {}
  ~AsyncUnitCompletion() { lean_dec(promise); }
  lean_object* promise;
};

class RuntimeLoop;

// Shared runtime id generator used by both the RPC and KjAsync test runtimes to avoid collisions
// when a single process creates both kinds of runtimes.
extern std::atomic<uint64_t> gNextRuntimeId;

void completeSuccess(const std::shared_ptr<RawCallCompletion>& completion, RawCallResult result);
void completeFailure(const std::shared_ptr<RawCallCompletion>& completion, std::string message);
void completeFailureKj(const std::shared_ptr<RawCallCompletion>& completion, const kj::Exception& e);
void completeRegisterSuccess(const std::shared_ptr<RegisterTargetCompletion>& completion, uint32_t targetId);
void completeRegisterFailure(const std::shared_ptr<RegisterTargetCompletion>& completion, std::string message);
void completeUnitSuccess(const std::shared_ptr<UnitCompletion>& completion);
void completeUnitFailure(const std::shared_ptr<UnitCompletion>& completion, std::string message);
void completeUInt64Success(const std::shared_ptr<UInt64Completion>& completion, uint64_t value);
void completeUInt64Failure(const std::shared_ptr<UInt64Completion>& completion, std::string message);
void completeBoolSuccess(const std::shared_ptr<BoolCompletion>& completion, bool value);
void completeBoolFailure(const std::shared_ptr<BoolCompletion>& completion, std::string message);
void completeOptionalStringSuccess(const std::shared_ptr<OptionalStringCompletion>& completion,
                                   kj::Maybe<std::string> value);
void completeOptionalStringFailure(const std::shared_ptr<OptionalStringCompletion>& completion,
                                   std::string message);
void completeInt64Success(const std::shared_ptr<Int64Completion>& completion, int64_t value);
void completeInt64Failure(const std::shared_ptr<Int64Completion>& completion, std::string message);
void completeByteArraySuccess(const std::shared_ptr<ByteArrayCompletion>& completion,
                              std::vector<uint8_t> value);
void completeByteArrayFailure(const std::shared_ptr<ByteArrayCompletion>& completion,
                              std::string message);
void completeRegisterPairSuccess(const std::shared_ptr<RegisterPairCompletion>& completion, uint32_t first, uint32_t second);
void completeRegisterPairFailure(const std::shared_ptr<RegisterPairCompletion>& completion, std::string message);
void completeKjPromiseIdSuccess(const std::shared_ptr<KjPromiseIdCompletion>& completion, uint32_t promiseId);
void completeKjPromiseIdFailure(const std::shared_ptr<KjPromiseIdCompletion>& completion, std::string message);
void completeDiagnosticsSuccess(const std::shared_ptr<DiagnosticsCompletion>& completion,
                                capnp::_::RpcSystemBase::RpcDiagnostics value);
void completeDiagnosticsFailure(const std::shared_ptr<DiagnosticsCompletion>& completion,
                                std::string message);
void completeProtocolMessageCountsSuccess(
    const std::shared_ptr<ProtocolMessageCountsCompletion>& completion,
    ProtocolMessageCounts value);
void completeProtocolMessageCountsFailure(
    const std::shared_ptr<ProtocolMessageCountsCompletion>& completion,
    std::string message);

void completeAsyncUnitSuccess(const std::shared_ptr<AsyncUnitCompletion>& completion);
void completeAsyncUnitFailure(const std::shared_ptr<AsyncUnitCompletion>& completion, std::string message);

} // namespace capnp_lean_rpc
