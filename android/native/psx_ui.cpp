/*
 * psx_ui.cpp — in-game overlay core: Dear ImGui over the runtime's SDL
 * renderer, the on-screen touch controls, and how the menu is opened.
 *
 * Input: SDL delivers touch events on Android's UI thread, so an event filter
 * only queues the ones the overlay cares about; they are consumed on the
 * emulation thread, which is the only thread that touches ImGui or the touch
 * state (psx_ui_render and psx_ui_pad_buttons both run there).
 *
 * The menu opens from the gear on the touch controls, Android's Back gesture,
 * or Select+Start (View+Menu on an Xbox pad) held together.
 */
#include "psx_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "psx_ui_host.h"
#include "psx_ui_internal.h"

UiState g_ui;

namespace {

/* ---- settings file -------------------------------------------------------- */

const char *const k_el_keys[EL_COUNT] = {
    "dpad", "triangle", "circle", "cross", "square", "l1", "l2", "r1", "r2",
    "select", "start", "menu", "ff" };
const char *const k_el_names[EL_COUNT] = {
    "D-pad", "Triangle", "Circle", "Cross", "Square", "L1", "L2", "R1", "R2",
    "Select", "Start", "Menu button", "Fast-forward button" };
constexpr float k_el_min_scale = 0.5f, k_el_max_scale = 2.5f;

std::string settings_path() { return g_ui.data_dir + "/ui_settings.ini"; }

void load_settings() {
    FILE *f = std::fopen(settings_path().c_str(), "r");
    if (!f) return;
    char line[256];
    UiSettings &s = g_ui.s;
    while (std::fgets(line, sizeof(line), f)) {
        char key[64];
        char val[128];
        if (std::sscanf(line, " %63[^=]=%127s", key, val) != 2) continue;
        const std::string k = key;
        if (k == "touch_mode") s.touch_mode = std::clamp(std::atoi(val), 0, 2);
        else if (k == "screen_fit") s.screen_fit = std::clamp(std::atoi(val), 0, 2);
        else if (k == "touch_opacity") s.touch_opacity = std::clamp((float)std::atof(val), 0.1f, 1.0f);
        else if (k == "touch_scale") s.touch_scale = std::clamp((float)std::atof(val), 0.6f, 1.6f);
        else if (k == "speed") s.speed = std::clamp((float)std::atof(val), 0.25f, 8.0f);
        else if (k == "ff_speed") s.ff_speed = std::clamp((float)std::atof(val), 1.5f, 8.0f);
        else if (k == "show_fps") s.show_fps = std::atoi(val) != 0;
        else if (k == "volume") s.volume = std::clamp(std::atoi(val), 0, 100);
        else if (k.rfind("ctl_", 0) == 0) {
            /* ctl_face (the four face buttons moved as one, before they could
             * be placed singly) seeds all four. */
            const bool face = k == "ctl_face";
            for (int e = 0; e < EL_COUNT; e++) {
                const bool is_face = e >= EL_TRIANGLE && e <= EL_SQUARE;
                if (!(face && is_face) && k.compare(4, std::string::npos, k_el_keys[e]) != 0) continue;
                float dx = 0, dy = 0, sc = 1;
                if (std::sscanf(val, "%f,%f,%f", &dx, &dy, &sc) == 3) {
                    s.ctl[e].dx = std::clamp(dx, -1.0f, 1.0f);
                    s.ctl[e].dy = std::clamp(dy, -1.0f, 1.0f);
                    s.ctl[e].scale = std::clamp(sc, k_el_min_scale, k_el_max_scale);
                }
            }
        }
    }
    std::fclose(f);
}

/* ---- queued input ------------------------------------------------------------ */

std::mutex s_queue_mutex;
std::vector<SDL_Event> s_queue;

bool SDLCALL event_filter(void *, SDL_Event *ev) {
    switch (ev->type) {
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        std::lock_guard<std::mutex> lock(s_queue_mutex);
        if (s_queue.size() < 1024) s_queue.push_back(*ev);
        /* Android's Back (the edge-swipe gesture) belongs to the overlay. */
        if ((ev->type == SDL_EVENT_KEY_DOWN || ev->type == SDL_EVENT_KEY_UP) &&
            ev->key.key == SDLK_AC_BACK)
            return false;
        return true;
    }
    default:
        return true;
    }
}

/* ---- touch controls ------------------------------------------------------------ */

enum CtlKind { CTL_NONE, CTL_DPAD, CTL_BUTTON, CTL_MENU, CTL_FF };
enum Shape { SH_TRIANGLE, SH_CIRCLE, SH_CROSS, SH_SQUARE, SH_PILL, SH_SHOULDER, SH_ICON };

struct TouchButton {
    int el;         /* TouchEl it belongs to */
    uint16_t bit;
    CtlKind kind;
    Shape shape;
    ImVec2 c;       /* centre */
    ImVec2 half;    /* half extents (circles use half.x as radius) */
    const char *label;
};

struct Finger {
    SDL_FingerID id = 0;
    bool active = false;
    CtlKind kind = CTL_NONE;
    uint16_t bits = 0;       /* what this finger holds right now */
    uint64_t down_ms = 0;
    ImVec2 pos;
};

Finger s_fingers[10];

/* The on-screen menu button: a tap opens the menu, holding it this long opens
 * the touch-control editor instead. */
constexpr uint64_t k_menu_hold_ms = 550;

/* The game samples the pad once per frame, so a tap shorter than a frame
 * could be missed entirely. Every press is held for at least this long. */
constexpr uint64_t k_min_press_ms = 80;
struct Latched { uint16_t bits; uint64_t until_ms; };
std::vector<Latched> s_latched;
std::vector<TouchButton> s_buttons;
ImVec2 s_dpad_c;
float s_dpad_r = 0.0f;
uint16_t s_touch_bits = 0;          /* held buttons (active-high) */
uint64_t s_last_touch_ms = 0, s_last_pad_ms = 0;
bool s_chord_down = false;          /* Select+Start held on a pad */
bool s_r3_down = false;

