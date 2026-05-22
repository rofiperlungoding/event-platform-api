-- Seed N participants + linked devices for stampede testing.
-- Usage:  psql -v n=2000 -v run_id=$(date +%s) -d eventplatform -f seed.sql
--
-- Idempotent on (email, device_uuid). Safe to re-run with same run_id.

\set N :n
\set RUN :run_id

BEGIN;

-- Participants: stamp-{run_id}-{i}@test.local, name "Stamp i"
INSERT INTO "Participant" (name, email, team, password_hash, role, "createdAt", "updatedAt")
SELECT
    'Stamp ' || i,
    'stamp-' || :'RUN' || '-' || i || '@test.local',
    'T' || (i % 50),
    'x',                   -- placeholder hash; these accounts never log in
    'participant',
    NOW(), NOW()
FROM generate_series(0, :N - 1) AS s(i)
ON CONFLICT (email) DO NOTHING;

-- Devices: device_uuid = dev-stamp-{run_id}-{participant_id}
INSERT INTO "Device" (device_uuid, participant_id, user_agent, "linkedAt")
SELECT
    'dev-stamp-' || :'RUN' || '-' || p.id,
    p.id,
    'stampede-seed',
    NOW()
FROM "Participant" p
WHERE p.email LIKE 'stamp-' || :'RUN' || '-%@test.local'
ON CONFLICT (device_uuid) DO NOTHING;

COMMIT;

-- Report
SELECT
    (SELECT COUNT(*) FROM "Participant" WHERE email LIKE 'stamp-' || :'RUN' || '-%@test.local') AS participants,
    (SELECT COUNT(*) FROM "Device"      WHERE device_uuid LIKE 'dev-stamp-' || :'RUN' || '-%')    AS devices;
