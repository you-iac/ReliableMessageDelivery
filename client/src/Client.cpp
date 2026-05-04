#include "Client.h"

#include <cstring>
#include <iostream>

#include "Codec.h"
#include "EnvelopeFactory.h"
#include "EnvelopeInspector.h"

void Client::setVerbose(bool enabled) {
    verbose = enabled;
}

bool Client::startClient(uint64_t user_uid, int expected_messages) {
    uid = user_uid;
    stop.store(false);
    received_count.store(0);

    if (!connectServer("127.0.0.1", 8080)) {
        return false;
    }

    // 先启动接收线程，再发送登录包，避免服务端快速回包时无人读取。
    receiver = std::thread(&Client::receiverLoop, this, expected_messages);
    return sendEnvelope(EnvelopeFactory::CreateLoginRequest(1, uid));
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
    if (sock >= 0) {
        close(sock);
        sock = -1;
    }
}

bool Client::sendMessage(uint64_t to_uid, const std::string& msg) {
    // seq 用来让客户端和服务端 ACK 对上同一次请求；第一版先用进程内自增。
    static std::atomic<uint64_t> next_seq{2};

    uint64_t seq = next_seq.fetch_add(1);
    std::string client_msg_id =
        std::to_string(uid) + "-" + std::to_string(seq);

    message::Envelope envelope = EnvelopeFactory::CreateChatRequest(
        seq, uid, to_uid, msg, client_msg_id);
    return sendEnvelope(envelope);
}

std::vector<message::Envelope> Client::getMessage() {
    // 返回副本，调用方遍历时不再持有锁，避免长时间阻塞接收线程。
    std::lock_guard<std::mutex> lock(message_mutex);
    return message_queue;
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
//接收线程函数
void Client::receiverLoop(int expected_messages) {
    // TCP 是字节流，一次 recv 可能读到半包或多包，因此需要连接级缓存。
    std::string recv_buffer;

    while (!stop.load()) {
        //接收一次数据
        std::string buf(4096, '\0');
        ssize_t recvd = recv(sock, &buf[0], buf.size(), 0);
        
        if (recvd < 0) {
            if (!stop.load()) {
                std::cerr << "recv() failed: " << strerror(errno) << "\n";
            }
            stop.store(true);
            return;
        }
        if (recvd == 0) {
            stop.store(true);
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
        //把每个数据都放入消息队列，
        for (const auto& envelope : decode_result.envelopes) {
            pushMessage(envelope);
            ++received_count;
            if (verbose) {
                std::cout << "uid " << uid << " received: "
                          << EnvelopeInspector::ToString(envelope) << "\n";
            }
        }

        if (expected_messages > 0 &&
            received_count.load() >= expected_messages) {
            stop.store(true);
            return;
        }
        //如果解析错误，打印日志并关闭连接。
        if (decode_result.status == MessageCodec::DecodeStatus::kInvalidLength ||
            decode_result.status == MessageCodec::DecodeStatus::kParseError) {
            std::cerr << "decode failed, status="
                      << static_cast<int>(decode_result.status) << "\n";
            stop.store(true);
            shutdown(sock, SHUT_RDWR);
            return;
        }
    }
}

void Client::pushMessage(const message::Envelope& envelope) {
    // 接收线程写队列，主线程通过 getMessage() 读队列，因此 push 也必须加锁。
    std::lock_guard<std::mutex> lock(message_mutex);
    message_queue.push_back(envelope);
}
