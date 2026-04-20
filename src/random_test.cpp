/*
 =========================================================
 #       Created by Rahul Malik, Percona LLC             #
 =========================================================
*/
#include "random_test.hpp"
#include "common.hpp"
#include "node.hpp"
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>
#include <libgen.h>
using namespace rapidjson;
std::mt19937 rng;

const std::string TABLE_PREFIX = "tt_";
const std::string PARTITION_SUFFIX = "_p";
const std::string FK_SUFFIX = "_fk";
const std::string TEMP_SUFFIX = "_t";
const int version = 2;
/* range for random number int, integers, floats and double.
 more the value, less randomness.
 for example if it is 1. then there is very high chance of executing
 successful DML.
todo allow this option to be configured by user */
const int g_integer_range = 100;

static std::vector<Table *> *all_tables = new std::vector<Table *>;
static std::vector<std::string> locks;
static std::vector<std::string> algorithms;
static int g_max_columns_length = 30;
static int sum_of_all_opts = 0; // sum of all probablility
std::mutex ddl_logs_write;
static std::chrono::system_clock::time_point start_time =
    std::chrono::system_clock::now();

std::atomic<int> table_started(0);
std::atomic<size_t> check_failures(0);
std::atomic<size_t> table_completed(0);
std::atomic_flag lock_stream = ATOMIC_FLAG_INIT;
std::atomic<bool> run_query_failed(false);
/* partition type supported by system */
std::vector<Partition::PART_TYPE> Partition::supported;
const int maximum_records_in_each_parititon_list = 100;

static bool is_connection_lost(const PGconn *conn, const PGresult *result) {
  if (conn != nullptr && PQstatus(conn) == CONNECTION_BAD) {
    return true;
  }
  if (result == nullptr) {
    return false;
  }
  const char *sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
  if (sqlstate == nullptr) {
    return false;
  }
  return strncmp(sqlstate, "08", 2) == 0 || strcmp(sqlstate, "57P01") == 0 ||
         strcmp(sqlstate, "57P02") == 0 || strcmp(sqlstate, "57P03") == 0;
}

static std::string partition_child_name(const Table *table,
                                        const std::string &partition_name) {
  return table->name_ + "_" + partition_name;
}

static std::string pg_partition_target(Table *table) {
  auto *part = static_cast<Partition *>(table);
  if (part->part_type == Partition::RANGE && !part->positions.empty()) {
    return partition_child_name(table,
                                part->positions.at(rand_int(part->positions.size() - 1)).name);
  }
  if (part->part_type == Partition::LIST && !part->lists.empty()) {
    return partition_child_name(table,
                                part->lists.at(rand_int(part->lists.size() - 1)).name);
  }
  return partition_child_name(
      table, "p" + std::to_string(rand_int(part->number_of_part - 1)));
}

static bool supports_like_predicate(const Column *column) {
  if (column->type_ == Column::GENERATED) {
    auto generated =
        static_cast<const Generated_Column *>(column)->generate_type();
    return generated == Column::CHAR || generated == Column::VARCHAR ||
           generated == Column::BLOB;
  }
  switch (column->type_) {
  case Column::CHAR:
  case Column::VARCHAR:
  case Column::BLOB:
    return true;
  case Column::INTEGER:
  case Column::INT:
  case Column::FLOAT:
  case Column::DOUBLE:
  case Column::TIMESTAMP:
  case Column::BOOL:
  case Column::JSON:
  case Column::GENERATED:
  case Column::COLUMN_MAX:
    return false;
  }
  return false;
}

static bool column_name_exists(const Table *table, const std::string &name) {
  for (auto *column : *table->columns_) {
    if (column->name_ == name) {
      return true;
    }
  }
  return false;
}

static bool index_name_exists(const Table *table, const std::string &name) {
  for (auto *index : *table->indexes_) {
    if (index->name_ == name) {
      return true;
    }
  }
  return false;
}

static int pg_index_width_estimate(const Column *column) {
  switch (column->type_) {
  case Column::BOOL:
    return 1;
  case Column::INT:
  case Column::INTEGER:
  case Column::FLOAT:
  case Column::DOUBLE:
  case Column::TIMESTAMP:
    return 8;
  case Column::CHAR:
  case Column::VARCHAR:
    return std::max(1, std::min(column->length, 64));
  case Column::BLOB:
    return 128;
  case Column::JSON:
    return 128;
  case Column::GENERATED: {
    auto generated =
        static_cast<const Generated_Column *>(column)->generate_type();
    switch (generated) {
    case Column::BOOL:
      return 1;
    case Column::INT:
    case Column::INTEGER:
    case Column::FLOAT:
    case Column::DOUBLE:
    case Column::TIMESTAMP:
      return 8;
    case Column::CHAR:
    case Column::VARCHAR:
      return std::max(1, std::min(column->length, 64));
    case Column::BLOB:
    case Column::JSON:
    case Column::GENERATED:
    case Column::COLUMN_MAX:
      return 128;
    }
  }
  case Column::COLUMN_MAX:
    break;
  }
  return 128;
}

static bool pg_indexable_column(const Column *column) {
  return pg_index_width_estimate(column) <= 64;
}

static int pg_index_total_width(const Index *index) {
  int width = 0;
  for (auto *ind_col : *index->columns_) {
    width += pg_index_width_estimate(ind_col->column);
  }
  return width;
}

/* return table pointer of matching table. This is only done during the
 * first step or during the prepare, so you would have only tables that are not
 * renamed  */
static Table *pick_table(Table::TABLE_TYPES type, int id) {
  std::string name = TABLE_PREFIX + std::to_string(id);
  if (type == Table::FK) {
    name += FK_SUFFIX;
  } else if (type == Table::PARTITION) {
    name += PARTITION_SUFFIX;
  }
  for (auto const &table : *all_tables) {
    if (table->name_ == name)
      return table;
  }
  return nullptr;
}

static bool referenced_by_fk(const Table *table) {
  for (auto *candidate : *all_tables) {
    if (candidate->type == Table::FK &&
        static_cast<FK_table *>(candidate)->parent == table) {
      return true;
    }
  }
  return false;
}

static bool result_num_fields_safe(Thd1 *thd, int req) {
  if (!thd->result) {
    thd->thread_log << "PQnfields called with nullptr arg!";
    return 0;
  }
  auto num_fields = PQnfields(thd->result.get());
  auto ret = req <= num_fields;
  if (!ret) {
    thd->thread_log << "Expected at least " << req << " fields but only "
                    << num_fields << " exist";
  }
  return ret;
}

/* generate random numbers to populate in primary and fk
@param[in] number_of_records
@param[out] vector containing unique elements */
static std::vector<int> generateUniqueRandomNumbers(int number_of_records) {

  std::unordered_set<int> unique_keys_set(number_of_records);

  int max_size =
      g_integer_range * options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt();

  while (unique_keys_set.size() < static_cast<size_t>(number_of_records)) {
    unique_keys_set.insert(rand_int(max_size, 1));
  }

  std::vector<int> unique_keys(unique_keys_set.begin(), unique_keys_set.end());
  return unique_keys;
}

/* run check table */
static bool get_check_result(const std::string &sql, Thd1 *thd) {

  execute_sql(sql, thd);
  if (thd->result && PQntuples(thd->result.get()) > 0 &&
      result_num_fields_safe(thd, 4) &&
      !PQgetisnull(thd->result.get(), 0, 3) &&
      strcmp(PQgetvalue(thd->result.get(), 0, 3), "OK") != 0) {
    thd->thread_log << "Error: "
                    << (PQgetisnull(thd->result.get(), 0, 0)
                            ? ""
                            : PQgetvalue(thd->result.get(), 0, 0))
                    << " "
                    << (PQgetisnull(thd->result.get(), 0, 1)
                            ? ""
                            : PQgetvalue(thd->result.get(), 0, 1))
                    << " "
                    << (PQgetisnull(thd->result.get(), 0, 2)
                            ? ""
                            : PQgetvalue(thd->result.get(), 0, 2))
                    << " " << PQgetvalue(thd->result.get(), 0, 3)
                    << std::endl;
    return false;
  }

  return true;
}

static std::string read_single_value(const std::string &sql, Thd1 *thd) {
  std::string query_result = "";

  execute_sql(sql, thd);
  if (thd->result && PQntuples(thd->result.get()) > 0 &&
      result_num_fields_safe(thd, 1) &&
      !PQgetisnull(thd->result.get(), 0, 0))
    query_result = PQgetvalue(thd->result.get(), 0, 0);

  return query_result;
}

/* return probabality of all options and disable some feature based on user
 * request/ branch/ fork */
int sum_of_all_options(Thd1 *thd) {
  (void)thd;
  options->at(Option::ADD_DROP_PARTITION)->setInt(0);
  options->at(Option::DROP_COLUMN)->setInt(0);
  options->at(Option::ALTER_COLUMN_MODIFY)->setInt(0);
  options->at(Option::RENAME_COLUMN)->setInt(0);
  options->at(Option::DROP_CREATE)->setInt(0);
  options->at(Option::PARTITION_PROB)->setInt(25);
  options->at(Option::PARTITION_SUPPORTED)->setString("RANGE,LIST,HASH,KEY");
  locks = {"DEFAULT"};
  algorithms = {"DEFAULT"};

  /*check which all partition type supported */
  auto part_supp = opt_string(PARTITION_SUPPORTED);
  if (part_supp.compare("all") == 0) {
    Partition::supported.push_back(Partition::KEY);
    Partition::supported.push_back(Partition::LIST);
    Partition::supported.push_back(Partition::HASH);
    Partition::supported.push_back(Partition::RANGE);
  } else {
    std::transform(part_supp.begin(), part_supp.end(), part_supp.begin(),
                   ::toupper);
    if (part_supp.find("HASH") != std::string::npos)
      Partition::supported.push_back(Partition::HASH);
    if (part_supp.find("KEY") != std::string::npos)
      Partition::supported.push_back(Partition::KEY);
    if (part_supp.find("LIST") != std::string::npos)
      Partition::supported.push_back(Partition::LIST);
    if (part_supp.find("RANGE") != std::string::npos)
      Partition::supported.push_back(Partition::RANGE);
  }

  if (options->at(Option::MAX_PARTITIONS)->getInt() < 1 ||
      options->at(Option::MAX_PARTITIONS)->getInt() > 8192)
    throw std::runtime_error(
        "invalid range for --max-partition. Choose between 1 and 8192");
  ;

  auto lock = opt_string(LOCK);
  if (lock.compare("all") == 0) {
    locks.push_back("DEFAULT");
    locks.push_back("EXCLUSIVE");
    locks.push_back("SHARED");
    locks.push_back("NONE");
  } else {
    std::transform(lock.begin(), lock.end(), lock.begin(), ::toupper);
    if (lock.find("EXCLUSIVE") != std::string::npos)
      locks.push_back("EXCLUSIVE");
    if (lock.find("SHARED") != std::string::npos)
      locks.push_back("SHARED");
    if (lock.find("NONE") != std::string::npos)
      locks.push_back("NONE");
    if (lock.find("DEFAULT") != std::string::npos)
      locks.push_back("DEFAULT");
  }
  auto algorithm = opt_string(ALGORITHM);
  if (algorithm.compare("all") == 0) {
    algorithms.push_back("INPLACE");
    algorithms.push_back("COPY");
    algorithms.push_back("INSTANT");
    algorithms.push_back("DEFAULT");
  } else {
    std::transform(algorithm.begin(), algorithm.end(), algorithm.begin(),
                   ::toupper);
    if (algorithm.find("INPLACE") != std::string::npos)
      algorithms.push_back("INPLACE");
    if (algorithm.find("COPY") != std::string::npos)
      algorithms.push_back("COPY");
    if (algorithm.find("INSTANT") != std::string::npos)
      algorithms.push_back("INSTANT");
    if (algorithm.find("DEFAULT") != std::string::npos)
      algorithms.push_back("DEFAULT");
  }

  if (!options->at(Option::COLUMNS)->cl)
    options->at(Option::COLUMNS)->setInt(7);

  if (options->at(Option::ONLY_PARTITION)->getBool() &&
      options->at(Option::ONLY_TEMPORARY)->getBool())
    throw std::runtime_error("choose either only partition or only temporary ");

  if (options->at(Option::ONLY_PARTITION)->getBool() &&
      options->at(Option::NO_PARTITION)->getBool())
    throw std::runtime_error("choose either only partition or no partition");

  if (options->at(Option::ONLY_PARTITION)->getBool()) {
    options->at(Option::NO_TEMPORARY)->setBool("true");
    options->at(Option::PARTITION_PROB)->setInt(100);
  }

  if (options->at(Option::ONLY_TEMPORARY)->getBool()) {
    options->at(Option::NO_PARTITION)->setBool("true");
    options->at(Option::TEMPORARY_PROB)->setInt(100);
  }

  /* if select is set as zero, disable all type of selects */
  if (options->at(Option::NO_SELECT)->getBool()) {
    options->at(Option::SELECT_ALL_ROW)->setInt(0);
    options->at(Option::SELECT_ROW_USING_PKEY)->setInt(0);
  }
  /* if delete is set as zero, disable all type of deletes */
  if (options->at(Option::NO_DELETE)->getBool()) {
    options->at(Option::DELETE_ALL_ROW)->setInt(0);
    options->at(Option::DELETE_ROW_USING_PKEY)->setInt(0);
  }
  /* If update is disable, set all update probability to zero */
  if (options->at(Option::NO_UPDATE)->getBool()) {
    options->at(Option::UPDATE_ROW_USING_PKEY)->setInt(0);
  }
  /* if insert is disable, set all insert probability to zero */
  if (options->at(Option::NO_INSERT)->getBool()) {
    opt_int_set(INSERT_RANDOM_ROW, 0);
  }
  auto only_cl_ddl = opt_bool(ONLY_CL_DDL);
  auto only_cl_sql = opt_bool(ONLY_CL_SQL);
  auto no_ddl = opt_bool(NO_DDL);

  /* if set, then disable all other SQL*/
  if (only_cl_sql) {
    for (auto &opt : *options) {
      if (opt != nullptr && opt->sql && !opt->cl)
        opt->setInt(0);
    }
  }

  /* only-cl-ddl, if set then disable all other DDL */
  if (only_cl_ddl) {
    for (auto &opt : *options) {
      if (opt != nullptr && opt->ddl && !opt->cl)
        opt->setInt(0);
    }
  }

  if (only_cl_ddl && no_ddl)
    throw std::runtime_error("noddl && only-cl-ddl can't be passed together");

  /* if no ddl is set disable all ddl */
  if (no_ddl) {
    for (auto &opt : *options) {
      if (opt != nullptr && opt->sql && opt->ddl)
        opt->setInt(0);
    }
  }

  int total = 0;
  for (auto &opt : *options) {
    if (opt == nullptr)
      continue;
    if (opt->getType() == Option::INT)
      thd->thread_log << opt->getName() << "=>" << opt->getInt() << std::endl;
    else if (opt->getType() == Option::BOOL)
      thd->thread_log << opt->getName() << "=>" << opt->getBool() << std::endl;
    if (!opt->sql)
      continue;
    total += opt->getInt();
  }

  if (total == 0)
    throw std::runtime_error("no option selected");
  return total;
}

