/*
 * psx_ui_menu.cpp — the in-game menu: a sidebar of sections (in the spirit of
 * Ship of Harkinian's enhancement menus) over a paused game.
 *
 *   Game          resume, save states, game speed, change disc, quit
 *   Graphics      widescreen, internal resolution, filtering, colour
 *   Audio         volume
 *   Controls      on-screen controls, controller shortcuts
 *   Enhancements  the port's mods (trainer, training, lifespan, roster, ...)
 *   Cheats        the GameShark list, applied live
 *   Partner       your Digimon's hidden stats, editable, plus bits and merit
 *   About
 *
 * Settings the runtime reads at boot (resolution, widescreen, mods) are
 * written straight away and flagged "restart to apply"; everything else takes
 * effect immediately.
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "config_loader.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "mod_packages.h"
#include "psx_ui_host.h"
#include "psx_ui_internal.h"

namespace fs = std::filesystem;
using PSXRecompV4::ModPackageManager;

namespace {

enum Section { SEC_GAME, SEC_GRAPHICS, SEC_AUDIO, SEC_CONTROLS, SEC_ENHANCE,
               SEC_CHEATS, SEC_PARTNER, SEC_ABOUT, SEC_COUNT };
const char *const k_section_names[SEC_COUNT] = {
    "Game", "Graphics", "Audio", "Controls", "Enhancements", "Cheats", "Partner", "About" };

Section s_section = SEC_GAME;
bool s_slider_active_last = false, s_slider_active = false;
bool s_confirm_quit = false, s_confirm_disc = false;
int s_confirm_load_slot = -1;   /* slot waiting for "load anyway" */
int s_confirm_load_match = 0;   /* its savestate_slot_mods_match() */

/* Runtime-owned settings (settings.toml) and mods (mods/state.toml). */
PSXRecompV4::UserSettings s_us;
ModPackageManager s_mods;
bool s_mods_ok = false;

const char *const k_cheats_pkg = "dw.vanilla.cheats";
const char *const k_cheats_feat = "cheats";
const char *const k_ws_pkg = "psx.enhancement.widescreen";
const char *const k_ws_feat = "widescreen";

std::string settings_toml() { return g_ui.data_dir + "/settings.toml"; }

void save_runtime_settings() {
    PSXRecompV4::save_user_settings(settings_toml(), s_us);
}

void save_mods() {
    std::string err;
    if (!s_mods.save_state(&err))
        host_osd_push(("Could not save mods: " + err).c_str(), 3000);
}

void mark_restart() { g_ui.restart_needed = true; }

/* ---- small widgets ------------------------------------------------------------- */

float em() { return ImGui::GetFontSize(); }

/* Explanations are set smaller and dimmer than the settings they explain. */
void note(const char *text) {
    ImGui::PushFont(g_ui.font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.64f, 0.74f, 1.0f));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void heading(const char *text) {
    ImGui::Dummy(ImVec2(0, em() * 0.2f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.80f, 0.36f, 1.0f));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

void track_slider() {
    if (ImGui::IsItemActive()) s_slider_active = true;
}

/* A label in a fixed-width column, with its control to the right of it. */
constexpr float k_label_em = 8.5f;
void row_label(const char *text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(em() * k_label_em);
}

/* Small text on a line of buttons, centred on them. */
void small_text_on_frame_line(const char *text, bool dim) {
    ImFont *f = g_ui.font_small;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 sz = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, text);
    const float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddText(f, f->FontSize, ImVec2(pos.x, pos.y + (h - sz.y) * 0.5f),
                                        ImGui::GetColorU32(dim ? ImGuiCol_TextDisabled : ImGuiCol_Text), text);
    ImGui::Dummy(ImVec2(sz.x, h));
}

/* How many columns of at least min_em fit in the space left. */
int grid_cols(float min_em, int max_cols = 4) {
    const float avail = ImGui::GetContentRegionAvail().x;
    return std::clamp((int)(avail / (min_em * em())), 1, max_cols);
}

bool begin_grid(const char *id, int cols) {
    return ImGui::BeginTable(id, cols, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings);
}

