#include "common.hpp"
#include "pstress.hpp"
#include <iostream>
#include <libpq-fe.h>

Opx *options = new Opx;

static std::string pq_client_version_string() {
  const int version = PQlibVersion();
  return std::to_string(version / 10000) + "." +
         std::to_string((version / 100) % 100) + "." +
         std::to_string(version % 100);
}

/* add new options */
inline Option *newOption(Option::Type t, Option::Opt o, std::string s) {
  auto *opt = new Option(t, o, s);
  options->at(o) = opt;
  return opt;
}

/* All available options */
void add_options() {
  options->resize(Option::MAX);
  Option *opt;

  /* Mode of Pstress */
  opt = newOption(Option::BOOL, Option::PQUERY, "pquery");
  opt->help = "run pstress as pquery 2.0. sqls will be executed from --infine "
              "in some order based on shuffle. basically it will run in pquery "
              "mode you can also use -k";
  opt->setBool(false); // todo disable in release
  opt->setArgs(no_argument);

  /* Intial Seed for test */
  opt = newOption(Option::INT, Option::INITIAL_SEED, "seed");
  opt->help = "Initial seed used for the test";
  opt->setInt(1);

  /* Just Load DDL*/
  opt = newOption(Option::BOOL, Option::JUST_LOAD_DDL, "jlddl");
  opt->help = "load DDL and exit";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* DDL option */
  opt = newOption(Option::BOOL, Option::NO_DDL, "no-ddl");
  opt->help = "do not use ddl in workload";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* Only command line sql option */
  opt = newOption(Option::BOOL, Option::ONLY_CL_SQL, "only-cl-sql");
  opt->help = "only run command line sql. other sql will be disable";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* Only command line ddl option */
  opt = newOption(Option::BOOL, Option::ONLY_CL_DDL, "only-cl-ddl");
  opt->help = "only run command line ddl. other ddl will be disable";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* disable virtual columns*/
  opt = newOption(Option::BOOL, Option::NO_VIRTUAL_COLUMNS, "no-generated-columns");
  opt->help = "disable generated columns";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* disable blob,text columns*/
  opt = newOption(Option::BOOL, Option::NO_BLOB, "no-blob");
  opt->help = "Disable blob columns";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* Initial Table */
  opt = newOption(Option::INT, Option::TABLES, "tables");
  opt->help = "Number of initial tables";
  opt->setInt(10);

  /* Number of indexes in a table */
  opt = newOption(Option::INT, Option::INDEXES, "indexes");
  opt->help = "maximum indexes in a table,default depends on page-size as well";
  opt->setInt(7);

  /* algorithm for alter */
  opt = newOption(Option::STRING, Option::ALGORITHM, "alter-algorithm");
  opt->help = "algorithm used in alter table.\n"
              "--alter-algorithm INPLACE,COPY,DEFAULT\n --alter-algorithm all "
              "means randomly one of them will be picked. PostgreSQL ignores "
              "this setting. Pass options comma separated without space";
  opt->setString("all");

  /* lock for alter */
  opt = newOption(Option::STRING, Option::LOCK, "alter-lock");
  opt->help = "lock mechanism used in alter table.\n "
              "--alter-lock DEFAULT,NONE,SHARED,EXCLUSIVE\n --alter-lock all "
              "means randomly one of them will be picked. PostgreSQL ignores "
              "this setting. Pass options comma separated without space";
  opt->setString("all");

  /* Number of columns in a table */
  opt = newOption(Option::INT, Option::COLUMNS, "columns");
  opt->help = "maximum columns in a table, default depends on page-size, "
              "branch. for 8.0 it is 7 for 5.7 it 10";
  opt->setInt(10);

  /* Number of columns in an index of a table */
  opt = newOption(Option::INT, Option::INDEX_COLUMNS, "index-columns");
  opt->help = "maximum columns in an index of a table, default depends on "
              "page-size as well";
  opt->setInt(10);

  /* autoinc column */
  opt = newOption(Option::BOOL, Option::NO_AUTO_INC, "no-auto-inc");
  opt->help = "Disable auto inc columns in table, including pkey";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* desc index support */
  opt = newOption(Option::BOOL, Option::NO_DESC_INDEX, "no-desc-index");
  opt->help = "Disable index with desc on tables ";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* Only Partition tables */
  opt =
      newOption(Option::BOOL, Option::ONLY_PARTITION, "only-partition-tables");
  opt->help = "Work only on Partition tables";
  opt->setArgs(no_argument);
  opt->setBool(false);

  /* Only Temporary tables */
  opt = newOption(Option::BOOL, Option::ONLY_TEMPORARY, "only-temp-tables");
  opt->help = "Work only on temporary tables";
  opt->setArgs(no_argument);
  opt->setBool(false);

  /* Only Unlogged tables */
  opt = newOption(Option::BOOL, Option::ONLY_UNLOGGED, "only-unlogged-tables");
  opt->help = "Work only on unlogged tables";
  opt->setArgs(no_argument);
  opt->setBool(false);

  /* No FK tables */
  opt = newOption(Option::BOOL, Option::NO_FK, "no-fk-tables");
  opt->help = "do not work on foriegn tables";
  opt->setArgs(no_argument);
  opt->setBool(false);

  /* No Partition tables */
  opt = newOption(Option::BOOL, Option::NO_PARTITION, "no-partition-tables");
  opt->help = "do not work on partition tables";
  opt->setArgs(no_argument);
  opt->setBool(false);

  /* NO Temporary tables */
  opt = newOption(Option::BOOL, Option::NO_TEMPORARY, "no-temp-tables");
  opt->help = "do not work on temporary tables";
  opt->setArgs(no_argument);
  opt->setBool(false);

  /* NO Unlogged tables */
  opt = newOption(Option::BOOL, Option::NO_UNLOGGED, "no-unlogged-tables");
  opt->help = "do not work on unlogged tables";
  opt->setArgs(no_argument);
  opt->setBool(false);

  opt = newOption(Option::INT, Option::FK_PROB, "fk-prob");
  opt->help = R"(
    Probability of each normal table having the FK. Currently, FKs are only linked
    to the primary key of the parent table. So, even with 100% probability, a table
    will have an FK only if its parent has a primary key.
  )";
  opt->setInt(50);

  opt = newOption(Option::INT, Option::PARTITION_PROB, "partition-prob");
  opt->help = "Probability of parititon tables";
  opt->setInt(10);

  /* Ratio of temporary table to normal table */
  opt = newOption(Option::INT, Option::TEMPORARY_PROB, "temporary-prob");
  opt->help = "Probability of temporary tables";
  opt->setInt(10);

  opt = newOption(Option::INT, Option::UNLOGGED_PROB, "unlogged-prob");
  opt->help = "Probability of unlogged tables";
  opt->setInt(10);

  opt = newOption(Option::INT, Option::VACUUM_TABLE, "vacuum");
  opt->help = "VACUUM table or partition child";
  opt->setInt(3);
  opt->setSQL();
  opt->setDDL();

  opt = newOption(Option::INT, Option::VACUUM_FULL, "vacuum-full");
  opt->help = "VACUUM FULL table or partition child";
  opt->setInt(3);
  opt->setSQL();
  opt->setDDL();

  opt = newOption(Option::INT, Option::CHECKPOINT, "checkpoint");
  opt->help = "CHECKPOINT command";
  opt->setInt(1);
  opt->setSQL();
  opt->setDDL();

  opt = newOption(Option::INT, Option::CREATE_INDEX_CONCURRENTLY,
                  "create-index-concurrently");
  opt->help = "create index concurrently on a random table";
  opt->setInt(5);
  opt->setSQL();
  opt->setDDL();

  opt = newOption(Option::INT, Option::REINDEX, "reindex");
  opt->help = "REINDEX table or index";
  opt->setInt(3);
  opt->setSQL();
  opt->setDDL();

  opt = newOption(Option::INT, Option::CLUSTER_TABLE, "cluster-table");
  opt->help = "CLUSTER table on a random index";
  opt->setInt(2);
  opt->setSQL();
  opt->setDDL();

  opt = newOption(Option::INT, Option::BRIN_EXPRESSION_INDEX,
                  "brin-expression-index");
  opt->help = "create BRIN, expression, or GIN index on a random table";
  opt->setInt(3);
  opt->setSQL();
  opt->setDDL();

  opt = newOption(Option::INT, Option::CREATE_MATVIEW, "create-matview");
  opt->help = "CREATE MATERIALIZED VIEW from a random table";
  opt->setInt(5);
  opt->setSQL();
  opt->setDDL();

  opt = newOption(Option::INT, Option::REFRESH_MATVIEW_CONCURRENTLY,
                  "refresh-matview-concurrently");
  opt->help = "REFRESH MATERIALIZED VIEW CONCURRENTLY with non-concurrent fallback";
  opt->setInt(5);
  opt->setSQL();
  opt->setDDL();

  opt = newOption(Option::INT, Option::SELECT_MATVIEW, "select-matview");
  opt->help = "SELECT from a random materialized view";
  opt->setInt(5);
  opt->setSQL();

  opt = newOption(Option::INT, Option::DROP_MATVIEW, "drop-matview");
  opt->help = "DROP MATERIALIZED VIEW IF EXISTS on a random materialized view";
  opt->setInt(2);
  opt->setSQL();
  opt->setDDL();

  opt = newOption(Option::INT, Option::PREPARED_TRANSACTION_STRESS,
                  "prepared-tx-stress");
  opt->help = "run PREPARE TRANSACTION and random COMMIT/ROLLBACK PREPARED";
  opt->setInt(5);
  opt->setSQL();
  opt->setDDL();

  opt = newOption(Option::INT, Option::PG18_FUNCTIONS, "pg18-functions");
  opt->help = "run PostgreSQL 18 built-in function queries";
  opt->setInt(5);
  opt->setSQL();

  /* Initial Records in table */
  opt = newOption(Option::INT, Option::INITIAL_RECORDS_IN_TABLE, "records");
  opt->help =
      "Number of initial records (N) in each table. The table will have random "
      "records in range of 0 to N. Also check --exact-initial-records ";
  opt->setInt(1000);

  /* Initial Records in table */
  opt = newOption(Option::BOOL, Option::EXACT_INITIAL_RECORDS,
                  "exact-initial-records");
  opt->help = " When passed with --records (N) option "
              "inserts exact number of  N records in tables.";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* Execute workload for number of seconds */
  opt = newOption(Option::INT, Option::NUMBER_OF_SECONDS_WORKLOAD, "seconds");
  opt->help = "Number of seconds to execute workload";
  opt->setInt(1000);

  /* primary key probability */
  opt = newOption(Option::INT, Option::PRIMARY_KEY, "pk-prob");
  opt->help = "Probability of adding primary key in a table";
  opt->setInt(50);

  /* modify column */
  opt = newOption(Option::INT, Option::ALTER_COLUMN_MODIFY, "modify-column");
  opt->help = "Alter table column modify";
  opt->setInt(10);
  opt->setSQL();
  opt->setDDL();

  /* SELECT */
  opt = newOption(Option::BOOL, Option::NO_SELECT, "no-select");
  opt->help = "do not execute any type select on tables";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* INSERT */
  opt = newOption(Option::BOOL, Option::NO_INSERT, "no-insert");
  opt->help = "do not execute insert into tables";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* UPDATE */
  opt = newOption(Option::BOOL, Option::NO_UPDATE, "no-update");
  opt->help = "do not execute any type of update on tables";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* DELETE */
  opt = newOption(Option::BOOL, Option::NO_DELETE, "no-delete");
  opt->help = "do not execute any type of delete on tables";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* PREPARE new metadata */
  opt = newOption(Option::BOOL, Option::PREPARE, "prepare");
  opt->help = "create new random tables and insert initial records";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* Select all row */
  opt = newOption(Option::INT, Option::SELECT_ALL_ROW, "select-all-rows");
  opt->help = "select all table data and in case of partition randomly pick "
              "some partition";
  opt->setInt(8);
  opt->setSQL();

  opt = newOption(Option::INT, Option::SELECT_ROW_USING_PKEY,
                  "select-single-row");
  opt->help = "Select table using single row";
  opt->setInt(800);
  opt->setSQL();

  opt = newOption(Option::INT, Option::SELECT_WITH_JOIN, "select-with-join");
  opt->help =
      "Select rows using metadata-driven INNER JOIN between compatible tables";
  opt->setInt(40);
  opt->setSQL();

  opt = newOption(Option::INT, Option::SELECT_WITH_CTE, "select-with-cte");
  opt->help =
      "Select rows using non-recursive read-only WITH queries";
  opt->setInt(30);
  opt->setSQL();

  /* Insert random row */
  opt = newOption(Option::INT, Option::INSERT_RANDOM_ROW, "insert-row");
  opt->help = "insert random row";
  opt->setInt(600);
  opt->setSQL();

  /* Update row using pkey */
  opt =
      newOption(Option::INT, Option::UPDATE_ROW_USING_PKEY, "update-with-cond");
  opt->help = "Update row using where clause";
  opt->setInt(200);
  opt->setSQL();

  /* Delete all rows */
  opt = newOption(Option::INT, Option::DELETE_ALL_ROW, "delete-all-rows");
  opt->help = "delete all rows of a table";
  opt->setInt(1);
  opt->setSQL();

  /* Delete row using pkey */
  opt =
      newOption(Option::INT, Option::DELETE_ROW_USING_PKEY, "delete-with-cond");
  opt->help = "delete row with where condition";
  opt->setInt(200);
  opt->setSQL();

  /* Drop column */
  opt = newOption(Option::INT, Option::DROP_COLUMN, "drop-column");
  opt->help = "alter table drop some random column";
  opt->setInt(1);
  opt->setSQL();
  opt->setDDL();

  /* Add column */
  opt = newOption(Option::INT, Option::ADD_COLUMN, "add-column");
  opt->help = "alter table add some random column";
  opt->setInt(1);
  opt->setSQL();
  opt->setDDL();

  /* Drop index */
  opt = newOption(Option::INT, Option::DROP_INDEX, "drop-index");
  opt->help = "alter table drop random index";
  opt->setInt(1);
  opt->setSQL();
  opt->setDDL();

  /* Add column */
  opt = newOption(Option::INT, Option::ADD_INDEX, "add-index");
  opt->help = "alter table add random index";
  opt->setInt(1);
  opt->setSQL();
  opt->setDDL();

  /* Rename Index */
  opt = newOption(Option::INT, Option::RENAME_INDEX, "rename-index");
  opt->help = "alter table rename index";
  opt->setInt(1);
  opt->setSQL();
  opt->setDDL();

  /* Rename Column */
  opt = newOption(Option::INT, Option::RENAME_COLUMN, "rename-column");
  opt->help = "alter table rename column";
  opt->setInt(1);
  opt->setSQL();
  opt->setDDL();

  /* Analyze Table */
  opt = newOption(Option::INT, Option::ANALYZE, "analyze");
  opt->help = "analyze table, for partition table randomly analyze either "
              "partition or full table";
  opt->setInt(1);
  opt->setSQL();
  opt->setDDL();

  /* Check Table */
  opt = newOption(Option::INT, Option::CHECK_TABLE, "check");
  opt->help = "check table health. PostgreSQL currently uses a lightweight synthetic check";
  opt->setInt(5);
  opt->setSQL();

  /* Check Table Pre-load */
  opt = newOption(Option::BOOL, Option::CHECK_TABLE_PRELOAD, "check-preload");
  opt->help = "run preload table checks before starting workload; PostgreSQL uses a synthetic check";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* Add drop Partition */
  opt =
      newOption(Option::INT, Option::ADD_DROP_PARTITION, "add-drop-partition");
  opt->help = "randomly add drop new partitions";
  opt->setInt(3);
  opt->setSQL();
  opt->setDDL();

  /* maximum number Partition */
  opt = newOption(Option::INT, Option::MAX_PARTITIONS, "max-partitions");
  opt->help =
      "maximum number of partitions in table. choose between 1 and 8192";
  opt->setInt(25);

  /* Total number of partition supported */
  opt =
      newOption(Option::STRING, Option::PARTITION_SUPPORTED, "partition-types");
  opt->help =
      "total partition supported, all for LIST, HASH, KEY, RANGE or to provide "
      "or pass for example --partition-types LIST,HASH,RANGE without space";
  opt->setString("all");

  /* Optimize Table */
  opt = newOption(Option::INT, Option::OPTIMIZE, "optimize");
  opt->help = "optimize table. PostgreSQL maps this to VACUUM ANALYZE";
  opt->setInt(3);
  opt->setSQL();
  opt->setDDL();

  /* Truncate table */
  opt = newOption(Option::INT, Option::TRUNCATE, "truncate");
  opt->help = "truncate table or in case of partition truncate partition";
  opt->setInt(1);
  opt->setSQL();
  opt->setDDL();

  /* Drop and recreate table */
  opt = newOption(Option::INT, Option::DROP_CREATE, "recreate-table");
  opt->help = "drop and recreate table";
  opt->setInt(1);
  opt->setSQL();
  opt->setDDL();

  /* DATABASE */
  opt = newOption(Option::STRING, Option::DATABASE, "database");
  opt->help = "The database or schema target to use";
  opt->setString("pstress");

  /* Address */
  opt = newOption(Option::STRING, Option::ADDRESS, "address");
  opt->help = "IP address to connect to";

  /* Infile */
  opt = newOption(Option::STRING, Option::INFILE, "infile");
  opt->help = "The SQL input file";
  opt->setString("pquery.sql");

  /* Logdir */
  opt = newOption(Option::STRING, Option::LOGDIR, "logdir");
  opt->help = "Log directory";
  opt->setString("/tmp");

  /* Socket */
  opt = newOption(Option::STRING, Option::SOCKET, "socket");
  opt->help = "Unix domain socket directory to use";
  opt->setString("");

  /*config file */
  opt = newOption(Option::STRING, Option::CONFIGFILE, "config-file");
  opt->help = "Config file to use for test";

  /*Port */
  opt = newOption(Option::INT, Option::PORT, "port");
  opt->help = "Port to use";
  opt->setInt(5432);

  /* Password*/
  opt = newOption(Option::STRING, Option::PASSWORD, "password");
  opt->help = "The PostgreSQL user's password";
  opt->setString("");

  /* HELP */
  opt = newOption(Option::BOOL, Option::HELP, "help");
  opt->help = "user asked for help";
  opt->setArgs(optional_argument);

  opt = newOption(Option::BOOL, Option::VERBOSE, "verbose");
  opt->help = "verbose";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* Threads */
  opt = newOption(Option::INT, Option::THREADS, "threads");
  opt->help = "The number of threads to use";
  opt->setInt(1);

  /* User*/
  opt = newOption(Option::STRING, Option::USER, "user");
  opt->help = "The PostgreSQL user to be used";
  opt->setString("postgres");

  /* log all queries */
  opt = newOption(Option::BOOL, Option::LOG_ALL_QUERIES, "log-all-queries");
  opt->help = "Log all queries (succeeded and failed)";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* execute sql sequentially */
  opt = newOption(Option::BOOL, Option::NO_SHUFFLE, "no-shuffle");
  opt->help = "execute SQL sequentially | randomly\n";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* log query statistics */
  opt = newOption(Option::BOOL, Option::LOG_QUERY_STATISTICS,
                  "log-query-statistics");
  opt->help = "extended output of query result";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* log client output*/
  opt = newOption(Option::BOOL, Option::LOG_CLIENT_OUTPUT, "log-client-output");
  opt->help = "Log query output to separate file";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* log query number*/
  opt = newOption(Option::BOOL, Option::LOG_QUERY_NUMBERS, "log-query-numbers");
  opt->help = "write query # to logs";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* log query duration */
  opt =
      newOption(Option::BOOL, Option::LOG_QUERY_DURATION, "log-query-duration");
  opt->help = "Log query duration in milliseconds";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* log failed queries */
  opt =
      newOption(Option::BOOL, Option::LOG_FAILED_QUERIES, "log-failed-queries");
  opt->help = "Log all failed queries";
  opt->setBool(true);
  opt->setArgs(no_argument);

  /* log success queries */
  opt = newOption(Option::BOOL, Option::LOG_SUCCEDED_QUERIES,
                  "log-succeeded-queries");
  opt->help = "Log succeeded queries";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* queries per thread */
  opt =
      newOption(Option::INT, Option::QUERIES_PER_THREAD, "queries-per-thread");
  opt->help = "The number of queries per thread";
  opt->setInt(1);

  /* test connection */
  opt = newOption(Option::BOOL, Option::TEST_CONNECTION, "test-connection");
  opt->help = "Test connection to server and exit";
  opt->setBool(false);
  opt->setArgs(no_argument);

  /* transaction probability */
  opt = newOption(Option::INT, Option::TRANSATION_PRB_K, "trx-prob-k");
  opt->help = "probability(out of 1000) of combining sql as single trx";
  opt->setInt(1);

  /* tranasaction size */
  opt = newOption(Option::INT, Option::TRANSACTIONS_SIZE, "trx-size");
  opt->help = "average size of each trx";
  opt->setInt(10);

  /* probability of executing commit */
  opt = newOption(Option::INT, Option::COMMIT_PROB, "commit-prob");
  opt->help = "probability of executing commit after a transaction. Else it "
              "would be rollback ";
  opt->setInt(95);

  /* number of savepoints in trxs */
  opt = newOption(Option::INT, Option::SAVEPOINT_PRB_K, "savepoint-prob-k");
  opt->help = "probability of using savepoint in a transaction.\n Also 10% "
              "such transaction will be rollback to some savepoint";
  opt->setInt(10);

  /* transactional ddl probability */
  opt = newOption(Option::INT, Option::TRX_DDL_PROB_K, "trx-ddl-prob-k");
  opt->help =
      "probability(out of 1000) of running a dedicated DDL-only transaction";
  opt->setInt(0);

  /* transactional ddl size */
  opt = newOption(Option::INT, Option::TRX_DDL_SIZE, "trx-ddl-size");
  opt->help = "maximum number of DDL statements in one dedicated DDL transaction";
  opt->setInt(3);

  /* steps */
  opt = newOption(Option::INT, Option::STEP, "step");
  opt->help = "current step in pstress script";
  opt->setInt(1);

  /* metadata file path */
  opt = newOption(Option::STRING, Option::METADATA_PATH, "metadata-path");
  opt->help = "path of metadata file";

  /* sql format for */
  opt = newOption(Option::INT, Option::GRAMMAR_SQL, "grammar-sql");
  opt->help = "grammar sql";
  opt->setInt(10);
  opt->setSQL();

  /* file name of special sql */
  opt = newOption(Option::STRING, Option::GRAMMAR_FILE, "grammar-file");
  opt->help =
      "file to be used  for grammar sql\nT1_INT_1, T1_INT_2 will be replaced "
      "with int columns of some table\n in database T1_VARCHAR_1, T1_VARCHAR_2 "
      "will be replaced with varchar columns of some table in database";
  opt->setString("grammar.sql");
}

