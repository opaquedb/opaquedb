#include "opaquedb/sql/tokenizer.h"

#include <cctype>

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"

namespace opaquedb::sql {
namespace {

bool IsIdentifierStart(char c) {
  return absl::ascii_isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool IsIdentifierChar(char c) {
  return absl::ascii_isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Maps an identifier-spelled word to a keyword token, or kIdentifier if it is
// not a keyword. Keywords are matched case-insensitively.
TokenType KeywordOrIdentifier(std::string_view word) {
  const std::string upper = absl::AsciiStrToUpper(word);
  if (upper == "SELECT")
    return TokenType::kSelect;
  if (upper == "FROM")
    return TokenType::kFrom;
  if (upper == "WHERE")
    return TokenType::kWhere;
  if (upper == "AND")
    return TokenType::kAnd;
  if (upper == "OR")
    return TokenType::kOr;
  if (upper == "IN")
    return TokenType::kIn;
  if (upper == "LIKE")
    return TokenType::kLike;
  if (upper == "BETWEEN")
    return TokenType::kBetween;
  if (upper == "LIMIT")
    return TokenType::kLimit;
  if (upper == "OFFSET")
    return TokenType::kOffset;
  if (upper == "CREATE")
    return TokenType::kCreate;
  if (upper == "TABLE")
    return TokenType::kTable;
  if (upper == "KEY")
    return TokenType::kKey;
  if (upper == "INDEX")
    return TokenType::kIndex;
  return TokenType::kIdentifier;
}

} // namespace

absl::StatusOr<std::vector<Token>> Tokenize(std::string_view sql) {
  if (sql.size() > kMaxSqlLength) {
    return absl::InvalidArgumentError(absl::StrCat(
        "SQL is ", sql.size(), " bytes, over the ", kMaxSqlLength, " limit"));
  }

  std::vector<Token> tokens;
  std::size_t i = 0;
  const std::size_t n = sql.size();

  auto push = [&](TokenType type, std::string text,
                  std::size_t pos) -> absl::Status {
    if (tokens.size() >= kMaxTokens) {
      return absl::InvalidArgumentError("SQL has too many tokens");
    }
    tokens.push_back(Token{type, std::move(text), pos});
    return absl::OkStatus();
  };

  while (i < n) {
    const char c = sql[i];
    if (absl::ascii_isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }

    const std::size_t start = i;

    if (IsIdentifierStart(c)) {
      std::size_t j = i + 1;
      while (j < n && IsIdentifierChar(sql[j]))
        ++j;
      const std::string_view word = sql.substr(i, j - i);
      if (word.size() > kMaxIdentifierLength) {
        return absl::InvalidArgumentError("identifier is too long");
      }
      const TokenType type = KeywordOrIdentifier(word);
      std::string text =
          type == TokenType::kIdentifier ? std::string(word) : std::string();
      if (absl::Status s = push(type, std::move(text), start); !s.ok())
        return s;
      i = j;
      continue;
    }

    if (c == ':') {
      std::size_t j = i + 1;
      if (j >= n || !IsIdentifierStart(sql[j])) {
        return absl::InvalidArgumentError(absl::StrCat(
            "expected a parameter name after ':' at offset ", start));
      }
      ++j;
      while (j < n && IsIdentifierChar(sql[j]))
        ++j;
      std::string name(sql.substr(i + 1, j - (i + 1)));
      if (name.size() > kMaxIdentifierLength) {
        return absl::InvalidArgumentError("parameter name is too long");
      }
      if (absl::Status s = push(TokenType::kParameter, std::move(name), start);
          !s.ok()) {
        return s;
      }
      i = j;
      continue;
    }

    if (c == '\'' || c == '"') {
      // A string literal, single or double quoted. The client may use one as an
      // inline match value; it extracts and encrypts it and never sends it to
      // the server. The matching closing quote ends the literal.
      const char quote = c;
      std::size_t j = i + 1;
      while (j < n && sql[j] != quote) {
        if (j - i > kMaxIdentifierLength) {
          return absl::InvalidArgumentError("string literal is too long");
        }
        ++j;
      }
      if (j >= n) {
        return absl::InvalidArgumentError(
            absl::StrCat("unterminated string literal at offset ", start));
      }
      std::string text(sql.substr(i + 1, j - (i + 1)));
      if (absl::Status s =
              push(TokenType::kStringLiteral, std::move(text), start);
          !s.ok()) {
        return s;
      }
      i = j + 1;
      continue;
    }

    if (absl::ascii_isdigit(static_cast<unsigned char>(c))) {
      std::size_t j = i + 1;
      while (j < n && absl::ascii_isdigit(static_cast<unsigned char>(sql[j]))) {
        ++j;
      }
      std::string text(sql.substr(i, j - i));
      if (absl::Status s =
              push(TokenType::kNumberLiteral, std::move(text), start);
          !s.ok()) {
        return s;
      }
      i = j;
      continue;
    }

    // Single and double character operators and punctuation.
    auto two = [&](char a, char b) {
      return i + 1 < n && sql[i] == a && sql[i + 1] == b;
    };
    absl::Status status;
    if (two('<', '=')) {
      status = push(TokenType::kLe, std::string(), start);
      i += 2;
    } else if (two('>', '=')) {
      status = push(TokenType::kGe, std::string(), start);
      i += 2;
    } else if (two('<', '>')) {
      status = push(TokenType::kNe, std::string(), start);
      i += 2;
    } else if (two('!', '=')) {
      status = push(TokenType::kNe, std::string(), start);
      i += 2;
    } else {
      switch (c) {
      case ',':
        status = push(TokenType::kComma, std::string(), start);
        break;
      case '*':
        status = push(TokenType::kStar, std::string(), start);
        break;
      case '(':
        status = push(TokenType::kLParen, std::string(), start);
        break;
      case ')':
        status = push(TokenType::kRParen, std::string(), start);
        break;
      case '=':
        status = push(TokenType::kEq, std::string(), start);
        break;
      case '<':
        status = push(TokenType::kLt, std::string(), start);
        break;
      case '>':
        status = push(TokenType::kGt, std::string(), start);
        break;
      default:
        return absl::InvalidArgumentError(absl::StrCat("unexpected character '",
                                                       std::string(1, c),
                                                       "' at offset ", start));
      }
      ++i;
    }
    if (!status.ok())
      return status;
  }

  if (absl::Status s = push(TokenType::kEnd, std::string(), n); !s.ok()) {
    return s;
  }
  return tokens;
}

std::string ToString(TokenType type) {
  switch (type) {
  case TokenType::kSelect:
    return "SELECT";
  case TokenType::kFrom:
    return "FROM";
  case TokenType::kWhere:
    return "WHERE";
  case TokenType::kAnd:
    return "AND";
  case TokenType::kOr:
    return "OR";
  case TokenType::kIn:
    return "IN";
  case TokenType::kLike:
    return "LIKE";
  case TokenType::kBetween:
    return "BETWEEN";
  case TokenType::kLimit:
    return "LIMIT";
  case TokenType::kOffset:
    return "OFFSET";
  case TokenType::kCreate:
    return "CREATE";
  case TokenType::kTable:
    return "TABLE";
  case TokenType::kKey:
    return "KEY";
  case TokenType::kIndex:
    return "INDEX";
  case TokenType::kIdentifier:
    return "identifier";
  case TokenType::kParameter:
    return "parameter";
  case TokenType::kStringLiteral:
    return "string literal";
  case TokenType::kNumberLiteral:
    return "number literal";
  case TokenType::kComma:
    return "','";
  case TokenType::kStar:
    return "'*'";
  case TokenType::kLParen:
    return "'('";
  case TokenType::kRParen:
    return "')'";
  case TokenType::kEq:
    return "'='";
  case TokenType::kNe:
    return "'<>'";
  case TokenType::kLt:
    return "'<'";
  case TokenType::kLe:
    return "'<='";
  case TokenType::kGt:
    return "'>'";
  case TokenType::kGe:
    return "'>='";
  case TokenType::kEnd:
    return "end of input";
  }
  return "unknown";
}

} // namespace opaquedb::sql
