-- Migration tracking. Run before any other migration on a fresh database.
-- Idempotent.
--
-- Convention:
--   Each subsequent migration MUST end with:
--     INSERT INTO _migrations (id, name) VALUES (NNN, '_filename_')
--     ON CONFLICT (id) DO NOTHING;
--
-- A migration is considered applied when its id is present.

CREATE TABLE IF NOT EXISTS _migrations (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL UNIQUE,
    applied_at  TIMESTAMP DEFAULT NOW()
);

-- Backfill known historical migrations
INSERT INTO _migrations (id, name) VALUES
    (1, '001_init_participant'),
    (2, '002_auth_attendance'),
    (3, '003_named_sessions'),
    (4, '004_multi_event')
ON CONFLICT (id) DO NOTHING;
