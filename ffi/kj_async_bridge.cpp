#include <lean/lean.h>

#include <kj/async.h>
#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/compat/tls.h>
#include <kj/time.h>

#include "rpc_bridge_runtime.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

lean_obj_res mkIoUserError(const std::string& message) {
  lean_object* msg = lean_mk_string(message.c_str());
  lean_object* err = lean_mk_io_user_error(msg);
  return lean_io_result_mk_error(err);
}

void mkIoOkUnit(lean_obj_res& out) { out = lean_io_result_mk_ok(lean_box(0)); }

lean_obj_res mkByteArrayCopy(const uint8_t* data, size_t size) {
  lean_object* out = lean_alloc_sarray(1, size, size);
  if (size != 0) {
    std::memcpy(lean_sarray_cptr(out), data, size);
  }
  lean_sarray_set_size(out, size);
  return out;
}

std::shared_ptr<std::vector<uint8_t>> makeSharedBytes(std::vector<uint8_t>&& bytes) {
  return std::make_shared<std::vector<uint8_t>>(std::move(bytes));
}

std::shared_ptr<std::vector<uint8_t>> copyByteArrayToSharedBytes(b_lean_obj_arg bytes) {
  const auto size = lean_sarray_size(bytes);
  const auto* data =
      reinterpret_cast<const uint8_t*>(lean_sarray_cptr(const_cast<lean_object*>(bytes)));
  auto out = std::make_shared<std::vector<uint8_t>>(size);
  if (size != 0) {
    std::memcpy(out->data(), data, size);
  }
  return out;
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

kj::ArrayPtr<const kj::byte> viewAsKjBytes(const std::vector<uint8_t>& bytes) {
  return kj::ArrayPtr<const kj::byte>(reinterpret_cast<const kj::byte*>(bytes.data()), bytes.size());
}

kj::ArrayPtr<kj::byte> viewAsMutableKjBytes(std::vector<uint8_t>& bytes) {
  return kj::ArrayPtr<kj::byte>(reinterpret_cast<kj::byte*>(bytes.data()), bytes.size());
}

std::string byteArrayToString(b_lean_obj_arg bytes) {
  const auto size = lean_sarray_size(bytes);
  const auto* data =
      reinterpret_cast<const char*>(lean_sarray_cptr(const_cast<lean_object*>(bytes)));
  return std::string(data, size);
}

struct BytesRefData {
  std::shared_ptr<std::vector<uint8_t>> bytes;
};

void bytesRefFinalize(void* data) {
  delete static_cast<BytesRefData*>(data);
}

void bytesRefForeach(void*, b_lean_obj_arg) {}

lean_external_class* bytesRefClass() {
  static lean_external_class* cls =
      lean_register_external_class(bytesRefFinalize, bytesRefForeach);
  return cls;
}

lean_obj_res mkBytesRef(std::shared_ptr<std::vector<uint8_t>> bytes) {
  if (!bytes) {
    bytes = std::make_shared<std::vector<uint8_t>>();
  }
  return lean_alloc_external(bytesRefClass(), new BytesRefData{std::move(bytes)});
}

std::shared_ptr<std::vector<uint8_t>> getBytesRefDataOrThrow(b_lean_obj_arg bytesRef) {
  auto* obj = const_cast<lean_object*>(bytesRef);
  if (!lean_is_external(obj) || lean_get_external_class(obj) != bytesRefClass()) {
    throw std::runtime_error("invalid Capnp.KjAsync.BytesRef object");
  }
  auto* data = static_cast<BytesRefData*>(lean_get_external_data(obj));
  if (data == nullptr || !data->bytes) {
    return std::make_shared<std::vector<uint8_t>>();
  }
  return data->bytes;
}

uint32_t readUint32Le(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

std::vector<uint32_t> decodeUint32Array(b_lean_obj_arg bytes) {
  const auto size = lean_sarray_size(bytes);
  const auto* data =
      reinterpret_cast<const uint8_t*>(lean_sarray_cptr(const_cast<lean_object*>(bytes)));
  if ((size % 4) != 0) {
    throw std::runtime_error("uint32 array payload must be a multiple of 4 bytes");
  }
  std::vector<uint32_t> out;
  out.reserve(size / 4);
  for (size_t i = 0; i < size; i += 4) {
    out.push_back(readUint32Le(data + i));
  }
  return out;
}

void appendUint32Le(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

uint32_t decodeUint32LeChecked(const std::vector<uint8_t>& in, size_t& offset,
                               const char* what) {
  if (offset + 4 > in.size()) {
    throw std::runtime_error(std::string("invalid ") + what + ": truncated uint32");
  }
  uint32_t value = static_cast<uint32_t>(in[offset]) |
                   (static_cast<uint32_t>(in[offset + 1]) << 8) |
                   (static_cast<uint32_t>(in[offset + 2]) << 16) |
                   (static_cast<uint32_t>(in[offset + 3]) << 24);
  offset += 4;
  return value;
}

std::vector<uint8_t> copyKjByteArrayToVector(kj::ArrayPtr<const kj::byte> bytes) {
  std::vector<uint8_t> out(bytes.size());
  if (!out.empty()) {
    std::memcpy(out.data(), bytes.begin(), out.size());
  }
  return out;
}

std::vector<uint8_t> encodeHeaderPairs(
    const std::vector<std::pair<std::string, std::string>>& headers) {
  std::vector<uint8_t> out;
  out.reserve(8 + headers.size() * 16);
  appendUint32Le(out, static_cast<uint32_t>(headers.size()));
  for (const auto& kv : headers) {
    const auto& name = kv.first;
    const auto& value = kv.second;
    if (name.size() > std::numeric_limits<uint32_t>::max() ||
        value.size() > std::numeric_limits<uint32_t>::max()) {
      throw std::runtime_error("header field exceeds UInt32 size");
    }
    appendUint32Le(out, static_cast<uint32_t>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
    appendUint32Le(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
  }
  return out;
}

const std::vector<uint8_t>& encodedEmptyHeaderPairs() {
  static const std::vector<uint8_t> encoded = []() {
    const std::vector<std::pair<std::string, std::string>> emptyHeaders;
    return encodeHeaderPairs(emptyHeaders);
  }();
  return encoded;
}

constexpr uint32_t defaultWebSocketReceiveMaxBytes() {
  return kj::WebSocket::SUGGESTED_MAX_MESSAGE_SIZE > std::numeric_limits<uint32_t>::max()
             ? std::numeric_limits<uint32_t>::max()
             : static_cast<uint32_t>(kj::WebSocket::SUGGESTED_MAX_MESSAGE_SIZE);
}

std::vector<std::pair<std::string, std::string>> decodeHeaderPairs(
    const std::vector<uint8_t>& bytes) {
  size_t offset = 0;
  auto count = decodeUint32LeChecked(bytes, offset, "header list");
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    auto nameSize = decodeUint32LeChecked(bytes, offset, "header name length");
    if (offset + nameSize > bytes.size()) {
      throw std::runtime_error("invalid header list: truncated header name");
    }
    std::string name(reinterpret_cast<const char*>(bytes.data() + offset), nameSize);
    offset += nameSize;

    auto valueSize = decodeUint32LeChecked(bytes, offset, "header value length");
    if (offset + valueSize > bytes.size()) {
      throw std::runtime_error("invalid header list: truncated header value");
    }
    std::string value(reinterpret_cast<const char*>(bytes.data() + offset), valueSize);
    offset += valueSize;
    out.emplace_back(std::move(name), std::move(value));
  }
  if (offset != bytes.size()) {
    throw std::runtime_error("invalid header list: trailing bytes");
  }
  return out;
}

void applyHeadersFromPairs(kj::HttpHeaders& headers,
                           const std::vector<std::pair<std::string, std::string>>& pairs) {
  for (const auto& kv : pairs) {
    auto name = kj::str(kv.first.c_str());
    auto value = kj::str(kv.second.c_str());
    headers.add(kj::mv(name), kj::mv(value));
  }
}

std::vector<std::pair<std::string, std::string>> captureHeaders(const kj::HttpHeaders& headers) {
  std::vector<std::pair<std::string, std::string>> out;
  headers.forEach([&](kj::StringPtr name, kj::StringPtr value) {
    out.emplace_back(std::string(name.cStr()), std::string(value.cStr()));
  });
  return out;
}

std::string describeKjException(const kj::Exception& e) {
  std::string message;
  message.reserve(256);
  auto file = e.getFile();
  if (file != nullptr) {
    message += file;
    message += ":";
    message += std::to_string(e.getLine());
    message += ": ";
  }
  auto description = e.getDescription();
  if (description == nullptr) {
    message += "(no description)";
  } else {
    message += description.cStr();
  }
  for (uint i = 0;; ++i) {
    auto detailMaybe = e.getDetail(i);
    if (detailMaybe == kj::none) {
      break;
    }
    auto detail = KJ_REQUIRE_NONNULL(detailMaybe);
    message += "\ndetail[";
    message += std::to_string(i);
    message += "]: ";
    message.append(reinterpret_cast<const char*>(detail.begin()), detail.size());
  }
  return message;
}

struct UnitCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string error;
};

struct PromiseIdCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  uint32_t promiseId = 0;
  std::string error;
};

struct HandleCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  uint32_t handle = 0;
  std::string error;
};

struct BytesCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::shared_ptr<std::vector<uint8_t>> bytes;
  std::string error;
};

struct HandlePairCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  uint32_t first = 0;
  uint32_t second = 0;
  std::string error;
};

struct BoolCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  bool value = false;
  std::string error;
};

struct UInt32Completion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  uint32_t value = 0;
  std::string error;
};

struct OptionalUInt32Completion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  bool hasValue = false;
  uint32_t value = 0;
  std::string error;
};

struct OptionalStringCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  bool hasValue = false;
  std::string value;
  std::string error;
};

struct DatagramReceiveCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string sourceAddress;
  std::vector<uint8_t> bytes;
  std::string error;
};

struct HttpResponseCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  uint32_t statusCode = 0;
  std::string statusText;
  std::vector<uint8_t> headers;
  std::vector<uint8_t> body;
  uint32_t bodyHandle = 0;
  std::string error;
};

struct HttpServerRequestCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  bool hasRequest = false;
  std::vector<uint8_t> requestBytes;
  std::string error;
};

struct WebSocketMessageCompletion {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  uint8_t tag = 0;
  uint16_t closeCode = 0;
  std::string text;
  std::vector<uint8_t> bytes;
  std::string error;
};

template <typename T>
void completeFailureWithError(const std::shared_ptr<T>& completion, std::string message) {
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
  completeFailureWithError(completion, std::move(message));
}

void completePromiseIdSuccess(const std::shared_ptr<PromiseIdCompletion>& completion,
                              uint32_t promiseId) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->promiseId = promiseId;
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completePromiseIdFailure(const std::shared_ptr<PromiseIdCompletion>& completion,
                              std::string message) {
  completeFailureWithError(completion, std::move(message));
}

void completeHandleSuccess(const std::shared_ptr<HandleCompletion>& completion, uint32_t handle) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->handle = handle;
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeHandleFailure(const std::shared_ptr<HandleCompletion>& completion,
                           std::string message) {
  completeFailureWithError(completion, std::move(message));
}

void completeBytesSuccess(const std::shared_ptr<BytesCompletion>& completion,
                          std::shared_ptr<std::vector<uint8_t>> bytes) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->bytes = bytes ? std::move(bytes) : std::make_shared<std::vector<uint8_t>>();
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeBytesFailure(const std::shared_ptr<BytesCompletion>& completion, std::string message) {
  completeFailureWithError(completion, std::move(message));
}

