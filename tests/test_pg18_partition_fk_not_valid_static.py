from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Pg18PartitionFkNotValidStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.common = (ROOT / "src/common.hpp").read_text()
        cls.help = (ROOT / "src/help.cpp").read_text()
        cls.source = (ROOT / "src/random_test.cpp").read_text()
        cls.randomizer = (ROOT / "pstress/randomize_pstress_pg.sh").read_text()

    def test_pg18_partition_fk_not_valid_option_exists(self):
        self.assertIn("PG18_PARTITION_FK_NOT_VALID", self.common)
        self.assertIn('"pg18-partition-fk-not-valid"', self.help)
        self.assertIn("pg18-partition-fk-not-valid", self.randomizer)

    def test_pg18_partition_fk_not_valid_is_version_gated_and_dispatched(self):
        self.assertIn("Option::PG18_PARTITION_FK_NOT_VALID)->setInt(0)", self.source)
        self.assertIn("static void pg18_partition_fk_not_valid", self.source)
        self.assertIn("case Option::PG18_PARTITION_FK_NOT_VALID", self.source)

    def test_pg18_partition_fk_not_valid_sql_shapes_exist(self):
        for fragment in (
            "pg18_fk_nv_pstress_",
            "PARTITION BY RANGE",
            "PARTITION OF",
            "FOREIGN KEY",
            " REFERENCES ",
            " NOT VALID",
            "VALIDATE CONSTRAINT",
            "DROP TABLE IF EXISTS",
        ):
            self.assertIn(fragment, self.source)


if __name__ == "__main__":
    unittest.main()
