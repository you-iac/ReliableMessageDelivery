#ifndef CLIENT_H
#define CLIENT_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Message.pb.h"

// Client 封装一个用户侧 TCP 长连接。
// 对外提供启动、停止、发送消息和读取已收到消息；socket 与接收线程由类内部维护。
class Client {
private:
    uint64_t uid = 0;       // 当前客户端绑定的用户 ID。
    int sock = -1;          // 当前 TCP 连接的 socket fd。
    std::thread receiver;   // 后台接收线程。

    // 接收线程写 message_queue，主线程通过 getMessage() 读，因此需要互斥锁保护。
    std::mutex message_mutex;
    std::vector<message::Envelope> message_queue;

    std::atomic<bool> stop{false};
    std::atomic<int> received_count{0}; 

public:
    bool startClient(uint64_t uid, int expected_messages);
    void stopClient();

    bool sendMessage(uint64_t to_uid, const std::string& msg);
    std::vector<message::Envelope> getMessage();


private:    
    bool connectServer(const char* server_ip, uint16_t server_port);
    bool sendAll(const std::string& frame);
    bool sendEnvelope(const message::Envelope& envelope);
    void receiverLoop(int expected_messages);
    void pushMessage(const message::Envelope& envelope);
};

#endif
