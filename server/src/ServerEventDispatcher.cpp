#include "ServerEventDispatcher.h"

#include <muduo/base/Logging.h>

#include <chrono>

#include "Codec.h"
#include "EnvelopeFactory.h"
#include "EnvelopeInspector.h"

namespace {

const uint64_t kAckTimeoutMs = 10000;
const int kDeliveryScanIntervalMs = 1000;
const int kMaxDeliveryRetryCount = 3;
//获取当前时间
uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count());
}

}  // namespace

ServerEventDispatcher::ServerEventDispatcher() {
}

// 停止后台 worker，避免析构后线程继续访问对象。
ServerEventDispatcher::~ServerEventDispatcher() {
    stop();
}

// 启动业务 worker 线程。
void ServerEventDispatcher::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!stopped_) {
        return;
    }
    
    stopped_ = false;
    worker_ = std::thread(&ServerEventDispatcher::workerLoop, this);
    delivery_worker_ = std::thread(&ServerEventDispatcher::deliveryLoop, this);
}

// 停止 worker，并等待已入队事件处理完成。
void ServerEventDispatcher::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return;
        }

        stopped_ = true;
    }

    not_empty_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (delivery_worker_.joinable()) {
        delivery_worker_.join();
    }
}

// 将客户端 Envelope 事件投递到业务队列。
bool ServerEventDispatcher::enqueueEnvelope(
            const muduo::net::TcpConnectionPtr& conn,
            const message::Envelope& envelope) {
    // 网络线程只负责把已解码的 Envelope 入队，业务处理统一交给 worker。
    // 这样 ChatServer 不需要持有业务状态，也避免在 Muduo IO 线程里做耗时逻辑。
    ServerEvent event;
    event.type = ServerEvent::Type::Envelope;
    event.conn = conn;
    event.envelope = envelope;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return false;
        }


        inbox_.push(event);
    }

    not_empty_.notify_one();
    return true;
}

// 将连接断开事件投递到业务队列。
bool ServerEventDispatcher::enqueueConnectionClosed(
    const muduo::net::TcpConnectionPtr& conn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return false;
        }

        // 断开事件也进入同一个队列，保证同一连接上的 LOGIN_REQ/CHAT_REQ/断开按顺序生效。
        // 否则可能出现连接已断开但稍后处理 LOGIN_REQ 又把用户标记为在线的竞态。
        ServerEvent event;
        event.type = ServerEvent::Type::ConnectionClosed;
        event.conn = conn;
        inbox_.push(event);
    }
    
    not_empty_.notify_one();
    return true;
}

// worker 主循环：等待事件、取出事件、在锁外处理。
void ServerEventDispatcher::workerLoop() {
    while (true) {
        ServerEvent event;
        {
            //条件变量，等待队列非空或 stop() 被调用。
            std::unique_lock<std::mutex> lock(mutex_);
            not_empty_.wait(lock, [this] {
                return stopped_ || !inbox_.empty();
            });

            // stop() 只阻止新事件继续入队；已经入队的事件会先处理完再退出。
            if (stopped_ && inbox_.empty()) {
                return;
            }

            event = inbox_.front();
            inbox_.pop();
        }

        handle(event);
    }
}

// 根据事件类型和 Envelope 类型分发到具体处理函数。
void ServerEventDispatcher::handle(const ServerEvent& event) {
    if (event.type == ServerEvent::Type::ConnectionClosed) {
        handleConnectionClosed(event);
        return;
    }

    switch (event.envelope.type()) {
        case message::LOGIN_REQ:
            handleLogin(event);
            break;
        case message::CHAT_REQ:
            handleChat(event);
            break;
        case message::ACK:
            handleAck(event);
            break;
        case message::HEARTBEAT:
            handleHeartbeat(event);
            break;
        default: {
            sendErrorAck(event.conn,
                         event.envelope.seq(),
                         "unsupported message type");
            break;
        }
    }
}

