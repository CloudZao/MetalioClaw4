#include "game_2048_screen.h"

#include "home_screen/home_screen.h"

#include <stdlib.h>
#include <string.h>

#include <cmath>

#include "lvgl.h"

LV_FONT_DECLARE(font_puhui_20_4);

/* 720x720 layout (tuned for puhui 20px font) */
#define SCR_W           720
#define SCR_H           720
#define MARGIN_X        20
#define HEADER_Y        20
#define HEADER_H        80
#define BOARD_GAP_Y     10
#define BOARD_SIZE      552
#define BOARD_X         ((SCR_W - BOARD_SIZE) / 2)
#define BOARD_Y         (HEADER_Y + HEADER_H + BOARD_GAP_Y)
#define FOOTER_H        25
#define FOOTER_BOTTOM   14
#define FOOTER_Y        (SCR_H - FOOTER_H - FOOTER_BOTTOM)

#define GRID_N          4
#define PAD             12
#define GAP             12
#define CELL_SIZE       ((BOARD_SIZE - 2 * PAD - 3 * GAP) / GRID_N)

namespace {

uint16_t s_grid[GRID_N][GRID_N];
uint32_t s_score;
uint32_t s_best;
bool s_game_over;
bool s_won;
bool s_win_dismissed;

lv_obj_t* s_score_val;
lv_obj_t* s_best_val;
lv_obj_t* s_board;
lv_obj_t* s_cells[GRID_N][GRID_N];
lv_obj_t* s_labels[GRID_N][GRID_N];
lv_obj_t* s_overlay;
lv_obj_t* s_overlay_msg;
lv_point_t s_press_pt;
bool s_pressing;

uint32_t tile_bg_color(uint16_t v) {
    switch (v) {
    case 0:    return 0xcdc1b4;
    case 2:    return 0xeee4da;
    case 4:    return 0xede0c8;
    case 8:    return 0xf2b179;
    case 16:   return 0xf59563;
    case 32:   return 0xf67c5f;
    case 64:   return 0xf65e3b;
    case 128:  return 0xedcf72;
    case 256:  return 0xedcc61;
    case 512:  return 0xedc850;
    case 1024: return 0xedc53f;
    case 2048: return 0xedc22e;
    default:   return 0x3c3a32;
    }
}

uint32_t tile_fg_color(uint16_t v) {
    return (v <= 4) ? 0x776e65 : 0xf9f6f2;
}

int cell_px(int idx) {
    return PAD + idx * (CELL_SIZE + GAP);
}

void grid_clear() {
    memset(s_grid, 0, sizeof(s_grid));
}

bool grid_has_empty() {
    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            if (s_grid[r][c] == 0) {
                return true;
            }
        }
    }
    return false;
}

bool grid_can_merge() {
    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            uint16_t v = s_grid[r][c];
            if (v == 0) {
                continue;
            }
            if (c + 1 < GRID_N && s_grid[r][c + 1] == v) {
                return true;
            }
            if (r + 1 < GRID_N && s_grid[r + 1][c] == v) {
                return true;
            }
        }
    }
    return false;
}

void spawn_tile() {
    int empties[16];
    int n = 0;
    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            if (s_grid[r][c] == 0) {
                empties[n++] = r * GRID_N + c;
            }
        }
    }
    if (n == 0) {
        return;
    }
    int pick = empties[rand() % n];
    int r = pick / GRID_N;
    int c = pick % GRID_N;
    s_grid[r][c] = (rand() % 10 < 9) ? 2 : 4;
}

bool move_line(uint16_t line[4], uint32_t* score_add) {
    uint16_t orig[4];
    memcpy(orig, line, sizeof(orig));

    int w = 0;
    for (int i = 0; i < 4; i++) {
        if (line[i] != 0) {
            line[w++] = line[i];
        }
    }
    while (w < 4) {
        line[w++] = 0;
    }

    for (int i = 0; i < 3; i++) {
        if (line[i] != 0 && line[i] == line[i + 1]) {
            line[i] *= 2;
            *score_add += line[i];
            for (int j = i + 1; j < 3; j++) {
                line[j] = line[j + 1];
            }
            line[3] = 0;
            i--;
        }
    }

    w = 0;
    for (int i = 0; i < 4; i++) {
        if (line[i] != 0) {
            line[w++] = line[i];
        }
    }
    while (w < 4) {
        line[w++] = 0;
    }

    return memcmp(orig, line, sizeof(orig)) != 0;
}

bool move_left() {
    bool moved = false;
    uint32_t add = 0;
    for (int r = 0; r < GRID_N; r++) {
        uint16_t line[4] = {s_grid[r][0], s_grid[r][1], s_grid[r][2], s_grid[r][3]};
        if (move_line(line, &add)) {
            moved = true;
            for (int c = 0; c < GRID_N; c++) {
                s_grid[r][c] = line[c];
            }
        }
    }
    s_score += add;
    return moved;
}

