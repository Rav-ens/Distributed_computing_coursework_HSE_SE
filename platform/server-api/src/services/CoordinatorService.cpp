#include "services/CoordinatorService.h"

#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <drogon/drogon.h>
#include <spdlog/spdlog.h>

#include "util/PipelineDocker.h"

using namespace std::chrono_literals;
using drogon::orm::Result;

namespace {

constexpr int kMaxPipelineSubtasks = 4096;
constexpr std::size_t kMaxDockerJsonBytes = 16 * 1024 * 1024;

bool parseJsonText(const std::string &s, Json::Value &out, std::string &err) {
    Json::CharReaderBuilder b;
    const std::unique_ptr<Json::CharReader> r(b.newCharReader());
    const char *begin = s.data();
    const char *end = begin + s.size();
    return r->parse(begin, end, &out, &err);
}

std::string trimCopy(std::string s) {
    while (!s.empty() && (static_cast<unsigned char>(s.front()) <= 32)) {
        s.erase(s.begin());
    }
    while (!s.empty() && (static_cast<unsigned char>(s.back()) <= 32)) {
        s.pop_back();
    }
    return s;
}

void mergePayloadRegistry(Json::Value &payload,
                          const std::string &server,
                          const std::string &username,
                          const std::string &password) {
    if (!server.empty()) {
        payload["registry_server"] = server;
    }
    if (!username.empty()) {
        payload["registry_username"] = username;
        payload["registry_password"] = password;
    }
}

void enforceExecutorImagePolicy(const std::string &image, const coordinator::config::AppConfig &cfg) {
    if (!coordinator::util::dockerImageTrusted(image, cfg.trustedRegistryPrefixes,
                                              cfg.enforceTrustedRegistryPrefixes)) {
        throw std::invalid_argument(
            "executor image not allowed: set ENFORCE_TRUSTED_REGISTRY_PREFIXES=0 or add TRUSTED_REGISTRY_PREFIXES "
            "prefix");
    }
}

}  // namespace