/* return some options */
Option::Opt pick_some_option() {
  int rd = rand_int(sum_of_all_opts, 1);
  for (auto &opt : *options) {
    if (opt == nullptr || !opt->sql)
      continue;
    if (rd <= opt->getInt())
      return opt->getOption();
    else
      rd -= opt->getInt();
  }
  return Option::MAX;
}

/* pick some algorithm. and if caller pass value of algo & lock set it */
inline static std::string
pick_algorithm_lock(std::string *const algo = nullptr,
                    std::string *const lock = nullptr) {
  if (algo != nullptr)
    *algo = "DEFAULT";
  if (lock != nullptr)
    *lock = "DEFAULT";
  return "";
}

/* set seed of current thread */
int set_seed(Thd1 *thd) {

  auto initial_seed = opt_int(INITIAL_SEED);
  initial_seed += options->at(Option::STEP)->getInt();

  rng = std::mt19937(initial_seed);
  thd->thread_log << "Initial seed " << initial_seed << std::endl;
  for (int i = 0; i < thd->thread_id; i++)
    rand_int(MAX_SEED_SIZE, MIN_SEED_SIZE);
  thd->seed = rand_int(MAX_SEED_SIZE, MIN_SEED_SIZE);
  thd->thread_log << "CURRENT SEED IS " << thd->seed << std::endl;
  return thd->seed;
}

/* generate random strings of size N_STR */
std::vector<std::string> *random_strs_generator(unsigned long int seed) {
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz"
                                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                 "0123456789";

  static const size_t N_STRS = 10000;

  std::default_random_engine rng(seed);
  std::uniform_int_distribution<> dist(0, sizeof(alphabet) / sizeof(*alphabet) -
                                              2);

  std::vector<std::string> *strs = new std::vector<std::string>;
  strs->reserve(N_STRS);
  std::generate_n(std::back_inserter(*strs), strs->capacity(), [&] {
    std::string str;
    str.reserve(MAX_RANDOM_STRING_SIZE);
    std::generate_n(std::back_inserter(str), MAX_RANDOM_STRING_SIZE,
                    [&]() { return alphabet[dist(rng)]; });

    return str;
  });
  return strs;
}

std::vector<std::string> *random_strs;

int rand_int(int upper, int lower) {
  assert(upper >= lower);
  std::uniform_int_distribution<std::mt19937::result_type> dist(
      lower, upper); // distribution in range [lower, upper]
  return dist(rng);
}

/* return random float number in the range of upper and lower */
std::string rand_float(float upper, float lower) {
  assert(upper >= lower);
  static std::uniform_real_distribution<> dis(lower, upper);
  std::ostringstream out;
  out << std::fixed;
  out << std::setprecision(2) << (float)(dis(rng));
  return out.str();
}

std::string rand_double(double upper, double lower) {
  assert(upper >= lower);
  static std::uniform_real_distribution<> dis(lower, upper);
  std::ostringstream out;
  out << std::fixed;
  out << std::setprecision(5) << (double)(dis(rng));
  return out.str();
}

/* return random string in range of upper and lower */
std::string rand_string(int upper, int lower) {
  std::string rs = ""; /*random_string*/
  auto size = rand_int(upper, lower);

  while (size > 0) {
    auto str = random_strs->at(rand_int(random_strs->size() - 1));
    if (size > MAX_RANDOM_STRING_SIZE)
      rs += str;
    else
      rs += str.substr(0, size);
    size -= MAX_RANDOM_STRING_SIZE;
  }
  return rs;
}

static std::string rand_timestamp_value() {
  int year = rand_int(2030, 2000);
  int month = rand_int(12, 1);
  int day = rand_int(28, 1);
  int hour = rand_int(23, 0);
  int minute = rand_int(59, 0);
  int second = rand_int(59, 0);
  std::ostringstream out;
  out << "'" << year << "-"
      << std::setw(2) << std::setfill('0') << month << "-"
      << std::setw(2) << std::setfill('0') << day << " "
      << std::setw(2) << std::setfill('0') << hour << ":"
      << std::setw(2) << std::setfill('0') << minute << ":"
      << std::setw(2) << std::setfill('0') << second << "'";
  return out.str();
}

static std::string rand_json_value() {
  return "'{\"k\":" + std::to_string(rand_int(100000)) + ",\"s\":\"" +
         rand_string(12, 3) + "\",\"b\":" + (rand_int(1) == 1 ? "true" : "false") +
         "}'::jsonb";
}

/* return column type from a string */
Column::COLUMN_TYPES Column::col_type(std::string type) {
  if (type.compare("INTEGER") == 0)
    return INTEGER;
  else if (type.compare("INT") == 0)
    return INT;
  else if (type.compare("CHAR") == 0)
    return CHAR;
  else if (type.compare("VARCHAR") == 0)
    return VARCHAR;
  else if (type.compare("BOOL") == 0)
    return BOOL;
  else if (type.compare("BOOLEAN") == 0)
    return BOOL;
  else if (type.compare("GENERATED") == 0)
    return GENERATED;
  else if (type.compare("BLOB") == 0)
    return BLOB;
  else if (type.compare("TEXT") == 0)
    return BLOB;
  else if (type.compare("FLOAT") == 0)
    return FLOAT;
  else if (type.compare("REAL") == 0)
    return FLOAT;
  else if (type.compare("DOUBLE") == 0)
    return DOUBLE;
  else if (type.compare("DOUBLE PRECISION") == 0)
    return DOUBLE;
  else if (type.compare("TIMESTAMP") == 0)
    return TIMESTAMP;
  else if (type.compare("JSON") == 0 || type.compare("JSONB") == 0)
    return JSON;
  else
    throw std::runtime_error("unhandled " + col_type_to_string(type_) +
                             " at line " + std::to_string(__LINE__));
}

/* return string from a column type */
const std::string Column::col_type_to_string(COLUMN_TYPES type) {
  switch (type) {
  case INTEGER:
    return "INTEGER";
  case INT:
    return "INT";
  case CHAR:
    return "CHAR";
  case DOUBLE:
    return "DOUBLE PRECISION";
  case FLOAT:
    return "REAL";
  case VARCHAR:
    return "VARCHAR";
  case TIMESTAMP:
    return "TIMESTAMP";
  case BOOL:
    return "BOOLEAN";
  case BLOB:
    return "TEXT";
  case JSON:
    return "JSONB";
  case GENERATED:
    return "GENERATED";
  case COLUMN_MAX:
    break;
  }
  return "FAIL";
}

/* integer range */

static std::string rand_value_universal(Column::COLUMN_TYPES type_,
                                        int length) {
  int rand_length;
  switch (type_) {
  case (Column::COLUMN_TYPES::INTEGER):
    return std::to_string(
        rand_int(options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt()));
    break;
  case (Column::COLUMN_TYPES::INT):
    return std::to_string(
        rand_int(g_integer_range *
                 options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt()));
    break;
  case (Column::COLUMN_TYPES::FLOAT): {
    return rand_float(options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt());
    break;
  }
  case (Column::COLUMN_TYPES::DOUBLE): {
    return rand_double(1.0 / g_integer_range *
                       options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt());
    break;
  }
  case Column::COLUMN_TYPES::TIMESTAMP:
    return rand_timestamp_value();
  case Column::COLUMN_TYPES::CHAR:
  case Column::COLUMN_TYPES::VARCHAR:
    return "\'" + rand_string(length) + "\'";
    break;
  case Column::COLUMN_TYPES::BOOL:
    return (rand_int(1) == 1 ? "true" : "false");
    break;
  case Column::COLUMN_TYPES::BLOB:
    rand_length = rand_int(length);
    if (rand_int(10) != 10)
      rand_length /= 10;
    return "\'" + rand_string(rand_length) + "\'";
  case Column::COLUMN_TYPES::JSON:
    return rand_json_value();
    break;
  case Column::COLUMN_TYPES::GENERATED:
  case Column::COLUMN_TYPES::COLUMN_MAX:
    throw std::runtime_error("unhandled " + Column::col_type_to_string(type_) +
                             " at line " + std::to_string(__LINE__));
  }
  return "";
}

/* return random value of  a column*/
std::string Column::rand_value() { return rand_value_universal(type_, length); }

/* return random value of sub string */
std::string Generated_Column::rand_value() {
  return rand_value_universal(g_type, length);
}

/* prepare single quoted string for LIKE clause */
std::string &Table::prepare_like_string(std::string &&str) {
  /* Check if the incoming string is empty */
  if (str.at(0) == '\'' && str.at(1) == '\'')
    str = str.insert(1, 1, '%');
  /* Processing the single quoted values that are returned by 'rand_string' */
  else if (str.at(0) == '\'') {
    str = str.substr(0, 2);
    str = str.insert(2, 1, '\'');
    str = str.insert(1, 1, '%');
    str = str.insert(3, 1, '%');
  } else /*Return non-string number with single quotes */ {
    str = "\'%" + str + "%\'";
  }
  return str;
}

/* return table definition */
std::string Column::definition() {
  std::string def = name_ + " " + clause();
  if (null)
    def += " NOT NULL";
  if (auto_increment)
    def += " GENERATED BY DEFAULT AS IDENTITY";
  return def;
}

/* add new column, part of create table or Alter table */
Column::Column(std::string name, Table *table, COLUMN_TYPES type)
    : table_(table) {
  type_ = type;
  switch (type) {
  case CHAR:
    name_ = "c" + name;
    length = rand_int(g_max_columns_length, 10);
    break;
  case VARCHAR:
    name_ = "v" + name;
    length = rand_int(g_max_columns_length, 10);
    break;
  case INT:
  case INTEGER:
    name_ = "i" + name;
    if (rand_int(10) == 1)
      length = rand_int(100, 20);
    break;
  case FLOAT:
    name_ = "f" + name;
    break;
  case DOUBLE:
    name_ = "d" + name;
    break;
  case TIMESTAMP:
    name_ = "ts" + name;
    break;
  case BOOL:
    name_ = "t" + name;
    break;
  case JSON:
    name_ = "j" + name;
    break;
  default:
    throw std::runtime_error("unhandled " + col_type_to_string(type_) +
                             " at line " + std::to_string(__LINE__));
  }
}

/* add new blob column, part of create table or Alter table */
Blob_Column::Blob_Column(std::string name, Table *table)
    : Column(table, Column::BLOB) {

  sub_type = "TEXT";
  name_ = "t" + name;
  compressed = false;
}

Blob_Column::Blob_Column(std::string name, Table *table, std::string sub_type_)
    : Column(table, Column::BLOB) {
  name_ = name;
  sub_type = sub_type_;
}

static std::string pg_generated_numeric_term(const Column *col) {
  switch (col->type_) {
  case Column::INT:
  case Column::INTEGER:
    return col->name_;
  case Column::FLOAT:
  case Column::DOUBLE:
    return "ROUND(" + col->name_ + ")::INTEGER";
  case Column::TIMESTAMP:
    return "(MOD(EXTRACT(EPOCH FROM " + col->name_ +
           ")::BIGINT, 1000000))::INTEGER";
  case Column::BOOL:
    return "(CASE WHEN " + col->name_ + " THEN 1 ELSE 0 END)";
  case Column::VARCHAR:
  case Column::CHAR:
  case Column::BLOB:
    return "LENGTH(COALESCE(" + col->name_ + "::TEXT, ''))";
  case Column::JSON:
    return "COALESCE((" + col->name_ + "->>'k')::INTEGER, 0)";
  case Column::GENERATED:
  case Column::COLUMN_MAX:
    break;
  }
  throw std::runtime_error("unhandled " + Column::col_type_to_string(col->type_) +
                           " at line " + std::to_string(__LINE__));
}

static std::string pg_generated_text_term(const Column *col, int limit,
                                          int &actual_size) {
  std::string expr;
  int column_size = 0;
  switch (col->type_) {
  case Column::INT:
  case Column::INTEGER:
    column_size = 10;
    expr = "COALESCE(" + col->name_ + "::TEXT, '')";
    break;
  case Column::FLOAT:
  case Column::DOUBLE:
    column_size = 10;
    expr = "COALESCE(" + col->name_ + "::TEXT, '')";
    break;
  case Column::TIMESTAMP:
    column_size = 19;
    expr = "COALESCE((MOD(EXTRACT(EPOCH FROM " + col->name_ +
           ")::BIGINT, 1000000))::TEXT, '')";
    break;
  case Column::BOOL:
    column_size = 5;
    expr = "(CASE WHEN " + col->name_ + " THEN 'true' ELSE 'false' END)";
    break;
  case Column::VARCHAR:
  case Column::CHAR:
    column_size = col->length;
    expr = "COALESCE(" + col->name_ + "::TEXT, '')";
    break;
  case Column::BLOB:
    column_size = 5000;
    expr = "COALESCE(" + col->name_ + "::TEXT, '')";
    break;
  case Column::JSON:
    column_size = 128;
    expr = "COALESCE(" + col->name_ + "->>'s', '')";
    break;
  case Column::GENERATED:
  case Column::COLUMN_MAX:
    break;
  }

  if (column_size > limit) {
    actual_size += limit;
    return "LEFT(" + expr + ", " + std::to_string(limit) + ")";
  }

  actual_size += column_size;
  return expr;
}

/* Constructor used for load metadata */
Generated_Column::Generated_Column(std::string name, Table *table,
                                   std::string clause, std::string sub_type)
    : Column(table, Column::GENERATED) {
  name_ = name;
  str = clause;
  g_type = Column::col_type(sub_type);
}