/* A checkbox whose label wraps inside its column instead of running off. */
bool wrapped_checkbox(const char *label, bool *v) {
    ImGui::PushID(label);
    bool changed = ImGui::Checkbox("##cb", v);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped("%s", label);
    /* Toggle on release, and only for a tap: a drag that starts on the label
     * is the player scrolling the page. */
    const float slop = 12.0f * g_ui.dpi;
    if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(0) &&
        ImGui::GetIO().MouseDragMaxDistanceSqr[0] < slop * slop) {
        *v = !*v;
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

/* A row of mutually exclusive buttons. Returns true when the value changed. */
template <typename T>
bool segmented(const char *id, T *value, const T *values, const char *const *labels, int n) {
    bool changed = false;
    ImGui::PushID(id);
    for (int i = 0; i < n; i++) {
        if (i) ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        const bool on = *value == values[i];
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        if (ImGui::Button(labels[i], ImVec2(0, 0)) && !on) {
            *value = values[i];
            changed = true;
        }
        if (on) ImGui::PopStyleColor();
    }
    ImGui::PopID();
    return changed;
}

bool restart_note(bool pending) {
    if (!pending) return false;
    ImGui::PushFont(g_ui.font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
    ImGui::TextWrapped("Takes effect the next time the game starts (Game > Restart now).");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    return true;
}

/* Touch-drag scrolling, for the sidebar and the content pane alike: a
 * vertical drag that does not start on a slider scrolls the window instead of
 * pressing whatever was under the finger. Called inside the child window. */
void touch_scroll() {
    struct Drag { ImGuiID id = 0; bool dragging = false; float press_y = 0.0f; };
    static Drag drags[4];
    ImGuiIO &io = ImGui::GetIO();
    const ImGuiID id = ImGui::GetCurrentWindow()->ID;
    Drag *d = nullptr;
    for (Drag &x : drags) if (x.id == id) { d = &x; break; }
    if (!d) for (Drag &x : drags) if (x.id == 0) { d = &x; d->id = id; break; }
    if (!d) return;
    if (!io.MouseDown[0]) {
        d->dragging = false;
        d->press_y = -1.0f;
    }
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                                ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (hovered && ImGui::IsMouseClicked(0)) {
        d->press_y = io.MousePos.y;
        d->dragging = false;
    }
    if (!io.MouseDown[0] || d->press_y < 0.0f || s_slider_active_last) return;
    if (!d->dragging && std::fabs(io.MousePos.y - d->press_y) > 12.0f * g_ui.dpi) {
        d->dragging = true;
        ImGui::ClearActiveID();
    }
    if (d->dragging) ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
}

/* ---- pages ------------------------------------------------------------------------ */

void page_game() {
    heading("Save states");
    note("A snapshot of the game this instant, separate from the memory card "
         "(your normal save is still made by sleeping).");
    const int cols = grid_cols(19.0f, 3);
    if (begin_grid("##slots", cols)) {
        for (int slot = 0; slot < 12; slot++) {
            ImGui::TableNextColumn();
            ImGui::PushID(slot);
            char path[1024];
            const bool exists = savestate_slot_exists(slot) != 0;
            char when[64] = "empty";
            struct stat st;
            if (exists && savestate_slot_path(slot, path, sizeof(path)) && stat(path, &st) == 0) {
                const time_t t = st.st_mtime;
                std::strftime(when, sizeof(when), "%d %b %H:%M", std::localtime(&t));
            }
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%2d", slot + 1);
            ImGui::SameLine();
            small_text_on_frame_line(when, !exists);
            const float bw = em() * 3.6f, sp = ImGui::GetStyle().ItemInnerSpacing.x;
            ImGui::SameLine();
            const float room = ImGui::GetContentRegionAvail().x - (2 * bw + sp);
            if (room > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room);
            if (ImGui::Button("Save", ImVec2(bw, 0))) {
                if (psx_host_savestate_submit(slot, 1)) ui_set_menu_open(false);
            }
            ImGui::SameLine(0, sp);
            ImGui::BeginDisabled(!exists);
            if (ImGui::Button("Load", ImVec2(bw, 0))) {
                /* A state from under other Enhancements would bring their
                 * code patches back with it: ask first. */
                const int match = savestate_slot_mods_match(slot);
                if (match == 1) {
                    if (psx_host_savestate_submit(slot, 0)) ui_set_menu_open(false);
                } else {
                    s_confirm_load_slot = slot;
                    s_confirm_load_match = match;
                }
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    heading("Speed");
    static const float speeds[] = { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
    static const char *const labels[] = { "0.5x", "1x", "1.5x", "2x", "3x", "4x" };
    row_label("Game speed");
    if (segmented("speed", &g_ui.s.speed, speeds, labels, 6)) ui_save_settings();
    static const float ffs[] = { 2.0f, 3.0f, 4.0f, 6.0f, 8.0f };
    static const char *const fflabels[] = { "2x", "3x", "4x", "6x", "8x" };
    row_label("Fast-forward");
    if (segmented("ff", &g_ui.s.ff_speed, ffs, fflabels, 5)) ui_save_settings();
    note("Fast-forward toggles with the >> button on screen or R3 (click the right "
         "stick). Sound is muted at any speed but 1x.");
    if (ImGui::Checkbox("Show FPS counter", &g_ui.s.show_fps)) ui_save_settings();

    heading("Disc and app");
    if (g_ui.restart_needed) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.38f, 0.10f, 1.0f));
        if (ImGui::Button("Restart now to apply changes")) {
            psx_host_shutdown();
            psx_android_restart_app();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    if (ImGui::Button("Change disc...")) s_confirm_disc = true;
    ImGui::SameLine();
    if (ImGui::Button("Quit game")) s_confirm_quit = true;
    if (g_ui.restart_needed) note("Restarting loses progress since your last save or save state.");
}

void page_graphics() {
    heading("Widescreen");
    int ws = 0;   /* 0 off, 1 16:9, 2 16:10, 3 widest */
    if (s_mods_ok && s_mods.feature_enabled(k_ws_pkg, k_ws_feat)) {
        const std::string a = s_mods.feature_option_value(k_ws_pkg, k_ws_feat, "aspect");
        ws = a == "16:10" ? 2 : a == "widest" ? 3 : 1;
    }
    static const int wsv[] = { 0, 2, 1, 3 };
    static const char *const wsl[] = { "Off (4:3)", "16:10", "16:9", "Widest" };
    static const char *const wsval[] = { "", "16:9", "16:10", "widest" };
    row_label("Aspect ratio");
    if (s_mods_ok && segmented("ws", &ws, wsv, wsl, 4)) {
        s_mods.set_feature_enabled(k_ws_pkg, k_ws_feat, ws != 0);
        if (ws) s_mods.set_feature_option(k_ws_pkg, k_ws_feat, "aspect", wsval[ws]);
        save_mods();
        mark_restart();
    }
    note("Shows more of the world at the sides instead of stretching; menus keep "
         "their shape. Widest (1.86:1) is as far as the game's scenery reaches. "
         "Rough edges: about a second of 4:3 after a battle, and a few interiors "
         "end at the old screen edge.");
    restart_note(g_ui.restart_needed);

    static const int fv[] = { FIT_BARS, FIT_STRETCH, FIT_ZOOM };
    static const char *const fl[] = { "Black bars", "Stretch", "Zoom" };
    row_label("Fill screen");
    if (segmented("fit", &g_ui.s.screen_fit, fv, fl, 3)) ui_save_settings();
    note("For screens wider than the picture: keep black bars at the sides, "
         "stretch it sideways to fill (about 16% on a 19.5:9 phone with Widest), "
         "or zoom in to fill, trimming a little off the top and bottom.");

    heading("Picture");
    int scale = s_us.has_supersampling ? s_us.supersampling : psx_host_internal_scale();
    static const int sv[] = { 1, 2, 3, 4 };
    static const char *const sl[] = { "1x", "2x", "3x", "4x" };
    row_label("Resolution");
    if (segmented("scale", &scale, sv, sl, 4)) {
        s_us.has_supersampling = true;
        s_us.supersampling = scale;
        save_runtime_settings();
        mark_restart();
    }
    note("Internal 3D resolution (1x is the original). 4x needs a fast phone; "
         "drop to 2x if the game slows down.");

    int kind = psx_host_screen_kind();
    static const int kv[] = { 0, 1, 2, 3 };
    static const char *const kl[] = { "Raw", "CRT", "Composite", "Trinitron" };
    row_label("Colour");
    if (segmented("kind", &kind, kv, kl, 4)) {
        psx_host_set_screen_kind(kind);
        s_us.has_screen_kind = true;
        s_us.screen_kind = kind;
        save_runtime_settings();
    }
    note("The console's raw colours, or the look of a period TV (Trinitron is "
         "this port's default).");

    if (begin_grid("##filters", grid_cols(15.0f, 2))) {
        ImGui::TableNextColumn();
        bool smooth = psx_host_video_smooth() != 0;
        if (ImGui::Checkbox("Smooth scaling", &smooth)) {
            psx_host_set_video_smooth(smooth);
            s_us.has_antialiasing = true;
            s_us.antialiasing = smooth;
            save_runtime_settings();
        }
        note("Softens the picture when scaled to your screen.");
        ImGui::TableNextColumn();
        bool bilinear = psx_host_texture_filter() != 0;
        if (ImGui::Checkbox("Texture filtering", &bilinear)) {
            psx_host_set_texture_filter(bilinear);
            s_us.has_texture_filter = true;
            s_us.texture_filter = bilinear ? 1 : 0;
            save_runtime_settings();
        }
        note("Smooths the original blocky textures.");
        ImGui::EndTable();
    }
}

void page_audio() {
    heading("Volume");
    int vol = host_volume_get();
    row_label("Game volume");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##vol", &vol, 0, 100, "%d%%")) {
        host_volume_set(vol);
        g_ui.s.volume = vol;
    }
    track_slider();
    if (ImGui::IsItemDeactivatedAfterEdit()) ui_save_settings();
    note("Game sound is also muted while fast-forwarding.");
}

void page_controls() {
    heading("On-screen controls");
    static const int mv[] = { TOUCH_AUTO, TOUCH_ALWAYS, TOUCH_OFF };
    static const char *const ml[] = { "Auto", "Always", "Hidden" };
    row_label("Show");
    if (segmented("tm", &g_ui.s.touch_mode, mv, ml, 3)) ui_save_settings();
    note("Auto hides them while you play with a controller and brings them back "
         "when you touch the screen.");
    if (begin_grid("##touch", grid_cols(16.0f, 2))) {
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Opacity");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        int op = (int)std::lround(g_ui.s.touch_opacity * 100.0f);
        if (ImGui::SliderInt("##op", &op, 15, 100, "%d%%")) g_ui.s.touch_opacity = op / 100.0f;
        track_slider();
        if (ImGui::IsItemDeactivatedAfterEdit()) ui_save_settings();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Size");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        int sz = (int)std::lround(g_ui.s.touch_scale * 100.0f);
        if (ImGui::SliderInt("##sz", &sz, 60, 160, "%d%%")) g_ui.s.touch_scale = sz / 100.0f;
        track_slider();
        if (ImGui::IsItemDeactivatedAfterEdit()) ui_save_settings();
        ImGui::EndTable();
    }
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.36f, 0.62f, 1.0f));
    if (ImGui::Button("Move and resize controls...")) ui_begin_layout_edit();
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("Reset to default")) {
        ui_reset_touch_layout();
        ui_save_settings();
        host_osd_push("Touch controls reset", 1200);
    }
    note("Move any button anywhere and set each one's size. Reset puts every "
         "control back to its default size and place.");

    heading("Controller");
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (n == 0) note("No controller connected. Pair one in Android's Bluetooth settings; "
                     "it is picked up even while the game is running.");
    for (int i = 0; ids && i < n; i++) {
        const char *name = SDL_GetGamepadNameForID(ids[i]);
        ImGui::BulletText("%s", name ? name : "Controller");
    }
    SDL_free(ids);
    static const char *const rows[][2] = {
        { "A / B / X / Y", "Cross / Circle / Square / Triangle" },
        { "LB / RB, LT / RT", "L1 / R1, L2 / R2" },
        { "Menu / View", "Start / Select" },
        { "D-pad or left stick", "D-pad" },
        { "View + Menu", "Open this menu" },
        { "View + RB", "Quick save-state slots" },
        { "R3 (click right stick)", "Fast-forward on/off" },
        { "Back gesture", "Open this menu" },
    };
    const int pairs = grid_cols(22.0f, 2);
    if (ImGui::BeginTable("map", pairs * 2, ImGuiTableFlags_BordersInnerH |
                                            ImGuiTableFlags_SizingStretchProp |
                                            ImGuiTableFlags_NoSavedSettings)) {
        for (auto &r : rows) {
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.84f, 1.0f, 1.0f));
            ImGui::TextWrapped("%s", r[0]);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", r[1]);
        }
        ImGui::EndTable();
    }
}

