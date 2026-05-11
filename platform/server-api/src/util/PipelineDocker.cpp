#include "util/PipelineDocker.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

namespace coordinator::util {

namespace {

ssize_t writeAll(int fd, const char *buf, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        const ssize_t n = ::write(fd, buf + off, len - off);
        if (n <= 0) {
            return n;
        }
        off += static_cast<std::size_t>(n);
    }
    return static_cast<ssize_t>(len);
}

std::string readAll(int fd) {
    std::string acc;
    std::array<char, 8192> buf{};
    for (;;) {
        const ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n <= 0) {
            break;
        }
        acc.append(buf.data(), static_cast<std::size_t>(n));
    }
    return acc;
}

bool waitPidTimeout(pid_t pid, int timeoutSeconds, int &exitStatus) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    for (;;) {
        const pid_t w = waitpid(pid, &exitStatus, WNOHANG);
        if (w == pid) {
            return true;
        }
        if (w < 0) {
            return false;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            waitpid(pid, &exitStatus, 0);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

DockerRunResult runDockerChild(std::vector<std::string> argvStore,
                               const std::string *stdinUtf8,
                               int timeoutSeconds) {
    DockerRunResult out{};
    int inPipe[2]{};
    int outPipe[2]{};
    int errPipe[2]{};
    const bool needIn = stdinUtf8 != nullptr && !stdinUtf8->empty();
    if (needIn) {
        if (pipe(inPipe) != 0) {
            out.stderrText = "pipe() failed";
            return out;
        }
    }
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        out.stderrText = "pipe() failed";
        if (needIn) {
            close(inPipe[0]);
            close(inPipe[1]);
        }
        return out;
    }

    std::vector<char *> argv;
    argv.reserve(argvStore.size() + 1);
    for (auto &s : argvStore) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        out.stderrText = "fork() failed";
        if (needIn) {
            close(inPipe[0]);
            close(inPipe[1]);
        }
        close(outPipe[0]);
        close(outPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);
        return out;
    }

    if (pid == 0) {
        if (needIn) {
            dup2(inPipe[0], STDIN_FILENO);
            close(inPipe[1]);
        } else {
            const int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
        }
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(outPipe[0]);
        close(errPipe[0]);
        close(outPipe[1]);
        close(errPipe[1]);
        if (needIn) {
            close(inPipe[0]);
        }
        execvp("docker", argv.data());
        _exit(127);
    }

    if (needIn) {
        close(inPipe[0]);
        const std::string inData = *stdinUtf8 + "\n";
        writeAll(inPipe[1], inData.data(), inData.size());
        close(inPipe[1]);
    }
    close(outPipe[1]);
    close(errPipe[1]);

    out.stdoutText = readAll(outPipe[0]);
    out.stderrText = readAll(errPipe[0]);
    close(outPipe[0]);
    close(errPipe[0]);

    int status = 0;
    if (!waitPidTimeout(pid, timeoutSeconds, status)) {
        out.ok = false;
        if (out.stderrText.empty()) {
            out.stderrText = "docker timeout";
        } else {
            out.stderrText += "; docker timeout";
        }
        return out;
    }
    out.ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return out;
}

}  // namespace

bool dockerImageTrusted(const std::string &image,
                        const std::vector<std::string> &trustedPrefixes,
                        const bool enforce) {
    if (!enforce || trustedPrefixes.empty()) {
        return true;
    }
    for (const auto &p : trustedPrefixes) {
        if (p.empty()) {
            continue;
        }
        if (image.size() >= p.size() && image.compare(0, p.size(), p) == 0) {
            return true;
        }
    }
    return false;
}

DockerRunResult dockerRegistryLogin(const std::string &registryHost,
                                    const std::string &username,
                                    const std::string &password,
                                    const int timeoutSeconds) {
    if (username.empty()) {
        DockerRunResult ok{};
        ok.ok = true;
        return ok;
    }
    std::vector<std::string> argv;
    argv.push_back("docker");
    argv.push_back("login");
    if (!registryHost.empty()) {
        argv.push_back(registryHost);
    }
    argv.push_back("-u");
    argv.push_back(username);
    argv.push_back("--password-stdin");
    return runDockerChild(argv, &password, timeoutSeconds);
}

DockerRunResult dockerPull(const std::string &image, const int timeoutSeconds) {
    std::vector<std::string> argv{"docker", "pull", image};
    return runDockerChild(argv, nullptr, timeoutSeconds);
}

DockerRunResult dockerRun(const std::string &image,
                          const std::vector<std::string> &argvAfterImage,
                          const std::string &stdinUtf8,
                          const int timeoutSeconds) {
    std::vector<std::string> argvStore;
    argvStore.reserve(argvAfterImage.size() + 8);
    argvStore.push_back("docker");
    argvStore.push_back("run");
    argvStore.push_back("--rm");
    argvStore.push_back("-i");
    argvStore.push_back(image);
    for (const auto &a : argvAfterImage) {
        argvStore.push_back(a);
    }
    return runDockerChild(argvStore, &stdinUtf8, timeoutSeconds);
}

DockerRunResult dockerRunWithPull(const std::string &image,
                                 const std::vector<std::string> &argvAfterImage,
                                 const std::string &stdinUtf8,
                                 const int runTimeoutSeconds,
                                 const std::string &registryServer,
                                 const std::string &registryUsername,
                                 const std::string &registryPassword,
                                 const int pullPhaseTimeoutSeconds,
                                 const std::vector<std::string> &trustedPrefixes,
                                 const bool enforceTrustedPrefixes) {
    DockerRunResult out{};
    if (!dockerImageTrusted(image, trustedPrefixes, enforceTrustedPrefixes)) {
        out.stderrText = "image not allowed by TRUSTED_REGISTRY_PREFIXES policy";
        return out;
    }

    const int loginTimeout = std::min(120, std::max(30, pullPhaseTimeoutSeconds));
    if (!registryUsername.empty()) {
        const std::string host = registryServer.empty() ? std::string() : registryServer;
        const auto login = dockerRegistryLogin(host, registryUsername, registryPassword, loginTimeout);
        if (!login.ok) {
            out.stderrText = "docker login failed: " + login.stderrText;
            if (!login.stdoutText.empty()) {
                out.stderrText += " ";
                out.stderrText += login.stdoutText;
            }
            return out;
        }
    }

    const auto pull = dockerPull(image, pullPhaseTimeoutSeconds);
    if (!pull.ok) {
        out.stderrText = "docker pull failed: " + pull.stderrText;
        if (!pull.stdoutText.empty()) {
            out.stderrText += " ";
            out.stderrText += pull.stdoutText;
        }
        return out;
    }

    std::vector<std::string> argvStore;
    argvStore.reserve(argvAfterImage.size() + 10);
    argvStore.push_back("docker");
    argvStore.push_back("run");
    argvStore.push_back("--pull=missing");
    argvStore.push_back("--rm");
    argvStore.push_back("-i");
    argvStore.push_back(image);
    for (const auto &a : argvAfterImage) {
        argvStore.push_back(a);
    }
    return runDockerChild(argvStore, &stdinUtf8, runTimeoutSeconds);
}

}  // namespace coordinator::util