/* Generated column constructor. lock table before calling */
Generated_Column::Generated_Column(std::string name, Table *table)
    : Column(table, Column::GENERATED) {
  name_ = "g" + name;
  auto blob_supported = !options->at(Option::NO_BLOB)->getBool();
  g_type = COLUMN_MAX;
  /* Generated columns are 2:2:2:2 (INT:VARCHAR:CHAR:BLOB) */
  while (g_type == COLUMN_MAX) {
    auto x = rand_int(4, 1);
    if (x <= 1)
      g_type = INT;
    else if (x <= 2)
      g_type = VARCHAR;
    else if (x <= 3)
      g_type = CHAR;
    else if (blob_supported && x <= 4) {
      g_type = BLOB;
    }
  }

  /*number of columns in generated columns */
  size_t columns = rand_int(.6 * table->columns_->size()) + 1;

  std::vector<size_t> col_pos; // position of columns
  while (col_pos.size() < columns) {
    size_t col = rand_int(table->columns_->size() - 1);
    if (!table->columns_->at(col)->auto_increment &&
        table->columns_->at(col)->type_ != GENERATED)
      col_pos.push_back(col);
  }

  if (g_type == INT || g_type == INTEGER) {
    std::vector<std::string> terms;
    for (auto pos : col_pos) {
      terms.push_back(pg_generated_numeric_term(table->columns_->at(pos)));
    }
    str = " " + col_type_to_string(g_type) + " GENERATED ALWAYS AS ((";
    for (const auto &term : terms) {
      str += term + " + ";
    }
    str.erase(str.length() - 3);
    str += ")::INTEGER) STORED";
    return;
  } else if (g_type == VARCHAR || g_type == CHAR || g_type == BLOB) {
    int min_size = std::min(static_cast<int>(col_pos.size()), g_max_columns_length);
    int max_size = std::max(g_max_columns_length, min_size);
    auto size = rand_int(max_size, std::max(1, min_size));
    int actual_size = 0;
    std::vector<std::string> parts;
    for (auto pos : col_pos) {
      int current_upper =
          std::max(1, static_cast<int>(size) / static_cast<int>(col_pos.size()) * 2);
      auto current_size = rand_int(current_upper, 1);
      parts.push_back(pg_generated_text_term(table->columns_->at(pos),
                                             current_size, actual_size));
    }

    str = " " + col_type_to_string(g_type);
    if (g_type == VARCHAR || g_type == CHAR)
      str += "(" + std::to_string(actual_size) + ")";
    str += " GENERATED ALWAYS AS (";
    if (g_type == VARCHAR || g_type == CHAR)
      str += "(";
    for (size_t i = 0; i < parts.size(); ++i) {
      str += parts[i];
      if (i + 1 != parts.size())
        str += " || ";
    }
    if (g_type == VARCHAR || g_type == CHAR)
      str += ")::" + col_type_to_string(g_type) + "(" +
             std::to_string(actual_size) + ")";
    str += ") STORED";
    length = actual_size;
    return;
  } else {
    throw std::runtime_error("unhandled " + col_type_to_string(g_type) +
                             " at line " + std::to_string(__LINE__));
  }
}

template <typename Writer> void Column::Serialize(Writer &writer) const {
  writer.String("name");
  writer.String(name_.c_str(), static_cast<SizeType>(name_.length()));
  writer.String("type");
  std::string typ = col_type_to_string(type_);
  writer.String(typ.c_str(), static_cast<SizeType>(typ.length()));
  writer.String("null");
  writer.Bool(null);
  writer.String("primary_key");
  writer.Bool(primary_key);
  writer.String("compressed");
  writer.Bool(compressed);
  writer.String("auto_increment");
  writer.Bool(auto_increment);
  writer.String("length");
  writer.Int(length);
}

/* add sub_type metadata */
template <typename Writer> void Blob_Column::Serialize(Writer &writer) const {
  writer.String("sub_type");
  writer.String(sub_type.c_str(), static_cast<SizeType>(sub_type.length()));
}

/* add sub_type and clause in metadata */
template <typename Writer>
void Generated_Column::Serialize(Writer &writer) const {
  writer.String("sub_type");
  auto type = col_type_to_string(g_type);
  writer.String(type.c_str(), static_cast<SizeType>(type.length()));
  writer.String("clause");
  writer.String(str.c_str(), static_cast<SizeType>(str.length()));
}

template <typename Writer> void Ind_col::Serialize(Writer &writer) const {
  writer.StartObject();
  writer.String("name");
  auto &name = column->name_;
  writer.String(name.c_str(), static_cast<SizeType>(name.length()));
  writer.String("desc");
  writer.Bool(desc);
  writer.String("length");
  writer.Uint(length);
  writer.EndObject();
}

template <typename Writer> void Index::Serialize(Writer &writer) const {
  writer.StartObject();
  writer.String("name");
  writer.String(name_.c_str(), static_cast<SizeType>(name_.length()));
  writer.String(("index_columns"));
  writer.StartArray();
  for (auto ic : *columns_)
    ic->Serialize(writer);
  writer.EndArray();
  writer.EndObject();
}

Index::~Index() {
  for (auto id_col : *columns_) {
    delete id_col;
  }
  delete columns_;
}

static const char *json_string_member(const rapidjson::Value &obj,
                                      const char *key) {
  if (!obj.HasMember(key) || !obj[key].IsString())
    return nullptr;
  return obj[key].GetString();
}

static std::string metadata_string_member(const rapidjson::Value &obj,
                                          const char *new_key,
                                          const char *legacy_key,
                                          const std::string &legacy_default =
                                              std::string()) {
  if (const auto *value = json_string_member(obj, new_key))
    return value;
  if (const auto *value = json_string_member(obj, legacy_key)) {
    if (!legacy_default.empty() && value == legacy_default)
      return "";
    return value;
  }
  return "";
}

static int metadata_int_member(const rapidjson::Value &obj, const char *new_key,
                               const char *legacy_key) {
  if (obj.HasMember(new_key) && obj[new_key].IsInt())
    return obj[new_key].GetInt();
  if (legacy_key != nullptr && obj.HasMember(legacy_key) &&
      obj[legacy_key].IsInt())
    return obj[legacy_key].GetInt();
  return 0;
}

template <typename Writer> void Table::Serialize(Writer &writer) const {
  writer.StartObject();

  writer.String("name");
  writer.String(name_.c_str(), static_cast<SizeType>(name_.length()));
  writer.String("type");
  writer.String(get_type().c_str(), static_cast<SizeType>(get_type().length()));

  if (type == PARTITION) {
    auto part_table = static_cast<const Partition *>(this);
    writer.String("part_type");
    std::string part_type = part_table->get_part_type();
    writer.String(part_type.c_str(), static_cast<SizeType>(part_type.length()));
    writer.String("number_of_part");
    writer.Int(part_table->number_of_part);
    if (part_table->part_type == Partition::RANGE) {
      writer.String("part_range");
      writer.StartArray();
      for (auto par : part_table->positions) {
        writer.StartArray();
        writer.String(par.name.c_str(),
                      static_cast<SizeType>(par.name.length()));
        writer.Int(par.range);
        writer.EndArray();
      }
      writer.EndArray();
    } else if (part_table->part_type == Partition::LIST) {

      writer.String("part_list");
      writer.StartArray();
      for (auto list : part_table->lists) {
        writer.StartArray();
        writer.String(list.name.c_str(),
                      static_cast<SizeType>(list.name.length()));
        writer.StartArray();
        for (auto i : list.list)
          writer.Int(i);
        writer.EndArray();
        writer.EndArray();
      };
      writer.EndArray();
    }
  } else if (type == FK) {
    auto fk_table = static_cast<const FK_table *>(this);
    std::string parent = fk_table->parent->name_;
    std::string on_update = fk_table->enumToString(fk_table->on_update);
    std::string on_delete = fk_table->enumToString(fk_table->on_delete);
    writer.String("parent");
    writer.String(parent.c_str(), static_cast<SizeType>(parent.length()));
    writer.String("on_update");
    writer.String(on_update.c_str(), static_cast<SizeType>(on_update.length()));
    writer.String("on_delete");
    writer.String(on_delete.c_str(), static_cast<SizeType>(on_delete.length()));
  }

  writer.String("storage_engine");
  if (!storage_engine.empty())
    writer.String(storage_engine.c_str(),
                  static_cast<SizeType>(storage_engine.length()));
  else
    writer.String("");

  writer.String("storage_layout");
  if (!storage_layout.empty())
    writer.String(storage_layout.c_str(),
                  static_cast<SizeType>(storage_layout.length()));
  else
    writer.String("");

  writer.String("storage_tablespace");
  if (!storage_tablespace.empty())
    writer.String(storage_tablespace.c_str(),
                  static_cast<SizeType>(storage_tablespace.length()));
  else
    writer.String("");

  writer.String("storage_encryption");
  writer.String("");

  writer.String("storage_compression");
  if (!storage_compression.empty())
    writer.String(storage_compression.c_str(),
                  static_cast<SizeType>(storage_compression.length()));
  else
    writer.String("");

  writer.String("storage_block_size");
  writer.Int(storage_block_size);

  writer.String(("columns"));
  writer.StartArray();

  /* write all colummns */
  for (auto &col : *columns_) {
    writer.StartObject();
    col->Serialize(writer);
    if (col->type_ == Column::GENERATED) {
      static_cast<Generated_Column *>(col)->Serialize(writer);
    } else if (col->type_ == Column::BLOB) {
      static_cast<Blob_Column *>(col)->Serialize(writer);
    }
    writer.EndObject();
  }

  writer.EndArray();

  writer.String(("indexes"));
  writer.StartArray();
  for (auto *ind : *indexes_)
    ind->Serialize(writer);
  writer.EndArray();
  writer.EndObject();
}

Ind_col::Ind_col(Column *c, bool d) : column(c), desc(d) {}

Index::Index(std::string n) : name_(n), columns_() {
  columns_ = new std::vector<Ind_col *>;
}

void Index::AddInternalColumn(Ind_col *column) { columns_->push_back(column); }

static std::string index_column_list(const Index *index) {
  std::string def;
  for (auto idc : *index->columns_) {
    def += idc->column->name_;

    def += (idc->desc ? " DESC" : (rand_int(3) ? "" : " ASC"));
    def += ", ";
  }
  def.erase(def.length() - 2);
  return def;
}

static std::string create_index_sql(const Table *table, const Index *index) {
  return "CREATE INDEX " + index->name_ + " ON " + table->name_ + "(" +
         index_column_list(index) + ")";
}

static std::vector<Column *> primary_key_columns(const Table *table) {
  std::vector<Column *> pk_columns;
  for (auto *col : *table->columns_) {
    if (col->primary_key) {
      pk_columns.push_back(col);
    }
  }
  return pk_columns;
}

static std::string fk_reference_value_expr(const Table *table) {
  if (table == nullptr || table->type != Table::FK) {
    return "";
  }

  const auto *fk_table = static_cast<const FK_table *>(table);
  if (fk_table->parent == nullptr) {
    return "NULL";
  }

  auto pk_columns = primary_key_columns(fk_table->parent);
  if (pk_columns.empty()) {
    return "NULL";
  }

  return "(SELECT " + pk_columns.front()->name_ + " FROM " +
         fk_table->parent->name_ + " ORDER BY random() LIMIT 1)";
}

/* index definition */
std::string Index::definition() {
  return "INDEX " + name_ + "(" + index_column_list(this) + ") ";
}

static bool load_pg_partitions(Table *table, Thd1 *thd) {
  if (table->type != Table::PARTITION) {
    return true;
  }

  auto *part = static_cast<Partition *>(table);
  switch (part->part_type) {
  case Partition::HASH:
  case Partition::KEY:
    for (int i = 0; i < part->number_of_part; ++i) {
      std::string child = partition_child_name(table, "p" + std::to_string(i));
      std::string sql = "CREATE TABLE " + child + " PARTITION OF " + table->name_ +
                        " FOR VALUES WITH (MODULUS " +
                        std::to_string(part->number_of_part) + ", REMAINDER " +
                        std::to_string(i) + ")";
      if (!execute_sql(sql, thd))
        return false;
    }
    return true;
  case Partition::LIST:
    for (const auto &list : part->lists) {
      std::string sql = "CREATE TABLE " + partition_child_name(table, list.name) +
                        " PARTITION OF " + table->name_ + " FOR VALUES IN (";
      for (size_t i = 0; i < list.list.size(); ++i) {
        sql += std::to_string(list.list[i]);
        if (i + 1 != list.list.size())
          sql += ", ";
      }
      sql += ")";
      if (!execute_sql(sql, thd))
        return false;
    }
    return true;
  case Partition::RANGE:
    for (size_t i = 0; i < part->positions.size(); ++i) {
      std::string lower = (i == 0) ? "MINVALUE"
                                   : std::to_string(part->positions[i - 1].range);
      std::string upper = (i + 1 == part->positions.size())
                              ? "MAXVALUE"
                              : std::to_string(part->positions[i].range);
      std::string sql = "CREATE TABLE " +
                        partition_child_name(table, part->positions[i].name) +
                        " PARTITION OF " + table->name_ + " FOR VALUES FROM (" +
                        lower + ") TO (" + upper + ")";
      if (!execute_sql(sql, thd))
        return false;
    }
    return true;
  }
  return true;
}

bool Table::load(Thd1 *thd) {
  thd->ddl_query = true;
  if (!execute_sql(definition(false), thd)) {
    thd->thread_log << "Failed to create table " << name_ << std::endl;
    run_query_failed = true;
    return false;
  }

  if (!load_pg_partitions(this, thd)) {
    thd->thread_log << "Failed to create PostgreSQL partitions for " << name_
                    << std::endl;
    run_query_failed = true;
    return false;
  }

  /* load default data in table */
  if (!options->at(Option::JUST_LOAD_DDL)->getBool()) {

    thd->ddl_query = false;
    if (!InsertBulkRecord(thd))
      return false;
  }

  thd->ddl_query = true;
  if (!load_secondary_indexes(thd)) {
    return false;
  }

  if (this->type == Table::TABLE_TYPES::FK) {
    if (!static_cast<FK_table *>(this)->load_fk_constraint(thd)) {
      return false;
    }
  }

  if (run_query_failed) {
    thd->thread_log << "some other thread failed, Exiting. Please check logs "
                    << std::endl;
    return false;
  }

  return true;
}

Table::Table(std::string n) : name_(n), indexes_() {
  columns_ = new std::vector<Column *>;
  indexes_ = new std::vector<Index *>;
}

bool Table::load_secondary_indexes(Thd1 *thd) {

  if (indexes_->size() == 0)
    return true;

  for (auto id : *indexes_) {
    std::string sql = create_index_sql(this, id);
    if (!execute_sql(sql, thd)) {
      thd->thread_log << "Failed to add index " << id->name_ << " on " << name_
                      << std::endl;
      run_query_failed = true;
      return false;
    }
  }

  return true;
}

