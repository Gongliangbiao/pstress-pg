from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class PgMaintenanceOpsStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.common = (ROOT / "src/common.hpp").read_text()
        cls.help = (ROOT / "src/help.cpp").read_text()
        cls.header = (ROOT / "src/random_test.hpp").read_text()
        cls.source = (ROOT / "src/random_test.cpp").read_text()
        cls.randomizer = (ROOT / "pstress/randomize_pstress_pg.sh").read_text()

    def test_options_are_parameterized(self):
        for enum_name, cli_name in [
            ("VACUUM_TABLE", "vacuum"),
            ("VACUUM_FULL", "vacuum-full"),
            ("CHECKPOINT", "checkpoint"),
            ("CREATE_INDEX_CONCURRENTLY", "create-index-concurrently"),
            ("REINDEX", "reindex"),
            ("CLUSTER_TABLE", "cluster-table"),
            ("BRIN_EXPRESSION_INDEX", "brin-expression-index"),
            ("CREATE_MATVIEW", "create-matview"),
            ("REFRESH_MATVIEW_CONCURRENTLY", "refresh-matview-concurrently"),
            ("SELECT_MATVIEW", "select-matview"),
            ("DROP_MATVIEW", "drop-matview"),
            ("PREPARED_TRANSACTION_STRESS", "prepared-tx-stress"),
        ]:
            self.assertIn(enum_name, self.common)
            self.assertIn(f'"{cli_name}"', self.help)
            self.assertIn(cli_name, self.randomizer)

    def test_table_methods_and_dispatch_exist(self):
        for method in [
            "Vacuum(",
            "VacuumFull(",
            "Checkpoint(",
            "AddIndexConcurrently(",
            "Reindex(",
            "ClusterTable(",
            "AddBrinExpressionIndex(",
            "SelectMatview(",
            "DropMatview(",
        ]:
            self.assertIn(method, self.header)
            self.assertIn(f"void Table::{method}", self.source)

        for enum_name in [
            "VACUUM_TABLE",
            "VACUUM_FULL",
            "CHECKPOINT",
            "CREATE_INDEX_CONCURRENTLY",
            "REINDEX",
            "CLUSTER_TABLE",
            "BRIN_EXPRESSION_INDEX",
            "CREATE_MATVIEW",
            "REFRESH_MATVIEW_CONCURRENTLY",
            "SELECT_MATVIEW",
            "DROP_MATVIEW",
            "PREPARED_TRANSACTION_STRESS",
        ]:
            self.assertIn(f"case Option::{enum_name}", self.source)

    def test_sql_operations_are_emitted(self):
        for sql in [
            "VACUUM ",
            "VACUUM FULL ",
            "CHECKPOINT",
            "CREATE MATERIALIZED VIEW ",
            "REFRESH MATERIALIZED VIEW CONCURRENTLY ",
            "DROP MATERIALIZED VIEW IF EXISTS ",
            "INDEX CONCURRENTLY ",
            "REINDEX INDEX CONCURRENTLY ",
            "CLUSTER ",
            "USING brin",
            "PREPARE TRANSACTION",
            "COMMIT PREPARED",
            "ROLLBACK PREPARED",
        ]:
            self.assertIn(sql, self.source)


if __name__ == "__main__":
    unittest.main()
