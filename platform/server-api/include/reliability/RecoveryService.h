#pragma once

#include <atomic>
#include <memory>

#include <drogon/drogon.h>

#include "services/CoordinatorService.h"

namespace coordinator::reliability {

class RecoveryService {
  public:
    explicit RecoveryService(std::shared_ptr<services::CoordinatorService> coordinatorService);
    void start();
    void stop();

  private:
    std::shared_ptr<services::CoordinatorService> coordinatorService_;
    std::atomic<bool> running_{false};
};

}  // namespace coordinator::reliability
