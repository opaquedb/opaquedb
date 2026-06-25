#include "data_commands.h"

#include <iostream>
#include <string>
#include <vector>

#include "../util.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "opaquedb/admin/service.h"
#include "opaquedb/admin/keyring_store.h"
#include "opaquedb/cluster/shard_map.h"
#include "opaquedb/server/ingest.h"
#include "opaquedb/sql/ddl.h"
#include "opaquedb/storage/repository.h"
#include "spdlog/spdlog.h"

namespace opaquedb::cli {

void LoadCommand::Register(CLI::App &parent, const GlobalOptions &globals,
                           int &exit_code) {
  CLI::App *load = parent.add_subcommand(
      "load", "Load a CSV under a CREATE TABLE schema and publish an epoch");
  load->add_option("--schema", schema_, "CREATE TABLE schema file")->required();
  load->add_option("--csv", csv_, "CSV data file with a header row")
      ->required();
  load->add_option("--database", database_,
                   "Database to load the table into (default \"default\")");
  load->add_option("--shard-id", shard_id_,
                   "This node's id; with --shard-nodes, load only its shard");
  load->add_option(
      "--shard-nodes", shard_nodes_,
      "Comma-separated node ids for the consistent-hash shard map");
  load->callback([this, &globals, &exit_code]() {
    absl::StatusOr<config::Config> config = LoadConfig(globals);
    if (!config.ok()) {
      spdlog::error("load: {}", config.status().message());
      exit_code = 1;
      return;
    }

    absl::StatusOr<std::string> ddl = ReadFile(schema_);
    if (!ddl.ok()) {
      spdlog::error("load: {}", ddl.status().message());
      exit_code = 1;
      return;
    }
    absl::StatusOr<core::Schema> schema = sql::ParseCreateTable(*ddl);
    if (!schema.ok()) {
      spdlog::error("load: schema: {}", schema.status().message());
      exit_code = 1;
      return;
    }

    absl::StatusOr<std::vector<server::IngestRow>> rows =
        server::ParseCsvRows(*schema, csv_);
    if (!rows.ok()) {
      spdlog::error("load: {}", rows.status().message());
      exit_code = 1;
      return;
    }

    // When sharding, keep only the rows this node owns so each node holds a
    // disjoint shard; the query path fans out and combines across all of them.
    if (!shard_id_.empty() && !shard_nodes_.empty()) {
      std::vector<std::string> nodes =
          absl::StrSplit(shard_nodes_, ',', absl::SkipEmpty());
      cluster::ShardMap map;
      map.SetNodes(nodes);
      std::vector<server::IngestRow> mine;
      for (server::IngestRow &row : *rows) {
        // Namespace the shard key by database.table so each table's rows
        // distribute independently across the nodes.
        auto owner =
            map.OwnerOfKey(absl::StrCat(database_, ".", schema->table(), ".",
                                        core::ValueToString(row.key)));
        if (owner.ok() && *owner == shard_id_)
          mine.push_back(std::move(row));
      }
      rows = std::move(mine);
    }

    absl::StatusOr<storage::StagingEpoch> staging =
        server::BuildStagingFromRows(*config, *schema, *rows);
    if (!staging.ok()) {
      spdlog::error("load: {}", staging.status().message());
      exit_code = 1;
      return;
    }

    absl::StatusOr<std::unique_ptr<storage::LocalEpochRepository>> repo =
        storage::LocalEpochRepository::Open(
            config->EpochRootFor(database_, schema->table()));
    if (!repo.ok()) {
      spdlog::error("load: {}", repo.status().message());
      exit_code = 1;
      return;
    }
    // Publish through the AdminService facade so the publish logic lives in one
    // place shared with the gRPC admin path.
    admin::InMemoryKeyringStore keyring;
    admin::AdminService admin(*config, repo->get(), &keyring);
    absl::StatusOr<std::uint64_t> version = admin.Publish(*staging);
    if (!version.ok()) {
      spdlog::error("load: {}", version.status().message());
      exit_code = 1;
      return;
    }
    std::cout << "published epoch " << *version << " with "
              << staging->rows().size() << " rows\n";
  });
}

} // namespace opaquedb::cli
