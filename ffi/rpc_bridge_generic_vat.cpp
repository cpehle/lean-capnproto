#include "rpc_bridge_generic_vat.h"
#include <kj/debug.h>
#include <stdexcept>

namespace capnp_lean_rpc {

LeanVatId decodeLeanVatId(capnp_test::TestSturdyRefHostId::Reader data) {
  LeanVatId vatId;
  vatId.host = std::string(data.getHost().cStr());
  vatId.unique = data.getUnique();
  return vatId;
}

LeanVatId decodeLeanVatId(lean_object* data) {
  // Mirror `Capnp.Rpc.VatId` (see `lean/Capnp/Rpc.lean`).
  LeanVatId vatId;
  vatId.host = std::string(lean_string_cstr(lean_ctor_get(data, 0)));
  vatId.unique = lean_ctor_get_uint8(data, sizeof(void*)) != 0;
  return vatId;
}

void setLeanVatId(capnp_test::TestSturdyRefHostId::Builder builder, const LeanVatId& vatId) {
  builder.setHost(vatId.host.c_str());
  builder.setUnique(vatId.unique);
}

static void setThirdPartyToken(capnp_test::TestThirdPartyCompletion::Builder builder, uint64_t token) {
  builder.setToken(token);
}

static void setThirdPartyToken(capnp_test::TestThirdPartyToAwait::Builder builder, uint64_t token) {
  builder.setToken(token);
}

static uint64_t decodeThirdPartyToken(capnp_test::TestThirdPartyCompletion::Reader data) {
  return data.getToken();
}

static uint64_t decodeThirdPartyToken(capnp_test::TestThirdPartyToAwait::Reader data) {
  return data.getToken();
}

static LeanThirdPartyContact decodeThirdPartyContact(capnp_test::TestThirdPartyToContact::Reader data) {
  LeanThirdPartyContact contact;
  contact.path = decodeLeanVatId(data.getPath());
  contact.token = data.getToken();
  contact.sentBy = std::string(data.getSentBy().cStr());
  return contact;
}

static void setThirdPartyContact(capnp_test::TestThirdPartyToContact::Builder builder,
                                 const LeanThirdPartyContact& contact) {
  auto path = builder.initPath();
  setLeanVatId(path, contact.path);
  builder.setToken(contact.token);
  builder.setSentBy(contact.sentBy.c_str());
}

GenericVat::GenericVat(GenericVatNetwork& network, std::string name)
    : network_(network), name_(std::move(name)) {}

GenericVat::~GenericVat() {
  kj::Exception exception(kj::Exception::Type::FAILED, __FILE__, __LINE__,
                          kj::str("GenericVat network was destroyed."));
  for (auto& entry : connections_) {
    entry.second->disconnect(kj::cp(exception));
  }
}

GenericVat::ConnectionImpl::ConnectionImpl(GenericVat& vat, GenericVat& peerVat, bool unique)
    : vat_(vat),
      peerVat_(peerVat),
      unique_(unique),
      peerVatIdMessage_(8) {
  auto peerVatId = peerVatIdMessage_.initRoot<GenericVatId>();
  peerVatId.setHost(peerVat.name_.c_str());
  peerVatId.setUnique(unique);
  if (!unique_) {
    vat_.connections_[&peerVat_] = this;
  }
  tasks_ = kj::heap<kj::TaskSet>(*static_cast<kj::TaskSet::ErrorHandler*>(this));
}

GenericVat::ConnectionImpl::~ConnectionImpl() noexcept(false) {
  if (!unique_) {
    vat_.connections_.erase(&peerVat_);
  }
  KJ_IF_SOME(partner, partner_) {
    partner.partner_ = kj::none;
  }
}

void GenericVat::ConnectionImpl::attach(ConnectionImpl& other) {
  KJ_REQUIRE(partner_ == kj::none);
  KJ_REQUIRE(other.partner_ == kj::none);
  partner_ = other;
  partnerName_ = other.vat_.name_;
  other.partner_ = *this;
  other.partnerName_ = vat_.name_;
}

void GenericVat::ConnectionImpl::initiateIdleShutdown() {
  initiatedIdleShutdown_ = true;
  messageQueue_.push(kj::none);
  KJ_IF_SOME(f, fulfillOnEnd_) {
    f->fulfill();
  }
}

void GenericVat::ConnectionImpl::disconnect(kj::Exception&& exception) {
  messageQueue_.rejectAll(kj::cp(exception));
  networkException_ = kj::mv(exception);
  tasks_ = nullptr;
}

void GenericVat::ConnectionImpl::block() {
  auto paf = kj::newPromiseAndFulfiller<void>();
  currentBlock_ = paf.promise.fork();
  currentBlockFulfiller_ = kj::mv(paf.fulfiller);
}

void GenericVat::ConnectionImpl::unblock() {
  KJ_IF_SOME(f, currentBlockFulfiller_) {
    f->fulfill();
  }
  currentBlock_ = kj::none;
  currentBlockFulfiller_ = kj::none;
}

void GenericVat::ConnectionImpl::onSend(MessageHandler handler) {
  onSendHandler_ = kj::mv(handler);
}

void GenericVat::ConnectionImpl::recordProtocolMessage(::capnp::rpc::Message::Reader message) {
  switch (message.which()) {
    case ::capnp::rpc::Message::RESOLVE:
      ++protocolMessageCounts_.resolveCount;
      protocolMessageTrace_.push_back(static_cast<uint16_t>(1));
      break;
    case ::capnp::rpc::Message::DISEMBARGO:
      ++protocolMessageCounts_.disembargoCount;
      protocolMessageTrace_.push_back(static_cast<uint16_t>(2));
      break;
    default:
      break;
  }
}

GenericVatId::Reader GenericVat::ConnectionImpl::getPeerVatId() {
  return peerVatIdMessage_.getRoot<GenericVatId>().asReader();
}

class IncomingRpcMessageImpl final : public capnp::IncomingRpcMessage, public kj::Refcounted {
 public:
  explicit IncomingRpcMessageImpl(kj::Array<capnp::word> data)
      : data_(kj::mv(data)), message_(data_) {}

