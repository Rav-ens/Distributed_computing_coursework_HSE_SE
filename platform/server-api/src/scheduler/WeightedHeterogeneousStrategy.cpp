#include "scheduler/WeightedHeterogeneousStrategy.h"

#include <limits>

namespace coordinator::scheduler {

double WeightedHeterogeneousStrategy::calculateScore(const models::WorkerNode &node) {
    const double cpu = static_cast<double>(node.cpuCores) * 1.5;
    const double ram = static_cast<double>(node.ramMb) / 1024.0;
    const double throughput = node.throughputHistory * 2.0;
    const double failurePenalty = node.failureRate * 5.0;
    const double loadPenalty = static_cast<double>(node.currentLoad) * 1.2;
    const double latencyPenalty = node.avgLatencyMs / 100.0;
    const double baseline = node.performanceScore * 1.8;

    return cpu + ram + throughput + baseline - failurePenalty - loadPenalty - latencyPenalty;
}

std::optional<models::WorkerNode> WeightedHeterogeneousStrategy::selectNode(
    const models::TaskDescriptor & /*task*/,
    const std::vector<models::WorkerNode> &nodes) {
    if (nodes.empty()) {
        return std::nullopt;
    }

    double bestScore = -std::numeric_limits<double>::infinity();
    const models::WorkerNode *bestNode = nullptr;
    for (const auto &node : nodes) {
        const auto score = calculateScore(node);
        if (score > bestScore) {
            bestScore = score;
            bestNode = &node;
        }
    }

    if (!bestNode) {
        return std::nullopt;
    }
    return *bestNode;
}

}  // namespace coordinator::scheduler