bool move_right() {
    bool moved = false;
    uint32_t add = 0;
    for (int r = 0; r < GRID_N; r++) {
        uint16_t line[4] = {s_grid[r][3], s_grid[r][2], s_grid[r][1], s_grid[r][0]};
        if (move_line(line, &add)) {
            moved = true;
            for (int c = 0; c < GRID_N; c++) {
                s_grid[r][c] = line[3 - c];
            }
        }
    }
    s_score += add;
    return moved;
}

bool move_up() {
    bool moved = false;
    uint32_t add = 0;
    for (int c = 0; c < GRID_N; c++) {
        uint16_t line[4] = {s_grid[0][c], s_grid[1][c], s_grid[2][c], s_grid[3][c]};
        if (move_line(line, &add)) {
            moved = true;
            for (int r = 0; r < GRID_N; r++) {
                s_grid[r][c] = line[r];
            }
        }
    }
    s_score += add;
    return moved;
}

bool move_down() {
    bool moved = false;
    uint32_t add = 0;
    for (int c = 0; c < GRID_N; c++) {
        uint16_t line[4] = {s_grid[3][c], s_grid[2][c], s_grid[1][c], s_grid[0][c]};
        if (move_line(line, &add)) {
            moved = true;
            for (int r = 0; r < GRID_N; r++) {
                s_grid[r][c] = line[3 - r];
            }
        }
    }
    s_score += add;
    return moved;
}

void check_end_state() {
    if (s_score > s_best) {
        s_best = s_score;
    }
    if (!s_won) {
        for (int r = 0; r < GRID_N; r++) {
            for (int c = 0; c < GRID_N; c++) {
                if (s_grid[r][c] >= 2048) {
                    s_won = true;
                    break;
                }
            }
        }
    }
    if (!grid_has_empty() && !grid_can_merge()) {
        s_game_over = true;
    }
}

void update_scores_ui() {
    lv_label_set_text_fmt(s_score_val, "%u", (unsigned)s_score);
    lv_label_set_text_fmt(s_best_val, "%u", (unsigned)s_best);
}

void update_tiles_ui() {
    char buf[8];
    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            uint16_t v = s_grid[r][c];
            lv_obj_t* cell = s_cells[r][c];
            lv_obj_t* lbl = s_labels[r][c];
            lv_obj_set_style_bg_color(cell, lv_color_hex(tile_bg_color(v)), LV_PART_MAIN);
            if (v == 0) {
                lv_label_set_text(lbl, "");
                lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_snprintf(buf, sizeof(buf), "%u", (unsigned)v);
                lv_label_set_text(lbl, buf);
                lv_obj_remove_flag(lbl, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_text_color(lbl, lv_color_hex(tile_fg_color(v)), LV_PART_MAIN);
            }
        }
    }
    update_scores_ui();
}

void show_overlay(const char* msg) {
    lv_label_set_text(s_overlay_msg, msg);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

void hide_overlay() {
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

void refresh_after_move() {
    update_tiles_ui();
    if (s_game_over) {
        show_overlay("游戏结束!\n点击继续");
    } else if (s_won && !s_win_dismissed) {
        show_overlay("你赢了 2048!\n继续玩或点这里");
    }
}

bool do_move(bool (*fn)()) {
    if (s_game_over) {
        return false;
    }
    bool moved = fn();
    if (!moved) {
        return false;
    }
    spawn_tile();
    check_end_state();
    refresh_after_move();
    return true;
}

void game_reset() {
    s_score = 0;
    s_game_over = false;
    s_won = false;
    s_win_dismissed = false;
    grid_clear();
    spawn_tile();
    spawn_tile();
    hide_overlay();
    refresh_after_move();
}

lv_obj_t* make_score_box(lv_obj_t* parent, const char* title, lv_obj_t** value_out,
                         int x, int y, int w, int h) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_radius(box, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, lv_color_hex(0xbbada0), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 6, LV_PART_MAIN);

    lv_obj_t* t = lv_label_create(box);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(0xeee4da), LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 0);

    *value_out = lv_label_create(box);
    lv_label_set_text(*value_out, "0");
    lv_obj_set_style_text_color(*value_out, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(*value_out, LV_ALIGN_BOTTOM_MID, 0, 0);
    return box;
}

void board_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &s_press_pt);
        s_pressing = true;
    } else if (code == LV_EVENT_RELEASED && s_pressing) {
        lv_point_t rel;
        lv_indev_get_point(indev, &rel);
        s_pressing = false;

        int dx = rel.x - s_press_pt.x;
        int dy = rel.y - s_press_pt.y;
        const int th = 40;

        if (std::abs(dx) < th && std::abs(dy) < th) {
            return;
        }

        if (std::abs(dx) > std::abs(dy)) {
            if (dx > 0) {
                do_move(move_right);
            } else {
                do_move(move_left);
            }
        } else {
            if (dy > 0) {
                do_move(move_down);
            } else {
                do_move(move_up);
            }
        }
    }
}

