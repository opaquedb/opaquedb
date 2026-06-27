#include "opaquedb/core/record_codec.h"

#include <cstring>

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "nlohmann/json.hpp"
#include "opaquedb/core/key_codec.h"

namespace opaquedb::core {
namespace {

void AppendU16(std::vector<std::uint8_t> &out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xff));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
}

void AppendU64(std::vector<std::uint8_t> &out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i)
    out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
}

std::uint16_t ReadU16(std::span<const std::uint8_t> b, std::size_t off) {
  return static_cast<std::uint16_t>(b[off]) |
         (static_cast<std::uint16_t>(b[off + 1]) << 8);
}

std::uint64_t ReadU64(std::span<const std::uint8_t> b, std::size_t off) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v |= static_cast<std::uint64_t>(b[off + i]) << (8 * i);
  return v;
}

} // namespace

std::string ValueToString(const Value &value) {
  if (const auto *i = std::get_if<std::int64_t>(&value))
    return absl::StrCat(*i);
  if (const auto *d = std::get_if<double>(&value))
    return absl::StrCat(*d);
  return std::get<std::string>(value);
}

ColumnType ColumnTypeOf(const Value &value) {
  if (std::holds_alternative<std::int64_t>(value))
    return ColumnType::kInt;
  if (std::holds_alternative<double>(value))
    return ColumnType::kReal;
  return ColumnType::kText;
}

absl::StatusOr<Value> ParseValue(ColumnType type, std::string_view text) {
  switch (type) {
  case ColumnType::kInt: {
    std::int64_t v = 0;
    if (!absl::SimpleAtoi(text, &v)) {
      return absl::InvalidArgumentError(
          absl::StrCat("'", text, "' is not an integer"));
    }
    return Value{v};
  }
  case ColumnType::kReal: {
    double v = 0;
    if (!absl::SimpleAtod(text, &v)) {
      return absl::InvalidArgumentError(
          absl::StrCat("'", text, "' is not a number"));
    }
    return Value{v};
  }
  case ColumnType::kText:
    return Value{std::string(text)};
  case ColumnType::kJson: {
    // JSON is stored as text but must be well formed so the client gets back
    // parseable JSON, not an opaque string. Validate here, the single text ->
    // Value boundary every ingest path flows through, then store the bytes.
    if (!nlohmann::json::accept(text)) {
      return absl::InvalidArgumentError(
          absl::StrCat("'", text, "' is not valid JSON"));
    }
    return Value{std::string(text)};
  }
  }
  return absl::InternalError("unknown column type");
}

absl::StatusOr<std::vector<std::uint8_t>>
EncodeRecord(const Schema &schema, const std::vector<Value> &payload,
             std::uint32_t record_bytes) {
  std::vector<std::uint8_t> out;
  std::size_t payload_index = 0;
  for (const Column &col : schema.columns()) {
    if (col.encoding == ColumnEncoding::kEq)
      continue; // the key is stored separately as a codeword
    if (payload_index >= payload.size()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "record has no value for payload column '", col.name, "'"));
    }
    const Value &value = payload[payload_index++];
    switch (col.type) {
    case ColumnType::kInt: {
      const auto *i = std::get_if<std::int64_t>(&value);
      if (i == nullptr)
        return absl::InvalidArgumentError(
            absl::StrCat("column '", col.name, "' expects an integer"));
      AppendU64(out, static_cast<std::uint64_t>(*i));
      break;
    }
    case ColumnType::kReal: {
      const auto *d = std::get_if<double>(&value);
      if (d == nullptr)
        return absl::InvalidArgumentError(
            absl::StrCat("column '", col.name, "' expects a number"));
      std::uint64_t bits = 0;
      std::memcpy(&bits, d, sizeof(bits));
      AppendU64(out, bits);
      break;
    }
    case ColumnType::kJson:
    case ColumnType::kText: {
      const auto *s = std::get_if<std::string>(&value);
      if (s == nullptr)
        return absl::InvalidArgumentError(
            absl::StrCat("column '", col.name, "' expects text"));
      if (s->size() > 0xffff) {
        return absl::InvalidArgumentError(
            absl::StrCat("text column '", col.name, "' exceeds 65535 bytes"));
      }
      AppendU16(out, static_cast<std::uint16_t>(s->size()));
      out.insert(out.end(), s->begin(), s->end());
      break;
    }
    }
  }
  if (out.size() > record_bytes) {
    return absl::InvalidArgumentError(
        absl::StrCat("encoded record is ", out.size(), " bytes, over the ",
                     record_bytes, "-byte record size"));
  }
  out.resize(record_bytes, 0);
  return out;
}

absl::StatusOr<std::vector<std::uint8_t>>
EncodeMatchRecord(const Schema &schema, const Value &key_value,
                  const std::vector<Value> &payload, std::uint32_t key_bits) {
  std::vector<std::uint8_t> out;
  std::size_t payload_index = 0;
  for (const Column &col : schema.columns()) {
    // The key column's value comes from key_value; every other column's value
    // comes from the payload vector, in the same order EncodeRecord consumes
    // it. Advance the payload cursor for all non-key columns, searchable or
    // not.
    const Value *value = nullptr;
    if (col.encoding == ColumnEncoding::kEq) {
      value = &key_value;
    } else {
      if (payload_index >= payload.size()) {
        return absl::InvalidArgumentError(
            absl::StrCat("record has no value for column '", col.name, "'"));
      }
      value = &payload[payload_index++];
    }
    if (!IsSearchable(col.encoding))
      continue;
    absl::StatusOr<KeyEncoding> enc =
        EncodeKeyValue(col.type, *value, key_bits);
    if (!enc.ok())
      return enc.status();
    std::vector<std::uint8_t> packed = PackKey(enc->value, key_bits);
    out.insert(out.end(), packed.begin(), packed.end());
  }
  return out;
}

absl::StatusOr<std::vector<Value>>
DecodeRecord(const Schema &schema, std::span<const std::uint8_t> record) {
  std::vector<Value> values;
  std::size_t off = 0;
  for (const Column &col : schema.columns()) {
    if (col.encoding == ColumnEncoding::kEq)
      continue;
    switch (col.type) {
    case ColumnType::kInt: {
      if (off + 8 > record.size())
        return absl::DataLossError("record truncated reading an int column");
      values.emplace_back(static_cast<std::int64_t>(ReadU64(record, off)));
      off += 8;
      break;
    }
    case ColumnType::kReal: {
      if (off + 8 > record.size())
        return absl::DataLossError("record truncated reading a real column");
      const std::uint64_t bits = ReadU64(record, off);
      double d = 0;
      std::memcpy(&d, &bits, sizeof(d));
      values.emplace_back(d);
      off += 8;
      break;
    }
    case ColumnType::kJson:
    case ColumnType::kText: {
      if (off + 2 > record.size())
        return absl::DataLossError("record truncated reading a text length");
      const std::uint16_t len = ReadU16(record, off);
      off += 2;
      if (off + len > record.size())
        return absl::DataLossError("record truncated reading text bytes");
      values.emplace_back(std::string(
          reinterpret_cast<const char *>(record.data() + off), len));
      off += len;
      break;
    }
    }
  }
  return values;
}

} // namespace opaquedb::core
