#pragma once
/// @file translator.h — i18n with external XML/INI resource loading (Android-style).
///
/// Supports loading translations from external resource files at runtime,
/// similar to Android's res/strings.xml mechanism:
///
///   // Android-style XML (res/strings.xml):
///   <resources>
///       <string name="home">Home</string>
///   </resources>
///
///   // Or simple INI format (translations.ini):
///   [en]
///   home=Home
///   [zh]
///   home=主页
///
/// Usage:
///   // Load all languages from a directory structure:
///   Translator::instance().loadFromDirectory("res");
///   //   → loads res/strings.xml      (default / English)
///   //   → loads res/strings-zh.xml   (Chinese)
///
///   // Or load individual files:
///   Translator::instance().loadXmlFile("strings.xml", "zh");
///   Translator::instance().loadIniFile("translations.ini");
///
///   // Switch language at runtime:
///   Translator::instance().setLanguage("zh");
///   auto t = tr("home");   // "主页"
///
///   // Programmatic registration still works:
///   Translator::instance().add("Home", "zh", "主页");

#include <string>
#include <unordered_map>
#include <vector>

namespace cui {

class Translator {
public:
    static Translator& instance() {
        static Translator t;
        return t;
    }

    // ── Language control ──────────────────────────────────────────────────
    void setLanguage(const std::string& lang) { lang_ = lang; }
    const std::string& language() const { return lang_; }

    /// All languages that have at least one entry.
    std::vector<std::string> availableLanguages() const;

    // ── Programmatic registration ─────────────────────────────────────────
    /// Register one translation of `key` for language `lang` ("en", "zh", …).
    void add(const std::string& key, const std::string& lang, const std::string& text) {
        entries_[key + '\x1F' + lang] = text;
    }

    // ── File loading ──────────────────────────────────────────────────────

    /// Load Android-style XML string resources.
    /// Format: <resources><string name="key">value</string>…</resources>
    /// @param path   Path to the XML file.
    /// @param lang   Language code to register under ("zh", "ja", etc.).
    /// @return true on success.
    bool loadXmlFile(const std::string& path, const std::string& lang);

    /// Load XML from a string (useful for embedded resources / testing).
    bool loadXmlString(const std::string& xml, const std::string& lang);

    /// Load multi-language INI file.
    /// Format: [lang]\nkey=value\nkey2=value2\n[lang2]…
    /// @param path Path to the INI file.
    /// @return true on success.
    bool loadIniFile(const std::string& path);

    /// Load INI from a string.
    bool loadIniString(const std::string& ini);

    /// Android-style directory scan: load all strings.xml under `resDir`.
    ///   resDir/strings.xml      → language "en" (default)
    ///   resDir/strings-zh.xml   → language "zh"
    ///   resDir/values-ja/strings.xml   → language "ja"
    /// @return number of files loaded.
    int loadFromDirectory(const std::string& resDir);

    // ── Translation lookup ────────────────────────────────────────────────
    /// Translation of `key` in the active language, or `key` itself.
    std::string translate(const std::string& key) const {
        auto it = entries_.find(key + '\x1F' + lang_);
        return it != entries_.end() ? it->second : key;
    }

    /// Clear all translations (or just one language).
    void clear() { entries_.clear(); }
    void clearLanguage(const std::string& lang);

    /// Total number of entries across all languages.
    size_t size() const { return entries_.size(); }

private:
    Translator() = default;

    std::string lang_ = "en";
    std::unordered_map<std::string, std::string> entries_;
};

/// Convenience: translate `key` using the active language.
inline std::string tr(const std::string& key) {
    return Translator::instance().translate(key);
}

}  // namespace cui_core
