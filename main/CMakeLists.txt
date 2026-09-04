# 《回声·下沉》主组件 —— 替换原 demo 菜单（原版已备份为 CMakeLists.txt.demo）。
# 只编译游戏源文件；main.c / demo_*.c / ui_pixel*.c 保留在磁盘但不再编译，
# 避免与 echo_main.c 的 app_main 重定义。
idf_component_register(
    SRCS "echo_core.c"
         "echo_render.c"
         "echo_main.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_lcd esp_timer freertos nvs_flash
    EMBED_FILES "sfx_data/knock.raw"
                "sfx_data/knock_soft.raw"
                "sfx_data/echo_open.raw"
                "sfx_data/echo_blocked.raw"
                "sfx_data/echo_alive.raw"
                "sfx_data/echo_answered.raw"
                "sfx_data/elev_up.raw"
                "sfx_data/elev_down.raw"
                "sfx_data/door.raw"
                "sfx_data/blip.raw"
                "sfx_data/breath.raw"
                "sfx_data/stinger.raw"
                "sfx_data/truth.raw"
                "sfx_data/footstep.raw"
                "sfx_data/ambience.raw"
)
