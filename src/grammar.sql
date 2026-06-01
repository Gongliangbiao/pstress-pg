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
SELECT clock_timestamp()
SELECT statement_timestamp()
SELECT transaction_timestamp()
SELECT count(*) FROM pg_catalog.pg_class
SELECT count(*) FROM pg_catalog.pg_namespace
SELECT datname FROM pg_catalog.pg_database ORDER BY datname
