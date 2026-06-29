#ifndef MESSAGE_STORE_H
#define MESSAGE_STORE_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct redisContext;

// MessageStatus 表示服务端视角下一条消息的可靠投递状态。
// 后续 ACK、重试和离线补偿都会围绕这个状态流转。
enum class MessageStatus {
    Pending,     // 已创建，等待投递
    Delivered,   // 已推送给接收方，但还没收到接收方 ACK
    Acked,       // 接收方已确认消费
    Failed       // 多次重试失败
};

// MessageRecord 是一条消息在服务端的账本记录。
// 它不表示网络包本身，而表示“服务端是否已经创建、投递、确认或失败”。
struct MessageRecord {
    // 服务端分配的全局消息 ID，用于 ACK、重试、去重和追踪。
    uint64_t msg_id = 0;

    // 发送方和接收方用户 ID。
    uint64_t from_uid = 0;
    uint64_t to_uid = 0;

    // 第一版只存文本内容；client_msg_id 用于后续幂等去重。
    std::string content;
    std::string client_msg_id;

    // 当前消息状态，初始为 Pending。
    MessageStatus status = MessageStatus::Pending;

    // 生命周期时间戳，单位为毫秒。
    uint64_t created_at_ms = 0;
    uint64_t delivered_at_ms = 0;
    uint64_t acked_at_ms = 0;

    // 重试次数，后续超时重投时递增。
    int retry_count = 0;
};

// CreateMessageResult 表示一次 createMessage() 调用的结果。
// ok 表示 Redis 写入/读取是否成功；created 用来区分新消息和幂等命中。
struct CreateMessageResult {
    MessageRecord record;
    bool created = false;
    bool ok = false;
};

// MessageStore 是 Redis 版消息存储。
// 它负责消息账本、发送侧幂等索引、Pending 索引、最近消息索引和 Delivered 超时索引。
class MessageStore {
public:
    MessageStore();
    ~MessageStore();

    MessageStore(const MessageStore&) = delete;
    MessageStore& operator=(const MessageStore&) = delete;

    // 创建一条消息记录，生成 msg_id，并以 Pending 状态写入 Redis。
    // 如果 from_uid + client_msg_id 已存在，则返回已有记录，不重复创建。
    CreateMessageResult createMessage(
        uint64_t from_uid,
        uint64_t to_uid,
        const std::string& content,
        const std::string& client_msg_id);

    // 标记消息已推送给接收方，但还没收到接收方 ACK。
    bool markDelivered(uint64_t msg_id);

    // 标记消息等待投递。目标用户离线或连接不可用时会回到 Pending。
    bool markPending(uint64_t msg_id);

    // 标记消息已被接收方确认消费；ack_uid 必须匹配消息接收方。
    bool markAcked(uint64_t msg_id, uint64_t ack_uid);

    // 标记消息投递失败。
    bool markFailed(uint64_t msg_id);

    // 按 msg_id 查询消息记录；不存在时返回 false。
    bool getMessage(uint64_t msg_id, MessageRecord& out);

    // 增加消息重试次数。
    bool incrementRetryCount(uint64_t msg_id);

    // 查询指定用户当前等待投递的消息。
    std::vector<MessageRecord> getPendingMessages(uint64_t to_uid);

    // 查询指定用户最近的消息，结果按创建时间从旧到新返回。
    std::vector<MessageRecord> getRecentMessages(uint64_t uid,
                                                 std::size_t limit);

    // 查询 Delivered 但超过指定 ACK 等待时间的消息。
    std::vector<MessageRecord> getTimeoutDeliveredMessages(
        uint64_t now_ms,
        uint64_t timeout_ms);

private:
    // 调用方必须已经持有 redis_mutex_。
    bool ensureConnectedLocked();

    // 调用方必须已经持有 redis_mutex_。
    void closeConnectionLocked();

    redisContext* redis_ = nullptr;
    std::mutex redis_mutex_;
};

#endif
