#include "admin_commands.h"

#include <iostream>
#include <memory>

#include "../util.h"
#include "opaquedb/admin/keyring_store.h"
#include "opaquedb/admin/service.h"
#include "opaquedb/storage/catalog.h"
#include "opaquedb/storage/repository.h"

namespace opaquedb::cli {
namespace {

// Builds an AdminService facade over one table's local repository. The
// repository and keyring are returned so the caller keeps them alive for the
// facade.
struct LocalAdmin {
  std::unique_ptr<storage::LocalEpochRepository> repo;
  admin::InMemoryKeyringStore keyring;
  std::unique_ptr<admin::AdminService> admin;
};

absl::StatusOr<std::unique_ptr<LocalAdmin>>
OpenLocalAdmin(const config::Config &config, const std::string &database,
               const std::string &table) {
  auto bundle = std::make_unique<LocalAdmin>();
  absl::StatusOr<std::unique_ptr<storage::LocalEpochRepository>> repo =
      storage::LocalEpochRepository::Open(config.EpochRootFor(database, table));
  if (!repo.ok())
    return repo.status();
  bundle->repo = *std::move(repo);
  bundle->admin = std::make_unique<admin::AdminService>(
      config, bundle->repo.get(), &bundle->keyring);
  return bundle;
}

} // namespace

void StatusCommand::Register(CLI::App &parent, const GlobalOptions &globals,
                             int &exit_code) {
  CLI::App *status =
      parent.add_subcommand("status", "Show node and table status");
  status->add_option("--database", database_, "Database (default \"default\")");
  status->add_option("--table", table_, "Table to report on")->required();
  status->callback([this, &globals, &exit_code]() {
    absl::StatusOr<config::Config> config = LoadConfig(globals);
    if (!config.ok()) {
      std::cerr << "status: " << config.status().message() << "\n";
      exit_code = 1;
      return;
    }
    absl::StatusOr<std::unique_ptr<LocalAdmin>> admin =
        OpenLocalAdmin(*config, database_, table_);
    if (!admin.ok()) {
      std::cerr << "status: " << admin.status().message() << "\n";
      exit_code = 1;
      return;
    }
    absl::StatusOr<admin::AdminService::NodeStatus> s =
        (*admin)->admin->GetStatus();
    if (!s.ok()) {
      std::cerr << "status: " << s.status().message() << "\n";
      exit_code = 1;
      return;
    }
    std::cout << "table: " << database_ << "." << table_ << "\n";
    std::cout << "role: " << s->role << "\n";
    std::cout << "auth_mode: " << s->auth_mode << "\n";
    std::cout << "epoch: "
              << (s->has_epoch ? std::to_string(s->epoch_version) : "none")
              << "\n";
    std::cout << "rows: " << s->row_count << "\n";
    std::cout << "backends:";
    for (const std::string &b : s->backends)
      std::cout << " " << b;
    std::cout << "\n";
  });
}

void SchemaCommand::Register(CLI::App &parent, const GlobalOptions &globals,
                             int &exit_code) {
  CLI::App *schema =
      parent.add_subcommand("schema", "Inspect the table schema");
  schema->require_subcommand(1);
  CLI::App *inspect =
      schema->add_subcommand("inspect", "Show the current epoch schema");
  inspect->add_option("--database", database_,
                      "Database (default \"default\")");
  inspect->add_option("--table", table_, "Table to inspect")->required();
  inspect->callback([this, &globals, &exit_code]() {
    absl::StatusOr<config::Config> config = LoadConfig(globals);
    if (!config.ok()) {
      std::cerr << "schema: " << config.status().message() << "\n";
      exit_code = 1;
      return;
    }
    absl::StatusOr<std::unique_ptr<LocalAdmin>> admin =
        OpenLocalAdmin(*config, database_, table_);
    if (!admin.ok()) {
      std::cerr << "schema: " << admin.status().message() << "\n";
      exit_code = 1;
      return;
    }
    absl::StatusOr<core::Schema> s = (*admin)->admin->InspectSchema();
    if (!s.ok()) {
      std::cerr << "schema: " << s.status().message() << "\n";
      exit_code = 1;
      return;
    }
    std::cout << "table: " << s->table() << "\n";
    for (const core::Column &column : s->columns()) {
      const char *role = "";
      if (column.encoding == core::ColumnEncoding::kEq)
        role = " (key)";
      else if (column.encoding == core::ColumnEncoding::kIndex)
        role = " (index)";
      std::cout << "  " << column.name << " " << core::ToString(column.type)
                << " " << core::ToString(column.encoding) << role << "\n";
    }
  });
}

void EpochCommand::Register(CLI::App &parent, const GlobalOptions &globals,
                            int &exit_code) {
  CLI::App *epoch = parent.add_subcommand("epoch", "Manage epochs");
  epoch->require_subcommand(1);

  CLI::App *list = epoch->add_subcommand("list", "List published epochs");
  list->add_option("--database", database_, "Database (default \"default\")");
  list->add_option("--table", table_, "Table")->required();
  list->callback([this, &globals, &exit_code]() {
    absl::StatusOr<config::Config> config = LoadConfig(globals);
    if (!config.ok()) {
      std::cerr << "epoch list: " << config.status().message() << "\n";
      exit_code = 1;
      return;
    }
    absl::StatusOr<std::unique_ptr<LocalAdmin>> admin =
        OpenLocalAdmin(*config, database_, table_);
    if (!admin.ok()) {
      std::cerr << "epoch list: " << admin.status().message() << "\n";
      exit_code = 1;
      return;
    }
    absl::StatusOr<std::vector<std::uint64_t>> versions =
        (*admin)->admin->ListEpochs();
    if (!versions.ok()) {
      std::cerr << "epoch list: " << versions.status().message() << "\n";
      exit_code = 1;
      return;
    }
    for (std::uint64_t v : *versions)
      std::cout << v << "\n";
  });

  CLI::App *rollback =
      epoch->add_subcommand("rollback", "Point CURRENT at an older epoch");
  rollback->add_option("--database", database_,
                       "Database (default \"default\")");
  rollback->add_option("--table", table_, "Table")->required();
  rollback->add_option("version", rollback_version_, "Epoch version")
      ->required();
  rollback->callback([this, &globals, &exit_code]() {
    absl::StatusOr<config::Config> config = LoadConfig(globals);
    if (!config.ok()) {
      std::cerr << "epoch rollback: " << config.status().message() << "\n";
      exit_code = 1;
      return;
    }
    absl::StatusOr<std::unique_ptr<LocalAdmin>> admin =
        OpenLocalAdmin(*config, database_, table_);
    if (!admin.ok()) {
      std::cerr << "epoch rollback: " << admin.status().message() << "\n";
      exit_code = 1;
      return;
    }
    if (absl::Status s = (*admin)->admin->RollbackEpoch(rollback_version_);
        !s.ok()) {
      std::cerr << "epoch rollback: " << s.message() << "\n";
      exit_code = 1;
      return;
    }
    std::cout << "current epoch is now " << rollback_version_ << "\n";
  });
}

void TablesCommand::Register(CLI::App &parent, const GlobalOptions &globals,
                             int &exit_code) {
  CLI::App *tables =
      parent.add_subcommand("tables", "List databases and tables on disk");
  tables->callback([&globals, &exit_code]() {
    absl::StatusOr<config::Config> config = LoadConfig(globals);
    if (!config.ok()) {
      std::cerr << "tables: " << config.status().message() << "\n";
      exit_code = 1;
      return;
    }
    storage::Catalog catalog(config->DatabasesDir());
    absl::StatusOr<std::vector<core::TableId>> ids = catalog.ListTables();
    if (!ids.ok()) {
      std::cerr << "tables: " << ids.status().message() << "\n";
      exit_code = 1;
      return;
    }
    for (const core::TableId &id : *ids)
      std::cout << id.database << "." << id.table << "\n";
  });
}

} // namespace opaquedb::cli
