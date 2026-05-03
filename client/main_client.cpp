#include "Client.h"

#include <chrono>
#include <iostream>
#include <thread>

#include "EnvelopeInspector.h"

int main() {
    //创建两个客户端
    Client user1;
    Client user2;
    //开启客户端，登录服务器
    if (!user1.startClient(1, 2)) {
        user1.stopClient();
        return 1;
    }
    if (!user2.startClient(2, 2)) {
        user1.stopClient();
        user2.stopClient();
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    user1.sendMessage(2, "Hello from user1!");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    

    std::vector<message::Envelope> user1_messages = user1.getMessage();
    std::vector<message::Envelope> user2_messages = user2.getMessage();

    std::cout << "user1 message count: " << user1_messages.size() << "\n";
    for (const auto& envelope : user1_messages) {
        std::cout << EnvelopeInspector::ToString(envelope) << "\n";
    }

    std::cout << "user2 message count: " << user2_messages.size() << "\n";
    for (const auto& envelope : user2_messages) {
        std::cout << EnvelopeInspector::ToString(envelope) << "\n";
    }

    user1.stopClient();
    user2.stopClient();
    return 0;
}
