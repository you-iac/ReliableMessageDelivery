#ifndef SERVER_STATUS_H
#define SERVER_STATUS_H

#include <cstdint>
#include <string>
#include <vector>

class ServerStatus {
public:
    struct StatusMetric {
        std::string key;
        std::vector<uint64_t> numbers;
    };

    void addMetric(const StatusMetric& metric) {
        metrics_.push_back(metric);
    }

    const std::vector<StatusMetric>& metrics() const {
        return metrics_;
    }

private:
    std::vector<StatusMetric> metrics_;
};

#endif
