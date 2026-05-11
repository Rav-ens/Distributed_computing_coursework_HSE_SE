#pragma once

#include <string>
#include <vector>

namespace coordinator::util {

struct DockerRunResult {
    bool ok = false;
    std::string stdoutText;
    std::string stderrText;
};

bool dockerImageTrusted(const std::string &image,
                        const std::vector<std::string> &trustedPrefixes,
                        bool enforce);

DockerRunResult dockerRegistryLogin(const std::string &registryHost,
                                    const std::string &username,
                                    const std::string &password,
                                    int timeoutSeconds);

DockerRunResult dockerPull(const std::string &image, int timeoutSeconds);

DockerRunResult dockerRunWithPull(const std::string &image,
                                  const std::vector<std::string> &argvAfterImage,
                                  const std::string &stdinUtf8,
                                  int runTimeoutSeconds,
                                  const std::string &registryServer,
                                  const std::string &registryUsername,
                                  const std::string &registryPassword,
                                  int pullPhaseTimeoutSeconds,
                                  const std::vector<std::string> &trustedPrefixes,
                                  bool enforceTrustedPrefixes);

DockerRunResult dockerRun(const std::string &image,
                          const std::vector<std::string> &argvAfterImage,
                          const std::string &stdinUtf8,
                          int timeoutSeconds = 120);

}  // namespace coordinator::util
