#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace coordinator::config {

struct AppConfig {
    std::string dbClientName{"default"};
    std::chrono::seconds heartbeatTimeout{15};
    std::chrono::seconds taskTimeout{60};
    std::chrono::seconds staleRecoveryInterval{5};
    std::vector<std::string> trustedRegistryPrefixes;
    bool enforceTrustedRegistryPrefixes{false};
    int registryPullTimeoutSeconds{300};
};

AppConfig loadConfigFromEnv();

}  // namespace coordinator::config
