// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: the map screen's area pictures and names (bbport_map_charts.h).
#include "bbport_map_charts.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "../bbcoop.h"
#include "bbport_game_art.h"
#include "bbport_save_editor.h"
#include "bbport_settings.h"

namespace BbMapCharts {

namespace {

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

std::vector<std::uint8_t> ReadGameFile(const std::string& relative) {
#ifdef _WIN32
    // The drawing threads (the worker's and ChartArea's) all read through here: each yields to
    // the game's own threads.
    thread_local bool lowered = false;
    if (!lowered) {
        lowered = true;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    }
#endif
    char path[1024];
    if (!bbcoop_game_file(("dvdroot_ps4/" + relative).c_str(), path, sizeof(path))) {
        return {};
    }
    return ReadFile(std::filesystem::u8path(path));
}

/// Bytes [offset, offset + size) of a file of the game's (a texture archive's piece).
std::vector<std::uint8_t> ReadGameRange(const std::string& relative, std::uint64_t offset, std::uint64_t size) {
    char path[1024];
    if (!bbcoop_game_file(("dvdroot_ps4/" + relative).c_str(), path, sizeof(path))) {
        return {};
    }
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    std::vector<std::uint8_t> bytes(size);
    file.seekg(std::streamoff(offset));
    file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size));
    bytes.resize(std::size_t(std::max<std::streamsize>(0, file.gcount())));
    return bytes;
}

/// The pictures' folder: the saves folder's map/ (BB_GPU_USER_DIR, as run.py and the launcher's
/// prepare step name it), else user/map/.
std::filesystem::path MapFolder() {
    const char* user = std::getenv("BB_GPU_USER_DIR");
    return (user != nullptr && *user != '\0' ? std::filesystem::u8path(user) : std::filesystem::path("user")) / "map";
}

std::filesystem::path CachePath(std::uint32_t key) {
    const int view = int(key & 7u);
    const std::string suffix = view > 0 ? "_v" + std::to_string(view) : "";
    return MapFolder() / (BbMapArt::MapNameOf(key & ~7u) + suffix + ".bbmap");
}

/// An area's picture (`view` 0: from above; 1: the 3D view's side) as the map draws it, from the
/// files `read` gives.
std::optional<BbMapArt::Chart> DrawArea(std::uint32_t packed_map, int view, const BbMapArt::ReadFile& read,
                                        const BbMapArt::ReadRange& read_range) {
    const int threads = int(std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u));
    // Kaniel 2026-10-18 ("sharper close-up"), 2026-10-19 ("it generally should be sharper"): 6
    // pixels a metre, in the game's own textures ("it looks much better this way"). The 3D view's
    // side ("can we prerender it?", "increase the quality of the render") drawn at up to 10 pixels
    // a metre and halved (smooth edges), on two threads (its canvas is large).
    auto drawn = view > 0 ? BbMapArt::ChartArea(packed_map, read, 10.0f, 6144, std::min(threads, 2), read_range, true, view)
                          : BbMapArt::ChartArea(packed_map, read, 6.0f, 6144, threads, read_range, true);
    if (drawn && view > 0) {
        drawn = BbMapArt::Halved(*drawn);
    }
    return drawn;
}

