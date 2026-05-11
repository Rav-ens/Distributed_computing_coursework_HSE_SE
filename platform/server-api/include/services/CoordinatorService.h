#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include "config/AppConfig.h"
#include "scheduler/WeightedHeterogeneousStrategy.h"

namespace coordinator::services {

class CoordinatorService {
  public:
    CoordinatorService(drogon::orm::DbClientPtr dbClient, config::AppConfig config);

    Json::Value registerNode(const Json::Value &request);
    Json::Value heartbeat(std::int64_t nodeId);
    Json::Value listNodes();
    Json::Value createProject(const Json::Value &request);
    Json::Value updateProject(std::int64_t projectId, const Json::Value &request);
    Json::Value listProjects();
    Json::Value getProject(std::int64_t projectId);
    Json::Value generateTasks(std::int64_t projectId, const Json::Value &request);
    Json::Value submitProjectTask(std::int64_t projectId, const Json::Value &request);
    Json::Value getTasksByStatus(const std::string &status);
    Json::Value getTasksForNode(std::int64_t nodeId);
    Json::Value submitResult(std::int64_t taskId, const Json::Value &request);
    Json::Value projectResultSummary(std::int64_t projectId);
    Json::Value health();
    Json::Value recoverStaleNodesAndTasks();
    Json::Value assignPendingTasks();

    Json::Value createPipelineJob(std::int64_t projectId, const Json::Value &request);
    Json::Value getPipelineJob(std::int64_t jobId);
    Json::Value getPipelineJobResult(std::int64_t jobId);
    void processPendingPipelineSplits();
    void processReadyPipelineReduces();

    std::string currentStrategyName() const;

  private:
    std::vector<models::WorkerNode> loadOnlineNodes();
    std::optional<models::TaskDescriptor> fetchNextQueueTask();
    void ensureExecutorInstalled(std::int64_t nodeId, std::int64_t projectId);
    void onPipelineSubtaskFinished(std::int64_t taskId, bool success, bool idempotentDuplicate);
    void tryRunReduceForJob(std::int64_t jobId);
    void failPipelineJob(std::int64_t jobId, const std::string &reason);
    static bool dockerImageNameInvalid(const std::string &image);
    static std::string nodeStatusToString(models::NodeStatus status);
    static std::string taskStatusToString(models::TaskStatus status);

    drogon::orm::DbClientPtr dbClient_;
    config::AppConfig config_;
    scheduler::WeightedHeterogeneousStrategy weighted_;
};

}  // namespace coordinator::services
