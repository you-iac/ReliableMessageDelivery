#include "ChatServer.h"

int main(int argc, char* argv[]) {
    ChatServer server(8080, 16, ChatServer::LogOutput::kTerminal);
    server.start();
    return 0;
}
