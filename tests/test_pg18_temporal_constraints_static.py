from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Pg18TemporalConstraintsStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.common = (ROOT / "src/common.hpp").read_text()
        cls.help = (ROOT / "src/help.cpp").read_text()
        cls.source = (ROOT / "src/random_test.cpp").read_text()
        cls.randomizer = (ROOT / "pstress/randomize_pstress_pg.sh").read_text()

    def test_pg18_temporal_constraints_option_exists(self):
        self.assertIn("PG18_TEMPORAL_CONSTRAINTS", self.common)
        self.assertIn('"pg18-temporal-constraints"', self.help)
        self.assertIn("pg18-temporal-constraints", self.randomizer)

    def test_pg18_temporal_constraints_is_version_gated_and_dispatched(self):
        self.assertIn("Option::PG18_TEMPORAL_CONSTRAINTS)->setInt(0)", self.source)
        self.assertIn("static void pg18_temporal_constraints", self.source)
        self.assertIn("case Option::PG18_TEMPORAL_CONSTRAINTS", self.source)

    def test_pg18_temporal_constraints_sql_shapes_exist(self):
        for fragment in (
            "CREATE EXTENSION IF NOT EXISTS btree_gist",
            "WITHOUT OVERLAPS",
            "PERIOD valid_at",
            "REFERENCES",
            "DROP TABLE",
            "temporal_pstress_",
        ):
            self.assertIn(fragment, self.source)


if __name__ == "__main__":
    unittest.main()
