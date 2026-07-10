SELECT * FROM T1 JOIN T2 ON T1_INT_1 = T2_INT_1 ORDER BY T2_VARCHAR_1 DESC, T1_VARCHAR_1 
SELECT version()
SELECT * FROM T1
SELECT NOW()
SELECT current_database()
SELECT current_schema()
SELECT current_user
SELECT session_user
SELECT inet_server_addr()
SELECT inet_server_port()
SELECT pg_backend_pid()
SELECT pg_postmaster_start_time()
SELECT pg_is_in_recovery()
SELECT pg_current_wal_lsn()
SELECT pg_current_wal_insert_lsn()
SELECT pg_current_wal_flush_lsn()
SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), '0/0')
SELECT pg_current_xact_id_if_assigned()
SELECT pg_snapshot_xmin(pg_current_snapshot())
SELECT pg_blocking_pids(pg_backend_pid())
SELECT pg_conf_load_time()
SELECT clock_timestamp()
SELECT statement_timestamp()
SELECT transaction_timestamp()
SELECT count(*) FROM pg_catalog.pg_class
SELECT count(*) FROM pg_catalog.pg_namespace
SELECT datname FROM pg_catalog.pg_database ORDER BY datname
SELECT word, catdesc FROM pg_catalog.pg_get_keywords() ORDER BY word LIMIT 10
SELECT name, setting FROM pg_catalog.pg_settings WHERE name IN ('server_version', 'server_version_num', 'max_connections') ORDER BY name
SELECT backend_type, state, wait_event_type FROM pg_catalog.pg_stat_activity WHERE pid = pg_backend_pid()
SELECT wal_records, wal_fpi, wal_bytes FROM pg_catalog.pg_stat_wal
SELECT backend_type, object, context, reads, writes FROM pg_catalog.pg_stat_io LIMIT 10
SELECT name, total_bytes, used_bytes FROM pg_catalog.pg_backend_memory_contexts LIMIT 10
SELECT uuidv7(), uuidv4() -- pg18
SELECT array_sort(ARRAY[3, 1, 2]), array_reverse(ARRAY[1, 2, 3]) -- pg18
SELECT reverse('\\x123456'::bytea) -- pg18
SELECT casefold(U&'Stra\00DFe') -- pg18
SELECT crc32('postgres'::bytea), crc32c('postgres'::bytea) -- pg18
SELECT gamma(5.0), lgamma(5.0) -- pg18
SELECT jsonb_strip_nulls('{"a": null, "b": [1, null], "c": {"d": null}}'::jsonb, true) -- pg18
SELECT EXTRACT(WEEK FROM TIMESTAMP '2026-01-05') -- pg18
SELECT backend_type, object, context, reads, read_bytes, writes, write_bytes FROM pg_stat_get_backend_io(pg_backend_pid()) LIMIT 8 -- pg18
SELECT wal_records, wal_fpi, wal_bytes FROM pg_stat_get_backend_wal(pg_backend_pid()) -- pg18
SELECT pid, io_id, state, operation FROM pg_get_aios() LIMIT 8 -- pg18
SELECT module_name, version FROM pg_get_loaded_modules() LIMIT 8 -- pg18
SELECT type, name FROM pg_get_wait_events() LIMIT 16 -- pg18
SELECT * FROM pg_get_wal_summarizer_state() -- pg18
SELECT name, level, total_bytes, used_bytes FROM pg_backend_memory_contexts LIMIT 16 -- pg18
SELECT backend_type, object, context, read_bytes, write_bytes, extend_bytes FROM pg_stat_io LIMIT 16 -- pg18
SELECT num_done, restartpoints_done, slru_written FROM pg_stat_checkpointer -- pg18
