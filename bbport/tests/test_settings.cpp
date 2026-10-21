// SPDX-License-Identifier: GPL-2.0-or-later
// bbport_settings.cpp: the menu saves its keys into bbport.ini and keeps the launcher's (controls).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>
#include "gpu/shim/bbport_settings.h"

#ifdef _WIN32
// MSYS2 builds: the CRT has no setenv.
static int setenv(const char* name, const char* value, int) { return _putenv_s(name, value); }
static int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif

static std::string Read(const char* path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

int main() {
    // Windows (MSYS2): the user's temp folder; elsewhere /tmp.
    const char* temp = std::getenv("TEMP");
    std::string pattern = std::string(temp && temp[0] ? temp : "/tmp") + "/bbport-settings-test-XXXXXX";
    char* path = pattern.data();
    const int fd = mkstemp(path);
    assert(fd >= 0);
    const char ini[] = "# launcher\nupscaler=fsr3\nkey.cross=X, Space\npad.circle=a\nshow_fps=0\n"
                       "fullscreen_hint=1\n";
    assert(write(fd, ini, sizeof(ini) - 1) == ssize_t(sizeof(ini) - 1));
    close(fd);
    setenv("BB_CONFIG", path, 1);

    BbSettings::Load();
    auto& s = BbSettings::Get();
    assert(s.upscaler == BbSettings::UpscalerFsr3 && !s.show_fps && s.menu_x < 0.0f);
    s.show_fps = true;
    s.upscaler = BbSettings::UpscalerFsr411;
    s.menu_x = 0.625f;
    s.menu_y = 0.125f;
    BbSettings::Save();

    const std::string saved = Read(path);
    // The launcher's controls, its other keys and comments stay; the menu's keys are replaced
    // in place, new ones appended.
    assert(saved.find("# launcher\nupscaler=fsr411\nkey.cross=X, Space\npad.circle=a\nshow_fps=1\n"
                      "fullscreen_hint=1\n") == 0);
    assert(saved.find("menu_pos=0.6250,0.1250\n") != std::string::npos);
    assert(saved.find("upscaler=fsr3") == std::string::npos);

    s.menu_x = -1.0f;
    s.menu_y = -1.0f;
    BbSettings::Load();
    assert(s.menu_x == 0.625f && s.menu_y == 0.125f && s.upscaler == BbSettings::UpscalerFsr411);
    unlink(path);
    std::puts("PASS: settings save keeps other keys, menu position");

    // Kaniel 2026-10-10: an FPS limit and the DLSS model in the game's own menu. The menu's
    // frame cap is kept in bbport.ini and wins over the launcher's (BB_FPS_LIMIT) once set there.
    std::string second = std::string(temp && temp[0] ? temp : "/tmp") + "/bbport-settings-test-XXXXXX";
    const int fd2 = mkstemp(second.data());
    assert(fd2 >= 0);
    const char ini2[] = "fps_limit=144\ndlss_model=K\n";
    assert(write(fd2, ini2, sizeof(ini2) - 1) == ssize_t(sizeof(ini2) - 1));
    close(fd2);
    setenv("BB_CONFIG", second.c_str(), 1);
    setenv("BB_FPS_LIMIT", "60", 1);
    BbSettings::Load();
    assert(s.fps_limit == 144 && "the menu's cap, not the launcher's");
    assert(s.dlss_model == 11 && "NVSDK_NGX_DLSS_Hint_Render_Preset_K");
    s.fps_limit = 240;
    s.dlss_model = 13;
    BbSettings::Save();
    const std::string saved2 = Read(second.c_str());
    assert(saved2.find("fps_limit=240\n") != std::string::npos);
    assert(saved2.find("dlss_model=M\n") != std::string::npos);
    unlink(second.c_str());
    // No fps_limit in the file: the launcher's.
    char third[512];
    std::snprintf(third, sizeof(third), "%s/bbport-settings-test-XXXXXX", temp && temp[0] ? temp : "/tmp");
    close(mkstemp(third));
    setenv("BB_CONFIG", third, 1);
    BbSettings::Load();
    assert(s.fps_limit == 60);
    unlink(third);
    unsetenv("BB_FPS_LIMIT");

    // The cap applies to the unlocked frame rate only: the game's 30 and 60 modes keep their
    // own fixed timestep (a cap below it would slow the game down).
    assert(BbSettings::EffectiveFrameLimit(144, true) == 144);
    assert(BbSettings::EffectiveFrameLimit(144, false) == 0);
    assert(BbSettings::EffectiveFrameLimit(0, true) == 0);
    // The menu's rows: a cap or model from elsewhere shows as the nearest choice below it.
    assert(BbSettings::FpsLimitIndex(0) == 0);
    assert(BbSettings::FpsLimitIndex(144) == 5);
    assert(BbSettings::FpsLimits[BbSettings::FpsLimitIndex(75)] == 60);
    assert(BbSettings::FpsLimits[BbSettings::FpsLimitIndex(1000)] == 240);
    assert(BbSettings::DlssModelIndex(0) == 0);
    assert(BbSettings::DlssModels[BbSettings::DlssModelIndex(11)] == 11);
    assert(BbSettings::DlssModelIndex(99) == 0 && "unknown: NVIDIA's default");
    // Online (co-op) play keeps the fixed 60 FPS mode (Kaniel 2026-10-13: a host above 60 made
    // the guests see everyone freeze and fly): the FPS limit rows are locked there.
    assert(BbSettings::FrameRateLocked("1"));
    assert(!BbSettings::FrameRateLocked("0"));
    assert(!BbSettings::FrameRateLocked(""));
    assert(!BbSettings::FrameRateLocked(nullptr));
    std::puts("PASS: FPS limit and DLSS model settings");

    // Kaniel 2026-10-17: the teammate compass, on at the top by default; the Insert menu turns it
    // off or moves it (bottom, around the middle), kept in bbport.ini.
    {
        const BbSettings::Values fresh;
        assert(fresh.compass && fresh.compass_position == BbSettings::CompassTop);
        // Kaniel 2026-10-20: the party's bloodstains on the compass and the map, on by default.
        assert(fresh.bloodstains);
        char path[512];
        std::snprintf(path, sizeof(path), "%s/bbport-settings-test-XXXXXX", temp && temp[0] ? temp : "/tmp");
        const int file = mkstemp(path);
        const char text[] = "compass=0\ncompass_position=reticle\nbloodstains=0\n";
        assert(write(file, text, sizeof(text) - 1) == ssize_t(sizeof(text) - 1));
        close(file);
        setenv("BB_CONFIG", path, 1);
        BbSettings::Load();
        assert(!s.compass && s.compass_position == BbSettings::CompassReticle);
        assert(!s.bloodstains);
        s.compass = true;
        s.compass_position = BbSettings::CompassBottom;
        s.bloodstains = true;
        BbSettings::Save();
        const std::string saved_compass = Read(path);
        assert(saved_compass.find("compass=1\n") != std::string::npos);
        assert(saved_compass.find("compass_position=bottom\n") != std::string::npos);
        assert(saved_compass.find("bloodstains=1\n") != std::string::npos);
        unlink(path);
    }
    std::puts("PASS: compass settings");

    // Kaniel 2026-10-19 ("a setting to remove fog of war, so I will be able to see the whole map"):
    // the map's fog, on by default, kept in bbport.ini.
    {
        const BbSettings::Values fresh;
        assert(fresh.map_fog);
        char path[512];
        std::snprintf(path, sizeof(path), "%s/bbport-settings-test-XXXXXX", temp && temp[0] ? temp : "/tmp");
        const int file = mkstemp(path);
        const char text[] = "map_fog=0\n";
        assert(write(file, text, sizeof(text) - 1) == ssize_t(sizeof(text) - 1));
        close(file);
        setenv("BB_CONFIG", path, 1);
        BbSettings::Load();
        assert(!s.map_fog);
        s.map_fog = true;
        BbSettings::Save();
        assert(Read(path).find("map_fog=1\n") != std::string::npos);
        unlink(path);
    }
    std::puts("PASS: map fog setting");

    // Kaniel 2026-10-21 ("another 3D map mode that uses the game's own camera"): map_3d=3 is the
    // game camera's view, kept in bbport.ini; opt-in only (Kaniel): never the default.
    {
        const BbSettings::Values fresh;
        assert(fresh.map_3d == BbSettings::Map3dOff && fresh.map_3d != BbSettings::Map3dGameCamera);
        char path[512];
        std::snprintf(path, sizeof(path), "%s/bbport-settings-test-XXXXXX", temp && temp[0] ? temp : "/tmp");
        const int file = mkstemp(path);
        const char text[] = "map_3d=3\n";
        assert(write(file, text, sizeof(text) - 1) == ssize_t(sizeof(text) - 1));
        close(file);
        setenv("BB_CONFIG", path, 1);
        BbSettings::Load();
        assert(s.map_3d == BbSettings::Map3dGameCamera);
        BbSettings::Save();
        assert(Read(path).find("map_3d=3\n") != std::string::npos);
        s.map_3d = BbSettings::Map3dOff;
        unlink(path);
    }
    std::puts("PASS: map 3D game camera setting");

    // Kaniel 2026-10-10: window mode (windowed, borderless, exclusive) and HDR output in the game's
    // menus. The launcher's BB_FULLSCREEN counts until the file has a window mode.
    char fourth[512];
    std::snprintf(fourth, sizeof(fourth), "%s/bbport-settings-test-XXXXXX", temp && temp[0] ? temp : "/tmp");
    close(mkstemp(fourth));
    setenv("BB_CONFIG", fourth, 1);
    setenv("BB_FULLSCREEN", "1", 1);
    BbSettings::Load();
    assert(s.window_mode == BbSettings::Borderless && "the launcher's fullscreen is borderless");
    // Kaniel 2026-10-19: white and peak follow the display (the desktop's SDR white and the
    // display's peak) unless the player picks a brightness.
    assert(!s.hdr && s.hdr_peak_nits == BbSettings::HdrAuto && s.hdr_paper_white == BbSettings::HdrAuto);
    assert(s.hdr_scene && "highlights from the 16-bit scene by default");
    s.window_mode = BbSettings::ExclusiveFullscreen;
    s.hdr = true;
    s.hdr_peak_nits = 1500;
    s.hdr_paper_white = 250;
    s.hdr_highlights = 0.7f;
    s.hdr_scene = false;
    BbSettings::Save();
    const std::string saved4 = Read(fourth);
    assert(saved4.find("window_mode=fullscreen\n") != std::string::npos);
    assert(saved4.find("hdr=1\n") != std::string::npos);
    assert(saved4.find("hdr_peak_nits=1500\n") != std::string::npos);
    assert(saved4.find("hdr_paper_white=250\n") != std::string::npos);
    assert(saved4.find("hdr_highlights=0.70\n") != std::string::npos);
    assert(saved4.find("hdr_scene=0\n") != std::string::npos);
    s.hdr = false;
    BbSettings::Load();
    assert(s.window_mode == BbSettings::ExclusiveFullscreen && "the file's mode wins over BB_FULLSCREEN");
    assert(s.hdr && s.hdr_peak_nits == 1500 && s.hdr_paper_white == 250 && s.hdr_highlights == 0.7f);
    assert(!s.hdr_scene);
    setenv("BB_FULLSCREEN", "0", 1);
    unlink(fourth);
    BbSettings::Load();
    assert(s.window_mode == BbSettings::Windowed);
    unsetenv("BB_FULLSCREEN");
    // The menu's rows show the nearest offered brightness.
    assert(BbSettings::HdrPeakNits[BbSettings::HdrPeakIndex(1000)] == 1000);
    assert(BbSettings::HdrPeakNits[BbSettings::HdrPeakIndex(1100)] == 1000);
    assert(BbSettings::HdrPeakNits[BbSettings::HdrPeakIndex(9000)] == 4000);
    assert(BbSettings::HdrPaperWhiteNits[BbSettings::HdrPaperWhiteIndex(203)] == 200);
    assert(BbSettings::HdrPaperWhiteNits[BbSettings::HdrPaperWhiteIndex(480)] == 480 &&
           "Windows' brightest SDR white is offered");
    assert(BbSettings::HdrPeakIndex(BbSettings::HdrAuto) == 0 &&
           BbSettings::HdrPaperWhiteIndex(BbSettings::HdrAuto) == 0);
    assert(BbSettings::HdrPeakNits[BbSettings::HdrPeakIndex(100)] == 400 && "a brightness is never auto");
    assert(BbSettings::HdrPaperWhiteNits[BbSettings::HdrPaperWhiteIndex(50)] == 80);
    std::puts("PASS: window mode and HDR settings");

    // Kaniel 2026-10-19 (PG27UCDM, Windows SDR white 480 nits): auto takes the display's values;
    // without them (an unknown display) 200 and 1000; the peak is never below the white. Auto
    // white stays at half the peak or less, so lamps have a stop of room (a quarter, 250 nits on
    // his display, was "too dark, especially on auto").
    {
        char hdr_file[512];
        std::snprintf(hdr_file, sizeof(hdr_file), "%s/bbport-settings-test-XXXXXX", temp && temp[0] ? temp : "/tmp");
        const int fd = mkstemp(hdr_file);
        assert(fd >= 0);
        const char ini[] = "hdr=1\nhdr_peak_nits=auto\nhdr_paper_white=auto\n";
        assert(write(fd, ini, sizeof(ini) - 1) == ssize_t(sizeof(ini) - 1));
        close(fd);
        setenv("BB_CONFIG", hdr_file, 1);
        s.hdr_peak_nits = 1500;
        s.hdr_paper_white = 250;
        BbSettings::Load();
        assert(s.hdr_peak_nits == BbSettings::HdrAuto && s.hdr_paper_white == BbSettings::HdrAuto);
        s.hdr_display_white = 0.0f;
        s.hdr_display_peak = 0.0f;
        assert(BbSettings::HdrPaperWhite() == 200.0f && BbSettings::HdrPeak() == 1000.0f);
        s.hdr_display_white = 480.0f;
        s.hdr_display_peak = 1000.0f;
        assert(BbSettings::HdrPaperWhite() == 480.0f && BbSettings::HdrPeak() == 1000.0f &&
               "the desktop's white under half the peak");
        s.hdr_display_peak = 800.0f;
        assert(BbSettings::HdrPaperWhite() == 400.0f && "half the peak");
        s.hdr_display_peak = 1000.0f;
        s.hdr_display_white = 200.0f;
        assert(BbSettings::HdrPaperWhite() == 200.0f && "a dimmer desktop's own white");
        s.hdr_display_white = 480.0f;
        s.hdr_display_peak = 400.0f;
        assert(BbSettings::HdrPaperWhite() == 200.0f && BbSettings::HdrPeak() == 400.0f);
        s.hdr_paper_white = 480;
        assert(BbSettings::HdrPeak() == 480.0f && "no peak below a picked white");
        s.hdr_paper_white = BbSettings::HdrAuto;
        s.hdr_paper_white = 250;
        s.hdr_peak_nits = 1500;
        assert(BbSettings::HdrPaperWhite() == 250.0f && BbSettings::HdrPeak() == 1500.0f &&
               "a picked brightness wins");
        s.hdr_paper_white = BbSettings::HdrAuto;
        s.hdr_peak_nits = BbSettings::HdrAuto;
        BbSettings::Save();
        const std::string saved = Read(hdr_file);
        assert(saved.find("hdr_peak_nits=auto\n") != std::string::npos);
        assert(saved.find("hdr_paper_white=auto\n") != std::string::npos);
        s.hdr_display_white = 0.0f;
        s.hdr_display_peak = 0.0f;
        unlink(hdr_file);
    }
    std::puts("PASS: HDR brightness follows the display");

    // Kaniel 2026-10-11: the controls in the game's menu. The lines it does not keep in Values
    // (key.*, pad.*, mouse_*, button_icons: runtime_pad.c and the icon step read them) are read
    // from the file, and Save replaces them in place, appends new ones and removes cleared ones
    // (the default applies again).
    char fifth[512];
    std::snprintf(fifth, sizeof(fifth), "%s/bbport-settings-test-XXXXXX", temp && temp[0] ? temp : "/tmp");
    const int fd5 = mkstemp(fifth);
    assert(fd5 >= 0);
    const char ini5[] = "key.cross=X, Space\npad.circle=a\nmouse_sensitivity=1.50\nupscaler=fsr3\n";
    assert(write(fd5, ini5, sizeof(ini5) - 1) == ssize_t(sizeof(ini5) - 1));
    close(fd5);
    setenv("BB_CONFIG", fifth, 1);
    BbSettings::Load();
    assert(BbSettings::Line("key.cross") == std::optional<std::string>("X, Space"));
    assert(BbSettings::Line("mouse_sensitivity") == std::optional<std::string>("1.50"));
    assert(!BbSettings::Line("key.circle"));
    BbSettings::SetLine("key.cross", "Ctrl+F");
    BbSettings::SetLine("pad.circle", std::nullopt);
    BbSettings::SetLine("key.l1", "Mouse X1");
    assert(BbSettings::Line("key.cross") == std::optional<std::string>("Ctrl+F") && "read back before the save");
    assert(!BbSettings::Line("pad.circle"));
    BbSettings::Save();
    const std::string saved5 = Read(fifth);
    assert(saved5.find("key.cross=Ctrl+F\nmouse_sensitivity=1.50\nupscaler=") == 0);
    assert(saved5.find("pad.circle") == std::string::npos);
    assert(saved5.find("key.l1=Mouse X1\n") != std::string::npos);
    BbSettings::Load();
    assert(BbSettings::Line("key.l1") == std::optional<std::string>("Mouse X1") && !BbSettings::Line("pad.circle"));
    unlink(fifth);
    std::puts("PASS: control lines read, changed and removed");

    // Frame generation (Supermedo's 026f8ff, 389124d): the switch stays frame_generation; the
    // provider (FSR 3.1 or DLSS), the multiplier (2x-4x) and fitting to the display are new keys,
    // with environment overrides for scripts.
    char sixth[512];
    std::snprintf(sixth, sizeof(sixth), "%s/bbport-settings-test-XXXXXX", temp && temp[0] ? temp : "/tmp");
    const int fd6 = mkstemp(sixth);
    assert(fd6 >= 0);
    const char ini6[] = "frame_generation=1\nframe_gen_provider=dlss\nframe_gen_multiplier=7\n"
                        "frame_gen_fit_display=1\n";
    assert(write(fd6, ini6, sizeof(ini6) - 1) == ssize_t(sizeof(ini6) - 1));
    close(fd6);
    setenv("BB_CONFIG", sixth, 1);
    BbSettings::Load();
    assert(s.frame_generation && s.frame_gen_provider == BbSettings::FrameGenDlss);
    assert(s.frame_gen_multiplier == 4 && "clamped to 2..4");
    assert(s.frame_gen_fit_display);
    s.frame_gen_multiplier = 3;
    BbSettings::Save();
    const std::string saved6 = Read(sixth);
    assert(saved6.find("frame_gen_provider=dlss\n") != std::string::npos);
    assert(saved6.find("frame_gen_multiplier=3\n") != std::string::npos);
    assert(saved6.find("frame_gen_fit_display=1\n") != std::string::npos);
    setenv("BB_FRAME_GEN_PROVIDER", "fsr3", 1);
    setenv("BB_FRAME_GEN_MULTIPLIER", "1", 1);
    setenv("BB_FRAME_GEN_FIT_DISPLAY", "0", 1);
    BbSettings::Load();
    assert(s.frame_gen_provider == BbSettings::FrameGenFsr3 && s.frame_gen_multiplier == 2 &&
           !s.frame_gen_fit_display);
    unsetenv("BB_FRAME_GEN_PROVIDER");
    unsetenv("BB_FRAME_GEN_MULTIPLIER");
    unsetenv("BB_FRAME_GEN_FIT_DISPLAY");
    // An unknown provider is FSR 3.1 (any GPU).
    {
        std::FILE* file = std::fopen(sixth, "w");
        std::fputs("frame_gen_provider=xyz\n", file);
        std::fclose(file);
    }
    s.frame_gen_provider = BbSettings::FrameGenDlss;
    BbSettings::Load();
    assert(s.frame_gen_provider == BbSettings::FrameGenFsr3);
    unlink(sixth);
    std::puts("PASS: frame generation provider, multiplier, fit to display");

    // The menus speak the game's language (the launcher's BB_LANGUAGE, the PS4 system language
    // the game reads), every one the game has, until one is chosen in the Insert menu
    // (overlay_language in bbport.ini; "auto" is the game's).
    {
        using BbSettings::MenuLanguage;
        assert(BbSettings::LanguageFromPs4(1) == MenuLanguage::English);
        assert(BbSettings::LanguageFromPs4(18) == MenuLanguage::English && "English (UK)");
        assert(BbSettings::LanguageFromPs4(8) == MenuLanguage::Russian);
        assert(BbSettings::LanguageFromPs4(0) == MenuLanguage::Ja);
        assert(BbSettings::LanguageFromPs4(4) == MenuLanguage::De);
        assert(BbSettings::LanguageFromPs4(20) == MenuLanguage::Es419);
        assert(BbSettings::LanguageFromPs4(11) == MenuLanguage::ZhCn);
        assert(BbSettings::LanguageFromPs4(17) == MenuLanguage::PtBr);
        assert(BbSettings::LanguageFromPs4(99) == MenuLanguage::English && "a language the game lacks");
        assert(std::string(BbSettings::LanguageCode(MenuLanguage::ZhTw)) == "zh_TW");
        assert(std::string(BbSettings::LanguageName(MenuLanguage::Russian)) == "Русский");
        assert(std::string(BbSettings::LanguageName(MenuLanguage::De)) == "Deutsch");
        for (int l = 0; l < BbSettings::MenuLanguageCount; ++l) {
            const auto language = MenuLanguage(l);
            assert(BbSettings::LanguageFromCode(BbSettings::LanguageCode(language)) == language);
        }
        assert(!BbSettings::LanguageFromCode("xx"));

        // Every language has every text; Russian is the source's, unknown texts stay English.
        assert(std::string(BbSettings::Translate(MenuLanguage::Russian, "Off", "Выкл")) == "Выкл");
        assert(std::string(BbSettings::Translate(MenuLanguage::English, "Off", "Выкл")) == "Off");
        assert(std::string(BbSettings::Translate(MenuLanguage::De, "Blood Echoes", "Отголоски крови")) == "Blutechos");
        assert(std::string(BbSettings::Translate(MenuLanguage::Ja, "Insight", "Озарение")) == "啓蒙");
        assert(std::string(BbSettings::Translate(MenuLanguage::Fr, "not a menu text", "нет")) == "not a menu text");
        const std::string german(BbSettings::Translate(MenuLanguage::De, "Level %d, HP %d / %d", "Уровень %d, здоровье %d / %d"));
        assert(german.find("%d") != std::string::npos && german != "Level %d, HP %d / %d");

        char path[512];
        std::snprintf(path, sizeof(path), "%s/bbport-settings-test-XXXXXX", temp && temp[0] ? temp : "/tmp");
        const int file = mkstemp(path);
        const char text[] = "menu_language=ru\n"; // the old key: written by every save, not a choice
        assert(write(file, text, sizeof(text) - 1) == ssize_t(sizeof(text) - 1));
        close(file);
        setenv("BB_CONFIG", path, 1);
        setenv("BB_LANGUAGE", "4", 1);
        BbSettings::Load();
        assert(BbSettings::GameLanguage() == MenuLanguage::De);
        assert(s.menu_language_auto && s.menu_language == MenuLanguage::De && "the game's language");
        assert(std::string(BbSettings::MenuText("Blood Echoes", "Отголоски крови")) == "Blutechos");
        BbSettings::SetMenuLanguage(MenuLanguage::Pl);
        assert(!s.menu_language_auto && s.menu_language == MenuLanguage::Pl);
        BbSettings::Save();
        assert(Read(path).find("overlay_language=pl\n") != std::string::npos);
        BbSettings::Load();
        assert(s.menu_language == MenuLanguage::Pl && "the choice wins over the game's");
        BbSettings::SetMenuLanguage(std::nullopt);
        assert(s.menu_language_auto && s.menu_language == MenuLanguage::De);
        BbSettings::Save();
        assert(Read(path).find("overlay_language=auto\n") != std::string::npos);
        unsetenv("BB_LANGUAGE");
        BbSettings::Load();
        assert(s.menu_language == MenuLanguage::English && "no BB_LANGUAGE: the game runs in English");
        unlink(path);
    }
    std::puts("PASS: menu languages follow the game's");

    // Kaniel 2026-10-21: shadPS4's fast_shader_math (#5455) as an option, off by default. The
    // shader compiler reads the value the game started with: a change applies after a restart.
    {
        const BbSettings::Values fresh;
        assert(!fresh.fast_shader_math && !fresh.startup_fast_shader_math);
        char path[512];
        std::snprintf(path, sizeof(path), "%s/bbport-settings-test-XXXXXX", temp && temp[0] ? temp : "/tmp");
        const int file = mkstemp(path);
        const char text[] = "fast_shader_math=1\n";
        assert(write(file, text, sizeof(text) - 1) == ssize_t(sizeof(text) - 1));
        close(file);
        setenv("BB_CONFIG", path, 1);
        BbSettings::Load();
        assert(s.fast_shader_math && s.startup_fast_shader_math);
        s.fast_shader_math = false;
        BbSettings::Save();
        assert(Read(path).find("fast_shader_math=0\n") != std::string::npos);
        assert(s.startup_fast_shader_math && "the running game keeps its shaders");
        BbSettings::Load();
        assert(!s.fast_shader_math && !s.startup_fast_shader_math);
        unlink(path);
    }
    std::puts("PASS: fast shader math setting");
}
