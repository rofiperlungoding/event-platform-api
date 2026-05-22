-- Clean up leftover stampede test data
DELETE FROM "Attendance" WHERE participant_id IN (
    SELECT id FROM "Participant" WHERE email LIKE 'stamp-%@test.local'
);
DELETE FROM "Device" WHERE device_uuid LIKE 'dev-stamp-%';
DELETE FROM "Participant" WHERE email LIKE 'stamp-%@test.local';
SELECT COUNT(*) AS remaining_participants FROM "Participant";
