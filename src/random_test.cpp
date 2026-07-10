/*
 =========================================================
 #       Created by Rahul Malik, Percona LLC             #
 =========================================================
*/
#include "random_test.hpp"
#include "common.hpp"
#include "node.hpp"
#include <array>
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
const std::string UNLOGGED_SUFFIX = "_u";
const int version = 2;
/* range for random number int, integers, floats and double.
 more the value, less randomness.
 for example if it is 1. then there is very high chance of executing
 successful DML.
todo allow this option to be configured by user */
const int g_integer_range = 100;
const int k_hit_cache_limit = 64;
const int k_hit_where_probability = 70;
const size_t k_generated_columns_hard_cap = 8;
const size_t k_generated_min_base_columns = 4;
const size_t k_generated_dependency_soft_cap = 4;
const size_t k_generated_dependency_hard_cap = 8;
const int k_generated_text_budget_min = 32;
const int k_generated_text_budget_max = 128;

static std::vector<Table *> *all_tables = new std::vector<Table *>;
static std::vector<std::string> locks;
static std::vector<std::string> algorithms;
static int g_max_columns_length = 30;
static int sum_of_all_opts = 0; // sum of all probablility
static int g_server_version_num = 0;
std::mutex ddl_logs_write;
static std::chrono::system_clock::time_point start_time =
    std::chrono::system_clock::now();

std::atomic<int> table_started(0);
std::atomic<size_t> check_failures(0);
std::atomic<size_t> table_completed(0);
std::atomic_flag lock_stream = ATOMIC_FLAG_INIT;
std::atomic<bool> run_query_failed(false);
std::mutex ddl_workload_mutex;
std::atomic<unsigned long long> trx_ddl_table_seq(0);
static constexpr size_t k_transactional_ddl_existing_column_cap = 256;
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

static bool is_list_partition_key_column(const Table *table,
                                         const Column *column) {
  if (table->type != Table::PARTITION || table->columns_->empty()) {
    return false;
  }

  auto *part = static_cast<const Partition *>(table);
  return part->part_type == Partition::LIST &&
         table->columns_->front() == column;
}

static bool is_partition_key_column(const Table *table, const Column *column) {
  return table->type == Table::PARTITION && !table->columns_->empty() &&
         table->columns_->front() == column;
}

static std::string partition_key_column_name(const Table *table) {
  return table->columns_->front()->name_;
}

