#include "ChatServer.h"

int main(int argc, char* argv[]) {
    ChatServer server(8080, 16, ChatServer::LogOutput::kFile);
    server.start();
    return 0;
}
