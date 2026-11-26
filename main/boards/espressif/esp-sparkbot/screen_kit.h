// Piezas comunes para dibujar las pantallas propias del SparkBot (estilo HUD, fuente de consola).
#ifndef SCREEN_KIT_H
#define SCREEN_KIT_H

#include <lvgl.h>

#include <cstdint>
#include <string>

// Fuente JetBrains Mono (licencia OFL) convertida para LVGL
LV_FONT_DECLARE(font_mono_14);  // normal, texto pequeño
LV_FONT_DECLARE(font_mono_18);  // normal, subtítulos
LV_FONT_DECLARE(font_mono_20);  // negrita, texto mediano
LV_FONT_DECLARE(font_mono_36);  // negrita, solo "0123456789:- " (reloj)
LV_FONT_DECLARE(font_mono_56);  // negrita, solo "0123456789:-%° "
LV_FONT_DECLARE(font_mono_80);  // negrita, solo cifras

namespace kit {

constexpr uint32_t kHudCyan = 0x5FE3FF;
constexpr uint32_t kHudDim = 0x4A7A88;
constexpr uint32_t kHudLine = 0x1E4A56;
constexpr uint32_t kWhite = 0xFFFFFF;

inline lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h, int radius, uint32_t color) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

// Contenedor transparente (para agrupar y animar piezas)
inline lv_obj_t* Group(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

// Etiqueta en una posición; align = LV_TEXT_ALIGN_LEFT / CENTER / RIGHT dentro de su ancho
inline lv_obj_t* Text(lv_obj_t* parent, int x, int y, int width, const lv_font_t* font, uint32_t color,
                      lv_text_align_t align = LV_TEXT_ALIGN_CENTER,
                      lv_label_long_mode_t mode = LV_LABEL_LONG_MODE_DOTS) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_width(label, width);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_label_set_long_mode(label, mode);
    if (mode == LV_LABEL_LONG_MODE_DOTS || mode == LV_LABEL_LONG_MODE_CLIP) {
        lv_obj_set_height(label, lv_font_get_line_height(font));  // una sola línea
    }
    lv_label_set_text(label, "");
    return label;
}

// Etiqueta centrada en la pantalla de 240 px
inline lv_obj_t* Centered(lv_obj_t* parent, int y, const lv_font_t* font, uint32_t color, int width = 224,
                          lv_label_long_mode_t mode = LV_LABEL_LONG_MODE_DOTS) {
    return Text(parent, (240 - width) / 2, y, width, font, color, LV_TEXT_ALIGN_CENTER, mode);
}

inline void SetColor(lv_obj_t* label, uint32_t color) {
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

// Línea horizontal fina
inline lv_obj_t* HLine(lv_obj_t* parent, int x, int y, int w, uint32_t color = kHudLine) {
    return Box(parent, x, y, w, 1, 0, color);
}

// Esquinas tipo visor en los bordes de la pantalla
inline void Corners(lv_obj_t* parent, uint32_t color = kHudCyan, int inset = 4, int len = 14, int t = 2) {
    const int far = 240 - inset;
    Box(parent, inset, inset, len, t, 0, color);
    Box(parent, inset, inset, t, len, 0, color);
    Box(parent, far - len, inset, len, t, 0, color);
    Box(parent, far - t, inset, t, len, 0, color);
    Box(parent, inset, far - t, len, t, 0, color);
    Box(parent, inset, far - len, t, len, 0, color);
    Box(parent, far - len, far - t, len, t, 0, color);
    Box(parent, far - t, far - len, t, len, 0, color);
}

// Capa negra a pantalla completa encima de todo, oculta al principio
inline lv_obj_t* Overlay() {
    lv_obj_t* root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    return root;
}

// Deja solo los caracteres que tiene la fuente (ASCII y Latin-1: tildes, ñ, ü, ¿, ¡, °)
// y corta a max_chars caracteres.
inline std::string ScreenText(const std::string& utf8, size_t max_chars) {
    std::string out;
    size_t count = 0;
    for (size_t i = 0; i < utf8.size() && count < max_chars; ++i) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
            ++count;
            continue;
        }
        if ((c == 0xC2 || c == 0xC3) && i + 1 < utf8.size()) {
            unsigned char n = static_cast<unsigned char>(utf8[i + 1]);
            if ((n & 0xC0) == 0x80 && !(c == 0xC2 && n < 0xA0)) {
                out.push_back(static_cast<char>(c));
                out.push_back(static_cast<char>(n));
                ++count;
            }
            ++i;
            continue;
        }
        // Otros caracteres (emojis, otros alfabetos): se saltan
        while (i + 1 < utf8.size() && (static_cast<unsigned char>(utf8[i + 1]) & 0xC0) == 0x80) {
            ++i;
        }
    }
    return out;
}

// Animaciones sencillas en bucle
using AnimSetter = void (*)(void*, int32_t);

inline void Loop(lv_obj_t* obj, AnimSetter setter, int32_t from, int32_t to, uint32_t time_ms,
                 uint32_t delay_ms = 0, bool back_and_forth = false, uint32_t repeat_delay = 0) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, setter);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, time_ms);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_repeat_delay(&a, repeat_delay);
    if (back_and_forth) {
        lv_anim_set_reverse_duration(&a, time_ms);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    }
    lv_anim_start(&a);
}

inline void SetY(void* obj, int32_t v) { lv_obj_set_y(static_cast<lv_obj_t*>(obj), v); }
inline void SetTranslateX(void* obj, int32_t v) {
    lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), v, 0);
}
inline void SetRotation(void* obj, int32_t v) {
    lv_obj_set_style_transform_rotation(static_cast<lv_obj_t*>(obj), v, 0);
}
inline void SetOpacity(void* obj, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v), 0);
}

}  // namespace kit

#endif  // SCREEN_KIT_H
