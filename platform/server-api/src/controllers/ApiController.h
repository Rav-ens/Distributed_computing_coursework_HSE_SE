#pragma once

#include <memory>

#include <drogon/HttpController.h>

#include "services/CoordinatorService.h"

namespace coordinator::controllers {

class ApiController : public drogon::HttpController<ApiController> {
  public:
    METHOD_LIST_BEGIN
    // Абсолютные пути: у Drogon METHOD_ADD иначе префиксует имя контроллера.
    ADD_METHOD_TO(ApiController::registerNode, "/nodes/register", drogon::Post);
    ADD_METHOD_TO(ApiController::heartbeat, "/nodes/{1}/heartbeat", drogon::Post);
    ADD_METHOD_TO(ApiController::nodeTasks, "/nodes/{1}/tasks", drogon::Get);
    ADD_METHOD_TO(ApiController::nodes, "/nodes", drogon::Get);
    ADD_METHOD_TO(ApiController::createProject, "/projects", drogon::Post);
    ADD_METHOD_TO(ApiController::projects, "/projects", drogon::Get);
    ADD_METHOD_TO(ApiController::getProject, "/projects/{1}", drogon::Get);
    ADD_METHOD_TO(ApiController::updateProject, "/projects/{1}", drogon::Put);
    ADD_METHOD_TO(ApiController::generateTasks, "/projects/{1}/tasks/generate", drogon::Post);
    ADD_METHOD_TO(ApiController::submitProjectTask, "/projects/{1}/tasks", drogon::Post);
    ADD_METHOD_TO(ApiController::createPipelineJob, "/projects/{1}/pipeline-jobs", drogon::Post);
    ADD_METHOD_TO(ApiController::getPipelineJob, "/pipeline-jobs/{1}", drogon::Get);
    ADD_METHOD_TO(ApiController::getPipelineJobResult, "/pipeline-jobs/{1}/result", drogon::Get);
    ADD_METHOD_TO(ApiController::tasksByStatus, "/tasks", drogon::Get);
    ADD_METHOD_TO(ApiController::taskResult, "/tasks/{1}/result", drogon::Post);
    ADD_METHOD_TO(ApiController::projectSummary, "/projects/{1}/results/summary", drogon::Get);
    ADD_METHOD_TO(ApiController::health, "/health", drogon::Get);
    ADD_METHOD_TO(ApiController::ping, "/ping", drogon::Get);
    METHOD_LIST_END

    static void setService(std::shared_ptr<services::CoordinatorService> service);

    void registerNode(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void heartbeat(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                   std::int64_t nodeId);
    void nodeTasks(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                   std::int64_t nodeId);
    void nodes(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void createProject(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void projects(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void getProject(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                    std::int64_t projectId);
    void updateProject(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                       std::int64_t projectId);
    void generateTasks(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                       std::int64_t projectId);
    void submitProjectTask(const drogon::HttpRequestPtr &req,
                           std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                           std::int64_t projectId);
    void createPipelineJob(const drogon::HttpRequestPtr &req,
                           std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                           std::int64_t projectId);
    void getPipelineJob(const drogon::HttpRequestPtr & /*req*/,
                        std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                        std::int64_t jobId);
    void getPipelineJobResult(const drogon::HttpRequestPtr & /*req*/,
                              std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                              std::int64_t jobId);
    void tasksByStatus(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void taskResult(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                    std::int64_t taskId);
    void projectSummary(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                        std::int64_t projectId);
    void health(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void ping(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&callback);

  private:
    static std::shared_ptr<services::CoordinatorService> service_;
};

}  // namespace coordinator::controllers
