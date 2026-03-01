#include "rpc_bridge_runtime.h"
#include "rpc_bridge_generic_vat.h"

#include <capnp/any.h>
#include <capnp/capability.h>
#include <capnp/rpc.h>
#include <capnp/rpc-twoparty.h>
#include <capnp/serialize.h>
#include <kj/async.h>
#include <kj/async-queue.h>
#include <kj/async-io.h>
#if _WIN32
#include <kj/async-win32.h>
#else
#include <kj/async-unix.h>
#endif
#include <kj/io.h>
#include <kj/map.h>
#include <kj/time.h>
#include <kj/vector.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
#if !defined(_WIN32)
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#endif

extern "C" void lean_initialize_thread();

namespace capnp_lean_rpc {

using TwoPartyRpcSystem = capnp::RpcSystem<capnp::rpc::twoparty::VatId>;

[[noreturn]] void throwRemoteException(kj::Exception::Type type, const std::string& message,
                                       const uint8_t* detailBytes, size_t detailBytesSize) {
  auto ex = kj::Exception(type, __FILE__, __LINE__, kj::str(message.c_str()));
  if (detailBytesSize != 0) {
    auto copy = kj::heapArray<kj::byte>(detailBytesSize);
    std::memcpy(copy.begin(), detailBytes, detailBytesSize);
    ex.setDetail(1, kj::mv(copy));
  }
  throw kj::mv(ex);
}

class OneShotCaptureServer final : public capnp::Capability::Server {
 public:
  OneShotCaptureServer(uint64_t expectedInterfaceId, uint16_t expectedMethodId,
                       kj::Own<kj::PromiseFulfiller<RawCallResult>> fulfiller,
                       uint32_t delayMillis = 0, bool throwException = false,
                       bool throwWithDetail = false)
      : expectedInterfaceId_(expectedInterfaceId),
        expectedMethodId_(expectedMethodId),
        fulfiller_(kj::mv(fulfiller)),
        delayMillis_(delayMillis),
        throwException_(throwException),
        throwWithDetail_(throwWithDetail) {}

  DispatchCallResult dispatchCall(
      uint64_t interfaceId, uint16_t methodId,
      capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer> context) override {
    if (interfaceId != expectedInterfaceId_ || methodId != expectedMethodId_) {
      throw std::runtime_error("unexpected method in capnp_lean_rpc_cpp_serve_echo_once");
    }

    capnp::MallocMessageBuilder requestMessage;
    capnp::BuilderCapabilityTable requestCapTable;
    requestCapTable
        .imbue(requestMessage.getRoot<capnp::AnyPointer>())
        .setAs<capnp::AnyPointer>(context.getParams().getAs<capnp::AnyPointer>());

    auto requestWords = capnp::messageToFlatArray(requestMessage);

    std::vector<uint8_t> requestCaps;
    auto requestCapEntries = requestCapTable.getTable();
    requestCaps.reserve(requestCapEntries.size() * 4);
    for (auto& maybeHook : requestCapEntries) {
      KJ_IF_SOME(_hook, maybeHook) {
        throw std::runtime_error(
            "capnp_lean_rpc_cpp_serve_echo_once does not support capability arguments");
      } else {
        appendUint32Le(requestCaps, 0);
      }
    }

    RawCallResult captured{std::move(requestWords), std::move(requestCaps)};
    auto fulfillCaptured = [&]() {
      if (!fulfilled_) {
        fulfiller_->fulfill(kj::mv(captured));
        fulfilled_ = true;
      }
    };

    if (delayMillis_ != 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delayMillis_));
    }

    if (throwException_) {
      fulfillCaptured();
      auto ex = KJ_EXCEPTION(FAILED, "test exception");
      if (throwWithDetail_) {
        ex.setDetail(1, kj::heapArray("cpp-detail-1"_kj.asBytes()));
      }
      throw kj::mv(ex);
    }

    context.getResults().setAs<capnp::AnyPointer>(context.getParams().getAs<capnp::AnyPointer>());
    fulfillCaptured();
    return {kj::READY_NOW, false};
  }

 private:
  uint64_t expectedInterfaceId_;
  uint16_t expectedMethodId_;
  kj::Own<kj::PromiseFulfiller<RawCallResult>> fulfiller_;
  bool fulfilled_ = false;
  uint32_t delayMillis_ = 0;
  bool throwException_ = false;
  bool throwWithDetail_ = false;
};

RawCallResult cppCallOneShot(const std::string& address, uint32_t portHint, uint64_t interfaceId,
                             uint16_t methodId, const std::vector<uint8_t>& requestBytes,
                             const std::vector<uint32_t>& requestCapIds) {
  for (auto capId : requestCapIds) {
    if (capId != 0) {
      throw std::runtime_error(
          "capnp_lean_rpc_cpp_call_one_shot does not support non-zero capability ids");
    }
  }

  auto io = kj::setupAsyncIo();
  auto addr = io.provider->getNetwork().parseAddress(address.c_str(), portHint).wait(io.waitScope);
  auto stream = addr->connect().wait(io.waitScope);
  auto network = kj::heap<capnp::TwoPartyVatNetwork>(*stream, capnp::rpc::twoparty::Side::CLIENT);
  auto rpcSystem = kj::heap<TwoPartyRpcSystem>(capnp::makeRpcClient(*network));

  capnp::word scratch[4];
  memset(&scratch, 0, sizeof(scratch));
  capnp::MallocMessageBuilder message(scratch);
  auto vatId = message.getRoot<capnp::rpc::twoparty::VatId>();
  vatId.setSide(capnp::rpc::twoparty::Side::SERVER);
  auto target = rpcSystem->bootstrap(vatId);

  auto requestBuilder = target.typelessRequest(interfaceId, methodId, kj::none, {});
  if (!requestBytes.empty()) {
    kj::ArrayPtr<const kj::byte> reqBytes(reinterpret_cast<const kj::byte*>(requestBytes.data()),
                                          requestBytes.size());
    kj::ArrayInputStream input(reqBytes);
    capnp::ReaderOptions options;
    options.traversalLimitInWords = 1ull << 30;
    capnp::InputStreamMessageReader reader(input, options);
    requestBuilder.setAs<capnp::AnyPointer>(reader.getRoot<capnp::AnyPointer>());
  }

  auto response = requestBuilder.send().wait(io.waitScope);
  capnp::MallocMessageBuilder responseMessage;
  capnp::BuilderCapabilityTable responseCapTable;
  responseCapTable
      .imbue(responseMessage.getRoot<capnp::AnyPointer>())
      .setAs<capnp::AnyPointer>(response.getAs<capnp::AnyPointer>());

  auto responseWords = capnp::messageToFlatArray(responseMessage);

  std::vector<uint8_t> responseCaps;
  auto responseCapEntries = responseCapTable.getTable();
  responseCaps.reserve(responseCapEntries.size() * 4);
  for (auto& maybeHook : responseCapEntries) {
    KJ_IF_SOME(_hook, maybeHook) {
      throw std::runtime_error(
          "capnp_lean_rpc_cpp_call_one_shot does not support capability results");
    } else {
      appendUint32Le(responseCaps, 0);
    }
  }

  return RawCallResult{std::move(responseWords), std::move(responseCaps)};
}

void setRequestPayloadNoCaps(capnp::Request<capnp::AnyPointer, capnp::AnyPointer>& requestBuilder,
                             const std::vector<uint8_t>& requestBytes,
                             const std::vector<uint32_t>& requestCapIds,
                             const char* context) {
  for (auto capId : requestCapIds) {
    if (capId != 0) {
      throw std::runtime_error(std::string(context) + " does not support non-zero capability ids");
    }
  }
  if (!requestBytes.empty()) {
    kj::ArrayPtr<const kj::byte> reqBytes(reinterpret_cast<const kj::byte*>(requestBytes.data()),
                                          requestBytes.size());
    kj::ArrayInputStream input(reqBytes);
    capnp::ReaderOptions options;
    options.traversalLimitInWords = 1ull << 30;
    capnp::InputStreamMessageReader reader(input, options);
    requestBuilder.setAs<capnp::AnyPointer>(reader.getRoot<capnp::AnyPointer>());
  }
}

RawCallResult serializeResponseNoCaps(capnp::Response<capnp::AnyPointer>& response,
                                      const char* context) {
  capnp::MallocMessageBuilder responseMessage;
  capnp::BuilderCapabilityTable responseCapTable;
  responseCapTable
      .imbue(responseMessage.getRoot<capnp::AnyPointer>())
      .setAs<capnp::AnyPointer>(response.getAs<capnp::AnyPointer>());

  auto responseWords = capnp::messageToFlatArray(responseMessage);

  std::vector<uint8_t> responseCaps;
  auto responseCapEntries = responseCapTable.getTable();
  responseCaps.reserve(responseCapEntries.size() * 4);
  for (auto& maybeHook : responseCapEntries) {
    KJ_IF_SOME(_hook, maybeHook) {
      throw std::runtime_error(std::string(context) + " does not support capability results");
    } else {
      appendUint32Le(responseCaps, 0);
    }
  }

  return RawCallResult{std::move(responseWords), std::move(responseCaps)};
}

RawCallResult cppCallPipelinedCapOneShot(const std::string& address, uint32_t portHint,
                                         uint64_t interfaceId, uint16_t methodId,
                                         const std::vector<uint8_t>& requestBytes,
                                         const std::vector<uint32_t>& requestCapIds,
                                         const std::vector<uint8_t>& pipelinedRequestBytes,
                                         const std::vector<uint32_t>& pipelinedRequestCapIds) {
  auto io = kj::setupAsyncIo();
  auto addr = io.provider->getNetwork().parseAddress(address.c_str(), portHint).wait(io.waitScope);
  auto stream = addr->connect().wait(io.waitScope).downcast<kj::AsyncCapabilityStream>();
  auto network = kj::heap<capnp::TwoPartyVatNetwork>(
      *stream, 16, capnp::rpc::twoparty::Side::CLIENT);
  auto rpcSystem = kj::heap<TwoPartyRpcSystem>(capnp::makeRpcClient(*network));

  capnp::word scratch[4];
  memset(&scratch, 0, sizeof(scratch));
  capnp::MallocMessageBuilder message(scratch);
  auto vatId = message.getRoot<capnp::rpc::twoparty::VatId>();
  vatId.setSide(capnp::rpc::twoparty::Side::SERVER);
  auto target = rpcSystem->bootstrap(vatId);

  auto firstRequest = target.typelessRequest(interfaceId, methodId, kj::none, {});
  setRequestPayloadNoCaps(firstRequest, requestBytes, requestCapIds,
                          "capnp_lean_rpc_cpp_call_pipelined_cap_one_shot(first call)");
  auto firstPromise = firstRequest.send();

  auto pipelinedTarget = capnp::Capability::Client(firstPromise.noop().asCap());
  auto pipelinedRequest = pipelinedTarget.typelessRequest(interfaceId, methodId, kj::none, {});
  setRequestPayloadNoCaps(pipelinedRequest, pipelinedRequestBytes, pipelinedRequestCapIds,
                          "capnp_lean_rpc_cpp_call_pipelined_cap_one_shot(pipelined call)");
  auto pipelinedResponse = pipelinedRequest.send().wait(io.waitScope);

  // Ensure the original call eventually settles as well.
  (void)kj::mv(firstPromise).wait(io.waitScope);
  return serializeResponseNoCaps(
      pipelinedResponse, "capnp_lean_rpc_cpp_call_pipelined_cap_one_shot");
}

RawCallResult cppServeOneShotEx(const std::string& address, uint32_t portHint, uint64_t interfaceId,
                                uint16_t methodId, uint32_t delayMillis, bool throwException,
                                bool throwWithDetail, bool waitForDisconnect = true) {
  auto io = kj::setupAsyncIo();
  auto addr = io.provider->getNetwork().parseAddress(address.c_str(), portHint).wait(io.waitScope);
  auto listener = addr->listen();

  auto paf = kj::newPromiseAndFulfiller<RawCallResult>();
  auto server = kj::heap<capnp::TwoPartyServer>(capnp::Capability::Client(
      kj::heap<OneShotCaptureServer>(interfaceId, methodId, kj::mv(paf.fulfiller), delayMillis,
                                     throwException, throwWithDetail)));

  auto connection = listener->accept().wait(io.waitScope);
  server->accept(kj::mv(connection));
  auto result = paf.promise.wait(io.waitScope);
  if (waitForDisconnect) {
    server->drain().wait(io.waitScope);
  } else {
    // Keep the server alive briefly so the first response can flush before closing.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return result;
}

RawCallResult cppServeOneShot(const std::string& address, uint32_t portHint, uint64_t interfaceId,
                              uint16_t methodId) {
  return cppServeOneShotEx(address, portHint, interfaceId, methodId, 0, false, false, true);
}

class LoopbackCapabilityServer final : public capnp::Capability::Server {
 public:
  DispatchCallResult dispatchCall(uint64_t interfaceId, uint16_t methodId,
                                  capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer>
                                      context) override {
    (void)interfaceId;
    (void)methodId;

    debugLog(
        "loopback.dispatch",
        "interfaceId=" + std::to_string(interfaceId) + " methodId=" + std::to_string(methodId));

    context.getResults().setAs<capnp::AnyPointer>(context.getParams().getAs<capnp::AnyPointer>());
    return {kj::READY_NOW, false};
  }
};

class TailCallForwardingServer final : public capnp::Capability::Server {
 public:
  explicit TailCallForwardingServer(capnp::Capability::Client target)
      : target_(kj::mv(target)) {}

  DispatchCallResult dispatchCall(
      uint64_t interfaceId, uint16_t methodId,
      capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer> context) override {
    debugLog(
        "tailcall.dispatch",
        "interfaceId=" + std::to_string(interfaceId) + " methodId=" + std::to_string(methodId));
    auto requestBuilder = target_.typelessRequest(interfaceId, methodId, kj::none, {});
    requestBuilder.setAs<capnp::AnyPointer>(context.getParams().getAs<capnp::AnyPointer>());
    return {context.tailCall(kj::mv(requestBuilder)), false};
  }

 private:
  capnp::Capability::Client target_;
};

class FdCapabilityServer final : public capnp::Capability::Server {
 public:
  explicit FdCapabilityServer(int fd) : fd_(fd) {}
  ~FdCapabilityServer() {
#if !defined(_WIN32)
    if (fd_ >= 0) {
      close(fd_);
    }
#endif
  }

  DispatchCallResult dispatchCall(
      uint64_t interfaceId, uint16_t methodId,
      capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer> context) override {
    (void)interfaceId;
    (void)methodId;
    context.getResults().setAs<capnp::AnyPointer>(context.getParams().getAs<capnp::AnyPointer>());
    return {kj::READY_NOW, false};
  }

  kj::Maybe<int> getFd() override {
#if defined(_WIN32)
    return kj::none;
#else
    if (fd_ < 0) {
      return kj::none;
    }
    return fd_;
#endif
  }

 private:
  int fd_;
};

class FdProbeCapabilityServer final : public capnp::Capability::Server {
 public:
  DispatchCallResult dispatchCall(
      uint64_t interfaceId, uint16_t methodId,
      capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer> context) override {
    (void)interfaceId;
    (void)methodId;

    auto params = context.getParams().getAs<capnp::List<capnp::Capability>>();
    auto fdPromises = kj::heapArrayBuilder<kj::Promise<kj::Maybe<int>>>(params.size());
    for (uint i = 0; i < params.size(); ++i) {
      capnp::Capability::Client cap = params[i];
      fdPromises.add(cap.getFd());
    }

    auto completion = kj::joinPromises(fdPromises.finish())
                          .then([context = kj::mv(context)](kj::Array<kj::Maybe<int>> fds) mutable {
                            uint64_t deliveredCount = 0;
                            for (auto& maybeFd : fds) {
                              KJ_IF_SOME(_fd, maybeFd) {
                                ++deliveredCount;
                              }
                            }
                            auto result = context.getResults().initAsAnyStruct(1, 0);
                            auto data = result.getDataSection();
                            for (uint i = 0; i < data.size() && i < sizeof(uint64_t); ++i) {
                              data[i] = static_cast<kj::byte>((deliveredCount >> (i * 8)) & 0xff);
                            }
                          });
    return {kj::mv(completion), false};
  }
};

class WakeUpManager {
 public:
  WakeUpManager() {
#if _WIN32
    // Windows implementation omitted for now, would use Event
#else
    if (pipe(pipeFds_) != 0) {
      throw std::runtime_error("WakeUpManager: pipe() failed");
    }
    // Set non-blocking
    fcntl(pipeFds_[0], F_SETFL, O_NONBLOCK);
    fcntl(pipeFds_[1], F_SETFL, O_NONBLOCK);
#endif
  }

  ~WakeUpManager() {
#if !_WIN32
    close(pipeFds_[0]);
    close(pipeFds_[1]);
#endif
  }

  void signal() {
#if !_WIN32
    uint8_t buf = 1;
    (void)write(pipeFds_[1], &buf, 1);
#endif
  }

  void drain() {
#if !_WIN32
    uint8_t buf[128];
    while (read(pipeFds_[0], buf, sizeof(buf)) > 0);
#endif
  }

  int getReadFd() const { return pipeFds_[0]; }

 private:
  int pipeFds_[2];
};

class RuntimeLoop {
 public:
  explicit RuntimeLoop(uint32_t maxFdsPerMessage = kRuntimeDefaultMaxFdsPerMessage)
      : maxFdsPerMessage_(sanitizeMaxFdsPerMessage(maxFdsPerMessage)),
        worker_(&RuntimeLoop::run, this) {
    std::unique_lock<std::mutex> lock(startupMutex_);
    startupCv_.wait(lock, [this]() { return startupComplete_; });
    if (!startupError_.empty()) {
      throw std::runtime_error(startupError_);
    }
  }

  ~RuntimeLoop() { shutdown(); }

  void pollCompletions() { wakeUp_.drain(); }
  int getWakeUpReadFd() const { return wakeUp_.getReadFd(); }

  std::shared_ptr<RawCallCompletion> enqueueRawCall(
      uint32_t target, uint64_t interfaceId, uint16_t methodId, LeanByteArrayRef request,
      std::vector<uint32_t> requestCaps) {
    auto completion = std::make_shared<RawCallCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedRawCall{
          target, interfaceId, methodId, std::move(request), std::move(requestCaps), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RawCallCompletion> enqueueRawCallData(
      uint32_t target, uint64_t interfaceId, uint16_t methodId, const uint8_t* requestData,
      size_t requestSize, std::shared_ptr<const void> requestOwner,
      std::vector<uint32_t> requestCaps) {
    auto completion = std::make_shared<RawCallCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedRawCallData{target, interfaceId, methodId, requestData, requestSize,
                            std::move(requestOwner), std::move(requestCaps), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueStartPendingCall(
      uint32_t target, uint64_t interfaceId, uint16_t methodId, LeanByteArrayRef request,
      std::vector<uint32_t> requestCaps) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedStartPendingCall{
          target, interfaceId, methodId, std::move(request), std::move(requestCaps), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueStartPendingCallData(
      uint32_t target, uint64_t interfaceId, uint16_t methodId, const uint8_t* requestData,
      size_t requestSize, std::shared_ptr<const void> requestOwner,
      std::vector<uint32_t> requestCaps) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedStartPendingCallData{
          target, interfaceId, methodId, requestData, requestSize, std::move(requestOwner),
          std::move(requestCaps), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueStartStreamingPendingCall(
      uint32_t target, uint64_t interfaceId, uint16_t methodId, LeanByteArrayRef request,
      std::vector<uint32_t> requestCaps) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedStartStreamingPendingCall{
          target, interfaceId, methodId, std::move(request), std::move(requestCaps), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueStartStreamingPendingCallData(
      uint32_t target, uint64_t interfaceId, uint16_t methodId, const uint8_t* requestData,
      size_t requestSize, std::shared_ptr<const void> requestOwner,
      std::vector<uint32_t> requestCaps) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedStartStreamingPendingCallData{
          target, interfaceId, methodId, requestData, requestSize, std::move(requestOwner),
          std::move(requestCaps), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RawCallCompletion> enqueueAwaitPendingCall(uint32_t pendingCallId) {
    auto completion = std::make_shared<RawCallCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedAwaitPendingCall{pendingCallId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueReleasePendingCall(uint32_t pendingCallId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedReleasePendingCall{pendingCallId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueGetPipelinedCap(
      uint32_t pendingCallId, std::vector<uint16_t> pointerPath) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedGetPipelinedCap{pendingCallId, std::move(pointerPath), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueStreamingCall(
      uint32_t target, uint64_t interfaceId, uint16_t methodId, LeanByteArrayRef request,
      std::vector<uint32_t> requestCaps) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedStreamingCall{
          target, interfaceId, methodId, std::move(request), std::move(requestCaps), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<Int64Completion> enqueueTargetGetFd(uint32_t target) {
    auto completion = std::make_shared<Int64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedTargetGetFd{target, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueTargetWhenResolved(uint32_t target) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedTargetWhenResolved{target, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueTargetWhenResolvedStart(uint32_t target) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedTargetWhenResolvedStart{target, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<BoolCompletion> enqueueTargetWhenResolvedPoll(uint32_t target) {
    auto completion = std::make_shared<BoolCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeBoolFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedTargetWhenResolvedPoll{target, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueEnableTraceEncoder() {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedEnableTraceEncoder{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueDisableTraceEncoder() {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedDisableTraceEncoder{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueSetTraceEncoder(b_lean_obj_arg encoder) {
    auto completion = std::make_shared<UnitCompletion>();
    auto* encoderObj = const_cast<lean_object*>(encoder);
    lean_mark_mt(encoderObj);
    lean_inc(encoderObj);
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        lean_dec(encoderObj);
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedSetTraceEncoder{encoderObj, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RawCallCompletion> enqueueCppCallWithAccept(
      uint32_t serverId, uint32_t listenerId, std::string address, uint32_t portHint,
      uint64_t interfaceId, uint16_t methodId, LeanByteArrayRef request,
      std::vector<uint32_t> requestCaps) {
    auto completion = std::make_shared<RawCallCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedCppCallWithAccept{
          serverId, listenerId, std::move(address), portHint, interfaceId, methodId,
          std::move(request), std::move(requestCaps), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RawCallCompletion> enqueueCppCallPipelinedWithAccept(
      uint32_t serverId, uint32_t listenerId, std::string address, uint32_t portHint,
      uint64_t interfaceId, uint16_t methodId, LeanByteArrayRef request,
      std::vector<uint32_t> requestCaps, LeanByteArrayRef pipelinedRequest,
      std::vector<uint32_t> pipelinedRequestCaps) {
    auto completion = std::make_shared<RawCallCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedCppCallPipelinedWithAccept{
          serverId, listenerId, std::move(address), portHint, interfaceId, methodId,
          std::move(request), std::move(requestCaps), std::move(pipelinedRequest),
          std::move(pipelinedRequestCaps), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueRegisterLoopbackTarget() {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedRegisterLoopbackTarget{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueRegisterLoopbackTarget(
      uint32_t bootstrapTarget) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedRegisterLoopbackBootstrapTarget{bootstrapTarget, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueRegisterHandlerTarget(
      b_lean_obj_arg handler) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    auto* handlerObj = const_cast<lean_object*>(handler);
    lean_mark_mt(handlerObj);
    lean_inc(handlerObj);
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        lean_dec(handlerObj);
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedRegisterHandlerTarget{handlerObj, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueRegisterAdvancedHandlerTarget(
      b_lean_obj_arg handler) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    auto* handlerObj = const_cast<lean_object*>(handler);
    lean_mark_mt(handlerObj);
    lean_inc(handlerObj);
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        lean_dec(handlerObj);
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedRegisterAdvancedHandlerTarget{handlerObj, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueRegisterTailCallHandlerTarget(
      b_lean_obj_arg handler) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    auto* handlerObj = const_cast<lean_object*>(handler);
    lean_mark_mt(handlerObj);
    lean_inc(handlerObj);
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        lean_dec(handlerObj);
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedRegisterTailCallHandlerTarget{handlerObj, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueRegisterTailCallTarget(uint32_t target) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedRegisterTailCallTarget{target, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueRegisterFdTarget(uint32_t fd) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedRegisterFdTarget{fd, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueRegisterFdProbeTarget() {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedRegisterFdProbeTarget{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueReleaseTarget(uint32_t target) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedReleaseTarget{target, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueReleaseTargets(std::vector<uint32_t> targets) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedReleaseTargets{std::move(targets), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueRetainTarget(uint32_t target) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedRetainTarget{target, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterPairCompletion> enqueueNewPromiseCapability() {
    auto completion = std::make_shared<RegisterPairCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterPairFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedNewPromiseCapability{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueuePromiseCapabilityFulfill(uint32_t fulfillerId,
                                                                  uint32_t target) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedPromiseCapabilityFulfill{fulfillerId, target, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueuePromiseCapabilityReject(uint32_t fulfillerId,
                                                                 uint8_t exceptionTypeTag,
                                                                 std::string message,
                                                                 LeanByteArrayRef detailBytes) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedPromiseCapabilityReject{
          fulfillerId, exceptionTypeTag, std::move(message), std::move(detailBytes), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueuePromiseCapabilityRelease(uint32_t fulfillerId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedPromiseCapabilityRelease{fulfillerId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueOrderingSetResolveHold(bool enabled) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedOrderingSetResolveHold{enabled, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UInt64Completion> enqueueOrderingFlushHeldResolves() {
    auto completion = std::make_shared<UInt64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedOrderingFlushHeldResolves{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UInt64Completion> enqueueOrderingHeldResolveCount() {
    auto completion = std::make_shared<UInt64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedOrderingHeldResolveCount{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueConnectTarget(std::string address,
                                                                 uint32_t portHint) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedConnectTarget{std::move(address), portHint, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueConnectTargetStart(std::string address,
                                                                      uint32_t portHint) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedConnectTargetStart{std::move(address), portHint, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueConnectTargetFd(uint32_t fd) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedConnectTargetFd{fd, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterPairCompletion> enqueueNewTransportPipe() {
    auto completion = std::make_shared<RegisterPairCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterPairFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedNewTransportPipe{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueNewTransportFromFd(uint32_t fd) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedNewTransportFromFd{fd, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueNewTransportFromFdTake(uint32_t fd) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedNewTransportFromFdTake{fd, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueReleaseTransport(uint32_t transportId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedReleaseTransport{transportId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<Int64Completion> enqueueTransportGetFd(uint32_t transportId) {
    auto completion = std::make_shared<Int64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedTransportGetFd{transportId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueConnectTargetTransport(uint32_t transportId) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedConnectTargetTransport{transportId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueListenLoopback(std::string address,
                                                              uint32_t portHint) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedListenLoopback{std::move(address), portHint, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueAcceptLoopback(uint32_t listenerId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedAcceptLoopback{listenerId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueReleaseListener(uint32_t listenerId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedReleaseListener{listenerId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueNewClient(std::string address,
                                                             uint32_t portHint) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedNewClient{std::move(address), portHint, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueNewClientStart(std::string address,
                                                                  uint32_t portHint) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedNewClientStart{std::move(address), portHint, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueReleaseClient(uint32_t clientId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedReleaseClient{clientId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueClientBootstrap(uint32_t clientId) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedClientBootstrap{clientId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueClientOnDisconnect(uint32_t clientId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedClientOnDisconnect{clientId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueClientOnDisconnectStart(uint32_t clientId) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedClientOnDisconnectStart{clientId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueClientSetFlowLimit(uint32_t clientId, uint64_t words) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedClientSetFlowLimit{clientId, words, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueNewServer(uint32_t bootstrapTarget) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedNewServer{bootstrapTarget, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueNewServerWithBootstrapFactory(
      b_lean_obj_arg bootstrapFactory) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    auto* bootstrapFactoryObj = const_cast<lean_object*>(bootstrapFactory);
    lean_mark_mt(bootstrapFactoryObj);
    lean_inc(bootstrapFactoryObj);
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        lean_dec(bootstrapFactoryObj);
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedNewServerWithBootstrapFactory{bootstrapFactoryObj, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueReleaseServer(uint32_t serverId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedReleaseServer{serverId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueServerListen(uint32_t serverId,
                                                                std::string address,
                                                                uint32_t portHint) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedServerListen{serverId, std::move(address), portHint, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueServerAccept(uint32_t serverId, uint32_t listenerId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedServerAccept{serverId, listenerId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueServerAcceptStart(
      uint32_t serverId, uint32_t listenerId) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedServerAcceptStart{serverId, listenerId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueServerAcceptFd(uint32_t serverId, uint32_t fd) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedServerAcceptFd{serverId, fd, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueServerAcceptTransport(uint32_t serverId,
                                                               uint32_t transportId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedServerAcceptTransport{serverId, transportId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueServerDrain(uint32_t serverId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedServerDrain{serverId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueServerDrainStart(uint32_t serverId) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedServerDrainStart{serverId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UInt64Completion> enqueueClientQueueSize(uint32_t clientId) {
    auto completion = std::make_shared<UInt64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedClientQueueSize{clientId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UInt64Completion> enqueueClientQueueCount(uint32_t clientId) {
    auto completion = std::make_shared<UInt64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedClientQueueCount{clientId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UInt64Completion> enqueueClientOutgoingWaitNanos(uint32_t clientId) {
    auto completion = std::make_shared<UInt64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedClientOutgoingWaitNanos{clientId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UInt64Completion> enqueueTargetCount() {
    auto completion = std::make_shared<UInt64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedTargetCount{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UInt64Completion> enqueueListenerCount() {
    auto completion = std::make_shared<UInt64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedListenerCount{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UInt64Completion> enqueueClientCount() {
    auto completion = std::make_shared<UInt64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedClientCount{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UInt64Completion> enqueueServerCount() {
    auto completion = std::make_shared<UInt64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedServerCount{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UInt64Completion> enqueuePendingCallCount() {
    auto completion = std::make_shared<UInt64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedPendingCallCount{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueAwaitRegisterPromise(uint32_t promiseId) {
    return enqueueRegisterCompletion(
        [this, promiseId](const std::shared_ptr<RegisterTargetCompletion>& completion) {
          queue_.emplace_back(QueuedAwaitRegisterPromise{promiseId, completion});
        });
  }

  std::shared_ptr<UnitCompletion> enqueueCancelRegisterPromise(uint32_t promiseId) {
    return enqueueUnitCompletion([this, promiseId](const std::shared_ptr<UnitCompletion>& completion) {
      queue_.emplace_back(QueuedCancelRegisterPromise{promiseId, completion});
    });
  }

  std::shared_ptr<UnitCompletion> enqueueReleaseRegisterPromise(uint32_t promiseId) {
    return enqueueUnitCompletion([this, promiseId](const std::shared_ptr<UnitCompletion>& completion) {
      queue_.emplace_back(QueuedReleaseRegisterPromise{promiseId, completion});
    });
  }

  std::shared_ptr<UnitCompletion> enqueueAwaitUnitPromise(uint32_t promiseId) {
    return enqueueUnitCompletion([this, promiseId](const std::shared_ptr<UnitCompletion>& completion) {
      queue_.emplace_back(QueuedAwaitUnitPromise{promiseId, completion});
    });
  }

  std::shared_ptr<UnitCompletion> enqueueCancelUnitPromise(uint32_t promiseId) {
    return enqueueUnitCompletion([this, promiseId](const std::shared_ptr<UnitCompletion>& completion) {
      queue_.emplace_back(QueuedCancelUnitPromise{promiseId, completion});
    });
  }

  std::shared_ptr<UnitCompletion> enqueueReleaseUnitPromise(uint32_t promiseId) {
    return enqueueUnitCompletion([this, promiseId](const std::shared_ptr<UnitCompletion>& completion) {
      queue_.emplace_back(QueuedReleaseUnitPromise{promiseId, completion});
    });
  }

  std::shared_ptr<KjPromiseIdCompletion> enqueueKjAsyncSleepNanosStart(uint64_t delayNanos) {
    auto completion = std::make_shared<KjPromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeKjPromiseIdFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedKjAsyncSleepNanosStart{delayNanos, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueKjAsyncPromiseAwait(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedKjAsyncPromiseAwait{promiseId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueKjAsyncPromiseCancel(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedKjAsyncPromiseCancel{promiseId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueKjAsyncPromiseRelease(uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedKjAsyncPromiseRelease{promiseId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<KjPromiseIdCompletion> enqueueKjAsyncPromiseThenStart(
      uint32_t firstPromiseId, uint32_t secondPromiseId) {
    auto completion = std::make_shared<KjPromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeKjPromiseIdFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedKjAsyncPromiseThenStart{
          firstPromiseId, secondPromiseId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<KjPromiseIdCompletion> enqueueKjAsyncPromiseCatchStart(
      uint32_t promiseId, uint32_t fallbackPromiseId) {
    auto completion = std::make_shared<KjPromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeKjPromiseIdFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedKjAsyncPromiseCatchStart{
          promiseId, fallbackPromiseId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<KjPromiseIdCompletion> enqueueKjAsyncPromiseAllStart(
      std::vector<uint32_t> promiseIds) {
    auto completion = std::make_shared<KjPromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeKjPromiseIdFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedKjAsyncPromiseAllStart{std::move(promiseIds), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<KjPromiseIdCompletion> enqueueKjAsyncPromiseRaceStart(
      std::vector<uint32_t> promiseIds) {
    auto completion = std::make_shared<KjPromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeKjPromiseIdFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedKjAsyncPromiseRaceStart{std::move(promiseIds), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueKjAsyncTaskSetNew() {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedKjAsyncTaskSetNew{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueKjAsyncTaskSetRelease(uint32_t taskSetId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedKjAsyncTaskSetRelease{taskSetId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueKjAsyncTaskSetAddPromise(
      uint32_t taskSetId, uint32_t promiseId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedKjAsyncTaskSetAddPromise{taskSetId, promiseId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueKjAsyncTaskSetClear(uint32_t taskSetId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedKjAsyncTaskSetClear{taskSetId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<BoolCompletion> enqueueKjAsyncTaskSetIsEmpty(uint32_t taskSetId) {
    auto completion = std::make_shared<BoolCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeBoolFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedKjAsyncTaskSetIsEmpty{taskSetId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<KjPromiseIdCompletion> enqueueKjAsyncTaskSetOnEmptyStart(uint32_t taskSetId) {
    auto completion = std::make_shared<KjPromiseIdCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeKjPromiseIdFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedKjAsyncTaskSetOnEmptyStart{taskSetId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UInt64Completion> enqueueKjAsyncTaskSetErrorCount(uint32_t taskSetId) {
    auto completion = std::make_shared<UInt64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedKjAsyncTaskSetErrorCount{taskSetId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<OptionalStringCompletion> enqueueKjAsyncTaskSetTakeLastError(
      uint32_t taskSetId) {
    auto completion = std::make_shared<OptionalStringCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeOptionalStringFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedKjAsyncTaskSetTakeLastError{taskSetId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueuePump() {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedPump{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<AsyncUnitCompletion> enqueuePumpAsync(lean_object* promise) {
    auto completion = std::make_shared<AsyncUnitCompletion>(promise);
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeAsyncUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedPumpAsync{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatNewClient(std::string name) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatNewClient{std::move(name), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatNewServer(std::string name,
                                                                     uint32_t bootstrapTarget) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatNewServer{std::move(name), bootstrapTarget, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatNewServerWithBootstrapFactory(
      std::string name, b_lean_obj_arg bootstrapFactory) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    auto* factoryObj = const_cast<lean_object*>(bootstrapFactory);
    lean_mark_mt(factoryObj);
    lean_inc(factoryObj);
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        lean_dec(factoryObj);
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedMultiVatNewServerWithBootstrapFactory{std::move(name), factoryObj, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueMultiVatReleasePeer(uint32_t peerId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatReleasePeer{peerId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatBootstrap(
      uint32_t sourcePeerId, std::string host, bool unique) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedMultiVatBootstrap{sourcePeerId, std::move(host), unique, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatBootstrapPeer(
      uint32_t sourcePeerId, uint32_t targetPeerId, bool unique) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedMultiVatBootstrapPeer{sourcePeerId, targetPeerId, unique, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueMultiVatSetForwardingEnabled(bool enabled) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatSetForwardingEnabled{enabled, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueMultiVatResetForwardingStats() {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatResetForwardingStats{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UInt64Completion> enqueueMultiVatForwardCount() {
    auto completion = std::make_shared<UInt64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatForwardCount{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UInt64Completion> enqueueMultiVatThirdPartyTokenCount() {
    auto completion = std::make_shared<UInt64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatThirdPartyTokenCount{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UInt64Completion> enqueueMultiVatDeniedForwardCount() {
    auto completion = std::make_shared<UInt64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatDeniedForwardCount{completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UInt64Completion> enqueueMultiVatHasConnection(uint32_t fromPeerId,
                                                                 uint32_t toPeerId) {
    auto completion = std::make_shared<UInt64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatHasConnection{fromPeerId, toPeerId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueMultiVatSetRestorer(uint32_t peerId,
                                                             b_lean_obj_arg restorer) {
    auto completion = std::make_shared<UnitCompletion>();
    auto* restorerObj = const_cast<lean_object*>(restorer);
    lean_mark_mt(restorerObj);
    lean_inc(restorerObj);
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        lean_dec(restorerObj);
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatSetRestorer{peerId, restorerObj, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueMultiVatClearRestorer(uint32_t peerId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatClearRestorer{peerId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueMultiVatPublishSturdyRef(
      uint32_t hostPeerId, LeanByteArrayRef objectId, uint32_t targetId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatPublishSturdyRef{
          hostPeerId, std::move(objectId), targetId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatPublishSturdyRefStart(
      uint32_t hostPeerId, LeanByteArrayRef objectId, uint32_t targetId) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatPublishSturdyRefStart{
          hostPeerId, std::move(objectId), targetId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueMultiVatUnpublishSturdyRef(
      uint32_t hostPeerId, LeanByteArrayRef objectId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedMultiVatUnpublishSturdyRef{hostPeerId, std::move(objectId), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatUnpublishSturdyRefStart(
      uint32_t hostPeerId, LeanByteArrayRef objectId) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatUnpublishSturdyRefStart{
          hostPeerId, std::move(objectId), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueMultiVatClearPublishedSturdyRefs(uint32_t hostPeerId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatClearPublishedSturdyRefs{hostPeerId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatClearPublishedSturdyRefsStart(
      uint32_t hostPeerId) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatClearPublishedSturdyRefsStart{hostPeerId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UInt64Completion> enqueueMultiVatPublishedSturdyRefCount(uint32_t hostPeerId) {
    auto completion = std::make_shared<UInt64Completion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUInt64Failure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatPublishedSturdyRefCount{hostPeerId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatRestoreSturdyRef(
      uint32_t sourcePeerId, std::string host, bool unique, LeanByteArrayRef objectId) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatRestoreSturdyRef{
          sourcePeerId, std::move(host), unique, std::move(objectId), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatRestoreSturdyRefStart(
      uint32_t sourcePeerId, std::string host, bool unique, LeanByteArrayRef objectId) {
    auto completion = std::make_shared<RegisterTargetCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatRestoreSturdyRefStart{
          sourcePeerId, std::move(host), unique, std::move(objectId), completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<DiagnosticsCompletion> enqueueMultiVatGetDiagnostics(uint32_t peerId,
                                                                       lean_object* targetVatId) {
    auto completion = std::make_shared<DiagnosticsCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeDiagnosticsFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      lean_inc(targetVatId);
      queue_.emplace_back(QueuedMultiVatGetDiagnostics{peerId, targetVatId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueMultiVatConnectionBlock(uint32_t fromPeerId,
                                                                 uint32_t toPeerId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatConnectionBlock{fromPeerId, toPeerId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueMultiVatConnectionUnblock(uint32_t fromPeerId,
                                                                   uint32_t toPeerId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatConnectionUnblock{fromPeerId, toPeerId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion> enqueueMultiVatConnectionDisconnect(
      uint32_t fromPeerId, uint32_t toPeerId, uint8_t exceptionTypeTag, std::string message,
      std::vector<uint8_t> detailBytes) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(QueuedMultiVatConnectionDisconnect{
          fromPeerId, toPeerId, exceptionTypeTag, std::move(message), std::move(detailBytes),
          completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<ProtocolMessageCountsCompletion>
  enqueueMultiVatConnectionResolveDisembargoCounts(uint32_t fromPeerId, uint32_t toPeerId) {
    auto completion = std::make_shared<ProtocolMessageCountsCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeProtocolMessageCountsFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedMultiVatConnectionResolveDisembargoCounts{fromPeerId, toPeerId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<ByteArrayCompletion>
  enqueueMultiVatConnectionResolveDisembargoTrace(uint32_t fromPeerId, uint32_t toPeerId) {
    auto completion = std::make_shared<ByteArrayCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeByteArrayFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedMultiVatConnectionResolveDisembargoTrace{fromPeerId, toPeerId, completion});
    }
    notifyWorker();
    return completion;
  }

  std::shared_ptr<UnitCompletion>
  enqueueMultiVatConnectionResetResolveDisembargoTrace(uint32_t fromPeerId, uint32_t toPeerId) {
    auto completion = std::make_shared<UnitCompletion>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        return completion;
      }
      queue_.emplace_back(
          QueuedMultiVatConnectionResetResolveDisembargoTrace{fromPeerId, toPeerId, completion});
    }
    notifyWorker();
    return completion;
  }

  bool isWorkerThread() const {
    return std::this_thread::get_id() == worker_.get_id();
  }

  uint32_t retainTargetInline(uint32_t target) {
    return retainTarget(target);
  }

  void releaseTargetInline(uint32_t target) {
    releaseTarget(target);
  }

  void releaseTargetsInline(const std::vector<uint32_t>& targets) {
    releaseTargets(targets);
  }

  std::pair<uint32_t, uint32_t> newPromiseCapabilityInline() {
    return newPromiseCapability();
  }

  void promiseCapabilityFulfillInline(uint32_t fulfillerId, uint32_t target) {
    promiseCapabilityFulfill(fulfillerId, target);
  }

  void promiseCapabilityRejectInline(uint32_t fulfillerId, uint8_t exceptionTypeTag,
                                     std::string message,
                                     std::vector<uint8_t> detailBytes) {
    promiseCapabilityReject(fulfillerId, exceptionTypeTag, message, detailBytes.data(),
                            detailBytes.size());
  }

  void promiseCapabilityReleaseInline(uint32_t fulfillerId) {
    promiseCapabilityRelease(fulfillerId);
  }

  uint32_t kjAsyncSleepNanosStartInline(uint64_t delayNanos) {
    auto* ioProvider = ioProvider_;
    if (ioProvider == nullptr) {
      throw std::runtime_error("RPC runtime is not ready for KJ async sleep");
    }
    return kjAsyncSleepNanosStart(*ioProvider, delayNanos);
  }

  void kjAsyncPromiseCancelInline(uint32_t promiseId) {
    cancelKjAsyncPromise(promiseId);
  }

  void kjAsyncPromiseReleaseInline(uint32_t promiseId) {
    releaseKjAsyncPromise(promiseId);
  }

  uint32_t kjAsyncPromiseThenStartInline(uint32_t firstPromiseId, uint32_t secondPromiseId) {
    return kjAsyncPromiseThenStart(firstPromiseId, secondPromiseId);
  }

  uint32_t kjAsyncPromiseCatchStartInline(uint32_t promiseId, uint32_t fallbackPromiseId) {
    return kjAsyncPromiseCatchStart(promiseId, fallbackPromiseId);
  }

  uint32_t kjAsyncPromiseAllStartInline(std::vector<uint32_t> promiseIds) {
    return kjAsyncPromiseAllStart(std::move(promiseIds));
  }

  uint32_t kjAsyncPromiseRaceStartInline(std::vector<uint32_t> promiseIds) {
    return kjAsyncPromiseRaceStart(std::move(promiseIds));
  }

  uint32_t kjAsyncTaskSetNewInline() { return kjAsyncTaskSetNew(); }

  void kjAsyncTaskSetReleaseInline(uint32_t taskSetId) { kjAsyncTaskSetRelease(taskSetId); }

  void kjAsyncTaskSetAddPromiseInline(uint32_t taskSetId, uint32_t promiseId) {
    kjAsyncTaskSetAddPromise(taskSetId, promiseId);
  }

  void kjAsyncTaskSetClearInline(uint32_t taskSetId) { kjAsyncTaskSetClear(taskSetId); }

  bool kjAsyncTaskSetIsEmptyInline(uint32_t taskSetId) { return kjAsyncTaskSetIsEmpty(taskSetId); }

  uint32_t kjAsyncTaskSetOnEmptyStartInline(uint32_t taskSetId) {
    return kjAsyncTaskSetOnEmptyStart(taskSetId);
  }

  uint32_t kjAsyncTaskSetErrorCountInline(uint32_t taskSetId) {
    return kjAsyncTaskSetErrorCount(taskSetId);
  }

  kj::Maybe<std::string> kjAsyncTaskSetTakeLastErrorInline(uint32_t taskSetId) {
    return kjAsyncTaskSetTakeLastError(taskSetId);
  }

  void notifyWorker() {
    // Wake the runtime thread whether it's blocked on the legacy condition variable or on the KJ
    // event port. This keeps KJ async I/O progressing without requiring explicit Runtime.pump calls.
    queueCv_.notify_one();
    auto* port = eventPort_.load(std::memory_order_acquire);
    if (port != nullptr) {
      port->wake();
    }
  }

  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        return;
      }
      stopping_ = true;
    }
    notifyWorker();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

 private:
  static uint sanitizeMaxFdsPerMessage(uint32_t maxFdsPerMessage) {
    if (maxFdsPerMessage == 0) {
      return static_cast<uint>(kRuntimeDefaultMaxFdsPerMessage);
    }
    return static_cast<uint>(maxFdsPerMessage);
  }

  template <typename CompletionType, typename OnStoppingFn, typename EnqueueFn>
  std::shared_ptr<CompletionType> enqueueWithCompletion(OnStoppingFn&& onStopping,
                                                        EnqueueFn&& enqueue) {
    auto completion = std::make_shared<CompletionType>();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (stopping_) {
        std::forward<OnStoppingFn>(onStopping)(completion);
        return completion;
      }
      std::forward<EnqueueFn>(enqueue)(completion);
    }
    notifyWorker();
    return completion;
  }

  template <typename EnqueueFn>
  std::shared_ptr<RegisterTargetCompletion> enqueueRegisterCompletion(EnqueueFn&& enqueue) {
    return enqueueWithCompletion<RegisterTargetCompletion>(
        [this](const std::shared_ptr<RegisterTargetCompletion>& completion) {
          completeRegisterFailure(completion, "Capnp.Rpc runtime is shutting down");
        },
        std::forward<EnqueueFn>(enqueue));
  }

  template <typename EnqueueFn>
  std::shared_ptr<UnitCompletion> enqueueUnitCompletion(EnqueueFn&& enqueue) {
    return enqueueWithCompletion<UnitCompletion>(
        [this](const std::shared_ptr<UnitCompletion>& completion) {
          completeUnitFailure(completion, "Capnp.Rpc runtime is shutting down");
        },
        std::forward<EnqueueFn>(enqueue));
  }

  struct QueuedRawCall {
    uint32_t target;
    uint64_t interfaceId;
    uint16_t methodId;
    LeanByteArrayRef request;
    std::vector<uint32_t> requestCaps;
    std::shared_ptr<RawCallCompletion> completion;
  };

  struct QueuedRawCallData {
    uint32_t target;
    uint64_t interfaceId;
    uint16_t methodId;
    const uint8_t* requestData = nullptr;
    size_t requestSize = 0;
    std::shared_ptr<const void> requestOwner;
    std::vector<uint32_t> requestCaps;
    std::shared_ptr<RawCallCompletion> completion;
  };

  struct QueuedStartPendingCall {
    uint32_t target;
    uint64_t interfaceId;
    uint16_t methodId;
    LeanByteArrayRef request;
    std::vector<uint32_t> requestCaps;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedStartPendingCallData {
    uint32_t target;
    uint64_t interfaceId;
    uint16_t methodId;
    const uint8_t* requestData = nullptr;
    size_t requestSize = 0;
    std::shared_ptr<const void> requestOwner;
    std::vector<uint32_t> requestCaps;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedStartStreamingPendingCall {
    uint32_t target;
    uint64_t interfaceId;
    uint16_t methodId;
    LeanByteArrayRef request;
    std::vector<uint32_t> requestCaps;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedStartStreamingPendingCallData {
    uint32_t target;
    uint64_t interfaceId;
    uint16_t methodId;
    const uint8_t* requestData = nullptr;
    size_t requestSize = 0;
    std::shared_ptr<const void> requestOwner;
    std::vector<uint32_t> requestCaps;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedAwaitPendingCall {
    uint32_t pendingCallId;
    std::shared_ptr<RawCallCompletion> completion;
  };

  struct QueuedReleasePendingCall {
    uint32_t pendingCallId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedGetPipelinedCap {
    uint32_t pendingCallId;
    std::vector<uint16_t> pointerPath;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedStreamingCall {
    uint32_t target;
    uint64_t interfaceId;
    uint16_t methodId;
    LeanByteArrayRef request;
    std::vector<uint32_t> requestCaps;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedTargetGetFd {
    uint32_t target;
    std::shared_ptr<Int64Completion> completion;
  };

  struct QueuedTargetWhenResolved {
    uint32_t target;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedTargetWhenResolvedStart {
    uint32_t target;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedTargetWhenResolvedPoll {
    uint32_t target;
    std::shared_ptr<BoolCompletion> completion;
  };

  struct QueuedEnableTraceEncoder {
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedDisableTraceEncoder {
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedSetTraceEncoder {
    lean_object* encoder;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedCppCallWithAccept {
    uint32_t serverId;
    uint32_t listenerId;
    std::string address;
    uint32_t portHint;
    uint64_t interfaceId;
    uint16_t methodId;
    LeanByteArrayRef request;
    std::vector<uint32_t> requestCaps;
    std::shared_ptr<RawCallCompletion> completion;
  };

  struct QueuedCppCallPipelinedWithAccept {
    uint32_t serverId;
    uint32_t listenerId;
    std::string address;
    uint32_t portHint;
    uint64_t interfaceId;
    uint16_t methodId;
    LeanByteArrayRef request;
    std::vector<uint32_t> requestCaps;
    LeanByteArrayRef pipelinedRequest;
    std::vector<uint32_t> pipelinedRequestCaps;
    std::shared_ptr<RawCallCompletion> completion;
  };

  struct QueuedRegisterLoopbackTarget {
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedRegisterLoopbackBootstrapTarget {
    uint32_t bootstrapTarget;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedRegisterHandlerTarget {
    lean_object* handler;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedRegisterAdvancedHandlerTarget {
    lean_object* handler;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedRegisterTailCallHandlerTarget {
    lean_object* handler;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedRegisterTailCallTarget {
    uint32_t target;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedRegisterFdTarget {
    uint32_t fd;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedRegisterFdProbeTarget {
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedReleaseTarget {
    uint32_t target;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedReleaseTargets {
    std::vector<uint32_t> targets;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedRetainTarget {
    uint32_t target;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedNewPromiseCapability {
    std::shared_ptr<RegisterPairCompletion> completion;
  };

  struct QueuedPromiseCapabilityFulfill {
    uint32_t fulfillerId;
    uint32_t target;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedPromiseCapabilityReject {
    uint32_t fulfillerId;
    uint8_t exceptionTypeTag;
    std::string message;
    LeanByteArrayRef detailBytes;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedPromiseCapabilityRelease {
    uint32_t fulfillerId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedOrderingSetResolveHold {
    bool enabled;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedOrderingFlushHeldResolves {
    std::shared_ptr<UInt64Completion> completion;
  };

  struct QueuedOrderingHeldResolveCount {
    std::shared_ptr<UInt64Completion> completion;
  };

  struct QueuedConnectTarget {
    std::string address;
    uint32_t portHint;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedConnectTargetStart {
    std::string address;
    uint32_t portHint;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedConnectTargetFd {
    uint32_t fd;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedNewTransportPipe {
    std::shared_ptr<RegisterPairCompletion> completion;
  };

  struct QueuedNewTransportFromFd {
    uint32_t fd;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedNewTransportFromFdTake {
    uint32_t fd;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedReleaseTransport {
    uint32_t transportId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedTransportGetFd {
    uint32_t transportId;
    std::shared_ptr<Int64Completion> completion;
  };

  struct QueuedConnectTargetTransport {
    uint32_t transportId;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedListenLoopback {
    std::string address;
    uint32_t portHint;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedAcceptLoopback {
    uint32_t listenerId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedReleaseListener {
    uint32_t listenerId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedNewClient {
    std::string address;
    uint32_t portHint;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedNewClientStart {
    std::string address;
    uint32_t portHint;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedReleaseClient {
    uint32_t clientId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedClientBootstrap {
    uint32_t clientId;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedClientOnDisconnect {
    uint32_t clientId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedClientOnDisconnectStart {
    uint32_t clientId;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedClientSetFlowLimit {
    uint32_t clientId;
    uint64_t words;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedNewServer {
    uint32_t bootstrapTarget;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedNewServerWithBootstrapFactory {
    lean_object* bootstrapFactory;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedReleaseServer {
    uint32_t serverId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedServerListen {
    uint32_t serverId;
    std::string address;
    uint32_t portHint;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedServerAccept {
    uint32_t serverId;
    uint32_t listenerId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedServerAcceptStart {
    uint32_t serverId;
    uint32_t listenerId;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedServerAcceptFd {
    uint32_t serverId;
    uint32_t fd;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedServerAcceptTransport {
    uint32_t serverId;
    uint32_t transportId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedServerDrain {
    uint32_t serverId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedServerDrainStart {
    uint32_t serverId;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedClientQueueSize {
    uint32_t clientId;
    std::shared_ptr<UInt64Completion> completion;
  };

  struct QueuedClientQueueCount {
    uint32_t clientId;
    std::shared_ptr<UInt64Completion> completion;
  };

  struct QueuedClientOutgoingWaitNanos {
    uint32_t clientId;
    std::shared_ptr<UInt64Completion> completion;
  };

  struct QueuedTargetCount {
    std::shared_ptr<UInt64Completion> completion;
  };

  struct QueuedListenerCount {
    std::shared_ptr<UInt64Completion> completion;
  };

  struct QueuedClientCount {
    std::shared_ptr<UInt64Completion> completion;
  };

  struct QueuedServerCount {
    std::shared_ptr<UInt64Completion> completion;
  };

  struct QueuedPendingCallCount {
    std::shared_ptr<UInt64Completion> completion;
  };

  struct QueuedAwaitRegisterPromise {
    uint32_t promiseId;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedCancelRegisterPromise {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedReleaseRegisterPromise {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedAwaitUnitPromise {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedCancelUnitPromise {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedReleaseUnitPromise {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedKjAsyncSleepNanosStart {
    uint64_t delayNanos;
    std::shared_ptr<KjPromiseIdCompletion> completion;
  };

  struct QueuedKjAsyncPromiseAwait {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedKjAsyncPromiseCancel {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedKjAsyncPromiseRelease {
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedKjAsyncPromiseThenStart {
    uint32_t firstPromiseId;
    uint32_t secondPromiseId;
    std::shared_ptr<KjPromiseIdCompletion> completion;
  };

  struct QueuedKjAsyncPromiseCatchStart {
    uint32_t promiseId;
    uint32_t fallbackPromiseId;
    std::shared_ptr<KjPromiseIdCompletion> completion;
  };

  struct QueuedKjAsyncPromiseAllStart {
    std::vector<uint32_t> promiseIds;
    std::shared_ptr<KjPromiseIdCompletion> completion;
  };

  struct QueuedKjAsyncPromiseRaceStart {
    std::vector<uint32_t> promiseIds;
    std::shared_ptr<KjPromiseIdCompletion> completion;
  };

  struct QueuedKjAsyncTaskSetNew {
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedKjAsyncTaskSetRelease {
    uint32_t taskSetId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedKjAsyncTaskSetAddPromise {
    uint32_t taskSetId;
    uint32_t promiseId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedKjAsyncTaskSetClear {
    uint32_t taskSetId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedKjAsyncTaskSetIsEmpty {
    uint32_t taskSetId;
    std::shared_ptr<BoolCompletion> completion;
  };

  struct QueuedKjAsyncTaskSetOnEmptyStart {
    uint32_t taskSetId;
    std::shared_ptr<KjPromiseIdCompletion> completion;
  };

  struct QueuedKjAsyncTaskSetErrorCount {
    uint32_t taskSetId;
    std::shared_ptr<UInt64Completion> completion;
  };

  struct QueuedKjAsyncTaskSetTakeLastError {
    uint32_t taskSetId;
    std::shared_ptr<OptionalStringCompletion> completion;
  };

  struct QueuedPump {
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedPumpAsync {
    std::shared_ptr<AsyncUnitCompletion> completion;
  };

  struct QueuedMultiVatNewClient {
    std::string name;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedMultiVatNewServer {
    std::string name;
    uint32_t bootstrapTarget;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedMultiVatNewServerWithBootstrapFactory {
    std::string name;
    lean_object* bootstrapFactory;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedMultiVatReleasePeer {
    uint32_t peerId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedMultiVatBootstrap {
    uint32_t sourcePeerId;
    std::string host;
    bool unique;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedMultiVatBootstrapPeer {
    uint32_t sourcePeerId;
    uint32_t targetPeerId;
    bool unique;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedMultiVatSetForwardingEnabled {
    bool enabled;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedMultiVatResetForwardingStats {
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedMultiVatForwardCount {
    std::shared_ptr<UInt64Completion> completion;
  };

  struct QueuedMultiVatThirdPartyTokenCount {
    std::shared_ptr<UInt64Completion> completion;
  };

  struct QueuedMultiVatDeniedForwardCount {
    std::shared_ptr<UInt64Completion> completion;
  };

  struct QueuedMultiVatHasConnection {
    uint32_t fromPeerId;
    uint32_t toPeerId;
    std::shared_ptr<UInt64Completion> completion;
  };

  struct QueuedMultiVatSetRestorer {
    uint32_t peerId;
    lean_object* restorer;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedMultiVatClearRestorer {
    uint32_t peerId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedMultiVatPublishSturdyRef {
    uint32_t hostPeerId;
    LeanByteArrayRef objectId;
    uint32_t targetId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedMultiVatPublishSturdyRefStart {
    uint32_t hostPeerId;
    LeanByteArrayRef objectId;
    uint32_t targetId;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedMultiVatUnpublishSturdyRef {
    uint32_t hostPeerId;
    LeanByteArrayRef objectId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedMultiVatUnpublishSturdyRefStart {
    uint32_t hostPeerId;
    LeanByteArrayRef objectId;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedMultiVatClearPublishedSturdyRefs {
    uint32_t hostPeerId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedMultiVatClearPublishedSturdyRefsStart {
    uint32_t hostPeerId;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedMultiVatPublishedSturdyRefCount {
    uint32_t hostPeerId;
    std::shared_ptr<UInt64Completion> completion;
  };

  struct QueuedMultiVatRestoreSturdyRef {
    uint32_t sourcePeerId;
    std::string host;
    bool unique;
    LeanByteArrayRef objectId;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedMultiVatRestoreSturdyRefStart {
    uint32_t sourcePeerId;
    std::string host;
    bool unique;
    LeanByteArrayRef objectId;
    std::shared_ptr<RegisterTargetCompletion> completion;
  };

  struct QueuedMultiVatGetDiagnostics {
    uint32_t peerId;
    lean_object* targetVatId;
    std::shared_ptr<DiagnosticsCompletion> completion;
  };

  struct QueuedMultiVatConnectionBlock {
    uint32_t fromPeerId;
    uint32_t toPeerId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedMultiVatConnectionUnblock {
    uint32_t fromPeerId;
    uint32_t toPeerId;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedMultiVatConnectionDisconnect {
    uint32_t fromPeerId;
    uint32_t toPeerId;
    uint8_t exceptionTypeTag;
    std::string message;
    std::vector<uint8_t> detailBytes;
    std::shared_ptr<UnitCompletion> completion;
  };

  struct QueuedMultiVatConnectionResolveDisembargoCounts {
    uint32_t fromPeerId;
    uint32_t toPeerId;
    std::shared_ptr<ProtocolMessageCountsCompletion> completion;
  };

  struct QueuedMultiVatConnectionResolveDisembargoTrace {
    uint32_t fromPeerId;
    uint32_t toPeerId;
    std::shared_ptr<ByteArrayCompletion> completion;
  };

  struct QueuedMultiVatConnectionResetResolveDisembargoTrace {
    uint32_t fromPeerId;
    uint32_t toPeerId;
    std::shared_ptr<UnitCompletion> completion;
  };

  using QueuedOperation =
      std::variant<QueuedRawCall, QueuedRawCallData, QueuedStartPendingCall,
                   QueuedStartPendingCallData, QueuedStartStreamingPendingCall,
                   QueuedStartStreamingPendingCallData, QueuedAwaitPendingCall,
                   QueuedReleasePendingCall, QueuedGetPipelinedCap, QueuedStreamingCall,
                   QueuedTargetGetFd, QueuedTargetWhenResolved, QueuedTargetWhenResolvedStart,
                   QueuedTargetWhenResolvedPoll,
                   QueuedEnableTraceEncoder,
                   QueuedDisableTraceEncoder, QueuedSetTraceEncoder, QueuedCppCallWithAccept,
                   QueuedCppCallPipelinedWithAccept,
                   QueuedRegisterLoopbackTarget, QueuedRegisterLoopbackBootstrapTarget,
                   QueuedRegisterHandlerTarget, QueuedRegisterAdvancedHandlerTarget,
                   QueuedRegisterTailCallHandlerTarget,
                   QueuedRegisterTailCallTarget, QueuedRegisterFdTarget,
                   QueuedRegisterFdProbeTarget,
                   QueuedReleaseTarget, QueuedReleaseTargets, QueuedRetainTarget,
                   QueuedNewPromiseCapability, QueuedPromiseCapabilityFulfill,
                   QueuedPromiseCapabilityReject, QueuedPromiseCapabilityRelease,
                   QueuedOrderingSetResolveHold, QueuedOrderingFlushHeldResolves,
                   QueuedOrderingHeldResolveCount,
                   QueuedConnectTarget, QueuedConnectTargetStart, QueuedConnectTargetFd,
                   QueuedNewTransportPipe, QueuedNewTransportFromFd, QueuedNewTransportFromFdTake,
                   QueuedReleaseTransport, QueuedTransportGetFd,
                   QueuedConnectTargetTransport,
                   QueuedListenLoopback,
                   QueuedAcceptLoopback, QueuedReleaseListener, QueuedNewClient,
                   QueuedNewClientStart, QueuedReleaseClient, QueuedClientBootstrap,
                   QueuedClientOnDisconnect, QueuedClientOnDisconnectStart,
                   QueuedClientSetFlowLimit,
                   QueuedNewServer, QueuedNewServerWithBootstrapFactory,
                   QueuedReleaseServer, QueuedServerListen, QueuedServerAccept,
                   QueuedServerAcceptStart, QueuedServerAcceptFd, QueuedServerAcceptTransport,
                   QueuedServerDrain, QueuedServerDrainStart,
                   QueuedClientQueueSize, QueuedClientQueueCount,
                   QueuedClientOutgoingWaitNanos, QueuedTargetCount,
                   QueuedListenerCount, QueuedClientCount, QueuedServerCount,
                   QueuedPendingCallCount, QueuedAwaitRegisterPromise,
                   QueuedCancelRegisterPromise, QueuedReleaseRegisterPromise,
                   QueuedAwaitUnitPromise, QueuedCancelUnitPromise,
                   QueuedReleaseUnitPromise, QueuedKjAsyncSleepNanosStart,
                   QueuedKjAsyncPromiseAwait, QueuedKjAsyncPromiseCancel,
                   QueuedKjAsyncPromiseRelease, QueuedKjAsyncPromiseThenStart,
                   QueuedKjAsyncPromiseCatchStart, QueuedKjAsyncPromiseAllStart,
                   QueuedKjAsyncPromiseRaceStart, QueuedKjAsyncTaskSetNew,
                   QueuedKjAsyncTaskSetRelease, QueuedKjAsyncTaskSetAddPromise,
                   QueuedKjAsyncTaskSetClear, QueuedKjAsyncTaskSetIsEmpty,
                   QueuedKjAsyncTaskSetOnEmptyStart, QueuedKjAsyncTaskSetErrorCount,
                   QueuedKjAsyncTaskSetTakeLastError, QueuedPump, QueuedPumpAsync,
                   QueuedMultiVatNewClient, QueuedMultiVatNewServer,
                   QueuedMultiVatNewServerWithBootstrapFactory, QueuedMultiVatReleasePeer,
                   QueuedMultiVatBootstrap, QueuedMultiVatBootstrapPeer,
                   QueuedMultiVatSetForwardingEnabled, QueuedMultiVatResetForwardingStats,
                   QueuedMultiVatForwardCount, QueuedMultiVatThirdPartyTokenCount,
                   QueuedMultiVatDeniedForwardCount,
                   QueuedMultiVatHasConnection, QueuedMultiVatSetRestorer,
                   QueuedMultiVatClearRestorer, QueuedMultiVatPublishSturdyRef,
                   QueuedMultiVatPublishSturdyRefStart, QueuedMultiVatUnpublishSturdyRef,
                   QueuedMultiVatUnpublishSturdyRefStart,
                   QueuedMultiVatClearPublishedSturdyRefs,
                   QueuedMultiVatClearPublishedSturdyRefsStart,
                   QueuedMultiVatPublishedSturdyRefCount,
                   QueuedMultiVatRestoreSturdyRef, QueuedMultiVatRestoreSturdyRefStart,
                   QueuedMultiVatGetDiagnostics,
                   QueuedMultiVatConnectionBlock,
                   QueuedMultiVatConnectionUnblock, QueuedMultiVatConnectionDisconnect,
                   QueuedMultiVatConnectionResolveDisembargoCounts,
                   QueuedMultiVatConnectionResolveDisembargoTrace,
                   QueuedMultiVatConnectionResetResolveDisembargoTrace>;

  struct LoopbackPeer {
    kj::Own<kj::AsyncCapabilityStream> clientStream;
    kj::Own<kj::AsyncCapabilityStream> serverStream;
    kj::Own<capnp::TwoPartyVatNetwork> serverNetwork;
    kj::Own<TwoPartyRpcSystem> serverRpcSystem;
    kj::Own<capnp::TwoPartyClient> client;
  };

  struct NetworkClientPeer {
    kj::Own<kj::AsyncCapabilityStream> stream;
    kj::Own<capnp::TwoPartyVatNetwork> network;
    kj::Own<TwoPartyRpcSystem> rpcSystem;
  };

  struct NetworkServerPeer {
    kj::Maybe<kj::Own<kj::AsyncIoStream>> ioConnection;
    kj::Maybe<kj::Own<kj::AsyncCapabilityStream>> capConnection;
    kj::Own<capnp::TwoPartyVatNetwork> network;
    kj::Own<TwoPartyRpcSystem> rpcSystem;
  };

  struct MultiVatPeer {
    std::string name;
    GenericVat* vat = nullptr;
    kj::Own<GenericRpcSystem> rpcSystem;
    lean_object* sturdyRefRestorer = nullptr;

    ~MultiVatPeer() {
      if (sturdyRefRestorer != nullptr) {
        lean_dec(sturdyRefRestorer);
        sturdyRefRestorer = nullptr;
      }
    }
  };

  class LeanGenericBootstrapFactory final : public capnp::BootstrapFactory<GenericVatId> {
   public:
    LeanGenericBootstrapFactory(RuntimeLoop& runtime, lean_object* bootstrapFactory)
        : runtime_(runtime), bootstrapFactory_(bootstrapFactory) {
      lean_inc(bootstrapFactory_);
    }

    ~LeanGenericBootstrapFactory() { lean_dec(bootstrapFactory_); }

    capnp::Capability::Client createFor(GenericVatId::Reader clientId) override {
      auto decoded = decodeLeanVatId(clientId);
      lean_inc(bootstrapFactory_);
      auto ioResult = lean_apply_3(bootstrapFactory_, lean_mk_string(decoded.host.c_str()),
                                   lean_box(decoded.unique ? 1 : 0), lean_box(0));
      if (lean_io_result_is_error(ioResult)) {
        lean_dec(ioResult);
        throw std::runtime_error("Lean generic bootstrap factory returned IO error");
      }

      auto targetObj = lean_io_result_take_value(ioResult);
      uint32_t targetId = lean_unbox_uint32(targetObj);
      lean_dec(targetObj);

      auto targetIt = runtime_.targets_.find(targetId);
      if (targetIt == runtime_.targets_.end()) {
        throw std::runtime_error(
            "unknown RPC bootstrap capability id from Lean generic bootstrap factory: " +
            std::to_string(targetId));
      }
      return targetIt->second;
    }

   private:
    RuntimeLoop& runtime_;
    lean_object* bootstrapFactory_;
  };

  class LeanBootstrapFactory final : public capnp::BootstrapFactory<capnp::rpc::twoparty::VatId> {
   public:
    LeanBootstrapFactory(RuntimeLoop& runtime, lean_object* bootstrapFactory)
        : runtime_(runtime), bootstrapFactory_(bootstrapFactory) {
      lean_inc(bootstrapFactory_);
    }

    ~LeanBootstrapFactory() { lean_dec(bootstrapFactory_); }

    capnp::Capability::Client createFor(
        capnp::rpc::twoparty::VatId::Reader clientId) override {
      auto side = static_cast<uint16_t>(clientId.getSide());
      lean_inc(bootstrapFactory_);
      auto ioResult = lean_apply_2(bootstrapFactory_, lean_box(static_cast<size_t>(side)),
                                   lean_box(0));
      if (lean_io_result_is_error(ioResult)) {
        lean_dec(ioResult);
        throw std::runtime_error("Lean bootstrap factory returned IO error");
      }

      auto targetObj = lean_io_result_take_value(ioResult);
      uint32_t targetId = lean_unbox_uint32(targetObj);
      lean_dec(targetObj);

      auto targetIt = runtime_.targets_.find(targetId);
      if (targetIt == runtime_.targets_.end()) {
        throw std::runtime_error(
            "unknown RPC bootstrap capability id from Lean bootstrap factory: " +
            std::to_string(targetId));
      }
      return targetIt->second;
    }

   private:
    RuntimeLoop& runtime_;
    lean_object* bootstrapFactory_;
  };

  struct RuntimeServer {
    explicit RuntimeServer(capnp::Capability::Client bootstrap)
        : bootstrap(kj::mv(bootstrap)) {}
    explicit RuntimeServer(
        kj::Own<capnp::BootstrapFactory<capnp::rpc::twoparty::VatId>> bootstrapFactory)
        : bootstrapFactory(kj::mv(bootstrapFactory)) {}
    kj::Maybe<capnp::Capability::Client> bootstrap;
    kj::Maybe<kj::Own<capnp::BootstrapFactory<capnp::rpc::twoparty::VatId>>> bootstrapFactory;
    kj::Vector<kj::Own<NetworkServerPeer>> peers;
  };

  struct PendingCall {
    PendingCall(kj::Promise<capnp::Response<capnp::AnyPointer>>&& promise,
                capnp::AnyPointer::Pipeline&& pipeline, kj::Own<kj::Canceler>&& canceler)
        : promise(kj::mv(promise)), pipeline(kj::mv(pipeline)), canceler(kj::mv(canceler)) {}
    PendingCall(PendingCall&&) = default;
    PendingCall& operator=(PendingCall&&) = default;
    PendingCall(const PendingCall&) = delete;
    PendingCall& operator=(const PendingCall&) = delete;

    kj::Promise<capnp::Response<capnp::AnyPointer>> promise;
    capnp::AnyPointer::Pipeline pipeline;
    kj::Own<kj::Canceler> canceler;
  };

  struct PendingRegisterPromise {
    PendingRegisterPromise(kj::Promise<uint32_t>&& promise, kj::Own<kj::Canceler>&& canceler)
        : promise(kj::mv(promise)), canceler(kj::mv(canceler)) {}
    PendingRegisterPromise(PendingRegisterPromise&&) = default;
    PendingRegisterPromise& operator=(PendingRegisterPromise&&) = default;
    PendingRegisterPromise(const PendingRegisterPromise&) = delete;
    PendingRegisterPromise& operator=(const PendingRegisterPromise&) = delete;

    kj::Promise<uint32_t> promise;
    kj::Own<kj::Canceler> canceler;
  };

  struct PendingUnitPromise {
    PendingUnitPromise(kj::Promise<void>&& promise, kj::Own<kj::Canceler>&& canceler)
        : promise(kj::mv(promise)), canceler(kj::mv(canceler)) {}
    PendingUnitPromise(PendingUnitPromise&&) = default;
    PendingUnitPromise& operator=(PendingUnitPromise&&) = default;
    PendingUnitPromise(const PendingUnitPromise&) = delete;
    PendingUnitPromise& operator=(const PendingUnitPromise&) = delete;

    kj::Promise<void> promise;
    kj::Own<kj::Canceler> canceler;
  };

  struct RuntimeKjAsyncTaskSet {
    class ErrorHandler final : public kj::TaskSet::ErrorHandler {
     public:
      explicit ErrorHandler(RuntimeKjAsyncTaskSet& state) : state_(state) {}

      void taskFailed(kj::Exception&& exception) override {
        std::lock_guard<std::mutex> lock(state_.mutex);
        ++state_.errorCount;
        state_.lastError = describeKjException(exception);
      }

     private:
      RuntimeKjAsyncTaskSet& state_;
    };

    RuntimeKjAsyncTaskSet() : errorHandler(*this), tasks(errorHandler) {}

    std::mutex mutex;
    uint32_t errorCount = 0;
    std::string lastError;
    ErrorHandler errorHandler;
    kj::TaskSet tasks;
  };

  struct PendingPromiseCapability {
    PendingPromiseCapability(
        kj::Own<kj::PromiseFulfiller<kj::Own<capnp::ClientHook>>>&& fulfiller,
        uint32_t promiseTarget)
        : fulfiller(kj::mv(fulfiller)), promiseTarget(promiseTarget) {}
    PendingPromiseCapability(PendingPromiseCapability&&) = default;
    PendingPromiseCapability& operator=(PendingPromiseCapability&&) = default;
    PendingPromiseCapability(const PendingPromiseCapability&) = delete;
    PendingPromiseCapability& operator=(const PendingPromiseCapability&) = delete;

    kj::Own<kj::PromiseFulfiller<kj::Own<capnp::ClientHook>>> fulfiller;
    uint32_t promiseTarget = 0;
  };

  struct HeldPromiseCapabilityResolve {
    uint32_t fulfillerId = 0;
    uint32_t target = 0;
  };

  void retainPeerOwnership(
      uint32_t sourceTarget, uint32_t retainedTarget,
      std::unordered_map<uint32_t, uint32_t>& ownerByTarget,
      std::unordered_map<uint32_t, uint32_t>& ownerRefCounts) {
    auto ownerIt = ownerByTarget.find(sourceTarget);
    if (ownerIt == ownerByTarget.end()) {
      return;
    }
    auto owner = ownerIt->second;
    ownerByTarget.emplace(retainedTarget, owner);
    auto refIt = ownerRefCounts.find(owner);
    if (refIt == ownerRefCounts.end()) {
      ownerRefCounts.emplace(owner, 1);
      refIt = ownerRefCounts.find(owner);
    }
    ++(refIt->second);
  }

  template <typename PeerMap>
  void releasePeerOwnership(
      uint32_t target, PeerMap& peers, std::unordered_map<uint32_t, uint32_t>& ownerByTarget,
      std::unordered_map<uint32_t, uint32_t>& ownerRefCounts) {
    auto ownerIt = ownerByTarget.find(target);
    if (ownerIt == ownerByTarget.end()) {
      return;
    }
    auto owner = ownerIt->second;
    ownerByTarget.erase(ownerIt);

    auto refIt = ownerRefCounts.find(owner);
    if (refIt == ownerRefCounts.end()) {
      return;
    }
    if (refIt->second > 1) {
      --(refIt->second);
      return;
    }
    ownerRefCounts.erase(refIt);
    peers.erase(owner);
  }

  struct EncodedRequestForLean {
    lean_object* requestObj;
    lean_object* requestCapsObj;
    std::vector<uint32_t> requestCapIds;
  };

  EncodedRequestForLean encodeRequestForLean(
      capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer>& context) {
    capnp::MallocMessageBuilder requestMessage;
    capnp::BuilderCapabilityTable requestCapTable;
    requestCapTable
        .imbue(requestMessage.getRoot<capnp::AnyPointer>())
        .setAs<capnp::AnyPointer>(context.getParams().getAs<capnp::AnyPointer>());

    auto requestWords = capnp::messageToFlatArray(requestMessage);
    auto requestBytes = requestWords.asBytes();
    auto requestObj = mkByteArrayCopy(reinterpret_cast<const uint8_t*>(requestBytes.begin()),
                                      requestBytes.size());

    std::vector<uint32_t> requestCapIds;
    auto requestCapEntries = requestCapTable.getTable();
    requestCapIds.reserve(requestCapEntries.size());
    const size_t requestCapsSize = requestCapEntries.size() * 4;
    auto requestCapsObj = lean_alloc_sarray(1, requestCapsSize, requestCapsSize);
    auto* requestCapsData = reinterpret_cast<uint8_t*>(lean_sarray_cptr(requestCapsObj));
    size_t requestCapsOffset = 0;
    for (auto& maybeHook : requestCapEntries) {
      uint32_t capId = 0;
      KJ_IF_SOME(hook, maybeHook) {
        auto cap = capnp::Capability::Client(hook->addRef());
        capId = addTarget(kj::mv(cap));
        requestCapIds.push_back(capId);
      }
      requestCapsData[requestCapsOffset] = static_cast<uint8_t>(capId & 0xff);
      requestCapsData[requestCapsOffset + 1] = static_cast<uint8_t>((capId >> 8) & 0xff);
      requestCapsData[requestCapsOffset + 2] = static_cast<uint8_t>((capId >> 16) & 0xff);
      requestCapsData[requestCapsOffset + 3] = static_cast<uint8_t>((capId >> 24) & 0xff);
      requestCapsOffset += 4;
    }
    lean_sarray_set_size(requestCapsObj, requestCapsOffset);
    return {requestObj, requestCapsObj, kj::mv(requestCapIds)};
  }

  void dropRequestCaps(const std::vector<uint32_t>& requestCapIds) {
    for (auto capId : requestCapIds) {
      dropTargetIfPresent(capId);
    }
  }

  void dropRequestCapsExcept(const std::vector<uint32_t>& requestCapIds,
                             const std::vector<uint32_t>& retainedCaps) {
    if (retainedCaps.empty()) {
      dropRequestCaps(requestCapIds);
      return;
    }

    kj::HashSet<uint32_t> retained;
    retained.reserve(retainedCaps.size());
    for (auto capId : retainedCaps) {
      if (capId != 0) {
        retained.insert(capId);
      }
    }
    for (auto capId : requestCapIds) {
      if (!retained.contains(capId)) {
        dropTargetIfPresent(capId);
      }
    }
  }

  void dropRequestCapsExcept(const std::vector<uint32_t>& requestCapIds,
                             uint32_t retainedCapId) {
    for (auto capId : requestCapIds) {
      if (capId == 0 || capId == retainedCapId) {
        continue;
      }
      dropTargetIfPresent(capId);
    }
  }

  template <typename SetPayloadFn>
  void withDecodedPayloadAndCapTable(const uint8_t* payloadBytesData, size_t payloadBytesSize,
                                     const std::vector<uint32_t>& payloadCapIds,
                                     const char* unknownCapErrorPrefix,
                                     SetPayloadFn&& setPayload) {
    kj::ArrayPtr<const kj::byte> payloadBytes(
        reinterpret_cast<const kj::byte*>(payloadBytesData), payloadBytesSize);
    kj::ArrayInputStream input(payloadBytes);
    capnp::ReaderOptions options;
    options.traversalLimitInWords = 1ull << 30;
    capnp::InputStreamMessageReader reader(input, options);
    auto payloadRoot = reader.getRoot<capnp::AnyPointer>();

    if (payloadCapIds.empty()) {
      setPayload(payloadRoot);
      return;
    }

    auto capTableBuilder =
        kj::heapArrayBuilder<kj::Maybe<kj::Own<capnp::ClientHook>>>(payloadCapIds.size());
    for (auto capId : payloadCapIds) {
      if (capId == 0) {
        capTableBuilder.add(kj::none);
        continue;
      }
      auto capIt = targets_.find(capId);
      if (capIt == targets_.end()) {
        throw std::runtime_error(std::string(unknownCapErrorPrefix) + std::to_string(capId));
      }
      capnp::Capability::Client cap = capIt->second;
      capTableBuilder.add(capnp::ClientHook::from(kj::mv(cap)));
    }

    capnp::ReaderCapabilityTable payloadCapTable(capTableBuilder.finish());
    setPayload(payloadCapTable.imbue(payloadRoot));
  }

  void setContextResultsFromPayloadWithCapIds(
      capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer>& context,
      const uint8_t* responseBytesData, size_t responseBytesSize,
      const std::vector<uint32_t>& responseCapIds, const char* unknownCapErrorPrefix) {
    withDecodedPayloadAndCapTable(
        responseBytesData, responseBytesSize, responseCapIds, unknownCapErrorPrefix,
        [&context](capnp::AnyPointer::Reader responseRoot) {
          context.getResults().setAs<capnp::AnyPointer>(responseRoot);
        });
  }

  void setContextResultsFromPayload(
      capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer>& context,
      const uint8_t* responseBytesData, size_t responseBytesSize,
      const uint8_t* responseCapsData, size_t responseCapsSize,
      const char* unknownCapErrorPrefix) {
    auto responseCapIds = decodeCapTable(responseCapsData, responseCapsSize);
    setContextResultsFromPayloadWithCapIds(context, responseBytesData, responseBytesSize,
                                           responseCapIds, unknownCapErrorPrefix);
  }

  void setContextResultsFromPayload(
      capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer>& context,
      const std::vector<uint8_t>& responseBytesCopy, const std::vector<uint8_t>& responseCapsCopy,
      const char* unknownCapErrorPrefix) {
    setContextResultsFromPayload(context, responseBytesCopy.data(), responseBytesCopy.size(),
                                 responseCapsCopy.data(), responseCapsCopy.size(),
                                 unknownCapErrorPrefix);
  }

  void setContextPipelineFromPayload(
      capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer>& context,
      const uint8_t* pipelineBytesData, size_t pipelineBytesSize,
      const uint8_t* pipelineCapsData, size_t pipelineCapsSize,
      const char* unknownCapErrorPrefix) {
    if (pipelineBytesSize == 0) {
      if (pipelineCapsSize != 0) {
        throw std::runtime_error(
            "RPC pipeline capability table requires a non-empty payload");
      }
      return;
    }

    std::vector<uint32_t> pipelineCapIds;
    if (pipelineCapsSize != 0) {
      pipelineCapIds = decodeCapTable(pipelineCapsData, pipelineCapsSize);
    }

    capnp::PipelineBuilder<capnp::AnyPointer> pipelineBuilder;
    withDecodedPayloadAndCapTable(
        pipelineBytesData, pipelineBytesSize, pipelineCapIds, unknownCapErrorPrefix,
        [&pipelineBuilder](capnp::AnyPointer::Reader pipelineRoot) {
          pipelineBuilder.setAs<capnp::AnyPointer>(pipelineRoot);
        });
    context.setPipeline(pipelineBuilder.build());
  }

  void setContextPipelineFromPayload(
      capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer>& context,
      const std::vector<uint8_t>& pipelineBytesCopy,
      const std::vector<uint8_t>& pipelineCapsCopy, const char* unknownCapErrorPrefix) {
    setContextPipelineFromPayload(context, pipelineBytesCopy.data(), pipelineBytesCopy.size(),
                                  pipelineCapsCopy.data(), pipelineCapsCopy.size(),
                                  unknownCapErrorPrefix);
  }

  class LeanCapabilityServer final : public capnp::Capability::Server {
   public:
    LeanCapabilityServer(RuntimeLoop& runtime,
                         std::shared_ptr<std::atomic<uint32_t>> targetId,
                         lean_object* handler)
        : runtime_(runtime), targetId_(kj::mv(targetId)), handler_(handler) {
      lean_inc(handler_);
    }

    ~LeanCapabilityServer() { lean_dec(handler_); }

    DispatchCallResult dispatchCall(
        uint64_t interfaceId, uint16_t methodId,
        capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer> context) override {
      auto encodedRequest = runtime_.encodeRequestForLean(context);
      auto requestObj = encodedRequest.requestObj;
      auto requestCapsObj = encodedRequest.requestCapsObj;
      auto requestCapIds = kj::mv(encodedRequest.requestCapIds);

      auto targetId = targetId_->load(std::memory_order_relaxed);
      lean_inc(handler_);
      auto ioResult = lean_apply_6(handler_, lean_box_uint32(targetId), lean_box_uint64(interfaceId),
                                   lean_box(static_cast<size_t>(methodId)), requestObj,
                                   requestCapsObj, lean_box(0));
      if (lean_io_result_is_error(ioResult)) {
        runtime_.dropRequestCapsExcept(requestCapIds, 0);
        lean_dec(ioResult);
        throw std::runtime_error("Lean RPC handler returned IO error");
      }

      auto resultPair = lean_io_result_take_value(ioResult);
      auto responseObj = lean_ctor_get(resultPair, 0);
      lean_inc(responseObj);
      auto responseCapsObj = lean_ctor_get(resultPair, 1);
      lean_inc(responseCapsObj);
      lean_dec(resultPair);

      const auto responseBytesSize = lean_sarray_size(responseObj);
      const auto* responseBytesData =
          reinterpret_cast<const uint8_t*>(lean_sarray_cptr(responseObj));
      const auto responseCapsSize = lean_sarray_size(responseCapsObj);
      const auto* responseCapsData =
          reinterpret_cast<const uint8_t*>(lean_sarray_cptr(responseCapsObj));

      try {
        auto responseCapIds = decodeCapTable(responseCapsData, responseCapsSize);
        runtime_.dropRequestCapsExcept(requestCapIds, responseCapIds);
        runtime_.setContextResultsFromPayloadWithCapIds(
            context, responseBytesData, responseBytesSize, responseCapIds,
            "unknown RPC response capability id from Lean handler: ");
      } catch (...) {
        runtime_.dropRequestCaps(requestCapIds);
        lean_dec(responseObj);
        lean_dec(responseCapsObj);
        throw;
      }

      lean_dec(responseObj);
      lean_dec(responseCapsObj);
      return {kj::READY_NOW, false};
    }

  private:
    RuntimeLoop& runtime_;
    std::shared_ptr<std::atomic<uint32_t>> targetId_;
    lean_object* handler_;
  };

  class LeanTailCallHandlerServer final : public capnp::Capability::Server {
   public:
    LeanTailCallHandlerServer(RuntimeLoop& runtime,
                              std::shared_ptr<std::atomic<uint32_t>> targetId,
                              lean_object* handler)
        : runtime_(runtime), targetId_(kj::mv(targetId)), handler_(handler) {
      lean_inc(handler_);
    }

    ~LeanTailCallHandlerServer() { lean_dec(handler_); }

    DispatchCallResult dispatchCall(
        uint64_t interfaceId, uint16_t methodId,
        capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer> context) override {
      auto encodedRequest = runtime_.encodeRequestForLean(context);
      auto requestObj = encodedRequest.requestObj;
      auto requestCapsObj = encodedRequest.requestCapsObj;
      auto requestCapIds = kj::mv(encodedRequest.requestCapIds);

      auto targetId = targetId_->load(std::memory_order_relaxed);
      lean_inc(handler_);
      auto ioResult = lean_apply_6(handler_, lean_box_uint32(targetId), lean_box_uint64(interfaceId),
                                   lean_box(static_cast<size_t>(methodId)), requestObj,
                                   requestCapsObj, lean_box(0));
      if (lean_io_result_is_error(ioResult)) {
        runtime_.dropRequestCapsExcept(requestCapIds, 0);
        lean_dec(ioResult);
        throw std::runtime_error("Lean RPC tail-call handler returned IO error");
      }

      auto targetObj = lean_io_result_take_value(ioResult);
      uint32_t forwardTargetId = lean_unbox_uint32(targetObj);
      lean_dec(targetObj);

      auto targetIt = runtime_.targets_.find(forwardTargetId);
      if (targetIt == runtime_.targets_.end()) {
        runtime_.dropRequestCapsExcept(requestCapIds, 0);
        throw std::runtime_error("unknown RPC tail-call target capability id from Lean handler: " +
                                 std::to_string(forwardTargetId));
      }

      runtime_.dropRequestCapsExcept(requestCapIds, forwardTargetId);

      auto requestBuilder = targetIt->second.typelessRequest(interfaceId, methodId, kj::none, {});
      requestBuilder.setAs<capnp::AnyPointer>(context.getParams().getAs<capnp::AnyPointer>());
      return {context.tailCall(kj::mv(requestBuilder)), false};
    }

   private:
    RuntimeLoop& runtime_;
    std::shared_ptr<std::atomic<uint32_t>> targetId_;
    lean_object* handler_;
  };

  class LeanAdvancedCapabilityServer final : public capnp::Capability::Server {
   public:
    LeanAdvancedCapabilityServer(RuntimeLoop& runtime,
                                 std::shared_ptr<std::atomic<uint32_t>> targetId,
                                 lean_object* handler)
        : runtime_(runtime), targetId_(kj::mv(targetId)), handler_(handler) {
      lean_inc(handler_);
    }

    ~LeanAdvancedCapabilityServer() { lean_dec(handler_); }

    DispatchCallResult dispatchCall(
        uint64_t interfaceId, uint16_t methodId,
        capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer> context) override {
      auto encodedRequest = runtime_.encodeRequestForLean(context);
      auto requestObj = encodedRequest.requestObj;
      auto requestCapsObj = encodedRequest.requestCapsObj;
      auto requestCapIds = kj::mv(encodedRequest.requestCapIds);
      auto cleanupState =
          std::make_shared<RequestCapCleanupState>(std::move(requestCapIds));

      auto targetId = targetId_->load(std::memory_order_relaxed);
      lean_inc(handler_);
      auto ioResult = lean_apply_6(handler_, lean_box_uint32(targetId), lean_box_uint64(interfaceId),
                                   lean_box(static_cast<size_t>(methodId)), requestObj,
                                   requestCapsObj, lean_box(0));
      if (lean_io_result_is_error(ioResult)) {
        cleanupRequestCaps(cleanupState, {});
        lean_dec(ioResult);
        throw std::runtime_error("Lean RPC advanced handler returned IO error");
      }

      try {
        auto actionObj = lean_io_result_take_value(ioResult);
        auto action = decodeOwnedAction(actionObj);
        return dispatchDecodedAction(kj::mv(action), kj::mv(context), cleanupState);
      } catch (...) {
        cleanupRequestCaps(cleanupState, {});
        throw;
      }
    }

   private:
    struct RequestCapCleanupState {
      explicit RequestCapCleanupState(std::vector<uint32_t>&& capIds)
          : requestCapIds(kj::mv(capIds)) {}
      std::vector<uint32_t> requestCapIds;
      bool done = false;
    };

    void cleanupRequestCaps(const std::shared_ptr<RequestCapCleanupState>& cleanupState,
                            const std::vector<uint32_t>& retainedCaps) {
      if (cleanupState->done) {
        return;
      }
      runtime_.dropRequestCapsExcept(cleanupState->requestCapIds, retainedCaps);
      cleanupState->done = true;
    }

    kj::Promise<LeanAdvancedHandlerAction> waitLeanActionTask(
        const std::shared_ptr<DeferredLeanTaskState>& taskState) {
      constexpr uint8_t kLeanTaskStateFinished = 2;
      if (lean_io_get_task_state_core(taskState->waitTask->task) == kLeanTaskStateFinished) {
        auto taskResult = lean_task_get(taskState->waitTask->task);
        taskState->completed.store(true, std::memory_order_release);
        auto taskResultTag = lean_obj_tag(taskResult);
        if (taskResultTag == 0) {
          throw std::runtime_error("Lean RPC advanced deferred handler task returned IO error");
        }
        if (taskResultTag != 1) {
          throw std::runtime_error("Lean RPC advanced deferred handler task returned invalid result");
        }
        auto actionObj = lean_ctor_get(taskResult, 0);
        lean_inc(actionObj);
        auto action = decodeOwnedAction(actionObj);
        return action;
      }
      return kj::yield().then([this, taskState]() mutable {
        return waitLeanActionTask(taskState);
      });
    }

    DispatchCallResult dispatchDecodedAction(
        LeanAdvancedHandlerAction action,
        capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer> context,
        const std::shared_ptr<RequestCapCleanupState>& cleanupState) {
      capnp::Capability::Client::CallHints callHints;
      callHints.noPromisePipelining = action.noPromisePipelining;
      callHints.onlyPromisePipeline = action.onlyPromisePipeline;

      if ((action.sendResultsToCaller || action.noPromisePipelining || action.onlyPromisePipeline) &&
          action.kind != LeanAdvancedHandlerAction::Kind::ASYNC_CALL) {
        cleanupRequestCaps(cleanupState, {});
        throw std::runtime_error(
            "Lean RPC advanced handler: forward options are only valid with forwardCall");
      }
      if (action.remoteExceptionType != kj::Exception::Type::FAILED &&
          action.kind != LeanAdvancedHandlerAction::Kind::THROW_REMOTE) {
        cleanupRequestCaps(cleanupState, {});
        throw std::runtime_error(
            "Lean RPC advanced handler: exceptionType is only valid with throwRemote");
      }

      if (action.releaseParams) {
        context.releaseParams();
      }

      if (action.hasPipeline) {
        if (action.kind != LeanAdvancedHandlerAction::Kind::AWAIT_TASK) {
          cleanupRequestCaps(cleanupState, {});
          throw std::runtime_error(
              "Lean RPC advanced handler: setPipeline is only valid with defer");
        }
        try {
          runtime_.setContextPipelineFromPayload(context, action.pipelineBytes.data(),
                                                 action.pipelineBytes.size(),
                                                 action.pipelineCaps.data(),
                                                 action.pipelineCaps.size(),
                                                 "unknown RPC pipeline capability id: ");
        } catch (...) {
          cleanupRequestCaps(cleanupState, {});
          throw;
        }
      }

      if (action.kind == LeanAdvancedHandlerAction::Kind::RETURN_PAYLOAD) {
        auto responseCapIds = decodeCapTable(action.payloadCaps.data(), action.payloadCaps.size());
        if (action.isStreaming) {
          cleanupRequestCaps(cleanupState, {});
          for (auto capId : responseCapIds) {
            if (capId != 0) {
              runtime_.dropTargetIfPresent(capId);
            }
          }
        } else {
          cleanupRequestCaps(cleanupState, responseCapIds);
          runtime_.setContextResultsFromPayloadWithCapIds(
              context, action.payloadBytes.data(), action.payloadBytes.size(), responseCapIds,
              "unknown RPC response capability id from Lean advanced handler: ");
        }
        return {kj::READY_NOW, action.isStreaming, action.allowCancellation};
      }

      if (action.kind == LeanAdvancedHandlerAction::Kind::ASYNC_CALL) {
        auto targetIt = runtime_.targets_.find(action.target);
        if (targetIt == runtime_.targets_.end()) {
          cleanupRequestCaps(cleanupState, {});
          throw std::runtime_error(
              "unknown RPC async-call target capability id from Lean handler: " +
              std::to_string(action.target));
        }
        auto requestCapIdsOut = decodeCapTable(action.payloadCaps.data(), action.payloadCaps.size());
        auto requestBuilder =
            targetIt->second.typelessRequest(action.interfaceId, action.methodId, kj::none, callHints);
        runtime_.setRequestPayload(requestBuilder, action.payloadBytes.data(),
                                   action.payloadBytes.size(), requestCapIdsOut);
        cleanupRequestCaps(cleanupState, {});

        if (action.sendResultsToCaller) {
          return {context.tailCall(kj::mv(requestBuilder)), action.isStreaming,
                  action.allowCancellation};
        }
        if (action.onlyPromisePipeline) {
          throw std::runtime_error(
              "Lean RPC advanced handler: onlyPromisePipeline requires sendResultsTo.caller");
        }

        auto promiseAndPipeline = requestBuilder.send();
        if (action.isStreaming) {
          auto completion = promiseAndPipeline.then(
              [](capnp::Response<capnp::AnyPointer>&&) mutable {});
          return {kj::mv(completion), true, action.allowCancellation};
        }
        context.setPipeline(promiseAndPipeline.noop());
        auto completion = promiseAndPipeline.then(
            [this, context = kj::mv(context)](
                capnp::Response<capnp::AnyPointer>&& response) mutable {
              setContextResultsFromResponse(context, response);
            });
        return {kj::mv(completion), action.isStreaming, action.allowCancellation};
      }

      if (action.kind == LeanAdvancedHandlerAction::Kind::TAIL_CALL) {
        auto targetIt = runtime_.targets_.find(action.target);
        if (targetIt == runtime_.targets_.end()) {
          cleanupRequestCaps(cleanupState, {});
          throw std::runtime_error(
              "unknown RPC tail-call target capability id from Lean advanced handler: " +
              std::to_string(action.target));
        }
        auto requestCapIdsOut = decodeCapTable(action.payloadCaps.data(), action.payloadCaps.size());
        auto requestBuilder =
            targetIt->second.typelessRequest(action.interfaceId, action.methodId, kj::none, callHints);
        runtime_.setRequestPayload(requestBuilder, action.payloadBytes.data(),
                                   action.payloadBytes.size(), requestCapIdsOut);
        cleanupRequestCaps(cleanupState, {});
        return {context.tailCall(kj::mv(requestBuilder)), action.isStreaming,
                action.allowCancellation};
      }

      if (action.kind == LeanAdvancedHandlerAction::Kind::THROW_REMOTE) {
        cleanupRequestCaps(cleanupState, {});
        throwRemoteException(action.remoteExceptionType, action.message, action.detailBytes.data(),
                             action.detailBytes.size());
      }

      auto deferredTaskState =
          std::make_shared<DeferredLeanTaskState>(kj::mv(action.deferredWaitTask),
                                                  kj::mv(action.deferredCancelTask),
                                                  action.allowCancellation);
      if (action.allowCancellation) {
        runtime_.activeCancelableDeferredTasks_.push_back(deferredTaskState);
      }
      auto completion = waitLeanActionTask(deferredTaskState).then(
          [this, context = kj::mv(context), cleanupState, earlyAllowCancellation = action.allowCancellation,
           earlyIsStreaming = action.isStreaming](
              LeanAdvancedHandlerAction nextAction) mutable -> kj::Promise<void> {
            if (nextAction.allowCancellation && !earlyAllowCancellation) {
              cleanupRequestCaps(cleanupState, {});
              throw std::runtime_error(
                  "Lean RPC advanced deferred handler: allowCancellation must be set before defer");
            }
            if (nextAction.isStreaming && !earlyIsStreaming) {
              cleanupRequestCaps(cleanupState, {});
              throw std::runtime_error(
                  "Lean RPC advanced deferred handler: isStreaming must be set before defer");
            }
            auto nextResult = dispatchDecodedAction(kj::mv(nextAction), kj::mv(context), cleanupState);
            return kj::mv(nextResult.promise).then(
                []() {},
                [this, cleanupState](kj::Exception&& e) {
                  cleanupRequestCaps(cleanupState, {});
                  throw kj::mv(e);
                });
          },
          [this, cleanupState](kj::Exception&& e) -> kj::Promise<void> {
            cleanupRequestCaps(cleanupState, {});
            return kj::mv(e);
          }).attach(std::move(deferredTaskState));
      return {kj::mv(completion), action.isStreaming, action.allowCancellation};
    }

    static LeanAdvancedHandlerAction decodeOwnedAction(lean_object* actionObj) {
      try {
        auto action = decodeAction(actionObj);
        lean_dec(actionObj);
        return action;
      } catch (...) {
        lean_dec(actionObj);
        throw;
      }
    }

    static LeanAdvancedHandlerAction decodeAction(lean_object* actionObj) {
      LeanAdvancedHandlerAction action;
      constexpr unsigned kRawAdvancedWrapperScalarBase = sizeof(void*) * 1;
      constexpr unsigned kRawAdvancedControlReleaseOffset = kRawAdvancedWrapperScalarBase;
      constexpr unsigned kRawAdvancedControlAllowOffset = kRawAdvancedWrapperScalarBase + 1;
      constexpr unsigned kRawAdvancedControlStreamingOffset = kRawAdvancedWrapperScalarBase + 2;
      constexpr unsigned kRawAdvancedHintsNoPromiseOffset = kRawAdvancedWrapperScalarBase;
      constexpr unsigned kRawAdvancedHintsOnlyPipelineOffset = kRawAdvancedWrapperScalarBase + 1;
      constexpr unsigned kRawAdvancedExceptionTypeOffset = kRawAdvancedWrapperScalarBase;
      constexpr unsigned kRawAdvancedScalarBase = sizeof(void*) * 2;
      constexpr unsigned kRawAdvancedInterfaceIdOffset = kRawAdvancedScalarBase;
      constexpr unsigned kRawAdvancedTargetOffset = kRawAdvancedScalarBase + 8;
      constexpr unsigned kRawAdvancedMethodIdOffset = kRawAdvancedScalarBase + 12;

      while (true) {
        auto tag = lean_obj_tag(actionObj);
        if (tag == 4) {
          if (lean_ctor_get_uint8(actionObj, kRawAdvancedControlReleaseOffset) != 0) {
            action.releaseParams = true;
          }
          if (lean_ctor_get_uint8(actionObj, kRawAdvancedControlAllowOffset) != 0) {
            action.allowCancellation = true;
          }
          if (lean_ctor_get_uint8(actionObj, kRawAdvancedControlStreamingOffset) != 0) {
            action.isStreaming = true;
          }
          actionObj = lean_ctor_get(actionObj, 0);
          continue;
        }
        if (tag == 6) {
          action.sendResultsToCaller = true;
          actionObj = lean_ctor_get(actionObj, 0);
          continue;
        }
        if (tag == 7) {
          if (lean_ctor_get_uint8(actionObj, kRawAdvancedHintsNoPromiseOffset) != 0) {
            action.noPromisePipelining = true;
          }
          if (lean_ctor_get_uint8(actionObj, kRawAdvancedHintsOnlyPipelineOffset) != 0) {
            action.onlyPromisePipeline = true;
          }
          actionObj = lean_ctor_get(actionObj, 0);
          continue;
        }
        if (tag == 8) {
          auto exTypeTag = lean_ctor_get_uint8(actionObj, kRawAdvancedExceptionTypeOffset);
          switch (exTypeTag) {
            case 0:
              action.remoteExceptionType = kj::Exception::Type::FAILED;
              break;
            case 1:
              action.remoteExceptionType = kj::Exception::Type::OVERLOADED;
              break;
            case 2:
              action.remoteExceptionType = kj::Exception::Type::DISCONNECTED;
              break;
            case 3:
              action.remoteExceptionType = kj::Exception::Type::UNIMPLEMENTED;
              break;
            default:
              action.remoteExceptionType = kj::Exception::Type::FAILED;
              break;
          }
          actionObj = lean_ctor_get(actionObj, 0);
          continue;
        }
        if (tag == 9) {
          if (action.hasPipeline) {
            throw std::runtime_error("Lean RPC advanced handler: setPipeline may only be specified once");
          }
          action.hasPipeline = true;
          action.pipelineBytes = LeanByteArrayRef(lean_ctor_get(actionObj, 0));
          action.pipelineCaps = LeanByteArrayRef(lean_ctor_get(actionObj, 1));
          actionObj = lean_ctor_get(actionObj, 2);
          continue;
        }

        switch (tag) {
          case 0: {
            action.kind = LeanAdvancedHandlerAction::Kind::RETURN_PAYLOAD;
            action.payloadBytes = LeanByteArrayRef(lean_ctor_get(actionObj, 0));
            action.payloadCaps = LeanByteArrayRef(lean_ctor_get(actionObj, 1));
            return action;
          }
          case 1: {
            action.kind = LeanAdvancedHandlerAction::Kind::ASYNC_CALL;
            action.interfaceId = lean_ctor_get_uint64(actionObj, kRawAdvancedInterfaceIdOffset);
            action.target = lean_ctor_get_uint32(actionObj, kRawAdvancedTargetOffset);
            action.methodId = lean_ctor_get_uint16(actionObj, kRawAdvancedMethodIdOffset);
            action.payloadBytes = LeanByteArrayRef(lean_ctor_get(actionObj, 0));
            action.payloadCaps = LeanByteArrayRef(lean_ctor_get(actionObj, 1));
            return action;
          }
          case 2: {
            action.kind = LeanAdvancedHandlerAction::Kind::TAIL_CALL;
            action.interfaceId = lean_ctor_get_uint64(actionObj, kRawAdvancedInterfaceIdOffset);
            action.target = lean_ctor_get_uint32(actionObj, kRawAdvancedTargetOffset);
            action.methodId = lean_ctor_get_uint16(actionObj, kRawAdvancedMethodIdOffset);
            action.payloadBytes = LeanByteArrayRef(lean_ctor_get(actionObj, 0));
            action.payloadCaps = LeanByteArrayRef(lean_ctor_get(actionObj, 1));
            return action;
          }
          case 3: {
            action.kind = LeanAdvancedHandlerAction::Kind::THROW_REMOTE;
            action.message = std::string(lean_string_cstr(lean_ctor_get(actionObj, 0)));
            action.detailBytes = LeanByteArrayRef(lean_ctor_get(actionObj, 1));
            return action;
          }
          case 5: {
            action.kind = LeanAdvancedHandlerAction::Kind::AWAIT_TASK;
            auto waitTaskObj = lean_ctor_get(actionObj, 0);
            auto cancelTaskObj = lean_ctor_get(actionObj, 1);
            lean_inc(waitTaskObj);
            lean_inc(cancelTaskObj);
            action.deferredWaitTask = kj::heap<DeferredLeanTask>(waitTaskObj);
            action.deferredCancelTask = kj::heap<DeferredLeanTask>(cancelTaskObj);
            return action;
          }
          default:
            throw std::runtime_error("unknown Lean RPC advanced handler result tag");
        }
      }
    }

    void setContextResultsFromResponse(
        capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer>& context,
        capnp::Response<capnp::AnyPointer>& response) {
      auto raw = runtime_.serializeResponse(response);
      runtime_.setContextResultsFromPayload(
          context, raw.responseData(), raw.responseSize(),
          raw.responseCaps.data(), raw.responseCaps.size(),
          "unknown RPC response capability id from Lean advanced handler: ");
    }

    RuntimeLoop& runtime_;
    std::shared_ptr<std::atomic<uint32_t>> targetId_;
    lean_object* handler_;
  };

  uint32_t addTarget(capnp::Capability::Client cap) {
    uint32_t targetId = nextTargetId_++;
    while (targets_.find(targetId) != targets_.end()) {
      targetId = nextTargetId_++;
    }
    targets_.emplace(targetId, kj::mv(cap));
    return targetId;
  }

  uint32_t addPendingCall(PendingCall&& pendingCall) {
    uint32_t pendingCallId = nextPendingCallId_++;
    while (pendingCalls_.find(pendingCallId) != pendingCalls_.end()) {
      pendingCallId = nextPendingCallId_++;
    }
    pendingCalls_.emplace(pendingCallId, std::move(pendingCall));
    return pendingCallId;
  }

  uint32_t addRegisterPromise(PendingRegisterPromise&& promise) {
    uint32_t promiseId = nextRegisterPromiseId_++;
    while (registerPromises_.find(promiseId) != registerPromises_.end()) {
      promiseId = nextRegisterPromiseId_++;
    }
    registerPromises_.emplace(promiseId, std::move(promise));
    return promiseId;
  }

  uint32_t addUnitPromise(PendingUnitPromise&& promise) {
    uint32_t promiseId = nextUnitPromiseId_++;
    while (unitPromises_.find(promiseId) != unitPromises_.end()) {
      promiseId = nextUnitPromiseId_++;
    }
    unitPromises_.emplace(promiseId, std::move(promise));
    return promiseId;
  }

  uint32_t addKjAsyncPromise(PendingUnitPromise&& promise) {
    uint32_t promiseId = nextKjAsyncPromiseId_++;
    while (kjAsyncPromises_.find(promiseId) != kjAsyncPromises_.end()) {
      promiseId = nextKjAsyncPromiseId_++;
    }
    retiredKjAsyncPromises_.erase(promiseId);
    kjAsyncPromises_.emplace(promiseId, std::move(promise));
    return promiseId;
  }

  uint32_t kjAsyncSleepNanosStart(kj::AsyncIoProvider& ioProvider, uint64_t delayNanos) {
    auto canceler = kj::heap<kj::Canceler>();
    auto promise =
        canceler->wrap(ioProvider.getTimer().afterDelay(delayNanos * kj::NANOSECONDS));
    return addKjAsyncPromise(PendingUnitPromise(kj::mv(promise), kj::mv(canceler)));
  }

  PendingUnitPromise takeKjAsyncPromise(uint32_t promiseId) {
    auto it = kjAsyncPromises_.find(promiseId);
    if (it == kjAsyncPromises_.end()) {
      if (retiredKjAsyncPromises_.find(promiseId) != retiredKjAsyncPromises_.end()) {
        throw std::runtime_error("KJ async promise id already consumed or released: " +
                                 std::to_string(promiseId));
      }
      throw std::runtime_error("unknown KJ async promise id: " + std::to_string(promiseId));
    }
    auto pending = kj::mv(it->second);
    kjAsyncPromises_.erase(it);
    retiredKjAsyncPromises_.insert(promiseId);
    pending.canceler->release();
    return pending;
  }

  void awaitKjAsyncPromise(kj::WaitScope& waitScope, uint32_t promiseId) {
    auto it = kjAsyncPromises_.find(promiseId);
    if (it == kjAsyncPromises_.end()) {
      if (retiredKjAsyncPromises_.find(promiseId) != retiredKjAsyncPromises_.end()) {
        return;
      }
      throw std::runtime_error("unknown KJ async promise id: " + std::to_string(promiseId));
    }
    auto pending = kj::mv(it->second);
    kjAsyncPromises_.erase(it);
    retiredKjAsyncPromises_.insert(promiseId);
    pending.canceler->release();
    kj::mv(pending.promise).wait(waitScope);
  }

  void cancelKjAsyncPromise(uint32_t promiseId) {
    auto it = kjAsyncPromises_.find(promiseId);
    if (it == kjAsyncPromises_.end()) {
      if (retiredKjAsyncPromises_.find(promiseId) != retiredKjAsyncPromises_.end()) {
        return;
      }
      throw std::runtime_error("unknown KJ async promise id: " + std::to_string(promiseId));
    }
    it->second.canceler->cancel("Capnp.KjAsync promise canceled from Lean");
  }

  void releaseKjAsyncPromise(uint32_t promiseId) {
    auto it = kjAsyncPromises_.find(promiseId);
    if (it == kjAsyncPromises_.end()) {
      if (retiredKjAsyncPromises_.find(promiseId) != retiredKjAsyncPromises_.end()) {
        return;
      }
      throw std::runtime_error("unknown KJ async promise id: " + std::to_string(promiseId));
    }
    kjAsyncPromises_.erase(it);
    retiredKjAsyncPromises_.insert(promiseId);
  }

  uint32_t kjAsyncPromiseThenStart(uint32_t firstPromiseId, uint32_t secondPromiseId) {
    auto firstPending = takeKjAsyncPromise(firstPromiseId);
    auto secondPending = takeKjAsyncPromise(secondPromiseId);

    auto canceler = kj::heap<kj::Canceler>();
    auto second = kj::mv(secondPending.promise);
    auto promise = canceler->wrap(kj::mv(firstPending.promise).then(
        [second = kj::mv(second)]() mutable { return kj::mv(second); }));
    return addKjAsyncPromise(PendingUnitPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t kjAsyncPromiseCatchStart(uint32_t promiseId, uint32_t fallbackPromiseId) {
    auto firstPending = takeKjAsyncPromise(promiseId);
    auto fallbackPending = takeKjAsyncPromise(fallbackPromiseId);

    auto canceler = kj::heap<kj::Canceler>();
    auto fallback = kj::mv(fallbackPending.promise);
    auto recovered = kj::mv(firstPending.promise).then(
        []() -> kj::Promise<void> { return kj::READY_NOW; },
        [fallback = kj::mv(fallback)](kj::Exception&&) mutable -> kj::Promise<void> {
          return kj::mv(fallback);
        });
    auto promise = canceler->wrap(kj::mv(recovered));
    return addKjAsyncPromise(PendingUnitPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t kjAsyncPromiseAllStart(std::vector<uint32_t> promiseIds) {
    auto promises = kj::heapArrayBuilder<kj::Promise<void>>(promiseIds.size());
    for (auto promiseId : promiseIds) {
      auto pending = takeKjAsyncPromise(promiseId);
      promises.add(kj::mv(pending.promise));
    }
    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(kj::joinPromises(promises.finish()));
    return addKjAsyncPromise(PendingUnitPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t kjAsyncPromiseRaceStart(std::vector<uint32_t> promiseIds) {
    if (promiseIds.empty()) {
      throw std::runtime_error("promiseRaceStart requires at least one promise id");
    }

    auto firstPending = takeKjAsyncPromise(promiseIds[0]);
    auto raced = kj::mv(firstPending.promise);

    for (size_t i = 1; i < promiseIds.size(); ++i) {
      auto pending = takeKjAsyncPromise(promiseIds[i]);
      raced = kj::mv(raced).exclusiveJoin(kj::mv(pending.promise));
    }

    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(kj::mv(raced));
    return addKjAsyncPromise(PendingUnitPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t addKjAsyncTaskSet(kj::Own<RuntimeKjAsyncTaskSet>&& taskSet) {
    uint32_t taskSetId = nextKjAsyncTaskSetId_++;
    while (kjAsyncTaskSets_.find(taskSetId) != kjAsyncTaskSets_.end()) {
      taskSetId = nextKjAsyncTaskSetId_++;
    }
    kjAsyncTaskSets_.emplace(taskSetId, kj::mv(taskSet));
    return taskSetId;
  }

  uint32_t kjAsyncTaskSetNew() { return addKjAsyncTaskSet(kj::heap<RuntimeKjAsyncTaskSet>()); }

  void kjAsyncTaskSetRelease(uint32_t taskSetId) {
    auto erased = kjAsyncTaskSets_.erase(taskSetId);
    if (erased == 0) {
      throw std::runtime_error("unknown KJ async task set id: " + std::to_string(taskSetId));
    }
  }

  void kjAsyncTaskSetAddPromise(uint32_t taskSetId, uint32_t promiseId) {
    auto taskSetIt = kjAsyncTaskSets_.find(taskSetId);
    if (taskSetIt == kjAsyncTaskSets_.end()) {
      throw std::runtime_error("unknown KJ async task set id: " + std::to_string(taskSetId));
    }

    auto pending = takeKjAsyncPromise(promiseId);
    taskSetIt->second->tasks.add(kj::mv(pending.promise));
  }

  void kjAsyncTaskSetClear(uint32_t taskSetId) {
    auto taskSetIt = kjAsyncTaskSets_.find(taskSetId);
    if (taskSetIt == kjAsyncTaskSets_.end()) {
      throw std::runtime_error("unknown KJ async task set id: " + std::to_string(taskSetId));
    }
    taskSetIt->second->tasks.clear();
  }

  bool kjAsyncTaskSetIsEmpty(uint32_t taskSetId) {
    auto taskSetIt = kjAsyncTaskSets_.find(taskSetId);
    if (taskSetIt == kjAsyncTaskSets_.end()) {
      throw std::runtime_error("unknown KJ async task set id: " + std::to_string(taskSetId));
    }
    return taskSetIt->second->tasks.isEmpty();
  }

  uint32_t kjAsyncTaskSetOnEmptyStart(uint32_t taskSetId) {
    auto taskSetIt = kjAsyncTaskSets_.find(taskSetId);
    if (taskSetIt == kjAsyncTaskSets_.end()) {
      throw std::runtime_error("unknown KJ async task set id: " + std::to_string(taskSetId));
    }
    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(taskSetIt->second->tasks.onEmpty());
    return addKjAsyncPromise(PendingUnitPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t kjAsyncTaskSetErrorCount(uint32_t taskSetId) {
    auto taskSetIt = kjAsyncTaskSets_.find(taskSetId);
    if (taskSetIt == kjAsyncTaskSets_.end()) {
      throw std::runtime_error("unknown KJ async task set id: " + std::to_string(taskSetId));
    }
    std::lock_guard<std::mutex> lock(taskSetIt->second->mutex);
    return taskSetIt->second->errorCount;
  }

  kj::Maybe<std::string> kjAsyncTaskSetTakeLastError(uint32_t taskSetId) {
    auto taskSetIt = kjAsyncTaskSets_.find(taskSetId);
    if (taskSetIt == kjAsyncTaskSets_.end()) {
      throw std::runtime_error("unknown KJ async task set id: " + std::to_string(taskSetId));
    }
    std::lock_guard<std::mutex> lock(taskSetIt->second->mutex);
    if (taskSetIt->second->lastError.empty()) {
      return kj::none;
    }
    auto out = taskSetIt->second->lastError;
    taskSetIt->second->lastError.clear();
    return out;
  }

  uint32_t addPromiseCapabilityFulfiller(PendingPromiseCapability&& promise) {
    uint32_t fulfillerId = nextPromiseCapabilityFulfillerId_++;
    while (promiseCapabilityFulfillers_.find(fulfillerId) != promiseCapabilityFulfillers_.end()) {
      fulfillerId = nextPromiseCapabilityFulfillerId_++;
    }
    promiseCapabilityFulfillers_.emplace(fulfillerId, std::move(promise));
    return fulfillerId;
  }

  uint32_t addMultiVatPeer(kj::Own<MultiVatPeer>&& peer) {
    uint32_t peerId = nextMultiVatPeerId_++;
    while (multiVatPeers_.find(peerId) != multiVatPeers_.end()) {
      peerId = nextMultiVatPeerId_++;
    }
    multiVatPeerIdsByName_[peer->name] = peerId;
    multiVatPeers_.emplace(peerId, kj::mv(peer));
    return peerId;
  }

  MultiVatPeer& requireMultiVatPeer(uint32_t peerId) {
    auto peerIt = multiVatPeers_.find(peerId);
    if (peerIt == multiVatPeers_.end()) {
      throw std::runtime_error("unknown multi-vat peer id: " + std::to_string(peerId));
    }
    return *peerIt->second;
  }

  MultiVatPeer& requireMultiVatPeerByHost(const std::string& host) {
    auto idIt = multiVatPeerIdsByName_.find(host);
    if (idIt == multiVatPeerIdsByName_.end()) {
      throw std::runtime_error("unknown multi-vat host: " + host);
    }
    return requireMultiVatPeer(idIt->second);
  }

  static std::string sturdyObjectKey(const uint8_t* objectIdData, size_t objectIdSize) {
    if (objectIdSize == 0) {
      return std::string();
    }
    return std::string(reinterpret_cast<const char*>(objectIdData), objectIdSize);
  }

  uint32_t newMultiVatClient(const std::string& name) {
    auto& vat = genericVatNetwork_.add(name);
    auto peer = kj::heap<MultiVatPeer>();
    peer->name = name;
    peer->vat = &vat;
    peer->rpcSystem = kj::heap<GenericRpcSystem>(capnp::makeRpcClient(vat));
    return addMultiVatPeer(kj::mv(peer));
  }

  uint32_t newMultiVatServer(const std::string& name, uint32_t bootstrapTarget) {
    auto targetIt = targets_.find(bootstrapTarget);
    if (targetIt == targets_.end()) {
      throw std::runtime_error("unknown RPC target capability id: " +
                               std::to_string(bootstrapTarget));
    }
    auto& vat = genericVatNetwork_.add(name);
    auto peer = kj::heap<MultiVatPeer>();
    peer->name = name;
    peer->vat = &vat;
    peer->rpcSystem = kj::heap<GenericRpcSystem>(capnp::makeRpcServer(vat, targetIt->second));
    return addMultiVatPeer(kj::mv(peer));
  }

  uint32_t newMultiVatServerWithBootstrapFactory(const std::string& name,
                                                 lean_object* bootstrapFactory) {
    auto& vat = genericVatNetwork_.add(name);
    auto peer = kj::heap<MultiVatPeer>();
    peer->name = name;
    peer->vat = &vat;
    auto factory = kj::heap<LeanGenericBootstrapFactory>(*this, bootstrapFactory);
    peer->rpcSystem = kj::heap<GenericRpcSystem>(capnp::makeRpcServer(vat, *factory));
    // Keep bootstrap factory alive for the lifetime of the RPC system.
    genericBootstrapFactories_.add(kj::mv(factory));
    return addMultiVatPeer(kj::mv(peer));
  }

  void releaseMultiVatPeer(uint32_t peerId) {
    auto peerIt = multiVatPeers_.find(peerId);
    if (peerIt == multiVatPeers_.end()) {
      throw std::runtime_error("unknown multi-vat peer id: " + std::to_string(peerId));
    }
    multiVatPeerIdsByName_.erase(peerIt->second->name);
    sturdyRefs_.erase(peerId);
    multiVatPeers_.erase(peerIt);
  }

  uint32_t multiVatBootstrap(uint32_t sourcePeerId, const LeanVatId& vatId) {
    auto& source = requireMultiVatPeer(sourcePeerId);
    capnp::MallocMessageBuilder message;
    auto hostId = message.initRoot<GenericVatId>();
    setLeanVatId(hostId, vatId);
    auto cap = source.rpcSystem->bootstrap(hostId.asReader());
    return addTarget(kj::mv(cap));
  }

  uint32_t multiVatBootstrapPeer(uint32_t sourcePeerId, uint32_t targetPeerId, bool unique) {
    auto& target = requireMultiVatPeer(targetPeerId);
    return multiVatBootstrap(sourcePeerId, LeanVatId{target.name, unique});
  }

  void multiVatSetForwardingEnabled(bool enabled) {
    genericVatNetwork_.setForwardingEnabled(enabled);
  }

  void multiVatResetForwardingStats() {
    genericVatNetwork_.resetForwardingStats();
  }

  uint64_t multiVatForwardCount() const {
    return genericVatNetwork_.forwardCount();
  }

  uint64_t multiVatThirdPartyTokenCount() const {
    return genericVatNetwork_.tokenCount();
  }

  uint64_t multiVatDeniedForwardCount() const {
    return genericVatNetwork_.deniedForwardCount();
  }

  bool multiVatHasConnection(uint32_t fromPeerId, uint32_t toPeerId) {
    auto& from = requireMultiVatPeer(fromPeerId);
    auto& to = requireMultiVatPeer(toPeerId);
    auto conn = from.vat->getConnectionTo(*to.vat);
    return conn != kj::none;
  }

  capnp::_::RpcSystemBase::RpcDiagnostics multiVatGetDiagnostics(uint32_t peerId,
                                                                 lean_object* targetVatIdObj) {
    auto& peer = requireMultiVatPeer(peerId);
    LeanVatId targetVatId = decodeLeanVatId(targetVatIdObj);
    capnp::MallocMessageBuilder message;
    auto hostId = message.initRoot<GenericVatId>();
    setLeanVatId(hostId, targetVatId);
    return peer.rpcSystem->getDiagnostics(hostId.asReader());
  }

  void multiVatConnectionBlock(uint32_t fromPeerId, uint32_t toPeerId) {
    auto& from = requireMultiVatPeer(fromPeerId);
    auto& to = requireMultiVatPeer(toPeerId);
    auto conn = from.vat->getConnectionTo(*to.vat);
    if (conn == kj::none) {
      throw std::runtime_error("no connection from " + from.name + " to " + to.name);
    }
    KJ_ASSERT_NONNULL(conn).block();
  }

  void multiVatConnectionUnblock(uint32_t fromPeerId, uint32_t toPeerId) {
    auto& from = requireMultiVatPeer(fromPeerId);
    auto& to = requireMultiVatPeer(toPeerId);
    auto conn = from.vat->getConnectionTo(*to.vat);
    if (conn == kj::none) {
      throw std::runtime_error("no connection from " + from.name + " to " + to.name);
    }
    KJ_ASSERT_NONNULL(conn).unblock();
  }

  void multiVatConnectionDisconnect(uint32_t fromPeerId, uint32_t toPeerId, uint8_t exceptionTypeTag,
                                    std::string message, std::vector<uint8_t> detailBytes) {
    auto& from = requireMultiVatPeer(fromPeerId);
    auto& to = requireMultiVatPeer(toPeerId);
    auto conn = from.vat->getConnectionTo(*to.vat);
    if (conn == kj::none) {
      throw std::runtime_error("no connection from " + from.name + " to " + to.name);
    }

    auto ex = kj::Exception(decodeRemoteExceptionType(exceptionTypeTag), __FILE__, __LINE__,
                            kj::str(message.c_str()));
    if (!detailBytes.empty()) {
      ex.setDetail(1, kj::heapArray(detailBytes.data(), detailBytes.size()));
    }
    KJ_ASSERT_NONNULL(conn).disconnect(kj::mv(ex));
  }

  ProtocolMessageCounts multiVatConnectionResolveDisembargoCounts(uint32_t fromPeerId,
                                                                   uint32_t toPeerId) {
    auto& from = requireMultiVatPeer(fromPeerId);
    auto& to = requireMultiVatPeer(toPeerId);
    auto conn = from.vat->getConnectionTo(*to.vat);
    if (conn == kj::none) {
      throw std::runtime_error("no connection from " + from.name + " to " + to.name);
    }
    return KJ_ASSERT_NONNULL(conn).getProtocolMessageCounts();
  }

  std::vector<uint8_t> multiVatConnectionResolveDisembargoTrace(uint32_t fromPeerId,
                                                                 uint32_t toPeerId) {
    auto& from = requireMultiVatPeer(fromPeerId);
    auto& to = requireMultiVatPeer(toPeerId);
    auto conn = from.vat->getConnectionTo(*to.vat);
    if (conn == kj::none) {
      throw std::runtime_error("no connection from " + from.name + " to " + to.name);
    }

    auto trace = KJ_ASSERT_NONNULL(conn).getProtocolMessageTrace();
    std::vector<uint8_t> out;
    out.reserve(trace.size() * 2);
    for (auto tag : trace) {
      appendUint16Le(out, tag);
    }
    return out;
  }

  void multiVatConnectionResetResolveDisembargoTrace(uint32_t fromPeerId, uint32_t toPeerId) {
    auto& from = requireMultiVatPeer(fromPeerId);
    auto& to = requireMultiVatPeer(toPeerId);
    auto conn = from.vat->getConnectionTo(*to.vat);
    if (conn == kj::none) {
      throw std::runtime_error("no connection from " + from.name + " to " + to.name);
    }

    KJ_ASSERT_NONNULL(conn).resetProtocolMessageTrace();
  }

  void multiVatSetRestorer(uint32_t peerId, lean_object* restorer) {
    auto& peer = requireMultiVatPeer(peerId);
    if (peer.sturdyRefRestorer != nullptr) {
      lean_dec(peer.sturdyRefRestorer);
    }
    peer.sturdyRefRestorer = restorer;
  }

  void multiVatClearRestorer(uint32_t peerId) {
    auto& peer = requireMultiVatPeer(peerId);
    if (peer.sturdyRefRestorer != nullptr) {
      lean_dec(peer.sturdyRefRestorer);
      peer.sturdyRefRestorer = nullptr;
    }
  }

  void multiVatPublishSturdyRef(uint32_t hostPeerId, const uint8_t* objectIdData,
                                size_t objectIdSize, uint32_t targetId) {
    requireMultiVatPeer(hostPeerId);
    auto targetIt = targets_.find(targetId);
    if (targetIt == targets_.end()) {
      throw std::runtime_error("unknown RPC target capability id: " + std::to_string(targetId));
    }
    auto& hostRefs = sturdyRefs_[hostPeerId];
    hostRefs.insert_or_assign(sturdyObjectKey(objectIdData, objectIdSize), targetIt->second);
  }

  uint32_t multiVatPublishSturdyRefStart(uint32_t hostPeerId, LeanByteArrayRef objectId,
                                         uint32_t targetId) {
    auto canceler = kj::heap<kj::Canceler>();
    kj::Promise<void> ready = kj::READY_NOW;
    auto promise = canceler->wrap(kj::mv(ready).then(
        [this, hostPeerId, objectId = std::move(objectId), targetId]() mutable {
          multiVatPublishSturdyRef(hostPeerId, objectId.data(), objectId.size(), targetId);
        }));
    return addUnitPromise(PendingUnitPromise(kj::mv(promise), kj::mv(canceler)));
  }

  void multiVatUnpublishSturdyRef(uint32_t hostPeerId, const uint8_t* objectIdData,
                                  size_t objectIdSize) {
    requireMultiVatPeer(hostPeerId);
    auto hostRefsIt = sturdyRefs_.find(hostPeerId);
    if (hostRefsIt == sturdyRefs_.end()) {
      throw std::runtime_error("no sturdy refs published for host peer id: " +
                               std::to_string(hostPeerId));
    }
    auto key = sturdyObjectKey(objectIdData, objectIdSize);
    auto erased = hostRefsIt->second.erase(key);
    if (erased == 0) {
      throw std::runtime_error("unknown sturdy ref object id");
    }
    if (hostRefsIt->second.empty()) {
      sturdyRefs_.erase(hostRefsIt);
    }
  }

  uint32_t multiVatUnpublishSturdyRefStart(uint32_t hostPeerId, LeanByteArrayRef objectId) {
    auto canceler = kj::heap<kj::Canceler>();
    kj::Promise<void> ready = kj::READY_NOW;
    auto promise = canceler->wrap(kj::mv(ready).then(
        [this, hostPeerId, objectId = std::move(objectId)]() mutable {
          multiVatUnpublishSturdyRef(hostPeerId, objectId.data(), objectId.size());
        }));
    return addUnitPromise(PendingUnitPromise(kj::mv(promise), kj::mv(canceler)));
  }

  void multiVatClearPublishedSturdyRefs(uint32_t hostPeerId) {
    requireMultiVatPeer(hostPeerId);
    sturdyRefs_.erase(hostPeerId);
  }

  uint32_t multiVatClearPublishedSturdyRefsStart(uint32_t hostPeerId) {
    auto canceler = kj::heap<kj::Canceler>();
    kj::Promise<void> ready = kj::READY_NOW;
    auto promise = canceler->wrap(kj::mv(ready).then(
        [this, hostPeerId]() { multiVatClearPublishedSturdyRefs(hostPeerId); }));
    return addUnitPromise(PendingUnitPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint64_t multiVatPublishedSturdyRefCount(uint32_t hostPeerId) {
    requireMultiVatPeer(hostPeerId);
    auto hostRefsIt = sturdyRefs_.find(hostPeerId);
    if (hostRefsIt == sturdyRefs_.end()) {
      return 0;
    }
    return static_cast<uint64_t>(hostRefsIt->second.size());
  }

  uint32_t multiVatRestoreSturdyRef(uint32_t sourcePeerId, const LeanVatId& hostVatId,
                                    const uint8_t* objectIdData, size_t objectIdSize) {
    auto& source = requireMultiVatPeer(sourcePeerId);
    auto& host = requireMultiVatPeerByHost(hostVatId.host);

    if (host.sturdyRefRestorer != nullptr) {
      lean_inc(host.sturdyRefRestorer);
      auto objectIdObj = mkByteArrayCopy(objectIdData, objectIdSize);
      auto ioResult = lean_apply_4(host.sturdyRefRestorer, lean_mk_string(source.name.c_str()),
                                   lean_box(hostVatId.unique ? 1 : 0), objectIdObj, lean_box(0));
      if (lean_io_result_is_error(ioResult)) {
        lean_dec(ioResult);
        throw std::runtime_error("multi-vat sturdy ref restorer returned IO error");
      }
      auto targetObj = lean_io_result_take_value(ioResult);
      uint32_t targetId = lean_unbox_uint32(targetObj);
      lean_dec(targetObj);
      return retainTarget(targetId);
    }

    auto hostRefsIt = sturdyRefs_.find(multiVatPeerIdsByName_.at(host.name));
    if (hostRefsIt == sturdyRefs_.end()) {
      throw std::runtime_error("no sturdy refs published for host: " + host.name);
    }
    auto key = sturdyObjectKey(objectIdData, objectIdSize);
    auto refIt = hostRefsIt->second.find(key);
    if (refIt == hostRefsIt->second.end()) {
      throw std::runtime_error("unknown sturdy ref object id");
    }
    return addTarget(refIt->second);
  }

  uint32_t multiVatRestoreSturdyRefStart(uint32_t sourcePeerId, std::string host, bool unique,
                                         LeanByteArrayRef objectId) {
    auto canceler = kj::heap<kj::Canceler>();
    kj::Promise<void> ready = kj::READY_NOW;
    auto promise = canceler->wrap(kj::mv(ready).then(
        [this, sourcePeerId, host = std::move(host), unique,
         objectId = std::move(objectId)]() mutable {
          return multiVatRestoreSturdyRef(sourcePeerId, LeanVatId{host, unique}, objectId.data(),
                                          objectId.size());
        }));
    return addRegisterPromise(PendingRegisterPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t awaitRegisterPromise(kj::WaitScope& waitScope, uint32_t promiseId) {
    auto it = registerPromises_.find(promiseId);
    if (it == registerPromises_.end()) {
      throw std::runtime_error("unknown RPC register promise id: " + std::to_string(promiseId));
    }
    auto pending = kj::mv(it->second);
    registerPromises_.erase(it);
    pending.canceler->release();
    return kj::mv(pending.promise).wait(waitScope);
  }

  void cancelRegisterPromise(uint32_t promiseId) {
    auto it = registerPromises_.find(promiseId);
    if (it == registerPromises_.end()) {
      throw std::runtime_error("unknown RPC register promise id: " + std::to_string(promiseId));
    }
    it->second.canceler->cancel("Capnp.Rpc register promise canceled from Lean");
  }

  void releaseRegisterPromise(uint32_t promiseId) {
    auto it = registerPromises_.find(promiseId);
    if (it == registerPromises_.end()) {
      throw std::runtime_error("unknown RPC register promise id: " + std::to_string(promiseId));
    }
    registerPromises_.erase(it);
  }

  void awaitUnitPromise(kj::WaitScope& waitScope, uint32_t promiseId) {
    auto it = unitPromises_.find(promiseId);
    if (it == unitPromises_.end()) {
      throw std::runtime_error("unknown RPC unit promise id: " + std::to_string(promiseId));
    }
    auto pending = kj::mv(it->second);
    unitPromises_.erase(it);
    pending.canceler->release();
    kj::mv(pending.promise).wait(waitScope);
  }

  void cancelUnitPromise(uint32_t promiseId) {
    auto it = unitPromises_.find(promiseId);
    if (it == unitPromises_.end()) {
      throw std::runtime_error("unknown RPC unit promise id: " + std::to_string(promiseId));
    }
    it->second.canceler->cancel("Capnp.Rpc unit promise canceled from Lean");
  }

  void releaseUnitPromise(uint32_t promiseId) {
    auto it = unitPromises_.find(promiseId);
    if (it == unitPromises_.end()) {
      throw std::runtime_error("unknown RPC unit promise id: " + std::to_string(promiseId));
    }
    unitPromises_.erase(it);
  }

  uint32_t registerHandlerTarget(lean_object* handler) {
    auto targetIdRef = std::make_shared<std::atomic<uint32_t>>(0);
    auto server = kj::heap<LeanCapabilityServer>(*this, targetIdRef, handler);
    auto targetId = addTarget(capnp::Capability::Client(kj::mv(server)));
    targetIdRef->store(targetId, std::memory_order_relaxed);
    return targetId;
  }

  uint32_t registerAdvancedHandlerTarget(lean_object* handler) {
    auto targetIdRef = std::make_shared<std::atomic<uint32_t>>(0);
    auto server = kj::heap<LeanAdvancedCapabilityServer>(*this, targetIdRef, handler);
    auto targetId = addTarget(capnp::Capability::Client(kj::mv(server)));
    targetIdRef->store(targetId, std::memory_order_relaxed);
    return targetId;
  }

  uint32_t registerTailCallHandlerTarget(lean_object* handler) {
    auto targetIdRef = std::make_shared<std::atomic<uint32_t>>(0);
    auto server = kj::heap<LeanTailCallHandlerServer>(*this, targetIdRef, handler);
    auto targetId = addTarget(capnp::Capability::Client(kj::mv(server)));
    targetIdRef->store(targetId, std::memory_order_relaxed);
    return targetId;
  }

  uint32_t registerTailCallTarget(uint32_t target) {
    auto targetIt = targets_.find(target);
    if (targetIt == targets_.end()) {
      throw std::runtime_error("unknown RPC target capability id: " + std::to_string(target));
    }

    auto server = kj::heap<TailCallForwardingServer>(targetIt->second);
    auto forwarderId = addTarget(capnp::Capability::Client(kj::mv(server)));

    // Keep any underlying peer ownership alive while the forwarder exists.
    retainPeerOwnership(target, forwarderId, loopbackPeerOwnerByTarget_, loopbackPeerOwnerRefCount_);
    retainPeerOwnership(target, forwarderId, networkPeerOwnerByTarget_, networkPeerOwnerRefCount_);
    return forwarderId;
  }

  uint32_t registerFdTarget(uint32_t fd) {
#if defined(_WIN32)
    (void)fd;
    throw std::runtime_error("registerFdTarget is not supported on Windows");
#else
    constexpr uint32_t maxInt = static_cast<uint32_t>(std::numeric_limits<int>::max());
    if (fd > maxInt) {
      throw std::runtime_error("fd exceeds platform int range");
    }
    int fdCopy = dup(static_cast<int>(fd));
    if (fdCopy < 0) {
      throw std::runtime_error("dup() failed while registering fd target");
    }
    auto server = kj::heap<FdCapabilityServer>(fdCopy);
    return addTarget(capnp::Capability::Client(kj::mv(server)));
#endif
  }

  uint32_t registerFdProbeTarget() {
    auto server = kj::heap<FdProbeCapabilityServer>();
    return addTarget(capnp::Capability::Client(kj::mv(server)));
  }

  template <typename RequestBuilder>
  void setRequestPayload(RequestBuilder& requestBuilder, const uint8_t* requestData,
                         size_t requestSize,
                         const std::vector<uint32_t>& requestCaps) {
    if (requestSize == 0) {
      if (!requestCaps.empty()) {
        throw std::runtime_error("RPC request capability table requires a non-empty payload");
      }
      return;
    }

    kj::ArrayPtr<const kj::byte> reqBytes(reinterpret_cast<const kj::byte*>(requestData),
                                          requestSize);
    kj::ArrayInputStream input(reqBytes);
    capnp::ReaderOptions options;
    options.traversalLimitInWords = 1ull << 30;
    capnp::InputStreamMessageReader reader(input, options);
    auto requestRoot = reader.getRoot<capnp::AnyPointer>();
    if (requestCaps.empty()) {
      requestBuilder.template setAs<capnp::AnyPointer>(requestRoot);
    } else {
      auto capTableBuilder =
          kj::heapArrayBuilder<kj::Maybe<kj::Own<capnp::ClientHook>>>(requestCaps.size());
      for (auto capId : requestCaps) {
        if (capId == 0) {
          capTableBuilder.add(kj::none);
          continue;
        }
        auto capIt = targets_.find(capId);
        if (capIt == targets_.end()) {
          throw std::runtime_error("unknown RPC request capability id: " + std::to_string(capId));
        }
        capnp::Capability::Client cap = capIt->second;
        capTableBuilder.add(capnp::ClientHook::from(kj::mv(cap)));
      }
      capnp::ReaderCapabilityTable requestCapTable(capTableBuilder.finish());
      requestBuilder.template setAs<capnp::AnyPointer>(requestCapTable.imbue(requestRoot));
    }
  }

  template <typename RequestBuilder>
  void setRequestPayload(RequestBuilder& requestBuilder, const std::vector<uint8_t>& request,
                         const std::vector<uint32_t>& requestCaps) {
    setRequestPayload(requestBuilder, request.data(), request.size(), requestCaps);
  }

  RawCallResult serializeResponse(capnp::Response<capnp::AnyPointer>& response) {
    capnp::MallocMessageBuilder responseMessage;
    capnp::BuilderCapabilityTable responseCapTable;
    responseCapTable
        .imbue(responseMessage.getRoot<capnp::AnyPointer>())
        .setAs<capnp::AnyPointer>(response.getAs<capnp::AnyPointer>());

    auto responseWords = capnp::messageToFlatArray(responseMessage);

    std::vector<uint8_t> responseCaps;
    auto responseCapTableEntries = responseCapTable.getTable();
    responseCaps.reserve(responseCapTableEntries.size() * 4);
    for (auto& maybeHook : responseCapTableEntries) {
      KJ_IF_SOME(hook, maybeHook) {
        auto cap = capnp::Capability::Client(hook->addRef());
        appendUint32Le(responseCaps, addTarget(kj::mv(cap)));
      } else {
        appendUint32Le(responseCaps, 0);
      }
    }

    return RawCallResult{std::move(responseWords), std::move(responseCaps)};
  }

  uint32_t startPendingCall(uint32_t target, uint64_t interfaceId, uint16_t methodId,
                            const uint8_t* requestData, size_t requestSize,
                            const std::vector<uint32_t>& requestCaps) {
    auto targetIt = targets_.find(target);
    if (targetIt == targets_.end()) {
      throw std::runtime_error("unknown RPC target capability id: " + std::to_string(target));
    }

    auto requestBuilder = targetIt->second.typelessRequest(interfaceId, methodId, kj::none, {});
    setRequestPayload(requestBuilder, requestData, requestSize, requestCaps);
    auto promiseAndPipeline = requestBuilder.send();
    auto canceler = kj::heap<kj::Canceler>();
    auto pipeline = promiseAndPipeline.noop();
    auto promise = canceler->wrap(kj::mv(promiseAndPipeline).dropPipeline());
    return addPendingCall(PendingCall(kj::mv(promise), kj::mv(pipeline), kj::mv(canceler)));
  }

  uint32_t startStreamingPendingCall(uint32_t target, uint64_t interfaceId, uint16_t methodId,
                                     const uint8_t* requestData, size_t requestSize,
                                     const std::vector<uint32_t>& requestCaps) {
    auto targetIt = targets_.find(target);
    if (targetIt == targets_.end()) {
      throw std::runtime_error("unknown RPC target capability id: " + std::to_string(target));
    }

    struct StreamingClientAccess final : public capnp::Capability::Client {
      explicit StreamingClientAccess(capnp::Capability::Client& client)
          : capnp::Capability::Client(client) {}
      using capnp::Capability::Client::newStreamingCall;
    };

    StreamingClientAccess client(targetIt->second);
    auto requestBuilder = client.newStreamingCall<capnp::AnyPointer>(interfaceId, methodId, kj::none, {});
    setRequestPayload(requestBuilder, requestData, requestSize, requestCaps);
    auto promise = requestBuilder.send();
    auto canceler = kj::heap<kj::Canceler>();
    auto wrappedPromise = canceler->wrap(kj::mv(promise).then([]() -> capnp::Response<capnp::AnyPointer> {
      throw std::logic_error("streaming call cannot be awaited");
    }));
    // We can't easily construct a valid Pipeline from nothing, but we know streaming calls
    // don't have pipelines. We use a trick to get a null/broken pipeline.
    capnp::Capability::Client nullClient(nullptr);
    auto nullRequest = nullClient.typelessRequest(0, 0, kj::none, {});
    auto pipeline = nullRequest.send().noop(); 

    return addPendingCall(PendingCall(kj::mv(wrappedPromise), kj::mv(pipeline), kj::mv(canceler)));
  }

  kj::Promise<RawCallResult> awaitPendingCall(uint32_t pendingCallId) {
    auto pendingIt = pendingCalls_.find(pendingCallId);
    if (pendingIt == pendingCalls_.end()) {
      throw std::runtime_error("unknown pending RPC call id: " + std::to_string(pendingCallId));
    }
    auto pending = kj::mv(pendingIt->second);
    pendingCalls_.erase(pendingIt);
    pending.canceler->release();
    return kj::mv(pending.promise).then([this](capnp::Response<capnp::AnyPointer>&& response) {
      return kj::evalNow([this, &response]() { return serializeResponse(response); });
    });
  }

  void releasePendingCall(uint32_t pendingCallId) {
    auto pendingIt = pendingCalls_.find(pendingCallId);
    if (pendingIt == pendingCalls_.end()) {
      throw std::runtime_error("unknown pending RPC call id: " + std::to_string(pendingCallId));
    }
    cancelOneActiveDeferredTask();
    pendingIt->second.canceler->cancel("Capnp.Rpc pending call released from Lean");
    pendingCalls_.erase(pendingIt);
  }

  void cancelOneActiveDeferredTask() {
    while (!activeCancelableDeferredTasks_.empty()) {
      auto taskState = activeCancelableDeferredTasks_.front().lock();
      activeCancelableDeferredTasks_.pop_front();
      if (taskState && taskState->requestCancellation()) {
        return;
      }
    }
  }

  uint32_t getPipelinedCap(uint32_t pendingCallId, const std::vector<uint16_t>& pointerPath) {
    auto pendingIt = pendingCalls_.find(pendingCallId);
    if (pendingIt == pendingCalls_.end()) {
      throw std::runtime_error("unknown pending RPC call id: " + std::to_string(pendingCallId));
    }

    auto pipeline = pendingIt->second.pipeline.noop();
    for (auto pointerIndex : pointerPath) {
      pipeline = pipeline.getPointerField(pointerIndex);
    }
    auto cap = capnp::Capability::Client(pipeline.asCap());
    return addTarget(kj::mv(cap));
  }

  void processStreamingCall(uint32_t target, uint64_t interfaceId, uint16_t methodId,
                            const uint8_t* requestData, size_t requestSize,
                            const std::vector<uint32_t>& requestCaps,
                            kj::WaitScope& waitScope) {
    auto targetIt = targets_.find(target);
    if (targetIt == targets_.end()) {
      throw std::runtime_error("unknown RPC target capability id: " + std::to_string(target));
    }

    struct StreamingClientAccess final : public capnp::Capability::Client {
      explicit StreamingClientAccess(capnp::Capability::Client& client)
          : capnp::Capability::Client(client) {}
      using capnp::Capability::Client::newStreamingCall;
    };

    StreamingClientAccess client(targetIt->second);
    auto requestBuilder =
        client.newStreamingCall<capnp::AnyPointer>(interfaceId, methodId, kj::none, {});
    setRequestPayload(requestBuilder, requestData, requestSize, requestCaps);
    requestBuilder.send().wait(waitScope);
  }

  int64_t targetGetFd(kj::WaitScope& waitScope, uint32_t target) {
    auto targetIt = targets_.find(target);
    if (targetIt == targets_.end()) {
      throw std::runtime_error("unknown RPC target capability id: " + std::to_string(target));
    }
    auto fdMaybe = targetIt->second.getFd().wait(waitScope);
    KJ_IF_SOME(fd, fdMaybe) {
      return static_cast<int64_t>(fd);
    }
    return -1;
  }

  void targetWhenResolved(kj::WaitScope& waitScope, uint32_t target) {
    auto targetIt = targets_.find(target);
    if (targetIt == targets_.end()) {
      throw std::runtime_error("unknown RPC target capability id: " + std::to_string(target));
    }
    targetIt->second.whenResolved().wait(waitScope);
  }

  uint32_t targetWhenResolvedStart(uint32_t target) {
    auto targetIt = targets_.find(target);
    if (targetIt == targets_.end()) {
      throw std::runtime_error("unknown RPC target capability id: " + std::to_string(target));
    }
    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(targetIt->second.whenResolved());
    return addUnitPromise(PendingUnitPromise(kj::mv(promise), kj::mv(canceler)));
  }

  bool targetWhenResolvedPoll(kj::WaitScope& waitScope, uint32_t target) {
    auto targetIt = targets_.find(target);
    if (targetIt == targets_.end()) {
      throw std::runtime_error("unknown RPC target capability id: " + std::to_string(target));
    }
    return targetIt->second.whenResolved().poll(waitScope);
  }

  kj::String encodeTraceWithLean(const kj::Exception& e) {
    if (traceEncoderHandler_ == nullptr) {
      return kj::String();
    }
    lean_inc(traceEncoderHandler_);
    auto ioResult = lean_apply_2(traceEncoderHandler_, lean_mk_string(e.getDescription().cStr()),
                                 lean_box(0));
    if (lean_io_result_is_error(ioResult)) {
      lean_dec(ioResult);
      return kj::String();
    }
    auto traceObj = lean_io_result_take_value(ioResult);
    // Copy the Lean string before dropping its reference; `lean_string_cstr()` borrows storage.
    auto traceCopy = kj::str(lean_string_cstr(traceObj));
    lean_dec(traceObj);
    return traceCopy;
  }

  kj::String encodeTrace(const kj::Exception& e) {
    if (traceEncoderHandler_ != nullptr) {
      return encodeTraceWithLean(e);
    }
    if (traceEncoderEnabled_) {
      return kj::str("lean4-rpc-trace: ", e.getDescription());
    }
    return kj::String();
  }

  bool traceEncoderActive() const { return traceEncoderEnabled_ || traceEncoderHandler_ != nullptr; }

  kj::Maybe<kj::Function<kj::String(const kj::Exception&)>> makeTraceEncoderMaybe() {
    if (!traceEncoderActive()) {
      return kj::none;
    }
    return kj::Function<kj::String(const kj::Exception&)>(
        [this](const kj::Exception& e) { return encodeTrace(e); });
  }

  void applyTraceEncoder(TwoPartyRpcSystem& rpcSystem) {
    if (!traceEncoderActive()) {
      rpcSystem.setTraceEncoder([](const kj::Exception&) { return kj::String(); });
      return;
    }
    rpcSystem.setTraceEncoder([this](const kj::Exception& e) { return encodeTrace(e); });
  }

  void applyTraceEncoderToAllActiveConnections() {
    for (auto& entry : clients_) {
      applyTraceEncoder(*entry.second->rpcSystem);
    }
    for (auto& entry : networkClientPeers_) {
      applyTraceEncoder(*entry.second->rpcSystem);
    }
    for (auto& loopback : loopbackPeers_) {
      applyTraceEncoder(*loopback.second->serverRpcSystem);
    }
    for (auto& peer : networkServerPeers_) {
      applyTraceEncoder(*peer->rpcSystem);
    }
    for (auto& server : servers_) {
      for (auto& peer : server.second->peers) {
        applyTraceEncoder(*peer->rpcSystem);
      }
    }
  }

  void clearTraceEncoderHandler() {
    if (traceEncoderHandler_ != nullptr) {
      lean_dec(traceEncoderHandler_);
      traceEncoderHandler_ = nullptr;
    }
  }

  void setTraceEncoderEnabled(bool enabled) {
    clearTraceEncoderHandler();
    traceEncoderEnabled_ = enabled;
    applyTraceEncoderToAllActiveConnections();
  }

  void setTraceEncoderFromLean(lean_object* encoder) {
    clearTraceEncoderHandler();
    traceEncoderEnabled_ = false;
    traceEncoderHandler_ = encoder;
    applyTraceEncoderToAllActiveConnections();
  }

  kj::Promise<RawCallResult> processRawCall(uint32_t target, uint64_t interfaceId,
                                            uint16_t methodId,
                                            const uint8_t* requestData, size_t requestSize,
                                            const std::vector<uint32_t>& requestCaps) {
    auto targetIt = targets_.find(target);
    if (targetIt == targets_.end()) {
      throw std::runtime_error("unknown RPC target capability id: " + std::to_string(target));
    }

    debugLog(
        "rawcall.start",
        "target=" + std::to_string(target) + " interfaceId=" + std::to_string(interfaceId) +
            " methodId=" + std::to_string(methodId));

    auto requestBuilder = targetIt->second.typelessRequest(interfaceId, methodId, kj::none, {});
    setRequestPayload(requestBuilder, requestData, requestSize, requestCaps);
    return requestBuilder.send().then([this, target](capnp::Response<capnp::AnyPointer>&& response) {
      debugLog("rawcall.done", "target=" + std::to_string(target));
      return kj::evalNow([this, &response]() { return serializeResponse(response); });
    });
  }

  RawCallResult processCppCallWithAccept(
      kj::AsyncIoProvider& ioProvider, kj::WaitScope& waitScope, uint32_t serverId,
      uint32_t listenerId, const std::string& address, uint32_t portHint,
      uint64_t interfaceId, uint16_t methodId, const uint8_t* requestData, size_t requestSize,
      const std::vector<uint32_t>& requestCaps) {
    auto serverIt = servers_.find(serverId);
    if (serverIt == servers_.end()) {
      throw std::runtime_error("unknown RPC server id: " + std::to_string(serverId));
    }
    auto listenerIt = listeners_.find(listenerId);
    if (listenerIt == listeners_.end()) {
      throw std::runtime_error("unknown RPC listener id: " + std::to_string(listenerId));
    }

    auto addr = ioProvider.getNetwork().parseAddress(address.c_str(), portHint).wait(waitScope);
    auto connectPromise = addr->connect();
    auto acceptPromise = listenerIt->second->accept();
    auto stream = connectPromise.wait(waitScope).downcast<kj::AsyncCapabilityStream>();
    auto connection = acceptPromise.wait(waitScope).downcast<kj::AsyncCapabilityStream>();

    // Accept the incoming client connection and keep the server peer alive in runtime-owned state.
    networkServerPeers_.add(makeRuntimeServerPeerWithFds(*serverIt->second, kj::mv(connection)));
    kj::Array<capnp::word> responseWords;
    std::vector<uint8_t> responseCaps;
    {
      auto network = kj::heap<capnp::TwoPartyVatNetwork>(
          *stream, maxFdsPerMessage_, capnp::rpc::twoparty::Side::CLIENT);
      auto rpcSystem = kj::heap<TwoPartyRpcSystem>(capnp::makeRpcClient(*network));
      applyTraceEncoder(*rpcSystem);

      capnp::word scratch[4];
      memset(&scratch, 0, sizeof(scratch));
      capnp::MallocMessageBuilder message(scratch);
      auto vatId = message.getRoot<capnp::rpc::twoparty::VatId>();
      vatId.setSide(network->getSide() == capnp::rpc::twoparty::Side::CLIENT
                        ? capnp::rpc::twoparty::Side::SERVER
                        : capnp::rpc::twoparty::Side::CLIENT);
      auto target = rpcSystem->bootstrap(vatId);

      auto requestBuilder = target.typelessRequest(interfaceId, methodId, kj::none, {});
      setRequestPayload(requestBuilder, requestData, requestSize, requestCaps);

      auto response = requestBuilder.send().wait(waitScope);
      capnp::MallocMessageBuilder responseMessage;
      capnp::BuilderCapabilityTable responseCapTable;
      responseCapTable
          .imbue(responseMessage.getRoot<capnp::AnyPointer>())
          .setAs<capnp::AnyPointer>(response.getAs<capnp::AnyPointer>());

      responseWords = capnp::messageToFlatArray(responseMessage);

      auto responseCapTableEntries = responseCapTable.getTable();
      responseCaps.reserve(responseCapTableEntries.size() * 4);
      for (auto& maybeHook : responseCapTableEntries) {
        KJ_IF_SOME(hook, maybeHook) {
          auto cap = capnp::Capability::Client(hook->addRef());
          appendUint32Le(responseCaps, addTarget(kj::mv(cap)));
        } else {
          appendUint32Le(responseCaps, 0);
        }
      }
    }
    return RawCallResult{std::move(responseWords), std::move(responseCaps)};
  }

  RawCallResult processCppPipelinedCallWithAccept(
      kj::AsyncIoProvider& ioProvider, kj::WaitScope& waitScope, uint32_t serverId,
      uint32_t listenerId, const std::string& address, uint32_t portHint,
      uint64_t interfaceId, uint16_t methodId, const uint8_t* requestData, size_t requestSize,
      const std::vector<uint32_t>& requestCaps, const uint8_t* pipelinedRequestData,
      size_t pipelinedRequestSize,
      const std::vector<uint32_t>& pipelinedRequestCaps) {
    auto serverIt = servers_.find(serverId);
    if (serverIt == servers_.end()) {
      throw std::runtime_error("unknown RPC server id: " + std::to_string(serverId));
    }
    auto listenerIt = listeners_.find(listenerId);
    if (listenerIt == listeners_.end()) {
      throw std::runtime_error("unknown RPC listener id: " + std::to_string(listenerId));
    }

    auto addr = ioProvider.getNetwork().parseAddress(address.c_str(), portHint).wait(waitScope);
    auto connectPromise = addr->connect();
    auto acceptPromise = listenerIt->second->accept();
    auto stream = connectPromise.wait(waitScope).downcast<kj::AsyncCapabilityStream>();
    auto connection = acceptPromise.wait(waitScope).downcast<kj::AsyncCapabilityStream>();

    // Accept the incoming client connection and keep the server peer alive in runtime-owned state.
    networkServerPeers_.add(makeRuntimeServerPeerWithFds(*serverIt->second, kj::mv(connection)));

    auto network = kj::heap<capnp::TwoPartyVatNetwork>(
        *stream, maxFdsPerMessage_, capnp::rpc::twoparty::Side::CLIENT);
    auto rpcSystem = kj::heap<TwoPartyRpcSystem>(capnp::makeRpcClient(*network));
    applyTraceEncoder(*rpcSystem);

    capnp::word scratch[4];
    memset(&scratch, 0, sizeof(scratch));
    capnp::MallocMessageBuilder message(scratch);
    auto vatId = message.getRoot<capnp::rpc::twoparty::VatId>();
    vatId.setSide(network->getSide() == capnp::rpc::twoparty::Side::CLIENT
                      ? capnp::rpc::twoparty::Side::SERVER
                      : capnp::rpc::twoparty::Side::CLIENT);
    auto target = rpcSystem->bootstrap(vatId);

    auto firstRequest = target.typelessRequest(interfaceId, methodId, kj::none, {});
    setRequestPayload(firstRequest, requestData, requestSize, requestCaps);
    auto firstPromise = firstRequest.send();

    auto pipelinedTarget = capnp::Capability::Client(firstPromise.noop().asCap());
    auto pipelinedRequestBuilder =
        pipelinedTarget.typelessRequest(interfaceId, methodId, kj::none, {});
    setRequestPayload(pipelinedRequestBuilder, pipelinedRequestData, pipelinedRequestSize,
                      pipelinedRequestCaps);
    auto pipelinedResponse = pipelinedRequestBuilder.send().wait(waitScope);
    return serializeResponse(pipelinedResponse);
  }

  bool dropTargetIfPresent(uint32_t target) {
    auto erased = targets_.erase(target);
    releasePeerOwnership(target, loopbackPeers_, loopbackPeerOwnerByTarget_,
                         loopbackPeerOwnerRefCount_);
    releasePeerOwnership(target, networkClientPeers_, networkPeerOwnerByTarget_,
                         networkPeerOwnerRefCount_);
    return erased > 0;
  }

  uint32_t retainTarget(uint32_t target) {
    auto targetIt = targets_.find(target);
    if (targetIt == targets_.end()) {
      throw std::runtime_error("unknown RPC target capability id: " + std::to_string(target));
    }
    auto retainedTarget = addTarget(targetIt->second);
    retainPeerOwnership(target, retainedTarget, loopbackPeerOwnerByTarget_,
                        loopbackPeerOwnerRefCount_);
    retainPeerOwnership(target, retainedTarget, networkPeerOwnerByTarget_,
                        networkPeerOwnerRefCount_);
    return retainedTarget;
  }

  void releaseTarget(uint32_t target) {
    if (!dropTargetIfPresent(target)) {
      throw std::runtime_error("unknown RPC target capability id: " + std::to_string(target));
    }
  }

  void releaseTargets(const std::vector<uint32_t>& targets) {
    for (auto target : targets) {
      if (target == 0) {
        continue;
      }
      if (!dropTargetIfPresent(target)) {
        throw std::runtime_error("unknown RPC target capability id: " + std::to_string(target));
      }
    }
  }

  std::pair<uint32_t, uint32_t> newPromiseCapability() {
    auto paf = kj::newPromiseAndFulfiller<kj::Own<capnp::ClientHook>>();
    auto promiseHook = capnp::newLocalPromiseClient(kj::mv(paf.promise));
    auto promiseTarget = capnp::Capability::Client(kj::mv(promiseHook));
    auto targetId = addTarget(kj::mv(promiseTarget));
    auto fulfillerId = addPromiseCapabilityFulfiller(
        PendingPromiseCapability(kj::mv(paf.fulfiller), targetId));
    return {targetId, fulfillerId};
  }

  static kj::Exception::Type decodeRemoteExceptionType(uint8_t typeTag) {
    switch (typeTag) {
      case 0:
        return kj::Exception::Type::FAILED;
      case 1:
        return kj::Exception::Type::OVERLOADED;
      case 2:
        return kj::Exception::Type::DISCONNECTED;
      case 3:
        return kj::Exception::Type::UNIMPLEMENTED;
      default:
        return kj::Exception::Type::FAILED;
    }
  }

  void promiseCapabilityFulfillNow(uint32_t fulfillerId, uint32_t target) {
    auto promiseIt = promiseCapabilityFulfillers_.find(fulfillerId);
    if (promiseIt == promiseCapabilityFulfillers_.end()) {
      throw std::runtime_error("unknown RPC promise capability fulfiller id: " +
                               std::to_string(fulfillerId));
    }
    if (target == 0) {
      throw std::runtime_error("cannot fulfill promise capability with null target");
    }
    if (target == promiseIt->second.promiseTarget) {
      throw std::runtime_error("cannot fulfill promise capability with itself");
    }

    auto targetIt = targets_.find(target);
    if (targetIt == targets_.end()) {
      throw std::runtime_error("unknown RPC target capability id: " + std::to_string(target));
    }
    capnp::Capability::Client cap = targetIt->second;
    promiseIt->second.fulfiller->fulfill(capnp::ClientHook::from(kj::mv(cap)));
    promiseCapabilityFulfillers_.erase(promiseIt);
  }

  void promiseCapabilityFulfill(uint32_t fulfillerId, uint32_t target) {
    if (heldPromiseCapabilityFulfillerIds_.find(fulfillerId) !=
        heldPromiseCapabilityFulfillerIds_.end()) {
      throw std::runtime_error("promise capability fulfiller already has a held resolve");
    }

    if (!holdPromiseCapabilityResolves_) {
      promiseCapabilityFulfillNow(fulfillerId, target);
      return;
    }

    auto promiseIt = promiseCapabilityFulfillers_.find(fulfillerId);
    if (promiseIt == promiseCapabilityFulfillers_.end()) {
      throw std::runtime_error("unknown RPC promise capability fulfiller id: " +
                               std::to_string(fulfillerId));
    }
    if (target == 0) {
      throw std::runtime_error("cannot fulfill promise capability with null target");
    }
    if (target == promiseIt->second.promiseTarget) {
      throw std::runtime_error("cannot fulfill promise capability with itself");
    }
    if (targets_.find(target) == targets_.end()) {
      throw std::runtime_error("unknown RPC target capability id: " + std::to_string(target));
    }

    heldPromiseCapabilityResolves_.push_back({fulfillerId, target});
    heldPromiseCapabilityFulfillerIds_.insert(fulfillerId);
  }

  void dropHeldPromiseCapabilityResolve(uint32_t fulfillerId) {
    if (heldPromiseCapabilityFulfillerIds_.erase(fulfillerId) == 0) {
      return;
    }
    for (auto it = heldPromiseCapabilityResolves_.begin();
         it != heldPromiseCapabilityResolves_.end(); ++it) {
      if (it->fulfillerId == fulfillerId) {
        heldPromiseCapabilityResolves_.erase(it);
        return;
      }
    }
  }

  uint64_t flushHeldPromiseCapabilityResolves() {
    uint64_t flushed = 0;
    while (!heldPromiseCapabilityResolves_.empty()) {
      auto request = heldPromiseCapabilityResolves_.front();
      heldPromiseCapabilityResolves_.pop_front();
      heldPromiseCapabilityFulfillerIds_.erase(request.fulfillerId);
      promiseCapabilityFulfillNow(request.fulfillerId, request.target);
      ++flushed;
    }
    return flushed;
  }

  void setResolveHoldEnabled(bool enabled) { holdPromiseCapabilityResolves_ = enabled; }

  uint64_t heldResolveCount() const {
    return static_cast<uint64_t>(heldPromiseCapabilityResolves_.size());
  }

  void promiseCapabilityReject(uint32_t fulfillerId, uint8_t exceptionTypeTag,
                               const std::string& message,
                               const uint8_t* detailBytesData, size_t detailBytesSize) {
    dropHeldPromiseCapabilityResolve(fulfillerId);
    auto promiseIt = promiseCapabilityFulfillers_.find(fulfillerId);
    if (promiseIt == promiseCapabilityFulfillers_.end()) {
      throw std::runtime_error("unknown RPC promise capability fulfiller id: " +
                               std::to_string(fulfillerId));
    }
    auto type = decodeRemoteExceptionType(exceptionTypeTag);
    auto ex = kj::Exception(type, __FILE__, __LINE__, kj::str(message.c_str()));
    if (detailBytesSize != 0) {
      auto copy = kj::heapArray<kj::byte>(detailBytesSize);
      std::memcpy(copy.begin(), detailBytesData, detailBytesSize);
      ex.setDetail(1, kj::mv(copy));
    }
    promiseIt->second.fulfiller->reject(kj::mv(ex));
    promiseCapabilityFulfillers_.erase(promiseIt);
  }

  void promiseCapabilityRelease(uint32_t fulfillerId) {
    dropHeldPromiseCapabilityResolve(fulfillerId);
    auto promiseIt = promiseCapabilityFulfillers_.find(fulfillerId);
    if (promiseIt == promiseCapabilityFulfillers_.end()) {
      return;
    }
    auto ex = kj::Exception(kj::Exception::Type::DISCONNECTED, __FILE__, __LINE__,
                            kj::str("promise capability fulfiller released"));
    promiseIt->second.fulfiller->reject(kj::mv(ex));
    promiseCapabilityFulfillers_.erase(promiseIt);
  }

  uint32_t addListener(kj::Own<kj::ConnectionReceiver>&& listener) {
    uint32_t listenerId = nextListenerId_++;
    while (listeners_.find(listenerId) != listeners_.end()) {
      listenerId = nextListenerId_++;
    }
    listeners_.emplace(listenerId, kj::mv(listener));
    return listenerId;
  }

  uint32_t addClient(kj::Own<NetworkClientPeer>&& client) {
    uint32_t clientId = nextClientId_++;
    while (clients_.find(clientId) != clients_.end()) {
      clientId = nextClientId_++;
    }
    clients_.emplace(clientId, kj::mv(client));
    return clientId;
  }

  uint32_t addServer(kj::Own<RuntimeServer>&& server) {
    uint32_t serverId = nextServerId_++;
    while (servers_.find(serverId) != servers_.end()) {
      serverId = nextServerId_++;
    }
    servers_.emplace(serverId, kj::mv(server));
    return serverId;
  }

  uint32_t addTransport(kj::Own<kj::AsyncCapabilityStream>&& stream) {
    uint32_t transportId = nextTransportId_++;
    while (transports_.find(transportId) != transports_.end()) {
      transportId = nextTransportId_++;
    }
    transports_.emplace(transportId, kj::mv(stream));
    return transportId;
  }

  kj::Own<kj::AsyncCapabilityStream> takeTransport(uint32_t transportId) {
    auto transportIt = transports_.find(transportId);
    if (transportIt == transports_.end()) {
      throw std::runtime_error("unknown RPC transport id: " + std::to_string(transportId));
    }
    auto stream = kj::mv(transportIt->second);
    transports_.erase(transportIt);
    return stream;
  }

  std::pair<uint32_t, uint32_t> newTransportPipe(kj::AsyncIoProvider& ioProvider) {
    auto pipe = ioProvider.newCapabilityPipe();
    auto firstId = addTransport(kj::mv(pipe.ends[0]));
    auto secondId = addTransport(kj::mv(pipe.ends[1]));
    return {firstId, secondId};
  }

  uint32_t newTransportFromFd(kj::LowLevelAsyncIoProvider& lowLevelProvider, uint32_t fd) {
#if defined(_WIN32)
    (void)lowLevelProvider;
    (void)fd;
    throw std::runtime_error("newTransportFromFd is not supported on Windows");
#else
    constexpr uint32_t maxInt = static_cast<uint32_t>(std::numeric_limits<int>::max());
    if (fd > maxInt) {
      throw std::runtime_error("fd exceeds platform int range");
    }
    int fdCopy = dup(static_cast<int>(fd));
    if (fdCopy < 0) {
      throw std::runtime_error("dup() failed while creating RPC transport from fd");
    }
    auto stream = lowLevelProvider.wrapUnixSocketFd(kj::LowLevelAsyncIoProvider::OwnFd{fdCopy});
    return addTransport(kj::mv(stream));
#endif
  }

  uint32_t newTransportFromFdTake(kj::LowLevelAsyncIoProvider& lowLevelProvider, uint32_t fd) {
#if defined(_WIN32)
    (void)lowLevelProvider;
    (void)fd;
    throw std::runtime_error("newTransportFromFdTake is not supported on Windows");
#else
    constexpr uint32_t maxInt = static_cast<uint32_t>(std::numeric_limits<int>::max());
    if (fd > maxInt) {
      throw std::runtime_error("fd exceeds platform int range");
    }
    auto stream = lowLevelProvider.wrapUnixSocketFd(
        kj::LowLevelAsyncIoProvider::OwnFd{static_cast<int>(fd)});
    return addTransport(kj::mv(stream));
#endif
  }

  void releaseTransport(uint32_t transportId) {
    auto erased = transports_.erase(transportId);
    if (erased == 0) {
      throw std::runtime_error("unknown RPC transport id: " + std::to_string(transportId));
    }
  }

  int64_t transportGetFd(kj::WaitScope& waitScope, uint32_t transportId) {
    (void)waitScope;
    auto transportIt = transports_.find(transportId);
    if (transportIt == transports_.end()) {
      throw std::runtime_error("unknown RPC transport id: " + std::to_string(transportId));
    }
    auto fdMaybe = transportIt->second->getFd();
    if (fdMaybe == kj::none) {
      return -1;
    }
    return static_cast<int64_t>(KJ_ASSERT_NONNULL(fdMaybe));
  }

  uint32_t connectTargetPeer(kj::Own<NetworkClientPeer>&& peer) {
    capnp::word scratch[4];
    memset(&scratch, 0, sizeof(scratch));
    capnp::MallocMessageBuilder message(scratch);
    auto vatId = message.getRoot<capnp::rpc::twoparty::VatId>();
    vatId.setSide(peer->network->getSide() == capnp::rpc::twoparty::Side::CLIENT
                      ? capnp::rpc::twoparty::Side::SERVER
                      : capnp::rpc::twoparty::Side::CLIENT);
    auto cap = peer->rpcSystem->bootstrap(vatId);

    auto targetId = addTarget(kj::mv(cap));
    networkClientPeers_.emplace(targetId, kj::mv(peer));
    networkPeerOwnerByTarget_.emplace(targetId, targetId);
    networkPeerOwnerRefCount_.emplace(targetId, 1);
    return targetId;
  }

  kj::Own<NetworkClientPeer> connectPeer(kj::AsyncIoProvider& ioProvider, kj::WaitScope& waitScope,
                                         const std::string& address, uint32_t portHint) {
    auto addr = ioProvider.getNetwork().parseAddress(address.c_str(), portHint).wait(waitScope);
    auto stream = addr->connect().wait(waitScope).downcast<kj::AsyncCapabilityStream>();
    auto network = kj::heap<capnp::TwoPartyVatNetwork>(
        *stream, maxFdsPerMessage_, capnp::rpc::twoparty::Side::CLIENT);
    auto rpcSystem = kj::heap<TwoPartyRpcSystem>(capnp::makeRpcClient(*network));
    applyTraceEncoder(*rpcSystem);

    auto peer = kj::heap<NetworkClientPeer>();
    peer->stream = kj::mv(stream);
    peer->network = kj::mv(network);
    peer->rpcSystem = kj::mv(rpcSystem);
    return peer;
  }

  kj::Promise<kj::Own<NetworkClientPeer>> connectPeerStart(kj::AsyncIoProvider& ioProvider,
                                                            std::string address,
                                                            uint32_t portHint) {
    return ioProvider.getNetwork()
        .parseAddress(address.c_str(), portHint)
        .then([](kj::Own<kj::NetworkAddress>&& addr) { return addr->connect(); })
        .then([this](kj::Own<kj::AsyncIoStream>&& stream) {
          auto capStream = stream.downcast<kj::AsyncCapabilityStream>();
          auto network = kj::heap<capnp::TwoPartyVatNetwork>(
              *capStream, maxFdsPerMessage_, capnp::rpc::twoparty::Side::CLIENT);
          auto rpcSystem = kj::heap<TwoPartyRpcSystem>(capnp::makeRpcClient(*network));
          applyTraceEncoder(*rpcSystem);

          auto peer = kj::heap<NetworkClientPeer>();
          peer->stream = kj::mv(capStream);
          peer->network = kj::mv(network);
          peer->rpcSystem = kj::mv(rpcSystem);
          return peer;
        });
  }

  uint32_t connectTarget(kj::AsyncIoProvider& ioProvider, kj::WaitScope& waitScope,
                         const std::string& address, uint32_t portHint) {
    auto peer = connectPeer(ioProvider, waitScope, address, portHint);
    return connectTargetPeer(kj::mv(peer));
  }

  uint32_t connectTargetStart(kj::AsyncIoProvider& ioProvider, std::string address,
                              uint32_t portHint) {
    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(
        connectPeerStart(ioProvider, std::move(address), portHint)
            .then([this](kj::Own<NetworkClientPeer>&& peer) {
              return connectTargetPeer(kj::mv(peer));
            }));
    return addRegisterPromise(PendingRegisterPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t connectTargetFd(kj::LowLevelAsyncIoProvider& lowLevelProvider, uint32_t fd) {
#if defined(_WIN32)
    (void)lowLevelProvider;
    (void)fd;
    throw std::runtime_error("connectFd is not supported on Windows");
#else
    constexpr uint32_t maxInt = static_cast<uint32_t>(std::numeric_limits<int>::max());
    if (fd > maxInt) {
      throw std::runtime_error("fd exceeds platform int range");
    }
    int fdCopy = dup(static_cast<int>(fd));
    if (fdCopy < 0) {
      throw std::runtime_error("dup() failed while connecting RPC target fd");
    }
    auto stream = lowLevelProvider.wrapUnixSocketFd(
        kj::LowLevelAsyncIoProvider::OwnFd{fdCopy});
    auto network = kj::heap<capnp::TwoPartyVatNetwork>(
        *stream, maxFdsPerMessage_, capnp::rpc::twoparty::Side::CLIENT);
    auto rpcSystem = kj::heap<TwoPartyRpcSystem>(capnp::makeRpcClient(*network));
    applyTraceEncoder(*rpcSystem);

    auto peer = kj::heap<NetworkClientPeer>();
    peer->stream = kj::mv(stream);
    peer->network = kj::mv(network);
    peer->rpcSystem = kj::mv(rpcSystem);
    return connectTargetPeer(kj::mv(peer));
#endif
  }

  uint32_t connectTargetTransport(uint32_t transportId) {
    auto stream = takeTransport(transportId);
    auto network = kj::heap<capnp::TwoPartyVatNetwork>(
        *stream, maxFdsPerMessage_, capnp::rpc::twoparty::Side::CLIENT);
    auto rpcSystem = kj::heap<TwoPartyRpcSystem>(capnp::makeRpcClient(*network));
    applyTraceEncoder(*rpcSystem);

    auto peer = kj::heap<NetworkClientPeer>();
    peer->stream = kj::mv(stream);
    peer->network = kj::mv(network);
    peer->rpcSystem = kj::mv(rpcSystem);
    return connectTargetPeer(kj::mv(peer));
  }

  uint32_t newClient(kj::AsyncIoProvider& ioProvider, kj::WaitScope& waitScope,
                     const std::string& address, uint32_t portHint) {
    auto peer = connectPeer(ioProvider, waitScope, address, portHint);
    return addClient(kj::mv(peer));
  }

  uint32_t newClientStart(kj::AsyncIoProvider& ioProvider, std::string address,
                          uint32_t portHint) {
    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(
        connectPeerStart(ioProvider, std::move(address), portHint)
            .then([this](kj::Own<NetworkClientPeer>&& peer) { return addClient(kj::mv(peer)); }));
    return addRegisterPromise(PendingRegisterPromise(kj::mv(promise), kj::mv(canceler)));
  }

  void releaseClient(uint32_t clientId) {
    auto erased = clients_.erase(clientId);
    if (erased == 0) {
      throw std::runtime_error("unknown RPC client id: " + std::to_string(clientId));
    }
  }

  uint32_t clientBootstrap(uint32_t clientId) {
    auto clientIt = clients_.find(clientId);
    if (clientIt == clients_.end()) {
      throw std::runtime_error("unknown RPC client id: " + std::to_string(clientId));
    }
    capnp::word scratch[4];
    memset(&scratch, 0, sizeof(scratch));
    capnp::MallocMessageBuilder message(scratch);
    auto vatId = message.getRoot<capnp::rpc::twoparty::VatId>();
    vatId.setSide(clientIt->second->network->getSide() == capnp::rpc::twoparty::Side::CLIENT
                      ? capnp::rpc::twoparty::Side::SERVER
                      : capnp::rpc::twoparty::Side::CLIENT);
    return addTarget(clientIt->second->rpcSystem->bootstrap(vatId));
  }

  void clientOnDisconnect(kj::WaitScope& waitScope, uint32_t clientId) {
    auto clientIt = clients_.find(clientId);
    if (clientIt == clients_.end()) {
      throw std::runtime_error("unknown RPC client id: " + std::to_string(clientId));
    }
    clientIt->second->network->onDisconnect().wait(waitScope);
  }

  uint32_t clientOnDisconnectStart(uint32_t clientId) {
    auto clientIt = clients_.find(clientId);
    if (clientIt == clients_.end()) {
      throw std::runtime_error("unknown RPC client id: " + std::to_string(clientId));
    }
    auto canceler = kj::heap<kj::Canceler>();
    auto promise = canceler->wrap(clientIt->second->network->onDisconnect());
    return addUnitPromise(PendingUnitPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint64_t clientQueueSize(uint32_t clientId) {
    auto clientIt = clients_.find(clientId);
    if (clientIt == clients_.end()) {
      throw std::runtime_error("unknown RPC client id: " + std::to_string(clientId));
    }
    return static_cast<uint64_t>(clientIt->second->network->getCurrentQueueSize());
  }

  uint64_t clientQueueCount(uint32_t clientId) {
    auto clientIt = clients_.find(clientId);
    if (clientIt == clients_.end()) {
      throw std::runtime_error("unknown RPC client id: " + std::to_string(clientId));
    }
    return static_cast<uint64_t>(clientIt->second->network->getCurrentQueueCount());
  }

  uint64_t clientOutgoingWaitNanos(uint32_t clientId) {
    auto clientIt = clients_.find(clientId);
    if (clientIt == clients_.end()) {
      throw std::runtime_error("unknown RPC client id: " + std::to_string(clientId));
    }
    auto wait = clientIt->second->network->getOutgoingMessageWaitTime();
    return static_cast<uint64_t>(wait / kj::NANOSECONDS);
  }

  uint64_t targetCount() const {
    return static_cast<uint64_t>(targets_.size());
  }

  uint64_t listenerCount() const {
    return static_cast<uint64_t>(listeners_.size());
  }

  uint64_t clientCount() const {
    return static_cast<uint64_t>(clients_.size());
  }

  uint64_t serverCount() const {
    return static_cast<uint64_t>(servers_.size());
  }

  uint64_t pendingCallCount() const {
    return static_cast<uint64_t>(pendingCalls_.size());
  }

  void clientSetFlowLimit(uint32_t clientId, uint64_t words) {
    auto clientIt = clients_.find(clientId);
    if (clientIt == clients_.end()) {
      throw std::runtime_error("unknown RPC client id: " + std::to_string(clientId));
    }
    constexpr uint64_t maxWords = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
    if (words > maxWords) {
      throw std::runtime_error("flow limit exceeds platform size_t range");
    }
    clientIt->second->rpcSystem->setFlowLimit(static_cast<size_t>(words));
  }

  kj::Own<NetworkServerPeer> makeServerPeer(capnp::Capability::Client bootstrap,
                                            kj::Own<kj::AsyncIoStream>&& connection) {
    auto peer = kj::heap<NetworkServerPeer>();
    peer->ioConnection = kj::mv(connection);
    auto& ioConnection = KJ_ASSERT_NONNULL(peer->ioConnection);
    peer->network = kj::heap<capnp::TwoPartyVatNetwork>(
        *ioConnection, capnp::rpc::twoparty::Side::SERVER);
    peer->rpcSystem = kj::heap<TwoPartyRpcSystem>(
        capnp::makeRpcServer(*peer->network, kj::mv(bootstrap)));
    applyTraceEncoder(*peer->rpcSystem);
    return peer;
  }

  kj::Own<NetworkServerPeer> makeServerPeer(
      capnp::BootstrapFactory<capnp::rpc::twoparty::VatId>& bootstrapFactory,
      kj::Own<kj::AsyncIoStream>&& connection) {
    auto peer = kj::heap<NetworkServerPeer>();
    peer->ioConnection = kj::mv(connection);
    auto& ioConnection = KJ_ASSERT_NONNULL(peer->ioConnection);
    peer->network = kj::heap<capnp::TwoPartyVatNetwork>(
        *ioConnection, capnp::rpc::twoparty::Side::SERVER);
    peer->rpcSystem =
        kj::heap<TwoPartyRpcSystem>(capnp::makeRpcServer(*peer->network, bootstrapFactory));
    applyTraceEncoder(*peer->rpcSystem);
    return peer;
  }

  kj::Own<NetworkServerPeer> makeServerPeerWithFds(capnp::Capability::Client bootstrap,
                                                    kj::Own<kj::AsyncCapabilityStream>&& connection) {
    auto peer = kj::heap<NetworkServerPeer>();
    peer->capConnection = kj::mv(connection);
    auto& capConnection = KJ_ASSERT_NONNULL(peer->capConnection);
    peer->network = kj::heap<capnp::TwoPartyVatNetwork>(
        *capConnection, maxFdsPerMessage_, capnp::rpc::twoparty::Side::SERVER);
    peer->rpcSystem = kj::heap<TwoPartyRpcSystem>(
        capnp::makeRpcServer(*peer->network, kj::mv(bootstrap)));
    applyTraceEncoder(*peer->rpcSystem);
    return peer;
  }

  kj::Own<NetworkServerPeer> makeServerPeerWithFds(
      capnp::BootstrapFactory<capnp::rpc::twoparty::VatId>& bootstrapFactory,
      kj::Own<kj::AsyncCapabilityStream>&& connection) {
    auto peer = kj::heap<NetworkServerPeer>();
    peer->capConnection = kj::mv(connection);
    auto& capConnection = KJ_ASSERT_NONNULL(peer->capConnection);
    peer->network = kj::heap<capnp::TwoPartyVatNetwork>(
        *capConnection, maxFdsPerMessage_, capnp::rpc::twoparty::Side::SERVER);
    peer->rpcSystem =
        kj::heap<TwoPartyRpcSystem>(capnp::makeRpcServer(*peer->network, bootstrapFactory));
    applyTraceEncoder(*peer->rpcSystem);
    return peer;
  }

  kj::Own<NetworkServerPeer> makeRuntimeServerPeerWithFds(
      RuntimeServer& server, kj::Own<kj::AsyncCapabilityStream>&& connection) {
    KJ_IF_SOME(bootstrapFactory, server.bootstrapFactory) {
      return makeServerPeerWithFds(*bootstrapFactory, kj::mv(connection));
    }
    auto bootstrap = KJ_ASSERT_NONNULL(server.bootstrap);
    return makeServerPeerWithFds(bootstrap, kj::mv(connection));
  }

  uint32_t listenLoopback(kj::AsyncIoProvider& ioProvider, kj::WaitScope& waitScope,
                      const std::string& address, uint32_t portHint) {
    auto addr = ioProvider.getNetwork().parseAddress(address.c_str(), portHint).wait(waitScope);
    auto listener = addr->listen();
    return addListener(kj::mv(listener));
  }

  void acceptLoopback(kj::WaitScope& waitScope, uint32_t listenerId) {
    auto listenerIt = listeners_.find(listenerId);
    if (listenerIt == listeners_.end()) {
      throw std::runtime_error("unknown RPC listener id: " + std::to_string(listenerId));
    }
    auto connection = listenerIt->second->accept().wait(waitScope);
    networkServerPeers_.add(makeServerPeer(
        capnp::Capability::Client(kj::heap<LoopbackCapabilityServer>()), kj::mv(connection)));
  }

  void releaseListener(uint32_t listenerId) {
    auto erased = listeners_.erase(listenerId);
    if (erased == 0) {
      throw std::runtime_error("unknown RPC listener id: " + std::to_string(listenerId));
    }
  }

  uint32_t newServer(uint32_t bootstrapTarget) {
    auto targetIt = targets_.find(bootstrapTarget);
    if (targetIt == targets_.end()) {
      throw std::runtime_error("unknown RPC target capability id: " +
                               std::to_string(bootstrapTarget));
    }
    auto server = kj::heap<RuntimeServer>(targetIt->second);
    return addServer(kj::mv(server));
  }

  uint32_t newServerWithBootstrapFactory(lean_object* bootstrapFactory) {
    auto factory = kj::heap<LeanBootstrapFactory>(*this, bootstrapFactory);
    auto server = kj::heap<RuntimeServer>(kj::mv(factory));
    return addServer(kj::mv(server));
  }

  void releaseServer(uint32_t serverId) {
    auto erased = servers_.erase(serverId);
    if (erased == 0) {
      throw std::runtime_error("unknown RPC server id: " + std::to_string(serverId));
    }
  }

  uint32_t serverListen(kj::AsyncIoProvider& ioProvider, kj::WaitScope& waitScope,
                        uint32_t serverId, const std::string& address, uint32_t portHint) {
    auto serverIt = servers_.find(serverId);
    if (serverIt == servers_.end()) {
      throw std::runtime_error("unknown RPC server id: " + std::to_string(serverId));
    }
    auto addr = ioProvider.getNetwork().parseAddress(address.c_str(), portHint).wait(waitScope);
    auto listener = addr->listen();
    return addListener(kj::mv(listener));
  }

  void serverAccept(kj::WaitScope& waitScope, uint32_t serverId, uint32_t listenerId) {
    auto serverIt = servers_.find(serverId);
    if (serverIt == servers_.end()) {
      throw std::runtime_error("unknown RPC server id: " + std::to_string(serverId));
    }
    auto listenerIt = listeners_.find(listenerId);
    if (listenerIt == listeners_.end()) {
      throw std::runtime_error("unknown RPC listener id: " + std::to_string(listenerId));
    }

    auto connection = listenerIt->second->accept().wait(waitScope).downcast<kj::AsyncCapabilityStream>();
    auto peer = makeRuntimeServerPeerWithFds(*serverIt->second, kj::mv(connection));
    serverIt->second->peers.add(kj::mv(peer));
  }

  uint32_t serverAcceptStart(uint32_t serverId, uint32_t listenerId) {
    auto canceler = kj::heap<kj::Canceler>();
    kj::Promise<void> ready = kj::READY_NOW;
    auto promise = canceler->wrap(kj::mv(ready)
                                      .then([this, listenerId]() {
                                        auto listenerIt = listeners_.find(listenerId);
                                        if (listenerIt == listeners_.end()) {
                                          throw std::runtime_error("unknown RPC listener id: " +
                                                                   std::to_string(listenerId));
                                        }
                                        return listenerIt->second->accept();
                                      })
                                      .then([this, serverId](kj::Own<kj::AsyncIoStream>&& conn) {
                                        auto serverIt = servers_.find(serverId);
                                        if (serverIt == servers_.end()) {
                                          throw std::runtime_error("unknown RPC server id: " +
                                                                   std::to_string(serverId));
                                        }
                                        auto connection =
                                            conn.downcast<kj::AsyncCapabilityStream>();
                                        auto peer = makeRuntimeServerPeerWithFds(
                                            *serverIt->second, kj::mv(connection));
                                        serverIt->second->peers.add(kj::mv(peer));
                                      }));
    return addUnitPromise(PendingUnitPromise(kj::mv(promise), kj::mv(canceler)));
  }

  void serverAcceptFd(kj::LowLevelAsyncIoProvider& lowLevelProvider, uint32_t serverId,
                      uint32_t fd) {
#if defined(_WIN32)
    (void)lowLevelProvider;
    (void)serverId;
    (void)fd;
    throw std::runtime_error("serverAcceptFd is not supported on Windows");
#else
    auto serverIt = servers_.find(serverId);
    if (serverIt == servers_.end()) {
      throw std::runtime_error("unknown RPC server id: " + std::to_string(serverId));
    }

    constexpr uint32_t maxInt = static_cast<uint32_t>(std::numeric_limits<int>::max());
    if (fd > maxInt) {
      throw std::runtime_error("fd exceeds platform int range");
    }
    int fdCopy = dup(static_cast<int>(fd));
    if (fdCopy < 0) {
      throw std::runtime_error("dup() failed while accepting RPC server fd");
    }
    auto connection = lowLevelProvider.wrapUnixSocketFd(
        kj::LowLevelAsyncIoProvider::OwnFd{fdCopy});
    auto peer = makeRuntimeServerPeerWithFds(*serverIt->second, kj::mv(connection));
    serverIt->second->peers.add(kj::mv(peer));
#endif
  }

  void serverAcceptTransport(uint32_t serverId, uint32_t transportId) {
    auto serverIt = servers_.find(serverId);
    if (serverIt == servers_.end()) {
      throw std::runtime_error("unknown RPC server id: " + std::to_string(serverId));
    }
    auto connection = takeTransport(transportId);
    auto peer = makeRuntimeServerPeerWithFds(*serverIt->second, kj::mv(connection));
    serverIt->second->peers.add(kj::mv(peer));
  }

  void serverDrain(kj::WaitScope& waitScope, uint32_t serverId) {
    auto serverIt = servers_.find(serverId);
    if (serverIt == servers_.end()) {
      throw std::runtime_error("unknown RPC server id: " + std::to_string(serverId));
    }
    for (auto& peer : serverIt->second->peers) {
      peer->network->onDisconnect().wait(waitScope);
    }
  }

  uint32_t serverDrainStart(uint32_t serverId) {
    auto serverIt = servers_.find(serverId);
    if (serverIt == servers_.end()) {
      throw std::runtime_error("unknown RPC server id: " + std::to_string(serverId));
    }
    auto canceler = kj::heap<kj::Canceler>();
    auto promises = kj::heapArrayBuilder<kj::Promise<void>>(serverIt->second->peers.size());
    for (auto& peer : serverIt->second->peers) {
      promises.add(peer->network->onDisconnect());
    }
    auto promise = canceler->wrap(kj::joinPromises(promises.finish()));
    return addUnitPromise(PendingUnitPromise(kj::mv(promise), kj::mv(canceler)));
  }

  uint32_t registerLoopbackTarget(kj::AsyncIoProvider& ioProvider,
                                  capnp::Capability::Client bootstrap) {
    auto pipe = ioProvider.newCapabilityPipe();
    auto serverStream = kj::mv(pipe.ends[0]);
    auto serverNetwork = kj::heap<capnp::TwoPartyVatNetwork>(
        *serverStream, 2, capnp::rpc::twoparty::Side::SERVER);
    auto serverRpcSystem =
        kj::heap<TwoPartyRpcSystem>(capnp::makeRpcServer(*serverNetwork, kj::mv(bootstrap)));
    applyTraceEncoder(*serverRpcSystem);

    auto client = kj::heap<capnp::TwoPartyClient>(*pipe.ends[1], 2);
    auto cap = client->bootstrap();

    auto peer = kj::heap<LoopbackPeer>();
    peer->clientStream = kj::mv(pipe.ends[1]);
    peer->serverStream = kj::mv(serverStream);
    peer->serverNetwork = kj::mv(serverNetwork);
    peer->serverRpcSystem = kj::mv(serverRpcSystem);
    peer->client = kj::mv(client);

    auto targetId = addTarget(kj::mv(cap));
    loopbackPeers_.emplace(targetId, kj::mv(peer));
    loopbackPeerOwnerByTarget_.emplace(targetId, targetId);
    loopbackPeerOwnerRefCount_.emplace(targetId, 1);
    return targetId;
  }

  uint32_t registerLoopbackTarget(kj::AsyncIoProvider& ioProvider) {
    return registerLoopbackTarget(
        ioProvider, capnp::Capability::Client(kj::heap<LoopbackCapabilityServer>()));
  }

  uint32_t registerLoopbackTarget(kj::AsyncIoProvider& ioProvider, uint32_t bootstrapTarget) {
    auto targetIt = targets_.find(bootstrapTarget);
    if (targetIt == targets_.end()) {
      throw std::runtime_error("unknown RPC target capability id: " +
                               std::to_string(bootstrapTarget));
    }
    return registerLoopbackTarget(ioProvider, targetIt->second);
  }

  void run() {
    lean_initialize_thread();
    lean_inc_heartbeat();
    try {
      auto io = kj::setupAsyncIo();
#if _WIN32
      eventPort_.store(&io.win32EventPort, std::memory_order_release);
#else
      eventPort_.store(&io.unixEventPort, std::memory_order_release);
#endif
      ioProvider_ = io.provider;
      waitScope_ = &io.waitScope;
      {
        std::lock_guard<std::mutex> lock(startupMutex_);
        startupComplete_ = true;
      }
      startupCv_.notify_all();

      class RuntimeTaskErrorHandler final : public kj::TaskSet::ErrorHandler {
       public:
        void taskFailed(kj::Exception&& exception) override {
          debugLog("runtime.task_failed", describeKjException(exception));
        }
      };

      RuntimeTaskErrorHandler taskErrorHandler;
      kj::TaskSet tasks(taskErrorHandler);

      auto scheduleRawCallCompletion =
          [&tasks](kj::Promise<RawCallResult>&& promise,
                   const std::shared_ptr<RawCallCompletion>& completion) {
            tasks.add(kj::mv(promise).then(
                [completion](RawCallResult&& result) mutable {
                  completeSuccess(completion, kj::mv(result));
                },
                [completion](kj::Exception&& e) mutable {
                  completeFailureKj(completion, e);
                }));
          };

      while (true) {
        QueuedOperation op;
        bool haveOp = false;
        {
          std::lock_guard<std::mutex> lock(queueMutex_);
          if (!queue_.empty()) {
            op = std::move(queue_.front());
            queue_.pop_front();
            haveOp = true;
          } else if (stopping_) {
            if (tasks.isEmpty()) {
              break;
            }
          }
        }

        if (!haveOp) {
          // No queued work. Pump KJ once (non-blocking) and then *block via KJ* for a short
          // duration.
          //
          // We intentionally avoid calling EventPort::wait() directly: on some platforms we've
          // observed that wake() does not reliably interrupt a naked EventPort wait, causing the
          // runtime queue to deadlock. Waiting on a short timer promise guarantees forward
          // progress, while still letting the event loop process async I/O immediately.
          if (io.waitScope.poll(1) > 0) {
            continue;
          }
          io.provider->getTimer().afterDelay(1 * kj::MILLISECONDS).wait(io.waitScope);
          continue;
        }

        if (std::holds_alternative<QueuedRawCall>(op)) {
          auto call = std::get<QueuedRawCall>(std::move(op));
          try {
            auto promise =
                processRawCall(call.target, call.interfaceId, call.methodId, call.request.data(),
                               call.request.size(),
                               call.requestCaps);
            scheduleRawCallCompletion(kj::mv(promise), call.completion);
          } catch (const kj::Exception& e) {
            completeFailureKj(call.completion, e);
          } catch (const std::exception& e) {
            completeFailure(call.completion, e.what());
          } catch (...) {
            completeFailure(call.completion, "unknown exception in capnp_lean_rpc_raw_call");
          }
        } else if (std::holds_alternative<QueuedRawCallData>(op)) {
          auto call = std::get<QueuedRawCallData>(std::move(op));
          try {
            auto promise = processRawCall(call.target, call.interfaceId, call.methodId,
                                          call.requestData, call.requestSize, call.requestCaps);
            scheduleRawCallCompletion(kj::mv(promise), call.completion);
          } catch (const kj::Exception& e) {
            completeFailureKj(call.completion, e);
          } catch (const std::exception& e) {
            completeFailure(call.completion, e.what());
          } catch (...) {
            completeFailure(call.completion, "unknown exception in capnp_lean_rpc_raw_call");
          }
        } else if (std::holds_alternative<QueuedStartPendingCall>(op)) {
          auto call = std::get<QueuedStartPendingCall>(std::move(op));
          try {
            auto pendingCallId =
                startPendingCall(call.target, call.interfaceId, call.methodId, call.request.data(),
                                 call.request.size(),
                                 call.requestCaps);
            completeRegisterSuccess(call.completion, pendingCallId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(call.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(call.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                call.completion,
                "unknown exception in capnp_lean_rpc_runtime_start_call_with_caps");
          }
        } else if (std::holds_alternative<QueuedStartPendingCallData>(op)) {
          auto call = std::get<QueuedStartPendingCallData>(std::move(op));
          try {
            auto pendingCallId =
                startPendingCall(call.target, call.interfaceId, call.methodId, call.requestData,
                                 call.requestSize, call.requestCaps);
            completeRegisterSuccess(call.completion, pendingCallId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(call.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(call.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                call.completion,
                "unknown exception in capnp_lean_rpc_runtime_start_call_with_caps");
          }
        } else if (std::holds_alternative<QueuedStartStreamingPendingCall>(op)) {
          auto call = std::get<QueuedStartStreamingPendingCall>(std::move(op));
          try {
            auto pendingCallId =
                startStreamingPendingCall(call.target, call.interfaceId, call.methodId,
                                          call.request.data(),
                                          call.request.size(),
                                          call.requestCaps);
            completeRegisterSuccess(call.completion, pendingCallId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(call.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(call.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                call.completion,
                "unknown exception in capnp_lean_rpc_runtime_start_streaming_call_with_caps");
          }
        } else if (std::holds_alternative<QueuedStartStreamingPendingCallData>(op)) {
          auto call = std::get<QueuedStartStreamingPendingCallData>(std::move(op));
          try {
            auto pendingCallId = startStreamingPendingCall(call.target, call.interfaceId,
                                                           call.methodId, call.requestData,
                                                           call.requestSize, call.requestCaps);
            completeRegisterSuccess(call.completion, pendingCallId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(call.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(call.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                call.completion,
                "unknown exception in capnp_lean_rpc_runtime_start_streaming_call_with_caps");
          }
        } else if (std::holds_alternative<QueuedAwaitPendingCall>(op)) {
          auto call = std::get<QueuedAwaitPendingCall>(std::move(op));
          try {
            auto promise = awaitPendingCall(call.pendingCallId);
            scheduleRawCallCompletion(kj::mv(promise), call.completion);
          } catch (const kj::Exception& e) {
            completeFailureKj(call.completion, e);
          } catch (const std::exception& e) {
            completeFailure(call.completion, e.what());
          } catch (...) {
            completeFailure(
                call.completion,
                "unknown exception in capnp_lean_rpc_runtime_pending_call_await");
          }
        } else if (std::holds_alternative<QueuedReleasePendingCall>(op)) {
          auto release = std::get<QueuedReleasePendingCall>(std::move(op));
          try {
            releasePendingCall(release.pendingCallId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                release.completion,
                "unknown exception in capnp_lean_rpc_runtime_pending_call_release");
          }
        } else if (std::holds_alternative<QueuedGetPipelinedCap>(op)) {
          auto call = std::get<QueuedGetPipelinedCap>(std::move(op));
          try {
            auto targetId = getPipelinedCap(call.pendingCallId, call.pointerPath);
            completeRegisterSuccess(call.completion, targetId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(call.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(call.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                call.completion,
                "unknown exception in capnp_lean_rpc_runtime_pending_call_get_pipelined_cap");
          }
        } else if (std::holds_alternative<QueuedStreamingCall>(op)) {
          auto call = std::get<QueuedStreamingCall>(std::move(op));
          try {
            processStreamingCall(call.target, call.interfaceId, call.methodId,
                                 call.request.data(),
                                 call.request.size(),
                                 call.requestCaps, io.waitScope);
            completeUnitSuccess(call.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(call.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(call.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                call.completion,
                "unknown exception in capnp_lean_rpc_runtime_streaming_call_with_caps");
          }
        } else if (std::holds_alternative<QueuedTargetGetFd>(op)) {
          auto call = std::get<QueuedTargetGetFd>(std::move(op));
          try {
            completeInt64Success(call.completion, targetGetFd(io.waitScope, call.target));
          } catch (const kj::Exception& e) {
            completeInt64Failure(call.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeInt64Failure(call.completion, e.what());
          } catch (...) {
            completeInt64Failure(call.completion,
                                 "unknown exception in capnp_lean_rpc_runtime_target_get_fd");
          }
        } else if (std::holds_alternative<QueuedTargetWhenResolved>(op)) {
          auto call = std::get<QueuedTargetWhenResolved>(std::move(op));
          try {
            targetWhenResolved(io.waitScope, call.target);
            completeUnitSuccess(call.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(call.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(call.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                call.completion,
                "unknown exception in capnp_lean_rpc_runtime_target_when_resolved");
          }
        } else if (std::holds_alternative<QueuedTargetWhenResolvedStart>(op)) {
          auto call = std::get<QueuedTargetWhenResolvedStart>(std::move(op));
          try {
            auto promiseId = targetWhenResolvedStart(call.target);
            completeRegisterSuccess(call.completion, promiseId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(call.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(call.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                call.completion,
                "unknown exception in capnp_lean_rpc_runtime_target_when_resolved_start");
          }
        } else if (std::holds_alternative<QueuedTargetWhenResolvedPoll>(op)) {
          auto call = std::get<QueuedTargetWhenResolvedPoll>(std::move(op));
          try {
            completeBoolSuccess(call.completion, targetWhenResolvedPoll(io.waitScope, call.target));
          } catch (const kj::Exception& e) {
            completeBoolFailure(call.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeBoolFailure(call.completion, e.what());
          } catch (...) {
            completeBoolFailure(
                call.completion,
                "unknown exception in capnp_lean_rpc_runtime_target_when_resolved_poll");
          }
        } else if (std::holds_alternative<QueuedEnableTraceEncoder>(op)) {
          auto enable = std::get<QueuedEnableTraceEncoder>(std::move(op));
          try {
            setTraceEncoderEnabled(true);
            completeUnitSuccess(enable.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(enable.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(enable.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                enable.completion,
                "unknown exception in capnp_lean_rpc_runtime_enable_trace_encoder");
          }
        } else if (std::holds_alternative<QueuedDisableTraceEncoder>(op)) {
          auto disable = std::get<QueuedDisableTraceEncoder>(std::move(op));
          try {
            setTraceEncoderEnabled(false);
            completeUnitSuccess(disable.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(disable.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(disable.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                disable.completion,
                "unknown exception in capnp_lean_rpc_runtime_disable_trace_encoder");
          }
        } else if (std::holds_alternative<QueuedSetTraceEncoder>(op)) {
          auto setTrace = std::get<QueuedSetTraceEncoder>(std::move(op));
          try {
            setTraceEncoderFromLean(setTrace.encoder);
            completeUnitSuccess(setTrace.completion);
          } catch (const kj::Exception& e) {
            lean_dec(setTrace.encoder);
            completeUnitFailure(setTrace.completion, describeKjException(e));
          } catch (const std::exception& e) {
            lean_dec(setTrace.encoder);
            completeUnitFailure(setTrace.completion, e.what());
          } catch (...) {
            lean_dec(setTrace.encoder);
            completeUnitFailure(
                setTrace.completion,
                "unknown exception in capnp_lean_rpc_runtime_set_trace_encoder");
          }
        } else if (std::holds_alternative<QueuedCppCallWithAccept>(op)) {
          auto call = std::get<QueuedCppCallWithAccept>(std::move(op));
          try {
            auto promise = kj::evalNow([&]() {
              return processCppCallWithAccept(
                  *io.provider, io.waitScope, call.serverId, call.listenerId, call.address,
                  call.portHint, call.interfaceId, call.methodId, call.request.data(),
                  call.request.size(), call.requestCaps);
            });
            completeSuccess(call.completion, promise.wait(io.waitScope));
          } catch (const kj::Exception& e) {
            completeFailureKj(call.completion, e);
          } catch (const std::exception& e) {
            completeFailure(call.completion, e.what());
          } catch (...) {
            completeFailure(call.completion,
                            "unknown exception in capnp_lean_rpc_runtime_cpp_call_with_accept");
          }
        } else if (std::holds_alternative<QueuedCppCallPipelinedWithAccept>(op)) {
          auto call = std::get<QueuedCppCallPipelinedWithAccept>(std::move(op));
          try {
            auto promise = kj::evalNow([&]() {
              return processCppPipelinedCallWithAccept(
                  *io.provider, io.waitScope, call.serverId, call.listenerId, call.address,
                  call.portHint, call.interfaceId, call.methodId, call.request.data(),
                  call.request.size(), call.requestCaps, call.pipelinedRequest.data(),
                  call.pipelinedRequest.size(), call.pipelinedRequestCaps);
            });
            completeSuccess(call.completion, promise.wait(io.waitScope));
          } catch (const kj::Exception& e) {
            completeFailureKj(call.completion, e);
          } catch (const std::exception& e) {
            completeFailure(call.completion, e.what());
          } catch (...) {
            completeFailure(call.completion,
                            "unknown exception in "
                            "capnp_lean_rpc_runtime_cpp_call_pipelined_with_accept");
          }
        } else if (std::holds_alternative<QueuedRegisterLoopbackTarget>(op)) {
          auto registration = std::get<QueuedRegisterLoopbackTarget>(std::move(op));
          try {
            completeRegisterSuccess(registration.completion, registerLoopbackTarget(*io.provider));
          } catch (const kj::Exception& e) {
            completeRegisterFailure(registration.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(registration.completion, e.what());
          } catch (...) {
            completeRegisterFailure(registration.completion,
                                    "unknown exception in capnp_lean_rpc_runtime_register_echo_target");
          }
        } else if (std::holds_alternative<QueuedRegisterLoopbackBootstrapTarget>(op)) {
          auto registration = std::get<QueuedRegisterLoopbackBootstrapTarget>(std::move(op));
          try {
            auto targetId = registerLoopbackTarget(*io.provider, registration.bootstrapTarget);
            completeRegisterSuccess(registration.completion, targetId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(registration.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(registration.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                registration.completion,
                "unknown exception in capnp_lean_rpc_runtime_register_loopback_target");
          }
        } else if (std::holds_alternative<QueuedRegisterHandlerTarget>(op)) {
          auto registration = std::get<QueuedRegisterHandlerTarget>(std::move(op));
          try {
            auto targetId = registerHandlerTarget(registration.handler);
            completeRegisterSuccess(registration.completion, targetId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(registration.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(registration.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                registration.completion,
                "unknown exception in capnp_lean_rpc_runtime_register_handler_target");
          }
          lean_dec(registration.handler);
        } else if (std::holds_alternative<QueuedRegisterAdvancedHandlerTarget>(op)) {
          auto registration = std::get<QueuedRegisterAdvancedHandlerTarget>(std::move(op));
          try {
            auto targetId = registerAdvancedHandlerTarget(registration.handler);
            completeRegisterSuccess(registration.completion, targetId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(registration.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(registration.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                registration.completion,
                "unknown exception in capnp_lean_rpc_runtime_register_advanced_handler_target");
          }
          lean_dec(registration.handler);
        } else if (std::holds_alternative<QueuedRegisterTailCallHandlerTarget>(op)) {
          auto registration = std::get<QueuedRegisterTailCallHandlerTarget>(std::move(op));
          try {
            auto targetId = registerTailCallHandlerTarget(registration.handler);
            completeRegisterSuccess(registration.completion, targetId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(registration.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(registration.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                registration.completion,
                "unknown exception in capnp_lean_rpc_runtime_register_tailcall_handler_target");
          }
          lean_dec(registration.handler);
        } else if (std::holds_alternative<QueuedRegisterTailCallTarget>(op)) {
          auto registration = std::get<QueuedRegisterTailCallTarget>(std::move(op));
          try {
            auto targetId = registerTailCallTarget(registration.target);
            completeRegisterSuccess(registration.completion, targetId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(registration.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(registration.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                registration.completion,
                "unknown exception in capnp_lean_rpc_runtime_register_tailcall_target");
          }
        } else if (std::holds_alternative<QueuedRegisterFdTarget>(op)) {
          auto registration = std::get<QueuedRegisterFdTarget>(std::move(op));
          try {
            auto targetId = registerFdTarget(registration.fd);
            completeRegisterSuccess(registration.completion, targetId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(registration.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(registration.completion, e.what());
          } catch (...) {
            completeRegisterFailure(registration.completion,
                                    "unknown exception in capnp_lean_rpc_runtime_register_fd_target");
          }
        } else if (std::holds_alternative<QueuedRegisterFdProbeTarget>(op)) {
          auto registration = std::get<QueuedRegisterFdProbeTarget>(std::move(op));
          try {
            auto targetId = registerFdProbeTarget();
            completeRegisterSuccess(registration.completion, targetId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(registration.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(registration.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                registration.completion,
                "unknown exception in capnp_lean_rpc_runtime_register_fd_probe_target");
          }
        } else if (std::holds_alternative<QueuedReleaseTarget>(op)) {
          auto release = std::get<QueuedReleaseTarget>(std::move(op));
          try {
            releaseTarget(release.target);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(release.completion,
                                "unknown exception in capnp_lean_rpc_runtime_release_target");
          }
        } else if (std::holds_alternative<QueuedReleaseTargets>(op)) {
          auto release = std::get<QueuedReleaseTargets>(std::move(op));
          try {
            releaseTargets(release.targets);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(release.completion,
                                "unknown exception in capnp_lean_rpc_runtime_release_targets");
          }
        } else if (std::holds_alternative<QueuedRetainTarget>(op)) {
          auto retain = std::get<QueuedRetainTarget>(std::move(op));
          try {
            auto retained = retainTarget(retain.target);
            completeRegisterSuccess(retain.completion, retained);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(retain.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(retain.completion, e.what());
          } catch (...) {
            completeRegisterFailure(retain.completion,
                                    "unknown exception in capnp_lean_rpc_runtime_retain_target");
          }
        } else if (std::holds_alternative<QueuedNewPromiseCapability>(op)) {
          auto request = std::get<QueuedNewPromiseCapability>(std::move(op));
          try {
            auto ids = newPromiseCapability();
            completeRegisterPairSuccess(request.completion, ids.first, ids.second);
          } catch (const kj::Exception& e) {
            completeRegisterPairFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterPairFailure(request.completion, e.what());
          } catch (...) {
            completeRegisterPairFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_new_promise_capability");
          }
        } else if (std::holds_alternative<QueuedPromiseCapabilityFulfill>(op)) {
          auto request = std::get<QueuedPromiseCapabilityFulfill>(std::move(op));
          try {
            promiseCapabilityFulfill(request.fulfillerId, request.target);
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_promise_capability_fulfill");
          }
        } else if (std::holds_alternative<QueuedPromiseCapabilityReject>(op)) {
          auto request = std::get<QueuedPromiseCapabilityReject>(std::move(op));
          try {
            promiseCapabilityReject(request.fulfillerId, request.exceptionTypeTag,
                                    request.message, request.detailBytes.data(),
                                    request.detailBytes.size());
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_promise_capability_reject");
          }
        } else if (std::holds_alternative<QueuedPromiseCapabilityRelease>(op)) {
          auto request = std::get<QueuedPromiseCapabilityRelease>(std::move(op));
          try {
            promiseCapabilityRelease(request.fulfillerId);
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_promise_capability_release");
          }
        } else if (std::holds_alternative<QueuedOrderingSetResolveHold>(op)) {
          auto request = std::get<QueuedOrderingSetResolveHold>(std::move(op));
          try {
            setResolveHoldEnabled(request.enabled);
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_ordering_set_resolve_hold");
          }
        } else if (std::holds_alternative<QueuedOrderingFlushHeldResolves>(op)) {
          auto request = std::get<QueuedOrderingFlushHeldResolves>(std::move(op));
          try {
            completeUInt64Success(request.completion, flushHeldPromiseCapabilityResolves());
          } catch (const kj::Exception& e) {
            completeUInt64Failure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt64Failure(request.completion, e.what());
          } catch (...) {
            completeUInt64Failure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_ordering_flush_resolves");
          }
        } else if (std::holds_alternative<QueuedOrderingHeldResolveCount>(op)) {
          auto request = std::get<QueuedOrderingHeldResolveCount>(std::move(op));
          try {
            completeUInt64Success(request.completion, heldResolveCount());
          } catch (const kj::Exception& e) {
            completeUInt64Failure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt64Failure(request.completion, e.what());
          } catch (...) {
            completeUInt64Failure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_ordering_held_resolve_count");
          }
        } else if (std::holds_alternative<QueuedConnectTarget>(op)) {
          auto connect = std::get<QueuedConnectTarget>(std::move(op));
          try {
            auto targetId =
                connectTarget(*io.provider, io.waitScope, connect.address, connect.portHint);
            completeRegisterSuccess(connect.completion, targetId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(connect.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(connect.completion, e.what());
          } catch (...) {
            completeRegisterFailure(connect.completion,
                                    "unknown exception in capnp_lean_rpc_runtime_connect");
          }
        } else if (std::holds_alternative<QueuedConnectTargetStart>(op)) {
          auto connect = std::get<QueuedConnectTargetStart>(std::move(op));
          try {
            auto promiseId = connectTargetStart(*io.provider, std::move(connect.address),
                                                connect.portHint);
            completeRegisterSuccess(connect.completion, promiseId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(connect.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(connect.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                connect.completion,
                "unknown exception in capnp_lean_rpc_runtime_connect_start");
          }
        } else if (std::holds_alternative<QueuedConnectTargetFd>(op)) {
          auto connect = std::get<QueuedConnectTargetFd>(std::move(op));
          try {
            auto targetId = connectTargetFd(*io.lowLevelProvider, connect.fd);
            completeRegisterSuccess(connect.completion, targetId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(connect.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(connect.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                connect.completion,
                "unknown exception in capnp_lean_rpc_runtime_connect_fd");
          }
        } else if (std::holds_alternative<QueuedNewTransportPipe>(op)) {
          auto pipe = std::get<QueuedNewTransportPipe>(std::move(op));
          try {
            auto ids = newTransportPipe(*io.provider);
            completeRegisterPairSuccess(pipe.completion, ids.first, ids.second);
          } catch (const kj::Exception& e) {
            completeRegisterPairFailure(pipe.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterPairFailure(pipe.completion, e.what());
          } catch (...) {
            completeRegisterPairFailure(pipe.completion,
                                        "unknown exception in capnp_lean_rpc_runtime_new_transport_pipe");
          }
        } else if (std::holds_alternative<QueuedNewTransportFromFd>(op)) {
          auto request = std::get<QueuedNewTransportFromFd>(std::move(op));
          try {
            auto transportId = newTransportFromFd(*io.lowLevelProvider, request.fd);
            completeRegisterSuccess(request.completion, transportId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(request.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_new_transport_from_fd");
          }
        } else if (std::holds_alternative<QueuedNewTransportFromFdTake>(op)) {
          auto request = std::get<QueuedNewTransportFromFdTake>(std::move(op));
          try {
            auto transportId = newTransportFromFdTake(*io.lowLevelProvider, request.fd);
            completeRegisterSuccess(request.completion, transportId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(request.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_new_transport_from_fd_take");
          }
        } else if (std::holds_alternative<QueuedReleaseTransport>(op)) {
          auto release = std::get<QueuedReleaseTransport>(std::move(op));
          try {
            releaseTransport(release.transportId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(release.completion,
                                "unknown exception in capnp_lean_rpc_runtime_release_transport");
          }
        } else if (std::holds_alternative<QueuedTransportGetFd>(op)) {
          auto request = std::get<QueuedTransportGetFd>(std::move(op));
          try {
            completeInt64Success(request.completion, transportGetFd(io.waitScope, request.transportId));
          } catch (const kj::Exception& e) {
            completeInt64Failure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeInt64Failure(request.completion, e.what());
          } catch (...) {
            completeInt64Failure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_transport_get_fd");
          }
        } else if (std::holds_alternative<QueuedConnectTargetTransport>(op)) {
          auto connect = std::get<QueuedConnectTargetTransport>(std::move(op));
          try {
            auto targetId = connectTargetTransport(connect.transportId);
            completeRegisterSuccess(connect.completion, targetId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(connect.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(connect.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                connect.completion,
                "unknown exception in capnp_lean_rpc_runtime_connect_transport");
          }
        } else if (std::holds_alternative<QueuedListenLoopback>(op)) {
          auto listen = std::get<QueuedListenLoopback>(std::move(op));
          try {
            auto listenerId = listenLoopback(*io.provider, io.waitScope, listen.address, listen.portHint);
            completeRegisterSuccess(listen.completion, listenerId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(listen.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(listen.completion, e.what());
          } catch (...) {
            completeRegisterFailure(listen.completion,
                                    "unknown exception in capnp_lean_rpc_runtime_listen_echo");
          }
        } else if (std::holds_alternative<QueuedAcceptLoopback>(op)) {
          auto accept = std::get<QueuedAcceptLoopback>(std::move(op));
          try {
            acceptLoopback(io.waitScope, accept.listenerId);
            completeUnitSuccess(accept.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(accept.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(accept.completion, e.what());
          } catch (...) {
            completeUnitFailure(accept.completion,
                                "unknown exception in capnp_lean_rpc_runtime_accept_echo");
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
                                "unknown exception in capnp_lean_rpc_runtime_release_listener");
          }
        } else if (std::holds_alternative<QueuedNewClient>(op)) {
          auto newClientReq = std::get<QueuedNewClient>(std::move(op));
          try {
            auto clientId =
                newClient(*io.provider, io.waitScope, newClientReq.address, newClientReq.portHint);
            completeRegisterSuccess(newClientReq.completion, clientId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(newClientReq.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(newClientReq.completion, e.what());
          } catch (...) {
            completeRegisterFailure(newClientReq.completion,
                                    "unknown exception in capnp_lean_rpc_runtime_new_client");
          }
        } else if (std::holds_alternative<QueuedNewClientStart>(op)) {
          auto newClientReq = std::get<QueuedNewClientStart>(std::move(op));
          try {
            auto promiseId =
                newClientStart(*io.provider, std::move(newClientReq.address), newClientReq.portHint);
            completeRegisterSuccess(newClientReq.completion, promiseId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(newClientReq.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(newClientReq.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                newClientReq.completion,
                "unknown exception in capnp_lean_rpc_runtime_new_client_start");
          }
        } else if (std::holds_alternative<QueuedReleaseClient>(op)) {
          auto release = std::get<QueuedReleaseClient>(std::move(op));
          try {
            releaseClient(release.clientId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(release.completion,
                                "unknown exception in capnp_lean_rpc_runtime_release_client");
          }
        } else if (std::holds_alternative<QueuedClientBootstrap>(op)) {
          auto bootstrap = std::get<QueuedClientBootstrap>(std::move(op));
          try {
            auto targetId = clientBootstrap(bootstrap.clientId);
            completeRegisterSuccess(bootstrap.completion, targetId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(bootstrap.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(bootstrap.completion, e.what());
          } catch (...) {
            completeRegisterFailure(bootstrap.completion,
                                    "unknown exception in capnp_lean_rpc_runtime_client_bootstrap");
          }
        } else if (std::holds_alternative<QueuedClientOnDisconnect>(op)) {
          auto disconnect = std::get<QueuedClientOnDisconnect>(std::move(op));
          try {
            clientOnDisconnect(io.waitScope, disconnect.clientId);
            completeUnitSuccess(disconnect.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(disconnect.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(disconnect.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                disconnect.completion,
                "unknown exception in capnp_lean_rpc_runtime_client_on_disconnect");
          }
        } else if (std::holds_alternative<QueuedClientOnDisconnectStart>(op)) {
          auto disconnect = std::get<QueuedClientOnDisconnectStart>(std::move(op));
          try {
            auto promiseId = clientOnDisconnectStart(disconnect.clientId);
            completeRegisterSuccess(disconnect.completion, promiseId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(disconnect.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(disconnect.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                disconnect.completion,
                "unknown exception in capnp_lean_rpc_runtime_client_on_disconnect_start");
          }
        } else if (std::holds_alternative<QueuedClientSetFlowLimit>(op)) {
          auto setLimit = std::get<QueuedClientSetFlowLimit>(std::move(op));
          try {
            clientSetFlowLimit(setLimit.clientId, setLimit.words);
            completeUnitSuccess(setLimit.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(setLimit.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(setLimit.completion, e.what());
          } catch (...) {
            completeUnitFailure(setLimit.completion,
                                "unknown exception in capnp_lean_rpc_runtime_client_set_flow_limit");
          }
        } else if (std::holds_alternative<QueuedClientQueueSize>(op)) {
          auto metric = std::get<QueuedClientQueueSize>(std::move(op));
          try {
            completeUInt64Success(metric.completion, clientQueueSize(metric.clientId));
          } catch (const kj::Exception& e) {
            completeUInt64Failure(metric.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt64Failure(metric.completion, e.what());
          } catch (...) {
            completeUInt64Failure(metric.completion,
                                  "unknown exception in capnp_lean_rpc_runtime_client_queue_size");
          }
        } else if (std::holds_alternative<QueuedClientQueueCount>(op)) {
          auto metric = std::get<QueuedClientQueueCount>(std::move(op));
          try {
            completeUInt64Success(metric.completion, clientQueueCount(metric.clientId));
          } catch (const kj::Exception& e) {
            completeUInt64Failure(metric.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt64Failure(metric.completion, e.what());
          } catch (...) {
            completeUInt64Failure(metric.completion,
                                  "unknown exception in capnp_lean_rpc_runtime_client_queue_count");
          }
        } else if (std::holds_alternative<QueuedClientOutgoingWaitNanos>(op)) {
          auto metric = std::get<QueuedClientOutgoingWaitNanos>(std::move(op));
          try {
            completeUInt64Success(metric.completion, clientOutgoingWaitNanos(metric.clientId));
          } catch (const kj::Exception& e) {
            completeUInt64Failure(metric.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt64Failure(metric.completion, e.what());
          } catch (...) {
            completeUInt64Failure(
                metric.completion,
                "unknown exception in capnp_lean_rpc_runtime_client_outgoing_wait_nanos");
          }
        } else if (std::holds_alternative<QueuedTargetCount>(op)) {
          auto metric = std::get<QueuedTargetCount>(std::move(op));
          try {
            completeUInt64Success(metric.completion, targetCount());
          } catch (const kj::Exception& e) {
            completeUInt64Failure(metric.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt64Failure(metric.completion, e.what());
          } catch (...) {
            completeUInt64Failure(metric.completion,
                                  "unknown exception in capnp_lean_rpc_runtime_target_count");
          }
        } else if (std::holds_alternative<QueuedListenerCount>(op)) {
          auto metric = std::get<QueuedListenerCount>(std::move(op));
          try {
            completeUInt64Success(metric.completion, listenerCount());
          } catch (const kj::Exception& e) {
            completeUInt64Failure(metric.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt64Failure(metric.completion, e.what());
          } catch (...) {
            completeUInt64Failure(metric.completion,
                                  "unknown exception in capnp_lean_rpc_runtime_listener_count");
          }
        } else if (std::holds_alternative<QueuedClientCount>(op)) {
          auto metric = std::get<QueuedClientCount>(std::move(op));
          try {
            completeUInt64Success(metric.completion, clientCount());
          } catch (const kj::Exception& e) {
            completeUInt64Failure(metric.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt64Failure(metric.completion, e.what());
          } catch (...) {
            completeUInt64Failure(metric.completion,
                                  "unknown exception in capnp_lean_rpc_runtime_client_count");
          }
        } else if (std::holds_alternative<QueuedServerCount>(op)) {
          auto metric = std::get<QueuedServerCount>(std::move(op));
          try {
            completeUInt64Success(metric.completion, serverCount());
          } catch (const kj::Exception& e) {
            completeUInt64Failure(metric.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt64Failure(metric.completion, e.what());
          } catch (...) {
            completeUInt64Failure(metric.completion,
                                  "unknown exception in capnp_lean_rpc_runtime_server_count");
          }
        } else if (std::holds_alternative<QueuedPendingCallCount>(op)) {
          auto metric = std::get<QueuedPendingCallCount>(std::move(op));
          try {
            completeUInt64Success(metric.completion, pendingCallCount());
          } catch (const kj::Exception& e) {
            completeUInt64Failure(metric.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt64Failure(metric.completion, e.what());
          } catch (...) {
            completeUInt64Failure(
                metric.completion,
                "unknown exception in capnp_lean_rpc_runtime_pending_call_count");
          }
        } else if (std::holds_alternative<QueuedNewServer>(op)) {
          auto newServerReq = std::get<QueuedNewServer>(std::move(op));
          try {
            auto serverId = newServer(newServerReq.bootstrapTarget);
            completeRegisterSuccess(newServerReq.completion, serverId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(newServerReq.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(newServerReq.completion, e.what());
          } catch (...) {
            completeRegisterFailure(newServerReq.completion,
                                    "unknown exception in capnp_lean_rpc_runtime_new_server");
          }
        } else if (std::holds_alternative<QueuedNewServerWithBootstrapFactory>(op)) {
          auto newServerReq = std::get<QueuedNewServerWithBootstrapFactory>(std::move(op));
          try {
            auto serverId = newServerWithBootstrapFactory(newServerReq.bootstrapFactory);
            completeRegisterSuccess(newServerReq.completion, serverId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(newServerReq.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(newServerReq.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                newServerReq.completion,
                "unknown exception in capnp_lean_rpc_runtime_new_server_with_bootstrap_factory");
          }
          lean_dec(newServerReq.bootstrapFactory);
        } else if (std::holds_alternative<QueuedReleaseServer>(op)) {
          auto release = std::get<QueuedReleaseServer>(std::move(op));
          try {
            releaseServer(release.serverId);
            completeUnitSuccess(release.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(release.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(release.completion, e.what());
          } catch (...) {
            completeUnitFailure(release.completion,
                                "unknown exception in capnp_lean_rpc_runtime_release_server");
          }
        } else if (std::holds_alternative<QueuedServerListen>(op)) {
          auto listen = std::get<QueuedServerListen>(std::move(op));
          try {
            auto listenerId = serverListen(*io.provider, io.waitScope, listen.serverId, listen.address,
                                           listen.portHint);
            completeRegisterSuccess(listen.completion, listenerId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(listen.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(listen.completion, e.what());
          } catch (...) {
            completeRegisterFailure(listen.completion,
                                    "unknown exception in capnp_lean_rpc_runtime_server_listen");
          }
        } else if (std::holds_alternative<QueuedServerAccept>(op)) {
          auto accept = std::get<QueuedServerAccept>(std::move(op));
          try {
            serverAccept(io.waitScope, accept.serverId, accept.listenerId);
            completeUnitSuccess(accept.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(accept.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(accept.completion, e.what());
          } catch (...) {
            completeUnitFailure(accept.completion,
                                "unknown exception in capnp_lean_rpc_runtime_server_accept");
          }
        } else if (std::holds_alternative<QueuedServerAcceptStart>(op)) {
          auto accept = std::get<QueuedServerAcceptStart>(std::move(op));
          try {
            auto promiseId = serverAcceptStart(accept.serverId, accept.listenerId);
            completeRegisterSuccess(accept.completion, promiseId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(accept.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(accept.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                accept.completion,
                "unknown exception in capnp_lean_rpc_runtime_server_accept_start");
          }
        } else if (std::holds_alternative<QueuedServerAcceptFd>(op)) {
          auto accept = std::get<QueuedServerAcceptFd>(std::move(op));
          try {
            serverAcceptFd(*io.lowLevelProvider, accept.serverId, accept.fd);
            completeUnitSuccess(accept.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(accept.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(accept.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                accept.completion,
                "unknown exception in capnp_lean_rpc_runtime_server_accept_fd");
          }
        } else if (std::holds_alternative<QueuedServerAcceptTransport>(op)) {
          auto accept = std::get<QueuedServerAcceptTransport>(std::move(op));
          try {
            serverAcceptTransport(accept.serverId, accept.transportId);
            completeUnitSuccess(accept.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(accept.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(accept.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                accept.completion,
                "unknown exception in capnp_lean_rpc_runtime_server_accept_transport");
          }
        } else if (std::holds_alternative<QueuedServerDrain>(op)) {
          auto drain = std::get<QueuedServerDrain>(std::move(op));
          try {
            serverDrain(io.waitScope, drain.serverId);
            completeUnitSuccess(drain.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(drain.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(drain.completion, e.what());
          } catch (...) {
            completeUnitFailure(drain.completion,
                                "unknown exception in capnp_lean_rpc_runtime_server_drain");
          }
        } else if (std::holds_alternative<QueuedServerDrainStart>(op)) {
          auto drain = std::get<QueuedServerDrainStart>(std::move(op));
          try {
            auto promiseId = serverDrainStart(drain.serverId);
            completeRegisterSuccess(drain.completion, promiseId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(drain.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(drain.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                drain.completion,
                "unknown exception in capnp_lean_rpc_runtime_server_drain_start");
          }
        } else if (std::holds_alternative<QueuedAwaitRegisterPromise>(op)) {
          auto promise = std::get<QueuedAwaitRegisterPromise>(std::move(op));
          try {
            auto id = awaitRegisterPromise(io.waitScope, promise.promiseId);
            completeRegisterSuccess(promise.completion, id);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(promise.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(promise.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                promise.completion,
                "unknown exception in capnp_lean_rpc_runtime_register_promise_await");
          }
        } else if (std::holds_alternative<QueuedCancelRegisterPromise>(op)) {
          auto promise = std::get<QueuedCancelRegisterPromise>(std::move(op));
          try {
            cancelRegisterPromise(promise.promiseId);
            completeUnitSuccess(promise.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(promise.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(promise.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                promise.completion,
                "unknown exception in capnp_lean_rpc_runtime_register_promise_cancel");
          }
        } else if (std::holds_alternative<QueuedReleaseRegisterPromise>(op)) {
          auto promise = std::get<QueuedReleaseRegisterPromise>(std::move(op));
          try {
            releaseRegisterPromise(promise.promiseId);
            completeUnitSuccess(promise.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(promise.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(promise.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                promise.completion,
                "unknown exception in capnp_lean_rpc_runtime_register_promise_release");
          }
        } else if (std::holds_alternative<QueuedAwaitUnitPromise>(op)) {
          auto promise = std::get<QueuedAwaitUnitPromise>(std::move(op));
          try {
            awaitUnitPromise(io.waitScope, promise.promiseId);
            completeUnitSuccess(promise.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(promise.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(promise.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                promise.completion,
                "unknown exception in capnp_lean_rpc_runtime_unit_promise_await");
          }
        } else if (std::holds_alternative<QueuedCancelUnitPromise>(op)) {
          auto promise = std::get<QueuedCancelUnitPromise>(std::move(op));
          try {
            cancelUnitPromise(promise.promiseId);
            completeUnitSuccess(promise.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(promise.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(promise.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                promise.completion,
                "unknown exception in capnp_lean_rpc_runtime_unit_promise_cancel");
          }
        } else if (std::holds_alternative<QueuedReleaseUnitPromise>(op)) {
          auto promise = std::get<QueuedReleaseUnitPromise>(std::move(op));
          try {
            releaseUnitPromise(promise.promiseId);
            completeUnitSuccess(promise.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(promise.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(promise.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                promise.completion,
                "unknown exception in capnp_lean_rpc_runtime_unit_promise_release");
          }
        } else if (std::holds_alternative<QueuedKjAsyncSleepNanosStart>(op)) {
          auto promise = std::get<QueuedKjAsyncSleepNanosStart>(std::move(op));
          try {
            auto promiseId = kjAsyncSleepNanosStart(*io.provider, promise.delayNanos);
            completeKjPromiseIdSuccess(promise.completion, promiseId);
          } catch (const kj::Exception& e) {
            completeKjPromiseIdFailure(promise.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeKjPromiseIdFailure(promise.completion, e.what());
          } catch (...) {
            completeKjPromiseIdFailure(
                promise.completion,
                "unknown exception in capnp_lean_kj_async_runtime_sleep_nanos_start");
          }
        } else if (std::holds_alternative<QueuedKjAsyncPromiseAwait>(op)) {
          auto promise = std::get<QueuedKjAsyncPromiseAwait>(std::move(op));
          try {
            awaitKjAsyncPromise(io.waitScope, promise.promiseId);
            completeUnitSuccess(promise.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(promise.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(promise.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                promise.completion,
                "unknown exception in capnp_lean_kj_async_runtime_promise_await");
          }
        } else if (std::holds_alternative<QueuedKjAsyncPromiseCancel>(op)) {
          auto promise = std::get<QueuedKjAsyncPromiseCancel>(std::move(op));
          try {
            cancelKjAsyncPromise(promise.promiseId);
            completeUnitSuccess(promise.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(promise.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(promise.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                promise.completion,
                "unknown exception in capnp_lean_kj_async_runtime_promise_cancel");
          }
        } else if (std::holds_alternative<QueuedKjAsyncPromiseRelease>(op)) {
          auto promise = std::get<QueuedKjAsyncPromiseRelease>(std::move(op));
          try {
            releaseKjAsyncPromise(promise.promiseId);
            completeUnitSuccess(promise.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(promise.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(promise.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                promise.completion,
                "unknown exception in capnp_lean_kj_async_runtime_promise_release");
          }
        } else if (std::holds_alternative<QueuedKjAsyncPromiseThenStart>(op)) {
          auto promise = std::get<QueuedKjAsyncPromiseThenStart>(std::move(op));
          try {
            auto promiseId =
                kjAsyncPromiseThenStart(promise.firstPromiseId, promise.secondPromiseId);
            completeKjPromiseIdSuccess(promise.completion, promiseId);
          } catch (const kj::Exception& e) {
            completeKjPromiseIdFailure(promise.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeKjPromiseIdFailure(promise.completion, e.what());
          } catch (...) {
            completeKjPromiseIdFailure(
                promise.completion,
                "unknown exception in capnp_lean_kj_async_runtime_promise_then_start");
          }
        } else if (std::holds_alternative<QueuedKjAsyncPromiseCatchStart>(op)) {
          auto promise = std::get<QueuedKjAsyncPromiseCatchStart>(std::move(op));
          try {
            auto promiseId =
                kjAsyncPromiseCatchStart(promise.promiseId, promise.fallbackPromiseId);
            completeKjPromiseIdSuccess(promise.completion, promiseId);
          } catch (const kj::Exception& e) {
            completeKjPromiseIdFailure(promise.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeKjPromiseIdFailure(promise.completion, e.what());
          } catch (...) {
            completeKjPromiseIdFailure(
                promise.completion,
                "unknown exception in capnp_lean_kj_async_runtime_promise_catch_start");
          }
        } else if (std::holds_alternative<QueuedKjAsyncPromiseAllStart>(op)) {
          auto promise = std::get<QueuedKjAsyncPromiseAllStart>(std::move(op));
          try {
            auto promiseId = kjAsyncPromiseAllStart(std::move(promise.promiseIds));
            completeKjPromiseIdSuccess(promise.completion, promiseId);
          } catch (const kj::Exception& e) {
            completeKjPromiseIdFailure(promise.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeKjPromiseIdFailure(promise.completion, e.what());
          } catch (...) {
            completeKjPromiseIdFailure(
                promise.completion,
                "unknown exception in capnp_lean_kj_async_runtime_promise_all_start");
          }
        } else if (std::holds_alternative<QueuedKjAsyncPromiseRaceStart>(op)) {
          auto promise = std::get<QueuedKjAsyncPromiseRaceStart>(std::move(op));
          try {
            auto promiseId = kjAsyncPromiseRaceStart(std::move(promise.promiseIds));
            completeKjPromiseIdSuccess(promise.completion, promiseId);
          } catch (const kj::Exception& e) {
            completeKjPromiseIdFailure(promise.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeKjPromiseIdFailure(promise.completion, e.what());
          } catch (...) {
            completeKjPromiseIdFailure(
                promise.completion,
                "unknown exception in capnp_lean_kj_async_runtime_promise_race_start");
          }
        } else if (std::holds_alternative<QueuedKjAsyncTaskSetNew>(op)) {
          auto request = std::get<QueuedKjAsyncTaskSetNew>(std::move(op));
          try {
            auto taskSetId = kjAsyncTaskSetNew();
            completeRegisterSuccess(request.completion, taskSetId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(request.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                request.completion,
                "unknown exception in capnp_lean_kj_async_runtime_task_set_new");
          }
        } else if (std::holds_alternative<QueuedKjAsyncTaskSetRelease>(op)) {
          auto request = std::get<QueuedKjAsyncTaskSetRelease>(std::move(op));
          try {
            kjAsyncTaskSetRelease(request.taskSetId);
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                request.completion,
                "unknown exception in capnp_lean_kj_async_runtime_task_set_release");
          }
        } else if (std::holds_alternative<QueuedKjAsyncTaskSetAddPromise>(op)) {
          auto request = std::get<QueuedKjAsyncTaskSetAddPromise>(std::move(op));
          try {
            kjAsyncTaskSetAddPromise(request.taskSetId, request.promiseId);
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                request.completion,
                "unknown exception in capnp_lean_kj_async_runtime_task_set_add_promise");
          }
        } else if (std::holds_alternative<QueuedKjAsyncTaskSetClear>(op)) {
          auto request = std::get<QueuedKjAsyncTaskSetClear>(std::move(op));
          try {
            kjAsyncTaskSetClear(request.taskSetId);
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                request.completion,
                "unknown exception in capnp_lean_kj_async_runtime_task_set_clear");
          }
        } else if (std::holds_alternative<QueuedKjAsyncTaskSetIsEmpty>(op)) {
          auto request = std::get<QueuedKjAsyncTaskSetIsEmpty>(std::move(op));
          try {
            completeBoolSuccess(request.completion, kjAsyncTaskSetIsEmpty(request.taskSetId));
          } catch (const kj::Exception& e) {
            completeBoolFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeBoolFailure(request.completion, e.what());
          } catch (...) {
            completeBoolFailure(
                request.completion,
                "unknown exception in capnp_lean_kj_async_runtime_task_set_is_empty");
          }
        } else if (std::holds_alternative<QueuedKjAsyncTaskSetOnEmptyStart>(op)) {
          auto request = std::get<QueuedKjAsyncTaskSetOnEmptyStart>(std::move(op));
          try {
            auto promiseId = kjAsyncTaskSetOnEmptyStart(request.taskSetId);
            completeKjPromiseIdSuccess(request.completion, promiseId);
          } catch (const kj::Exception& e) {
            completeKjPromiseIdFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeKjPromiseIdFailure(request.completion, e.what());
          } catch (...) {
            completeKjPromiseIdFailure(
                request.completion,
                "unknown exception in capnp_lean_kj_async_runtime_task_set_on_empty_start");
          }
        } else if (std::holds_alternative<QueuedKjAsyncTaskSetErrorCount>(op)) {
          auto request = std::get<QueuedKjAsyncTaskSetErrorCount>(std::move(op));
          try {
            completeUInt64Success(request.completion, kjAsyncTaskSetErrorCount(request.taskSetId));
          } catch (const kj::Exception& e) {
            completeUInt64Failure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt64Failure(request.completion, e.what());
          } catch (...) {
            completeUInt64Failure(
                request.completion,
                "unknown exception in capnp_lean_kj_async_runtime_task_set_error_count");
          }
        } else if (std::holds_alternative<QueuedKjAsyncTaskSetTakeLastError>(op)) {
          auto request = std::get<QueuedKjAsyncTaskSetTakeLastError>(std::move(op));
          try {
            completeOptionalStringSuccess(
                request.completion, kjAsyncTaskSetTakeLastError(request.taskSetId));
          } catch (const kj::Exception& e) {
            completeOptionalStringFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeOptionalStringFailure(request.completion, e.what());
          } catch (...) {
            completeOptionalStringFailure(
                request.completion,
                "unknown exception in capnp_lean_kj_async_runtime_task_set_take_last_error");
          }
        } else if (std::holds_alternative<QueuedMultiVatNewClient>(op)) {
          auto request = std::get<QueuedMultiVatNewClient>(std::move(op));
          try {
            auto peerId = newMultiVatClient(request.name);
            completeRegisterSuccess(request.completion, peerId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(request.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_new_client");
          }
        } else if (std::holds_alternative<QueuedMultiVatNewServer>(op)) {
          auto request = std::get<QueuedMultiVatNewServer>(std::move(op));
          try {
            auto peerId = newMultiVatServer(request.name, request.bootstrapTarget);
            completeRegisterSuccess(request.completion, peerId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(request.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_new_server");
          }
        } else if (std::holds_alternative<QueuedMultiVatNewServerWithBootstrapFactory>(op)) {
          auto request = std::get<QueuedMultiVatNewServerWithBootstrapFactory>(std::move(op));
          try {
            auto peerId = newMultiVatServerWithBootstrapFactory(request.name, request.bootstrapFactory);
            completeRegisterSuccess(request.completion, peerId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(request.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_new_server_with_bootstrap_factory");
          }
          lean_dec(request.bootstrapFactory);
        } else if (std::holds_alternative<QueuedMultiVatReleasePeer>(op)) {
          auto request = std::get<QueuedMultiVatReleasePeer>(std::move(op));
          try {
            releaseMultiVatPeer(request.peerId);
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_release_peer");
          }
        } else if (std::holds_alternative<QueuedMultiVatBootstrap>(op)) {
          auto request = std::get<QueuedMultiVatBootstrap>(std::move(op));
          try {
            auto target = multiVatBootstrap(request.sourcePeerId, LeanVatId{request.host, request.unique});
            completeRegisterSuccess(request.completion, target);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(request.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_bootstrap");
          }
        } else if (std::holds_alternative<QueuedMultiVatBootstrapPeer>(op)) {
          auto request = std::get<QueuedMultiVatBootstrapPeer>(std::move(op));
          try {
            auto target = multiVatBootstrapPeer(
                request.sourcePeerId, request.targetPeerId, request.unique);
            completeRegisterSuccess(request.completion, target);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(request.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_bootstrap_peer");
          }
        } else if (std::holds_alternative<QueuedMultiVatSetForwardingEnabled>(op)) {
          auto request = std::get<QueuedMultiVatSetForwardingEnabled>(std::move(op));
          try {
            multiVatSetForwardingEnabled(request.enabled);
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_set_forwarding_enabled");
          }
        } else if (std::holds_alternative<QueuedMultiVatResetForwardingStats>(op)) {
          auto request = std::get<QueuedMultiVatResetForwardingStats>(std::move(op));
          try {
            multiVatResetForwardingStats();
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_reset_forwarding_stats");
          }
        } else if (std::holds_alternative<QueuedMultiVatForwardCount>(op)) {
          auto request = std::get<QueuedMultiVatForwardCount>(std::move(op));
          try {
            completeUInt64Success(request.completion, multiVatForwardCount());
          } catch (const kj::Exception& e) {
            completeUInt64Failure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt64Failure(request.completion, e.what());
          } catch (...) {
            completeUInt64Failure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_forward_count");
          }
        } else if (std::holds_alternative<QueuedMultiVatThirdPartyTokenCount>(op)) {
          auto request = std::get<QueuedMultiVatThirdPartyTokenCount>(std::move(op));
          try {
            completeUInt64Success(request.completion, multiVatThirdPartyTokenCount());
          } catch (const kj::Exception& e) {
            completeUInt64Failure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt64Failure(request.completion, e.what());
          } catch (...) {
            completeUInt64Failure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_third_party_token_count");
          }
        } else if (std::holds_alternative<QueuedMultiVatDeniedForwardCount>(op)) {
          auto request = std::get<QueuedMultiVatDeniedForwardCount>(std::move(op));
          try {
            completeUInt64Success(request.completion, multiVatDeniedForwardCount());
          } catch (const kj::Exception& e) {
            completeUInt64Failure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt64Failure(request.completion, e.what());
          } catch (...) {
            completeUInt64Failure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_denied_forward_count");
          }
        } else if (std::holds_alternative<QueuedMultiVatHasConnection>(op)) {
          auto request = std::get<QueuedMultiVatHasConnection>(std::move(op));
          try {
            completeUInt64Success(
                request.completion,
                multiVatHasConnection(request.fromPeerId, request.toPeerId) ? 1 : 0);
          } catch (const kj::Exception& e) {
            completeUInt64Failure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt64Failure(request.completion, e.what());
          } catch (...) {
            completeUInt64Failure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_has_connection");
          }
        } else if (std::holds_alternative<QueuedMultiVatSetRestorer>(op)) {
          auto request = std::get<QueuedMultiVatSetRestorer>(std::move(op));
          try {
            multiVatSetRestorer(request.peerId, request.restorer);
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
            lean_dec(request.restorer);
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
            lean_dec(request.restorer);
          } catch (...) {
            completeUnitFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_set_restorer");
            lean_dec(request.restorer);
          }
        } else if (std::holds_alternative<QueuedMultiVatClearRestorer>(op)) {
          auto request = std::get<QueuedMultiVatClearRestorer>(std::move(op));
          try {
            multiVatClearRestorer(request.peerId);
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_clear_restorer");
          }
        } else if (std::holds_alternative<QueuedMultiVatPublishSturdyRef>(op)) {
          auto request = std::get<QueuedMultiVatPublishSturdyRef>(std::move(op));
          try {
            multiVatPublishSturdyRef(request.hostPeerId, request.objectId.data(),
                                     request.objectId.size(), request.targetId);
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_publish_sturdy_ref");
          }
        } else if (std::holds_alternative<QueuedMultiVatPublishSturdyRefStart>(op)) {
          auto request = std::get<QueuedMultiVatPublishSturdyRefStart>(std::move(op));
          try {
            auto promiseId = multiVatPublishSturdyRefStart(
                request.hostPeerId, std::move(request.objectId), request.targetId);
            completeRegisterSuccess(request.completion, promiseId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(request.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_publish_sturdy_ref_start");
          }
        } else if (std::holds_alternative<QueuedMultiVatUnpublishSturdyRef>(op)) {
          auto request = std::get<QueuedMultiVatUnpublishSturdyRef>(std::move(op));
          try {
            multiVatUnpublishSturdyRef(request.hostPeerId, request.objectId.data(),
                                       request.objectId.size());
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_unpublish_sturdy_ref");
          }
        } else if (std::holds_alternative<QueuedMultiVatUnpublishSturdyRefStart>(op)) {
          auto request = std::get<QueuedMultiVatUnpublishSturdyRefStart>(std::move(op));
          try {
            auto promiseId = multiVatUnpublishSturdyRefStart(
                request.hostPeerId, std::move(request.objectId));
            completeRegisterSuccess(request.completion, promiseId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(request.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_unpublish_sturdy_ref_start");
          }
        } else if (std::holds_alternative<QueuedMultiVatClearPublishedSturdyRefs>(op)) {
          auto request = std::get<QueuedMultiVatClearPublishedSturdyRefs>(std::move(op));
          try {
            multiVatClearPublishedSturdyRefs(request.hostPeerId);
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_clear_published_sturdy_refs");
          }
        } else if (std::holds_alternative<QueuedMultiVatClearPublishedSturdyRefsStart>(op)) {
          auto request = std::get<QueuedMultiVatClearPublishedSturdyRefsStart>(std::move(op));
          try {
            auto promiseId = multiVatClearPublishedSturdyRefsStart(request.hostPeerId);
            completeRegisterSuccess(request.completion, promiseId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(request.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_clear_published_sturdy_refs_start");
          }
        } else if (std::holds_alternative<QueuedMultiVatPublishedSturdyRefCount>(op)) {
          auto request = std::get<QueuedMultiVatPublishedSturdyRefCount>(std::move(op));
          try {
            auto count = multiVatPublishedSturdyRefCount(request.hostPeerId);
            completeUInt64Success(request.completion, count);
          } catch (const kj::Exception& e) {
            completeUInt64Failure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUInt64Failure(request.completion, e.what());
          } catch (...) {
            completeUInt64Failure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_published_sturdy_ref_count");
          }
        } else if (std::holds_alternative<QueuedMultiVatRestoreSturdyRef>(op)) {
          auto request = std::get<QueuedMultiVatRestoreSturdyRef>(std::move(op));
          try {
            auto target = multiVatRestoreSturdyRef(
                request.sourcePeerId, LeanVatId{request.host, request.unique},
                request.objectId.data(), request.objectId.size());
            completeRegisterSuccess(request.completion, target);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(request.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_restore_sturdy_ref");
          }
        } else if (std::holds_alternative<QueuedMultiVatRestoreSturdyRefStart>(op)) {
          auto request = std::get<QueuedMultiVatRestoreSturdyRefStart>(std::move(op));
          try {
            auto promiseId = multiVatRestoreSturdyRefStart(
                request.sourcePeerId, std::move(request.host), request.unique,
                std::move(request.objectId));
            completeRegisterSuccess(request.completion, promiseId);
          } catch (const kj::Exception& e) {
            completeRegisterFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeRegisterFailure(request.completion, e.what());
          } catch (...) {
            completeRegisterFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_restore_sturdy_ref_start");
          }
        } else if (std::holds_alternative<QueuedMultiVatGetDiagnostics>(op)) {
          auto request = std::get<QueuedMultiVatGetDiagnostics>(std::move(op));
          try {
            auto diag = multiVatGetDiagnostics(request.peerId, request.targetVatId);
            completeDiagnosticsSuccess(request.completion, diag);
            lean_dec(request.targetVatId);
          } catch (const kj::Exception& e) {
            completeDiagnosticsFailure(request.completion, describeKjException(e));
            lean_dec(request.targetVatId);
          } catch (const std::exception& e) {
            completeDiagnosticsFailure(request.completion, e.what());
            lean_dec(request.targetVatId);
          } catch (...) {
            completeDiagnosticsFailure(
                request.completion,
                "unknown exception in capnp_lean_rpc_runtime_multivat_get_diagnostics");
            lean_dec(request.targetVatId);
          }
        } else if (std::holds_alternative<QueuedMultiVatConnectionBlock>(op)) {
          auto request = std::get<QueuedMultiVatConnectionBlock>(std::move(op));
          try {
            multiVatConnectionBlock(request.fromPeerId, request.toPeerId);
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(request.completion,
                                "unknown exception in multiVatConnectionBlock");
          }
        } else if (std::holds_alternative<QueuedMultiVatConnectionUnblock>(op)) {
          auto request = std::get<QueuedMultiVatConnectionUnblock>(std::move(op));
          try {
            multiVatConnectionUnblock(request.fromPeerId, request.toPeerId);
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(request.completion,
                                "unknown exception in multiVatConnectionUnblock");
          }
        } else if (std::holds_alternative<QueuedMultiVatConnectionDisconnect>(op)) {
          auto request = std::get<QueuedMultiVatConnectionDisconnect>(std::move(op));
          try {
            multiVatConnectionDisconnect(request.fromPeerId, request.toPeerId,
                                         request.exceptionTypeTag, request.message,
                                         std::move(request.detailBytes));
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(request.completion,
                                "unknown exception in multiVatConnectionDisconnect");
          }
        } else if (std::holds_alternative<QueuedMultiVatConnectionResolveDisembargoCounts>(op)) {
          auto request = std::get<QueuedMultiVatConnectionResolveDisembargoCounts>(std::move(op));
          try {
            auto counts =
                multiVatConnectionResolveDisembargoCounts(request.fromPeerId, request.toPeerId);
            completeProtocolMessageCountsSuccess(request.completion, counts);
          } catch (const kj::Exception& e) {
            completeProtocolMessageCountsFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeProtocolMessageCountsFailure(request.completion, e.what());
          } catch (...) {
            completeProtocolMessageCountsFailure(
                request.completion,
                "unknown exception in multiVatConnectionResolveDisembargoCounts");
          }
        } else if (std::holds_alternative<QueuedMultiVatConnectionResolveDisembargoTrace>(op)) {
          auto request = std::get<QueuedMultiVatConnectionResolveDisembargoTrace>(std::move(op));
          try {
            auto trace =
                multiVatConnectionResolveDisembargoTrace(request.fromPeerId, request.toPeerId);
            completeByteArraySuccess(request.completion, std::move(trace));
          } catch (const kj::Exception& e) {
            completeByteArrayFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeByteArrayFailure(request.completion, e.what());
          } catch (...) {
            completeByteArrayFailure(
                request.completion,
                "unknown exception in multiVatConnectionResolveDisembargoTrace");
          }
        } else if (std::holds_alternative<QueuedMultiVatConnectionResetResolveDisembargoTrace>(op)) {
          auto request =
              std::get<QueuedMultiVatConnectionResetResolveDisembargoTrace>(std::move(op));
          try {
            multiVatConnectionResetResolveDisembargoTrace(request.fromPeerId, request.toPeerId);
            completeUnitSuccess(request.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(request.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(request.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                request.completion,
                "unknown exception in multiVatConnectionResetResolveDisembargoTrace");
          }
        } else if (std::holds_alternative<QueuedPump>(op)) {
          auto pump = std::get<QueuedPump>(std::move(op));
          try {
            io.waitScope.poll(1);
            completeUnitSuccess(pump.completion);
          } catch (const kj::Exception& e) {
            completeUnitFailure(pump.completion, describeKjException(e));
          } catch (const std::exception& e) {
            completeUnitFailure(pump.completion, e.what());
          } catch (...) {
            completeUnitFailure(
                pump.completion,
                "unknown exception in capnp_lean_rpc_runtime_pump");
          }
        } else if (std::holds_alternative<QueuedPumpAsync>(op)) {
          auto pump = std::get<QueuedPumpAsync>(std::move(op));
          try {
            io.waitScope.poll(1);
            lean_inc_heartbeat();
            completeAsyncUnitSuccess(pump.completion);
          } catch (const kj::Exception& e) {
            lean_inc_heartbeat();
            completeAsyncUnitFailure(pump.completion, describeKjException(e));
          } catch (const std::exception& e) {
            lean_inc_heartbeat();
            completeAsyncUnitFailure(pump.completion, e.what());
          } catch (...) {
            lean_inc_heartbeat();
            completeAsyncUnitFailure(
                pump.completion,
                "unknown exception in capnp_lean_rpc_runtime_pump_async");
          }
        }

        // Keep KJ progressing even if the runtime queue stays busy.
        io.waitScope.poll(1);
      }

      // Tear down RPC clients/servers on the runtime thread while async I/O is still valid.
      targets_.clear();
      listeners_.clear();
      loopbackPeers_.clear();
      networkClientPeers_.clear();
      transports_.clear();
      loopbackPeerOwnerByTarget_.clear();
      loopbackPeerOwnerRefCount_.clear();
      networkPeerOwnerByTarget_.clear();
      networkPeerOwnerRefCount_.clear();
      networkServerPeers_.clear();
      pendingCalls_.clear();
      activeCancelableDeferredTasks_.clear();
      registerPromises_.clear();
      unitPromises_.clear();
      kjAsyncPromises_.clear();
      retiredKjAsyncPromises_.clear();
      kjAsyncTaskSets_.clear();
      promiseCapabilityFulfillers_.clear();
      heldPromiseCapabilityResolves_.clear();
      heldPromiseCapabilityFulfillerIds_.clear();
      holdPromiseCapabilityResolves_ = false;
      clients_.clear();
      servers_.clear();
      sturdyRefs_.clear();
      multiVatPeerIdsByName_.clear();
      multiVatPeers_.clear();
      genericBootstrapFactories_.clear();
      clearTraceEncoderHandler();
      traceEncoderEnabled_ = false;
      ioProvider_ = nullptr;
      waitScope_ = nullptr;
      eventPort_.store(nullptr, std::memory_order_release);

      failPendingCalls("Capnp.Rpc runtime shut down");
    } catch (const kj::Exception& e) {
      ioProvider_ = nullptr;
      waitScope_ = nullptr;
      eventPort_.store(nullptr, std::memory_order_release);
      reportStartupFailure(describeKjException(e));
      failPendingCalls(describeKjException(e));
    } catch (const std::exception& e) {
      ioProvider_ = nullptr;
      waitScope_ = nullptr;
      eventPort_.store(nullptr, std::memory_order_release);
      reportStartupFailure(e.what());
      failPendingCalls(e.what());
    } catch (...) {
      ioProvider_ = nullptr;
      waitScope_ = nullptr;
      eventPort_.store(nullptr, std::memory_order_release);
      reportStartupFailure("unknown exception while starting Capnp.Rpc runtime");
      failPendingCalls("unknown exception while starting Capnp.Rpc runtime");
    }
  }

  void reportStartupFailure(std::string message) {
    {
      std::lock_guard<std::mutex> lock(startupMutex_);
      if (startupComplete_) {
        return;
      }
      startupError_ = std::move(message);
      startupComplete_ = true;
    }
    startupCv_.notify_all();
  }

  void failPendingCalls(const std::string& message) {
    std::deque<QueuedOperation> pending;
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      pending.swap(queue_);
    }
    for (auto& op : pending) {
      if (std::holds_alternative<QueuedRawCall>(op)) {
        completeFailure(std::get<QueuedRawCall>(op).completion, message);
      } else if (std::holds_alternative<QueuedRawCallData>(op)) {
        completeFailure(std::get<QueuedRawCallData>(op).completion, message);
      } else if (std::holds_alternative<QueuedStartPendingCall>(op)) {
        completeRegisterFailure(std::get<QueuedStartPendingCall>(op).completion, message);
      } else if (std::holds_alternative<QueuedStartPendingCallData>(op)) {
        completeRegisterFailure(std::get<QueuedStartPendingCallData>(op).completion, message);
      } else if (std::holds_alternative<QueuedStartStreamingPendingCall>(op)) {
        completeRegisterFailure(std::get<QueuedStartStreamingPendingCall>(op).completion,
                                message);
      } else if (std::holds_alternative<QueuedStartStreamingPendingCallData>(op)) {
        completeRegisterFailure(std::get<QueuedStartStreamingPendingCallData>(op).completion,
                                message);
      } else if (std::holds_alternative<QueuedAwaitPendingCall>(op)) {
        completeFailure(std::get<QueuedAwaitPendingCall>(op).completion, message);
      } else if (std::holds_alternative<QueuedReleasePendingCall>(op)) {
        completeUnitFailure(std::get<QueuedReleasePendingCall>(op).completion, message);
      } else if (std::holds_alternative<QueuedGetPipelinedCap>(op)) {
        completeRegisterFailure(std::get<QueuedGetPipelinedCap>(op).completion, message);
      } else if (std::holds_alternative<QueuedStreamingCall>(op)) {
        completeUnitFailure(std::get<QueuedStreamingCall>(op).completion, message);
      } else if (std::holds_alternative<QueuedTargetGetFd>(op)) {
        completeInt64Failure(std::get<QueuedTargetGetFd>(op).completion, message);
      } else if (std::holds_alternative<QueuedTargetWhenResolved>(op)) {
        completeUnitFailure(std::get<QueuedTargetWhenResolved>(op).completion, message);
      } else if (std::holds_alternative<QueuedTargetWhenResolvedStart>(op)) {
        completeRegisterFailure(std::get<QueuedTargetWhenResolvedStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedTargetWhenResolvedPoll>(op)) {
        completeBoolFailure(std::get<QueuedTargetWhenResolvedPoll>(op).completion, message);
      } else if (std::holds_alternative<QueuedEnableTraceEncoder>(op)) {
        completeUnitFailure(std::get<QueuedEnableTraceEncoder>(op).completion, message);
      } else if (std::holds_alternative<QueuedDisableTraceEncoder>(op)) {
        completeUnitFailure(std::get<QueuedDisableTraceEncoder>(op).completion, message);
      } else if (std::holds_alternative<QueuedSetTraceEncoder>(op)) {
        auto& setTrace = std::get<QueuedSetTraceEncoder>(op);
        lean_dec(setTrace.encoder);
        completeUnitFailure(setTrace.completion, message);
      } else if (std::holds_alternative<QueuedCppCallWithAccept>(op)) {
        completeFailure(std::get<QueuedCppCallWithAccept>(op).completion, message);
      } else if (std::holds_alternative<QueuedCppCallPipelinedWithAccept>(op)) {
        completeFailure(std::get<QueuedCppCallPipelinedWithAccept>(op).completion, message);
      } else if (std::holds_alternative<QueuedRegisterLoopbackTarget>(op)) {
        completeRegisterFailure(std::get<QueuedRegisterLoopbackTarget>(op).completion, message);
      } else if (std::holds_alternative<QueuedRegisterLoopbackBootstrapTarget>(op)) {
        completeRegisterFailure(
            std::get<QueuedRegisterLoopbackBootstrapTarget>(op).completion, message);
      } else if (std::holds_alternative<QueuedRegisterHandlerTarget>(op)) {
        auto& registration = std::get<QueuedRegisterHandlerTarget>(op);
        lean_dec(registration.handler);
        completeRegisterFailure(registration.completion, message);
      } else if (std::holds_alternative<QueuedRegisterAdvancedHandlerTarget>(op)) {
        auto& registration = std::get<QueuedRegisterAdvancedHandlerTarget>(op);
        lean_dec(registration.handler);
        completeRegisterFailure(registration.completion, message);
      } else if (std::holds_alternative<QueuedRegisterTailCallHandlerTarget>(op)) {
        auto& registration = std::get<QueuedRegisterTailCallHandlerTarget>(op);
        lean_dec(registration.handler);
        completeRegisterFailure(registration.completion, message);
      } else if (std::holds_alternative<QueuedRegisterTailCallTarget>(op)) {
        completeRegisterFailure(std::get<QueuedRegisterTailCallTarget>(op).completion, message);
      } else if (std::holds_alternative<QueuedRegisterFdTarget>(op)) {
        completeRegisterFailure(std::get<QueuedRegisterFdTarget>(op).completion, message);
      } else if (std::holds_alternative<QueuedRegisterFdProbeTarget>(op)) {
        completeRegisterFailure(std::get<QueuedRegisterFdProbeTarget>(op).completion, message);
      } else if (std::holds_alternative<QueuedReleaseTarget>(op)) {
        completeUnitFailure(std::get<QueuedReleaseTarget>(op).completion, message);
      } else if (std::holds_alternative<QueuedReleaseTargets>(op)) {
        completeUnitFailure(std::get<QueuedReleaseTargets>(op).completion, message);
      } else if (std::holds_alternative<QueuedRetainTarget>(op)) {
        completeRegisterFailure(std::get<QueuedRetainTarget>(op).completion, message);
      } else if (std::holds_alternative<QueuedNewPromiseCapability>(op)) {
        completeRegisterPairFailure(std::get<QueuedNewPromiseCapability>(op).completion, message);
      } else if (std::holds_alternative<QueuedPromiseCapabilityFulfill>(op)) {
        completeUnitFailure(std::get<QueuedPromiseCapabilityFulfill>(op).completion, message);
      } else if (std::holds_alternative<QueuedPromiseCapabilityReject>(op)) {
        completeUnitFailure(std::get<QueuedPromiseCapabilityReject>(op).completion, message);
      } else if (std::holds_alternative<QueuedPromiseCapabilityRelease>(op)) {
        completeUnitFailure(std::get<QueuedPromiseCapabilityRelease>(op).completion, message);
      } else if (std::holds_alternative<QueuedOrderingSetResolveHold>(op)) {
        completeUnitFailure(std::get<QueuedOrderingSetResolveHold>(op).completion, message);
      } else if (std::holds_alternative<QueuedOrderingFlushHeldResolves>(op)) {
        completeUInt64Failure(std::get<QueuedOrderingFlushHeldResolves>(op).completion, message);
      } else if (std::holds_alternative<QueuedOrderingHeldResolveCount>(op)) {
        completeUInt64Failure(std::get<QueuedOrderingHeldResolveCount>(op).completion, message);
      } else if (std::holds_alternative<QueuedConnectTarget>(op)) {
        completeRegisterFailure(std::get<QueuedConnectTarget>(op).completion, message);
      } else if (std::holds_alternative<QueuedConnectTargetStart>(op)) {
        completeRegisterFailure(std::get<QueuedConnectTargetStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedConnectTargetFd>(op)) {
        completeRegisterFailure(std::get<QueuedConnectTargetFd>(op).completion, message);
      } else if (std::holds_alternative<QueuedNewTransportPipe>(op)) {
        completeRegisterPairFailure(std::get<QueuedNewTransportPipe>(op).completion, message);
      } else if (std::holds_alternative<QueuedNewTransportFromFd>(op)) {
        completeRegisterFailure(std::get<QueuedNewTransportFromFd>(op).completion, message);
      } else if (std::holds_alternative<QueuedNewTransportFromFdTake>(op)) {
        completeRegisterFailure(std::get<QueuedNewTransportFromFdTake>(op).completion, message);
      } else if (std::holds_alternative<QueuedReleaseTransport>(op)) {
        completeUnitFailure(std::get<QueuedReleaseTransport>(op).completion, message);
      } else if (std::holds_alternative<QueuedTransportGetFd>(op)) {
        completeInt64Failure(std::get<QueuedTransportGetFd>(op).completion, message);
      } else if (std::holds_alternative<QueuedConnectTargetTransport>(op)) {
        completeRegisterFailure(std::get<QueuedConnectTargetTransport>(op).completion, message);
      } else if (std::holds_alternative<QueuedListenLoopback>(op)) {
        completeRegisterFailure(std::get<QueuedListenLoopback>(op).completion, message);
      } else if (std::holds_alternative<QueuedAcceptLoopback>(op)) {
        completeUnitFailure(std::get<QueuedAcceptLoopback>(op).completion, message);
      } else if (std::holds_alternative<QueuedReleaseListener>(op)) {
        completeUnitFailure(std::get<QueuedReleaseListener>(op).completion, message);
      } else if (std::holds_alternative<QueuedNewClient>(op)) {
        completeRegisterFailure(std::get<QueuedNewClient>(op).completion, message);
      } else if (std::holds_alternative<QueuedNewClientStart>(op)) {
        completeRegisterFailure(std::get<QueuedNewClientStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedReleaseClient>(op)) {
        completeUnitFailure(std::get<QueuedReleaseClient>(op).completion, message);
      } else if (std::holds_alternative<QueuedClientBootstrap>(op)) {
        completeRegisterFailure(std::get<QueuedClientBootstrap>(op).completion, message);
      } else if (std::holds_alternative<QueuedClientOnDisconnect>(op)) {
        completeUnitFailure(std::get<QueuedClientOnDisconnect>(op).completion, message);
      } else if (std::holds_alternative<QueuedClientOnDisconnectStart>(op)) {
        completeRegisterFailure(std::get<QueuedClientOnDisconnectStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedClientSetFlowLimit>(op)) {
        completeUnitFailure(std::get<QueuedClientSetFlowLimit>(op).completion, message);
      } else if (std::holds_alternative<QueuedNewServer>(op)) {
        completeRegisterFailure(std::get<QueuedNewServer>(op).completion, message);
      } else if (std::holds_alternative<QueuedNewServerWithBootstrapFactory>(op)) {
        auto& newServer = std::get<QueuedNewServerWithBootstrapFactory>(op);
        lean_dec(newServer.bootstrapFactory);
        completeRegisterFailure(newServer.completion, message);
      } else if (std::holds_alternative<QueuedReleaseServer>(op)) {
        completeUnitFailure(std::get<QueuedReleaseServer>(op).completion, message);
      } else if (std::holds_alternative<QueuedServerListen>(op)) {
        completeRegisterFailure(std::get<QueuedServerListen>(op).completion, message);
      } else if (std::holds_alternative<QueuedServerAccept>(op)) {
        completeUnitFailure(std::get<QueuedServerAccept>(op).completion, message);
      } else if (std::holds_alternative<QueuedServerAcceptStart>(op)) {
        completeRegisterFailure(std::get<QueuedServerAcceptStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedServerAcceptFd>(op)) {
        completeUnitFailure(std::get<QueuedServerAcceptFd>(op).completion, message);
      } else if (std::holds_alternative<QueuedServerAcceptTransport>(op)) {
        completeUnitFailure(std::get<QueuedServerAcceptTransport>(op).completion, message);
      } else if (std::holds_alternative<QueuedServerDrain>(op)) {
        completeUnitFailure(std::get<QueuedServerDrain>(op).completion, message);
      } else if (std::holds_alternative<QueuedServerDrainStart>(op)) {
        completeRegisterFailure(std::get<QueuedServerDrainStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedClientQueueSize>(op)) {
        completeUInt64Failure(std::get<QueuedClientQueueSize>(op).completion, message);
      } else if (std::holds_alternative<QueuedClientQueueCount>(op)) {
        completeUInt64Failure(std::get<QueuedClientQueueCount>(op).completion, message);
      } else if (std::holds_alternative<QueuedClientOutgoingWaitNanos>(op)) {
        completeUInt64Failure(std::get<QueuedClientOutgoingWaitNanos>(op).completion, message);
      } else if (std::holds_alternative<QueuedTargetCount>(op)) {
        completeUInt64Failure(std::get<QueuedTargetCount>(op).completion, message);
      } else if (std::holds_alternative<QueuedListenerCount>(op)) {
        completeUInt64Failure(std::get<QueuedListenerCount>(op).completion, message);
      } else if (std::holds_alternative<QueuedClientCount>(op)) {
        completeUInt64Failure(std::get<QueuedClientCount>(op).completion, message);
      } else if (std::holds_alternative<QueuedServerCount>(op)) {
        completeUInt64Failure(std::get<QueuedServerCount>(op).completion, message);
      } else if (std::holds_alternative<QueuedPendingCallCount>(op)) {
        completeUInt64Failure(std::get<QueuedPendingCallCount>(op).completion, message);
      } else if (std::holds_alternative<QueuedAwaitRegisterPromise>(op)) {
        completeRegisterFailure(std::get<QueuedAwaitRegisterPromise>(op).completion, message);
      } else if (std::holds_alternative<QueuedCancelRegisterPromise>(op)) {
        completeUnitFailure(std::get<QueuedCancelRegisterPromise>(op).completion, message);
      } else if (std::holds_alternative<QueuedReleaseRegisterPromise>(op)) {
        completeUnitFailure(std::get<QueuedReleaseRegisterPromise>(op).completion, message);
      } else if (std::holds_alternative<QueuedAwaitUnitPromise>(op)) {
        completeUnitFailure(std::get<QueuedAwaitUnitPromise>(op).completion, message);
      } else if (std::holds_alternative<QueuedCancelUnitPromise>(op)) {
        completeUnitFailure(std::get<QueuedCancelUnitPromise>(op).completion, message);
      } else if (std::holds_alternative<QueuedReleaseUnitPromise>(op)) {
        completeUnitFailure(std::get<QueuedReleaseUnitPromise>(op).completion, message);
      } else if (std::holds_alternative<QueuedKjAsyncSleepNanosStart>(op)) {
        completeKjPromiseIdFailure(std::get<QueuedKjAsyncSleepNanosStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedKjAsyncPromiseAwait>(op)) {
        completeUnitFailure(std::get<QueuedKjAsyncPromiseAwait>(op).completion, message);
      } else if (std::holds_alternative<QueuedKjAsyncPromiseCancel>(op)) {
        completeUnitFailure(std::get<QueuedKjAsyncPromiseCancel>(op).completion, message);
      } else if (std::holds_alternative<QueuedKjAsyncPromiseRelease>(op)) {
        completeUnitFailure(std::get<QueuedKjAsyncPromiseRelease>(op).completion, message);
      } else if (std::holds_alternative<QueuedKjAsyncPromiseThenStart>(op)) {
        completeKjPromiseIdFailure(std::get<QueuedKjAsyncPromiseThenStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedKjAsyncPromiseCatchStart>(op)) {
        completeKjPromiseIdFailure(std::get<QueuedKjAsyncPromiseCatchStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedKjAsyncPromiseAllStart>(op)) {
        completeKjPromiseIdFailure(std::get<QueuedKjAsyncPromiseAllStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedKjAsyncPromiseRaceStart>(op)) {
        completeKjPromiseIdFailure(std::get<QueuedKjAsyncPromiseRaceStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedKjAsyncTaskSetNew>(op)) {
        completeRegisterFailure(std::get<QueuedKjAsyncTaskSetNew>(op).completion, message);
      } else if (std::holds_alternative<QueuedKjAsyncTaskSetRelease>(op)) {
        completeUnitFailure(std::get<QueuedKjAsyncTaskSetRelease>(op).completion, message);
      } else if (std::holds_alternative<QueuedKjAsyncTaskSetAddPromise>(op)) {
        completeUnitFailure(std::get<QueuedKjAsyncTaskSetAddPromise>(op).completion, message);
      } else if (std::holds_alternative<QueuedKjAsyncTaskSetClear>(op)) {
        completeUnitFailure(std::get<QueuedKjAsyncTaskSetClear>(op).completion, message);
      } else if (std::holds_alternative<QueuedKjAsyncTaskSetIsEmpty>(op)) {
        completeBoolFailure(std::get<QueuedKjAsyncTaskSetIsEmpty>(op).completion, message);
      } else if (std::holds_alternative<QueuedKjAsyncTaskSetOnEmptyStart>(op)) {
        completeKjPromiseIdFailure(std::get<QueuedKjAsyncTaskSetOnEmptyStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedKjAsyncTaskSetErrorCount>(op)) {
        completeUInt64Failure(std::get<QueuedKjAsyncTaskSetErrorCount>(op).completion, message);
      } else if (std::holds_alternative<QueuedKjAsyncTaskSetTakeLastError>(op)) {
        completeOptionalStringFailure(
            std::get<QueuedKjAsyncTaskSetTakeLastError>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatNewClient>(op)) {
        completeRegisterFailure(std::get<QueuedMultiVatNewClient>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatNewServer>(op)) {
        completeRegisterFailure(std::get<QueuedMultiVatNewServer>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatNewServerWithBootstrapFactory>(op)) {
        auto& request = std::get<QueuedMultiVatNewServerWithBootstrapFactory>(op);
        lean_dec(request.bootstrapFactory);
        completeRegisterFailure(request.completion, message);
      } else if (std::holds_alternative<QueuedMultiVatReleasePeer>(op)) {
        completeUnitFailure(std::get<QueuedMultiVatReleasePeer>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatBootstrap>(op)) {
        completeRegisterFailure(std::get<QueuedMultiVatBootstrap>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatBootstrapPeer>(op)) {
        completeRegisterFailure(std::get<QueuedMultiVatBootstrapPeer>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatSetForwardingEnabled>(op)) {
        completeUnitFailure(std::get<QueuedMultiVatSetForwardingEnabled>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatResetForwardingStats>(op)) {
        completeUnitFailure(std::get<QueuedMultiVatResetForwardingStats>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatForwardCount>(op)) {
        completeUInt64Failure(std::get<QueuedMultiVatForwardCount>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatThirdPartyTokenCount>(op)) {
        completeUInt64Failure(std::get<QueuedMultiVatThirdPartyTokenCount>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatDeniedForwardCount>(op)) {
        completeUInt64Failure(std::get<QueuedMultiVatDeniedForwardCount>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatHasConnection>(op)) {
        completeUInt64Failure(std::get<QueuedMultiVatHasConnection>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatSetRestorer>(op)) {
        auto& request = std::get<QueuedMultiVatSetRestorer>(op);
        lean_dec(request.restorer);
        completeUnitFailure(request.completion, message);
      } else if (std::holds_alternative<QueuedMultiVatClearRestorer>(op)) {
        completeUnitFailure(std::get<QueuedMultiVatClearRestorer>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatPublishSturdyRef>(op)) {
        completeUnitFailure(std::get<QueuedMultiVatPublishSturdyRef>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatPublishSturdyRefStart>(op)) {
        completeRegisterFailure(std::get<QueuedMultiVatPublishSturdyRefStart>(op).completion,
                                message);
      } else if (std::holds_alternative<QueuedMultiVatUnpublishSturdyRef>(op)) {
        completeUnitFailure(std::get<QueuedMultiVatUnpublishSturdyRef>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatUnpublishSturdyRefStart>(op)) {
        completeRegisterFailure(std::get<QueuedMultiVatUnpublishSturdyRefStart>(op).completion,
                                message);
      } else if (std::holds_alternative<QueuedMultiVatClearPublishedSturdyRefs>(op)) {
        completeUnitFailure(std::get<QueuedMultiVatClearPublishedSturdyRefs>(op).completion,
                            message);
      } else if (std::holds_alternative<QueuedMultiVatClearPublishedSturdyRefsStart>(op)) {
        completeRegisterFailure(
            std::get<QueuedMultiVatClearPublishedSturdyRefsStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatPublishedSturdyRefCount>(op)) {
        completeUInt64Failure(std::get<QueuedMultiVatPublishedSturdyRefCount>(op).completion,
                              message);
      } else if (std::holds_alternative<QueuedMultiVatRestoreSturdyRef>(op)) {
        completeRegisterFailure(std::get<QueuedMultiVatRestoreSturdyRef>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatRestoreSturdyRefStart>(op)) {
        completeRegisterFailure(
            std::get<QueuedMultiVatRestoreSturdyRefStart>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatGetDiagnostics>(op)) {
        auto& request = std::get<QueuedMultiVatGetDiagnostics>(op);
        lean_dec(request.targetVatId);
        completeDiagnosticsFailure(request.completion, message);
      } else if (std::holds_alternative<QueuedMultiVatConnectionBlock>(op)) {
        completeUnitFailure(std::get<QueuedMultiVatConnectionBlock>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatConnectionUnblock>(op)) {
        completeUnitFailure(std::get<QueuedMultiVatConnectionUnblock>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatConnectionDisconnect>(op)) {
        completeUnitFailure(std::get<QueuedMultiVatConnectionDisconnect>(op).completion, message);
      } else if (std::holds_alternative<QueuedMultiVatConnectionResolveDisembargoCounts>(op)) {
        completeProtocolMessageCountsFailure(
            std::get<QueuedMultiVatConnectionResolveDisembargoCounts>(op).completion, message);
      } else if (std::holds_alternative<QueuedPump>(op)) {
        completeUnitFailure(std::get<QueuedPump>(op).completion, message);
      } else if (std::holds_alternative<QueuedPumpAsync>(op)) {
        completeAsyncUnitFailure(std::get<QueuedPumpAsync>(op).completion, message);
      } else {
        KJ_FAIL_ASSERT("unhandled runtime queued operation in failPendingCalls");
      }
    }
  }

  uint maxFdsPerMessage_;
  std::thread worker_;
  WakeUpManager wakeUp_;

  std::mutex startupMutex_;
  std::condition_variable startupCv_;
  bool startupComplete_ = false;
  std::string startupError_;

  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::atomic<kj::EventPort*> eventPort_{nullptr};
  kj::AsyncIoProvider* ioProvider_ = nullptr;
  kj::WaitScope* waitScope_ = nullptr;
  bool stopping_ = false;
  std::deque<QueuedOperation> queue_;

  std::unordered_map<uint32_t, capnp::Capability::Client> targets_;
  std::unordered_map<uint32_t, kj::Own<kj::ConnectionReceiver>> listeners_;
  std::unordered_map<uint32_t, kj::Own<LoopbackPeer>> loopbackPeers_;
  std::unordered_map<uint32_t, kj::Own<NetworkClientPeer>> networkClientPeers_;
  std::unordered_map<uint32_t, kj::Own<kj::AsyncCapabilityStream>> transports_;
  std::unordered_map<uint32_t, uint32_t> loopbackPeerOwnerByTarget_;
  std::unordered_map<uint32_t, uint32_t> loopbackPeerOwnerRefCount_;
  std::unordered_map<uint32_t, uint32_t> networkPeerOwnerByTarget_;
  std::unordered_map<uint32_t, uint32_t> networkPeerOwnerRefCount_;
  kj::Vector<kj::Own<NetworkServerPeer>> networkServerPeers_;
  std::unordered_map<uint32_t, PendingCall> pendingCalls_;
  std::deque<std::weak_ptr<DeferredLeanTaskState>> activeCancelableDeferredTasks_;
  std::unordered_map<uint32_t, PendingRegisterPromise> registerPromises_;
  std::unordered_map<uint32_t, PendingUnitPromise> unitPromises_;
  std::unordered_map<uint32_t, PendingUnitPromise> kjAsyncPromises_;
  std::unordered_set<uint32_t> retiredKjAsyncPromises_;
  std::unordered_map<uint32_t, kj::Own<RuntimeKjAsyncTaskSet>> kjAsyncTaskSets_;
  std::unordered_map<uint32_t, PendingPromiseCapability> promiseCapabilityFulfillers_;
  std::deque<HeldPromiseCapabilityResolve> heldPromiseCapabilityResolves_;
  std::unordered_set<uint32_t> heldPromiseCapabilityFulfillerIds_;
  bool holdPromiseCapabilityResolves_ = false;
  std::unordered_map<uint32_t, kj::Own<NetworkClientPeer>> clients_;
  std::unordered_map<uint32_t, kj::Own<RuntimeServer>> servers_;
  GenericVatNetwork genericVatNetwork_;
  std::unordered_map<uint32_t, kj::Own<MultiVatPeer>> multiVatPeers_;
  std::unordered_map<std::string, uint32_t> multiVatPeerIdsByName_;
  std::unordered_map<uint32_t, std::unordered_map<std::string, capnp::Capability::Client>>
      sturdyRefs_;
  kj::Vector<kj::Own<LeanGenericBootstrapFactory>> genericBootstrapFactories_;
  bool traceEncoderEnabled_ = false;
  lean_object* traceEncoderHandler_ = nullptr;
  uint32_t nextTargetId_ = 1;
  uint32_t nextListenerId_ = 1;
  uint32_t nextTransportId_ = 1;
  uint32_t nextClientId_ = 1;
  uint32_t nextServerId_ = 1;
  uint32_t nextMultiVatPeerId_ = 1;
  uint32_t nextPendingCallId_ = 1;
  uint32_t nextRegisterPromiseId_ = 1;
  uint32_t nextUnitPromiseId_ = 1;
  uint32_t nextKjAsyncPromiseId_ = 1;
  uint32_t nextKjAsyncTaskSetId_ = 1;
  uint32_t nextPromiseCapabilityFulfillerId_ = 1;
};

std::mutex gRuntimeRegistryMutex;
kj::HashMap<uint64_t, std::shared_ptr<RuntimeLoop>> gRuntimes;

uint64_t allocateRuntimeIdLocked() {
  while (true) {
    uint64_t id = gNextRuntimeId.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) {
      continue;
    }
    if (gRuntimes.find(id) == kj::none) {
      return id;
    }
  }
}

std::shared_ptr<RuntimeLoop> getRuntime(uint64_t runtimeId) {
  std::lock_guard<std::mutex> lock(gRuntimeRegistryMutex);
  KJ_IF_SOME(runtime, gRuntimes.find(runtimeId)) {
    return runtime;
  }
  return nullptr;
}

bool isRuntimeAlive(uint64_t runtimeId) {
  std::lock_guard<std::mutex> lock(gRuntimeRegistryMutex);
  return gRuntimes.find(runtimeId) != kj::none;
}

std::shared_ptr<RuntimeLoop> unregisterRuntime(uint64_t runtimeId) {
  std::lock_guard<std::mutex> lock(gRuntimeRegistryMutex);
  KJ_IF_SOME(runtime, gRuntimes.find(runtimeId)) {
    auto out = runtime;
    gRuntimes.erase(runtimeId);
    return out;
  }
  return nullptr;
}


uint64_t createRuntime(uint32_t maxFdsPerMessage) {
  auto runtime = std::make_shared<RuntimeLoop>(maxFdsPerMessage);
  uint64_t runtimeId;
  {
    std::lock_guard<std::mutex> lock(gRuntimeRegistryMutex);
    runtimeId = allocateRuntimeIdLocked();
    gRuntimes.insert(runtimeId, runtime);
  }
  return runtimeId;
}

void shutdown(RuntimeLoop& runtime) { runtime.shutdown(); }

bool isWorkerThread(const RuntimeLoop& runtime) { return runtime.isWorkerThread(); }

uint32_t retainTargetInline(RuntimeLoop& runtime, uint32_t target) {
  return runtime.retainTargetInline(target);
}

void releaseTargetInline(RuntimeLoop& runtime, uint32_t target) { runtime.releaseTargetInline(target); }

void releaseTargetsInline(RuntimeLoop& runtime, const std::vector<uint32_t>& targets) {
  runtime.releaseTargetsInline(targets);
}

std::pair<uint32_t, uint32_t> newPromiseCapabilityInline(RuntimeLoop& runtime) {
  return runtime.newPromiseCapabilityInline();
}

void promiseCapabilityFulfillInline(RuntimeLoop& runtime, uint32_t fulfillerId, uint32_t target) {
  runtime.promiseCapabilityFulfillInline(fulfillerId, target);
}

void promiseCapabilityRejectInline(RuntimeLoop& runtime, uint32_t fulfillerId,
                                   uint8_t exceptionTypeTag, std::string message,
                                   std::vector<uint8_t> detailBytes) {
  runtime.promiseCapabilityRejectInline(fulfillerId, exceptionTypeTag, std::move(message),
                                        std::move(detailBytes));
}

void promiseCapabilityReleaseInline(RuntimeLoop& runtime, uint32_t fulfillerId) {
  runtime.promiseCapabilityReleaseInline(fulfillerId);
}

uint32_t kjAsyncSleepNanosStartInline(RuntimeLoop& runtime, uint64_t delayNanos) {
  return runtime.kjAsyncSleepNanosStartInline(delayNanos);
}

void kjAsyncPromiseCancelInline(RuntimeLoop& runtime, uint32_t promiseId) {
  runtime.kjAsyncPromiseCancelInline(promiseId);
}

void kjAsyncPromiseReleaseInline(RuntimeLoop& runtime, uint32_t promiseId) {
  runtime.kjAsyncPromiseReleaseInline(promiseId);
}

uint32_t kjAsyncPromiseThenStartInline(RuntimeLoop& runtime, uint32_t firstPromiseId,
                                       uint32_t secondPromiseId) {
  return runtime.kjAsyncPromiseThenStartInline(firstPromiseId, secondPromiseId);
}

uint32_t kjAsyncPromiseCatchStartInline(RuntimeLoop& runtime, uint32_t promiseId,
                                        uint32_t fallbackPromiseId) {
  return runtime.kjAsyncPromiseCatchStartInline(promiseId, fallbackPromiseId);
}

uint32_t kjAsyncPromiseAllStartInline(RuntimeLoop& runtime,
                                      std::vector<uint32_t> promiseIds) {
  return runtime.kjAsyncPromiseAllStartInline(std::move(promiseIds));
}

uint32_t kjAsyncPromiseRaceStartInline(RuntimeLoop& runtime,
                                       std::vector<uint32_t> promiseIds) {
  return runtime.kjAsyncPromiseRaceStartInline(std::move(promiseIds));
}

uint32_t kjAsyncTaskSetNewInline(RuntimeLoop& runtime) {
  return runtime.kjAsyncTaskSetNewInline();
}

void kjAsyncTaskSetReleaseInline(RuntimeLoop& runtime, uint32_t taskSetId) {
  runtime.kjAsyncTaskSetReleaseInline(taskSetId);
}

void kjAsyncTaskSetAddPromiseInline(RuntimeLoop& runtime, uint32_t taskSetId,
                                    uint32_t promiseId) {
  runtime.kjAsyncTaskSetAddPromiseInline(taskSetId, promiseId);
}

void kjAsyncTaskSetClearInline(RuntimeLoop& runtime, uint32_t taskSetId) {
  runtime.kjAsyncTaskSetClearInline(taskSetId);
}

bool kjAsyncTaskSetIsEmptyInline(RuntimeLoop& runtime, uint32_t taskSetId) {
  return runtime.kjAsyncTaskSetIsEmptyInline(taskSetId);
}

uint32_t kjAsyncTaskSetOnEmptyStartInline(RuntimeLoop& runtime, uint32_t taskSetId) {
  return runtime.kjAsyncTaskSetOnEmptyStartInline(taskSetId);
}

uint32_t kjAsyncTaskSetErrorCountInline(RuntimeLoop& runtime, uint32_t taskSetId) {
  return runtime.kjAsyncTaskSetErrorCountInline(taskSetId);
}

std::pair<bool, std::string> kjAsyncTaskSetTakeLastErrorInline(RuntimeLoop& runtime,
                                                                uint32_t taskSetId) {
  auto maybeValue = runtime.kjAsyncTaskSetTakeLastErrorInline(taskSetId);
  KJ_IF_SOME(value, maybeValue) {
    return {true, value};
  }
  return {false, std::string()};
}

std::shared_ptr<RawCallCompletion> enqueueRawCall(
    RuntimeLoop& runtime, uint32_t target, uint64_t interfaceId, uint16_t methodId,
    LeanByteArrayRef request, std::vector<uint32_t> requestCaps) {
  return runtime.enqueueRawCall(target, interfaceId, methodId, std::move(request),
                                std::move(requestCaps));
}

std::shared_ptr<RawCallCompletion> enqueueRawCallData(
    RuntimeLoop& runtime, uint32_t target, uint64_t interfaceId, uint16_t methodId,
    const uint8_t* requestData, size_t requestSize, std::shared_ptr<const void> requestOwner,
    std::vector<uint32_t> requestCaps) {
  return runtime.enqueueRawCallData(target, interfaceId, methodId, requestData, requestSize,
                                    std::move(requestOwner), std::move(requestCaps));
}

std::shared_ptr<RegisterTargetCompletion> enqueueStartPendingCall(
    RuntimeLoop& runtime, uint32_t target, uint64_t interfaceId, uint16_t methodId,
    LeanByteArrayRef request, std::vector<uint32_t> requestCaps) {
  return runtime.enqueueStartPendingCall(target, interfaceId, methodId, std::move(request),
                                         std::move(requestCaps));
}

std::shared_ptr<RegisterTargetCompletion> enqueueStartPendingCallData(
    RuntimeLoop& runtime, uint32_t target, uint64_t interfaceId, uint16_t methodId,
    const uint8_t* requestData, size_t requestSize, std::shared_ptr<const void> requestOwner,
    std::vector<uint32_t> requestCaps) {
  return runtime.enqueueStartPendingCallData(target, interfaceId, methodId, requestData,
                                             requestSize, std::move(requestOwner),
                                             std::move(requestCaps));
}

std::shared_ptr<RegisterTargetCompletion> enqueueStartStreamingPendingCall(
    RuntimeLoop& runtime, uint32_t target, uint64_t interfaceId, uint16_t methodId,
    LeanByteArrayRef request, std::vector<uint32_t> requestCaps) {
  return runtime.enqueueStartStreamingPendingCall(target, interfaceId, methodId, std::move(request),
                                                  std::move(requestCaps));
}

std::shared_ptr<RegisterTargetCompletion> enqueueStartStreamingPendingCallData(
    RuntimeLoop& runtime, uint32_t target, uint64_t interfaceId, uint16_t methodId,
    const uint8_t* requestData, size_t requestSize, std::shared_ptr<const void> requestOwner,
    std::vector<uint32_t> requestCaps) {
  return runtime.enqueueStartStreamingPendingCallData(
      target, interfaceId, methodId, requestData, requestSize, std::move(requestOwner),
      std::move(requestCaps));
}

std::shared_ptr<RawCallCompletion> enqueueAwaitPendingCall(RuntimeLoop& runtime,
                                                           uint32_t pendingCallId) {
  return runtime.enqueueAwaitPendingCall(pendingCallId);
}

std::shared_ptr<UnitCompletion> enqueueReleasePendingCall(RuntimeLoop& runtime,
                                                          uint32_t pendingCallId) {
  return runtime.enqueueReleasePendingCall(pendingCallId);
}

std::shared_ptr<RegisterTargetCompletion> enqueueGetPipelinedCap(
    RuntimeLoop& runtime, uint32_t pendingCallId, std::vector<uint16_t> pointerPath) {
  return runtime.enqueueGetPipelinedCap(pendingCallId, std::move(pointerPath));
}

std::shared_ptr<UnitCompletion> enqueueStreamingCall(
    RuntimeLoop& runtime, uint32_t target, uint64_t interfaceId, uint16_t methodId,
    LeanByteArrayRef request, std::vector<uint32_t> requestCaps) {
  return runtime.enqueueStreamingCall(target, interfaceId, methodId, std::move(request),
                                      std::move(requestCaps));
}

std::shared_ptr<Int64Completion> enqueueTargetGetFd(RuntimeLoop& runtime, uint32_t target) {
  return runtime.enqueueTargetGetFd(target);
}

std::shared_ptr<UnitCompletion> enqueueTargetWhenResolved(RuntimeLoop& runtime, uint32_t target) {
  return runtime.enqueueTargetWhenResolved(target);
}

std::shared_ptr<RegisterTargetCompletion> enqueueTargetWhenResolvedStart(RuntimeLoop& runtime,
                                                                         uint32_t target) {
  return runtime.enqueueTargetWhenResolvedStart(target);
}

std::shared_ptr<BoolCompletion> enqueueTargetWhenResolvedPoll(RuntimeLoop& runtime,
                                                              uint32_t target) {
  return runtime.enqueueTargetWhenResolvedPoll(target);
}

std::shared_ptr<UnitCompletion> enqueueEnableTraceEncoder(RuntimeLoop& runtime) {
  return runtime.enqueueEnableTraceEncoder();
}

std::shared_ptr<UnitCompletion> enqueueDisableTraceEncoder(RuntimeLoop& runtime) {
  return runtime.enqueueDisableTraceEncoder();
}

std::shared_ptr<UnitCompletion> enqueueSetTraceEncoder(RuntimeLoop& runtime,
                                                       b_lean_obj_arg encoder) {
  return runtime.enqueueSetTraceEncoder(encoder);
}

std::shared_ptr<UnitCompletion> enqueueReleaseTarget(RuntimeLoop& runtime, uint32_t target) {
  return runtime.enqueueReleaseTarget(target);
}

std::shared_ptr<UnitCompletion> enqueueReleaseTargets(RuntimeLoop& runtime,
                                                      std::vector<uint32_t> targets) {
  return runtime.enqueueReleaseTargets(std::move(targets));
}

std::shared_ptr<RegisterTargetCompletion> enqueueRetainTarget(RuntimeLoop& runtime,
                                                              uint32_t target) {
  return runtime.enqueueRetainTarget(target);
}

std::shared_ptr<RegisterPairCompletion> enqueueNewPromiseCapability(RuntimeLoop& runtime) {
  return runtime.enqueueNewPromiseCapability();
}

std::shared_ptr<UnitCompletion> enqueuePromiseCapabilityFulfill(RuntimeLoop& runtime,
                                                                uint32_t fulfillerId,
                                                                uint32_t target) {
  return runtime.enqueuePromiseCapabilityFulfill(fulfillerId, target);
}

std::shared_ptr<UnitCompletion> enqueuePromiseCapabilityReject(RuntimeLoop& runtime,
                                                               uint32_t fulfillerId,
                                                               uint8_t exceptionTypeTag,
                                                               std::string message,
                                                               LeanByteArrayRef detailBytes) {
  return runtime.enqueuePromiseCapabilityReject(fulfillerId, exceptionTypeTag, std::move(message),
                                                std::move(detailBytes));
}

std::shared_ptr<UnitCompletion> enqueuePromiseCapabilityRelease(RuntimeLoop& runtime,
                                                                uint32_t fulfillerId) {
  return runtime.enqueuePromiseCapabilityRelease(fulfillerId);
}

std::shared_ptr<UnitCompletion> enqueueOrderingSetResolveHold(RuntimeLoop& runtime, bool enabled) {
  return runtime.enqueueOrderingSetResolveHold(enabled);
}

std::shared_ptr<UInt64Completion> enqueueOrderingFlushHeldResolves(RuntimeLoop& runtime) {
  return runtime.enqueueOrderingFlushHeldResolves();
}

std::shared_ptr<UInt64Completion> enqueueOrderingHeldResolveCount(RuntimeLoop& runtime) {
  return runtime.enqueueOrderingHeldResolveCount();
}

std::shared_ptr<RegisterTargetCompletion> enqueueConnectTarget(RuntimeLoop& runtime,
                                                               std::string address,
                                                               uint32_t portHint) {
  return runtime.enqueueConnectTarget(std::move(address), portHint);
}

std::shared_ptr<RegisterTargetCompletion> enqueueConnectTargetStart(RuntimeLoop& runtime,
                                                                    std::string address,
                                                                    uint32_t portHint) {
  return runtime.enqueueConnectTargetStart(std::move(address), portHint);
}

std::shared_ptr<RegisterTargetCompletion> enqueueConnectTargetFd(RuntimeLoop& runtime,
                                                                 uint32_t fd) {
  return runtime.enqueueConnectTargetFd(fd);
}

std::shared_ptr<RegisterPairCompletion> enqueueNewTransportPipe(RuntimeLoop& runtime) {
  return runtime.enqueueNewTransportPipe();
}

std::shared_ptr<RegisterTargetCompletion> enqueueNewTransportFromFd(RuntimeLoop& runtime,
                                                                    uint32_t fd) {
  return runtime.enqueueNewTransportFromFd(fd);
}

std::shared_ptr<RegisterTargetCompletion> enqueueNewTransportFromFdTake(RuntimeLoop& runtime,
                                                                        uint32_t fd) {
  return runtime.enqueueNewTransportFromFdTake(fd);
}

std::shared_ptr<UnitCompletion> enqueueReleaseTransport(RuntimeLoop& runtime,
                                                        uint32_t transportId) {
  return runtime.enqueueReleaseTransport(transportId);
}

std::shared_ptr<Int64Completion> enqueueTransportGetFd(RuntimeLoop& runtime,
                                                       uint32_t transportId) {
  return runtime.enqueueTransportGetFd(transportId);
}

std::shared_ptr<RegisterTargetCompletion> enqueueConnectTargetTransport(RuntimeLoop& runtime,
                                                                        uint32_t transportId) {
  return runtime.enqueueConnectTargetTransport(transportId);
}

std::shared_ptr<RegisterTargetCompletion> enqueueListenLoopback(RuntimeLoop& runtime,
                                                                std::string address,
                                                                uint32_t portHint) {
  return runtime.enqueueListenLoopback(std::move(address), portHint);
}

std::shared_ptr<UnitCompletion> enqueueAcceptLoopback(RuntimeLoop& runtime, uint32_t listenerId) {
  return runtime.enqueueAcceptLoopback(listenerId);
}

std::shared_ptr<UnitCompletion> enqueueReleaseListener(RuntimeLoop& runtime, uint32_t listenerId) {
  return runtime.enqueueReleaseListener(listenerId);
}

std::shared_ptr<RegisterTargetCompletion> enqueueNewClient(RuntimeLoop& runtime,
                                                           std::string address,
                                                           uint32_t portHint) {
  return runtime.enqueueNewClient(std::move(address), portHint);
}

std::shared_ptr<RegisterTargetCompletion> enqueueNewClientStart(RuntimeLoop& runtime,
                                                                std::string address,
                                                                uint32_t portHint) {
  return runtime.enqueueNewClientStart(std::move(address), portHint);
}

std::shared_ptr<UnitCompletion> enqueueReleaseClient(RuntimeLoop& runtime, uint32_t clientId) {
  return runtime.enqueueReleaseClient(clientId);
}

std::shared_ptr<RegisterTargetCompletion> enqueueClientBootstrap(RuntimeLoop& runtime,
                                                                 uint32_t clientId) {
  return runtime.enqueueClientBootstrap(clientId);
}

std::shared_ptr<UnitCompletion> enqueueClientOnDisconnect(RuntimeLoop& runtime, uint32_t clientId) {
  return runtime.enqueueClientOnDisconnect(clientId);
}

std::shared_ptr<RegisterTargetCompletion> enqueueClientOnDisconnectStart(RuntimeLoop& runtime,
                                                                         uint32_t clientId) {
  return runtime.enqueueClientOnDisconnectStart(clientId);
}

std::shared_ptr<UnitCompletion> enqueueClientSetFlowLimit(RuntimeLoop& runtime, uint32_t clientId,
                                                          uint64_t words) {
  return runtime.enqueueClientSetFlowLimit(clientId, words);
}

std::shared_ptr<UInt64Completion> enqueueClientQueueSize(RuntimeLoop& runtime, uint32_t clientId) {
  return runtime.enqueueClientQueueSize(clientId);
}

std::shared_ptr<UInt64Completion> enqueueClientQueueCount(RuntimeLoop& runtime, uint32_t clientId) {
  return runtime.enqueueClientQueueCount(clientId);
}

std::shared_ptr<UInt64Completion> enqueueClientOutgoingWaitNanos(RuntimeLoop& runtime,
                                                                 uint32_t clientId) {
  return runtime.enqueueClientOutgoingWaitNanos(clientId);
}

std::shared_ptr<UInt64Completion> enqueueTargetCount(RuntimeLoop& runtime) {
  return runtime.enqueueTargetCount();
}

std::shared_ptr<UInt64Completion> enqueueListenerCount(RuntimeLoop& runtime) {
  return runtime.enqueueListenerCount();
}

std::shared_ptr<UInt64Completion> enqueueClientCount(RuntimeLoop& runtime) {
  return runtime.enqueueClientCount();
}

std::shared_ptr<UInt64Completion> enqueueServerCount(RuntimeLoop& runtime) {
  return runtime.enqueueServerCount();
}

std::shared_ptr<UInt64Completion> enqueuePendingCallCount(RuntimeLoop& runtime) {
  return runtime.enqueuePendingCallCount();
}

std::shared_ptr<RegisterTargetCompletion> enqueueNewServer(RuntimeLoop& runtime,
                                                           uint32_t bootstrapTarget) {
  return runtime.enqueueNewServer(bootstrapTarget);
}

std::shared_ptr<RegisterTargetCompletion> enqueueNewServerWithBootstrapFactory(
    RuntimeLoop& runtime, b_lean_obj_arg bootstrapFactory) {
  return runtime.enqueueNewServerWithBootstrapFactory(bootstrapFactory);
}

std::shared_ptr<UnitCompletion> enqueueReleaseServer(RuntimeLoop& runtime, uint32_t serverId) {
  return runtime.enqueueReleaseServer(serverId);
}

std::shared_ptr<RegisterTargetCompletion> enqueueServerListen(RuntimeLoop& runtime, uint32_t serverId,
                                                              std::string address,
                                                              uint32_t portHint) {
  return runtime.enqueueServerListen(serverId, std::move(address), portHint);
}

std::shared_ptr<UnitCompletion> enqueueServerAccept(RuntimeLoop& runtime, uint32_t serverId,
                                                    uint32_t listenerId) {
  return runtime.enqueueServerAccept(serverId, listenerId);
}

std::shared_ptr<RegisterTargetCompletion> enqueueServerAcceptStart(RuntimeLoop& runtime,
                                                                   uint32_t serverId,
                                                                   uint32_t listenerId) {
  return runtime.enqueueServerAcceptStart(serverId, listenerId);
}

std::shared_ptr<UnitCompletion> enqueueServerAcceptFd(RuntimeLoop& runtime, uint32_t serverId,
                                                      uint32_t fd) {
  return runtime.enqueueServerAcceptFd(serverId, fd);
}

std::shared_ptr<UnitCompletion> enqueueServerAcceptTransport(RuntimeLoop& runtime,
                                                             uint32_t serverId,
                                                             uint32_t transportId) {
  return runtime.enqueueServerAcceptTransport(serverId, transportId);
}

std::shared_ptr<UnitCompletion> enqueueServerDrain(RuntimeLoop& runtime, uint32_t serverId) {
  return runtime.enqueueServerDrain(serverId);
}

std::shared_ptr<RegisterTargetCompletion> enqueueServerDrainStart(RuntimeLoop& runtime,
                                                                  uint32_t serverId) {
  return runtime.enqueueServerDrainStart(serverId);
}

std::shared_ptr<RegisterTargetCompletion> enqueueRegisterLoopbackTarget(RuntimeLoop& runtime) {
  return runtime.enqueueRegisterLoopbackTarget();
}

std::shared_ptr<RegisterTargetCompletion> enqueueRegisterLoopbackTarget(RuntimeLoop& runtime,
                                                                        uint32_t bootstrapTarget) {
  return runtime.enqueueRegisterLoopbackTarget(bootstrapTarget);
}

std::shared_ptr<RegisterTargetCompletion> enqueueRegisterHandlerTarget(RuntimeLoop& runtime,
                                                                       b_lean_obj_arg handler) {
  return runtime.enqueueRegisterHandlerTarget(handler);
}

std::shared_ptr<RegisterTargetCompletion> enqueueRegisterAdvancedHandlerTarget(RuntimeLoop& runtime,
                                                                               b_lean_obj_arg handler) {
  return runtime.enqueueRegisterAdvancedHandlerTarget(handler);
}

std::shared_ptr<RegisterTargetCompletion> enqueueRegisterTailCallHandlerTarget(RuntimeLoop& runtime,
                                                                               b_lean_obj_arg handler) {
  return runtime.enqueueRegisterTailCallHandlerTarget(handler);
}

std::shared_ptr<RegisterTargetCompletion> enqueueRegisterTailCallTarget(RuntimeLoop& runtime,
                                                                        uint32_t target) {
  return runtime.enqueueRegisterTailCallTarget(target);
}

std::shared_ptr<RegisterTargetCompletion> enqueueRegisterFdTarget(RuntimeLoop& runtime, uint32_t fd) {
  return runtime.enqueueRegisterFdTarget(fd);
}

std::shared_ptr<RegisterTargetCompletion> enqueueRegisterFdProbeTarget(RuntimeLoop& runtime) {
  return runtime.enqueueRegisterFdProbeTarget();
}

std::shared_ptr<RawCallCompletion> enqueueCppCallWithAccept(
    RuntimeLoop& runtime, uint32_t serverId, uint32_t listenerId, std::string address,
    uint32_t portHint, uint64_t interfaceId, uint16_t methodId, LeanByteArrayRef request,
    std::vector<uint32_t> requestCaps) {
  return runtime.enqueueCppCallWithAccept(serverId, listenerId, std::move(address), portHint,
                                          interfaceId, methodId, std::move(request),
                                          std::move(requestCaps));
}

std::shared_ptr<RawCallCompletion> enqueueCppCallPipelinedWithAccept(
    RuntimeLoop& runtime, uint32_t serverId, uint32_t listenerId, std::string address,
    uint32_t portHint, uint64_t interfaceId, uint16_t methodId, LeanByteArrayRef request,
    std::vector<uint32_t> requestCaps, LeanByteArrayRef pipelinedRequest,
    std::vector<uint32_t> pipelinedRequestCaps) {
  return runtime.enqueueCppCallPipelinedWithAccept(
      serverId, listenerId, std::move(address), portHint, interfaceId, methodId,
      std::move(request), std::move(requestCaps), std::move(pipelinedRequest),
      std::move(pipelinedRequestCaps));
}

std::shared_ptr<RegisterTargetCompletion> enqueueAwaitRegisterPromise(RuntimeLoop& runtime,
                                                                      uint32_t promiseId) {
  return runtime.enqueueAwaitRegisterPromise(promiseId);
}

std::shared_ptr<UnitCompletion> enqueueCancelRegisterPromise(RuntimeLoop& runtime,
                                                             uint32_t promiseId) {
  return runtime.enqueueCancelRegisterPromise(promiseId);
}

std::shared_ptr<UnitCompletion> enqueueReleaseRegisterPromise(RuntimeLoop& runtime,
                                                              uint32_t promiseId) {
  return runtime.enqueueReleaseRegisterPromise(promiseId);
}

std::shared_ptr<UnitCompletion> enqueueAwaitUnitPromise(RuntimeLoop& runtime, uint32_t promiseId) {
  return runtime.enqueueAwaitUnitPromise(promiseId);
}

std::shared_ptr<UnitCompletion> enqueueCancelUnitPromise(RuntimeLoop& runtime, uint32_t promiseId) {
  return runtime.enqueueCancelUnitPromise(promiseId);
}

std::shared_ptr<UnitCompletion> enqueueReleaseUnitPromise(RuntimeLoop& runtime,
                                                          uint32_t promiseId) {
  return runtime.enqueueReleaseUnitPromise(promiseId);
}

std::shared_ptr<KjPromiseIdCompletion> enqueueKjAsyncSleepNanosStart(
    RuntimeLoop& runtime, uint64_t delayNanos) {
  return runtime.enqueueKjAsyncSleepNanosStart(delayNanos);
}

std::shared_ptr<UnitCompletion> enqueueKjAsyncPromiseAwait(RuntimeLoop& runtime,
                                                           uint32_t promiseId) {
  return runtime.enqueueKjAsyncPromiseAwait(promiseId);
}

std::shared_ptr<UnitCompletion> enqueueKjAsyncPromiseCancel(RuntimeLoop& runtime,
                                                            uint32_t promiseId) {
  return runtime.enqueueKjAsyncPromiseCancel(promiseId);
}

std::shared_ptr<UnitCompletion> enqueueKjAsyncPromiseRelease(RuntimeLoop& runtime,
                                                             uint32_t promiseId) {
  return runtime.enqueueKjAsyncPromiseRelease(promiseId);
}

std::shared_ptr<KjPromiseIdCompletion> enqueueKjAsyncPromiseThenStart(
    RuntimeLoop& runtime, uint32_t firstPromiseId, uint32_t secondPromiseId) {
  return runtime.enqueueKjAsyncPromiseThenStart(firstPromiseId, secondPromiseId);
}

std::shared_ptr<KjPromiseIdCompletion> enqueueKjAsyncPromiseCatchStart(
    RuntimeLoop& runtime, uint32_t promiseId, uint32_t fallbackPromiseId) {
  return runtime.enqueueKjAsyncPromiseCatchStart(promiseId, fallbackPromiseId);
}

std::shared_ptr<KjPromiseIdCompletion> enqueueKjAsyncPromiseAllStart(
    RuntimeLoop& runtime, std::vector<uint32_t> promiseIds) {
  return runtime.enqueueKjAsyncPromiseAllStart(std::move(promiseIds));
}

std::shared_ptr<KjPromiseIdCompletion> enqueueKjAsyncPromiseRaceStart(
    RuntimeLoop& runtime, std::vector<uint32_t> promiseIds) {
  return runtime.enqueueKjAsyncPromiseRaceStart(std::move(promiseIds));
}

std::shared_ptr<RegisterTargetCompletion> enqueueKjAsyncTaskSetNew(RuntimeLoop& runtime) {
  return runtime.enqueueKjAsyncTaskSetNew();
}

std::shared_ptr<UnitCompletion> enqueueKjAsyncTaskSetRelease(RuntimeLoop& runtime,
                                                             uint32_t taskSetId) {
  return runtime.enqueueKjAsyncTaskSetRelease(taskSetId);
}

std::shared_ptr<UnitCompletion> enqueueKjAsyncTaskSetAddPromise(RuntimeLoop& runtime,
                                                                uint32_t taskSetId,
                                                                uint32_t promiseId) {
  return runtime.enqueueKjAsyncTaskSetAddPromise(taskSetId, promiseId);
}

std::shared_ptr<UnitCompletion> enqueueKjAsyncTaskSetClear(RuntimeLoop& runtime,
                                                           uint32_t taskSetId) {
  return runtime.enqueueKjAsyncTaskSetClear(taskSetId);
}

std::shared_ptr<BoolCompletion> enqueueKjAsyncTaskSetIsEmpty(RuntimeLoop& runtime,
                                                             uint32_t taskSetId) {
  return runtime.enqueueKjAsyncTaskSetIsEmpty(taskSetId);
}

std::shared_ptr<KjPromiseIdCompletion> enqueueKjAsyncTaskSetOnEmptyStart(RuntimeLoop& runtime,
                                                                          uint32_t taskSetId) {
  return runtime.enqueueKjAsyncTaskSetOnEmptyStart(taskSetId);
}

std::shared_ptr<UInt64Completion> enqueueKjAsyncTaskSetErrorCount(RuntimeLoop& runtime,
                                                                  uint32_t taskSetId) {
  return runtime.enqueueKjAsyncTaskSetErrorCount(taskSetId);
}

std::shared_ptr<OptionalStringCompletion> enqueueKjAsyncTaskSetTakeLastError(
    RuntimeLoop& runtime, uint32_t taskSetId) {
  return runtime.enqueueKjAsyncTaskSetTakeLastError(taskSetId);
}

std::shared_ptr<UnitCompletion> enqueuePump(RuntimeLoop& runtime) { return runtime.enqueuePump(); }
std::shared_ptr<AsyncUnitCompletion> enqueuePumpAsync(RuntimeLoop& runtime, lean_object* promise) {
  return runtime.enqueuePumpAsync(promise);
}

std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatNewClient(RuntimeLoop& runtime,
                                                                   std::string name) {
  return runtime.enqueueMultiVatNewClient(std::move(name));
}

std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatNewServer(RuntimeLoop& runtime,
                                                                   std::string name,
                                                                   uint32_t bootstrapTarget) {
  return runtime.enqueueMultiVatNewServer(std::move(name), bootstrapTarget);
}

std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatNewServerWithBootstrapFactory(
    RuntimeLoop& runtime, std::string name, b_lean_obj_arg bootstrapFactory) {
  return runtime.enqueueMultiVatNewServerWithBootstrapFactory(std::move(name), bootstrapFactory);
}

std::shared_ptr<UnitCompletion> enqueueMultiVatReleasePeer(RuntimeLoop& runtime, uint32_t peerId) {
  return runtime.enqueueMultiVatReleasePeer(peerId);
}

std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatBootstrap(RuntimeLoop& runtime,
                                                                   uint32_t sourcePeerId,
                                                                   std::string host,
                                                                   bool unique) {
  return runtime.enqueueMultiVatBootstrap(sourcePeerId, std::move(host), unique);
}

std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatBootstrapPeer(RuntimeLoop& runtime,
                                                                       uint32_t sourcePeerId,
                                                                       uint32_t peerId,
                                                                       bool unique) {
  return runtime.enqueueMultiVatBootstrapPeer(sourcePeerId, peerId, unique);
}

std::shared_ptr<UnitCompletion> enqueueMultiVatSetForwardingEnabled(RuntimeLoop& runtime,
                                                                    bool enabled) {
  return runtime.enqueueMultiVatSetForwardingEnabled(enabled);
}

std::shared_ptr<UnitCompletion> enqueueMultiVatResetForwardingStats(RuntimeLoop& runtime) {
  return runtime.enqueueMultiVatResetForwardingStats();
}

std::shared_ptr<UInt64Completion> enqueueMultiVatForwardCount(RuntimeLoop& runtime) {
  return runtime.enqueueMultiVatForwardCount();
}

std::shared_ptr<UInt64Completion> enqueueMultiVatThirdPartyTokenCount(RuntimeLoop& runtime) {
  return runtime.enqueueMultiVatThirdPartyTokenCount();
}

std::shared_ptr<UInt64Completion> enqueueMultiVatDeniedForwardCount(RuntimeLoop& runtime) {
  return runtime.enqueueMultiVatDeniedForwardCount();
}

std::shared_ptr<UInt64Completion> enqueueMultiVatHasConnection(RuntimeLoop& runtime,
                                                               uint32_t fromPeerId,
                                                               uint32_t toPeerId) {
  return runtime.enqueueMultiVatHasConnection(fromPeerId, toPeerId);
}

std::shared_ptr<UnitCompletion> enqueueMultiVatSetRestorer(RuntimeLoop& runtime, uint32_t peerId,
                                                           b_lean_obj_arg restorer) {
  return runtime.enqueueMultiVatSetRestorer(peerId, restorer);
}

std::shared_ptr<UnitCompletion> enqueueMultiVatClearRestorer(RuntimeLoop& runtime, uint32_t peerId) {
  return runtime.enqueueMultiVatClearRestorer(peerId);
}

std::shared_ptr<UnitCompletion> enqueueMultiVatPublishSturdyRef(RuntimeLoop& runtime,
                                                                uint32_t hostPeerId,
                                                                LeanByteArrayRef objectId,
                                                                uint32_t targetId) {
  return runtime.enqueueMultiVatPublishSturdyRef(hostPeerId, std::move(objectId), targetId);
}

std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatPublishSturdyRefStart(
    RuntimeLoop& runtime, uint32_t hostPeerId, LeanByteArrayRef objectId, uint32_t targetId) {
  return runtime.enqueueMultiVatPublishSturdyRefStart(hostPeerId, std::move(objectId), targetId);
}

std::shared_ptr<UnitCompletion> enqueueMultiVatUnpublishSturdyRef(RuntimeLoop& runtime,
                                                                  uint32_t hostPeerId,
                                                                  LeanByteArrayRef objectId) {
  return runtime.enqueueMultiVatUnpublishSturdyRef(hostPeerId, std::move(objectId));
}

std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatUnpublishSturdyRefStart(
    RuntimeLoop& runtime, uint32_t hostPeerId, LeanByteArrayRef objectId) {
  return runtime.enqueueMultiVatUnpublishSturdyRefStart(hostPeerId, std::move(objectId));
}

std::shared_ptr<UnitCompletion> enqueueMultiVatClearPublishedSturdyRefs(RuntimeLoop& runtime,
                                                                         uint32_t hostPeerId) {
  return runtime.enqueueMultiVatClearPublishedSturdyRefs(hostPeerId);
}

std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatClearPublishedSturdyRefsStart(
    RuntimeLoop& runtime, uint32_t hostPeerId) {
  return runtime.enqueueMultiVatClearPublishedSturdyRefsStart(hostPeerId);
}

std::shared_ptr<UInt64Completion> enqueueMultiVatPublishedSturdyRefCount(RuntimeLoop& runtime,
                                                                          uint32_t hostPeerId) {
  return runtime.enqueueMultiVatPublishedSturdyRefCount(hostPeerId);
}

std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatRestoreSturdyRef(
    RuntimeLoop& runtime, uint32_t sourcePeerId, std::string host, bool unique,
    LeanByteArrayRef objectId) {
  return runtime.enqueueMultiVatRestoreSturdyRef(sourcePeerId, std::move(host), unique,
                                                 std::move(objectId));
}

std::shared_ptr<RegisterTargetCompletion> enqueueMultiVatRestoreSturdyRefStart(
    RuntimeLoop& runtime, uint32_t sourcePeerId, std::string host, bool unique,
    LeanByteArrayRef objectId) {
  return runtime.enqueueMultiVatRestoreSturdyRefStart(sourcePeerId, std::move(host), unique,
                                                      std::move(objectId));
}

std::shared_ptr<DiagnosticsCompletion> enqueueMultiVatGetDiagnostics(
    RuntimeLoop& runtime, uint32_t peerId, b_lean_obj_arg targetVatId) {
  return runtime.enqueueMultiVatGetDiagnostics(peerId, targetVatId);
}

std::shared_ptr<UnitCompletion> enqueueMultiVatConnectionBlock(RuntimeLoop& runtime,
                                                               uint32_t fromPeerId,
                                                               uint32_t toPeerId) {
  return runtime.enqueueMultiVatConnectionBlock(fromPeerId, toPeerId);
}

std::shared_ptr<UnitCompletion> enqueueMultiVatConnectionUnblock(RuntimeLoop& runtime,
                                                                 uint32_t fromPeerId,
                                                                 uint32_t toPeerId) {
  return runtime.enqueueMultiVatConnectionUnblock(fromPeerId, toPeerId);
}

std::shared_ptr<UnitCompletion> enqueueMultiVatConnectionDisconnect(
    RuntimeLoop& runtime, uint32_t fromPeerId, uint32_t toPeerId, uint8_t exceptionTypeTag,
    std::string message, std::vector<uint8_t> detailBytes) {
  return runtime.enqueueMultiVatConnectionDisconnect(fromPeerId, toPeerId, exceptionTypeTag,
                                                     std::move(message), std::move(detailBytes));
}

std::shared_ptr<ProtocolMessageCountsCompletion>
enqueueMultiVatConnectionResolveDisembargoCounts(RuntimeLoop& runtime, uint32_t fromPeerId,
                                                  uint32_t toPeerId) {
  return runtime.enqueueMultiVatConnectionResolveDisembargoCounts(fromPeerId, toPeerId);
}

std::shared_ptr<ByteArrayCompletion>
enqueueMultiVatConnectionResolveDisembargoTrace(RuntimeLoop& runtime, uint32_t fromPeerId,
                                                uint32_t toPeerId) {
  return runtime.enqueueMultiVatConnectionResolveDisembargoTrace(fromPeerId, toPeerId);
}

std::shared_ptr<UnitCompletion>
enqueueMultiVatConnectionResetResolveDisembargoTrace(RuntimeLoop& runtime, uint32_t fromPeerId,
                                                     uint32_t toPeerId) {
  return runtime.enqueueMultiVatConnectionResetResolveDisembargoTrace(fromPeerId, toPeerId);
}

}  // namespace capnp_lean_rpc

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_pending_call_release_deferred(
    uint64_t runtimeId, uint32_t pendingCallId) {
  auto runtime = capnp_lean_rpc::getRuntime(runtimeId);
  if (!runtime) {
    return capnp_lean_rpc::mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    capnp_lean_rpc::enqueueReleasePendingCall(*runtime, pendingCallId);
    return capnp_lean_rpc::mkIoOkUnit();
  } catch (const kj::Exception& e) {
    return capnp_lean_rpc::mkIoUserError(capnp_lean_rpc::describeKjException(e));
  } catch (const std::exception& e) {
    return capnp_lean_rpc::mkIoUserError(e.what());
  } catch (...) {
    return capnp_lean_rpc::mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_pending_call_release_deferred");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_release_target_deferred(
    uint64_t runtimeId, uint32_t target) {
  auto runtime = capnp_lean_rpc::getRuntime(runtimeId);
  if (!runtime) {
    return capnp_lean_rpc::mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    capnp_lean_rpc::enqueueReleaseTarget(*runtime, target);
    return capnp_lean_rpc::mkIoOkUnit();
  } catch (const kj::Exception& e) {
    return capnp_lean_rpc::mkIoUserError(capnp_lean_rpc::describeKjException(e));
  } catch (const std::exception& e) {
    return capnp_lean_rpc::mkIoUserError(e.what());
  } catch (...) {
    return capnp_lean_rpc::mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_release_target_deferred");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_target_when_resolved_poll(
    uint64_t runtimeId, uint32_t target) {
  auto runtime = capnp_lean_rpc::getRuntime(runtimeId);
  if (!runtime) {
    return capnp_lean_rpc::mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = capnp_lean_rpc::enqueueTargetWhenResolvedPoll(*runtime, target);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return capnp_lean_rpc::mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box(completion->value ? 1 : 0));
    }
  } catch (const kj::Exception& e) {
    return capnp_lean_rpc::mkIoUserError(capnp_lean_rpc::describeKjException(e));
  } catch (const std::exception& e) {
    return capnp_lean_rpc::mkIoUserError(e.what());
  } catch (...) {
    return capnp_lean_rpc::mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_target_when_resolved_poll");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_ordering_set_resolve_hold(
    uint64_t runtimeId, uint8_t enabled) {
  auto runtime = capnp_lean_rpc::getRuntime(runtimeId);
  if (!runtime) {
    return capnp_lean_rpc::mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = capnp_lean_rpc::enqueueOrderingSetResolveHold(*runtime, enabled != 0);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return capnp_lean_rpc::mkIoUserError(completion->error);
      }
    }
    return capnp_lean_rpc::mkIoOkUnit();
  } catch (const kj::Exception& e) {
    return capnp_lean_rpc::mkIoUserError(capnp_lean_rpc::describeKjException(e));
  } catch (const std::exception& e) {
    return capnp_lean_rpc::mkIoUserError(e.what());
  } catch (...) {
    return capnp_lean_rpc::mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_ordering_set_resolve_hold");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_ordering_flush_resolves(
    uint64_t runtimeId) {
  auto runtime = capnp_lean_rpc::getRuntime(runtimeId);
  if (!runtime) {
    return capnp_lean_rpc::mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = capnp_lean_rpc::enqueueOrderingFlushHeldResolves(*runtime);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return capnp_lean_rpc::mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint64(completion->value));
    }
  } catch (const kj::Exception& e) {
    return capnp_lean_rpc::mkIoUserError(capnp_lean_rpc::describeKjException(e));
  } catch (const std::exception& e) {
    return capnp_lean_rpc::mkIoUserError(e.what());
  } catch (...) {
    return capnp_lean_rpc::mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_ordering_flush_resolves");
  }
}

extern "C" LEAN_EXPORT lean_obj_res capnp_lean_rpc_runtime_ordering_held_resolve_count(
    uint64_t runtimeId) {
  auto runtime = capnp_lean_rpc::getRuntime(runtimeId);
  if (!runtime) {
    return capnp_lean_rpc::mkIoUserError("Capnp.Rpc runtime handle is invalid or already released");
  }

  try {
    auto completion = capnp_lean_rpc::enqueueOrderingHeldResolveCount(*runtime);
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      completion->cv.wait(lock, [&completion]() { return completion->done; });
      if (!completion->ok) {
        return capnp_lean_rpc::mkIoUserError(completion->error);
      }
      return lean_io_result_mk_ok(lean_box_uint64(completion->value));
    }
  } catch (const kj::Exception& e) {
    return capnp_lean_rpc::mkIoUserError(capnp_lean_rpc::describeKjException(e));
  } catch (const std::exception& e) {
    return capnp_lean_rpc::mkIoUserError(e.what());
  } catch (...) {
    return capnp_lean_rpc::mkIoUserError(
        "unknown exception in capnp_lean_rpc_runtime_ordering_held_resolve_count");
  }
}
