from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ThreadLocalConnectionLossStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "src/random_test.cpp").read_text()
        cls.header = (ROOT / "src/random_test.hpp").read_text()
        cls.grammar = (ROOT / "src/grammar.sql").read_text()

    def test_connection_loss_is_thread_local(self):
        self.assertIn("bool connection_lost = false", self.header)
        self.assertIn("thd->connection_lost = true", self.source)
        connection_block = self.source[
            self.source.index("if (is_connection_lost")
            : self.source.index("} else {", self.source.index("if (is_connection_lost"))
        ]
        self.assertNotIn("run_query_failed = true", connection_block)
        self.assertIn("if (this->connection_lost)", self.source)

    def test_initial_load_failures_remain_global(self):
        self.assertIn("run_query_failed = true", self.source)
        self.assertIn("if (run_query_failed)", self.source)

    def test_grammar_has_postgresql_system_queries(self):
        expected_queries = [
            "SELECT current_database()",
            "SELECT current_schema()",
            "SELECT current_user",
            "SELECT session_user",
            "SELECT inet_server_addr()",
            "SELECT inet_server_port()",
            "SELECT pg_backend_pid()",
            "SELECT pg_postmaster_start_time()",
            "SELECT pg_is_in_recovery()",
            "SELECT pg_current_wal_lsn()",
            "SELECT clock_timestamp()",
            "SELECT statement_timestamp()",
            "SELECT transaction_timestamp()",
            "SELECT count(*) FROM pg_catalog.pg_class",
            "SELECT count(*) FROM pg_catalog.pg_namespace",
        ]
        for query in expected_queries:
            self.assertIn(query, self.grammar)


if __name__ == "__main__":
    unittest.main()
