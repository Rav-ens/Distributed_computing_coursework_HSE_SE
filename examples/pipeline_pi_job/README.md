# Пайплайн split → map → reduce (π Монте-Карло)

Один образ, три подкоманды (аргументы после имени образа в `docker run`):


| Команда  | Кто вызывает | stdin (JSON)                                                      | stdout (JSON)                           |
| -------- | ------------ | ----------------------------------------------------------------- | --------------------------------------- |
| `split`  | координатор  | `{"input": <объект задания>}`                                     | `{"subtasks":[{"payload":{...}}, ...]}` |
| `map`    | worker       | объект `payload` подзадачи (координатор добавляет `docker_image`) | результат map (валидный JSON)           |
| `reduce` | координатор  | `{"input": ..., "results": [{"task_id", "result"}, ...]}`         | итоговый объект                         |


Требования: код возврата `0`, разумный объём stdout (лимиты см. основной README). Ошибки — ненулевой код и/или текст в stderr.

## Поля задания (`input`)

- `chunks` — число подзадач (1..500).
- `samples_per_chunk` — точек на одну карту (по умолчанию 200000).
- `base_seed` — начальный seed (подзадачи получают `base_seed`, `base_seed+1`, ...).

## Сборка

```bash
docker build -t coursework-pipeline-pi:latest ./examples/pipeline_pi_job
```

## Локальная проверка

```bash
echo '{"input":{"chunks":3,"samples_per_chunk":10000,"base_seed":7}}' | docker run --rm -i coursework-pipeline-pi:latest split
echo '{"samples":8000,"seed":1}' | docker run --rm -i coursework-pipeline-pi:latest map
echo '{"input":{},"results":[{"task_id":1,"result":{"samples":8000,"pi_estimate":3.14}}]}' | docker run --rm -i coursework-pipeline-pi:latest reduce
```

## API координатора

`BASE` — URL API координатора (в репозитории по умолчанию в примерах: `http://194.87.98.244:8080`). На **каждой** машине с воркером соберите тот же тег образа, что в `executor_url`.

```bash
export BASE=http://194.87.98.244:8080

curl -s -X POST "$BASE/projects" -H "Content-Type: application/json" \
  -d '{"name":"pi-pipeline-demo","version":"1.0","executor_url":"coursework-pipeline-pi:latest","checksum":"demo"}'

curl -s -X POST "$BASE/projects/1/pipeline-jobs" -H "Content-Type: application/json" \
  -d '{"input":{"chunks":4,"samples_per_chunk":120000,"base_seed":1}}'

curl -s "$BASE/pipeline-jobs/1"
curl -s "$BASE/pipeline-jobs/1/result"
```

Разбиение (`split`) и сборка (`reduce`) идут на **хосте координатора**; `map` — на **воркерах**. Нужны запущенные воркеры с `DOCKER_MAP_CMD=map`, `COORDINATOR_URL=$BASE` и локальный образ `coursework-pipeline-pi:latest` (или pull из registry).

После успеха в `GET .../result` поле `result_json` содержит `pi_estimate_combined`, `total_samples`, `abs_error` к истинному π.

## Локально всё на одной машине (отладка)

```bash
docker build -t coursework-pipeline-pi:latest ./examples/pipeline_pi_job
docker compose -f deploy/combined/docker-compose.full.yml --profile with-workers up --build -d
```

Дальше те же `curl` к `$BASE` (для локального combined без публичного API задайте `export BASE=http://localhost:8080`).