/* Every control sits at a default spot (worked out from the screen size),
 * moved and resized by the player's layout. 1.0 in the size settings is this
 * default, which is 15% larger than the first release's. */
constexpr float k_base_size = 1.15f;
ImVec2 s_el_home[EL_COUNT];               /* default centre */
ImVec2 s_el_c[EL_COUNT];                  /* centre after the player's layout */
ImVec2 s_el_half[EL_COUNT];               /* half extents, for the editor */

float layout_unit() {
    return (float)g_ui.height / 100.0f * k_base_size * g_ui.s.touch_scale;
}

ImVec2 place(int el, ImVec2 home, ImVec2 half) {
    const float W = (float)g_ui.width, H = (float)g_ui.height;
    const CtlPlace &p = g_ui.s.ctl[el];
    s_el_home[el] = home;
    ImVec2 c(home.x + p.dx * W, home.y + p.dy * H);
    c.x = std::clamp(c.x, 0.0f, W);
    c.y = std::clamp(c.y, 0.0f, H);
    s_el_c[el] = c;
    s_el_half[el] = ImVec2(half.x * p.scale, half.y * p.scale);
    return c;
}

void layout_touch() {
    const float W = (float)g_ui.width, H = (float)g_ui.height;
    const float u = layout_unit();
    auto sc = [](int el) { return g_ui.s.ctl[el].scale; };
    s_buttons.clear();

    const float dr = 15.0f * u;
    s_dpad_c = place(EL_DPAD, ImVec2(6.0f * u + dr, H - 8.0f * u - dr), ImVec2(dr * 1.1f, dr * 1.1f));
    s_dpad_r = dr * sc(EL_DPAD);

    {
        /* The face buttons default to a diamond, but each is its own control. */
        const float d = 10.5f * u, r = 6.2f * u;
        const ImVec2 f(W - 6.0f * u - 17.0f * u, H - 8.0f * u - 17.0f * u);
        struct Face { int el; uint16_t bit; Shape shape; ImVec2 home; } faces[] = {
            {EL_TRIANGLE, PADB_TRIANGLE, SH_TRIANGLE, ImVec2(f.x, f.y - d)},
            {EL_CIRCLE, PADB_CIRCLE, SH_CIRCLE, ImVec2(f.x + d, f.y)},
            {EL_CROSS, PADB_CROSS, SH_CROSS, ImVec2(f.x, f.y + d)},
            {EL_SQUARE, PADB_SQUARE, SH_SQUARE, ImVec2(f.x - d, f.y)},
        };
        for (const Face &b : faces) {
            const ImVec2 c = place(b.el, b.home, ImVec2(r, r));
            s_buttons.push_back({b.el, b.bit, CTL_BUTTON, b.shape, c, s_el_half[b.el], nullptr});
        }
    }

    struct Simple { int el; uint16_t bit; CtlKind kind; Shape shape; ImVec2 home, half; const char *label; };
    const ImVec2 sh(10.0f * u, 4.2f * u), pill(6.5f * u, 2.8f * u);
    const float ir = 4.2f * u;
    const Simple simple[] = {
        {EL_L2, PADB_L2, CTL_BUTTON, SH_SHOULDER, ImVec2(6.0f * u + sh.x, 5.0f * u + sh.y), sh, "L2"},
        {EL_L1, PADB_L1, CTL_BUTTON, SH_SHOULDER, ImVec2(6.0f * u + sh.x, 15.5f * u + sh.y), sh, "L1"},
        {EL_R2, PADB_R2, CTL_BUTTON, SH_SHOULDER, ImVec2(W - 6.0f * u - sh.x, 5.0f * u + sh.y), sh, "R2"},
        {EL_R1, PADB_R1, CTL_BUTTON, SH_SHOULDER, ImVec2(W - 6.0f * u - sh.x, 15.5f * u + sh.y), sh, "R1"},
        {EL_SELECT, PADB_SELECT, CTL_BUTTON, SH_PILL, ImVec2(W * 0.5f - 9.0f * u, H - 5.0f * u), pill, "SELECT"},
        {EL_START, PADB_START, CTL_BUTTON, SH_PILL, ImVec2(W * 0.5f + 9.0f * u, H - 5.0f * u), pill, "START"},
        {EL_MENU, 0, CTL_MENU, SH_ICON, ImVec2(W * 0.5f - 6.0f * u, 5.5f * u), ImVec2(ir, ir), "menu"},
        {EL_FF, 0, CTL_FF, SH_ICON, ImVec2(W * 0.5f + 6.0f * u, 5.5f * u), ImVec2(ir, ir), "ff"},
    };
    for (const Simple &b : simple) {
        const ImVec2 c = place(b.el, b.home, b.half);
        s_buttons.push_back({b.el, b.bit, b.kind, b.shape, c, s_el_half[b.el], b.label});
    }
}

bool touch_visible() {
    if (g_ui.menu_open || g_ui.layout_edit) return false;
    switch (g_ui.s.touch_mode) {
    case TOUCH_ALWAYS: return true;
    case TOUCH_OFF: return false;
    default: {
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        SDL_free(ids);
        return n == 0 || s_last_touch_ms >= s_last_pad_ms;
    }
    }
}

uint16_t dpad_bits(ImVec2 p) {
    const float dx = p.x - s_dpad_c.x, dy = p.y - s_dpad_c.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < s_dpad_r * 0.22f) return 0;
    /* 8 sectors: diagonals press two directions. */
    const float a = std::atan2(dy, dx);   /* 0 = right, +pi/2 = down */
    const int sector = ((int)std::lround(a / (float)(M_PI / 4.0)) + 8) % 8;
    static const uint16_t k[8] = {
        PADB_RIGHT, PADB_RIGHT | PADB_DOWN, PADB_DOWN, PADB_DOWN | PADB_LEFT,
        PADB_LEFT, PADB_LEFT | PADB_UP, PADB_UP, PADB_UP | PADB_RIGHT,
    };
    return k[sector];
}

