# PI Job Example

Пример пользовательского docker-образа для вашего проекта.

Контракт:

- Команда: `map`
- Вход (`stdin`): JSON, например `{"samples": 500000, "seed": 7}`
- Выход (`stdout`): JSON с оценкой числа pi методом Монте-Карло

## Сборка образа

Из корня репозитория:

```bash
docker build -t coursework-pi-job:latest ./examples/pi_job
```

Для публикации в registry укажите полный тег, например `docker.io/YOUR_USER/coursework-pi-job:1.0.0`, и выполните `docker push`.

## Локальная проверка образа

```bash
echo '{"samples": 200000, "seed": 1}' | docker run --rm -i coursework-pi-job:latest map
```

## Использование в Coordinator

Создайте проект с `executor_url`, указывающим на образ в registry (строка для `docker pull`, не URL страницы в браузере), затем отправляйте задачи:

```json
{"payload":{"samples":500000,"seed":123}}
```

Эндпоинт: `POST /projects/{project_id}/tasks` с телом, содержащим объект `payload`.