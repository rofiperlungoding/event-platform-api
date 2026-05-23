-- Snapshot DB limits + schema state for the audit
SHOW max_connections \gset
SELECT 'max_connections' AS k, current_setting('max_connections') AS v
UNION ALL SELECT 'shared_buffers', current_setting('shared_buffers')
UNION ALL SELECT 'work_mem', current_setting('work_mem')
UNION ALL SELECT 'effective_cache_size', current_setting('effective_cache_size')
UNION ALL SELECT 'wal_level', current_setting('wal_level')
UNION ALL SELECT 'fsync', current_setting('fsync')
UNION ALL SELECT 'synchronous_commit', current_setting('synchronous_commit');

SELECT 'pg_stat_activity rows' AS k, COUNT(*)::text AS v FROM pg_stat_activity;

SELECT relname, pg_size_pretty(pg_total_relation_size(c.oid)) AS sz
FROM pg_class c
WHERE relkind = 'r' AND relnamespace = 'public'::regnamespace
ORDER BY pg_total_relation_size(c.oid) DESC;

SELECT schemaname, relname, n_live_tup, n_dead_tup, n_mod_since_analyze
FROM pg_stat_user_tables ORDER BY n_dead_tup DESC LIMIT 10;
