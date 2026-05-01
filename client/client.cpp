#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

int main() {
    const char* server_ip = "127.0.0.1";
    const uint16_t server_port = 8080;

    //创建描述符
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "socket() failed: " << strerror(errno) << "\n";
        return 1;
    }
    //创建地址
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(server_port);
    //地址转换
    if (inet_pton(AF_INET, server_ip, &srv.sin_addr) != 1) {
        std::cerr << "inet_pton() failed\n";
        close(sock);
        return 1;
    }

    //连接服务器
    if (connect(sock, (sockaddr*)&srv, sizeof(srv)) < 0) {
        std::cerr << "connect() failed: " << strerror(errno) << "\n";
        close(sock);
        return 1;
    }

    char msg[1024] = {0};
    std::cout << "input: ";
    std::cin.getline(msg, sizeof(msg));
    ssize_t sent = send(sock, &msg, strlen(msg), 0);
    if (sent < 0) {
        std::cerr << "send() failed: " << strerror(errno) << "\n";
        close(sock);
        return 1;
    }

    char buf[1024];
    ssize_t recvd = recv(sock, buf, sizeof(buf)-1, 0);
    if (recvd < 0) {
        std::cerr << "recv() failed: " << strerror(errno) << "\n";
        close(sock);
        return 1;
    }
    buf[recvd] = '\0';
    
    std::cout << "Received: " << buf << "\n";

    close(sock);
    return 0;
}