std::mutex mutex;
std::condition_variable wake;
std::map<std::uint32_t, std::shared_ptr<const BbMapArt::Chart>> ready;
std::map<std::uint32_t, std::shared_ptr<const BbMapArt::AreaMesh>> ready_meshes;
constexpr std::uint32_t MeshKey = 7; // `map | MeshKey` in the queue: the mesh, not a picture
constexpr std::uint32_t RayMeshKey = 6; // the ray tracing's finer mesh (GetRayMesh)
/// Kaniel 2026-10-21 (ray-traced lamp shadows): the 3D map's 0.5 m grid made the floor shadow
/// itself; the rays get the models on a 0.12 m grid. Kaniel 2026-10-22 ("fully ray trace... all
/// shadows reflections and so on should work"): at 0.12 m fences, railings and bars folded away;
/// now 0.03 m (kept as .rtmesh3, a new file beside the old grid's).
constexpr float RayMeshCell = 0.03f;
/// Kaniel 2026-10-22: a larger triangle's texture as a patch of colours 0.25 m apart, up to 8 x 8
/// (.rtmesh5; 0.1 m and 16 x 16 overflowed 2^27 colours).
constexpr float RayPatchCell = 0.25f;
std::map<std::uint32_t, std::shared_ptr<const BbMapArt::AreaMesh>> ready_ray_meshes;
std::set<std::uint32_t> failed, asked;
std::deque<std::uint32_t> queue;
bool worker_started = false;

std::shared_ptr<const BbMapArt::Chart> Make(std::uint32_t key) {
    const std::uint32_t packed_map = key & ~7u;
    const int view = int(key & 7u);
    const auto cache = CachePath(key);
    if (auto kept = BbMapArt::Load(ReadFile(cache))) {
        std::printf("Map: read %s%s from %s\n", BbMapArt::MapNameOf(packed_map).c_str(),
                    view > 0 ? (" view " + std::to_string(view)).c_str() : "", cache.string().c_str());
        return std::make_shared<const BbMapArt::Chart>(std::move(*kept));
    }
    const auto start = std::chrono::steady_clock::now();
    auto drawn = DrawArea(packed_map, view, ReadGameFile, ReadGameRange);
    if (!drawn) {
        std::printf("Map: no picture of %s in the game's files\n", BbMapArt::MapNameOf(packed_map).c_str());
        return nullptr;
    }
    std::printf("Map: drew %s%s (%dx%d) in %.1f s\n", BbMapArt::MapNameOf(packed_map).c_str(),
                view > 0 ? (" view " + std::to_string(view)).c_str() : "", drawn->width, drawn->height,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    std::error_code error;
    std::filesystem::create_directories(cache.parent_path(), error);
    const auto file = BbMapArt::Save(*drawn);
    std::ofstream(cache, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
    return std::make_shared<const BbMapArt::Chart>(std::move(*drawn));
}

std::shared_ptr<const BbMapArt::AreaMesh> MakeMesh(std::uint32_t packed_map, float cell = 0.5f,
                                                  const char* suffix = ".bbmesh", float patch_cell = 0.0f) {
    const auto cache = MapFolder() / (BbMapArt::MapNameOf(packed_map) + suffix);
    if (auto kept = BbMapArt::LoadMesh(ReadFile(cache))) {
        std::printf("Map: read %s's mesh\n", BbMapArt::MapNameOf(packed_map).c_str());
        return std::make_shared<const BbMapArt::AreaMesh>(std::move(*kept));
    }
    const auto start = std::chrono::steady_clock::now();
    const int threads = int(std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u));
    auto made = BbMapArt::MeshOf(packed_map, ReadGameFile, threads, ReadGameRange, cell, patch_cell);
    if (!made) {
        return nullptr;
    }
    std::printf("Map: made %s's mesh (%zu triangles) in %.1f s\n", BbMapArt::MapNameOf(packed_map).c_str(),
                made->indices.size() / 3,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    std::error_code error;
    std::filesystem::create_directories(cache.parent_path(), error);
    const auto file = BbMapArt::SaveMesh(*made);
    std::ofstream(cache, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
    return std::make_shared<const BbMapArt::AreaMesh>(std::move(*made));
}

void Work() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;) {
        std::uint32_t next = 0;
        {
            std::unique_lock lock{mutex};
            wake.wait(lock, [] { return !queue.empty(); });
            next = queue.front();
            queue.pop_front();
        }
        if ((next & 7u) == RayMeshKey) {
            auto mesh = MakeMesh(next & ~7u, RayMeshCell, ".rtmesh5", RayPatchCell);
            std::scoped_lock lock{mutex};
            if (mesh) {
                ready_ray_meshes[next & ~7u] = std::move(mesh);
            } else {
                failed.insert(next);
            }
            continue;
        }
        if ((next & 7u) == MeshKey) {
            auto mesh = MakeMesh(next & ~7u);
            std::scoped_lock lock{mutex};
            if (mesh) {
                ready_meshes[next & ~7u] = std::move(mesh);
            } else {
                failed.insert(next);
            }
            continue;
        }
        auto chart = Make(next);
        std::scoped_lock lock{mutex};
        if (chart) {
            ready[next] = std::move(chart);
        } else {
            failed.insert(next);
        }
    }
}

/// The game's text folder (msg/<folder>) for the menus' language.
const char* TextFolder() {
    static const std::map<std::string, const char*> folders{
        {"en", "engus"},  {"ru", "rusru"},  {"ja", "jpnjp"},  {"fr", "frafr"},  {"es", "spaes"},
        {"es_419", "spaar"}, {"de", "deude"}, {"it", "itait"}, {"nl", "nldnl"}, {"pt", "porpt"},
        {"ko", "korkr"},  {"zh_TW", "zhotw"}, {"zh_CN", "zhocn"}, {"fi", "finfi"}, {"sv", "swese"},
        {"da", "dandk"},  {"no", "norno"},  {"pl", "polpl"},  {"pt_BR", "porbr"}, {"tr", "turtr"}};
    const auto found = folders.find(BbSettings::LanguageCode(BbSettings::Get().menu_language.load()));
    return found != folders.end() ? found->second : "engus";
}

/// The game's text of one FMG, in the menus' language (loaded once each).
const std::map<std::uint32_t, std::string>& Text(const char* bnd, std::int32_t fmg) {
    static std::mutex text_mutex;
    static std::map<std::string, std::map<std::uint32_t, std::string>> loaded;
    const std::string folder = TextFolder();
    const std::string key = folder + "/" + bnd + "#" + std::to_string(fmg);
    std::scoped_lock lock{text_mutex};
    auto [it, fresh] = loaded.try_emplace(key);
    if (fresh) {
        auto file = ReadGameFile("msg/" + folder + "/" + bnd);
        if (file.empty()) {
            file = ReadGameFile(std::string("msg/engus/") + bnd);
        }
        if (const auto inflated = BbGameArt::InflateDcx(file)) {
            it->second = BbSaveEditor::GameText(*inflated, fmg);
        }
    }
    return it->second;
}

std::string Named(const char* bnd, std::int32_t fmg, std::int32_t id) {
    const auto& text = Text(bnd, fmg);
    const auto found = text.find(std::uint32_t(id));
    if (found == text.end()) {
        return {};
    }
    // The game pads some names with spaces.
    std::string name = found->second;
    while (!name.empty() && (name.back() == ' ' || name.front() == ' ')) {
        name.erase(name.back() == ' ' ? name.size() - 1 : 0, 1);
    }
    return name;
}

} // namespace

