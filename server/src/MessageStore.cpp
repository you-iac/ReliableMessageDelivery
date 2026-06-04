#include "MessageStore.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

#include <sys/time.h>

#include <hiredis/hiredis.h>

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

std::string MakeMessageKey(uint64_t msg_id) {
    return "rmd:msg:" + std::to_string(msg_id);
}

std::string MakePendingKey(uint64_t uid) {
    return "rmd:pending:" + std::to_string(uid);
}

std::string MakeIdempotencyKey(uint64_t from_uid,
                               const std::string& client_msg_id) {
    return "rmd:idem:" + MakeClientMsgKey(from_uid, client_msg_id);
}

const char kRedisHost[] = "127.0.0.1";
const int kRedisPort = 6379;
const char kRedisMsgNextIdKey[] = "rmd:msg:next_id";
const char kRedisMsgKeyPrefix[] = "rmd:msg:";
const char kRedisPendingKeyPrefix[] = "rmd:pending:";
const char kRedisDeliveredTimeoutKey[] = "rmd:delivered_timeout";

const char kCreateMessageScript[] = R"lua(
local msg_id = redis.call('GET', KEYS[1])
if msg_id then
    local msg_key = ARGV[1] .. msg_id
    if redis.call('EXISTS', msg_key) == 1 then
        local fields = redis.call('HMGET', msg_key,
            'msg_id', 'from_uid', 'to_uid', 'content', 'client_msg_id',
            'status', 'created_at_ms', 'delivered_at_ms', 'acked_at_ms',
            'retry_count')
        table.insert(fields, 1, '0')
        return fields
    end
end

msg_id = redis.call('INCR', KEYS[2])
local msg_key = ARGV[1] .. msg_id
redis.call('SET', KEYS[1], msg_id)
redis.call('HSET', msg_key,
    'msg_id', msg_id,
    'from_uid', ARGV[2],
    'to_uid', ARGV[3],
    'content', ARGV[4],
    'client_msg_id', ARGV[5],
    'status', 'Pending',
    'created_at_ms', ARGV[6],
    'delivered_at_ms', '0',
    'acked_at_ms', '0',
    'retry_count', '0')
redis.call('ZADD', KEYS[3], ARGV[6], msg_id)

return {'1', tostring(msg_id), ARGV[2], ARGV[3], ARGV[4], ARGV[5],
        'Pending', ARGV[6], '0', '0', '0'}
)lua";

const char kMarkDeliveredScript[] = R"lua(
local to_uid = redis.call('HGET', KEYS[1], 'to_uid')
if not to_uid then
    return 0
end
redis.call('HSET', KEYS[1],
    'status', 'Delivered',
    'delivered_at_ms', ARGV[2])
redis.call('ZREM', ARGV[3] .. to_uid, ARGV[1])
redis.call('ZADD', KEYS[2], ARGV[2], ARGV[1])
return 1
)lua";

const char kMarkPendingScript[] = R"lua(
local to_uid = redis.call('HGET', KEYS[1], 'to_uid')
if not to_uid then
    return 0
end
local created_at_ms = redis.call('HGET', KEYS[1], 'created_at_ms') or ARGV[2]
redis.call('HSET', KEYS[1], 'status', 'Pending')
redis.call('ZREM', KEYS[2], ARGV[1])
redis.call('ZADD', ARGV[3] .. to_uid, created_at_ms, ARGV[1])
return 1
)lua";

const char kMarkAckedScript[] = R"lua(
local to_uid = redis.call('HGET', KEYS[1], 'to_uid')
if not to_uid then
    return 0
end
redis.call('HSET', KEYS[1],
    'status', 'Acked',
    'acked_at_ms', ARGV[2])
redis.call('ZREM', KEYS[2], ARGV[1])
redis.call('ZREM', ARGV[3] .. to_uid, ARGV[1])
return 1
)lua";

const char kMarkFailedScript[] = R"lua(
local to_uid = redis.call('HGET', KEYS[1], 'to_uid')
if not to_uid then
    return 0
end
redis.call('HSET', KEYS[1], 'status', 'Failed')
redis.call('ZREM', KEYS[2], ARGV[1])
redis.call('ZREM', ARGV[3] .. to_uid, ARGV[1])
return 1
)lua";

const char kIncrementRetryScript[] = R"lua(
if redis.call('EXISTS', KEYS[1]) == 0 then
    return 0
end
redis.call('HINCRBY', KEYS[1], 'retry_count', 1)
return 1
)lua";

struct RedisReplyDeleter {
    void operator()(redisReply* reply) const {
        if (reply != nullptr) {
            freeReplyObject(reply);
        }
    }
};

using RedisReplyPtr = std::unique_ptr<redisReply, RedisReplyDeleter>;

