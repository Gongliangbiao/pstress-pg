from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Pg18FunctionsStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.common = (ROOT / "src/common.hpp").read_text()
        cls.help = (ROOT / "src/help.cpp").read_text()
        cls.source = (ROOT / "src/random_test.cpp").read_text()
        cls.grammar = (ROOT / "src/grammar.sql").read_text()
        cls.randomizer = (ROOT / "pstress/randomize_pstress_pg.sh").read_text()

    def test_pg18_function_option_is_not_separate_parameter(self):
        self.assertNotIn("PG18_FUNCTIONS", self.common)
        self.assertNotIn('"pg18-functions"', self.help)
        self.assertNotIn("pg18-functions", self.randomizer)

    def test_pg18_function_workload_reuses_grammar_sql(self):
        self.assertIn("pg18_only_grammar_sql", self.source)
        self.assertIn("if (pg18_only_grammar_sql(sql) && !pg_server_at_least(thd, 18))",
                      self.source)
        self.assertNotIn("static void pg18_functions(Thd1 *thd)", self.source)
        self.assertNotIn("case Option::PG18_FUNCTIONS", self.source)

    def test_pg18_function_queries_are_present_in_grammar_sql(self):
        for sql_fragment in [
            "uuidv7()",
            "uuidv4()",
            "array_sort",
            "array_reverse",
            "reverse('\\\\x123456'::bytea)",
            "casefold",
            "crc32",
            "crc32c",
            "gamma",
            "lgamma",
            "jsonb_strip_nulls",
            "EXTRACT(WEEK",
            "pg_stat_get_backend_io",
            "pg_stat_get_backend_wal",
            "pg_get_aios",
            "pg_get_loaded_modules",
            "pg_get_wait_events",
            "pg_get_wal_summarizer_state",
            "pg_backend_memory_contexts",
            "pg_stat_io",
            "pg_stat_checkpointer",
        ]:
            self.assertIn(sql_fragment, self.grammar)
        self.assertIn("-- pg18", self.grammar)


if __name__ == "__main__":
    unittest.main()
