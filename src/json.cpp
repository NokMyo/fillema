#include "json.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

namespace fillema::json {
namespace {

std::string ErrorText(std::size_t offset, const std::string& message) {
    return "JSON parse error at byte " + std::to_string(offset) + ": " + message;
}

void AppendUtf8(std::string& out, unsigned codepoint) {
    if (codepoint <= 0x7fU) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        out.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        out.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        out.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
}

class Parser {
public:
    explicit Parser(std::string_view source) : source_(source) {}

    Value parse() {
        skipWhitespace();
        Value result = parseValue();
        skipWhitespace();
        if (position_ != source_.size()) {
            fail("unexpected trailing characters");
        }
        return result;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw ParseError(position_, message);
    }

    void skipWhitespace() {
        while (position_ < source_.size()) {
            const char c = source_[position_];
            if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
                break;
            }
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ < source_.size() && source_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    Value parseValue() {
        if (position_ >= source_.size()) {
            fail("expected a value");
        }
        switch (source_[position_]) {
        case 'n': return parseLiteral("null", Value{});
        case 't': return parseLiteral("true", Value{true});
        case 'f': return parseLiteral("false", Value{false});
        case '"': return Value{parseString()};
        case '[': return parseArray();
        case '{': return parseObject();
        default:
            if (source_[position_] == '-' || (source_[position_] >= '0' && source_[position_] <= '9')) {
                return Value{parseNumber()};
            }
            fail("unexpected token");
        }
    }

    Value parseLiteral(std::string_view literal, Value value) {
        if (source_.substr(position_, literal.size()) != literal) {
            fail("invalid literal");
        }
        position_ += literal.size();
        return value;
    }

    std::string parseString() {
        if (!consume('"')) {
            fail("expected a string");
        }
        std::string result;
        while (position_ < source_.size()) {
            const unsigned char c = static_cast<unsigned char>(source_[position_++]);
            if (c == '"') {
                return result;
            }
            if (c < 0x20U) {
                fail("control character in string");
            }
            if (c != '\\') {
                result.push_back(static_cast<char>(c));
                continue;
            }
            if (position_ >= source_.size()) {
                fail("unfinished escape sequence");
            }
            const char escaped = source_[position_++];
            switch (escaped) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                const unsigned first = parseHexQuad();
                if (first >= 0xd800U && first <= 0xdbffU) {
                    if (position_ + 2 > source_.size() || source_[position_] != '\\' || source_[position_ + 1] != 'u') {
                        fail("missing low surrogate");
                    }
                    position_ += 2;
                    const unsigned second = parseHexQuad();
                    if (second < 0xdc00U || second > 0xdfffU) {
                        fail("invalid low surrogate");
                    }
                    AppendUtf8(result, 0x10000U + ((first - 0xd800U) << 10U) + (second - 0xdc00U));
                } else {
                    AppendUtf8(result, first);
                }
                break;
            }
            default: fail("unknown escape sequence");
            }
        }
        fail("unterminated string");
    }

    unsigned parseHexQuad() {
        if (position_ + 4 > source_.size()) {
            fail("unfinished unicode escape");
        }
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = source_[position_++];
            value <<= 4U;
            if (c >= '0' && c <= '9') value += static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') value += static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value += static_cast<unsigned>(c - 'A' + 10);
            else fail("invalid unicode escape");
        }
        return value;
    }

    double parseNumber() {
        const std::size_t begin = position_;
        consume('-');
        if (consume('0')) {
            if (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9') {
                fail("leading zero in number");
            }
        } else {
            if (position_ >= source_.size() || source_[position_] < '1' || source_[position_] > '9') {
                fail("invalid number");
            }
            while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9') ++position_;
        }
        if (consume('.')) {
            if (position_ >= source_.size() || source_[position_] < '0' || source_[position_] > '9') fail("invalid fraction");
            while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9') ++position_;
        }
        if (position_ < source_.size() && (source_[position_] == 'e' || source_[position_] == 'E')) {
            ++position_;
            if (position_ < source_.size() && (source_[position_] == '+' || source_[position_] == '-')) ++position_;
            if (position_ >= source_.size() || source_[position_] < '0' || source_[position_] > '9') fail("invalid exponent");
            while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9') ++position_;
        }
        const std::string token{source_.substr(begin, position_ - begin)};
        char* end = nullptr;
        const double number = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(number)) fail("number is outside the supported range");
        return number;
    }

    Value parseArray() {
        consume('[');
        Value::Array result;
        skipWhitespace();
        if (consume(']')) return Value{std::move(result)};
        while (true) {
            skipWhitespace();
            result.push_back(parseValue());
            skipWhitespace();
            if (consume(']')) return Value{std::move(result)};
            if (!consume(',')) fail("expected ',' or ']'");
        }
    }

    Value parseObject() {
        consume('{');
        Value::Object result;
        skipWhitespace();
        if (consume('}')) return Value{std::move(result)};
        while (true) {
            skipWhitespace();
            if (position_ >= source_.size() || source_[position_] != '"') fail("expected an object key");
            std::string key = parseString();
            skipWhitespace();
            if (!consume(':')) fail("expected ':'");
            skipWhitespace();
            result.insert_or_assign(std::move(key), parseValue());
            skipWhitespace();
            if (consume('}')) return Value{std::move(result)};
            if (!consume(',')) fail("expected ',' or '}'");
        }
    }

    std::string_view source_;
    std::size_t position_ = 0;
};

