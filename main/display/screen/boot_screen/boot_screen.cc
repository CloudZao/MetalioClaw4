#include "boot_screen.h"
#include "lv_eaf.h"
#include "i18n.h"
LV_FONT_DECLARE(font_puhui_30_4);

lv_obj_t* BootScreen::Create() {
    lv_obj_t* screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * eaf_anim = lv_eaf_create(screen);
    lv_eaf_set_src(eaf_anim, "A:ic_boot_animation.eaf");
    lv_eaf_set_frame_delay(eaf_anim,30); 
    lv_eaf_set_loop_count(eaf_anim,0);
    lv_obj_center(eaf_anim);
    return screen;
}
