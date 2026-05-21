#!/usr/bin/env node
/**
 * Cleanup load test data
 * Usage: node cleanup.js <DATABASE_URL or via psql directly>
 *
 * Or simpler: just run via psql
 *   psql -U rofi -d eventplatform -c "DELETE FROM \"Attendance\" WHERE device_id LIKE 'loadtest-%'; DELETE FROM \"Device\" WHERE device_uuid LIKE 'loadtest-%'; DELETE FROM \"Participant\" WHERE email LIKE 'loadtest%';"
 */

console.log('Run on tablet:');
console.log('');
console.log('psql -U rofi -d eventplatform -c "DELETE FROM \\"Attendance\\" WHERE device_id LIKE \'loadtest-%\'; DELETE FROM \\"Device\\" WHERE device_uuid LIKE \'loadtest-%\'; DELETE FROM \\"Participant\\" WHERE email LIKE \'loadtest%\';"');
