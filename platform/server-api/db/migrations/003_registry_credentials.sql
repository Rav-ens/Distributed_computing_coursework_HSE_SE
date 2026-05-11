-- Реестр образов: учётные данные на уровне проекта (пароль не отдаётся в API-ответах).
-- Снимок копируется в jobs при создании pipeline-job для стабильного split/reduce.

ALTER TABLE projects ADD COLUMN IF NOT EXISTS registry_server TEXT NOT NULL DEFAULT '';
ALTER TABLE projects ADD COLUMN IF NOT EXISTS registry_username TEXT NOT NULL DEFAULT '';
ALTER TABLE projects ADD COLUMN IF NOT EXISTS registry_password_secret TEXT NOT NULL DEFAULT '';

ALTER TABLE jobs ADD COLUMN IF NOT EXISTS registry_server TEXT NOT NULL DEFAULT '';
ALTER TABLE jobs ADD COLUMN IF NOT EXISTS registry_username TEXT NOT NULL DEFAULT '';
ALTER TABLE jobs ADD COLUMN IF NOT EXISTS registry_password_secret TEXT NOT NULL DEFAULT '';