namespace coordinator::services {

CoordinatorService::CoordinatorService(drogon::orm::DbClientPtr dbClient, config::AppConfig config)
    : dbClient_(std::move(dbClient)), config_(std::move(config)) {}

std::string CoordinatorService::nodeStatusToString(models::NodeStatus status) {
    switch (status) {
        case models::NodeStatus::online:
            return "online";
        case models::NodeStatus::offline:
            return "offline";
        case models::NodeStatus::degraded:
            return "degraded";
    }
    return "offline";
}

std::string CoordinatorService::taskStatusToString(models::TaskStatus status) {
    switch (status) {
        case models::TaskStatus::queued:
            return "queued";
        case models::TaskStatus::assigned:
            return "assigned";
        case models::TaskStatus::running:
            return "running";
        case models::TaskStatus::succeeded:
            return "succeeded";
        case models::TaskStatus::failed:
            return "failed";
        case models::TaskStatus::retry_wait:
            return "retry_wait";
        case models::TaskStatus::stale:
            return "stale";
    }
    return "failed";
}

Json::Value CoordinatorService::registerNode(const Json::Value &request) {
    const auto host = request["host"].asString();
    const auto cpu = request.get("cpu_cores", 1).asInt();
    const auto ram = request.get("ram_mb", 1024).asInt();
    const auto score = request.get("performance_score", 1.0).asDouble();

    auto tx = dbClient_->newTransaction();
    const Result r = tx->execSqlSync(
        "INSERT INTO worker_nodes(host,status,cpu_cores,ram_mb,performance_score,last_heartbeat,"
        "throughput_history,failure_rate,current_load,avg_latency_ms) "
        "VALUES($1,'online',$2,$3,$4,NOW(),0,0,0,0) RETURNING id",
        host,
        cpu,
        ram,
        score);
    Json::Value out;
    out["node_id"] = static_cast<Json::Int64>(r[0]["id"].as<std::int64_t>());
    out["status"] = "registered";
    return out;
}

Json::Value CoordinatorService::heartbeat(std::int64_t nodeId) {
    auto tx = dbClient_->newTransaction();
    tx->execSqlSync("UPDATE worker_nodes SET last_heartbeat = NOW(), status='online' WHERE id=$1", nodeId);
    Json::Value out;
    out["node_id"] = static_cast<Json::Int64>(nodeId);
    out["heartbeat"] = "ok";
    return out;
}

Json::Value CoordinatorService::listNodes() {
    const Result r = dbClient_->execSqlSync(
        "SELECT id,host,status,cpu_cores,ram_mb,performance_score,last_heartbeat FROM worker_nodes ORDER BY id");
    Json::Value arr(Json::arrayValue);
    for (const auto &row : r) {
        Json::Value item;
        item["id"] = static_cast<Json::Int64>(row["id"].as<std::int64_t>());
        item["host"] = row["host"].as<std::string>();
        item["status"] = row["status"].as<std::string>();
        item["cpu_cores"] = row["cpu_cores"].as<int>();
        item["ram_mb"] = row["ram_mb"].as<int>();
        item["performance_score"] = row["performance_score"].as<double>();
        item["last_heartbeat"] = row["last_heartbeat"].as<std::string>();
        arr.append(item);
    }
    Json::Value out;
    out["nodes"] = arr;
    return out;
}

Json::Value CoordinatorService::createProject(const Json::Value &request) {
    const std::string image = request["executor_url"].asString();
    if (dockerImageNameInvalid(image)) {
        throw std::invalid_argument("executor_url must be a Docker image reference (not http(s) URL)");
    }
    enforceExecutorImagePolicy(image, config_);
    const std::string regServer = request.get("registry_server", "").asString();
    const std::string regUser = request.get("registry_username", "").asString();
    const std::string regPass = request.get("registry_password", "").asString();
    auto tx = dbClient_->newTransaction();
    const Result r = tx->execSqlSync(
        "INSERT INTO projects(name,version,executor_url,checksum,registry_server,registry_username,"
        "registry_password_secret,created_at) VALUES($1,$2,$3,$4,$5,$6,$7,NOW()) RETURNING id",
        request["name"].asString(),
        request["version"].asString(),
        image,
        request["checksum"].asString(),
        regServer,
        regUser,
        regPass);
    Json::Value out;
    out["project_id"] = static_cast<Json::Int64>(r[0]["id"].as<std::int64_t>());
    return out;
}

Json::Value CoordinatorService::updateProject(const std::int64_t projectId, const Json::Value &request) {
    const Result cur =
        dbClient_->execSqlSync("SELECT name,version,executor_url,checksum,registry_server,registry_username,"
                               "registry_password_secret FROM projects WHERE id=$1",
                               projectId);
    if (cur.empty()) {
        throw std::runtime_error("project not found");
    }
    std::string name = cur[0]["name"].as<std::string>();
    std::string version = cur[0]["version"].as<std::string>();
    std::string executor = cur[0]["executor_url"].as<std::string>();
    std::string checksum = cur[0]["checksum"].as<std::string>();
    std::string regServer = cur[0]["registry_server"].as<std::string>();
    std::string regUser = cur[0]["registry_username"].as<std::string>();
    std::string regPass = cur[0]["registry_password_secret"].as<std::string>();
    if (request.isMember("name")) {
        name = request["name"].asString();
    }
    if (request.isMember("version")) {
        version = request["version"].asString();
    }
    if (request.isMember("executor_url")) {
        executor = request["executor_url"].asString();
    }
    if (request.isMember("checksum")) {
        checksum = request["checksum"].asString();
    }
    if (request.isMember("registry_server")) {
        regServer = request["registry_server"].asString();
    }
    if (request.isMember("registry_username")) {
        regUser = request["registry_username"].asString();
    }
    if (request.isMember("registry_password")) {
        regPass = request["registry_password"].asString();
    }
    if (dockerImageNameInvalid(executor)) {
        throw std::invalid_argument("executor_url must be a Docker image reference (not http(s) URL)");
    }
    enforceExecutorImagePolicy(executor, config_);
    dbClient_->execSqlSync(
        "UPDATE projects SET name=$2,version=$3,executor_url=$4,checksum=$5,registry_server=$6,"
        "registry_username=$7,registry_password_secret=$8 WHERE id=$1",
        projectId,
        name.c_str(),
        version.c_str(),
        executor.c_str(),
        checksum.c_str(),
        regServer.c_str(),
        regUser.c_str(),
        regPass.c_str());
    return getProject(projectId);
}

Json::Value CoordinatorService::getProject(std::int64_t projectId) {
    const Result r = dbClient_->execSqlSync(
        "SELECT id,name,version,executor_url,checksum,created_at,registry_server,registry_username,"
        "CASE WHEN COALESCE(registry_username,'') <> '' OR COALESCE(registry_password_secret,'') <> '' "
        "THEN 1 ELSE 0 END AS has_creds "
        "FROM projects WHERE id=$1",
        projectId);
    if (r.empty()) {
        throw std::runtime_error("project not found");
    }
    const auto &row = r[0];
    Json::Value item;
    item["id"] = static_cast<Json::Int64>(row["id"].as<std::int64_t>());
    item["name"] = row["name"].as<std::string>();
    item["version"] = row["version"].as<std::string>();
    item["executor_url"] = row["executor_url"].as<std::string>();
    item["checksum"] = row["checksum"].as<std::string>();
    item["created_at"] = row["created_at"].as<std::string>();
    item["registry_server"] = row["registry_server"].as<std::string>();
    item["registry_username"] = row["registry_username"].as<std::string>();
    item["has_registry_credentials"] = row["has_creds"].as<int>() != 0;
    return item;
}

Json::Value CoordinatorService::listProjects() {
    const Result r = dbClient_->execSqlSync(
        "SELECT id,name,version,executor_url,checksum,created_at,registry_server,registry_username,"
        "CASE WHEN COALESCE(registry_username,'') <> '' OR COALESCE(registry_password_secret,'') <> '' "
        "THEN 1 ELSE 0 END AS has_creds "
        "FROM projects ORDER BY id DESC");
    Json::Value arr(Json::arrayValue);
    for (const auto &row : r) {
        Json::Value item;
        item["id"] = static_cast<Json::Int64>(row["id"].as<std::int64_t>());
        item["name"] = row["name"].as<std::string>();
        item["version"] = row["version"].as<std::string>();
        item["executor_url"] = row["executor_url"].as<std::string>();
        item["checksum"] = row["checksum"].as<std::string>();
        item["created_at"] = row["created_at"].as<std::string>();
        item["registry_server"] = row["registry_server"].as<std::string>();
        item["registry_username"] = row["registry_username"].as<std::string>();
        item["has_registry_credentials"] = row["has_creds"].as<int>() != 0;
        arr.append(item);
    }
    Json::Value out;
    out["projects"] = arr;
    return out;
}

Json::Value CoordinatorService::generateTasks(std::int64_t projectId, const Json::Value &request) {
    const int count = request.get("count", 10).asInt();
    const int maxAttempts = request.get("max_attempts", 5).asInt();
    const int priority = request.get("priority", 10).asInt();

    auto tx = dbClient_->newTransaction();
    for (int i = 0; i < count; ++i) {
        Json::Value payload;
        payload["chunk_id"] = i;
        payload["value"] = request.get("base_payload", Json::Value{});
        const std::string payloadStr = Json::writeString(Json::StreamWriterBuilder(), payload);
        tx->execSqlSync(
            "INSERT INTO tasks(project_id,payload_json,priority,status,attempts,max_attempts,created_at) "
            "VALUES($1,$2::jsonb,$3,'queued',0,$4,NOW())",
            projectId,
            payloadStr,
            priority,
            maxAttempts);
    }
    Json::Value out;
    out["generated"] = count;
    return out;
}

Json::Value CoordinatorService::submitProjectTask(std::int64_t projectId, const Json::Value &request) {
    if (!request.isMember("payload") || !request["payload"].isObject()) {
        throw std::invalid_argument("JSON must contain object field \"payload\"");
    }
    const int maxAttempts = request.get("max_attempts", 5).asInt();
    const int priority = request.get("priority", 10).asInt();
    Json::Value payload = request["payload"];
    const Result pr = dbClient_->execSqlSync(
        "SELECT registry_server,registry_username,registry_password_secret FROM projects WHERE id=$1", projectId);
    if (!pr.empty()) {
        mergePayloadRegistry(payload, pr[0]["registry_server"].as<std::string>(),
                             pr[0]["registry_username"].as<std::string>(),
                             pr[0]["registry_password_secret"].as<std::string>());
    }
    const std::string payloadStr = Json::writeString(Json::StreamWriterBuilder(), payload);
    auto tx = dbClient_->newTransaction();
    const Result r = tx->execSqlSync(
        "INSERT INTO tasks(project_id,payload_json,priority,status,attempts,max_attempts,created_at) "
        "VALUES($1,$2::jsonb,$3,'queued',0,$4,NOW()) RETURNING id",
        projectId,
        payloadStr,
        priority,
        maxAttempts);
    Json::Value out;
    out["task_id"] = static_cast<Json::Int64>(r[0]["id"].as<std::int64_t>());
    out["status"] = "queued";
    return out;
}

std::vector<models::WorkerNode> CoordinatorService::loadOnlineNodes() {
    const Result r = dbClient_->execSqlSync(
        "SELECT id,host,status,cpu_cores,ram_mb,performance_score,last_heartbeat,"
        "throughput_history,failure_rate,current_load,avg_latency_ms "
        "FROM worker_nodes WHERE status='online'");
    std::vector<models::WorkerNode> nodes;
    nodes.reserve(r.size());
    for (const auto &row : r) {
        models::WorkerNode n;
        n.id = row["id"].as<std::int64_t>();
        n.host = row["host"].as<std::string>();
        n.cpuCores = row["cpu_cores"].as<int>();
        n.ramMb = row["ram_mb"].as<int>();
        n.performanceScore = row["performance_score"].as<double>();
        n.throughputHistory = row["throughput_history"].as<double>();
        n.failureRate = row["failure_rate"].as<double>();
        n.currentLoad = row["current_load"].as<int>();
        n.avgLatencyMs = row["avg_latency_ms"].as<double>();
        nodes.push_back(std::move(n));
    }
    return nodes;
}

std::optional<models::TaskDescriptor> CoordinatorService::fetchNextQueueTask() {
    const Result r = dbClient_->execSqlSync(
        "SELECT id,project_id,payload_json,priority,status,assigned_node_id,attempts,max_attempts "
        "FROM tasks WHERE status='queued' AND (next_retry_at IS NULL OR next_retry_at <= NOW()) "
        "ORDER BY priority DESC, created_at ASC LIMIT 1");
    if (r.empty()) {
        return std::nullopt;
    }
    models::TaskDescriptor t;
    const auto &row = r[0];
    t.id = row["id"].as<std::int64_t>();
    t.projectId = row["project_id"].as<std::int64_t>();
    t.payloadJson = row["payload_json"].as<std::string>();
    t.priority = row["priority"].as<int>();
    t.attempts = row["attempts"].as<int>();
    t.maxAttempts = row["max_attempts"].as<int>();
    return t;
}

void CoordinatorService::ensureExecutorInstalled(std::int64_t nodeId, std::int64_t projectId) {
    const Result p = dbClient_->execSqlSync("SELECT version,checksum FROM projects WHERE id=$1", projectId);
    if (p.empty()) {
        throw std::runtime_error("project not found");
    }
    const auto version = p[0]["version"].as<std::string>();
    const auto checksum = p[0]["checksum"].as<std::string>();

    const Result current =
        dbClient_->execSqlSync("SELECT installed_version,status FROM node_executor_install "
                               "WHERE node_id=$1 AND project_id=$2",
                               nodeId,
                               projectId);
    if (!current.empty() && current[0]["installed_version"].as<std::string>() == version &&
        current[0]["status"].as<std::string>() == "installed") {
        return;
    }

    auto tx = dbClient_->newTransaction();
    tx->execSqlSync("INSERT INTO node_executor_install(node_id,project_id,installed_version,status,installed_at) "
                    "VALUES($1,$2,$3,'installed',NOW()) "
                    "ON CONFLICT(node_id,project_id) DO UPDATE SET installed_version=EXCLUDED.installed_version,"
                    "status='installed',installed_at=NOW()",
                    nodeId,
                    projectId,
                    version);
    spdlog::info("executor version={} checksum={} installed for node={} project={}", version, checksum,
                 nodeId, projectId);
}

Json::Value CoordinatorService::assignPendingTasks() {
    int assigned = 0;
    while (true) {
        auto task = fetchNextQueueTask();
        if (!task) {
            break;
        }
        auto nodes = loadOnlineNodes();
        if (nodes.empty()) {
            break;
        }
        auto selected = weighted_.selectNode(*task, nodes);
        if (!selected) {
            break;
        }
        ensureExecutorInstalled(selected->id, task->projectId);
        auto tx = dbClient_->newTransaction();
        const int timeoutSec = static_cast<int>(config_.taskTimeout.count());
        tx->execSqlSync(std::string("UPDATE tasks SET status='assigned',assigned_node_id=$1,started_at=NOW(),"
                                    "timeout_at=NOW() + interval '") +
                            std::to_string(timeoutSec) + " seconds' WHERE id=$2",
                        selected->id,
                        task->id);
        tx->execSqlSync("UPDATE worker_nodes SET current_load=current_load+1 WHERE id=$1", selected->id);
        ++assigned;
    }

    Json::Value out;
    out["assigned"] = assigned;
    out["strategy"] = currentStrategyName();
    return out;
}

Json::Value CoordinatorService::getTasksByStatus(const std::string &status) {
    const Result r = dbClient_->execSqlSync(
        "SELECT id,project_id,priority,status,assigned_node_id,attempts,max_attempts,job_id,is_job_subtask,"
        "created_at,started_at,finished_at FROM tasks WHERE status=$1 ORDER BY id",
        status);
    Json::Value arr(Json::arrayValue);
    for (const auto &row : r) {
        Json::Value item;
        item["id"] = static_cast<Json::Int64>(row["id"].as<std::int64_t>());
        item["project_id"] = static_cast<Json::Int64>(row["project_id"].as<std::int64_t>());
        item["priority"] = row["priority"].as<int>();
        item["status"] = row["status"].as<std::string>();
        if (!row["assigned_node_id"].isNull()) {
            item["assigned_node_id"] = static_cast<Json::Int64>(row["assigned_node_id"].as<std::int64_t>());
        }
        item["attempts"] = row["attempts"].as<int>();
        item["max_attempts"] = row["max_attempts"].as<int>();
        if (!row["job_id"].isNull()) {
            item["job_id"] = static_cast<Json::Int64>(row["job_id"].as<std::int64_t>());
        }
        item["is_job_subtask"] = row["is_job_subtask"].as<bool>();
        arr.append(item);
    }
    Json::Value out;
    out["tasks"] = arr;
    return out;
}

Json::Value CoordinatorService::getTasksForNode(std::int64_t nodeId) {
    const Result r = dbClient_->execSqlSync(
        "SELECT id,project_id,payload_json,priority,status,assigned_node_id,attempts,max_attempts,job_id,"
        "is_job_subtask,created_at,started_at,finished_at FROM tasks WHERE assigned_node_id=$1 AND status IN "
        "('assigned','running') "
        "ORDER BY priority DESC, id ASC",
        nodeId);
    Json::Value arr(Json::arrayValue);
    for (const auto &row : r) {
        Json::Value item;
        item["id"] = static_cast<Json::Int64>(row["id"].as<std::int64_t>());
        item["project_id"] = static_cast<Json::Int64>(row["project_id"].as<std::int64_t>());
        item["payload_json"] = row["payload_json"].as<std::string>();
        item["priority"] = row["priority"].as<int>();
        item["status"] = row["status"].as<std::string>();
        item["assigned_node_id"] = static_cast<Json::Int64>(row["assigned_node_id"].as<std::int64_t>());
        item["attempts"] = row["attempts"].as<int>();
        item["max_attempts"] = row["max_attempts"].as<int>();
        if (!row["job_id"].isNull()) {
            item["job_id"] = static_cast<Json::Int64>(row["job_id"].as<std::int64_t>());
        }
        item["is_job_subtask"] = row["is_job_subtask"].as<bool>();
        arr.append(item);
    }
    Json::Value out;
    out["node_id"] = static_cast<Json::Int64>(nodeId);
    out["tasks"] = arr;
    return out;
}

Json::Value CoordinatorService::submitResult(std::int64_t taskId, const Json::Value &request) {
    const auto nodeId = request["node_id"].asInt64();
    const auto success = request.get("success", false).asBool();
    const auto executionTime = request.get("execution_time_ms", 0).asInt64();
    const std::string resultJson = Json::writeString(Json::StreamWriterBuilder(), request.get("result_json", Json::Value{}));
    const auto errorMsg = request.get("error_message", "").asString();

    bool duplicated = false;
    {
        auto tx = dbClient_->newTransaction();
        const Result insertResult = tx->execSqlSync(
            "INSERT INTO task_results(task_id,node_id,success,result_json,error_message,execution_time_ms) "
            "VALUES($1,$2,$3,$4::jsonb,$5,$6) ON CONFLICT(task_id,node_id) DO NOTHING RETURNING id",
            taskId,
            nodeId,
            success,
            resultJson,
            errorMsg,
            executionTime);

        duplicated = insertResult.empty();
        if (!duplicated) {
            if (success) {
                tx->execSqlSync("UPDATE tasks SET status='succeeded',finished_at=NOW() WHERE id=$1", taskId);
            } else {
                const Result task = tx->execSqlSync("SELECT attempts,max_attempts FROM tasks WHERE id=$1", taskId);
                const int attempts = task[0]["attempts"].as<int>() + 1;
                const int maxAttempts = task[0]["max_attempts"].as<int>();
                if (attempts < maxAttempts) {
                    const int backoffSec = static_cast<int>(std::pow(2.0, attempts));
                    tx->execSqlSync(std::string("UPDATE tasks SET attempts=$1,status='queued',assigned_node_id=NULL,"
                                                "started_at=NULL,next_retry_at=NOW() + interval '") +
                                        std::to_string(backoffSec) + " seconds' WHERE id=$2",
                                    attempts,
                                    taskId);
                } else {
                    tx->execSqlSync("UPDATE tasks SET attempts=$1,status='failed',finished_at=NOW() WHERE id=$2",
                                    attempts,
                                    taskId);
                }
            }
            tx->execSqlSync("UPDATE worker_nodes SET current_load=GREATEST(current_load-1,0) WHERE id=$1", nodeId);
        }
    }

    Json::Value out;
    out["task_id"] = static_cast<Json::Int64>(taskId);
    out["idempotent_duplicate"] = duplicated;
    out["status"] = success ? "accepted" : "accepted_with_retry";

    try {
        onPipelineSubtaskFinished(taskId, success, duplicated);
    } catch (const std::exception &ex) {
        spdlog::error("onPipelineSubtaskFinished: {}", ex.what());
        throw;
    }
    return out;
}

Json::Value CoordinatorService::projectResultSummary(std::int64_t projectId) {
    const Result r = dbClient_->execSqlSync(
        "SELECT COUNT(*)::bigint AS total, "
        "COALESCE(SUM(CASE WHEN t.status='succeeded' THEN 1 ELSE 0 END), 0)::bigint AS succeeded, "
        "COALESCE(SUM(CASE WHEN t.status='failed' THEN 1 ELSE 0 END), 0)::bigint AS failed "
        "FROM tasks t WHERE t.project_id=$1",
        projectId);
    Json::Value out;
    out["project_id"] = static_cast<Json::Int64>(projectId);
    out["total"] = r[0]["total"].as<std::int64_t>();
    out["succeeded"] = r[0]["succeeded"].as<std::int64_t>();
    out["failed"] = r[0]["failed"].as<std::int64_t>();
    return out;
}

std::string CoordinatorService::currentStrategyName() const {
    return weighted_.name();
}

Json::Value CoordinatorService::health() {
    Json::Value out;
    out["service"] = "coordinator";
    out["strategy"] = currentStrategyName();
    if (!dbClient_) {
        out["db"] = "no_client";
        return out;
    }
    try {
        const Result r = dbClient_->execSqlSync("SELECT 1");
        out["db"] = r.empty() ? "down" : "up";
    } catch (const std::exception &ex) {
        out["db"] = "error";
        out["db_error"] = ex.what();
    }
    return out;
}

Json::Value CoordinatorService::recoverStaleNodesAndTasks() {
    auto tx = dbClient_->newTransaction();
    const int hbSec = static_cast<int>(config_.heartbeatTimeout.count());
    tx->execSqlSync(std::string("UPDATE worker_nodes SET status='offline' "
                                "WHERE last_heartbeat < NOW() - interval '") +
                    std::to_string(hbSec) + " seconds'");

    tx->execSqlSync("UPDATE tasks SET status='queued',assigned_node_id=NULL,started_at=NULL,"
                    "next_retry_at=NOW()+interval '1 second' "
                    "WHERE status IN ('assigned','running') AND "
                    "(timeout_at IS NOT NULL AND timeout_at < NOW())");

    tx->execSqlSync("UPDATE tasks t SET status='queued',assigned_node_id=NULL,started_at=NULL,"
                    "next_retry_at=NOW()+interval '1 second' "
                    "FROM worker_nodes n WHERE t.assigned_node_id=n.id AND t.status IN ('assigned','running') "
                    "AND n.status='offline'");

    Json::Value out;
    out["recovery"] = "ok";
    return out;
}

bool CoordinatorService::dockerImageNameInvalid(const std::string &image) {
    if (image.empty()) {
        return true;
    }
    if (image.rfind("http://", 0) == 0 || image.rfind("https://", 0) == 0) {
        return true;
    }
    for (char c : image) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            return true;
        }
    }
    return false;
}

