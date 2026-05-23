-- Migration 004 — Multi-Event Support
-- Introduces the Event entity. Sessions and participants may optionally
-- be scoped to a specific event. Existing rows are migrated to a
-- "legacy" event for backward compatibility.

CREATE TABLE IF NOT EXISTS "Event" (
    id          SERIAL PRIMARY KEY,
    slug        VARCHAR(64) UNIQUE NOT NULL,
    name        VARCHAR(255) NOT NULL,
    description TEXT,
    starts_at   TIMESTAMP,
    ends_at     TIMESTAMP,
    created_by  INTEGER REFERENCES "Participant"(id),
    "createdAt" TIMESTAMP DEFAULT NOW(),
    "updatedAt" TIMESTAMP DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_event_slug     ON "Event"(slug);
CREATE INDEX IF NOT EXISTS idx_event_schedule ON "Event"(starts_at, ends_at);

-- Add event_id to Session and Participant. NULL means "no event scope" 
-- (backward compatible). New sessions can be tied to events.
ALTER TABLE "Session"     ADD COLUMN IF NOT EXISTS event_id INTEGER REFERENCES "Event"(id) ON DELETE SET NULL;
ALTER TABLE "Participant" ADD COLUMN IF NOT EXISTS event_id INTEGER REFERENCES "Event"(id) ON DELETE SET NULL;

CREATE INDEX IF NOT EXISTS idx_session_event     ON "Session"(event_id);
CREATE INDEX IF NOT EXISTS idx_participant_event ON "Participant"(event_id);

-- Create a default event for legacy data
INSERT INTO "Event" (slug, name, description, "createdAt")
VALUES ('legacy', 'Legacy Data', 'Imported from pre-multi-event installation', NOW())
ON CONFLICT (slug) DO NOTHING;

INSERT INTO _migrations (id, name) VALUES (4, '004_multi_event') ON CONFLICT (id) DO NOTHING;