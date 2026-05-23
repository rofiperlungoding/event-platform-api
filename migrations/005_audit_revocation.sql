-- Audit log + token revocation list + admin event tracking.
-- Mitigates audit items 91, 92, 122. Idempotent.

-- ─── Token revocation list ────────────────────────────────────────────
-- Tokens are stateless HMAC, but we can deny-list specific tokens by
-- their HMAC tag prefix until their natural expiry. The set is small
-- because tokens auto-expire after 24h.
CREATE TABLE IF NOT EXISTS "RevokedToken" (
    sig_prefix  CHAR(16) PRIMARY KEY,        -- first 16 hex chars of the HMAC tag
    revoked_at  TIMESTAMP DEFAULT NOW(),
    expires_at  TIMESTAMP NOT NULL,           -- when the underlying token would expire anyway
    reason      VARCHAR(64)
);

CREATE INDEX IF NOT EXISTS idx_revoked_expires ON "RevokedToken"(expires_at);

-- ─── Audit log ────────────────────────────────────────────────────────
-- Every privileged action (admin operations, session lifecycle, deletes,
-- bulk imports, seed-stamp/cleanup) writes here. This is the canonical
-- log; pm2 stderr is supplementary.
CREATE TABLE IF NOT EXISTS "AuditLog" (
    id              SERIAL PRIMARY KEY,
    "actorId"       INTEGER REFERENCES "Participant"(id),
    actor_email     VARCHAR(256),             -- denormalised for post-deletion auditability
    action          VARCHAR(64) NOT NULL,     -- e.g. "session.create", "participant.delete"
    target_type     VARCHAR(32),              -- "session", "participant", "device"
    target_id       VARCHAR(64),              -- string to support varied id types
    client_ip       VARCHAR(64),
    metadata        TEXT,                     -- free-form JSON, optional
    "createdAt"     TIMESTAMP DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_audit_actor   ON "AuditLog"("actorId");
CREATE INDEX IF NOT EXISTS idx_audit_action  ON "AuditLog"(action);
CREATE INDEX IF NOT EXISTS idx_audit_created ON "AuditLog"("createdAt" DESC);

INSERT INTO _migrations (id, name) VALUES (5, '005_audit_revocation')
ON CONFLICT (id) DO NOTHING;