std::shared_ptr<const BbMapArt::Chart> Get(std::uint32_t packed_map) {
    std::scoped_lock lock{mutex};
    if (const auto found = ready.find(packed_map); found != ready.end()) {
        return found->second;
    }
    if (packed_map != 0 && !failed.contains(packed_map) && asked.insert(packed_map).second) {
        queue.push_back(packed_map);
        if (!worker_started) {
            worker_started = true;
            std::thread(Work).detach();
        }
        wake.notify_one();
    }
    return nullptr;
}

std::shared_ptr<const BbMapArt::AreaMesh> GetRayMesh(std::uint32_t packed_map) {
    std::scoped_lock lock{mutex};
    if (const auto found = ready_ray_meshes.find(packed_map); found != ready_ray_meshes.end()) {
        return found->second;
    }
    const std::uint32_t key = packed_map | RayMeshKey;
    if (packed_map != 0 && !failed.contains(key) && asked.insert(key).second) {
        queue.push_back(key);
        if (!worker_started) {
            worker_started = true;
            std::thread(Work).detach();
        }
        wake.notify_one();
    }
    return nullptr;
}

std::shared_ptr<const BbMapArt::AreaMesh> GetMesh(std::uint32_t packed_map) {
    std::scoped_lock lock{mutex};
    if (const auto found = ready_meshes.find(packed_map); found != ready_meshes.end()) {
        return found->second;
    }
    const std::uint32_t key = packed_map | MeshKey;
    if (packed_map != 0 && !failed.contains(key) && asked.insert(key).second) {
        queue.push_back(key);
        if (!worker_started) {
            worker_started = true;
            std::thread(Work).detach();
        }
        wake.notify_one();
    }
    return nullptr;
}

