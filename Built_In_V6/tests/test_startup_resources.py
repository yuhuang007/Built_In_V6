"""Static startup resource ownership/timeout guards, not device acceptance."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/p4_main/main"


class StartupTests(unittest.TestCase):
    def test_network_not_blocked_by_font_load(self):
        source = (MAIN / "main.c").read_text(encoding="utf-8")
        boot = source[source.index("void app_main(void)"):]
        self.assertNotIn("try_load_sdcard_font();", boot)
        self.assertLess(boot.index("start_wifi_station();"),
                        boot.index("xTaskCreate(startup_resource_task"))
        self.assertIn("elapsed >= 20000", source)
        self.assertIn("startup_overlay = lv_obj_create(NULL)", source)

    def test_startup_screen_avoids_rendering_hidden_sd_home(self):
        source = (MAIN / "main.c").read_text(encoding="utf-8")
        create = source[source.index("static void create_startup_placeholder"):
                        source.index("void app_main(void)")]
        self.assertLess(create.index("startup_home_screen = lv_screen_active()"),
                        create.index("startup_overlay = lv_obj_create(NULL)"))
        self.assertIn("lv_screen_load(startup_overlay)", create)
        self.assertNotIn("lv_layer_top()", create)
        timer = source[source.index("static void startup_status_timer"):
                       source.index("static void create_startup_placeholder")]
        self.assertLess(timer.index("lv_screen_load(startup_home_screen)"),
                        timer.index("lv_obj_delete(startup_overlay)"))
        self.assertNotIn("lv_obj_delete(startup_home_screen)", timer)

    def test_loader_yields_without_dependency_patch(self):
        source = (MAIN / "p4_font_loader.c").read_text(encoding="utf-8")
        self.assertIn("p4_font_load_checkpoint();", source)
        self.assertIn("bsp_display_unlock();\n    vTaskDelay", source)
        self.assertIn("esp_timer_get_time() + 4000", source)
        self.assertIn(".dynamic_glyph_load = true", source)

    def test_sd_io_runs_without_display_lock_and_driver_restored(self):
        source = (MAIN / "p4_font_loader.c").read_text(encoding="utf-8")
        read = source[source.index("static lv_fs_res_t p4_font_read"):
                      source.index("static lv_fs_res_t p4_font_seek")]
        self.assertLess(read.index("bsp_display_unlock();"), read.index("io->original->read_cb"))
        self.assertGreater(read.index("p4_font_relock();"), read.index("io->original->read_cb"))
        self.assertIn("amount - total > 16384", read)
        self.assertIn("p4_font_restore_driver(&loader->file)", source)
        self.assertIn("p4_font_restore_driver(&file)", source)
        self.assertIn("p4_font_report_progress(fp, i, loca_count)", source)

    def test_no_20_second_exit_during_font_parse(self):
        source = (MAIN / "main.c").read_text(encoding="utf-8")
        timer = source[source.index("static void startup_status_timer"):
                       source.index("static void create_startup_placeholder")]
        self.assertIn("const bool preparation_ended = fonts_done &&", timer)
        self.assertIn("elapsed >= 60000", timer)
        self.assertIn("font_percent * 15 / 100", timer)
        self.assertIn("font_percent * 24 / 100", timer)
        self.assertIn("LV_ANIM_ON", timer)

    def test_fonts_survive_page_exit(self):
        source = (MAIN / "speech_to_text_mode.c").read_text(encoding="utf-8")
        release = source[source.index("static void release_mode_fonts"):
                         source.index("esp_err_t speech_to_text_mode_resources_init")]
        self.assertIn("if (transcript_font_owned)", release)
        self.assertNotIn("binfont_destroy", release)
        self.assertIn("xSemaphoreTake(resources_lock", source)
        self.assertIn("atomic_store(&fonts_ready, true)", source)

    def test_startup_does_not_install_missing_fonts(self):
        source = (MAIN / "sd_font_upgrade.c").read_text(encoding="utf-8")
        self.assertIn("if (!allow_install) return ESP_ERR_NOT_FOUND;", source)
        verify = source[source.index("esp_err_t sd_font_upgrade_verify_installed"):]
        self.assertIn("NULL, NULL, NULL, false", verify)
        mode = (MAIN / "speech_to_text_mode.c").read_text(encoding="utf-8")
        prewarm = mode[mode.index("esp_err_t speech_to_text_mode_preload_fonts"):
                       mode.index("static bool bind_native_mode_fonts")]
        self.assertIn("sd_font_upgrade_verify_installed()", prewarm)
        self.assertNotIn("sd_font_upgrade_ensure", prewarm)

    def test_welcome_waits_for_home_without_lifecycle_lock(self):
        source = (MAIN / "main.c").read_text(encoding="utf-8")
        registration = source[source.index("static void cloud_registration_task"):
                              source.index("static void start_cloud_registration(void)\n{")]
        self.assertLess(registration.index("while (!atomic_load(&startup_home_visible))"),
                        registration.index("xSemaphoreTake(cloud_engine_lock"))
        self.assertLess(registration.index("while (!atomic_load(&startup_home_visible))"),
                        registration.index("volc_start(cloud_engine"))
        timer = source[source.index("static void startup_status_timer"):
                       source.index("static void create_startup_placeholder")]
        self.assertIn("if (startup_overlay == NULL)", timer)
        self.assertIn("atomic_store(&startup_home_visible, true)", timer)
        self.assertIn("const bool preparation_ended = fonts_done &&", timer)
        self.assertIn("wifi_link_up && network_prepared", timer)
        self.assertNotIn("fonts_done && cloud_connected", timer)


if __name__ == "__main__":
    unittest.main()
