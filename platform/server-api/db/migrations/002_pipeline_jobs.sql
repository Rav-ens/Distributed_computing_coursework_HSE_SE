-- Пайплайн split → map (подзадачи) → reduce (агрегат на координаторе)

CREATE TABLE IF NOT EXISTS jobs (
    id BIGSERIAL PRIMARY KEY,
    project_id BIGINT NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
    status TEXT NOT NULL CHECK (status IN (
        'pending_split',
        'splitting',
        'failed_split',
        'running_children',
        'reducing',
        'completed',
        'failed'
    )),
    input_json JSONB NOT NULL DEFAULT '{}'::jsonb,
    result_json JSONB,
    error_message TEXT NOT NULL DEFAULT '',
    executor_image TEXT NOT NULL,
    children_total INT NOT NULL DEFAULT 0,
    priority INT NOT NULL DEFAULT 10,
    max_attempts_children INT NOT NULL DEFAULT 5,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    finished_at TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS idx_jobs_status ON jobs(status);

ALTER TABLE tasks ADD COLUMN IF NOT EXISTS job_id BIGINT REFERENCES jobs(id) ON DELETE CASCADE;
ALTER TABLE tasks ADD COLUMN IF NOT EXISTS is_job_subtask BOOLEAN NOT NULL DEFAULT false;

CREATE INDEX IF NOT EXISTS idx_tasks_job_sub ON tasks(job_id) WHERE job_id IS NOT NULL;
