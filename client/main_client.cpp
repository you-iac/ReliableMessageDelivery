#include "Client.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int ParsePositiveInt(const char* value, int fallback) {
    if (value == nullptr) {
        return fallback;
    }

    int parsed = std::atoi(value);
    return parsed > 0 ? parsed : fallback;
}

int CountByType(const std::vector<message::Envelope>& messages,
                message::MessageType type) {
    int count = 0;
    for (const auto& envelope : messages) {
        if (envelope.type() == type) {
            ++count;
        }
    }
    return count;
}

int TotalClientAcks(const std::vector<std::unique_ptr<Client>>& clients) {
    int total = 0;
    for (const auto& client : clients) {
        total += client->getAckCount();
    }
    return total;
}

int TotalClientLoginResponses(
    const std::vector<std::unique_ptr<Client>>& clients) {
    int total = 0;
    for (const auto& client : clients) {
        total += client->getLoginResponseCount();
    }
    return total;
}

int TotalClientChatPushes(const std::vector<std::unique_ptr<Client>>& clients) {
    int total = 0;
    for (const auto& client : clients) {
        total += client->getChatPushCount();
    }
    return total;
}

uint64_t ClientUid(int index) {
    return static_cast<uint64_t>(1000 + index);
}

void StopClientsConcurrently(std::vector<std::unique_ptr<Client>>& clients) {
    const std::size_t kCloseGroupSize = 10;
    std::vector<std::thread> close_workers;
    close_workers.reserve((clients.size() + kCloseGroupSize - 1) /
                          kCloseGroupSize);

    for (std::size_t begin = 0; begin < clients.size();
         begin += kCloseGroupSize) {
        std::size_t end = begin + kCloseGroupSize;
        if (end > clients.size()) {
            end = clients.size();
        }

        close_workers.emplace_back([&clients, begin, end] {
            for (std::size_t i = begin; i < end; ++i) {
                clients[i]->stopClient();
            }
        });
    }

    for (auto& worker : close_workers) {
        worker.join();
    }
}

int RunStressTest(int client_count,
                  int messages_per_client,
                  bool verbose,
                  int wait_timeout_seconds) {
    const int total_messages = client_count * messages_per_client;

    std::cout << "stress config: clients=" << client_count
              << ", messages_per_client=" << messages_per_client
              << ", total_messages=" << total_messages
              << ", route=ring"
              << ", verbose=" << (verbose ? "true" : "false")
              << ", wait_timeout_seconds=" << wait_timeout_seconds << "\n";

    std::vector<std::unique_ptr<Client>> clients;
    clients.reserve(static_cast<std::size_t>(client_count));
    for (int i = 0; i < client_count; ++i) {
        std::unique_ptr<Client> client(new Client());
        client->setVerbose(verbose);
        uint64_t uid = ClientUid(i);
        if (!client->startClient(uid)) {
            std::cerr << "failed to start client uid=" << uid << "\n";
            client->stopClient();
            StopClientsConcurrently(clients);
            return 1;
        }
        clients.push_back(std::move(client));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::atomic<int> send_ok{0};
    std::atomic<int> send_failed{0};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(client_count));

    auto started_at = std::chrono::steady_clock::now();
    for (int i = 0; i < client_count; ++i) {
        workers.emplace_back([&, i] {
            uint64_t sender_uid = ClientUid(i);
            uint64_t receiver_uid = ClientUid((i + 1) % client_count);
            for (int n = 0; n < messages_per_client; ++n) {
                std::ostringstream oss;
                oss << "stress message sender=" << sender_uid
                    << " receiver=" << receiver_uid
                    << " index=" << n;

                if (clients[static_cast<std::size_t>(i)]->sendMessage(
                        receiver_uid, oss.str())) {
                    ++send_ok;
                } else {
                    ++send_failed;
                }
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    auto send_finished_at = std::chrono::steady_clock::now();
    double send_elapsed_seconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            send_finished_at - started_at).count() / 1000.0;

    int client_acks = 0;
    int client_pushes = 0;
    auto wait_started_at = std::chrono::steady_clock::now();
    auto ack_completed_at = wait_started_at;
    auto push_completed_at = wait_started_at;
    bool ack_done = false;
    bool push_done = false;

    while (true) {
        client_acks = TotalClientAcks(clients);
        client_pushes = TotalClientChatPushes(clients);

        auto now = std::chrono::steady_clock::now();
        if (!ack_done && client_acks >= total_messages) {
            ack_done = true;
            ack_completed_at = now;
        }
        if (!push_done && client_pushes >= total_messages) {
            push_done = true;
            push_completed_at = now;
        }

        // 即使本轮计数已经达标，也继续等到超时，方便客户端消费历史 Pending。
        int waited_seconds = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(
                now - wait_started_at).count());
        if (waited_seconds >= wait_timeout_seconds) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto finished_at = std::chrono::steady_clock::now();
    int client_login_resps = TotalClientLoginResponses(clients);

    double ack_elapsed_seconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            (ack_done ? ack_completed_at : finished_at) - started_at).count() /
        1000.0;
    double push_elapsed_seconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            (push_done ? push_completed_at : finished_at) - started_at).count() /
        1000.0;
    double total_elapsed_seconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            finished_at - started_at).count() / 1000.0;

    std::cout << "\nStress summary\n";
    std::cout << "  send_elapsed_seconds:  " << send_elapsed_seconds << "\n";
    std::cout << "  ack_elapsed_seconds:   " << ack_elapsed_seconds << "\n";
    std::cout << "  push_elapsed_seconds:  " << push_elapsed_seconds << "\n";
    std::cout << "  total_elapsed_seconds: " << total_elapsed_seconds << "\n";
    std::cout << "  send_ok:               " << send_ok.load() << "\n";
    std::cout << "  send_failed:           " << send_failed.load() << "\n";
    std::cout << "  client_login_resps:    " << client_login_resps << "\n";
    std::cout << "  client_chat_pushes:    " << client_pushes << "\n";
    std::cout << "  client_acks:           " << client_acks << "\n";
    if (send_elapsed_seconds > 0) {
        std::cout << "  client_write_throughput: "
                  << send_ok.load() / send_elapsed_seconds << " msg/s\n";
    }
    if (ack_elapsed_seconds > 0) {
        std::cout << "  server_ack_throughput:   "
                  << client_acks / ack_elapsed_seconds << " msg/s\n";
    }
    if (push_elapsed_seconds > 0) {
        std::cout << "  client_push_throughput:  "
                  << client_pushes / push_elapsed_seconds << " msg/s\n";
    }

    StopClientsConcurrently(clients);

    return (send_failed.load() == 0 &&
            client_acks >= total_messages &&
            client_pushes >= total_messages)
               ? 0
               : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    const int client_count = argc > 1 ? ParsePositiveInt(argv[1], 8) : 8;
    const int messages_per_client =
        argc > 2 ? ParsePositiveInt(argv[2], 100) : 100;
    const bool verbose = argc > 3 && std::string(argv[3]) == "verbose";
    const int wait_timeout_seconds =
        argc > 4 ? ParsePositiveInt(argv[4], 30) : 30;
    
    return RunStressTest(client_count,
                         messages_per_client,
                         verbose,
                         wait_timeout_seconds);
    // Client c;
    // c.startClient(1);
    // sleep(1000 * 60);
    // c.stopClient();
}
