#pragma once

#include "rpc_bridge_common.h"
#include <capnp/rpc.h>
#include <capnp/rpc.capnp.h>
#include <capnp/serialize.h>
#include <capnp/message.h>
#include <capnp/test.capnp.h>
#include <kj/async-queue.h>
#include <kj/async-io.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

namespace capnp_lean_rpc {

namespace capnp_test = ::capnproto_test::capnp::test;

struct LeanVatId {
  std::string host;
  bool unique = false;
};

struct LeanThirdPartyContact {
  LeanVatId path;
  uint64_t token = 0;
  std::string sentBy;
};

LeanVatId decodeLeanVatId(capnp_test::TestSturdyRefHostId::Reader data);
LeanVatId decodeLeanVatId(lean_object* data);
void setLeanVatId(capnp_test::TestSturdyRefHostId::Builder builder, const LeanVatId& vatId);

using GenericVatId = capnp_test::TestSturdyRefHostId;
using GenericThirdPartyCompletion = capnp_test::TestThirdPartyCompletion;
using GenericThirdPartyToAwait = capnp_test::TestThirdPartyToAwait;
using GenericThirdPartyToContact = capnp_test::TestThirdPartyToContact;
using GenericJoinResult = capnp_test::TestJoinResult;
using GenericVatNetworkBase = capnp::VatNetwork<GenericVatId, GenericThirdPartyCompletion,
                                                GenericThirdPartyToAwait,
                                                GenericThirdPartyToContact, GenericJoinResult>;
using GenericRpcSystem = capnp::RpcSystem<GenericVatId>;

class GenericVat;

class GenericVatNetwork {
 public:
  GenericVatNetwork() = default;
  ~GenericVatNetwork() = default;

  GenericVat& add(const std::string& name);
  GenericVat* find(const std::string& name);

  uint64_t newToken() { return ++tokenCounter_; }
  uint64_t tokenCount() const { return tokenCounter_; }

  bool forwardingEnabled() const { return forwardingEnabled_; }
  void setForwardingEnabled(bool enabled) { forwardingEnabled_ = enabled; }
  void resetForwardingStats() {
    forwardCount_ = 0;
    deniedForwardCount_ = 0;
  }

  uint64_t forwardCount() const { return forwardCount_; }
  uint64_t deniedForwardCount() const { return deniedForwardCount_; }
  void incrementForwardCount() { ++forwardCount_; }
  void incrementDeniedForwardCount() { ++deniedForwardCount_; }

 private:
  std::unordered_map<std::string, std::unique_ptr<GenericVat>> vats_;
  uint64_t tokenCounter_ = 0;
  bool forwardingEnabled_ = false;
  uint64_t forwardCount_ = 0;
  uint64_t deniedForwardCount_ = 0;
};

class GenericVat final : public GenericVatNetworkBase {
 public:
  using Connection = GenericVatNetworkBase::Connection;
  using ProtocolMessageCounts = capnp_lean_rpc::ProtocolMessageCounts;
  friend class ConnectionImpl;
  friend class IncomingRpcMessageImpl;
  friend class OutgoingRpcMessageImpl;

  GenericVat(GenericVatNetwork& network, std::string name);
  ~GenericVat();

