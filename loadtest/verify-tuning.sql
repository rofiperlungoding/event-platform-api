SELECT 'max_connections' AS k, current_setting('max_connections') AS v
UNION ALL SELECT 'synchronous_commit', current_setting('synchronous_commit')
UNION ALL SELECT 'wal_compression', current_setting('wal_compression')
UNION ALL SELECT 'shared_buffers', current_setting('shared_buffers')
UNION ALL SELECT 'work_mem', current_setting('work_mem')
UNION ALL SELECT 'effective_cache_size', current_setting('effective_cache_size')
UNION ALL SELECT 'autovacuum_naptime', current_setting('autovacuum_naptime');
