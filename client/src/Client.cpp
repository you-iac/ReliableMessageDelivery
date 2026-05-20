#include "Client.h"

#include <chrono>
#include <cstring>
#include <iostream>

#include "Codec.h"
#include "EnvelopeFactory.h"
#include "EnvelopeInspector.h"

namespace {

const uint64_t kBaseRetryTimeoutMs = 2000;
const int kMaxRetryCount = 5;//<最大重传次数
const int kRetryScanIntervalMs = 1000;

}  // namespace

void Client::setVerbose(bool enabled) {
    verbose = enabled;
}

bool Client::startClient(uint64_t user_uid) {
    uid = user_uid;

    // 每次启动都重置运行状态，避免复用 Client 对象时继承上一次连接的数据。
    stop.store(false);
    ack_count.store(0);
    login_resp_count.store(0);
    chat_push_count.store(0);

    {
        // pending_acks 只记录当前连接上的未确认请求，重连后旧请求不能继续复用。
        std::lock_guard<std::mutex> lock(pending_mutex);
        pending_acks.clear();
    }
    {
        // startClient 会等待 LOGIN_RESP；这里清空登录状态
        std::lock_guard<std::mutex> lock(login_mutex);
        login_done = false;
        login_ok = false;
        login_reason.clear();
    }

    if (!connectServer("127.0.0.1", 8080)) {
        return false;
    }

    // 先启动接收线程，再发送登录包，避免服务端快速回包时无人读取。
    receiver = std::thread(&Client::receiverLoop, this);
    if (!sendEnvelope(EnvelopeFactory::CreateLoginRequest(1, uid))) {
        stopClient();
        return false;
    }

    // sendEnvelope 成功只代表登录请求写入 socket，这里继续等待服务端业务确认。
    std::unique_lock<std::mutex> lock(login_mutex);
    bool received = login_cv.wait_for(lock,
                                      std::chrono::seconds(10),
                                      [this] { return login_done; });
    bool ok = received && login_ok;
    lock.unlock();
    if (!ok) {
        stopClient();
        return false;
    }

    retry_thread = std::thread(&Client::retryLoop, this);
    return true;
}

void Client::stopClient() {
    stop.store(true);

    // shutdown 会唤醒阻塞在 recv() 上的接收线程，让 join 可以顺利返回。
    if (sock >= 0) {
        shutdown(sock, SHUT_RDWR);
    }
    if (receiver.joinable()) {
        receiver.join();
    }
    if (retry_thread.joinable()) {
        retry_thread.join();
    }
    if (sock >= 0) {
        close(sock);
        sock = -1;
    }
}

bool Client::sendMessage(uint64_t to_uid, const std::string& msg) {
    if (stop.load()) {
        return false;
    }
    {
        // 只有收到服务端 LOGIN_RESP(ok=true) 后，才允许发送业务消息。
        std::lock_guard<std::mutex> lock(login_mutex);
        if (!login_ok) {return false;}
    }

    // seq 用来让客户端和服务端 ACK 对上同一次请求；第一版先用进程内自增。
    static std::atomic<uint64_t> next_seq{2};
    static std::atomic<uint64_t> next_client_msg_no{1};

    uint64_t seq = next_seq.fetch_add(1);
    uint64_t msg_no = next_client_msg_no.fetch_add(1);
    std::string client_msg_id =
        std::to_string(uid) + "-" +
        std::to_string(nowMs()) + "-" +
        std::to_string(msg_no);
    
    message::Envelope envelope = EnvelopeFactory::CreateChatRequest(
        seq, uid, to_uid, msg, client_msg_id);
    {
        // 先登记再发送，避免服务端快速 ACK 到达时 pending_acks 中还没有记录。
        std::lock_guard<std::mutex> lock(pending_mutex);
        PendingAck pending;
        pending.envelope = envelope;
        pending.last_send_ms = nowMs();
        pending.retry_count = 0;
        pending_acks[seq] = pending;
    }

    if (sendEnvelope(envelope)) {
        return true;
    }

    {
        // 写 socket 失败说明服务端不可能处理该请求，移除待 ACK 记录。
        std::lock_guard<std::mutex> lock(pending_mutex);
        pending_acks.erase(seq);
    }
    return false;
}