Json::Value CoordinatorService::createPipelineJob(std::int64_t projectId, const Json::Value &request) {
    if (!request.isMember("input")) {
        throw std::invalid_argument(R"(JSON must contain field "input")");
    }
    const Json::Value proj = getProject(projectId);
    const std::string image = request.get("executor_url", proj["executor_url"]).asString();
    if (dockerImageNameInvalid(image)) {
        throw std::invalid_argument("executor_url must be a Docker image name (not http(s) URL)");
    }
    enforceExecutorImagePolicy(image, config_);
    const int priority = request.get("priority", 10).asInt();
    const int maxAttempts = request.get("max_attempts", 5).asInt();
    const std::string inputStr = Json::writeString(Json::StreamWriterBuilder(), request["input"]);
    const Result preg = dbClient_->execSqlSync(
        "SELECT registry_server,registry_username,registry_password_secret FROM projects WHERE id=$1", projectId);
    const std::string regS = preg.empty() ? "" : preg[0]["registry_server"].as<std::string>();
    const std::string regU = preg.empty() ? "" : preg[0]["registry_username"].as<std::string>();
    const std::string regP = preg.empty() ? "" : preg[0]["registry_password_secret"].as<std::string>();
    auto tx = dbClient_->newTransaction();
    const Result r =
        tx->execSqlSync("INSERT INTO jobs(project_id,status,input_json,executor_image,children_total,priority,"
                        "max_attempts_children,registry_server,registry_username,registry_password_secret) "
                        "VALUES($1,'pending_split',$2::jsonb,$3,0,$4,$5,$6,$7,$8) RETURNING id",
                        projectId,
                        inputStr,
                        image,
                        priority,
                        maxAttempts,
                        regS.c_str(),
                        regU.c_str(),
                        regP.c_str());
    Json::Value out;
    out["job_id"] = static_cast<Json::Int64>(r[0]["id"].as<std::int64_t>());
    out["status"] = "pending_split";
    return out;
}