  capnp::AnyPointer::Reader getBody() override {
    return message_.getRoot<capnp::AnyPointer>();
  }

  size_t sizeInWords() override { return data_.size(); }

 private:
  kj::Array<capnp::word> data_;
  capnp::FlatArrayMessageReader message_;
};

class OutgoingRpcMessageImpl final : public capnp::OutgoingRpcMessage {
 public:
  OutgoingRpcMessageImpl(GenericVat::ConnectionImpl& connection, unsigned int firstSegmentWordSize)
      : connection_(connection),
        message_(firstSegmentWordSize == 0 ? 1024 : firstSegmentWordSize) {}

  capnp::AnyPointer::Builder getBody() override {
    return message_.getRoot<capnp::AnyPointer>();
  }

  void send() override {
    if (connection_.networkException_ != kj::none) {
      return;
    }

    {
      auto reader = message_.getRoot<::capnp::rpc::Message>().asReader();
      KJ_IF_SOME(handler, connection_.onSendHandler_) {
        if (!handler(reader)) {
          return;
        }
      }
      connection_.recordProtocolMessage(reader);
    }

    ++connection_.vat_.sent_;
    auto incoming = kj::heap<IncomingRpcMessageImpl>(capnp::messageToFlatArray(message_));
    auto* connectionPtr = &connection_;

    kj::Promise<void> promise = kj::READY_NOW;
    KJ_IF_SOME(block, connection_.currentBlock_) {
      promise = block.addBranch();
    } else {
      promise = kj::evalLater([]() {});
    }

    connection_.tasks_->add(
        promise.then([connectionPtr, incomingMsg = kj::mv(incoming)]() mutable {
          KJ_IF_SOME(partner, connectionPtr->partner_) {
            partner.messageQueue_.push(kj::Own<capnp::IncomingRpcMessage>(kj::mv(incomingMsg)));
          }
        }));
  }

  size_t sizeInWords() override { return message_.sizeInWords(); }

