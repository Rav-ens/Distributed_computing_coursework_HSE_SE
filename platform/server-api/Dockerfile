FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# libdrogon-dev в Ubuntu тянет CMake-зависимости (jsoncpp, sqlite3, mysql/mariadb, yaml-cpp и т.д.),
# даже если приложение использует только PostgreSQL.
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    g++ \
    make \
    libssl-dev \
    zlib1g-dev \
    uuid-dev \
    libjsoncpp-dev \
    libsqlite3-dev \
    libyaml-cpp-dev \
    libmariadb-dev \
    libbrotli-dev \
    libdrogon-dev \
    libspdlog-dev \
    libpq-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    libhiredis-dev \
    docker.io \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DCOORDINATOR_BUILD_WORKER=OFF \
    && cmake --build build --target coordinator -j

EXPOSE 8080
CMD ["./build/coordinator"]