Json::Value CoordinatorService::getPipelineJob(std::int64_t jobId) {
    const Result r =
        dbClient_->execSqlSync("SELECT j.id,j.project_id,j.status,j.executor_image,j.children_total,"
                               "j.error_message,j.created_at,j.finished_at,"
                               "(SELECT COUNT(*)::bigint FROM tasks t WHERE t.job_id=j.id AND t.is_job_subtask=TRUE "
                               "AND t.status='succeeded') AS succeeded,"
                               "(SELECT COUNT(*)::bigint FROM tasks t WHERE t.job_id=j.id AND t.is_job_subtask=TRUE "
                               "AND t.status='failed') AS failed_subtasks "
                               "FROM jobs j WHERE j.id=$1",
                               jobId);
    if (r.empty()) {
        throw std::runtime_error("job not found");
    }
    const auto &row = r[0];
    Json::Value item;
    item["id"] = static_cast<Json::Int64>(row["id"].as<std::int64_t>());
    item["project_id"] = static_cast<Json::Int64>(row["project_id"].as<std::int64_t>());
    item["status"] = row["status"].as<std::string>();
    item["executor_image"] = row["executor_image"].as<std::string>();
    item["children_total"] = row["children_total"].as<int>();
    item["succeeded"] = static_cast<Json::Int64>(row["succeeded"].as<std::int64_t>());
    item["failed_subtasks"] = static_cast<Json::Int64>(row["failed_subtasks"].as<std::int64_t>());
    item["error_message"] = row["error_message"].as<std::string>();
    item["created_at"] = row["created_at"].as<std::string>();
    if (!row["finished_at"].isNull()) {
        item["finished_at"] = row["finished_at"].as<std::string>();
    }
    return item;
}

