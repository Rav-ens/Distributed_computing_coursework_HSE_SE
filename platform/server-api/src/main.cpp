#include <memory>

#include <drogon/drogon.h>
#include <spdlog/spdlog.h>

#include "config/AppConfig.h"
#include "controllers/ApiController.h"
#include "reliability/RecoveryService.h"
#include "services/CoordinatorService.h"

namespace {

struct AppRuntime {
    coordinator::config::AppConfig cfg;
    std::shared_ptr<coordinator::services::CoordinatorService> service;
    std::shared_ptr<coordinator::reliability::RecoveryService> recovery;
};

void stopRecoveryIfRunning(const std::shared_ptr<AppRuntime> &runtime) {
    if (runtime->recovery) {
        runtime->recovery->stop();
    }
}

void initServices(const std::shared_ptr<AppRuntime> &runtime) {
    if (runtime->service) {
        return;
    }

    auto dbClient = drogon::app().getDbClient(runtime->cfg.dbClientName);
    if (!dbClient) {
        spdlog::error(
            "DbClient '{}' is null after app start — проверьте config/config.json (db_clients) и доступность "
            "Postgres",
            runtime->cfg.dbClientName);
        return;
    }

    runtime->service = std::make_shared<coordinator::services::CoordinatorService>(dbClient, runtime->cfg);
    coordinator::controllers::ApiController::setService(runtime->service);
    runtime->recovery = std::make_shared<coordinator::reliability::RecoveryService>(runtime->service);
    runtime->recovery->start();
}

}  // namespace

int main() {
    const auto cfg = coordinator::config::loadConfigFromEnv();

    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%^%l%$] %v");
    spdlog::info("Starting coordinator (scheduler=weighted_heterogeneous)");

    drogon::app().loadConfigFile("config/config.json");

    auto runtime = std::make_shared<AppRuntime>();
    runtime->cfg = cfg;

    drogon::app().setTermSignalHandler([runtime]() {
        spdlog::warn("Termination signal received, stopping recovery service");
        stopRecoveryIfRunning(runtime);
    });

    drogon::app().registerBeginningAdvice([runtime]() {
        initServices(runtime);
        spdlog::info("Drogon app started");
    });

    drogon::app().run();
    return 0;
}
