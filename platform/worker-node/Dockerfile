# Worker-only image: Docker CLI + coordinator_worker binary.
# This folder is fully isolated from server-api sources.

FROM alpine:3.20 AS build
RUN apk add --no-cache g++ curl-dev nlohmann-json
WORKDIR /app
COPY main.cpp /app/main.cpp
RUN g++ -std=c++20 -O2 -o /app/coordinator_worker /app/main.cpp -lcurl

FROM docker:27.3.1-cli-alpine3.20
RUN apk add --no-cache libstdc++ libcurl
COPY --from=build /app/coordinator_worker /usr/local/bin/coordinator_worker
ENV COORDINATOR_URL=http://194.87.98.244:8080
ENV DOCKER_MAP_CMD=map
ENV HEARTBEAT_SEC=5
CMD ["/usr/local/bin/coordinator_worker"]
