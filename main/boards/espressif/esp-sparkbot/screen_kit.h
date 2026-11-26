// Piezas comunes para dibujar las pantallas propias del SparkBot.
#ifndef SCREEN_KIT_H
#define SCREEN_KIT_H

#include <lvgl.h>

#include <cctype>
#include <cstdint>
#include <string>

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace kit {

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

inline lv_obj_t* Label(lv_obj_t* parent, int y, int width, uint32_t color,
                       lv_label_long_mode_t mode = LV_LABEL_LONG_MODE_DOTS) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, mode);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(label, "");
    return label;
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

// Dígito de 7 segmentos
struct Digit {
    lv_obj_t* seg[7] = {};
};

inline void MakeDigit(lv_obj_t* parent, Digit& d, int x, int y, int w, int h, int t, uint32_t off) {
    const int vh = (h - 3 * t) / 2;
    const int r = t / 2;
    d.seg[0] = Box(parent, x + t, y, w - 2 * t, t, r, off);
    d.seg[1] = Box(parent, x + w - t, y + t, t, vh, r, off);
    d.seg[2] = Box(parent, x + w - t, y + 2 * t + vh, t, vh, r, off);
    d.seg[3] = Box(parent, x + t, y + h - t, w - 2 * t, t, r, off);
    d.seg[4] = Box(parent, x, y + 2 * t + vh, t, vh, r, off);
    d.seg[5] = Box(parent, x, y + t, t, vh, r, off);
    d.seg[6] = Box(parent, x + t, y + t + vh, w - 2 * t, t, r, off);
}

inline void SetDigit(Digit& d, int value, uint32_t on, uint32_t off) {
    static const uint8_t kMask[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    const uint8_t mask = (value >= 0 && value <= 9) ? kMask[value] : 0;
    for (int i = 0; i < 7; ++i) {
        if (d.seg[i] != nullptr) {
            lv_obj_set_style_bg_color(d.seg[i], lv_color_hex((mask & (1 << i)) ? on : off), 0);
        }
    }
}

// La fuente integrada solo tiene caracteres básicos: mayúsculas sin tildes
inline std::string ScreenText(const std::string& utf8, size_t max_chars) {
    std::string out;
    for (size_t i = 0; i < utf8.size() && out.size() < max_chars; ++i) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(std::toupper(c)));
            continue;
        }
        if (c == 0xC3 && i + 1 < utf8.size()) {
            unsigned char n = static_cast<unsigned char>(utf8[++i]);
            char mapped = 0;
            if ((n >= 0x80 && n <= 0x85) || (n >= 0xA0 && n <= 0xA5)) mapped = 'A';
            else if ((n >= 0x88 && n <= 0x8B) || (n >= 0xA8 && n <= 0xAB)) mapped = 'E';
            else if ((n >= 0x8C && n <= 0x8F) || (n >= 0xAC && n <= 0xAF)) mapped = 'I';
            else if ((n >= 0x92 && n <= 0x96) || (n >= 0xB2 && n <= 0xB6)) mapped = 'O';
            else if ((n >= 0x99 && n <= 0x9C) || (n >= 0xB9 && n <= 0xBC)) mapped = 'U';
            else if (n == 0x91 || n == 0xB1) mapped = 'N';
            else if (n == 0x87 || n == 0xA7) mapped = 'C';
            if (mapped) out.push_back(mapped);
            continue;
        }
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
