#include "reliability/RecoveryService.h"

#include <spdlog/spdlog.h>

namespace coordinator::reliability {

namespace {
constexpr double kRecoveryLoopIntervalSec = 5.0;
}

RecoveryService::RecoveryService(std::shared_ptr<services::CoordinatorService> coordinatorService)
    : coordinatorService_(std::move(coordinatorService)) {}

void RecoveryService::start() {
    running_.store(true);
    auto *loop = drogon::app().getLoop();
    if (loop == nullptr) {
        spdlog::error("RecoveryService::start: getLoop() is null, skip scheduling");
        return;
    }
    loop->runEvery(kRecoveryLoopIntervalSec, [this]() {
        if (!running_.load()) {
            return;
        }
        try {
            coordinatorService_->recoverStaleNodesAndTasks();
            coordinatorService_->processPendingPipelineSplits();
            coordinatorService_->assignPendingTasks();
            coordinatorService_->processReadyPipelineReduces();
        } catch (const std::exception &ex) {
            spdlog::error("recovery loop failed: {}", ex.what());
        }
    });
    spdlog::info("RecoveryService: periodic recovery scheduled (every {}s)", kRecoveryLoopIntervalSec);
}

void RecoveryService::stop() { running_.store(false); }

}  // namespace coordinator::reliability
