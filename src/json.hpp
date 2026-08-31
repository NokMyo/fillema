#pragma once

#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fillema::json {

class ParseError final : public std::runtime_error {
public:
    ParseError(std::size_t offset, const std::string& message);
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

private:
    std::size_t offset_;
};

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;

    Value() = default;
    Value(std::nullptr_t) noexcept;
    Value(bool value) noexcept;
    Value(double value) noexcept;
    Value(int value) noexcept;
    Value(long long value) noexcept;
    Value(std::string value);
    Value(const char* value);
    Value(Array value);
    Value(Object value);

    [[nodiscard]] bool isNull() const noexcept;
    [[nodiscard]] bool isBool() const noexcept;
    [[nodiscard]] bool isNumber() const noexcept;
    [[nodiscard]] bool isString() const noexcept;
    [[nodiscard]] bool isArray() const noexcept;
    [[nodiscard]] bool isObject() const noexcept;

    [[nodiscard]] bool asBool(bool fallback = false) const noexcept;
    [[nodiscard]] double asNumber(double fallback = 0.0) const noexcept;
    [[nodiscard]] long long asInteger(long long fallback = 0) const noexcept;
    [[nodiscard]] const std::string& asString() const;
    [[nodiscard]] const std::string& asString(const std::string& fallback) const noexcept;
    [[nodiscard]] const Array& asArray() const;
    [[nodiscard]] Array& asArray();
    [[nodiscard]] const Object& asObject() const;
    [[nodiscard]] Object& asObject();

    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
    Value& operator[](std::string key);

private:
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
    Storage storage_{nullptr};
};

[[nodiscard]] Value Parse(std::string_view source);
[[nodiscard]] std::string Stringify(const Value& value, bool pretty = true, int indentSize = 2);

} // namespace fillema::json