static bool list_partition_key_value_expr(Table *table, Column *column,
                                          std::string &value_expr) {
  if (!is_list_partition_key_column(table, column)) {
    return false;
  }

  auto *part = static_cast<Partition *>(table);
  if (part->lists.empty()) {
    return false;
  }

  const auto &partition_values =
      part->lists.at(rand_int(part->lists.size() - 1)).list;
  if (partition_values.empty()) {
    return false;
  }

  value_expr =
      std::to_string(partition_values.at(rand_int(partition_values.size() - 1)));
  return true;
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
	  case Column::SMALLINT:
	  case Column::INTEGER:
	  case Column::INT:
	  case Column::BIGINT:
	  case Column::NUMERIC:
	  case Column::FLOAT:
	  case Column::DOUBLE:
	  case Column::DATE:
	  case Column::TIME:
	  case Column::TIMETZ:
	  case Column::TIMESTAMP:
    case Column::TIMESTAMPTZ:
    case Column::INTERVAL:
    case Column::BIT:
    case Column::VARBIT:
    case Column::INET:
    case Column::CIDR:
    case Column::MACADDR:
    case Column::MACADDR8:
    case Column::MONEY:
    case Column::XML:
    case Column::TSVECTOR:
    case Column::TSQUERY:
    case Column::POINT:
    case Column::LINE:
    case Column::LSEG:
    case Column::BOX:
    case Column::PATH:
    case Column::POLYGON:
    case Column::CIRCLE:
    case Column::INTARRAY:
    case Column::BIGINTARRAY:
    case Column::NUMERICARRAY:
    case Column::TEXTARRAY:
    case Column::BOOLARRAY:
    case Column::TIMESTAMPARRAY:
    case Column::INT4RANGE:
    case Column::INT8RANGE:
    case Column::NUMRANGE:
    case Column::TSRANGE:
    case Column::TSTZRANGE:
    case Column::DATERANGE:
    case Column::BOOL:
	  case Column::BYTEA:
	  case Column::JSON:
	  case Column::JSONB:
	  case Column::UUID:
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
  case Column::SMALLINT:
    return 2;
  case Column::INT:
  case Column::INTEGER:
  case Column::BIGINT:
  case Column::FLOAT:
  case Column::DOUBLE:
  case Column::DATE:
  case Column::TIME:
  case Column::TIMETZ:
  case Column::TIMESTAMP:
  case Column::TIMESTAMPTZ:
  case Column::INTERVAL:
  case Column::NUMERIC:
  case Column::UUID:
  case Column::INET:
  case Column::CIDR:
  case Column::MACADDR:
  case Column::MACADDR8:
  case Column::MONEY:
    return 8;
  case Column::POINT:
  case Column::LINE:
  case Column::LSEG:
  case Column::BOX:
  case Column::PATH:
  case Column::POLYGON:
  case Column::CIRCLE:
  case Column::INTARRAY:
  case Column::BIGINTARRAY:
  case Column::NUMERICARRAY:
  case Column::TEXTARRAY:
  case Column::BOOLARRAY:
  case Column::TIMESTAMPARRAY:
  case Column::INT4RANGE:
  case Column::INT8RANGE:
  case Column::NUMRANGE:
  case Column::TSRANGE:
  case Column::TSTZRANGE:
  case Column::DATERANGE:
    return 128;
  case Column::BIT:
  case Column::VARBIT:
    return std::max(1, std::min(column->length, 64));
  case Column::CHAR:
  case Column::VARCHAR:
    return std::max(1, std::min(column->length, 64));
  case Column::BLOB:
  case Column::BYTEA:
    return 128;
  case Column::JSON:
  case Column::JSONB:
  case Column::XML:
  case Column::TSVECTOR:
  case Column::TSQUERY:
    return 128;
  case Column::GENERATED: {
    auto generated =
        static_cast<const Generated_Column *>(column)->generate_type();
    switch (generated) {
    case Column::BOOL:
      return 1;
    case Column::SMALLINT:
      return 2;
    case Column::INT:
    case Column::INTEGER:
    case Column::BIGINT:
    case Column::FLOAT:
    case Column::DOUBLE:
    case Column::DATE:
    case Column::TIME:
    case Column::TIMETZ:
    case Column::TIMESTAMP:
    case Column::TIMESTAMPTZ:
    case Column::INTERVAL:
    case Column::NUMERIC:
    case Column::UUID:
    case Column::INET:
    case Column::CIDR:
    case Column::MACADDR:
    case Column::MACADDR8:
    case Column::MONEY:
      return 8;
    case Column::POINT:
    case Column::LINE:
    case Column::LSEG:
    case Column::BOX:
    case Column::PATH:
    case Column::POLYGON:
    case Column::CIRCLE:
    case Column::INTARRAY:
    case Column::BIGINTARRAY:
    case Column::NUMERICARRAY:
    case Column::TEXTARRAY:
    case Column::BOOLARRAY:
    case Column::TIMESTAMPARRAY:
    case Column::INT4RANGE:
    case Column::INT8RANGE:
    case Column::NUMRANGE:
    case Column::TSRANGE:
    case Column::TSTZRANGE:
    case Column::DATERANGE:
      return 128;
    case Column::BIT:
    case Column::VARBIT:
      return std::max(1, std::min(column->length, 64));
    case Column::CHAR:
    case Column::VARCHAR:
      return std::max(1, std::min(column->length, 64));
    case Column::BLOB:
    case Column::BYTEA:
    case Column::JSON:
    case Column::JSONB:
    case Column::XML:
    case Column::TSVECTOR:
    case Column::TSQUERY:
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

static bool fk_supporting_index(const Index *index) {
  return index != nullptr && index->unique && index->columns_->size() == 1 &&
         index->columns_->front()->column->referenced_key;
}

static bool pg_indexable_column(const Column *column) {
  if (column != nullptr && column->type_ == Column::GENERATED &&
      static_cast<const Generated_Column *>(column)->generated_kind ==
          Generated_Column::VIRTUAL)
    return false;
  return pg_index_width_estimate(column) <= 64;
}

static bool pg_fk_referenceable_column(const Column *column) {
  if (column == nullptr || column->type_ == Column::GENERATED ||
      column->type_ == Column::BLOB || column->type_ == Column::BYTEA ||
      column->type_ == Column::JSON || column->type_ == Column::JSONB ||
      column->type_ == Column::XML || column->type_ == Column::TSVECTOR ||
      column->type_ == Column::TSQUERY ||
      column->type_ == Column::POINT || column->type_ == Column::LINE ||
      column->type_ == Column::LSEG || column->type_ == Column::BOX ||
      column->type_ == Column::PATH || column->type_ == Column::POLYGON ||
      column->type_ == Column::CIRCLE ||
      column->type_ == Column::INTARRAY ||
      column->type_ == Column::BIGINTARRAY ||
      column->type_ == Column::NUMERICARRAY ||
      column->type_ == Column::TEXTARRAY ||
      column->type_ == Column::BOOLARRAY ||
      column->type_ == Column::TIMESTAMPARRAY ||
      column->type_ == Column::INT4RANGE ||
      column->type_ == Column::INT8RANGE ||
      column->type_ == Column::NUMRANGE ||
      column->type_ == Column::TSRANGE ||
      column->type_ == Column::TSTZRANGE ||
      column->type_ == Column::DATERANGE ||
      column->type_ == Column::BIT || column->type_ == Column::VARBIT ||
      column->type_ == Column::INTERVAL ||
      column->type_ == Column::BOOL) {
    return false;
  }
  if ((column->type_ == Column::CHAR || column->type_ == Column::VARCHAR) &&
      column->length < 8) {
    return false;
  }
  return pg_indexable_column(column);
}

static int pg_index_total_width(const Index *index) {
  int width = 0;
  for (auto *ind_col : *index->columns_) {
    width += pg_index_width_estimate(ind_col->column);
  }
  return width;
}

static bool pg_generated_source_column(const Column *column) {
  switch (column->type_) {
  case Column::GENERATED:
  case Column::BIT:
  case Column::VARBIT:
  case Column::BYTEA:
  case Column::BLOB:
  case Column::JSON:
  case Column::JSONB:
  case Column::TIMETZ:
  case Column::TIMESTAMPTZ:
  case Column::MONEY:
  case Column::XML:
  case Column::TSVECTOR:
  case Column::TSQUERY:
  case Column::POINT:
  case Column::LINE:
  case Column::LSEG:
  case Column::BOX:
  case Column::PATH:
  case Column::POLYGON:
  case Column::CIRCLE:
  case Column::INTARRAY:
  case Column::BIGINTARRAY:
  case Column::NUMERICARRAY:
  case Column::TEXTARRAY:
  case Column::BOOLARRAY:
  case Column::TIMESTAMPARRAY:
  case Column::INT4RANGE:
  case Column::INT8RANGE:
  case Column::NUMRANGE:
  case Column::TSRANGE:
  case Column::TSTZRANGE:
  case Column::DATERANGE:
    return false;
  case Column::COLUMN_MAX:
    return false;
  default:
    return !column->auto_increment;
  }
}

static size_t count_generated_columns(const Table *table) {
  size_t count = 0;
  for (const auto *column : *table->columns_) {
    if (column->type_ == Column::GENERATED) {
      ++count;
    }
  }
  return count;
}

static std::vector<size_t> generated_source_positions(const Table *table) {
  std::vector<size_t> positions;
  for (size_t i = 0; i < table->columns_->size(); ++i) {
    if (pg_generated_source_column(table->columns_->at(i))) {
      positions.push_back(i);
    }
  }
  return positions;
}

static size_t generated_column_budget(size_t total_columns) {
  if (total_columns < k_generated_min_base_columns + 2) {
    return 0;
  }
  return std::min(k_generated_columns_hard_cap,
                  std::max<size_t>(1, total_columns / 40));
}

static bool generated_column_allowed(const Table *table, size_t total_columns_budget) {
  return count_generated_columns(table) < generated_column_budget(total_columns_budget) &&
         generated_source_positions(table).size() >= k_generated_min_base_columns;
}

static Column::COLUMN_TYPES fallback_generated_column_type() {
  std::vector<Column::COLUMN_TYPES> types = {
      Column::INT, Column::BIGINT, Column::NUMERIC, Column::VARCHAR, Column::CHAR};
  if (!options->at(Option::NO_BLOB)->getBool()) {
    types.push_back(Column::BLOB);
  }
  return types.at(rand_int(types.size() - 1));
}

static Column *make_generated_or_fallback_column(const std::string &name, Table *table,
                                                 size_t total_columns_budget,
                                                 bool &created_generated) {
  created_generated = false;
  if (generated_column_allowed(table, total_columns_budget)) {
    for (int attempt = 0; attempt < 8; ++attempt) {
      try {
        auto *generated = new Generated_Column(name, table);
        created_generated = true;
        return generated;
      } catch (const std::exception &) {
      }
    }
  }

  auto fallback_type = fallback_generated_column_type();
  if (fallback_type == Column::BLOB) {
    return new Blob_Column(name, table);
  }
  return new Column(name, table, fallback_type);
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
  } else if (type == Table::UNLOGGED) {
    name += UNLOGGED_SUFFIX;
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

static Column *clone_column_for_table(const Column *column, Table *owner) {
  Column *copy = nullptr;
  auto type_name = Column::col_type_to_string(column->type_);

  if (column->type_ == Column::GENERATED) {
    const auto *generated = static_cast<const Generated_Column *>(column);
    copy = new Generated_Column(column->name_, owner, generated->str,
                                Column::col_type_to_string(generated->generate_type()),
                                generated->generated_kind_string());
  } else if (column->type_ == Column::BLOB) {
    const auto *blob = static_cast<const Blob_Column *>(column);
    copy = new Blob_Column(column->name_, owner, blob->sub_type);
  } else {
    copy = new Column(column->name_, type_name, owner);
  }

  copy->null = column->null;
  copy->length = column->length;
  copy->default_value = column->default_value;
  copy->primary_key = column->primary_key;
  copy->auto_increment = column->auto_increment;
  copy->referenced_key = column->referenced_key;
  copy->compressed = column->compressed;
  copy->unique_values = column->unique_values;
  return copy;
}

static Index *clone_index_for_table(const Index *index, Table *owner) {
  auto *copy = new Index(index->name_);
  copy->unique = index->unique;
  for (auto *ind_col : *index->columns_) {
    Column *column = nullptr;
    for (auto *candidate : *owner->columns_) {
      if (candidate->name_ == ind_col->column->name_) {
        column = candidate;
        break;
      }
    }
    if (column == nullptr) {
      continue;
    }
    auto *copy_col = new Ind_col(column, ind_col->desc);
    copy_col->length = ind_col->length;
    copy->AddInternalColumn(copy_col);
  }
  return copy;
}

static Table *clone_table_metadata(const Table *table) {
  Table *copy = nullptr;
  switch (table->type) {
  case Table::NORMAL:
    copy = new Table(table->name_);
    break;
  case Table::TEMPORARY:
    copy = new Temporary_table(table->name_);
    break;
  case Table::UNLOGGED:
    copy = new Table(table->name_);
    break;
  default:
    throw std::runtime_error("transactional ddl clone only supports normal/temporary/unlogged tables");
  }

  copy->type = table->type;
  copy->storage_engine = table->storage_engine;
  copy->storage_layout = table->storage_layout;
  copy->storage_tablespace = table->storage_tablespace;
  copy->storage_compression = table->storage_compression;
  copy->storage_encryption = table->storage_encryption;
  copy->storage_block_size = table->storage_block_size;
  copy->number_of_initial_records = table->number_of_initial_records;
  copy->auto_inc_index = table->auto_inc_index;

  for (auto *column : *table->columns_) {
    copy->AddInternalColumn(clone_column_for_table(column, copy));
  }
  for (auto *index : *table->indexes_) {
    copy->AddInternalIndex(clone_index_for_table(index, copy));
  }

  {
    std::lock_guard<std::mutex> guard(
        const_cast<std::mutex &>(table->hit_value_mutex));
    copy->hit_value_cache = table->hit_value_cache;
  }
  return copy;
}

static void clear_table_metadata(Table *table) {
  for (auto *index : *table->indexes_) {
    delete index;
  }
  table->indexes_->clear();

  for (auto *column : *table->columns_) {
    delete column;
  }
  table->columns_->clear();
}

static void restore_table_metadata(Table *target, const Table *snapshot) {
  std::lock_guard<std::recursive_mutex> guard(target->table_mutex);
  clear_table_metadata(target);

  target->name_ = snapshot->name_;
  target->type = snapshot->type;
  target->storage_engine = snapshot->storage_engine;
  target->storage_layout = snapshot->storage_layout;
  target->storage_tablespace = snapshot->storage_tablespace;
  target->storage_compression = snapshot->storage_compression;
  target->storage_encryption = snapshot->storage_encryption;
  target->storage_block_size = snapshot->storage_block_size;
  target->number_of_initial_records = snapshot->number_of_initial_records;
  target->auto_inc_index = snapshot->auto_inc_index;

  for (auto *column : *snapshot->columns_) {
    target->AddInternalColumn(clone_column_for_table(column, target));
  }
  for (auto *index : *snapshot->indexes_) {
    target->AddInternalIndex(clone_index_for_table(index, target));
  }

  {
    std::lock_guard<std::mutex> guard_cache(target->hit_value_mutex);
    target->hit_value_cache = snapshot->hit_value_cache;
  }
}

static void delete_table_list(std::vector<Table *> &tables) {
  for (auto *table : tables) {
    delete table;
  }
  tables.clear();
}

static std::vector<Table *> clone_table_list(const std::vector<Table *> &tables) {
  std::vector<Table *> copies;
  copies.reserve(tables.size());
  for (auto *table : tables) {
    copies.push_back(clone_table_metadata(table));
  }
  return copies;
}

static Column::COLUMN_TYPES random_trx_ddl_scratch_column_type() {
  static const std::vector<Column::COLUMN_TYPES> types = {
      Column::INT,      Column::BIGINT,  Column::NUMERIC, Column::VARCHAR,
      Column::CHAR,     Column::BOOL,    Column::DATE,    Column::TIME,
      Column::TIMESTAMP, Column::INET,   Column::CIDR,    Column::UUID};
  return types.at(rand_int(types.size() - 1));
}

static bool run_transactional_safe_add_column(Table *table, Thd1 *thd) {
  table->table_mutex.lock();
  Column *column = nullptr;
  for (int attempt = 0; attempt < 64 && column == nullptr; ++attempt) {
    auto type = random_trx_ddl_scratch_column_type();
    auto name = "tdN" + std::to_string(rand_int(100000, 1000)) + "_" +
                std::to_string(attempt);
    auto *candidate = new Column(name, table, type);
    if (column_name_exists(table, candidate->name_)) {
      delete candidate;
      continue;
    }
    column = candidate;
  }

  if (column == nullptr) {
    table->table_mutex.unlock();
    return false;
  }

  auto sql = "ALTER TABLE " + table->name_ + " ADD COLUMN " + column->definition();
  table->table_mutex.unlock();

  if (execute_sql(sql, thd)) {
    table->table_mutex.lock();
    if (!column_name_exists(table, column->name_)) {
      table->AddInternalColumn(column);
    } else {
      delete column;
    }
    table->table_mutex.unlock();
    return true;
  }

  delete column;
  return false;
}

static Table *build_trx_ddl_scratch_table(Thd1 *thd) {
  auto id = ++trx_ddl_table_seq;
  auto *table = new Table("trxddl_t" + std::to_string(thd->thread_id) + "_" +
                          std::to_string(id));
  table->type = Table::NORMAL;
  table->number_of_initial_records = 0;

  auto *pk = new Column("pkey", table, Column::INT);
  pk->primary_key = true;
  pk->auto_increment = true;
  table->AddInternalColumn(pk);

  int extra_columns = rand_int(5, 2);
  for (int i = 0; i < extra_columns; ++i) {
    auto type = random_trx_ddl_scratch_column_type();
    auto name = "trx_" + std::to_string(i);
    Column *column = nullptr;
    if (type == Column::BLOB) {
      column = new Blob_Column(name, table);
    } else {
      column = new Column(name, table, type);
    }
    table->AddInternalColumn(column);
  }

  return table;
}

static Table *pick_transactional_ddl_target_table() {
  std::vector<Table *> candidates;
  for (auto *table : *all_tables) {
    if (table->type != Table::NORMAL || referenced_by_fk(table)) {
      continue;
    }
    candidates.push_back(table);
  }
  if (candidates.empty()) {
    return nullptr;
  }
  return candidates.at(rand_int(candidates.size() - 1));
}

static bool transactional_ddl_enabled() {
  return options->at(Option::TRX_DDL_PROB_K)->getInt() > 0 &&
         options->at(Option::TRX_DDL_SIZE)->getInt() > 0 &&
         !options->at(Option::ONLY_CL_SQL)->getBool() &&
         !options->at(Option::ONLY_CL_DDL)->getBool();
}

enum class TransactionalDDLOp {
  CREATE_TABLE,
  DROP_TABLE,
  ADD_COLUMN,
  DROP_COLUMN,
  ADD_INDEX,
  DROP_INDEX,
  RENAME_COLUMN,
  RENAME_INDEX
};

static bool run_transactional_ddl_on_table(Table *table, Thd1 *thd) {
  std::vector<TransactionalDDLOp> ops = {
      TransactionalDDLOp::ADD_INDEX,     TransactionalDDLOp::DROP_INDEX,
      TransactionalDDLOp::RENAME_COLUMN, TransactionalDDLOp::RENAME_INDEX};
  if (table->columns_->size() < k_transactional_ddl_existing_column_cap) {
    ops.push_back(TransactionalDDLOp::ADD_COLUMN);
  }
  auto op = ops.at(rand_int(ops.size() - 1));
  switch (op) {
  case TransactionalDDLOp::ADD_COLUMN:
    return run_transactional_safe_add_column(table, thd);
    break;
  case TransactionalDDLOp::ADD_INDEX:
    table->AddIndex(thd);
    break;
  case TransactionalDDLOp::DROP_INDEX:
    table->DropIndex(thd);
    break;
  case TransactionalDDLOp::RENAME_COLUMN:
    table->ColumnRename(thd);
    break;
  case TransactionalDDLOp::RENAME_INDEX:
    table->IndexRename(thd);
    break;
  default:
    return false;
  }
  return thd->success;
}

static bool run_transactional_ddl_on_scratch(
    Thd1 *thd, std::vector<Table *> &trx_ddl_tables) {
  std::vector<TransactionalDDLOp> ops = {
      TransactionalDDLOp::CREATE_TABLE,  TransactionalDDLOp::ADD_COLUMN,
      TransactionalDDLOp::DROP_COLUMN,   TransactionalDDLOp::ADD_INDEX,
      TransactionalDDLOp::DROP_INDEX,    TransactionalDDLOp::RENAME_COLUMN,
      TransactionalDDLOp::RENAME_INDEX,  TransactionalDDLOp::DROP_TABLE};

  if (trx_ddl_tables.empty()) {
    ops = {TransactionalDDLOp::CREATE_TABLE};
  }

  auto op = ops.at(rand_int(ops.size() - 1));
  if (op == TransactionalDDLOp::CREATE_TABLE) {
    auto *table = build_trx_ddl_scratch_table(thd);
    bool success = execute_sql(table->definition(false), thd);
    if (success) {
      trx_ddl_tables.push_back(table);
    } else {
      delete table;
    }
    return success;
  }

  if (trx_ddl_tables.empty()) {
    return false;
  }

  auto pos = rand_int(trx_ddl_tables.size() - 1);
  auto *table = trx_ddl_tables.at(pos);
  switch (op) {
  case TransactionalDDLOp::DROP_TABLE: {
    if (!execute_sql("DROP TABLE " + table->name_, thd)) {
      return false;
    }
    delete table;
    trx_ddl_tables.erase(trx_ddl_tables.begin() + pos);
    return true;
  }
  case TransactionalDDLOp::ADD_COLUMN:
    return run_transactional_safe_add_column(table, thd);
  case TransactionalDDLOp::DROP_COLUMN:
    table->DropColumn(thd);
    break;
  case TransactionalDDLOp::ADD_INDEX:
    table->AddIndex(thd);
    break;
  case TransactionalDDLOp::DROP_INDEX:
    table->DropIndex(thd);
    break;
  case TransactionalDDLOp::RENAME_COLUMN:
    table->ColumnRename(thd);
    break;
  case TransactionalDDLOp::RENAME_INDEX:
    table->IndexRename(thd);
    break;
  default:
    return false;
  }
  return thd->success;
}

static bool run_transactional_ddl_block(Thd1 *thd,
                                        std::vector<Table *> &trx_ddl_tables) {
  auto max_size = options->at(Option::TRX_DDL_SIZE)->getInt();
  if (max_size <= 0) {
    return false;
  }

  auto scratch_snapshot = clone_table_list(trx_ddl_tables);
  Table *target = nullptr;
  Table *target_snapshot = nullptr;
  bool scratch_mode = trx_ddl_tables.empty() || rand_int(1) == 0;
  if (!scratch_mode) {
    target = pick_transactional_ddl_target_table();
    if (target == nullptr) {
      scratch_mode = true;
    }
  }

  if (scratch_mode == false && target != nullptr) {
    target->table_mutex.lock();
    target_snapshot = clone_table_metadata(target);
  }

  bool committed = false;
  bool started = false;
  bool rolled_back = false;
  int statements = 0;
  thd->ddl_query = true;

  if (!execute_sql("START TRANSACTION", thd)) {
    goto cleanup;
  }
  started = true;

  statements = rand_int(max_size, 1);
  for (int i = 0; i < statements; ++i) {
    bool success = scratch_mode ? run_transactional_ddl_on_scratch(thd, trx_ddl_tables)
                                : run_transactional_ddl_on_table(target, thd);
    if (!success) {
      execute_sql("ROLLBACK", thd);
      rolled_back = true;
      goto cleanup;
    }
  }

  if (!execute_sql("COMMIT", thd)) {
    goto cleanup;
  }
  committed = true;

cleanup:
  if (!committed && started && !rolled_back) {
    execute_sql("ROLLBACK", thd);
  }

  if (!committed) {
    delete_table_list(trx_ddl_tables);
    trx_ddl_tables = std::move(scratch_snapshot);
    if (target != nullptr && target_snapshot != nullptr) {
      restore_table_metadata(target, target_snapshot);
    }
  } else {
    delete_table_list(scratch_snapshot);
  }

  if (target_snapshot != nullptr) {
    delete target_snapshot;
  }
  if (target != nullptr) {
    target->table_mutex.unlock();
  }

  thd->ddl_query = false;
  return committed;
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

static bool pg_server_at_least(Thd1 *thd, int major) {
  return thd != nullptr && thd->conn != nullptr &&
         PQserverVersion(thd->conn) >= major * 10000;
}

static bool current_server_at_least(int major) {
  return g_server_version_num >= major * 10000;
}

static std::string normalized_generated_column_kind() {
  auto kind = opt_string(GENERATED_COLUMN_KIND);
  std::transform(kind.begin(), kind.end(), kind.begin(), ::tolower);
  if (kind != "random" && kind != "virtual" && kind != "stored")
    throw std::runtime_error(
        "invalid --generated-column-kind. Choose random, virtual, or stored");
  if (!current_server_at_least(18))
    return "stored";
  if (kind == "random")
    return rand_int(1) == 0 ? "virtual" : "stored";
  return kind;
}

static bool use_returning_old_new() {
  auto probability = opt_int(RETURNING_OLD_NEW);
  return probability > 0 && rand_int(100) < probability;
}

/* return probabality of all options and disable some feature based on user
 * request/ branch/ fork */
int sum_of_all_options(Thd1 *thd) {
  g_server_version_num =
      thd != nullptr && thd->conn != nullptr ? PQserverVersion(thd->conn) : 0;
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

  if (!pg_server_at_least(thd, 18)) {
    options->at(Option::RETURNING_OLD_NEW)->setInt(0);
    options->at(Option::PG18_EXPLAIN)->setInt(0);
    options->at(Option::PG18_FUNCTIONS)->setInt(0);
  }

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

  if (options->at(Option::ONLY_UNLOGGED)->getBool() &&
      options->at(Option::ONLY_TEMPORARY)->getBool())
    throw std::runtime_error("choose either only unlogged or only temporary ");

  if (options->at(Option::ONLY_UNLOGGED)->getBool() &&
      options->at(Option::ONLY_PARTITION)->getBool())
    throw std::runtime_error("choose either only unlogged or only partition ");

  if (options->at(Option::ONLY_PARTITION)->getBool() &&
      options->at(Option::NO_PARTITION)->getBool())
    throw std::runtime_error("choose either only partition or no partition");

  if (options->at(Option::ONLY_UNLOGGED)->getBool() &&
      options->at(Option::NO_UNLOGGED)->getBool())
    throw std::runtime_error("choose either only unlogged or no unlogged");

  if (options->at(Option::ONLY_PARTITION)->getBool()) {
    options->at(Option::NO_TEMPORARY)->setBool("true");
    options->at(Option::NO_UNLOGGED)->setBool("true");
    options->at(Option::PARTITION_PROB)->setInt(100);
  }

  if (options->at(Option::ONLY_TEMPORARY)->getBool()) {
    options->at(Option::NO_PARTITION)->setBool("true");
    options->at(Option::NO_UNLOGGED)->setBool("true");
    options->at(Option::TEMPORARY_PROB)->setInt(100);
  }

  if (options->at(Option::ONLY_UNLOGGED)->getBool()) {
    options->at(Option::NO_FK)->setBool("true");
    options->at(Option::NO_PARTITION)->setBool("true");
    options->at(Option::NO_TEMPORARY)->setBool("true");
    options->at(Option::UNLOGGED_PROB)->setInt(100);
  }

  /* if select is set as zero, disable all type of selects */
  if (options->at(Option::NO_SELECT)->getBool()) {
    options->at(Option::SELECT_ALL_ROW)->setInt(0);
    options->at(Option::SELECT_ROW_USING_PKEY)->setInt(0);
    options->at(Option::SELECT_WITH_JOIN)->setInt(0);
    options->at(Option::SELECT_WITH_CTE)->setInt(0);
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

static std::string rand_json_text_value() {
  return "'{\"k\":" + std::to_string(rand_int(100000)) + ",\"s\":\"" +
         rand_string(12, 3) + "\",\"b\":" + (rand_int(1) == 1 ? "true" : "false") +
         "}'::json";
}

static std::string rand_date_value() {
  int year = rand_int(2040, 2000);
  int month = rand_int(12, 1);
  int day = rand_int(28, 1);
  std::ostringstream out;
  out << "DATE '" << year << "-" << std::setw(2) << std::setfill('0') << month
      << "-" << std::setw(2) << std::setfill('0') << day << "'";
  return out.str();
}

static std::string rand_time_value() {
  int hour = rand_int(23, 0);
  int minute = rand_int(59, 0);
  int second = rand_int(59, 0);
  std::ostringstream out;
  out << "TIME '" << std::setw(2) << std::setfill('0') << hour << ":"
      << std::setw(2) << std::setfill('0') << minute << ":" << std::setw(2)
      << std::setfill('0') << second << "'";
  return out.str();
}

static std::string rand_timetz_value() {
  int hour = rand_int(23, 0);
  int minute = rand_int(59, 0);
  int second = rand_int(59, 0);
  int offset = rand_int(12, -12);
  std::ostringstream out;
  out << "TIME WITH TIME ZONE '" << std::setw(2) << std::setfill('0') << hour
      << ":" << std::setw(2) << std::setfill('0') << minute << ":"
      << std::setw(2) << std::setfill('0') << second
      << (offset >= 0 ? "+" : "") << offset << "'";
  return out.str();
}

static std::string rand_timestamptz_value() {
  return rand_timestamp_value() + "::timestamptz";
}

static std::string rand_interval_value() {
  return "INTERVAL '" + std::to_string(rand_int(3650, 1)) + " seconds'";
}

static std::string rand_bytea_value() {
  return "decode('" + rand_string(32, 4) + "', 'escape')";
}

static std::string rand_uuid_value() {
  auto hex = [](int len) {
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < len; ++i)
      out += digits[rand_int(15, 0)];
    return out;
  };
  return "'" + hex(8) + "-" + hex(4) + "-4" + hex(3) + "-a" + hex(3) + "-" +
         hex(12) + "'::uuid";
}

static std::string rand_bit_value(int length, bool varying) {
  int bit_length = std::max(1, length);
  if (varying)
    bit_length = rand_int(bit_length, 1);
  std::string bits;
  for (int i = 0; i < bit_length; ++i)
    bits += rand_int(1) == 1 ? '1' : '0';
  return "B'" + bits + "'";
}

static std::string rand_inet_value() {
  return "'" + std::to_string(rand_int(223, 1)) + "." +
         std::to_string(rand_int(255, 0)) + "." +
         std::to_string(rand_int(255, 0)) + "." +
         std::to_string(rand_int(254, 1)) + "'::inet";
}

static std::string rand_cidr_value() {
  return "'" + std::to_string(rand_int(223, 1)) + "." +
         std::to_string(rand_int(255, 0)) + "." +
         std::to_string(rand_int(255, 0)) + ".0/24'::cidr";
}

static std::string rand_macaddr_value(bool macaddr8) {
  static const char *digits = "0123456789abcdef";
  int groups = macaddr8 ? 8 : 6;
  std::string out = "'";
  for (int i = 0; i < groups; ++i) {
    if (i > 0)
      out += ":";
    out += digits[rand_int(15, 0)];
    out += digits[rand_int(15, 0)];
  }
  out += macaddr8 ? "'::macaddr8" : "'::macaddr";
  return out;
}

static std::string rand_money_value() {
  return std::to_string(rand_int(100000, 1)) + "." +
         std::to_string(rand_int(99, 0)) + "::money";
}

static std::string rand_xml_value() {
  return "XMLPARSE(CONTENT '<r k=\"" + std::to_string(rand_int(100000)) +
         "\">" + rand_string(12, 3) + "</r>')";
}

static std::string rand_tsvector_value() {
  return "to_tsvector('simple', '" + rand_string(12, 3) + " " +
         rand_string(12, 3) + "')";
}

static std::string rand_tsquery_value() {
  return "to_tsquery('simple', '" + rand_string(12, 3) + "')";
}

static std::string rand_geom_coord() {
  return std::to_string(rand_int(1000, -1000)) + "." +
         std::to_string(rand_int(9999, 0));
}

static std::string rand_geom_point_literal() {
  return "(" + rand_geom_coord() + "," + rand_geom_coord() + ")";
}

static std::string rand_point_value() {
  return "'" + rand_geom_point_literal() + "'::point";
}

static std::string rand_line_value() {
  auto first = rand_geom_point_literal();
  auto second = rand_geom_point_literal();
  while (second == first) {
    second = rand_geom_point_literal();
  }
  return "'[" + first + "," + second + "]'::line";
}

static std::string rand_lseg_value() {
  return "'[" + rand_geom_point_literal() + "," + rand_geom_point_literal() +
         "]'::lseg";
}

static std::string rand_box_value() {
  return "'(" + rand_geom_point_literal() + "," + rand_geom_point_literal() +
         ")'::box";
}

static std::string rand_path_value() {
  std::string open = rand_int(1) == 1 ? "[" : "(";
  std::string close = open == "[" ? "]" : ")";
  return "'" + open + rand_geom_point_literal() + "," +
         rand_geom_point_literal() + "," + rand_geom_point_literal() + close +
         "'::path";
}

static std::string rand_polygon_value() {
  return "'(" + rand_geom_point_literal() + "," + rand_geom_point_literal() +
         "," + rand_geom_point_literal() + ")'::polygon";
}

static std::string rand_circle_value() {
  return "'<" + rand_geom_point_literal() + "," +
         std::to_string(rand_int(500, 1)) + "." +
         std::to_string(rand_int(9999, 0)) + ">'::circle";
}

static std::string rand_int_array_value() {
  return "ARRAY[" + std::to_string(rand_int(100000, 1)) + "," +
         std::to_string(rand_int(100000, 1)) + "," +
         std::to_string(rand_int(100000, 1)) + "]::int[]";
}

static std::string rand_bigint_array_value() {
  return "ARRAY[" +
         std::to_string(static_cast<long long>(rand_int(1000000000, 1)) *
                        rand_int(1000, 1)) +
         "," +
         std::to_string(static_cast<long long>(rand_int(1000000000, 1)) *
                        rand_int(1000, 1)) +
         "]::bigint[]";
}

static std::string rand_numeric_array_value() {
  return "ARRAY[" + std::to_string(rand_int(100000, 1)) + "." +
         std::to_string(rand_int(9999, 0)) + "," +
         std::to_string(rand_int(100000, 1)) + "." +
         std::to_string(rand_int(9999, 0)) + "]::numeric[]";
}

static std::string rand_text_array_value() {
  return "ARRAY['" + rand_string(12, 1) + "','" + rand_string(12, 1) +
         "']::text[]";
}

static std::string rand_bool_array_value() {
  return "ARRAY[" + std::string(rand_int(1) == 1 ? "true" : "false") + "," +
         std::string(rand_int(1) == 1 ? "true" : "false") + "]::boolean[]";
}

static std::string rand_timestamp_array_value() {
  return "ARRAY[" + rand_timestamp_value() + "," + rand_timestamp_value() +
         "]::timestamp[]";
}

static std::string rand_int4range_value() {
  int lower = rand_int(90000, 1);
  int upper = lower + rand_int(1000, 1);
  return "int4range(" + std::to_string(lower) + ", " + std::to_string(upper) +
         ", '[)')";
}

static std::string rand_int8range_value() {
  long long lower =
      static_cast<long long>(rand_int(1000000000, 1)) * rand_int(1000, 1);
  long long upper = lower + rand_int(100000, 1);
  return "int8range(" + std::to_string(lower) + ", " + std::to_string(upper) +
         ", '[)')";
}

static std::string rand_numrange_value() {
  double lower = rand_int(100000, 1) + rand_int(9999, 0) / 10000.0;
  double upper = lower + rand_int(1000, 1) / 10.0;
  std::ostringstream out;
  out << std::fixed << std::setprecision(4);
  out << "numrange(" << lower << ", " << upper << ", '[)')";
  return out.str();
}

static std::string rand_tsrange_value() {
  int start_days = rand_int(10000, 0);
  int span_days = rand_int(365, 1);
  return "tsrange(TIMESTAMP '2000-01-01 00:00:00' + INTERVAL '" +
         std::to_string(start_days) + " days', TIMESTAMP '2000-01-01 00:00:00' + "
         "INTERVAL '" + std::to_string(start_days + span_days) + " days', '[)')";
}

static std::string rand_tstzrange_value() {
  int start_days = rand_int(10000, 0);
  int span_days = rand_int(365, 1);
  return "tstzrange(TIMESTAMPTZ '2000-01-01 00:00:00+00' + INTERVAL '" +
         std::to_string(start_days) + " days', TIMESTAMPTZ '2000-01-01 00:00:00+00' + "
         "INTERVAL '" + std::to_string(start_days + span_days) + " days', '[)')";
}

static std::string rand_daterange_value() {
  int start_days = rand_int(10000, 0);
  int span_days = rand_int(365, 1);
  return "daterange(DATE '2000-01-01' + " + std::to_string(start_days) +
         ", DATE '2000-01-01' + " + std::to_string(start_days + span_days) +
         ", '[)')";
}

static std::string two_digit_hex(int value) {
  std::ostringstream out;
  out << std::hex << std::setw(2) << std::setfill('0') << (value & 0xff);
  return out.str();
}

static std::string deterministic_unique_value(const Column *column, int offset) {
  int value = offset + 1;
  switch (column->type_) {
  case Column::SMALLINT:
  case Column::INTEGER:
  case Column::INT:
  case Column::BIGINT:
    return std::to_string(value);
  case Column::NUMERIC:
    return std::to_string(value) + ".01";
  case Column::DATE:
    return "DATE '2000-01-01' + " + std::to_string(value);
  case Column::TIME:
    return "TIME '00:00:00' + INTERVAL '" + std::to_string(value) +
           " seconds'";
  case Column::TIMETZ:
    return "(TIME WITH TIME ZONE '00:00:00+00' + INTERVAL '" +
           std::to_string(value) + " seconds')";
  case Column::TIMESTAMP:
    return "TIMESTAMP '2000-01-01 00:00:00' + INTERVAL '" +
           std::to_string(value) + " seconds'";
  case Column::TIMESTAMPTZ:
    return "TIMESTAMPTZ '2000-01-01 00:00:00+00' + INTERVAL '" +
           std::to_string(value) + " seconds'";
  case Column::CHAR:
  case Column::VARCHAR: {
    std::string text = "fk" + std::to_string(value);
    if (column->length > 0 && static_cast<int>(text.size()) > column->length)
      text = text.substr(0, column->length);
    return "'" + text + "'";
  }
  case Column::FLOAT:
    return std::to_string(value) + ".25";
  case Column::DOUBLE:
    return std::to_string(value) + ".125";
  case Column::UUID:
    return "'00000000-0000-4000-a000-" + std::to_string(100000000000 + value) +
           "'::uuid";
  case Column::INET:
    return "'10." + std::to_string((value / 65536) % 256) + "." +
           std::to_string((value / 256) % 256) + "." +
           std::to_string(value % 256) + "'::inet";
  case Column::CIDR:
    return "'10." + std::to_string((value / 256) % 256) + "." +
           std::to_string(value % 256) + ".0/24'::cidr";
  case Column::MACADDR:
    return "'02:00:00:" + two_digit_hex((value / 65536) % 256) + ":" +
           two_digit_hex((value / 256) % 256) + ":" + two_digit_hex(value % 256) +
           "'::macaddr";
  case Column::MACADDR8:
    return "'02:00:00:00:00:" + two_digit_hex((value / 65536) % 256) + ":" +
           two_digit_hex((value / 256) % 256) + ":" + two_digit_hex(value % 256) +
           "'::macaddr8";
  case Column::MONEY:
    return std::to_string(value) + ".01::money";
  case Column::BOOL:
  case Column::BIT:
  case Column::VARBIT:
  case Column::BYTEA:
  case Column::BLOB:
  case Column::JSON:
  case Column::JSONB:
  case Column::XML:
  case Column::TSVECTOR:
  case Column::TSQUERY:
  case Column::GENERATED:
  case Column::POINT:
  case Column::LINE:
  case Column::LSEG:
  case Column::BOX:
  case Column::PATH:
  case Column::POLYGON:
  case Column::CIRCLE:
  case Column::INTARRAY:
  case Column::BIGINTARRAY:
  case Column::NUMERICARRAY:
  case Column::TEXTARRAY:
  case Column::BOOLARRAY:
  case Column::TIMESTAMPARRAY:
  case Column::INT4RANGE:
  case Column::INT8RANGE:
  case Column::NUMRANGE:
  case Column::TSRANGE:
  case Column::TSTZRANGE:
  case Column::DATERANGE:
  case Column::INTERVAL:
  case Column::COLUMN_MAX:
    break;
  }
  throw std::runtime_error("unhandled unique value type " +
                           Column::col_type_to_string(column->type_));
}

static std::string random_unique_value_expr(const Column *column) {
  switch (column->type_) {
  case Column::SMALLINT:
    return "floor(1000 + random() * 30000)::smallint";
  case Column::INTEGER:
  case Column::INT:
    return "floor(100000000 + random() * 1000000000)::int";
  case Column::BIGINT:
    return "floor(1000000000000 + random() * 1000000000000)::bigint";
  case Column::NUMERIC:
    return "round((100000000 + random() * 1000000000)::numeric, 4)";
  case Column::DATE:
    return "DATE '2000-01-01' + floor(random() * 100000)::int";
  case Column::TIME:
    return "TIME '00:00:00' + (random() * interval '24 hours')";
  case Column::TIMETZ:
    return "TIME WITH TIME ZONE '00:00:00+00' + (random() * interval '24 hours')";
  case Column::TIMESTAMP:
    return "clock_timestamp() + (random() * interval '100 years')";
  case Column::TIMESTAMPTZ:
    return "clock_timestamp() + (random() * interval '100 years')";
  case Column::CHAR:
  case Column::VARCHAR:
    return "substr(md5(clock_timestamp()::text || random()::text), 1, " +
           std::to_string(std::max(1, column->length)) + ")";
  case Column::FLOAT:
    return "(random() * 1000000000)::real";
  case Column::DOUBLE:
    return "(random() * 1000000000)::double precision";
  case Column::UUID:
    return "md5(clock_timestamp()::text || random()::text)::uuid";
  case Column::INET:
    return "('10.' || floor(random() * 256)::int || '.' || "
           "floor(random() * 256)::int || '.' || floor(random() * 256)::int)::inet";
  case Column::CIDR:
    return "('10.' || floor(random() * 256)::int || '.' || "
           "floor(random() * 256)::int || '.0/24')::cidr";
  case Column::MACADDR:
    return "('02:00:00:' || lpad(to_hex(floor(random() * 256)::int), 2, '0') || ':' || "
           "lpad(to_hex(floor(random() * 256)::int), 2, '0') || ':' || "
           "lpad(to_hex(floor(random() * 256)::int), 2, '0'))::macaddr";
  case Column::MACADDR8:
    return "('02:00:00:00:00:' || lpad(to_hex(floor(random() * 256)::int), 2, '0') || ':' || "
           "lpad(to_hex(floor(random() * 256)::int), 2, '0') || ':' || "
           "lpad(to_hex(floor(random() * 256)::int), 2, '0'))::macaddr8";
  case Column::MONEY:
    return "(random() * 1000000000)::numeric::money";
  case Column::BOOL:
  case Column::BIT:
  case Column::VARBIT:
  case Column::BYTEA:
  case Column::BLOB:
  case Column::JSON:
  case Column::JSONB:
  case Column::XML:
  case Column::TSVECTOR:
  case Column::TSQUERY:
  case Column::GENERATED:
  case Column::POINT:
  case Column::LINE:
  case Column::LSEG:
  case Column::BOX:
  case Column::PATH:
  case Column::POLYGON:
  case Column::CIRCLE:
  case Column::INTARRAY:
  case Column::BIGINTARRAY:
  case Column::NUMERICARRAY:
  case Column::TEXTARRAY:
  case Column::BOOLARRAY:
  case Column::TIMESTAMPARRAY:
  case Column::INT4RANGE:
  case Column::INT8RANGE:
  case Column::NUMRANGE:
  case Column::TSRANGE:
  case Column::TSTZRANGE:
  case Column::DATERANGE:
  case Column::INTERVAL:
  case Column::COLUMN_MAX:
    break;
  }
  throw std::runtime_error("unhandled random unique value type " +
                           Column::col_type_to_string(column->type_));
}

/* return column type from a string */
Column::COLUMN_TYPES Column::col_type(std::string type) {
  if (type.compare("SMALLINT") == 0)
    return SMALLINT;
  else if (type.compare("INTEGER") == 0)
    return INTEGER;
  else if (type.compare("INT") == 0)
    return INT;
  else if (type.compare("BIGINT") == 0)
    return BIGINT;
  else if (type.compare("NUMERIC") == 0 || type.compare("DECIMAL") == 0)
    return NUMERIC;
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
  else if (type.compare("DATE") == 0)
    return DATE;
  else if (type.compare("TIME") == 0 ||
          type.compare("TIME WITHOUT TIME ZONE") == 0)
    return TIME;
  else if (type.compare("TIME WITH TIME ZONE") == 0 ||
          type.compare("TIMETZ") == 0)
    return TIMETZ;
  else if (type.compare("TIMESTAMP") == 0)
    return TIMESTAMP;
  else if (type.compare("TIMESTAMP WITHOUT TIME ZONE") == 0)
    return TIMESTAMP;
  else if (type.compare("TIMESTAMP WITH TIME ZONE") == 0 ||
          type.compare("TIMESTAMPTZ") == 0)
    return TIMESTAMPTZ;
  else if (type.compare("INTERVAL") == 0)
    return INTERVAL;
  else if (type.compare("BIT") == 0)
    return BIT;
  else if (type.compare("BIT VARYING") == 0 || type.compare("VARBIT") == 0)
    return VARBIT;
  else if (type.compare("INET") == 0)
    return INET;
  else if (type.compare("CIDR") == 0)
    return CIDR;
  else if (type.compare("MACADDR") == 0)
    return MACADDR;
  else if (type.compare("MACADDR8") == 0)
    return MACADDR8;
  else if (type.compare("MONEY") == 0)
    return MONEY;
  else if (type.compare("XML") == 0)
    return XML;
  else if (type.compare("TSVECTOR") == 0)
    return TSVECTOR;
  else if (type.compare("TSQUERY") == 0)
    return TSQUERY;
  else if (type.compare("POINT") == 0)
    return POINT;
  else if (type.compare("LINE") == 0)
    return LINE;
  else if (type.compare("LSEG") == 0)
    return LSEG;
  else if (type.compare("BOX") == 0)
    return BOX;
  else if (type.compare("PATH") == 0)
    return PATH;
  else if (type.compare("POLYGON") == 0)
    return POLYGON;
  else if (type.compare("CIRCLE") == 0)
    return CIRCLE;
  else if (type.compare("INT[]") == 0 || type.compare("INTEGER[]") == 0)
    return INTARRAY;
  else if (type.compare("BIGINT[]") == 0)
    return BIGINTARRAY;
  else if (type.compare("NUMERIC[]") == 0)
    return NUMERICARRAY;
  else if (type.compare("TEXT[]") == 0)
    return TEXTARRAY;
  else if (type.compare("BOOLEAN[]") == 0 || type.compare("BOOL[]") == 0)
    return BOOLARRAY;
  else if (type.compare("TIMESTAMP[]") == 0)
    return TIMESTAMPARRAY;
  else if (type.compare("INT4RANGE") == 0)
    return INT4RANGE;
  else if (type.compare("INT8RANGE") == 0)
    return INT8RANGE;
  else if (type.compare("NUMRANGE") == 0)
    return NUMRANGE;
  else if (type.compare("TSRANGE") == 0)
    return TSRANGE;
  else if (type.compare("TSTZRANGE") == 0)
    return TSTZRANGE;
  else if (type.compare("DATERANGE") == 0)
    return DATERANGE;
  else if (type.compare("BYTEA") == 0)
    return BYTEA;
  else if (type.compare("UUID") == 0)
    return UUID;
  else if (type.compare("JSON") == 0)
    return JSON;
  else if (type.compare("JSONB") == 0)
    return JSONB;
  else
    throw std::runtime_error("unhandled " + col_type_to_string(type_) +
                             " at line " + std::to_string(__LINE__));
}

/* return string from a column type */
const std::string Column::col_type_to_string(COLUMN_TYPES type) {
  switch (type) {
  case SMALLINT:
    return "SMALLINT";
  case INTEGER:
    return "INTEGER";
  case INT:
    return "INT";
  case BIGINT:
    return "BIGINT";
  case NUMERIC:
    return "NUMERIC";
  case CHAR:
    return "CHAR";
  case DOUBLE:
    return "DOUBLE PRECISION";
  case FLOAT:
    return "REAL";
  case VARCHAR:
    return "VARCHAR";
  case DATE:
    return "DATE";
  case TIME:
    return "TIME";
  case TIMETZ:
    return "TIME WITH TIME ZONE";
  case TIMESTAMP:
    return "TIMESTAMP";
  case TIMESTAMPTZ:
    return "TIMESTAMP WITH TIME ZONE";
  case INTERVAL:
    return "INTERVAL";
  case BIT:
    return "BIT";
  case VARBIT:
    return "BIT VARYING";
  case INET:
    return "INET";
  case CIDR:
    return "CIDR";
  case MACADDR:
    return "MACADDR";
  case MACADDR8:
    return "MACADDR8";
  case MONEY:
    return "MONEY";
  case XML:
    return "XML";
  case TSVECTOR:
    return "TSVECTOR";
  case TSQUERY:
    return "TSQUERY";
  case POINT:
    return "POINT";
  case LINE:
    return "LINE";
  case LSEG:
    return "LSEG";
  case BOX:
    return "BOX";
  case PATH:
    return "PATH";
  case POLYGON:
    return "POLYGON";
  case CIRCLE:
    return "CIRCLE";
  case INTARRAY:
    return "INT[]";
  case BIGINTARRAY:
    return "BIGINT[]";
  case NUMERICARRAY:
    return "NUMERIC[]";
  case TEXTARRAY:
    return "TEXT[]";
  case BOOLARRAY:
    return "BOOLEAN[]";
  case TIMESTAMPARRAY:
    return "TIMESTAMP[]";
  case INT4RANGE:
    return "INT4RANGE";
  case INT8RANGE:
    return "INT8RANGE";
  case NUMRANGE:
    return "NUMRANGE";
  case TSRANGE:
    return "TSRANGE";
  case TSTZRANGE:
    return "TSTZRANGE";
  case DATERANGE:
    return "DATERANGE";
  case BOOL:
    return "BOOLEAN";
  case BYTEA:
    return "BYTEA";
  case BLOB:
    return "TEXT";
  case JSON:
    return "JSON";
  case JSONB:
    return "JSONB";
  case UUID:
    return "UUID";
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
  case (Column::COLUMN_TYPES::SMALLINT):
    return std::to_string(rand_int(32767, 1));
    break;
  case (Column::COLUMN_TYPES::INTEGER):
    return std::to_string(
        rand_int(options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt()));
    break;
  case (Column::COLUMN_TYPES::INT):
    return std::to_string(
        rand_int(g_integer_range *
                 options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt()));
    break;
  case (Column::COLUMN_TYPES::BIGINT):
    return std::to_string(static_cast<long long>(rand_int(1000000000, 1)) *
                          rand_int(1000, 1));
    break;
  case (Column::COLUMN_TYPES::NUMERIC):
    return std::to_string(rand_int(100000, 1)) + "." +
           std::to_string(rand_int(9999, 0));
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
  case Column::COLUMN_TYPES::TIMESTAMPTZ:
    return rand_timestamptz_value();
  case Column::COLUMN_TYPES::DATE:
    return rand_date_value();
  case Column::COLUMN_TYPES::TIME:
    return rand_time_value();
  case Column::COLUMN_TYPES::TIMETZ:
    return rand_timetz_value();
  case Column::COLUMN_TYPES::INTERVAL:
    return rand_interval_value();
  case Column::COLUMN_TYPES::BIT:
    return rand_bit_value(length, false);
  case Column::COLUMN_TYPES::VARBIT:
    return rand_bit_value(length, true);
  case Column::COLUMN_TYPES::INET:
    return rand_inet_value();
  case Column::COLUMN_TYPES::CIDR:
    return rand_cidr_value();
  case Column::COLUMN_TYPES::MACADDR:
    return rand_macaddr_value(false);
  case Column::COLUMN_TYPES::MACADDR8:
    return rand_macaddr_value(true);
  case Column::COLUMN_TYPES::MONEY:
    return rand_money_value();
  case Column::COLUMN_TYPES::XML:
    return rand_xml_value();
  case Column::COLUMN_TYPES::TSVECTOR:
    return rand_tsvector_value();
  case Column::COLUMN_TYPES::TSQUERY:
    return rand_tsquery_value();
  case Column::COLUMN_TYPES::POINT:
    return rand_point_value();
  case Column::COLUMN_TYPES::LINE:
    return rand_line_value();
  case Column::COLUMN_TYPES::LSEG:
    return rand_lseg_value();
  case Column::COLUMN_TYPES::BOX:
    return rand_box_value();
  case Column::COLUMN_TYPES::PATH:
    return rand_path_value();
  case Column::COLUMN_TYPES::POLYGON:
    return rand_polygon_value();
  case Column::COLUMN_TYPES::CIRCLE:
    return rand_circle_value();
  case Column::COLUMN_TYPES::INTARRAY:
    return rand_int_array_value();
  case Column::COLUMN_TYPES::BIGINTARRAY:
    return rand_bigint_array_value();
  case Column::COLUMN_TYPES::NUMERICARRAY:
    return rand_numeric_array_value();
  case Column::COLUMN_TYPES::TEXTARRAY:
    return rand_text_array_value();
  case Column::COLUMN_TYPES::BOOLARRAY:
    return rand_bool_array_value();
  case Column::COLUMN_TYPES::TIMESTAMPARRAY:
    return rand_timestamp_array_value();
  case Column::COLUMN_TYPES::INT4RANGE:
    return rand_int4range_value();
  case Column::COLUMN_TYPES::INT8RANGE:
    return rand_int8range_value();
  case Column::COLUMN_TYPES::NUMRANGE:
    return rand_numrange_value();
  case Column::COLUMN_TYPES::TSRANGE:
    return rand_tsrange_value();
  case Column::COLUMN_TYPES::TSTZRANGE:
    return rand_tstzrange_value();
  case Column::COLUMN_TYPES::DATERANGE:
    return rand_daterange_value();
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
  case Column::COLUMN_TYPES::BYTEA:
    return rand_bytea_value();
  case Column::COLUMN_TYPES::JSON:
    return rand_json_text_value();
    break;
  case Column::COLUMN_TYPES::JSONB:
    return rand_json_value();
    break;
  case Column::COLUMN_TYPES::UUID:
    return rand_uuid_value();
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
  case SMALLINT:
    name_ = "si" + name;
    break;
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
  case BIGINT:
    name_ = "bi" + name;
    break;
  case NUMERIC:
    name_ = "n" + name;
    break;
  case FLOAT:
    name_ = "f" + name;
    break;
  case DOUBLE:
    name_ = "d" + name;
    break;
  case DATE:
    name_ = "dt" + name;
    break;
  case TIME:
    name_ = "tm" + name;
    break;
  case TIMETZ:
    name_ = "tt" + name;
    break;
  case TIMESTAMP:
    name_ = "ts" + name;
    break;
  case TIMESTAMPTZ:
    name_ = "tz" + name;
    break;
  case INTERVAL:
    name_ = "iv" + name;
    break;
  case BIT:
    name_ = "bt" + name;
    length = rand_int(32, 1);
    break;
  case VARBIT:
    name_ = "vb" + name;
    length = rand_int(64, 1);
    break;
  case INET:
    name_ = "in" + name;
    break;
  case CIDR:
    name_ = "cd" + name;
    break;
  case MACADDR:
    name_ = "ma" + name;
    break;
  case MACADDR8:
    name_ = "m8" + name;
    break;
  case MONEY:
    name_ = "mo" + name;
    break;
  case XML:
    name_ = "x" + name;
    break;
  case TSVECTOR:
    name_ = "tv" + name;
    break;
  case TSQUERY:
    name_ = "tq" + name;
    break;
  case POINT:
    name_ = "pt" + name;
    break;
  case LINE:
    name_ = "ln" + name;
    break;
  case LSEG:
    name_ = "ls" + name;
    break;
  case BOX:
    name_ = "bx" + name;
    break;
  case PATH:
    name_ = "ph" + name;
    break;
  case POLYGON:
    name_ = "pl" + name;
    break;
  case CIRCLE:
    name_ = "cr" + name;
    break;
  case INTARRAY:
    name_ = "ai" + name;
    break;
  case BIGINTARRAY:
    name_ = "ab" + name;
    break;
  case NUMERICARRAY:
    name_ = "an" + name;
    break;
  case TEXTARRAY:
    name_ = "at" + name;
    break;
  case BOOLARRAY:
    name_ = "ao" + name;
    break;
  case TIMESTAMPARRAY:
    name_ = "ats" + name;
    break;
  case INT4RANGE:
    name_ = "r4" + name;
    break;
  case INT8RANGE:
    name_ = "r8" + name;
    break;
  case NUMRANGE:
    name_ = "rn" + name;
    break;
  case TSRANGE:
    name_ = "rt" + name;
    break;
  case TSTZRANGE:
    name_ = "rz" + name;
    break;
  case DATERANGE:
    name_ = "rd" + name;
    break;
  case BOOL:
    name_ = "t" + name;
    break;
  case BYTEA:
    name_ = "ba" + name;
    break;
  case JSON:
    name_ = "j" + name;
    break;
  case JSONB:
    name_ = "jb" + name;
    break;
  case UUID:
    name_ = "u" + name;
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
  case Column::SMALLINT:
  case Column::INT:
  case Column::INTEGER:
  case Column::BIGINT:
    return col->name_;
  case Column::NUMERIC:
  case Column::FLOAT:
  case Column::DOUBLE:
    return "ROUND(" + col->name_ + ")::INTEGER";
  case Column::DATE:
    return "(MOD((" + col->name_ + " - DATE '2000-01-01'), 1000000))::INTEGER";
  case Column::TIME:
  case Column::TIMETZ:
    return "(MOD(EXTRACT(EPOCH FROM " + col->name_ +
           ")::BIGINT, 1000000))::INTEGER";
  case Column::TIMESTAMP:
  case Column::TIMESTAMPTZ:
    return "(MOD(EXTRACT(EPOCH FROM " + col->name_ +
           ")::BIGINT, 1000000))::INTEGER";
  case Column::INTERVAL:
    return "(MOD(EXTRACT(EPOCH FROM " + col->name_ +
           ")::BIGINT, 1000000))::INTEGER";
  case Column::BIT:
  case Column::VARBIT:
    return "LENGTH(COALESCE(" + col->name_ + "::TEXT, ''))";
  case Column::INET:
  case Column::CIDR:
  case Column::MACADDR:
  case Column::MACADDR8:
    return "LENGTH(COALESCE(" + col->name_ + "::TEXT, ''))";
  case Column::MONEY:
    return "ROUND(" + col->name_ + "::NUMERIC)::INTEGER";
  case Column::XML:
    return "LENGTH(COALESCE(XMLSERIALIZE(CONTENT " + col->name_ +
           " AS TEXT), ''))";
  case Column::TSVECTOR:
  case Column::TSQUERY:
    return "LENGTH(COALESCE(" + col->name_ + "::TEXT, ''))";
  case Column::POINT:
  case Column::LINE:
  case Column::LSEG:
  case Column::BOX:
  case Column::PATH:
  case Column::POLYGON:
  case Column::CIRCLE:
  case Column::INTARRAY:
  case Column::BIGINTARRAY:
  case Column::NUMERICARRAY:
  case Column::TEXTARRAY:
  case Column::BOOLARRAY:
  case Column::TIMESTAMPARRAY:
  case Column::INT4RANGE:
  case Column::INT8RANGE:
  case Column::NUMRANGE:
  case Column::TSRANGE:
  case Column::TSTZRANGE:
  case Column::DATERANGE:
    return "LENGTH(COALESCE(" + col->name_ + "::TEXT, ''))";
  case Column::BOOL:
    return "(CASE WHEN " + col->name_ + " THEN 1 ELSE 0 END)";
  case Column::VARCHAR:
  case Column::CHAR:
  case Column::BLOB:
  case Column::UUID:
    return "LENGTH(COALESCE(" + col->name_ + "::TEXT, ''))";
  case Column::BYTEA:
    return "LENGTH(COALESCE(encode(" + col->name_ + ", 'hex'), ''))";
  case Column::JSON:
  case Column::JSONB:
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
  case Column::SMALLINT:
  case Column::INT:
  case Column::INTEGER:
  case Column::BIGINT:
  case Column::NUMERIC:
    column_size = 10;
    expr = "COALESCE(" + col->name_ + "::TEXT, '')";
    break;
  case Column::FLOAT:
  case Column::DOUBLE:
    column_size = 10;
    expr = "COALESCE(" + col->name_ + "::TEXT, '')";
    break;
  case Column::DATE:
    column_size = 10;
    expr = "COALESCE((MOD((" + col->name_ +
           " - DATE '2000-01-01'), 1000000))::TEXT, '')";
    break;
  case Column::TIME:
  case Column::TIMETZ:
    column_size = 15;
    expr = "COALESCE((MOD(EXTRACT(EPOCH FROM " + col->name_ +
           ")::BIGINT, 1000000))::TEXT, '')";
    break;
  case Column::TIMESTAMP:
  case Column::TIMESTAMPTZ:
    column_size = 19;
    expr = "COALESCE((MOD(EXTRACT(EPOCH FROM " + col->name_ +
           ")::BIGINT, 1000000))::TEXT, '')";
    break;
  case Column::INTERVAL:
    column_size = 24;
    expr = "COALESCE(EXTRACT(EPOCH FROM " + col->name_ + ")::TEXT, '')";
    break;
  case Column::BIT:
  case Column::VARBIT:
    column_size = std::max(1, col->length);
    expr = "COALESCE(" + col->name_ + "::TEXT, '')";
    break;
  case Column::INET:
  case Column::CIDR:
    column_size = 43;
    expr = "COALESCE(" + col->name_ + "::TEXT, '')";
    break;
  case Column::MACADDR:
    column_size = 17;
    expr = "COALESCE(" + col->name_ + "::TEXT, '')";
    break;
  case Column::MACADDR8:
    column_size = 23;
    expr = "COALESCE(" + col->name_ + "::TEXT, '')";
    break;
  case Column::MONEY:
    column_size = 32;
    expr = "COALESCE(" + col->name_ + "::TEXT, '')";
    break;
  case Column::XML:
    column_size = 128;
    expr = "COALESCE(XMLSERIALIZE(CONTENT " + col->name_ + " AS TEXT), '')";
    break;
  case Column::TSVECTOR:
  case Column::TSQUERY:
    column_size = 128;
    expr = "COALESCE(" + col->name_ + "::TEXT, '')";
    break;
  case Column::POINT:
  case Column::LINE:
  case Column::LSEG:
  case Column::BOX:
  case Column::PATH:
  case Column::POLYGON:
  case Column::CIRCLE:
  case Column::INTARRAY:
  case Column::BIGINTARRAY:
  case Column::NUMERICARRAY:
  case Column::TEXTARRAY:
  case Column::BOOLARRAY:
  case Column::TIMESTAMPARRAY:
  case Column::INT4RANGE:
  case Column::INT8RANGE:
  case Column::NUMRANGE:
  case Column::TSRANGE:
  case Column::TSTZRANGE:
  case Column::DATERANGE:
    column_size = 128;
    expr = "COALESCE(" + col->name_ + "::TEXT, '')";
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
    expr = "COALESCE(" + col->name_ + ", '')";
    break;
  case Column::BYTEA:
    column_size = 5000;
    expr = "COALESCE(encode(" + col->name_ + ", 'hex'), '')";
    break;
  case Column::UUID:
    column_size = 36;
    expr = "COALESCE(" + col->name_ + "::TEXT, '')";
    break;
  case Column::JSON:
  case Column::JSONB:
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
                                   std::string clause, std::string sub_type,
                                   std::string generated_kind_arg)
    : Column(table, Column::GENERATED) {
  name_ = name;
  str = clause;
  g_type = Column::col_type(sub_type);
  std::transform(generated_kind_arg.begin(), generated_kind_arg.end(),
                 generated_kind_arg.begin(), ::tolower);
  generated_kind = generated_kind_arg == "virtual" ? VIRTUAL : STORED;
}

/* Generated column constructor. lock table before calling */
Generated_Column::Generated_Column(std::string name, Table *table)
    : Column(table, Column::GENERATED) {
  name_ = "g" + name;
  auto generated_kind_clause = normalized_generated_column_kind();
  generated_kind = generated_kind_clause == "virtual" ? VIRTUAL : STORED;
  auto blob_supported = !options->at(Option::NO_BLOB)->getBool();
  g_type = COLUMN_MAX;
  /* Generated columns keep to stable scalar/text results. */
  while (g_type == COLUMN_MAX) {
    auto x = rand_int(6, 1);
    if (x <= 1)
      g_type = INT;
    else if (x <= 2)
      g_type = BIGINT;
    else if (x <= 3)
      g_type = NUMERIC;
    else if (x <= 4)
      g_type = VARCHAR;
    else if (x <= 5)
      g_type = CHAR;
    else if (blob_supported && x <= 6) {
      g_type = BLOB;
    }
  }

  auto col_pos = generated_source_positions(table);
  if (col_pos.size() < k_generated_min_base_columns) {
    throw std::runtime_error("insufficient generated column sources");
  }

  std::shuffle(col_pos.begin(), col_pos.end(), rng);

  size_t dependency_cap =
      std::min(k_generated_dependency_soft_cap, col_pos.size());
  if (col_pos.size() > k_generated_dependency_soft_cap && rand_int(3) == 0) {
    dependency_cap = std::min(k_generated_dependency_hard_cap, col_pos.size());
  }

  if (dependency_cap == 0) {
    throw std::runtime_error("zero generated dependency budget");
  }

  col_pos.resize(rand_int(static_cast<int>(dependency_cap), 1));

  if (g_type == INT || g_type == INTEGER || g_type == BIGINT ||
      g_type == SMALLINT || g_type == NUMERIC) {
    std::vector<std::string> terms;
    for (auto pos : col_pos) {
      terms.push_back("(" +
                      pg_generated_numeric_term(table->columns_->at(pos)) +
                      ")::NUMERIC");
    }
    std::string sum_expr = "(";
    for (const auto &term : terms)
      sum_expr += term + " + ";
    sum_expr.erase(sum_expr.length() - 3);
    sum_expr += ")";

    str = " " + col_type_to_string(g_type) + " GENERATED ALWAYS AS (";
    if (g_type == SMALLINT) {
      str += "(MOD((" + sum_expr + ")::NUMERIC, 30000))::SMALLINT";
    } else if (g_type == INT || g_type == INTEGER) {
      str += "(MOD((" + sum_expr + ")::NUMERIC, 2000000000))::" +
             col_type_to_string(g_type);
    } else if (g_type == BIGINT) {
      str += "(MOD((" + sum_expr + ")::NUMERIC, 9000000000000000000))::BIGINT";
    } else {
      str += "(" + sum_expr + ")::NUMERIC";
    }
    str += ") " + std::string(generated_kind == VIRTUAL ? "VIRTUAL" : "STORED");
    return;
  } else if (g_type == VARCHAR || g_type == CHAR || g_type == BLOB) {
    auto size = rand_int(k_generated_text_budget_max, k_generated_text_budget_min);
    int actual_size = 0;
    std::vector<std::string> parts;
    int remaining_budget = size;
    for (size_t i = 0; i < col_pos.size() && remaining_budget > 0; ++i) {
      auto pos = col_pos[i];
      int remaining_sources = static_cast<int>(col_pos.size() - i);
      int current_upper = std::max(1, remaining_budget / remaining_sources);
      auto current_size = rand_int(current_upper, 1);
      parts.push_back(pg_generated_text_term(table->columns_->at(pos),
                                             current_size, actual_size));
      remaining_budget = std::max(0, size - actual_size);
    }

    if (parts.empty() || actual_size == 0) {
      throw std::runtime_error("empty generated text expression");
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
    str += ") " + std::string(generated_kind == VIRTUAL ? "VIRTUAL" : "STORED");
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
  writer.String("referenced_key");
  writer.Bool(referenced_key);
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
  writer.String("generated_kind");
  auto kind = generated_kind_string();
  writer.String(kind.c_str(), static_cast<SizeType>(kind.length()));
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
  writer.String("unique");
  writer.Bool(unique);
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
  if (legacy_key != nullptr) {
    if (const auto *value = json_string_member(obj, legacy_key)) {
      if (!legacy_default.empty() && value == legacy_default)
        return "";
      return value;
    }
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

static bool metadata_bool_member(const rapidjson::Value &obj, const char *key) {
  return obj.HasMember(key) && obj[key].IsBool() && obj[key].GetBool();
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
    std::string parent_key =
        fk_table->parent_key != nullptr ? fk_table->parent_key->name_ : "";
    std::string child_key =
        fk_table->child_key != nullptr ? fk_table->child_key->name_ : "";
    writer.String("parent");
    writer.String(parent.c_str(), static_cast<SizeType>(parent.length()));
    writer.String("parent_key");
    writer.String(parent_key.c_str(), static_cast<SizeType>(parent_key.length()));
    writer.String("child_key");
    writer.String(child_key.c_str(), static_cast<SizeType>(child_key.length()));
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
  return "CREATE " + std::string(index->unique ? "UNIQUE " : "") + "INDEX " +
         index->name_ + " ON " + table->name_ + "(" +
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

static Column *find_column_by_name(Table *table, const std::string &name) {
  if (table == nullptr)
    return nullptr;
  for (auto *col : *table->columns_) {
    if (col->name_ == name)
      return col;
  }
  return nullptr;
}

static Column *pick_fk_parent_key(Table *parent) {
  std::vector<Column *> unique_candidates;
  std::vector<Column *> pk_candidates;

  for (auto *col : *parent->columns_) {
    if (!pg_fk_referenceable_column(col))
      continue;
    if (col->primary_key)
      pk_candidates.push_back(col);
    else
      unique_candidates.push_back(col);
  }

  if (!unique_candidates.empty() &&
      (pk_candidates.empty() || rand_int(1) == 0)) {
    return unique_candidates.at(rand_int(unique_candidates.size() - 1));
  }
  if (!pk_candidates.empty()) {
    return pk_candidates.at(rand_int(pk_candidates.size() - 1));
  }
  return nullptr;
}

static Column *make_fk_child_key(Column *parent_key, Table *child) {
  auto *child_key = new Column("fk_col", child, parent_key->type_);
  child_key->length = parent_key->length;
  child_key->null = false;
  return child_key;
}

static std::string fk_reference_value_expr(const Table *table) {
  if (table == nullptr || table->type != Table::FK) {
    return "";
  }

  const auto *fk_table = static_cast<const FK_table *>(table);
  if (fk_table->parent == nullptr) {
    return "NULL";
  }

  if (fk_table->parent_key == nullptr) {
    return "NULL";
  }

  return "(SELECT " + fk_table->parent_key->name_ + " FROM " +
         fk_table->parent->name_ + " ORDER BY random() LIMIT 1)";
}

static bool simple_scalar_hit_column(const Column *column) {
  if (column == nullptr || column->type_ == Column::GENERATED) {
    return false;
  }

  switch (column->type_) {
  case Column::BOOL:
  case Column::SMALLINT:
  case Column::INTEGER:
  case Column::INT:
  case Column::BIGINT:
  case Column::NUMERIC:
  case Column::FLOAT:
  case Column::DOUBLE:
  case Column::DATE:
  case Column::TIME:
  case Column::TIMETZ:
  case Column::TIMESTAMP:
  case Column::TIMESTAMPTZ:
  case Column::INTERVAL:
  case Column::VARCHAR:
  case Column::CHAR:
  case Column::UUID:
  case Column::INET:
  case Column::CIDR:
  case Column::MACADDR:
  case Column::MACADDR8:
  case Column::MONEY:
    return true;
  case Column::BIT:
  case Column::VARBIT:
  case Column::BYTEA:
  case Column::BLOB:
  case Column::JSON:
  case Column::JSONB:
  case Column::XML:
  case Column::TSVECTOR:
  case Column::TSQUERY:
  case Column::GENERATED:
  case Column::POINT:
  case Column::LINE:
  case Column::LSEG:
  case Column::BOX:
  case Column::PATH:
  case Column::POLYGON:
  case Column::CIRCLE:
  case Column::INTARRAY:
  case Column::BIGINTARRAY:
  case Column::NUMERICARRAY:
  case Column::TEXTARRAY:
  case Column::BOOLARRAY:
  case Column::TIMESTAMPARRAY:
  case Column::INT4RANGE:
  case Column::INT8RANGE:
  case Column::NUMRANGE:
  case Column::TSRANGE:
  case Column::TSTZRANGE:
  case Column::DATERANGE:
  case Column::COLUMN_MAX:
    return false;
  }

  return false;
}

static bool cacheable_sql_value(const std::string &expr) {
  if (expr.empty()) {
    return false;
  }

  std::string lowered = expr;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (lowered == "default" || lowered == "null") {
    return false;
  }

  return lowered.find("select ") == std::string::npos &&
         lowered.find("(select ") == std::string::npos;
}

static bool has_cached_hit_value(Table *table, const Column *column) {
  if (table == nullptr || column == nullptr) {
    return false;
  }

  std::lock_guard<std::mutex> guard(table->hit_value_mutex);
  auto it = table->hit_value_cache.find(column->name_);
  return it != table->hit_value_cache.end() && !it->second.empty();
}

static void remember_hit_value(Table *table, const Column *column,
                               const std::string &value_expr) {
  if (table == nullptr || !simple_scalar_hit_column(column) ||
      !cacheable_sql_value(value_expr)) {
    return;
  }

  std::lock_guard<std::mutex> guard(table->hit_value_mutex);
  auto &bucket = table->hit_value_cache[column->name_];
  bucket.push_back(value_expr);
  if (bucket.size() > k_hit_cache_limit) {
    bucket.pop_front();
  }
}

static std::string cached_hit_value(Table *table, const Column *column) {
  if (table == nullptr || column == nullptr) {
    return "";
  }

  std::lock_guard<std::mutex> guard(table->hit_value_mutex);
  auto it = table->hit_value_cache.find(column->name_);
  if (it == table->hit_value_cache.end() || it->second.empty()) {
    return "";
  }
  return it->second.at(rand_int(it->second.size() - 1));
}

static void clear_hit_value_cache(Table *table) {
  if (table == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> guard(table->hit_value_mutex);
  table->hit_value_cache.clear();
}

static void rename_hit_value_cache(Table *table, const std::string &from,
                                   const std::string &to) {
  if (table == nullptr || from == to) {
    return;
  }

  std::lock_guard<std::mutex> guard(table->hit_value_mutex);
  auto it = table->hit_value_cache.find(from);
  if (it == table->hit_value_cache.end()) {
    return;
  }
  table->hit_value_cache[to] = std::move(it->second);
  table->hit_value_cache.erase(it);
}

static void erase_hit_value_cache(Table *table, const std::string &name) {
  if (table == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> guard(table->hit_value_mutex);
  table->hit_value_cache.erase(name);
}

static bool use_hit_oriented_where() {
  return rand_int(99) < k_hit_where_probability;
}

static Column *pick_hit_where_column(Table *table) {
  if (table == nullptr) {
    return nullptr;
  }

  std::vector<Column *> primary_columns;
  std::vector<Column *> key_columns;
  std::vector<Column *> scalar_columns;

  for (auto *column : *table->columns_) {
    if (!simple_scalar_hit_column(column) || !has_cached_hit_value(table, column)) {
      continue;
    }

    if (column->primary_key) {
      primary_columns.push_back(column);
    } else if (column->referenced_key) {
      key_columns.push_back(column);
    } else {
      scalar_columns.push_back(column);
    }
  }

  if (!primary_columns.empty()) {
    return primary_columns.at(rand_int(primary_columns.size() - 1));
  }
  if (!key_columns.empty()) {
    return key_columns.at(rand_int(key_columns.size() - 1));
  }
  if (!scalar_columns.empty()) {
    return scalar_columns.at(rand_int(scalar_columns.size() - 1));
  }
  return nullptr;
}

static bool build_hit_oriented_where(Table *table, std::string &predicate) {
  if (!use_hit_oriented_where()) {
    return false;
  }

  auto *column = pick_hit_where_column(table);
  if (column == nullptr) {
    return false;
  }

  auto value_expr = cached_hit_value(table, column);
  if (value_expr.empty()) {
    return false;
  }

  predicate = column->name_ + " = " + value_expr;
  return true;
}

static bool build_aliased_hit_oriented_where(Table *table,
                                             const std::string &alias,
                                             std::string &predicate) {
  if (!use_hit_oriented_where()) {
    return false;
  }

  auto *column = pick_hit_where_column(table);
  if (column == nullptr) {
    return false;
  }

  auto value_expr = cached_hit_value(table, column);
  if (value_expr.empty()) {
    return false;
  }

  predicate = alias + "." + column->name_ + " = " + value_expr;
  return true;
}

static bool join_select_column(const Column *column) {
  if (column == nullptr || column->type_ == Column::GENERATED) {
    return false;
  }

  switch (column->type_) {
  case Column::SMALLINT:
  case Column::INTEGER:
  case Column::INT:
  case Column::BIGINT:
  case Column::NUMERIC:
  case Column::CHAR:
  case Column::VARCHAR:
  case Column::DATE:
  case Column::TIME:
  case Column::TIMETZ:
  case Column::TIMESTAMP:
  case Column::TIMESTAMPTZ:
  case Column::UUID:
    return true;
  case Column::BOOL:
  case Column::FLOAT:
  case Column::DOUBLE:
  case Column::INTERVAL:
  case Column::BIT:
  case Column::VARBIT:
  case Column::INET:
  case Column::CIDR:
  case Column::MACADDR:
  case Column::MACADDR8:
  case Column::MONEY:
  case Column::XML:
  case Column::TSVECTOR:
  case Column::TSQUERY:
  case Column::POINT:
  case Column::LINE:
  case Column::LSEG:
  case Column::BOX:
  case Column::PATH:
  case Column::POLYGON:
  case Column::CIRCLE:
  case Column::INTARRAY:
  case Column::BIGINTARRAY:
  case Column::NUMERICARRAY:
  case Column::TEXTARRAY:
  case Column::BOOLARRAY:
  case Column::TIMESTAMPARRAY:
  case Column::INT4RANGE:
  case Column::INT8RANGE:
  case Column::NUMRANGE:
  case Column::TSRANGE:
  case Column::TSTZRANGE:
  case Column::DATERANGE:
  case Column::BYTEA:
  case Column::BLOB:
  case Column::JSON:
  case Column::JSONB:
  case Column::GENERATED:
  case Column::COLUMN_MAX:
    return false;
  }

  return false;
}

static bool join_compatible_columns(const Column *left, const Column *right) {
  return left != nullptr && right != nullptr && left->type_ == right->type_ &&
         join_select_column(left) && join_select_column(right);
}

struct JoinSelectTarget {
  Table *left_table = nullptr;
  Table *right_table = nullptr;
  Column *left_column = nullptr;
  Column *right_column = nullptr;
};

static bool sample_join_target_from_fk(std::vector<Table *> *all_tables,
                                       JoinSelectTarget &target) {
  if (all_tables == nullptr) {
    return false;
  }

  size_t candidates_seen = 0;
  for (auto *table : *all_tables) {
    if (table == nullptr || table->type != Table::FK) {
      continue;
    }

    auto *fk_table = static_cast<FK_table *>(table);
    if (fk_table->parent == nullptr || fk_table->child_key == nullptr ||
        fk_table->parent_key == nullptr ||
        !join_compatible_columns(fk_table->child_key, fk_table->parent_key)) {
      continue;
    }

    candidates_seen++;
    if (candidates_seen == 1 ||
        rand_int(static_cast<int>(candidates_seen - 1), 0) == 0) {
      target.left_table = table;
      target.right_table = fk_table->parent;
      target.left_column = fk_table->child_key;
      target.right_column = fk_table->parent_key;
    }
  }

  return candidates_seen > 0;
}

static bool sample_join_target_from_table_pairs(std::vector<Table *> *all_tables,
                                                JoinSelectTarget &target) {
  if (all_tables == nullptr || all_tables->empty()) {
    return false;
  }

  size_t candidates_seen = 0;
  for (size_t left_index = 0; left_index < all_tables->size(); ++left_index) {
    auto *left_table = all_tables->at(left_index);
    if (left_table == nullptr) {
      continue;
    }

    for (size_t right_index = left_index + 1; right_index < all_tables->size();
         ++right_index) {
      auto *right_table = all_tables->at(right_index);
      if (right_table == nullptr) {
        continue;
      }

      std::scoped_lock lock(left_table->table_mutex, right_table->table_mutex);
      std::array<std::vector<Column *>, Column::COLUMN_MAX> left_columns;
      std::array<std::vector<Column *>, Column::COLUMN_MAX> right_columns;
      std::vector<Column::COLUMN_TYPES> common_types;

      for (auto *column : *left_table->columns_) {
        if (join_select_column(column)) {
          left_columns[column->type_].push_back(column);
        }
      }
      for (auto *column : *right_table->columns_) {
        if (join_select_column(column)) {
          right_columns[column->type_].push_back(column);
        }
      }

      for (size_t type = 0; type < Column::COLUMN_MAX; ++type) {
        if (!left_columns[type].empty() && !right_columns[type].empty()) {
          common_types.push_back(static_cast<Column::COLUMN_TYPES>(type));
        }
      }

      if (common_types.empty()) {
        continue;
      }

      auto selected_type =
          common_types.at(rand_int(common_types.size() - 1, 0));
      auto *left_column =
          left_columns[selected_type].at(rand_int(left_columns[selected_type].size() - 1, 0));
      auto *right_column = right_columns[selected_type].at(
          rand_int(right_columns[selected_type].size() - 1, 0));

      candidates_seen++;
      if (candidates_seen == 1 ||
          rand_int(static_cast<int>(candidates_seen - 1), 0) == 0) {
        target.left_table = left_table;
        target.right_table = right_table;
        target.left_column = left_column;
        target.right_column = right_column;
      }
    }
  }

  return candidates_seen > 0;
}

static bool sample_self_join_target(std::vector<Table *> *all_tables,
                                    JoinSelectTarget &target) {
  if (all_tables == nullptr || all_tables->empty()) {
    return false;
  }

  size_t candidates_seen = 0;
  for (auto *table : *all_tables) {
    if (table == nullptr) {
      continue;
    }

    std::lock_guard<std::recursive_mutex> lock(table->table_mutex);
    std::array<std::vector<Column *>, Column::COLUMN_MAX> columns_by_type;
    std::vector<Column::COLUMN_TYPES> joinable_types;

    for (auto *column : *table->columns_) {
      if (join_select_column(column)) {
        columns_by_type[column->type_].push_back(column);
      }
    }

    for (size_t type = 0; type < Column::COLUMN_MAX; ++type) {
      if (!columns_by_type[type].empty()) {
        joinable_types.push_back(static_cast<Column::COLUMN_TYPES>(type));
      }
    }

    if (joinable_types.empty()) {
      continue;
    }

    auto selected_type =
        joinable_types.at(rand_int(joinable_types.size() - 1, 0));
    auto &columns = columns_by_type[selected_type];
    auto *left_column = columns.at(rand_int(columns.size() - 1, 0));
    auto *right_column = columns.at(rand_int(columns.size() - 1, 0));
    if (columns.size() > 1 && left_column == right_column) {
      size_t right_position = rand_int(columns.size() - 2, 0);
      if (columns.at(right_position) == left_column) {
        right_position = columns.size() - 1;
      }
      right_column = columns.at(right_position);
    }

    candidates_seen++;
    if (candidates_seen == 1 ||
        rand_int(static_cast<int>(candidates_seen - 1), 0) == 0) {
      target.left_table = table;
      target.right_table = table;
      target.left_column = left_column;
      target.right_column = right_column;
    }
  }

  return candidates_seen > 0;
}

static bool pick_join_select_target(std::vector<Table *> *all_tables,
                                    JoinSelectTarget &target) {
  if (sample_join_target_from_fk(all_tables, target)) {
    return true;
  }
  if (sample_join_target_from_table_pairs(all_tables, target)) {
    return true;
  }
  return sample_self_join_target(all_tables, target);
}

static void select_with_join(std::vector<Table *> *all_tables, Thd1 *thd) {
  JoinSelectTarget target;
  if (!pick_join_select_target(all_tables, target) ||
      target.left_table == nullptr || target.right_table == nullptr ||
      target.left_column == nullptr || target.right_column == nullptr) {
    return;
  }

  std::string sql = "SELECT * FROM " + target.left_table->name_ +
                    " T1 INNER JOIN " + target.right_table->name_ +
                    " T2 ON T1." + target.left_column->name_ + " = T2." +
                    target.right_column->name_;

  std::string predicate;
  if (!build_aliased_hit_oriented_where(target.left_table, "T1", predicate)) {
    build_aliased_hit_oriented_where(target.right_table, "T2", predicate);
  }
  if (!predicate.empty()) {
    sql += " WHERE " + predicate;
  }

  sql += " LIMIT 100";
  execute_sql(sql, thd);
}

static int pick_random_where_column(const Table *table, bool prefer_primary_key);

static std::string random_read_source(Table *table,
                                      int partition_child_probability = 10) {
  if (table != nullptr && table->type == Table::PARTITION &&
      rand_int(100) < partition_child_probability) {
    return pg_partition_target(table);
  }
  return table == nullptr ? "" : table->name_;
}

// Caller must hold table->table_mutex while building a column-based predicate.
static bool build_read_predicate_locked(Table *table, std::string &predicate) {
  if (table == nullptr) {
    return false;
  }

  auto where = pick_random_where_column(table, false);
  if (where < 0) {
    return false;
  }

  if (build_hit_oriented_where(table, predicate)) {
    return true;
  }

  auto *column = table->columns_->at(where);
  predicate = column->name_;
  auto prob = rand_int(100);
  if (rand_int(1000) < 2) {
    predicate += " NOT BETWEEN " + column->rand_value() + " AND " +
                 column->rand_value();
  } else if (prob <= 90) {
    predicate += " = " + column->rand_value();
  } else if (prob <= 92) {
    predicate += " >= " + column->rand_value();
  } else if (prob <= 94) {
    predicate += " >= " + column->rand_value() + " AND " + column->name_ +
                 " <= " + column->rand_value();
  } else if (prob <= 96) {
    predicate += " IN (" + column->rand_value() + ", " + column->rand_value() +
                 ")";
  } else if (prob <= 98 && supports_like_predicate(column)) {
    predicate += " LIKE " + Table::prepare_like_string(column->rand_value());
  } else {
    predicate += " BETWEEN " + column->rand_value() + " AND " +
                 column->rand_value();
  }
  return true;
}

static std::string build_cte_base_query_locked(Table *table) {
  if (table == nullptr) {
    return "";
  }

  std::string sql = "SELECT * FROM " + random_read_source(table);
  std::string predicate;
  if (rand_int(99) < 70 && build_read_predicate_locked(table, predicate)) {
    sql += " WHERE " + predicate;
  }
  sql += " LIMIT " + std::to_string(rand_int(64, 1));
  return sql;
}

static void select_with_cte(std::vector<Table *> *all_tables, Thd1 *thd) {
  if (all_tables == nullptr || all_tables->empty()) {
    return;
  }

  auto *table = all_tables->at(rand_int(all_tables->size() - 1));
  if (table == nullptr) {
    return;
  }

  std::string base_query;
  {
    std::lock_guard<std::recursive_mutex> lock(table->table_mutex);
    base_query = build_cte_base_query_locked(table);
  }
  if (base_query.empty()) {
    return;
  }

  std::string sql;
  if (rand_int(99) < 50) {
    sql = "WITH cte_base AS (" + base_query + ") SELECT * FROM cte_base LIMIT " +
          std::to_string(rand_int(48, 1));
  } else {
    sql = "WITH cte_base AS (" + base_query +
          "), cte_window AS (SELECT * FROM cte_base LIMIT " +
          std::to_string(rand_int(48, 1)) +
          ") SELECT * FROM cte_window LIMIT " +
          std::to_string(rand_int(32, 1));
  }

  execute_sql(sql, thd);
}

static int pick_random_updatable_column(const Table *table) {
  std::vector<int> candidates;
  for (size_t i = 0; i < table->columns_->size(); ++i) {
    if (table->type == Table::PARTITION && i == 0) {
      continue;
    }
    if (table->columns_->at(i)->type_ != Column::GENERATED) {
      candidates.push_back(static_cast<int>(i));
    }
  }
  if (candidates.empty()) {
    return -1;
  }
  return candidates.at(rand_int(candidates.size() - 1));
}

static int pick_random_where_column(const Table *table, bool prefer_primary_key) {
  std::vector<int> preferred;
  std::vector<int> jsonb_columns;
  std::vector<int> bool_columns;
  std::vector<int> integer_columns;
  int primary_key = -1;
  bool only_bool = true;

  for (size_t i = 0; i < table->columns_->size(); ++i) {
    auto *column = table->columns_->at(i);
    if (column->primary_key) {
      primary_key = static_cast<int>(i);
    }
    if (column->type_ != Column::BOOL) {
      only_bool = false;
    }

    switch (column->type_) {
    case Column::SMALLINT:
    case Column::INT:
    case Column::BIGINT:
    case Column::NUMERIC:
    case Column::DATE:
    case Column::TIME:
    case Column::TIMETZ:
    case Column::TIMESTAMP:
    case Column::TIMESTAMPTZ:
    case Column::INTERVAL:
    case Column::BIT:
    case Column::VARBIT:
    case Column::INET:
    case Column::CIDR:
    case Column::MACADDR:
    case Column::MACADDR8:
    case Column::MONEY:
    case Column::FLOAT:
    case Column::DOUBLE:
    case Column::VARCHAR:
    case Column::CHAR:
    case Column::BLOB:
    case Column::BYTEA:
    case Column::UUID:
    case Column::GENERATED:
      preferred.push_back(static_cast<int>(i));
      break;
    case Column::JSONB:
      jsonb_columns.push_back(static_cast<int>(i));
      break;
    case Column::BOOL:
      bool_columns.push_back(static_cast<int>(i));
      break;
    case Column::INTEGER:
      integer_columns.push_back(static_cast<int>(i));
      break;
    case Column::JSON:
    case Column::XML:
    case Column::TSVECTOR:
    case Column::TSQUERY:
    case Column::POINT:
    case Column::LINE:
    case Column::LSEG:
    case Column::BOX:
    case Column::PATH:
    case Column::POLYGON:
    case Column::CIRCLE:
    case Column::INTARRAY:
    case Column::BIGINTARRAY:
    case Column::NUMERICARRAY:
    case Column::TEXTARRAY:
    case Column::BOOLARRAY:
    case Column::TIMESTAMPARRAY:
    case Column::INT4RANGE:
    case Column::INT8RANGE:
    case Column::NUMRANGE:
    case Column::TSRANGE:
    case Column::TSTZRANGE:
    case Column::DATERANGE:
    case Column::COLUMN_MAX:
      break;
    }
  }

  if (prefer_primary_key && primary_key != -1 && rand_int(100) <= 50) {
    return primary_key;
  }
  if (!preferred.empty()) {
    return preferred.at(rand_int(preferred.size() - 1));
  }
  if (!jsonb_columns.empty()) {
    return jsonb_columns.at(rand_int(jsonb_columns.size() - 1));
  }
  if (!integer_columns.empty()) {
    return integer_columns.at(rand_int(integer_columns.size() - 1));
  }
  if (!bool_columns.empty() && only_bool) {
    return bool_columns.at(rand_int(bool_columns.size() - 1));
  }
  if (!bool_columns.empty()) {
    return bool_columns.at(rand_int(bool_columns.size() - 1));
  }
  return -1;
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
  if (!resolve_reference_columns()) {
    thd->thread_log << "Failed to resolve fk reference columns for " << name_
                    << std::endl;
    run_query_failed = true;
    return false;
  }

  std::string constraint = name_ + "_" + parent->name_;

  std::string sql = "ALTER TABLE " + name_ + " ADD CONSTRAINT " + constraint +
                    " FOREIGN KEY (" + child_key->name_ + ") REFERENCES " +
                    parent->name_ + " (" + parent_key->name_ + ")";
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

bool FK_table::resolve_reference_columns() {
  if (parent == nullptr)
    return false;

  if (parent_key == nullptr && !parent_key_name.empty())
    parent_key = find_column_by_name(parent, parent_key_name);
  if (child_key == nullptr && !child_key_name.empty())
    child_key = find_column_by_name(this, child_key_name);

  if (parent_key == nullptr || child_key == nullptr) {
    auto pk_columns = primary_key_columns(parent);
    if (parent_key == nullptr && !pk_columns.empty())
      parent_key = pk_columns.front();
    if (child_key == nullptr) {
      for (auto *col : *columns_) {
        if (col->name_.find("fk_col") != std::string::npos) {
          child_key = col;
          break;
        }
      }
    }
  }

  if (parent_key == nullptr || child_key == nullptr)
    return false;

  parent_key_name = parent_key->name_;
  child_key_name = child_key->name_;
  return true;
}

bool FK_table::configure_reference() {
  if (parent == nullptr)
    return false;

  parent_key = pick_fk_parent_key(parent);
  if (parent_key == nullptr)
    return false;

  child_key = make_fk_child_key(parent_key, this);
  AddInternalColumn(child_key);
  parent_key_name = parent_key->name_;
  child_key_name = child_key->name_;

  if (!parent_key->primary_key) {
    parent_key->referenced_key = true;
    auto *unique_index =
        new Index(parent->name_ + "_fkref_" + parent_key->name_);
    unique_index->unique = true;
    unique_index->AddInternalColumn(new Ind_col(parent_key, false));
    parent->AddInternalIndex(unique_index);
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
  if (execute_sql(definition(), thd)) {
    clear_hit_value_cache(this);
  }
}

void Table::Optimize(Thd1 *thd) {
  execute_sql("ANALYZE " + (type == PARTITION ? pg_partition_target(this) : name_),
              thd);
}

void Table::Vacuum(Thd1 *thd) {
  execute_sql("VACUUM " + (type == PARTITION ? pg_partition_target(this) : name_),
              thd);
}

void Table::VacuumFull(Thd1 *thd) {
  if (referenced_by_fk(this)) {
    return;
  }
  if (execute_sql("VACUUM FULL " +
                      (type == PARTITION ? pg_partition_target(this) : name_),
                  thd)) {
    clear_hit_value_cache(this);
  }
}

void Table::Checkpoint(Thd1 *thd) {
  execute_sql("CHECKPOINT", thd);
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
  if (execute_sql("TRUNCATE TABLE " +
                      (type == PARTITION ? pg_partition_target(this) : name_),
                  thd)) {
    clear_hit_value_cache(this);
  }
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
        read_single_value("SELECT COALESCE(MAX(" + partition_key_column_name(this) +
                              "), 0) FROM " + max_child,
                          thd);
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
  auto generated_budget = generated_column_budget(max_columns);

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
      bool want_generated = false;

      if (!no_virtual_col && generated_column_allowed(this, max_columns)) {
        size_t remaining_slots = static_cast<size_t>(max_columns - i);
        size_t remaining_generated = generated_budget - count_generated_columns(this);
        if (remaining_generated > 0 && remaining_slots > 0 &&
            rand_int(static_cast<int>(remaining_slots - 1)) <
                static_cast<int>(remaining_generated)) {
          want_generated = true;
        }
      }

      /* loop untill we select some column */
      while (col_type == Column::COLUMN_MAX) {

        auto prob = rand_int(89);

        if (want_generated)
          col_type = Column::GENERATED;
        else if (prob < 3)
          col_type = Column::SMALLINT;
        else if (prob < 8)
          col_type = Column::INT;
        else if (prob < 9)
          col_type = Column::INTEGER;
        else if (prob < 12)
          col_type = Column::BIGINT;
        else if (prob < 15)
          col_type = Column::NUMERIC;
        else if (prob < 17)
          col_type = Column::FLOAT;
        else if (prob < 19)
          col_type = Column::DOUBLE;
        else if (prob < 23)
          col_type = Column::VARCHAR;
        else if (prob < 25)
          col_type = Column::CHAR;
        else if (prob < 27)
          col_type = Column::DATE;
        else if (prob < 29)
          col_type = Column::TIME;
        else if (prob < 30)
          col_type = Column::TIMETZ;
        else if (prob < 32)
          col_type = Column::TIMESTAMP;
        else if (prob < 34)
          col_type = Column::TIMESTAMPTZ;
        else if (prob < 36)
          col_type = Column::INTERVAL;
        else if (prob < 37)
          col_type = Column::BIT;
        else if (prob < 38)
          col_type = Column::VARBIT;
        else if (prob < 40)
          col_type = Column::INET;
        else if (prob < 42)
          col_type = Column::CIDR;
        else if (prob < 43)
          col_type = Column::MACADDR;
        else if (prob < 44)
          col_type = Column::MACADDR8;
        else if (prob < 46)
          col_type = Column::MONEY;
        else if (prob < 47)
          col_type = Column::XML;
        else if (prob < 49)
          col_type = Column::TSVECTOR;
        else if (prob < 51)
          col_type = Column::TSQUERY;
        else if (prob < 52)
          col_type = Column::POINT;
        else if (prob < 53)
          col_type = Column::LINE;
        else if (prob < 54)
          col_type = Column::LSEG;
        else if (prob < 55)
          col_type = Column::BOX;
        else if (prob < 56)
          col_type = Column::PATH;
        else if (prob < 57)
          col_type = Column::POLYGON;
        else if (prob < 58)
          col_type = Column::CIRCLE;
        else if (prob < 60)
          col_type = Column::INTARRAY;
        else if (prob < 62)
          col_type = Column::BIGINTARRAY;
        else if (prob < 64)
          col_type = Column::NUMERICARRAY;
        else if (prob < 66)
          col_type = Column::TEXTARRAY;
        else if (prob < 67)
          col_type = Column::BOOLARRAY;
        else if (prob < 68)
          col_type = Column::TIMESTAMPARRAY;
        else if (prob < 71)
          col_type = Column::INT4RANGE;
        else if (prob < 74)
          col_type = Column::INT8RANGE;
        else if (prob < 77)
          col_type = Column::NUMRANGE;
        else if (prob < 79)
          col_type = Column::TSRANGE;
        else if (prob < 81)
          col_type = Column::TSTZRANGE;
        else if (prob < 83)
          col_type = Column::DATERANGE;
        else if (!no_blob_col && prob < 85)
          col_type = Column::BLOB;
        else if (!no_blob_col && prob < 86)
          col_type = Column::BYTEA;
        else if (prob < 88)
          col_type = Column::BOOL;
        else if (prob < 89)
          col_type = Column::JSON;
        else if (prob < 90)
          col_type = Column::JSONB;
        else
          col_type = Column::UUID;
      }

      if (col_type == Column::GENERATED) {
        bool created_generated = false;
        col = make_generated_or_fallback_column(name, this, max_columns,
                                                created_generated);
      } else if (col_type == Column::BLOB)
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
  case UNLOGGED:
    table = new Table(name + UNLOGGED_SUFFIX);
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
  else if (type == UNLOGGED)
    def += " UNLOGGED";
  def += " TABLE " + name_ + " (";

  if (columns_->size() == 0)
    throw std::runtime_error("no column in table " + name_);

  /* add columns */
  for (auto col : *columns_) {
    def += col->definition() + ", ";
  }

  /* if column has primary key */
  const std::string partition_key =
      type == PARTITION ? partition_key_column_name(this) : "";
  for (auto col : *columns_) {
    if (col->primary_key) {
      def += " PRIMARY KEY(";
      if (type == PARTITION) {
        if (rand_int(1) == 0)
          def += col->name_ + ", " + partition_key;
        else
          def += partition_key + ", " + col->name_;
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
      def += " PARTITION BY RANGE (" + partition_key + ")";
      break;
    case Partition::LIST:
      def += " PARTITION BY LIST (" + partition_key + ")";
      break;
    case Partition::HASH:
    case Partition::KEY:
      def += " PARTITION BY HASH (" + partition_key + ")";
      break;
    }
  }
  return def;
}

/* create default table includes all tables*/
void generate_metadata_for_tables() {
  auto tables = opt_int(TABLES);

  auto only_temporary_tables = opt_bool(ONLY_TEMPORARY);
  auto only_unlogged_tables = opt_bool(ONLY_UNLOGGED);

  if (!only_temporary_tables) {
    for (int i = 1; i <= tables; i++) {
      if (!options->at(Option::ONLY_PARTITION)->getBool() &&
          !only_unlogged_tables) {
        auto parent_table = Table::table_id(Table::NORMAL, i);
        all_tables->push_back(parent_table);

        /* Create FK table */
        if (!options->at(Option::NO_FK)->getBool() &&
            options->at(Option::FK_PROB)->getInt() > rand_int(100) &&
            pick_fk_parent_key(parent_table) != nullptr) {
          auto child_table = Table::table_id(Table::FK, i);
          all_tables->push_back(child_table);
          static_cast<FK_table *>(child_table)->parent = parent_table;
          if (!static_cast<FK_table *>(child_table)->configure_reference()) {
            all_tables->pop_back();
            delete child_table;
          }
        }
      }

      if (!options->at(Option::NO_UNLOGGED)->getBool() &&
          options->at(Option::UNLOGGED_PROB)->getInt() > rand_int(100))
        all_tables->push_back(Table::table_id(Table::UNLOGGED, i));

      if (!options->at(Option::NO_PARTITION)->getBool() &&
          !only_unlogged_tables &&
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
      thd->connection_lost = true;
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
    if (is_partition_key_column(this, col1)) {
      i++;
      continue;
    }
    switch (col1->type_) {
    case Column::BLOB:
    case Column::BYTEA:
    case Column::VARCHAR:
    case Column::CHAR:
    case Column::SMALLINT:
    case Column::FLOAT:
    case Column::DOUBLE:
    case Column::INT:
    case Column::INTEGER:
    case Column::BIGINT:
    case Column::NUMERIC:
    case Column::DATE:
    case Column::TIME:
    case Column::TIMETZ:
    case Column::TIMESTAMP:
    case Column::TIMESTAMPTZ:
    case Column::INTERVAL:
    case Column::BIT:
    case Column::VARBIT:
    case Column::INET:
    case Column::CIDR:
    case Column::MACADDR:
    case Column::MACADDR8:
    case Column::MONEY:
    case Column::XML:
    case Column::TSVECTOR:
    case Column::TSQUERY:
    case Column::POINT:
    case Column::LINE:
    case Column::LSEG:
    case Column::BOX:
    case Column::PATH:
    case Column::POLYGON:
    case Column::CIRCLE:
    case Column::INTARRAY:
    case Column::BIGINTARRAY:
    case Column::NUMERICARRAY:
    case Column::TEXTARRAY:
    case Column::BOOLARRAY:
    case Column::TIMESTAMPARRAY:
    case Column::INT4RANGE:
    case Column::INT8RANGE:
    case Column::NUMRANGE:
    case Column::TSRANGE:
    case Column::TSTZRANGE:
    case Column::DATERANGE:
    case Column::BOOL:
    case Column::JSON:
    case Column::JSONB:
    case Column::UUID:
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
  if (is_partition_key_column(this, columns_->at(ps))) {
    table_mutex.unlock();
    return;
  }

  if (rand_int(100, 1) <= options->at(Option::PRIMARY_KEY)->getInt() &&
      name.find("pkey") != std::string::npos) {
    table_mutex.unlock();
    return;
  }

  std::string sql = "ALTER TABLE " + name_ + " DROP COLUMN " + name;
  table_mutex.unlock();

  if (execute_sql(sql, thd)) {
    table_mutex.lock();
    erase_hit_value_cache(this, name);

    std::vector<int> indexes_to_drop;
    for (auto id = indexes_->begin(); id != indexes_->end(); id++) {
      auto index = *id;

      for (auto id_col = index->columns_->begin();
           id_col != index->columns_->end(); id_col++) {
        auto ic = *id_col;
        if (ic->column->name_.compare(name) == 0) {
          delete index;
          indexes_to_drop.push_back(id - indexes_->begin());
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
  size_t total_columns_budget = columns_->size() + 1;

  // lock table to create definition
  table_mutex.lock();

  if (no_use_virtual ||
      (columns_->size() == 1 && columns_->at(0)->auto_increment == true) ||
      !generated_column_allowed(this, total_columns_budget))
    use_virtual = false;

  while (col_type == Column::COLUMN_MAX) {
    /* new columns are in ratio of 2:2:2:1:1:1:1:1
     * INT:VARCHAR:CHAR:BOOL:GENERATED:TIMESTAMP:JSON:BLOB */
    auto prob = rand_int(44);
    if (prob < 1)
      col_type = Column::INTEGER;
    else if (prob < 3)
      col_type = Column::INT;
    else if (prob < 4)
      col_type = Column::BIGINT;
    else if (prob < 5)
      col_type = Column::NUMERIC;
    else if (prob < 7)
      col_type = Column::VARCHAR;
    else if (prob < 8)
      col_type = Column::CHAR;
    else if (prob < 9 && use_virtual)
      col_type = Column::GENERATED;
    else if (prob < 10)
      col_type = Column::BOOL;
    else if (prob < 11)
      col_type = Column::TIMESTAMP;
    else if (prob < 12)
      col_type = Column::DATE;
    else if (prob < 13)
      col_type = Column::TIME;
    else if (prob < 14)
      col_type = Column::INET;
    else if (prob < 15)
      col_type = Column::CIDR;
    else if (prob < 16)
      col_type = Column::MACADDR;
    else if (prob < 17)
      col_type = Column::MACADDR8;
    else if (prob < 18)
      col_type = Column::BIT;
    else if (prob < 19)
      col_type = Column::VARBIT;
    else if (prob < 20)
      col_type = Column::MONEY;
    else if (prob < 21)
      col_type = Column::XML;
    else if (prob < 22)
      col_type = Column::TSVECTOR;
    else if (prob < 23)
      col_type = Column::TSQUERY;
    else if (prob < 24)
      col_type = Column::POINT;
    else if (prob < 25)
      col_type = Column::LINE;
    else if (prob < 26)
      col_type = Column::LSEG;
    else if (prob < 27)
      col_type = Column::BOX;
    else if (prob < 28)
      col_type = Column::PATH;
    else if (prob < 29)
      col_type = Column::POLYGON;
    else if (prob < 30)
      col_type = Column::CIRCLE;
    else if (prob < 32)
      col_type = Column::INTARRAY;
    else if (prob < 33)
      col_type = Column::BIGINTARRAY;
    else if (prob < 34)
      col_type = Column::NUMERICARRAY;
    else if (prob < 35)
      col_type = Column::TEXTARRAY;
    else if (prob < 36)
      col_type = Column::BOOLARRAY;
    else if (prob < 37)
      col_type = Column::TIMESTAMPARRAY;
    else if (prob < 38)
      col_type = Column::INT4RANGE;
    else if (prob < 39)
      col_type = Column::INT8RANGE;
    else if (prob < 40)
      col_type = Column::NUMRANGE;
    else if (prob < 41)
      col_type = Column::TSRANGE;
    else if (prob < 42)
      col_type = Column::TSTZRANGE;
    else if (prob < 43)
      col_type = Column::DATERANGE;
    else if (prob < 44)
      col_type = Column::JSON;
    else if (prob < 45 && use_blob)
      col_type = Column::BLOB;
  }

  Column *tc = nullptr;

  for (int attempt = 0; attempt < 64 && tc == nullptr; ++attempt) {
    std::string name =
        "N" + std::to_string(rand_int(100000, 1000)) + "_" + std::to_string(attempt);

    if (col_type == Column::GENERATED) {
      bool created_generated = false;
      tc = make_generated_or_fallback_column(name, this, total_columns_budget,
                                             created_generated);
    } else if (col_type == Column::BLOB)
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
    std::vector<Index *> droppable_indexes;
    for (auto *index : *indexes_) {
      if (!fk_supporting_index(index))
        droppable_indexes.push_back(index);
    }
    if (droppable_indexes.empty()) {
      table_mutex.unlock();
      return;
    }
    auto index = droppable_indexes.at(rand_int(droppable_indexes.size() - 1));
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

void Table::AddIndexConcurrently(Thd1 *thd) {
  static size_t max_columns = opt_int(INDEX_COLUMNS);
  table_mutex.lock();
  Index *id = nullptr;
  for (int attempt = 0; attempt < 64 && id == nullptr; ++attempt) {
    auto i = rand_int(100000, 1000);
    std::string candidate = name_ + "coni" + std::to_string(i);
    if (!index_name_exists(this, candidate))
      id = new Index(candidate);
  }

  if (id == nullptr) {
    table_mutex.unlock();
    return;
  }

  int no_of_columns = rand_int(
      (max_columns < columns_->size() ? max_columns : columns_->size()), 1);
  no_of_columns = std::min(no_of_columns, 4);

  std::vector<int> col_pos;
  size_t attempts = 0;
  while (col_pos.size() < (size_t)no_of_columns &&
         attempts++ < columns_->size() * 8) {
    int current = rand_int(columns_->size() - 1);
    if (!pg_indexable_column(columns_->at(current)))
      continue;
    bool already_added = false;
    for (auto id : col_pos) {
      if (id == current)
        already_added = true;
    }
    if (!already_added)
      col_pos.push_back(current);
  }

  for (auto pos : col_pos) {
    auto col = columns_->at(pos);
    bool column_desc = rand_int(100) < DESC_INDEXES_IN_COLUMN;
    id->AddInternalColumn(new Ind_col(col, column_desc));
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

  std::string sql = "CREATE " + std::string(id->unique ? "UNIQUE " : "") +
                    "INDEX CONCURRENTLY " + id->name_ + " ON " + name_ + "(" +
                    index_column_list(id) + ")";
  table_mutex.unlock();

  if (execute_sql(sql, thd)) {
    table_mutex.lock();
    auto do_not_add = false;
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

void Table::Reindex(Thd1 *thd) {
  std::string target = type == PARTITION ? pg_partition_target(this) : name_;
  if (rand_int(2) == 0) {
    execute_sql("REINDEX TABLE " + target, thd);
    return;
  }

  table_mutex.lock();
  if (!indexes_->empty()) {
    auto *index = indexes_->at(rand_int(indexes_->size() - 1));
    std::string idx_name = index->name_;
    table_mutex.unlock();
    execute_sql("REINDEX INDEX CONCURRENTLY " + idx_name, thd);
  } else {
    table_mutex.unlock();
    execute_sql("REINDEX TABLE CONCURRENTLY " + target, thd);
  }
}

void Table::ClusterTable(Thd1 *thd) {
  table_mutex.lock();
  if (indexes_->empty() || type == Table::FK) {
    table_mutex.unlock();
    return;
  }

  auto *idx = indexes_->at(rand_int(indexes_->size() - 1));
  std::string target = type == PARTITION ? pg_partition_target(this) : name_;
  std::string sql = "CLUSTER " + target + " USING " + idx->name_;
  table_mutex.unlock();

  execute_sql(sql, thd);
}

void Table::AddBrinExpressionIndex(Thd1 *thd) {
  table_mutex.lock();
  if (columns_->empty()) {
    table_mutex.unlock();
    return;
  }

  int index_type = rand_int(4);
  std::string sql;

  if (index_type <= 2) {
    std::vector<int> candidates;
    for (size_t i = 0; i < columns_->size(); ++i) {
      auto *col = columns_->at(i);
      if (col->type_ == Column::INT || col->type_ == Column::INTEGER ||
          col->type_ == Column::BIGINT || col->type_ == Column::NUMERIC ||
          col->type_ == Column::TIMESTAMP ||
          col->type_ == Column::TIMESTAMPTZ || col->type_ == Column::DATE) {
        candidates.push_back(static_cast<int>(i));
      }
    }
    if (!candidates.empty()) {
      auto *col = columns_->at(candidates.at(rand_int(candidates.size() - 1)));
      std::string idx_name =
          "brin_" + name_ + "_" + col->name_ + "_" + std::to_string(rand_int(100000));
      int pages_per_range = rand_int(128, 4);
      sql = "CREATE INDEX " + idx_name + " ON " + name_ + " USING brin (" +
            col->name_ + ") WITH (pages_per_range = " +
            std::to_string(pages_per_range) + ")";
    }
  } else if (index_type == 3) {
    std::vector<int> candidates;
    for (size_t i = 0; i < columns_->size(); ++i) {
      auto *col = columns_->at(i);
      if (col->type_ == Column::VARCHAR || col->type_ == Column::CHAR ||
          col->type_ == Column::INT || col->type_ == Column::INTEGER) {
        candidates.push_back(static_cast<int>(i));
      }
    }
    if (!candidates.empty()) {
      auto *col = columns_->at(candidates.at(rand_int(candidates.size() - 1)));
      std::string idx_name =
          "expr_" + name_ + "_" + col->name_ + "_" + std::to_string(rand_int(100000));
      std::string expr = col->name_;
      if (col->type_ == Column::VARCHAR || col->type_ == Column::CHAR)
        expr = "lower(" + col->name_ + ")";
      else if (col->type_ == Column::INT || col->type_ == Column::INTEGER)
        expr = "abs(" + col->name_ + ")";
      sql = "CREATE INDEX " + idx_name + " ON " + name_ + " (" + expr + ")";
    }
  } else {
    std::vector<int> candidates;
    for (size_t i = 0; i < columns_->size(); ++i) {
      auto *col = columns_->at(i);
      if (col->type_ == Column::JSONB || col->type_ == Column::INTARRAY ||
          col->type_ == Column::TEXTARRAY) {
        candidates.push_back(static_cast<int>(i));
      }
    }
    if (!candidates.empty()) {
      auto *col = columns_->at(candidates.at(rand_int(candidates.size() - 1)));
      std::string idx_name =
          "gin_" + name_ + "_" + col->name_ + "_" + std::to_string(rand_int(100000));
      std::string opclass = col->type_ == Column::JSONB ? " jsonb_path_ops" : "";
      sql = "CREATE INDEX " + idx_name + " ON " + name_ + " USING gin (" +
            col->name_ + opclass + ")";
    }
  }

  table_mutex.unlock();
  if (!sql.empty())
    execute_sql(sql, thd);
}

void Table::DeleteAllRows(Thd1 *thd) {
  std::string sql = "DELETE FROM " + name_;
  if (type == PARTITION && rand_int(100) < 98) {
    if (execute_sql("DELETE FROM " + pg_partition_target(this), thd)) {
      clear_hit_value_cache(this);
    }
    return;
  }
  if (execute_sql(sql, thd)) {
    clear_hit_value_cache(this);
  }
}

void Table::SelectAllRow(Thd1 *thd) {
  execute_sql("SELECT * FROM " + random_read_source(this, 98), thd);
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
  if (is_partition_key_column(this, columns_->at(ps))) {
    table_mutex.unlock();
    return;
  }
  /* ALTER column to _rename or back to orignal_name */
  std::string new_name = "_rename";
  static auto s = new_name.size();
  if (name.size() > s && name.substr(name.length() - s).compare("_rename") == 0)
    new_name = name.substr(0, name.length() - s);
  else
    new_name = name + new_name;
  if (column_name_exists(this, new_name)) {
    table_mutex.unlock();
    return;
  }
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
    rename_hit_value_cache(this, name, new_name);
  }
}

void Table::DeleteRandomRow(Thd1 *thd) {
  table_mutex.lock();
  auto where = pick_random_where_column(this, true);
  if (where < 0) {
    table_mutex.unlock();
    return;
  }
  std::string sql = "DELETE FROM " + name_;

  if (type == PARTITION && rand_int(10) < 2)
    sql = "DELETE FROM " + pg_partition_target(this);
  std::string predicate;
  if (!build_hit_oriented_where(this, predicate)) {
    predicate = columns_->at(where)->name_;
    auto prob = rand_int(100);
    if (prob <= 90)
      predicate += " = " + columns_->at(where)->rand_value();
    else if (prob <= 92)
      predicate += " >= " + columns_->at(where)->rand_value() + " AND " +
                   columns_->at(where)->name_ +
                   " <= " + columns_->at(where)->rand_value();
    else if (prob <= 96)
      predicate += " IN (" + columns_->at(where)->rand_value() + "," +
                   columns_->at(where)->rand_value() + ")";
    else if (prob <= 99)
      predicate += " BETWEEN " + columns_->at(where)->rand_value() + " AND " +
                   columns_->at(where)->rand_value();
    else if (supports_like_predicate(columns_->at(where)))
      predicate += " LIKE " +
                   prepare_like_string(columns_->at(where)->rand_value());
    else
      predicate += " = " + columns_->at(where)->rand_value();
  }
  sql += " WHERE " + predicate;
  if (use_returning_old_new())
    sql += " RETURNING old.*";

  table_mutex.unlock();
  execute_sql(sql, thd);
}

void Table::SelectRandomRow(Thd1 *thd) {
  table_mutex.lock();
  std::string predicate;
  if (!build_read_predicate_locked(this, predicate)) {
    table_mutex.unlock();
    return;
  }
  std::string sql = "SELECT * FROM " + random_read_source(this) + " WHERE " +
                    predicate;

  table_mutex.unlock();
  execute_sql(sql, thd);
}

/* update random row */
void Table::UpdateRandomROW(Thd1 *thd) {
  table_mutex.lock();
  int set = pick_random_updatable_column(this);
  if (set < 0) {
    table_mutex.unlock();
    return;
  }

  auto where = pick_random_where_column(this, true);
  if (where < 0) {
    table_mutex.unlock();
    return;
  }
  std::string sql = "UPDATE " + name_;

  if (type == PARTITION && rand_int(10) < 2)
    sql = "UPDATE " + pg_partition_target(this);

  auto set_value =
      type == TABLE_TYPES::FK &&
              static_cast<FK_table *>(this)->child_key == columns_->at(set)
          ? fk_reference_value_expr(this)
          : columns_->at(set)->referenced_key
                ? random_unique_value_expr(columns_->at(set))
          : columns_->at(set)->rand_value();
  sql += " SET " + columns_->at(set)->name_ + " = " + set_value + " WHERE ";
  std::string predicate;
  if (!build_hit_oriented_where(this, predicate)) {
    auto prob = rand_int(100);
    if (prob <= 90)
      predicate =
          columns_->at(where)->name_ + " = " + columns_->at(where)->rand_value();
    else if (prob <= 92)
      predicate = columns_->at(where)->name_ +
                  " >= " + columns_->at(where)->rand_value() + " AND " +
                  columns_->at(where)->name_ +
                  " >= " + columns_->at(where)->rand_value();
    else if (prob <= 94)
      predicate = columns_->at(where)->name_ + " IN (" +
                  columns_->at(where)->rand_value() + "," +
                  columns_->at(where)->rand_value() + ")";
    else if (prob <= 98)
      predicate = columns_->at(where)->name_ + " BETWEEN " +
                  columns_->at(where)->rand_value() + " AND " +
                  columns_->at(where)->rand_value();
    else if (supports_like_predicate(columns_->at(where)))
      predicate = columns_->at(where)->name_ + " LIKE " +
                  prepare_like_string(columns_->at(where)->rand_value());
    else
      predicate = columns_->at(where)->name_ + " = " +
                  columns_->at(where)->rand_value();
  }
  sql += predicate;
  if (use_returning_old_new())
    sql += " RETURNING old.*, new.*";

  table_mutex.unlock();
  execute_sql(sql, thd);
}

bool Table::InsertBulkRecord(Thd1 *thd) {
  // if parent has no records, child can't have records
  if (type == FK) {
    if (static_cast<FK_table *>(this)->parent->number_of_initial_records == 0)
      number_of_initial_records = 0;
  }

  if (number_of_initial_records == 0)
    return true;

  std::string prepare_sql = "INSERT ";

  if (has_pk()) {
    thd->unique_keys = generateUniqueRandomNumbers(number_of_initial_records);
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
  std::vector<std::vector<std::pair<Column *, std::string>>> batch_cache_entries;
  int records = 0;

  while (records < number_of_initial_records) {
    std::string value = "(";
    std::vector<std::pair<Column *, std::string>> row_cache_entries;
    for (const auto &column : *columns_) {
      std::string value_expr;
      if (type == TABLE_TYPES::FK &&
          static_cast<FK_table *>(this)->child_key == column) {
        value_expr = fk_reference_value_expr(this);
      } else if (column->type_ == Column::COLUMN_TYPES::GENERATED) {
        value_expr = "DEFAULT";
      } else if (column->primary_key) {
        value_expr = std::to_string(thd->unique_keys.at(records));
      } else if (column->referenced_key) {
        value_expr = deterministic_unique_value(column, records);
      } else if (list_partition_key_value_expr(this, column, value_expr)) {
      } else if (column->auto_increment == true) {
        value_expr = "DEFAULT";
      } else {
        value_expr = column->rand_value();
      }

      value += value_expr;
      value += ", ";
      row_cache_entries.push_back({column, value_expr});
    }
    value.erase(value.size() - 2);
    value += ")";
    values += value;
    batch_cache_entries.push_back(std::move(row_cache_entries));
    records++;
    if (values.size() > 1024 * 1024 || number_of_initial_records == records) {
      if (!execute_sql(prepare_sql + values, thd)) {
        ddl_logs_write.lock();
        thd->ddl_logs << "Bulk insert failed for table  " << name_ << std::endl;
        ddl_logs_write.unlock();
        run_query_failed = true;
        return false;
      }
      for (const auto &row_entries : batch_cache_entries) {
        for (const auto &entry : row_entries) {
          remember_hit_value(this, entry.first, entry.second);
        }
      }
      batch_cache_entries.clear();
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
  std::vector<Column *> value_columns;
  std::vector<std::string> value_exprs;
  for (auto &column : *columns_) {
    sql += column->name_ + " ,";
    column_names.push_back(column->name_);
    value_columns.push_back(column);
    std::string val;
    bool used_list_partition_key = false;
    if (type == TABLE_TYPES::FK &&
        static_cast<FK_table *>(this)->child_key == column)
      val = fk_reference_value_expr(this);
    else if (column->type_ == Column::COLUMN_TYPES::GENERATED)
      val = "default";
    else if (column->referenced_key)
      val = random_unique_value_expr(column);
    else {
      used_list_partition_key = list_partition_key_value_expr(this, column, val);
      if (!used_list_partition_key)
        val = column->rand_value();
    }
    if (column->auto_increment == true && !used_list_partition_key &&
        rand_int(100) < 10)
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

  if (type != TABLE_TYPES::PARTITION && rand_int(3) != 0) {
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
  if (use_returning_old_new())
    sql += " RETURNING old.*, new.*";

  table_mutex.unlock();
  if (execute_sql(sql, thd)) {
    for (size_t i = 0; i < value_columns.size(); ++i) {
      remember_hit_value(this, value_columns[i], value_exprs[i]);
    }
  }
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

static void pg18_functions(Thd1 *thd) {
  if (!pg_server_at_least(thd, 18))
    return;

  static const std::vector<std::string> sqls = {
      "SELECT uuidv7(), uuidv4()",
      "SELECT array_sort(ARRAY[3, 1, 2]), array_reverse(ARRAY[1, 2, 3])",
      "SELECT reverse('\\\\x123456'::bytea)",
      "SELECT casefold(U&'Stra\\00DFe')",
      "SELECT crc32('postgres'::bytea), crc32c('postgres'::bytea)",
      "SELECT gamma(5.0), lgamma(5.0)",
      "SELECT jsonb_strip_nulls('{\"a\": null, \"b\": [1, null], "
      "\"c\": {\"d\": null}}'::jsonb, true)",
      "SELECT EXTRACT(WEEK FROM TIMESTAMP '2026-01-05')"};

  execute_sql(sqls.at(rand_int(sqls.size() - 1)), thd);
}

static void pg18_explain(Table *table, Thd1 *thd) {
  if (!pg_server_at_least(thd, 18) || table == nullptr)
    return;

  table->table_mutex.lock();
  auto source = random_read_source(table);
  table->table_mutex.unlock();

  if (source.empty())
    return;

  static const std::vector<std::string> explain_options = {
      "MEMORY",
      "ANALYZE, SERIALIZE TEXT",
      "ANALYZE, WAL"};
  auto option = explain_options.at(rand_int(explain_options.size() - 1));
  auto sql = "EXPLAIN (" + option + ") SELECT * FROM " + source + " LIMIT " +
             std::to_string(rand_int(64, 1));
  execute_sql(sql, thd);
}

static void create_matview(Table *table, Thd1 *thd) {
  std::string mv_name =
      "mv_" + table->name_ + "_" + std::to_string(rand_int(100000, 1000));

  table->table_mutex.lock();
  std::vector<std::string> col_names;
  for (auto *col : *table->columns_)
    col_names.push_back(col->name_);
  table->table_mutex.unlock();

  if (col_names.empty())
    return;

  int num_cols = rand_int(std::min(4, static_cast<int>(col_names.size())), 1);
  std::string proj_cols;
  std::vector<int> picked;
  for (int i = 0; i < num_cols; ++i) {
    int idx = rand_int(col_names.size() - 1);
    bool duplicate = false;
    for (auto p : picked) {
      if (p == idx)
        duplicate = true;
    }
    if (duplicate)
      continue;
    picked.push_back(idx);
    if (!proj_cols.empty())
      proj_cols += ", ";
    proj_cols += col_names[idx];
  }

  if (proj_cols.empty())
    proj_cols = "*";

  execute_sql("CREATE MATERIALIZED VIEW " + mv_name + " AS SELECT " +
                  proj_cols + " FROM " + table->name_ + " WITH DATA",
              thd);
}

static void refresh_matview_concurrently(Table *, Thd1 *thd) {
  auto mv_name = read_single_value(
      "SELECT matviewname FROM pg_matviews WHERE schemaname = current_schema() "
      "ORDER BY random() LIMIT 1",
      thd);
  if (mv_name.empty())
    return;

  if (!execute_sql("REFRESH MATERIALIZED VIEW CONCURRENTLY " + mv_name, thd))
    execute_sql("REFRESH MATERIALIZED VIEW " + mv_name, thd);
}

void Table::SelectMatview(Thd1 *thd) {
  auto mv_name = read_single_value(
      "SELECT matviewname FROM pg_matviews WHERE schemaname = current_schema() "
      "ORDER BY random() LIMIT 1",
      thd);
  if (mv_name.empty())
    return;

  execute_sql("SELECT * FROM " + mv_name + " LIMIT " +
                  std::to_string(rand_int(50, 1)),
              thd);
}

void Table::DropMatview(Thd1 *thd) {
  auto mv_name = read_single_value(
      "SELECT matviewname FROM pg_matviews WHERE schemaname = current_schema() "
      "ORDER BY random() LIMIT 1",
      thd);
  if (mv_name.empty())
    return;

  execute_sql("DROP MATERIALIZED VIEW IF EXISTS " + mv_name, thd);
}

static void prepared_tx_stress(Table *table, Thd1 *thd) {
  if (table->type == Table::TEMPORARY)
    return;

  std::string tx_name = "pstress_tx_" + std::to_string(rand_int(100000, 1000));
  execute_sql("BEGIN", thd);

  switch (rand_int(3)) {
  case 0:
    table->InsertRandomRow(thd);
    break;
  case 1:
    table->UpdateRandomROW(thd);
    break;
  case 2:
    table->DeleteRandomRow(thd);
    break;
  default:
    table->InsertRandomRow(thd);
    break;
  }

  if (thd->connection_lost)
    return;

  if (!execute_sql("PREPARE TRANSACTION '" + tx_name + "'", thd)) {
    execute_sql("ROLLBACK", thd);
    return;
  }

  if (rand_int(100) < 60)
    execute_sql("COMMIT PREPARED '" + tx_name + "'", thd);
  else
    execute_sql("ROLLBACK PREPARED '" + tx_name + "'", thd);
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
    } else if (table_type.compare("UNLOGGED") == 0) {
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
          type.compare("TIMESTAMP WITH TIME ZONE") == 0 ||
          type.compare("TIMESTAMPTZ") == 0 || type.compare("DATE") == 0 ||
          type.compare("TIME") == 0 ||
          type.compare("TIME WITHOUT TIME ZONE") == 0 ||
          type.compare("TIME WITH TIME ZONE") == 0 ||
          type.compare("TIMETZ") == 0 || type.compare("INTERVAL") == 0 ||
          type.compare("SMALLINT") == 0 || type.compare("BIGINT") == 0 ||
          type.compare("DECIMAL") == 0 || type.compare("NUMERIC") == 0 ||
          type.compare("BIT") == 0 ||
          type.compare("BIT VARYING") == 0 || type.compare("VARBIT") == 0 ||
          type.compare("INET") == 0 || type.compare("CIDR") == 0 ||
          type.compare("MACADDR") == 0 || type.compare("MACADDR8") == 0 ||
          type.compare("MONEY") == 0 || type.compare("XML") == 0 ||
          type.compare("POINT") == 0 || type.compare("LINE") == 0 ||
          type.compare("LSEG") == 0 || type.compare("BOX") == 0 ||
          type.compare("PATH") == 0 || type.compare("POLYGON") == 0 ||
          type.compare("CIRCLE") == 0 ||
          type.compare("TSVECTOR") == 0 || type.compare("TSQUERY") == 0 ||
          type.compare("BYTEA") == 0 || type.compare("UUID") == 0 ||
          type.compare("JSON") == 0 || type.compare("JSONB") == 0 ||
          type.compare("INT[]") == 0 || type.compare("INTEGER[]") == 0 ||
          type.compare("BIGINT[]") == 0 ||
          type.compare("NUMERIC[]") == 0 || type.compare("TEXT[]") == 0 ||
          type.compare("BOOLEAN[]") == 0 || type.compare("BOOL[]") == 0 ||
          type.compare("TIMESTAMP WITHOUT TIME ZONE") == 0 ||
          type.compare("TIMESTAMP[]") == 0 ||
          type.compare("INT4RANGE") == 0 ||
          type.compare("INT8RANGE") == 0 ||
          type.compare("NUMRANGE") == 0 ||
          type.compare("TSRANGE") == 0 ||
          type.compare("TSTZRANGE") == 0 ||
          type.compare("DATERANGE") == 0) {
        a = new Column(col["name"].GetString(), type, table);
      } else if (type.compare("GENERATED") == 0) {
        auto name = col["name"].GetString();
        auto clause = col["clause"].GetString();
        auto sub_type = col["sub_type"].GetString();
        auto generated_kind =
            metadata_string_member(col, "generated_kind", nullptr, "stored");
        a = new Generated_Column(name, table, clause, sub_type, generated_kind);
      } else if (type.compare("BLOB") == 0 || type.compare("TEXT") == 0) {
        auto sub_type = col["sub_type"].GetString();
        a = new Blob_Column(col["name"].GetString(), table, sub_type);
      } else
        throw std::runtime_error("unhandled column type");

      a->null = col["null"].GetBool();
      a->auto_increment = col["auto_increment"].GetBool();
      a->length = metadata_int_member(col, "length", "lenght");
      a->primary_key = col["primary_key"].GetBool();
      a->referenced_key = metadata_bool_member(col, "referenced_key");
      a->compressed = col["compressed"].GetBool();
      table->AddInternalColumn(a);
    }

    for (auto &ind : tab["indexes"].GetArray()) {
      Index *index = new Index(ind["name"].GetString());
      index->unique = metadata_bool_member(ind, "unique");

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

    if (table->type == Table::FK) {
      auto *fk_table = static_cast<FK_table *>(table);
      fk_table->parent_key_name = metadata_string_member(tab, "parent_key", nullptr);
      fk_table->child_key_name = metadata_string_member(tab, "child_key", nullptr);
      fk_table->resolve_reference_columns();
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
                                                Table::PARTITION,
                                                Table::UNLOGGED};
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
      /* Load normal tables before FK tables so FK values can select from the
       * parent reference column during initial child-table load. */

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
  std::vector<Table *> trx_ddl_tables;
  while (std::chrono::system_clock::now() < end) {
    if (trx_left == 0 && transactional_ddl_enabled() &&
        rand_int(1000) < options->at(Option::TRX_DDL_PROB_K)->getInt()) {
      std::lock_guard<std::mutex> ddl_guard(ddl_workload_mutex);
      run_transactional_ddl_block(this, trx_ddl_tables);
      if (this->connection_lost) {
        break;
      }
      if (run_query_failed) {
        break;
      }
      continue;
    }

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

    std::unique_lock<std::mutex> ddl_guard;
    if (ddl_query) {
      ddl_guard = std::unique_lock<std::mutex>(ddl_workload_mutex);
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
    case Option::SELECT_WITH_JOIN:
      select_with_join(all_session_tables, this);
      break;
    case Option::SELECT_WITH_CTE:
      select_with_cte(all_session_tables, this);
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
    case Option::VACUUM_TABLE:
      table->Vacuum(this);
      break;
    case Option::VACUUM_FULL:
      table->VacuumFull(this);
      break;
    case Option::CHECKPOINT:
      table->Checkpoint(this);
      break;
    case Option::CREATE_INDEX_CONCURRENTLY:
      table->AddIndexConcurrently(this);
      break;
    case Option::REINDEX:
      table->Reindex(this);
      break;
    case Option::CLUSTER_TABLE:
      table->ClusterTable(this);
      break;
    case Option::BRIN_EXPRESSION_INDEX:
      table->AddBrinExpressionIndex(this);
      break;
    case Option::CREATE_MATVIEW:
      create_matview(table, this);
      break;
    case Option::REFRESH_MATVIEW_CONCURRENTLY:
      refresh_matview_concurrently(table, this);
      break;
    case Option::SELECT_MATVIEW:
      table->SelectMatview(this);
      break;
    case Option::DROP_MATVIEW:
      table->DropMatview(this);
      break;
    case Option::PREPARED_TRANSACTION_STRESS:
      prepared_tx_stress(table, this);
      break;
    case Option::PG18_EXPLAIN:
      pg18_explain(table, this);
      break;
    case Option::PG18_FUNCTIONS:
      pg18_functions(this);
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

    if (this->connection_lost) {
      break;
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

  if (!trx_ddl_tables.empty()) {
    std::lock_guard<std::mutex> ddl_guard(ddl_workload_mutex);
    ddl_query = true;
    for (auto *table : trx_ddl_tables) {
      execute_sql("DROP TABLE IF EXISTS " + table->name_, this);
      delete table;
    }
    trx_ddl_tables.clear();
    ddl_query = false;
  }

  /* cleanup session temporary tables tables */
  for (auto &table : *all_session_tables)
    if (table->type == Table::TEMPORARY)
      delete table;
  delete all_session_tables;
  return true;
}