const TouchButton *hit_button(ImVec2 p, float slack) {
    const TouchButton *best = nullptr;
    float best_d = 1e30f;
    for (const TouchButton &b : s_buttons) {
        const float dx = std::fabs(p.x - b.c.x), dy = std::fabs(p.y - b.c.y);
        bool inside;
        if (b.shape == SH_SHOULDER || b.shape == SH_PILL)
            inside = dx <= b.half.x * slack && dy <= b.half.y * slack;
        else
            inside = dx * dx + dy * dy <= (b.half.x * slack) * (b.half.x * slack);
        const float d = dx * dx + dy * dy;
        if (inside && d < best_d) { best = &b; best_d = d; }
    }
    return best;
}

void finger_down(SDL_FingerID id, ImVec2 p) {
    const bool was_visible = touch_visible();
    s_last_touch_ms = SDL_GetTicks();
    if (!was_visible) return;              /* first tap only reveals the controls */
    Finger *f = nullptr;
    for (Finger &x : s_fingers) if (!x.active) { f = &x; break; }
    if (!f) return;
    *f = Finger();
    f->id = id;
    f->active = true;
    f->pos = p;
    f->down_ms = SDL_GetTicks();
    const float ddx = p.x - s_dpad_c.x, ddy = p.y - s_dpad_c.y;
    if (ddx * ddx + ddy * ddy <= (s_dpad_r * 1.45f) * (s_dpad_r * 1.45f)) {
        f->kind = CTL_DPAD;
        f->bits = dpad_bits(p);
        return;
    }
    if (const TouchButton *b = hit_button(p, 1.35f)) {
        f->kind = b->kind;
        f->bits = b->bit;
        /* The menu button acts on release (a tap) or after a hold. */
        if (b->kind == CTL_FF) g_ui.ff_active = !g_ui.ff_active;
    }
}

void finger_motion(SDL_FingerID id, ImVec2 p) {
    for (Finger &f : s_fingers) {
        if (!f.active || f.id != id) continue;
        f.pos = p;
        if (f.kind == CTL_DPAD) {
            f.bits = dpad_bits(p);
        } else if (f.kind == CTL_BUTTON) {
            /* Sliding onto another button moves the press there. */
            const TouchButton *b = hit_button(p, 1.2f);
            if (b && b->kind == CTL_BUTTON) f.bits = b->bit;
        }
    }
}

void finger_up(SDL_FingerID id) {
    for (Finger &f : s_fingers) {
        if (!f.active || f.id != id) continue;
        if ((f.kind == CTL_DPAD || f.kind == CTL_BUTTON) && f.bits)
            s_latched.push_back({ f.bits, f.down_ms + k_min_press_ms });
        const bool menu_tap = f.kind == CTL_MENU;
        f = Finger();
        if (menu_tap) ui_set_menu_open(true);   /* released before the hold */
    }
}

/* How far along a hold on the menu button is (0..1), or -1 if none. */
float menu_hold_progress() {
    for (const Finger &f : s_fingers)
        if (f.active && f.kind == CTL_MENU)
            return std::min(1.0f, (float)(SDL_GetTicks() - f.down_ms) / (float)k_menu_hold_ms);
    return -1.0f;
}

void release_all_fingers() {
    for (Finger &f : s_fingers) f = Finger();
    s_latched.clear();
    s_touch_bits = 0;
}

void recompute_touch_bits() {
    if (menu_hold_progress() >= 1.0f) {
        ui_begin_layout_edit(false);   /* held long enough: edit the controls */
        return;
    }
    uint16_t bits = 0;
    for (const Finger &f : s_fingers)
        if (f.active && (f.kind == CTL_DPAD || f.kind == CTL_BUTTON)) bits |= f.bits;
    const uint64_t now = SDL_GetTicks();
    s_latched.erase(std::remove_if(s_latched.begin(), s_latched.end(),
                                   [now](const Latched &l) { return l.until_ms <= now; }),
                    s_latched.end());
    for (const Latched &l : s_latched) bits |= l.bits;
    s_touch_bits = touch_visible() ? bits : 0;
}

/* Controller-side shortcuts, polled: Select+Start opens the menu, R3 toggles
 * fast-forward, and any activity marks the pad as the current input. */
void poll_gamepads() {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    bool chord = false, r3 = false, any = false;
    for (int i = 0; ids && i < n; i++) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
        if (!g) continue;
        const bool back = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK);
        const bool start = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START);
        const bool guide = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_GUIDE);
        chord |= (back && start) || guide;
        r3 |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && !any; b++)
            any |= SDL_GetGamepadButton(g, (SDL_GamepadButton)b);
        const int dz = 12000;
        any |= std::abs(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX)) > dz ||
               std::abs(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY)) > dz;
    }
    SDL_free(ids);
    if (any) s_last_pad_ms = SDL_GetTicks();
    if (chord && !s_chord_down && !g_ui.layout_edit) ui_set_menu_open(!g_ui.menu_open);
    s_chord_down = chord;
    if (r3 && !s_r3_down && !g_ui.menu_open && !g_ui.layout_edit) {
        g_ui.ff_active = !g_ui.ff_active;
        host_osd_push(g_ui.ff_active ? "Fast forward on" : "Fast forward off", 900);
    }
    s_r3_down = r3;
}

void end_layout_edit(bool keep);
void edit_finger_down(SDL_FingerID id, ImVec2 p);
void edit_finger_motion(SDL_FingerID id, ImVec2 p);
void edit_finger_up(SDL_FingerID id);

/* Consume queued events: to ImGui while the menu or the layout editor is
 * open, to the touch controls otherwise. */
