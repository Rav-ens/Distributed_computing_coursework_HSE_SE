#include "config/AppConfig.h"

#include <cstdlib>
#include <sstream>

namespace coordinator::config {

namespace {
std::string getenvOrDefault(const char *key, std::string fallback) {
    if (const auto *value = std::getenv(key); value != nullptr) {
        return value;
    }
    return fallback;
}

bool getenvBool(const char *key, bool fallback) {
    const std::string v = getenvOrDefault(key, fallback ? "1" : "0");
    return v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "YES";
}

int getenvInt(const char *key, int fallback) {
    const std::string v = getenvOrDefault(key, "");
    if (v.empty()) {
        return fallback;
    }
    try {
        return std::stoi(v);
    } catch (...) {
        return fallback;
    }
}

void parseCommaList(const std::string &raw, std::vector<std::string> &out) {
    std::istringstream iss(raw);
    std::string part;
    while (std::getline(iss, part, ',')) {
        while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) {
            part.erase(part.begin());
        }
        while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) {
            part.pop_back();
        }
        if (!part.empty()) {
            out.push_back(part);
        }
    }
}
}  // namespace

AppConfig loadConfigFromEnv() {
    AppConfig config;
    config.dbClientName = getenvOrDefault("DB_CLIENT_NAME", "default");
    config.enforceTrustedRegistryPrefixes = getenvBool("ENFORCE_TRUSTED_REGISTRY_PREFIXES", false);
    parseCommaList(getenvOrDefault("TRUSTED_REGISTRY_PREFIXES", ""), config.trustedRegistryPrefixes);
    config.registryPullTimeoutSeconds = getenvInt("REGISTRY_PULL_TIMEOUT_SEC", 300);
    if (config.registryPullTimeoutSeconds < 30) {
        config.registryPullTimeoutSeconds = 30;
    }
    return config;
}

}  // namespace coordinator::config