// 处理登录请求，并把 uid 绑定到当前连接。
void ServerEventDispatcher::handleLogin(const ServerEvent& event) {
    // 登录只是把 uid 绑定到当前 TCP 连接；当前项目没有注册/密码校验。
    bool ok = event.envelope.has_login_req() &&
              event.envelope.login_req().uid() != 0;
    std::string reason;
    if (!ok) {
        reason = "invalid login request";
    } else if (!user_state_.bindUser(event.conn,
                                     event.envelope.login_req().uid())) {
        // bindUser 失败通常意味着 conn 无效或 uid 非法。
        // 当前实现里 bindUser 会自动创建/更新连接状态。
        ok = false;
        reason = "bind user failed";
    }

    // LOGIN_RESP 使用原始 seq，客户端可以通过 seq 对应自己的 LOGIN_REQ。
    message::Envelope response = EnvelopeFactory::CreateLoginResponse(
        event.envelope.seq(), ok, reason);
    sendEnvelope(event.conn, response);

    if (ok) {
        LOG_INFO << "User online: uid=" << event.envelope.login_req().uid();
        deliverPendingMessages(event.envelope.login_req().uid());
    }
}

// 处理聊天请求，校验发送方并做在线转发。
void ServerEventDispatcher::handleChat(const ServerEvent& event) {
    if (!event.envelope.has_chat_req()) {
        sendErrorAck(event.conn, event.envelope.seq(), "invalid chat request");
        return;
    }

    const message::ChatRequest& request = event.envelope.chat_req();

    // 发送聊天前必须先登录。getUidByConn() 返回 0 表示该连接尚未绑定有效 uid。
    uint64_t uid = user_state_.getUidByConn(event.conn);
    if (uid == 0) {
        sendErrorAck(event.conn, event.envelope.seq(), "please login first");
        return;
    }

    // from_uid 必须和连接登录身份一致，防止客户端伪造其他用户发送消息。
    if (request.from_uid() != uid) {
        sendErrorAck(event.conn,
                     event.envelope.seq(),
                     "from_uid does not match session");
        return;
    }

    CreateMessageResult create_result = message_store_.createMessage(
        request.from_uid(),
        request.to_uid(),
        request.content(),
        request.client_msg_id());
    if (!create_result.ok) {
        sendErrorAck(event.conn,
                     event.envelope.seq(),
                     "message store unavailable");
        return;
    }

    const MessageRecord& record = create_result.record;

    if (!create_result.created) {
        // 客户端超时重试会携带相同 client_msg_id。
        // 命中幂等索引时只返回原 msg_id 的 ACK，不重复创建和投递消息。
        sendEnvelope(event.conn, EnvelopeFactory::CreateAck(
            event.envelope.seq(), record.msg_id, true, "duplicate message"));
        return;
    }

    tryDeliverMessage(record);
    // 这里的 ACK 是给发送方的“服务端已接收/处理”确认，不是接收方消费确认。
    sendEnvelope(event.conn, EnvelopeFactory::CreateAck(
        event.envelope.seq(), record.msg_id, true, ""));
}

// 处理接收方 ACK，并更新消息消费状态。
void ServerEventDispatcher::handleAck(const ServerEvent& event) {
    if (!event.envelope.has_ack()) {
        LOG_INFO << "Invalid ACK without payload";
        return;
    }

    const message::Ack& ack = event.envelope.ack();
    if (ack.msg_id() == 0) {
        LOG_INFO << "Invalid ACK without msg_id: "
                 << EnvelopeInspector::ToString(event.envelope);
        return;
    }

    uint64_t uid = user_state_.getUidByConn(event.conn);
    if (uid == 0) {
        LOG_INFO << "ACK from anonymous connection. msg_id=" << ack.msg_id();
        return;
    }

    // Redis 脚本内部校验 uid 是否匹配消息接收方，避免 ACK 路径额外读一次消息记录。
    if (!message_store_.markAcked(ack.msg_id(), uid)) {
        LOG_INFO << "ACK update rejected. msg_id=" << ack.msg_id()
                 << ", ack_uid=" << uid;
        return;
    }
    
    // LOG_INFO << "Received ack: " << EnvelopeInspector::ToString(event.envelope);
}

