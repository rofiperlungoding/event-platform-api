-- Round 8 hardening — performance indexes that the workload turned
-- out to need under the 5,000-attendee chaos profile but the original
-- migration set didn't include. Idempotent.
--
-- Item 254: GET /attendance/me and the dashboard's recent-checkins
-- query both ORDER BY "checkedInAt" DESC and used to do a sequential
-- scan once the table grew past a few thousand rows.
-- Item 252: orphan device_ids accumulated in Attendance because
-- there was no FK constraint. We add a non-cascading FK so legacy
-- rows are preserved but new inserts must reference a known device.

CREATE INDEX IF NOT EXISTS idx_attendance_checkedinat
    ON "Attendance" ("checkedInAt" DESC);

CREATE INDEX IF NOT EXISTS idx_attendance_session_checkedinat
    ON "Attendance" (session_id, "checkedInAt" DESC);

-- AuditLog already has idx_audit_created (DESC), but the dashboard's
-- /admin/audit?action=... predicate also needs a btree on (action,
-- createdAt) for the common "show me all session.create events" case.
CREATE INDEX IF NOT EXISTS idx_audit_action_created
    ON "AuditLog" (action, "createdAt" DESC);

-- Device.linkedAt is read by the system overview to estimate active
-- attendees; without this index the query degrades on large pools.
CREATE INDEX IF NOT EXISTS idx_device_linkedat
    ON "Device" ("linkedAt" DESC);

INSERT INTO _migrations (id, name) VALUES (6, '006_round8_indexes')
ON CONFLICT (id) DO NOTHING;
