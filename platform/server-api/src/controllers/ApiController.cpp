#include "controllers/ApiController.h"

#include <string>
#include <stdexcept>

#include <drogon/drogon.h>

namespace coordinator::controllers {

std::shared_ptr<services::CoordinatorService> ApiController::service_ = nullptr;

namespace {
void sendJson(const std::function<void(const drogon::HttpResponsePtr &)> &callback,
              const Json::Value &value,
              drogon::HttpStatusCode code = drogon::k200OK) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(value);
    response->setStatusCode(code);
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    callback(response);
}

Json::Value bodyOrEmpty(const drogon::HttpRequestPtr &req) {
    if (auto json = req->getJsonObject(); json) {
        return *json;
    }
    return Json::Value{};
}

void sendError(const std::function<void(const drogon::HttpResponsePtr &)> &callback,
               drogon::HttpStatusCode code,
               const std::string &message) {
    Json::Value err;
    err["error"] = message;
    sendJson(callback, err, code);
}

std::string trimQueryParam(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    return s;
}
}  // namespace

void ApiController::setService(std::shared_ptr<services::CoordinatorService> service) {
    service_ = std::move(service);
}

void ApiController::registerNode(const drogon::HttpRequestPtr &req,
                                 std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    sendJson(callback, service_->registerNode(bodyOrEmpty(req)), drogon::k201Created);
}

void ApiController::heartbeat(const drogon::HttpRequestPtr & /*req*/,
                              std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                              std::int64_t nodeId) {
    sendJson(callback, service_->heartbeat(nodeId));
}

void ApiController::nodeTasks(const drogon::HttpRequestPtr & /*req*/,
                              std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                              std::int64_t nodeId) {
    sendJson(callback, service_->getTasksForNode(nodeId));
}

void ApiController::nodes(const drogon::HttpRequestPtr & /*req*/,
                          std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    sendJson(callback, service_->listNodes());
}

void ApiController::createProject(const drogon::HttpRequestPtr &req,
                                  std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    try {
        sendJson(callback, service_->createProject(bodyOrEmpty(req)), drogon::k201Created);
    } catch (const std::invalid_argument &ex) {
        sendError(callback, drogon::k400BadRequest, ex.what());
    } catch (const std::exception &ex) {
        drogon::HttpStatusCode code = drogon::k500InternalServerError;
        const std::string msg = ex.what();
        if (msg.find("duplicate key") != std::string::npos || msg.find("unique constraint") != std::string::npos ||
            msg.find("already exists") != std::string::npos) {
            code = drogon::k409Conflict;
        }
        sendError(callback, code, ex.what());
    }
}

void ApiController::projects(const drogon::HttpRequestPtr & /*req*/,
                             std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    sendJson(callback, service_->listProjects());
}

void ApiController::getProject(const drogon::HttpRequestPtr & /*req*/,
                               std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                               std::int64_t projectId) {
    try {
        sendJson(callback, service_->getProject(projectId));
    } catch (const std::runtime_error &) {
        sendError(callback, drogon::k404NotFound, "not found");
    }
}

void ApiController::updateProject(const drogon::HttpRequestPtr &req,
                                  std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                                  std::int64_t projectId) {
    try {
        sendJson(callback, service_->updateProject(projectId, bodyOrEmpty(req)));
    } catch (const std::runtime_error &) {
        sendError(callback, drogon::k404NotFound, "not found");
    } catch (const std::invalid_argument &ex) {
        sendError(callback, drogon::k400BadRequest, ex.what());
    }
}

void ApiController::generateTasks(const drogon::HttpRequestPtr &req,
                                  std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                                  std::int64_t projectId) {
    sendJson(callback, service_->generateTasks(projectId, bodyOrEmpty(req)), drogon::k201Created);
}

void ApiController::submitProjectTask(const drogon::HttpRequestPtr &req,
                                      std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                                      std::int64_t projectId) {
    try {
        sendJson(callback, service_->submitProjectTask(projectId, bodyOrEmpty(req)), drogon::k201Created);
    } catch (const std::invalid_argument &ex) {
        sendError(callback, drogon::k400BadRequest, ex.what());
    }
}

void ApiController::createPipelineJob(const drogon::HttpRequestPtr &req,
                                      std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                                      std::int64_t projectId) {
    try {
        sendJson(callback, service_->createPipelineJob(projectId, bodyOrEmpty(req)), drogon::k201Created);
    } catch (const std::invalid_argument &ex) {
        sendError(callback, drogon::k400BadRequest, ex.what());
    } catch (const std::runtime_error &ex) {
        sendError(callback, drogon::k404NotFound, ex.what());
    }
}

void ApiController::getPipelineJob(const drogon::HttpRequestPtr & /*req*/,
                                   std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                                   std::int64_t jobId) {
    try {
        sendJson(callback, service_->getPipelineJob(jobId));
    } catch (const std::runtime_error &) {
        sendError(callback, drogon::k404NotFound, "not found");
    }
}

void ApiController::getPipelineJobResult(const drogon::HttpRequestPtr & /*req*/,
                                         std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                                         std::int64_t jobId) {
    try {
        sendJson(callback, service_->getPipelineJobResult(jobId));
    } catch (const std::runtime_error &ex) {
        sendError(callback, drogon::k404NotFound, ex.what());
    }
}

void ApiController::tasksByStatus(const drogon::HttpRequestPtr &req,
                                  std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    const std::string status = trimQueryParam(req->getParameter("status"));
    if (status.empty()) {
        sendError(callback,
                  drogon::k400BadRequest,
                  R"(missing query parameter "status"; use e.g. ?status=queued|assigned|running|succeeded|failed)");
        return;
    }
    sendJson(callback, service_->getTasksByStatus(status));
}

void ApiController::taskResult(const drogon::HttpRequestPtr &req,
                               std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                               std::int64_t taskId) {
    sendJson(callback, service_->submitResult(taskId, bodyOrEmpty(req)));
}

void ApiController::projectSummary(const drogon::HttpRequestPtr & /*req*/,
                                   std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                                   std::int64_t projectId) {
    sendJson(callback, service_->projectResultSummary(projectId));
}

void ApiController::health(const drogon::HttpRequestPtr & /*req*/,
                           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    if (!service_) {
        sendError(callback, drogon::k503ServiceUnavailable, "coordinator not initialized");
        return;
    }
    try {
        sendJson(callback, service_->health());
    } catch (const std::exception &ex) {
        Json::Value err;
        err["service"] = "coordinator";
        err["db"] = "error";
        err["message"] = ex.what();
        sendJson(callback, err, drogon::k503ServiceUnavailable);
    }
}

void ApiController::ping(const drogon::HttpRequestPtr & /*req*/,
                         std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    Json::Value ok;
    ok["ok"] = true;
    sendJson(callback, ok);
}

}  // namespace coordinator::controllers
