-- Participant base table — the foundation referenced by all later
-- migrations. Idempotent.

CREATE TABLE IF NOT EXISTS "Participant" (
    id           SERIAL PRIMARY KEY,
    name         VARCHAR(256) NOT NULL,
    email        VARCHAR(256) NOT NULL UNIQUE,
    team         VARCHAR(128),
    "createdAt"  TIMESTAMP DEFAULT NOW(),
    "updatedAt"  TIMESTAMP DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_participant_email     ON "Participant"(email);
CREATE INDEX IF NOT EXISTS idx_participant_team      ON "Participant"(team);
CREATE INDEX IF NOT EXISTS idx_participant_createdat ON "Participant"("createdAt" DESC);
