#include "prx/libkernel/AppMetadata/include/ParamJsonParser.hpp"
#include "prx/libkernel/System/include/JsonParser.hpp"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

std::string readFileToString(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("failed to open param.json");
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

const std::string& asString(const JsonValue& value, const char* context) {
    if (value.type != JsonType::String) throw std::runtime_error(std::string(context) + " is not a string in param.json");
    return value.stringValue;
}

}

ParsedParamJson parseParamJson(const std::filesystem::path& paramJsonPath) {
    const std::string text = readFileToString(paramJsonPath);
    const JsonValue root = parseJsonDocument(text);
    if (root.type != JsonType::Object) throw std::runtime_error("param.json root is not an object");
    const JsonValue* titleIdValue = findObjectMember(root, "titleId");
    if (titleIdValue == nullptr) throw std::runtime_error("param.json is missing titleId");
    const std::string titleId = asString(*titleIdValue, "titleId");
    if (titleId.empty()) throw std::runtime_error("param.json titleId is empty");
    const JsonValue* localizedParametersValue = findObjectMember(root, "localizedParameters");
    if (localizedParametersValue == nullptr) throw std::runtime_error("param.json is missing localizedParameters");
    if (localizedParametersValue->type != JsonType::Object) throw std::runtime_error("param.json localizedParameters is not an object");
    if (localizedParametersValue->objectValue.empty()) throw std::runtime_error("param.json localizedParameters is empty");
    std::string selectedLanguage;
    const JsonValue* defaultLanguageValue = findObjectMember(*localizedParametersValue, "defaultLanguage");
    if (defaultLanguageValue == nullptr) defaultLanguageValue = findObjectMember(root, "defaultLanguage");
    if (defaultLanguageValue != nullptr) selectedLanguage = asString(*defaultLanguageValue, "defaultLanguage");
    const JsonValue* languageObject = nullptr;
    if (!selectedLanguage.empty()) languageObject = findObjectMember(*localizedParametersValue, selectedLanguage);
    if (languageObject == nullptr) languageObject = findObjectMember(*localizedParametersValue, "en-US");
    if (languageObject == nullptr) {
        for (const auto& member : localizedParametersValue->objectValue) {
            if (member.second.type == JsonType::Object) {
                languageObject = &member.second;
                break;
            }
        }
    }
    if (languageObject == nullptr) throw std::runtime_error("param.json localizedParameters has no language entry");
    const JsonValue* titleValue = findObjectMember(*languageObject, "titleName");
    if (titleValue == nullptr) throw std::runtime_error("param.json localized entry is missing titleName");
    const std::string title = asString(*titleValue, "titleName");
    if (title.empty()) throw std::runtime_error("param.json titleName is empty");
    ParsedParamJson result;
    result.title = title;
    result.titleId = titleId;
    if (const JsonValue* downloadData = findObjectMember(root, "downloadDataSize")) {
        if (downloadData->type != JsonType::Number || downloadData->numberValue < 0) throw std::runtime_error("param.json downloadDataSize is not a size");
        if (downloadData->numberText.find_first_of("-.eE") == std::string::npos) {
            const auto& number = downloadData->numberText;
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), result.downloadDataSizeMiB);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size()) throw std::runtime_error("param.json downloadDataSize exceeds uint64 range");
        } else {
            if (downloadData->numberValue >= 0x1p64) throw std::runtime_error("param.json downloadDataSize exceeds uint64 range");
            result.downloadDataSizeMiB = static_cast<std::uint64_t>(downloadData->numberValue);
        }
    }
    for (int index = 0; index < 4; ++index) {
        const std::string key = "userDefinedParam" + std::to_string(index + 1);
        const JsonValue* param = findObjectMember(root, key);
        if (param == nullptr) continue;
        const auto& number = param->numberText;
        if (param->type != JsonType::Number || number.find_first_of(".eE") != std::string::npos) throw std::runtime_error("param.json " + key + " is not an integer");
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), result.userDefinedParams[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size()) throw std::runtime_error("param.json " + key + " exceeds int32 range");
    }
    return result;
}
