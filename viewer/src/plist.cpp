#include "plist.h"

#include <cstdlib>

namespace plist {

namespace {

std::string escape(std::string_view text) {
    std::string out;
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string unescape(std::string_view text) {
    static constexpr std::pair<std::string_view, char> entities[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        bool replaced = false;
        if (text[i] == '&') {
            for (auto [entity, c] : entities) {
                if (text.substr(i, entity.size()) == entity) {
                    out += c;
                    i += entity.size();
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) out += text[i++];
    }
    return out;
}

struct Tag {
    std::string_view name;
    bool closing = false;
    bool empty = false;  // <tag/>
};

class Parser {
public:
    explicit Parser(std::string_view xml) : xml_(xml) {}

    bool document(Value& out) {
        Tag tag;
        do {
            if (!next(tag)) return false;
        } while (tag.name != "plist" || tag.closing);
        if (!value(out)) return false;
        return next(tag) && tag.closing && tag.name == "plist";
    }

private:
    std::string_view xml_;
    std::size_t at_ = 0;

    /// The next element tag, skipping text between tags, the XML declaration, DOCTYPE and comments.
    bool next(Tag& tag) {
        for (;;) {
            at_ = xml_.find('<', at_);
            if (at_ == std::string_view::npos) return false;
            if (xml_.substr(at_, 4) == "<!--") {
                at_ = xml_.find("-->", at_);
                if (at_ == std::string_view::npos) return false;
                continue;
            }
            std::size_t end = xml_.find('>', at_);
            if (end == std::string_view::npos) return false;
            std::string_view inside = xml_.substr(at_ + 1, end - at_ - 1);
            at_ = end + 1;
            if (inside.empty() || inside[0] == '?' || inside[0] == '!') continue;
            tag = {};
            if (inside[0] == '/') {
                tag.closing = true;
                inside.remove_prefix(1);
            }
            if (!inside.empty() && inside.back() == '/') {
                tag.empty = true;
                inside.remove_suffix(1);
            }
            tag.name = inside.substr(0, inside.find_first_of(" \t\r\n"));
            return true;
        }
    }

    /// Text up to the closing tag of `name`.
    bool text(std::string_view name, std::string& out) {
        std::size_t end = xml_.find('<', at_);
        if (end == std::string_view::npos) return false;
        out = unescape(xml_.substr(at_, end - at_));
        at_ = end;
        Tag tag;
        return next(tag) && tag.closing && tag.name == name;
    }

    bool value(Value& out) {
        Tag tag;
        return next(tag) && !tag.closing && value(tag, out);
    }

    bool value(const Tag& tag, Value& out) {
        std::string body;
        if (tag.name == "dict") {
            out.type = Value::Type::dict;
            if (tag.empty) return true;
            for (;;) {
                Tag key;
                if (!next(key)) return false;
                if (key.closing && key.name == "dict") return true;
                if (key.name != "key") return false;
                std::string name;
                if (!key.empty && !text("key", name)) return false;
                Value item;
                if (!value(item)) return false;
                out.keys.push_back(std::move(name));
                out.items.push_back(std::move(item));
            }
        }
        if (tag.name == "array") {
            out.type = Value::Type::array;
            if (tag.empty) return true;
            for (;;) {
                Tag itemTag;
                if (!next(itemTag)) return false;
                if (itemTag.closing && itemTag.name == "array") return true;
                Value item;
                if (itemTag.closing || !value(itemTag, item)) return false;
                out.items.push_back(std::move(item));
            }
        }
        if (tag.name == "true" || tag.name == "false") {
            out.type = Value::Type::boolean;
            out.boolean = tag.name == "true";
            Tag close;
            return tag.empty || (next(close) && close.closing && close.name == tag.name);
        }
        if (!tag.empty && !text(tag.name, body)) return false;
        if (tag.name == "string") {
            out.type = Value::Type::string;
        } else if (tag.name == "integer") {
            out.type = Value::Type::integer;
            out.integer = std::strtoll(body.c_str(), nullptr, 10);
        } else if (tag.name == "real") {
            out.type = Value::Type::real;
            out.real = std::strtod(body.c_str(), nullptr);
        } else if (tag.name == "data") {
            out.type = Value::Type::data;
        } else if (tag.name == "date") {
            out.type = Value::Type::date;
        } else {
            return false;
        }
        out.text = std::move(body);
        return true;
    }
};

}  // namespace

const Value* Value::find(std::string_view key) const {
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) return &items[i];
    }
    return nullptr;
}

std::string Value::string(std::string_view key) const {
    const Value* value = find(key);
    return value && value->type == Type::string ? value->text : std::string();
}

std::int64_t Value::integerOr(std::string_view key, std::int64_t fallback) const {
    const Value* value = find(key);
    return value && value->type == Type::integer ? value->integer : fallback;
}

std::string Dict::xml() const {
    std::string out =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n";
    for (const auto& [key, value] : strings) {
        out += "\t<key>" + escape(key) + "</key>\n\t<string>" + escape(value) + "</string>\n";
    }
    for (const auto& [key, value] : integers) {
        out += "\t<key>" + escape(key) + "</key>\n\t<integer>" + std::to_string(value) + "</integer>\n";
    }
    out += "</dict>\n</plist>\n";
    return out;
}

bool parse(std::string_view xml, Value& out) {
    out = {};
    return Parser(xml).document(out);
}

}  // namespace plist