void pump_events() {
    std::vector<SDL_Event> events;
    {
        std::lock_guard<std::mutex> lock(s_queue_mutex);
        events.swap(s_queue);
    }
    for (const SDL_Event &ev : events) {
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_AC_BACK) {
            if (ev.key.repeat) continue;
            if (g_ui.layout_edit) end_layout_edit(true);
            else ui_set_menu_open(!g_ui.menu_open);
            continue;
        }
        if (ev.type == SDL_EVENT_KEY_UP && ev.key.key == SDLK_AC_BACK) continue;
        const ImVec2 p(ev.tfinger.x * (float)g_ui.width, ev.tfinger.y * (float)g_ui.height);
        if (g_ui.layout_edit) {
            /* The editor reads fingers directly (it needs two for a pinch);
             * its panel is ImGui's, reached through synthesized mouse. */
            switch (ev.type) {
            case SDL_EVENT_FINGER_DOWN: edit_finger_down(ev.tfinger.fingerID, p); break;
            case SDL_EVENT_FINGER_MOTION: edit_finger_motion(ev.tfinger.fingerID, p); break;
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED: edit_finger_up(ev.tfinger.fingerID); break;
            default: ImGui_ImplSDL3_ProcessEvent(&ev); break;
            }
            continue;
        }
        if (g_ui.menu_open) {
            /* Touch reaches ImGui as SDL's synthesized mouse events. */
            if (ev.type != SDL_EVENT_FINGER_DOWN && ev.type != SDL_EVENT_FINGER_UP &&
                ev.type != SDL_EVENT_FINGER_MOTION && ev.type != SDL_EVENT_FINGER_CANCELED)
                ImGui_ImplSDL3_ProcessEvent(&ev);
            continue;
        }
        switch (ev.type) {
        case SDL_EVENT_FINGER_DOWN: finger_down(ev.tfinger.fingerID, p); break;
        case SDL_EVENT_FINGER_MOTION: finger_motion(ev.tfinger.fingerID, p); break;
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: finger_up(ev.tfinger.fingerID); break;
        default: break;
        }
    }
    poll_gamepads();
    recompute_touch_bits();
}

/* ---- drawing -------------------------------------------------------------------- */

ImU32 col(int r, int g, int b, float a) {
    return IM_COL32(r, g, b, (int)(std::clamp(a, 0.0f, 1.0f) * 255.0f));
}

