#pragma once

#include "rpc_bridge_common.h"

#include <memory>
#include <string>
#include <vector>

namespace capnp_lean_rpc_payload_ref {

struct RuntimePayloadRawBytes {
  kj::Array<capnp::word> messageWords;
  std::vector<uint8_t> capBytes;

  inline const uint8_t* messageData() const {
    auto bytes = messageWords.asBytes();
    return bytes.size() == 0 ? nullptr : reinterpret_cast<const uint8_t*>(bytes.begin());
  }

  inline size_t messageSize() const { return messageWords.asBytes().size(); }

  inline const uint8_t* capData() const { return capBytes.empty() ? nullptr : capBytes.data(); }

  inline size_t capSize() const { return capBytes.size(); }
};

struct RetainedRuntimePayloadRef {
  uint64_t runtimeId = 0;
  lean_object* messageBytes = nullptr;
  lean_object* capBytes = nullptr;
  std::shared_ptr<const RuntimePayloadRawBytes> rawBytes;

  RetainedRuntimePayloadRef() = default;

  RetainedRuntimePayloadRef(uint64_t runtimeId, lean_object* messageBytes,
                            lean_object* capBytes)
      : runtimeId(runtimeId), messageBytes(messageBytes), capBytes(capBytes) {}

  RetainedRuntimePayloadRef(uint64_t runtimeId,
                            std::shared_ptr<const RuntimePayloadRawBytes> rawBytes)
      : runtimeId(runtimeId), rawBytes(std::move(rawBytes)) {}

  RetainedRuntimePayloadRef(const RetainedRuntimePayloadRef&) = delete;
  RetainedRuntimePayloadRef& operator=(const RetainedRuntimePayloadRef&) = delete;

  RetainedRuntimePayloadRef(RetainedRuntimePayloadRef&& other) noexcept
      : runtimeId(other.runtimeId),
        messageBytes(other.messageBytes),
        capBytes(other.capBytes),
        rawBytes(std::move(other.rawBytes)) {
    other.messageBytes = nullptr;
    other.capBytes = nullptr;
  }

  RetainedRuntimePayloadRef& operator=(RetainedRuntimePayloadRef&& other) noexcept {
    if (this != &other) {
      reset();
      runtimeId = other.runtimeId;
      messageBytes = other.messageBytes;
      capBytes = other.capBytes;
      rawBytes = std::move(other.rawBytes);
      other.messageBytes = nullptr;
      other.capBytes = nullptr;
    }
    return *this;
  }

  ~RetainedRuntimePayloadRef() { reset(); }

  void reset() {
    if (messageBytes != nullptr) {
      lean_dec(messageBytes);
      messageBytes = nullptr;
    }
    if (capBytes != nullptr) {
      lean_dec(capBytes);
      capBytes = nullptr;
    }
    rawBytes.reset();
  }

  inline bool hasLeanBytes() const { return messageBytes != nullptr && capBytes != nullptr; }

  inline bool hasRawBytes() const { return static_cast<bool>(rawBytes); }

  inline const uint8_t* messageData() const {
    if (messageBytes != nullptr) {
      return reinterpret_cast<const uint8_t*>(lean_sarray_cptr(messageBytes));
    }
    if (rawBytes) {
      return rawBytes->messageData();
    }
    return nullptr;
  }

  inline size_t messageSize() const {
    if (messageBytes != nullptr) {
      return lean_sarray_size(messageBytes);
    }
    if (rawBytes) {
      return rawBytes->messageSize();
    }
    return 0;
  }

  inline const uint8_t* capData() const {
    if (capBytes != nullptr) {
      return reinterpret_cast<const uint8_t*>(lean_sarray_cptr(capBytes));
    }
    if (rawBytes) {
      return rawBytes->capData();
    }
    return nullptr;
  }

  inline size_t capSize() const {
    if (capBytes != nullptr) {
      return lean_sarray_size(capBytes);
    }
    if (rawBytes) {
      return rawBytes->capSize();
    }
    return 0;
  }

  inline std::shared_ptr<const void> rawOwner() const {
    return std::static_pointer_cast<const void>(rawBytes);
  }

  lean_object* takeMessageBytes() {
    auto* out = messageBytes;
    messageBytes = nullptr;
    return out;
  }

  lean_object* takeCapBytes() {
    auto* out = capBytes;
    capBytes = nullptr;
    return out;
  }
};

uint32_t registerRuntimePayloadRef(uint64_t runtimeId, lean_object* messageBytes,
                                   lean_object* capBytes, bool retainInputs);
uint32_t registerRuntimePayloadRefFromRawCallResult(
    uint64_t runtimeId, capnp_lean_rpc::RawCallResult&& result);
kj::Maybe<RetainedRuntimePayloadRef> retainRuntimePayloadRef(uint32_t payloadRefId);
bool releaseRuntimePayloadRef(uint64_t runtimeId, uint32_t payloadRefId,
                              std::string& errorOut);
uint64_t releaseRuntimePayloadRefsForRuntime(uint64_t runtimeId);

}  // namespace capnp_lean_rpc_payload_ref