bool Failed(std::uint32_t packed_map) {
    std::scoped_lock lock{mutex};
    return failed.contains(packed_map);
}

std::string LampName(std::int32_t name_id) {
    return Named("menu.msgbnd.dcx", 30, name_id);
}

std::string AreaName(std::int32_t name_id) {
    return Named("item.msgbnd.dcx", 19, name_id);
}

// Kaniel 2026-10-19 ("prerender should happen during setup/in launcher on visible place"): every
// area's map picture and 3D view drawn ahead into the saves folder (bb-probe --prepare-map, which
// the launcher runs with a progress bar), skipping what is drawn already. Prints a line an area
// ("Map prepare: 3/24 m24_01_00_00"); 0 when done, 1 when the game's files are not there.
extern "C" int bbport_prepare_map(const char* dvdroot, const char* user_dir) {
    const std::filesystem::path root = std::filesystem::u8path(dvdroot);
    std::vector<std::uint32_t> areas;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(root / "map" / "MapStudio", error)) {
        // mAA_BB_00_00.msb.dcx: one area; the others (_00_01, …) are its variants.
        const std::string name = entry.path().filename().string();
        unsigned aa = 0, bb = 0;
        if (name.size() == 20 && name.ends_with("_00_00.msb.dcx") && std::sscanf(name.c_str(), "m%2u_%2u", &aa, &bb) == 2) {
            areas.push_back(aa << 24 | bb << 16);
        }
    }
    if (areas.empty()) {
        std::printf("Map prepare: no map files in %s\n", dvdroot);
        return 1;
    }
    std::sort(areas.begin(), areas.end());
#ifdef _WIN32
    _putenv_s("BB_GPU_USER_DIR", user_dir);
#else
    setenv("BB_GPU_USER_DIR", user_dir, 1);
#endif
    const BbMapArt::ReadFile read = [&](const std::string& relative) { return ReadFile(root / relative); };
    const BbMapArt::ReadRange read_range = [&](const std::string& relative, std::uint64_t offset, std::uint64_t size) {
        std::ifstream file(root / relative, std::ios::binary);
        std::vector<std::uint8_t> bytes(size);
        file.seekg(std::streamoff(offset));
        file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size));
        bytes.resize(std::size_t(std::max<std::streamsize>(0, file.gcount())));
        return bytes;
    };
    std::filesystem::create_directories(MapFolder(), error);
    int done = 0;
    for (const std::uint32_t area : areas) {
        ++done;
        std::printf("Map prepare: %d/%zu %s\n", done, areas.size(), BbMapArt::MapNameOf(area).c_str());
        for (const int view : {0, 1}) {
            const auto cache = CachePath(area | std::uint32_t(view));
            if (BbMapArt::Load(ReadFile(cache))) {
                continue; // drawn before
            }
            if (const auto drawn = DrawArea(area, view, read, read_range)) {
                const auto file = BbMapArt::Save(*drawn);
                std::ofstream(cache, std::ios::binary)
                    .write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
            }
        }
    }
    std::printf("Map prepare: done\n");
    return 0;
}

} // namespace BbMapCharts
