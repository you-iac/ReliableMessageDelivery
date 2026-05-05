#include "MessageStore.h"

#include <chrono>

namespace {

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count());
}

std::string MakeClientMsgKey(uint64_t from_uid,
                             const std::string& client_msg_id) {
    return std::to_string(from_uid) + ":" + client_msg_id;
}

}  // namespace

CreateMessageResult MessageStore::createMessage(
    uint64_t from_uid,
    uint64_t to_uid,
    const std::string& content,
    const std::string& client_msg_id) {
    CreateMessageResult result;

    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = MakeClientMsgKey(from_uid, client_msg_id);
    auto index_it = client_msg_index_.find(key);
    if (index_it != client_msg_index_.end()) {
        auto record_it = message_pool_.find(index_it->second);
        if (record_it != message_pool_.end()) {
            result.record = record_it->second;
            result.created = false;
            return result;
        }
    }

    MessageRecord record;
    record.msg_id = next_msg_id_.fetch_add(1);
    record.from_uid = from_uid;
    record.to_uid = to_uid;
    record.content = content;
    record.client_msg_id = client_msg_id;
    record.status = MessageStatus::Pending;
    record.created_at_ms = NowMs();

    message_pool_[record.msg_id] = record;
    client_msg_index_[key] = record.msg_id;

    result.record = record;
    result.created = true;
    return result;
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

bool MessageStore::markPending(uint64_t msg_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = message_pool_.find(msg_id);
    if (it == message_pool_.end()) {
        return false;
    }

    it->second.status = MessageStatus::Pending;
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

bool MessageStore::incrementRetryCount(uint64_t msg_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = message_pool_.find(msg_id);
    if (it == message_pool_.end()) {
        return false;
    }

    ++it->second.retry_count;
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

std::vector<MessageRecord> MessageStore::getTimeoutDeliveredMessages(
    uint64_t now_ms,
    uint64_t timeout_ms) {
    std::vector<MessageRecord> result;
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& item : message_pool_) {
        const MessageRecord& record = item.second;
        if (record.status == MessageStatus::Delivered &&
            record.delivered_at_ms > 0 &&
            now_ms >= record.delivered_at_ms &&
            now_ms - record.delivered_at_ms >= timeout_ms) {
            result.push_back(record);
        }
    }

    return result;
}
