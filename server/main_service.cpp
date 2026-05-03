#include "ChatServer.h"


int main() {
    ChatServer server;
    if (!server.start()) {
        return 1;
    }  
    
    return 0;
}
