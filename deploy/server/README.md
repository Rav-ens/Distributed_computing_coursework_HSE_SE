# Server Deployment

Эта папка содержит все для запуска "основного сервера" (API + PostgreSQL).

Для пайплайна split/reduce координатор вызывает `docker run` на хосте: в compose для сервиса `coordinator` смонтирован `/var/run/docker.sock` и в образ установлен пакет `docker.io` (клиент).

Образы пользователей подтягиются с **registry** (`docker pull`); ручной `docker build` на этом сервере не обязателен.

Опциональные переменные окружения для `coordinator` (см. `AppConfig` / `README.md` в корне):

- `TRUSTED_REGISTRY_PREFIXES` — список префиксов через запятую
- `ENFORCE_TRUSTED_REGISTRY_PREFIXES` — `0` или `1`
- `REGISTRY_PULL_TIMEOUT_SEC` — таймаут pull/login (секунды)

## Запуск

Из каталога `deploy/server` (опционально: `cp .env.example .env`; пароль БД должен совпадать с `platform/server-api/config/config.json`):

```bash
cd deploy/server
docker compose -f docker-compose.server.yml up --build -d
```

Проверка:

```bash
export BASE=http://194.87.98.244:8080
curl -s "$BASE/health"
```

Остановка:

```bash
docker compose -f docker-compose.server.yml down
```

