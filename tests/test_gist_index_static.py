from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GistIndexStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.common = (ROOT / "src/common.hpp").read_text()
        cls.help = (ROOT / "src/help.cpp").read_text()
        cls.header = (ROOT / "src/random_test.hpp").read_text()
        cls.source = (ROOT / "src/random_test.cpp").read_text()
        cls.randomizer = (ROOT / "pstress/randomize_pstress_pg.sh").read_text()

    def test_gist_index_option_exists(self):
        self.assertIn("GIST_INDEX", self.common)
        self.assertIn('"gist-index"', self.help)
        self.assertIn("gist-index", self.randomizer)

    def test_gist_index_method_and_dispatch_exist(self):
        self.assertIn("AddGistIndex(", self.header)
        self.assertIn("void Table::AddGistIndex", self.source)
        self.assertIn("case Option::GIST_INDEX", self.source)

    def test_gist_index_sql_shape_and_candidate_types_exist(self):
        self.assertIn("USING gist", self.source)
        for fragment in (
            "Column::POINT",
            "Column::BOX",
            "Column::CIRCLE",
            "Column::INT4RANGE",
            "Column::TSTZRANGE",
            "Column::INET",
            "inet_ops",
        ):
            self.assertIn(fragment, self.source)


if __name__ == "__main__":
    unittest.main()