std::vector<message::Envelope> Client::getMessage() {
    // 返回副本，调用方遍历时不再持有锁，避免长时间阻塞接收线程。
    std::lock_guard<std::mutex> lock(message_mutex);
    return message_queue;
}

int Client::getAckCount() const {
    return ack_count.load();
}

int Client::getLoginResponseCount() const {
    return login_resp_count.load();
}

int Client::getChatPushCount() const {
    return chat_push_count.load();
}

bool Client::connectServer(const char* server_ip, uint16_t server_port) {
    //创建描述符
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "socket() failed: " << strerror(errno) << "\n";
        return false;
    }
    //绑定地址
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &srv.sin_addr) != 1) {
        std::cerr << "inet_pton() failed\n";
        close(sock);
        sock = -1;
        return false;
    }
    //连接服务器
    if (connect(sock, reinterpret_cast<sockaddr*>(&srv), sizeof(srv)) < 0) {
        std::cerr << "connect() failed: " << strerror(errno) << "\n";
        close(sock);
        sock = -1;
        return false;
    }

    return true;
}

bool Client::sendAll(const std::string& frame) {
    // frame 是二进制数据，不能用 strlen；必须按 string::size() 发送。
    // 锁覆盖整个 frame，避免多个线程把不同 frame 的字节交错写入同一条 TCP 流。
    std::lock_guard<std::mutex> lock(send_mutex);
    std::size_t total_sent = 0;
    while (total_sent < frame.size()) {
        ssize_t sent = send(sock,
                            frame.data() + total_sent,
                            frame.size() - total_sent,
                            0);
        if (sent < 0) {
            std::cerr << "send() failed: " << strerror(errno) << "\n";
            return false;
        }
        if (sent == 0) {
            std::cerr << "send() returned 0 before all data was sent\n";
            return false;
        }
        total_sent += static_cast<std::size_t>(sent);
    }
    return true;
}

bool Client::sendEnvelope(const message::Envelope& envelope) {
    // 业务消息先交给 MessageCodec 加 4 字节长度头，再写入 TCP。
    std::string frame = MessageCodec::Encode(envelope);
    if (frame.empty()) {
        std::cerr << "encode failed: "
                  << EnvelopeInspector::ToString(envelope) << "\n";
        return false;
    }

    if (verbose) {
        std::cout << "send: " << EnvelopeInspector::ToString(envelope) << "\n";
    }
    return sendAll(frame);
}

void Client::receiverLoop() {
    // TCP 是字节流，一次 recv 可能读到半包或多包，因此需要连接级缓存。
    std::string recv_buffer;

    while (!stop.load()) {
        // recv 是阻塞调用；没有数据时线程会睡眠，不会因为 while 循环空转。
        std::string buf(4096, '\0');
        ssize_t recvd = recv(sock, &buf[0], buf.size(), 0);
        
        if (recvd < 0) {
            if (!stop.load()) {
                std::cerr << "recv() failed: " << strerror(errno) << "\n";
            }
            stop.store(true);
            failPendingLogin("recv failed");
            return;
        }
        if (recvd == 0) {
            stop.store(true);
            // 连接在登录完成前关闭时，需要唤醒 startClient 的等待。
            failPendingLogin("connection closed");
            return;
        }
        //添加数据到接收缓冲区，并尝试解析出完整消息
        recv_buffer.append(buf.data(), static_cast<std::size_t>(recvd));
        MessageCodec::DecodeResult decode_result =
            MessageCodec::DecodeAll(recv_buffer);

        // DecodeAll 只消费完整 frame，剩下的半包继续留到下一次 recv 后解析。
        if (decode_result.consumed_bytes > 0) {
            recv_buffer.erase(0, decode_result.consumed_bytes);
        }
        // 控制消息和业务消息在这里分流，message_queue 只保存真正的业务推送。
        for (const auto& envelope : decode_result.envelopes) {
            handleEnvelope(envelope);        
            if (verbose) {
                std::cout << "recv: " << EnvelopeInspector::ToString(envelope) << "\n";
            }
        }

        //如果解析错误，打印日志并关闭连接。
        if (decode_result.status == MessageCodec::DecodeStatus::kInvalidLength ||
            decode_result.status == MessageCodec::DecodeStatus::kParseError) {
            std::cerr << "decode failed, status="
                      << static_cast<int>(decode_result.status) << "\n";
            stop.store(true);
            failPendingLogin("decode failed");
            shutdown(sock, SHUT_RDWR);
            return;
        }
    }
}

