#include <muduo/base/Logging.h>
#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include<muduo/net/InetAddress.h>
#include <muduo/net/TcpConnection.h>

#include <iostream>
void onConnection(const muduo::net::TcpConnectionPtr& conn) {
   if (conn->connected()) {
       LOG_INFO << "New connection: " << conn->peerAddress().toIpPort() << " -> " << conn->localAddress().toIpPort();
   } else {
       LOG_INFO << "Connection closed: " << conn->peerAddress().toIpPort() << " -> " << conn->localAddress().toIpPort();
   }
}
// 处理数据可读的回调函数
void onMessage(const muduo::net::TcpConnectionPtr& conn, muduo::net::Buffer* buffer, muduo::Timestamp time) {
   std::string msg = buffer->retrieveAllAsString();
   LOG_INFO << "Received message: " << msg << " at " << time.toString();
   conn->send(msg); // 将接收到的数据原样返回给客户端
}
int main()
{
    //设置时间和服务器地址
    muduo::net::EventLoop event_loop;
    muduo::net::InetAddress addr(8080);
    //创建服务器
    muduo::net::TcpServer server(&event_loop, addr, "MiChat");
    

    //设置回调函数
    server.setConnectionCallback(onConnection);
    server.setMessageCallback(onMessage);

    LOG_INFO << "server init sucessfull!!";
    server.start();
    event_loop.loop();
    return 0;

}