Json::Value CoordinatorService::getPipelineJobResult(std::int64_t jobId) {
    const Result r = dbClient_->execSqlSync(
        "SELECT j.status, j.result_json::text, j.error_message, j.children_total,"
        "(SELECT COUNT(*)::bigint FROM tasks t WHERE t.job_id=j.id AND t.is_job_subtask=TRUE AND "
        "t.status='succeeded') AS succeeded "
        "FROM jobs j WHERE j.id=$1",
        jobId);
    if (r.empty()) {
        throw std::runtime_error("job not found");
    }
    const auto &row = r[0];
    const std::string st = row["status"].as<std::string>();
    if (st != "completed") {
        const int childrenTotal = row["children_total"].isNull() ? 0 : row["children_total"].as<int>();
        const std::int64_t succeeded = row["succeeded"].isNull() ? 0 : row["succeeded"].as<std::int64_t>();
        std::string msg = "job not completed: status=" + st + " succeeded=" + std::to_string(succeeded) + "/" +
                          std::to_string(childrenTotal);
        if (st == "failed" || st == "failed_split") {
            msg += " error_message=" + row["error_message"].as<std::string>();
        } else {
            msg += " (poll GET /pipeline-jobs/" + std::to_string(jobId) + " for progress)";
        }
        throw std::runtime_error(msg);
    }
    Json::Value out;
    out["job_id"] = static_cast<Json::Int64>(jobId);
    out["status"] = "completed";
    std::string err;
    Json::Value res;
    const std::string raw = row["result_json"].isNull() ? "{}" : row["result_json"].as<std::string>();
    if (!parseJsonText(raw, res, err)) {
        Json::Value rawWrap;
        rawWrap["parse_error"] = err;
        rawWrap["raw"] = raw;
        out["result_json"] = rawWrap;
    } else {
        out["result_json"] = res;
    }
    return out;
}

