#include "IconCache.hpp"
#include "globals.hpp"

#include <cairo/cairo.h>
#include <hyprland/src/render/OpenGL.hpp>

SP<CTexture> CIconCache::getTexture(const std::string& iconPath) {
    auto it = m_cache.find(iconPath);
    if (it != m_cache.end())
        return it->second;

    SP<CTexture> tex;

    if (iconPath.ends_with(".png"))
        tex = loadPNG(iconPath);

    // TODO: SVG support via Hyprgraphics::CImage

    if (tex)
        m_cache[iconPath] = tex;

    return tex;
}

SP<CTexture> CIconCache::loadPNG(const std::string& path) {
    logToFile(std::format("loadPNG: loading {}", path));

    auto surface = cairo_image_surface_create_from_png(path.c_str());
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        logToFile(std::format("loadPNG: cairo error for {}", path));
        cairo_surface_destroy(surface);
        return nullptr;
    }

    int            w    = cairo_image_surface_get_width(surface);
    int            h    = cairo_image_surface_get_height(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    cairo_surface_flush(surface);

    auto tex = makeShared<CTexture>();
    tex->allocate();

    glBindTexture(GL_TEXTURE_2D, tex->m_texID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    // Cairo uses BGRA, swap R and B channels via swizzle
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    glBindTexture(GL_TEXTURE_2D, 0);

    tex->m_size = {w, h};

    logToFile(std::format("loadPNG: loaded {}x{} texture from {}", w, h, path));

    cairo_surface_destroy(surface);
    return tex;
}