bool FK_table::load_fk_constraint(Thd1 *thd) {

  std::string constraint = name_ + "_" + parent->name_;
  std::string pk;
  for (const auto &col : *parent->columns_) {
    if (col->primary_key == true) {
      pk = col->name_;
      break;
    }
  }
  assert(pk.size() > 0);

  std::string sql = "ALTER TABLE " + name_ + " ADD CONSTRAINT " + constraint +
                    " FOREIGN KEY (ifk_col) REFERENCES " + parent->name_ +
                    " (" + pk + ")";
  sql += " ON UPDATE " + enumToString(on_update);
  sql += " ON DELETE  " + enumToString(on_delete);

  if (!execute_sql(sql, thd)) {
    thd->thread_log << "Failed to add fk constraint "
                    << " on " << name_ << std::endl;
    run_query_failed = true;
    return false;
  }
  return true;
}

/* Constructor used by load_metadata */
Partition::Partition(std::string n, std::string part_type_, int number_of_part_)
    : Table(n), number_of_part(number_of_part_) {
  set_part_type(part_type_);
}

/* Constructor used by new Partiton table */
Partition::Partition(std::string n) : Table(n) {

  part_type = supported[rand_int(supported.size() - 1)];

  number_of_part = rand_int(options->at(Option::MAX_PARTITIONS)->getInt(), 2);

  /* randomly pick ranges for partition */
  if (part_type == RANGE) {
    auto number_of_records =
        options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt();
    for (int i = 0; i < number_of_part; i++) {
      positions.emplace_back("p",
                             rand_int(g_integer_range * number_of_records));
    }
    std::sort(positions.begin(), positions.end(), Partition::compareRange);
    for (int i = 0; i < number_of_part; i++) {
      positions.at(i).name = "p" + std::to_string(i);
    }
    // adjust the range so we don't have overlapping ranges
    for (int i = 1; i < number_of_part; i++) {
      if (positions.at(i).range == positions.at(i - 1).range)
        for (int j = i; j < number_of_part; j++)
          positions.at(j).range++;
    }

  } else if (part_type == LIST) {
    auto number_of_records =
        rand_int(maximum_records_in_each_parititon_list * number_of_part,
                 number_of_part);

    /* temporary vector to store all number_of_records */
    for (int i = 0; i < number_of_records; i++)
      total_left_list.push_back(i);

    for (int i = 0; i < number_of_part; i++) {
      lists.emplace_back("p" + std::to_string(i));
      auto number_of_records_in_partition =
          rand_int(number_of_records) / number_of_part;

      if (number_of_records_in_partition == 0)
        number_of_records_in_partition = 1;

      for (int j = 0; j < number_of_records_in_partition; j++) {
        auto curr = rand_int(total_left_list.size() - 1);
        lists.at(i).list.push_back(total_left_list.at(curr));
        total_left_list.erase(total_left_list.begin() + curr);
      }
    }
  }
}

void Table::DropCreate(Thd1 *thd) {
  execute_sql("DROP TABLE " + name_, thd);
  execute_sql(definition(), thd);
}

void Table::Optimize(Thd1 *thd) {
  execute_sql("ANALYZE " + (type == PARTITION ? pg_partition_target(this) : name_),
              thd);
}

void Table::Check(Thd1 *thd) {
  auto rel = type == PARTITION ? pg_partition_target(this) : name_;
  get_check_result("SELECT current_database(), '" + rel + "', 'check', 'OK'",
                   thd);
}

void Table::Analyze(Thd1 *thd) {
  execute_sql("ANALYZE " + (type == PARTITION ? pg_partition_target(this) : name_),
              thd);
}

void Table::Truncate(Thd1 *thd) {
  if (referenced_by_fk(this)) {
    return;
  }
  execute_sql("TRUNCATE TABLE " + (type == PARTITION ? pg_partition_target(this) : name_),
              thd);
}

/* add or drop average 10% of max partitions */
void Partition::AddDrop(Thd1 *thd) {
  if (part_type == KEY || part_type == HASH) {
    int new_partition_count = number_of_part;
    if (rand_int(1) == 0) {
      if (number_of_part >= options->at(Option::MAX_PARTITIONS)->getInt()) {
        return;
      }
      new_partition_count++;
    } else {
      if (number_of_part <= 2) {
        return;
      }
      new_partition_count--;
    }

    std::string staging_table =
        name_ + "_rehash_" + std::to_string(rand_int(100000, 1000));
    bool success = execute_sql("CREATE TEMP TABLE " + staging_table +
                                   " AS TABLE " + name_,
                               thd);

    for (int i = 0; success && i < number_of_part; ++i) {
      std::string child = partition_child_name(this, "p" + std::to_string(i));
      success = execute_sql("ALTER TABLE " + name_ + " DETACH PARTITION " + child,
                            thd) &&
                execute_sql("DROP TABLE " + child, thd);
    }

    for (int i = 0; success && i < new_partition_count; ++i) {
      std::string child = partition_child_name(this, "p" + std::to_string(i));
      success = execute_sql("CREATE TABLE " + child + " PARTITION OF " + name_ +
                                " FOR VALUES WITH (MODULUS " +
                                std::to_string(new_partition_count) +
                                ", REMAINDER " + std::to_string(i) + ")",
                            thd);
    }

    if (success) {
      success = execute_sql("INSERT INTO " + name_ + " SELECT * FROM " +
                                staging_table,
                            thd);
    }

    execute_sql("DROP TABLE IF EXISTS " + staging_table, thd);

    if (success) {
      table_mutex.lock();
      number_of_part = new_partition_count;
      table_mutex.unlock();
    }
    return;
  }

  if (part_type == RANGE) {
    table_mutex.lock();
    if (positions.empty()) {
      table_mutex.unlock();
      return;
    }

    if (rand_int(1) == 0) {
      if (positions.size() <= 1) {
        table_mutex.unlock();
        return;
      }

      auto max_part = positions.back();
      auto prev_part = positions.at(positions.size() - 2);
      const std::string max_child = partition_child_name(this, max_part.name);
      const std::string prev_child = partition_child_name(this, prev_part.name);
      const std::string lower =
          positions.size() > 2 ? std::to_string(positions.at(positions.size() - 3).range)
                               : "MINVALUE";
      table_mutex.unlock();

      if (execute_sql("ALTER TABLE " + name_ + " DETACH PARTITION " + max_child,
                      thd) &&
          execute_sql("DROP TABLE " + max_child, thd) &&
          execute_sql("ALTER TABLE " + name_ + " DETACH PARTITION " + prev_child,
                      thd) &&
          execute_sql("ALTER TABLE " + name_ + " ATTACH PARTITION " + prev_child +
                          " FOR VALUES FROM (" + lower + ") TO (MAXVALUE)",
                      thd)) {
        table_mutex.lock();
        positions.pop_back();
        number_of_part--;
        table_mutex.unlock();
      }
      return;
    }

    auto max_part = positions.back();
    const std::string max_child = partition_child_name(this, max_part.name);
    const std::string lower =
        positions.size() > 1 ? std::to_string(positions.at(positions.size() - 2).range)
                             : "MINVALUE";
    table_mutex.unlock();

    std::string max_value_str =
        read_single_value("SELECT COALESCE(MAX(ip_col), 0) FROM " + max_child, thd);
    int max_value = 0;
    if (!max_value_str.empty()) {
      max_value = std::stoi(max_value_str);
    }
    int lower_num = lower == "MINVALUE" ? 0 : std::stoi(lower);
    int new_upper = std::max(max_value + rand_int(10, 1), lower_num + 1);
    std::string new_part_name = "p" + std::to_string(rand_int(1000, 100));
    std::string new_child = partition_child_name(this, new_part_name);

    if (execute_sql("ALTER TABLE " + name_ + " DETACH PARTITION " + max_child,
                    thd) &&
        execute_sql("ALTER TABLE " + name_ + " ATTACH PARTITION " + max_child +
                        " FOR VALUES FROM (" + lower + ") TO (" +
                        std::to_string(new_upper) + ")",
                    thd) &&
        execute_sql("CREATE TABLE " + new_child + " PARTITION OF " + name_ +
                        " FOR VALUES FROM (" + std::to_string(new_upper) +
                        ") TO (MAXVALUE)",
                    thd)) {
      table_mutex.lock();
      positions.back().range = new_upper;
      positions.emplace_back(new_part_name, new_upper);
      number_of_part++;
      table_mutex.unlock();
    }
    return;
  }

  if (part_type == LIST) {
    if (rand_int(1) == 0) {
      table_mutex.lock();
      if (lists.empty()) {
        table_mutex.unlock();
        return;
      }
      auto par = lists.at(rand_int(lists.size() - 1));
      table_mutex.unlock();
      std::string child = partition_child_name(this, par.name);
      if (execute_sql("ALTER TABLE " + name_ + " DETACH PARTITION " + child,
                      thd) &&
          execute_sql("DROP TABLE " + child, thd)) {
        table_mutex.lock();
        number_of_part--;
        for (auto i = lists.begin(); i != lists.end(); i++) {
          if (i->name.compare(par.name) == 0) {
            for (auto j : i->list)
              total_left_list.push_back(j);
            lists.erase(i);
            break;
          }
        }
        table_mutex.unlock();
      }
      return;
    }

    size_t number_of_records_in_partition =
        rand_int(options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt()) /
        rand_int(options->at(Option::MAX_PARTITIONS)->getInt(), 1);

    if (number_of_records_in_partition == 0)
      number_of_records_in_partition = 1;

    table_mutex.lock();
    if (number_of_records_in_partition > total_left_list.size()) {
      table_mutex.unlock();
      return;
    }

    std::vector<int> temp_list;
    while (temp_list.size() != number_of_records_in_partition) {
      auto curr = rand_int(total_left_list.size() - 1);
      int value = total_left_list.at(curr);
      bool exists = false;
      for (auto l : temp_list) {
        if (l == value)
          exists = true;
      }
      if (!exists)
        temp_list.push_back(value);
    }
    std::string new_part_name = "p" + std::to_string(rand_int(1000, 100));
    table_mutex.unlock();

    std::string sql = "CREATE TABLE " +
                      partition_child_name(this, new_part_name) +
                      " PARTITION OF " + name_ + " FOR VALUES IN (";
    for (size_t i = 0; i < temp_list.size(); i++) {
      sql += std::to_string(temp_list.at(i));
      if (i + 1 != temp_list.size())
        sql += ", ";
    }
    sql += ")";
    if (execute_sql(sql, thd)) {
      table_mutex.lock();
      number_of_part++;
      lists.emplace_back(new_part_name);
      for (auto l : temp_list) {
        lists.back().list.push_back(l);
        total_left_list.erase(
            std::remove(total_left_list.begin(), total_left_list.end(), l),
            total_left_list.end());
      }
      table_mutex.unlock();
    }
  }
}

Table::~Table() {
  for (auto ind : *indexes_)
    delete ind;
  for (auto col : *columns_) {
    col->mutex.lock();
    delete col;
  }
  delete columns_;
  delete indexes_;
}

/* create default column */
void Table::CreateDefaultColumn() {
  auto no_auto_inc = opt_bool(NO_AUTO_INC);
  bool has_auto_increment = false;

  if (type == FK) {
    std::string name = "fk_col";
    Column::COLUMN_TYPES type = Column::INTEGER;
    AddInternalColumn(new Column{name, this, type});
  }

  /* if table is partition add new column */
  if (type == PARTITION) {
    std::string name = "p_col";
    Column::COLUMN_TYPES type;
    if (static_cast<Partition *>(this)->part_type == Partition::LIST)
      type = Column::INTEGER;
    else
      type = Column::INT;
    auto col = new Column{name, this, type};
    AddInternalColumn(col);
  }

  /* create normal column */
  static auto max_col = opt_int(COLUMNS);

  auto max_columns = rand_int(max_col, 1);

  for (int i = 0; i < max_columns; i++) {
    std::string name;
    Column::COLUMN_TYPES type;
    Column *col;
    /*  if we need to create primary column */

    /* First column can be primary */
    if (i == 0 && rand_int(100) <= options->at(Option::PRIMARY_KEY)->getInt()) {
      type = Column::INT;
      name = "pkey";
      col = new Column{name, this, type};
      col->primary_key = true;

      if (!no_auto_inc && rand_int(3) < 3) {
        /* 75% of primary key tables are autoinc */
        if (this->type == PARTITION && rand_int(3) == 1)
          columns_->at(0)->auto_increment = true;
        else
          col->auto_increment = true;
        has_auto_increment = true;
      }
    } else {
      name = std::to_string(i);
      Column::COLUMN_TYPES col_type = Column::COLUMN_MAX;
      static auto no_virtual_col = opt_bool(NO_VIRTUAL_COLUMNS);
      static auto no_blob_col = opt_bool(NO_BLOB);

      /* loop untill we select some column */
      while (col_type == Column::COLUMN_MAX) {

        /* columns are 6:1:2:2:4:2:1:1 INT:INTEGER:FLOAT:DOUBLE:VARCHAR:CHAR:TIMESTAMP:BOOL plus JSON/BLOB */
        auto prob = rand_int(23);

        /* intial columns can't be generated columns. also 50% of tables last
         * columns are virtuals */
        if (!no_virtual_col && i >= .8 * max_columns && rand_int(1) == 1)
          col_type = Column::GENERATED;
        else if (prob < 5)
          col_type = Column::INT;
        else if (prob < 6)
          col_type = Column::INTEGER;
        else if (prob < 8)
          col_type = Column::FLOAT;
        else if (prob < 10)
          col_type = Column::DOUBLE;
        else if (prob < 14)
          col_type = Column::VARCHAR;
        else if (prob < 16)
          col_type = Column::CHAR;
        else if (prob < 17)
          col_type = Column::TIMESTAMP;
        else if (!no_blob_col && prob < 19)
          col_type = Column::BLOB;
        else if (prob < 21)
          col_type = Column::BOOL;
        else if (prob < 23)
          col_type = Column::JSON;
      }

      if (col_type == Column::GENERATED)
        col = new Generated_Column(name, this);
      else if (col_type == Column::BLOB)
        col = new Blob_Column(name, this);
      else
        col = new Column(name, this, col_type);

      /* 25% column can have auto_inc */
      if (col->type_ == Column::INT && !no_auto_inc &&
          has_auto_increment == false && rand_int(100) > 25) {
        col->auto_increment = true;
        has_auto_increment = true;
      }
    }
    AddInternalColumn(col);
  }
}

