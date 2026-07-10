#ifndef __COMMON_HPP__
#define __COMMON_HPP__

#ifndef PQVERSION
#define PQVERSION "1"
#endif

#ifdef MAXPACKET
  #ifndef MAX_PACKET_DEFAULT
  #define MAX_PACKET_DEFAULT 4194304
  #endif
#endif

#ifndef FORK
#define FORK "PostgreSQL"
#endif

#ifndef PQREVISION
#define PQREVISION "unknown"
#endif

#ifdef __APPLE__
#define PLATFORM_ID "Darwin"
#else
#define PLATFORM_ID "Linux"
#endif

#include <getopt.h>
#include <atomic>
#include <map>
#include <string>
#include <algorithm>
#include <vector>

struct Option {
  enum Type { BOOL, INT, STRING } type;
  enum Opt {
    INITIAL_SEED,
    JUST_LOAD_DDL,
    NO_DDL,
    ONLY_CL_DDL,
    ONLY_CL_SQL,
    NO_BLOB,
    NO_VIRTUAL_COLUMNS,
    GENERATED_COLUMN_KIND,
    TABLES,
    INDEXES,
    ALGORITHM,
    LOCK,
    COLUMNS,
    INDEX_COLUMNS,
    NO_AUTO_INC,
    NO_DESC_INDEX,
    ONLY_TEMPORARY,
    ONLY_UNLOGGED,
    ONLY_PARTITION,
    INITIAL_RECORDS_IN_TABLE,
    NUMBER_OF_SECONDS_WORKLOAD,
    ALTER_COLUMN_MODIFY,
    PRIMARY_KEY,
    NO_SELECT,
    NO_INSERT,
    NO_UPDATE,
    NO_DELETE,
    SELECT_ALL_ROW,
    SELECT_ROW_USING_PKEY,
    SELECT_WITH_JOIN,
    SELECT_WITH_CTE,
    INSERT_RANDOM_ROW,
    UPDATE_ROW_USING_PKEY,
    DELETE_ALL_ROW,
    DELETE_ROW_USING_PKEY,
    INVALID_OPTION = 256,
    LOG_ALL_QUERIES = 'A',
    PQUERY = 'k',
    DATABASE = 'd',
    ADDRESS = 'a',
    INFILE = 'i',
    LOGDIR = 'l',
    SOCKET = 's',
    CONFIGFILE = 'c',
    PORT = 'p',
    PASSWORD = 'P',
    NO_SHUFFLE = 'n',
    THREADS = 't',
    LOG_FAILED_QUERIES = 'F',
    LOG_SUCCEDED_QUERIES = 'S',
    LOG_QUERY_STATISTICS = 'L',
    LOG_QUERY_DURATION = 'D',
    LOG_QUERY_NUMBERS = 'N',
    LOG_CLIENT_OUTPUT = 'O',
    TEST_CONNECTION = 'T',
    QUERIES_PER_THREAD = 'q',
    USER = 'u',
    HELP = 'h',
    VERBOSE = 'v',
    TRANSATION_PRB_K,
    TRANSACTIONS_SIZE,
    COMMIT_PROB,
    SAVEPOINT_PRB_K,
    TRX_DDL_PROB_K,
    TRX_DDL_SIZE,
    CHECK_TABLE,
    CHECK_TABLE_PRELOAD,
    PARTITION_SUPPORTED,
    ADD_DROP_PARTITION,
    MAX_PARTITIONS,
    STEP,
    METADATA_PATH,
    GRAMMAR_SQL,
    GRAMMAR_FILE,
    DROP_COLUMN,
    ADD_COLUMN,
    DROP_INDEX,
    ADD_INDEX,
    RENAME_COLUMN,
    RENAME_INDEX,
    OPTIMIZE,
    ANALYZE,
    TRUNCATE,
    DROP_CREATE,
    EXACT_INITIAL_RECORDS,
    PREPARE,
    NO_TEMPORARY,
    NO_UNLOGGED,
    NO_PARTITION,
    NO_FK,
    FK_PROB,
    PARTITION_PROB,
    TEMPORARY_PROB,
    UNLOGGED_PROB,
    PG18_VACUUM_ANALYZE_ONLY,
    VACUUM_TABLE,
    VACUUM_FULL,
    CHECKPOINT,
    CREATE_INDEX_CONCURRENTLY,
    REINDEX,
    CLUSTER_TABLE,
    BRIN_EXPRESSION_INDEX,
    CREATE_MATVIEW,
    REFRESH_MATVIEW_CONCURRENTLY,
    SELECT_MATVIEW,
    DROP_MATVIEW,
    PREPARED_TRANSACTION_STRESS,
    RETURNING_OLD_NEW,
    PG18_COPY,
    PG18_COPY_MODE,
    PG18_COPY_REJECT_LIMIT,
    PG18_COPY_LOG_VERBOSITY,
    PG18_EXPLAIN,
    PG18_FUNCTIONS,
    MAX
  } option;
  Option(Type t, Opt o, std::string n)
      : type(t), option(o), name(n), sql(false), ddl(false), total_queries(0),
        success_queries(0){};
  ~Option();

  void print_pretty();
  Type getType() { return type; };
  Opt getOption() { return option; };
  const char *getName() { return name.c_str(); };
  bool getBool() { return default_bool; }
  int getInt() { return default_int; }
  std::string getString() { return default_value; }
  short getArgs() { return args; }
  void setArgs(short s) { args = s; };
  void setBool(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    if (s.compare("ON") == 0 || s.compare("TRUE") == 0 || s.compare("1") == 0)
      default_bool = true;
    else if (s.compare("OFF") == 0 || s.compare("FALSE") == 0 ||
             s.compare("0") == 0)
      default_bool = false;
    else {
      // todo throw some execption
    }
  }

  void setBool(bool in) {
    default_bool = in;
  }
  void setInt(std::string n) {
    default_int = stoi(n);
  }
  void setInt(int n) {
    default_int = n;
  }
  void setString(std::string n) {
    default_value = n;
  };
  void setSQL() { sql = true; };
  void setDDL() { ddl = true; };
  void set_cl() { cl = true; }

  std::string name;
  std::string help;
  std::string default_value;
  int default_int; // if default value is integer
  bool default_bool; // if default value is bool
  bool sql; // true if option is SQL, False if others
  bool ddl; // If SQL is DDL, or false if it is not
  bool cl = false;                // set if it was pass trough command line
  short args = required_argument; // default is required argument
  std::atomic<unsigned long int> total_queries;   // totatl times executed
  std::atomic<unsigned long int> success_queries; // successful count
};

/* delete options */
void delete_options();
typedef std::vector<Option *> Opx;
extern Opx *options;
extern const char *binary_fullpath;
void add_options();
Option *newOption(Option::Type t, Option::Opt o, std::string s);

#endif
