#include "opaquedb/sql/ddl.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "opaquedb/sql/tokenizer.h"

namespace opaquedb::sql {
namespace {

// A small cursor over the DDL tokens. It reuses the SQL tokenizer; the type
// names (INT, REAL, TEXT) arrive as identifiers and are matched by text.
class DdlParser {
public:
  explicit DdlParser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  absl::StatusOr<core::Schema> Parse() {
    if (absl::Status s = Expect(TokenType::kCreate); !s.ok())
      return s;
    if (absl::Status s = Expect(TokenType::kTable); !s.ok())
      return s;
    absl::StatusOr<std::string> table = Identifier("a table name");
    if (!table.ok())
      return table.status();
    if (absl::Status s = Expect(TokenType::kLParen); !s.ok())
      return s;

    std::vector<core::Column> columns;
    std::size_t key_count = 0;
    while (true) {
      absl::StatusOr<core::Column> col = ParseColumn();
      if (!col.ok())
        return col.status();
      if (col->encoding == core::ColumnEncoding::kEq)
        ++key_count;
      columns.push_back(*std::move(col));
      if (Match(TokenType::kComma))
        continue;
      break;
    }
    if (absl::Status s = Expect(TokenType::kRParen); !s.ok())
      return s;
    // A trailing semicolon is not its own token; the tokenizer would reject it,
    // so the caller strips it. We just require end of input here.
    if (absl::Status s = Expect(TokenType::kEnd); !s.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("unexpected trailing input near offset ", Peek().pos));
    }
    if (key_count != 1) {
      return absl::InvalidArgumentError(absl::StrCat(
          "CREATE TABLE needs exactly one KEY column, found ", key_count));
    }
    core::Schema schema(*std::move(table), std::move(columns));
    if (absl::Status s = schema.Validate(); !s.ok())
      return s;
    return schema;
  }

private:
  const Token &Peek() const { return tokens_[pos_]; }
  bool Check(TokenType type) const { return Peek().type == type; }
  bool Match(TokenType type) {
    if (Check(type)) {
      ++pos_;
      return true;
    }
    return false;
  }
  absl::Status Expect(TokenType type) {
    if (Check(type)) {
      ++pos_;
      return absl::OkStatus();
    }
    return absl::InvalidArgumentError(
        absl::StrCat("expected ", ToString(type), " but found ",
                     ToString(Peek().type), " at offset ", Peek().pos));
  }
  absl::StatusOr<std::string> Identifier(const char *what) {
    if (!Check(TokenType::kIdentifier)) {
      return absl::InvalidArgumentError(
          absl::StrCat("expected ", what, " but found ", ToString(Peek().type),
                       " at offset ", Peek().pos));
    }
    return tokens_[pos_++].text;
  }

  absl::StatusOr<core::Column> ParseColumn() {
    absl::StatusOr<std::string> name = Identifier("a column name");
    if (!name.ok())
      return name.status();
    absl::StatusOr<std::string> type_word = Identifier("a column type");
    if (!type_word.ok())
      return type_word.status();
    absl::StatusOr<core::ColumnType> type =
        core::ParseColumnType(absl::AsciiStrToLower(*type_word));
    if (!type.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("column '", *name, "': unknown type '", *type_word,
                       "', use INT, REAL, or TEXT"));
    }
    core::Column column;
    column.name = *std::move(name);
    column.type = *type;
    if (Match(TokenType::kKey)) {
      column.encoding = core::ColumnEncoding::kEq;
    } else if (Match(TokenType::kIndex)) {
      column.encoding = core::ColumnEncoding::kIndex;
    } else {
      column.encoding = core::ColumnEncoding::kRaw;
    }
    return column;
  }

  std::vector<Token> tokens_;
  std::size_t pos_ = 0;
};

} // namespace

absl::StatusOr<core::Schema> ParseCreateTable(std::string_view ddl) {
  // The SQL tokenizer does not know ';'. A trailing semicolon is common in DDL,
  // so strip one before tokenizing.
  std::string_view trimmed = absl::StripAsciiWhitespace(ddl);
  if (!trimmed.empty() && trimmed.back() == ';')
    trimmed.remove_suffix(1);
  absl::StatusOr<std::vector<Token>> tokens = Tokenize(trimmed);
  if (!tokens.ok())
    return tokens.status();
  DdlParser parser(*std::move(tokens));
  return parser.Parse();
}

} // namespace opaquedb::sql
