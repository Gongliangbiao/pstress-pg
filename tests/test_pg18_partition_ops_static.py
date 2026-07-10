from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Pg18PartitionOpsStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.common = (ROOT / "src/common.hpp").read_text()
        cls.help = (ROOT / "src/help.cpp").read_text()
        cls.source = (ROOT / "src/random_test.cpp").read_text()
        cls.randomizer = (ROOT / "pstress/randomize_pstress_pg.sh").read_text()

    def test_pg18_partition_ops_option_exists(self):
        self.assertIn("PG18_PARTITION_OPS", self.common)
        self.assertIn('"pg18-partition-ops"', self.help)
        self.assertIn("pg18-partition-ops", self.randomizer)

    def test_pg18_partition_ops_is_version_gated_and_dispatched(self):
        self.assertIn("Option::PG18_PARTITION_OPS)->setInt(0)", self.source)
        self.assertIn("static void pg18_partition_ops", self.source)
        self.assertIn("case Option::PG18_PARTITION_OPS", self.source)

    def test_pg18_partition_ops_sql_shapes_exist(self):
        for fragment in (
            "DETACH PARTITION ",
            " CONCURRENTLY",
            "GENERATED ALWAYS AS (payload + 1)",
            "VIRTUAL) PARTITION BY RANGE",
            "pg18part_pstress_",
        ):
            self.assertIn(fragment, self.source)


if __name__ == "__main__":
    unittest.main()