 private:
  GenericVat::ConnectionImpl& connection_;
  capnp::MallocMessageBuilder message_;
};

kj::Own<capnp::OutgoingRpcMessage> GenericVat::ConnectionImpl::newOutgoingMessage(unsigned int firstSegmentWordSize) {
  KJ_REQUIRE(!idle_);
  return kj::heap<OutgoingRpcMessageImpl>(*this, firstSegmentWordSize);
}

kj::Promise<kj::Maybe<kj::Own<capnp::IncomingRpcMessage>>> GenericVat::ConnectionImpl::receiveIncomingMessage() {
  KJ_IF_SOME(e, networkException_) {
    kj::throwFatalException(kj::cp(e));
  }
  if (initiatedIdleShutdown_) {
    co_return kj::none;
  }
  auto result = co_await messageQueue_.pop();
  if (result == kj::none) {
    KJ_IF_SOME(f, fulfillOnEnd_) {
      f->fulfill();
    }
  } else {
    ++vat_.received_;
  }
  co_return result;
}

kj::Promise<void> GenericVat::ConnectionImpl::shutdown() {
  KJ_IF_SOME(partner, partner_) {
    if (partner.initiatedIdleShutdown_) {
      return kj::READY_NOW;
    }
    return kj::evalLater([this]() -> kj::Promise<void> {
      KJ_IF_SOME(activePartner, partner_) {
        activePartner.messageQueue_.push(kj::none);
        auto paf = kj::newPromiseAndFulfiller<void>();
        activePartner.fulfillOnEnd_ = kj::mv(paf.fulfiller);
        return kj::mv(paf.promise);
      }
      return kj::READY_NOW;
    });
  }
  return kj::READY_NOW;
}

void GenericVat::ConnectionImpl::setIdle(bool idle) {
  KJ_REQUIRE(idle != idle_);
  idle_ = idle;
}

bool GenericVat::ConnectionImpl::canIntroduceTo(Connection& other) {
  (void)other;
  return true;
}

void GenericVat::ConnectionImpl::introduceTo(Connection& other, GenericThirdPartyToContact::Builder otherContactInfo,
                 GenericThirdPartyToAwait::Builder thisAwaitInfo) {
  auto token = vat_.network_.newToken();
  LeanThirdPartyContact contact;
  contact.path = LeanVatId{kj::downcast<ConnectionImpl>(other).partnerName_, false};
  contact.token = token;
  contact.sentBy = vat_.name_;
  setThirdPartyContact(otherContactInfo, contact);
  setThirdPartyToken(thisAwaitInfo, token);
}

kj::Maybe<kj::Own<GenericVat::Connection>> GenericVat::ConnectionImpl::connectToIntroduced(
    GenericThirdPartyToContact::Reader contact,
    GenericThirdPartyCompletion::Builder completion) {
  auto decoded = decodeThirdPartyContact(contact);
  KJ_REQUIRE(decoded.sentBy == partnerName_);
  setThirdPartyToken(completion, decoded.token);
  return vat_.connectByVatId(decoded.path);
}

bool GenericVat::ConnectionImpl::canForwardThirdPartyToContact(GenericThirdPartyToContact::Reader contact,
                                   Connection& destination) {
  (void)contact;
  (void)destination;
  if (!vat_.network_.forwardingEnabled()) {
    vat_.network_.incrementDeniedForwardCount();
  }
  return vat_.network_.forwardingEnabled();
}

void GenericVat::ConnectionImpl::forwardThirdPartyToContact(GenericThirdPartyToContact::Reader contact,
                                Connection& destination,
                                GenericThirdPartyToContact::Builder result) {
  (void)destination;
  KJ_REQUIRE(vat_.network_.forwardingEnabled());
  auto decoded = decodeThirdPartyContact(contact);
  KJ_REQUIRE(decoded.sentBy == partnerName_);
  vat_.network_.incrementForwardCount();
  decoded.sentBy = vat_.name_;
  setThirdPartyContact(result, decoded);
}

kj::Own<void> GenericVat::ConnectionImpl::awaitThirdParty(GenericThirdPartyToAwait::Reader party,
                              kj::Rc<kj::Refcounted> value) {
  auto token = decodeThirdPartyToken(party);
  auto& exchange = vat_.getThirdPartyExchange(token);
  exchange.fulfiller->fulfill(kj::mv(value));
  class TokenRelease final {
   public:
    TokenRelease(GenericVat& vat, uint64_t token) : vat_(vat), token_(token) {}
    ~TokenRelease() { vat_.eraseThirdPartyExchange(token_); }

   private:
    GenericVat& vat_;
    uint64_t token_;
  };
  return kj::heap<TokenRelease>(vat_, token);
}

kj::Promise<kj::Rc<kj::Refcounted>> GenericVat::ConnectionImpl::completeThirdParty(
    GenericThirdPartyCompletion::Reader completion) {
  auto token = decodeThirdPartyToken(completion);
  auto& exchange = vat_.getThirdPartyExchange(token);
  return exchange.promise.addBranch();
}

kj::Array<capnp::byte> GenericVat::ConnectionImpl::generateEmbargoId() {
  static uint32_t counter = 0;
  auto out = kj::heapArray<capnp::byte>(sizeof(counter));
  out.asPtr().copyFrom(kj::asBytes(counter));
  ++counter;
  return out;
}

void GenericVat::ConnectionImpl::taskFailed(kj::Exception&& exception) { (void)exception; }

kj::Maybe<kj::Own<GenericVat::Connection>> GenericVat::connect(GenericVatId::Reader hostId) {
  return connectByVatId(decodeLeanVatId(hostId));
}

kj::Promise<kj::Own<GenericVat::Connection>> GenericVat::accept() {
  return acceptQueue_.pop();
}

kj::Maybe<GenericVat::ConnectionImpl&> GenericVat::getConnectionTo(GenericVat& other) {
  auto it = connections_.find(&other);
  if (it == connections_.end()) {
    return kj::none;
  }
  return *it->second;
}

GenericVat::ThirdPartyExchange::ThirdPartyExchange(
    kj::PromiseFulfillerPair<kj::Rc<kj::Refcounted>> paf)
    : promise(paf.promise.fork()), fulfiller(kj::mv(paf.fulfiller)) {}

GenericVat::ThirdPartyExchange& GenericVat::getThirdPartyExchange(uint64_t token) {
  auto it = tphExchanges_.find(token);
  if (it != tphExchanges_.end()) {
    return *it->second;
  }
  auto exchange = std::make_unique<ThirdPartyExchange>();
  auto* ptr = exchange.get();
  tphExchanges_.emplace(token, std::move(exchange));
  return *ptr;
}

void GenericVat::eraseThirdPartyExchange(uint64_t token) {
  tphExchanges_.erase(token);
}

kj::Maybe<kj::Own<GenericVat::Connection>> GenericVat::connectByVatId(const LeanVatId& hostId) {
  if (hostId.host == name_) {
    return kj::none;
  }
  auto* destination = network_.find(hostId.host);
  if (destination == nullptr) {
    throw std::runtime_error("unknown vat host: " + hostId.host);
  }
  if (!hostId.unique) {
    auto existingIt = connections_.find(destination);
    if (existingIt != connections_.end()) {
      return kj::addRef(*existingIt->second);
    }
  }
  auto local = kj::refcounted<ConnectionImpl>(*this, *destination, hostId.unique);
  auto remote = kj::refcounted<ConnectionImpl>(*destination, *this, hostId.unique);
  local->attach(*remote);
  destination->acceptQueue_.push(kj::addRef(*remote));
  return kj::addRef(*local);
}

GenericVat& GenericVatNetwork::add(const std::string& name) {
  auto it = vats_.find(name);
  if (it != vats_.end()) {
    throw std::runtime_error("vat already exists: " + name);
  }
  auto vat = std::make_unique<GenericVat>(*this, name);
  auto* ptr = vat.get();
  vats_.emplace(name, std::move(vat));
  return *ptr;
}

GenericVat* GenericVatNetwork::find(const std::string& name) {
  auto it = vats_.find(name);
  if (it == vats_.end()) {
    return nullptr;
  }
  return it->second.get();
}

} // namespace capnp_lean_rpc
