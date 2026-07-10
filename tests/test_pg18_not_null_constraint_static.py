from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Pg18NotNullConstraintStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.common = (ROOT / "src/common.hpp").read_text()
        cls.help = (ROOT / "src/help.cpp").read_text()
        cls.source = (ROOT / "src/random_test.cpp").read_text()
        cls.randomizer = (ROOT / "pstress/randomize_pstress_pg.sh").read_text()

    def test_pg18_not_null_constraint_option_exists(self):
        self.assertIn("PG18_NOT_NULL_CONSTRAINT", self.common)
        self.assertIn('"pg18-not-null-constraint"', self.help)
        self.assertIn("pg18-not-null-constraint", self.randomizer)

    def test_pg18_not_null_constraint_is_version_gated_and_dispatched(self):
        self.assertIn("Option::PG18_NOT_NULL_CONSTRAINT)->setInt(0)",
                      self.source)
        self.assertIn("static void pg18_not_null_constraint", self.source)
        self.assertIn("case Option::PG18_NOT_NULL_CONSTRAINT", self.source)

    def test_pg18_not_null_constraint_sql_shapes_exist(self):
        for fragment in (
            "ADD CONSTRAINT",
            " NOT NULL ",
            " NOT VALID",
            "VALIDATE CONSTRAINT",
            "ALTER CONSTRAINT",
            " NO INHERIT",
            "DROP CONSTRAINT",
        ):
            self.assertIn(fragment, self.source)


if __name__ == "__main__":
    unittest.main()
