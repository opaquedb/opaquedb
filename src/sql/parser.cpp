#include "opaquedb/sql/parser.h"

#include <memory>
#include <utility>
#include <vector>

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/strip.h"
#include "opaquedb/sql/tokenizer.h"

namespace opaquedb::sql {
namespace {

// Trims surrounding whitespace and one optional trailing semicolon, which the
// tokenizer does not know but is common and standard at the end of a statement.
std::string_view TrimStatement(std::string_view sql) {
  std::string_view t = absl::StripAsciiWhitespace(sql);
  if (!t.empty() && t.back() == ';')
    t.remove_suffix(1);
  return absl::StripAsciiWhitespace(t);
}

// A single-pass cursor over the token stream. Every helper that consumes input
// checks the token type and returns a clear status on a mismatch, so a
// malformed template never reads past the end.
class Parser {
public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  absl::StatusOr<SelectStatement> ParseStatement() {
    SelectStatement stmt;
    if (absl::Status s = Expect(TokenType::kSelect); !s.ok())
      return s;

    // SELECT COUNT(*) returns the encrypted match count, not rows.
    if (Match(TokenType::kCount)) {
      if (absl::Status s = Expect(TokenType::kLParen); !s.ok())
        return s;
      if (absl::Status s = Expect(TokenType::kStar); !s.ok())
        return s;
      if (absl::Status s = Expect(TokenType::kRParen); !s.ok())
        return s;
      stmt.count_star = true;
    } else if (Match(TokenType::kStar)) {
      // SELECT * returns every column; the planner expands it.
      stmt.select_all = true;
    } else {
      absl::StatusOr<std::vector<std::string>> cols = ParseColumns();
      if (!cols.ok())
        return cols.status();
      stmt.projection = *std::move(cols);
    }

    if (absl::Status s = Expect(TokenType::kFrom); !s.ok())
      return s;
    absl::StatusOr<std::string> table = ParseIdentifier("table name");
    if (!table.ok())
      return table.status();
    stmt.table = *std::move(table);

    if (absl::Status s = Expect(TokenType::kWhere); !s.ok())
      return s;
    absl::StatusOr<std::unique_ptr<Predicate>> where = ParsePredicate();
    if (!where.ok())
      return where.status();
    stmt.where = *std::move(where);

    // Optional LIMIT then optional OFFSET. Both are public bounds on how many
    // result buckets to return and from where; they are not secret values.
    if (Match(TokenType::kLimit)) {
      absl::StatusOr<std::uint64_t> n = ParseCount("LIMIT");
      if (!n.ok())
        return n.status();
      if (*n == 0) {
        return absl::InvalidArgumentError("LIMIT must be at least 1");
      }
      stmt.limit = *n;
    }
    if (Match(TokenType::kOffset)) {
      absl::StatusOr<std::uint64_t> m = ParseCount("OFFSET");
      if (!m.ok())
        return m.status();
      stmt.offset = *m;
    }

    if (absl::Status s = Expect(TokenType::kEnd); !s.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("unexpected trailing input near offset ", Peek().pos));
    }
    return stmt;
  }

private:
  const Token &Peek() const { return tokens_[pos_]; }
  const Token &Advance() { return tokens_[pos_++]; }
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

  absl::StatusOr<std::string> ParseIdentifier(const char *what) {
    if (!Check(TokenType::kIdentifier)) {
      return absl::InvalidArgumentError(
          absl::StrCat("expected ", what, " but found ", ToString(Peek().type),
                       " at offset ", Peek().pos));
    }
    return Advance().text;
  }

  // Parses a non-negative integer count following LIMIT or OFFSET. The value is
  // a plain number literal in the public template (not a secret), so it is read
  // here rather than treated as a bound parameter.
  absl::StatusOr<std::uint64_t> ParseCount(const char *what) {
    if (!Check(TokenType::kNumberLiteral)) {
      return absl::InvalidArgumentError(
          absl::StrCat("expected a number after ", what, " but found ",
                       ToString(Peek().type), " at offset ", Peek().pos));
    }
    const Token &tok = Advance();
    std::uint64_t v = 0;
    if (!absl::SimpleAtoi(tok.text, &v)) {
      return absl::InvalidArgumentError(absl::StrCat(
          what, " value '", tok.text, "' is out of range at offset ", tok.pos));
    }
    return v;
  }

