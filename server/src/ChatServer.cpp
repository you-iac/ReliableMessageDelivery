#include "ChatServer.h"

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpServer.h>

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Codec.h"
#include "EnvelopeInspector.h"

namespace {

bool QueueMonitorEnabled() {
    const char* value = std::getenv("RMD_QUEUE_MONITOR");
    return value == nullptr || value[0] != '0';
}

std::string MakeBar(std::size_t value, std::size_t max_value) {
    const std::size_t kWidth = 40;
    std::size_t width = 0;
    if (max_value > 0) {
        width = value * kWidth / max_value;
        if (value > 0 && width == 0) {
            width = 1;
        }
    }
    return std::string(width, '#') + std::string(kWidth - width, '.');
}

}  // namespace

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
    // 启动业务分发器线程池，准备处理后续到达的 Envelope。
    dispatcher_.start();
    startQueueMonitor();

    LOG_INFO << "server init successful";
    server.start();
    event_loop.loop();
    stopQueueMonitor();
    dispatcher_.stop();
    return true;
}

void ChatServer::onConnection(const TcpConnectionPtr& conn) {
    if (conn->connected()) {
        // 新连接先创建未登录 session，等 LOGIN_REQ 到达后再绑定 uid。
        ClientSession session;
        session.conn = conn;
        std::lock_guard<std::mutex> lock(mutex_);
        sessionsTable_[conn.get()] = session;
        LOG_INFO << "New connection: " << conn->peerAddress().toIpPort();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessionsTable_.find(conn.get());
        if (it != sessionsTable_.end()) {
            // ChatServer 只清理连接级半包缓存；用户在线状态由业务事件处理。
            sessionsTable_.erase(it);
        }
    }

    dispatcher_.enqueueConnectionClosed(conn);
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
        // LOG_INFO << "Get message: " << EnvelopeInspector::ToString(envelope);
        dispatcher_.enqueueEnvelope(conn, envelope);
    }

}

void ChatServer::startQueueMonitor() {
    if (!QueueMonitorEnabled()) {
        return;
    }

    stop_queue_monitor_.store(false);
    queue_monitor_ = std::thread([this] {
        while (!stop_queue_monitor_.load()) {
            renderQueueStatus();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });
}

void ChatServer::stopQueueMonitor() {
    stop_queue_monitor_.store(true);
    if (queue_monitor_.joinable()) {
        queue_monitor_.join();
    }
}

void ChatServer::renderQueueStatus() {
    std::vector<std::size_t> sizes = dispatcher_.getWorkerQueueSizes();
    std::size_t total = 0;
    std::size_t max_value = 0;
    for (std::size_t size : sizes) {
        total += size;
        if (size > max_value) {
            max_value = size;
        }
    }

    std::ostringstream output;
    output << "\033[2J\033[H";
    output << "RMD server queue monitor\n";
    output << "logs: logs/server.log.* | workers=" << sizes.size()
           << " total_queued=" << total << "\n\n";

    for (std::size_t i = 0; i < sizes.size(); ++i) {
        output << "worker " << std::setw(2) << std::setfill('0') << i
               << std::setfill(' ') << " [" << MakeBar(sizes[i], max_value)
               << "] " << sizes[i] << "\n";
    }

    std::cout << output.str() << std::flush;
}

ClientSession* ChatServer::findSessionLocked(const TcpConnectionPtr& conn) {
    auto it = sessionsTable_.find(conn.get());
    if (it == sessionsTable_.end()) {
        return nullptr;
    }
    return &it->second;
}