void draw_touch(ImDrawList *dl) {
    const float a = g_ui.s.touch_opacity;
    const float u = layout_unit();
    const float line = std::max(2.0f, 0.45f * u);

    /* D-pad: a cross with the pressed arms lit. */
    uint16_t held = s_touch_bits;
    const float arm = s_dpad_r * 0.36f;
    const ImU32 base = col(20, 22, 30, a * 0.75f), edge = col(200, 205, 220, a);
    const float cx = s_dpad_c.x, cy = s_dpad_c.y, R = s_dpad_r, rr = arm * 0.35f;
    dl->AddCircleFilled(s_dpad_c, R * 1.08f, col(0, 0, 0, a * 0.25f), 48);
    /* Fill the cross as three pieces that do not overlap, so the translucent
     * centre is no darker than the arms. */
    dl->AddRectFilled(ImVec2(cx - R, cy - arm), ImVec2(cx + R, cy + arm), base, rr);
    dl->AddRectFilled(ImVec2(cx - arm, cy - R), ImVec2(cx + arm, cy - arm), base, rr,
                      ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(cx - arm, cy + arm), ImVec2(cx + arm, cy + R), base, rr,
                      ImDrawFlags_RoundCornersBottom);
    struct Arm { uint16_t bit; float dx, dy; } arms[4] = {
        {PADB_UP, 0, -1}, {PADB_DOWN, 0, 1}, {PADB_LEFT, -1, 0}, {PADB_RIGHT, 1, 0}};
    for (const Arm &m : arms) {
        const ImVec2 tip(s_dpad_c.x + m.dx * s_dpad_r * 0.82f, s_dpad_c.y + m.dy * s_dpad_r * 0.82f);
        const ImVec2 bl(s_dpad_c.x + m.dx * s_dpad_r * 0.5f - m.dy * arm * 0.55f,
                        s_dpad_c.y + m.dy * s_dpad_r * 0.5f - m.dx * arm * 0.55f);
        const ImVec2 br(s_dpad_c.x + m.dx * s_dpad_r * 0.5f + m.dy * arm * 0.55f,
                        s_dpad_c.y + m.dy * s_dpad_r * 0.5f + m.dx * arm * 0.55f);
        const bool on = (held & m.bit) != 0;
        dl->AddTriangleFilled(tip, bl, br, on ? col(255, 255, 255, a) : col(150, 155, 170, a));
    }
    /* One outline around the whole cross (rounded at the arm ends, square at
     * the inner corners), rather than two rectangles crossing in the middle.
     * ImGui arc angles count in twelfths of a turn, clockwise from +x. */
    dl->PathArcToFast(ImVec2(cx - arm + rr, cy - R + rr), rr, 6, 9);   /* up arm */
    dl->PathArcToFast(ImVec2(cx + arm - rr, cy - R + rr), rr, 9, 12);
    dl->PathLineTo(ImVec2(cx + arm, cy - arm));
    dl->PathArcToFast(ImVec2(cx + R - rr, cy - arm + rr), rr, 9, 12);  /* right arm */
    dl->PathArcToFast(ImVec2(cx + R - rr, cy + arm - rr), rr, 0, 3);
    dl->PathLineTo(ImVec2(cx + arm, cy + arm));
    dl->PathArcToFast(ImVec2(cx + arm - rr, cy + R - rr), rr, 0, 3);   /* down arm */
    dl->PathArcToFast(ImVec2(cx - arm + rr, cy + R - rr), rr, 3, 6);
    dl->PathLineTo(ImVec2(cx - arm, cy + arm));
    dl->PathArcToFast(ImVec2(cx - R + rr, cy + arm - rr), rr, 3, 6);   /* left arm */
    dl->PathArcToFast(ImVec2(cx - R + rr, cy - arm + rr), rr, 6, 9);
    dl->PathLineTo(ImVec2(cx - arm, cy - arm));
    dl->PathStroke(edge, ImDrawFlags_Closed, line * 0.6f);

    for (const TouchButton &b : s_buttons) {
        const bool on = b.bit ? (held & b.bit) != 0 : (b.kind == CTL_FF && g_ui.ff_active);
        const ImU32 fill = on ? col(255, 255, 255, a * 0.55f) : col(20, 22, 30, a * 0.7f);
        const ImU32 rim = col(200, 205, 220, a);
        const float r = b.half.x;
        switch (b.shape) {
        case SH_TRIANGLE: case SH_CIRCLE: case SH_CROSS: case SH_SQUARE: {
            dl->AddCircleFilled(b.c, r, fill, 40);
            dl->AddCircle(b.c, r, rim, 40, line * 0.6f);
            const float s = r * 0.45f;
            if (b.shape == SH_TRIANGLE)
                dl->AddTriangle(ImVec2(b.c.x, b.c.y - s * 1.05f), ImVec2(b.c.x - s, b.c.y + s * 0.75f),
                                ImVec2(b.c.x + s, b.c.y + s * 0.75f), col(64, 226, 160, a), line);
            else if (b.shape == SH_CIRCLE)
                dl->AddCircle(b.c, s, col(255, 102, 102, a), 32, line);
            else if (b.shape == SH_CROSS) {
                dl->AddLine(ImVec2(b.c.x - s, b.c.y - s), ImVec2(b.c.x + s, b.c.y + s), col(124, 178, 232, a), line);
                dl->AddLine(ImVec2(b.c.x + s, b.c.y - s), ImVec2(b.c.x - s, b.c.y + s), col(124, 178, 232, a), line);
            } else
                dl->AddRect(ImVec2(b.c.x - s * 0.85f, b.c.y - s * 0.85f), ImVec2(b.c.x + s * 0.85f, b.c.y + s * 0.85f),
                            col(255, 105, 248, a), 0, 0, line);
            break;
        }
        case SH_SHOULDER: case SH_PILL: {
            const ImVec2 lo(b.c.x - b.half.x, b.c.y - b.half.y), hi(b.c.x + b.half.x, b.c.y + b.half.y);
            dl->AddRectFilled(lo, hi, fill, b.half.y);
            dl->AddRect(lo, hi, rim, b.half.y, 0, line * 0.6f);
            ImFont *font = g_ui.font;
            const float fs = b.shape == SH_PILL ? b.half.y * 1.1f : b.half.y * 1.2f;
            const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, b.label);
            dl->AddText(font, fs, ImVec2(b.c.x - ts.x * 0.5f, b.c.y - ts.y * 0.5f), col(230, 232, 240, a), b.label);
            break;
        }
        case SH_ICON: {
            dl->AddCircleFilled(b.c, r, fill, 32);
            dl->AddCircle(b.c, r, rim, 32, line * 0.6f);
            const float s = r * 0.5f;
            if (b.kind == CTL_MENU) {   /* three bars */
                for (int i = -1; i <= 1; i++)
                    dl->AddLine(ImVec2(b.c.x - s, b.c.y + i * s * 0.6f), ImVec2(b.c.x + s, b.c.y + i * s * 0.6f), rim, line);
                /* While held, a ring fills towards opening the editor. */
                const float hold = menu_hold_progress();
                if (hold > 0.1f) {
                    const float a0 = -(float)M_PI * 0.5f;
                    dl->PathArcTo(b.c, r + line, a0, a0 + hold * 2.0f * (float)M_PI, 40);
                    dl->PathStroke(col(255, 204, 64, 1.0f), 0, line * 1.3f);
                }
            } else {                     /* fast-forward: two chevrons */
                for (int i = 0; i < 2; i++) {
                    const float x0 = b.c.x - s * 0.9f + i * s * 0.9f;
                    dl->AddTriangleFilled(ImVec2(x0, b.c.y - s * 0.7f), ImVec2(x0, b.c.y + s * 0.7f),
                                          ImVec2(x0 + s * 0.9f, b.c.y), on ? col(255, 214, 90, a) : rim);
                }
            }
            break;
        }
        }
    }
}

/* ---- layout editor ------------------------------------------------------------------ */

/* Opened by holding the menu button (or from the menu's Controls page), with
 * the game paused. Touch goes straight to the controls: press a control and
 * drag to move it, pinch it with two fingers to resize it (it also follows
 * the fingers). A panel holds the transparency and overall size sliders and
 * the reset, cancel and done buttons; it hides while a control is handled
 * and steps aside from the one picked. */
UiSettings s_edit_backup;
bool s_edit_from_menu = false;     /* Done returns to the menu, not the game */
int s_edit_sel = -1;               /* picked control */

struct EditFinger { SDL_FingerID id = 0; bool active = false; ImVec2 pos; };
EditFinger s_ef[2];
bool s_edit_drag = false;          /* one finger moving the picked control */
ImVec2 s_edit_grab;                /* finger offset from the control's centre */
bool s_pinch = false;              /* two fingers resizing it */
float s_pinch_d0 = 1.0f, s_pinch_scale0 = 1.0f;
ImVec2 s_pinch_mid0, s_pinch_c0;
ImVec4 s_panel_rect(0, 0, 0, 0);   /* last frame's panel (x0, y0, x1, y1) */
bool s_panel_shown = false;

int el_at(ImVec2 p, float slack = 1.3f) {
    int best = -1;
    float best_d = 1e30f;
    for (int e = 0; e < EL_COUNT; e++) {
        const ImVec2 c = s_el_c[e], h = s_el_half[e];
        if (std::fabs(p.x - c.x) > h.x * slack || std::fabs(p.y - c.y) > h.y * slack) continue;
        const float d = (p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y);
        if (d < best_d) { best = e; best_d = d; }
    }
    return best;
}

