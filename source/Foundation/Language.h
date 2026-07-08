#pragma once

#include <string>
#include <unordered_map>

class LanguageManager {
public:
    enum class Language {
        English,
        Chinese
    };

    static LanguageManager& GetInstance() {
        static LanguageManager instance;
        return instance;
    }

    void SetLanguage(Language lang) { m_CurrentLanguage = lang; }
    Language GetLanguage() const { return m_CurrentLanguage; }
    bool IsChinese() const { return m_CurrentLanguage == Language::Chinese; }
    void Toggle() {
        m_CurrentLanguage = (m_CurrentLanguage == Language::Chinese) 
            ? Language::English 
            : Language::Chinese;
    }

    const char* Get(const char* en, const char* zh) const {
        return (m_CurrentLanguage == Language::Chinese) ? zh : en;
    }

private:
    Language m_CurrentLanguage = Language::Chinese;
};

// 便捷宏
#define TR(en, zh) LanguageManager::GetInstance().Get(en, zh)
#define IS_CN() LanguageManager::GetInstance().IsChinese()
