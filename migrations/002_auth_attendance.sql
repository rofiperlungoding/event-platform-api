-- Auth + Attendance + Device tables
-- Run: psql -U rofi -d eventplatform -f 002_auth_attendance.sql

ALTER TABLE "Participant" ADD COLUMN IF NOT EXISTS password_hash VARCHAR(128);
ALTER TABLE "Participant" ADD COLUMN IF NOT EXISTS role VARCHAR(16) DEFAULT 'participant';

CREATE TABLE IF NOT EXISTS "Session" (
    id SERIAL PRIMARY KEY,
    code VARCHAR(64) UNIQUE NOT NULL,
    created_by INTEGER REFERENCES "Participant"(id),
    expires_at TIMESTAMP NOT NULL,
    active BOOLEAN DEFAULT true,
    "createdAt" TIMESTAMP DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS "Attendance" (
    id SERIAL PRIMARY KEY,
    participant_id INTEGER REFERENCES "Participant"(id) NOT NULL,
    session_id INTEGER REFERENCES "Session"(id) NOT NULL,
    device_id VARCHAR(64) NOT NULL,
    "checkedInAt" TIMESTAMP DEFAULT NOW(),
    UNIQUE(participant_id, session_id)
);

CREATE TABLE IF NOT EXISTS "Device" (
    id SERIAL PRIMARY KEY,
    device_uuid VARCHAR(64) UNIQUE NOT NULL,
    participant_id INTEGER REFERENCES "Participant"(id),
    user_agent TEXT,
    "linkedAt" TIMESTAMP DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_attendance_session ON "Attendance"(session_id);
CREATE INDEX IF NOT EXISTS idx_attendance_participant ON "Attendance"(participant_id);
CREATE INDEX IF NOT EXISTS idx_device_uuid ON "Device"(device_uuid);
CREATE INDEX IF NOT EXISTS idx_session_code ON "Session"(code);
CREATE INDEX IF NOT EXISTS idx_session_active ON "Session"(active, expires_at);

INSERT INTO _migrations (id, name) VALUES (2, '002_auth_attendance') ON CONFLICT (id) DO NOTHING;