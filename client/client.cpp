#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <functional>
#include <atomic>

#include "Codec.h"
#include "EnvelopeFactory.h"

// include fixed-width integer types
#include <cstdint>

//发送一条消息
bool sendMessage(const char* server_ip,
                 const uint16_t server_port,
                 const std::string& msgbuff);

void workthreadFunc(int threadId, int N, const char* msgbuff,std::atomic<int> &count) {
    for (int i = 0; i < N; ++i) {
        sendMessage("127.0.0.1", 8080, std::string(msgbuff));
        count++;
    }
}


int main() {
    int i = 0;
    while(i++<100000){
    message::Envelope envelope;
    envelope = EnvelopeFactory::CreateChatRequest(i, i, i+1, "Hello, TCP!", "msgid123");
    
    std::string buff = MessageCodec::Encode(envelope);
    
    std::cout << "Encoded frame size: " << buff.size() << " bytes\n";
    std::cout << EnvelopeFactory::ToString(envelope) << std::endl;

    sendMessage("127.0.0.1", 8080, buff);

    }
    

    return 0;
}




bool sendMessage(const char* server_ip,
                 const uint16_t server_port,
                 const std::string& msgbuff)
{
    //创建描述符
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "socket() failed: " << strerror(errno) << "\n";
        return false;
    }
    //创建地址
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(server_port);
    //地址转换
    if (inet_pton(AF_INET, server_ip, &srv.sin_addr) != 1) {
        std::cerr << "inet_pton() failed\n";
        close(sock);
        return false;
    }
    std::cout << "Connecting to " << server_ip << ":" << server_port << "...\n";
    //连接服务器
    if (connect(sock, (sockaddr*)&srv, sizeof(srv)) < 0) {
        std::cerr << "connect() failed: " << strerror(errno) << "\n";
        close(sock);
        return false;
    }
    std::cout << "sent to server.\n";
    // 发送消息。
    // msgbuff 是二进制帧，前 4 字节长度头可能包含 '\0'，不能用 strlen() 计算长度。
    std::size_t total_sent = 0;
    while (total_sent < msgbuff.size()) {
        ssize_t sent = send(sock,
                            msgbuff.data() + total_sent,
                            msgbuff.size() - total_sent,
                            0);
        if (sent < 0) {
            std::cerr << "send() failed: " << strerror(errno) << "\n";
            close(sock);
            return false;
        }
        if (sent == 0) {
            std::cerr << "send() returned 0 before all data was sent\n";
            close(sock);
            return false;
        }
        total_sent += static_cast<std::size_t>(sent);
    }
    std::cout << "sent bytes: " << total_sent << "\n";
    std::cout << "recving from server.\n";
    //接收消息
    std::string buf;
    buf.resize(4096); // 预分配缓冲区，实际接收的字节数由 recv() 返回值决定
    ssize_t recvd = recv(sock, &buf[0], buf.size(), 0);
    if (recvd < 0) {
        std::cerr << "recv() failed: " << strerror(errno) << "\n";
        close(sock);
        return false;
    }
    std::cout << "Received from server: buf size" << buf.size() << "\n";
    MessageCodec::DecodeResult decode_result = MessageCodec::DecodeAll(buf.data(), recvd);
    if(decode_result.status == MessageCodec::DecodeStatus::kOk) {
        for (const auto& env : decode_result.envelopes) {
            std::cout << "Decoded Envelope: " << EnvelopeFactory::ToString(env) << std::endl;
        }
    } else if (decode_result.status == MessageCodec::DecodeStatus::kNeedMoreData) {
        std::cout << "Need more data to decode a complete message." << std::endl;
    } else {
        std::cout << "Failed to decode message. Status: " << static_cast<int>(decode_result.status) << std::endl;
    }
    
    close(sock);
    return true;
}