void WriteEscaped(std::ostringstream& stream, const std::string& value) {
    stream << '"';
    for (const unsigned char c : value) {
        switch (c) {
        case '"': stream << "\\\""; break;
        case '\\': stream << "\\\\"; break;
        case '\b': stream << "\\b"; break;
        case '\f': stream << "\\f"; break;
        case '\n': stream << "\\n"; break;
        case '\r': stream << "\\r"; break;
        case '\t': stream << "\\t"; break;
        default:
            if (c < 0x20U) {
                stream << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
            } else {
                stream << static_cast<char>(c);
            }
        }
    }
    stream << '"';
}

void WriteValue(std::ostringstream& stream, const Value& value, bool pretty, int indentSize, int depth) {
    const auto newline = [&]() {
        if (pretty) stream << '\n' << std::string(static_cast<std::size_t>((depth + 1) * indentSize), ' ');
    };
    if (value.isNull()) stream << "null";
    else if (value.isBool()) stream << (value.asBool() ? "true" : "false");
    else if (value.isNumber()) {
        stream << std::setprecision(15) << value.asNumber();
    } else if (value.isString()) {
        WriteEscaped(stream, value.asString());
    } else if (value.isArray()) {
        const auto& array = value.asArray();
        stream << '[';
        for (std::size_t index = 0; index < array.size(); ++index) {
            if (index) stream << ',';
            newline();
            WriteValue(stream, array[index], pretty, indentSize, depth + 1);
        }
        if (pretty && !array.empty()) stream << '\n' << std::string(static_cast<std::size_t>(depth * indentSize), ' ');
        stream << ']';
    } else {
        const auto& object = value.asObject();
        stream << '{';
        std::size_t index = 0;
        for (const auto& [key, child] : object) {
            if (index++) stream << ',';
            newline();
            WriteEscaped(stream, key);
            stream << (pretty ? ": " : ":");
            WriteValue(stream, child, pretty, indentSize, depth + 1);
        }
        if (pretty && !object.empty()) stream << '\n' << std::string(static_cast<std::size_t>(depth * indentSize), ' ');
        stream << '}';
    }
}

} // namespace

ParseError::ParseError(std::size_t offset, const std::string& message)
    : std::runtime_error(ErrorText(offset, message)), offset_(offset) {}

Value::Value(std::nullptr_t) noexcept : storage_(nullptr) {}
Value::Value(bool value) noexcept : storage_(value) {}
Value::Value(double value) noexcept : storage_(value) {}
Value::Value(int value) noexcept : storage_(static_cast<double>(value)) {}
Value::Value(long long value) noexcept : storage_(static_cast<double>(value)) {}
Value::Value(std::string value) : storage_(std::move(value)) {}
Value::Value(const char* value) : storage_(std::string(value ? value : "")) {}
Value::Value(Array value) : storage_(std::move(value)) {}
Value::Value(Object value) : storage_(std::move(value)) {}

bool Value::isNull() const noexcept { return std::holds_alternative<std::nullptr_t>(storage_); }
bool Value::isBool() const noexcept { return std::holds_alternative<bool>(storage_); }
bool Value::isNumber() const noexcept { return std::holds_alternative<double>(storage_); }
bool Value::isString() const noexcept { return std::holds_alternative<std::string>(storage_); }
bool Value::isArray() const noexcept { return std::holds_alternative<Array>(storage_); }
bool Value::isObject() const noexcept { return std::holds_alternative<Object>(storage_); }

bool Value::asBool(bool fallback) const noexcept {
    if (const auto* value = std::get_if<bool>(&storage_)) return *value;
    return fallback;
}
double Value::asNumber(double fallback) const noexcept {
    if (const auto* value = std::get_if<double>(&storage_)) return *value;
    return fallback;
}
long long Value::asInteger(long long fallback) const noexcept {
    if (const auto* value = std::get_if<double>(&storage_)) return static_cast<long long>(*value);
    return fallback;
}
const std::string& Value::asString() const { return std::get<std::string>(storage_); }
const std::string& Value::asString(const std::string& fallback) const noexcept {
    if (const auto* value = std::get_if<std::string>(&storage_)) return *value;
    return fallback;
}
const Value::Array& Value::asArray() const { return std::get<Array>(storage_); }
Value::Array& Value::asArray() { return std::get<Array>(storage_); }
const Value::Object& Value::asObject() const { return std::get<Object>(storage_); }
Value::Object& Value::asObject() { return std::get<Object>(storage_); }
const Value* Value::find(std::string_view key) const noexcept {
    const auto* object = std::get_if<Object>(&storage_);
    if (!object) return nullptr;
    const auto iterator = object->find(key);
    return iterator == object->end() ? nullptr : &iterator->second;
}
Value& Value::operator[](std::string key) {
    if (!isObject()) storage_ = Object{};
    return std::get<Object>(storage_)[std::move(key)];
}

Value Parse(std::string_view source) { return Parser(source).parse(); }

std::string Stringify(const Value& value, bool pretty, int indentSize) {
    std::ostringstream stream;
    WriteValue(stream, value, pretty, indentSize, 0);
    if (pretty) stream << '\n';
    return stream.str();
}

} // namespace fillema::json