  class ConnectionImpl final
      : public Connection, public kj::Refcounted, public kj::TaskSet::ErrorHandler {
   public:
    friend class IncomingRpcMessageImpl;
    friend class OutgoingRpcMessageImpl;
    ConnectionImpl(GenericVat& vat, GenericVat& peerVat, bool unique);
    ~ConnectionImpl() noexcept(false);

    void attach(ConnectionImpl& other);
    bool isIdle() const { return idle_; }
    void initiateIdleShutdown();
    void disconnect(kj::Exception&& exception);

    void block();
    void unblock();
    using MessageHandler = kj::Function<bool(::capnp::rpc::Message::Reader)>;
    void onSend(MessageHandler handler);
    ProtocolMessageCounts getProtocolMessageCounts() const { return protocolMessageCounts_; }
    void resetProtocolMessageCounts() { protocolMessageCounts_ = {}; }
    std::vector<uint16_t> getProtocolMessageTrace() const { return protocolMessageTrace_; }
    void resetProtocolMessageTrace() { protocolMessageTrace_.clear(); }

    GenericVatId::Reader getPeerVatId() override;
    kj::Own<capnp::OutgoingRpcMessage> newOutgoingMessage(unsigned int firstSegmentWordSize) override;
    kj::Promise<kj::Maybe<kj::Own<capnp::IncomingRpcMessage>>> receiveIncomingMessage() override;
    kj::Promise<void> shutdown() override;
    void setIdle(bool idle) override;
    bool canIntroduceTo(Connection& other) override;
    void introduceTo(Connection& other, GenericThirdPartyToContact::Builder otherContactInfo,
                     GenericThirdPartyToAwait::Builder thisAwaitInfo) override;
    kj::Maybe<kj::Own<Connection>> connectToIntroduced(
        GenericThirdPartyToContact::Reader contact,
        GenericThirdPartyCompletion::Builder completion) override;
    bool canForwardThirdPartyToContact(GenericThirdPartyToContact::Reader contact,
                                       Connection& destination) override;
    void forwardThirdPartyToContact(GenericThirdPartyToContact::Reader contact,
                                    Connection& destination,
                                    GenericThirdPartyToContact::Builder result) override;
    kj::Own<void> awaitThirdParty(GenericThirdPartyToAwait::Reader party,
                                  kj::Rc<kj::Refcounted> value) override;
    kj::Promise<kj::Rc<kj::Refcounted>> completeThirdParty(
        GenericThirdPartyCompletion::Reader completion) override;
    kj::Array<capnp::byte> generateEmbargoId() override;
    void taskFailed(kj::Exception&& exception) override;

   private:
    GenericVat& vat_;
    GenericVat& peerVat_;
    bool unique_;
    capnp::MallocMessageBuilder peerVatIdMessage_;
    kj::Maybe<ConnectionImpl&> partner_;
    std::string partnerName_;
    kj::Maybe<kj::Exception> networkException_;
    kj::ProducerConsumerQueue<kj::Maybe<kj::Own<capnp::IncomingRpcMessage>>> messageQueue_;
    kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> fulfillOnEnd_;
    bool idle_ = true;
    bool initiatedIdleShutdown_ = false;
    kj::Own<kj::TaskSet> tasks_;

    kj::Maybe<kj::ForkedPromise<void>> currentBlock_;
    kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> currentBlockFulfiller_;
    kj::Maybe<MessageHandler> onSendHandler_;
    ProtocolMessageCounts protocolMessageCounts_;
    std::vector<uint16_t> protocolMessageTrace_;

    void recordProtocolMessage(::capnp::rpc::Message::Reader message);
  };

  kj::Maybe<kj::Own<Connection>> connect(GenericVatId::Reader hostId) override;
  kj::Promise<kj::Own<Connection>> accept() override;
  kj::Maybe<ConnectionImpl&> getConnectionTo(GenericVat& other);
  const std::string& name() const { return name_; }

 private:
  friend class GenericVatNetwork;
  friend class ConnectionImpl;

  struct ThirdPartyExchange {
    kj::ForkedPromise<kj::Rc<kj::Refcounted>> promise;
    kj::Own<kj::PromiseFulfiller<kj::Rc<kj::Refcounted>>> fulfiller;

    ThirdPartyExchange(
        kj::PromiseFulfillerPair<kj::Rc<kj::Refcounted>> paf =
            kj::newPromiseAndFulfiller<kj::Rc<kj::Refcounted>>());
  };

  ThirdPartyExchange& getThirdPartyExchange(uint64_t token);
  void eraseThirdPartyExchange(uint64_t token);
  kj::Maybe<kj::Own<Connection>> connectByVatId(const LeanVatId& hostId);

  GenericVatNetwork& network_;
  std::string name_;
  uint64_t sent_ = 0;
  uint64_t received_ = 0;
  std::unordered_map<const GenericVat*, ConnectionImpl*> connections_;
  kj::ProducerConsumerQueue<kj::Own<Connection>> acceptQueue_;
  std::unordered_map<uint64_t, std::unique_ptr<ThirdPartyExchange>> tphExchanges_;
};

} // namespace capnp_lean_rpc
