#include "ServerEventDispatcher.h"

#include <muduo/base/Logging.h>

#include "Codec.h"
#include "EnvelopeFactory.h"
#include "EnvelopeInspector.h"

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
}

// 将客户端 Envelope 事件投递到业务队列。
bool ServerEventDispatcher::enqueueEnvelope(
            const muduo::net::TcpConnectionPtr& conn,
            const message::Envelope& envelope) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return false;
        }

        ServerEvent event;
        event.type = ServerEvent::Type::Envelope;
        event.conn = conn;
        event.envelope = envelope;
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
            std::unique_lock<std::mutex> lock(mutex_);
            not_empty_.wait(lock, [this] {
                return stopped_ || !inbox_.empty();
            });

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
    }
}

// 处理聊天请求，校验发送方并做在线转发。
void ServerEventDispatcher::handleChat(const ServerEvent& event) {
    if (!event.envelope.has_chat_req()) {
        sendErrorAck(event.conn, event.envelope.seq(), "invalid chat request");
        return;
    }

    const message::ChatRequest& request = event.envelope.chat_req();

    uint64_t uid = user_state_.getUidByConn(event.conn);
    if (uid == 0) {
        sendErrorAck(event.conn, event.envelope.seq(), "please login first");
        return;
    }

    if (request.from_uid() != uid) {
        sendErrorAck(event.conn,
                     event.envelope.seq(),
                     "from_uid does not match session");
        return;
    }

    MessageRecord record = message_store_.createMessage(
        request.from_uid(),
        request.to_uid(),
        request.content(),
        request.client_msg_id());

    muduo::net::TcpConnectionPtr target =
        user_state_.getConnByUid(request.to_uid());
    if (!target) {
        sendEnvelope(event.conn, EnvelopeFactory::CreateAck(
            event.envelope.seq(), record.msg_id, true, "target user offline"));
        return;
    }

    message::Envelope push = EnvelopeFactory::CreateChatPush(
        event.envelope.seq(),
        record.msg_id,
        record.from_uid,
        record.to_uid,
        record.content);

    sendEnvelope(target, push);
    message_store_.markDelivered(record.msg_id);
    sendEnvelope(event.conn, EnvelopeFactory::CreateAck(
        event.envelope.seq(), record.msg_id, true, ""));
}

// 处理 ACK；当前只记录日志，后续接入消息状态机。
void ServerEventDispatcher::handleAck(const ServerEvent& event) {
    LOG_INFO << "Received ack: " << EnvelopeInspector::ToString(event.envelope);
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

// 按统一帧格式发送 Envelope。
void ServerEventDispatcher::sendEnvelope(
    const muduo::net::TcpConnectionPtr& conn,
    const message::Envelope& envelope) {
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