/* Mods shown elsewhere or not usable in this build. */
bool hidden_feature(const std::string &pkg, const std::string &feat) {
    return pkg == k_cheats_pkg || pkg == k_ws_pkg || pkg == "psx.enhancement.pgxp" ||
           pkg == "psx.enhancement.fast-loading" || feat == "native-evolution";
}

void option_widget(const std::string &pkg, const std::string &feat,
                   const PSXRecompV4::ModOption &o) {
    std::string v = s_mods.feature_option_value(pkg, feat, o.id);
    ImGui::PushID(o.id.c_str());
    bool changed = false;
    switch (o.type) {
    case PSXRecompV4::ModOptionType::Boolean: {
        bool b = v == "true";
        if (wrapped_checkbox(o.label.c_str(), &b)) { v = b ? "true" : "false"; changed = true; }
        break;
    }
    case PSXRecompV4::ModOptionType::Integer: {
        int iv = std::atoi(v.c_str());
        ImGui::PushFont(g_ui.font_small);
        ImGui::TextUnformatted(o.label.c_str());
        ImGui::PopFont();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##i", &iv, (int)o.min_value, (int)o.max_value)) {
            const int step = (int)std::max<int64_t>(1, o.step);
            iv = (int)o.min_value + ((iv - (int)o.min_value) / step) * step;
        }
        track_slider();
        if (ImGui::IsItemDeactivatedAfterEdit()) { v = std::to_string(iv); changed = true; }
        break;
    }
    case PSXRecompV4::ModOptionType::Choice: {
        ImGui::PushFont(g_ui.font_small);
        ImGui::TextUnformatted(o.label.c_str());
        ImGui::PopFont();
        std::string cur_label = v;
        for (auto &c : o.choices) if (c.value == v) cur_label = c.label;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##c", cur_label.c_str())) {
            for (auto &c : o.choices) {
                if (ImGui::Selectable(c.label.c_str(), c.value == v)) { v = c.value; changed = true; }
            }
            ImGui::EndCombo();
        }
        break;
    }
    }
    if (!o.description.empty()) note(o.description.c_str());
    if (changed) {
        s_mods.set_feature_option(pkg, feat, o.id, v);
        save_mods();
        mark_restart();
    }
    ImGui::PopID();
}

