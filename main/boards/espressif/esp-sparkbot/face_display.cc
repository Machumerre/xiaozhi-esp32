// Cara propia para el ESP-SparkBot: ojos animados.
#include "face_display.h"

#include "application.h"

#include <esp_random.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr uint32_t kCyan = 0x5FE3FF;
constexpr uint32_t kListen = 0x4DFFA0;
constexpr uint32_t kBusy = 0xFFB300;
constexpr int kEyeGap = 44;     // distancia del centro a cada ojo
constexpr int kEyeY = -22;      // ojos un poco por encima del centro
constexpr int kMouthY = 44;     // boca debajo de los ojos

uint32_t RandomRange(uint32_t min, uint32_t max) { return min + esp_random() % (max - min + 1); }

lv_obj_t* CreatePlain(lv_obj_t* parent, uint32_t color) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

bool Is(const std::string& value, std::initializer_list<const char*> names) {
    for (const char* name : names) {
        if (value == name) {
            return true;
        }
    }
    return false;
}

}  // namespace

SparkBotDisplay::SparkBotDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                 int width, int height, int offset_x, int offset_y, bool mirror_x,
                                 bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {}

void SparkBotDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();
    DisplayLockGuard lock(this);
    BuildFace();
}

void SparkBotDisplay::BuildFace() {
    if (face_ != nullptr) {
        return;
    }
    lv_obj_t* screen = lv_screen_active();
    face_ = lv_obj_create(screen);
    lv_obj_remove_style_all(face_);
    lv_obj_set_size(face_, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(face_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(face_, LV_OBJ_FLAG_CLICKABLE);
    // Justo encima del fondo y por debajo de las barras de estado y de los subtítulos
    lv_obj_move_to_index(face_, 1);

    for (int i = 0; i < 2; ++i) {
        eye_[i] = CreatePlain(face_, kCyan);
        lv_obj_align(eye_[i], LV_ALIGN_CENTER, i == 0 ? -kEyeGap : kEyeGap, kEyeY);
        lid_top_[i] = CreatePlain(eye_[i], 0x000000);
        lid_bottom_[i] = CreatePlain(eye_[i], 0x000000);
        lv_obj_add_flag(lid_top_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lid_bottom_[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 7; ++i) {
        mouth_[i] = CreatePlain(face_, kCyan);
        lv_obj_set_size(mouth_[i], 6, 8);
        lv_obj_set_style_radius(mouth_[i], 3, 0);
        lv_obj_align(mouth_[i], LV_ALIGN_CENTER, (i - 3) * 11, kMouthY);
        lv_obj_add_flag(mouth_[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    ApplyShape();
    timer_ = lv_timer_create(&SparkBotDisplay::TimerCallback, 40, this);
}

bool SparkBotDisplay::SetEmotion(const char* emotion) {
    SpiLcdDisplay::SetEmotion(emotion);
    const std::string e = emotion != nullptr ? emotion : "";

    Shape shape = Shape::kNormal;
    uint32_t color = kCyan;
    if (Is(e, {"happy", "laughing", "funny", "silly", "delicious", "confident", "cool", "relaxed"})) {
        shape = Shape::kHappy;
    } else if (Is(e, {"loving", "kissy"})) {
        shape = Shape::kHappy;
        color = 0xFF7FC8;
    } else if (Is(e, {"embarrassed"})) {
        shape = Shape::kHappy;
        color = 0xFF9AB8;
    } else if (Is(e, {"sad", "crying"})) {
        shape = Shape::kSad;
        color = 0x5C9DFF;
    } else if (Is(e, {"angry"})) {
        shape = Shape::kAngry;
        color = 0xFF4A4A;
    } else if (Is(e, {"surprised", "shocked"})) {
        shape = Shape::kSurprised;
        color = 0xFFE082;
    } else if (Is(e, {"sleepy"})) {
        shape = Shape::kSleepy;
        color = 0x3A8FA8;
    } else if (Is(e, {"thinking", "confused"})) {
        shape = Shape::kThinking;
        color = 0xB388FF;
    } else if (Is(e, {"winking"})) {
        shape = Shape::kWink;
    }

    DisplayLockGuard lock(this);
    shape_ = shape;
    emotion_color_ = color;
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    if (face_ != nullptr) {
        ApplyShape();
    }
    return true;
}

// Coloca ojos y párpados según la expresión actual (con la pantalla bloqueada)
void SparkBotDisplay::ApplyShape() {
    for (int i = 0; i < 2; ++i) {
        int w = 50, h = 66, radius = 18;
        switch (shape_) {
            case Shape::kSurprised: w = 60; h = 60; radius = LV_RADIUS_CIRCLE; break;
            case Shape::kSleepy: w = 52; h = 10; radius = 5; break;
            case Shape::kSad: h = 56; break;
            case Shape::kThinking: if (i == 0) { h = 54; } break;
            case Shape::kWink: if (i == 1) { w = 52; h = 10; radius = 5; } break;
            default: break;
        }
        if (i == 0) {
            eye_w_ = w;
            eye_h_ = h;
        }
        lv_obj_set_size(eye_[i], w, h);
        lv_obj_set_style_radius(eye_[i], radius, 0);
        lv_obj_set_style_bg_color(eye_[i], lv_color_hex(emotion_color_), 0);

        lv_obj_t* top = lid_top_[i];
        lv_obj_t* bottom = lid_bottom_[i];
        lv_obj_add_flag(top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bottom, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_transform_rotation(top, 0, 0);

        if (shape_ == Shape::kHappy) {
            // Párpado inferior redondo: ojos sonrientes en forma de media luna
            lv_obj_set_size(bottom, w + 24, h);
            lv_obj_set_style_radius(bottom, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_pos(bottom, -12, h * 55 / 100);
            lv_obj_remove_flag(bottom, LV_OBJ_FLAG_HIDDEN);
        } else if (shape_ == Shape::kSad || shape_ == Shape::kAngry) {
            // Párpado superior inclinado: tristeza o enfado según el sentido
            const int lid_w = w + 30;
            const int lid_h = h * 45 / 100;
            lv_obj_set_size(top, lid_w, lid_h);
            lv_obj_set_style_radius(top, 0, 0);
            lv_obj_set_pos(top, -15, -lid_h / 3);
            lv_obj_set_style_transform_pivot_x(top, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(top, lid_h / 2, 0);
            int angle = shape_ == Shape::kAngry ? 160 : -160;  // décimas de grado
            lv_obj_set_style_transform_rotation(top, i == 0 ? angle : -angle, 0);
            lv_obj_remove_flag(top, LV_OBJ_FLAG_HIDDEN);
        }
    }
    last_color_ = emotion_color_;
}

void SparkBotDisplay::TimerCallback(lv_timer_t* timer) {
    static_cast<SparkBotDisplay*>(lv_timer_get_user_data(timer))->OnTimer();
}

// Se ejecuta 25 veces por segundo dentro de LVGL (ya con la pantalla bloqueada)
void SparkBotDisplay::OnTimer() {
    if (face_ == nullptr) {
        return;
    }
    ticks_++;
    if (emoji_box_ != nullptr && !lv_obj_has_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }

    const DeviceState state = Application::GetInstance().GetDeviceState();
    const bool listening = state == kDeviceStateListening;
    const bool speaking = state == kDeviceStateSpeaking;
    const bool busy = state == kDeviceStateConnecting || state == kDeviceStateStarting ||
                      state == kDeviceStateActivating || state == kDeviceStateUpgrading;

    // Color: verde cuando te escucha, ámbar cuando está ocupado, y si no el de la emoción
    uint32_t color = listening ? kListen : (busy ? kBusy : emotion_color_);
    if (color != last_color_) {
        for (int i = 0; i < 2; ++i) {
            lv_obj_set_style_bg_color(eye_[i], lv_color_hex(color), 0);
        }
        for (int i = 0; i < 7; ++i) {
            lv_obj_set_style_bg_color(mouth_[i], lv_color_hex(color), 0);
        }
        last_color_ = color;
    }

    // Mirada: cuando te escucha te mira de frente; si piensa, mira arriba a la derecha
    if (ticks_ >= next_gaze_) {
        if (listening) {
            target_x_ = 0;
            target_y_ = 0;
        } else if (shape_ == Shape::kThinking) {
            target_x_ = 14;
            target_y_ = -10;
        } else {
            target_x_ = static_cast<float>(RandomRange(0, 28)) - 14.0f;
            target_y_ = static_cast<float>(RandomRange(0, 12)) - 6.0f;
        }
        next_gaze_ = ticks_ + RandomRange(40, 110);
    }
    const int old_x = static_cast<int>(std::lround(gaze_x_));
    const int old_y = static_cast<int>(std::lround(gaze_y_));
    gaze_x_ += (target_x_ - gaze_x_) * 0.25f;
    gaze_y_ += (target_y_ - gaze_y_) * 0.25f;
    const int new_x = static_cast<int>(std::lround(gaze_x_));
    const int new_y = static_cast<int>(std::lround(gaze_y_));
    if (new_x != old_x || new_y != old_y) {
        for (int i = 0; i < 2; ++i) {
            lv_obj_set_style_translate_x(eye_[i], new_x, 0);
            lv_obj_set_style_translate_y(eye_[i], new_y, 0);
        }
    }

    // Parpadeo cada 3-5 segundos (no si tiene sueño)
    if (shape_ != Shape::kSleepy) {
        if (blink_step_ < 0 && ticks_ >= next_blink_) {
            blink_step_ = 0;
        }
        if (blink_step_ >= 0) {
            static const int kBlink[6] = {60, 20, 10, 20, 60, 100};  // % de la altura
            const int h = std::max(6, eye_h_ * kBlink[blink_step_] / 100);
            for (int i = 0; i < 2; ++i) {
                lv_obj_add_flag(lid_top_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(lid_bottom_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_height(eye_[i], h);
            }
            blink_step_++;
            if (blink_step_ >= 6) {
                blink_step_ = -1;
                ApplyShape();
                last_color_ = 0xFFFFFFFF;  // fuerza a repintar el color en el siguiente ciclo
                // A veces parpadea dos veces seguidas
                next_blink_ = ticks_ + (RandomRange(0, 4) == 0 ? 6 : RandomRange(75, 125));
            }
        }
    }

    // Boca: barras que se mueven mientras habla
    if (speaking) {
        if (ticks_ % 2 == 0) {
            for (int i = 0; i < 7; ++i) {
                lv_obj_remove_flag(mouth_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_height(mouth_[i], static_cast<int32_t>(RandomRange(4, 22)));
            }
        }
    } else if (!lv_obj_has_flag(mouth_[0], LV_OBJ_FLAG_HIDDEN)) {
        for (int i = 0; i < 7; ++i) {
            lv_obj_add_flag(mouth_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}
