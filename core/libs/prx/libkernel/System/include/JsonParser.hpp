#ifndef CORE_LIBS_PRX_LIBKERNEL_SYSTEM_INCLUDE_JSONPARSER_HPP
#define CORE_LIBS_PRX_LIBKERNEL_SYSTEM_INCLUDE_JSONPARSER_HPP

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

enum class JsonType { Null, Boolean, Number, String, Array, Object };

struct JsonValue;

using JsonArray = std::vector<JsonValue>;
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;

struct JsonValue {
    JsonType type = JsonType::Null;
    bool boolValue = false;
    double numberValue = 0.0;
    std::string numberText;
    std::string stringValue;
    JsonArray arrayValue;
    JsonObject objectValue;
};

inline void skipWhitespace(const std::string& text, std::size_t& position) {
    while (position < text.size()) {
        const char c = text[position];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++position; else break;
    }
}

inline char peekChar(const std::string& text, std::size_t position) {
    if (position >= text.size()) throw std::runtime_error("unexpected end of param.json");
    return text[position];
}

inline void expectChar(const std::string& text, std::size_t& position, char expected) {
    if (position >= text.size() || text[position] != expected) throw std::runtime_error("malformed param.json");
    ++position;
}

inline void expectLiteral(const std::string& text, std::size_t& position, const char* literal) {
    for (const char* p = literal; *p != '\0'; ++p) {
        if (position >= text.size() || text[position] != *p) throw std::runtime_error("malformed param.json");
        ++position;
    }
}

inline void appendUtf8(std::string& destination, std::uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        destination.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        destination.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        destination.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        destination.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        destination.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        destination.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0x10FFFF) {
        destination.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        destination.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        destination.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        destination.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        throw std::runtime_error("invalid unicode codepoint in param.json");
    }
}

inline std::uint32_t parseHex4(const std::string& text, std::size_t& position) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        if (position >= text.size()) throw std::runtime_error("malformed unicode escape in param.json");
        const char c = text[position];
        value <<= 4;
        if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
        else throw std::runtime_error("malformed unicode escape in param.json");
        ++position;
    }
    return value;
}

inline void appendRawUtf8(const std::string& text, std::size_t& position, unsigned char first, std::string& result) {
    unsigned count;
    std::uint32_t codepoint;
    std::uint32_t minimum;
    if (first >= 0xC2 && first <= 0xDF) { count = 1; codepoint = first & 0x1F; minimum = 0x80; }
    else if (first >= 0xE0 && first <= 0xEF) { count = 2; codepoint = first & 0x0F; minimum = 0x800; }
    else if (first >= 0xF0 && first <= 0xF4) { count = 3; codepoint = first & 0x07; minimum = 0x10000; }
    else throw std::runtime_error("invalid UTF-8 in param.json");
    if (count > text.size() - position) throw std::runtime_error("truncated UTF-8 in param.json");
    for (unsigned index = 0; index < count; ++index) {
        const auto byte = static_cast<unsigned char>(text[position + index]);
        if ((byte & 0xC0) != 0x80) throw std::runtime_error("invalid UTF-8 continuation in param.json");
        codepoint = (codepoint << 6) | (byte & 0x3F);
    }
    if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
        throw std::runtime_error("invalid UTF-8 codepoint in param.json");
    result.push_back(static_cast<char>(first));
    result.append(text, position, count);
    position += count;
}


inline std::string parseJsonString(const std::string& text, std::size_t& position) {
    expectChar(text, position, '"');
    std::string result;
    while (true) {
        if (position >= text.size()) throw std::runtime_error("unterminated string in param.json");
        const char c = text[position++];
        if (c == '"') break;
        if (static_cast<unsigned char>(c) < 0x20) throw std::runtime_error("control character in param.json string");
        if (c != '\\') {
            if (static_cast<unsigned char>(c) < 0x80) result.push_back(c);
            else appendRawUtf8(text, position, static_cast<unsigned char>(c), result);
            continue;
        }
        if (position >= text.size()) throw std::runtime_error("unterminated escape in param.json");
        const char escape = text[position++];
        switch (escape) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                std::uint32_t codepoint = parseHex4(text, position);
                if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                    if (position + 1 >= text.size() || text[position] != '\\' || text[position + 1] != 'u') throw std::runtime_error("unpaired surrogate in param.json");
                    position += 2;
                    const std::uint32_t low = parseHex4(text, position);
                    if (low < 0xDC00 || low > 0xDFFF) throw std::runtime_error("invalid surrogate pair in param.json");
                    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                    throw std::runtime_error("unpaired surrogate in param.json");
                }
                appendUtf8(result, codepoint);
                break;
            }
            default: throw std::runtime_error("invalid escape sequence in param.json");
        }
    }
    return result;
}

inline JsonValue parseJsonValue(const std::string& text, std::size_t& position, std::size_t depth, std::size_t maxDepth, bool rejectDuplicates);

