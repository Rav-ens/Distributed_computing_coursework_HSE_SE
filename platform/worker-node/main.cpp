// Worker loop: heartbeat, poll tasks, run docker map, post result.

#include <curl/curl.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <vector>

namespace {

using json = nlohmann::json;

std::string getenv_or(const char *name, const char *fallback) {
    const char *v = std::getenv(name);
    return (v && v[0]) ? std::string(v) : std::string(fallback);
}

std::vector<std::string> split_ws(const std::string &s) {
    std::istringstream iss(s);
    std::vector<std::string> out;
    std::string w;
    while (iss >> w) {
        out.push_back(std::move(w));
    }
    return out;
}

std::string trim_ws_copy(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    return s;
}

bool has_http_prefix(const std::string &s) {
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *s = static_cast<std::string *>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

json http_json(const std::string &method, const std::string &url, std::optional<json> body = std::nullopt) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }
    std::string response;
    std::string payload;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        const json to_send = body.value_or(json::object());
        payload = to_send.dump();
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    } else if (method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        throw std::runtime_error("unsupported HTTP method: " + method);
    }

    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("curl: ") + curl_easy_strerror(rc));
    }
    if (http_code < 200 || http_code >= 300) {
        throw std::runtime_error("HTTP " + std::to_string(http_code) + ": " + response);
    }
    if (response.empty()) {
        return json::object();
    }
    return json::parse(response);
}