/* create default indexes */
void Table::CreateDefaultIndex() {

  int auto_inc_pos = -1; // auto_inc_column_position

  static size_t max_indexes = opt_int(INDEXES);

  if (max_indexes == 0)
    return;

  /* if table have few column, decrease number of indexes */
  size_t index_limit = columns_->size() < max_indexes ? columns_->size() : max_indexes;
  index_limit = std::min<size_t>(index_limit, 4);
  size_t indexes = rand_int(index_limit, 1);

  /* for auto-inc columns handling, we need to add auto_inc as first column */
  for (size_t i = 0; i < columns_->size(); i++) {
    if (columns_->at(i)->auto_increment) {
      auto_inc_pos = i;
    }
  }

  /*which column will have auto_inc */
  auto_inc_index = rand_int(indexes - 1, 0);

  for (size_t i = 0; i < indexes; i++) {
    Index *id = new Index(name_ + "i" + std::to_string(i));

    static size_t max_columns = opt_int(INDEX_COLUMNS);

    int number_of_compressed = 0;

    for (auto column : *columns_)
      if (column->compressed)
        number_of_compressed++;

    size_t number_of_columns = columns_->size() - number_of_compressed;

    /* only compressed columns */
    if (number_of_columns == 0)
      return;

    number_of_columns = rand_int(
        (max_columns < number_of_columns ? max_columns : number_of_columns), 1);
    number_of_columns = std::min<size_t>(number_of_columns, 4);

    std::vector<int> col_pos; // position of columns

    /* pick some columns */
    size_t attempts = 0;
    while (col_pos.size() < number_of_columns && attempts++ < columns_->size() * 8) {
      int current = rand_int(columns_->size() - 1);
      if (columns_->at(current)->compressed)
        continue;
      if (!pg_indexable_column(columns_->at(current)))
        continue;
      /* auto-inc column should be first column in auto_inc_index */
      if (auto_inc_pos != -1 && i == auto_inc_index && col_pos.size() == 0)
        col_pos.push_back(auto_inc_pos);
      else {
        bool already_added = false;
        for (auto id : col_pos) {
          if (id == current)
            already_added = true;
        }
        if (!already_added)
          col_pos.push_back(current);
      }
    } // while

    if (col_pos.empty())
      continue;

    for (auto pos : col_pos) {
      auto col = columns_->at(pos);
      static bool no_desc_support = opt_bool(NO_DESC_INDEX);
      bool column_desc = false;
      if (!no_desc_support) {
        column_desc = rand_int(100) < DESC_INDEXES_IN_COLUMN
                          ? true
                          : false; // 33 % are desc //
      }
      id->AddInternalColumn(
          new Ind_col(col, column_desc)); // desc is set as true
      if (id->columns_->size() >= 4 || pg_index_total_width(id) > 192) {
        delete id->columns_->back();
        id->columns_->pop_back();
        break;
      }
    }
    if (id->columns_->empty()) {
      delete id;
      continue;
    }
    AddInternalIndex(id);
  }
}

/* Create new table and pick some attributes */
Table *Table::table_id(TABLE_TYPES type, int id) {
  Table *table;
  std::string name = TABLE_PREFIX + std::to_string(id);
  switch (type) {
  case PARTITION:
    table = new Partition(name + PARTITION_SUFFIX);
    break;
  case NORMAL:
    table = new Table(name);
    break;
  case TEMPORARY:
    table = new Temporary_table(name + TEMP_SUFFIX);
    break;
  default:
    throw std::runtime_error("Unhandle Table type");
  case FK:
    table = new FK_table(name + FK_SUFFIX);
    break;
  }

  table->type = type;

  table->number_of_initial_records =
      options->at(Option::EXACT_INITIAL_RECORDS)->getBool()
          ? options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt()
          : rand_int(options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt());
  table->storage_engine.clear();
  table->storage_layout.clear();
  table->storage_tablespace.clear();
  table->storage_compression.clear();
  table->storage_encryption = "N";
  table->storage_block_size = 0;

  /* If indexes are disabled, also disable auto_inc */
  if (!options->at(Option::INDEXES)->getInt())
    options->at(Option::NO_AUTO_INC)->setBool(true);

  table->CreateDefaultColumn();
  table->CreateDefaultIndex();
  if (type == FK) {
    static_cast<FK_table *>(table)->pickRefrence(table);
  }

  return table;
}

/* check if table has a primary key */
bool Table::has_pk() const {
  for (const auto &col : *columns_) {
    if (col->primary_key)
      return true;
  }
  return false;
}

/* prepare table definition */
std::string Table::definition(bool with_index) {
  std::string def = "CREATE";
  if (type == TEMPORARY)
    def += " TEMPORARY";
  def += " TABLE " + name_ + " (";

  if (columns_->size() == 0)
    throw std::runtime_error("no column in table " + name_);

  /* add columns */
  for (auto col : *columns_) {
    def += col->definition() + ", ";
  }

  /* if column has primary key */
  for (auto col : *columns_) {
    if (col->primary_key) {
      def += " PRIMARY KEY(";
      if (type == PARTITION) {
        if (rand_int(1) == 0)
          def += col->name_ + ", ip_col";
        else
          def += "ip_col, " + col->name_;
      } else
        def += col->name_;
      def += "), ";
    }
  }

  (void)with_index;

  def.erase(def.length() - 2);

  def += " )";
  if (type == PARTITION) {
    auto par = static_cast<Partition *>(this);
    auto part_type = par->part_type == Partition::KEY ? Partition::HASH
                                                      : par->part_type;
    switch (part_type) {
    case Partition::RANGE:
      def += " PARTITION BY RANGE (ip_col)";
      break;
    case Partition::LIST:
      def += " PARTITION BY LIST (ip_col)";
      break;
    case Partition::HASH:
    case Partition::KEY:
      def += " PARTITION BY HASH (ip_col)";
      break;
    }
  }
  return def;
}

/* create default table includes all tables*/
void generate_metadata_for_tables() {
  auto tables = opt_int(TABLES);

  auto only_temporary_tables = opt_bool(ONLY_TEMPORARY);

  if (!only_temporary_tables) {
    for (int i = 1; i <= tables; i++) {
      if (!options->at(Option::ONLY_PARTITION)->getBool()) {
        auto parent_table = Table::table_id(Table::NORMAL, i);
        all_tables->push_back(parent_table);

        /* Create FK table */
        if (!options->at(Option::NO_FK)->getBool() &&
            options->at(Option::FK_PROB)->getInt() > rand_int(100) &&
            parent_table->has_pk()) {
          auto child_table = Table::table_id(Table::FK, i);
          all_tables->push_back(child_table);
          static_cast<FK_table *>(child_table)->parent = parent_table;
        }
      }

      if (!options->at(Option::NO_PARTITION)->getBool() &&
          options->at(Option::PARTITION_PROB)->getInt() > rand_int(100))
        all_tables->push_back(Table::table_id(Table::PARTITION, i));
      /*
      if (!options->at(Option::NO_FK)->getBool() &&
          options->at(Option::FK_PROB)->getInt() > rand_int(100))
        all_tables->push_back(Table::table_id(Table::FK, i));
        */
    }
  }
}

bool execute_sql(const std::string &sql, Thd1 *thd) {
  auto query = sql.c_str();
  static auto log_all = opt_bool(LOG_ALL_QUERIES);
  static auto log_failed = opt_bool(LOG_FAILED_QUERIES);
  static auto log_success = opt_bool(LOG_SUCCEDED_QUERIES);
  static auto log_query_duration = opt_bool(LOG_QUERY_DURATION);
  static auto log_client_output = opt_bool(LOG_CLIENT_OUTPUT);
  static auto log_query_numbers = opt_bool(LOG_QUERY_NUMBERS);
  std::chrono::system_clock::time_point begin, end;

  if (log_query_duration) {
    begin = std::chrono::system_clock::now();
  }

  thd->success = false;
  thd->result.reset();
  PGresult *raw_result = PQexec(thd->conn, query);
  ExecStatusType status =
      raw_result ? PQresultStatus(raw_result) : PGRES_FATAL_ERROR;
  bool query_success = status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;

  if (log_query_duration) {
    end = std::chrono::system_clock::now();

    /* elpased time in micro-seconds */
    auto te_start = std::chrono::duration_cast<std::chrono::microseconds>(
        begin - start_time);
    auto te_query =
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin);
    auto in_time_t = std::chrono::system_clock::to_time_t(begin);

    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%dT%X");

    thd->thread_log << ss.str() << " " << te_start.count() << "=>"
                    << te_query.count() << "ms ";
  }
  thd->performed_queries_total++;

  if (!query_success) { // query failed
    thd->failed_queries_total++;
    thd->max_con_fail_count++;
    const char *sqlstate =
        raw_result ? PQresultErrorField(raw_result, PG_DIAG_SQLSTATE) : nullptr;
    if (log_all || log_failed) {
      thd->thread_log << " F " << sql << std::endl;
      thd->thread_log << "Error code "
                      << (sqlstate ? sqlstate : "n/a") << " "
                      << "message "
                      << (raw_result ? PQresultErrorMessage(raw_result)
                                     : PQerrorMessage(thd->conn))
                      << std::endl;
    }
    if (is_connection_lost(thd->conn, raw_result)) {
      thd->thread_log << "connection lost while processing " + sql;
      if (sqlstate != nullptr) {
        thd->thread_log << " sqlstate=" << sqlstate;
      }
      thd->thread_log << std::endl;
      run_query_failed = true;
    }
  } else {
    thd->max_con_fail_count = 0;
    thd->success = true;
    thd->result = std::shared_ptr<PGresult>(raw_result, [](PGresult *r) {
      if (r)
        PQclear(r);
    });
    raw_result = nullptr;

    if (log_client_output) {
      if (thd->result != nullptr &&
          PQresultStatus(thd->result.get()) == PGRES_TUPLES_OK) {
        const int num_fields = PQnfields(thd->result.get());
        const int row_count = PQntuples(thd->result.get());
        for (int row_idx = 0; row_idx < row_count; ++row_idx) {
          for (int i = 0; i < num_fields; i++) {
            if (PQgetisnull(thd->result.get(), row_idx, i)) {
              thd->client_log << "#NO DATA"
                              << "#";
            } else if (strlen(PQgetvalue(thd->result.get(), row_idx, i)) == 0) {
              thd->client_log << "EMPTY"
                              << "#";
            } else {
              thd->client_log << PQgetvalue(thd->result.get(), row_idx, i)
                              << "#";
            }
          }
          if (log_query_numbers) {
            thd->client_log << ++thd->query_number;
          }
          thd->client_log << '\n';
        }
      }
    }

    /* log successful query */
    if (log_all || log_success) {
      thd->thread_log << " S " << sql;
      long long number = 0;
      if (thd->result != nullptr &&
          PQresultStatus(thd->result.get()) == PGRES_TUPLES_OK) {
        number = PQntuples(thd->result.get());
      } else if (thd->result != nullptr) {
        auto tuples = PQcmdTuples(thd->result.get());
        if (tuples != nullptr && tuples[0] != '\0') {
          number = std::stoll(tuples);
        }
      }
      thd->thread_log << " rows:" << number << std::endl;
    }
  }

  if (raw_result != nullptr) {
    PQclear(raw_result);
  }

  if (thd->ddl_query) {
    ddl_logs_write.lock();
    thd->ddl_logs << thd->thread_id << " " << sql << " "
                  << PQerrorMessage(thd->conn) << std::endl;
    ddl_logs_write.unlock();
  }

  return query_success;
}

// todo pick relevent table//
void Table::ModifyColumn(Thd1 *thd) {
  Column *col = nullptr;
  size_t col_pos = 0;
  /* store old value */
  int length = 0;
  bool auto_increment = false;
  bool compressed = false; // percona type compressed

  // try maximum 50 times to get a valid column
  int i = 0;
  while (i < 50 && col == nullptr) {
    auto col1 = columns_->at(rand_int(columns_->size() - 1));
    switch (col1->type_) {
    case Column::BLOB:
    case Column::VARCHAR:
    case Column::CHAR:
    case Column::FLOAT:
    case Column::DOUBLE:
    case Column::INT:
    case Column::INTEGER:
    case Column::TIMESTAMP:
    case Column::BOOL:
    case Column::JSON:
      col = col1;
      col_pos = static_cast<size_t>(std::distance(columns_->begin(),
                                                  std::find(columns_->begin(),
                                                            columns_->end(),
                                                            col1)));
      length = col->length;
      auto_increment = col->auto_increment;
      compressed = col->compressed;
      col->mutex.lock(); // lock column so no one can modify it //
      break;
    case Column::GENERATED:
      col = col1;
      col_pos = static_cast<size_t>(std::distance(columns_->begin(),
                                                  std::find(columns_->begin(),
                                                            columns_->end(),
                                                            col1)));
      col->mutex.lock();
      break;
    case Column::COLUMN_MAX:
      break;
    }
    i++;
  }

  /* could not find a valid column to process */
  if (col == nullptr)
    return;

  if (col->length != 0)
    col->length = rand_int(g_max_columns_length, 0);

  if (col->auto_increment == true and rand_int(5) == 0)
    col->auto_increment = false;

  col->compressed = false;

  if (col->type_ == Column::GENERATED) {
    auto *new_col = new Generated_Column(col->name_, this);
    new_col->name_ = col->name_;
    bool success = execute_sql("ALTER TABLE " + name_ + " DROP COLUMN " +
                                   col->name_ + ", ADD COLUMN " +
                                   new_col->definition(),
                               thd);
    if (success) {
      auto *old_col = col;
      table_mutex.lock();
      columns_->at(col_pos) = new_col;
      table_mutex.unlock();
      old_col->mutex.unlock();
      delete old_col;
    } else {
      delete new_col;
      col->mutex.unlock();
    }
    return;
  }

  bool success = true;
  std::string sql = "ALTER TABLE " + name_ + " ALTER COLUMN " + col->name_ +
                    " TYPE " + col->type_clause();
  success = execute_sql(sql, thd);

  if (success && auto_increment != col->auto_increment) {
    if (col->auto_increment) {
      success = execute_sql("ALTER TABLE " + name_ + " ALTER COLUMN " +
                                col->name_ +
                                " ADD GENERATED BY DEFAULT AS IDENTITY",
                            thd);
    } else {
      success = execute_sql("ALTER TABLE " + name_ + " ALTER COLUMN " +
                                col->name_ + " DROP IDENTITY IF EXISTS",
                            thd);
    }
  }

  /* if not successful rollback */
  if (!success) {
    col->length = length;
    col->auto_increment = auto_increment;
    col->compressed = compressed;
  }

  col->mutex.unlock();
}

