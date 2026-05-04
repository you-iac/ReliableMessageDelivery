#ifndef MESSAGE_STORE_H
#define MESSAGE_STORE_H


#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <atomic>
#include <vector>

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

// MessageStore 是内存版消息存储。
//
// 第一阶段用 unordered_map 保存 MessageRecord，先跑通可靠投递状态流转；
// 后续接入 MySQL 时，可以保持接口基本不变，把内部存储替换成数据库。
class MessageStore {
public:
    // 创建一条消息记录，生成 msg_id，并以 Pending 状态写入内存池。
    MessageRecord createMessage(
        uint64_t from_uid,
        uint64_t to_uid,
        const std::string& content,
        const std::string& client_msg_id);

    // 标记消息已推送给接收方，但还没收到接收方 ACK。
    bool markDelivered(uint64_t msg_id);

    // 标记消息已被接收方确认消费。
    bool markAcked(uint64_t msg_id);

    // 标记消息投递失败。
    bool markFailed(uint64_t msg_id);

    // 按 msg_id 查询消息记录；out 为空或不存在时返回 false。
    bool getMessage(uint64_t msg_id, MessageRecord* out);

    // 查询指定用户当前等待投递的消息。
    std::vector<MessageRecord> getPendingMessages(uint64_t to_uid);

private:
    // message_pool_ 会被业务 worker、ACK 处理和后续重试线程共同访问。
    std::mutex mutex_;

    // 第一版用进程内自增 ID；持久化后可替换为数据库/雪花 ID 等方案。
    std::atomic<uint64_t> next_msg_id_{1};

    // msg_id -> MessageRecord。
    std::unordered_map<uint64_t, MessageRecord> message_pool_;
};

#endif
