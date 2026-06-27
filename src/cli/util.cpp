#include "util.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>
#include <variant>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "opaquedb/client/query_client.h"
#include "opaquedb/config/loader.h"
#include "opaquedb/core/record_codec.h"
#include "opaquedb/log/log.h"
#include "opaquedb/sql/parser.h"

namespace opaquedb::cli {

absl::StatusOr<config::Config> LoadConfig(const GlobalOptions &globals,
                                          bool file_optional) {
  absl::StatusOr<config::LoadOptions> opts =
      globals.ToLoadOptions(file_optional);
  if (!opts.ok())
    return opts.status();
  absl::StatusOr<config::Config> config = config::Load(*opts);
  if (!config.ok())
    return config.status();
  if (absl::Status s = log::Init(config->logging); !s.ok())
    return s;
  return config;
}

absl::StatusOr<std::string> ReadFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return absl::NotFoundError(absl::StrCat("cannot read '", path, "'"));
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string RenderRaw(const std::vector<std::uint8_t> &record) {
  std::size_t end = record.size();
  while (end > 0 && record[end - 1] == 0)
    --end;
  return std::string(record.begin(), record.begin() + end);
}

std::string RenderWithSchema(const core::Schema &schema,
                             const std::vector<std::uint8_t> &record,
                             const std::vector<std::string> &projection) {
  absl::StatusOr<std::vector<core::Value>> values =
      core::DecodeRecord(schema, record);
  if (!values.ok())
    return absl::StrCat("(decode error: ", values.status().message(), ")");
  // DecodeRecord yields payload (non-key) columns in schema order. Map each
  // payload column name to its value so the projection can pick by name.
  std::vector<std::pair<std::string, std::string>> payload;
  std::size_t i = 0;
  for (const core::Column &col : schema.columns()) {
    if (col.encoding == core::ColumnEncoding::kEq)
      continue;
    if (i >= values->size())
      break;
    payload.emplace_back(col.name, core::ValueToString((*values)[i++]));
  }

  std::vector<std::string> parts;
  if (projection.empty()) {
    for (const auto &[name, value] : payload)
      parts.push_back(absl::StrCat(name, "=", value));
  } else {
    for (const std::string &want : projection) {
      for (const auto &[name, value] : payload) {
        if (name == want) {
          parts.push_back(absl::StrCat(name, "=", value));
          break;
        }
      }
    }
  }
  return absl::StrJoin(parts, " ");
}

std::string RenderTable(const std::vector<std::string> &headers,
                        const std::vector<std::vector<std::string>> &rows) {
  std::vector<std::size_t> width(headers.size());
  for (std::size_t i = 0; i < headers.size(); ++i)
    width[i] = headers[i].size();
  for (const std::vector<std::string> &row : rows)
    for (std::size_t i = 0; i < row.size() && i < width.size(); ++i)
      width[i] = std::max(width[i], row[i].size());

  // One cell is " value " padded to the column width, cells joined by '|'.
  auto render_row = [&](const std::vector<std::string> &cells) {
    std::string line;
    for (std::size_t i = 0; i < cells.size(); ++i) {
      if (i > 0)
        line += "|";
      line +=
          " " + cells[i] + std::string(width[i] - cells[i].size(), ' ') + " ";
    }
    return line;
  };

  std::string out = render_row(headers) + "\n";
  for (std::size_t i = 0; i < width.size(); ++i) {
    if (i > 0)
      out += "+";
    out += std::string(width[i] + 2, '-');
  }
  out += "\n";
  for (const std::vector<std::string> &row : rows)
    out += render_row(row) + "\n";
  return out;
}

namespace {

// Orders two values. Same alternative compares directly; a number against a
// number compares numerically across int/real; anything else falls back to the
// string form so the order is at least total and stable.
bool ValueLess(const core::Value &a, const core::Value &b) {
  const auto *ai = std::get_if<std::int64_t>(&a);
  const auto *bi = std::get_if<std::int64_t>(&b);
  const auto *ad = std::get_if<double>(&a);
  const auto *bd = std::get_if<double>(&b);
  if (ai && bi)
    return *ai < *bi;
  if ((ai || ad) && (bi || bd)) {
    const double av = ai ? static_cast<double>(*ai) : *ad;
    const double bv = bi ? static_cast<double>(*bi) : *bd;
    return av < bv;
  }
  const auto *as = std::get_if<std::string>(&a);
  const auto *bs = std::get_if<std::string>(&b);
  if (as && bs)
    return *as < *bs;
  return core::ValueToString(a) < core::ValueToString(b);
}

} // namespace

absl::StatusOr<TableData>
BuildResultTable(const core::Schema &schema,
                 const std::vector<std::vector<std::uint8_t>> &records,
                 const sql::SelectStatement &statement) {
  // Payload columns (everything except the primary key) in schema order; these
  // are what a decoded record yields and the only columns a query returns.
  std::vector<std::string> payload_names;
  for (const core::Column &col : schema.columns())
    if (col.encoding != core::ColumnEncoding::kEq)
      payload_names.push_back(col.name);

  auto payload_index =
      [&](const std::string &name) -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < payload_names.size(); ++i)
      if (payload_names[i] == name)
        return i;
    return std::nullopt;
  };

  // The columns to display, and the header text for each. With no projection
  // (SELECT * or a scan with no column list) show every payload column.
  std::vector<std::size_t> display_idx;
  TableData out;
  if (statement.projection.empty()) {
    for (std::size_t i = 0; i < payload_names.size(); ++i) {
      display_idx.push_back(i);
      out.headers.push_back(payload_names[i]);
    }
  } else {
    for (std::size_t i = 0; i < statement.projection.size(); ++i) {
      const std::string &col = statement.projection[i];
      const std::optional<std::size_t> idx = payload_index(col);
      if (!idx) {
        return absl::InvalidArgumentError(absl::StrCat(
            "column '", col,
            "' is not a returnable column (the primary key is matched, not "
            "returned, and unknown columns cannot be shown)"));
      }
      display_idx.push_back(*idx);
      out.headers.push_back(i < statement.projection_aliases.size() &&
                                    !statement.projection_aliases[i].empty()
                                ? statement.projection_aliases[i]
                                : col);
    }
  }

  // Resolve ORDER BY columns to payload indices up front.
  std::vector<std::pair<std::size_t, bool>> order_cols; // (payload index, desc)
  for (const sql::OrderByItem &item : statement.order_by) {
    const std::optional<std::size_t> idx = payload_index(item.column);
    if (!idx) {
      return absl::InvalidArgumentError(
          absl::StrCat("cannot ORDER BY '", item.column,
                       "': it is not among the returned columns"));
    }
    order_cols.emplace_back(*idx, item.descending);
  }

  // Decode each record into its payload values, then the display cells and the
  // order keys.
  struct Row {
    std::vector<std::string> cells;
    std::vector<core::Value> order_keys;
  };
  std::vector<Row> table;
  table.reserve(records.size());
  for (const std::vector<std::uint8_t> &rec : records) {
    absl::StatusOr<std::vector<core::Value>> values =
        core::DecodeRecord(schema, rec);
    if (!values.ok())
      return values.status();
    if (values->size() != payload_names.size()) {
      return absl::InternalError("decoded value count does not match schema");
    }
    Row row;
    row.cells.reserve(display_idx.size());
    for (std::size_t i : display_idx)
      row.cells.push_back(core::ValueToString((*values)[i]));
    row.order_keys.reserve(order_cols.size());
    for (const auto &[idx, desc] : order_cols)
      row.order_keys.push_back((*values)[idx]);
    table.push_back(std::move(row));
  }

  // DISTINCT: drop duplicate display rows, keeping the first occurrence.
  if (statement.distinct) {
    std::vector<Row> unique;
    for (Row &row : table) {
      bool seen = false;
      for (const Row &kept : unique)
        if (kept.cells == row.cells) {
          seen = true;
          break;
        }
      if (!seen)
        unique.push_back(std::move(row));
    }
    table = std::move(unique);
  }

  // ORDER BY: stable sort over the resolved order columns.
  if (!order_cols.empty()) {
    std::stable_sort(
        table.begin(), table.end(), [&](const Row &a, const Row &b) {
          for (std::size_t i = 0; i < order_cols.size(); ++i) {
            const core::Value &av = a.order_keys[i];
            const core::Value &bv = b.order_keys[i];
            if (ValueLess(av, bv))
              return !order_cols[i].second; // asc: less comes first
            if (ValueLess(bv, av))
              return order_cols[i].second; // desc: greater comes first
          }
          return false;
        });
  }

  // OFFSET/LIMIT window over the decoded rows.
  const std::uint64_t offset = statement.offset.value_or(0);
  const std::uint64_t limit =
      statement.limit.value_or(sql::kDefaultSelectLimit);
  for (std::uint64_t i = offset; i < table.size() && out.rows.size() < limit;
       ++i)
    out.rows.push_back(std::move(table[i].cells));
  return out;
}