/* Nearest control to p, however far (for a pinch that starts beside one). */
int el_nearest(ImVec2 p) {
    int best = -1;
    float best_d = 1e30f;
    for (int e = 0; e < EL_COUNT; e++) {
        const float dx = p.x - s_el_c[e].x, dy = p.y - s_el_c[e].y;
        if (dx * dx + dy * dy < best_d) { best = e; best_d = dx * dx + dy * dy; }
    }
    return best;
}

void set_el_center(int e, ImVec2 c) {
    const float W = (float)g_ui.width, H = (float)g_ui.height;
    c.x = std::clamp(c.x, 0.0f, W);
    c.y = std::clamp(c.y, 0.0f, H);
    g_ui.s.ctl[e].dx = (c.x - s_el_home[e].x) / W;
    g_ui.s.ctl[e].dy = (c.y - s_el_home[e].y) / H;
}

float dist(ImVec2 a, ImVec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }
ImVec2 midpoint(ImVec2 a, ImVec2 b) { return ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f); }

void begin_pinch() {
    if (s_edit_sel < 0) s_edit_sel = el_nearest(midpoint(s_ef[0].pos, s_ef[1].pos));
    if (s_edit_sel < 0) return;
    s_pinch = true;
    s_edit_drag = false;
    s_pinch_d0 = std::max(1.0f, dist(s_ef[0].pos, s_ef[1].pos));
    s_pinch_scale0 = g_ui.s.ctl[s_edit_sel].scale;
    s_pinch_mid0 = midpoint(s_ef[0].pos, s_ef[1].pos);
    s_pinch_c0 = s_el_c[s_edit_sel];
}

void edit_finger_down(SDL_FingerID id, ImVec2 p) {
    /* Touches on the panel are ImGui's (through SDL's synthesized mouse). */
    if (s_panel_shown && p.x >= s_panel_rect.x && p.x <= s_panel_rect.z &&
        p.y >= s_panel_rect.y && p.y <= s_panel_rect.w)
        return;
    EditFinger *slot = !s_ef[0].active ? &s_ef[0] : !s_ef[1].active ? &s_ef[1] : nullptr;
    if (!slot) return;
    slot->id = id;
    slot->active = true;
    slot->pos = p;
    if (s_ef[0].active && s_ef[1].active) {
        begin_pinch();
        return;
    }
    s_edit_sel = el_at(p);
    s_edit_drag = s_edit_sel >= 0;
    if (s_edit_drag) s_edit_grab = ImVec2(p.x - s_el_c[s_edit_sel].x, p.y - s_el_c[s_edit_sel].y);
}

void edit_finger_motion(SDL_FingerID id, ImVec2 p) {
    EditFinger *f = s_ef[0].active && s_ef[0].id == id ? &s_ef[0]
                  : s_ef[1].active && s_ef[1].id == id ? &s_ef[1] : nullptr;
    if (!f) return;
    f->pos = p;
    if (s_pinch && s_ef[0].active && s_ef[1].active) {
        const float ratio = dist(s_ef[0].pos, s_ef[1].pos) / s_pinch_d0;
        g_ui.s.ctl[s_edit_sel].scale = std::clamp(s_pinch_scale0 * ratio, k_el_min_scale, k_el_max_scale);
        const ImVec2 mid = midpoint(s_ef[0].pos, s_ef[1].pos);
        set_el_center(s_edit_sel, ImVec2(s_pinch_c0.x + mid.x - s_pinch_mid0.x,
                                         s_pinch_c0.y + mid.y - s_pinch_mid0.y));
        layout_touch();
    } else if (s_edit_drag && s_edit_sel >= 0) {
        set_el_center(s_edit_sel, ImVec2(p.x - s_edit_grab.x, p.y - s_edit_grab.y));
        layout_touch();
    }
}

void edit_finger_up(SDL_FingerID id) {
    for (EditFinger &f : s_ef)
        if (f.active && f.id == id) f = EditFinger();
    if (s_pinch) {
        /* Down to one finger: carry on moving from where it is, no jump. */
        s_pinch = false;
        EditFinger *left = s_ef[0].active ? &s_ef[0] : s_ef[1].active ? &s_ef[1] : nullptr;
        s_edit_drag = left && s_edit_sel >= 0;
        if (s_edit_drag)
            s_edit_grab = ImVec2(left->pos.x - s_el_c[s_edit_sel].x, left->pos.y - s_el_c[s_edit_sel].y);
    } else if (!s_ef[0].active && !s_ef[1].active) {
        s_edit_drag = false;
    }
}

void end_layout_edit(bool keep) {
    if (!keep) g_ui.s = s_edit_backup;
    g_ui.layout_edit = false;
    for (EditFinger &f : s_ef) f = EditFinger();
    s_edit_drag = s_pinch = false;
    ui_save_settings();
    layout_touch();
    if (s_edit_from_menu) {
        ui_set_menu_open(true);   /* back to the Controls page */
    } else {
        release_all_fingers();
        psx_host_input_guard();   /* back to the game */
    }
}

