#include "rpc_bridge_payload_ref.h"

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace capnp_lean_rpc_payload_ref {

namespace {

struct RuntimePayloadRefEntry {
  uint64_t runtimeId = 0;
  lean_object* messageBytes = nullptr;
  lean_object* capBytes = nullptr;
  std::shared_ptr<const RuntimePayloadRawBytes> rawBytes;

  inline bool hasLeanBytes() const { return messageBytes != nullptr && capBytes != nullptr; }
};

std::mutex gRuntimePayloadRefsMutex;
std::unordered_map<uint32_t, RuntimePayloadRefEntry> gRuntimePayloadRefs;
std::atomic<uint32_t> gNextRuntimePayloadRefId{1};

uint32_t allocateRuntimePayloadRefIdLocked() {
  auto id = gNextRuntimePayloadRefId.fetch_add(1, std::memory_order_relaxed);
  while (id == 0 || gRuntimePayloadRefs.find(id) != gRuntimePayloadRefs.end()) {
    id = gNextRuntimePayloadRefId.fetch_add(1, std::memory_order_relaxed);
  }
  return id;
}

}  // namespace

uint32_t registerRuntimePayloadRef(uint64_t runtimeId, lean_object* messageBytes,
                                   lean_object* capBytes, bool retainInputs) {
  if (messageBytes == nullptr || capBytes == nullptr) {
    throw std::runtime_error("runtime payload ref requires message and cap byte arrays");
  }
  lean_mark_mt(messageBytes);
  lean_mark_mt(capBytes);
  if (retainInputs) {
    lean_inc(messageBytes);
    lean_inc(capBytes);
  }

  std::lock_guard<std::mutex> lock(gRuntimePayloadRefsMutex);
  uint32_t id = allocateRuntimePayloadRefIdLocked();
  gRuntimePayloadRefs.emplace(
      id, RuntimePayloadRefEntry{runtimeId, messageBytes, capBytes, nullptr});
  return id;
}

uint32_t registerRuntimePayloadRefFromRawCallResult(
    uint64_t runtimeId, capnp_lean_rpc::RawCallResult&& result) {
  auto rawBytes = std::make_shared<RuntimePayloadRawBytes>();
  rawBytes->messageWords = std::move(result.responseWords);
  rawBytes->capBytes = std::move(result.responseCaps);

  std::lock_guard<std::mutex> lock(gRuntimePayloadRefsMutex);
  uint32_t id = allocateRuntimePayloadRefIdLocked();
  gRuntimePayloadRefs.emplace(id, RuntimePayloadRefEntry{runtimeId, nullptr, nullptr, rawBytes});
  return id;
}

kj::Maybe<RetainedRuntimePayloadRef> retainRuntimePayloadRef(uint32_t payloadRefId) {
  std::lock_guard<std::mutex> lock(gRuntimePayloadRefsMutex);
  auto it = gRuntimePayloadRefs.find(payloadRefId);
  if (it == gRuntimePayloadRefs.end()) {
    return kj::none;
  }
  if (it->second.hasLeanBytes()) {
    lean_inc(it->second.messageBytes);
    lean_inc(it->second.capBytes);
    return RetainedRuntimePayloadRef{it->second.runtimeId, it->second.messageBytes,
                                     it->second.capBytes};
  }
  return RetainedRuntimePayloadRef{it->second.runtimeId, it->second.rawBytes};
}

bool releaseRuntimePayloadRef(uint64_t runtimeId, uint32_t payloadRefId, std::string& errorOut) {
  std::lock_guard<std::mutex> lock(gRuntimePayloadRefsMutex);
  auto it = gRuntimePayloadRefs.find(payloadRefId);
  if (it == gRuntimePayloadRefs.end()) {
    errorOut = "unknown runtime payload ref id";
    return false;
  }
  if (it->second.runtimeId != runtimeId) {
    errorOut = "runtime payload ref belongs to a different Capnp.Rpc runtime";
    return false;
  }
  if (it->second.hasLeanBytes()) {
    lean_dec(it->second.messageBytes);
    lean_dec(it->second.capBytes);
  }
  gRuntimePayloadRefs.erase(it);
  return true;
}

uint64_t releaseRuntimePayloadRefsForRuntime(uint64_t runtimeId) {
  std::lock_guard<std::mutex> lock(gRuntimePayloadRefsMutex);
  uint64_t released = 0;
  for (auto it = gRuntimePayloadRefs.begin(); it != gRuntimePayloadRefs.end();) {
    if (it->second.runtimeId == runtimeId) {
      if (it->second.hasLeanBytes()) {
        lean_dec(it->second.messageBytes);
        lean_dec(it->second.capBytes);
      }
      it = gRuntimePayloadRefs.erase(it);
      released += 1;
    } else {
      ++it;
    }
  }
  return released;
}

}  // namespace capnp_lean_rpc_payload_ref
