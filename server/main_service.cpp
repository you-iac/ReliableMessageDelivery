#include "ChatServer.h"

#include <muduo/base/LogFile.h>
#include <muduo/base/Logging.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

namespace {

std::unique_ptr<muduo::LogFile> g_log_file;

void FileLogOutput(const char* msg, int len) {
    if (g_log_file) {
        g_log_file->append(msg, len);
    }
}

void FileLogFlush() {
    if (g_log_file) {
        g_log_file->flush();
    }
}

std::string ServerLogBasename() {
    const char* value = std::getenv("RMD_SERVER_LOG_BASENAME");
    if (value != nullptr && value[0] != '\0') {
        return value;
    }
    return "logs/server.log";
}

void ConfigureLogging() {
    mkdir("logs", 0755);
    g_log_file.reset(new muduo::LogFile(ServerLogBasename(),
                                        500 * 1000 * 1000,
                                        true));
    muduo::Logger::setOutput(FileLogOutput);
    muduo::Logger::setFlush(FileLogFlush);
}

}  // namespace

int main(int argc, char* argv[]) {
    ConfigureLogging();

    ChatServer server;
    server.start();
    return 0;
}