void CoordinatorService::failPipelineJob(std::int64_t jobId, const std::string &reason) {
    dbClient_->execSqlSync(
        "UPDATE jobs SET status='failed', finished_at=NOW(), error_message=$2 "
        "WHERE id=$1 AND status NOT IN ('completed','failed','failed_split')",
        jobId,
        reason.c_str());
}

void CoordinatorService::onPipelineSubtaskFinished(std::int64_t taskId, bool success, bool idempotentDuplicate) {
    if (idempotentDuplicate && !success) {
        return;
    }
    const Result tq = dbClient_->execSqlSync(
        "SELECT job_id, CASE WHEN is_job_subtask THEN 1 ELSE 0 END AS isa FROM tasks WHERE id=$1", taskId);
    if (tq.empty() || tq[0]["job_id"].isNull()) {
        return;
    }
    const std::int64_t isa = tq[0]["isa"].as<std::int64_t>();
    if (isa == 0) {
        return;
    }
    const std::int64_t jobId = tq[0]["job_id"].as<std::int64_t>();
    const Result st = dbClient_->execSqlSync("SELECT status FROM tasks WHERE id=$1", taskId);
    if (st.empty()) {
        return;
    }
    const std::string ts = st[0]["status"].as<std::string>();
    if (ts == "failed") {
        failPipelineJob(jobId, "subtask permanently failed (task " + std::to_string(taskId) + ")");
        return;
    }
    if (success) {
        tryRunReduceForJob(jobId);
    }
}

