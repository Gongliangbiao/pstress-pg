from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Pg18VacuumAnalyzeOnlyStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.common = (ROOT / "src/common.hpp").read_text()
        cls.help = (ROOT / "src/help.cpp").read_text()
        cls.source = (ROOT / "src/random_test.cpp").read_text()
        cls.randomizer = (ROOT / "pstress/randomize_pstress_pg.sh").read_text()

    def test_pg18_vacuum_analyze_only_option_exists(self):
        self.assertIn("PG18_VACUUM_ANALYZE_ONLY", self.common)
        self.assertIn('"pg18-vacuum-analyze-only"', self.help)
        self.assertIn("pg18-vacuum-analyze-only", self.randomizer)

    def test_pg18_vacuum_analyze_only_is_version_gated(self):
        self.assertIn("Option::PG18_VACUUM_ANALYZE_ONLY)->setInt(0)",
                      self.source)
        self.assertIn("pg18_vacuum_analyze_target", self.source)

    def test_vacuum_and_analyze_can_emit_only(self):
        self.assertIn('"VACUUM " + pg18_vacuum_analyze_target', self.source)
        self.assertIn('"ANALYZE " + pg18_vacuum_analyze_target', self.source)
        self.assertIn('"ONLY "', self.source)


if __name__ == "__main__":
    unittest.main()