/* alter table drop column */
void Table::DropColumn(Thd1 *thd) {
  table_mutex.lock();

  /* do not drop last column */
  if (columns_->size() == 1) {
    table_mutex.unlock();
    return;
  }
  auto ps = rand_int(columns_->size() - 1); // position

  auto name = columns_->at(ps)->name_;

  if (rand_int(100, 1) <= options->at(Option::PRIMARY_KEY)->getInt() &&
      name.find("pkey") != std::string::npos) {
    table_mutex.unlock();
    return;
  }

  std::string sql = "ALTER TABLE " + name_ + " DROP COLUMN " + name;
  table_mutex.unlock();

  if (execute_sql(sql, thd)) {
    table_mutex.lock();

    std::vector<int> indexes_to_drop;
    for (auto id = indexes_->begin(); id != indexes_->end(); id++) {
      auto index = *id;

      for (auto id_col = index->columns_->begin();
           id_col != index->columns_->end(); id_col++) {
        auto ic = *id_col;
        if (ic->column->name_.compare(name) == 0) {
          if (index->columns_->size() == 1) {
            delete index;
            indexes_to_drop.push_back(id - indexes_->begin());
          } else {
            delete ic;
            index->columns_->erase(id_col);
          }
          break;
        }
      }
    }
    std::sort(indexes_to_drop.begin(), indexes_to_drop.end(),
              std::greater<int>());

    for (auto &i : indexes_to_drop) {
      indexes_->at(i) = indexes_->back();
      indexes_->pop_back();
    }
    // table->indexes_->erase(id);

    for (auto pos = columns_->begin(); pos != columns_->end(); pos++) {
      auto col = *pos;
      if (col->name_.compare(name) == 0) {
        col->mutex.lock();
        delete col;
        columns_->erase(pos);
        break;
      }
    }
    table_mutex.unlock();
  }
}

/* alter table add random column */
void Table::AddColumn(Thd1 *thd) {

  static auto no_use_virtual = opt_bool(NO_VIRTUAL_COLUMNS);
  static auto use_blob = !options->at(Option::NO_BLOB)->getBool();

  std::string sql = "ALTER TABLE " + name_ + " ADD COLUMN ";

  Column::COLUMN_TYPES col_type = Column::COLUMN_MAX;

  auto use_virtual = true;

  // lock table to create definition
  table_mutex.lock();

  if (no_use_virtual ||
      (columns_->size() == 1 && columns_->at(0)->auto_increment == true))
    use_virtual = false;

  while (col_type == Column::COLUMN_MAX) {
    /* new columns are in ratio of 2:2:2:1:1:1:1:1
     * INT:VARCHAR:CHAR:BOOL:GENERATED:TIMESTAMP:JSON:BLOB */
    auto prob = rand_int(10);
    if (prob < 1)
      col_type = Column::INTEGER;
    else if (prob < 3)
      col_type = Column::INT;
    else if (prob < 5)
      col_type = Column::VARCHAR;
    else if (prob < 6)
      col_type = Column::CHAR;
    else if (prob < 7 && use_virtual)
      col_type = Column::GENERATED;
    else if (prob < 8)
      col_type = Column::BOOL;
    else if (prob < 9)
      col_type = Column::TIMESTAMP;
    else if (prob < 10)
      col_type = Column::JSON;
    else if (prob < 11 && use_blob)
      col_type = Column::BLOB;
  }

  Column *tc = nullptr;

  for (int attempt = 0; attempt < 64 && tc == nullptr; ++attempt) {
    std::string name =
        "N" + std::to_string(rand_int(100000, 1000)) + "_" + std::to_string(attempt);

    if (col_type == Column::GENERATED)
      tc = new Generated_Column(name, this);
    else if (col_type == Column::BLOB)
      tc = new Blob_Column(name, this);
    else
      tc = new Column(name, this, col_type);

    if (column_name_exists(this, tc->name_)) {
      delete tc;
      tc = nullptr;
    }
  }

  if (tc == nullptr) {
    table_mutex.unlock();
    return;
  }

  sql += tc->definition();

  (void)pick_algorithm_lock;

  table_mutex.unlock();

  if (execute_sql(sql, thd)) {
    table_mutex.lock();
    auto add_new_column =
        true; // check if there is already a column with this name
    for (auto col : *columns_) {
      if (col->name_.compare(tc->name_) == 0)
        add_new_column = false;
    }

    if (add_new_column)
      AddInternalColumn(tc);
    else
      delete tc;

    table_mutex.unlock();
  } else
    delete tc;
}

/* randomly drop some index of table */
void Table::DropIndex(Thd1 *thd) {
  table_mutex.lock();
  if (indexes_ != nullptr && indexes_->size() > 0) {
    auto index = indexes_->at(rand_int(indexes_->size() - 1));
    auto name = index->name_;
    std::string sql = "DROP INDEX IF EXISTS " + name;
    table_mutex.unlock();
    if (execute_sql(sql, thd)) {
      table_mutex.lock();
      for (size_t i = 0; i < indexes_->size(); i++) {
        auto ix = indexes_->at(i);
        if (ix->name_.compare(name) == 0) {
          delete ix;
          indexes_->at(i) = indexes_->back();
          indexes_->pop_back();
          break;
        }
      }
      table_mutex.unlock();
    }
  } else {
    table_mutex.unlock();
    thd->thread_log << "no index to drop " + name_ << std::endl;
  }
}

/*randomly add some index on the table */
void Table::AddIndex(Thd1 *thd) {
  static size_t max_columns = opt_int(INDEX_COLUMNS);
  table_mutex.lock();
  Index *id = nullptr;
  for (int attempt = 0; attempt < 64 && id == nullptr; ++attempt) {
    auto i = rand_int(100000, 1000);
    std::string candidate = name_ + std::to_string(i);
    if (!index_name_exists(this, candidate))
      id = new Index(candidate);
  }

  if (id == nullptr) {
    table_mutex.unlock();
    return;
  }

  /* number of columns to be added */
  int no_of_columns = rand_int(
      (max_columns < columns_->size() ? max_columns : columns_->size()), 1);
  no_of_columns = std::min(no_of_columns, 4);

  std::vector<int> col_pos; // position of columns

  /* pick some columns */
  size_t attempts = 0;
  while (col_pos.size() < (size_t)no_of_columns && attempts++ < columns_->size() * 8) {
    int current = rand_int(columns_->size() - 1);
    if (!pg_indexable_column(columns_->at(current)))
      continue;
    /* auto-inc column should be first column in auto_inc_index */
    bool already_added = false;
    for (auto id : col_pos) {
      if (id == current)
        already_added = true;
    }
    if (!already_added)
      col_pos.push_back(current);
  } // while

  for (auto pos : col_pos) {
    auto col = columns_->at(pos);
    static bool no_desc_support = opt_bool(NO_DESC_INDEX);
    bool column_desc = false;
    if (!no_desc_support) {
      column_desc = rand_int(100) < DESC_INDEXES_IN_COLUMN
                        ? true
                        : false; // 33 % are desc //
    }
    id->AddInternalColumn(new Ind_col(col, column_desc)); // desc is set as true
    if (id->columns_->size() >= 4 || pg_index_total_width(id) > 192) {
      delete id->columns_->back();
      id->columns_->pop_back();
      break;
    }
  }

  if (id->columns_->empty()) {
    table_mutex.unlock();
    delete id;
    return;
  }

  std::string sql = create_index_sql(this, id);
  table_mutex.unlock();

  if (execute_sql(sql, thd)) {
    table_mutex.lock();
    auto do_not_add = false; // check if there is already a index with this name
    for (auto ind : *indexes_) {
      if (ind->name_.compare(id->name_) == 0)
        do_not_add = true;
    }
    if (!do_not_add)
      AddInternalIndex(id);
    else
      delete id;

    table_mutex.unlock();
  } else {
    delete id;
  }
}

void Table::DeleteAllRows(Thd1 *thd) {
  std::string sql = "DELETE FROM " + name_;
  if (type == PARTITION && rand_int(100) < 98) {
    execute_sql("DELETE FROM " + pg_partition_target(this), thd);
    return;
  }
  execute_sql(sql, thd);
}

void Table::SelectAllRow(Thd1 *thd) {
  std::string sql = "SELECT * FROM " + name_;
  if (type == PARTITION && rand_int(100) < 98) {
    execute_sql("SELECT * FROM " + pg_partition_target(this), thd);
    return;
  }
  execute_sql(sql, thd);
}

void Table::IndexRename(Thd1 *thd) {
  table_mutex.lock();
  if (indexes_->size() == 0)
    table_mutex.unlock();
  else {
    auto ps = rand_int(indexes_->size() - 1);
    auto name = indexes_->at(ps)->name_;
    /* ALTER index to _rename or back to orignal_name */
    std::string new_name = "_rename";
    static auto s = new_name.size();
    if (name.size() > s &&
        name.substr(name.length() - s).compare("_rename") == 0)
      new_name = name.substr(0, name.length() - s);
    else
      new_name = name + new_name;
    if (index_name_exists(this, new_name)) {
      table_mutex.unlock();
      return;
    }
    std::string sql = "ALTER INDEX " + name + " RENAME TO " + new_name;
    table_mutex.unlock();
    if (execute_sql(sql, thd)) {
      table_mutex.lock();
      for (auto &ind : *indexes_) {
        if (ind->name_.compare(name) == 0)
          ind->name_ = new_name;
      }
      table_mutex.unlock();
    }
  }
}

void Table::ColumnRename(Thd1 *thd) {
  table_mutex.lock();
  auto ps = rand_int(columns_->size() - 1);
  auto name = columns_->at(ps)->name_;
  /* ALTER column to _rename or back to orignal_name */
  std::string new_name = "_rename";
  static auto s = new_name.size();
  if (name.size() > s && name.substr(name.length() - s).compare("_rename") == 0)
    new_name = name.substr(0, name.length() - s);
  else
    new_name = name + new_name;
  std::string sql =
      "ALTER TABLE " + name_ + " RENAME COLUMN " + name + " To " + new_name;
  table_mutex.unlock();
  if (execute_sql(sql, thd)) {
    table_mutex.lock();
    for (auto &col : *columns_) {
      if (col->name_.compare(name) == 0)
        col->name_ = new_name;
    }
    table_mutex.unlock();
  }
}

void Table::DeleteRandomRow(Thd1 *thd) {
  table_mutex.lock();
  auto where = -1;
  bool only_bool = true;
  int pk_pos = -1;

  for (size_t i = 0; i < columns_->size(); i++) {
    auto col = columns_->at(i);
    if (col->type_ != Column::BOOL)
      only_bool = false;
    if (col->primary_key)
      pk_pos = i;
  }

  /* 50% time we use primary key column */
  if (pk_pos != -1 && rand_int(100) > 50)
    where = pk_pos;
  else {
    /* iterate over and over to find a valid column */
    while (where < 0) {
      auto col_pos = rand_int(columns_->size() - 1);
      switch (columns_->at(col_pos)->type_) {
      case Column::BOOL:
        if (only_bool || rand_int(1000) == 0)
          where = col_pos;
        break;
      case Column::INT:
      case Column::TIMESTAMP:
      case Column::FLOAT:
      case Column::DOUBLE:
      case Column::VARCHAR:
      case Column::CHAR:
      case Column::BLOB:
      case Column::GENERATED:
        where = col_pos;
        break;
      case Column::JSON:
        if (rand_int(1000) < 50)
          where = col_pos;
        break;
      case Column::INTEGER:
        if (rand_int(1000) < 10)
          where = col_pos;
        break;
      case Column::COLUMN_MAX:
        break;
      }
    }
  }
  std::string sql = "DELETE FROM " + name_;

  if (type == PARTITION && rand_int(10) < 2)
    sql = "DELETE FROM " + pg_partition_target(this);
  sql += " WHERE " + columns_->at(where)->name_;

  auto prob = rand_int(100);
  if (prob <= 90)
    sql += " = " + columns_->at(where)->rand_value();
  else if (prob <= 92)
    sql += " >= " + columns_->at(where)->rand_value() + " AND " +
           columns_->at(where)->name_ +
           " <= " + columns_->at(where)->rand_value();
  else if (prob <= 96)
    sql += " IN (" + columns_->at(where)->rand_value() + "," +
           columns_->at(where)->rand_value() + ")";
  else if (prob <= 99)
    sql += " BETWEEN " + columns_->at(where)->rand_value() + " AND " +
           columns_->at(where)->rand_value();
  else if (supports_like_predicate(columns_->at(where)))
    sql += " LIKE " + prepare_like_string(columns_->at(where)->rand_value());
  else
    sql += " = " + columns_->at(where)->rand_value();

  table_mutex.unlock();
  execute_sql(sql, thd);
}

void Table::SelectRandomRow(Thd1 *thd) {
  table_mutex.lock();
  auto where = -1;
  while (where < 0) {
    auto col_pos = rand_int(columns_->size() - 1);
    switch (columns_->at(col_pos)->type_) {
    case Column::BOOL:
      if (rand_int(1000) < 10)
        where = col_pos;
      break;
    case Column::INT:
    case Column::TIMESTAMP:
    case Column::FLOAT:
    case Column::DOUBLE:
    case Column::VARCHAR:
    case Column::CHAR:
    case Column::BLOB:
    case Column::GENERATED:
      where = col_pos;
      break;
    case Column::JSON:
      if (rand_int(1000) < 50)
        where = col_pos;
      break;
    case Column::INTEGER:
      if (rand_int(1000) < 10)
        where = col_pos;
      break;
    case Column::COLUMN_MAX:
      break;
    }
  }
  std::string sql = "SELECT * FROM " + name_;

  /* if it partition table randomly pick some partition */
  if (type == PARTITION && rand_int(10) < 2)
    sql = "SELECT * FROM " + pg_partition_target(this);

  sql += " WHERE " + columns_->at(where)->name_;
  auto prob = rand_int(100);
  if (rand_int(1000) < 2)
    sql += " NOT BETWEEN " + columns_->at(where)->rand_value() + " AND " +
           columns_->at(where)->rand_value();
  else if (prob <= 90)
    sql += " = " + columns_->at(where)->rand_value();
  else if (prob <= 92)
    sql += " >= " + columns_->at(where)->rand_value();
  else if (prob <= 94)
    sql += " >= " + columns_->at(where)->rand_value() + " AND " +
           columns_->at(where)->name_ +
           " <= " + columns_->at(where)->rand_value();
  else if (prob <= 96)
    sql += " IN (" + columns_->at(where)->rand_value() + ", " +
           columns_->at(where)->rand_value() + ")";
  else if (prob <= 98 && supports_like_predicate(columns_->at(where)))
    sql += " LIKE " + prepare_like_string(columns_->at(where)->rand_value());
  else
    sql += " BETWEEN " + columns_->at(where)->rand_value() + " AND " +
           columns_->at(where)->rand_value();

  table_mutex.unlock();
  execute_sql(sql, thd);
}

