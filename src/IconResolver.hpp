#pragma once

#include <string>
#include <optional>
#include <unordered_map>

class CIconResolver {
  public:
    std::optional<std::string> resolveIconPath(const std::string& appId, int size = 48);

  private:
    std::optional<std::string> resolveDesktopFile(const std::string& appId);
    std::optional<std::string> resolveIconFromTheme(const std::string& iconName, int size);
    std::optional<std::string> parseIconFromDesktop(const std::string& desktopPath);

    std::unordered_map<std::string, std::string> m_cache; // appId -> icon file path
};