inline JsonValue parseJsonObject(const std::string& text, std::size_t& position, std::size_t depth, std::size_t maxDepth, bool rejectDuplicates) {
    expectChar(text, position, '{');
    JsonValue value;
    value.type = JsonType::Object;
    skipWhitespace(text, position);
    if (peekChar(text, position) == '}') { ++position; return value; }
    while (true) {
        skipWhitespace(text, position);
        std::string key = parseJsonString(text, position);
        skipWhitespace(text, position);
        expectChar(text, position, ':');
        JsonValue member = parseJsonValue(text, position, depth + 1, maxDepth, rejectDuplicates);
        if (rejectDuplicates) {
            for (const auto& existing : value.objectValue)
                if (existing.first == key) throw std::runtime_error("duplicate JSON object member");
        }
        value.objectValue.emplace_back(std::move(key), std::move(member));
        skipWhitespace(text, position);
        const char next = peekChar(text, position);
        if (next == ',') { ++position; continue; }
        if (next == '}') { ++position; break; }
        throw std::runtime_error("malformed object in param.json");
    }
    return value;
}

inline JsonValue parseJsonArray(const std::string& text, std::size_t& position, std::size_t depth, std::size_t maxDepth, bool rejectDuplicates) {
    expectChar(text, position, '[');
    JsonValue value;
    value.type = JsonType::Array;
    skipWhitespace(text, position);
    if (peekChar(text, position) == ']') { ++position; return value; }
    while (true) {
        JsonValue element = parseJsonValue(text, position, depth + 1, maxDepth, rejectDuplicates);
        value.arrayValue.push_back(std::move(element));
        skipWhitespace(text, position);
        const char next = peekChar(text, position);
        if (next == ',') { ++position; continue; }
        if (next == ']') { ++position; break; }
        throw std::runtime_error("malformed array in param.json");
    }
    return value;
}

inline JsonValue parseJsonNumber(const std::string& text, std::size_t& position) {
    const std::size_t start = position;
    if (peekChar(text, position) == '-') ++position;
    if (position >= text.size() || text[position] < '0' || text[position] > '9') throw std::runtime_error("malformed number in param.json");
    if (text[position] == '0') {
        ++position;
        if (position < text.size() && text[position] >= '0' && text[position] <= '9') throw std::runtime_error("malformed number in param.json");
    } else {
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') ++position;
    }
    if (position < text.size() && text[position] == '.') {
        ++position;
        if (position >= text.size() || text[position] < '0' || text[position] > '9') throw std::runtime_error("malformed number in param.json");
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') ++position;
    }
    if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        if (position < text.size() && (text[position] == '+' || text[position] == '-')) ++position;
        if (position >= text.size() || text[position] < '0' || text[position] > '9') throw std::runtime_error("malformed number in param.json");
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') ++position;
    }
    JsonValue value;
    value.type = JsonType::Number;
    value.numberText = text.substr(start, position - start);
    value.numberValue = std::stod(value.numberText);
    return value;
}

inline JsonValue parseJsonValue(const std::string& text, std::size_t& position, std::size_t depth, std::size_t maxDepth, bool rejectDuplicates) {
    skipWhitespace(text, position);
    const char c = peekChar(text, position);
    if (depth > maxDepth) throw std::runtime_error("JSON nesting limit exceeded");
    if (c == '{') return parseJsonObject(text, position, depth, maxDepth, rejectDuplicates);
    if (c == '[') return parseJsonArray(text, position, depth, maxDepth, rejectDuplicates);
    if (c == '"') { JsonValue value; value.type = JsonType::String; value.stringValue = parseJsonString(text, position); return value; }
    if (c == 't') { expectLiteral(text, position, "true"); JsonValue value; value.type = JsonType::Boolean; value.boolValue = true; return value; }
    if (c == 'f') { expectLiteral(text, position, "false"); JsonValue value; value.type = JsonType::Boolean; value.boolValue = false; return value; }
    if (c == 'n') { expectLiteral(text, position, "null"); JsonValue value; value.type = JsonType::Null; return value; }
    if (c == '-' || (c >= '0' && c <= '9')) return parseJsonNumber(text, position);
    throw std::runtime_error("unexpected character in param.json");
}

inline JsonValue parseJsonDocument(const std::string& text, std::size_t maxDepth = std::numeric_limits<std::size_t>::max(), bool rejectDuplicates = false) {
    std::size_t position = 0;
    JsonValue root = parseJsonValue(text, position, 0, maxDepth, rejectDuplicates);
    skipWhitespace(text, position);
    if (position != text.size()) throw std::runtime_error("unexpected trailing data in param.json");
    return root;
}

inline const JsonValue* findObjectMember(const JsonValue& object, const std::string& key) {
    if (object.type != JsonType::Object) throw std::runtime_error("expected JSON object in param.json");
    for (const auto& member : object.objectValue) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

}

#endif