namespace {

// How many rows a scan asks the server for. With no client-side reordering only
// the OFFSET+LIMIT window is needed. DISTINCT or ORDER BY have to see the whole
// set, so ask for a large bound; the server clamps it to its own row cap, so
// the client does not need to know that cap exactly.
inline constexpr std::uint64_t kScanReorderFetch = 100000;

std::uint64_t ScanFetchCount(const sql::SelectStatement &stmt) {
  if (stmt.distinct || !stmt.order_by.empty())
    return kScanReorderFetch;
  const std::uint64_t offset = stmt.offset.value_or(0);
  const std::uint64_t limit = stmt.limit.value_or(sql::kDefaultSelectLimit);
  return offset + limit;
}

} // namespace

absl::Status RunSelect(client::QueryClient &client,
                       const std::string &client_id, const std::string &sql,
                       std::uint64_t value, const std::string &backend,
                       const std::string &database, std::ostream &out,
                       std::map<std::string, core::Schema> *schema_cache) {
  absl::StatusOr<sql::SelectStatement> stmt = sql::Parse(sql);
  if (!stmt.ok())
    return stmt.status();
  const bool is_scan = stmt->where == nullptr;

  // COUNT(*) with no WHERE is the table's row count from the scan path; with a
  // WHERE it is the encrypted match count. Either way it renders as one number.
  if (stmt->count_star) {
    if (is_scan) {
      absl::StatusOr<client::QueryClient::ScanResult> r =
          client.Scan(database, stmt->table, /*max_rows=*/0);
      if (!r.ok())
        return r.status();
      out << r->total_rows << "\n";
      return absl::OkStatus();
    }
    absl::StatusOr<std::uint64_t> n =
        client.QueryCount(client_id, sql, value, backend, database);
    if (!n.ok())
      return n.status();
    out << *n << "\n";
    return absl::OkStatus();
  }

  // Fetch (and cache) the table schema so rows decode into typed columns. Fall
  // back to raw hex-ish bytes if the fetch fails (e.g. an old node).
  const core::Schema *schema = nullptr;
  core::Schema fetched_local;
  if (schema_cache != nullptr) {
    const std::string key = database + "." + stmt->table;
    auto it = schema_cache->find(key);
    if (it == schema_cache->end()) {
      if (absl::StatusOr<core::Schema> fetched =
              client.DescribeTable(database, stmt->table);
          fetched.ok())
        it = schema_cache->emplace(key, *std::move(fetched)).first;
    }
    if (it != schema_cache->end())
      schema = &it->second;
  } else if (absl::StatusOr<core::Schema> f =
                 client.DescribeTable(database, stmt->table);
             f.ok()) {
    fetched_local = *std::move(f);
    schema = &fetched_local;
  }

  // Gather the raw result rows: a plaintext scan when there is no WHERE, else
  // the clean matched rows from the encrypted query.
  std::vector<std::vector<std::uint8_t>> rows;
  std::uint32_t collided = 0;
  if (is_scan) {
    absl::StatusOr<client::QueryClient::ScanResult> r =
        client.Scan(database, stmt->table, ScanFetchCount(*stmt));
    if (!r.ok())
      return r.status();
    rows = std::move(r->rows);
  } else {
    absl::StatusOr<std::vector<std::vector<std::uint8_t>>> r =
        client.QueryClean(client_id, sql, value, backend, database, &collided);
    if (!r.ok())
      return r.status();
    rows = *std::move(r);
  }

  if (schema == nullptr) {
    // No schema: print raw rows after a client-side window, no decode.
    const std::uint64_t offset = stmt->offset.value_or(0);
    const std::uint64_t limit = stmt->limit.value_or(sql::kDefaultSelectLimit);
    std::uint64_t shown = 0;
    for (std::uint64_t i = offset; i < rows.size() && shown < limit;
         ++i, ++shown)
      out << RenderRaw(rows[i]) << "\n";
    if (shown == 0)
      out << "(no rows)\n";
  } else {
    absl::StatusOr<TableData> table = BuildResultTable(*schema, rows, *stmt);
    if (!table.ok())
      return table.status();
    if (table->rows.empty())
      out << "(no rows)\n";
    else
      out << RenderTable(table->headers, table->rows);
  }

  if (collided > 0) {
    out << "(" << collided
        << " bucket(s) dropped: same-key collision; raise "
           "crypto.result_buckets or page with OFFSET)\n";
  }
  return absl::OkStatus();
}

} // namespace opaquedb::cli