void page_enhancements() {
    if (!s_mods_ok) {
        note("The mod catalog could not be read.");
        return;
    }
    note("Changes to the game's rules, applied when the game starts. Turning one "
         "off restores the original game exactly.");
    restart_note(g_ui.restart_needed);
    /* group -> (package, feature) */
    std::map<std::string, std::vector<std::pair<const PSXRecompV4::ModPackage *,
                                                const PSXRecompV4::ModFeature *>>> groups;
    for (auto &[id, versions] : s_mods.packages()) {
        (void)versions;
        const PSXRecompV4::ModPackage *p = s_mods.selected_package(id);
        if (!p) continue;
        for (auto &f : p->features)
            if (!hidden_feature(p->id, f.id)) groups[f.group].push_back({ p, &f });
    }
    const int cols = grid_cols(14.0f, 3);
    for (auto &[group, feats] : groups) {
        heading(group.c_str());
        if (!begin_grid(group.c_str(), cols)) continue;
        for (auto &[p, f] : feats) {
            ImGui::TableNextColumn();
            ImGui::PushID((p->id + "/" + f->id).c_str());
            bool on = s_mods.feature_enabled(p->id, f->id);
            if (wrapped_checkbox(f->name.c_str(), &on)) {
                s_mods.set_feature_enabled(p->id, f->id, on);
                save_mods();
                mark_restart();
            }
            if (!f->description.empty()) note(f->description.c_str());
            if (on) {
                ImGui::Indent(em() * 0.8f);
                for (auto &o : p->options)
                    if (o.feature_id == f->id) option_widget(p->id, f->id, o);
                ImGui::Unindent(em() * 0.8f);
            }
            ImGui::Dummy(ImVec2(0, em() * 0.15f));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void page_cheats() {
    if (!s_mods_ok) return;
    const PSXRecompV4::ModPackage *p = s_mods.selected_package(k_cheats_pkg);
    if (!p) {
        note("The cheat list is not installed.");
        return;
    }
    note("GameShark-style codes, applied instantly. Some change things saved to "
         "your memory card (stats, items, story flags), so their effects can "
         "outlast turning them off. Back up your saves first.");
    const int cols = grid_cols(15.0f, 3);
    std::string last_group;
    bool open = false, in_table = false;
    for (auto &o : p->options) {
        if (o.feature_id != k_cheats_feat) continue;
        if (o.group != last_group) {
            if (in_table) { ImGui::EndTable(); in_table = false; }
            last_group = o.group;
            open = ImGui::CollapsingHeader(o.group.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            if (open) in_table = begin_grid(o.group.c_str(), cols);
            open = open && in_table;
        }
        if (!open) continue;
        ImGui::TableNextColumn();
        const std::string v = s_mods.feature_option_value(k_cheats_pkg, k_cheats_feat, o.id);
        bool on = v == "true";
        if (wrapped_checkbox(o.label.c_str(), &on)) {
            /* Hand the whole list to these toggles (the cheats feature), then
             * flip this one live in the running game. */
            if (!s_mods.feature_enabled(k_cheats_pkg, k_cheats_feat))
                s_mods.set_feature_enabled(k_cheats_pkg, k_cheats_feat, true);
            s_mods.set_feature_option(k_cheats_pkg, k_cheats_feat, o.id, on ? "true" : "false");
            save_mods();
            psx_cheats_set_enabled(o.label.c_str(), on ? 1 : 0);
        }
    }
    if (in_table) ImGui::EndTable();
}

/* ---- Partner: Digimon World's own data ------------------------------------------ */

constexpr uint32_t PARTNER = 0x801557A8u;      /* PARTNER_ENTITY (type word) */
constexpr uint32_t STATS = PARTNER + 0x38u;    /* Stats block after the Entity */
constexpr uint32_t PARA = 0x80138460u;         /* PARTNER_PARA (raise sim) */
constexpr uint32_t DIGIMON_DATA = 0x8012CEB4u; /* per-species records */
constexpr uint32_t DIGIMON_STRIDE = 52u;
constexpr uint32_t BITS = 0x80134EB8u;         /* money, clamps at 999999 */
constexpr uint32_t MERIT = 0x80134FC4u;        /* merit points, clamps at 9999 */

int16_t rd16(uint32_t a) { return (int16_t)psx_read_half(a); }
void wr16(uint32_t a, int v) { psx_write_half(a, (uint16_t)(int16_t)v); }

bool stat_slider(const char *label, uint32_t addr, int lo, int hi) {
    int v = rd16(addr);
    ImGui::TableNextColumn();
    ImGui::PushID((int)addr);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(em() * 7.0f);
    ImGui::SetNextItemWidth(-1);
    const bool changed = ImGui::SliderInt("##v", &v, lo, hi);
    track_slider();
    if (changed) wr16(addr, std::clamp(v, lo, hi));
    ImGui::PopID();
    return changed;
}

void page_partner() {
    const uint32_t type = psx_read_word(PARTNER);
    if (type == 0 || type > 180) {
        note("No partner yet. Start or continue a game and this page shows your "
             "Digimon's hidden stats.");
        return;
    }
    char name[24] = {0};
    for (int i = 0; i < 20; i++) {
        const uint8_t c = psx_read_byte(DIGIMON_DATA + type * DIGIMON_STRIDE + (uint32_t)i);
        if (!c) break;
        name[i] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    ImGui::PushFont(g_ui.font_big);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", name[0] ? name : "Partner");
    ImGui::PopFont();
    ImGui::SameLine();
    if (ImGui::Button("Restore HP and MP")) {
        wr16(STATS + 0x14, rd16(STATS + 0x10));
        wr16(STATS + 0x16, rd16(STATS + 0x12));
    }
    note("Changes apply to the running game at once. Stats and care values reach "
         "your memory card the next time you sleep.");

    const int cols = grid_cols(17.0f, 3);
    heading("Battle stats");
    if (begin_grid("##battle", cols)) {
        stat_slider("Offense", STATS + 0x00, 0, 999);
        stat_slider("Defense", STATS + 0x02, 0, 999);
        stat_slider("Speed", STATS + 0x04, 0, 999);
        stat_slider("Brains", STATS + 0x06, 0, 999);
        stat_slider("Max HP", STATS + 0x10, 1, 9999);
        stat_slider("Max MP", STATS + 0x12, 0, 9999);
        stat_slider("HP", STATS + 0x14, 1, std::max(1, (int)rd16(STATS + 0x10)));
        stat_slider("MP", STATS + 0x16, 0, std::max(0, (int)rd16(STATS + 0x12)));
        ImGui::EndTable();
    }

    heading("Care");
    if (begin_grid("##care", cols)) {
        stat_slider("Happiness", PARA + 0x2A, -100, 100);
        stat_slider("Discipline", PARA + 0x28, 0, 100);
        stat_slider("Tiredness", PARA + 0x22, 0, 100);
        stat_slider("Weight", PARA + 0x42, 1, 99);
        stat_slider("Mistakes", PARA + 0x52, 0, 99);
        stat_slider("Virus", PARA + 0x1E, 0, 16);
        stat_slider("Battles won", PARA + 0x54, 0, 999);
        ImGui::EndTable();
    }

    heading("Age, lifespan and tamer");
    if (begin_grid("##age", cols)) {
        stat_slider("Age (days)", PARA + 0x4A, 0, 99);
        stat_slider("Life left (h)", PARA + 0x48, 0, 999);
        ImGui::TableNextColumn();
        int bits = (int)psx_read_word(BITS);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Bits");
        ImGui::SameLine(em() * 7.0f);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##bits", &bits, 0, 999999)) psx_write_word(BITS, (uint32_t)bits);
        track_slider();
        stat_slider("Merit", MERIT, 0, 9999);
        ImGui::EndTable();
    }
    note("Lifespan is counted in in-game hours; when it runs out your partner "
         "dies of old age.");
}

void page_about() {
    heading("Digimon World for Android");
    note("A native port built from the psx-recomp-port static recompilation of "
         "Digimon World (USA, SLUS-01032). The app contains nothing from the "
         "game: your disc was translated into C on this phone and is compiled "
         "each time the game starts.");
    heading("Credits");
    note("psx-recomp-port (Digimon World) by ethan4love. PSXRecomp framework by "
         "Matthew Stan (PolyForm Noncommercial). OpenBIOS (PCSX-Redux project). "
         "TinyCC, SDL3, Dear ImGui, libchdr, rabbitizer.");
    heading("Your files");
    ImGui::TextWrapped("%s", g_ui.data_dir.c_str());
    note("Saves are in saves/card1.mcd; back them up over USB. This folder is "
         "deleted if you uninstall the app (Android asks whether to keep it).");
}

void confirm_popups() {
    static int s_load_slot = -1, s_load_match = 0;
    if (s_confirm_load_slot >= 0) {
        s_load_slot = s_confirm_load_slot;
        s_load_match = s_confirm_load_match;
        s_confirm_load_slot = -1;
        ImGui::OpenPopup("Load this save state?");
    }
    ImGui::SetNextWindowPos(ImVec2(g_ui.width * 0.5f, g_ui.height * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(g_ui.width * 0.6f, (float)g_ui.height));
    if (ImGui::BeginPopupModal("Load this save state?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(g_ui.width * 0.55f);
        if (s_load_match == 0)
            ImGui::TextWrapped("Slot %d was saved with different Enhancements turned on.", s_load_slot + 1);
        else
            ImGui::TextWrapped("Slot %d was saved by an older version of the app, which did not "
                               "record which Enhancements were on.", s_load_slot + 1);
        note("A save state brings back the game exactly as it was, including the changes "
             "the Enhancements on at the time made to the game. Ones you have turned off "
             "since would stay on until you restart, and in rare cases the game can "
             "misbehave. Your memory card is not affected.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Load anyway", ImVec2(em() * 8, 0))) {
            ImGui::CloseCurrentPopup();
            if (psx_host_savestate_submit(s_load_slot, 2)) ui_set_menu_open(false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(em() * 7, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (s_confirm_quit) { ImGui::OpenPopup("Quit?"); s_confirm_quit = false; }
    if (s_confirm_disc) { ImGui::OpenPopup("Change disc?"); s_confirm_disc = false; }
    const ImVec2 center(g_ui.width * 0.5f, g_ui.height * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Quit the game? Unsaved progress is lost.");
        if (ImGui::Button("Quit", ImVec2(em() * 7, 0))) {
            psx_host_shutdown();
            psx_android_quit_app();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(em() * 7, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Change disc?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Remove the current disc and choose another?");
        note("Your memory card saves are kept.");
        if (ImGui::Button("Change disc", ImVec2(em() * 8, 0))) {
            std::error_code ec;
            for (const char *ext : { ".bin", ".cue", ".chd" })
                fs::remove(fs::path(g_ui.data_dir) / "disc" / (std::string("Digimon World (USA)") + ext), ec);
            fs::remove(fs::path(g_ui.data_dir) / "generated" / ".translated", ec);
            psx_host_shutdown();
            psx_android_restart_app();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(em() * 7, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

}  // namespace

void ui_menu_on_open() {
    s_us = PSXRecompV4::load_user_settings(settings_toml());
    std::string err;
    s_mods.set_root(fs::path(g_ui.data_dir) / "mods");
    s_mods_ok = s_mods.scan(&err) && s_mods.load_state(&err);
}

void ui_menu_draw() {
    s_slider_active_last = s_slider_active;
    s_slider_active = false;
    const float margin = 6.0f * g_ui.dpi;
    ImGui::SetNextWindowPos(ImVec2(margin, margin));
    ImGui::SetNextWindowSize(ImVec2(g_ui.width - 2 * margin, g_ui.height - 2 * margin));
    ImGui::Begin("##menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    /* Title bar */
    ImGui::PushFont(g_ui.font_big);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Digimon World");
    ImGui::PopFont();
    const float resume_w = em() * 7.5f;
    ImGui::SameLine(ImGui::GetWindowWidth() - resume_w - ImGui::GetStyle().WindowPadding.x);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.24f, 1.0f));
    if (ImGui::Button("Resume", ImVec2(resume_w, 0))) ui_set_menu_open(false);
    ImGui::PopStyleColor();
    ImGui::Separator();

    /* Sidebar: every section fits, so it never scrolls. A section opens only
     * on a tap (press and release on it without sliding); a finger dragged
     * across the list neither highlights nor selects anything. */
    const float side_w = em() * 8.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.0f * g_ui.dpi));
    ImGui::BeginChild("##side", ImVec2(side_w, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImGuiIO &io = ImGui::GetIO();
        const float slop = 12.0f * g_ui.dpi;
        const float row_h = em() * 1.9f;
        for (int i = 0; i < SEC_COUNT; i++) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const ImVec2 sz(ImGui::GetContentRegionAvail().x, row_h);
            ImGui::PushID(i);
            bool tapped = ImGui::InvisibleButton("##sec", sz);
            /* A touch release counts only if the finger stayed put; controller
             * activation has no mouse release and always counts. */
            if (tapped && io.MouseReleased[0] &&
                io.MouseDragMaxDistanceSqr[0] > slop * slop)
                tapped = false;
            if (tapped) s_section = (Section)i;
            ImGui::PopID();
            if (s_section == i)
                dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y),
                                  ImGui::GetColorU32(ImGuiCol_Header), ImGui::GetStyle().FrameRounding);
            dl->AddText(ImVec2(p.x + ImGui::GetStyle().FramePadding.x, p.y + (row_h - em()) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_Text), k_section_names[i]);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::SameLine();

    /* Content */
    ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_None);
    touch_scroll();
    switch (s_section) {
    case SEC_GAME: page_game(); break;
    case SEC_GRAPHICS: page_graphics(); break;
    case SEC_AUDIO: page_audio(); break;
    case SEC_CONTROLS: page_controls(); break;
    case SEC_ENHANCE: page_enhancements(); break;
    case SEC_CHEATS: page_cheats(); break;
    case SEC_PARTNER: page_partner(); break;
    case SEC_ABOUT: page_about(); break;
    default: break;
    }
    ImGui::Dummy(ImVec2(0, em()));
    ImGui::EndChild();

    confirm_popups();

    /* Controller: B or Start closes the menu when nothing else wants it. */
    if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
         ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)))
        ui_set_menu_open(false);
    ImGui::End();
}