  absl::StatusOr<Parameter> ParseParameter() {
    if (Check(TokenType::kStringLiteral)) {
      return Parameter{"", core::Value{Advance().text}};
    }
    if (Check(TokenType::kNumberLiteral)) {
      const Token &tok = Advance();
      std::int64_t v = 0;
      if (!absl::SimpleAtoi(tok.text, &v)) {
        return absl::InvalidArgumentError(
            absl::StrCat("number literal '", tok.text,
                         "' is out of range at offset ", tok.pos));
      }
      return Parameter{"", core::Value{v}};
    }
    if (!Check(TokenType::kParameter)) {
      return absl::InvalidArgumentError(absl::StrCat(
          "expected a bound parameter (:name) or a literal but found ",
          ToString(Peek().type), " at offset ", Peek().pos));
    }
    return Parameter{Advance().text, std::nullopt};
  }

  absl::StatusOr<std::vector<std::string>> ParseColumns() {
    std::vector<std::string> columns;
    absl::StatusOr<std::string> first = ParseIdentifier("a column name");
    if (!first.ok())
      return first.status();
    columns.push_back(*std::move(first));
    while (Match(TokenType::kComma)) {
      absl::StatusOr<std::string> next = ParseIdentifier("a column name");
      if (!next.ok())
        return next.status();
      columns.push_back(*std::move(next));
    }
    return columns;
  }

  // predicate := or_term (OR or_term)*
  absl::StatusOr<std::unique_ptr<Predicate>> ParsePredicate() {
    absl::StatusOr<std::unique_ptr<Predicate>> left = ParseOrTerm();
    if (!left.ok())
      return left;
    std::unique_ptr<Predicate> node = *std::move(left);
    while (Match(TokenType::kOr)) {
      absl::StatusOr<std::unique_ptr<Predicate>> right = ParseOrTerm();
      if (!right.ok())
        return right;
      node = std::make_unique<OrPredicate>(std::move(node), *std::move(right));
    }
    return node;
  }

  // or_term := and_term (AND and_term)*
  absl::StatusOr<std::unique_ptr<Predicate>> ParseOrTerm() {
    absl::StatusOr<std::unique_ptr<Predicate>> left = ParseComparison();
    if (!left.ok())
      return left;
    std::unique_ptr<Predicate> node = *std::move(left);
    while (Match(TokenType::kAnd)) {
      absl::StatusOr<std::unique_ptr<Predicate>> right = ParseComparison();
      if (!right.ok())
        return right;
      node = std::make_unique<AndPredicate>(std::move(node), *std::move(right));
    }
    return node;
  }

  // and_term := identifier (comparison | IN ... | LIKE ... | BETWEEN ...)
  absl::StatusOr<std::unique_ptr<Predicate>> ParseComparison() {
    absl::StatusOr<std::string> column = ParseIdentifier("a column name");
    if (!column.ok())
      return column.status();

    if (Match(TokenType::kIn))
      return ParseIn(*std::move(column));
    if (Match(TokenType::kLike))
      return ParseLike(*std::move(column));
    if (Match(TokenType::kBetween))
      return ParseBetween(*std::move(column));

    CompareOp op;
    if (Match(TokenType::kEq)) {
      op = CompareOp::kEq;
    } else if (Match(TokenType::kNe)) {
      op = CompareOp::kNe;
    } else if (Match(TokenType::kLt)) {
      op = CompareOp::kLt;
    } else if (Match(TokenType::kLe)) {
      op = CompareOp::kLe;
    } else if (Match(TokenType::kGt)) {
      op = CompareOp::kGt;
    } else if (Match(TokenType::kGe)) {
      op = CompareOp::kGe;
    } else {
      return absl::InvalidArgumentError(absl::StrCat(
          "expected a comparison operator after column '", *column,
          "' but found ", ToString(Peek().type), " at offset ", Peek().pos));
    }
    absl::StatusOr<Parameter> param = ParseParameter();
    if (!param.ok())
      return param.status();
    return std::unique_ptr<Predicate>(std::make_unique<ComparisonPredicate>(
        *std::move(column), op, *std::move(param)));
  }

  absl::StatusOr<std::unique_ptr<Predicate>> ParseIn(std::string column) {
    if (absl::Status s = Expect(TokenType::kLParen); !s.ok())
      return s;
    std::vector<Parameter> params;
    absl::StatusOr<Parameter> first = ParseParameter();
    if (!first.ok())
      return first.status();
    params.push_back(*std::move(first));
    while (Match(TokenType::kComma)) {
      absl::StatusOr<Parameter> next = ParseParameter();
      if (!next.ok())
        return next.status();
      params.push_back(*std::move(next));
    }
    if (absl::Status s = Expect(TokenType::kRParen); !s.ok())
      return s;
    return std::unique_ptr<Predicate>(
        std::make_unique<InPredicate>(std::move(column), std::move(params)));
  }