void layout_editor() {
    const float W = (float)g_ui.width, H = (float)g_ui.height;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, 90));

    /* Drawn at the chosen transparency, so the slider previews it. */
    s_touch_bits = 0;
    draw_touch(dl);
    for (int e = 0; e < EL_COUNT; e++) {
        const ImVec2 c = s_el_c[e], h = s_el_half[e];
        const bool sel = e == s_edit_sel;
        dl->AddRect(ImVec2(c.x - h.x, c.y - h.y), ImVec2(c.x + h.x, c.y + h.y),
                    sel ? IM_COL32(255, 204, 64, 255) : IM_COL32(255, 255, 255, 70),
                    6.0f * g_ui.dpi, 0, (sel ? 3.0f : 1.5f) * g_ui.dpi);
    }

    s_panel_shown = !(s_edit_drag || s_pinch);
    if (!s_panel_shown) return;
    ImVec2 panel_pos(W * 0.5f, H * 0.5f), pivot(0.5f, 0.5f);
    if (s_edit_sel >= 0) {
        const ImVec2 c = s_el_c[s_edit_sel];
        const float edge = 6.0f * g_ui.dpi;
        if (std::fabs(c.x - W * 0.5f) < W * 0.3f && std::fabs(c.y - H * 0.5f) < H * 0.3f) {
            if (c.y < H * 0.5f) { panel_pos.y = H - edge; pivot.y = 1.0f; }  /* to the bottom edge */
            else                { panel_pos.y = edge;     pivot.y = 0.0f; }  /* to the top edge */
        }
    }
    ImGui::SetNextWindowPos(panel_pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 17.0f, 0), ImVec2(W * 0.5f, H));
    ImGui::Begin("##layout", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    const float em = ImGui::GetFontSize();
    ImGui::TextUnformatted("Customize touch controls");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.66f, 0.76f, 1.0f));
    ImGui::TextWrapped("Drag a control to move it. Pinch it with two fingers to resize it.");
    ImGui::PopStyleColor();
    ImGui::Separator();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Transparency");
    ImGui::SameLine(em * 7.0f);
    ImGui::SetNextItemWidth(-1);
    /* Shown as transparency (0% = solid), stored as opacity. */
    int clear = (int)std::lround((1.0f - g_ui.s.touch_opacity) * 100.0f);
    if (ImGui::SliderInt("##clear", &clear, 0, 85, "%d%%"))
        g_ui.s.touch_opacity = 1.0f - clear / 100.0f;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("All controls");
    ImGui::SameLine(em * 7.0f);
    ImGui::SetNextItemWidth(-1);
    int all = (int)std::lround(g_ui.s.touch_scale * 100.0f);
    if (ImGui::SliderInt("##all", &all, 60, 160, "%d%%")) {
        g_ui.s.touch_scale = all / 100.0f;
        layout_touch();
    }

    if (s_edit_sel >= 0) {
        CtlPlace &p = g_ui.s.ctl[s_edit_sel];
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s  %d%%", k_el_names[s_edit_sel], (int)std::lround(p.scale * 100.0f));
        ImGui::SameLine();
        if (ImGui::Button("Reset this control")) {
            p = CtlPlace();
            layout_touch();
        }
    }
    ImGui::Separator();
    if (ImGui::Button("Reset all")) {
        ui_reset_touch_layout();
        s_edit_sel = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) end_layout_edit(false);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.24f, 1.0f));
    if (ImGui::Button("Done", ImVec2(em * 5.0f, 0))) end_layout_edit(true);
    ImGui::PopStyleColor();
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    s_panel_rect = ImVec4(wp.x, wp.y, wp.x + ws.x, wp.y + ws.y);
    ImGui::End();
}

void draw_fps(ImDrawList *dl) {
    static uint64_t last_ms = 0, last_frame = 0;
    const uint64_t now = SDL_GetTicks(), frame = psx_host_frame_count();
    if (last_ms == 0) { last_ms = now; last_frame = frame; }
    if (now - last_ms >= 500) {
        g_ui.fps = (double)(frame - last_frame) * 1000.0 / (double)(now - last_ms);
        last_ms = now;
        last_frame = frame;
    }
    char text[48];
    std::snprintf(text, sizeof(text), "%.0f FPS%s", g_ui.fps,
                  psx_ui_game_speed() != 1.0f ? "  (speed changed)" : "");
    const float fs = g_ui.font->FontSize * 0.8f;
    const ImVec2 p(12.0f * g_ui.dpi, 8.0f * g_ui.dpi);
    dl->AddText(g_ui.font, fs, ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(g_ui.font, fs, p, IM_COL32(255, 235, 120, 255), text);
}

void setup_style() {
    ImGuiStyle &st = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    st.WindowRounding = 10.0f;
    st.ChildRounding = 8.0f;
    st.FrameRounding = 7.0f;
    st.GrabRounding = 7.0f;
    st.PopupRounding = 8.0f;
    st.ScrollbarRounding = 8.0f;
    st.TabRounding = 7.0f;
    st.FramePadding = ImVec2(10, 7);
    st.ItemSpacing = ImVec2(10, 6);
    st.ItemInnerSpacing = ImVec2(7, 6);
    st.WindowPadding = ImVec2(12, 10);
    st.CellPadding = ImVec2(8, 3);
    st.IndentSpacing = 16.0f;
    st.GrabMinSize = 22.0f;
    st.ScrollbarSize = 18.0f;
    ImVec4 *c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.063f, 0.086f, 0.96f);
    c[ImGuiCol_ChildBg] = ImVec4(0.075f, 0.086f, 0.118f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.16f, 0.24f, 0.45f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.30f, 0.56f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.24f, 0.36f, 0.66f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.17f, 0.25f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.27f, 0.42f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.26f, 0.38f, 0.64f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.14f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.19f, 0.28f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.25f, 0.38f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.80f, 0.26f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(1.0f, 0.74f, 0.22f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.84f, 0.40f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.22f, 0.26f, 0.36f, 1.0f);
    st.ScaleAllSizes(g_ui.dpi);
}

void load_fonts() {
    ImGuiIO &io = ImGui::GetIO();
    const float px = std::round(15.5f * g_ui.dpi);
    static const char *const k_fonts[] = {
        "/system/fonts/Roboto-Regular.ttf", "/system/fonts/RobotoStatic-Regular.ttf",
        "/system/fonts/NotoSans-Regular.ttf", "/system/fonts/DroidSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (const char *path : k_fonts) {
        FILE *f = std::fopen(path, "rb");
        if (!f) continue;
        std::fclose(f);
        g_ui.font = io.Fonts->AddFontFromFileTTF(path, px);
        g_ui.font_big = io.Fonts->AddFontFromFileTTF(path, std::round(px * 1.3f));
        g_ui.font_small = io.Fonts->AddFontFromFileTTF(path, std::round(px * 0.82f));
        if (g_ui.font && g_ui.font_big && g_ui.font_small) return;
    }
    ImFontConfig cfg;
    cfg.SizePixels = px;
    g_ui.font = io.Fonts->AddFontDefault(&cfg);
    cfg.SizePixels = std::round(px * 1.3f);
    g_ui.font_big = io.Fonts->AddFontDefault(&cfg);
    cfg.SizePixels = std::round(px * 0.82f);
    g_ui.font_small = io.Fonts->AddFontDefault(&cfg);
}

}  // namespace