void CoordinatorService::tryRunReduceForJob(std::int64_t jobId) {
    try {
        int timeoutSec = static_cast<int>(config_.taskTimeout.count());
        if (timeoutSec < 30) {
            timeoutSec = 120;
        }

        std::optional<std::string> inputText;
        std::optional<std::string> executorImage;
        std::string regS;
        std::string regU;
        std::string regP;
        {
            auto tx = dbClient_->newTransaction();
            const Result claim = tx->execSqlSync(
                "UPDATE jobs j SET status='reducing' "
                "WHERE j.id=$1 "
                "AND j.status='running_children' "
                "AND j.children_total > 0 "
                "AND NOT EXISTS (SELECT 1 FROM tasks t WHERE t.job_id=j.id AND t.is_job_subtask=TRUE AND "
                "t.status='failed') "
                "AND (SELECT COUNT(*) FROM tasks t WHERE t.job_id=j.id AND t.is_job_subtask=TRUE AND "
                "t.status='succeeded') = j.children_total "
                "RETURNING j.input_json::text, j.executor_image,j.registry_server,j.registry_username,"
                "j.registry_password_secret",
                jobId);
            if (claim.empty()) {
                return;
            }
            inputText = claim[0]["input_json"].as<std::string>();
            executorImage = claim[0]["executor_image"].as<std::string>();
            regS = claim[0]["registry_server"].as<std::string>();
            regU = claim[0]["registry_username"].as<std::string>();
            regP = claim[0]["registry_password_secret"].as<std::string>();
        }

        const Result parts = dbClient_->execSqlSync(
            "SELECT t.id, tr.result_json::text FROM tasks t INNER JOIN task_results tr ON tr.task_id=t.id AND "
            "tr.success=TRUE "
            "WHERE t.job_id=$1 AND t.is_job_subtask=TRUE AND t.status='succeeded' ORDER BY t.id",
            jobId);

        Json::Value reduceIn(Json::objectValue);
        std::string perr;
        Json::Value inputRoot;
        if (!parseJsonText(*inputText, inputRoot, perr)) {
            failPipelineJob(jobId, std::string("reduce: bad job input_json: ") + perr);
            return;
        }
        reduceIn["input"] = inputRoot;

        Json::Value partsArr(Json::arrayValue);
        for (const auto &row : parts) {
            Json::Value item(Json::objectValue);
            item["task_id"] = static_cast<Json::Int64>(row["id"].as<std::int64_t>());
            Json::Value rj;
            const std::string js = row["result_json"].isNull() ? "{}" : row["result_json"].as<std::string>();
            std::string e2;
            if (!parseJsonText(js, rj, e2)) {
                failPipelineJob(jobId, std::string("reduce: bad subtask result json: ") + e2);
                return;
            }
            item["result"] = rj;
            partsArr.append(item);
        }
        reduceIn["results"] = partsArr;

        const std::string stdinStr = Json::writeString(Json::StreamWriterBuilder(), reduceIn);
        if (stdinStr.size() > kMaxDockerJsonBytes) {
            failPipelineJob(jobId, "reduce: stdin too large");
            return;
        }

        const int pullT = config_.registryPullTimeoutSeconds;
        const auto dr = coordinator::util::dockerRunWithPull(
            *executorImage, {"reduce"}, stdinStr, timeoutSec, regS, regU, regP, pullT, config_.trustedRegistryPrefixes,
            config_.enforceTrustedRegistryPrefixes);
        if (!dr.ok) {
            const std::string errMsg = !dr.stderrText.empty() ? dr.stderrText : dr.stdoutText;
            const std::string errStore = errMsg.empty() ? std::string("docker reduce failed") : errMsg;
            auto tx = dbClient_->newTransaction();
            tx->execSqlSync("UPDATE jobs SET status='failed', finished_at=NOW(), error_message=$2 "
                            "WHERE id=$1 AND status='reducing'",
                            jobId,
                            errStore.c_str());
            return;
        }

        const std::string outTrim = trimCopy(dr.stdoutText);
        if (outTrim.size() > kMaxDockerJsonBytes) {
            auto tx = dbClient_->newTransaction();
            tx->execSqlSync("UPDATE jobs SET status='failed', finished_at=NOW(), error_message=$2 "
                            "WHERE id=$1 AND status='reducing'",
                            jobId,
                            "reduce stdout too large");
            return;
        }

        std::string jerr;
        Json::Value finalJson;
        if (!parseJsonText(outTrim, finalJson, jerr)) {
            const std::string jsonErr = std::string("reduce output not valid JSON: ") + jerr;
            auto tx = dbClient_->newTransaction();
            tx->execSqlSync("UPDATE jobs SET status='failed', finished_at=NOW(), error_message=$2 "
                            "WHERE id=$1 AND status='reducing'",
                            jobId,
                            jsonErr.c_str());
            return;
        }

        const std::string finalStr = Json::writeString(Json::StreamWriterBuilder(), finalJson);
        auto tx = dbClient_->newTransaction();
        tx->execSqlSync("UPDATE jobs SET status='completed', finished_at=NOW(), result_json=$2::jsonb "
                        "WHERE id=$1 AND status='reducing'",
                        jobId,
                        finalStr.c_str());
    } catch (const std::exception &ex) {
        spdlog::error("tryRunReduceForJob job_id={} {}", jobId, ex.what());
        failPipelineJob(jobId, std::string("reduce exception: ") + ex.what());
    }
}

