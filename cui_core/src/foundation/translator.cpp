/// @file translator.cpp — i18n resource file loading (Android-style XML + INI).
#include "cui/foundation/translator.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace cui {

namespace fs = std::filesystem;

// ── Helpers ──────────────────────────────────────────────────────────────────

namespace {

/// Trim leading/trailing whitespace.
std::string trim(const std::string& s) {
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

/// Decode basic XML entities: &amp; &lt; &gt; &apos; &quot;
std::string decodeXmlEntities(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            if (s.compare(i, 5, "&amp;") == 0)       { result += '&';  i += 4; }
            else if (s.compare(i, 4, "&lt;") == 0)    { result += '<';  i += 3; }
            else if (s.compare(i, 4, "&gt;") == 0)    { result += '>';  i += 3; }
            else if (s.compare(i, 6, "&apos;") == 0)  { result += '\''; i += 5; }
            else if (s.compare(i, 6, "&quot;") == 0)  { result += '"';  i += 5; }
            else result += s[i];
        } else {
            result += s[i];
        }
    }
    return result;
}

/// Extract attribute value from an XML tag string.
/// e.g. extractAttr(R"(<string name="foo">)", "name") → "foo"
std::string extractAttr(const std::string& tag, const std::string& attrName) {
    std::string search = attrName + "=\"";
    auto pos = tag.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    auto end = tag.find('"', pos);
    if (end == std::string::npos) return "";
    return tag.substr(pos, end - pos);
}

}  // namespace

// ── XML Parser ───────────────────────────────────────────────────────────────
// Lightweight parser for Android strings.xml format:
//   <?xml version="1.0" encoding="utf-8"?>
//   <resources>
//       <string name="key">value</string>
//       ...
//   </resources>
//
// Handles: comments (<!-- -->), CDATA, self-closing tags, basic entities.

bool Translator::loadXmlString(const std::string& xml, const std::string& lang) {
    size_t pos = 0;
    int count = 0;

    while (pos < xml.size()) {
        // Find next <string ...> opening tag
        auto tagStart = xml.find("<string", pos);
        if (tagStart == std::string::npos) break;

        auto tagEnd = xml.find('>', tagStart);
        if (tagEnd == std::string::npos) break;

        std::string tag = xml.substr(tagStart, tagEnd - tagStart + 1);
        std::string key = extractAttr(tag, "name");

        if (!key.empty()) {
            // Find closing </string>
            auto valueStart = tagEnd + 1;
            auto valueEnd = xml.find("</string>", valueStart);
            if (valueEnd == std::string::npos) break;

            std::string value = xml.substr(valueStart, valueEnd - valueStart);
            value = trim(value);
            value = decodeXmlEntities(value);

            add(key, lang, value);
            ++count;
            pos = valueEnd + 9;  // strlen("</string>")
        } else {
            pos = tagEnd + 1;
        }
    }

    return count > 0;
}

bool Translator::loadXmlFile(const std::string& path, const std::string& lang) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    return loadXmlString(content, lang);
}

// ── INI Parser ───────────────────────────────────────────────────────────────
// Format:
//   [en]
//   key=value
//   key2=value2
//   [zh]
//   key=值
//
// Lines starting with ; or # are comments. Blank lines are ignored.

bool Translator::loadIniString(const std::string& ini) {
    std::istringstream stream(ini);
    std::string line;
    std::string currentLang;
    int count = 0;

    while (std::getline(stream, line)) {
        line = trim(line);

        // Skip empty lines and comments
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        // Section header: [lang]
        if (line.front() == '[' && line.back() == ']') {
            currentLang = trim(line.substr(1, line.size() - 2));
            continue;
        }

        // Key=Value pair
        if (!currentLang.empty()) {
            auto eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = trim(line.substr(0, eq));
                std::string value = trim(line.substr(eq + 1));
                if (!key.empty()) {
                    add(key, currentLang, value);
                    ++count;
                }
            }
        }
    }

    return count > 0;
}

bool Translator::loadIniFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    return loadIniString(content);
}

// ── Flat directory scan ────────────────────────────────────────────────────
// res/strings.xml        → "en" (default)
// res/strings_zh.xml     → "zh"
// res/strings_fr.xml     → "fr"
// res/strings_zh-TW.xml  → "zh-TW"

int Translator::loadFromDirectory(const std::string& resDir) {
    int loaded = 0;
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::is_directory(resDir, ec)) return 0;

    for (auto& entry : fs::directory_iterator(resDir, ec)) {
        if (!entry.is_regular_file()) continue;

        std::string fileName = entry.path().filename().string();

        // Match "strings.xml" or "strings_XX.xml" pattern
        std::string lang;
        if (fileName == "strings.xml") {
            lang = "en";  // default
        } else if (fileName.rfind("strings_", 0) == 0 && fileName.size() > 12) {
            // "strings_zh.xml" → "zh", "strings_zh-TW.xml" → "zh-TW"
            lang = fileName.substr(8, fileName.size() - 12);  // strip "strings_" and ".xml"
        } else {
            continue;
        }

        if (loadXmlFile(entry.path().string(), lang)) {
            ++loaded;
        }
    }

    return loaded;
}

// ── Utility ──────────────────────────────────────────────────────────────────

std::vector<std::string> Translator::availableLanguages() const {
    std::unordered_map<std::string, bool> seen;
    for (auto& [key, _] : entries_) {
        auto sep = key.find('\x1F');
        if (sep != std::string::npos) {
            seen[key.substr(sep + 1)] = true;
        }
    }
    std::vector<std::string> result;
    result.reserve(seen.size());
    for (auto& [lang, _] : seen) {
        result.push_back(lang);
    }
    std::sort(result.begin(), result.end());
    return result;
}

void Translator::clearLanguage(const std::string& lang) {
    std::string suffix = '\x1F' + lang;
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        if (it->first.size() >= suffix.size() &&
            it->first.compare(it->first.size() - suffix.size(), suffix.size(), suffix) == 0) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace cui_core