std::string random_hex(std::size_t nbytes) {
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    std::string out;
    out.resize(nbytes * 2, '0');
    std::vector<unsigned char> buf(nbytes);
    if (urandom) {
        urandom.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
    } else {
        for (auto &b : buf) {
            b = static_cast<unsigned char>(rand() & 0xff);
        }
    }
    static const char *hex = "0123456789abcdef";
    for (std::size_t i = 0; i < nbytes; ++i) {
        out[i * 2] = hex[(buf[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex[buf[i] & 0xf];
    }
    return out;
}

std::string default_worker_host() {
    char hostname[256]{};
    if (gethostname(hostname, sizeof(hostname) - 1) != 0) {
        std::strncpy(hostname, "host", sizeof(hostname) - 1);
    }
    return std::string(hostname) + "-" + random_hex(5);
}

struct DockerOutcome {
    bool ok{};
    std::string stdout_text;
    std::string stderr_text;
};

DockerOutcome run_docker(const std::vector<std::string> &argv_s,
                         const std::string *stdin_payload,
                         int timeout_sec) {
    int in_fds[2]{};
    int out_fds[2]{};
    int err_fds[2]{};
    const bool need_in = stdin_payload != nullptr;
    if (need_in && pipe(in_fds) != 0) {
        return {false, "", "pipe() failed"};
    }
    if (pipe(out_fds) != 0 || pipe(err_fds) != 0) {
        if (need_in) {
            close(in_fds[0]);
            close(in_fds[1]);
        }
        return {false, "", "pipe() failed"};
    }

    std::vector<char *> argv;
    argv.reserve(argv_s.size() + 1);
    for (auto &s : const_cast<std::vector<std::string> &>(argv_s)) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        if (need_in) {
            close(in_fds[0]);
            close(in_fds[1]);
        }
        close(out_fds[0]);
        close(out_fds[1]);
        close(err_fds[0]);
        close(err_fds[1]);
        return {false, "", "fork() failed"};
    }

    if (pid == 0) {
        if (need_in) {
            dup2(in_fds[0], STDIN_FILENO);
            close(in_fds[1]);
        } else {
            const int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
        }
        dup2(out_fds[1], STDOUT_FILENO);
        dup2(err_fds[1], STDERR_FILENO);
        close(out_fds[0]);
        close(err_fds[0]);
        close(out_fds[1]);
        close(err_fds[1]);
        if (need_in) {
            close(in_fds[0]);
        }
        execvp("docker", argv.data());
        _exit(127);
    }

    if (need_in) {
        close(in_fds[0]);
        const std::string in_data = *stdin_payload + "\n";
        if (write(in_fds[1], in_data.data(), in_data.size()) != static_cast<ssize_t>(in_data.size())) {
        }
        close(in_fds[1]);
    }
    close(out_fds[1]);
    close(err_fds[1]);

    auto read_all = [](int fd) {
        std::string acc;
        char buf[4096];
        for (;;) {
            const ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            acc.append(buf, static_cast<std::size_t>(n));
        }
        return acc;
    };

    std::string out = read_all(out_fds[0]);
    std::string err = read_all(err_fds[0]);
    close(out_fds[0]);
    close(err_fds[0]);

    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    for (;;) {
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            break;
        }
        if (w < 0) {
            return {false, out, err + " waitpid failed"};
        }
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return {false, out, "docker timeout"};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return {ok, out, err};
}

DockerOutcome docker_registry_login(const std::string &registry_server,
                                    const std::string &username,
                                    const std::string &password) {
    std::vector<std::string> argv{"docker", "login"};
    if (!registry_server.empty()) {
        argv.push_back(registry_server);
    }
    argv.push_back("-u");
    argv.push_back(username);
    argv.push_back("--password-stdin");
    return run_docker(argv, &password, 120);
}

DockerOutcome docker_pull_image(const std::string &image, int timeout_sec) {
    std::vector<std::string> argv{"docker", "pull", image};
    return run_docker(argv, nullptr, timeout_sec);
}

DockerOutcome docker_map(const std::string &image,
                         const std::vector<std::string> &map_args,
                         const std::string &stdin_payload,
                         int map_timeout_sec) {
    std::vector<std::string> argv_s{"docker", "run", "--rm", "-i", image};
    argv_s.insert(argv_s.end(), map_args.begin(), map_args.end());
    return run_docker(argv_s, &stdin_payload, map_timeout_sec);
}

void post_task_result(const std::string &base,
                      std::int64_t task_id,
                      std::int64_t node_id,
                      bool success,
                      std::int64_t execution_time_ms,
                      const json &result_json,
                      const std::string &error_message) {
    json body = {
        {"node_id", node_id},
        {"success", success},
        {"execution_time_ms", execution_time_ms},
    };
    if (success) {
        body["result_json"] = result_json;
    } else {
        body["error_message"] = error_message;
    }
    http_json("POST", base + "/tasks/" + std::to_string(task_id) + "/result", body);
}

}  // namespace

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::string base = getenv_or("COORDINATOR_URL", "http://194.87.98.244:8080");
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    const std::string map_cmd = getenv_or("DOCKER_MAP_CMD", "map");
    const std::vector<std::string> map_args = split_ws(map_cmd);
    const double hb_sec = std::stod(getenv_or("HEARTBEAT_SEC", "5"));
    const std::string host = getenv_or("WORKER_HOST", default_worker_host().c_str());

    std::optional<std::string> node_id_env;
    if (const char *nid = std::getenv("NODE_ID"); nid && nid[0]) {
        node_id_env = std::string(nid);
    }

    std::int64_t node_id = 0;
    try {
        if (node_id_env) {
            node_id = std::stoll(*node_id_env);
            std::cerr << "[worker] NODE_ID=" << node_id << "\n";
        } else {
            json reg = {{"host", host},
                         {"cpu_cores", std::stoi(getenv_or("WORKER_CPU", "4"))},
                         {"ram_mb", std::stoi(getenv_or("WORKER_RAM_MB", "8192"))},
                         {"performance_score", std::stod(getenv_or("WORKER_SCORE", "1.0"))}};
            const json resp = http_json("POST", base + "/nodes/register", reg);
            node_id = resp.at("node_id").get<std::int64_t>();
            std::cerr << "[worker] registered host=" << host << " node_id=" << node_id << "\n";
        }
    } catch (const std::exception &ex) {
        std::cerr << "[worker] register failed: " << ex.what() << "\n";
        curl_global_cleanup();
        return 1;
    }

    std::map<std::int64_t, json> projects_cache;

    while (true) {
        try {
            http_json("POST", base + "/nodes/" + std::to_string(node_id) + "/heartbeat", json::object());
        } catch (const std::exception &ex) {
            std::cerr << "[worker] heartbeat: " << ex.what() << "\n";
        }

        try {
            const json data = http_json("GET", base + "/nodes/" + std::to_string(node_id) + "/tasks");
            const json tasks = data.value("tasks", json::array());
            for (const auto &t : tasks) {
                const std::int64_t tid = t.at("id").get<std::int64_t>();
                const std::int64_t pid = t.at("project_id").get<std::int64_t>();
                std::string raw = t.value("payload_json", std::string("{}"));
                json payload_obj;
                try {
                    payload_obj = json::parse(raw);
                } catch (...) {
                    payload_obj = json{{"raw", std::move(raw)}};
                }

                if (!projects_cache.count(pid)) {
                    projects_cache[pid] = http_json("GET", base + "/projects/" + std::to_string(pid));
                }
                const json &proj = projects_cache[pid];

                std::string image;
                if (payload_obj.contains("docker_image") && payload_obj["docker_image"].is_string()) {
                    image = payload_obj["docker_image"].get<std::string>();
                }
                if (image.empty() && proj.contains("executor_url") && proj["executor_url"].is_string()) {
                    image = proj["executor_url"].get<std::string>();
                }
                image = trim_ws_copy(image);

                auto fail_task = [&](const std::string &msg) {
                    post_task_result(base, tid, node_id, false, 0, json::object(), msg);
                };

                if (image.empty() || has_http_prefix(image)) {
                    std::cerr << "[worker] task " << tid << ": executor_url must be Docker image name\n";
                    fail_task("executor_url must be Docker image name (e.g. myrepo/job:1)");
                    continue;
                }

                const int pull_sec = std::stoi(getenv_or("REGISTRY_PULL_TIMEOUT_SEC", "300"));
                const int map_sec = std::stoi(getenv_or("DOCKER_MAP_TIMEOUT_SEC", "300"));
                if (payload_obj.contains("registry_username") && payload_obj["registry_username"].is_string()) {
                    const std::string ru = payload_obj["registry_username"].get<std::string>();
                    if (!ru.empty()) {
                        const std::string rs =
                            payload_obj.contains("registry_server") && payload_obj["registry_server"].is_string()
                                ? payload_obj["registry_server"].get<std::string>()
                                : std::string();
                        const std::string rp =
                            payload_obj.contains("registry_password") && payload_obj["registry_password"].is_string()
                                ? payload_obj["registry_password"].get<std::string>()
                                : std::string();
                        const auto lg = docker_registry_login(rs, ru, rp);
                        if (!lg.ok) {
                            const std::string em =
                                !lg.stderr_text.empty() ? lg.stderr_text
                                                        : (!lg.stdout_text.empty() ? lg.stdout_text : "login failed");
                            fail_task(std::string("docker login: ") + em);
                            continue;
                        }
                    }
                }
                const auto pull = docker_pull_image(image, pull_sec);
                if (!pull.ok) {
                    const std::string em = !pull.stderr_text.empty() ? pull.stderr_text
                                            : (!pull.stdout_text.empty() ? pull.stdout_text : "pull failed");
                    fail_task(std::string("docker pull: ") + em);
                    continue;
                }

                json stdin_obj = payload_obj;
                stdin_obj.erase("registry_server");
                stdin_obj.erase("registry_username");
                stdin_obj.erase("registry_password");
                const std::string stdin_payload = stdin_obj.dump();
                const auto t0 = std::chrono::steady_clock::now();
                DockerOutcome run = docker_map(image, map_args, stdin_payload, map_sec);
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();

                if (run.ok) {
                    json result_json;
                    try {
                        result_json = run.stdout_text.empty() ? json{{"stdout", ""}} : json::parse(run.stdout_text);
                    } catch (...) {
                        result_json = json{{"stdout", run.stdout_text}, {"parse_error", true}};
                    }
                    post_task_result(base, tid, node_id, true, ms, result_json, "");
                    std::cerr << "[worker] task " << tid << " OK image=" << image << "\n";
                } else {
                    const std::string err_msg = !run.stderr_text.empty() ? run.stderr_text
                                                : !run.stdout_text.empty() ? run.stdout_text
                                                                           : std::string("docker failed");
                    post_task_result(base, tid, node_id, false, ms, json::object(), err_msg);
                    std::cerr << "[worker] task " << tid << " FAIL: " << err_msg << "\n";
                }
            }
        } catch (const std::exception &ex) {
            std::cerr << "[worker] poll/exec: " << ex.what() << "\n";
        }

        std::this_thread::sleep_for(std::chrono::duration<double>(hb_sec));
    }
}