/* update random row */
void Table::UpdateRandomROW(Thd1 *thd) {
  table_mutex.lock();
  int set;
  while (true) {
    set = rand_int(columns_->size() - 1);
    if (columns_->at(set)->type_ != Column::GENERATED)
      break;
  }

  auto where = -1;
  while (where < 0) {
    auto col_pos = rand_int(columns_->size() - 1);
    switch (columns_->at(col_pos)->type_) {
    case Column::BOOL:
      if (rand_int(1000) < 10)
        where = col_pos;
      break;
    case Column::INT:
    case Column::TIMESTAMP:
    case Column::FLOAT:
    case Column::DOUBLE:
    case Column::VARCHAR:
    case Column::CHAR:
    case Column::BLOB:
    case Column::GENERATED:
      where = col_pos;
      break;
    case Column::JSON:
      if (rand_int(1000) < 50)
        where = col_pos;
      break;
    case Column::INTEGER:
      if (rand_int(1000) < 10)
        where = col_pos;
      break;
    case Column::COLUMN_MAX:
      break;
    }
  }
  std::string sql = "UPDATE " + name_;

  if (type == PARTITION && rand_int(10) < 2)
    sql = "UPDATE " + pg_partition_target(this);

  auto set_value =
      columns_->at(set)->name_.find("fk_col") != std::string::npos
          ? fk_reference_value_expr(this)
          : columns_->at(set)->rand_value();
  sql += " SET " + columns_->at(set)->name_ + " = " + set_value + " WHERE ";

  /* if tables has pkey try to use that in where clause for 50% cases */
  for (size_t i = 0; i < columns_->size(); i++) {
    if (columns_->at(i)->primary_key && rand_int(100) <= 50) {
      where = i;
      break;
    }
  }
  auto prob = rand_int(100);
  if (prob <= 90)
    sql +=
        columns_->at(where)->name_ + " = " + columns_->at(where)->rand_value();
  else if (prob <= 92)
    sql += columns_->at(where)->name_ +
           " >= " + columns_->at(where)->rand_value() + " AND " +
           columns_->at(where)->name_ +
           " >= " + columns_->at(where)->rand_value();
  else if (prob <= 94)
    sql += columns_->at(where)->name_ + " IN (" +
           columns_->at(where)->rand_value() + "," +
           columns_->at(where)->rand_value() + ")";
  else if (prob <= 98)
    sql += columns_->at(where)->name_ + " BETWEEN " +
           columns_->at(where)->rand_value() + " AND " +
           columns_->at(where)->rand_value();
  else if (supports_like_predicate(columns_->at(where)))
    sql += columns_->at(where)->name_ + " LIKE " +
           prepare_like_string(columns_->at(where)->rand_value());
  else
    sql += columns_->at(where)->name_ + " = " +
           columns_->at(where)->rand_value();

  table_mutex.unlock();
  execute_sql(sql, thd);
}

bool Table::InsertBulkRecord(Thd1 *thd) {
  bool is_list_partition = false;

  // if parent has no records, child can't have records
  if (type == FK) {
    if (static_cast<FK_table *>(this)->parent->number_of_initial_records == 0)
      number_of_initial_records = 0;
  }

  if (number_of_initial_records == 0)
    return true;

  std::string prepare_sql = "INSERT ";

  std::vector<int> fk_unique_keys;

  /* If a table has FK move its parent keys in fk_unique_keys */
  if (type == TABLE_TYPES::FK) {
    fk_unique_keys = std::move(thd->unique_keys);
  }
  if (has_pk()) {
    thd->unique_keys = generateUniqueRandomNumbers(number_of_initial_records);
  }

  /* ignore error in the case parition list  */
  if (type == PARTITION &&
      static_cast<Partition *>(this)->part_type == Partition::LIST) {
    is_list_partition = true;
  }

  prepare_sql += "INTO " + name_ + " (";

  assert(number_of_initial_records <=
         (g_integer_range *
          options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt()));

  for (const auto &column : *columns_) {
    prepare_sql += column->name_ + ", ";
  }

  prepare_sql.erase(prepare_sql.length() - 2);
  prepare_sql += ")";

  std::string values = " VALUES";
  int records = 0;

  while (records < number_of_initial_records) {
    std::string value = "(";
    for (const auto &column : *columns_) {
      /* For FK we get the unique value from the parent table unique vector */
      if (column->name_.find("fk_col") != std::string::npos) {
        value +=
            std::to_string(fk_unique_keys[rand_int(fk_unique_keys.size() - 1)]);
      } else if (column->type_ == Column::COLUMN_TYPES::GENERATED) {
        value += "DEFAULT";
      } else if (column->primary_key) {
        value += std::to_string(thd->unique_keys.at(records));
      } else if (column->auto_increment == true) {
        value += "DEFAULT";
      } else if (is_list_partition && column->name_.compare("ip_col") == 0) {
        /* for list partition we insert only maximum possible value
         * todo modify rand_value to return list parititon range */
        value += std::to_string(
            rand_int(maximum_records_in_each_parititon_list *
                     options->at(Option::MAX_PARTITIONS)->getInt()));
      } else {
        value += column->rand_value();
      }

      value += ", ";
    }
    value.erase(value.size() - 2);
    value += ")";
    values += value;
    records++;
    if (values.size() > 1024 * 1024 || number_of_initial_records == records) {
      if (!execute_sql(prepare_sql + values, thd)) {
        ddl_logs_write.lock();
        thd->ddl_logs << "Bulk insert failed for table  " << name_ << std::endl;
        ddl_logs_write.unlock();
        run_query_failed = true;
        return false;
      }
      values = " VALUES";
    } else {
      values += ", ";
    }
  }

  return true;
}

void Table::InsertRandomRow(Thd1 *thd) {
  table_mutex.lock();
  std::string vals = "";
  std::string sql = "INSERT INTO " + name_ + "  ( ";
  std::vector<std::string> column_names;
  std::vector<std::string> value_exprs;
  for (auto &column : *columns_) {
    sql += column->name_ + " ,";
    column_names.push_back(column->name_);
    std::string val;
    if (column->name_.find("fk_col") != std::string::npos)
      val = fk_reference_value_expr(this);
    else if (column->type_ == Column::COLUMN_TYPES::GENERATED)
      val = "default";
    else
      val = column->rand_value();
    if (column->auto_increment == true && rand_int(100) < 10)
      val = "DEFAULT";
    vals += " " + val + ",";
    value_exprs.push_back(val);
  }

  if (vals.size() > 0) {
    vals.pop_back();
    sql.pop_back();
  }
  sql += ") VALUES(" + vals;
  sql += " )";

  if (rand_int(3) != 0) {
    auto pk_columns = primary_key_columns(this);
    if (!pk_columns.empty()) {
      sql += " ON CONFLICT (";
      for (auto *pk_col : pk_columns) {
        sql += pk_col->name_ + ", ";
      }
      sql.erase(sql.length() - 2);
      sql += ")";

      std::vector<std::string> update_assignments;
      for (size_t i = 0; i < columns_->size(); ++i) {
        auto *column = columns_->at(i);
        if (column->primary_key || column->type_ == Column::COLUMN_TYPES::GENERATED)
          continue;
        if (column->auto_increment && value_exprs[i] == "DEFAULT")
          continue;
        update_assignments.push_back(column->name_ + " = EXCLUDED." +
                                     column->name_);
      }

      if (update_assignments.empty()) {
        sql += " DO NOTHING";
      } else {
        sql += " DO UPDATE SET ";
        for (const auto &assignment : update_assignments) {
          sql += assignment + ", ";
        }
        sql.erase(sql.length() - 2);
      }
    }
  }

  table_mutex.unlock();
  execute_sql(sql, thd);
}

/* load special sql from a file */
static std::vector<std::string> load_grammar_sql_from() {
  std::vector<std::string> array;
  auto grammar_file = opt_string(GRAMMAR_FILE);
  std::string sql, file;
  if (grammar_file == "grammar.sql")
    file = std::string(binary_fullpath) + "/" + std::string(grammar_file);
  else
    file = grammar_file;

  std::ifstream myfile(file);
  if (myfile.is_open()) {
    while (!myfile.eof()) {
      getline(myfile, sql);
      /* do not process any blank lines */
      if (sql.find_first_not_of("\t\n ") != std::string::npos)
        array.push_back(sql);
    }
    myfile.close();
  } else
    throw std::runtime_error("unable to open file " + file);
  return array;
}

/* return preformatted sql */
static void grammar_sql(std::vector<Table *> *all_tables, Thd1 *thd) {

  static std::vector<std::string> all_sql = load_grammar_sql_from();
  enum sql_col_types { INT, VARCHAR };

  if (all_sql.size() == 0)
    return;

  struct table {
    table(std::string n, std::vector<std::string> i, std::vector<std::string> v)
        : name(n), int_col(i), varchar_col(v){};
    std::string name;
    std::vector<std::string> int_col;
    std::vector<std::string> varchar_col;
  };

  auto sql = all_sql[rand_int(all_sql.size() - 1)];

  /* parse SQL in table */
  std::vector<std::vector<int>> sql_tables;

  int tab_sql = 1; // number of tables in sql
  bool table_found;

  do { // search for table
    std::smatch match;
    std::string tab_p = "T" + std::to_string(tab_sql); // table pattern

    if (regex_search(sql, match, std::regex(tab_p))) {
      table_found = true;
      sql_tables.push_back({0, 0});

      int col_sql = 1;
      bool column_found;

      do { // search of int column
        std::string col_p = tab_p + "_INT_" + std::to_string(col_sql);
        if (regex_search(sql, match, std::regex(col_p))) {
          column_found = true;
          sql_tables.at(tab_sql - 1).at(INT)++;
          col_sql++;
        } else
          column_found = false;
      } while (column_found);

      col_sql = 1;
      do {
        std::string col_p = tab_p + "_VARCHAR_" + std::to_string(col_sql);
        if (regex_search(sql, match, std::regex(col_p))) {
          column_found = true;
          sql_tables.at(tab_sql - 1).at(VARCHAR)++;
          col_sql++;
        } else
          column_found = false;
      } while (column_found);
    } else
      table_found = false;
    tab_sql++;
  } while (table_found);

  std::vector<table> final_tables;

  /* try at max 100 times */
  int table_check = 100;

  while (sql_tables.size() > 0 && table_check-- > 0) {

    auto int_columns = sql_tables.back().at(INT);
    auto varchar_columns = sql_tables.back().at(VARCHAR);
    std::vector<std::string> int_cols_str, var_cols_str;
    int column_check = 20;
    auto table = all_tables->at(rand_int(all_tables->size() - 1));
    table->table_mutex.lock();
    auto columns = table->columns_;

    // find columns in table //
    do {
      auto col = columns->at(rand_int(columns->size() - 1));

      if (int_columns > 0 && col->type_ == Column::INT) {
        int_cols_str.push_back(col->name_);
        int_columns--;
      }
      if (varchar_columns > 0 && col->type_ == Column::VARCHAR) {
        var_cols_str.push_back(col->name_);
        varchar_columns--;
      }

      if (int_columns == 0 && varchar_columns == 0) {
        final_tables.emplace_back(table->name_, int_cols_str, var_cols_str);
        sql_tables.pop_back();
      }
    } while (!(int_columns == 0 && varchar_columns == 0) && column_check-- > 0);

    table->table_mutex.unlock();
  }

  if (sql_tables.size() == 0) {

    for (size_t i = 0; i < final_tables.size(); i++) {
      auto table = final_tables.at(i);
      auto table_name = "T" + std::to_string(i + 1);

      /* replace int column */
      for (size_t j = 0; j < table.int_col.size(); j++)
        sql = std::regex_replace(
            sql, std::regex(table_name + "_INT_" + std::to_string(j + 1)),
            table_name + "." + table.int_col.at(j));

      /* replace varchar column */
      for (size_t j = 0; j < table.varchar_col.size(); j++)
        sql = std::regex_replace(
            sql, std::regex(table_name + "_VARCHAR_" + std::to_string(j + 1)),
            table_name + "." + table.varchar_col.at(j));

      /* replace table "T1 " => tt_N T1 */
      sql = std::regex_replace(sql, std::regex(table_name + " "),
                               table.name + " " + table_name + " ");
      /* replace table "T1$" => tt_N T1*/
      sql = std::regex_replace(sql, std::regex(table_name + "$"),
                               table.name + " " + table_name + "");
    }

    execute_sql(sql, thd);
  } else
    std::cout << "NOT ABLE TO FIND any SQL in special SQL" << std::endl;
}

/* save metadata to a file */
void save_metadata_to_file() {
  std::string path = opt_string(METADATA_PATH);
  if (path.size() == 0)
    path = opt_string(LOGDIR);
  auto file = path + "/step_" +
              std::to_string(options->at(Option::STEP)->getInt()) + ".dll";
  std::cout << "Saving metadata to file " << file << std::endl;

  StringBuffer sb;
  PrettyWriter<StringBuffer> writer(sb);
  writer.StartObject();
  writer.String("version");
  writer.Uint(version);
  writer.String(("tables"));
  writer.StartArray();
  for (auto j = all_tables->begin(); j != all_tables->end(); j++) {
    auto table = *j;
    table->Serialize(writer);
  }
  writer.EndArray();
  writer.EndObject();
  std::ofstream of(file);
  of << sb.GetString();

  if (!of.good())
    throw std::runtime_error("can't write the JSON string to the file!");
}

