#ifndef CLIENT_H
#define CLIENT_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Message.pb.h"

// Client 封装一个用户侧 TCP 长连接。
// 对外提供启动、停止、发送消息和读取已收到消息；socket 与接收线程由类内部维护。
class Client {
private:
    // PendingAck 结构体保存一个待 ACK 的请求及其重试状态。
    struct PendingAck {
        message::Envelope envelope;
        uint64_t last_send_ms = 0;
        int retry_count = 0;
    };

    uint64_t uid = 0;       // 当前客户端绑定的用户 ID。
    int sock = -1;          // 当前 TCP 连接的 socket fd。
    std::thread receiver;   // 后台接收线程。
    std::thread retry_thread; // 后台重试线程。

    
    std::mutex message_mutex;
    std::vector<message::Envelope> message_queue;// 接收线程写 message_queue，主线程通过 getMessage() 读，因此需要互斥锁保护。

    // 保护完整 frame 的发送过程，避免多个线程同时写 socket 导致字节流交错。
    std::mutex send_mutex;

    // seq -> PendingAck。表中存在表示该请求还没有收到服务端 ACK。
    std::mutex pending_mutex;
    std::unordered_map<uint64_t, PendingAck> pending_acks;
    
    //登录状态确认
    std::mutex login_mutex;
    std::condition_variable login_cv;
    bool login_done = false;
    bool login_ok = false;
    std::string login_reason;

    // 统计信息，使用 atomic 以便在不加锁的情况下跨线程更新和读取。
    std::atomic<int> ack_count{0};
    std::atomic<int> login_resp_count{0};
    std::atomic<int> chat_push_count{0};
    
    std::atomic<bool> stop{false};
    bool verbose = false;

public:
    void setVerbose(bool enabled); // 设置客户端是否输出详细收发日志。@param enabled true 表示输出每条 Envelope 的日志，false 表示关闭详细日志。

    bool startClient(uint64_t uid); // 启动客户端连接，并向服务端发送登录请求。@param uid 当前客户端要绑定的用户 ID。@return 启动并发送登录请求成功返回 true，否则返回 false。
    void stopClient(); // 停止客户端连接，并回收接收线程和 socket 资源。

    bool sendMessage(uint64_t to_uid, const std::string& msg); // 向指定用户发送一条聊天消息。@param to_uid 接收方用户 ID。@param msg 要发送的文本内容。@return 消息编码并写入 socket 成功返回 true，否则返回 false。
    std::vector<message::Envelope> getMessage(); // 获取当前已接收消息队列的快照。@return 当前消息队列副本，调用方读取该副本时不会阻塞接收线程。
    int getAckCount() const; // 获取当前已收到的 ACK 数量。
    int getLoginResponseCount() const; // 获取当前已收到的登录响应数量。
    int getChatPushCount() const; // 获取当前已收到的聊天推送数量。


private:    
    bool connectServer(const char* server_ip, uint16_t server_port); // 建立到服务端的 TCP 连接。@param server_ip 服务端 IPv4 地址字符串。@param server_port 服务端监听端口。@return 连接建立成功返回 true，否则返回 false。
    bool sendAll(const std::string& frame); // 将完整二进制帧全部写入当前 socket。@param frame 已编码好的 TCP 帧数据。@return 全部字节写入成功返回 true，否则返回 false。
    bool sendEnvelope(const message::Envelope& envelope); // 编码并发送一个业务 Envelope。@param envelope 要发送的业务消息对象。@return 编码并写入 socket 成功返回 true，否则返回 false。
    void receiverLoop(); // 接收线程主循环，持续读取 socket 数据并解析 Envelope。
    void retryLoop(); // 定时扫描 pending_acks，对超时未确认请求执行重传。
    void pushMessage(const message::Envelope& envelope); // 将接收到的 Envelope 追加到本地消息队列。@param envelope 已解析完成的业务消息对象。
    void handleEnvelope(const message::Envelope& envelope); // 根据消息类型处理收到的 Envelope。
    void handleAck(const message::Envelope& envelope); // 处理服务端 ACK，并从 pending_acks 中删除对应请求。
    void handleChatPush(const message::Envelope& envelope); // 处理服务端聊天推送，保存消息并回 ACK。
    void handleLoginResponse(const message::Envelope& envelope); // 处理登录响应，并唤醒等待登录结果的线程。
    void failPendingLogin(const std::string& reason); // 在连接关闭或协议错误时唤醒登录等待。
    static uint64_t nowMs(); // 获取当前毫秒时间戳。
};

#endif