void completeHandlePairSuccess(const std::shared_ptr<HandlePairCompletion>& completion,
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

void completeHandlePairFailure(const std::shared_ptr<HandlePairCompletion>& completion,
                               std::string message) {
  completeFailureWithError(completion, std::move(message));
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

void completeBoolFailure(const std::shared_ptr<BoolCompletion>& completion, std::string message) {
  completeFailureWithError(completion, std::move(message));
}

void completeUInt32Success(const std::shared_ptr<UInt32Completion>& completion, uint32_t value) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->value = value;
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeUInt32Failure(const std::shared_ptr<UInt32Completion>& completion,
                           std::string message) {
  completeFailureWithError(completion, std::move(message));
}

void completeOptionalUInt32Success(const std::shared_ptr<OptionalUInt32Completion>& completion,
                                   kj::Maybe<uint32_t> value) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    KJ_IF_SOME(v, value) {
      completion->hasValue = true;
      completion->value = v;
    } else {
      completion->hasValue = false;
      completion->value = 0;
    }
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeOptionalUInt32Failure(const std::shared_ptr<OptionalUInt32Completion>& completion,
                                   std::string message) {
  completeFailureWithError(completion, std::move(message));
}

void completeOptionalStringSuccess(const std::shared_ptr<OptionalStringCompletion>& completion,
                                   kj::Maybe<std::string> value) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    KJ_IF_SOME(v, value) {
      completion->hasValue = true;
      completion->value = kj::mv(v);
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
  completeFailureWithError(completion, std::move(message));
}

void completeDatagramReceiveSuccess(const std::shared_ptr<DatagramReceiveCompletion>& completion,
                                    std::string sourceAddress, std::vector<uint8_t> bytes) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->sourceAddress = std::move(sourceAddress);
    completion->bytes = std::move(bytes);
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeDatagramReceiveFailure(const std::shared_ptr<DatagramReceiveCompletion>& completion,
                                    std::string message) {
  completeFailureWithError(completion, std::move(message));
}

void completeHttpResponseSuccess(const std::shared_ptr<HttpResponseCompletion>& completion,
                                 uint32_t statusCode, std::string statusText,
                                 std::vector<uint8_t> headers, std::vector<uint8_t> body,
                                 uint32_t bodyHandle = 0) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->statusCode = statusCode;
    completion->statusText = std::move(statusText);
    completion->headers = std::move(headers);
    completion->body = std::move(body);
    completion->bodyHandle = bodyHandle;
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeHttpResponseFailure(const std::shared_ptr<HttpResponseCompletion>& completion,
                                 std::string message) {
  completeFailureWithError(completion, std::move(message));
}

void completeHttpServerRequestSuccess(const std::shared_ptr<HttpServerRequestCompletion>& completion,
                                      bool hasRequest, std::vector<uint8_t> requestBytes) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->hasRequest = hasRequest;
    completion->requestBytes = std::move(requestBytes);
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeHttpServerRequestFailure(const std::shared_ptr<HttpServerRequestCompletion>& completion,
                                      std::string message) {
  completeFailureWithError(completion, std::move(message));
}

void completeWebSocketMessageSuccess(const std::shared_ptr<WebSocketMessageCompletion>& completion,
                                     uint8_t tag, uint16_t closeCode, std::string text,
                                     std::vector<uint8_t> bytes) {
  {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->ok = true;
    completion->tag = tag;
    completion->closeCode = closeCode;
    completion->text = std::move(text);
    completion->bytes = std::move(bytes);
    completion->done = true;
  }
  completion->cv.notify_one();
}

void completeWebSocketMessageFailure(const std::shared_ptr<WebSocketMessageCompletion>& completion,
                                     std::string message) {
  completeFailureWithError(completion, std::move(message));
}

class KjAsyncRuntimeLoop {
 public:
  struct HttpServerConfig {
    uint64_t headerTimeoutNanos = 15ULL * 1000ULL * 1000ULL * 1000ULL;
    uint64_t pipelineTimeoutNanos = 5ULL * 1000ULL * 1000ULL * 1000ULL;
    uint64_t canceledUploadGracePeriodNanos = 1ULL * 1000ULL * 1000ULL * 1000ULL;
    uint64_t canceledUploadGraceBytes = 65536;
    uint32_t webSocketCompressionMode = 0;
  };

  KjAsyncRuntimeLoop() : worker_(&KjAsyncRuntimeLoop::run, this) {
    std::unique_lock<std::mutex> lock(startupMutex_);
    startupCv_.wait(lock, [this]() { return startupComplete_; });
    if (!startupError_.empty()) {
      throw std::runtime_error(startupError_);
    }
  }

  ~KjAsyncRuntimeLoop() { shutdown(); }

  bool isAlive() const { return alive_.load(std::memory_order_acquire); }

  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        // Another thread already initiated shutdown.
      } else {
        stopping_ = true;
      }
    }
    queueCv_.notify_one();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  std::shared_ptr<PromiseIdCompletion> enqueueSleepNanos(uint64_t delayNanos) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedSleepNanos{delayNanos, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueAwaitPromise(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedAwaitPromise{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueCancelPromise(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedCancelPromise{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueReleasePromise(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedReleasePromise{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueListen(std::string address, uint32_t portHint) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedListen{std::move(address), portHint, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueReleaseListener(uint32_t listenerId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedReleaseListener{listenerId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueAccept(uint32_t listenerId) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedAccept{listenerId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueAcceptStart(uint32_t listenerId) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedAcceptStart{listenerId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueConnect(std::string address, uint32_t portHint) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedConnect{std::move(address), portHint, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueConnectStart(std::string address, uint32_t portHint) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedConnectStart{std::move(address), portHint, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueParseAddress(std::string address, uint32_t portHint) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedParseAddress{std::move(address), portHint, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueReleaseNetworkAddress(uint32_t addressId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedReleaseNetworkAddress{addressId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<OptionalStringCompletion> enqueueNetworkAddressToString(uint32_t addressId) {
    auto completion = std::make_shared<OptionalStringCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeOptionalStringFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedNetworkAddressToString{addressId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueNetworkAddressClone(uint32_t addressId) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedNetworkAddressClone{addressId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueNetworkAddressConnect(uint32_t addressId) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedNetworkAddressConnect{addressId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueNetworkAddressConnectStart(uint32_t addressId) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedNetworkAddressConnectStart{addressId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueNetworkAddressListen(uint32_t addressId) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedNetworkAddressListen{addressId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueNetworkAddressBindDatagramPort(uint32_t addressId) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedNetworkAddressBindDatagramPort{addressId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueAwaitConnectionPromise(uint32_t promiseId) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedAwaitConnectionPromise{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<OptionalUInt32Completion> enqueueAwaitConnectionPromiseWithTimeout(
      uint32_t promiseId, uint64_t timeoutNanos) {
    auto completion = std::make_shared<OptionalUInt32Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeOptionalUInt32Failure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedAwaitConnectionPromiseWithTimeout{promiseId, timeoutNanos, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueCancelConnectionPromise(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedCancelConnectionPromise{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueReleaseConnectionPromise(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedReleaseConnectionPromise{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueReleaseConnection(uint32_t connectionId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedReleaseConnection{connectionId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueConnectionWrite(uint32_t connectionId,
                                                         std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedConnectionWrite{connectionId, std::move(bytes), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueConnectionWriteStart(uint32_t connectionId,
                                                                   std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedConnectionWriteStart{connectionId, std::move(bytes), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<BytesCompletion> enqueueConnectionRead(uint32_t connectionId, uint32_t minBytes,
                                                         uint32_t maxBytes) {
    auto completion = std::make_shared<BytesCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeBytesFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedConnectionRead{connectionId, minBytes, maxBytes, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueConnectionReadStart(uint32_t connectionId,
                                                                  uint32_t minBytes,
                                                                  uint32_t maxBytes) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedConnectionReadStart{connectionId, minBytes, maxBytes, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<BytesCompletion> enqueueBytesPromiseAwait(uint32_t promiseId) {
    auto completion = std::make_shared<BytesCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeBytesFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedBytesPromiseAwait{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueBytesPromiseCancel(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedBytesPromiseCancel{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueBytesPromiseRelease(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedBytesPromiseRelease{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueConnectionShutdownWrite(uint32_t connectionId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedConnectionShutdownWrite{connectionId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueConnectionShutdownWriteStart(uint32_t connectionId) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedConnectionShutdownWriteStart{connectionId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueuePromiseThenStart(uint32_t firstPromiseId,
                                                               uint32_t secondPromiseId) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedPromiseThenStart{firstPromiseId, secondPromiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueuePromiseCatchStart(uint32_t promiseId,
                                                                uint32_t fallbackPromiseId) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedPromiseCatchStart{promiseId, fallbackPromiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueuePromiseAllStart(std::vector<uint32_t> promiseIds) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedPromiseAllStart{std::move(promiseIds), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueuePromiseRaceStart(std::vector<uint32_t> promiseIds) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedPromiseRaceStart{std::move(promiseIds), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueTaskSetNew() {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedTaskSetNew{completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueTaskSetRelease(uint32_t taskSetId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedTaskSetRelease{taskSetId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueTaskSetAddPromise(uint32_t taskSetId, uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedTaskSetAddPromise{taskSetId, promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueTaskSetClear(uint32_t taskSetId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedTaskSetClear{taskSetId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<BoolCompletion> enqueueTaskSetIsEmpty(uint32_t taskSetId) {
    auto completion = std::make_shared<BoolCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeBoolFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedTaskSetIsEmpty{taskSetId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueTaskSetOnEmptyStart(uint32_t taskSetId) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedTaskSetOnEmptyStart{taskSetId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UInt32Completion> enqueueTaskSetErrorCount(uint32_t taskSetId) {
    auto completion = std::make_shared<UInt32Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt32Failure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedTaskSetErrorCount{taskSetId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<OptionalStringCompletion> enqueueTaskSetTakeLastError(uint32_t taskSetId) {
    auto completion = std::make_shared<OptionalStringCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeOptionalStringFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedTaskSetTakeLastError{taskSetId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueConnectionWhenWriteDisconnectedStart(
      uint32_t connectionId) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedConnectionWhenWriteDisconnectedStart{connectionId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueConnectionAbortRead(uint32_t connectionId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedConnectionAbortRead{connectionId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueConnectionAbortWrite(uint32_t connectionId,
                                                              std::string reason) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedConnectionAbortWrite{connectionId, std::move(reason), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<OptionalUInt32Completion> enqueueConnectionDupFd(uint32_t connectionId) {
    auto completion = std::make_shared<OptionalUInt32Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeOptionalUInt32Failure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedConnectionDupFd{connectionId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueWrapSocketFd(uint32_t fd) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWrapSocketFd{fd, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueWrapSocketFdTake(uint32_t fd) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWrapSocketFdTake{fd, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueWrapListenSocketFd(uint32_t fd) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWrapListenSocketFd{fd, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueWrapListenSocketFdTake(uint32_t fd) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWrapListenSocketFdTake{fd, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueWrapDatagramSocketFd(uint32_t fd) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWrapDatagramSocketFd{fd, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueWrapDatagramSocketFdTake(uint32_t fd) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWrapDatagramSocketFdTake{fd, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandlePairCompletion> enqueueNewTwoWayPipe() {
    auto completion = std::make_shared<HandlePairCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandlePairFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedNewTwoWayPipe{completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandlePairCompletion> enqueueNewCapabilityPipe() {
    auto completion = std::make_shared<HandlePairCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandlePairFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedNewCapabilityPipe{completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueDatagramBind(std::string address, uint32_t portHint) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedDatagramBind{std::move(address), portHint, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueDatagramReleasePort(uint32_t portId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedDatagramReleasePort{portId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueDatagramGetPort(uint32_t portId) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedDatagramGetPort{portId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UInt32Completion> enqueueDatagramSend(
      uint32_t portId, std::string address, uint32_t portHint,
      std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto completion = std::make_shared<UInt32Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt32Failure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedDatagramSend{portId, std::move(address), portHint, std::move(bytes), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueDatagramSendStart(
      uint32_t portId, std::string address, uint32_t portHint,
      std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedDatagramSendStart{portId, std::move(address), portHint,
                                                  std::move(bytes), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UInt32Completion> enqueueUInt32PromiseAwait(uint32_t promiseId) {
    auto completion = std::make_shared<UInt32Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt32Failure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedUInt32PromiseAwait{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueUInt32PromiseCancel(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedUInt32PromiseCancel{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueUInt32PromiseRelease(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedUInt32PromiseRelease{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<DatagramReceiveCompletion> enqueueDatagramReceive(uint32_t portId,
                                                                    uint32_t maxBytes) {
    auto completion = std::make_shared<DatagramReceiveCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeDatagramReceiveFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedDatagramReceive{portId, maxBytes, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueDatagramReceiveStart(uint32_t portId,
                                                                   uint32_t maxBytes) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedDatagramReceiveStart{portId, maxBytes, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<DatagramReceiveCompletion> enqueueDatagramReceivePromiseAwait(uint32_t promiseId) {
    auto completion = std::make_shared<DatagramReceiveCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeDatagramReceiveFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedDatagramReceivePromiseAwait{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueDatagramReceivePromiseCancel(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedDatagramReceivePromiseCancel{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueDatagramReceivePromiseRelease(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedDatagramReceivePromiseRelease{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueEnableTls() {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedEnableTls{completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueConfigureTls(uint32_t useSystemTrustStore,
                                                      uint32_t verifyClients,
                                                      uint32_t minVersionTag,
                                                      std::string trustedCertificatesPem,
                                                      std::string certificateChainPem,
                                                      std::string privateKeyPem,
                                                      std::string cipherList,
                                                      std::string curveList,
                                                      uint64_t acceptTimeoutNanos) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedConfigureTls{
          useSystemTrustStore,
          verifyClients,
          minVersionTag,
          std::move(trustedCertificatesPem),
          std::move(certificateChainPem),
          std::move(privateKeyPem),
          std::move(cipherList),
          std::move(curveList),
          acceptTimeoutNanos,
          completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HttpResponseCompletion> enqueueHttpRequest(
      uint32_t method, std::string address, uint32_t portHint, std::string path,
      std::vector<uint8_t> requestHeaders, std::shared_ptr<std::vector<uint8_t>> body,
      bool useTls = false,
      uint64_t responseBodyLimit = std::numeric_limits<uint64_t>::max()) {
    auto completion = std::make_shared<HttpResponseCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHttpResponseFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpRequest{
          method, std::move(address), portHint, std::move(path), std::move(requestHeaders),
          std::move(body), useTls, responseBodyLimit, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueHttpRequestStart(
      uint32_t method, std::string address, uint32_t portHint, std::string path,
      std::vector<uint8_t> requestHeaders, std::shared_ptr<std::vector<uint8_t>> body,
      bool useTls = false,
      uint64_t responseBodyLimit = std::numeric_limits<uint64_t>::max()) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpRequestStart{
          method, std::move(address), portHint, std::move(path), std::move(requestHeaders),
          std::move(body), useTls, responseBodyLimit, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandlePairCompletion> enqueueHttpRequestStartStreaming(
      uint32_t method, std::string address, uint32_t portHint, std::string path,
      std::vector<uint8_t> requestHeaders, bool useTls = false) {
    auto completion = std::make_shared<HandlePairCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandlePairFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpRequestStartStreaming{
          method, std::move(address), portHint, std::move(path), std::move(requestHeaders), useTls,
          completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HttpResponseCompletion> enqueueHttpResponsePromiseAwait(uint32_t promiseId) {
    auto completion = std::make_shared<HttpResponseCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHttpResponseFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpResponsePromiseAwait{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HttpResponseCompletion> enqueueHttpResponsePromiseAwaitStreaming(
      uint32_t promiseId) {
    auto completion = std::make_shared<HttpResponseCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHttpResponseFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpResponsePromiseAwaitStreaming{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueHttpResponsePromiseCancel(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpResponsePromiseCancel{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueHttpResponsePromiseRelease(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpResponsePromiseRelease{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueHttpRequestBodyWriteStart(
      uint32_t requestBodyId, std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedHttpRequestBodyWriteStart{requestBodyId, std::move(bytes), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueHttpRequestBodyWrite(
      uint32_t requestBodyId, std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpRequestBodyWrite{requestBodyId, std::move(bytes), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueHttpRequestBodyFinishStart(uint32_t requestBodyId) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpRequestBodyFinishStart{requestBodyId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueHttpRequestBodyFinish(uint32_t requestBodyId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpRequestBodyFinish{requestBodyId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueHttpRequestBodyRelease(uint32_t requestBodyId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpRequestBodyRelease{requestBodyId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueHttpResponseBodyReadStart(uint32_t responseBodyId,
                                                                         uint32_t minBytes,
                                                                         uint32_t maxBytes) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedHttpResponseBodyReadStart{responseBodyId, minBytes, maxBytes, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<BytesCompletion> enqueueHttpResponseBodyRead(uint32_t responseBodyId,
                                                               uint32_t minBytes,
                                                               uint32_t maxBytes) {
    auto completion = std::make_shared<BytesCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeBytesFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedHttpResponseBodyRead{responseBodyId, minBytes, maxBytes, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueHttpResponseBodyRelease(uint32_t responseBodyId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpResponseBodyRelease{responseBodyId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueHttpServerRequestBodyReadStart(
      uint32_t requestBodyId, uint32_t minBytes, uint32_t maxBytes) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedHttpServerRequestBodyReadStart{requestBodyId, minBytes, maxBytes, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<BytesCompletion> enqueueHttpServerRequestBodyRead(uint32_t requestBodyId,
                                                                    uint32_t minBytes,
                                                                    uint32_t maxBytes) {
    auto completion = std::make_shared<BytesCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeBytesFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedHttpServerRequestBodyRead{requestBodyId, minBytes, maxBytes, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueHttpServerRequestBodyRelease(uint32_t requestBodyId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpServerRequestBodyRelease{requestBodyId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueWebSocketConnect(std::string address, uint32_t portHint,
                                                            std::string path,
                                                            std::vector<uint8_t> requestHeaders,
                                                            bool useTls = false) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedWebSocketConnect{std::move(address), portHint, std::move(path),
                                 std::move(requestHeaders), useTls, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueWebSocketConnectStart(std::string address,
                                                                     uint32_t portHint,
                                                                     std::string path,
                                                                     std::vector<uint8_t> requestHeaders,
                                                                     bool useTls = false) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedWebSocketConnectStart{std::move(address), portHint, std::move(path),
                                      std::move(requestHeaders), useTls, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueWebSocketPromiseAwait(uint32_t promiseId) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWebSocketPromiseAwait{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueWebSocketPromiseCancel(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWebSocketPromiseCancel{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueWebSocketPromiseRelease(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWebSocketPromiseRelease{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueWebSocketRelease(uint32_t webSocketId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWebSocketRelease{webSocketId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueWebSocketSendTextStart(uint32_t webSocketId,
                                                                      std::string text) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWebSocketSendTextStart{webSocketId, std::move(text), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueWebSocketSendText(uint32_t webSocketId, std::string text) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWebSocketSendText{webSocketId, std::move(text), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueWebSocketSendBinaryStart(
      uint32_t webSocketId, std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedWebSocketSendBinaryStart{webSocketId, std::move(bytes), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueWebSocketSendBinary(
      uint32_t webSocketId, std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWebSocketSendBinary{webSocketId, std::move(bytes), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueWebSocketReceiveStart(
      uint32_t webSocketId, uint32_t maxBytes = defaultWebSocketReceiveMaxBytes()) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWebSocketReceiveStart{webSocketId, maxBytes, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<WebSocketMessageCompletion> enqueueWebSocketMessagePromiseAwait(
      uint32_t promiseId) {
    auto completion = std::make_shared<WebSocketMessageCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeWebSocketMessageFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWebSocketMessagePromiseAwait{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueWebSocketMessagePromiseCancel(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWebSocketMessagePromiseCancel{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueWebSocketMessagePromiseRelease(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWebSocketMessagePromiseRelease{promiseId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<WebSocketMessageCompletion> enqueueWebSocketReceive(
      uint32_t webSocketId, uint32_t maxBytes = defaultWebSocketReceiveMaxBytes()) {
    auto completion = std::make_shared<WebSocketMessageCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeWebSocketMessageFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWebSocketReceive{webSocketId, maxBytes, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueWebSocketCloseStart(uint32_t webSocketId,
                                                                   uint16_t closeCode,
                                                                   std::string reason) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedWebSocketCloseStart{webSocketId, closeCode, std::move(reason), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueWebSocketClose(uint32_t webSocketId, uint16_t closeCode,
                                                        std::string reason) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedWebSocketClose{webSocketId, closeCode, std::move(reason), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueWebSocketDisconnect(uint32_t webSocketId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWebSocketDisconnect{webSocketId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueWebSocketAbort(uint32_t webSocketId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedWebSocketAbort{webSocketId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandlePairCompletion> enqueueNewWebSocketPipe() {
    auto completion = std::make_shared<HandlePairCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandlePairFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedNewWebSocketPipe{completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  static HttpServerConfig defaultHttpServerConfig() {
    return HttpServerConfig{};
  }

  std::shared_ptr<HandlePairCompletion> enqueueHttpServerListen(
      std::string address, uint32_t portHint, bool useTls = false,
      HttpServerConfig config = defaultHttpServerConfig()) {
    auto completion = std::make_shared<HandlePairCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandlePairFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpServerListen{
          std::move(address), portHint, useTls, std::move(config), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueHttpServerRelease(uint32_t serverId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpServerRelease{serverId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueHttpServerDrainStart(uint32_t serverId) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpServerDrainStart{serverId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueHttpServerDrain(uint32_t serverId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpServerDrain{serverId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HttpServerRequestCompletion> enqueueHttpServerPollRequest(uint32_t serverId) {
    auto completion = std::make_shared<HttpServerRequestCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHttpServerRequestFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpServerPollRequest{serverId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueHttpServerRespond(uint32_t serverId, uint32_t requestId,
                                                           uint32_t statusCode,
                                                           std::string statusText,
                                                           std::vector<uint8_t> responseHeaders,
                                                           std::shared_ptr<std::vector<uint8_t>> body) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedHttpServerRespond{serverId, requestId, statusCode, std::move(statusText),
                                  std::move(responseHeaders), std::move(body), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueHttpServerRespondWebSocket(
      uint32_t serverId, uint32_t requestId, std::vector<uint8_t> responseHeaders) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpServerRespondWebSocket{
          serverId, requestId, std::move(responseHeaders), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<HandleCompletion> enqueueHttpServerRespondStartStreaming(
      uint32_t serverId, uint32_t requestId, uint32_t statusCode, std::string statusText,
      std::vector<uint8_t> responseHeaders) {
    auto completion = std::make_shared<HandleCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeHandleFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpServerRespondStartStreaming{
          serverId, requestId, statusCode, std::move(statusText), std::move(responseHeaders),
          completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueHttpServerResponseBodyWriteStart(
      uint32_t responseBodyId, std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedHttpServerResponseBodyWriteStart{responseBodyId, std::move(bytes), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueHttpServerResponseBodyWrite(
      uint32_t responseBodyId, std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedHttpServerResponseBodyWrite{responseBodyId, std::move(bytes), completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<PromiseIdCompletion> enqueueHttpServerResponseBodyFinishStart(
      uint32_t responseBodyId) {
    auto completion = std::make_shared<PromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completePromiseIdFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpServerResponseBodyFinishStart{responseBodyId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueHttpServerResponseBodyFinish(uint32_t responseBodyId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpServerResponseBodyFinish{responseBodyId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueHttpServerResponseBodyRelease(uint32_t responseBodyId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.KjAsync runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedHttpServerResponseBodyRelease{responseBodyId, completion});
    }
    queueCv_.notify_one();
    return completion;
  }

 private:
  struct PendingPromise {
    PendingPromise(kj::Promise<void>&& promise, kj::Own<kj::Canceler>&& canceler)
        : promise(kj::mv(promise)), canceler(kj::mv(canceler)) {}

    PendingPromise(PendingPromise&&) = default;
    PendingPromise& operator=(PendingPromise&&) = default;
    PendingPromise(const PendingPromise&) = delete;
    PendingPromise& operator=(const PendingPromise&) = delete;

    kj::Promise<void> promise;
    kj::Own<kj::Canceler> canceler;
  };

  struct PendingConnectionPromise {
    PendingConnectionPromise(kj::Promise<kj::Own<kj::AsyncIoStream>>&& promise,
                             kj::Own<kj::Canceler>&& canceler)
        : promise(kj::mv(promise)), canceler(kj::mv(canceler)) {}

    PendingConnectionPromise(PendingConnectionPromise&&) = default;
    PendingConnectionPromise& operator=(PendingConnectionPromise&&) = default;
    PendingConnectionPromise(const PendingConnectionPromise&) = delete;
    PendingConnectionPromise& operator=(const PendingConnectionPromise&) = delete;

    kj::Promise<kj::Own<kj::AsyncIoStream>> promise;
    kj::Own<kj::Canceler> canceler;
  };

  struct PendingBytesPromise {
    PendingBytesPromise(kj::Promise<std::shared_ptr<std::vector<uint8_t>>>&& promise,
                        kj::Own<kj::Canceler>&& canceler)
        : promise(kj::mv(promise)), canceler(kj::mv(canceler)) {}

    PendingBytesPromise(PendingBytesPromise&&) = default;
    PendingBytesPromise& operator=(PendingBytesPromise&&) = default;
    PendingBytesPromise(const PendingBytesPromise&) = delete;
    PendingBytesPromise& operator=(const PendingBytesPromise&) = delete;

    kj::Promise<std::shared_ptr<std::vector<uint8_t>>> promise;
    kj::Own<kj::Canceler> canceler;
  };

  struct PendingUInt32Promise {
    PendingUInt32Promise(kj::Promise<uint32_t>&& promise, kj::Own<kj::Canceler>&& canceler)
        : promise(kj::mv(promise)), canceler(kj::mv(canceler)) {}

    PendingUInt32Promise(PendingUInt32Promise&&) = default;
    PendingUInt32Promise& operator=(PendingUInt32Promise&&) = default;
    PendingUInt32Promise(const PendingUInt32Promise&) = delete;
    PendingUInt32Promise& operator=(const PendingUInt32Promise&) = delete;

    kj::Promise<uint32_t> promise;
    kj::Own<kj::Canceler> canceler;
  };

  struct DatagramReceiveResult {
    std::string sourceAddress;
    std::vector<uint8_t> bytes;
  };

  struct PendingDatagramReceivePromise {
    PendingDatagramReceivePromise(kj::Promise<DatagramReceiveResult>&& promise,
                                  kj::Own<kj::Canceler>&& canceler)
        : promise(kj::mv(promise)), canceler(kj::mv(canceler)) {}

    PendingDatagramReceivePromise(PendingDatagramReceivePromise&&) = default;
    PendingDatagramReceivePromise& operator=(PendingDatagramReceivePromise&&) = default;
    PendingDatagramReceivePromise(const PendingDatagramReceivePromise&) = delete;
    PendingDatagramReceivePromise& operator=(const PendingDatagramReceivePromise&) = delete;

    kj::Promise<DatagramReceiveResult> promise;
    kj::Own<kj::Canceler> canceler;
  };

  struct HttpResponseResult {
    uint32_t statusCode = 0;
    std::string statusText;
    std::vector<uint8_t> headers;
    std::vector<uint8_t> body;
    uint32_t bodyHandle = 0;
  };

  struct HttpServerResponseCommand {
    enum class Kind : uint8_t {
      HTTP = 0,
      WEBSOCKET = 1,
      HTTP_STREAMING = 2,
    };

    Kind kind = Kind::HTTP;
    uint32_t statusCode = 200;
    std::string statusText = "OK";
    std::vector<std::pair<std::string, std::string>> headers;
    std::shared_ptr<std::vector<uint8_t>> body;
    std::shared_ptr<HandleCompletion> webSocketCompletion;
    std::shared_ptr<HandleCompletion> streamingCompletion;
  };

  struct HttpServerRequestRecord {
    uint32_t requestId = 0;
    uint32_t methodTag = 0;
    bool webSocketRequested = false;
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<uint8_t> body;
    uint32_t bodyHandle = 0;
  };

  struct HttpServerPollResult {
    bool hasRequest = false;
    std::vector<uint8_t> encodedRequest;
  };

  class RuntimeEntropySource final : public kj::EntropySource {
   public:
    void generate(kj::ArrayPtr<kj::byte> buffer) override {
      for (auto& value : buffer) {
        value = static_cast<kj::byte>(random_());
      }
    }

   private:
    std::random_device random_;
  };

  struct RuntimeWebSocketOwner {
    kj::Own<kj::HttpHeaderTable> headerTable;
    kj::Own<RuntimeEntropySource> entropySource;
    kj::Own<kj::HttpClient> client;
  };

  struct RuntimeWebSocket {
    kj::Maybe<kj::Own<RuntimeWebSocketOwner>> owner;
    kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> requestCompletion;
    kj::Own<kj::WebSocket> socket;
  };

  struct RuntimeHttpClientOwner {
    kj::Own<kj::HttpHeaderTable> headerTable;
    kj::Own<kj::HttpClient> client;
    kj::Own<kj::HttpHeaders> requestHeaders;
    kj::String requestUrl;
  };

  struct RuntimeHttpRequestBody {
    kj::Own<kj::AsyncOutputStream> stream;
    std::shared_ptr<RuntimeHttpClientOwner> owner;
  };

  struct RuntimeHttpResponseBody {
    kj::Own<kj::AsyncInputStream> stream;
    std::shared_ptr<RuntimeHttpClientOwner> owner;
  };

  struct RuntimeHttpServerRequestBody {
    kj::AsyncInputStream* stream = nullptr;
    uint32_t serverId = 0;
    uint32_t requestId = 0;
  };

  struct RuntimeHttpServerResponseBody {
    kj::Own<kj::AsyncOutputStream> stream;
    kj::Own<kj::PromiseFulfiller<void>> doneFulfiller;
    uint32_t serverId = 0;
    uint32_t requestId = 0;
  };

  struct HttpServerState;
  class RuntimeHttpService final : public kj::HttpService {
   public:
    RuntimeHttpService(KjAsyncRuntimeLoop& runtime, HttpServerState& state)
        : runtime_(runtime), state_(state) {}

    kj::Promise<void> request(kj::HttpMethod method, kj::StringPtr url,
                              const kj::HttpHeaders& headers,
                              kj::AsyncInputStream& requestBody, Response& response) override;

   private:
    KjAsyncRuntimeLoop& runtime_;
    HttpServerState& state_;
  };

  struct HttpServerState {
    uint32_t serverId = 0;
    kj::Own<kj::ConnectionReceiver> listener;
    kj::Own<kj::HttpHeaderTable> headerTable;
    kj::Own<RuntimeHttpService> service;
    kj::Own<kj::HttpServer> server;
    uint32_t listenPromiseId = 0;
    uint32_t nextRequestId = 1;
    std::deque<HttpServerRequestRecord> requestQueue;
    std::unordered_map<uint32_t, kj::Own<kj::PromiseFulfiller<HttpServerResponseCommand>>>
        pendingResponses;
  };

  struct WebSocketMessageResult;

  struct PendingHttpResponsePromise {
    PendingHttpResponsePromise(kj::Promise<HttpResponseResult>&& promise,
                               kj::Own<kj::Canceler>&& canceler)
        : promise(kj::mv(promise)), canceler(kj::mv(canceler)) {}

    PendingHttpResponsePromise(PendingHttpResponsePromise&&) = default;
    PendingHttpResponsePromise& operator=(PendingHttpResponsePromise&&) = default;
    PendingHttpResponsePromise(const PendingHttpResponsePromise&) = delete;
    PendingHttpResponsePromise& operator=(const PendingHttpResponsePromise&) = delete;

    kj::Promise<HttpResponseResult> promise = nullptr;
    kj::Own<kj::Canceler> canceler;
  };

  struct PendingWebSocketPromise {
    PendingWebSocketPromise(kj::Promise<RuntimeWebSocket>&& promise,
                            kj::Own<kj::Canceler>&& canceler)
        : promise(kj::mv(promise)), canceler(kj::mv(canceler)) {}

    PendingWebSocketPromise(PendingWebSocketPromise&&) = default;
    PendingWebSocketPromise& operator=(PendingWebSocketPromise&&) = default;
    PendingWebSocketPromise(const PendingWebSocketPromise&) = delete;
    PendingWebSocketPromise& operator=(const PendingWebSocketPromise&) = delete;

    kj::Promise<RuntimeWebSocket> promise;
    kj::Own<kj::Canceler> canceler;
  };

  struct PendingWebSocketMessagePromise {
    PendingWebSocketMessagePromise(kj::Promise<WebSocketMessageResult>&& promise,
                                   kj::Own<kj::Canceler>&& canceler)
        : promise(kj::mv(promise)), canceler(kj::mv(canceler)) {}

    PendingWebSocketMessagePromise(PendingWebSocketMessagePromise&&) = default;
    PendingWebSocketMessagePromise& operator=(PendingWebSocketMessagePromise&&) = default;
    PendingWebSocketMessagePromise(const PendingWebSocketMessagePromise&) = delete;
    PendingWebSocketMessagePromise& operator=(const PendingWebSocketMessagePromise&) = delete;

    kj::Promise<WebSocketMessageResult> promise;
    kj::Own<kj::Canceler> canceler;
  };

  struct WebSocketMessageResult {
    uint8_t tag = 0;
    uint16_t closeCode = 0;
    std::string text;
    std::vector<uint8_t> bytes;
  };

  struct RuntimeTaskSet {
    class ErrorHandler final : public kj::TaskSet::ErrorHandler {
     public:
      explicit ErrorHandler(RuntimeTaskSet& state) : state_(state) {}

      void taskFailed(kj::Exception&& exception) override {
        std::lock_guard<std::mutex> lock(state_.mutex);
        state_.errorCount += 1;
        state_.lastError = describeKjException(exception);
      }

     private:
      RuntimeTaskSet& state_;
    };

    RuntimeTaskSet() : errorHandler(*this), tasks(errorHandler) {}

    std::mutex mutex;
    uint32_t errorCount = 0;
    std::string lastError;
    ErrorHandler errorHandler;
    kj::TaskSet tasks;
  };

  struct QueuedSleepNanos {
    uint64_t delayNanos;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedAwaitPromise {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedCancelPromise {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedReleasePromise {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedPromiseThenStart {
    uint32_t firstPromiseId;
    uint32_t secondPromiseId;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedPromiseCatchStart {
    uint32_t promiseId;
    uint32_t fallbackPromiseId;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedListen {
    std::string address;
    uint32_t portHint;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedReleaseListener {
    uint32_t listenerId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedAccept {
    uint32_t listenerId;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedAcceptStart {
    uint32_t listenerId;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedConnect {
    std::string address;
    uint32_t portHint;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedConnectStart {
    std::string address;
    uint32_t portHint;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedParseAddress {
    std::string address;
    uint32_t portHint;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedReleaseNetworkAddress {
    uint32_t addressId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedNetworkAddressToString {
    uint32_t addressId;
    std::shared_ptr<OptionalStringCompletion> completion;
  };

  struct QueuedNetworkAddressClone {
    uint32_t addressId;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedNetworkAddressConnect {
    uint32_t addressId;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedNetworkAddressConnectStart {
    uint32_t addressId;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedNetworkAddressListen {
    uint32_t addressId;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedNetworkAddressBindDatagramPort {
    uint32_t addressId;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedAwaitConnectionPromise {
    uint32_t promiseId;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedAwaitConnectionPromiseWithTimeout {
    uint32_t promiseId;
    uint64_t timeoutNanos;
    std::shared_ptr<OptionalUInt32Completion> completion;
  };

  struct QueuedCancelConnectionPromise {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedReleaseConnectionPromise {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedReleaseConnection {
    uint32_t connectionId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedConnectionWrite {
    uint32_t connectionId;
    std::shared_ptr<std::vector<uint8_t>> bytes;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedConnectionWriteStart {
    uint32_t connectionId;
    std::shared_ptr<std::vector<uint8_t>> bytes;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedConnectionRead {
    uint32_t connectionId;
    uint32_t minBytes;
    uint32_t maxBytes;
    std::shared_ptr<BytesCompletion> completion;
  };

  struct QueuedConnectionReadStart {
    uint32_t connectionId;
    uint32_t minBytes;
    uint32_t maxBytes;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedBytesPromiseAwait {
    uint32_t promiseId;
    std::shared_ptr<BytesCompletion> completion;
  };

  struct QueuedBytesPromiseCancel {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedBytesPromiseRelease {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedConnectionShutdownWrite {
    uint32_t connectionId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedConnectionShutdownWriteStart {
    uint32_t connectionId;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedPromiseAllStart {
    std::vector<uint32_t> promiseIds;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedPromiseRaceStart {
    std::vector<uint32_t> promiseIds;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedTaskSetNew {
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedTaskSetRelease {
    uint32_t taskSetId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedTaskSetAddPromise {
    uint32_t taskSetId;
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedTaskSetClear {
    uint32_t taskSetId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedTaskSetIsEmpty {
    uint32_t taskSetId;
    std::shared_ptr<BoolCompletion> completion;
  };

  struct QueuedTaskSetOnEmptyStart {
    uint32_t taskSetId;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedTaskSetErrorCount {
    uint32_t taskSetId;
    std::shared_ptr<UInt32Completion> completion;
  };

  struct QueuedTaskSetTakeLastError {
    uint32_t taskSetId;
    std::shared_ptr<OptionalStringCompletion> completion;
  };

  struct QueuedConnectionWhenWriteDisconnectedStart {
    uint32_t connectionId;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedConnectionAbortRead {
    uint32_t connectionId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedConnectionAbortWrite {
    uint32_t connectionId;
    std::string reason;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedConnectionDupFd {
    uint32_t connectionId;
    std::shared_ptr<OptionalUInt32Completion> completion;
  };

  struct QueuedWrapSocketFd {
    uint32_t fd;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedWrapSocketFdTake {
    uint32_t fd;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedWrapListenSocketFd {
    uint32_t fd;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedWrapListenSocketFdTake {
    uint32_t fd;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedWrapDatagramSocketFd {
    uint32_t fd;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedWrapDatagramSocketFdTake {
    uint32_t fd;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedNewTwoWayPipe {
    std::shared_ptr<HandlePairCompletion> completion;
  };

  struct QueuedNewCapabilityPipe {
    std::shared_ptr<HandlePairCompletion> completion;
  };

  struct QueuedDatagramBind {
    std::string address;
    uint32_t portHint;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedDatagramReleasePort {
    uint32_t portId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedDatagramGetPort {
    uint32_t portId;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedDatagramSend {
    uint32_t portId;
    std::string address;
    uint32_t portHint;
    std::shared_ptr<std::vector<uint8_t>> bytes;
    std::shared_ptr<UInt32Completion> completion;
  };

  struct QueuedDatagramSendStart {
    uint32_t portId;
    std::string address;
    uint32_t portHint;
    std::shared_ptr<std::vector<uint8_t>> bytes;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedUInt32PromiseAwait {
    uint32_t promiseId;
    std::shared_ptr<UInt32Completion> completion;
  };

  struct QueuedUInt32PromiseCancel {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedUInt32PromiseRelease {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedDatagramReceive {
    uint32_t portId;
    uint32_t maxBytes;
    std::shared_ptr<DatagramReceiveCompletion> completion;
  };

  struct QueuedDatagramReceiveStart {
    uint32_t portId;
    uint32_t maxBytes;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedDatagramReceivePromiseAwait {
    uint32_t promiseId;
    std::shared_ptr<DatagramReceiveCompletion> completion;
  };

  struct QueuedDatagramReceivePromiseCancel {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedDatagramReceivePromiseRelease {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedEnableTls {
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedConfigureTls {
    uint32_t useSystemTrustStore;
    uint32_t verifyClients;
    uint32_t minVersionTag;
    std::string trustedCertificatesPem;
    std::string certificateChainPem;
    std::string privateKeyPem;
    std::string cipherList;
    std::string curveList;
    uint64_t acceptTimeoutNanos;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedHttpRequest {
    uint32_t method;
    std::string address;
    uint32_t portHint;
    std::string path;
    std::vector<uint8_t> requestHeaders;
    std::shared_ptr<std::vector<uint8_t>> body;
    bool useTls;
    uint64_t responseBodyLimit;
    std::shared_ptr<HttpResponseCompletion> completion;
  };

  struct QueuedHttpRequestStart {
    uint32_t method;
    std::string address;
    uint32_t portHint;
    std::string path;
    std::vector<uint8_t> requestHeaders;
    std::shared_ptr<std::vector<uint8_t>> body;
    bool useTls;
    uint64_t responseBodyLimit;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedHttpRequestStartStreaming {
    uint32_t method;
    std::string address;
    uint32_t portHint;
    std::string path;
    std::vector<uint8_t> requestHeaders;
    bool useTls;
    std::shared_ptr<HandlePairCompletion> completion;
  };

  struct QueuedHttpResponsePromiseAwait {
    uint32_t promiseId;
    std::shared_ptr<HttpResponseCompletion> completion;
  };

  struct QueuedHttpResponsePromiseAwaitStreaming {
    uint32_t promiseId;
    std::shared_ptr<HttpResponseCompletion> completion;
  };

  struct QueuedHttpResponsePromiseCancel {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedHttpResponsePromiseRelease {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedHttpRequestBodyWriteStart {
    uint32_t requestBodyId;
    std::shared_ptr<std::vector<uint8_t>> bytes;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedHttpRequestBodyWrite {
    uint32_t requestBodyId;
    std::shared_ptr<std::vector<uint8_t>> bytes;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedHttpRequestBodyFinishStart {
    uint32_t requestBodyId;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedHttpRequestBodyFinish {
    uint32_t requestBodyId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedHttpRequestBodyRelease {
    uint32_t requestBodyId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedHttpResponseBodyReadStart {
    uint32_t responseBodyId;
    uint32_t minBytes;
    uint32_t maxBytes;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedHttpResponseBodyRead {
    uint32_t responseBodyId;
    uint32_t minBytes;
    uint32_t maxBytes;
    std::shared_ptr<BytesCompletion> completion;
  };

  struct QueuedHttpResponseBodyRelease {
    uint32_t responseBodyId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedHttpServerRequestBodyReadStart {
    uint32_t requestBodyId;
    uint32_t minBytes;
    uint32_t maxBytes;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedHttpServerRequestBodyRead {
    uint32_t requestBodyId;
    uint32_t minBytes;
    uint32_t maxBytes;
    std::shared_ptr<BytesCompletion> completion;
  };

  struct QueuedHttpServerRequestBodyRelease {
    uint32_t requestBodyId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedWebSocketConnect {
    std::string address;
    uint32_t portHint;
    std::string path;
    std::vector<uint8_t> requestHeaders;
    bool useTls;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedWebSocketConnectStart {
    std::string address;
    uint32_t portHint;
    std::string path;
    std::vector<uint8_t> requestHeaders;
    bool useTls;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedWebSocketPromiseAwait {
    uint32_t promiseId;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedWebSocketPromiseCancel {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedWebSocketPromiseRelease {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedWebSocketRelease {
    uint32_t webSocketId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedWebSocketSendTextStart {
    uint32_t webSocketId;
    std::string text;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedWebSocketSendText {
    uint32_t webSocketId;
    std::string text;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedWebSocketSendBinaryStart {
    uint32_t webSocketId;
    std::shared_ptr<std::vector<uint8_t>> bytes;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedWebSocketSendBinary {
    uint32_t webSocketId;
    std::shared_ptr<std::vector<uint8_t>> bytes;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedHttpServerListen {
    std::string address;
    uint32_t portHint;
    bool useTls;
    HttpServerConfig config;
    std::shared_ptr<HandlePairCompletion> completion;
  };

  struct QueuedHttpServerRelease {
    uint32_t serverId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedHttpServerDrainStart {
    uint32_t serverId;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedHttpServerDrain {
    uint32_t serverId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedHttpServerPollRequest {
    uint32_t serverId;
    std::shared_ptr<HttpServerRequestCompletion> completion;
  };

  struct QueuedHttpServerRespond {
    uint32_t serverId;
    uint32_t requestId;
    uint32_t statusCode;
    std::string statusText;
    std::vector<uint8_t> responseHeaders;
    std::shared_ptr<std::vector<uint8_t>> body;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedHttpServerRespondWebSocket {
    uint32_t serverId;
    uint32_t requestId;
    std::vector<uint8_t> responseHeaders;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedHttpServerRespondStartStreaming {
    uint32_t serverId;
    uint32_t requestId;
    uint32_t statusCode;
    std::string statusText;
    std::vector<uint8_t> responseHeaders;
    std::shared_ptr<HandleCompletion> completion;
  };

  struct QueuedHttpServerResponseBodyWriteStart {
    uint32_t responseBodyId;
    std::shared_ptr<std::vector<uint8_t>> bytes;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedHttpServerResponseBodyWrite {
    uint32_t responseBodyId;
    std::shared_ptr<std::vector<uint8_t>> bytes;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedHttpServerResponseBodyFinishStart {
    uint32_t responseBodyId;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedHttpServerResponseBodyFinish {
    uint32_t responseBodyId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedHttpServerResponseBodyRelease {
    uint32_t responseBodyId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedWebSocketReceiveStart {
    uint32_t webSocketId;
    uint32_t maxBytes;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedWebSocketMessagePromiseAwait {
    uint32_t promiseId;
    std::shared_ptr<WebSocketMessageCompletion> completion;
  };

  struct QueuedWebSocketMessagePromiseCancel {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedWebSocketMessagePromiseRelease {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedWebSocketReceive {
    uint32_t webSocketId;
    uint32_t maxBytes;
    std::shared_ptr<WebSocketMessageCompletion> completion;
  };

  struct QueuedWebSocketCloseStart {
    uint32_t webSocketId;
    uint16_t closeCode;
    std::string reason;
    std::shared_ptr<PromiseIdCompletion> completion;
  };

  struct QueuedWebSocketClose {
    uint32_t webSocketId;
    uint16_t closeCode;
    std::string reason;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedWebSocketDisconnect {
    uint32_t webSocketId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedWebSocketAbort {
    uint32_t webSocketId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedNewWebSocketPipe {
    std::shared_ptr<HandlePairCompletion> completion;
  };

  using QueuedOperation =
      std::variant<QueuedSleepNanos, QueuedAwaitPromise, QueuedCancelPromise,
                   QueuedReleasePromise, QueuedPromiseThenStart, QueuedPromiseCatchStart,
                   QueuedListen, QueuedReleaseListener, QueuedAccept,
                   QueuedAcceptStart, QueuedConnect, QueuedConnectStart, QueuedParseAddress,
                   QueuedReleaseNetworkAddress, QueuedNetworkAddressToString,
                   QueuedNetworkAddressClone, QueuedNetworkAddressConnect,
                   QueuedNetworkAddressConnectStart, QueuedNetworkAddressListen,
                   QueuedNetworkAddressBindDatagramPort,
                   QueuedAwaitConnectionPromise, QueuedAwaitConnectionPromiseWithTimeout,
                   QueuedCancelConnectionPromise,
                   QueuedReleaseConnectionPromise, QueuedReleaseConnection,
                   QueuedConnectionWrite, QueuedConnectionWriteStart, QueuedConnectionRead,
                   QueuedConnectionReadStart, QueuedBytesPromiseAwait,
                   QueuedBytesPromiseCancel, QueuedBytesPromiseRelease,
                   QueuedConnectionShutdownWrite, QueuedConnectionShutdownWriteStart,
                   QueuedPromiseAllStart, QueuedPromiseRaceStart, QueuedTaskSetNew,
                   QueuedTaskSetRelease, QueuedTaskSetAddPromise, QueuedTaskSetClear,
                   QueuedTaskSetIsEmpty, QueuedTaskSetOnEmptyStart, QueuedTaskSetErrorCount,
                   QueuedTaskSetTakeLastError, QueuedConnectionWhenWriteDisconnectedStart,
                   QueuedConnectionAbortRead, QueuedConnectionAbortWrite, QueuedConnectionDupFd,
                   QueuedWrapSocketFd, QueuedWrapSocketFdTake,
                   QueuedWrapListenSocketFd, QueuedWrapListenSocketFdTake,
                   QueuedWrapDatagramSocketFd, QueuedWrapDatagramSocketFdTake,
                   QueuedNewTwoWayPipe, QueuedNewCapabilityPipe,
                   QueuedDatagramBind, QueuedDatagramReleasePort, QueuedDatagramGetPort,
                   QueuedDatagramSend, QueuedDatagramSendStart, QueuedUInt32PromiseAwait,
                   QueuedUInt32PromiseCancel, QueuedUInt32PromiseRelease, QueuedDatagramReceive,
                   QueuedDatagramReceiveStart, QueuedDatagramReceivePromiseAwait,
                   QueuedDatagramReceivePromiseCancel, QueuedDatagramReceivePromiseRelease,
                   QueuedEnableTls, QueuedConfigureTls, QueuedHttpRequest, QueuedHttpRequestStart,
                   QueuedHttpRequestStartStreaming, QueuedHttpResponsePromiseAwait,
                   QueuedHttpResponsePromiseAwaitStreaming, QueuedHttpResponsePromiseCancel,
                   QueuedHttpResponsePromiseRelease, QueuedHttpRequestBodyWriteStart,
                   QueuedHttpRequestBodyWrite, QueuedHttpRequestBodyFinishStart,
                   QueuedHttpRequestBodyFinish, QueuedHttpRequestBodyRelease,
                   QueuedHttpResponseBodyReadStart, QueuedHttpResponseBodyRead,
                   QueuedHttpResponseBodyRelease, QueuedHttpServerRequestBodyReadStart,
                   QueuedHttpServerRequestBodyRead, QueuedHttpServerRequestBodyRelease,
                   QueuedWebSocketConnect, QueuedWebSocketConnectStart,
                   QueuedWebSocketPromiseAwait, QueuedWebSocketPromiseCancel,
                   QueuedWebSocketPromiseRelease, QueuedWebSocketRelease,
                   QueuedWebSocketSendTextStart, QueuedWebSocketSendText,
                   QueuedWebSocketSendBinaryStart, QueuedWebSocketSendBinary,
                   QueuedWebSocketReceiveStart, QueuedWebSocketMessagePromiseAwait,
                   QueuedWebSocketMessagePromiseCancel, QueuedWebSocketMessagePromiseRelease,
                   QueuedWebSocketReceive, QueuedWebSocketCloseStart, QueuedWebSocketClose,
                   QueuedWebSocketDisconnect, QueuedWebSocketAbort, QueuedNewWebSocketPipe,
                   QueuedHttpServerListen, QueuedHttpServerRelease, QueuedHttpServerDrainStart,
                   QueuedHttpServerDrain, QueuedHttpServerPollRequest,
                   QueuedHttpServerRespond, QueuedHttpServerRespondWebSocket,
                   QueuedHttpServerRespondStartStreaming,
                   QueuedHttpServerResponseBodyWriteStart, QueuedHttpServerResponseBodyWrite,
                   QueuedHttpServerResponseBodyFinishStart, QueuedHttpServerResponseBodyFinish,
                   QueuedHttpServerResponseBodyRelease>;

  uint32_t addPromise(PendingPromise&& promise) {
    uint32_t promiseId = nextPromiseId_++;
    while (promises_.find(promiseId) != promises_.end()) {
      promiseId = nextPromiseId_++;
    }
    retiredPromiseIds_.erase(promiseId);
    promises_.emplace(promiseId, std::move(promise));
    return promiseId;
  }

  uint32_t addConnectionPromise(PendingConnectionPromise&& promise) {
    uint32_t promiseId = nextConnectionPromiseId_++;
    while (connectionPromises_.find(promiseId) != connectionPromises_.end()) {
      promiseId = nextConnectionPromiseId_++;
    }
    connectionPromises_.emplace(promiseId, std::move(promise));
    return promiseId;
  }

  uint32_t addBytesPromise(PendingBytesPromise&& promise) {
    uint32_t promiseId = nextBytesPromiseId_++;
    while (bytesPromises_.find(promiseId) != bytesPromises_.end()) {
      promiseId = nextBytesPromiseId_++;
    }
    bytesPromises_.emplace(promiseId, std::move(promise));
    return promiseId;
  }

  uint32_t addUInt32Promise(PendingUInt32Promise&& promise) {
    uint32_t promiseId = nextUInt32PromiseId_++;
    while (uint32Promises_.find(promiseId) != uint32Promises_.end()) {
      promiseId = nextUInt32PromiseId_++;
    }
    uint32Promises_.emplace(promiseId, std::move(promise));
    return promiseId;
  }

  uint32_t addDatagramReceivePromise(PendingDatagramReceivePromise&& promise) {
    uint32_t promiseId = nextDatagramReceivePromiseId_++;
    while (datagramReceivePromises_.find(promiseId) != datagramReceivePromises_.end()) {
      promiseId = nextDatagramReceivePromiseId_++;
    }
    datagramReceivePromises_.emplace(promiseId, std::move(promise));
    return promiseId;
  }

  uint32_t addHttpResponsePromise(PendingHttpResponsePromise&& promise) {
    uint32_t promiseId = nextHttpResponsePromiseId_++;
    while (httpResponsePromises_.find(promiseId) != httpResponsePromises_.end()) {
      promiseId = nextHttpResponsePromiseId_++;
    }
    httpResponsePromises_.emplace(promiseId, std::move(promise));
    return promiseId;
  }

  uint32_t addWebSocketPromise(PendingWebSocketPromise&& promise) {
    uint32_t promiseId = nextWebSocketPromiseId_++;
    while (webSocketPromises_.find(promiseId) != webSocketPromises_.end()) {
      promiseId = nextWebSocketPromiseId_++;
    }
    webSocketPromises_.emplace(promiseId, std::move(promise));
    return promiseId;
  }

  uint32_t addWebSocketMessagePromise(PendingWebSocketMessagePromise&& promise) {
    uint32_t promiseId = nextWebSocketMessagePromiseId_++;
    while (webSocketMessagePromises_.find(promiseId) != webSocketMessagePromises_.end()) {
      promiseId = nextWebSocketMessagePromiseId_++;
    }
    webSocketMessagePromises_.emplace(promiseId, std::move(promise));
    return promiseId;
  }

  uint32_t addListener(kj::Own<kj::ConnectionReceiver>&& listener) {
    uint32_t listenerId = nextListenerId_++;
    while (listeners_.find(listenerId) != listeners_.end()) {
      listenerId = nextListenerId_++;
    }
    listeners_.emplace(listenerId, kj::mv(listener));
    return listenerId;
  }

  uint32_t addNetworkAddress(kj::Own<kj::NetworkAddress>&& address) {
    uint32_t addressId = nextNetworkAddressId_++;
    while (networkAddresses_.find(addressId) != networkAddresses_.end()) {
      addressId = nextNetworkAddressId_++;
    }
    networkAddresses_.emplace(addressId, kj::mv(address));
    return addressId;
  }

  uint32_t addConnection(kj::Own<kj::AsyncIoStream>&& stream) {
    uint32_t connectionId = nextConnectionId_++;
    while (connections_.find(connectionId) != connections_.end()) {
      connectionId = nextConnectionId_++;
    }
    connections_.emplace(connectionId, kj::mv(stream));
    return connectionId;
  }

  uint32_t addTaskSet(kj::Own<RuntimeTaskSet>&& taskSet) {
    uint32_t taskSetId = nextTaskSetId_++;
    while (taskSets_.find(taskSetId) != taskSets_.end()) {
      taskSetId = nextTaskSetId_++;
    }
    taskSets_.emplace(taskSetId, kj::mv(taskSet));
    return taskSetId;
  }

  uint32_t addDatagramPort(kj::Own<kj::DatagramPort>&& port) {
    uint32_t portId = nextDatagramPortId_++;
    while (datagramPorts_.find(portId) != datagramPorts_.end()) {
      portId = nextDatagramPortId_++;
    }
    datagramPorts_.emplace(portId, kj::mv(port));
    return portId;
  }

  uint32_t addWebSocket(kj::Own<kj::WebSocket>&& socket,
                        kj::Maybe<kj::Own<RuntimeWebSocketOwner>> owner = kj::none,
                        kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> requestCompletion =
                            kj::none) {
    uint32_t webSocketId = nextWebSocketId_++;
    while (webSockets_.find(webSocketId) != webSockets_.end()) {
      webSocketId = nextWebSocketId_++;
    }
    webSockets_.emplace(webSocketId,
                        RuntimeWebSocket{kj::mv(owner), kj::mv(requestCompletion),
                                         kj::mv(socket)});
    return webSocketId;
  }

  uint32_t addHttpServer(kj::Own<HttpServerState>&& serverState) {
    uint32_t serverId = nextHttpServerId_++;
    while (httpServers_.find(serverId) != httpServers_.end()) {
      serverId = nextHttpServerId_++;
    }
    httpServers_.emplace(serverId, kj::mv(serverState));
    return serverId;
  }

  uint32_t addHttpRequestBody(kj::Own<kj::AsyncOutputStream>&& stream,
                              std::shared_ptr<RuntimeHttpClientOwner> owner) {
    uint32_t requestBodyId = nextHttpRequestBodyId_++;
    while (httpRequestBodies_.find(requestBodyId) != httpRequestBodies_.end()) {
      requestBodyId = nextHttpRequestBodyId_++;
    }
    httpRequestBodies_.emplace(
        requestBodyId, RuntimeHttpRequestBody{kj::mv(stream), std::move(owner)});
    return requestBodyId;
  }

  uint32_t addHttpResponseBody(kj::Own<kj::AsyncInputStream>&& stream,
                               std::shared_ptr<RuntimeHttpClientOwner> owner) {
    uint32_t responseBodyId = nextHttpResponseBodyId_++;
    while (httpResponseBodies_.find(responseBodyId) != httpResponseBodies_.end()) {
      responseBodyId = nextHttpResponseBodyId_++;
    }
    httpResponseBodies_.emplace(
        responseBodyId, RuntimeHttpResponseBody{kj::mv(stream), std::move(owner)});
    return responseBodyId;
  }

  uint32_t addHttpServerRequestBody(kj::AsyncInputStream& stream, uint32_t serverId,
                                    uint32_t requestId) {
    uint32_t requestBodyId = nextHttpServerRequestBodyId_++;
    while (httpServerRequestBodies_.find(requestBodyId) != httpServerRequestBodies_.end()) {
      requestBodyId = nextHttpServerRequestBodyId_++;
    }
    httpServerRequestBodies_.emplace(
        requestBodyId, RuntimeHttpServerRequestBody{&stream, serverId, requestId});
    return requestBodyId;
  }

  uint32_t addHttpServerResponseBody(kj::Own<kj::AsyncOutputStream>&& stream,
                                     kj::Own<kj::PromiseFulfiller<void>>&& doneFulfiller,
                                     uint32_t serverId, uint32_t requestId) {
    uint32_t responseBodyId = nextHttpServerResponseBodyId_++;
    while (httpServerResponseBodies_.find(responseBodyId) != httpServerResponseBodies_.end()) {
      responseBodyId = nextHttpServerResponseBodyId_++;
    }
    httpServerResponseBodies_.emplace(responseBodyId,
                                      RuntimeHttpServerResponseBody{kj::mv(stream),
                                                                    kj::mv(doneFulfiller),
                                                                    serverId,
                                                                    requestId});
    return responseBodyId;
  }

  void releaseHttpServerRequestBodiesForRequest(uint32_t serverId, uint32_t requestId) {
    std::vector<uint32_t> ids;
    ids.reserve(httpServerRequestBodies_.size());
    for (const auto& entry : httpServerRequestBodies_) {
      if (entry.second.serverId == serverId && entry.second.requestId == requestId) {
        ids.push_back(entry.first);
      }
    }
    for (auto id : ids) {
      httpServerRequestBodies_.erase(id);
    }
  }

  void releaseHttpServerResponseBodiesForRequest(uint32_t serverId, uint32_t requestId) {
    std::vector<uint32_t> ids;
    ids.reserve(httpServerResponseBodies_.size());
    for (const auto& entry : httpServerResponseBodies_) {
      if (entry.second.serverId == serverId && entry.second.requestId == requestId) {
        ids.push_back(entry.first);
      }
    }
    for (auto id : ids) {
      auto it = httpServerResponseBodies_.find(id);
      if (it != httpServerResponseBodies_.end()) {
        auto body = kj::mv(it->second);
        httpServerResponseBodies_.erase(it);
        body.stream = nullptr;
        body.doneFulfiller->reject(kj::Exception(kj::Exception::Type::DISCONNECTED, __FILE__,
                                                 __LINE__,
                                                 kj::str("HTTP server response stream released")));
      }
    }
  }

  void releaseHttpServerBodiesForServer(uint32_t serverId) {
    std::vector<uint32_t> requestBodyIds;
    requestBodyIds.reserve(httpServerRequestBodies_.size());
    for (const auto& entry : httpServerRequestBodies_) {
      if (entry.second.serverId == serverId) {
        requestBodyIds.push_back(entry.first);
      }
    }
    for (auto id : requestBodyIds) {
      httpServerRequestBodies_.erase(id);
    }

    std::vector<uint32_t> responseBodyIds;
    responseBodyIds.reserve(httpServerResponseBodies_.size());
    for (const auto& entry : httpServerResponseBodies_) {
      if (entry.second.serverId == serverId) {
        responseBodyIds.push_back(entry.first);
      }
    }
    for (auto id : responseBodyIds) {
      auto it = httpServerResponseBodies_.find(id);
      if (it != httpServerResponseBodies_.end()) {
        auto body = kj::mv(it->second);
        httpServerResponseBodies_.erase(it);
        body.stream = nullptr;
        body.doneFulfiller->reject(
            kj::Exception(kj::Exception::Type::DISCONNECTED, __FILE__, __LINE__,
                          kj::str("HTTP server released while streaming response")));
      }
    }
  }

  uint32_t promiseAllStart(std::vector<uint32_t> promiseIds) {
    auto promises = kj::heapArrayBuilder<kj::Promise<void>>(promiseIds.size());
    for (auto promiseId : promiseIds) {
      auto it = promises_.find(promiseId);
      if (it == promises_.end()) {
        if (retiredPromiseIds_.find(promiseId) != retiredPromiseIds_.end()) {
          throw std::runtime_error("KJ promise id already consumed or released: " +
                                   std::to_string(promiseId));
        }
        throw std::runtime_error("unknown KJ promise id: " + std::to_string(promiseId));
      }
      auto pending = kj::mv(it->second);
      promises_.erase(it);
      retiredPromiseIds_.insert(promiseId);
      pending.canceler->release();
      promises.add(kj::mv(pending.promise));
    }
    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(kj::joinPromises(promises.finish()));
    return addPromise(PendingPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t promiseRaceStart(std::vector<uint32_t> promiseIds) {
    if (promiseIds.empty()) {
      throw std::runtime_error("promiseRaceStart requires at least one promise id");
    }

    auto firstIt = promises_.find(promiseIds[0]);
    if (firstIt == promises_.end()) {
      if (retiredPromiseIds_.find(promiseIds[0]) != retiredPromiseIds_.end()) {
        throw std::runtime_error("KJ promise id already consumed or released: " +
                                 std::to_string(promiseIds[0]));
      }
      throw std::runtime_error("unknown KJ promise id: " + std::to_string(promiseIds[0]));
    }
    auto firstPending = kj::mv(firstIt->second);
    promises_.erase(firstIt);
    retiredPromiseIds_.insert(promiseIds[0]);
    firstPending.canceler->release();
    auto raced = kj::mv(firstPending.promise);

    for (size_t i = 1; i < promiseIds.size(); ++i) {
      auto promiseId = promiseIds[i];
      auto it = promises_.find(promiseId);
      if (it == promises_.end()) {
        if (retiredPromiseIds_.find(promiseId) != retiredPromiseIds_.end()) {
          throw std::runtime_error("KJ promise id already consumed or released: " +
                                   std::to_string(promiseId));
        }
        throw std::runtime_error("unknown KJ promise id: " + std::to_string(promiseId));
      }
      auto pending = kj::mv(it->second);
      promises_.erase(it);
      retiredPromiseIds_.insert(promiseId);
      pending.canceler->release();
      raced = kj::mv(raced).exclusiveJoin(kj::mv(pending.promise));
    }

    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(kj::mv(raced));
    return addPromise(PendingPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t promiseThenStart(uint32_t firstPromiseId, uint32_t secondPromiseId) {
    auto firstIt = promises_.find(firstPromiseId);
    if (firstIt == promises_.end()) {
      if (retiredPromiseIds_.find(firstPromiseId) != retiredPromiseIds_.end()) {
        throw std::runtime_error("KJ promise id already consumed or released: " +
                                 std::to_string(firstPromiseId));
      }
      throw std::runtime_error("unknown KJ promise id: " + std::to_string(firstPromiseId));
    }
    auto firstPending = kj::mv(firstIt->second);
    promises_.erase(firstIt);
    retiredPromiseIds_.insert(firstPromiseId);
    firstPending.canceler->release();

    auto secondIt = promises_.find(secondPromiseId);
    if (secondIt == promises_.end()) {
      if (retiredPromiseIds_.find(secondPromiseId) != retiredPromiseIds_.end()) {
        throw std::runtime_error("KJ promise id already consumed or released: " +
                                 std::to_string(secondPromiseId));
      }
      throw std::runtime_error("unknown KJ promise id: " + std::to_string(secondPromiseId));
    }
    auto secondPending = kj::mv(secondIt->second);
    promises_.erase(secondIt);
    retiredPromiseIds_.insert(secondPromiseId);
    secondPending.canceler->release();

    auto canceler = kj::heap<kj::Canceler>();
    auto second = kj::mv(secondPending.promise);
    auto promise = canceler->wrap(kj::mv(firstPending.promise).then(
        [second = kj::mv(second)]() mutable { return kj::mv(second); }));
    return addPromise(PendingPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t promiseCatchStart(uint32_t promiseId, uint32_t fallbackPromiseId) {
    auto firstIt = promises_.find(promiseId);
    if (firstIt == promises_.end()) {
      if (retiredPromiseIds_.find(promiseId) != retiredPromiseIds_.end()) {
        throw std::runtime_error("KJ promise id already consumed or released: " +
                                 std::to_string(promiseId));
      }
      throw std::runtime_error("unknown KJ promise id: " + std::to_string(promiseId));
    }
    auto firstPending = kj::mv(firstIt->second);
    promises_.erase(firstIt);
    retiredPromiseIds_.insert(promiseId);
    firstPending.canceler->release();

    auto fallbackIt = promises_.find(fallbackPromiseId);
    if (fallbackIt == promises_.end()) {
      if (retiredPromiseIds_.find(fallbackPromiseId) != retiredPromiseIds_.end()) {
        throw std::runtime_error("KJ promise id already consumed or released: " +
                                 std::to_string(fallbackPromiseId));
      }
      throw std::runtime_error("unknown KJ promise id: " + std::to_string(fallbackPromiseId));
    }
    auto fallbackPending = kj::mv(fallbackIt->second);
    promises_.erase(fallbackIt);
    retiredPromiseIds_.insert(fallbackPromiseId);
    fallbackPending.canceler->release();

    auto canceler = kj::heap<kj::Canceler>();
    auto fallback = kj::mv(fallbackPending.promise);
    auto recovered = kj::mv(firstPending.promise).then(
        []() -> kj::Promise<void> { return kj::READY_NOW; },
        [fallback = kj::mv(fallback)](kj::Exception&&) mutable -> kj::Promise<void> {
          return kj::mv(fallback);
        });
    auto promise = canceler->wrap(kj::mv(recovered));
    return addPromise(PendingPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t taskSetNew() { return addTaskSet(kj::heap<RuntimeTaskSet>()); }

  void taskSetRelease(uint32_t taskSetId) {
    auto erased = taskSets_.erase(taskSetId);
    if (erased == 0) {
      throw std::runtime_error("unknown KJ task set id: " + std::to_string(taskSetId));
    }
  }

  void taskSetAddPromise(uint32_t taskSetId, uint32_t promiseId) {
    auto taskSetIt = taskSets_.find(taskSetId);
    if (taskSetIt == taskSets_.end()) {
      throw std::runtime_error("unknown KJ task set id: " + std::to_string(taskSetId));
    }
    auto promiseIt = promises_.find(promiseId);
    if (promiseIt == promises_.end()) {
      throw std::runtime_error("unknown KJ promise id: " + std::to_string(promiseId));
    }
    auto pending = kj::mv(promiseIt->second);
    promises_.erase(promiseIt);
    retiredPromiseIds_.insert(promiseId);
    pending.canceler->release();
    taskSetIt->second->tasks.add(kj::mv(pending.promise));
  }

  void taskSetClear(uint32_t taskSetId) {
    auto taskSetIt = taskSets_.find(taskSetId);
    if (taskSetIt == taskSets_.end()) {
      throw std::runtime_error("unknown KJ task set id: " + std::to_string(taskSetId));
    }
    taskSetIt->second->tasks.clear();
  }

  bool taskSetIsEmpty(uint32_t taskSetId) {
    auto taskSetIt = taskSets_.find(taskSetId);
    if (taskSetIt == taskSets_.end()) {
      throw std::runtime_error("unknown KJ task set id: " + std::to_string(taskSetId));
    }
    return taskSetIt->second->tasks.isEmpty();
  }

  uint32_t taskSetOnEmptyStart(uint32_t taskSetId) {
    auto taskSetIt = taskSets_.find(taskSetId);
    if (taskSetIt == taskSets_.end()) {
      throw std::runtime_error("unknown KJ task set id: " + std::to_string(taskSetId));
    }
    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(taskSetIt->second->tasks.onEmpty());
    return addPromise(PendingPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t taskSetErrorCount(uint32_t taskSetId) {
    auto taskSetIt = taskSets_.find(taskSetId);
    if (taskSetIt == taskSets_.end()) {
      throw std::runtime_error("unknown KJ task set id: " + std::to_string(taskSetId));
    }
    std::lock_guard<std::mutex> lock(taskSetIt->second->mutex);
    return taskSetIt->second->errorCount;
  }

  kj::Maybe<std::string> taskSetTakeLastError(uint32_t taskSetId) {
    auto taskSetIt = taskSets_.find(taskSetId);
    if (taskSetIt == taskSets_.end()) {
      throw std::runtime_error("unknown KJ task set id: " + std::to_string(taskSetId));
    }
    std::lock_guard<std::mutex> lock(taskSetIt->second->mutex);
    if (taskSetIt->second->lastError.empty()) {
      return kj::none;
    }
    auto out = taskSetIt->second->lastError;
    taskSetIt->second->lastError.clear();
    return out;
  }

  void awaitPromise(kj::WaitScope& waitScope, uint32_t promiseId) {
    auto it = promises_.find(promiseId);
    if (it == promises_.end()) {
      if (retiredPromiseIds_.find(promiseId) != retiredPromiseIds_.end()) {
        return;
      }
      throw std::runtime_error("unknown KJ promise id: " + std::to_string(promiseId));
    }

    auto pending = kj::mv(it->second);
    promises_.erase(it);
    retiredPromiseIds_.insert(promiseId);
    kj::mv(pending.promise).wait(waitScope);
  }

  void cancelPromise(uint32_t promiseId) {
    auto it = promises_.find(promiseId);
    if (it == promises_.end()) {
      if (retiredPromiseIds_.find(promiseId) != retiredPromiseIds_.end()) {
        return;
      }
      throw std::runtime_error("unknown KJ promise id: " + std::to_string(promiseId));
    }
    it->second.canceler->cancel("Capnp.KjAsync promise canceled from Lean");
  }

  void releasePromise(uint32_t promiseId) {
    auto it = promises_.find(promiseId);
    if (it == promises_.end()) {
      if (retiredPromiseIds_.find(promiseId) != retiredPromiseIds_.end()) {
        return;
      }
      throw std::runtime_error("unknown KJ promise id: " + std::to_string(promiseId));
    }
    promises_.erase(it);
    retiredPromiseIds_.insert(promiseId);
  }

  uint32_t awaitConnectionPromise(kj::WaitScope& waitScope, uint32_t promiseId) {
    auto it = connectionPromises_.find(promiseId);
    if (it == connectionPromises_.end()) {
      throw std::runtime_error("unknown KJ connection promise id: " + std::to_string(promiseId));
    }

    auto pending = kj::mv(it->second);
    connectionPromises_.erase(it);
    auto stream = kj::mv(pending.promise).wait(waitScope);
    return addConnection(kj::mv(stream));
  }

  kj::Maybe<uint32_t> awaitConnectionPromiseWithTimeout(
      kj::AsyncIoProvider& ioProvider, kj::WaitScope& waitScope, uint32_t promiseId,
      uint64_t timeoutNanos) {
    if (timeoutNanos == 0) {
      return kj::Maybe<uint32_t>(awaitConnectionPromise(waitScope, promiseId));
    }

    auto it = connectionPromises_.find(promiseId);
    if (it == connectionPromises_.end()) {
      throw std::runtime_error("unknown KJ connection promise id: " + std::to_string(promiseId));
    }

    auto pending = kj::mv(it->second);
    connectionPromises_.erase(it);

    auto timeout = ioProvider.getTimer()
                       .afterDelay(nanosToDuration(timeoutNanos, "connection promise timeout"))
                       .then([]() -> kj::Maybe<kj::Own<kj::AsyncIoStream>> { return kj::none; });

    auto connect = kj::mv(pending.promise).then(
        [](kj::Own<kj::AsyncIoStream>&& stream) -> kj::Maybe<kj::Own<kj::AsyncIoStream>> {
          return kj::mv(stream);
        });

    auto raced = kj::mv(connect).exclusiveJoin(kj::mv(timeout));
    auto result = kj::mv(raced).wait(waitScope);
    KJ_IF_SOME(stream, result) {
      return addConnection(kj::mv(stream));
    }

    pending.canceler->cancel(
        "Capnp.KjAsync connection promise timed out from Lean awaitWithTimeout");
    return kj::none;
  }

  void cancelConnectionPromise(uint32_t promiseId) {
    auto it = connectionPromises_.find(promiseId);
    if (it == connectionPromises_.end()) {
      throw std::runtime_error("unknown KJ connection promise id: " + std::to_string(promiseId));
    }
    it->second.canceler->cancel("Capnp.KjAsync connection promise canceled from Lean");
  }

  void releaseConnectionPromise(uint32_t promiseId) {
    auto it = connectionPromises_.find(promiseId);
    if (it == connectionPromises_.end()) {
      throw std::runtime_error("unknown KJ connection promise id: " + std::to_string(promiseId));
    }
    connectionPromises_.erase(it);
  }

  std::shared_ptr<std::vector<uint8_t>> awaitBytesPromise(kj::WaitScope& waitScope,
                                                          uint32_t promiseId) {
    auto it = bytesPromises_.find(promiseId);
    if (it == bytesPromises_.end()) {
      throw std::runtime_error("unknown KJ bytes promise id: " + std::to_string(promiseId));
    }
    auto pending = kj::mv(it->second);
    bytesPromises_.erase(it);
    auto bytes = kj::mv(pending.promise).wait(waitScope);
    if (!bytes) {
      return std::make_shared<std::vector<uint8_t>>();
    }
    return bytes;
  }

  void cancelBytesPromise(uint32_t promiseId) {
    auto it = bytesPromises_.find(promiseId);
    if (it == bytesPromises_.end()) {
      throw std::runtime_error("unknown KJ bytes promise id: " + std::to_string(promiseId));
    }
    it->second.canceler->cancel("Capnp.KjAsync bytes promise canceled from Lean");
  }

  void releaseBytesPromise(uint32_t promiseId) {
    auto it = bytesPromises_.find(promiseId);
    if (it == bytesPromises_.end()) {
      throw std::runtime_error("unknown KJ bytes promise id: " + std::to_string(promiseId));
    }
    bytesPromises_.erase(it);
  }

  uint32_t awaitUInt32Promise(kj::WaitScope& waitScope, uint32_t promiseId) {
    auto it = uint32Promises_.find(promiseId);
    if (it == uint32Promises_.end()) {
      throw std::runtime_error("unknown KJ UInt32 promise id: " + std::to_string(promiseId));
    }
    auto pending = kj::mv(it->second);
    uint32Promises_.erase(it);
    return kj::mv(pending.promise).wait(waitScope);
  }

  void cancelUInt32Promise(uint32_t promiseId) {
    auto it = uint32Promises_.find(promiseId);
    if (it == uint32Promises_.end()) {
      throw std::runtime_error("unknown KJ UInt32 promise id: " + std::to_string(promiseId));
    }
    it->second.canceler->cancel("Capnp.KjAsync UInt32 promise canceled from Lean");
  }

  void releaseUInt32Promise(uint32_t promiseId) {
    auto it = uint32Promises_.find(promiseId);
    if (it == uint32Promises_.end()) {
      throw std::runtime_error("unknown KJ UInt32 promise id: " + std::to_string(promiseId));
    }
    uint32Promises_.erase(it);
  }

  DatagramReceiveResult awaitDatagramReceivePromise(kj::WaitScope& waitScope, uint32_t promiseId) {
    auto it = datagramReceivePromises_.find(promiseId);
    if (it == datagramReceivePromises_.end()) {
      throw std::runtime_error("unknown KJ datagram receive promise id: " +
                               std::to_string(promiseId));
    }
    auto pending = kj::mv(it->second);
    datagramReceivePromises_.erase(it);
    return kj::mv(pending.promise).wait(waitScope);
  }

  void cancelDatagramReceivePromise(uint32_t promiseId) {
    auto it = datagramReceivePromises_.find(promiseId);
    if (it == datagramReceivePromises_.end()) {
      throw std::runtime_error("unknown KJ datagram receive promise id: " +
                               std::to_string(promiseId));
    }
    it->second.canceler->cancel("Capnp.KjAsync datagram receive promise canceled from Lean");
  }

  void releaseDatagramReceivePromise(uint32_t promiseId) {
    auto it = datagramReceivePromises_.find(promiseId);
    if (it == datagramReceivePromises_.end()) {
      throw std::runtime_error("unknown KJ datagram receive promise id: " +
                               std::to_string(promiseId));
    }
    datagramReceivePromises_.erase(it);
  }

  HttpResponseResult awaitHttpResponsePromise(kj::WaitScope& waitScope, uint32_t promiseId) {
    auto it = httpResponsePromises_.find(promiseId);
    if (it == httpResponsePromises_.end()) {
      throw std::runtime_error("unknown KJ HTTP response promise id: " +
                               std::to_string(promiseId));
    }
    auto pending = kj::mv(it->second);
    httpResponsePromises_.erase(it);
    return kj::mv(pending.promise).wait(waitScope);
  }

  void cancelHttpResponsePromise(uint32_t promiseId) {
    auto it = httpResponsePromises_.find(promiseId);
    if (it == httpResponsePromises_.end()) {
      throw std::runtime_error("unknown KJ HTTP response promise id: " +
                               std::to_string(promiseId));
    }
    it->second.canceler->cancel("Capnp.KjAsync HTTP response promise canceled from Lean");
  }

  void releaseHttpResponsePromise(uint32_t promiseId) {
    auto it = httpResponsePromises_.find(promiseId);
    if (it == httpResponsePromises_.end()) {
      throw std::runtime_error("unknown KJ HTTP response promise id: " +
                               std::to_string(promiseId));
    }
    httpResponsePromises_.erase(it);
  }

  uint32_t awaitWebSocketPromise(kj::WaitScope& waitScope, uint32_t promiseId) {
    auto it = webSocketPromises_.find(promiseId);
    if (it == webSocketPromises_.end()) {
      throw std::runtime_error("unknown KJ websocket promise id: " + std::to_string(promiseId));
    }
    auto pending = kj::mv(it->second);
    webSocketPromises_.erase(it);
    auto runtimeWebSocket = kj::mv(pending.promise).wait(waitScope);
    return addWebSocket(kj::mv(runtimeWebSocket.socket), kj::mv(runtimeWebSocket.owner),
                        kj::mv(runtimeWebSocket.requestCompletion));
  }

  void cancelWebSocketPromise(uint32_t promiseId) {
    auto it = webSocketPromises_.find(promiseId);
    if (it == webSocketPromises_.end()) {
      throw std::runtime_error("unknown KJ websocket promise id: " + std::to_string(promiseId));
    }
    it->second.canceler->cancel("Capnp.KjAsync websocket promise canceled from Lean");
  }

  void releaseWebSocketPromise(uint32_t promiseId) {
    auto it = webSocketPromises_.find(promiseId);
    if (it == webSocketPromises_.end()) {
      throw std::runtime_error("unknown KJ websocket promise id: " + std::to_string(promiseId));
    }
    webSocketPromises_.erase(it);
  }

  WebSocketMessageResult awaitWebSocketMessagePromise(kj::WaitScope& waitScope,
                                                      uint32_t promiseId) {
    auto it = webSocketMessagePromises_.find(promiseId);
    if (it == webSocketMessagePromises_.end()) {
      throw std::runtime_error("unknown KJ websocket message promise id: " +
                               std::to_string(promiseId));
    }
    auto pending = kj::mv(it->second);
    webSocketMessagePromises_.erase(it);
    return kj::mv(pending.promise).wait(waitScope);
  }

  void cancelWebSocketMessagePromise(uint32_t promiseId) {
    auto it = webSocketMessagePromises_.find(promiseId);
    if (it == webSocketMessagePromises_.end()) {
      throw std::runtime_error("unknown KJ websocket message promise id: " +
                               std::to_string(promiseId));
    }
    it->second.canceler->cancel("Capnp.KjAsync websocket message promise canceled from Lean");
  }

  void releaseWebSocketMessagePromise(uint32_t promiseId) {
    auto it = webSocketMessagePromises_.find(promiseId);
    if (it == webSocketMessagePromises_.end()) {
      throw std::runtime_error("unknown KJ websocket message promise id: " +
                               std::to_string(promiseId));
    }
    webSocketMessagePromises_.erase(it);
  }

  uint32_t listen(kj::AsyncIoProvider& ioProvider, kj::WaitScope& waitScope,
                  const std::string& address, uint32_t portHint) {
    auto addr = ioProvider.getNetwork().parseAddress(address.c_str(), portHint).wait(waitScope);
    return addListener(addr->listen());
  }

  void releaseListener(uint32_t listenerId) {
    auto it = listeners_.find(listenerId);
    if (it == listeners_.end()) {
      throw std::runtime_error("unknown KJ listener id: " + std::to_string(listenerId));
    }
    listeners_.erase(it);
  }

  uint32_t accept(kj::WaitScope& waitScope, uint32_t listenerId) {
    auto it = listeners_.find(listenerId);
    if (it == listeners_.end()) {
      throw std::runtime_error("unknown KJ listener id: " + std::to_string(listenerId));
    }
    auto stream = it->second->accept().wait(waitScope).downcast<kj::AsyncIoStream>();
    return addConnection(kj::mv(stream));
  }

  uint32_t acceptStart(uint32_t listenerId) {
    auto it = listeners_.find(listenerId);
    if (it == listeners_.end()) {
      throw std::runtime_error("unknown KJ listener id: " + std::to_string(listenerId));
    }

    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(it->second->accept());
    return addConnectionPromise(PendingConnectionPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t connect(kj::AsyncIoProvider& ioProvider, kj::WaitScope& waitScope,
                   const std::string& address, uint32_t portHint) {
    auto addr = ioProvider.getNetwork().parseAddress(address.c_str(), portHint).wait(waitScope);
    auto stream = addr->connect().wait(waitScope).downcast<kj::AsyncIoStream>();
    return addConnection(kj::mv(stream));
  }

  uint32_t connectStart(kj::AsyncIoProvider& ioProvider, const std::string& address,
                        uint32_t portHint) {
    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(
        ioProvider.getNetwork()
            .parseAddress(address.c_str(), portHint)
            .then([](kj::Own<kj::NetworkAddress>&& addr) { return addr->connect(); }));
    return addConnectionPromise(PendingConnectionPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t parseAddress(kj::AsyncIoProvider& ioProvider, kj::WaitScope& waitScope,
                        const std::string& address, uint32_t portHint) {
    auto parsed = ioProvider.getNetwork().parseAddress(address.c_str(), portHint).wait(waitScope);
    return addNetworkAddress(kj::mv(parsed));
  }

  void releaseNetworkAddress(uint32_t addressId) {
    auto it = networkAddresses_.find(addressId);
    if (it == networkAddresses_.end()) {
      throw std::runtime_error("unknown KJ network address id: " + std::to_string(addressId));
    }
    networkAddresses_.erase(it);
  }

  kj::Maybe<std::string> networkAddressToString(uint32_t addressId) {
    auto it = networkAddresses_.find(addressId);
    if (it == networkAddresses_.end()) {
      throw std::runtime_error("unknown KJ network address id: " + std::to_string(addressId));
    }
    auto value = it->second->toString();
    return std::string(value.cStr());
  }

  uint32_t networkAddressClone(uint32_t addressId) {
    auto it = networkAddresses_.find(addressId);
    if (it == networkAddresses_.end()) {
      throw std::runtime_error("unknown KJ network address id: " + std::to_string(addressId));
    }
    return addNetworkAddress(it->second->clone());
  }

  uint32_t networkAddressConnect(kj::WaitScope& waitScope, uint32_t addressId) {
    auto it = networkAddresses_.find(addressId);
    if (it == networkAddresses_.end()) {
      throw std::runtime_error("unknown KJ network address id: " + std::to_string(addressId));
    }
    auto stream = it->second->connect().wait(waitScope).downcast<kj::AsyncIoStream>();
    return addConnection(kj::mv(stream));
  }

  uint32_t networkAddressConnectStart(uint32_t addressId) {
    auto it = networkAddresses_.find(addressId);
    if (it == networkAddresses_.end()) {
      throw std::runtime_error("unknown KJ network address id: " + std::to_string(addressId));
    }
    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(it->second->clone()->connect());
    return addConnectionPromise(PendingConnectionPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t networkAddressListen(uint32_t addressId) {
    auto it = networkAddresses_.find(addressId);
    if (it == networkAddresses_.end()) {
      throw std::runtime_error("unknown KJ network address id: " + std::to_string(addressId));
    }
    return addListener(it->second->listen());
  }

  uint32_t networkAddressBindDatagramPort(uint32_t addressId) {
    auto it = networkAddresses_.find(addressId);
    if (it == networkAddresses_.end()) {
      throw std::runtime_error("unknown KJ network address id: " + std::to_string(addressId));
    }
    return addDatagramPort(it->second->bindDatagramPort());
  }

  void releaseConnection(uint32_t connectionId) {
    auto it = connections_.find(connectionId);
    if (it == connections_.end()) {
      throw std::runtime_error("unknown KJ connection id: " + std::to_string(connectionId));
    }
    connections_.erase(it);
  }

  uint32_t connectionWriteStart(uint32_t connectionId,
                                std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto it = connections_.find(connectionId);
    if (it == connections_.end()) {
      throw std::runtime_error("unknown KJ connection id: " + std::to_string(connectionId));
    }

    auto canceler = kj::heap<kj::Canceler>();
    if (!bytes || bytes->empty()) {
      kj::Promise<void> ready = kj::READY_NOW;
      auto wrapped = canceler->wrap(kj::mv(ready));
      return addPromise(PendingPromise(kj::mv(wrapped), kj::mv(canceler)));
    }
    auto ptr = kj::ArrayPtr<const kj::byte>(
        reinterpret_cast<const kj::byte*>(bytes->data()), bytes->size());
    auto promise = canceler->wrap(it->second->write(ptr).attach(kj::mv(bytes)));
    return addPromise(PendingPromise(kj::mv(promise), kj::mv(canceler)));
  }

  void connectionWrite(kj::WaitScope& waitScope, uint32_t connectionId,
                       std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto promiseId = connectionWriteStart(connectionId, kj::mv(bytes));
    awaitPromise(waitScope, promiseId);
  }

  uint32_t connectionReadStart(uint32_t connectionId, uint32_t minBytes, uint32_t maxBytes) {
    if (minBytes > maxBytes) {
      throw std::runtime_error("connection read requires minBytes <= maxBytes");
    }

    auto it = connections_.find(connectionId);
    if (it == connections_.end()) {
      throw std::runtime_error("unknown KJ connection id: " + std::to_string(connectionId));
    }

    auto canceler = kj::heap<kj::Canceler>();
    if (maxBytes == 0) {
      kj::Promise<void> ready = kj::READY_NOW;
      auto wrapped = canceler->wrap(kj::mv(ready).then([]() {
        return std::make_shared<std::vector<uint8_t>>();
      }));
      return addBytesPromise(PendingBytesPromise(kj::mv(wrapped), kj::mv(canceler)));
    }

    auto buffer = kj::heapArray<kj::byte>(maxBytes);
    auto promise =
        canceler->wrap(it->second
                           ->tryRead(buffer.begin(), static_cast<size_t>(minBytes),
                                     static_cast<size_t>(maxBytes))
                           .then([buffer = kj::mv(buffer)](size_t readCount) mutable {
                             std::vector<uint8_t> bytes(readCount);
                             if (readCount != 0) {
                               std::memcpy(bytes.data(), buffer.begin(), readCount);
                             }
                             return makeSharedBytes(std::move(bytes));
                           }));
    return addBytesPromise(PendingBytesPromise(kj::mv(promise), kj::mv(canceler)));
  }

  std::shared_ptr<std::vector<uint8_t>> connectionRead(kj::WaitScope& waitScope,
                                                       uint32_t connectionId, uint32_t minBytes,
                                                       uint32_t maxBytes) {
    auto promiseId = connectionReadStart(connectionId, minBytes, maxBytes);
    return awaitBytesPromise(waitScope, promiseId);
  }

  uint32_t connectionShutdownWriteStart(uint32_t connectionId) {
    auto it = connections_.find(connectionId);
    if (it == connections_.end()) {
      throw std::runtime_error("unknown KJ connection id: " + std::to_string(connectionId));
    }
    it->second->shutdownWrite();

    auto canceler = kj::heap<kj::Canceler>();
    kj::Promise<void> ready = kj::READY_NOW;
    auto wrapped = canceler->wrap(kj::mv(ready));
    return addPromise(PendingPromise(kj::mv(wrapped), kj::mv(canceler)));
  }

  void connectionShutdownWrite(uint32_t connectionId) {
    auto it = connections_.find(connectionId);
    if (it == connections_.end()) {
      throw std::runtime_error("unknown KJ connection id: " + std::to_string(connectionId));
    }
    it->second->shutdownWrite();
  }

  uint32_t connectionWhenWriteDisconnectedStart(uint32_t connectionId) {
    auto it = connections_.find(connectionId);
    if (it == connections_.end()) {
      throw std::runtime_error("unknown KJ connection id: " + std::to_string(connectionId));
    }
    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(it->second->whenWriteDisconnected());
    return addPromise(PendingPromise(kj::mv(promise), kj::mv(canceler)));
  }

  void connectionAbortRead(uint32_t connectionId) {
    auto it = connections_.find(connectionId);
    if (it == connections_.end()) {
      throw std::runtime_error("unknown KJ connection id: " + std::to_string(connectionId));
    }
    it->second->abortRead();
  }

  void connectionAbortWrite(uint32_t connectionId, std::string reason) {
    auto it = connections_.find(connectionId);
    if (it == connections_.end()) {
      throw std::runtime_error("unknown KJ connection id: " + std::to_string(connectionId));
    }
    it->second->abortWrite(kj::Exception(
        kj::Exception::Type::FAILED, __FILE__, __LINE__, kj::str(reason.c_str())));
  }

  kj::Maybe<uint32_t> connectionDupFd(uint32_t connectionId) {
#if defined(_WIN32)
    (void)connectionId;
    return kj::none;
#else
    auto it = connections_.find(connectionId);
    if (it == connections_.end()) {
      throw std::runtime_error("unknown KJ connection id: " + std::to_string(connectionId));
    }

    KJ_IF_SOME(stream, kj::dynamicDowncastIfAvailable<kj::AsyncCapabilityStream>(*it->second)) {
      auto fdMaybe = stream.getFd();
      if (fdMaybe == kj::none) {
        return kj::none;
      }
      int fdCopy = dup(KJ_ASSERT_NONNULL(fdMaybe));
      if (fdCopy < 0) {
        throw std::runtime_error("dup() failed while duplicating KJ connection fd");
      }
      return static_cast<uint32_t>(fdCopy);
    }

    return kj::none;
#endif
  }

  int ensurePlatformFdAndDup(uint32_t fd, const char* opName) {
#if defined(_WIN32)
    (void)fd;
    (void)opName;
    throw std::runtime_error("fd wrapping is not supported on Windows");
#else
    auto maxFd = static_cast<uint32_t>(std::numeric_limits<int>::max());
    if (fd > maxFd) {
      throw std::runtime_error(std::string("fd exceeds supported range for ") + opName);
    }
    int duplicatedFd = dup(static_cast<int>(fd));
    if (duplicatedFd < 0) {
      throw std::runtime_error(std::string("dup() failed while running ") + opName);
    }
    return duplicatedFd;
#endif
  }

  uint32_t wrapSocketFdTake(kj::LowLevelAsyncIoProvider& lowLevelProvider, uint32_t fd) {
#if defined(_WIN32)
    (void)lowLevelProvider;
    (void)fd;
    throw std::runtime_error("wrapSocketFdTake is not supported on Windows");
#else
    auto maxFd = static_cast<uint32_t>(std::numeric_limits<int>::max());
    if (fd > maxFd) {
      throw std::runtime_error("fd exceeds supported range for wrapSocketFdTake");
    }
    auto stream = lowLevelProvider.wrapSocketFd(
        static_cast<int>(fd), kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
    return addConnection(kj::mv(stream));
#endif
  }

  uint32_t wrapSocketFd(kj::LowLevelAsyncIoProvider& lowLevelProvider, uint32_t fd) {
#if defined(_WIN32)
    (void)lowLevelProvider;
    (void)fd;
    throw std::runtime_error("wrapSocketFd is not supported on Windows");
#else
    int duplicatedFd = ensurePlatformFdAndDup(fd, "wrapSocketFd");
    auto stream = lowLevelProvider.wrapSocketFd(
        duplicatedFd, kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
    return addConnection(kj::mv(stream));
#endif
  }

  uint32_t wrapListenSocketFdTake(kj::LowLevelAsyncIoProvider& lowLevelProvider, uint32_t fd) {
#if defined(_WIN32)
    (void)lowLevelProvider;
    (void)fd;
    throw std::runtime_error("wrapListenSocketFdTake is not supported on Windows");
#else
    auto maxFd = static_cast<uint32_t>(std::numeric_limits<int>::max());
    if (fd > maxFd) {
      throw std::runtime_error("fd exceeds supported range for wrapListenSocketFdTake");
    }
    auto listener = lowLevelProvider.wrapListenSocketFd(
        static_cast<int>(fd), kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
    return addListener(kj::mv(listener));
#endif
  }

  uint32_t wrapListenSocketFd(kj::LowLevelAsyncIoProvider& lowLevelProvider, uint32_t fd) {
#if defined(_WIN32)
    (void)lowLevelProvider;
    (void)fd;
    throw std::runtime_error("wrapListenSocketFd is not supported on Windows");
#else
    int duplicatedFd = ensurePlatformFdAndDup(fd, "wrapListenSocketFd");
    auto listener = lowLevelProvider.wrapListenSocketFd(
        duplicatedFd, kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
    return addListener(kj::mv(listener));
#endif
  }

  uint32_t wrapDatagramSocketFdTake(kj::LowLevelAsyncIoProvider& lowLevelProvider, uint32_t fd) {
#if defined(_WIN32)
    (void)lowLevelProvider;
    (void)fd;
    throw std::runtime_error("wrapDatagramSocketFdTake is not supported on Windows");
#else
    auto maxFd = static_cast<uint32_t>(std::numeric_limits<int>::max());
    if (fd > maxFd) {
      throw std::runtime_error("fd exceeds supported range for wrapDatagramSocketFdTake");
    }
    auto port = lowLevelProvider.wrapDatagramSocketFd(
        static_cast<int>(fd), kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
    return addDatagramPort(kj::mv(port));
#endif
  }

  uint32_t wrapDatagramSocketFd(kj::LowLevelAsyncIoProvider& lowLevelProvider, uint32_t fd) {
#if defined(_WIN32)
    (void)lowLevelProvider;
    (void)fd;
    throw std::runtime_error("wrapDatagramSocketFd is not supported on Windows");
#else
    int duplicatedFd = ensurePlatformFdAndDup(fd, "wrapDatagramSocketFd");
    auto port = lowLevelProvider.wrapDatagramSocketFd(
        duplicatedFd, kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
    return addDatagramPort(kj::mv(port));
#endif
  }

  std::pair<uint32_t, uint32_t> newTwoWayPipe(kj::AsyncIoProvider& ioProvider) {
    auto pipe = ioProvider.newTwoWayPipe();
    auto first = addConnection(kj::mv(pipe.ends[0]));
    auto second = addConnection(kj::mv(pipe.ends[1]));
    return {first, second};
  }

  std::pair<uint32_t, uint32_t> newCapabilityPipe(kj::AsyncIoProvider& ioProvider) {
    auto pipe = ioProvider.newCapabilityPipe();
    auto first = addConnection(kj::mv(pipe.ends[0]));
    auto second = addConnection(kj::mv(pipe.ends[1]));
    return {first, second};
  }

  uint32_t datagramBind(kj::AsyncIoProvider& ioProvider, kj::WaitScope& waitScope,
                        const std::string& address, uint32_t portHint) {
    auto addr = ioProvider.getNetwork().parseAddress(address.c_str(), portHint).wait(waitScope);
    auto port = addr->bindDatagramPort();
    return addDatagramPort(kj::mv(port));
  }

  void datagramReleasePort(uint32_t portId) {
    auto erased = datagramPorts_.erase(portId);
    if (erased == 0) {
      throw std::runtime_error("unknown KJ datagram port id: " + std::to_string(portId));
    }
  }

  uint32_t datagramGetPort(uint32_t portId) {
    auto it = datagramPorts_.find(portId);
    if (it == datagramPorts_.end()) {
      throw std::runtime_error("unknown KJ datagram port id: " + std::to_string(portId));
    }
    return it->second->getPort();
  }

  uint32_t datagramSendStart(kj::AsyncIoProvider& ioProvider, uint32_t portId,
                             const std::string& address, uint32_t portHint,
                             std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto it = datagramPorts_.find(portId);
    if (it == datagramPorts_.end()) {
      throw std::runtime_error("unknown KJ datagram port id: " + std::to_string(portId));
    }

    auto canceler = kj::heap<kj::Canceler>();
    if (!bytes) {
      bytes = std::make_shared<std::vector<uint8_t>>();
    }
    auto promise = canceler->wrap(
        ioProvider.getNetwork()
            .parseAddress(address.c_str(), portHint)
            .then([this, portId, bytes = std::move(bytes)](
                      kj::Own<kj::NetworkAddress>&& addr) mutable {
              auto portIt = datagramPorts_.find(portId);
              if (portIt == datagramPorts_.end()) {
                throw std::runtime_error("unknown KJ datagram port id: " + std::to_string(portId));
              }
              auto ptr = kj::ArrayPtr<const kj::byte>(
                  reinterpret_cast<const kj::byte*>(bytes->data()), bytes->size());
              return portIt->second->send(ptr, *addr)
                  .attach(kj::mv(bytes))
                  .then([](size_t sent) {
                    if (sent > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
                      throw std::runtime_error("datagram send byte count exceeds UInt32 range");
                    }
                    return static_cast<uint32_t>(sent);
                  });
            }));
    return addUInt32Promise(PendingUInt32Promise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t datagramSend(kj::AsyncIoProvider& ioProvider, kj::WaitScope& waitScope, uint32_t portId,
                        const std::string& address, uint32_t portHint,
                        std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto promiseId = datagramSendStart(ioProvider, portId, address, portHint, kj::mv(bytes));
    return awaitUInt32Promise(waitScope, promiseId);
  }

  uint32_t datagramReceiveStart(uint32_t portId, uint32_t maxBytes) {
    if (maxBytes == 0) {
      throw std::runtime_error("datagram receive requires maxBytes > 0");
    }
    auto it = datagramPorts_.find(portId);
    if (it == datagramPorts_.end()) {
      throw std::runtime_error("unknown KJ datagram port id: " + std::to_string(portId));
    }

    kj::DatagramReceiver::Capacity capacity;
    capacity.content = maxBytes;
    capacity.ancillary = 0;
    auto receiver = it->second->makeReceiver(capacity);
    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(receiver->receive().then([receiver = kj::mv(receiver)]() mutable {
      DatagramReceiveResult result;
      auto content = receiver->getContent();
      auto contentPtr = content.value;
      result.bytes.resize(contentPtr.size());
      if (!result.bytes.empty()) {
        std::memcpy(result.bytes.data(), contentPtr.begin(), result.bytes.size());
      }
      auto source = receiver->getSource().toString();
      result.sourceAddress = source.cStr();
      return result;
    }));
    return addDatagramReceivePromise(
        PendingDatagramReceivePromise(kj::mv(promise), kj::mv(canceler)));
  }

  std::pair<std::string, std::vector<uint8_t>> datagramReceive(kj::WaitScope& waitScope,
                                                                uint32_t portId,
                                                                uint32_t maxBytes) {
    auto promiseId = datagramReceiveStart(portId, maxBytes);
    auto result = awaitDatagramReceivePromise(waitScope, promiseId);
    return {std::move(result.sourceAddress), std::move(result.bytes)};
  }

  static kj::HttpMethod decodeHttpMethod(uint32_t method) {
    switch (method) {
      case 0:
        return kj::HttpMethod::GET;
      case 1:
        return kj::HttpMethod::HEAD;
      case 2:
        return kj::HttpMethod::POST;
      case 3:
        return kj::HttpMethod::PUT;
      case 4:
        return kj::HttpMethod::DELETE;
      case 5:
        return kj::HttpMethod::PATCH;
      case 6:
        return kj::HttpMethod::PURGE;
      case 7:
        return kj::HttpMethod::OPTIONS;
      case 8:
        return kj::HttpMethod::TRACE;
      case 9:
        return kj::HttpMethod::COPY;
      case 10:
        return kj::HttpMethod::LOCK;
      case 11:
        return kj::HttpMethod::MKCOL;
      case 12:
        return kj::HttpMethod::MOVE;
      case 13:
        return kj::HttpMethod::PROPFIND;
      case 14:
        return kj::HttpMethod::PROPPATCH;
      case 15:
        return kj::HttpMethod::SEARCH;
      case 16:
        return kj::HttpMethod::UNLOCK;
      case 17:
        return kj::HttpMethod::ACL;
      case 18:
        return kj::HttpMethod::REPORT;
      case 19:
        return kj::HttpMethod::MKACTIVITY;
      case 20:
        return kj::HttpMethod::CHECKOUT;
      case 21:
        return kj::HttpMethod::MERGE;
      case 22:
        return kj::HttpMethod::MSEARCH;
      case 23:
        return kj::HttpMethod::NOTIFY;
      case 24:
        return kj::HttpMethod::SUBSCRIBE;
      case 25:
        return kj::HttpMethod::UNSUBSCRIBE;
      case 26:
        return kj::HttpMethod::QUERY;
      case 27:
        return kj::HttpMethod::BAN;
      default:
        throw std::runtime_error("unknown HTTP method tag: " + std::to_string(method));
    }
  }

  static uint32_t encodeHttpMethodTag(kj::HttpMethod method) {
    switch (method) {
      case kj::HttpMethod::GET:
        return 0;
      case kj::HttpMethod::HEAD:
        return 1;
      case kj::HttpMethod::POST:
        return 2;
      case kj::HttpMethod::PUT:
        return 3;
      case kj::HttpMethod::DELETE:
        return 4;
      case kj::HttpMethod::PATCH:
        return 5;
      case kj::HttpMethod::PURGE:
        return 6;
      case kj::HttpMethod::OPTIONS:
        return 7;
      case kj::HttpMethod::TRACE:
        return 8;
      case kj::HttpMethod::COPY:
        return 9;
      case kj::HttpMethod::LOCK:
        return 10;
      case kj::HttpMethod::MKCOL:
        return 11;
      case kj::HttpMethod::MOVE:
        return 12;
      case kj::HttpMethod::PROPFIND:
        return 13;
      case kj::HttpMethod::PROPPATCH:
        return 14;
      case kj::HttpMethod::SEARCH:
        return 15;
      case kj::HttpMethod::UNLOCK:
        return 16;
      case kj::HttpMethod::ACL:
        return 17;
      case kj::HttpMethod::REPORT:
        return 18;
      case kj::HttpMethod::MKACTIVITY:
        return 19;
      case kj::HttpMethod::CHECKOUT:
        return 20;
      case kj::HttpMethod::MERGE:
        return 21;
      case kj::HttpMethod::MSEARCH:
        return 22;
      case kj::HttpMethod::NOTIFY:
        return 23;
      case kj::HttpMethod::SUBSCRIBE:
        return 24;
      case kj::HttpMethod::UNSUBSCRIBE:
        return 25;
      case kj::HttpMethod::QUERY:
        return 26;
      case kj::HttpMethod::BAN:
        return 27;
      default:
        throw std::runtime_error("unsupported HTTP method in Capnp.KjAsync bridge");
    }
  }

  static kj::Maybe<kj::TlsVersion> decodeTlsVersionTag(uint32_t tag) {
    switch (tag) {
      case 0:
        return kj::none;
      case 1:
        return kj::TlsVersion::SSL_3;
      case 2:
        return kj::TlsVersion::TLS_1_0;
      case 3:
        return kj::TlsVersion::TLS_1_1;
      case 4:
        return kj::TlsVersion::TLS_1_2;
      case 5:
        return kj::TlsVersion::TLS_1_3;
      default:
        throw std::runtime_error("unknown TLS minVersion tag: " + std::to_string(tag));
    }
  }

  static kj::HttpServerSettings::WebSocketCompressionMode decodeWebSocketCompressionMode(
      uint32_t tag) {
    switch (tag) {
      case 0:
        return kj::HttpServerSettings::NO_COMPRESSION;
      case 1:
        return kj::HttpServerSettings::MANUAL_COMPRESSION;
      case 2:
        return kj::HttpServerSettings::AUTOMATIC_COMPRESSION;
      default:
        throw std::runtime_error("unknown websocket compression mode tag: " +
                                 std::to_string(tag));
    }
  }

  static kj::Duration nanosToDuration(uint64_t nanos, const char* what) {
    constexpr uint64_t maxI64 = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (nanos > maxI64) {
      throw std::runtime_error(std::string(what) + " exceeds Int64 nanoseconds range");
    }
    return static_cast<int64_t>(nanos) * kj::NANOSECONDS;
  }

  void configureTls(kj::AsyncIoProvider& ioProvider, bool useSystemTrustStore, bool verifyClients,
                    uint32_t minVersionTag,
                    const std::string& trustedCertificatesPem,
                    const std::string& certificateChainPem,
                    const std::string& privateKeyPem, const std::string& cipherList,
                    const std::string& curveList, uint64_t acceptTimeoutNanos) {
    if (certificateChainPem.empty() != privateKeyPem.empty()) {
      throw std::runtime_error(
          "TLS configuration requires both certificateChainPem and privateKeyPem to be set");
    }

    kj::TlsContext::Options options;
    options.useSystemTrustStore = useSystemTrustStore;
    options.verifyClients = verifyClients;
    KJ_IF_SOME(minVersion, decodeTlsVersionTag(minVersionTag)) {
      options.minVersion = minVersion;
    }

    kj::Maybe<kj::TlsCertificate> trustedCertificates = kj::none;
    if (!trustedCertificatesPem.empty()) {
      trustedCertificates = kj::TlsCertificate(kj::StringPtr(trustedCertificatesPem.c_str()));
    }
    KJ_IF_SOME(trustedCertificate, trustedCertificates) {
      options.trustedCertificates = kj::arrayPtr(trustedCertificate);
    }

    kj::Maybe<kj::TlsKeypair> keypair = kj::none;
    if (!certificateChainPem.empty()) {
      keypair = kj::TlsKeypair{kj::TlsPrivateKey(kj::StringPtr(privateKeyPem.c_str())),
                               kj::TlsCertificate(kj::StringPtr(certificateChainPem.c_str()))};
    }
    KJ_IF_SOME(keypairValue, keypair) {
      options.defaultKeypair = keypairValue;
    }

    kj::Maybe<kj::String> cipherListOwned = kj::none;
    if (!cipherList.empty()) {
      cipherListOwned = kj::str(cipherList.c_str());
    }
    KJ_IF_SOME(cipherListValue, cipherListOwned) {
      options.cipherList = cipherListValue;
    }

    kj::Maybe<kj::String> curveListOwned = kj::none;
    if (!curveList.empty()) {
      curveListOwned = kj::str(curveList.c_str());
    }
    KJ_IF_SOME(curveListValue, curveListOwned) {
      options.curveList = curveListValue;
    }

    if (acceptTimeoutNanos != 0) {
      options.timer = ioProvider.getTimer();
      options.acceptTimeout = nanosToDuration(acceptTimeoutNanos, "TLS accept timeout");
    }

    auto context = kj::heap<kj::TlsContext>(kj::mv(options));
    auto network = context->wrapNetwork(ioProvider.getNetwork());
    tlsContext_ = kj::mv(context);
    tlsNetwork_ = kj::mv(network);
  }

  void enableTls(kj::AsyncIoProvider& ioProvider) {
    KJ_IF_SOME(_, tlsContext_) {
      return;
    }
    configureTls(ioProvider, true, false, 0, "", "", "", "", "", 0);
  }

  kj::Maybe<kj::Network&> resolveTlsNetwork(bool useTls) {
    if (!useTls) {
      return kj::none;
    }
    KJ_IF_SOME(network, tlsNetwork_) {
      return *network;
    }
    throw std::runtime_error(
        "TLS is not enabled in this runtime; call Runtime.enableTls before HTTPS/WSS operations");
  }

  static kj::String buildUrl(kj::StringPtr scheme, const std::string& address, uint32_t portHint,
                             const std::string& path) {
    std::string host(address);
    if (host.find(':') != std::string::npos &&
        !(host.size() >= 2 && host.front() == '[' && host.back() == ']')) {
      host = "[" + host + "]";
    }

    std::string normalizedPath(path);
    if (normalizedPath.empty()) {
      normalizedPath = "/";
    } else if (normalizedPath.front() != '/') {
      normalizedPath.insert(normalizedPath.begin(), '/');
    }

    if (portHint == 0) {
      return kj::str(scheme, "://", host.c_str(), normalizedPath.c_str());
    }
    return kj::str(scheme, "://", host.c_str(), ":", portHint, normalizedPath.c_str());
  }

  static kj::Promise<HttpResponseResult> readHttpResponse(kj::HttpClient::Response&& response,
                                                          uint64_t bodyLimit) {
    if (response.statusCode > std::numeric_limits<uint32_t>::max()) {
      throw std::runtime_error("HTTP status code exceeds UInt32 range");
    }
    const uint32_t statusCode = static_cast<uint32_t>(response.statusCode);
    std::string statusText(response.statusText.cStr());
    std::vector<uint8_t> headerBytes;
    if (response.headers != nullptr) {
      headerBytes = encodeHeaderPairs(captureHeaders(*response.headers));
    }
    auto bodyReader = kj::mv(response.body);
    return bodyReader->readAllBytes(bodyLimit)
        .then(
        [statusCode, statusText = std::move(statusText),
         headerBytes = std::move(headerBytes)](kj::Array<kj::byte>&& responseBody) mutable {
          HttpResponseResult result;
          result.statusCode = statusCode;
          result.statusText = std::move(statusText);
          result.headers = std::move(headerBytes);
          result.body.resize(responseBody.size());
          if (!result.body.empty()) {
            std::memcpy(result.body.data(), responseBody.begin(), result.body.size());
          }
          return result;
        })
        .attach(kj::mv(bodyReader));
  }

  uint32_t httpRequestStart(kj::AsyncIoProvider& ioProvider, uint32_t method,
                            const std::string& address, uint32_t portHint,
                            const std::string& path,
                            std::vector<std::pair<std::string, std::string>> requestHeaders,
                            std::shared_ptr<std::vector<uint8_t>> body, bool useTls,
                            uint64_t responseBodyLimit) {
    const auto decodedMethod = decodeHttpMethod(method);
    auto headerTable = kj::heap<kj::HttpHeaderTable>();
    auto tlsNetwork = resolveTlsNetwork(useTls);
    auto client = kj::newHttpClient(ioProvider.getTimer(), *headerTable, ioProvider.getNetwork(),
                                    tlsNetwork);
    auto requestUrl = buildUrl(useTls ? "https" : "http", address, portHint, path);
    auto headers = kj::heap<kj::HttpHeaders>(*headerTable);
    applyHeadersFromPairs(*headers, requestHeaders);
    const size_t bodySize = body ? body->size() : 0;
    auto request = client->request(
        decodedMethod, requestUrl, *headers,
        bodySize == 0 ? kj::none : kj::Maybe<uint64_t>(static_cast<uint64_t>(bodySize)));

    auto responsePromise = kj::mv(request.response);
    kj::Promise<HttpResponseResult> promise = nullptr;
    if (request.body.get() == nullptr) {
      if (bodySize != 0) {
        throw std::runtime_error("HTTP method does not accept a request body");
      }
      promise = kj::mv(responsePromise).then(
          [responseBodyLimit](kj::HttpClient::Response&& response) {
            return readHttpResponse(kj::mv(response), responseBodyLimit);
          });
    } else {
      kj::Promise<void> writePromise = kj::READY_NOW;
      if (bodySize != 0) {
        auto bodyPtr = kj::ArrayPtr<const kj::byte>(
            reinterpret_cast<const kj::byte*>(body->data()), body->size());
        writePromise = request.body->write(bodyPtr).attach(kj::mv(body));
      }

      promise = kj::mv(writePromise).then(
          [requestBody = kj::mv(request.body), responsePromise = kj::mv(responsePromise),
           responseBodyLimit]() mutable -> kj::Promise<HttpResponseResult> {
            requestBody = nullptr;
            return kj::mv(responsePromise).then(
                [responseBodyLimit](kj::HttpClient::Response&& response) {
                  return readHttpResponse(kj::mv(response), responseBodyLimit);
                });
          });
    }

    promise = kj::mv(promise).attach(kj::mv(requestUrl), kj::mv(headers), kj::mv(client),
                                     kj::mv(headerTable));
    auto canceler = kj::heap<kj::Canceler>();
    promise = canceler->wrap(kj::mv(promise));
    return addHttpResponsePromise(PendingHttpResponsePromise(kj::mv(promise), kj::mv(canceler)));
  }

  HttpResponseResult decodeStreamingHttpResponse(kj::HttpClient::Response&& response,
                                                 std::shared_ptr<RuntimeHttpClientOwner> owner) {
    if (response.statusCode > std::numeric_limits<uint32_t>::max()) {
      throw std::runtime_error("HTTP status code exceeds UInt32 range");
    }
    HttpResponseResult result;
    result.statusCode = static_cast<uint32_t>(response.statusCode);
    result.statusText = response.statusText.cStr();
    if (response.headers != nullptr) {
      result.headers = encodeHeaderPairs(captureHeaders(*response.headers));
    }
    auto body = kj::mv(response.body);
    if (body.get() == nullptr) {
      throw std::runtime_error("HTTP response missing body stream");
    }
    result.bodyHandle = addHttpResponseBody(kj::mv(body), std::move(owner));
    return result;
  }

  std::pair<uint32_t, uint32_t> httpRequestStartStreaming(
      kj::AsyncIoProvider& ioProvider, uint32_t method, const std::string& address,
      uint32_t portHint, const std::string& path,
      std::vector<std::pair<std::string, std::string>> requestHeaders, bool useTls) {
    const auto decodedMethod = decodeHttpMethod(method);
    auto owner = std::make_shared<RuntimeHttpClientOwner>();
    owner->headerTable = kj::heap<kj::HttpHeaderTable>();
    auto tlsNetwork = resolveTlsNetwork(useTls);
    owner->client =
        kj::newHttpClient(ioProvider.getTimer(), *owner->headerTable, ioProvider.getNetwork(),
                          tlsNetwork);
    owner->requestUrl = buildUrl(useTls ? "https" : "http", address, portHint, path);
    owner->requestHeaders = kj::heap<kj::HttpHeaders>(*owner->headerTable);
    applyHeadersFromPairs(*owner->requestHeaders, requestHeaders);
    auto request = owner->client->request(decodedMethod, owner->requestUrl, *owner->requestHeaders,
                                          kj::none);

    uint32_t requestBodyId = 0;
    if (request.body.get() != nullptr) {
      requestBodyId = addHttpRequestBody(kj::mv(request.body), owner);
    }

    auto responsePromise = kj::mv(request.response).then(
        [this, owner = std::move(owner)](kj::HttpClient::Response&& response) mutable {
          return decodeStreamingHttpResponse(kj::mv(response), std::move(owner));
        });
    auto canceler = kj::heap<kj::Canceler>();
    auto wrapped = canceler->wrap(kj::mv(responsePromise));
    auto responsePromiseId =
        addHttpResponsePromise(PendingHttpResponsePromise(kj::mv(wrapped), kj::mv(canceler)));
    return {requestBodyId, responsePromiseId};
  }

  HttpResponseResult httpRequest(kj::AsyncIoProvider& ioProvider, kj::WaitScope& waitScope,
                                 uint32_t method, const std::string& address, uint32_t portHint,
                                 const std::string& path,
                                 std::vector<std::pair<std::string, std::string>> requestHeaders,
                                 std::shared_ptr<std::vector<uint8_t>> body, bool useTls,
                                 uint64_t responseBodyLimit) {
    auto promiseId = httpRequestStart(ioProvider, method, address, portHint, path,
                                      std::move(requestHeaders), kj::mv(body), useTls,
                                      responseBodyLimit);
    return awaitHttpResponsePromise(waitScope, promiseId);
  }

  uint32_t httpRequestBodyWriteStart(uint32_t requestBodyId,
                                     std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto it = httpRequestBodies_.find(requestBodyId);
    if (it == httpRequestBodies_.end()) {
      throw std::runtime_error("unknown KJ HTTP request body id: " +
                               std::to_string(requestBodyId));
    }
    auto canceler = kj::heap<kj::Canceler>();
    if (!bytes || bytes->empty()) {
      kj::Promise<void> ready = kj::READY_NOW;
      auto wrapped = canceler->wrap(kj::mv(ready));
      return addPromise(PendingPromise(kj::mv(wrapped), kj::mv(canceler)));
    }
    auto ptr = kj::ArrayPtr<const kj::byte>(
        reinterpret_cast<const kj::byte*>(bytes->data()), bytes->size());
    auto promise = canceler->wrap(it->second.stream->write(ptr).attach(kj::mv(bytes)));
    return addPromise(PendingPromise(kj::mv(promise), kj::mv(canceler)));
  }

  void httpRequestBodyWrite(kj::WaitScope& waitScope, uint32_t requestBodyId,
                            std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto promiseId = httpRequestBodyWriteStart(requestBodyId, kj::mv(bytes));
    awaitPromise(waitScope, promiseId);
  }

  uint32_t httpRequestBodyFinishStart(uint32_t requestBodyId) {
    auto it = httpRequestBodies_.find(requestBodyId);
    if (it == httpRequestBodies_.end()) {
      throw std::runtime_error("unknown KJ HTTP request body id: " +
                               std::to_string(requestBodyId));
    }
    auto body = std::move(it->second);
    httpRequestBodies_.erase(it);
    auto canceler = kj::heap<kj::Canceler>();
    kj::Promise<void> ready = kj::READY_NOW;
    auto wrapped = canceler->wrap(kj::mv(ready));
    (void)body;
    return addPromise(PendingPromise(kj::mv(wrapped), kj::mv(canceler)));
  }

  void httpRequestBodyFinish(kj::WaitScope& waitScope, uint32_t requestBodyId) {
    auto promiseId = httpRequestBodyFinishStart(requestBodyId);
    awaitPromise(waitScope, promiseId);
  }

  void httpRequestBodyRelease(uint32_t requestBodyId) {
    auto erased = httpRequestBodies_.erase(requestBodyId);
    if (erased == 0) {
      throw std::runtime_error("unknown KJ HTTP request body id: " +
                               std::to_string(requestBodyId));
    }
  }

  uint32_t httpResponseBodyReadStart(uint32_t responseBodyId, uint32_t minBytes,
                                     uint32_t maxBytes) {
    if (minBytes > maxBytes) {
      throw std::runtime_error("HTTP response body read requires minBytes <= maxBytes");
    }
    auto it = httpResponseBodies_.find(responseBodyId);
    if (it == httpResponseBodies_.end()) {
      throw std::runtime_error("unknown KJ HTTP response body id: " +
                               std::to_string(responseBodyId));
    }
    auto canceler = kj::heap<kj::Canceler>();
    if (maxBytes == 0) {
      kj::Promise<void> ready = kj::READY_NOW;
      auto wrapped = canceler->wrap(kj::mv(ready).then([]() {
        return std::make_shared<std::vector<uint8_t>>();
      }));
      return addBytesPromise(PendingBytesPromise(kj::mv(wrapped), kj::mv(canceler)));
    }

    auto buffer = kj::heapArray<kj::byte>(maxBytes);
    auto promise = canceler->wrap(it->second.stream
                                      ->tryRead(buffer.begin(), static_cast<size_t>(minBytes),
                                                static_cast<size_t>(maxBytes))
                                      .then([buffer = kj::mv(buffer)](size_t readCount) mutable {
                                        std::vector<uint8_t> bytes(readCount);
                                        if (readCount != 0) {
                                          std::memcpy(bytes.data(), buffer.begin(), readCount);
                                        }
                                        return makeSharedBytes(std::move(bytes));
                                      }));
    return addBytesPromise(PendingBytesPromise(kj::mv(promise), kj::mv(canceler)));
  }

  std::shared_ptr<std::vector<uint8_t>> httpResponseBodyRead(kj::WaitScope& waitScope,
                                                             uint32_t responseBodyId,
                                                             uint32_t minBytes,
                                                             uint32_t maxBytes) {
    auto promiseId = httpResponseBodyReadStart(responseBodyId, minBytes, maxBytes);
    return awaitBytesPromise(waitScope, promiseId);
  }

  void httpResponseBodyRelease(uint32_t responseBodyId) {
    auto erased = httpResponseBodies_.erase(responseBodyId);
    if (erased == 0) {
      throw std::runtime_error("unknown KJ HTTP response body id: " +
                               std::to_string(responseBodyId));
    }
  }

  uint32_t httpServerRequestBodyReadStart(uint32_t requestBodyId, uint32_t minBytes,
                                          uint32_t maxBytes) {
    if (minBytes > maxBytes) {
      throw std::runtime_error("HTTP server request body read requires minBytes <= maxBytes");
    }
    auto it = httpServerRequestBodies_.find(requestBodyId);
    if (it == httpServerRequestBodies_.end() || it->second.stream == nullptr) {
      throw std::runtime_error("unknown KJ HTTP server request body id: " +
                               std::to_string(requestBodyId));
    }
    auto canceler = kj::heap<kj::Canceler>();
    if (maxBytes == 0) {
      kj::Promise<void> ready = kj::READY_NOW;
      auto wrapped = canceler->wrap(kj::mv(ready).then([]() {
        return std::make_shared<std::vector<uint8_t>>();
      }));
      return addBytesPromise(PendingBytesPromise(kj::mv(wrapped), kj::mv(canceler)));
    }

    auto buffer = kj::heapArray<kj::byte>(maxBytes);
    auto promise = canceler->wrap(it->second.stream
                                      ->tryRead(buffer.begin(), static_cast<size_t>(minBytes),
                                                static_cast<size_t>(maxBytes))
                                      .then([buffer = kj::mv(buffer)](size_t readCount) mutable {
                                        std::vector<uint8_t> bytes(readCount);
                                        if (readCount != 0) {
                                          std::memcpy(bytes.data(), buffer.begin(), readCount);
                                        }
                                        return makeSharedBytes(std::move(bytes));
                                      }));
    return addBytesPromise(PendingBytesPromise(kj::mv(promise), kj::mv(canceler)));
  }

  std::shared_ptr<std::vector<uint8_t>> httpServerRequestBodyRead(kj::WaitScope& waitScope,
                                                                  uint32_t requestBodyId,
                                                                  uint32_t minBytes,
                                                                  uint32_t maxBytes) {
    auto promiseId = httpServerRequestBodyReadStart(requestBodyId, minBytes, maxBytes);
    return awaitBytesPromise(waitScope, promiseId);
  }

  void httpServerRequestBodyRelease(uint32_t requestBodyId) {
    auto erased = httpServerRequestBodies_.erase(requestBodyId);
    if (erased == 0) {
      throw std::runtime_error("unknown KJ HTTP server request body id: " +
                               std::to_string(requestBodyId));
    }
  }

  static std::vector<uint8_t> encodeHttpServerRequest(const HttpServerRequestRecord& request) {
    if (request.path.size() > std::numeric_limits<uint32_t>::max()) {
      throw std::runtime_error("HTTP server request path exceeds UInt32 size");
    }
    auto headerBytes = encodeHeaderPairs(request.headers);
    if (headerBytes.size() > std::numeric_limits<uint32_t>::max() ||
        request.body.size() > std::numeric_limits<uint32_t>::max()) {
      throw std::runtime_error("HTTP server request payload exceeds UInt32 size");
    }

    std::vector<uint8_t> out;
    out.reserve(28 + request.path.size() + headerBytes.size() + request.body.size());
    appendUint32Le(out, request.requestId);
    appendUint32Le(out, request.methodTag);
    appendUint32Le(out, request.webSocketRequested ? 1 : 0);
    appendUint32Le(out, static_cast<uint32_t>(request.path.size()));
    out.insert(out.end(), request.path.begin(), request.path.end());
    appendUint32Le(out, static_cast<uint32_t>(headerBytes.size()));
    out.insert(out.end(), headerBytes.begin(), headerBytes.end());
    appendUint32Le(out, static_cast<uint32_t>(request.body.size()));
    out.insert(out.end(), request.body.begin(), request.body.end());
    appendUint32Le(out, request.bodyHandle);
    return out;
  }

  uint32_t httpServerListen(kj::AsyncIoProvider& ioProvider, kj::WaitScope& waitScope,
                            const std::string& address, uint32_t portHint, bool useTls,
                            const HttpServerConfig& config, uint32_t& boundPort) {
    auto networkAddress =
        ioProvider.getNetwork().parseAddress(address.c_str(), portHint).wait(waitScope);
    auto listener = networkAddress->listen();
    if (useTls) {
      KJ_IF_SOME(context, tlsContext_) {
        listener = context->wrapPort(kj::mv(listener));
      } else {
        throw std::runtime_error(
            "TLS is not enabled in this runtime; call Runtime.enableTls or Runtime.configureTls before HTTPS/WSS server listen");
      }
    }
    boundPort = listener->getPort();

    auto state = kj::heap<HttpServerState>();
    state->listener = kj::mv(listener);
    state->headerTable = kj::heap<kj::HttpHeaderTable>();
    state->service = kj::heap<RuntimeHttpService>(*this, *state);
    kj::HttpServerSettings settings;
    settings.headerTimeout = nanosToDuration(config.headerTimeoutNanos, "HTTP header timeout");
    settings.pipelineTimeout = nanosToDuration(config.pipelineTimeoutNanos, "HTTP pipeline timeout");
    settings.canceledUploadGracePeriod = nanosToDuration(
        config.canceledUploadGracePeriodNanos, "HTTP canceled upload grace period");
    if (config.canceledUploadGraceBytes >
        static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      throw std::runtime_error("HTTP canceled upload grace bytes exceeds size_t range");
    }
    settings.canceledUploadGraceBytes = static_cast<size_t>(config.canceledUploadGraceBytes);
    settings.webSocketCompressionMode =
        decodeWebSocketCompressionMode(config.webSocketCompressionMode);
    state->server = kj::heap<kj::HttpServer>(ioProvider.getTimer(), *state->headerTable,
                                             *state->service, settings);
    auto serverId = addHttpServer(kj::mv(state));

    auto stateIt = httpServers_.find(serverId);
    KJ_REQUIRE(stateIt != httpServers_.end());
    stateIt->second->serverId = serverId;
    auto canceler = kj::heap<kj::Canceler>();
    auto listenPromise = canceler->wrap(stateIt->second->server->listenHttp(*stateIt->second->listener));
    stateIt->second->listenPromiseId =
        addPromise(PendingPromise(kj::mv(listenPromise), kj::mv(canceler)));
    return serverId;
  }

  uint32_t httpServerDrainStart(uint32_t serverId) {
    auto serverIt = httpServers_.find(serverId);
    if (serverIt == httpServers_.end()) {
      throw std::runtime_error("unknown KJ HTTP server id: " + std::to_string(serverId));
    }
    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(serverIt->second->server->drain());
    return addPromise(PendingPromise(kj::mv(promise), kj::mv(canceler)));
  }

  void httpServerDrain(kj::WaitScope& waitScope, uint32_t serverId) {
    auto promiseId = httpServerDrainStart(serverId);
    awaitPromise(waitScope, promiseId);
  }

  void httpServerRelease(uint32_t serverId) {
    auto serverIt = httpServers_.find(serverId);
    if (serverIt == httpServers_.end()) {
      throw std::runtime_error("unknown KJ HTTP server id: " + std::to_string(serverId));
    }

    auto& state = *serverIt->second;
    if (state.listenPromiseId != 0) {
      auto promiseIt = promises_.find(state.listenPromiseId);
      if (promiseIt != promises_.end()) {
        promiseIt->second.canceler->cancel("Capnp.KjAsync HTTP server released");
        promises_.erase(promiseIt);
        retiredPromiseIds_.insert(state.listenPromiseId);
      }
      state.listenPromiseId = 0;
    }

    while (!state.requestQueue.empty()) {
      state.requestQueue.pop_front();
    }

    for (auto& pending : state.pendingResponses) {
      pending.second->reject(kj::Exception(kj::Exception::Type::DISCONNECTED, __FILE__, __LINE__,
                                           kj::str("Capnp.KjAsync HTTP server released")));
    }
    state.pendingResponses.clear();
    releaseHttpServerBodiesForServer(serverId);
    httpServers_.erase(serverIt);
  }

  HttpServerPollResult httpServerPollRequest(uint32_t serverId) {
    auto serverIt = httpServers_.find(serverId);
    if (serverIt == httpServers_.end()) {
      throw std::runtime_error("unknown KJ HTTP server id: " + std::to_string(serverId));
    }

    auto& queue = serverIt->second->requestQueue;
    HttpServerPollResult out;
    if (queue.empty()) {
      out.hasRequest = false;
      return out;
    }

    auto request = std::move(queue.front());
    queue.pop_front();
    out.hasRequest = true;
    out.encodedRequest = encodeHttpServerRequest(request);
    return out;
  }

  void httpServerRespond(uint32_t serverId, uint32_t requestId, uint32_t statusCode,
                         std::string statusText,
                         std::vector<std::pair<std::string, std::string>> responseHeaders,
                         std::shared_ptr<std::vector<uint8_t>> body) {
    auto serverIt = httpServers_.find(serverId);
    if (serverIt == httpServers_.end()) {
      throw std::runtime_error("unknown KJ HTTP server id: " + std::to_string(serverId));
    }
    auto responseIt = serverIt->second->pendingResponses.find(requestId);
    if (responseIt == serverIt->second->pendingResponses.end()) {
      throw std::runtime_error("unknown KJ HTTP server request id: " + std::to_string(requestId));
    }

    HttpServerResponseCommand command;
    command.kind = HttpServerResponseCommand::Kind::HTTP;
    command.statusCode = statusCode;
    command.statusText = std::move(statusText);
    command.headers = std::move(responseHeaders);
    command.body = kj::mv(body);

    auto fulfiller = kj::mv(responseIt->second);
    serverIt->second->pendingResponses.erase(responseIt);
    fulfiller->fulfill(kj::mv(command));
  }

  void httpServerRespondWebSocket(uint32_t serverId, uint32_t requestId,
                                  std::vector<std::pair<std::string, std::string>> responseHeaders,
                                  const std::shared_ptr<HandleCompletion>& completion) {
    auto serverIt = httpServers_.find(serverId);
    if (serverIt == httpServers_.end()) {
      throw std::runtime_error("unknown KJ HTTP server id: " + std::to_string(serverId));
    }
    auto responseIt = serverIt->second->pendingResponses.find(requestId);
    if (responseIt == serverIt->second->pendingResponses.end()) {
      throw std::runtime_error("unknown KJ HTTP server request id: " + std::to_string(requestId));
    }

    HttpServerResponseCommand command;
    command.kind = HttpServerResponseCommand::Kind::WEBSOCKET;
    command.headers = std::move(responseHeaders);
    command.webSocketCompletion = completion;

    auto fulfiller = kj::mv(responseIt->second);
    serverIt->second->pendingResponses.erase(responseIt);
    fulfiller->fulfill(kj::mv(command));
  }

  void httpServerRespondStartStreaming(
      uint32_t serverId, uint32_t requestId, uint32_t statusCode, std::string statusText,
      std::vector<std::pair<std::string, std::string>> responseHeaders,
      const std::shared_ptr<HandleCompletion>& completion) {
    auto serverIt = httpServers_.find(serverId);
    if (serverIt == httpServers_.end()) {
      throw std::runtime_error("unknown KJ HTTP server id: " + std::to_string(serverId));
    }
    auto responseIt = serverIt->second->pendingResponses.find(requestId);
    if (responseIt == serverIt->second->pendingResponses.end()) {
      throw std::runtime_error("unknown KJ HTTP server request id: " + std::to_string(requestId));
    }

    HttpServerResponseCommand command;
    command.kind = HttpServerResponseCommand::Kind::HTTP_STREAMING;
    command.statusCode = statusCode;
    command.statusText = std::move(statusText);
    command.headers = std::move(responseHeaders);
    command.streamingCompletion = completion;

    auto fulfiller = kj::mv(responseIt->second);
    serverIt->second->pendingResponses.erase(responseIt);
    fulfiller->fulfill(kj::mv(command));
  }

  uint32_t httpServerResponseBodyWriteStart(
      uint32_t responseBodyId, std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto it = httpServerResponseBodies_.find(responseBodyId);
    if (it == httpServerResponseBodies_.end()) {
      throw std::runtime_error("unknown KJ HTTP server response body id: " +
                               std::to_string(responseBodyId));
    }
    auto canceler = kj::heap<kj::Canceler>();
    if (!bytes || bytes->empty()) {
      kj::Promise<void> ready = kj::READY_NOW;
      auto wrapped = canceler->wrap(kj::mv(ready));
      return addPromise(PendingPromise(kj::mv(wrapped), kj::mv(canceler)));
    }
    auto ptr = kj::ArrayPtr<const kj::byte>(
        reinterpret_cast<const kj::byte*>(bytes->data()), bytes->size());
    auto promise = canceler->wrap(it->second.stream->write(ptr).attach(kj::mv(bytes)));
    return addPromise(PendingPromise(kj::mv(promise), kj::mv(canceler)));
  }

  void httpServerResponseBodyWrite(kj::WaitScope& waitScope, uint32_t responseBodyId,
                                   std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto promiseId = httpServerResponseBodyWriteStart(responseBodyId, kj::mv(bytes));
    awaitPromise(waitScope, promiseId);
  }

  uint32_t httpServerResponseBodyFinishStart(uint32_t responseBodyId) {
    auto it = httpServerResponseBodies_.find(responseBodyId);
    if (it == httpServerResponseBodies_.end()) {
      throw std::runtime_error("unknown KJ HTTP server response body id: " +
                               std::to_string(responseBodyId));
    }
    auto body = kj::mv(it->second);
    httpServerResponseBodies_.erase(it);

    body.stream = nullptr;
    body.doneFulfiller->fulfill();

    auto canceler = kj::heap<kj::Canceler>();
    kj::Promise<void> ready = kj::READY_NOW;
    auto wrapped = canceler->wrap(kj::mv(ready));
    return addPromise(PendingPromise(kj::mv(wrapped), kj::mv(canceler)));
  }

  void httpServerResponseBodyFinish(kj::WaitScope& waitScope, uint32_t responseBodyId) {
    auto promiseId = httpServerResponseBodyFinishStart(responseBodyId);
    awaitPromise(waitScope, promiseId);
  }

  void httpServerResponseBodyRelease(uint32_t responseBodyId) {
    auto it = httpServerResponseBodies_.find(responseBodyId);
    if (it == httpServerResponseBodies_.end()) {
      throw std::runtime_error("unknown KJ HTTP server response body id: " +
                               std::to_string(responseBodyId));
    }
    auto body = kj::mv(it->second);
    httpServerResponseBodies_.erase(it);
    body.stream = nullptr;
    body.doneFulfiller->fulfill();
  }

  static WebSocketMessageResult decodeWebSocketMessage(kj::WebSocket::Message&& message) {
    WebSocketMessageResult result;
    KJ_SWITCH_ONEOF(message) {
      KJ_CASE_ONEOF(text, kj::String) {
        result.tag = 0;
        result.text = text.cStr();
      }
      KJ_CASE_ONEOF(binary, kj::Array<kj::byte>) {
        result.tag = 1;
        result.bytes.resize(binary.size());
        if (!result.bytes.empty()) {
          std::memcpy(result.bytes.data(), binary.begin(), result.bytes.size());
        }
      }
      KJ_CASE_ONEOF(close, kj::WebSocket::Close) {
        result.tag = 2;
        result.closeCode = close.code;
        result.text = close.reason.cStr();
      }
    }
    return result;
  }

  static kj::Promise<RuntimeWebSocket> decodeWebSocketResponse(
      kj::HttpClient::WebSocketResponse&& response, kj::Own<RuntimeWebSocketOwner>&& owner) {
    const auto statusCode = response.statusCode;
    if (statusCode != 101) {
      if (response.webSocketOrBody.is<kj::Own<kj::AsyncInputStream>>()) {
        auto stream = kj::mv(response.webSocketOrBody.get<kj::Own<kj::AsyncInputStream>>());
        return stream->readAllText().then([statusCode](kj::String&& body) -> RuntimeWebSocket {
          throw std::runtime_error("websocket connect failed with status " +
                                   std::to_string(statusCode) + ": " +
                                   std::string(body.cStr()));
        });
      }
      throw std::runtime_error("websocket connect failed with status " +
                               std::to_string(statusCode));
    }
    if (!response.webSocketOrBody.is<kj::Own<kj::WebSocket>>()) {
      throw std::runtime_error("websocket connect returned HTTP body instead of websocket upgrade");
    }

    RuntimeWebSocket out;
    out.owner = kj::mv(owner);
    out.socket = kj::mv(response.webSocketOrBody.get<kj::Own<kj::WebSocket>>());
    return out;
  }

  uint32_t webSocketConnectStart(
      kj::AsyncIoProvider& ioProvider, const std::string& address, uint32_t portHint,
      const std::string& path, std::vector<std::pair<std::string, std::string>> requestHeaders,
      bool useTls) {
    auto owner = kj::heap<RuntimeWebSocketOwner>();
    owner->headerTable = kj::heap<kj::HttpHeaderTable>();
    owner->entropySource = kj::heap<RuntimeEntropySource>();
    kj::HttpClientSettings settings;
    settings.entropySource = *owner->entropySource;
    auto tlsNetwork = resolveTlsNetwork(useTls);
    owner->client =
        kj::newHttpClient(ioProvider.getTimer(), *owner->headerTable, ioProvider.getNetwork(),
                          tlsNetwork, settings);
    auto requestUrl = buildUrl(useTls ? "https" : "http", address, portHint, path);
    auto headers = kj::HttpHeaders(*owner->headerTable);
    applyHeadersFromPairs(headers, requestHeaders);

    auto connectPromise =
        owner->client->openWebSocket(requestUrl, headers).then(
            [owner = kj::mv(owner)](kj::HttpClient::WebSocketResponse&& response) mutable {
              return decodeWebSocketResponse(kj::mv(response), kj::mv(owner));
            });

    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(kj::mv(connectPromise));
    return addWebSocketPromise(PendingWebSocketPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t webSocketConnect(kj::AsyncIoProvider& ioProvider, kj::WaitScope& waitScope,
                            const std::string& address, uint32_t portHint,
                            const std::string& path,
                            std::vector<std::pair<std::string, std::string>> requestHeaders,
                            bool useTls) {
    auto promiseId =
        webSocketConnectStart(ioProvider, address, portHint, path, std::move(requestHeaders),
                              useTls);
    return awaitWebSocketPromise(waitScope, promiseId);
  }

  void webSocketRelease(uint32_t webSocketId) {
    auto it = webSockets_.find(webSocketId);
    if (it == webSockets_.end()) {
      throw std::runtime_error("unknown KJ websocket id: " + std::to_string(webSocketId));
    }
    KJ_IF_SOME(fulfiller, it->second.requestCompletion) {
      fulfiller->fulfill();
      it->second.requestCompletion = kj::none;
    }
    webSockets_.erase(it);
  }

  void webSocketSendText(kj::WaitScope& waitScope, uint32_t webSocketId, const std::string& text) {
    auto it = webSockets_.find(webSocketId);
    if (it == webSockets_.end()) {
      throw std::runtime_error("unknown KJ websocket id: " + std::to_string(webSocketId));
    }
    it->second.socket->send(kj::ArrayPtr<const char>(text.data(), text.size())).wait(waitScope);
  }

  uint32_t webSocketSendTextStart(uint32_t webSocketId, std::string text) {
    auto it = webSockets_.find(webSocketId);
    if (it == webSockets_.end()) {
      throw std::runtime_error("unknown KJ websocket id: " + std::to_string(webSocketId));
    }
    auto canceler = kj::heap<kj::Canceler>();
    auto textCopy = kj::heapArray<char>(text.size());
    if (!text.empty()) {
      std::memcpy(textCopy.begin(), text.data(), text.size());
    }
    auto promise = canceler->wrap(it->second.socket->send(
                                      kj::ArrayPtr<const char>(textCopy.begin(), textCopy.size()))
                                      .attach(kj::mv(textCopy)));
    return addPromise(PendingPromise(kj::mv(promise), kj::mv(canceler)));
  }

  void webSocketSendBinary(kj::WaitScope& waitScope, uint32_t webSocketId,
                           std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto it = webSockets_.find(webSocketId);
    if (it == webSockets_.end()) {
      throw std::runtime_error("unknown KJ websocket id: " + std::to_string(webSocketId));
    }
    if (!bytes) {
      bytes = std::make_shared<std::vector<uint8_t>>();
    }
    auto ptr = kj::ArrayPtr<const kj::byte>(
        reinterpret_cast<const kj::byte*>(bytes->data()), bytes->size());
    it->second.socket->send(ptr).attach(kj::mv(bytes)).wait(waitScope);
  }

  uint32_t webSocketSendBinaryStart(uint32_t webSocketId,
                                    std::shared_ptr<std::vector<uint8_t>> bytes) {
    auto it = webSockets_.find(webSocketId);
    if (it == webSockets_.end()) {
      throw std::runtime_error("unknown KJ websocket id: " + std::to_string(webSocketId));
    }
    auto canceler = kj::heap<kj::Canceler>();
    if (!bytes) {
      bytes = std::make_shared<std::vector<uint8_t>>();
    }
    auto ptr = kj::ArrayPtr<const kj::byte>(
        reinterpret_cast<const kj::byte*>(bytes->data()), bytes->size());
    auto promise = canceler->wrap(it->second.socket->send(ptr).attach(kj::mv(bytes)));
    return addPromise(PendingPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t webSocketReceiveStart(uint32_t webSocketId, uint32_t maxBytes) {
    auto it = webSockets_.find(webSocketId);
    if (it == webSockets_.end()) {
      throw std::runtime_error("unknown KJ websocket id: " + std::to_string(webSocketId));
    }
    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(
        it->second.socket->receive(maxBytes).then([](kj::WebSocket::Message&& message) {
          return decodeWebSocketMessage(kj::mv(message));
        }));
    return addWebSocketMessagePromise(
        PendingWebSocketMessagePromise(kj::mv(promise), kj::mv(canceler)));
  }

  WebSocketMessageResult webSocketReceive(kj::WaitScope& waitScope, uint32_t webSocketId,
                                          uint32_t maxBytes) {
    auto promiseId = webSocketReceiveStart(webSocketId, maxBytes);
    return awaitWebSocketMessagePromise(waitScope, promiseId);
  }

  uint32_t webSocketCloseStart(uint32_t webSocketId, uint16_t closeCode, std::string reason) {
    auto it = webSockets_.find(webSocketId);
    if (it == webSockets_.end()) {
      throw std::runtime_error("unknown KJ websocket id: " + std::to_string(webSocketId));
    }
    auto canceler = kj::heap<kj::Canceler>();
    auto reasonCopy = kj::heapArray<char>(reason.size());
    if (!reason.empty()) {
      std::memcpy(reasonCopy.begin(), reason.data(), reason.size());
    }
    auto promise = canceler->wrap(
        it->second.socket
            ->close(closeCode, kj::StringPtr(reasonCopy.begin(), reasonCopy.size()))
            .attach(kj::mv(reasonCopy)));
    return addPromise(PendingPromise(kj::mv(promise), kj::mv(canceler)));
  }

  void webSocketClose(kj::WaitScope& waitScope, uint32_t webSocketId, uint16_t closeCode,
                      const std::string& reason) {
    auto promiseId = webSocketCloseStart(webSocketId, closeCode, reason);
    awaitPromise(waitScope, promiseId);
  }

  void webSocketDisconnect(uint32_t webSocketId) {
    auto it = webSockets_.find(webSocketId);
    if (it == webSockets_.end()) {
      throw std::runtime_error("unknown KJ websocket id: " + std::to_string(webSocketId));
    }
    it->second.socket->disconnect();
  }

  void webSocketAbort(uint32_t webSocketId) {
    auto it = webSockets_.find(webSocketId);
    if (it == webSockets_.end()) {
      throw std::runtime_error("unknown KJ websocket id: " + std::to_string(webSocketId));
    }
    it->second.socket->abort();
  }

  std::pair<uint32_t, uint32_t> newWebSocketPipe() {
    auto pipe = kj::newWebSocketPipe();
    auto first = addWebSocket(kj::mv(pipe.ends[0]));
    auto second = addWebSocket(kj::mv(pipe.ends[1]));
    return {first, second};
  }

  void failOutstanding(const std::string& message) {
    std::deque<QueuedOperation> queued;
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      queued.swap(queue_);
    }

    for (auto& op : queued) {
      if (std::holds_alternative<QueuedSleepNanos>(op)) {
        completePromiseIdFailure(std::get<QueuedSleepNanos>(op).completion, message);
      } else if (std::holds_alternative<QueuedAwaitPromise>(op)) {
        completeUnitFailure(std::get<QueuedAwaitPromise>(op).completion, message);
      } else if (std::holds_alternative<QueuedCancelPromise>(op)) {
        completeUnitFailure(std::get<QueuedCancelPromise>(op).completion, message);
      } else if (std::holds_alternative<QueuedReleasePromise>(op)) {
        completeUnitFailure(std::get<QueuedReleasePromise>(op).completion, message);
      } else if (std::holds_alternative<QueuedPromiseThenStart>(op)) {
        completePromiseIdFailure(std::get<QueuedPromiseThenStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedPromiseCatchStart>(op)) {
        completePromiseIdFailure(std::get<QueuedPromiseCatchStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedListen>(op)) {
        completeHandleFailure(std::get<QueuedListen>(op).completion, message);
      } else if (std::holds_alternative<QueuedReleaseListener>(op)) {
        completeUnitFailure(std::get<QueuedReleaseListener>(op).completion, message);
      } else if (std::holds_alternative<QueuedAccept>(op)) {
        completeHandleFailure(std::get<QueuedAccept>(op).completion, message);
      } else if (std::holds_alternative<QueuedAcceptStart>(op)) {
        completePromiseIdFailure(std::get<QueuedAcceptStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedConnect>(op)) {
        completeHandleFailure(std::get<QueuedConnect>(op).completion, message);
      } else if (std::holds_alternative<QueuedConnectStart>(op)) {
        completePromiseIdFailure(std::get<QueuedConnectStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedParseAddress>(op)) {
        completeHandleFailure(std::get<QueuedParseAddress>(op).completion, message);
      } else if (std::holds_alternative<QueuedReleaseNetworkAddress>(op)) {
        completeUnitFailure(std::get<QueuedReleaseNetworkAddress>(op).completion, message);
      } else if (std::holds_alternative<QueuedNetworkAddressToString>(op)) {
        completeOptionalStringFailure(
            std::get<QueuedNetworkAddressToString>(op).completion, message);
      } else if (std::holds_alternative<QueuedNetworkAddressClone>(op)) {
        completeHandleFailure(std::get<QueuedNetworkAddressClone>(op).completion, message);
      } else if (std::holds_alternative<QueuedNetworkAddressConnect>(op)) {
        completeHandleFailure(std::get<QueuedNetworkAddressConnect>(op).completion, message);
      } else if (std::holds_alternative<QueuedNetworkAddressConnectStart>(op)) {
        completePromiseIdFailure(
            std::get<QueuedNetworkAddressConnectStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedNetworkAddressListen>(op)) {
        completeHandleFailure(std::get<QueuedNetworkAddressListen>(op).completion, message);
      } else if (std::holds_alternative<QueuedNetworkAddressBindDatagramPort>(op)) {
        completeHandleFailure(
            std::get<QueuedNetworkAddressBindDatagramPort>(op).completion, message);
      } else if (std::holds_alternative<QueuedAwaitConnectionPromise>(op)) {
        completeHandleFailure(std::get<QueuedAwaitConnectionPromise>(op).completion, message);
      } else if (std::holds_alternative<QueuedAwaitConnectionPromiseWithTimeout>(op)) {
        completeOptionalUInt32Failure(
            std::get<QueuedAwaitConnectionPromiseWithTimeout>(op).completion, message);
      } else if (std::holds_alternative<QueuedCancelConnectionPromise>(op)) {
        completeUnitFailure(std::get<QueuedCancelConnectionPromise>(op).completion, message);
      } else if (std::holds_alternative<QueuedReleaseConnectionPromise>(op)) {
        completeUnitFailure(std::get<QueuedReleaseConnectionPromise>(op).completion, message);
      } else if (std::holds_alternative<QueuedReleaseConnection>(op)) {
        completeUnitFailure(std::get<QueuedReleaseConnection>(op).completion, message);
      } else if (std::holds_alternative<QueuedConnectionWrite>(op)) {
        completeUnitFailure(std::get<QueuedConnectionWrite>(op).completion, message);
      } else if (std::holds_alternative<QueuedConnectionWriteStart>(op)) {
        completePromiseIdFailure(std::get<QueuedConnectionWriteStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedConnectionRead>(op)) {
        completeBytesFailure(std::get<QueuedConnectionRead>(op).completion, message);
      } else if (std::holds_alternative<QueuedConnectionReadStart>(op)) {
        completePromiseIdFailure(std::get<QueuedConnectionReadStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedBytesPromiseAwait>(op)) {
        completeBytesFailure(std::get<QueuedBytesPromiseAwait>(op).completion, message);
      } else if (std::holds_alternative<QueuedBytesPromiseCancel>(op)) {
        completeUnitFailure(std::get<QueuedBytesPromiseCancel>(op).completion, message);
      } else if (std::holds_alternative<QueuedBytesPromiseRelease>(op)) {
        completeUnitFailure(std::get<QueuedBytesPromiseRelease>(op).completion, message);
      } else if (std::holds_alternative<QueuedConnectionShutdownWrite>(op)) {
        completeUnitFailure(std::get<QueuedConnectionShutdownWrite>(op).completion, message);
      } else if (std::holds_alternative<QueuedConnectionShutdownWriteStart>(op)) {
        completePromiseIdFailure(std::get<QueuedConnectionShutdownWriteStart>(op).completion,
                                 message);
      } else if (std::holds_alternative<QueuedPromiseAllStart>(op)) {
        completePromiseIdFailure(std::get<QueuedPromiseAllStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedPromiseRaceStart>(op)) {
        completePromiseIdFailure(std::get<QueuedPromiseRaceStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedTaskSetNew>(op)) {
        completeHandleFailure(std::get<QueuedTaskSetNew>(op).completion, message);
      } else if (std::holds_alternative<QueuedTaskSetRelease>(op)) {
        completeUnitFailure(std::get<QueuedTaskSetRelease>(op).completion, message);
      } else if (std::holds_alternative<QueuedTaskSetAddPromise>(op)) {
        completeUnitFailure(std::get<QueuedTaskSetAddPromise>(op).completion, message);
      } else if (std::holds_alternative<QueuedTaskSetClear>(op)) {
        completeUnitFailure(std::get<QueuedTaskSetClear>(op).completion, message);
      } else if (std::holds_alternative<QueuedTaskSetIsEmpty>(op)) {
        completeBoolFailure(std::get<QueuedTaskSetIsEmpty>(op).completion, message);
      } else if (std::holds_alternative<QueuedTaskSetOnEmptyStart>(op)) {
        completePromiseIdFailure(std::get<QueuedTaskSetOnEmptyStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedTaskSetErrorCount>(op)) {
        completeUInt32Failure(std::get<QueuedTaskSetErrorCount>(op).completion, message);
      } else if (std::holds_alternative<QueuedTaskSetTakeLastError>(op)) {
        completeOptionalStringFailure(std::get<QueuedTaskSetTakeLastError>(op).completion, message);
      } else if (std::holds_alternative<QueuedConnectionWhenWriteDisconnectedStart>(op)) {
        completePromiseIdFailure(
            std::get<QueuedConnectionWhenWriteDisconnectedStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedConnectionAbortRead>(op)) {
        completeUnitFailure(std::get<QueuedConnectionAbortRead>(op).completion, message);
      } else if (std::holds_alternative<QueuedConnectionAbortWrite>(op)) {
        completeUnitFailure(std::get<QueuedConnectionAbortWrite>(op).completion, message);
      } else if (std::holds_alternative<QueuedConnectionDupFd>(op)) {
        completeOptionalUInt32Failure(std::get<QueuedConnectionDupFd>(op).completion, message);
      } else if (std::holds_alternative<QueuedWrapSocketFd>(op)) {
        completeHandleFailure(std::get<QueuedWrapSocketFd>(op).completion, message);
      } else if (std::holds_alternative<QueuedWrapSocketFdTake>(op)) {
        completeHandleFailure(std::get<QueuedWrapSocketFdTake>(op).completion, message);
      } else if (std::holds_alternative<QueuedWrapListenSocketFd>(op)) {
        completeHandleFailure(std::get<QueuedWrapListenSocketFd>(op).completion, message);
      } else if (std::holds_alternative<QueuedWrapListenSocketFdTake>(op)) {
        completeHandleFailure(std::get<QueuedWrapListenSocketFdTake>(op).completion, message);
      } else if (std::holds_alternative<QueuedWrapDatagramSocketFd>(op)) {
        completeHandleFailure(std::get<QueuedWrapDatagramSocketFd>(op).completion, message);
      } else if (std::holds_alternative<QueuedWrapDatagramSocketFdTake>(op)) {
        completeHandleFailure(std::get<QueuedWrapDatagramSocketFdTake>(op).completion, message);
      } else if (std::holds_alternative<QueuedNewTwoWayPipe>(op)) {
        completeHandlePairFailure(std::get<QueuedNewTwoWayPipe>(op).completion, message);
      } else if (std::holds_alternative<QueuedNewCapabilityPipe>(op)) {
        completeHandlePairFailure(std::get<QueuedNewCapabilityPipe>(op).completion, message);
      } else if (std::holds_alternative<QueuedDatagramBind>(op)) {
        completeHandleFailure(std::get<QueuedDatagramBind>(op).completion, message);
      } else if (std::holds_alternative<QueuedDatagramReleasePort>(op)) {
        completeUnitFailure(std::get<QueuedDatagramReleasePort>(op).completion, message);
      } else if (std::holds_alternative<QueuedDatagramGetPort>(op)) {
        completeHandleFailure(std::get<QueuedDatagramGetPort>(op).completion, message);
      } else if (std::holds_alternative<QueuedDatagramSend>(op)) {
        completeUInt32Failure(std::get<QueuedDatagramSend>(op).completion, message);
      } else if (std::holds_alternative<QueuedDatagramSendStart>(op)) {
        completePromiseIdFailure(std::get<QueuedDatagramSendStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedUInt32PromiseAwait>(op)) {
        completeUInt32Failure(std::get<QueuedUInt32PromiseAwait>(op).completion, message);
      } else if (std::holds_alternative<QueuedUInt32PromiseCancel>(op)) {
        completeUnitFailure(std::get<QueuedUInt32PromiseCancel>(op).completion, message);
      } else if (std::holds_alternative<QueuedUInt32PromiseRelease>(op)) {
        completeUnitFailure(std::get<QueuedUInt32PromiseRelease>(op).completion, message);
      } else if (std::holds_alternative<QueuedDatagramReceive>(op)) {
        completeDatagramReceiveFailure(std::get<QueuedDatagramReceive>(op).completion, message);
      } else if (std::holds_alternative<QueuedDatagramReceiveStart>(op)) {
        completePromiseIdFailure(std::get<QueuedDatagramReceiveStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedDatagramReceivePromiseAwait>(op)) {
        completeDatagramReceiveFailure(std::get<QueuedDatagramReceivePromiseAwait>(op).completion,
                                       message);
      } else if (std::holds_alternative<QueuedDatagramReceivePromiseCancel>(op)) {
        completeUnitFailure(std::get<QueuedDatagramReceivePromiseCancel>(op).completion, message);
      } else if (std::holds_alternative<QueuedDatagramReceivePromiseRelease>(op)) {
        completeUnitFailure(std::get<QueuedDatagramReceivePromiseRelease>(op).completion,
                            message);
      } else if (std::holds_alternative<QueuedEnableTls>(op)) {
        completeUnitFailure(std::get<QueuedEnableTls>(op).completion, message);
      } else if (std::holds_alternative<QueuedConfigureTls>(op)) {
        completeUnitFailure(std::get<QueuedConfigureTls>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpRequest>(op)) {
        completeHttpResponseFailure(std::get<QueuedHttpRequest>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpRequestStart>(op)) {
        completePromiseIdFailure(std::get<QueuedHttpRequestStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpRequestStartStreaming>(op)) {
        completeHandlePairFailure(std::get<QueuedHttpRequestStartStreaming>(op).completion,
                                  message);
      } else if (std::holds_alternative<QueuedHttpResponsePromiseAwait>(op)) {
        completeHttpResponseFailure(std::get<QueuedHttpResponsePromiseAwait>(op).completion,
                                    message);
      } else if (std::holds_alternative<QueuedHttpResponsePromiseAwaitStreaming>(op)) {
        completeHttpResponseFailure(
            std::get<QueuedHttpResponsePromiseAwaitStreaming>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpResponsePromiseCancel>(op)) {
        completeUnitFailure(std::get<QueuedHttpResponsePromiseCancel>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpResponsePromiseRelease>(op)) {
        completeUnitFailure(std::get<QueuedHttpResponsePromiseRelease>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpRequestBodyWriteStart>(op)) {
        completePromiseIdFailure(std::get<QueuedHttpRequestBodyWriteStart>(op).completion,
                                 message);
      } else if (std::holds_alternative<QueuedHttpRequestBodyWrite>(op)) {
        completeUnitFailure(std::get<QueuedHttpRequestBodyWrite>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpRequestBodyFinishStart>(op)) {
        completePromiseIdFailure(std::get<QueuedHttpRequestBodyFinishStart>(op).completion,
                                 message);
      } else if (std::holds_alternative<QueuedHttpRequestBodyFinish>(op)) {
        completeUnitFailure(std::get<QueuedHttpRequestBodyFinish>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpRequestBodyRelease>(op)) {
        completeUnitFailure(std::get<QueuedHttpRequestBodyRelease>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpResponseBodyReadStart>(op)) {
        completePromiseIdFailure(std::get<QueuedHttpResponseBodyReadStart>(op).completion,
                                 message);
      } else if (std::holds_alternative<QueuedHttpResponseBodyRead>(op)) {
        completeBytesFailure(std::get<QueuedHttpResponseBodyRead>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpResponseBodyRelease>(op)) {
        completeUnitFailure(std::get<QueuedHttpResponseBodyRelease>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpServerRequestBodyReadStart>(op)) {
        completePromiseIdFailure(std::get<QueuedHttpServerRequestBodyReadStart>(op).completion,
                                 message);
      } else if (std::holds_alternative<QueuedHttpServerRequestBodyRead>(op)) {
        completeBytesFailure(std::get<QueuedHttpServerRequestBodyRead>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpServerRequestBodyRelease>(op)) {
        completeUnitFailure(std::get<QueuedHttpServerRequestBodyRelease>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketConnect>(op)) {
        completeHandleFailure(std::get<QueuedWebSocketConnect>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketConnectStart>(op)) {
        completePromiseIdFailure(std::get<QueuedWebSocketConnectStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketPromiseAwait>(op)) {
        completeHandleFailure(std::get<QueuedWebSocketPromiseAwait>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketPromiseCancel>(op)) {
        completeUnitFailure(std::get<QueuedWebSocketPromiseCancel>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketPromiseRelease>(op)) {
        completeUnitFailure(std::get<QueuedWebSocketPromiseRelease>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketRelease>(op)) {
        completeUnitFailure(std::get<QueuedWebSocketRelease>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketSendTextStart>(op)) {
        completePromiseIdFailure(std::get<QueuedWebSocketSendTextStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketSendText>(op)) {
        completeUnitFailure(std::get<QueuedWebSocketSendText>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketSendBinaryStart>(op)) {
        completePromiseIdFailure(std::get<QueuedWebSocketSendBinaryStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketSendBinary>(op)) {
        completeUnitFailure(std::get<QueuedWebSocketSendBinary>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketReceiveStart>(op)) {
        completePromiseIdFailure(std::get<QueuedWebSocketReceiveStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketMessagePromiseAwait>(op)) {
        completeWebSocketMessageFailure(
            std::get<QueuedWebSocketMessagePromiseAwait>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketMessagePromiseCancel>(op)) {
        completeUnitFailure(std::get<QueuedWebSocketMessagePromiseCancel>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketMessagePromiseRelease>(op)) {
        completeUnitFailure(std::get<QueuedWebSocketMessagePromiseRelease>(op).completion,
                            message);
      } else if (std::holds_alternative<QueuedWebSocketReceive>(op)) {
        completeWebSocketMessageFailure(std::get<QueuedWebSocketReceive>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketCloseStart>(op)) {
        completePromiseIdFailure(std::get<QueuedWebSocketCloseStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketClose>(op)) {
        completeUnitFailure(std::get<QueuedWebSocketClose>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketDisconnect>(op)) {
        completeUnitFailure(std::get<QueuedWebSocketDisconnect>(op).completion, message);
      } else if (std::holds_alternative<QueuedWebSocketAbort>(op)) {
        completeUnitFailure(std::get<QueuedWebSocketAbort>(op).completion, message);
      } else if (std::holds_alternative<QueuedNewWebSocketPipe>(op)) {
        completeHandlePairFailure(std::get<QueuedNewWebSocketPipe>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpServerListen>(op)) {
        completeHandlePairFailure(std::get<QueuedHttpServerListen>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpServerRelease>(op)) {
        completeUnitFailure(std::get<QueuedHttpServerRelease>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpServerDrainStart>(op)) {
        completePromiseIdFailure(std::get<QueuedHttpServerDrainStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpServerDrain>(op)) {
        completeUnitFailure(std::get<QueuedHttpServerDrain>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpServerPollRequest>(op)) {
        completeHttpServerRequestFailure(std::get<QueuedHttpServerPollRequest>(op).completion,
                                         message);
      } else if (std::holds_alternative<QueuedHttpServerRespond>(op)) {
        completeUnitFailure(std::get<QueuedHttpServerRespond>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpServerRespondWebSocket>(op)) {
        completeHandleFailure(std::get<QueuedHttpServerRespondWebSocket>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpServerRespondStartStreaming>(op)) {
        completeHandleFailure(std::get<QueuedHttpServerRespondStartStreaming>(op).completion,
                              message);
      } else if (std::holds_alternative<QueuedHttpServerResponseBodyWriteStart>(op)) {
        completePromiseIdFailure(std::get<QueuedHttpServerResponseBodyWriteStart>(op).completion,
                                 message);
      } else if (std::holds_alternative<QueuedHttpServerResponseBodyWrite>(op)) {
        completeUnitFailure(std::get<QueuedHttpServerResponseBodyWrite>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpServerResponseBodyFinishStart>(op)) {
        completePromiseIdFailure(std::get<QueuedHttpServerResponseBodyFinishStart>(op).completion,
                                 message);
      } else if (std::holds_alternative<QueuedHttpServerResponseBodyFinish>(op)) {
        completeUnitFailure(std::get<QueuedHttpServerResponseBodyFinish>(op).completion, message);
      } else if (std::holds_alternative<QueuedHttpServerResponseBodyRelease>(op)) {
        completeUnitFailure(std::get<QueuedHttpServerResponseBodyRelease>(op).completion, message);
      }
    }
  }

  void run() {
    try {
      auto io = kj::setupAsyncIo();
      {
        std::lock_guard<std::mutex> lock(startupMutex_);
        startupComplete_ = true;
      }
      alive_.store(true, std::memory_order_release);
      startupCv_.notify_one();

      while (true) {
        QueuedOperation op;
        {
          std::unique_lock<std::mutex> lock(queueMutex_);
          queueCv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
          if (stopping_ && queue_.empty()) {
            break;
          }
          op = std::move(queue_.front());
          queue_.pop_front();
        }

        if (std::holds_alternative<QueuedSleepNanos>(op)) {
          auto sleep = std::get<QueuedSleepNanos>(std::move(op));
          try {
            uint64_t clamped = std::min<uint64_t>(
                sleep.delayNanos, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
            auto delay = static_cast<int64_t>(clamped) * kj::NANOSECONDS;
            auto canceler = kj::heap<kj::Canceler>();
            auto promise = canceler->wrap(io.provider->getTimer().afterDelay(delay));
            auto promiseId = addPromise(PendingPromise(kj::mv(promise), kj::mv(canceler)));
            completePromiseIdSuccess(sleep.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(sleep.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(sleep.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(sleep.completion,
                                     "unknown exception in Capnp.KjAsync sleep");
          }
        } else if (std::holds_alternative<QueuedAwaitPromise>(op)) {
          auto await = std::get<QueuedAwaitPromise>(std::move(op));
          try {
            awaitPromise(io.waitScope, await.promiseId);
            completeUnitSuccess(await.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(await.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(await.completion, e.what());
          } catch (...) {
            completeUnitFailure(await.completion,
                                "unknown exception in Capnp.KjAsync promise await");
          }
        } else if (std::holds_alternative<QueuedCancelPromise>(op)) {
          auto cancel = std::get<QueuedCancelPromise>(std::move(op));
          try {
            cancelPromise(cancel.promiseId);
            completeUnitSuccess(cancel.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(cancel.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(cancel.completion, e.what());
          } catch (...) {
            completeUnitFailure(cancel.completion,
                                "unknown exception in Capnp.KjAsync promise cancel");
          }
        } else if (std::holds_alternative<QueuedReleasePromise>(op)) {
          auto release = std::get<QueuedReleasePromise>(std::move(op));
          try {
            releasePromise(release.promiseId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(release.completion,
                                "unknown exception in Capnp.KjAsync promise release");
          }
        } else if (std::holds_alternative<QueuedPromiseThenStart>(op)) {
          auto compose = std::get<QueuedPromiseThenStart>(std::move(op));
          try {
            auto promiseId = promiseThenStart(compose.firstPromiseId, compose.secondPromiseId);
            completePromiseIdSuccess(compose.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(compose.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(compose.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(compose.completion,
                                     "unknown exception in Capnp.KjAsync promiseThenStart");
          }
        } else if (std::holds_alternative<QueuedPromiseCatchStart>(op)) {
          auto compose = std::get<QueuedPromiseCatchStart>(std::move(op));
          try {
            auto promiseId = promiseCatchStart(compose.promiseId, compose.fallbackPromiseId);
            completePromiseIdSuccess(compose.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(compose.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(compose.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(compose.completion,
                                     "unknown exception in Capnp.KjAsync promiseCatchStart");
          }
        } else if (std::holds_alternative<QueuedListen>(op)) {
          auto listenReq = std::get<QueuedListen>(std::move(op));
          try {
            auto listenerId = listen(*io.provider, io.waitScope, listenReq.address,
                                     listenReq.portHint);
            completeHandleSuccess(listenReq.completion, listenerId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(listenReq.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(listenReq.completion, e.what());
          } catch (...) {
            completeHandleFailure(listenReq.completion,
                                  "unknown exception in Capnp.KjAsync listen");
          }
        } else if (std::holds_alternative<QueuedReleaseListener>(op)) {
          auto release = std::get<QueuedReleaseListener>(std::move(op));
          try {
            releaseListener(release.listenerId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(release.completion,
                                "unknown exception in Capnp.KjAsync release listener");
          }
        } else if (std::holds_alternative<QueuedAccept>(op)) {
          auto acceptReq = std::get<QueuedAccept>(std::move(op));
          try {
            auto connectionId = accept(io.waitScope, acceptReq.listenerId);
            completeHandleSuccess(acceptReq.completion, connectionId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(acceptReq.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(acceptReq.completion, e.what());
          } catch (...) {
            completeHandleFailure(acceptReq.completion,
                                  "unknown exception in Capnp.KjAsync accept");
          }
        } else if (std::holds_alternative<QueuedAcceptStart>(op)) {
          auto acceptReq = std::get<QueuedAcceptStart>(std::move(op));
          try {
            auto promiseId = acceptStart(acceptReq.listenerId);
            completePromiseIdSuccess(acceptReq.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(acceptReq.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(acceptReq.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(acceptReq.completion,
                                     "unknown exception in Capnp.KjAsync acceptStart");
          }
        } else if (std::holds_alternative<QueuedConnect>(op)) {
          auto connectReq = std::get<QueuedConnect>(std::move(op));
          try {
            auto connectionId =
                connect(*io.provider, io.waitScope, connectReq.address, connectReq.portHint);
            completeHandleSuccess(connectReq.completion, connectionId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(connectReq.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(connectReq.completion, e.what());
          } catch (...) {
            completeHandleFailure(connectReq.completion,
                                  "unknown exception in Capnp.KjAsync connect");
          }
        } else if (std::holds_alternative<QueuedConnectStart>(op)) {
          auto connectReq = std::get<QueuedConnectStart>(std::move(op));
          try {
            auto promiseId = connectStart(*io.provider, connectReq.address, connectReq.portHint);
            completePromiseIdSuccess(connectReq.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(connectReq.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(connectReq.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(connectReq.completion,
                                     "unknown exception in Capnp.KjAsync connectStart");
          }
        } else if (std::holds_alternative<QueuedParseAddress>(op)) {
          auto parseReq = std::get<QueuedParseAddress>(std::move(op));
          try {
            auto addressId =
                parseAddress(*io.provider, io.waitScope, parseReq.address, parseReq.portHint);
            completeHandleSuccess(parseReq.completion, addressId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(parseReq.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(parseReq.completion, e.what());
          } catch (...) {
            completeHandleFailure(parseReq.completion,
                                  "unknown exception in Capnp.KjAsync parseAddress");
          }
        } else if (std::holds_alternative<QueuedReleaseNetworkAddress>(op)) {
          auto release = std::get<QueuedReleaseNetworkAddress>(std::move(op));
          try {
            releaseNetworkAddress(release.addressId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                release.completion,
                "unknown exception in Capnp.KjAsync release network address");
          }
        } else if (std::holds_alternative<QueuedNetworkAddressToString>(op)) {
          auto query = std::get<QueuedNetworkAddressToString>(std::move(op));
          try {
            completeOptionalStringSuccess(query.completion, networkAddressToString(query.addressId));
          } catch (const kj::Exception& e) {
            completeOptionalStringFailure(query.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeOptionalStringFailure(query.completion, e.what());
          } catch (...) {
            completeOptionalStringFailure(
                query.completion,
                "unknown exception in Capnp.KjAsync network address toString");
          }
        } else if (std::holds_alternative<QueuedNetworkAddressClone>(op)) {
          auto query = std::get<QueuedNetworkAddressClone>(std::move(op));
          try {
            auto addressId = networkAddressClone(query.addressId);
            completeHandleSuccess(query.completion, addressId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(query.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(query.completion, e.what());
          } catch (...) {
            completeHandleFailure(
                query.completion,
                "unknown exception in Capnp.KjAsync network address clone");
          }
        } else if (std::holds_alternative<QueuedNetworkAddressConnect>(op)) {
          auto query = std::get<QueuedNetworkAddressConnect>(std::move(op));
          try {
            auto connectionId = networkAddressConnect(io.waitScope, query.addressId);
            completeHandleSuccess(query.completion, connectionId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(query.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(query.completion, e.what());
          } catch (...) {
            completeHandleFailure(
                query.completion,
                "unknown exception in Capnp.KjAsync network address connect");
          }
        } else if (std::holds_alternative<QueuedNetworkAddressConnectStart>(op)) {
          auto query = std::get<QueuedNetworkAddressConnectStart>(std::move(op));
          try {
            auto promiseId = networkAddressConnectStart(query.addressId);
            completePromiseIdSuccess(query.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(query.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(query.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(
                query.completion,
                "unknown exception in Capnp.KjAsync network address connectStart");
          }
        } else if (std::holds_alternative<QueuedNetworkAddressListen>(op)) {
          auto query = std::get<QueuedNetworkAddressListen>(std::move(op));
          try {
            auto listenerId = networkAddressListen(query.addressId);
            completeHandleSuccess(query.completion, listenerId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(query.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(query.completion, e.what());
          } catch (...) {
            completeHandleFailure(
                query.completion,
                "unknown exception in Capnp.KjAsync network address listen");
          }
        } else if (std::holds_alternative<QueuedNetworkAddressBindDatagramPort>(op)) {
          auto query = std::get<QueuedNetworkAddressBindDatagramPort>(std::move(op));
          try {
            auto portId = networkAddressBindDatagramPort(query.addressId);
            completeHandleSuccess(query.completion, portId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(query.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(query.completion, e.what());
          } catch (...) {
            completeHandleFailure(
                query.completion,
                "unknown exception in Capnp.KjAsync network address bindDatagramPort");
          }
        } else if (std::holds_alternative<QueuedAwaitConnectionPromise>(op)) {
          auto await = std::get<QueuedAwaitConnectionPromise>(std::move(op));
          try {
            auto connectionId = awaitConnectionPromise(io.waitScope, await.promiseId);
            completeHandleSuccess(await.completion, connectionId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(await.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(await.completion, e.what());
          } catch (...) {
            completeHandleFailure(
                await.completion,
                "unknown exception in Capnp.KjAsync connection promise await");
          }
        } else if (std::holds_alternative<QueuedAwaitConnectionPromiseWithTimeout>(op)) {
          auto await = std::get<QueuedAwaitConnectionPromiseWithTimeout>(std::move(op));
          try {
            auto connectionId = awaitConnectionPromiseWithTimeout(
                *io.provider, io.waitScope, await.promiseId, await.timeoutNanos);
            completeOptionalUInt32Success(await.completion, connectionId);
          } catch (const kj::Exception& e) {
            completeOptionalUInt32Failure(await.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeOptionalUInt32Failure(await.completion, e.what());
          } catch (...) {
            completeOptionalUInt32Failure(
                await.completion,
                "unknown exception in Capnp.KjAsync connection promise awaitWithTimeout");
          }
        } else if (std::holds_alternative<QueuedCancelConnectionPromise>(op)) {
          auto cancel = std::get<QueuedCancelConnectionPromise>(std::move(op));
          try {
            cancelConnectionPromise(cancel.promiseId);
            completeUnitSuccess(cancel.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(cancel.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(cancel.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                cancel.completion,
                "unknown exception in Capnp.KjAsync connection promise cancel");
          }
        } else if (std::holds_alternative<QueuedReleaseConnectionPromise>(op)) {
          auto release = std::get<QueuedReleaseConnectionPromise>(std::move(op));
          try {
            releaseConnectionPromise(release.promiseId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                release.completion,
                "unknown exception in Capnp.KjAsync connection promise release");
          }
        } else if (std::holds_alternative<QueuedReleaseConnection>(op)) {
          auto release = std::get<QueuedReleaseConnection>(std::move(op));
          try {
            releaseConnection(release.connectionId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(release.completion,
                                "unknown exception in Capnp.KjAsync release connection");
          }
        } else if (std::holds_alternative<QueuedConnectionWrite>(op)) {
          auto writeReq = std::get<QueuedConnectionWrite>(std::move(op));
          try {
            connectionWrite(io.waitScope, writeReq.connectionId, kj::mv(writeReq.bytes));
            completeUnitSuccess(writeReq.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(writeReq.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(writeReq.completion, e.what());
          } catch (...) {
            completeUnitFailure(writeReq.completion,
                                "unknown exception in Capnp.KjAsync connection write");
          }
        } else if (std::holds_alternative<QueuedConnectionWriteStart>(op)) {
          auto writeReq = std::get<QueuedConnectionWriteStart>(std::move(op));
          try {
            auto promiseId = connectionWriteStart(writeReq.connectionId, kj::mv(writeReq.bytes));
            completePromiseIdSuccess(writeReq.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(writeReq.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(writeReq.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(
                writeReq.completion,
                "unknown exception in Capnp.KjAsync connection write start");
          }
        } else if (std::holds_alternative<QueuedConnectionRead>(op)) {
          auto readReq = std::get<QueuedConnectionRead>(std::move(op));
          try {
            auto bytes = connectionRead(io.waitScope, readReq.connectionId, readReq.minBytes,
                                        readReq.maxBytes);
            completeBytesSuccess(readReq.completion, std::move(bytes));
          } catch (const kj::Exception& e) {
            completeBytesFailure(readReq.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeBytesFailure(readReq.completion, e.what());
          } catch (...) {
            completeBytesFailure(readReq.completion,
                                 "unknown exception in Capnp.KjAsync connection read");
          }
        } else if (std::holds_alternative<QueuedConnectionReadStart>(op)) {
          auto readReq = std::get<QueuedConnectionReadStart>(std::move(op));
          try {
            auto promiseId =
                connectionReadStart(readReq.connectionId, readReq.minBytes, readReq.maxBytes);
            completePromiseIdSuccess(readReq.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(readReq.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(readReq.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(readReq.completion,
                                     "unknown exception in Capnp.KjAsync connection read start");
          }
        } else if (std::holds_alternative<QueuedBytesPromiseAwait>(op)) {
          auto await = std::get<QueuedBytesPromiseAwait>(std::move(op));
          try {
            auto bytes = awaitBytesPromise(io.waitScope, await.promiseId);
            completeBytesSuccess(await.completion, std::move(bytes));
          } catch (const kj::Exception& e) {
            completeBytesFailure(await.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeBytesFailure(await.completion, e.what());
          } catch (...) {
            completeBytesFailure(await.completion,
                                 "unknown exception in Capnp.KjAsync bytes promise await");
          }
        } else if (std::holds_alternative<QueuedBytesPromiseCancel>(op)) {
          auto cancel = std::get<QueuedBytesPromiseCancel>(std::move(op));
          try {
            cancelBytesPromise(cancel.promiseId);
            completeUnitSuccess(cancel.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(cancel.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(cancel.completion, e.what());
          } catch (...) {
            completeUnitFailure(cancel.completion,
                                "unknown exception in Capnp.KjAsync bytes promise cancel");
          }
        } else if (std::holds_alternative<QueuedBytesPromiseRelease>(op)) {
          auto release = std::get<QueuedBytesPromiseRelease>(std::move(op));
          try {
            releaseBytesPromise(release.promiseId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(release.completion,
                                "unknown exception in Capnp.KjAsync bytes promise release");
          }
        } else if (std::holds_alternative<QueuedConnectionShutdownWrite>(op)) {
          auto shutdown = std::get<QueuedConnectionShutdownWrite>(std::move(op));
          try {
            connectionShutdownWrite(shutdown.connectionId);
            completeUnitSuccess(shutdown.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(shutdown.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(shutdown.completion, e.what());
          } catch (...) {
            completeUnitFailure(shutdown.completion,
                                "unknown exception in Capnp.KjAsync connection shutdownWrite");
          }
        } else if (std::holds_alternative<QueuedConnectionShutdownWriteStart>(op)) {
          auto shutdown = std::get<QueuedConnectionShutdownWriteStart>(std::move(op));
          try {
            auto promiseId = connectionShutdownWriteStart(shutdown.connectionId);
            completePromiseIdSuccess(shutdown.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(shutdown.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(shutdown.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(
                shutdown.completion,
                "unknown exception in Capnp.KjAsync connection shutdownWrite start");
          }
        } else if (std::holds_alternative<QueuedPromiseAllStart>(op)) {
          auto compose = std::get<QueuedPromiseAllStart>(std::move(op));
          try {
            auto promiseId = promiseAllStart(std::move(compose.promiseIds));
            completePromiseIdSuccess(compose.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(compose.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(compose.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(compose.completion,
                                     "unknown exception in Capnp.KjAsync promiseAllStart");
          }
        } else if (std::holds_alternative<QueuedPromiseRaceStart>(op)) {
          auto compose = std::get<QueuedPromiseRaceStart>(std::move(op));
          try {
            auto promiseId = promiseRaceStart(std::move(compose.promiseIds));
            completePromiseIdSuccess(compose.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(compose.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(compose.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(compose.completion,
                                     "unknown exception in Capnp.KjAsync promiseRaceStart");
          }
        } else if (std::holds_alternative<QueuedTaskSetNew>(op)) {
          auto create = std::get<QueuedTaskSetNew>(std::move(op));
          try {
            auto taskSetId = taskSetNew();
            completeHandleSuccess(create.completion, taskSetId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(create.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(create.completion, e.what());
          } catch (...) {
            completeHandleFailure(create.completion,
                                  "unknown exception in Capnp.KjAsync taskSetNew");
          }
        } else if (std::holds_alternative<QueuedTaskSetRelease>(op)) {
          auto release = std::get<QueuedTaskSetRelease>(std::move(op));
          try {
            taskSetRelease(release.taskSetId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(release.completion,
                                "unknown exception in Capnp.KjAsync taskSetRelease");
          }
        } else if (std::holds_alternative<QueuedTaskSetAddPromise>(op)) {
          auto add = std::get<QueuedTaskSetAddPromise>(std::move(op));
          try {
            taskSetAddPromise(add.taskSetId, add.promiseId);
            completeUnitSuccess(add.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(add.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(add.completion, e.what());
          } catch (...) {
            completeUnitFailure(add.completion,
                                "unknown exception in Capnp.KjAsync taskSetAddPromise");
          }
        } else if (std::holds_alternative<QueuedTaskSetClear>(op)) {
          auto clear = std::get<QueuedTaskSetClear>(std::move(op));
          try {
            taskSetClear(clear.taskSetId);
            completeUnitSuccess(clear.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(clear.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(clear.completion, e.what());
          } catch (...) {
            completeUnitFailure(clear.completion,
                                "unknown exception in Capnp.KjAsync taskSetClear");
          }
        } else if (std::holds_alternative<QueuedTaskSetIsEmpty>(op)) {
          auto query = std::get<QueuedTaskSetIsEmpty>(std::move(op));
          try {
            completeBoolSuccess(query.completion, taskSetIsEmpty(query.taskSetId));
          } catch (const kj::Exception& e) {
            completeBoolFailure(query.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeBoolFailure(query.completion, e.what());
          } catch (...) {
            completeBoolFailure(query.completion,
                                "unknown exception in Capnp.KjAsync taskSetIsEmpty");
          }
        } else if (std::holds_alternative<QueuedTaskSetOnEmptyStart>(op)) {
          auto start = std::get<QueuedTaskSetOnEmptyStart>(std::move(op));
          try {
            auto promiseId = taskSetOnEmptyStart(start.taskSetId);
            completePromiseIdSuccess(start.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(start.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(start.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(start.completion,
                                     "unknown exception in Capnp.KjAsync taskSetOnEmptyStart");
          }
        } else if (std::holds_alternative<QueuedTaskSetErrorCount>(op)) {
          auto query = std::get<QueuedTaskSetErrorCount>(std::move(op));
          try {
            completeUInt32Success(query.completion, taskSetErrorCount(query.taskSetId));
          } catch (const kj::Exception& e) {
            completeUInt32Failure(query.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt32Failure(query.completion, e.what());
          } catch (...) {
            completeUInt32Failure(query.completion,
                                  "unknown exception in Capnp.KjAsync taskSetErrorCount");
          }
        } else if (std::holds_alternative<QueuedTaskSetTakeLastError>(op)) {
          auto query = std::get<QueuedTaskSetTakeLastError>(std::move(op));
          try {
            completeOptionalStringSuccess(query.completion, taskSetTakeLastError(query.taskSetId));
          } catch (const kj::Exception& e) {
            completeOptionalStringFailure(query.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeOptionalStringFailure(query.completion, e.what());
          } catch (...) {
            completeOptionalStringFailure(
                query.completion, "unknown exception in Capnp.KjAsync taskSetTakeLastError");
          }
        } else if (std::holds_alternative<QueuedConnectionWhenWriteDisconnectedStart>(op)) {
          auto start = std::get<QueuedConnectionWhenWriteDisconnectedStart>(std::move(op));
          try {
            auto promiseId = connectionWhenWriteDisconnectedStart(start.connectionId);
            completePromiseIdSuccess(start.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(start.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(start.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(
                start.completion,
                "unknown exception in Capnp.KjAsync connection whenWriteDisconnected start");
          }
        } else if (std::holds_alternative<QueuedConnectionAbortRead>(op)) {
          auto abort = std::get<QueuedConnectionAbortRead>(std::move(op));
          try {
            connectionAbortRead(abort.connectionId);
            completeUnitSuccess(abort.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(abort.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(abort.completion, e.what());
          } catch (...) {
            completeUnitFailure(abort.completion,
                                "unknown exception in Capnp.KjAsync connection abortRead");
          }
        } else if (std::holds_alternative<QueuedConnectionAbortWrite>(op)) {
          auto abort = std::get<QueuedConnectionAbortWrite>(std::move(op));
          try {
            connectionAbortWrite(abort.connectionId, std::move(abort.reason));
            completeUnitSuccess(abort.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(abort.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(abort.completion, e.what());
          } catch (...) {
            completeUnitFailure(abort.completion,
                                "unknown exception in Capnp.KjAsync connection abortWrite");
          }
        } else if (std::holds_alternative<QueuedConnectionDupFd>(op)) {
          auto dup = std::get<QueuedConnectionDupFd>(std::move(op));
          try {
            auto fd = connectionDupFd(dup.connectionId);
            completeOptionalUInt32Success(dup.completion, fd);
          } catch (const kj::Exception& e) {
            completeOptionalUInt32Failure(dup.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeOptionalUInt32Failure(dup.completion, e.what());
          } catch (...) {
            completeOptionalUInt32Failure(
                dup.completion,
                "unknown exception in Capnp.KjAsync connection dupFd");
          }
        } else if (std::holds_alternative<QueuedWrapSocketFd>(op)) {
          auto wrap = std::get<QueuedWrapSocketFd>(std::move(op));
          try {
            auto connectionId = wrapSocketFd(*io.lowLevelProvider, wrap.fd);
            completeHandleSuccess(wrap.completion, connectionId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(wrap.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(wrap.completion, e.what());
          } catch (...) {
            completeHandleFailure(wrap.completion,
                                  "unknown exception in Capnp.KjAsync wrapSocketFd");
          }
        } else if (std::holds_alternative<QueuedWrapSocketFdTake>(op)) {
          auto wrap = std::get<QueuedWrapSocketFdTake>(std::move(op));
          try {
            auto connectionId = wrapSocketFdTake(*io.lowLevelProvider, wrap.fd);
            completeHandleSuccess(wrap.completion, connectionId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(wrap.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(wrap.completion, e.what());
          } catch (...) {
            completeHandleFailure(wrap.completion,
                                  "unknown exception in Capnp.KjAsync wrapSocketFdTake");
          }
        } else if (std::holds_alternative<QueuedWrapListenSocketFd>(op)) {
          auto wrap = std::get<QueuedWrapListenSocketFd>(std::move(op));
          try {
            auto listenerId = wrapListenSocketFd(*io.lowLevelProvider, wrap.fd);
            completeHandleSuccess(wrap.completion, listenerId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(wrap.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(wrap.completion, e.what());
          } catch (...) {
            completeHandleFailure(wrap.completion,
                                  "unknown exception in Capnp.KjAsync wrapListenSocketFd");
          }
        } else if (std::holds_alternative<QueuedWrapListenSocketFdTake>(op)) {
          auto wrap = std::get<QueuedWrapListenSocketFdTake>(std::move(op));
          try {
            auto listenerId = wrapListenSocketFdTake(*io.lowLevelProvider, wrap.fd);
            completeHandleSuccess(wrap.completion, listenerId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(wrap.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(wrap.completion, e.what());
          } catch (...) {
            completeHandleFailure(wrap.completion,
                                  "unknown exception in Capnp.KjAsync wrapListenSocketFdTake");
          }
        } else if (std::holds_alternative<QueuedWrapDatagramSocketFd>(op)) {
          auto wrap = std::get<QueuedWrapDatagramSocketFd>(std::move(op));
          try {
            auto datagramPortId = wrapDatagramSocketFd(*io.lowLevelProvider, wrap.fd);
            completeHandleSuccess(wrap.completion, datagramPortId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(wrap.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(wrap.completion, e.what());
          } catch (...) {
            completeHandleFailure(wrap.completion,
                                  "unknown exception in Capnp.KjAsync wrapDatagramSocketFd");
          }
        } else if (std::holds_alternative<QueuedWrapDatagramSocketFdTake>(op)) {
          auto wrap = std::get<QueuedWrapDatagramSocketFdTake>(std::move(op));
          try {
            auto datagramPortId = wrapDatagramSocketFdTake(*io.lowLevelProvider, wrap.fd);
            completeHandleSuccess(wrap.completion, datagramPortId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(wrap.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(wrap.completion, e.what());
          } catch (...) {
            completeHandleFailure(wrap.completion,
                                  "unknown exception in Capnp.KjAsync wrapDatagramSocketFdTake");
          }
        } else if (std::holds_alternative<QueuedNewTwoWayPipe>(op)) {
          auto create = std::get<QueuedNewTwoWayPipe>(std::move(op));
          try {
            auto pair = newTwoWayPipe(*io.provider);
            completeHandlePairSuccess(create.completion, pair.first, pair.second);
          } catch (const kj::Exception& e) {
            completeHandlePairFailure(create.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandlePairFailure(create.completion, e.what());
          } catch (...) {
            completeHandlePairFailure(create.completion,
                                      "unknown exception in Capnp.KjAsync newTwoWayPipe");
          }
        } else if (std::holds_alternative<QueuedNewCapabilityPipe>(op)) {
          auto create = std::get<QueuedNewCapabilityPipe>(std::move(op));
          try {
            auto pair = newCapabilityPipe(*io.provider);
            completeHandlePairSuccess(create.completion, pair.first, pair.second);
          } catch (const kj::Exception& e) {
            completeHandlePairFailure(create.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandlePairFailure(create.completion, e.what());
          } catch (...) {
            completeHandlePairFailure(create.completion,
                                      "unknown exception in Capnp.KjAsync newCapabilityPipe");
          }
        } else if (std::holds_alternative<QueuedDatagramBind>(op)) {
          auto bind = std::get<QueuedDatagramBind>(std::move(op));
          try {
            auto portId = datagramBind(*io.provider, io.waitScope, bind.address, bind.portHint);
            completeHandleSuccess(bind.completion, portId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(bind.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(bind.completion, e.what());
          } catch (...) {
            completeHandleFailure(bind.completion,
                                  "unknown exception in Capnp.KjAsync datagramBind");
          }
        } else if (std::holds_alternative<QueuedDatagramReleasePort>(op)) {
          auto release = std::get<QueuedDatagramReleasePort>(std::move(op));
          try {
            datagramReleasePort(release.portId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(release.completion,
                                "unknown exception in Capnp.KjAsync datagramReleasePort");
          }
        } else if (std::holds_alternative<QueuedDatagramGetPort>(op)) {
          auto query = std::get<QueuedDatagramGetPort>(std::move(op));
          try {
            completeHandleSuccess(query.completion, datagramGetPort(query.portId));
          } catch (const kj::Exception& e) {
            completeHandleFailure(query.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(query.completion, e.what());
          } catch (...) {
            completeHandleFailure(query.completion,
                                  "unknown exception in Capnp.KjAsync datagramGetPort");
          }
        } else if (std::holds_alternative<QueuedDatagramSend>(op)) {
          auto send = std::get<QueuedDatagramSend>(std::move(op));
          try {
            auto sent = datagramSend(*io.provider, io.waitScope, send.portId, send.address,
                                     send.portHint, std::move(send.bytes));
            completeUInt32Success(send.completion, sent);
          } catch (const kj::Exception& e) {
            completeUInt32Failure(send.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt32Failure(send.completion, e.what());
          } catch (...) {
            completeUInt32Failure(send.completion,
                                  "unknown exception in Capnp.KjAsync datagramSend");
          }
        } else if (std::holds_alternative<QueuedDatagramSendStart>(op)) {
          auto send = std::get<QueuedDatagramSendStart>(std::move(op));
          try {
            auto promiseId =
                datagramSendStart(*io.provider, send.portId, send.address, send.portHint,
                                  std::move(send.bytes));
            completePromiseIdSuccess(send.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(send.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(send.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(send.completion,
                                     "unknown exception in Capnp.KjAsync datagramSendStart");
          }
        } else if (std::holds_alternative<QueuedUInt32PromiseAwait>(op)) {
          auto await = std::get<QueuedUInt32PromiseAwait>(std::move(op));
          try {
            auto value = awaitUInt32Promise(io.waitScope, await.promiseId);
            completeUInt32Success(await.completion, value);
          } catch (const kj::Exception& e) {
            completeUInt32Failure(await.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt32Failure(await.completion, e.what());
          } catch (...) {
            completeUInt32Failure(await.completion,
                                  "unknown exception in Capnp.KjAsync uint32 promise await");
          }
        } else if (std::holds_alternative<QueuedUInt32PromiseCancel>(op)) {
          auto cancel = std::get<QueuedUInt32PromiseCancel>(std::move(op));
          try {
            cancelUInt32Promise(cancel.promiseId);
            completeUnitSuccess(cancel.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(cancel.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(cancel.completion, e.what());
          } catch (...) {
            completeUnitFailure(cancel.completion,
                                "unknown exception in Capnp.KjAsync uint32 promise cancel");
          }
        } else if (std::holds_alternative<QueuedUInt32PromiseRelease>(op)) {
          auto release = std::get<QueuedUInt32PromiseRelease>(std::move(op));
          try {
            releaseUInt32Promise(release.promiseId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(release.completion,
                                "unknown exception in Capnp.KjAsync uint32 promise release");
          }
        } else if (std::holds_alternative<QueuedDatagramReceive>(op)) {
          auto recv = std::get<QueuedDatagramReceive>(std::move(op));
          try {
            auto result = datagramReceive(io.waitScope, recv.portId, recv.maxBytes);
            completeDatagramReceiveSuccess(
                recv.completion, std::move(result.first), std::move(result.second));
          } catch (const kj::Exception& e) {
            completeDatagramReceiveFailure(recv.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeDatagramReceiveFailure(recv.completion, e.what());
          } catch (...) {
            completeDatagramReceiveFailure(
                recv.completion, "unknown exception in Capnp.KjAsync datagramReceive");
          }
        } else if (std::holds_alternative<QueuedDatagramReceiveStart>(op)) {
          auto recv = std::get<QueuedDatagramReceiveStart>(std::move(op));
          try {
            auto promiseId = datagramReceiveStart(recv.portId, recv.maxBytes);
            completePromiseIdSuccess(recv.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(recv.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(recv.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(
                recv.completion, "unknown exception in Capnp.KjAsync datagramReceiveStart");
          }
        } else if (std::holds_alternative<QueuedDatagramReceivePromiseAwait>(op)) {
          auto await = std::get<QueuedDatagramReceivePromiseAwait>(std::move(op));
          try {
            auto result = awaitDatagramReceivePromise(io.waitScope, await.promiseId);
            completeDatagramReceiveSuccess(
                await.completion, std::move(result.sourceAddress), std::move(result.bytes));
          } catch (const kj::Exception& e) {
            completeDatagramReceiveFailure(await.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeDatagramReceiveFailure(await.completion, e.what());
          } catch (...) {
            completeDatagramReceiveFailure(
                await.completion,
                "unknown exception in Capnp.KjAsync datagram receive promise await");
          }
        } else if (std::holds_alternative<QueuedDatagramReceivePromiseCancel>(op)) {
          auto cancel = std::get<QueuedDatagramReceivePromiseCancel>(std::move(op));
          try {
            cancelDatagramReceivePromise(cancel.promiseId);
            completeUnitSuccess(cancel.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(cancel.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(cancel.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                cancel.completion,
                "unknown exception in Capnp.KjAsync datagram receive promise cancel");
          }
        } else if (std::holds_alternative<QueuedDatagramReceivePromiseRelease>(op)) {
          auto release = std::get<QueuedDatagramReceivePromiseRelease>(std::move(op));
          try {
            releaseDatagramReceivePromise(release.promiseId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                release.completion,
                "unknown exception in Capnp.KjAsync datagram receive promise release");
          }
        } else if (std::holds_alternative<QueuedEnableTls>(op)) {
          auto enable = std::get<QueuedEnableTls>(std::move(op));
          try {
            enableTls(*io.provider);
            completeUnitSuccess(enable.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(enable.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(enable.completion, e.what());
          } catch (...) {
            completeUnitFailure(enable.completion,
                                "unknown exception in Capnp.KjAsync enableTls");
          }
        } else if (std::holds_alternative<QueuedConfigureTls>(op)) {
          auto configure = std::get<QueuedConfigureTls>(std::move(op));
          try {
            configureTls(*io.provider, configure.useSystemTrustStore != 0,
                         configure.verifyClients != 0, configure.minVersionTag,
                         configure.trustedCertificatesPem, configure.certificateChainPem,
                         configure.privateKeyPem, configure.cipherList, configure.curveList,
                         configure.acceptTimeoutNanos);
            completeUnitSuccess(configure.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(configure.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(configure.completion, e.what());
          } catch (...) {
            completeUnitFailure(configure.completion,
                                "unknown exception in Capnp.KjAsync configureTls");
          }
        } else if (std::holds_alternative<QueuedHttpRequest>(op)) {
          auto request = std::get<QueuedHttpRequest>(std::move(op));
          try {
            auto requestHeaders = decodeHeaderPairs(request.requestHeaders);
            auto result =
                httpRequest(*io.provider, io.waitScope, request.method, request.address,
                            request.portHint, request.path, std::move(requestHeaders),
                            kj::mv(request.body), request.useTls, request.responseBodyLimit);
            completeHttpResponseSuccess(request.completion, result.statusCode,
                                        std::move(result.statusText), std::move(result.headers),
                                        std::move(result.body));
          } catch (const kj::Exception& e) {
            completeHttpResponseFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHttpResponseFailure(request.completion, e.what());
          } catch (...) {
            completeHttpResponseFailure(request.completion,
                                        "unknown exception in Capnp.KjAsync httpRequest");
          }
        } else if (std::holds_alternative<QueuedHttpRequestStart>(op)) {
          auto request = std::get<QueuedHttpRequestStart>(std::move(op));
          try {
            auto requestHeaders = decodeHeaderPairs(request.requestHeaders);
            auto promiseId =
                httpRequestStart(*io.provider, request.method, request.address, request.portHint,
                                 request.path, std::move(requestHeaders), kj::mv(request.body),
                                 request.useTls, request.responseBodyLimit);
            completePromiseIdSuccess(request.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(request.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(request.completion,
                                     "unknown exception in Capnp.KjAsync httpRequestStart");
          }
        } else if (std::holds_alternative<QueuedHttpRequestStartStreaming>(op)) {
          auto request = std::get<QueuedHttpRequestStartStreaming>(std::move(op));
          try {
            auto requestHeaders = decodeHeaderPairs(request.requestHeaders);
            auto pair =
                httpRequestStartStreaming(*io.provider, request.method, request.address,
                                          request.portHint, request.path,
                                          std::move(requestHeaders), request.useTls);
            completeHandlePairSuccess(request.completion, pair.first, pair.second);
          } catch (const kj::Exception& e) {
            completeHandlePairFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandlePairFailure(request.completion, e.what());
          } catch (...) {
            completeHandlePairFailure(
                request.completion,
                "unknown exception in Capnp.KjAsync httpRequestStartStreaming");
          }
        } else if (std::holds_alternative<QueuedHttpResponsePromiseAwait>(op)) {
          auto await = std::get<QueuedHttpResponsePromiseAwait>(std::move(op));
          try {
            auto result = awaitHttpResponsePromise(io.waitScope, await.promiseId);
            completeHttpResponseSuccess(await.completion, result.statusCode,
                                        std::move(result.statusText), std::move(result.headers),
                                        std::move(result.body));
          } catch (const kj::Exception& e) {
            completeHttpResponseFailure(await.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHttpResponseFailure(await.completion, e.what());
          } catch (...) {
            completeHttpResponseFailure(
                await.completion, "unknown exception in Capnp.KjAsync HTTP response promise await");
          }
        } else if (std::holds_alternative<QueuedHttpResponsePromiseAwaitStreaming>(op)) {
          auto await = std::get<QueuedHttpResponsePromiseAwaitStreaming>(std::move(op));
          try {
            auto result = awaitHttpResponsePromise(io.waitScope, await.promiseId);
            completeHttpResponseSuccess(await.completion, result.statusCode,
                                        std::move(result.statusText), std::move(result.headers),
                                        std::move(result.body), result.bodyHandle);
          } catch (const kj::Exception& e) {
            completeHttpResponseFailure(await.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHttpResponseFailure(await.completion, e.what());
          } catch (...) {
            completeHttpResponseFailure(
                await.completion,
                "unknown exception in Capnp.KjAsync HTTP response streaming promise await");
          }
        } else if (std::holds_alternative<QueuedHttpResponsePromiseCancel>(op)) {
          auto cancel = std::get<QueuedHttpResponsePromiseCancel>(std::move(op));
          try {
            cancelHttpResponsePromise(cancel.promiseId);
            completeUnitSuccess(cancel.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(cancel.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(cancel.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                cancel.completion, "unknown exception in Capnp.KjAsync HTTP response promise cancel");
          }
        } else if (std::holds_alternative<QueuedHttpResponsePromiseRelease>(op)) {
          auto release = std::get<QueuedHttpResponsePromiseRelease>(std::move(op));
          try {
            releaseHttpResponsePromise(release.promiseId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(release.completion,
                                "unknown exception in Capnp.KjAsync HTTP response promise release");
          }
        } else if (std::holds_alternative<QueuedHttpRequestBodyWriteStart>(op)) {
          auto write = std::get<QueuedHttpRequestBodyWriteStart>(std::move(op));
          try {
            auto promiseId =
                httpRequestBodyWriteStart(write.requestBodyId, std::move(write.bytes));
            completePromiseIdSuccess(write.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(write.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(write.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(
                write.completion,
                "unknown exception in Capnp.KjAsync httpRequestBodyWriteStart");
          }
        } else if (std::holds_alternative<QueuedHttpRequestBodyWrite>(op)) {
          auto write = std::get<QueuedHttpRequestBodyWrite>(std::move(op));
          try {
            httpRequestBodyWrite(io.waitScope, write.requestBodyId, std::move(write.bytes));
            completeUnitSuccess(write.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(write.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(write.completion, e.what());
          } catch (...) {
            completeUnitFailure(write.completion,
                                "unknown exception in Capnp.KjAsync httpRequestBodyWrite");
          }
        } else if (std::holds_alternative<QueuedHttpRequestBodyFinishStart>(op)) {
          auto finish = std::get<QueuedHttpRequestBodyFinishStart>(std::move(op));
          try {
            auto promiseId = httpRequestBodyFinishStart(finish.requestBodyId);
            completePromiseIdSuccess(finish.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(finish.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(finish.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(
                finish.completion,
                "unknown exception in Capnp.KjAsync httpRequestBodyFinishStart");
          }
        } else if (std::holds_alternative<QueuedHttpRequestBodyFinish>(op)) {
          auto finish = std::get<QueuedHttpRequestBodyFinish>(std::move(op));
          try {
            httpRequestBodyFinish(io.waitScope, finish.requestBodyId);
            completeUnitSuccess(finish.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(finish.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(finish.completion, e.what());
          } catch (...) {
            completeUnitFailure(finish.completion,
                                "unknown exception in Capnp.KjAsync httpRequestBodyFinish");
          }
        } else if (std::holds_alternative<QueuedHttpRequestBodyRelease>(op)) {
          auto release = std::get<QueuedHttpRequestBodyRelease>(std::move(op));
          try {
            httpRequestBodyRelease(release.requestBodyId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(release.completion,
                                "unknown exception in Capnp.KjAsync httpRequestBodyRelease");
          }
        } else if (std::holds_alternative<QueuedHttpResponseBodyReadStart>(op)) {
          auto read = std::get<QueuedHttpResponseBodyReadStart>(std::move(op));
          try {
            auto promiseId = httpResponseBodyReadStart(read.responseBodyId, read.minBytes,
                                                       read.maxBytes);
            completePromiseIdSuccess(read.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(read.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(read.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(
                read.completion,
                "unknown exception in Capnp.KjAsync httpResponseBodyReadStart");
          }
        } else if (std::holds_alternative<QueuedHttpResponseBodyRead>(op)) {
          auto read = std::get<QueuedHttpResponseBodyRead>(std::move(op));
          try {
            auto bytes =
                httpResponseBodyRead(io.waitScope, read.responseBodyId, read.minBytes, read.maxBytes);
            completeBytesSuccess(read.completion, std::move(bytes));
          } catch (const kj::Exception& e) {
            completeBytesFailure(read.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeBytesFailure(read.completion, e.what());
          } catch (...) {
            completeBytesFailure(read.completion,
                                 "unknown exception in Capnp.KjAsync httpResponseBodyRead");
          }
        } else if (std::holds_alternative<QueuedHttpResponseBodyRelease>(op)) {
          auto release = std::get<QueuedHttpResponseBodyRelease>(std::move(op));
          try {
            httpResponseBodyRelease(release.responseBodyId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                release.completion,
                "unknown exception in Capnp.KjAsync httpResponseBodyRelease");
          }
        } else if (std::holds_alternative<QueuedHttpServerRequestBodyReadStart>(op)) {
          auto read = std::get<QueuedHttpServerRequestBodyReadStart>(std::move(op));
          try {
            auto promiseId =
                httpServerRequestBodyReadStart(read.requestBodyId, read.minBytes, read.maxBytes);
            completePromiseIdSuccess(read.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(read.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(read.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(
                read.completion,
                "unknown exception in Capnp.KjAsync httpServerRequestBodyReadStart");
          }
        } else if (std::holds_alternative<QueuedHttpServerRequestBodyRead>(op)) {
          auto read = std::get<QueuedHttpServerRequestBodyRead>(std::move(op));
          try {
            auto bytes = httpServerRequestBodyRead(io.waitScope, read.requestBodyId, read.minBytes,
                                                   read.maxBytes);
            completeBytesSuccess(read.completion, std::move(bytes));
          } catch (const kj::Exception& e) {
            completeBytesFailure(read.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeBytesFailure(read.completion, e.what());
          } catch (...) {
            completeBytesFailure(read.completion,
                                 "unknown exception in Capnp.KjAsync httpServerRequestBodyRead");
          }
        } else if (std::holds_alternative<QueuedHttpServerRequestBodyRelease>(op)) {
          auto release = std::get<QueuedHttpServerRequestBodyRelease>(std::move(op));
          try {
            httpServerRequestBodyRelease(release.requestBodyId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                release.completion,
                "unknown exception in Capnp.KjAsync httpServerRequestBodyRelease");
          }
        } else if (std::holds_alternative<QueuedWebSocketConnect>(op)) {
          auto connectReq = std::get<QueuedWebSocketConnect>(std::move(op));
          try {
            auto requestHeaders = decodeHeaderPairs(connectReq.requestHeaders);
            auto webSocketId = webSocketConnect(*io.provider, io.waitScope, connectReq.address,
                                                connectReq.portHint, connectReq.path,
                                                std::move(requestHeaders), connectReq.useTls);
            completeHandleSuccess(connectReq.completion, webSocketId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(connectReq.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(connectReq.completion, e.what());
          } catch (...) {
            completeHandleFailure(connectReq.completion,
                                  "unknown exception in Capnp.KjAsync websocketConnect");
          }
        } else if (std::holds_alternative<QueuedWebSocketConnectStart>(op)) {
          auto connectReq = std::get<QueuedWebSocketConnectStart>(std::move(op));
          try {
            auto requestHeaders = decodeHeaderPairs(connectReq.requestHeaders);
            auto promiseId = webSocketConnectStart(*io.provider, connectReq.address,
                                                   connectReq.portHint, connectReq.path,
                                                   std::move(requestHeaders), connectReq.useTls);
            completePromiseIdSuccess(connectReq.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(connectReq.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(connectReq.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(connectReq.completion,
                                     "unknown exception in Capnp.KjAsync websocketConnectStart");
          }
        } else if (std::holds_alternative<QueuedWebSocketPromiseAwait>(op)) {
          auto await = std::get<QueuedWebSocketPromiseAwait>(std::move(op));
          try {
            auto webSocketId = awaitWebSocketPromise(io.waitScope, await.promiseId);
            completeHandleSuccess(await.completion, webSocketId);
          } catch (const kj::Exception& e) {
            completeHandleFailure(await.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(await.completion, e.what());
          } catch (...) {
            completeHandleFailure(await.completion,
                                  "unknown exception in Capnp.KjAsync websocket promise await");
          }
        } else if (std::holds_alternative<QueuedWebSocketPromiseCancel>(op)) {
          auto cancel = std::get<QueuedWebSocketPromiseCancel>(std::move(op));
          try {
            cancelWebSocketPromise(cancel.promiseId);
            completeUnitSuccess(cancel.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(cancel.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(cancel.completion, e.what());
          } catch (...) {
            completeUnitFailure(cancel.completion,
                                "unknown exception in Capnp.KjAsync websocket promise cancel");
          }
        } else if (std::holds_alternative<QueuedWebSocketPromiseRelease>(op)) {
          auto release = std::get<QueuedWebSocketPromiseRelease>(std::move(op));
          try {
            releaseWebSocketPromise(release.promiseId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(release.completion,
                                "unknown exception in Capnp.KjAsync websocket promise release");
          }
        } else if (std::holds_alternative<QueuedWebSocketRelease>(op)) {
          auto release = std::get<QueuedWebSocketRelease>(std::move(op));
          try {
            webSocketRelease(release.webSocketId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(release.completion,
                                "unknown exception in Capnp.KjAsync websocketRelease");
          }
        } else if (std::holds_alternative<QueuedWebSocketSendTextStart>(op)) {
          auto send = std::get<QueuedWebSocketSendTextStart>(std::move(op));
          try {
            auto promiseId = webSocketSendTextStart(send.webSocketId, std::move(send.text));
            completePromiseIdSuccess(send.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(send.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(send.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(send.completion,
                                     "unknown exception in Capnp.KjAsync websocketSendTextStart");
          }
        } else if (std::holds_alternative<QueuedWebSocketSendText>(op)) {
          auto send = std::get<QueuedWebSocketSendText>(std::move(op));
          try {
            webSocketSendText(io.waitScope, send.webSocketId, send.text);
            completeUnitSuccess(send.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(send.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(send.completion, e.what());
          } catch (...) {
            completeUnitFailure(send.completion,
                                "unknown exception in Capnp.KjAsync websocketSendText");
          }
        } else if (std::holds_alternative<QueuedWebSocketSendBinaryStart>(op)) {
          auto send = std::get<QueuedWebSocketSendBinaryStart>(std::move(op));
          try {
            auto promiseId = webSocketSendBinaryStart(send.webSocketId, kj::mv(send.bytes));
            completePromiseIdSuccess(send.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(send.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(send.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(send.completion,
                                     "unknown exception in Capnp.KjAsync websocketSendBinaryStart");
          }
        } else if (std::holds_alternative<QueuedWebSocketSendBinary>(op)) {
          auto send = std::get<QueuedWebSocketSendBinary>(std::move(op));
          try {
            webSocketSendBinary(io.waitScope, send.webSocketId, kj::mv(send.bytes));
            completeUnitSuccess(send.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(send.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(send.completion, e.what());
          } catch (...) {
            completeUnitFailure(send.completion,
                                "unknown exception in Capnp.KjAsync websocketSendBinary");
          }
        } else if (std::holds_alternative<QueuedWebSocketReceiveStart>(op)) {
          auto recv = std::get<QueuedWebSocketReceiveStart>(std::move(op));
          try {
            auto promiseId = webSocketReceiveStart(recv.webSocketId, recv.maxBytes);
            completePromiseIdSuccess(recv.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(recv.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(recv.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(recv.completion,
                                     "unknown exception in Capnp.KjAsync websocketReceiveStart");
          }
        } else if (std::holds_alternative<QueuedWebSocketMessagePromiseAwait>(op)) {
          auto await = std::get<QueuedWebSocketMessagePromiseAwait>(std::move(op));
          try {
            auto message = awaitWebSocketMessagePromise(io.waitScope, await.promiseId);
            completeWebSocketMessageSuccess(await.completion, message.tag, message.closeCode,
                                            std::move(message.text), std::move(message.bytes));
          } catch (const kj::Exception& e) {
            completeWebSocketMessageFailure(await.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeWebSocketMessageFailure(await.completion, e.what());
          } catch (...) {
            completeWebSocketMessageFailure(
                await.completion,
                "unknown exception in Capnp.KjAsync websocket message promise await");
          }
        } else if (std::holds_alternative<QueuedWebSocketMessagePromiseCancel>(op)) {
          auto cancel = std::get<QueuedWebSocketMessagePromiseCancel>(std::move(op));
          try {
            cancelWebSocketMessagePromise(cancel.promiseId);
            completeUnitSuccess(cancel.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(cancel.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(cancel.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                cancel.completion,
                "unknown exception in Capnp.KjAsync websocket message promise cancel");
          }
        } else if (std::holds_alternative<QueuedWebSocketMessagePromiseRelease>(op)) {
          auto release = std::get<QueuedWebSocketMessagePromiseRelease>(std::move(op));
          try {
            releaseWebSocketMessagePromise(release.promiseId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                release.completion,
                "unknown exception in Capnp.KjAsync websocket message promise release");
          }
        } else if (std::holds_alternative<QueuedWebSocketReceive>(op)) {
          auto recv = std::get<QueuedWebSocketReceive>(std::move(op));
          try {
            auto message = webSocketReceive(io.waitScope, recv.webSocketId, recv.maxBytes);
            completeWebSocketMessageSuccess(recv.completion, message.tag, message.closeCode,
                                            std::move(message.text), std::move(message.bytes));
          } catch (const kj::Exception& e) {
            completeWebSocketMessageFailure(recv.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeWebSocketMessageFailure(recv.completion, e.what());
          } catch (...) {
            completeWebSocketMessageFailure(recv.completion,
                                            "unknown exception in Capnp.KjAsync websocketReceive");
          }
        } else if (std::holds_alternative<QueuedWebSocketCloseStart>(op)) {
          auto close = std::get<QueuedWebSocketCloseStart>(std::move(op));
          try {
            auto promiseId =
                webSocketCloseStart(close.webSocketId, close.closeCode, std::move(close.reason));
            completePromiseIdSuccess(close.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(close.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(close.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(close.completion,
                                     "unknown exception in Capnp.KjAsync websocketCloseStart");
          }
        } else if (std::holds_alternative<QueuedWebSocketClose>(op)) {
          auto close = std::get<QueuedWebSocketClose>(std::move(op));
          try {
            webSocketClose(io.waitScope, close.webSocketId, close.closeCode, close.reason);
            completeUnitSuccess(close.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(close.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(close.completion, e.what());
          } catch (...) {
            completeUnitFailure(close.completion,
                                "unknown exception in Capnp.KjAsync websocketClose");
          }
        } else if (std::holds_alternative<QueuedWebSocketDisconnect>(op)) {
          auto disconnect = std::get<QueuedWebSocketDisconnect>(std::move(op));
          try {
            webSocketDisconnect(disconnect.webSocketId);
            completeUnitSuccess(disconnect.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(disconnect.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(disconnect.completion, e.what());
          } catch (...) {
            completeUnitFailure(disconnect.completion,
                                "unknown exception in Capnp.KjAsync websocketDisconnect");
          }
        } else if (std::holds_alternative<QueuedWebSocketAbort>(op)) {
          auto abort = std::get<QueuedWebSocketAbort>(std::move(op));
          try {
            webSocketAbort(abort.webSocketId);
            completeUnitSuccess(abort.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(abort.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(abort.completion, e.what());
          } catch (...) {
            completeUnitFailure(abort.completion,
                                "unknown exception in Capnp.KjAsync websocketAbort");
          }
        } else if (std::holds_alternative<QueuedNewWebSocketPipe>(op)) {
          auto create = std::get<QueuedNewWebSocketPipe>(std::move(op));
          try {
            auto pair = newWebSocketPipe();
            completeHandlePairSuccess(create.completion, pair.first, pair.second);
          } catch (const kj::Exception& e) {
            completeHandlePairFailure(create.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandlePairFailure(create.completion, e.what());
          } catch (...) {
            completeHandlePairFailure(create.completion,
                                      "unknown exception in Capnp.KjAsync newWebSocketPipe");
          }
        } else if (std::holds_alternative<QueuedHttpServerListen>(op)) {
          auto listenReq = std::get<QueuedHttpServerListen>(std::move(op));
          try {
            uint32_t boundPort = 0;
            auto serverId = httpServerListen(*io.provider, io.waitScope, listenReq.address,
                                             listenReq.portHint, listenReq.useTls,
                                             listenReq.config, boundPort);
            completeHandlePairSuccess(listenReq.completion, serverId, boundPort);
          } catch (const kj::Exception& e) {
            completeHandlePairFailure(listenReq.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandlePairFailure(listenReq.completion, e.what());
          } catch (...) {
            completeHandlePairFailure(listenReq.completion,
                                      "unknown exception in Capnp.KjAsync http server listen");
          }
        } else if (std::holds_alternative<QueuedHttpServerRelease>(op)) {
          auto release = std::get<QueuedHttpServerRelease>(std::move(op));
          try {
            httpServerRelease(release.serverId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(release.completion,
                                "unknown exception in Capnp.KjAsync http server release");
          }
        } else if (std::holds_alternative<QueuedHttpServerDrainStart>(op)) {
          auto drain = std::get<QueuedHttpServerDrainStart>(std::move(op));
          try {
            auto promiseId = httpServerDrainStart(drain.serverId);
            completePromiseIdSuccess(drain.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(drain.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(drain.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(drain.completion,
                                     "unknown exception in Capnp.KjAsync http server drain start");
          }
        } else if (std::holds_alternative<QueuedHttpServerDrain>(op)) {
          auto drain = std::get<QueuedHttpServerDrain>(std::move(op));
          try {
            httpServerDrain(io.waitScope, drain.serverId);
            completeUnitSuccess(drain.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(drain.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(drain.completion, e.what());
          } catch (...) {
            completeUnitFailure(drain.completion,
                                "unknown exception in Capnp.KjAsync http server drain");
          }
        } else if (std::holds_alternative<QueuedHttpServerPollRequest>(op)) {
          auto poll = std::get<QueuedHttpServerPollRequest>(std::move(op));
          try {
            auto result = httpServerPollRequest(poll.serverId);
            completeHttpServerRequestSuccess(poll.completion, result.hasRequest,
                                             std::move(result.encodedRequest));
          } catch (const kj::Exception& e) {
            completeHttpServerRequestFailure(poll.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHttpServerRequestFailure(poll.completion, e.what());
          } catch (...) {
            completeHttpServerRequestFailure(
                poll.completion, "unknown exception in Capnp.KjAsync http server poll request");
          }
        } else if (std::holds_alternative<QueuedHttpServerRespond>(op)) {
          auto reply = std::get<QueuedHttpServerRespond>(std::move(op));
          try {
            auto responseHeaders = decodeHeaderPairs(reply.responseHeaders);
            httpServerRespond(reply.serverId, reply.requestId, reply.statusCode,
                              std::move(reply.statusText), std::move(responseHeaders),
                              kj::mv(reply.body));
            completeUnitSuccess(reply.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(reply.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(reply.completion, e.what());
          } catch (...) {
            completeUnitFailure(reply.completion,
                                "unknown exception in Capnp.KjAsync http server respond");
          }
        } else if (std::holds_alternative<QueuedHttpServerRespondWebSocket>(op)) {
          auto reply = std::get<QueuedHttpServerRespondWebSocket>(std::move(op));
          try {
            auto responseHeaders = decodeHeaderPairs(reply.responseHeaders);
            httpServerRespondWebSocket(reply.serverId, reply.requestId, std::move(responseHeaders),
                                       reply.completion);
            // WebSocket accept completion is produced by async HTTP service callbacks on this same
            // runtime thread; pump the KJ loop until that callback fires.
            while (true) {
              {
                std::unique_lock<std::mutex> lock(reply.completion->mutex);
                if (reply.completion->done) {
                  break;
                }
              }
              io.provider->getTimer().afterDelay(0 * kj::NANOSECONDS).wait(io.waitScope);
            }
          } catch (const kj::Exception& e) {
            completeHandleFailure(reply.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(reply.completion, e.what());
          } catch (...) {
            completeHandleFailure(
                reply.completion,
                "unknown exception in Capnp.KjAsync http server websocket respond");
          }
        } else if (std::holds_alternative<QueuedHttpServerRespondStartStreaming>(op)) {
          auto reply = std::get<QueuedHttpServerRespondStartStreaming>(std::move(op));
          try {
            auto responseHeaders = decodeHeaderPairs(reply.responseHeaders);
            httpServerRespondStartStreaming(reply.serverId, reply.requestId, reply.statusCode,
                                            std::move(reply.statusText),
                                            std::move(responseHeaders), reply.completion);
            while (true) {
              {
                std::unique_lock<std::mutex> lock(reply.completion->mutex);
                if (reply.completion->done) {
                  break;
                }
              }
              io.provider->getTimer().afterDelay(0 * kj::NANOSECONDS).wait(io.waitScope);
            }
          } catch (const kj::Exception& e) {
            completeHandleFailure(reply.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeHandleFailure(reply.completion, e.what());
          } catch (...) {
            completeHandleFailure(
                reply.completion,
                "unknown exception in Capnp.KjAsync http server respond start streaming");
          }
        } else if (std::holds_alternative<QueuedHttpServerResponseBodyWriteStart>(op)) {
          auto write = std::get<QueuedHttpServerResponseBodyWriteStart>(std::move(op));
          try {
            auto promiseId =
                httpServerResponseBodyWriteStart(write.responseBodyId, std::move(write.bytes));
            completePromiseIdSuccess(write.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(write.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(write.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(
                write.completion,
                "unknown exception in Capnp.KjAsync httpServerResponseBodyWriteStart");
          }
        } else if (std::holds_alternative<QueuedHttpServerResponseBodyWrite>(op)) {
          auto write = std::get<QueuedHttpServerResponseBodyWrite>(std::move(op));
          try {
            httpServerResponseBodyWrite(io.waitScope, write.responseBodyId, std::move(write.bytes));
            completeUnitSuccess(write.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(write.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(write.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                write.completion,
                "unknown exception in Capnp.KjAsync httpServerResponseBodyWrite");
          }
        } else if (std::holds_alternative<QueuedHttpServerResponseBodyFinishStart>(op)) {
          auto finish = std::get<QueuedHttpServerResponseBodyFinishStart>(std::move(op));
          try {
            auto promiseId = httpServerResponseBodyFinishStart(finish.responseBodyId);
            completePromiseIdSuccess(finish.completion, promiseId);
          } catch (const kj::Exception& e) {
            completePromiseIdFailure(finish.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completePromiseIdFailure(finish.completion, e.what());
          } catch (...) {
            completePromiseIdFailure(
                finish.completion,
                "unknown exception in Capnp.KjAsync httpServerResponseBodyFinishStart");
          }
        } else if (std::holds_alternative<QueuedHttpServerResponseBodyFinish>(op)) {
          auto finish = std::get<QueuedHttpServerResponseBodyFinish>(std::move(op));
          try {
            httpServerResponseBodyFinish(io.waitScope, finish.responseBodyId);
            completeUnitSuccess(finish.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(finish.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(finish.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                finish.completion,
                "unknown exception in Capnp.KjAsync httpServerResponseBodyFinish");
          }
        } else if (std::holds_alternative<QueuedHttpServerResponseBodyRelease>(op)) {
          auto release = std::get<QueuedHttpServerResponseBodyRelease>(std::move(op));
          try {
            httpServerResponseBodyRelease(release.responseBodyId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                release.completion,
                "unknown exception in Capnp.KjAsync httpServerResponseBodyRelease");
          }
        }
      }

      std::vector<uint32_t> httpServerIds;
      httpServerIds.reserve(httpServers_.size());
      for (const auto& entry : httpServers_) {
        httpServerIds.push_back(entry.first);
      }
      for (auto serverId : httpServerIds) {
        try {
          httpServerRelease(serverId);
        } catch (...) {
          // Best-effort during shutdown.
        }
      }

      promises_.clear();
      retiredPromiseIds_.clear();
      connectionPromises_.clear();
      bytesPromises_.clear();
      uint32Promises_.clear();
      datagramReceivePromises_.clear();
      httpResponsePromises_.clear();
      webSocketPromises_.clear();
      webSocketMessagePromises_.clear();
      listeners_.clear();
      networkAddresses_.clear();
      connections_.clear();
      taskSets_.clear();
      datagramPorts_.clear();
      for (auto& entry : webSockets_) {
        KJ_IF_SOME(fulfiller, entry.second.requestCompletion) {
          fulfiller->reject(kj::Exception(kj::Exception::Type::DISCONNECTED, __FILE__, __LINE__,
                                          kj::str("Capnp.KjAsync websocket released")));
        }
      }
      webSockets_.clear();
      httpRequestBodies_.clear();
      httpResponseBodies_.clear();
      httpServerRequestBodies_.clear();
      for (auto& entry : httpServerResponseBodies_) {
        entry.second.stream = nullptr;
        entry.second.doneFulfiller->reject(
            kj::Exception(kj::Exception::Type::DISCONNECTED, __FILE__, __LINE__,
                          kj::str("Capnp.KjAsync runtime released")));
      }
      httpServerResponseBodies_.clear();
      tlsNetwork_ = kj::none;
      tlsContext_ = kj::none;
    } catch (const kj::Exception& e) {
      {
        std::lock_guard<std::mutex> lock(startupMutex_);
        startupError_ = describeKjException(e);
        startupComplete_ = true;
      }
      startupCv_.notify_one();
      failOutstanding(startupError_);
    } catch (const std::exception& e) {
      {
        std::lock_guard<std::mutex> lock(startupMutex_);
        startupError_ = e.what();
        startupComplete_ = true;
      }
      startupCv_.notify_one();
      failOutstanding(startupError_);
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(startupMutex_);
        startupError_ = "unknown exception in Capnp.KjAsync runtime thread";
        startupComplete_ = true;
      }
      startupCv_.notify_one();
      failOutstanding(startupError_);
    }

    alive_.store(false, std::memory_order_release);
  }

  std::thread worker_;
  std::mutex startupMutex_;
  std::condition_variable startupCv_;
  bool startupComplete_ = false;
  std::string startupError_;

  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::deque<QueuedOperation> queue_;
  bool stopping_ = false;

  std::atomic<bool> alive_{false};
  uint32_t nextPromiseId_ = 1;
  std::unordered_map<uint32_t, PendingPromise> promises_;
  std::unordered_set<uint32_t> retiredPromiseIds_;
  uint32_t nextConnectionPromiseId_ = 1;
  std::unordered_map<uint32_t, PendingConnectionPromise> connectionPromises_;
  uint32_t nextBytesPromiseId_ = 1;
  std::unordered_map<uint32_t, PendingBytesPromise> bytesPromises_;
  uint32_t nextUInt32PromiseId_ = 1;
  std::unordered_map<uint32_t, PendingUInt32Promise> uint32Promises_;
  uint32_t nextDatagramReceivePromiseId_ = 1;
  std::unordered_map<uint32_t, PendingDatagramReceivePromise> datagramReceivePromises_;
  uint32_t nextHttpResponsePromiseId_ = 1;
  std::unordered_map<uint32_t, PendingHttpResponsePromise> httpResponsePromises_;
  uint32_t nextWebSocketPromiseId_ = 1;
  std::unordered_map<uint32_t, PendingWebSocketPromise> webSocketPromises_;
  uint32_t nextWebSocketMessagePromiseId_ = 1;
  std::unordered_map<uint32_t, PendingWebSocketMessagePromise> webSocketMessagePromises_;
  uint32_t nextListenerId_ = 1;
  std::unordered_map<uint32_t, kj::Own<kj::ConnectionReceiver>> listeners_;
  uint32_t nextNetworkAddressId_ = 1;
  std::unordered_map<uint32_t, kj::Own<kj::NetworkAddress>> networkAddresses_;
  uint32_t nextConnectionId_ = 1;
  std::unordered_map<uint32_t, kj::Own<kj::AsyncIoStream>> connections_;
  uint32_t nextTaskSetId_ = 1;
  std::unordered_map<uint32_t, kj::Own<RuntimeTaskSet>> taskSets_;
  uint32_t nextDatagramPortId_ = 1;
  std::unordered_map<uint32_t, kj::Own<kj::DatagramPort>> datagramPorts_;
  uint32_t nextWebSocketId_ = 1;
  std::unordered_map<uint32_t, RuntimeWebSocket> webSockets_;
  uint32_t nextHttpRequestBodyId_ = 1;
  std::unordered_map<uint32_t, RuntimeHttpRequestBody> httpRequestBodies_;
  uint32_t nextHttpResponseBodyId_ = 1;
  std::unordered_map<uint32_t, RuntimeHttpResponseBody> httpResponseBodies_;
  uint32_t nextHttpServerRequestBodyId_ = 1;
  std::unordered_map<uint32_t, RuntimeHttpServerRequestBody> httpServerRequestBodies_;
  uint32_t nextHttpServerResponseBodyId_ = 1;
  std::unordered_map<uint32_t, RuntimeHttpServerResponseBody> httpServerResponseBodies_;
  uint32_t nextHttpServerId_ = 1;
  std::unordered_map<uint32_t, kj::Own<HttpServerState>> httpServers_;
  kj::Maybe<kj::Own<kj::TlsContext>> tlsContext_;
  kj::Maybe<kj::Own<kj::Network>> tlsNetwork_;
};

std::mutex gKjAsyncRuntimeRegistryMutex;
std::unordered_map<uint64_t, std::shared_ptr<KjAsyncRuntimeLoop>> gKjAsyncRuntimes;

kj::Promise<void> KjAsyncRuntimeLoop::RuntimeHttpService::request(
    kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
    kj::AsyncInputStream& requestBody, Response& response) {
  auto methodTag = KjAsyncRuntimeLoop::encodeHttpMethodTag(method);
  auto path = std::string(url.cStr());
  auto headerPairs = captureHeaders(headers);
  auto webSocketRequested = headers.isWebSocket();
  HttpServerRequestRecord requestRecord;
  requestRecord.requestId = state_.nextRequestId++;
  requestRecord.methodTag = methodTag;
  requestRecord.webSocketRequested = webSocketRequested;
  requestRecord.path = std::move(path);
  requestRecord.headers = std::move(headerPairs);
  requestRecord.bodyHandle =
      runtime_.addHttpServerRequestBody(requestBody, state_.serverId, requestRecord.requestId);

  auto requestId = requestRecord.requestId;
  auto cleanup = kj::defer([this, requestId]() {
    runtime_.releaseHttpServerRequestBodiesForRequest(state_.serverId, requestId);
    runtime_.releaseHttpServerResponseBodiesForRequest(state_.serverId, requestId);
  });

  auto paf = kj::newPromiseAndFulfiller<HttpServerResponseCommand>();
  state_.pendingResponses.emplace(requestId, kj::mv(paf.fulfiller));
  state_.requestQueue.emplace_back(std::move(requestRecord));

  auto responsePromise =
      paf.promise.then([this, requestId, &response](HttpServerResponseCommand&& command) mutable
                           -> kj::Promise<void> {
        if (command.kind == HttpServerResponseCommand::Kind::WEBSOCKET) {
          try {
            kj::HttpHeaders responseHeaders(*state_.headerTable);
            applyHeadersFromPairs(responseHeaders, command.headers);
            auto socket = response.acceptWebSocket(responseHeaders);
            auto requestDone = kj::newPromiseAndFulfiller<void>();
            auto webSocketId =
                runtime_.addWebSocket(kj::mv(socket), kj::none, kj::mv(requestDone.fulfiller));
            if (command.webSocketCompletion) {
              completeHandleSuccess(command.webSocketCompletion, webSocketId);
            }
            return kj::mv(requestDone.promise);
          } catch (const kj::Exception& e) {
            if (command.webSocketCompletion) {
              completeHandleFailure(command.webSocketCompletion, describeKjException(e));
            }
            throw;
          } catch (const std::exception& e) {
            if (command.webSocketCompletion) {
              completeHandleFailure(command.webSocketCompletion, e.what());
            }
            throw;
          } catch (...) {
            if (command.webSocketCompletion) {
              completeHandleFailure(command.webSocketCompletion,
                                    "unknown exception in websocket accept");
            }
            throw;
          }
        }

        if (command.kind == HttpServerResponseCommand::Kind::HTTP_STREAMING) {
          try {
            kj::HttpHeaders responseHeaders(*state_.headerTable);
            applyHeadersFromPairs(responseHeaders, command.headers);
            auto output =
                response.send(command.statusCode, command.statusText.c_str(), responseHeaders,
                              kj::none);
            auto done = kj::newPromiseAndFulfiller<void>();
            auto responseBodyId = runtime_.addHttpServerResponseBody(
                kj::mv(output), kj::mv(done.fulfiller), state_.serverId, requestId);
            if (command.streamingCompletion) {
              completeHandleSuccess(command.streamingCompletion, responseBodyId);
            }
            return kj::mv(done.promise);
          } catch (const kj::Exception& e) {
            if (command.streamingCompletion) {
              completeHandleFailure(command.streamingCompletion, describeKjException(e));
            }
            throw;
          } catch (const std::exception& e) {
            if (command.streamingCompletion) {
              completeHandleFailure(command.streamingCompletion, e.what());
            }
            throw;
          } catch (...) {
            if (command.streamingCompletion) {
              completeHandleFailure(command.streamingCompletion,
                                    "unknown exception in http streaming respond");
            }
            throw;
          }
        }

        kj::HttpHeaders responseHeaders(*state_.headerTable);
        applyHeadersFromPairs(responseHeaders, command.headers);
        const size_t bodySize = (command.body ? command.body->size() : 0);
        auto output =
            response.send(command.statusCode, command.statusText.c_str(), responseHeaders,
                          kj::Maybe<uint64_t>(static_cast<uint64_t>(bodySize)));
        if (bodySize == 0) {
          return kj::READY_NOW;
        }
        auto bodyPtr = kj::ArrayPtr<const kj::byte>(
            reinterpret_cast<const kj::byte*>(command.body->data()), bodySize);
        return output->write(bodyPtr).attach(kj::mv(command.body), kj::mv(output));
      });
  return kj::mv(responsePromise).attach(kj::mv(cleanup));
}

uint64_t allocateKjAsyncRuntimeIdLocked() {
  while (true) {
    uint64_t id =
        capnp_lean_rpc::gNextRuntimeId.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) {
      continue;
    }
    if (gKjAsyncRuntimes.find(id) == gKjAsyncRuntimes.end()) {
      return id;
    }
  }
}

std::shared_ptr<KjAsyncRuntimeLoop> getKjAsyncRuntime(uint64_t runtimeId) {
  std::lock_guard<std::mutex> lock(gKjAsyncRuntimeRegistryMutex);
  auto it = gKjAsyncRuntimes.find(runtimeId);
  if (it == gKjAsyncRuntimes.end()) {
    return nullptr;
  }
  return it->second;
}

std::shared_ptr<KjAsyncRuntimeLoop> unregisterKjAsyncRuntime(uint64_t runtimeId) {
  std::lock_guard<std::mutex> lock(gKjAsyncRuntimeRegistryMutex);
  auto it = gKjAsyncRuntimes.find(runtimeId);
  if (it == gKjAsyncRuntimes.end()) {
    return nullptr;
  }
  auto result = std::move(it->second);
  gKjAsyncRuntimes.erase(runtimeId);
  return result;
}

bool isKjAsyncRuntimeAlive(uint64_t runtimeId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (runtime && runtime->isAlive()) {
    return true;
  }
  return capnp_lean_rpc::isRuntimeAlive(runtimeId);
}

}  // namespace

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_new() {
  try {
    auto runtime = std::make_shared<KjAsyncRuntimeLoop>();

    uint64_t runtimeId;
    {
      std::lock_guard<std::mutex> lock(gKjAsyncRuntimeRegistryMutex);
      runtimeId = allocateKjAsyncRuntimeIdLocked();
      gKjAsyncRuntimes.emplace(runtimeId, runtime);
    }

    return lean_io_result_mk_ok(lean_box_uint64(runtimeId));
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_new");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_release(uint64_t runtimeId) {
  try {
    auto runtime = unregisterKjAsyncRuntime(runtimeId);
    if (runtime) {
      runtime->shutdown();
    } else {
      auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId);
      if (rpcRuntime && capnp_lean_rpc::isWorkerThread(*rpcRuntime)) {
        return mkIoUserError(
            "Capnp.KjAsync runtime shutdown is not allowed from the Capnp.Rpc worker thread");
      }
      auto unregisteredRpcRuntime = capnp_lean_rpc::unregisterRuntime(runtimeId);
      if (unregisteredRpcRuntime) {
        capnp_lean_rpc::shutdown(*unregisteredRpcRuntime);
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_is_alive(uint64_t runtimeId) {
  return lean_io_result_mk_ok(lean_box(isKjAsyncRuntimeAlive(runtimeId) ? 1 : 0));
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_enable_tls(uint64_t runtimeId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueEnableTls();
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_enable_tls");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_configure_tls(
    uint64_t runtimeId, uint32_t useSystemTrustStore, uint32_t verifyClients,
    uint32_t minVersionTag, b_lean_obj_arg trustedCertificatesPem,
    b_lean_obj_arg certificateChainPem, b_lean_obj_arg privateKeyPem,
    b_lean_obj_arg cipherList, b_lean_obj_arg curveList, uint64_t acceptTimeoutNanos) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueConfigureTls(
        useSystemTrustStore, verifyClients, minVersionTag,
        std::string(lean_string_cstr(trustedCertificatesPem)),
        std::string(lean_string_cstr(certificateChainPem)),
        std::string(lean_string_cstr(privateKeyPem)),
        std::string(lean_string_cstr(cipherList)),
        std::string(lean_string_cstr(curveList)), acceptTimeoutNanos);
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_configure_tls");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_sleep_nanos_start(
    uint64_t runtimeId, uint64_t delayNanos) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueSleepNanos(delayNanos);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
      }
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      if (capnp_lean_rpc::isWorkerThread(*rpcRuntime)) {
        auto promiseId = capnp_lean_rpc::kjAsyncSleepNanosStartInline(*rpcRuntime, delayNanos);
        return lean_io_result_mk_ok(lean_box_uint32(promiseId));
      }

      auto completion =
          capnp_lean_rpc::enqueueKjAsyncSleepNanosStart(*rpcRuntime, delayNanos);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
      }
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_sleep_nanos_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_promise_await(
    uint64_t runtimeId, uint32_t promiseId) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueAwaitPromise(promiseId);
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
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      if (capnp_lean_rpc::isWorkerThread(*rpcRuntime)) {
        return mkIoUserError(
            "Capnp.KjAsync promise await is not allowed from the Capnp.Rpc worker thread");
      }
      auto completion = capnp_lean_rpc::enqueueKjAsyncPromiseAwait(*rpcRuntime, promiseId);
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
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_promise_await");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_promise_cancel(
    uint64_t runtimeId, uint32_t promiseId) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueCancelPromise(promiseId);
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
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      if (capnp_lean_rpc::isWorkerThread(*rpcRuntime)) {
        capnp_lean_rpc::kjAsyncPromiseCancelInline(*rpcRuntime, promiseId);
      } else {
        auto completion = capnp_lean_rpc::enqueueKjAsyncPromiseCancel(*rpcRuntime, promiseId);
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
      }
      lean_obj_res ok;
      mkIoOkUnit(ok);
      return ok;
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_promise_cancel");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_promise_release(
    uint64_t runtimeId, uint32_t promiseId) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueReleasePromise(promiseId);
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
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      if (capnp_lean_rpc::isWorkerThread(*rpcRuntime)) {
        capnp_lean_rpc::kjAsyncPromiseReleaseInline(*rpcRuntime, promiseId);
      } else {
        auto completion = capnp_lean_rpc::enqueueKjAsyncPromiseRelease(*rpcRuntime, promiseId);
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
      }
      lean_obj_res ok;
      mkIoOkUnit(ok);
      return ok;
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_promise_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_listen(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueListen(std::string(lean_string_cstr(address)), portHint);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_listen");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_release_listener(
    uint64_t runtimeId, uint32_t listenerId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueReleaseListener(listenerId);
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_release_listener");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_listener_accept(
    uint64_t runtimeId, uint32_t listenerId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueAccept(listenerId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_listener_accept");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_listener_accept_start(
    uint64_t runtimeId, uint32_t listenerId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueAcceptStart(listenerId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_listener_accept_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_connect(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueConnect(std::string(lean_string_cstr(address)), portHint);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_connect");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_connect_start(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueConnectStart(std::string(lean_string_cstr(address)), portHint);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_connect_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_parse_address(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion =
        runtime->enqueueParseAddress(std::string(lean_string_cstr(address)), portHint);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_parse_address");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_release_network_address(
    uint64_t runtimeId, uint32_t addressId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueReleaseNetworkAddress(addressId);
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_release_network_address");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_network_address_to_string(
    uint64_t runtimeId, uint32_t addressId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueNetworkAddressToString(addressId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box(completion->hasValue ? 1 : 0));
      lean_ctor_set(pair, 1, lean_mk_string(completion->value.c_str()));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_network_address_to_string");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_network_address_clone(
    uint64_t runtimeId, uint32_t addressId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueNetworkAddressClone(addressId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_network_address_clone");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_network_address_connect(
    uint64_t runtimeId, uint32_t addressId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueNetworkAddressConnect(addressId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_network_address_connect");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_network_address_connect_start(
    uint64_t runtimeId, uint32_t addressId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueNetworkAddressConnectStart(addressId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_network_address_connect_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_network_address_listen(
    uint64_t runtimeId, uint32_t addressId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueNetworkAddressListen(addressId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_network_address_listen");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_network_address_bind_datagram_port(uint64_t runtimeId,
                                                               uint32_t addressId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueNetworkAddressBindDatagramPort(addressId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_network_address_bind_datagram_port");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_connection_promise_await(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueAwaitConnectionPromise(promiseId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_connection_promise_await");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_connection_promise_await_with_timeout(uint64_t runtimeId,
                                                                  uint32_t promiseId,
                                                                  uint64_t timeoutNanos) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion =
        runtime->enqueueAwaitConnectionPromiseWithTimeout(promiseId, timeoutNanos);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box(completion->hasValue ? 1 : 0));
      lean_ctor_set(pair, 1, lean_box_uint32(completion->value));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_connection_promise_await_with_timeout");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_connection_promise_cancel(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueCancelConnectionPromise(promiseId);
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_connection_promise_cancel");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_connection_promise_release(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueReleaseConnectionPromise(promiseId);
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_connection_promise_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_release_connection(
    uint64_t runtimeId, uint32_t connectionId) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueReleaseConnection(connectionId);
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
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      auto completion = capnp_lean_rpc::enqueueReleaseTransport(*rpcRuntime, connectionId);
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
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_release_connection");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_bytes_ref_of_byte_array(
    b_lean_obj_arg bytes) {
  try {
    return lean_io_result_mk_ok(mkBytesRef(copyByteArrayToSharedBytes(bytes)));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_bytes_ref_of_byte_array");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_bytes_ref_to_byte_array(
    b_lean_obj_arg bytesRef) {
  try {
    auto bytes = getBytesRefDataOrThrow(bytesRef);
    return lean_io_result_mk_ok(mkByteArrayCopy(bytes->data(), bytes->size()));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_bytes_ref_to_byte_array");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_bytes_ref_size(
    b_lean_obj_arg bytesRef) {
  try {
    auto bytes = getBytesRefDataOrThrow(bytesRef);
    return lean_io_result_mk_ok(lean_box_uint64(static_cast<uint64_t>(bytes->size())));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_bytes_ref_size");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_connection_write_ref(
    uint64_t runtimeId, uint32_t connectionId, b_lean_obj_arg bytesRef) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion =
        runtime->enqueueConnectionWrite(connectionId, getBytesRefDataOrThrow(bytesRef));
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_connection_write_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_connection_write_start_ref(
    uint64_t runtimeId, uint32_t connectionId, b_lean_obj_arg bytesRef) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueConnectionWriteStart(connectionId,
                                                           getBytesRefDataOrThrow(bytesRef));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_connection_write_start_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_connection_read_ref(
    uint64_t runtimeId, uint32_t connectionId, uint32_t minBytes, uint32_t maxBytes) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueConnectionRead(connectionId, minBytes, maxBytes);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto bytes = completion->bytes ? completion->bytes : std::make_shared<std::vector<uint8_t>>();
      return lean_io_result_mk_ok(mkBytesRef(std::move(bytes)));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_connection_read_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_connection_read_start(
    uint64_t runtimeId, uint32_t connectionId, uint32_t minBytes, uint32_t maxBytes) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueConnectionReadStart(connectionId, minBytes, maxBytes);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_connection_read_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_bytes_promise_await_ref(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueBytesPromiseAwait(promiseId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto bytes = completion->bytes ? completion->bytes : std::make_shared<std::vector<uint8_t>>();
      return lean_io_result_mk_ok(mkBytesRef(std::move(bytes)));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_bytes_promise_await_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_bytes_promise_cancel(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueBytesPromiseCancel(promiseId);
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_bytes_promise_cancel");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_bytes_promise_release(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueBytesPromiseRelease(promiseId);
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_bytes_promise_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_connection_shutdown_write(
    uint64_t runtimeId, uint32_t connectionId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueConnectionShutdownWrite(connectionId);
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_connection_shutdown_write");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_connection_shutdown_write_start(
    uint64_t runtimeId, uint32_t connectionId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueConnectionShutdownWriteStart(connectionId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_connection_shutdown_write_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_promise_then_start(
    uint64_t runtimeId, uint32_t firstPromiseId, uint32_t secondPromiseId) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueuePromiseThenStart(firstPromiseId, secondPromiseId);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
      }
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      if (capnp_lean_rpc::isWorkerThread(*rpcRuntime)) {
        auto promiseId = capnp_lean_rpc::kjAsyncPromiseThenStartInline(
            *rpcRuntime, firstPromiseId, secondPromiseId);
        return lean_io_result_mk_ok(lean_box_uint32(promiseId));
      }
      auto completion = capnp_lean_rpc::enqueueKjAsyncPromiseThenStart(
          *rpcRuntime, firstPromiseId, secondPromiseId);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
      }
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_promise_then_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_promise_catch_start(
    uint64_t runtimeId, uint32_t promiseId, uint32_t fallbackPromiseId) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueuePromiseCatchStart(promiseId, fallbackPromiseId);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
      }
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      if (capnp_lean_rpc::isWorkerThread(*rpcRuntime)) {
        auto outPromiseId = capnp_lean_rpc::kjAsyncPromiseCatchStartInline(
            *rpcRuntime, promiseId, fallbackPromiseId);
        return lean_io_result_mk_ok(lean_box_uint32(outPromiseId));
      }
      auto completion = capnp_lean_rpc::enqueueKjAsyncPromiseCatchStart(
          *rpcRuntime, promiseId, fallbackPromiseId);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
      }
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_promise_catch_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_promise_all_start(
    uint64_t runtimeId, b_lean_obj_arg promiseIds) {
  try {
    auto ids = decodeUint32Array(promiseIds);
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueuePromiseAllStart(ids);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
      }
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      if (capnp_lean_rpc::isWorkerThread(*rpcRuntime)) {
        auto promiseId = capnp_lean_rpc::kjAsyncPromiseAllStartInline(*rpcRuntime, ids);
        return lean_io_result_mk_ok(lean_box_uint32(promiseId));
      }
      auto completion = capnp_lean_rpc::enqueueKjAsyncPromiseAllStart(*rpcRuntime, std::move(ids));
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
      }
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_promise_all_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_promise_race_start(
    uint64_t runtimeId, b_lean_obj_arg promiseIds) {
  try {
    auto ids = decodeUint32Array(promiseIds);
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueuePromiseRaceStart(ids);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
      }
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      if (capnp_lean_rpc::isWorkerThread(*rpcRuntime)) {
        auto promiseId = capnp_lean_rpc::kjAsyncPromiseRaceStartInline(*rpcRuntime, ids);
        return lean_io_result_mk_ok(lean_box_uint32(promiseId));
      }
      auto completion =
          capnp_lean_rpc::enqueueKjAsyncPromiseRaceStart(*rpcRuntime, std::move(ids));
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
      }
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_promise_race_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_task_set_new(uint64_t runtimeId) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueTaskSetNew();
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
      }
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      if (capnp_lean_rpc::isWorkerThread(*rpcRuntime)) {
        auto taskSetId = capnp_lean_rpc::kjAsyncTaskSetNewInline(*rpcRuntime);
        return lean_io_result_mk_ok(lean_box_uint32(taskSetId));
      }
      auto completion = capnp_lean_rpc::enqueueKjAsyncTaskSetNew(*rpcRuntime);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
      }
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_task_set_new");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_task_set_release(
    uint64_t runtimeId, uint32_t taskSetId) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueTaskSetRelease(taskSetId);
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
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      if (capnp_lean_rpc::isWorkerThread(*rpcRuntime)) {
        capnp_lean_rpc::kjAsyncTaskSetReleaseInline(*rpcRuntime, taskSetId);
      } else {
        auto completion = capnp_lean_rpc::enqueueKjAsyncTaskSetRelease(*rpcRuntime, taskSetId);
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
      }
      lean_obj_res ok;
      mkIoOkUnit(ok);
      return ok;
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_task_set_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_task_set_add_promise(
    uint64_t runtimeId, uint32_t taskSetId, uint32_t promiseId) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueTaskSetAddPromise(taskSetId, promiseId);
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
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      if (capnp_lean_rpc::isWorkerThread(*rpcRuntime)) {
        capnp_lean_rpc::kjAsyncTaskSetAddPromiseInline(*rpcRuntime, taskSetId, promiseId);
      } else {
        auto completion =
            capnp_lean_rpc::enqueueKjAsyncTaskSetAddPromise(*rpcRuntime, taskSetId, promiseId);
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
      }
      lean_obj_res ok;
      mkIoOkUnit(ok);
      return ok;
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_task_set_add_promise");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_task_set_clear(
    uint64_t runtimeId, uint32_t taskSetId) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueTaskSetClear(taskSetId);
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
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      if (capnp_lean_rpc::isWorkerThread(*rpcRuntime)) {
        capnp_lean_rpc::kjAsyncTaskSetClearInline(*rpcRuntime, taskSetId);
      } else {
        auto completion = capnp_lean_rpc::enqueueKjAsyncTaskSetClear(*rpcRuntime, taskSetId);
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
      }
      lean_obj_res ok;
      mkIoOkUnit(ok);
      return ok;
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_task_set_clear");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_task_set_is_empty(
    uint64_t runtimeId, uint32_t taskSetId) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueTaskSetIsEmpty(taskSetId);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box(completion->value ? 1 : 0));
      }
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      bool isEmpty;
      if (capnp_lean_rpc::isWorkerThread(*rpcRuntime)) {
        isEmpty = capnp_lean_rpc::kjAsyncTaskSetIsEmptyInline(*rpcRuntime, taskSetId);
      } else {
        auto completion = capnp_lean_rpc::enqueueKjAsyncTaskSetIsEmpty(*rpcRuntime, taskSetId);
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        isEmpty = completion->value;
      }
      return lean_io_result_mk_ok(lean_box(isEmpty ? 1 : 0));
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_task_set_is_empty");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_task_set_on_empty_start(
    uint64_t runtimeId, uint32_t taskSetId) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueTaskSetOnEmptyStart(taskSetId);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
      }
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      uint32_t promiseId;
      if (capnp_lean_rpc::isWorkerThread(*rpcRuntime)) {
        promiseId = capnp_lean_rpc::kjAsyncTaskSetOnEmptyStartInline(*rpcRuntime, taskSetId);
      } else {
        auto completion =
            capnp_lean_rpc::enqueueKjAsyncTaskSetOnEmptyStart(*rpcRuntime, taskSetId);
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        promiseId = completion->promiseId;
      }
      return lean_io_result_mk_ok(lean_box_uint32(promiseId));
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_task_set_on_empty_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_task_set_error_count(
    uint64_t runtimeId, uint32_t taskSetId) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueTaskSetErrorCount(taskSetId);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->value));
      }
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      uint32_t value;
      if (capnp_lean_rpc::isWorkerThread(*rpcRuntime)) {
        value = capnp_lean_rpc::kjAsyncTaskSetErrorCountInline(*rpcRuntime, taskSetId);
      } else {
        auto completion = capnp_lean_rpc::enqueueKjAsyncTaskSetErrorCount(*rpcRuntime, taskSetId);
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        value = static_cast<uint32_t>(completion->value);
      }
      return lean_io_result_mk_ok(lean_box_uint32(value));
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_task_set_error_count");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_task_set_take_last_error(
    uint64_t runtimeId, uint32_t taskSetId) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueTaskSetTakeLastError(taskSetId);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        auto pair = lean_alloc_ctor(0, 2, 0);
        lean_ctor_set(pair, 0, lean_box(completion->hasValue ? 1 : 0));
        lean_ctor_set(pair, 1, lean_mk_string(completion->value.c_str()));
        return lean_io_result_mk_ok(pair);
      }
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      bool hasValue = false;
      std::string value;
      if (capnp_lean_rpc::isWorkerThread(*rpcRuntime)) {
        auto result = capnp_lean_rpc::kjAsyncTaskSetTakeLastErrorInline(*rpcRuntime, taskSetId);
        hasValue = result.first;
        value = std::move(result.second);
      } else {
        auto completion =
            capnp_lean_rpc::enqueueKjAsyncTaskSetTakeLastError(*rpcRuntime, taskSetId);
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        hasValue = completion->hasValue;
        value = completion->value;
      }
      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box(hasValue ? 1 : 0));
      lean_ctor_set(pair, 1, lean_mk_string(value.c_str()));
      return lean_io_result_mk_ok(pair);
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_task_set_take_last_error");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_connection_when_write_disconnected_start(
    uint64_t runtimeId, uint32_t connectionId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueConnectionWhenWriteDisconnectedStart(connectionId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_connection_when_write_disconnected_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_connection_abort_read(
    uint64_t runtimeId, uint32_t connectionId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueConnectionAbortRead(connectionId);
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_connection_abort_read");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_connection_abort_write(
    uint64_t runtimeId, uint32_t connectionId, b_lean_obj_arg reason) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion =
        runtime->enqueueConnectionAbortWrite(connectionId, std::string(lean_string_cstr(reason)));
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_connection_abort_write");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_connection_dup_fd(
    uint64_t runtimeId, uint32_t connectionId) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueConnectionDupFd(connectionId);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        auto pair = lean_alloc_ctor(0, 2, 0);
        lean_ctor_set(pair, 0, lean_box(completion->hasValue ? 1 : 0));
        lean_ctor_set(pair, 1, lean_box_uint32(completion->value));
        return lean_io_result_mk_ok(pair);
      }
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      auto completion = capnp_lean_rpc::enqueueTransportGetFd(*rpcRuntime, connectionId);
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      bool hasValue = completion->value >= 0;
      uint32_t value = hasValue ? static_cast<uint32_t>(completion->value) : 0;
      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box(hasValue ? 1 : 0));
      lean_ctor_set(pair, 1, lean_box_uint32(value));
      return lean_io_result_mk_ok(pair);
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_connection_dup_fd");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_wrap_socket_fd(
    uint64_t runtimeId, uint32_t fd) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueWrapSocketFd(fd);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
      }
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      auto completion = capnp_lean_rpc::enqueueNewTransportFromFd(*rpcRuntime, fd);
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_wrap_socket_fd");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_wrap_socket_fd_take(
    uint64_t runtimeId, uint32_t fd) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueWrapSocketFdTake(fd);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
      }
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      auto completion = capnp_lean_rpc::enqueueNewTransportFromFdTake(*rpcRuntime, fd);
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->targetId));
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_wrap_socket_fd_take");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_wrap_listen_socket_fd(
    uint64_t runtimeId, uint32_t fd) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueWrapListenSocketFd(fd);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
      }
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_wrap_listen_socket_fd");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_wrap_listen_socket_fd_take(
    uint64_t runtimeId, uint32_t fd) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueWrapListenSocketFdTake(fd);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
      }
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_wrap_listen_socket_fd_take");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_wrap_datagram_socket_fd(
    uint64_t runtimeId, uint32_t fd) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueWrapDatagramSocketFd(fd);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
      }
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_wrap_datagram_socket_fd");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_wrap_datagram_socket_fd_take(
    uint64_t runtimeId, uint32_t fd) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueWrapDatagramSocketFdTake(fd);
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
      }
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_wrap_datagram_socket_fd_take");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_new_two_way_pipe(
    uint64_t runtimeId) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueNewTwoWayPipe();
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        auto pair = lean_alloc_ctor(0, 2, 0);
        lean_ctor_set(pair, 0, lean_box_uint32(completion->first));
        lean_ctor_set(pair, 1, lean_box_uint32(completion->second));
        return lean_io_result_mk_ok(pair);
      }
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      auto completion = capnp_lean_rpc::enqueueNewTransportPipe(*rpcRuntime);
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box_uint32(completion->first));
      lean_ctor_set(pair, 1, lean_box_uint32(completion->second));
      return lean_io_result_mk_ok(pair);
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_new_two_way_pipe");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_new_capability_pipe(
    uint64_t runtimeId) {
  try {
    if (auto runtime = getKjAsyncRuntime(runtimeId)) {
      auto completion = runtime->enqueueNewCapabilityPipe();
      {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&completion]() { return completion->done; });
        if (!completion->ok) {
          return mkIoUserError(completion->error);
        }
        auto pair = lean_alloc_ctor(0, 2, 0);
        lean_ctor_set(pair, 0, lean_box_uint32(completion->first));
        lean_ctor_set(pair, 1, lean_box_uint32(completion->second));
        return lean_io_result_mk_ok(pair);
      }
    }

    if (auto rpcRuntime = capnp_lean_rpc::getRuntime(runtimeId)) {
      auto completion = capnp_lean_rpc::enqueueNewTransportPipe(*rpcRuntime);
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box_uint32(completion->first));
      lean_ctor_set(pair, 1, lean_box_uint32(completion->second));
      return lean_io_result_mk_ok(pair);
    }

    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_new_capability_pipe");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_datagram_bind(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueDatagramBind(std::string(lean_string_cstr(address)), portHint);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_datagram_bind");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_datagram_release_port(
    uint64_t runtimeId, uint32_t portId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueDatagramReleasePort(portId);
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_datagram_release_port");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_datagram_get_port(
    uint64_t runtimeId, uint32_t portId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueDatagramGetPort(portId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_datagram_get_port");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_datagram_send_ref(
    uint64_t runtimeId, uint32_t portId, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg bytesRef) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion =
        runtime->enqueueDatagramSend(portId, std::string(lean_string_cstr(address)), portHint,
                                     getBytesRefDataOrThrow(bytesRef));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->value));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_datagram_send_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_datagram_send_start_ref(
    uint64_t runtimeId, uint32_t portId, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg bytesRef) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueDatagramSendStart(
        portId, std::string(lean_string_cstr(address)), portHint, getBytesRefDataOrThrow(bytesRef));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_datagram_send_start_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_uint32_promise_await(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueUInt32PromiseAwait(promiseId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->value));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_uint32_promise_await");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_uint32_promise_cancel(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueUInt32PromiseCancel(promiseId);
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_uint32_promise_cancel");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_uint32_promise_release(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueUInt32PromiseRelease(promiseId);
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_uint32_promise_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_datagram_receive_ref(
    uint64_t runtimeId, uint32_t portId, uint32_t maxBytes) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueDatagramReceive(portId, maxBytes);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_mk_string(completion->sourceAddress.c_str()));
      lean_ctor_set(pair, 1, mkBytesRef(makeSharedBytes(std::move(completion->bytes))));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_datagram_receive_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_datagram_receive_start(
    uint64_t runtimeId, uint32_t portId, uint32_t maxBytes) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueDatagramReceiveStart(portId, maxBytes);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_datagram_receive_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_datagram_receive_promise_await_ref(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueDatagramReceivePromiseAwait(promiseId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_mk_string(completion->sourceAddress.c_str()));
      lean_ctor_set(pair, 1, mkBytesRef(makeSharedBytes(std::move(completion->bytes))));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_datagram_receive_promise_await_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_datagram_receive_promise_cancel(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueDatagramReceivePromiseCancel(promiseId);
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_datagram_receive_promise_cancel");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_datagram_receive_promise_release(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueDatagramReceivePromiseRelease(promiseId);
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_datagram_receive_promise_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_request(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg body) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequest(
        method, std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        encodedEmptyHeaderPairs(),
        copyByteArrayToSharedBytes(body));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }

      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box_uint32(completion->statusCode));
      lean_ctor_set(pair, 1, mkByteArrayCopy(completion->body.data(), completion->body.size()));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_http_request");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_request_ref(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg bodyRef) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequest(
        method, std::string(lean_string_cstr(address)), portHint,
        std::string(lean_string_cstr(path)), encodedEmptyHeaderPairs(),
        getBytesRefDataOrThrow(bodyRef));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }

      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box_uint32(completion->statusCode));
      lean_ctor_set(pair, 1, mkBytesRef(makeSharedBytes(std::move(completion->body))));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_http_request_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_request_with_response_limit(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg body, uint64_t responseBodyLimit) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequest(
        method, std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        encodedEmptyHeaderPairs(),
        copyByteArrayToSharedBytes(body), false, responseBodyLimit);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }

      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box_uint32(completion->statusCode));
      lean_ctor_set(pair, 1, mkByteArrayCopy(completion->body.data(), completion->body.size()));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_with_response_limit");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_with_response_limit_ref(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg bodyRef, uint64_t responseBodyLimit) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequest(
        method, std::string(lean_string_cstr(address)), portHint,
        std::string(lean_string_cstr(path)), encodedEmptyHeaderPairs(),
        getBytesRefDataOrThrow(bodyRef), false, responseBodyLimit);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }

      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box_uint32(completion->statusCode));
      lean_ctor_set(pair, 1, mkBytesRef(makeSharedBytes(std::move(completion->body))));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_with_response_limit_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_request_start(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg body) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequestStart(
        method, std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        encodedEmptyHeaderPairs(),
        copyByteArrayToSharedBytes(body));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_http_request_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_request_start_ref(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg bodyRef) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequestStart(
        method, std::string(lean_string_cstr(address)), portHint,
        std::string(lean_string_cstr(path)), encodedEmptyHeaderPairs(),
        getBytesRefDataOrThrow(bodyRef));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_start_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_start_with_response_limit(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg body, uint64_t responseBodyLimit) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequestStart(
        method, std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        encodedEmptyHeaderPairs(),
        copyByteArrayToSharedBytes(body), false, responseBodyLimit);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_start_with_response_limit");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_start_with_response_limit_ref(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg bodyRef, uint64_t responseBodyLimit) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequestStart(
        method, std::string(lean_string_cstr(address)), portHint,
        std::string(lean_string_cstr(path)), encodedEmptyHeaderPairs(),
        getBytesRefDataOrThrow(bodyRef), false, responseBodyLimit);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_start_with_response_limit_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_response_promise_await(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpResponsePromiseAwait(promiseId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }

      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box_uint32(completion->statusCode));
      lean_ctor_set(pair, 1, mkByteArrayCopy(completion->body.data(), completion->body.size()));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_response_promise_await");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_response_promise_await_ref(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpResponsePromiseAwait(promiseId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }

      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box_uint32(completion->statusCode));
      lean_ctor_set(pair, 1, mkBytesRef(makeSharedBytes(std::move(completion->body))));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_response_promise_await_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_response_promise_cancel(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpResponsePromiseCancel(promiseId);
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_response_promise_cancel");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_response_promise_release(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpResponsePromiseRelease(promiseId);
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_response_promise_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_request_with_headers(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders, b_lean_obj_arg body) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequest(
        method, std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        copyByteArray(requestHeaders), copyByteArrayToSharedBytes(body));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }

      auto pairInner = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairInner, 0,
                    mkByteArrayCopy(completion->headers.data(), completion->headers.size()));
      lean_ctor_set(pairInner, 1, mkByteArrayCopy(completion->body.data(), completion->body.size()));
      auto pairMid = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairMid, 0, lean_mk_string(completion->statusText.c_str()));
      lean_ctor_set(pairMid, 1, pairInner);
      auto pairOuter = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairOuter, 0, lean_box_uint32(completion->statusCode));
      lean_ctor_set(pairOuter, 1, pairMid);
      return lean_io_result_mk_ok(pairOuter);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_with_headers");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_request_with_headers_ref(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders, b_lean_obj_arg bodyRef) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequest(
        method, std::string(lean_string_cstr(address)), portHint,
        std::string(lean_string_cstr(path)), copyByteArray(requestHeaders),
        getBytesRefDataOrThrow(bodyRef));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }

      auto pairInner = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairInner, 0,
                    mkByteArrayCopy(completion->headers.data(), completion->headers.size()));
      lean_ctor_set(pairInner, 1, mkBytesRef(makeSharedBytes(std::move(completion->body))));
      auto pairMid = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairMid, 0, lean_mk_string(completion->statusText.c_str()));
      lean_ctor_set(pairMid, 1, pairInner);
      auto pairOuter = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairOuter, 0, lean_box_uint32(completion->statusCode));
      lean_ctor_set(pairOuter, 1, pairMid);
      return lean_io_result_mk_ok(pairOuter);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_with_headers_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_request_with_headers_secure(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders, b_lean_obj_arg body) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequest(
        method, std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        copyByteArray(requestHeaders), copyByteArrayToSharedBytes(body), true);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }

      auto pairInner = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairInner, 0,
                    mkByteArrayCopy(completion->headers.data(), completion->headers.size()));
      lean_ctor_set(pairInner, 1, mkByteArrayCopy(completion->body.data(), completion->body.size()));
      auto pairMid = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairMid, 0, lean_mk_string(completion->statusText.c_str()));
      lean_ctor_set(pairMid, 1, pairInner);
      auto pairOuter = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairOuter, 0, lean_box_uint32(completion->statusCode));
      lean_ctor_set(pairOuter, 1, pairMid);
      return lean_io_result_mk_ok(pairOuter);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_with_headers_secure");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_with_headers_secure_ref(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders, b_lean_obj_arg bodyRef) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequest(
        method, std::string(lean_string_cstr(address)), portHint,
        std::string(lean_string_cstr(path)), copyByteArray(requestHeaders),
        getBytesRefDataOrThrow(bodyRef), true);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }

      auto pairInner = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairInner, 0,
                    mkByteArrayCopy(completion->headers.data(), completion->headers.size()));
      lean_ctor_set(pairInner, 1, mkBytesRef(makeSharedBytes(std::move(completion->body))));
      auto pairMid = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairMid, 0, lean_mk_string(completion->statusText.c_str()));
      lean_ctor_set(pairMid, 1, pairInner);
      auto pairOuter = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairOuter, 0, lean_box_uint32(completion->statusCode));
      lean_ctor_set(pairOuter, 1, pairMid);
      return lean_io_result_mk_ok(pairOuter);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_with_headers_secure_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_with_headers_with_response_limit(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders, b_lean_obj_arg body,
    uint64_t responseBodyLimit) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequest(
        method, std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        copyByteArray(requestHeaders), copyByteArrayToSharedBytes(body), false, responseBodyLimit);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }

      auto pairInner = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairInner, 0,
                    mkByteArrayCopy(completion->headers.data(), completion->headers.size()));
      lean_ctor_set(pairInner, 1, mkByteArrayCopy(completion->body.data(), completion->body.size()));
      auto pairMid = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairMid, 0, lean_mk_string(completion->statusText.c_str()));
      lean_ctor_set(pairMid, 1, pairInner);
      auto pairOuter = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairOuter, 0, lean_box_uint32(completion->statusCode));
      lean_ctor_set(pairOuter, 1, pairMid);
      return lean_io_result_mk_ok(pairOuter);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_with_headers_with_response_limit");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_with_headers_with_response_limit_ref(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders, b_lean_obj_arg bodyRef,
    uint64_t responseBodyLimit) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequest(
        method, std::string(lean_string_cstr(address)), portHint,
        std::string(lean_string_cstr(path)), copyByteArray(requestHeaders),
        getBytesRefDataOrThrow(bodyRef), false, responseBodyLimit);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }

      auto pairInner = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairInner, 0,
                    mkByteArrayCopy(completion->headers.data(), completion->headers.size()));
      lean_ctor_set(pairInner, 1, mkBytesRef(makeSharedBytes(std::move(completion->body))));
      auto pairMid = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairMid, 0, lean_mk_string(completion->statusText.c_str()));
      lean_ctor_set(pairMid, 1, pairInner);
      auto pairOuter = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairOuter, 0, lean_box_uint32(completion->statusCode));
      lean_ctor_set(pairOuter, 1, pairMid);
      return lean_io_result_mk_ok(pairOuter);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_with_headers_with_response_limit_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_with_headers_with_response_limit_secure(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders, b_lean_obj_arg body,
    uint64_t responseBodyLimit) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequest(
        method, std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        copyByteArray(requestHeaders), copyByteArrayToSharedBytes(body), true, responseBodyLimit);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }

      auto pairInner = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairInner, 0,
                    mkByteArrayCopy(completion->headers.data(), completion->headers.size()));
      lean_ctor_set(pairInner, 1, mkByteArrayCopy(completion->body.data(), completion->body.size()));
      auto pairMid = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairMid, 0, lean_mk_string(completion->statusText.c_str()));
      lean_ctor_set(pairMid, 1, pairInner);
      auto pairOuter = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairOuter, 0, lean_box_uint32(completion->statusCode));
      lean_ctor_set(pairOuter, 1, pairMid);
      return lean_io_result_mk_ok(pairOuter);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_with_headers_with_response_limit_secure");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_with_headers_with_response_limit_secure_ref(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders, b_lean_obj_arg bodyRef,
    uint64_t responseBodyLimit) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequest(
        method, std::string(lean_string_cstr(address)), portHint,
        std::string(lean_string_cstr(path)), copyByteArray(requestHeaders),
        getBytesRefDataOrThrow(bodyRef), true, responseBodyLimit);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }

      auto pairInner = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairInner, 0,
                    mkByteArrayCopy(completion->headers.data(), completion->headers.size()));
      lean_ctor_set(pairInner, 1, mkBytesRef(makeSharedBytes(std::move(completion->body))));
      auto pairMid = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairMid, 0, lean_mk_string(completion->statusText.c_str()));
      lean_ctor_set(pairMid, 1, pairInner);
      auto pairOuter = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairOuter, 0, lean_box_uint32(completion->statusCode));
      lean_ctor_set(pairOuter, 1, pairMid);
      return lean_io_result_mk_ok(pairOuter);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_with_headers_with_response_limit_secure_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_start_with_headers(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders, b_lean_obj_arg body) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequestStart(
        method, std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        copyByteArray(requestHeaders), copyByteArrayToSharedBytes(body));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_start_with_headers");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_start_with_headers_ref(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders, b_lean_obj_arg bodyRef) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequestStart(
        method, std::string(lean_string_cstr(address)), portHint,
        std::string(lean_string_cstr(path)), copyByteArray(requestHeaders),
        getBytesRefDataOrThrow(bodyRef));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_start_with_headers_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_start_with_headers_secure(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders, b_lean_obj_arg body) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequestStart(
        method, std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        copyByteArray(requestHeaders), copyByteArrayToSharedBytes(body), true);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_start_with_headers_secure");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_start_with_headers_secure_ref(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders, b_lean_obj_arg bodyRef) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequestStart(
        method, std::string(lean_string_cstr(address)), portHint,
        std::string(lean_string_cstr(path)), copyByteArray(requestHeaders),
        getBytesRefDataOrThrow(bodyRef), true);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_start_with_headers_secure_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_start_streaming_with_headers(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequestStartStreaming(
        method, std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        copyByteArray(requestHeaders));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box_uint32(completion->first));
      lean_ctor_set(pair, 1, lean_box_uint32(completion->second));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_start_streaming_with_headers");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_start_streaming_with_headers_secure(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequestStartStreaming(
        method, std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        copyByteArray(requestHeaders), true);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box_uint32(completion->first));
      lean_ctor_set(pair, 1, lean_box_uint32(completion->second));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_start_streaming_with_headers_secure");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_start_with_headers_with_response_limit(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders, b_lean_obj_arg body,
    uint64_t responseBodyLimit) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequestStart(
        method, std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        copyByteArray(requestHeaders), copyByteArrayToSharedBytes(body), false, responseBodyLimit);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_start_with_headers_with_response_limit");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_start_with_headers_with_response_limit_ref(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders, b_lean_obj_arg bodyRef,
    uint64_t responseBodyLimit) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequestStart(
        method, std::string(lean_string_cstr(address)), portHint,
        std::string(lean_string_cstr(path)), copyByteArray(requestHeaders),
        getBytesRefDataOrThrow(bodyRef), false, responseBodyLimit);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_start_with_headers_with_response_limit_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_start_with_headers_with_response_limit_secure(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders, b_lean_obj_arg body,
    uint64_t responseBodyLimit) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequestStart(
        method, std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        copyByteArray(requestHeaders), copyByteArrayToSharedBytes(body), true, responseBodyLimit);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_start_with_headers_with_response_limit_secure");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_start_with_headers_with_response_limit_secure_ref(
    uint64_t runtimeId, uint32_t method, b_lean_obj_arg address, uint32_t portHint,
    b_lean_obj_arg path, b_lean_obj_arg requestHeaders, b_lean_obj_arg bodyRef,
    uint64_t responseBodyLimit) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequestStart(
        method, std::string(lean_string_cstr(address)), portHint,
        std::string(lean_string_cstr(path)), copyByteArray(requestHeaders),
        getBytesRefDataOrThrow(bodyRef), true, responseBodyLimit);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_start_with_headers_with_response_limit_secure_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_response_promise_await_with_headers(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpResponsePromiseAwait(promiseId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }

      auto pairInner = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairInner, 0,
                    mkByteArrayCopy(completion->headers.data(), completion->headers.size()));
      lean_ctor_set(pairInner, 1, mkByteArrayCopy(completion->body.data(), completion->body.size()));
      auto pairMid = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairMid, 0, lean_mk_string(completion->statusText.c_str()));
      lean_ctor_set(pairMid, 1, pairInner);
      auto pairOuter = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairOuter, 0, lean_box_uint32(completion->statusCode));
      lean_ctor_set(pairOuter, 1, pairMid);
      return lean_io_result_mk_ok(pairOuter);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_response_promise_await_with_headers");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_response_promise_await_with_headers_ref(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpResponsePromiseAwait(promiseId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }

      auto pairInner = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairInner, 0,
                    mkByteArrayCopy(completion->headers.data(), completion->headers.size()));
      lean_ctor_set(pairInner, 1, mkBytesRef(makeSharedBytes(std::move(completion->body))));
      auto pairMid = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairMid, 0, lean_mk_string(completion->statusText.c_str()));
      lean_ctor_set(pairMid, 1, pairInner);
      auto pairOuter = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairOuter, 0, lean_box_uint32(completion->statusCode));
      lean_ctor_set(pairOuter, 1, pairMid);
      return lean_io_result_mk_ok(pairOuter);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_response_promise_await_with_headers_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_response_promise_await_streaming_with_headers(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpResponsePromiseAwaitStreaming(promiseId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }

      auto pairInner = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairInner, 0,
                    mkByteArrayCopy(completion->headers.data(), completion->headers.size()));
      lean_ctor_set(pairInner, 1, lean_box_uint32(completion->bodyHandle));
      auto pairMid = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairMid, 0, lean_mk_string(completion->statusText.c_str()));
      lean_ctor_set(pairMid, 1, pairInner);
      auto pairOuter = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairOuter, 0, lean_box_uint32(completion->statusCode));
      lean_ctor_set(pairOuter, 1, pairMid);
      return lean_io_result_mk_ok(pairOuter);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_response_promise_await_streaming_with_headers");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_request_body_write_start_ref(
    uint64_t runtimeId, uint32_t requestBodyId, b_lean_obj_arg bytesRef) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion =
        runtime->enqueueHttpRequestBodyWriteStart(requestBodyId, getBytesRefDataOrThrow(bytesRef));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_body_write_start_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_request_body_write_ref(
    uint64_t runtimeId, uint32_t requestBodyId, b_lean_obj_arg bytesRef) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion =
        runtime->enqueueHttpRequestBodyWrite(requestBodyId, getBytesRefDataOrThrow(bytesRef));
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_body_write_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_request_body_finish_start(
    uint64_t runtimeId, uint32_t requestBodyId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequestBodyFinishStart(requestBodyId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_request_body_finish_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_request_body_finish(
    uint64_t runtimeId, uint32_t requestBodyId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequestBodyFinish(requestBodyId);
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_http_request_body_finish");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_request_body_release(
    uint64_t runtimeId, uint32_t requestBodyId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpRequestBodyRelease(requestBodyId);
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_http_request_body_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_response_body_read_start(
    uint64_t runtimeId, uint32_t responseBodyId, uint32_t minBytes, uint32_t maxBytes) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpResponseBodyReadStart(responseBodyId, minBytes, maxBytes);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_response_body_read_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_response_body_read_ref(
    uint64_t runtimeId, uint32_t responseBodyId, uint32_t minBytes, uint32_t maxBytes) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpResponseBodyRead(responseBodyId, minBytes, maxBytes);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto bytes = completion->bytes ? completion->bytes : std::make_shared<std::vector<uint8_t>>();
      return lean_io_result_mk_ok(mkBytesRef(std::move(bytes)));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_response_body_read_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_response_body_release(
    uint64_t runtimeId, uint32_t responseBodyId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpResponseBodyRelease(responseBodyId);
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_http_response_body_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_server_request_body_read_start(
    uint64_t runtimeId, uint32_t requestBodyId, uint32_t minBytes, uint32_t maxBytes) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpServerRequestBodyReadStart(requestBodyId, minBytes, maxBytes);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_server_request_body_read_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_server_request_body_read_ref(
    uint64_t runtimeId, uint32_t requestBodyId, uint32_t minBytes, uint32_t maxBytes) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpServerRequestBodyRead(requestBodyId, minBytes, maxBytes);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto bytes = completion->bytes ? completion->bytes : std::make_shared<std::vector<uint8_t>>();
      return lean_io_result_mk_ok(mkBytesRef(std::move(bytes)));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_server_request_body_read_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_server_request_body_release(
    uint64_t runtimeId, uint32_t requestBodyId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpServerRequestBodyRelease(requestBodyId);
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_server_request_body_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_server_listen(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion =
        runtime->enqueueHttpServerListen(std::string(lean_string_cstr(address)), portHint);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box_uint32(completion->first));
      lean_ctor_set(pair, 1, lean_box_uint32(completion->second));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_http_server_listen");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_server_listen_with_config(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint, uint64_t headerTimeoutNanos,
    uint64_t pipelineTimeoutNanos, uint64_t canceledUploadGracePeriodNanos,
    uint64_t canceledUploadGraceBytes, uint32_t webSocketCompressionMode) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    KjAsyncRuntimeLoop::HttpServerConfig config;
    config.headerTimeoutNanos = headerTimeoutNanos;
    config.pipelineTimeoutNanos = pipelineTimeoutNanos;
    config.canceledUploadGracePeriodNanos = canceledUploadGracePeriodNanos;
    config.canceledUploadGraceBytes = canceledUploadGraceBytes;
    config.webSocketCompressionMode = webSocketCompressionMode;
    auto completion = runtime->enqueueHttpServerListen(std::string(lean_string_cstr(address)),
                                                       portHint, false, config);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box_uint32(completion->first));
      lean_ctor_set(pair, 1, lean_box_uint32(completion->second));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_server_listen_with_config");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_server_listen_secure(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion =
        runtime->enqueueHttpServerListen(std::string(lean_string_cstr(address)), portHint, true);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box_uint32(completion->first));
      lean_ctor_set(pair, 1, lean_box_uint32(completion->second));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_server_listen_secure");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_server_listen_secure_with_config(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint, uint64_t headerTimeoutNanos,
    uint64_t pipelineTimeoutNanos, uint64_t canceledUploadGracePeriodNanos,
    uint64_t canceledUploadGraceBytes, uint32_t webSocketCompressionMode) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    KjAsyncRuntimeLoop::HttpServerConfig config;
    config.headerTimeoutNanos = headerTimeoutNanos;
    config.pipelineTimeoutNanos = pipelineTimeoutNanos;
    config.canceledUploadGracePeriodNanos = canceledUploadGracePeriodNanos;
    config.canceledUploadGraceBytes = canceledUploadGraceBytes;
    config.webSocketCompressionMode = webSocketCompressionMode;
    auto completion = runtime->enqueueHttpServerListen(std::string(lean_string_cstr(address)),
                                                       portHint, true, config);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box_uint32(completion->first));
      lean_ctor_set(pair, 1, lean_box_uint32(completion->second));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_server_listen_secure_with_config");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_server_release(
    uint64_t runtimeId, uint32_t serverId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpServerRelease(serverId);
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_http_server_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_server_drain_start(
    uint64_t runtimeId, uint32_t serverId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpServerDrainStart(serverId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_server_drain_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_server_drain(
    uint64_t runtimeId, uint32_t serverId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpServerDrain(serverId);
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_http_server_drain");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_server_poll_request(
    uint64_t runtimeId, uint32_t serverId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpServerPollRequest(serverId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box(completion->hasRequest ? 1 : 0));
      lean_ctor_set(pair, 1, mkByteArrayCopy(completion->requestBytes.data(),
                                             completion->requestBytes.size()));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_server_poll_request");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_server_respond(
    uint64_t runtimeId, uint32_t serverId, uint32_t requestId, uint32_t statusCode,
    b_lean_obj_arg statusText, b_lean_obj_arg responseHeaders, b_lean_obj_arg body) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpServerRespond(
        serverId, requestId, statusCode, std::string(lean_string_cstr(statusText)),
        copyByteArray(responseHeaders), copyByteArrayToSharedBytes(body));
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_http_server_respond");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_server_respond_ref(
    uint64_t runtimeId, uint32_t serverId, uint32_t requestId, uint32_t statusCode,
    b_lean_obj_arg statusText, b_lean_obj_arg responseHeaders, b_lean_obj_arg bodyRef) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpServerRespond(
        serverId, requestId, statusCode, std::string(lean_string_cstr(statusText)),
        copyByteArray(responseHeaders), getBytesRefDataOrThrow(bodyRef));
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_server_respond_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_server_respond_websocket(
    uint64_t runtimeId, uint32_t serverId, uint32_t requestId, b_lean_obj_arg responseHeaders) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion =
        runtime->enqueueHttpServerRespondWebSocket(serverId, requestId, copyByteArray(responseHeaders));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_server_respond_websocket");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_server_respond_start_streaming(
    uint64_t runtimeId, uint32_t serverId, uint32_t requestId, uint32_t statusCode,
    b_lean_obj_arg statusText, b_lean_obj_arg responseHeaders) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpServerRespondStartStreaming(
        serverId, requestId, statusCode, std::string(lean_string_cstr(statusText)),
        copyByteArray(responseHeaders));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_server_respond_start_streaming");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_server_response_body_write_start_ref(
    uint64_t runtimeId, uint32_t responseBodyId, b_lean_obj_arg bytesRef) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpServerResponseBodyWriteStart(
        responseBodyId, getBytesRefDataOrThrow(bytesRef));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_server_response_body_write_start_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_server_response_body_write_ref(
    uint64_t runtimeId, uint32_t responseBodyId, b_lean_obj_arg bytesRef) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpServerResponseBodyWrite(
        responseBodyId, getBytesRefDataOrThrow(bytesRef));
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_server_response_body_write_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_http_server_response_body_finish_start(
    uint64_t runtimeId, uint32_t responseBodyId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpServerResponseBodyFinishStart(responseBodyId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_server_response_body_finish_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_server_response_body_finish(
    uint64_t runtimeId, uint32_t responseBodyId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpServerResponseBodyFinish(responseBodyId);
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_server_response_body_finish");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_http_server_response_body_release(
    uint64_t runtimeId, uint32_t responseBodyId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueHttpServerResponseBodyRelease(responseBodyId);
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_http_server_response_body_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_connect(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint, b_lean_obj_arg path) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketConnect(
        std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        encodedEmptyHeaderPairs());
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_websocket_connect");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_connect_start(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint, b_lean_obj_arg path) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketConnectStart(
        std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        encodedEmptyHeaderPairs());
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_websocket_connect_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_connect_with_headers(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint, b_lean_obj_arg path,
    b_lean_obj_arg requestHeaders) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketConnect(
        std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        copyByteArray(requestHeaders));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_websocket_connect_with_headers");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_websocket_connect_with_headers_secure(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint, b_lean_obj_arg path,
    b_lean_obj_arg requestHeaders) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketConnect(
        std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        copyByteArray(requestHeaders), true);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_websocket_connect_with_headers_secure");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_websocket_connect_start_with_headers(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint, b_lean_obj_arg path,
    b_lean_obj_arg requestHeaders) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketConnectStart(
        std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        copyByteArray(requestHeaders));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_websocket_connect_start_with_headers");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_websocket_connect_start_with_headers_secure(
    uint64_t runtimeId, b_lean_obj_arg address, uint32_t portHint, b_lean_obj_arg path,
    b_lean_obj_arg requestHeaders) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketConnectStart(
        std::string(lean_string_cstr(address)), portHint, std::string(lean_string_cstr(path)),
        copyByteArray(requestHeaders), true);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_websocket_connect_start_with_headers_secure");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_promise_await(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketPromiseAwait(promiseId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->handle));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_websocket_promise_await");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_promise_cancel(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketPromiseCancel(promiseId);
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_websocket_promise_cancel");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_promise_release(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketPromiseRelease(promiseId);
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_websocket_promise_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_release(
    uint64_t runtimeId, uint32_t webSocketId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketRelease(webSocketId);
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_websocket_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_send_text_start(
    uint64_t runtimeId, uint32_t webSocketId, b_lean_obj_arg text) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion =
        runtime->enqueueWebSocketSendTextStart(webSocketId, std::string(lean_string_cstr(text)));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_websocket_send_text_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_send_text(
    uint64_t runtimeId, uint32_t webSocketId, b_lean_obj_arg text) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketSendText(webSocketId, std::string(lean_string_cstr(text)));
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_websocket_send_text");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_send_binary_start_ref(
    uint64_t runtimeId, uint32_t webSocketId, b_lean_obj_arg bytesRef) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion =
        runtime->enqueueWebSocketSendBinaryStart(webSocketId, getBytesRefDataOrThrow(bytesRef));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_websocket_send_binary_start_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_send_binary_ref(
    uint64_t runtimeId, uint32_t webSocketId, b_lean_obj_arg bytesRef) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion =
        runtime->enqueueWebSocketSendBinary(webSocketId, getBytesRefDataOrThrow(bytesRef));
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_websocket_send_binary_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_receive_start(
    uint64_t runtimeId, uint32_t webSocketId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketReceiveStart(webSocketId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_websocket_receive_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_receive_start_with_max(
    uint64_t runtimeId, uint32_t webSocketId, uint32_t maxBytes) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketReceiveStart(webSocketId, maxBytes);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_websocket_receive_start_with_max");
  }
}

extern "C" LEAN_EXPORT lean_obj_res
capnp_lean_kj_async_runtime_websocket_message_promise_await_ref(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketMessagePromiseAwait(promiseId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto pairInner = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairInner, 0, lean_mk_string(completion->text.c_str()));
      lean_ctor_set(pairInner, 1, mkBytesRef(makeSharedBytes(std::move(completion->bytes))));
      auto pairMid = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairMid, 0, lean_box_uint32(completion->closeCode));
      lean_ctor_set(pairMid, 1, pairInner);
      auto pairOuter = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairOuter, 0, lean_box_uint32(completion->tag));
      lean_ctor_set(pairOuter, 1, pairMid);
      return lean_io_result_mk_ok(pairOuter);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_websocket_message_promise_await_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_message_promise_cancel(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketMessagePromiseCancel(promiseId);
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_websocket_message_promise_cancel");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_message_promise_release(
    uint64_t runtimeId, uint32_t promiseId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketMessagePromiseRelease(promiseId);
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_websocket_message_promise_release");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_receive_ref(
    uint64_t runtimeId, uint32_t webSocketId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketReceive(webSocketId);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto pairInner = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairInner, 0, lean_mk_string(completion->text.c_str()));
      lean_ctor_set(pairInner, 1, mkBytesRef(makeSharedBytes(std::move(completion->bytes))));
      auto pairMid = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairMid, 0, lean_box_uint32(completion->closeCode));
      lean_ctor_set(pairMid, 1, pairInner);
      auto pairOuter = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairOuter, 0, lean_box_uint32(completion->tag));
      lean_ctor_set(pairOuter, 1, pairMid);
      return lean_io_result_mk_ok(pairOuter);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_websocket_receive_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_receive_with_max_ref(
    uint64_t runtimeId, uint32_t webSocketId, uint32_t maxBytes) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketReceive(webSocketId, maxBytes);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto pairInner = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairInner, 0, lean_mk_string(completion->text.c_str()));
      lean_ctor_set(pairInner, 1, mkBytesRef(makeSharedBytes(std::move(completion->bytes))));
      auto pairMid = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairMid, 0, lean_box_uint32(completion->closeCode));
      lean_ctor_set(pairMid, 1, pairInner);
      auto pairOuter = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pairOuter, 0, lean_box_uint32(completion->tag));
      lean_ctor_set(pairOuter, 1, pairMid);
      return lean_io_result_mk_ok(pairOuter);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_websocket_receive_with_max_ref");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_close_start(
    uint64_t runtimeId, uint32_t webSocketId, uint32_t closeCode, b_lean_obj_arg reason) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }
  if (closeCode > std::numeric_limits<uint16_t>::max()) {
    return mkIoUserError("websocket close code exceeds UInt16 range");
  }

  try {
    auto completion = runtime->enqueueWebSocketCloseStart(
        webSocketId, static_cast<uint16_t>(closeCode), std::string(lean_string_cstr(reason)));
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint32(completion->promiseId));
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_websocket_close_start");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_close(
    uint64_t runtimeId, uint32_t webSocketId, uint32_t closeCode, b_lean_obj_arg reason) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }
  if (closeCode > std::numeric_limits<uint16_t>::max()) {
    return mkIoUserError("websocket close code exceeds UInt16 range");
  }

  try {
    auto completion = runtime->enqueueWebSocketClose(
        webSocketId, static_cast<uint16_t>(closeCode), std::string(lean_string_cstr(reason)));
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_websocket_close");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_disconnect(
    uint64_t runtimeId, uint32_t webSocketId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketDisconnect(webSocketId);
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
    return mkIoUserError(
        "unknown exception in capnp_lean_kj_async_runtime_websocket_disconnect");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_websocket_abort(
    uint64_t runtimeId, uint32_t webSocketId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueWebSocketAbort(webSocketId);
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
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_websocket_abort");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_kj_async_runtime_new_websocket_pipe(
    uint64_t runtimeId) {
  auto runtime = getKjAsyncRuntime(runtimeId);
  if (!runtime) {
    return mkIoUserError("Capnp.KjAsync runtime handle is invalid or already released");
  }

  try {
    auto completion = runtime->enqueueNewWebSocketPipe();
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return mkIoUserError(completion->error);
      }
      auto pair = lean_alloc_ctor(0, 2, 0);
      lean_ctor_set(pair, 0, lean_box_uint32(completion->first));
      lean_ctor_set(pair, 1, lean_box_uint32(completion->second));
      return lean_io_result_mk_ok(pair);
    }
  } catch (const kj::Exception& e) {
    return mkIoUserError(describeKjException(e));
  } catch (const std::exception& e) {
    return mkIoUserError(e.what());
  } catch (...) {
    return mkIoUserError("unknown exception in capnp_lean_kj_async_runtime_new_websocket_pipe");
  }
}
