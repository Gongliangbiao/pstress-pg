from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Pg18FunctionsStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.common = (ROOT / "src/common.hpp").read_text()
        cls.help = (ROOT / "src/help.cpp").read_text()
        cls.source = (ROOT / "src/random_test.cpp").read_text()
        cls.randomizer = (ROOT / "pstress/randomize_pstress_pg.sh").read_text()

    def test_pg18_function_option_is_parameterized(self):
        self.assertIn("PG18_FUNCTIONS", self.common)
        self.assertIn('"pg18-functions"', self.help)
        self.assertIn("pg18-functions", self.randomizer)

    def test_pg18_function_workload_has_version_gate_and_dispatch(self):
        self.assertIn("pg_server_at_least(thd, 18)", self.source)
        self.assertIn("options->at(Option::PG18_FUNCTIONS)->setInt(0)", self.source)
        self.assertIn("static void pg18_functions(Thd1 *thd)", self.source)
        self.assertIn("case Option::PG18_FUNCTIONS", self.source)

    def test_pg18_function_queries_are_present(self):
        for sql_fragment in [
            "uuidv7()",
            "uuidv4()",
            "array_sort",
            "array_reverse",
            "reverse('\\\\\\\\x123456'::bytea)",
            "casefold",
            "crc32",
            "crc32c",
            "gamma",
            "lgamma",
            "jsonb_strip_nulls",
            "EXTRACT(WEEK",
        ]:
            self.assertIn(sql_fragment, self.source)


if __name__ == "__main__":
    unittest.main()
