#include "MessageStore.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

#include <sys/time.h>

#include <hiredis/hiredis.h>

namespace {

// 返回当前毫秒时间戳，用作消息生命周期字段和 Redis zset score。
uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count());
}

// 构造发送侧幂等索引的业务 key。
std::string MakeClientMsgKey(uint64_t from_uid,
                             const std::string& client_msg_id) {
    return std::to_string(from_uid) + ":" + client_msg_id;
}

// 构造消息 hash 的 Redis key。
std::string MakeMessageKey(uint64_t msg_id) {
    return "rmd:msg:" + std::to_string(msg_id);
}

// 构造接收方 Pending 有序集合的 Redis key。
std::string MakePendingKey(uint64_t uid) {
    return "rmd:pending:" + std::to_string(uid);
}

// 幂等 key 加上固定前缀，避免和其他 Redis 数据结构冲突。
std::string MakeIdempotencyKey(uint64_t from_uid,
                               const std::string& client_msg_id) {
    return "rmd:idem:" + MakeClientMsgKey(from_uid, client_msg_id);
}

const char kDefaultRedisHost[] = "127.0.0.1";
const int kDefaultRedisPort = 6379;
const char kRedisMsgNextIdKey[] = "rmd:msg:next_id";
const char kRedisMsgKeyPrefix[] = "rmd:msg:";
const char kRedisPendingKeyPrefix[] = "rmd:pending:";
const char kRedisDeliveredTimeoutKey[] = "rmd:delivered_timeout";

// 原子地完成幂等检查、消息 ID 分配、消息 hash 写入和 Pending 索引写入。
// 返回数组第一项表示是否新建：'1' 为新消息，'0' 为命中幂等记录。
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

// 标记 Delivered 时，同时移出 Pending 索引并加入超时扫描索引。
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

// 回到 Pending 时使用原 created_at_ms 作为 score，保持离线补发顺序稳定。
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

// ACK 后在 Redis 内校验接收方身份，匹配时才进入 Acked 终态。
const char kMarkAckedScript[] = R"lua(
local to_uid = redis.call('HGET', KEYS[1], 'to_uid')
if not to_uid then
    return 0
end
if to_uid ~= ARGV[2] then
    return 0
end
redis.call('HSET', KEYS[1],
    'status', 'Acked',
    'acked_at_ms', ARGV[3])
redis.call('ZREM', KEYS[2], ARGV[1])
redis.call('ZREM', ARGV[4] .. to_uid, ARGV[1])
return 1
)lua";

// Failed 也是终态，需要清理 Pending 和 Delivered 超时索引。
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

// 重试计数独立递增，便于投递线程在重试前更新统计。
const char kIncrementRetryScript[] = R"lua(
if redis.call('EXISTS', KEYS[1]) == 0 then
    return 0
end
redis.call('HINCRBY', KEYS[1], 'retry_count', 1)
return 1
)lua";

// hiredis 的 redisReply 需要显式释放，这里用 unique_ptr 托管生命周期。
struct RedisReplyDeleter {
    void operator()(redisReply* reply) const {
        if (reply != nullptr) {
            freeReplyObject(reply);
        }
    }
};

using RedisReplyPtr = std::unique_ptr<redisReply, RedisReplyDeleter>;

const char* RedisHost() {
    const char* host = std::getenv("RMD_REDIS_HOST");
    return (host != nullptr && host[0] != '\0') ? host : kDefaultRedisHost;
}

int RedisPort() {
    const char* text = std::getenv("RMD_REDIS_PORT");
    if (text == nullptr || text[0] == '\0') {
        return kDefaultRedisPort;
    }

    char* end = nullptr;
    long port = std::strtol(text, &end, 10);
    if (end == nullptr || *end != '\0' || port <= 0 || port > 65535) {
        return kDefaultRedisPort;
    }

    return static_cast<int>(port);
}

