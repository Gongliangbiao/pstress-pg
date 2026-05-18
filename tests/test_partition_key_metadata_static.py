from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class PartitionKeyMetadataStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "src/random_test.cpp").read_text()

    def test_partition_key_column_matches_create_definition(self):
        self.assertIn('std::string name = "p_col";', self.source)
        self.assertIn("partition_key_column_name", self.source)
        self.assertIn('" PARTITION BY HASH (" + partition_key + ")"', self.source)
        self.assertNotIn('PARTITION BY HASH (ip_col)', self.source)

    def test_partition_key_is_not_mutated_by_column_ddl(self):
        self.assertIn("is_partition_key_column", self.source)
        self.assertIn("is_partition_key_column(this, col1)", self.source)
        self.assertIn("is_partition_key_column(this, columns_->at(ps))", self.source)


if __name__ == "__main__":
    unittest.main()
