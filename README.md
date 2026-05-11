# Distributed Compute Coordinator

Учебный распределённый координатор задач: **HTTP API** (C++, Drogon), **PostgreSQL**, **воркеры** с Docker. Поддерживаются **обычные задачи** (один шаг map на воркере) и **пайплайн** split → map → reduce.

**Содержание:** [структура репозитория](#структура-репозитория) · [как устроена система](#как-устроена-система) · [образ задачи и контракт Docker](#образ-задачи-и-контракт-docker) · [файлы и функции в примерах](#файлы-и-функции-в-примерах) · [REST API](#rest-api-кратко) · [создание проекта и постановка задач](#создание-проекта-и-постановка-задач) · [пайплайн](#пайплайн-split--map--reduce) · [деплой](#деплой) · [миграции БД](#миграции-postgresql) · [registry](#образы-из-container-registry) · [частые ошибки](#частые-ошибки)

---

## Структура репозитория


| Путь                                     | Назначение                                                                                                                                       |
| ---------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------ |
| `platform/server-api`                    | Координатор: REST API, планировщик **weighted_heterogeneous**, работа с БД, для пайплайна — вызовы `docker` (split/reduce) на хосте координатора |
| `platform/server-api/db/migrations/`     | SQL-миграции Postgres (`001` → `002` → `003` по порядку)                                                                                         |
| `platform/server-api/config/config.json` | Конфиг Drogon (порт, клиент БД `default` и пароль — согласовать с `.env` compose)                                                                |
| `platform/worker-node`                   | Воркер: опрос API, `docker pull` + `docker run` для **map**-задач                                                                                |
| `deploy/combined`                        | Compose: Postgres + coordinator + воркеры (профиль `with-workers`)                                                                               |
| `deploy/server`, `deploy/worker`         | Разнесённый деплой: только API или только воркеры                                                                                                |
| `examples/pi_job`                        | Пример **плоской** задачи: один образ, команда `map`                                                                                             |
| `examples/pipeline_pi_job`               | Пример **пайплайна**: один образ, команды `split`, `map`, `reduce`                                                                               |


---

## Как устроена система

1. **Проект** (`projects` в БД) хранит имя, версию, `**executor_url`** (строка образа в registry), checksum и опционально учётные данные registry.
2. **Задача** (`tasks`) — единица работы: JSON `**payload_json`**, статус (`queued` → `assigned` → `running` → `succeeded` / `failed` и др.), приоритет, ретраи.
3. **Воркер** регистрируется в API, шлёт heartbeat, забирает назначенные ему задачи, для каждой выполняет контейнер образа и отправляет `**POST /tasks/{id}/result`**.
4. **Пайплайн** (`jobs` + подзадачи в `tasks`): координатор запускает `**split`**, создаёт подзадачи; воркеры выполняют `**map`**; координатор вызывает `**reduce**` и сохраняет итог.

Схема потока для обычной задачи:

```text
Клиент  →  POST /projects/{id}/tasks (payload)
              ↓
Координатор  →  очередь, назначение воркеру
              ↓
Воркер       →  docker pull + docker run … map
              ↓
Воркер       →  POST /tasks/{id}/result
```

---

## Образ задачи и контракт Docker

### Что такое `executor_url`

В API это **не** ссылка на страницу в браузере. Нужна строка в формате `**docker pull`**: например `docker.io/ваш_логин/репозиторий:тег` или `ghcr.io/org/image:tag`.

Запрещено: URL с префиксом `http://` или `https://` (в т.ч. ссылки на Docker Hub в браузере) — координатор вернёт ошибку вида `executor_url must be a Docker image reference (not http(s) URL)`.

Образ должен быть **доступен из registry** (публичный pull или учётные данные в `POST /projects` / в payload для воркера — см. ниже).

### Общие требования к контейнеру

- Запуск: `docker run --rm -i <образ> <аргументы>`, на **stdin** передаётся JSON (строка), ответ — **валидный JSON в stdout**.
- Код выхода **0** при успехе; при ошибке — ненулевой код и/или текст в **stderr**.
- Воркер перед запуском может удалить из копии payload поля `registry_server`, `registry_username`, `registry_password` (секреты не уходят в stdin контейнера map).

### Плоская задача (только map)

Используется для `POST /projects/{id}/tasks` и для подзадач пайплайна на воркере.

- Аргументы после имени образа задаются переменной окружения воркера `**DOCKER_MAP_CMD`** (по умолчанию одно слово: `map`). Можно несколько слов, например `run map` — тогда после образа подставится `run map`.
- Фактическая команда воркера: `docker run --rm -i <executor_image> <слова из DOCKER_MAP_CMD>`.
- **stdin**: JSON объекта **payload** задачи (без служебных полей registry после очистки воркером).
- **stdout**: JSON результата (координатор сохраняет его как результат задачи).

### Пайплайн (split, map, reduce)

Один и тот же образ; различаются **первый аргумент** после имени образа: `split`, `map`, `reduce`.


| Команда  | Кто запускает | stdin (JSON)                                                                   | stdout (JSON)                           |
| -------- | ------------- | ------------------------------------------------------------------------------ | --------------------------------------- |
| `split`  | координатор   | `{"input": <объект задания>}`                                                  | `{"subtasks":[{"payload":{...}}, ...]}` |
| `map`    | воркер        | объект **payload** одной подзадачи (координатор может добавить `docker_image`) | результат map                           |
| `reduce` | координатор   | `{"input": ..., "results": [{"task_id", "result"}, ...]}`                      | итоговый объект                         |


Подробная таблица и поля `input` для демо: [examples/pipeline_pi_job/README.md](./examples/pipeline_pi_job/README.md).

### Какие файлы должны быть в вашем образе

Минимально (как в примерах):

1. `**Dockerfile`** — описывает сборку; задаёт `**ENTRYPOINT`** (или `CMD`) на ваш скрипт/бинарник.
2. **Точка входа** — например `entrypoint.py` (или исполняемый файл на любом языке): читает **stdin**, разбирает JSON, пишет JSON в **stdout**, выставляет код выхода.

Рекомендуется также:

- `**README.md`** в каталоге образа — описание полей payload и формата ответа для проверяющего.

Язык не фиксирован: главное — соблюсти контракт stdin/stdout и аргументы командной строки.

---

## Файлы и функции в примерах

### `examples/pi_job` (плоская задача)


| Файл            | Роль                                                                                           |
| --------------- | ---------------------------------------------------------------------------------------------- |
| `Dockerfile`    | Базовый образ Python, копирует `entrypoint.py`, `ENTRYPOINT ["python3", "/app/entrypoint.py"]` |
| `entrypoint.py` | Логика map                                                                                     |


В `**entrypoint.py`**:

- `**main()`** — читает argv[1] (ожидается `map`), читает stdin как JSON, считает π методом Монте-Карло, печатает JSON в stdout, возвращает код 0/1.

Локальная проверка:

```bash
docker build -t test-pi:latest ./examples/pi_job
echo '{"samples":200000,"seed":1}' | docker run --rm -i test-pi:latest map
```

Документация каталога: [examples/pi_job/README.md](./examples/pi_job/README.md).

### `examples/pipeline_pi_job` (пайплайн)


| Файл            | Роль                                                   |
| --------------- | ------------------------------------------------------ |
| `Dockerfile`    | Аналогично pi_job: один entrypoint, три режима по argv |
| `entrypoint.py` | Реализация `split` / `map` / `reduce`                  |


В `**entrypoint.py**`:

- `**cmd_split(inp)**` — из `inp["input"]` строит список подзадач с полями `payload` (chunks, samples, seed).
- `**cmd_map(payload)**` — выполняет одну подзадачу (Monte Carlo), игнорирует служебное поле `docker_image` в payload.
- `**cmd_reduce(inp)**` — агрегирует `inp["results"]` в итоговую оценку.
- `**main()**` — по `sys.argv[1]` выбирает `split` | `map` | `reduce`, читает JSON с stdin, пишет JSON в stdout.

Локальная проверка (из корня репозитория):

```bash
docker build -t test-pipeline:latest ./examples/pipeline_pi_job
echo '{"input":{"chunks":3,"samples_per_chunk":10000,"base_seed":7}}' | docker run --rm -i test-pipeline:latest split
```

---

## REST API (кратко)

База URL: `**BASE**` без завершающего `/`.


| Метод | Путь                             | Назначение                                              |
| ----- | -------------------------------- | ------------------------------------------------------- |
| GET   | `/health`, `/ping`               | Проверка живости                                        |
| POST  | `/nodes/register`                | Регистрация воркера                                     |
| POST  | `/nodes/{id}/heartbeat`          | Heartbeat                                               |
| GET   | `/nodes/{id}/tasks`              | Задачи, назначенные воркеру                             |
| POST  | `/projects`                      | Создать проект                                          |
| GET   | `/projects`, `/projects/{id}`    | Список / карточка проекта                               |
| PUT   | `/projects/{id}`                 | Обновить проект                                         |
| POST  | `/projects/{id}/tasks`           | Поставить **одну** задачу (тело с `payload`)            |
| POST  | `/projects/{id}/tasks/generate`  | Сгенерировать много мелких задач (демо)                 |
| GET   | `/tasks?status=...`              | Список задач по статусу (**обязателен** query `status`) |
| POST  | `/tasks/{id}/result`             | Результат от воркера (обычно не вызывают вручную)       |
| GET   | `/projects/{id}/results/summary` | Сводка по задачам проекта                               |
| POST  | `/projects/{id}/pipeline-jobs`   | Создать пайплайн job                                    |
| GET   | `/pipeline-jobs/{job_id}`        | Статус job                                              |
| GET   | `/pipeline-jobs/{job_id}/result` | Итог job (когда `completed`)                            |


Идентификаторы из ответов (`project_id`, `task_id`, `job_id`) удобно копировать из JSON вручную или подставлять в `export PROJECT_ID=…` и т.д.

---

## Создание проекта и постановка задач

### 1. Собрать образ и залить в registry

Из **корня репозитория** (где лежит каталог `examples/`):

```bash
export IMAGE_PI=docker.io/ВАШ_ЛОГИН/coursework-pi-job:1.0.0
docker build -t "$IMAGE_PI" ./examples/pi_job
docker login   # при необходимости
docker push "$IMAGE_PI"
```

Не используйте `docker build .` в корне репозитория для этих примеров — Dockerfile лежит в `examples/pi_job`.

### 2. Создать проект

Тело — валидный JSON: поля через запятую, без лишних символов между парами ключ/значение.

В примерах ниже по умолчанию используется учебный координатор (без завершающего `/` в URL):

```bash
export BASE=http://194.87.98.244:8080
```

Если поднимаете API у себя — подставьте свой базовый URL.

```bash
curl -s -X POST "$BASE/projects" \
  -H "Content-Type: application/json" \
  -d "{\"name\":\"pi-demo\",\"version\":\"1.0.0\",\"executor_url\":\"$IMAGE_PI\",\"checksum\":\"sha256:demo\"}"
```

Ответ содержит `project_id`. Подставьте в переменную:

```bash
export PROJECT_ID=1
```

Уникальность: пара `(name, version)` в БД уникальна — при повторе смените имя/версию.

### 3. Отправить задачу на вычисления

**Одна задача** с произвольным JSON внутри `payload` (должен совпадать с тем, что ожидает ваш `entrypoint` / бинарник map):

```bash
curl -s -X POST "$BASE/projects/$PROJECT_ID/tasks" \
  -H "Content-Type: application/json" \
  -d '{"payload":{"samples":500000,"seed":123},"priority":10,"max_attempts":5}'
```

Ответ содержит `task_id`. Должен быть запущен **хотя бы один воркер** с тем же `COORDINATOR_URL`, что и ваш `BASE` (по умолчанию в проекте это `http://194.87.98.244:8080`), плюс доступ к `docker.sock` и возможность `docker pull` вашего образа.

### 4. Наблюдение и сводка

```bash
curl -s "$BASE/tasks?status=queued"
curl -s "$BASE/tasks?status=running"
curl -s "$BASE/tasks?status=succeeded"
curl -s "$BASE/projects/$PROJECT_ID/results/summary"
```

Альтернатива для демо массовой генерации одинаковых задач: `POST /projects/{id}/tasks/generate` с полем `count` (см. реализацию в `CoordinatorService::generateTasks`).

---

## Пайплайн split → map → reduce

### 1. Образ в registry

```bash
export IMAGE_PIPELINE=docker.io/ВАШ_ЛОГИН/coursework-pipeline-pi:1.0
docker build -t "$IMAGE_PIPELINE" ./examples/pipeline_pi_job
docker push "$IMAGE_PIPELINE"
```

### 2. Проект

```bash
curl -s -X POST "$BASE/projects" \
  -H "Content-Type: application/json" \
  -d "{\"name\":\"pipeline-demo\",\"version\":\"1.0\",\"executor_url\":\"$IMAGE_PIPELINE\",\"checksum\":\"pipeline\"}"
```

Запомните `project_id` → `export PROJECT_ID=…`.

### 3. Создать job

```bash
curl -s -X POST "$BASE/projects/$PROJECT_ID/pipeline-jobs" \
  -H "Content-Type: application/json" \
  -d '{"input":{"chunks":4,"samples_per_chunk":200000,"base_seed":1}}'
```

В ответе — `job_id` (не путать с `project_id`). Для опроса:

```bash
export JOB_ID=1
curl -s "$BASE/pipeline-jobs/$JOB_ID"
curl -s "$BASE/pipeline-jobs/$JOB_ID/result"
```

Пока job не завершён, `GET .../result` может вернуть **404** с пояснением — сначала дождитесь успешного статуса в `GET /pipeline-jobs/{job_id}`.

Воркеры для map должны иметь `DOCKER_MAP_CMD=map` (значение по умолчанию), чтобы вызывалась подкоманда `map` того же образа, что и split/reduce на координаторе.

---

## Деплой

### Всё на одной машине (compose)

```bash
git clone <URL_ВАШЕГО_РЕПО> coursework
cd coursework/deploy/combined
cp .env.example .env
# Согласуйте пароли Postgres с platform/server-api/config/config.json
docker compose -f docker-compose.full.yml --profile with-workers up --build -d
```

В `deploy/combined` воркеру по умолчанию задаётся тот же `COORDINATOR_URL`, что и у отдельного воркера: `http://194.87.98.244:8080` (см. `deploy/combined/.env.example` и подстановку в `docker-compose.full.yml`). Если к этому адресу с контейнера нет маршрута и вы гоняете только локальный стек, в `.env` переопределите на `http://coordinator:8080`.

### API и воркеры раздельно

- Сервер: `deploy/server` — см. [deploy/server/README.md](./deploy/server/README.md).
- Воркеры: `deploy/worker` — см. [deploy/worker/README.md](./deploy/worker/README.md).

Воркеру нужны: исходящий HTTP до API, исходящий доступ к registry, смонтированный `/var/run/docker.sock`.

### Переменные воркера (важно для map)


| Переменная                                                   | Смысл                                                        |
| ------------------------------------------------------------ | ------------------------------------------------------------ |
| `COORDINATOR_URL`                                            | Базовый URL API (без `/` на конце)                           |
| `DOCKER_MAP_CMD`                                             | Слова после имени образа в `docker run` (по умолчанию `map`) |
| `DOCKER_MAP_TIMEOUT_SEC`                                     | Таймаут map                                                  |
| `REGISTRY_PULL_TIMEOUT_SEC`                                  | Таймаут `docker pull`                                        |
| `HEARTBEAT_SEC`                                              | Пауза между циклами опроса                                   |
| `WORKER_HOST`, `WORKER_CPU`, `WORKER_RAM_MB`, `WORKER_SCORE` | Регистрация узла                                             |
| `NODE_ID`                                                    | Опционально: продолжить как существующий узел                |


---

## Миграции PostgreSQL

Файлы в `platform/server-api/db/migrations/`:

1. `001_init.sql` — базовые таблицы.
2. `002_pipeline_jobs.sql` — пайплайн (`jobs`, поля в `tasks`).
3. `003_registry_credentials.sql` — учётные данные registry.

Применяйте **строго по порядку**. При первом создании тома Postgres в compose скрипты из каталога могут выполниться автоматически; на **существующей** БД при смене схемы прогоните SQL вручную через `psql`.

---

## Образы из container registry

- В `executor_url` — строка образа для `docker pull`, например `docker.io/USER/repo:tag`.
- Для приватных образов в `POST /projects` можно передать `registry_server`, `registry_username`, `registry_password` (в ответах пароль не отдаётся).

Переменные координатора (см. `platform/server-api/include/config/AppConfig.h` и `AppConfig.cpp`):

- `TRUSTED_REGISTRY_PREFIXES` — префиксы через запятую.
- `ENFORCE_TRUSTED_REGISTRY_PREFIXES=1` — разрешать только эти префиксы.
- `REGISTRY_PULL_TIMEOUT_SEC` — таймаут pull/login на координаторе.

---

## Частые ошибки


| Сообщение / симптом                                               | Причина                                                                                                                                                                  |
| ----------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `executor_url must be a Docker image reference (not http(s) URL)` | В `executor_url` попал `http://`/`https://`, или из-за **битого JSON** поле не распарсилось и пришла пустая/мусорная строка. Проверьте кавычки и запятые в `-d '{...}'`. |
| `duplicate key` / `unique constraint` для проекта                 | Пара `(name, version)` уже занята — смените или удалите старую запись.                                                                                                   |
| `GET /tasks` без `?status=`                                       | Нужен обязательный query, например `?status=queued`.                                                                                                                     |
| Пайплайн «не двигается»                                           | Нет воркеров, нет доступа к registry, неверный `DOCKER_MAP_CMD`, или смотрите не тот id: для пайплайна опрос по `**job_id`**, не `project_id`.                           |


Проверка API:

```bash
curl -s "$BASE/health"
curl -s "$BASE/ping"
```

