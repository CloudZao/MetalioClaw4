#include "boot_screen.h"
#include "lv_eaf.h"
LV_FONT_DECLARE(font_puhui_30_4);

lv_obj_t* BootScreen::Create() {
    lv_obj_t* screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // lv_obj_t* title = lv_label_create(screen);
    // lv_label_set_text(title, "XINGZHI");
    // lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    // lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    // lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);

    // lv_obj_t* tip = lv_label_create(screen);
    // lv_label_set_text(tip, "正在启动...");
    // lv_obj_set_style_text_color(tip, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    // lv_obj_set_style_text_font(tip, &font_puhui_30_4, LV_PART_MAIN);
    // lv_obj_align(tip, LV_ALIGN_CENTER, 0, 30);

    lv_obj_t * eaf_anim = lv_eaf_create(screen);
    lv_eaf_set_src(eaf_anim, "A:ic_boot_animation.eaf");
    lv_eaf_set_frame_delay(eaf_anim,1); 
    lv_eaf_set_loop_count(eaf_anim,0);
    lv_obj_center(eaf_anim);
    return screen;
}
