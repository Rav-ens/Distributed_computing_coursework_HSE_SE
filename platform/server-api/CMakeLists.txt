# CMake: API координатора и опционально бинарь coordinator_worker (libcurl).
# Сборка по умолчанию: cmake -S . -B build && cmake --build build
#
# Опции:
#   -DCOORDINATOR_BUILD_API=OFF        — без HTTP API (редко нужно)
#   -DCOORDINATOR_BUILD_WORKER=OFF    — без бинаря coordinator_worker (Docker-образ coordinator)
#
# Для coordinator_worker при первой конфигурации CMake скачивает nlohmann/json (FetchContent).

cmake_minimum_required(VERSION 3.20)
project(distributed_compute_coordinator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

option(COORDINATOR_BUILD_API "Собрать HTTP API координатора (Drogon + PostgreSQL)" ON)
option(COORDINATOR_BUILD_WORKER "Собрать исполнитель coordinator_worker (libcurl)" ON)

if(COORDINATOR_BUILD_API)
    find_package(Drogon REQUIRED)
    find_package(spdlog REQUIRED)
    find_package(PostgreSQL REQUIRED)

    set(COORDINATOR_SOURCES
        src/main.cpp
        src/config/AppConfig.cpp
        src/scheduler/WeightedHeterogeneousStrategy.cpp
        src/services/CoordinatorService.cpp
        src/reliability/RecoveryService.cpp
        src/controllers/ApiController.cpp
        src/util/PipelineDocker.cpp
    )

    add_executable(coordinator ${COORDINATOR_SOURCES})
    target_include_directories(coordinator PRIVATE include)
    target_include_directories(coordinator PRIVATE src)
    target_link_libraries(
        coordinator
        PRIVATE Drogon::Drogon
                spdlog::spdlog
                PostgreSQL::PostgreSQL
    )
endif()

if(COORDINATOR_BUILD_WORKER)
    find_package(CURL REQUIRED)
    include(FetchContent)
    FetchContent_Declare(
        json
        URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz)
    FetchContent_MakeAvailable(json)

    add_executable(coordinator_worker ../worker-node/main.cpp)
    target_link_libraries(coordinator_worker PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)
endif()