redisContext* ConnectRedis() {
    timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 500000;

    redisContext* context = redisConnectWithTimeout(kRedisHost,
                                                    kRedisPort,
                                                    timeout);
    if (context == nullptr) {
        return nullptr;
    }
    if (context->err != 0) {
        redisFree(context);
        return nullptr;
    }
    return context;
}

bool ReplyToString(const redisReply* reply, std::string* out) {
    if (reply == nullptr || out == nullptr) {
        return false;
    }

    if (reply->type == REDIS_REPLY_STRING ||
        reply->type == REDIS_REPLY_STATUS) {
        out->assign(reply->str, reply->len);
        return true;
    }

    if (reply->type == REDIS_REPLY_INTEGER) {
        *out = std::to_string(reply->integer);
        return true;
    }

    return false;
}

bool ParseUint64(const redisReply* reply, uint64_t* out) {
    std::string text;
    if (!ReplyToString(reply, &text) || text.empty()) {
        return false;
    }

    char* end = nullptr;
    unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }

    *out = static_cast<uint64_t>(value);
    return true;
}

bool ParseInt(const redisReply* reply, int* out) {
    std::string text;
    if (!ReplyToString(reply, &text) || text.empty()) {
        return false;
    }

    char* end = nullptr;
    long value = std::strtol(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }

    *out = static_cast<int>(value);
    return true;
}

bool ParseStatus(const redisReply* reply, MessageStatus* out) {
    std::string text;
    if (!ReplyToString(reply, &text)) {
        return false;
    }

    if (text == "Pending") {
        *out = MessageStatus::Pending;
        return true;
    }
    if (text == "Delivered") {
        *out = MessageStatus::Delivered;
        return true;
    }
    if (text == "Acked") {
        *out = MessageStatus::Acked;
        return true;
    }
    if (text == "Failed") {
        *out = MessageStatus::Failed;
        return true;
    }

    return false;
}

bool BuildRecordFromFields(const redisReply* reply,
                           std::size_t offset,
                           MessageRecord* record) {
    if (reply == nullptr || record == nullptr ||
        reply->type != REDIS_REPLY_ARRAY || reply->elements < offset + 10) {
        return false;
    }

    return ParseUint64(reply->element[offset + 0], &record->msg_id) &&
           ParseUint64(reply->element[offset + 1], &record->from_uid) &&
           ParseUint64(reply->element[offset + 2], &record->to_uid) &&
           ReplyToString(reply->element[offset + 3], &record->content) &&
           ReplyToString(reply->element[offset + 4], &record->client_msg_id) &&
           ParseStatus(reply->element[offset + 5], &record->status) &&
           ParseUint64(reply->element[offset + 6], &record->created_at_ms) &&
           ParseUint64(reply->element[offset + 7], &record->delivered_at_ms) &&
           ParseUint64(reply->element[offset + 8], &record->acked_at_ms) &&
           ParseInt(reply->element[offset + 9], &record->retry_count);
}

bool BuildCreateResult(const redisReply* reply, CreateMessageResult* result) {
    if (reply == nullptr || result == nullptr ||
        reply->type != REDIS_REPLY_ARRAY || reply->elements != 11) {
        return false;
    }

    std::string created;
    if (!ReplyToString(reply->element[0], &created) ||
        !BuildRecordFromFields(reply, 1, &result->record)) {
        return false;
    }

    result->created = created == "1";
    result->ok = true;
    return true;
}

bool FetchRecord(redisContext* context, uint64_t msg_id, MessageRecord* out) {
    if (context == nullptr || out == nullptr || msg_id == 0) {
        return false;
    }

    std::string msg_key = MakeMessageKey(msg_id);
    RedisReplyPtr reply(static_cast<redisReply*>(redisCommand(
        context,
        "HMGET %b msg_id from_uid to_uid content client_msg_id status "
        "created_at_ms delivered_at_ms acked_at_ms retry_count",
        msg_key.data(),
        msg_key.size())));

    if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements != 10) {
        return false;
    }

    return BuildRecordFromFields(reply.get(), 0, out);
}

bool ReplyIsTrue(const redisReply* reply) {
    return reply != nullptr &&
           reply->type == REDIS_REPLY_INTEGER &&
           reply->integer == 1;
}

