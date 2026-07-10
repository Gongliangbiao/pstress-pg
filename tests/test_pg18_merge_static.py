from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Pg18MergeStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.common = (ROOT / "src/common.hpp").read_text()
        cls.help = (ROOT / "src/help.cpp").read_text()
        cls.source = (ROOT / "src/random_test.cpp").read_text()
        cls.randomizer = (ROOT / "pstress/randomize_pstress_pg.sh").read_text()

    def test_pg18_merge_option_exists(self):
        self.assertIn("PG18_MERGE", self.common)
        self.assertIn('"pg18-merge"', self.help)
        self.assertIn("pg18-merge", self.randomizer)

    def test_pg18_merge_is_version_gated_and_dispatched(self):
        self.assertIn("Option::PG18_MERGE)->setInt(0)", self.source)
        self.assertIn("static void pg18_merge", self.source)
        self.assertIn("case Option::PG18_MERGE", self.source)

    def test_pg18_merge_returning_old_new_sql_shape_exists(self):
        for fragment in (
            "MERGE INTO ",
            " USING (VALUES ",
            "WHEN MATCHED THEN UPDATE",
            "WHEN NOT MATCHED THEN INSERT",
            " RETURNING old.*, new.*",
        ):
            self.assertIn(fragment, self.source)


if __name__ == "__main__":
    unittest.main()
