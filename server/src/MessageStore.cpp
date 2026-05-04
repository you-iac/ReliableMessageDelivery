#include "MessageStore.h"

#include <chrono>

namespace {

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count());
}

}  // namespace

MessageRecord MessageStore::createMessage(
    uint64_t from_uid,
    uint64_t to_uid,
    const std::string& content,
    const std::string& client_msg_id) {
    MessageRecord record;
    record.msg_id = next_msg_id_.fetch_add(1);
    record.from_uid = from_uid;
    record.to_uid = to_uid;
    record.content = content;
    record.client_msg_id = client_msg_id;
    record.status = MessageStatus::Pending;
    record.created_at_ms = NowMs();

    std::lock_guard<std::mutex> lock(mutex_);
    message_pool_[record.msg_id] = record;
    return record;
}

bool MessageStore::markDelivered(uint64_t msg_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = message_pool_.find(msg_id);
    if (it == message_pool_.end()) {
        return false;
    }

    it->second.status = MessageStatus::Delivered;
    it->second.delivered_at_ms = NowMs();
    return true;
}

bool MessageStore::markAcked(uint64_t msg_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = message_pool_.find(msg_id);
    if (it == message_pool_.end()) {
        return false;
    }

    it->second.status = MessageStatus::Acked;
    it->second.acked_at_ms = NowMs();
    return true;
}

bool MessageStore::markFailed(uint64_t msg_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = message_pool_.find(msg_id);
    if (it == message_pool_.end()) {
        return false;
    }

    it->second.status = MessageStatus::Failed;
    return true;
}

bool MessageStore::getMessage(uint64_t msg_id, MessageRecord* out) {
    if (out == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = message_pool_.find(msg_id);
    if (it == message_pool_.end()) {
        return false;
    }

    *out = it->second;
    return true;
}

std::vector<MessageRecord> MessageStore::getPendingMessages(uint64_t to_uid) {
    std::vector<MessageRecord> result;
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& item : message_pool_) {
        const MessageRecord& record = item.second;
        if (record.to_uid == to_uid &&
            record.status == MessageStatus::Pending) {
            result.push_back(record);
        }
    }

    return result;
}
