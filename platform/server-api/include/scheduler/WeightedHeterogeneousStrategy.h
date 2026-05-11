#pragma once

#include "scheduler/ISchedulingStrategy.h"

namespace coordinator::scheduler {

class WeightedHeterogeneousStrategy final : public ISchedulingStrategy {
  public:
    std::string name() const override { return "weighted_heterogeneous"; }
    std::optional<models::WorkerNode> selectNode(
        const models::TaskDescriptor &task,
        const std::vector<models::WorkerNode> &nodes) override;

  private:
    static double calculateScore(const models::WorkerNode &node);
};

}  // namespace coordinator::scheduler
