CREATE TABLE IF NOT EXISTS projects (
    id BIGSERIAL PRIMARY KEY,
    name TEXT NOT NULL,
    version TEXT NOT NULL,
    executor_url TEXT NOT NULL,
    checksum TEXT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(name, version)
);

CREATE TABLE IF NOT EXISTS worker_nodes (
    id BIGSERIAL PRIMARY KEY,
    host TEXT NOT NULL UNIQUE,
    status TEXT NOT NULL CHECK (status IN ('online', 'offline', 'degraded')),
    cpu_cores INT NOT NULL,
    ram_mb INT NOT NULL,
    performance_score DOUBLE PRECISION NOT NULL,
    last_heartbeat TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    throughput_history DOUBLE PRECISION NOT NULL DEFAULT 0,
    failure_rate DOUBLE PRECISION NOT NULL DEFAULT 0,
    current_load INT NOT NULL DEFAULT 0,
    avg_latency_ms DOUBLE PRECISION NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS tasks (
    id BIGSERIAL PRIMARY KEY,
    project_id BIGINT NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
    payload_json JSONB NOT NULL,
    priority INT NOT NULL DEFAULT 0,
    status TEXT NOT NULL CHECK (status IN ('queued','assigned','running','succeeded','failed','retry_wait','stale')),
    assigned_node_id BIGINT REFERENCES worker_nodes(id),
    attempts INT NOT NULL DEFAULT 0,
    max_attempts INT NOT NULL DEFAULT 5,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    started_at TIMESTAMPTZ,
    finished_at TIMESTAMPTZ,
    timeout_at TIMESTAMPTZ,
    next_retry_at TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS idx_tasks_status_priority ON tasks(status, priority DESC, created_at);

CREATE TABLE IF NOT EXISTS task_results (
    id BIGSERIAL PRIMARY KEY,
    task_id BIGINT NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
    node_id BIGINT NOT NULL REFERENCES worker_nodes(id) ON DELETE CASCADE,
    success BOOLEAN NOT NULL,
    result_json JSONB NOT NULL DEFAULT '{}'::jsonb,
    error_message TEXT NOT NULL DEFAULT '',
    execution_time_ms BIGINT NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(task_id, node_id)
);

CREATE TABLE IF NOT EXISTS node_executor_install (
    id BIGSERIAL PRIMARY KEY,
    node_id BIGINT NOT NULL REFERENCES worker_nodes(id) ON DELETE CASCADE,
    project_id BIGINT NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
    installed_version TEXT NOT NULL,
    status TEXT NOT NULL CHECK (status IN ('pending', 'installed', 'failed')),
    installed_at TIMESTAMPTZ,
    UNIQUE(node_id, project_id)
);