RedisReplyPtr EvalCreateMessage(redisContext* context,
                                const std::string& idem_key,
                                const std::string& pending_key,
                                uint64_t from_uid,
                                uint64_t to_uid,
                                const std::string& content,
                                const std::string& client_msg_id,
                                uint64_t created_at_ms) {
    return RedisReplyPtr(static_cast<redisReply*>(redisCommand(
        context,
        "EVAL %b 3 %b %b %b %b %llu %llu %b %b %llu",
        kCreateMessageScript,
        sizeof(kCreateMessageScript) - 1,
        idem_key.data(),
        idem_key.size(),
        kRedisMsgNextIdKey,
        sizeof(kRedisMsgNextIdKey) - 1,
        pending_key.data(),
        pending_key.size(),
        kRedisMsgKeyPrefix,
        sizeof(kRedisMsgKeyPrefix) - 1,
        static_cast<unsigned long long>(from_uid),
        static_cast<unsigned long long>(to_uid),
        content.data(),
        content.size(),
        client_msg_id.data(),
        client_msg_id.size(),
        static_cast<unsigned long long>(created_at_ms))));
}

RedisReplyPtr EvalMsgStateScript(redisContext* context,
                                 const char* script,
                                 std::size_t script_len,
                                 uint64_t msg_id,
                                 uint64_t timestamp_ms) {
    std::string msg_key = MakeMessageKey(msg_id);
    return RedisReplyPtr(static_cast<redisReply*>(redisCommand(
        context,
        "EVAL %b 2 %b %b %llu %llu %b",
        script,
        script_len,
        msg_key.data(),
        msg_key.size(),
        kRedisDeliveredTimeoutKey,
        sizeof(kRedisDeliveredTimeoutKey) - 1,
        static_cast<unsigned long long>(msg_id),
        static_cast<unsigned long long>(timestamp_ms),
        kRedisPendingKeyPrefix,
        sizeof(kRedisPendingKeyPrefix) - 1)));
}

bool IsConnectionBroken(const redisContext* context, const redisReply* reply) {
    return context == nullptr || context->err != 0 || reply == nullptr;
}

}  // namespace

MessageStore::MessageStore() {
}

MessageStore::~MessageStore() {
    std::lock_guard<std::mutex> lock(redis_mutex_);
    closeConnectionLocked();
}

bool MessageStore::ensureConnectedLocked() {
    if (redis_ != nullptr && redis_->err == 0) {
        return true;
    }

    closeConnectionLocked();
    redis_ = ConnectRedis();
    return redis_ != nullptr;
}

void MessageStore::closeConnectionLocked() {
    if (redis_ != nullptr) {
        redisFree(redis_);
        redis_ = nullptr;
    }
}

CreateMessageResult MessageStore::createMessage(
    uint64_t from_uid,
    uint64_t to_uid,
    const std::string& content,
    const std::string& client_msg_id) {
    CreateMessageResult result;
    std::lock_guard<std::mutex> lock(redis_mutex_);
    if (!ensureConnectedLocked()) {
        return result;
    }

    std::string idem_key = MakeIdempotencyKey(from_uid, client_msg_id);
    std::string pending_key = MakePendingKey(to_uid);
    RedisReplyPtr reply = EvalCreateMessage(redis_,
                                            idem_key,
                                            pending_key,
                                            from_uid,
                                            to_uid,
                                            content,
                                            client_msg_id,
                                            NowMs());
    if (IsConnectionBroken(redis_, reply.get())) {
        closeConnectionLocked();
        return result;
    }
    if (reply->type == REDIS_REPLY_ERROR ||
        !BuildCreateResult(reply.get(), &result)) {
        return CreateMessageResult();
    }

    return result;
}

bool MessageStore::markDelivered(uint64_t msg_id) {
    std::lock_guard<std::mutex> lock(redis_mutex_);
    if (!ensureConnectedLocked()) {
        return false;
    }

    RedisReplyPtr reply = EvalMsgStateScript(redis_,
                                             kMarkDeliveredScript,
                                             sizeof(kMarkDeliveredScript) - 1,
                                             msg_id,
                                             NowMs());
    if (IsConnectionBroken(redis_, reply.get())) {
        closeConnectionLocked();
        return false;
    }
    return ReplyIsTrue(reply.get());
}

bool MessageStore::markPending(uint64_t msg_id) {
    std::lock_guard<std::mutex> lock(redis_mutex_);
    if (!ensureConnectedLocked()) {
        return false;
    }

    RedisReplyPtr reply = EvalMsgStateScript(redis_,
                                             kMarkPendingScript,
                                             sizeof(kMarkPendingScript) - 1,
                                             msg_id,
                                             NowMs());
    if (IsConnectionBroken(redis_, reply.get())) {
        closeConnectionLocked();
        return false;
    }
    return ReplyIsTrue(reply.get());
}

bool MessageStore::markAcked(uint64_t msg_id) {
    std::lock_guard<std::mutex> lock(redis_mutex_);
    if (!ensureConnectedLocked()) {
        return false;
    }

    RedisReplyPtr reply = EvalMsgStateScript(redis_,
                                             kMarkAckedScript,
                                             sizeof(kMarkAckedScript) - 1,
                                             msg_id,
                                             NowMs());
    if (IsConnectionBroken(redis_, reply.get())) {
        closeConnectionLocked();
        return false;
    }
    return ReplyIsTrue(reply.get());
}

