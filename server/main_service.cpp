#include "ChatServer.h"

int main(int argc, char* argv[]) {
    ChatServer server(1556, 16, ChatServer::LogOutput::kTerminal);
    server.start();
    return 0;
}