void new_game_btn_cb(lv_event_t* e) {
    (void)e;
    game_reset();
}

void overlay_click_cb(lv_event_t* e) {
    (void)e;
    if (s_game_over) {
        game_reset();
    } else if (s_won) {
        s_win_dismissed = true;
        hide_overlay();
    }
}

void back_btn_cb(lv_event_t* e) {
    (void)e;
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void build_header(lv_obj_t* parent) {
    lv_obj_t* title = lv_label_create(parent);
    lv_label_set_text(title, "2048");
    lv_obj_set_style_text_color(title, lv_color_hex(0x776e65), LV_PART_MAIN);
    lv_obj_set_pos(title, MARGIN_X, HEADER_Y + 4);

    const int box_w = 120;
    const int box_h = 64;
    const int box_y = HEADER_Y + 8;
    make_score_box(parent, "分数", &s_score_val,
                   SCR_W - MARGIN_X - box_w * 2 - 10, box_y, box_w, box_h);
    make_score_box(parent, "最高", &s_best_val,
                   SCR_W - MARGIN_X - box_w, box_y, box_w, box_h);

    // "新游戏" button on the left.
    lv_obj_t* new_btn = lv_button_create(parent);
    lv_obj_set_size(new_btn, 120, 38);
    lv_obj_set_pos(new_btn, MARGIN_X, HEADER_Y + 38);
    lv_obj_set_style_bg_color(new_btn, lv_color_hex(0x8f7a66), LV_PART_MAIN);
    lv_obj_set_style_radius(new_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(new_btn, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(new_btn, new_game_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* nbt = lv_label_create(new_btn);
    lv_label_set_text(nbt, "新游戏");
    lv_obj_set_style_text_color(nbt, lv_color_hex(0xf9f6f2), LV_PART_MAIN);
    lv_obj_center(nbt);

    // "返回" button next to "新游戏".
    lv_obj_t* back_btn = lv_button_create(parent);
    lv_obj_set_size(back_btn, 90, 38);
    lv_obj_set_pos(back_btn, MARGIN_X + 120 + 10, HEADER_Y + 38);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xa39489), LV_PART_MAIN);
    lv_obj_set_style_radius(back_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(back_btn, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* bbt = lv_label_create(back_btn);
    lv_label_set_text(bbt, "返回");
    lv_obj_set_style_text_color(bbt, lv_color_hex(0xf9f6f2), LV_PART_MAIN);
    lv_obj_center(bbt);
}

void build_board(lv_obj_t* parent) {
    s_board = lv_obj_create(parent);
    lv_obj_remove_flag(s_board, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_board, BOARD_SIZE, BOARD_SIZE);
    lv_obj_set_pos(s_board, BOARD_X, BOARD_Y);
    lv_obj_set_style_radius(s_board, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_board, lv_color_hex(0xbbada0), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_board, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_board, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_board, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_board, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_board, board_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_board, board_event_cb, LV_EVENT_RELEASED, NULL);

    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            lv_obj_t* cell = lv_obj_create(s_board);
            s_cells[r][c] = cell;
            lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(cell, CELL_SIZE, CELL_SIZE);
            lv_obj_set_pos(cell, cell_px(c), cell_px(r));
            lv_obj_set_style_radius(cell, 6, LV_PART_MAIN);
            lv_obj_set_style_bg_color(cell, lv_color_hex(0xcdc1b4), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t* lbl = lv_label_create(cell);
            s_labels[r][c] = lbl;
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_center(lbl);
            lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void build_footer(lv_obj_t* parent) {
    lv_obj_t* hint = lv_label_create(parent);
    lv_label_set_text(hint, "在棋盘上滑动：上下左右移动方块");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x776e65), LV_PART_MAIN);
    lv_obj_set_width(hint, SCR_W - 2 * MARGIN_X);
    lv_obj_set_pos(hint, MARGIN_X, FOOTER_Y);
}

void build_overlay(lv_obj_t* parent) {
    s_overlay = lv_obj_create(parent);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_overlay, BOARD_SIZE, BOARD_SIZE);
    lv_obj_set_pos(s_overlay, BOARD_X, BOARD_Y);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_overlay, 10, LV_PART_MAIN);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_overlay, overlay_click_cb, LV_EVENT_CLICKED, NULL);

    s_overlay_msg = lv_label_create(s_overlay);
    lv_obj_set_style_text_color(s_overlay_msg, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_overlay_msg, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_overlay_msg, BOARD_SIZE - 40);
    lv_label_set_long_mode(s_overlay_msg, LV_LABEL_LONG_WRAP);
    lv_obj_center(s_overlay_msg);
}

}  // namespace

lv_obj_t* Game2048::Create() {
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xfaf8ef), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_font(scr, &font_puhui_20_4, LV_PART_MAIN);

    s_pressing = false;

    build_header(scr);
    build_board(scr);
    build_footer(scr);
    build_overlay(scr);

    srand((unsigned)lv_tick_get());
    game_reset();
    return scr;
}
