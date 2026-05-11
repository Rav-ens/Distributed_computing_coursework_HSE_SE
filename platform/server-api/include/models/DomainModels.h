#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace coordinator::models {

enum class NodeStatus { online, offline, degraded };
enum class TaskStatus { queued, assigned, running, succeeded, failed, retry_wait, stale };
enum class InstallStatus { pending, installed, failed };

struct WorkerNode {
    std::int64_t id{};
    std::string host;
    NodeStatus status{NodeStatus::online};
    std::int32_t cpuCores{};
    std::int32_t ramMb{};
    double performanceScore{};
    double throughputHistory{};
    double failureRate{};
    std::int32_t currentLoad{};
    double avgLatencyMs{};
    std::chrono::system_clock::time_point lastHeartbeat{};
};

struct TaskDescriptor {
    std::int64_t id{};
    std::int64_t projectId{};
    std::string payloadJson;
    std::int32_t priority{};
    TaskStatus status{TaskStatus::queued};
    std::optional<std::int64_t> assignedNodeId;
    std::int32_t attempts{};
    std::int32_t maxAttempts{};
};

struct ScoredNode {
    WorkerNode node;
    double score{};
};

}  // namespace coordinator::models