/*load objects from a file */
static std::string load_metadata_from_file() {
  auto previous_step = options->at(Option::STEP)->getInt() - 1;
  auto path = opt_string(METADATA_PATH);
  if (path.size() == 0)
    path = opt_string(LOGDIR);
  auto file = path + "/step_" + std::to_string(previous_step) + ".dll";
  FILE *fp = fopen(file.c_str(), "r");

  if (fp == nullptr)
    throw std::runtime_error("unable to open file " + file);

  char readBuffer[65536];
  FileReadStream is(fp, readBuffer, sizeof(readBuffer));
  Document d;
  d.ParseStream(is);
  auto v = d["version"].GetInt();

  if (d["version"].GetInt() != version)
    throw std::runtime_error("version mismatch between " + file +
                             " and codebase " + " file::version is " +
                             std::to_string(v) + " code::version is " +
                             std::to_string(version));

  for (auto &tab : d["tables"].GetArray()) {
    Table *table;
    std::string name = tab["name"].GetString();
    std::string table_type = tab["type"].GetString();

    if (table_type.compare("PARTITION") == 0) {
      std::string part_type = tab["part_type"].GetString();
      table = new Partition(name, part_type, tab["number_of_part"].GetInt());

      if (part_type.compare("RANGE") == 0) {
        for (auto &par_range : tab["part_range"].GetArray()) {
          static_cast<Partition *>(table)->positions.emplace_back(
              par_range[0].GetString(), par_range[1].GetInt());
        }
      } else if (part_type.compare("LIST") == 0) {
        int curr_index_of_list = 0;
        for (auto &par_list : tab["part_list"].GetArray()) {
          static_cast<Partition *>(table)->lists.emplace_back(
              par_list[0].GetString());
          for (auto &list_value : par_list[1].GetArray())
            static_cast<Partition *>(table)
                ->lists.at(curr_index_of_list)
                .list.push_back(list_value.GetInt());
          curr_index_of_list++;
        }
      }
    } else if (table_type.compare("NORMAL") == 0) {
      table = new Table(name);
    } else if (table_type == "FK") {
      std::string on_update = tab["on_update"].GetString();
      std::string on_delete = tab["on_delete"].GetString();
      std::string parent_name = tab["parent"].GetString();

      table = new FK_table(name, on_update, on_delete);
      for (auto &tbl : *all_tables) {
        if (tbl->name_ == parent_name) {
            static_cast<FK_table *>(table)->parent = tbl;
            break;
        }
      }
    } else
      throw std::runtime_error("Unhandle Table type " + table_type);

    table->set_type(table_type);

    table->storage_engine =
        metadata_string_member(tab, "storage_engine", "engine", "default");
    table->storage_layout =
        metadata_string_member(tab, "storage_layout", "row_format", "default");
    table->storage_tablespace = metadata_string_member(
        tab, "storage_tablespace", "tablespace", "file_per_table");

    auto storage_encryption =
        metadata_string_member(tab, "storage_encryption", "encryption");
    if (!storage_encryption.empty())
      table->storage_encryption = storage_encryption;

    auto storage_compression =
        metadata_string_member(tab, "storage_compression", "compression");
    if (!storage_compression.empty())
      table->storage_compression = storage_compression;

    table->storage_block_size =
        metadata_int_member(tab, "storage_block_size", "key_block_size");

    /* save columns */
    for (auto &col : tab["columns"].GetArray()) {
      Column *a;
      std::string type = col["type"].GetString();

      if (type.compare("INT") == 0 || type.compare("CHAR") == 0 ||
          type.compare("VARCHAR") == 0 || type.compare("BOOL") == 0 ||
          type.compare("BOOLEAN") == 0 || type.compare("FLOAT") == 0 ||
          type.compare("REAL") == 0 || type.compare("DOUBLE") == 0 ||
          type.compare("DOUBLE PRECISION") == 0 ||
          type.compare("INTEGER") == 0 || type.compare("TIMESTAMP") == 0 ||
          type.compare("JSON") == 0 || type.compare("JSONB") == 0) {
        a = new Column(col["name"].GetString(), type, table);
      } else if (type.compare("GENERATED") == 0) {
        auto name = col["name"].GetString();
        auto clause = col["clause"].GetString();
        auto sub_type = col["sub_type"].GetString();
        a = new Generated_Column(name, table, clause, sub_type);
      } else if (type.compare("BLOB") == 0 || type.compare("TEXT") == 0) {
        auto sub_type = col["sub_type"].GetString();
        a = new Blob_Column(col["name"].GetString(), table, sub_type);
      } else
        throw std::runtime_error("unhandled column type");

      a->null = col["null"].GetBool();
      a->auto_increment = col["auto_increment"].GetBool();
      a->length = metadata_int_member(col, "length", "lenght");
      a->primary_key = col["primary_key"].GetBool();
      a->compressed = col["compressed"].GetBool();
      table->AddInternalColumn(a);
    }

    for (auto &ind : tab["indexes"].GetArray()) {
      Index *index = new Index(ind["name"].GetString());

      for (auto &ind_col : ind["index_columns"].GetArray()) {
        std::string index_base_column = ind_col["name"].GetString();

        for (auto &column : *table->columns_) {
          if (index_base_column.compare(column->name_) == 0) {
            index->AddInternalColumn(
                new Ind_col(column, ind_col["desc"].GetBool()));
            break;
          }
        }
      }
      table->AddInternalIndex(index);
    }

    all_tables->push_back(table);
    options->at(Option::TABLES)->setInt(all_tables->size());
  }
  fclose(fp);
  return file;
}

/* clean tables from memory,random_strs */
void clean_up_at_end() {
  for (auto &table : *all_tables)
    delete table;
  delete all_tables;
  delete random_strs;
}

/* create new database and tablespace */
void create_database_tablespace(Thd1 *thd) {
  execute_sql("DROP SCHEMA IF EXISTS pstress CASCADE", thd);
  execute_sql("CREATE SCHEMA pstress", thd);
  execute_sql("SET search_path TO pstress", thd);
}

/* check all tables and partition in the starting and if any check table false
 * return false */
static bool check_tables_partitions_preload(Table *table, Thd1 *thd) {
  size_t failures = 0;
  get_check_result("SELECT current_database(), '" +
                       (table->type == Table::PARTITION ? pg_partition_target(table)
                                                        : table->name_) +
                       "', 'check', 'OK'",
                   thd) ||
      failures++;
  if (failures != 0) {
    check_failures++;
  }
  return failures == 0 ? true : false;
}

/* load metadata */
bool Thd1::load_metadata() {
  sum_of_all_opts = sum_of_all_options(this);

  auto seed = opt_int(INITIAL_SEED);
  seed += options->at(Option::STEP)->getInt();
  random_strs = random_strs_generator(seed);

  /*set seed for current step*/
  auto initial_seed = opt_int(INITIAL_SEED);
  initial_seed += options->at(Option::STEP)->getInt();
  rng = std::mt19937(initial_seed);

  if (options->at(Option::STEP)->getInt() > 1 &&
      !options->at(Option::PREPARE)->getBool()) {
    auto file = load_metadata_from_file();
    std::cout << "metadata loaded from " << file << std::endl;
  } else {
    create_database_tablespace(this);
    generate_metadata_for_tables();
    std::cout << "metadata created randomly" << std::endl;
  }

  if (options->at(Option::TABLES)->getInt() <= 0)
    throw std::runtime_error("no table to work on \n");

  return 1;
}

/* return true if successful or error out in case of fail */
bool Thd1::run_some_query() {
  std::vector<Table::TABLE_TYPES> tableTypes = {Table::NORMAL, Table::FK,
                                                Table::PARTITION};
  execute_sql("SET search_path TO pstress", this);

  /* first create temporary tables metadata if requried */
  int temp_tables;
  if (options->at(Option::ONLY_TEMPORARY)->getBool())
    temp_tables = options->at(Option::TABLES)->getInt();
  else if (options->at(Option::NO_TEMPORARY)->getBool())
    temp_tables = 0;
  else
    temp_tables = options->at(Option::TABLES)->getInt() /
                  options->at(Option::TEMPORARY_PROB)->getInt();

  /* create temporary table */
  std::vector<Table *> *all_session_tables = new std::vector<Table *>;
  for (int i = 0; i < temp_tables; i++) {

    Table *table = Table::table_id(Table::TEMPORARY, i);
    if (!table->load(this))
      return false;
    all_session_tables->push_back(table);
  }

  /* prepare is passed, create all tables */
  if (options->at(Option::PREPARE)->getBool() ||
      options->at(Option::STEP)->getInt() == 1) {
    auto current = table_started++;

    while (current <= options->at(Option::TABLES)->getInt()) {
      /* first load normal table , then FK and then partition
       FK table uses thd->unique_key vector to pick random FK
       thd->unique_key is populated from primary key */

      for (const auto &tableType : tableTypes) {
        auto table = pick_table(tableType, current + 1);
        if (table == nullptr)
          continue;
        if (!table->load(this)) {
          return false;
        }
      table_completed++;
      }
      current = table_started++;
    }

    // wait for all tables to finish loading
    while (table_completed < all_tables->size()) {
      thread_log << "Waiting for all threds to finish initial load "
                 << std::endl;
      std::chrono::seconds dura(1);
      if (run_query_failed) {
        thread_log << "Some other thread failed, Exiting. Please check logs "
                   << std::endl;
        return false;
      }
      std::this_thread::sleep_for(dura);
    }
    /* table initial data is created delete , empty the unique_keys */
    this->unique_keys.resize(0);

  } else if (options->at(Option::CHECK_TABLE_PRELOAD)->getBool()) {
    int number_of_tables = all_tables->size();
    auto current = table_started++;

    while (current < number_of_tables) {
      auto table = all_tables->at(current);
      check_tables_partitions_preload(table, this);
      table_completed++;
      current = table_started++;
    }

    // wait for all tables to finish check table
    while (table_completed < all_tables->size()) {
      thread_log << "Waiting for all threds to finish check tables "
                 << std::endl;
      std::chrono::seconds dura(1);
      std::this_thread::sleep_for(dura);
    }
  }

  if (options->at(Option::JUST_LOAD_DDL)->getBool() ||
      options->at(Option::PREPARE)->getBool())
    return true;

  /*Print once on screen and in general logs */
  if (!lock_stream.test_and_set()) {
    std::stringstream s;
    if (check_failures > 0) {
      s << "Check table failed for " << check_failures << " "
        << (check_failures == 1 ? "table" : " tables")
        << ". Check thread logs for details \n ";
    }
    s << "Starting random load in " << options->at(Option::THREADS)->getInt()
      << " threads.\n";
    std::cout << s.str();
    this->ddl_logs << s.str();
  }

  auto sec = opt_int(NUMBER_OF_SECONDS_WORKLOAD);
  auto begin = std::chrono::system_clock::now();
  auto end =
      std::chrono::system_clock::time_point(begin + std::chrono::seconds(sec));

  /* set seed for current thread */
  rng = std::mt19937(set_seed(this));
  thread_log << " value of rand_int(100) " << rand_int(100) << std::endl;

  /* combine session tables with all tables */
  all_session_tables->insert(all_session_tables->end(), all_tables->begin(),
                             all_tables->end());

  /* freqency of all options per thread */
  int opt_feq[Option::MAX][2] = {{0, 0}};

  static auto savepoint_prob = options->at(Option::SAVEPOINT_PRB_K)->getInt();

  int trx_left = 0;
  int current_save_point = 0;
  while (std::chrono::system_clock::now() < end) {


    /* check if we need to make sql as part of existing or new trx */
    if (trx_left > 0) {
      trx_left--;
      if (trx_left == 0) {
        if (rand_int(100, 1) > options->at(Option::COMMIT_PROB)->getInt()) {
          execute_sql("ROLLBACK", this);
        } else {
          execute_sql("COMMIT", this);
        }
        current_save_point = 0;
      } else {
        if (rand_int(1000) < savepoint_prob) {
          current_save_point++;
          execute_sql("SAVEPOINT SAVE" + std::to_string(current_save_point),
                      this);
        }

        /* 10% chances of rollbacking to savepoint */
        if (current_save_point > 0 && rand_int(10) == 1) {
          auto sv = rand_int(current_save_point, 1);
          execute_sql("ROLLBACK TO SAVEPOINT SAVE" + std::to_string(sv), this);
          current_save_point = sv - 1;
        }
      }
    }

    if (trx_left == 0 &&
        rand_int(1000) < options->at(Option::TRANSATION_PRB_K)->getInt()) {
      execute_sql("START TRANSACTION", this);
      trx_left = rand_int(options->at(Option::TRANSACTIONS_SIZE)->getInt(), 1);
    }

    auto table =
        all_session_tables->at(rand_int(all_session_tables->size() - 1));
    auto option = pick_some_option();
    ddl_query = options->at(option)->ddl == true ? true : false;

    // PostgreSQL DDL is transactional. Keep schema changes outside random
    // transactions so the in-memory schema model cannot outlive a rollback.
    if (ddl_query && trx_left > 0) {
      execute_sql("COMMIT", this);
      trx_left = 0;
      current_save_point = 0;
    }

    switch (option) {
    case Option::DROP_INDEX:
      table->DropIndex(this);
      break;
    case Option::ADD_INDEX:
      table->AddIndex(this);
      break;
    case Option::DROP_COLUMN:
      table->DropColumn(this);
      break;
    case Option::ADD_COLUMN:
      table->AddColumn(this);
      break;
    case Option::TRUNCATE:
      table->Truncate(this);
      break;
    case Option::DROP_CREATE:
      table->DropCreate(this);
      break;
    case Option::ALTER_COLUMN_MODIFY:
      table->ModifyColumn(this);
      break;
    case Option::SELECT_ALL_ROW:
      table->SelectAllRow(this);
      break;
    case Option::SELECT_ROW_USING_PKEY:
      table->SelectRandomRow(this);
      break;
    case Option::INSERT_RANDOM_ROW:
      table->InsertRandomRow(this);
      break;
    case Option::DELETE_ALL_ROW:
      table->DeleteAllRows(this);
      break;
    case Option::DELETE_ROW_USING_PKEY:
      table->DeleteRandomRow(this);
      break;
    case Option::UPDATE_ROW_USING_PKEY:
      table->UpdateRandomROW(this);
      break;
    case Option::OPTIMIZE:
      table->Optimize(this);
      break;
    case Option::CHECK_TABLE:
      table->Check(this);
      break;
    case Option::ADD_DROP_PARTITION:
      if (table->type == Table::PARTITION)
        static_cast<Partition *>(table)->AddDrop(this);
      break;
    case Option::ANALYZE:
      table->Analyze(this);
      break;
    case Option::RENAME_COLUMN:
      table->ColumnRename(this);
      break;
    case Option::RENAME_INDEX:
      table->IndexRename(this);
      break;
    case Option::GRAMMAR_SQL:
      grammar_sql(all_session_tables, this);
      break;

    default:
      throw std::runtime_error("invalid options");
    }

    options->at(option)->total_queries++;

    /* sql executed is at 0 index, and if successful at 1 */
    opt_feq[option][0]++;
    if (success) {
      options->at(option)->success_queries++;
      opt_feq[option][1]++;
      success = false;
    }

    if (run_query_failed) {
      break;
    }
  } // while

  /* print options frequency in logs */
  for (int i = 0; i < Option::MAX; i++) {
    if (opt_feq[i][0] > 0)
      thread_log << options->at(i)->help << ", total=>" << opt_feq[i][0]
                 << ", success=> " << opt_feq[i][1] << std::endl;
  }

  /* cleanup session temporary tables tables */
  for (auto &table : *all_session_tables)
    if (table->type == Table::TEMPORARY)
      delete table;
  delete all_session_tables;
  return true;
}