void CoordinatorService::processPendingPipelineSplits() {
    std::optional<std::int64_t> jobId;
    {
        auto tx = dbClient_->newTransaction();
        const Result r = tx->execSqlSync(
            "SELECT id FROM jobs WHERE status='pending_split' ORDER BY id LIMIT 1 FOR UPDATE SKIP LOCKED");
        if (r.empty()) {
            return;
        }
        jobId = r[0]["id"].as<std::int64_t>();
        tx->execSqlSync("UPDATE jobs SET status='splitting' WHERE id=$1", *jobId);
    }

    std::int64_t projectId = 0;
    std::string inputText;
    std::string image;
    int priority = 10;
    int maxAttempts = 5;
    std::string regS;
    std::string regU;
    std::string regP;
    {
        const Result j = dbClient_->execSqlSync(
            "SELECT project_id,input_json::text,executor_image,priority,max_attempts_children,"
            "registry_server,registry_username,registry_password_secret FROM jobs WHERE id=$1",
            *jobId);
        if (j.empty()) {
            return;
        }
        projectId = j[0]["project_id"].as<std::int64_t>();
        inputText = j[0]["input_json"].as<std::string>();
        image = j[0]["executor_image"].as<std::string>();
        priority = j[0]["priority"].as<int>();
        maxAttempts = j[0]["max_attempts_children"].as<int>();
        regS = j[0]["registry_server"].as<std::string>();
        regU = j[0]["registry_username"].as<std::string>();
        regP = j[0]["registry_password_secret"].as<std::string>();
    }

    Json::Value inputRoot;
    std::string perr;
    if (!parseJsonText(inputText, inputRoot, perr)) {
        const std::string err = std::string("invalid job input_json: ") + perr;
        dbClient_->execSqlSync("UPDATE jobs SET status='failed_split', finished_at=NOW(), error_message=$2 "
                               "WHERE id=$1",
                               *jobId,
                               err.c_str());
        return;
    }

    Json::Value splitStdin(Json::objectValue);
    splitStdin["input"] = inputRoot;
    const std::string splitIn = Json::writeString(Json::StreamWriterBuilder(), splitStdin);
    if (splitIn.size() > kMaxDockerJsonBytes) {
        dbClient_->execSqlSync("UPDATE jobs SET status='failed_split', finished_at=NOW(), error_message=$2 "
                               "WHERE id=$1",
                               *jobId,
                               "split stdin too large");
        return;
    }

    int timeoutSec = static_cast<int>(config_.taskTimeout.count());
    if (timeoutSec < 30) {
        timeoutSec = 120;
    }
    const int pullT = config_.registryPullTimeoutSeconds;
    const auto dr =
        coordinator::util::dockerRunWithPull(image, {"split"}, splitIn, timeoutSec, regS, regU, regP, pullT,
                                             config_.trustedRegistryPrefixes, config_.enforceTrustedRegistryPrefixes);
    if (!dr.ok) {
        const std::string errMsg = !dr.stderrText.empty() ? dr.stderrText : dr.stdoutText;
        const std::string splitErrStore = errMsg.empty() ? std::string("docker split failed") : errMsg;
        dbClient_->execSqlSync(
            "UPDATE jobs SET status='failed_split', finished_at=NOW(), error_message=$2 WHERE id=$1",
            *jobId,
            splitErrStore.c_str());
        return;
    }

    const std::string rawOut = trimCopy(dr.stdoutText);
    if (rawOut.empty() || rawOut.size() > kMaxDockerJsonBytes) {
        dbClient_->execSqlSync("UPDATE jobs SET status='failed_split', finished_at=NOW(), error_message=$2 "
                               "WHERE id=$1",
                               *jobId,
                               rawOut.empty() ? "split empty stdout" : "split stdout too large");
        return;
    }

    Json::Value splitOut;
    if (!parseJsonText(rawOut, splitOut, perr)) {
        const std::string splitJsonErr = std::string("split stdout not JSON: ") + perr;
        dbClient_->execSqlSync(
            "UPDATE jobs SET status='failed_split', finished_at=NOW(), error_message=$2 WHERE id=$1",
            *jobId,
            splitJsonErr.c_str());
        return;
    }
    const auto &subs = splitOut["subtasks"];
    if (!subs.isArray() || subs.empty()) {
        dbClient_->execSqlSync("UPDATE jobs SET status='failed_split', finished_at=NOW(), error_message=$2 "
                               "WHERE id=$1",
                               *jobId,
                               R"(split JSON must contain non-empty array "subtasks")");
        return;
    }
    const int n = subs.size();
    if (n > kMaxPipelineSubtasks) {
        dbClient_->execSqlSync(
            "UPDATE jobs SET status='failed_split', finished_at=NOW(), error_message=$2 WHERE id=$1",
            *jobId,
            "too many subtasks");
        return;
    }

    try {
        auto tx = dbClient_->newTransaction();
        for (Json::ArrayIndex i = 0; i < subs.size(); ++i) {
            const Json::Value &st = subs[static_cast<int>(i)];
            if (!st.isObject() || !st.isMember("payload") || !st["payload"].isObject()) {
                throw std::runtime_error(R"(each subtask must be object with object "payload")");
            }
            Json::Value payload = st["payload"];
            payload["docker_image"] = image;
            mergePayloadRegistry(payload, regS, regU, regP);
            const std::string payloadStr = Json::writeString(Json::StreamWriterBuilder(), payload);
            tx->execSqlSync("INSERT INTO tasks(project_id,payload_json,priority,status,attempts,max_attempts,job_id,"
                            "is_job_subtask,created_at) "
                            "VALUES($1,$2::jsonb,$3,'queued',0,$4,$5,true,NOW())",
                            projectId,
                            payloadStr.c_str(),
                            priority,
                            maxAttempts,
                            *jobId);
        }
        tx->execSqlSync(
            "UPDATE jobs SET children_total=$2, status='running_children', error_message='' WHERE id=$1",
            *jobId,
            n);
    } catch (const std::exception &ex) {
        const std::string ingestErr = std::string("split ingest failed: ") + ex.what();
        dbClient_->execSqlSync(
            "UPDATE jobs SET status='failed_split', finished_at=NOW(), error_message=$2 WHERE id=$1",
            *jobId,
            ingestErr.c_str());
    }
}

void CoordinatorService::processReadyPipelineReduces() {
    const Result r = dbClient_->execSqlSync(
        "SELECT j.id FROM jobs j WHERE j.status='running_children' AND j.children_total > 0 AND NOT EXISTS ("
        "SELECT 1 FROM tasks t WHERE t.job_id=j.id AND t.is_job_subtask=TRUE AND t.status='failed') AND ("
        "SELECT COUNT(*) FROM tasks t WHERE t.job_id=j.id AND t.is_job_subtask=TRUE AND t.status='succeeded') = "
        "j.children_total");
    for (const auto &row : r) {
        const std::int64_t jid = row["id"].as<std::int64_t>();
        tryRunReduceForJob(jid);
    }
}

}  // namespace coordinator::services