Option::~Option() {}

void Option::print_pretty() {
  std::string s;
  s = "--" + name + "      ";
  std::cout << "--" << name << ": " << help << std::endl;
  std::cout << " default";
  switch (type) {
  case STRING:
    std::cout << ": " << getString() << std::endl;
    break;
  case INT:
    std::cout << "#: " << getInt() << std::endl;
    break;
  case BOOL:
    std::cout << ": " << getBool() << std::endl;
  }
  std::cout << std::endl;
}

/* delete options */
void delete_options() {
  for (auto &i : *options)
    delete i;
  delete options;
}

void show_help(Option::Opt option) {
  /* Print all avilable options */

  if (option == Option::MAX) {
    print_version();
    for (auto &it : *options) {
      it->print_pretty();
    };
  } else {
    std::cout << " Invalid options " << std::endl;
    std::cout << " use --help --verbose to see all supported options "
              << std::endl;
  }
}

void print_version(void) {
  std::cout << " - PStress v" << PQVERSION << "-" << PQREVISION
            << " compiled with " << FORK << "-" << pq_client_version_string()
            << std::endl;
}

void show_help(std::string help) {
  if (help.compare("verbose") == 0) {

    for (auto &op : *options) {
      if (op != nullptr)
        op->print_pretty();
    }
  }
  bool help_found = false;
  for (auto &op : *options) {
    if (op != nullptr && help.size() > 0 && help.compare(op->getName()) == 0) {
      help_found = true;
      op->print_pretty();
    }
  }
  if (!help_found)
    std::cout << "Not a valid option! " << help << std::endl;
}

  void show_help() {
    print_version();
    std::cout << " - For complete help use => pstress  --help --verbose"
              << std::endl;
    std::cout << " - For help on any option => pstress --help=OPTION e.g. \n "
                 "            pstress --help=ddl"
              << std::endl;
  }

  void show_cli_help(void) {
    print_version();
    std::cout << " - General usage: pstress --user=USER --password=PASSWORD "
                 "--database=DATABASE"
              << std::endl;
    std::cout << "=> pstress doesn't support multiple nodes when using "
                 "commandline options mode!"
              << std::endl;
    std::cout
        << "---------------------------------------------------------------"
           "--------------------------\n"
        << "| OPTION               | EXPLANATION                           "
           "       | DEFAULT         |\n"
        << "---------------------------------------------------------------"
           "--------------------------\n"
        << "--database             | The database to connect to            "
           "       | \n"
        << "--address              | IP address to connect to              "
           "       | \n"
        << "--port                 | The port to connect to                "
           "       | 5432\n"
        << "--infile               | The SQL input file                    "
           "       | pquery.sql\n"
        << "--logdir               | Log directory                         "
           "       | /tmp\n"
        << "--socket               | Unix socket directory to use          "
           "       | <empty>\n"
        << "--user                 | The PostgreSQL user to be used       "
           "       | postgres\n"
        << "--password             | The PostgreSQL user's password       "
           "       | <empty>\n"
        << "--threads              | The number of threads to use          "
           "       | 1\n"
        << "--queries-per-thread   | The number of queries per thread      "
           "       | 10000\n"
        << "--verbose              | Duplicates the log to console when "
           "threads=1 | no\n"
        << "--log-all-queries      | Log all queries (succeeded and "
           "failed)       | no\n"
        << "--log-succeeded-queries| Log succeeded queries                 "
           "       | no\n"
        << "--log-failed-queries   | Log failed queries                    "
           "       | no\n"
        << "--no-shuffle           | Execute SQL sequentially              "
           "       | randomly\n"
        << "--test-connection      | Test connection to server and exit    "
           "       | no\n"
        << "--log-query-number     | Write query # to logs                 "
           "       | no\n"
        << "--log-client-output    | Log query output to separate file     "
           "       | no\n"
        << "--ddl		    | USE DDL in command line option           "
           "    | true\n"
        << "---------------------------------------------------------------"
           "--------------------------"
        << std::endl;
  }

  void show_config_help(void) {

    print_version();

    std::cout << " - Usage: pstress --config-file=pstress.cfg" << std::endl;
    std::cout << " - CLI params has been replaced by config file (INI format)"
              << std::endl;
    std::cout << " - You can redefine any global param=value pair in "
                 "host-specific section"
              << std::endl;
    std::cout << "\nConfig example:\n" << std::endl;
    std::cout
        <<

        "[node0.domain.tld]\n"
        << "# The database to connect to\n"
        << "database = \n"
        << "# IP address to connect to, default is AF_UNIX\n"
        << "address = <empty>\n"
        << "# The port to connect to\n"
        << "port = 5432\n"
        << "# The SQL input file\n"
        << "infile = pquery.sql\n"
        << "# Directory to store logs\n"
        << "logdir = /tmp\n"
        << "# Unix socket directory to use\n"
        << "socket = \n"
        << "# The PostgreSQL user to be used\n"
        << "user = postgres\n"
        << "# The PostgreSQL user's password\n"
        << "password = \n"
        << "# The number of threads to use by worker\n"
        << "threads = 1\n"
        << "# The number of queries per thread\n"
           "queries-per-thread = 10000\n"
        << "# Duplicates the log to console when threads=1 and workers=1\n"
           "verbose = No\n"
        << "# Log all queries\n"
        << "log-all-queries = No\n"
        << "# Log succeeded queries\n"
        << "log-succeeded-queries = No\n"
        << "# Log failed queries\n"
        << "log-failed-queries = No\n"
        << "# Log output from executed query (separate log)\n"
        << "log-client-output = No\n"
        << "# Log query numbers along the query results and statistics\n"
        << "log-query-number = No\n\n"
        << "[node1.domain.tld]\n"
        << "address = 10.10.6.10\n"
        << "# default for \"run\" is No, need to set it explicitly\n"
        << "run = Yes\n\n"
        << "[node2.domain.tld]\n"
        << "address = 10.10.6.11\n"
        << std::endl;
  }