// 建立到 Redis 的同步连接。失败时返回 nullptr，由调用方降级为操作失败。
redisContext* ConnectRedis() {
    // 避免 Redis 不可用时业务线程无限期阻塞。
    timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 500000;

    redisContext* context = redisConnectWithTimeout(RedisHost(),
                                                    RedisPort(),
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

// hiredis 可能把数字以 INTEGER 或 STRING 返回，统一转成字符串后再解析。
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

// Redis hash 中的数字字段按字符串协议返回，解析失败代表记录格式异常。
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

// retry_count 当前使用 int，保持和 MessageRecord 字段类型一致。
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

// Redis 中状态以字符串保存，这里集中做协议文本到枚举的转换。
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

// 按固定字段顺序把 HMGET/EVAL 返回值组装成 MessageRecord。
// offset 用来跳过 create 脚本返回数组中的 created 标记位。
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

// create 脚本返回 created 标记和完整记录，业务层据此区分新建/重复消息。
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

// 查询单条消息。字段顺序必须和 BuildRecordFromFields() 保持一致。
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

// 状态脚本约定返回整数 1 表示实际更新成功。
bool ReplyIsTrue(const redisReply* reply) {
    return reply != nullptr &&
           reply->type == REDIS_REPLY_INTEGER &&
           reply->integer == 1;
}

// 执行创建消息脚本，把跨 key 的幂等、写入和索引维护放在 Redis 内原子完成。
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

// 执行消息状态转换脚本。几个状态脚本共享同一套 key/argv 布局。
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

// 执行 ACK 状态脚本，额外传入 ack_uid 让 Redis 原子校验接收方身份。
RedisReplyPtr EvalMarkAckedScript(redisContext* context,
                                  uint64_t msg_id,
                                  uint64_t ack_uid,
                                  uint64_t timestamp_ms) {
    std::string msg_key = MakeMessageKey(msg_id);
    return RedisReplyPtr(static_cast<redisReply*>(redisCommand(
        context,
        "EVAL %b 2 %b %b %llu %llu %llu %b",
        kMarkAckedScript,
        sizeof(kMarkAckedScript) - 1,
        msg_key.data(),
        msg_key.size(),
        kRedisDeliveredTimeoutKey,
        sizeof(kRedisDeliveredTimeoutKey) - 1,
        static_cast<unsigned long long>(msg_id),
        static_cast<unsigned long long>(ack_uid),
        static_cast<unsigned long long>(timestamp_ms),
        kRedisPendingKeyPrefix,
        sizeof(kRedisPendingKeyPrefix) - 1)));
}

// nullptr reply 通常意味着连接已断开；context->err 非 0 表示连接进入错误态。
bool IsConnectionBroken(const redisContext* context, const redisReply* reply) {
    return context == nullptr || context->err != 0 || reply == nullptr;
}

}  // namespace

MessageStore::MessageStore() {
}

// 析构时持锁关闭连接，避免其他线程仍在使用 redis_。
MessageStore::~MessageStore() {
    std::lock_guard<std::mutex> lock(redis_mutex_);
    closeConnectionLocked();
}

// 调用方已持有 redis_mutex_；连接不存在或错误时重新建立连接。
bool MessageStore::ensureConnectedLocked() {
    if (redis_ != nullptr && redis_->err == 0) {
        return true;
    }

    closeConnectionLocked();
    redis_ = ConnectRedis();
    return redis_ != nullptr;
}

// 调用方已持有 redis_mutex_；关闭连接后把指针置空，便于后续重连。
void MessageStore::closeConnectionLocked() {
    if (redis_ != nullptr) {
        redisFree(redis_);
        redis_ = nullptr;
    }
}

// 创建消息或返回幂等命中的已有消息。失败时 result.ok 保持 false。
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

    // 幂等 key 和接收方 Pending 索引 key 由业务字段稳定生成。
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
    // Redis 返回错误或记录字段格式不符合预期时，统一视为创建失败。
    if (reply->type == REDIS_REPLY_ERROR ||
        !BuildCreateResult(reply.get(), &result)) {
        return CreateMessageResult();
    }

    return result;
}

// 标记消息已投递给接收方，并加入 Delivered 超时扫描索引。
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

// 目标离线或连接不可用时，将消息重新放回 Pending 索引。
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

// 接收方确认消费后，消息进入 Acked 状态并从重投相关索引中移除。
bool MessageStore::markAcked(uint64_t msg_id, uint64_t ack_uid) {
    std::lock_guard<std::mutex> lock(redis_mutex_);
    if (!ensureConnectedLocked()) {
        return false;
    }

    RedisReplyPtr reply = EvalMarkAckedScript(redis_,
                                              msg_id,
                                              ack_uid,
                                              NowMs());
    if (IsConnectionBroken(redis_, reply.get())) {
        closeConnectionLocked();
        return false;
    }
    return ReplyIsTrue(reply.get());
}

// 投递失败是终态，同样需要清理 Pending 和 Delivered 超时索引。
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

// 按 msg_id 读取完整消息记录。引用参数只在返回 true 时可用。
bool MessageStore::getMessage(uint64_t msg_id, MessageRecord& out) {
    std::lock_guard<std::mutex> lock(redis_mutex_);
    if (!ensureConnectedLocked()) {
        return false;
    }

    bool ok = FetchRecord(redis_, msg_id, &out);
    if (redis_ != nullptr && redis_->err != 0) {
        closeConnectionLocked();
    }
    return ok;
}

// 增加消息重试次数；消息不存在时脚本返回 0。
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

// 查询某个接收方所有 Pending 消息。当前实现先取索引，再逐条校验记录。
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
        // 再次校验 to_uid/status，避免索引残留或状态变更导致补发错误消息。
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

// 查询 Delivered 且 ACK 等待超时的消息，用于后台重投线程。
std::vector<MessageRecord> MessageStore::getTimeoutDeliveredMessages(
    uint64_t now_ms,
    uint64_t timeout_ms) {
    std::vector<MessageRecord> result;
    std::lock_guard<std::mutex> lock(redis_mutex_);
    if (!ensureConnectedLocked()) {
        return result;
    }

    uint64_t max_score = now_ms >= timeout_ms ? now_ms - timeout_ms : 0;
    // delivered_at_ms 作为 zset score，所以 0..max_score 即超时候选集合。
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
        // 候选集合只是索引结果，最终仍以消息 hash 中的状态和时间为准。
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
