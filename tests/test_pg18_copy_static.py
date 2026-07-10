from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Pg18CopyStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.common = (ROOT / "src/common.hpp").read_text()
        cls.help = (ROOT / "src/help.cpp").read_text()
        cls.source = (ROOT / "src/random_test.cpp").read_text()
        cls.randomizer = (ROOT / "pstress/randomize_pstress_pg.sh").read_text()

    def test_pg18_copy_options_exist(self):
        for option in ("PG18_COPY", "PG18_COPY_MODE",
                       "PG18_COPY_REJECT_LIMIT",
                       "PG18_COPY_LOG_VERBOSITY"):
            self.assertIn(option, self.common)
        for name in ("pg18-copy", "pg18-copy-mode",
                     "pg18-copy-reject-limit",
                     "pg18-copy-log-verbosity"):
            self.assertIn(f'"{name}"', self.help)
        self.assertIn("pg18-copy-mode)", self.randomizer)
        self.assertIn("pg18-copy-log-verbosity)", self.randomizer)

    def test_pg18_copy_is_version_gated_and_dispatched(self):
        self.assertIn("Option::PG18_COPY)->setInt(0)", self.source)
        self.assertIn("static void pg18_copy(Table *table, Thd1 *thd)",
                      self.source)
        self.assertIn("case Option::PG18_COPY", self.source)

    def test_pg18_copy_uses_libpq_copy_protocol(self):
        for symbol in ("PGRES_COPY_IN", "PGRES_COPY_OUT",
                       "PQputCopyData", "PQputCopyEnd", "PQgetCopyData"):
            self.assertIn(symbol, self.source)
        self.assertIn("ON_ERROR ignore", self.source)
        self.assertIn("REJECT_LIMIT", self.source)
        self.assertIn("LOG_VERBOSITY", self.source)
        self.assertIn("COPY (SELECT * FROM ", self.source)
        self.assertIn("COPY \" + mv_name + \" TO STDOUT", self.source)


if __name__ == "__main__":
    unittest.main()