bool MessageStore::markFailed(uint64_t msg_id) {
    std::lock_guard<std::mutex> lock(redis_mutex_);
    if (!ensureConnectedLocked()) {
        return false;
    }

    RedisReplyPtr reply = EvalMsgStateScript(redis_,
                                             kMarkFailedScript,
                                             sizeof(kMarkFailedScript) - 1,
                                             msg_id,
                                             NowMs());
    if (IsConnectionBroken(redis_, reply.get())) {
        closeConnectionLocked();
        return false;
    }
    return ReplyIsTrue(reply.get());
}

bool MessageStore::getMessage(uint64_t msg_id, MessageRecord* out) {
    std::lock_guard<std::mutex> lock(redis_mutex_);
    if (!ensureConnectedLocked()) {
        return false;
    }

    bool ok = FetchRecord(redis_, msg_id, out);
    if (redis_ != nullptr && redis_->err != 0) {
        closeConnectionLocked();
    }
    return ok;
}

bool MessageStore::incrementRetryCount(uint64_t msg_id) {
    std::lock_guard<std::mutex> lock(redis_mutex_);
    if (!ensureConnectedLocked()) {
        return false;
    }

    std::string msg_key = MakeMessageKey(msg_id);
    RedisReplyPtr reply(static_cast<redisReply*>(redisCommand(
        redis_,
        "EVAL %b 1 %b",
        kIncrementRetryScript,
        sizeof(kIncrementRetryScript) - 1,
        msg_key.data(),
        msg_key.size())));
    if (IsConnectionBroken(redis_, reply.get())) {
        closeConnectionLocked();
        return false;
    }
    return ReplyIsTrue(reply.get());
}

std::vector<MessageRecord> MessageStore::getPendingMessages(uint64_t to_uid) {
    std::vector<MessageRecord> result;
    std::lock_guard<std::mutex> lock(redis_mutex_);
    if (!ensureConnectedLocked()) {
        return result;
    }

    std::string pending_key = MakePendingKey(to_uid);
    RedisReplyPtr reply(static_cast<redisReply*>(redisCommand(
        redis_,
        "ZRANGE %b 0 -1",
        pending_key.data(),
        pending_key.size())));

    if (IsConnectionBroken(redis_, reply.get())) {
        closeConnectionLocked();
        return result;
    }
    if (reply->type != REDIS_REPLY_ARRAY) {
        return result;
    }

    for (std::size_t i = 0; i < reply->elements; ++i) {
        uint64_t msg_id = 0;
        MessageRecord record;
        if (ParseUint64(reply->element[i], &msg_id) &&
            FetchRecord(redis_, msg_id, &record) &&
            record.to_uid == to_uid &&
            record.status == MessageStatus::Pending) {
            result.push_back(record);
        }
        if (redis_ != nullptr && redis_->err != 0) {
            closeConnectionLocked();
            break;
        }
    }

    return result;
}

std::vector<MessageRecord> MessageStore::getTimeoutDeliveredMessages(
    uint64_t now_ms,
    uint64_t timeout_ms) {
    std::vector<MessageRecord> result;
    std::lock_guard<std::mutex> lock(redis_mutex_);
    if (!ensureConnectedLocked()) {
        return result;
    }

    uint64_t max_score = now_ms >= timeout_ms ? now_ms - timeout_ms : 0;
    RedisReplyPtr reply(static_cast<redisReply*>(redisCommand(
        redis_,
        "ZRANGEBYSCORE %b 0 %llu",
        kRedisDeliveredTimeoutKey,
        sizeof(kRedisDeliveredTimeoutKey) - 1,
        static_cast<unsigned long long>(max_score))));

    if (IsConnectionBroken(redis_, reply.get())) {
        closeConnectionLocked();
        return result;
    }
    if (reply->type != REDIS_REPLY_ARRAY) {
        return result;
    }

    for (std::size_t i = 0; i < reply->elements; ++i) {
        uint64_t msg_id = 0;
        MessageRecord record;
        if (!ParseUint64(reply->element[i], &msg_id) ||
            !FetchRecord(redis_, msg_id, &record)) {
            if (redis_ != nullptr && redis_->err != 0) {
                closeConnectionLocked();
                break;
            }
            continue;
        }

        if (record.status == MessageStatus::Delivered &&
            record.delivered_at_ms > 0 &&
            now_ms >= record.delivered_at_ms &&
            now_ms - record.delivered_at_ms >= timeout_ms) {
            result.push_back(record);
        }
    }

    return result;
}
