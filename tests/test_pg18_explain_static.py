from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Pg18ExplainStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.common = (ROOT / "src/common.hpp").read_text()
        cls.help = (ROOT / "src/help.cpp").read_text()
        cls.source = (ROOT / "src/random_test.cpp").read_text()
        cls.randomizer = (ROOT / "pstress/randomize_pstress_pg.sh").read_text()

    def test_pg18_explain_option_exists(self):
        self.assertIn("PG18_EXPLAIN", self.common)
        self.assertIn('"pg18-explain"', self.help)
        self.assertIn("MEMORY, SERIALIZE, or WAL", self.help)
        self.assertIn("returning-old-new|pg18-merge|pg18-copy|pg18-explain",
                      self.randomizer)

    def test_pg18_explain_is_version_gated_and_dispatched(self):
        self.assertIn("Option::PG18_EXPLAIN)->setInt(0)", self.source)
        self.assertIn("static void pg18_explain(Table *table, Thd1 *thd)",
                      self.source)
        self.assertIn("case Option::PG18_EXPLAIN", self.source)

    def test_pg18_explain_queries_use_new_options(self):
        for option in ("MEMORY", "ANALYZE, SERIALIZE TEXT", "ANALYZE, WAL"):
            self.assertIn(option, self.source)
        self.assertIn('EXPLAIN (" + option + ") SELECT * FROM ', self.source)


if __name__ == "__main__":
    unittest.main()
