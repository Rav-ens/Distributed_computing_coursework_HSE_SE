# Worker Deployment

Запуск воркеров **на отдельных машинах** от API. Координатор и Postgres поднимаются через `deploy/server/docker-compose.server.yml` на другом хосте.

## Запуск одного воркера

Из каталога `deploy/worker` (рядом с compose-файлом лежит `.env`, который подхватывает Compose):

```bash
cd deploy/worker
cp .env.example .env
# отредактируйте COORDINATOR_URL

docker compose -f docker-compose.worker.yml up --build -d
```

## Несколько воркеров на одной машине

```bash
docker compose -f docker-compose.worker.yml up --build -d --scale worker=3
```

## Важно

- `**COORDINATOR_URL**` — URL API без обязательного завершающего `/` (воркер обрежет хвостовой `/`). По умолчанию в `.env.example` — `http://194.87.98.244:8080`; при HTTPS — `https://...`.
- С машины воркера должен открываться этот URL (файрвол на стороне API: входящий **8080** к координатору).
- `**/var/run/docker.sock`** — воркер сам вызывает `docker run` для задач.
- Образы из `executor_url` / `payload.docker_image` подтягиваются с registry: воркер перед `map` выполняет `**docker pull`** (и при наличии `registry_username` в payload — `**docker login**`). Локальный `docker build` на машине воркера не обязателен, если образ уже в registry.

Дополнительные переменные окружения:

- `REGISTRY_PULL_TIMEOUT_SEC` — таймаут `docker pull` (по умолчанию 300)
- `DOCKER_MAP_TIMEOUT_SEC` — таймаут `docker run … map` (по умолчанию 300)

