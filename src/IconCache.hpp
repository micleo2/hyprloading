#pragma once

#include <string>
#include <unordered_map>
#include <hyprland/src/render/Texture.hpp>

class CIconCache {
  public:
    SP<CTexture> getTexture(const std::string& iconPath);

  private:
    SP<CTexture> loadPNG(const std::string& path);

    std::unordered_map<std::string, SP<CTexture>> m_cache;
};
