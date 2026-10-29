#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/// Just enough of Apple's XML property lists to talk to the USB service.
namespace plist {

struct Value {
    enum class Type { none, boolean, integer, real, string, data, date, array, dict };

    Type type = Type::none;
    bool boolean = false;
    std::int64_t integer = 0;
    double real = 0;
    /// Strings, plus data and dates as their raw text.
    std::string text;
    /// Array items, or dictionary values in the order of `keys`.
    std::vector<Value> items;
    std::vector<std::string> keys;

    const Value* find(std::string_view key) const;
    std::string string(std::string_view key) const;
    std::int64_t integerOr(std::string_view key, std::int64_t fallback) const;
};

/// A dictionary of string and integer values, as XML.
struct Dict {
    std::vector<std::pair<std::string, std::string>> strings;
    std::vector<std::pair<std::string, std::int64_t>> integers;

    std::string xml() const;
};

bool parse(std::string_view xml, Value& out);

}  // namespace plist