void Client::retryLoop() {
    while (!stop.load()) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kRetryScanIntervalMs));

        std::vector<message::Envelope> retry_messages;
        uint64_t now = nowMs();
        {
            std::lock_guard<std::mutex> lock(pending_mutex);
            for (auto it = pending_acks.begin(); it != pending_acks.end();) {
                PendingAck& pending = it->second;
                // 重试间隔随已重试次数线性增加：1s、2s、3s...
                uint64_t retry_timeout_ms =
                    kBaseRetryTimeoutMs * (pending.retry_count + 1);
                if (now - pending.last_send_ms < retry_timeout_ms) {
                    ++it;
                    continue;
                }
                // 如果重试次数已经达到上限，说明服务端可能永远处理不了这个请求了，直接放弃。
                if (pending.retry_count >= kMaxRetryCount) {
                    if (verbose) {
                        std::cerr << "drop unacked message after retry limit: "
                                  << EnvelopeInspector::ToString(pending.envelope)
                                  << "\n";
                    }
                    it = pending_acks.erase(it);
                    continue;
                }

                // 只在锁内更新重试状态和复制消息，真正发送放到锁外完成。
                ++pending.retry_count;
                pending.last_send_ms = now;
                retry_messages.push_back(pending.envelope);
                ++it;
            }
        }
        // 锁外重试发送，避免发送过程中阻塞其他线程对 pending_acks 的访问。
        for (const auto& envelope : retry_messages) {
            if (stop.load()) {
                return;
            }
            if (verbose) {
                std::cout << "retry: "
                          << EnvelopeInspector::ToString(envelope) << "\n";
            }
            if (!sendEnvelope(envelope)) {
                stop.store(true);
                return;
            }
        }
    }
}

void Client::pushMessage(const message::Envelope& envelope) {
    // 接收线程写队列，主线程通过 getMessage() 读队列，因此 push 也必须加锁。
    std::lock_guard<std::mutex> lock(message_mutex);
    message_queue.push_back(envelope);
}

void Client::handleEnvelope(const message::Envelope& envelope) {
    // receiverLoop 只负责收包和解码，具体状态更新集中在这里按类型分发。
    switch (envelope.type()) {
        case message::ACK:
            handleAck(envelope);
            break;

        case message::CHAT_PUSH:
            handleChatPush(envelope);
            break;

        case message::LOGIN_RESP:
            handleLoginResponse(envelope);
            break;

        default:
            break;
    }
}
void Client::handleAck(const message::Envelope& envelope) {
    if (!envelope.has_ack()) {
        return;
    }

    ++ack_count;
    std::lock_guard<std::mutex> lock(pending_mutex);
    // 服务端 ACK 使用原请求 seq，收到后即可认为该请求不再处于待确认状态。
    pending_acks.erase(envelope.ack().seq());
}

void Client::handleChatPush(const message::Envelope& envelope) {
    if (!envelope.has_chat_push()) {
        return;
    }

    // CHAT_PUSH 是真正需要业务层读取的消息，因此进入 message_queue。
    ++chat_push_count;
    pushMessage(envelope);

    // 收到服务端推送后回 ACK，表示接收方已经消费到该 msg_id。
    sendEnvelope(EnvelopeFactory::CreateAck(
        envelope.seq(),
        envelope.chat_push().msg_id(),
        true,
        ""));
}

void Client::handleLoginResponse(const message::Envelope& envelope) {
    ++login_resp_count;

    std::lock_guard<std::mutex> lock(login_mutex);
    login_done = true;
    if (!envelope.has_login_resp()) {
        login_ok = false;
        login_reason = "invalid login response";
    } else {
        login_ok = envelope.login_resp().ok();
        login_reason = envelope.login_resp().reason();
    }
    // 唤醒 startClient，让它根据 login_ok 决定是否启动成功。
    login_cv.notify_all();
}

void Client::failPendingLogin(const std::string& reason) {
    std::lock_guard<std::mutex> lock(login_mutex);
    if (login_done) {
        return;
    }
    // 如果登录响应还没到就发生连接/协议错误，也要结束 startClient 的等待。
    login_done = true;
    login_ok = false;
    login_reason = reason;
    login_cv.notify_all();
}

uint64_t Client::nowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count());
}
