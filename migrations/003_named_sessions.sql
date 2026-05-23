-- Migration 003 — Named Sessions and Bulk Operations
-- Adds metadata to Session table to support named, persistent sessions
-- (e.g., "Day 1 Morning Plenary") in addition to ad-hoc 30-minute codes.

ALTER TABLE "Session"
    ADD COLUMN IF NOT EXISTS title       VARCHAR(255),
    ADD COLUMN IF NOT EXISTS description TEXT,
    ADD COLUMN IF NOT EXISTS starts_at   TIMESTAMP,
    ADD COLUMN IF NOT EXISTS ends_at     TIMESTAMP;

-- Sessions with a non-null title are "scheduled" sessions whose lifecycle
-- is governed by starts_at/ends_at instead of expires_at + active flag.
CREATE INDEX IF NOT EXISTS idx_session_title    ON "Session"(title);
CREATE INDEX IF NOT EXISTS idx_session_schedule ON "Session"(starts_at, ends_at);

INSERT INTO _migrations (id, name) VALUES (3, '003_named_sessions') ON CONFLICT (id) DO NOTHING;