  absl::StatusOr<std::unique_ptr<Predicate>> ParseLike(std::string column) {
    absl::StatusOr<Parameter> param = ParseParameter();
    if (!param.ok())
      return param.status();
    return std::unique_ptr<Predicate>(
        std::make_unique<LikePredicate>(std::move(column), *std::move(param)));
  }

  absl::StatusOr<std::unique_ptr<Predicate>> ParseBetween(std::string column) {
    absl::StatusOr<Parameter> low = ParseParameter();
    if (!low.ok())
      return low.status();
    if (absl::Status s = Expect(TokenType::kAnd); !s.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("expected AND in BETWEEN at offset ", Peek().pos));
    }
    absl::StatusOr<Parameter> high = ParseParameter();
    if (!high.ok())
      return high.status();
    return std::unique_ptr<Predicate>(std::make_unique<BetweenPredicate>(
        std::move(column), *std::move(low), *std::move(high)));
  }

  std::vector<Token> tokens_;
  std::size_t pos_ = 0;
};

} // namespace

absl::StatusOr<SelectStatement> Parse(std::string_view sql) {
  absl::StatusOr<std::vector<Token>> tokens = Tokenize(TrimStatement(sql));
  if (!tokens.ok())
    return tokens.status();
  Parser parser(*std::move(tokens));
  return parser.ParseStatement();
}

absl::StatusOr<PreparedQuery> PrepareClientQuery(std::string_view sql) {
  // Work on the trimmed form (no trailing ';') so token offsets used for
  // splicing line up with what we send.
  const std::string_view src = TrimStatement(sql);

  // Validate the whole statement first so a malformed template fails clearly
  // and we never rewrite garbage.
  absl::StatusOr<SelectStatement> stmt = Parse(src);
  if (!stmt.ok())
    return stmt.status();

  absl::StatusOr<std::vector<Token>> tokens = Tokenize(src);
  if (!tokens.ok())
    return tokens.status();

  // Literals can appear only as match values (an equality value or the entries
  // of an IN list), so every literal token is a value to lift out, in order.
  // The number after LIMIT or OFFSET is a public bound, not a value, so skip it.
  std::vector<const Token *> literals;
  TokenType prev = TokenType::kEnd;
  for (const Token &tok : *tokens) {
    const TokenType cur = tok.type;
    if ((prev == TokenType::kLimit || prev == TokenType::kOffset) &&
        cur == TokenType::kNumberLiteral) {
      prev = cur;
      continue;
    }
    prev = cur;
    if (tok.type == TokenType::kStringLiteral ||
        tok.type == TokenType::kNumberLiteral)
      literals.push_back(&tok);
  }

  PreparedQuery out;
  out.limit = stmt->limit;
  out.offset = stmt->offset;
  out.count = stmt->count_star;
  if (literals.empty()) {
    out.sql_template = std::string(src);
    return out; // already parameterized; nothing to strip
  }

  // Type and record each literal in source order, then splice a bound parameter
  // over each. A single value is rewritten to ':v' (the historical form); a
  // list to ':v0', ':v1', ... so the server's template parses back to the same
  // IN / OR shape. Splice right-to-left so earlier byte offsets stay valid.
  const bool single = literals.size() == 1;
  for (const Token *lit : literals) {
    if (lit->type == TokenType::kStringLiteral) {
      out.literals.push_back(core::Value{lit->text});
    } else {
      std::int64_t v = 0;
      if (!absl::SimpleAtoi(lit->text, &v)) {
        return absl::InvalidArgumentError(
            absl::StrCat("number literal '", lit->text, "' is out of range"));
      }
      out.literals.push_back(core::Value{v});
    }
  }

  std::string templated(src);
  for (std::size_t i = literals.size(); i-- > 0;) {
    const Token *lit = literals[i];
    const std::size_t begin = lit->pos;
    const std::size_t span = lit->type == TokenType::kStringLiteral
                                 ? lit->text.size() + 2
                                 : lit->text.size();
    const std::string name = single ? "v" : absl::StrCat("v", i);
    templated = absl::StrCat(templated.substr(0, begin), ":", name,
                             templated.substr(begin + span));
  }
  out.sql_template = std::move(templated);
  return out;
}

} // namespace opaquedb::sql
