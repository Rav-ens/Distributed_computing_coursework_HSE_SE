#pragma once

#include <optional>
#include <string>
#include <vector>

#include "models/DomainModels.h"

namespace coordinator::scheduler {

class ISchedulingStrategy {
  public:
    virtual ~ISchedulingStrategy() = default;
    virtual std::string name() const = 0;
    virtual std::optional<models::WorkerNode> selectNode(
        const models::TaskDescriptor &task,
        const std::vector<models::WorkerNode> &nodes) = 0;
};

}  // namespace coordinator::scheduler
