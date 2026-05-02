#include <muduo/base/Logging.h>
#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include<muduo/net/InetAddress.h>
#include <muduo/net/TcpConnection.h>

#include "Codec.h"
#include "EnvelopeFactory.h"

#include <iostream>
void onConnection(const muduo::net::TcpConnectionPtr& conn) {
//    if (conn->connected()) {
//        LOG_INFO << "New connection: " << conn->peerAddress().toIpPort() << " -> " << conn->localAddress().toIpPort();
//    } else {
//        LOG_INFO << "Connection closed: " << conn->peerAddress().toIpPort() << " -> " << conn->localAddress().toIpPort();
//    }
}
// 处理数据可读的回调函数
void onMessage(const muduo::net::TcpConnectionPtr& conn, muduo::net::Buffer* buffer, muduo::Timestamp time) {
   std::string msg = buffer->retrieveAllAsString();
   LOG_INFO << "Received raw data: " << msg.size() << " bytes";

   //解析并且返回结果
   MessageCodec::DecodeResult decode_result = MessageCodec::DecodeAll(msg.data(), msg.size());
   if(decode_result.status == MessageCodec::DecodeStatus::kOk) {
       for (const auto& env : decode_result.envelopes) {
           LOG_INFO << "Decoded Envelope: " << EnvelopeFactory::ToString(env);
       }
   } else if (decode_result.status == MessageCodec::DecodeStatus::kNeedMoreData) {
       LOG_INFO << "Need more data to decode a complete message.";
   } else {
       LOG_INFO << "Failed to decode message. Status: " << static_cast<int>(decode_result.status);
   }
   
   LOG_INFO << "sendback message: "  << " at " << time.toString();
   conn->send(msg); // 将接收到的数据原样返回给客户端
}
int main()
{
    muduo::net::EventLoop event_loop;
    muduo::net::InetAddress addr(8080);
    //创建服务器
    muduo::net::TcpServer server(&event_loop, addr, "MiChat");
    
    server.setThreadNum(16); // 设置服务器线程数为4

    //设置回调函数
    server.setConnectionCallback(onConnection);
    server.setMessageCallback(onMessage);

    LOG_INFO << "server init sucessfull!!";
    server.start();
    event_loop.loop();
    return 0;

}
