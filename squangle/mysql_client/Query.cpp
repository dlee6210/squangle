/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "squangle/mysql_client/Query.h"
#include <folly/Format.h>
#include <folly/String.h>

#include <boost/algorithm/string.hpp>
#include <boost/variant.hpp>
#include <boost/variant/get.hpp>
#include <algorithm>
#include <vector>

namespace facebook {
namespace common {
namespace mysql_client {

#define TYPE_CHECK(expected) \
  if (type_ != expected)     \
  throw std::invalid_argument("DataType doesn't match with the call")

typedef std::pair<folly::fbstring, QueryArgument> ArgPair;

// fbstring constructors
QueryArgument::QueryArgument(StringPiece val)
    : value_(fbstring(val.data(), val.size())) {}

QueryArgument::QueryArgument(char const* val) : value_(fbstring(val)) {}

QueryArgument::QueryArgument(const std::string& string_value)
    : value_(fbstring(string_value)) {}

QueryArgument::QueryArgument(const fbstring& val) : value_(val) {}

QueryArgument::QueryArgument(fbstring&& val) : value_(std::move(val)) {}

QueryArgument::QueryArgument(double double_val) : value_(double_val) {}

QueryArgument::QueryArgument(std::initializer_list<QueryArgument> list)
    : value_(std::vector<QueryArgument>(list.begin(), list.end())) {}

QueryArgument::QueryArgument(std::vector<QueryArgument> arg_list)
    : value_(std::vector<QueryArgument>(arg_list.begin(), arg_list.end())) {}

QueryArgument::QueryArgument() : value_(std::vector<ArgPair>()) {}

QueryArgument::QueryArgument(folly::fbstring param1, QueryArgument param2)
    : value_(std::vector<ArgPair>()) {
  getPairs().emplace_back(ArgPair(param1, param2));
}

bool QueryArgument::isString() const {
  return value_.type() == typeid(folly::fbstring);
}

bool QueryArgument::isQuery() const {
  return value_.type() == typeid(Query);
}

bool QueryArgument::isPairList() const {
  return value_.type() == typeid(std::vector<ArgPair>);
}

bool QueryArgument::isBool() const {
  return value_.type() == typeid(bool);
}

bool QueryArgument::isNull() const {
  return value_.type() == typeid(std::nullptr_t);
}

bool QueryArgument::isList() const {
  return value_.type() == typeid(std::vector<QueryArgument>);
}

bool QueryArgument::isDouble() const {
  return value_.type() == typeid(double);
}

bool QueryArgument::isInt() const {
  return value_.type() == typeid(int64_t);
}

QueryArgument&& QueryArgument::operator()(
    const folly::fbstring& q1,
    const QueryArgument& q2) {
  getPairs().emplace_back(ArgPair(q1, q2));
  return std::move(*this);
}

QueryArgument&& QueryArgument::operator()(
    folly::fbstring&& q1,
    QueryArgument&& q2) {
  getPairs().emplace_back(ArgPair(std::move(q1), std::move(q2)));
  return std::move(*this);
}

struct FbStringConverter : public boost::static_visitor<fbstring> {
  fbstring operator()(const double& operand) const {
    return folly::to<fbstring>(operand);
  }
  fbstring operator()(const bool& operand) const {
    return folly::to<fbstring>(operand);
  }
  fbstring operator()(const int64_t& operand) const {
    return folly::to<fbstring>(operand);
  }
  fbstring operator()(const fbstring& operand) const {
    return folly::to<fbstring>(operand);
  }
  template <typename T>
  fbstring operator()(const T& operand) const {
    throw std::invalid_argument(
        "Only allowed type conversions are Int, Double, Bool and String");
  }
};

folly::fbstring QueryArgument::asString() const {
  return boost::apply_visitor(FbStringConverter(), value_);
}

double QueryArgument::getDouble() const {
  return boost::get<double>(value_);
}

int64_t QueryArgument::getInt() const {
  return boost::get<int64_t>(value_);
}

bool QueryArgument::getBool() const {
  return boost::get<bool>(value_);
}

const folly::fbstring& QueryArgument::getString() const {
  return boost::get<fbstring>(value_);
}

const std::vector<QueryArgument>& QueryArgument::getList() const {
  return boost::get<std::vector<QueryArgument>>(value_);
}

const std::vector<ArgPair>& QueryArgument::getPairs() const {
  return boost::get<std::vector<ArgPair>>(value_);
}

void QueryArgument::initFromDynamic(const folly::dynamic& param) {
  // Convert to basic values and get type
  if (param.isObject()) {
    // List of pairs
    std::vector<dynamic> keys(param.keys().begin(), param.keys().end());
    std::sort(keys.begin(), keys.end());
    value_ = std::vector<ArgPair>();

    for (const auto& key : keys) {
      QueryArgument q2(param[key]);

      boost::get<std::vector<ArgPair>>(value_).emplace_back(
          ArgPair(key.asString(), q2));
    }
  } else if (param.isNull()) {
    value_ = nullptr;
  } else if (param.isArray()) {
    value_ = std::vector<QueryArgument>();
    auto& v = boost::get<std::vector<QueryArgument>>(value_);
    for (const auto& val : param) {
      v.emplace_back(QueryArgument(val));
    }
  } else if (param.isString()) {
    value_ = fbstring(param.getString());
  } else if (param.isBool()) {
    value_ = param.asBool();
  } else if (param.isDouble()) {
    value_ = param.asDouble();
  } else if (param.isInt()) {
    value_ = param.asInt();
  } else {
    throw std::invalid_argument("Dynamic type doesn't match to accepted ones");
  }
}

std::vector<ArgPair>& QueryArgument::getPairs() {
  return boost::get<std::vector<ArgPair>>(value_);
}

Query::Query(const StringPiece query_text, std::vector<QueryArgument> params)
    : query_text_(query_text),
      unsafe_query_(false),
      params_(params) {}

Query::~Query() {}

namespace {

// Some helper functions for encoding/escaping.
void appendComment(folly::fbstring* s, const QueryArgument& d) {
  auto str = d.asString();
  boost::replace_all(str, "/*", " / * ");
  boost::replace_all(str, "*/", " * / ");
  s->append(str);
}

void appendColumnTableName(folly::fbstring* s, const QueryArgument& d) {
  if (d.isString()) {
    s->reserve(s->size() + d.getString().size() + 4);
    s->push_back('`');
    for (char c : d.getString()) {
      // Toss in an extra ` if we see one.
      if (c == '`') {
        s->push_back('`');
      }
      s->push_back(c);
    }
    s->push_back('`');
  } else {
    s->append(d.asString());
  }
}

// Raise an exception with, hopefully, a helpful error message.
void parseError(const StringPiece s, size_t offset, const StringPiece message) {
  const std::string msg =
      folly::format(
          "Parse error at offset {}: {}, query: {}", offset, message, s)
          .str();
  throw std::invalid_argument(msg);
}

// Raise an exception for format string/value mismatches
void formatStringParseError(
    StringPiece query_text,
    size_t offset,
    char format_specifier,
    StringPiece value_type) {
  parseError(
      query_text,
      offset,
      folly::sformat(
          "invalid value type {} for format string %{}",
          value_type,
          format_specifier));
}

// Consume the next x bytes from s, updating offset, and raising an
// exception if there aren't sufficient bytes left.
StringPiece advance(const StringPiece s, size_t* offset, size_t num) {
  if (s.size() <= *offset + num) {
    parseError(s, *offset, "unexpected end of string");
  }
  *offset += num;
  return StringPiece(s.data() + *offset - num + 1, s.data() + *offset + 1);
}

// Escape a string (or copy it through unmodified if no connection is
// available).
void appendEscapedString(
    folly::fbstring* dest,
    const folly::fbstring& value,
    MYSQL* connection) {
  if (!connection) {
    VLOG(3) << "connectionless escape performed; this should only occur in "
            << "testing.";
    *dest += value;
    return;
  }

  size_t old_size = dest->size();
  dest->resize(old_size + 2 * value.size() + 1);
  size_t actual_value_size = mysql_real_escape_string(
      connection, &(*dest)[old_size], value.data(), value.size());
  dest->resize(old_size + actual_value_size);
}

} // namespace

void Query::append(const Query& query2) {
  query_text_ += query2.query_text_;
  for (const auto& param2 : query2.params_) {
    params_.push_back(param2);
  }
}

void Query::append(Query&& query2) {
  query_text_ += query2.query_text_;
  for (const auto& param2 : query2.params_) {
    params_.push_back(std::move(param2));
  }
}

// Append a dynamic to the query string we're building.  We ensure the
// type matches the dynamic's type (or allow a magic 'v' type to be
// any value, but this isn't exposed to the users of the library).
void Query::appendValue(folly::fbstring* s,
                        size_t offset,
                        char type,
                        const QueryArgument& d,
                        MYSQL* connection) const {

  auto querySp = query_text_.getQuery();

  if (d.isString()) {
    if (type != 's' && type != 'v') {
      formatStringParseError(querySp, offset, type, "string");
    }
    auto value = d.asString();
    s->reserve(s->size() + value.size() + 4);
    s->push_back('"');
    appendEscapedString(s, value, connection);
    s->push_back('"');
  } else if (d.isInt()) {
    if (type != 'd' && type != 'v') {
      formatStringParseError(querySp, offset, type, "int");
    }
    s->append(d.asString());
  } else if (d.isDouble()) {
    if (type != 'f' && type != 'v') {
      formatStringParseError(querySp, offset, type, "double");
    }
    s->append(d.asString());
  } else if (d.isNull()) {
    s->append("NULL");
  } else {
    formatStringParseError(querySp, offset, type, d.typeName());
  }
}

void Query::appendValueClauses(folly::fbstring* ret,
                               size_t* idx,
                               const char* sep,
                               const QueryArgument& param,
                               MYSQL* connection) const {

  auto querySp = query_text_.getQuery();

  if (!param.isPairList()) {
    parseError(
        querySp,
        *idx,
        folly::format(
            "object expected for %Lx but received {}", param.typeName())
            .str());
  }
  // Sort these to get consistent query ordering (mainly for
  // testing, but also aesthetics of the final query).
  bool first_param = true;
  for (const auto& key_value : param.getPairs()) {
    if (!first_param) {
      ret->append(sep);
    }
    first_param = false;
    appendColumnTableName(ret, key_value.first);
    if (key_value.second.isNull() && sep[0] != ',') {
      ret->append(" IS NULL");
    } else {
      ret->append(" = ");
      appendValue(ret, *idx, 'v', key_value.second, connection);
    }
  }
}

folly::fbstring Query::renderMultiQuery(
    MYSQL* connection,
    const std::vector<Query>& queries) {
  auto reserve_size = 0;
  for (const Query& query : queries) {
    reserve_size +=
        query.query_text_.getQuery().size() + 8 * query.params_.size();
  }
  folly::fbstring ret;
  ret.reserve(reserve_size);

  // Not adding `;` in the end
  for (const Query& query : queries) {
    if (!ret.empty()) {
      ret.append(";");
    }
    ret.append(query.render(connection));
  }

  return ret;
}

folly::fbstring Query::renderInsecure() const {
  return render(nullptr, params_);
}

folly::fbstring Query::renderInsecure(
    const std::vector<QueryArgument>& params) const {
  return render(nullptr, params);
}

folly::fbstring Query::render(MYSQL* conn) const {
  return render(conn, params_);
}

folly::fbstring Query::render(MYSQL* conn,
                              const std::vector<QueryArgument>& params) const {

  auto querySp = query_text_.getQuery();

  if (unsafe_query_) {
    return querySp.toFbstring();
  }

  auto offset = querySp.find_first_of(";'\"`");
  if (offset != StringPiece::npos) {
    parseError(querySp, offset, "Saw dangerous characters in SQL query");
  }

  folly::fbstring ret;
  ret.reserve(querySp.size() + 8 * params.size());

  auto current_param = params.begin();
  bool after_percent = false;
  size_t idx;
  // Walk our string, watching for % values.
  for (idx = 0; idx < querySp.size(); ++idx) {
    char c = querySp[idx];
    if (!after_percent) {
      if (c != '%') {
        ret.push_back(c);
      } else {
        after_percent = true;
      }
      continue;
    }

    after_percent = false;
    if (c == '%') {
      ret.push_back('%');
      continue;
    }

    if (current_param == params.end()) {
      parseError(querySp, idx, "too few parameters for query");
    }

    const auto& param = *current_param++;
    if (c == 'd' || c == 's' || c == 'f') {
      appendValue(&ret, idx, c, param, conn);
    } else if (c == 'K') {
      ret.append("/*");
      appendComment(&ret, param);
      ret.append("*/");
    } else if (c == 'T' || c == 'C') {
      appendColumnTableName(&ret, param);
    } else if (c == '=') {
      StringPiece type = advance(querySp, &idx, 1);
      if (type != "d" && type != "s" && type != "f") {
        parseError(querySp, idx, "expected %=d, %=c, or %=s");
      }

      if (param.isNull()) {
        ret.append(" IS NULL");
      } else {
        ret.append(" = ");
        appendValue(&ret, idx, type[0], param, conn);
      }
    } else if (c == 'V') {
      if (param.isQuery()) {
        parseError(querySp, idx, "%V doesn't allow subquery");
      }
      size_t col_idx;
      size_t row_len = 0;
      bool first_row = true;
      bool first_in_row = true;
      for (const auto& row : param.getList()) {
        first_in_row = true;
        col_idx = 0;
        if (!first_row) {
          ret.append(", ");
        }
        ret.append("(");
        for (const auto& col : row.getList()) {
          if (!first_in_row) {
            ret.append(", ");
          }
          appendValue(&ret, idx, 'v', col, conn);
          col_idx++;
          first_in_row = false;
          if (first_row) {
            row_len++;
          }
        }
        ret.append(")");
        if (first_row) {
          first_row = false;
        } else if (col_idx != row_len) {
          parseError(
              querySp,
              idx,
              "not all rows provided for %V formatter are the same size");
        }
      }
    } else if (c == 'L') {
      StringPiece type = advance(querySp, &idx, 1);
      if (type == "O" || type == "A") {
        ret.append("(");
        const char* sep = (type == "O") ? " OR " : " AND ";
        appendValueClauses(&ret, &idx, sep, param, conn);
        ret.append(")");
      } else {
        if (!param.isList()) {
          parseError(querySp, idx, "expected array for %L formatter");
        }

        bool first_param = true;
        for (const auto& val : param.getList()) {
          if (!first_param) {
            ret.append(", ");
          }
          first_param = false;
          if (type == "C") {
            appendColumnTableName(&ret, val);
          } else {
            appendValue(&ret, idx, type[0], val, conn);
          }
        }
      }
    } else if (c == 'U' || c == 'W') {
      if (c == 'W') {
        appendValueClauses(&ret, &idx, " AND ", param, conn);
      } else {
        appendValueClauses(&ret, &idx, ", ", param, conn);
      }
    } else if (c == 'Q') {
      ret.append((param).asString());
    } else {
      parseError(querySp, idx, "unknown % code");
    }
  }

  if (after_percent) {
    parseError(querySp, idx, "string ended with unfinished % code");
  }

  if (current_param != params.end()) {
    parseError(querySp, 0, "too many parameters specified for query");
  }

  return ret;
}

folly::StringPiece MultiQuery::renderQuery(MYSQL* conn) {
  if (queries_.size() == 1 && queries_[0].isUnsafe()) {
    return queries_[0].getQueryFormat();
  }
  rendered_multi_query_ = Query::renderMultiQuery(conn, queries_);
  return folly::StringPiece(rendered_multi_query_);
}

}
}
} // namespace facebook::common::mysql_client