// 处理心跳，刷新连接活跃时间。
void ServerEventDispatcher::handleHeartbeat(const ServerEvent& event) {
    user_state_.updateHeartbeat(event.conn);
}

// 处理连接断开，清理在线用户状态。
void ServerEventDispatcher::handleConnectionClosed(
    const ServerEvent& event) {
    user_state_.removeConnection(event.conn);
    LOG_INFO << "Connection closed event: "
             << event.conn->peerAddress().toIpPort();
}

void ServerEventDispatcher::deliveryLoop() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) {
                return;
            }
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(kDeliveryScanIntervalMs));

        std::vector<MessageRecord> timeout_records =
            message_store_.getTimeoutDeliveredMessages(NowMs(), kAckTimeoutMs);

        for (const MessageRecord& record : timeout_records) {
            if (record.retry_count >= kMaxDeliveryRetryCount) {
                message_store_.markFailed(record.msg_id);
                continue;
            }

            retryDeliverMessage(record);
        }
    }
}

void ServerEventDispatcher::deliverPendingMessages(uint64_t uid) {
    std::vector<MessageRecord> pending_records =
        message_store_.getPendingMessages(uid);
    LOG_INFO << "tryDeliverMessage " << "size:" <<  pending_records.size();
    for (const MessageRecord& record : pending_records) {
        tryDeliverMessage(record);
    }
}

bool ServerEventDispatcher::tryDeliverMessage(const MessageRecord& record) {
    if (record.status == MessageStatus::Acked ||
        record.status == MessageStatus::Failed) {
        return false;
    }
    
    muduo::net::TcpConnectionPtr target =
        user_state_.getConnByUid(record.to_uid);
    if (!target || !target->connected()) {
        message_store_.markPending(record.msg_id);
        return false;
    }

    message::Envelope push = EnvelopeFactory::CreateChatPush(
        record.msg_id,
        record.msg_id,
        record.from_uid,
        record.to_uid,
        record.content,
        record.created_at_ms);

    sendEnvelope(target, push);
    message_store_.markDelivered(record.msg_id);
    return true;
}

bool ServerEventDispatcher::retryDeliverMessage(const MessageRecord& record) {
    if (record.status == MessageStatus::Acked ||
        record.status == MessageStatus::Failed) {
        return false;
    }

    muduo::net::TcpConnectionPtr target =
        user_state_.getConnByUid(record.to_uid);
    if (!target || !target->connected()) {//如果用户不在线，标记为待投递状态。
        message_store_.markPending(record.msg_id);
        return false;
    }

    message_store_.incrementRetryCount(record.msg_id);

    message::Envelope push = EnvelopeFactory::CreateChatPush(
        record.msg_id,
        record.msg_id,
        record.from_uid,
        record.to_uid,
        record.content,
        record.created_at_ms);

    sendEnvelope(target, push);
    message_store_.markDelivered(record.msg_id);
    return true;
}

// 按统一帧格式发送 Envelope。
void ServerEventDispatcher::sendEnvelope(
    const muduo::net::TcpConnectionPtr& conn,
    const message::Envelope& envelope) {
    // 所有业务回包都走统一 Codec，保证和客户端使用相同的 4 字节长度头帧格式。
    std::string frame = MessageCodec::Encode(envelope);
    if (frame.empty()) {
        LOG_INFO << "Encode failed: " << EnvelopeInspector::ToString(envelope);
        conn->shutdown();
        return;
    }

    conn->send(frame);
}

// 发送失败 ACK。
void ServerEventDispatcher::sendErrorAck(
    const muduo::net::TcpConnectionPtr& conn,
    uint64_t seq,
    const std::string& reason) {
    message::Envelope ack = EnvelopeFactory::CreateAck(seq, 0, false, reason);
    sendEnvelope(conn, ack);
}
