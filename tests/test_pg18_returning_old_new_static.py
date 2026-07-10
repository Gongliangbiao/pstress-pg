from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Pg18ReturningOldNewStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.common = (ROOT / "src/common.hpp").read_text()
        cls.help = (ROOT / "src/help.cpp").read_text()
        cls.source = (ROOT / "src/random_test.cpp").read_text()
        cls.randomizer = (ROOT / "pstress/randomize_pstress_pg.sh").read_text()

    def test_returning_old_new_option_exists(self):
        self.assertIn("RETURNING_OLD_NEW", self.common)
        self.assertIn('"returning-old-new"', self.help)
        self.assertIn("RETURNING old/new", self.help)
        self.assertIn("returning-old-new|pg18-explain|pg18-functions",
                      self.randomizer)

    def test_returning_old_new_is_pg18_gated(self):
        self.assertIn("use_returning_old_new", self.source)
        self.assertIn("Option::RETURNING_OLD_NEW)->setInt(0)", self.source)
        self.assertIn("pg_server_at_least(thd, 18)", self.source)

    def test_dml_can_emit_old_new_returning(self):
        self.assertIn('sql += " RETURNING old.*";', self.source)
        self.assertIn('sql += " RETURNING old.*, new.*";', self.source)


if __name__ == "__main__":
    unittest.main()
