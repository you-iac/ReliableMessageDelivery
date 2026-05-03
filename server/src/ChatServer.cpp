#include "ChatServer.h"

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpServer.h>

#include <string>
#include <vector>

#include "Codec.h"
#include "EnvelopeFactory.h"
#include "EnvelopeInspector.h"

ChatServer::ChatServer(uint16_t port, int thread_num)
    : port_(port),
      thread_num_(thread_num) {
}

bool ChatServer::start() {
    muduo::net::EventLoop event_loop;
    muduo::net::InetAddress addr(port_);
    muduo::net::TcpServer server(&event_loop, addr, "MiChat");

    // start() 内部完成 Muduo 服务器配置，让 main 只负责启动服务。
    server.setThreadNum(thread_num_);
    server.setConnectionCallback(
        [this](const TcpConnectionPtr& conn) {
            onConnection(conn);
        });
    server.setMessageCallback(
        [this](const TcpConnectionPtr& conn,
               Buffer* buffer,
               Timestamp time) {
            onMessage(conn, buffer, time);
        });

    LOG_INFO << "server init successful";
    server.start();
    event_loop.loop();
    return true;
}

void ChatServer::onConnection(const TcpConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (conn->connected()) {
        // 新连接先创建未登录 session，等 LOGIN_REQ 到达后再绑定 uid。
        ClientSession session;
        session.conn = conn;
        sessions_[conn.get()] = session;
        LOG_INFO << "New connection: " << conn->peerAddress().toIpPort();
        return;
    }

    auto it = sessions_.find(conn.get());
    if (it != sessions_.end()) {
        // 断开时同步清理在线用户表，避免后续消息推给失效连接。
        if (it->second.uid != 0) {
            online_users_.erase(it->second.uid);
            LOG_INFO << "User offline: uid=" << it->second.uid;
        }
        sessions_.erase(it);
    }
    LOG_INFO << "Connection closed: " << conn->peerAddress().toIpPort();
}

void ChatServer::onMessage(const TcpConnectionPtr& conn,
                           Buffer* buffer,
                           Timestamp) {
    std::string input = buffer->retrieveAllAsString();
    std::vector<message::Envelope> envelopes;

    {
        //根据连接获取对应的 session，解析 TCP 字节流中的完整消息，放入 envelopes。
        std::lock_guard<std::mutex> lock(mutex_);
        ClientSession* session = findSessionLocked(conn);
        if (session == nullptr) {
            conn->shutdown();
            return;
        }

        // TCP 是字节流，一次回调可能只有半包，也可能包含多条消息。
        session->recv_buffer.append(input.data(), input.size());
        MessageCodec::DecodeResult decode_result =
            MessageCodec::DecodeAll(session->recv_buffer);

        // 只移除已经解析成功的完整 frame，剩余半包留到下一次回调继续拼接。
        if (decode_result.consumed_bytes > 0) {
            session->recv_buffer.erase(0, decode_result.consumed_bytes);
        }

        if (decode_result.status == MessageCodec::DecodeStatus::kInvalidLength ||
            decode_result.status == MessageCodec::DecodeStatus::kParseError) {
            LOG_INFO << "Decode failed. status="
                     << static_cast<int>(decode_result.status);
            conn->shutdown();
            return;
        }

        envelopes.swap(decode_result.envelopes);
    }

    // 解码完成后再分发业务，避免业务处理时长占住连接表锁。
    for (const auto& envelope : envelopes) {
        LOG_INFO << "Decoded Envelope: " << EnvelopeInspector::ToString(envelope);
        dispatch(conn, envelope);
    }
}

ClientSession* ChatServer::findSessionLocked(
    const TcpConnectionPtr& conn) {
    auto it = sessions_.find(conn.get());
    if (it == sessions_.end()) {
        return nullptr;
    }
    return &it->second;
}

void ChatServer::dispatch(const TcpConnectionPtr& conn,
                          const Envelope& envelope) {
    switch (envelope.type()) {
        case message::LOGIN_REQ:
            handleLogin(conn, envelope);
            break;
        case message::CHAT_REQ:
            handleChat(conn, envelope);
            break;
        case message::HEARTBEAT:
            break;
        default:
            sendEnvelope(conn, EnvelopeFactory::CreateAck(
                envelope.seq(), 0, false, "unsupported message type"));
            break;
    }
}

void ChatServer::handleLogin(const TcpConnectionPtr& conn,
                             const Envelope& envelope) {
    if (!envelope.has_login_req() || envelope.login_req().uid() == 0) {
        sendEnvelope(conn, EnvelopeFactory::CreateLoginResponse(
            envelope.seq(), false, "invalid login request"));
        return;
    }

    uint64_t uid = envelope.login_req().uid();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ClientSession* session = findSessionLocked(conn);
        if (session == nullptr) {
            conn->shutdown();
            return;
        }

        // 同一连接重复登录时，先移除旧 uid 绑定，再写入新绑定。
        if (session->uid != 0) {
            online_users_.erase(session->uid);
        }
        session->uid = uid;
        online_users_[uid] = conn;
    }

    sendEnvelope(conn, EnvelopeFactory::CreateLoginResponse(
        envelope.seq(), true, ""));
    LOG_INFO << "User online: uid=" << uid;
}

void ChatServer::handleChat(const TcpConnectionPtr& conn,
                            const Envelope& envelope) {
    if (!envelope.has_chat_req()) {
        sendEnvelope(conn, EnvelopeFactory::CreateAck(
            envelope.seq(), 0, false, "invalid chat request"));
        return;
    }

    const message::ChatRequest& request = envelope.chat_req();
    TcpConnectionPtr target;
    uint64_t msg_id = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ClientSession* session = findSessionLocked(conn);
        if (session == nullptr) {
            conn->shutdown();
            return;
        }

        // 发送方 uid 必须和当前连接绑定的 uid 一致，避免冒充其他用户发送。
        if (session->uid == 0) {
            sendEnvelope(conn, EnvelopeFactory::CreateAck(
                envelope.seq(), 0, false, "please login first"));
            return;
        }
        if (request.from_uid() != session->uid) {
            sendEnvelope(conn, EnvelopeFactory::CreateAck(
                envelope.seq(), 0, false, "from_uid does not match session"));
            return;
        }

        // 第一版只做在线转发；目标不在线时先返回失败 ACK，不写离线存储。
        auto target_it = online_users_.find(request.to_uid());
        if (target_it == online_users_.end()) {
            sendEnvelope(conn, EnvelopeFactory::CreateAck(
                envelope.seq(), 0, false, "target user offline"));
            return;
        }

        target = target_it->second;
        msg_id = next_msg_id_.fetch_add(1);
    }

    // 给接收方推送 CHAT_PUSH，再给发送方返回服务端处理成功的 ACK。
    message::Envelope push = EnvelopeFactory::CreateChatPush(
        envelope.seq(),
        msg_id,
        request.from_uid(),
        request.to_uid(),
        request.content());
    sendEnvelope(target, push);

    sendEnvelope(conn, EnvelopeFactory::CreateAck(
        envelope.seq(), msg_id, true, ""));
}

void ChatServer::sendEnvelope(const TcpConnectionPtr& conn,
                              const Envelope& envelope) {
    // 所有服务端回包都复用统一帧格式：4 字节长度头 + protobuf Envelope。
    std::string frame = MessageCodec::Encode(envelope);
    if (frame.empty()) {
        LOG_INFO << "Encode failed: " << EnvelopeInspector::ToString(envelope);
        conn->shutdown();
        return;
    }
    conn->send(frame);
}
