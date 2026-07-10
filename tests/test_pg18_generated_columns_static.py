from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Pg18GeneratedColumnsStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.common = (ROOT / "src/common.hpp").read_text()
        cls.help = (ROOT / "src/help.cpp").read_text()
        cls.header = (ROOT / "src/random_test.hpp").read_text()
        cls.source = (ROOT / "src/random_test.cpp").read_text()
        cls.randomizer = (ROOT / "pstress/randomize_pstress_pg.sh").read_text()

    def test_generated_column_kind_option_exists(self):
        self.assertIn("GENERATED_COLUMN_KIND", self.common)
        self.assertIn('"generated-column-kind"', self.help)
        self.assertIn("random, virtual, or stored", self.help)
        self.assertIn("generated-column-kind)", self.randomizer)

    def test_generated_column_kind_is_version_gated(self):
        self.assertIn("normalized_generated_column_kind", self.source)
        self.assertIn("current_server_at_least(18)", self.source)
        self.assertIn('return "stored";', self.source)
        self.assertIn("invalid --generated-column-kind", self.source)

    def test_virtual_and_stored_clauses_are_generated_and_serialized(self):
        self.assertIn("GENERATED_KIND", self.header)
        self.assertIn("generated_kind_string", self.header)
        self.assertIn('generated_kind == VIRTUAL ? "VIRTUAL" : "STORED"', self.source)
        self.assertIn('"generated_kind"', self.source)
        self.assertIn('metadata_string_member(col, "generated_kind", nullptr, "stored")',
                      self.source)
        self.assertIn("Generated_Column::VIRTUAL", self.source)
        self.assertIn("pg_indexable_column", self.source)


if __name__ == "__main__":
    unittest.main()
