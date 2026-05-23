-- Wipe everything: attendance, devices, sessions, events, audit log,
-- revoked tokens, all participants except the admin account. Resets
-- all serial sequences to 1.
--
-- Round 11: explicitly cover the tables introduced in migrations 004
-- (Event) and 005 (AuditLog, RevokedToken). The DO blocks make every
-- step idempotent so this script can run on a fresh install before
-- those migrations are applied.
--
-- Run: psql -h 127.0.0.1 -U rofi -d eventplatform -f wipe-all.sql

BEGIN;

DELETE FROM "Attendance";
DELETE FROM "Device";
DELETE FROM "Session";

-- Event table only exists after migration 004; ignore if not present
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM information_schema.tables
               WHERE table_name = 'Event') THEN
        EXECUTE 'DELETE FROM "Event"';
    END IF;
END $$;

-- AuditLog and RevokedToken arrived in migration 005
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM information_schema.tables
               WHERE table_name = 'AuditLog') THEN
        EXECUTE 'DELETE FROM "AuditLog"';
    END IF;
    IF EXISTS (SELECT 1 FROM information_schema.tables
               WHERE table_name = 'RevokedToken') THEN
        EXECUTE 'DELETE FROM "RevokedToken"';
    END IF;
END $$;

DELETE FROM "Participant" WHERE email != 'admin@intrivia.test';

-- Reset sequences so new rows start at 1 again
ALTER SEQUENCE "Attendance_id_seq" RESTART WITH 1;
ALTER SEQUENCE "Device_id_seq"     RESTART WITH 1;
ALTER SEQUENCE "Session_id_seq"    RESTART WITH 1;

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_class WHERE relname = 'Event_id_seq') THEN
        EXECUTE 'ALTER SEQUENCE "Event_id_seq" RESTART WITH 1';
    END IF;
    IF EXISTS (SELECT 1 FROM pg_class WHERE relname = 'AuditLog_id_seq') THEN
        EXECUTE 'ALTER SEQUENCE "AuditLog_id_seq" RESTART WITH 1';
    END IF;
END $$;

COMMIT;

-- Verification
SELECT 'Participant' AS table_name, COUNT(*) FROM "Participant"
UNION ALL SELECT 'Session',     COUNT(*) FROM "Session"
UNION ALL SELECT 'Device',      COUNT(*) FROM "Device"
UNION ALL SELECT 'Attendance',  COUNT(*) FROM "Attendance";