/* ---- shared with the menu --------------------------------------------------------- */

void ui_save_settings() {
    /* Write a temp file and rename it over the settings, so a crash while
     * writing cannot leave a half-written file (and a lost layout). */
    const std::string path = settings_path(), tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    const UiSettings &s = g_ui.s;
    std::fprintf(f, "screen_fit=%d\n", s.screen_fit);
    std::fprintf(f, "touch_mode=%d\ntouch_opacity=%.2f\ntouch_scale=%.2f\nspeed=%.2f\n"
                    "ff_speed=%.2f\nshow_fps=%d\nvolume=%d\n",
                 s.touch_mode, s.touch_opacity, s.touch_scale, s.speed, s.ff_speed,
                 s.show_fps ? 1 : 0, s.volume);
    for (int e = 0; e < EL_COUNT; e++)
        std::fprintf(f, "ctl_%s=%.4f,%.4f,%.3f\n", k_el_keys[e], s.ctl[e].dx, s.ctl[e].dy, s.ctl[e].scale);
    const bool ok = std::fflush(f) == 0 && !std::ferror(f);
    if (std::fclose(f) == 0 && ok) std::rename(tmp.c_str(), path.c_str());
    else std::remove(tmp.c_str());
}

void ui_set_menu_open(bool open) {
    if (open == g_ui.menu_open) return;
    g_ui.menu_open = open;
    release_all_fingers();
    if (open) {
        ui_menu_on_open();
    } else {
        /* Buttons still held from closing the menu must not reach the game. */
        psx_host_input_guard();
    }
}

void ui_begin_layout_edit(bool from_menu) {
    s_edit_backup = g_ui.s;
    s_edit_from_menu = from_menu;
    s_edit_sel = -1;
    s_edit_drag = s_pinch = false;
    for (EditFinger &f : s_ef) f = EditFinger();
    g_ui.menu_open = false;     /* the game stays paused: see psx_ui_menu_open */
    release_all_fingers();
    g_ui.layout_edit = true;
}

void ui_reset_touch_layout() {
    g_ui.s.touch_scale = 1.0f;
    for (CtlPlace &p : g_ui.s.ctl) p = CtlPlace();
    layout_touch();
}

/* ---- C API ---------------------------------------------------------------------------- */

extern "C" void psx_ui_init(SDL_Window *window, SDL_Renderer *renderer, const char *data_dir) {
    if (g_ui.ready) return;
    g_ui.window = window;
    g_ui.renderer = renderer;
    g_ui.data_dir = data_dir ? data_dir : ".";
    load_settings();
    host_volume_set(g_ui.s.volume);

    SDL_GetRenderOutputSize(renderer, &g_ui.width, &g_ui.height);
    float dpi = SDL_GetWindowDisplayScale(window);
    if (!(dpi > 0.5f)) dpi = std::max(1.0f, (float)g_ui.height / 400.0f);
    g_ui.dpi = std::clamp(dpi, 1.0f, 4.0f);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    setup_style();
    load_fonts();

    SDL_SetEventFilter(event_filter, nullptr);
    layout_touch();
    g_ui.ready = true;
}

extern "C" void psx_ui_render(SDL_Renderer *renderer) {
    if (!g_ui.ready || renderer != g_ui.renderer) return;
    int w = 0, h = 0;
    SDL_GetRenderOutputSize(renderer, &w, &h);
    if (w != g_ui.width || h != g_ui.height || w == 0) {
        g_ui.width = w;
        g_ui.height = h;
    }
    layout_touch();
    pump_events();

    /* Draw in output pixels over the whole screen, not the game's letterboxed
     * logical area, so the controls can use the side bars. */
    int lw = 0, lh = 0;
    SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    SDL_GetRenderLogicalPresentation(renderer, &lw, &lh, &mode);
    SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_ui.width, (float)g_ui.height);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    ImGui::NewFrame();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    if (g_ui.layout_edit) layout_editor();
    else if (touch_visible()) draw_touch(dl);
    if (g_ui.s.show_fps && !g_ui.menu_open && !g_ui.layout_edit) draw_fps(dl);
    if (g_ui.menu_open) ui_menu_draw();
    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

    /* Put the game's logical size back, with the player's screen fit: its
     * picture is letterboxed to its own aspect, and a phone wider than that
     * either keeps the bars, stretches across them, or zooms to fill. Takes
     * effect from the next frame. */
    if (mode != SDL_LOGICAL_PRESENTATION_DISABLED) {
        /* 4:3-only frames (title, movies) keep their shape. */
        switch (psx_host_present_is_43() ? FIT_BARS : g_ui.s.screen_fit) {
        case FIT_STRETCH: mode = SDL_LOGICAL_PRESENTATION_STRETCH; break;
        case FIT_ZOOM: mode = SDL_LOGICAL_PRESENTATION_OVERSCAN; break;
        default: mode = SDL_LOGICAL_PRESENTATION_LETTERBOX; break;
        }
    }
    SDL_SetRenderLogicalPresentation(renderer, lw, lh, mode);
}

/* The layout editor pauses the game like the menu does. */
extern "C" int psx_ui_menu_open(void) { return g_ui.menu_open || g_ui.layout_edit ? 1 : 0; }

extern "C" uint16_t psx_ui_pad_buttons(void) {
    if (g_ui.ready && !g_ui.menu_open && !g_ui.layout_edit) {
        /* The game samples its pad before the frame is drawn, so take any
         * touches that arrived since the last frame now. */
        pump_events();
    }
    return (uint16_t)~s_touch_bits;
}

extern "C" float psx_ui_game_speed(void) {
    return g_ui.ff_active ? g_ui.s.ff_speed : g_ui.s.speed;
}
