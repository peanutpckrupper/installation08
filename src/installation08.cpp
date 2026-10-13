// mcchost: a minimal stand-in for MCC's game host, to run one Halo game DLL from
// The Master Chief Collection on its own (offline, single player).
//
// Stage 1 (this file) is a probe. It loads <game>\<game>.dll, starts the engine on
// a window and Direct3D 11 device of its own, and logs every call the game makes
// back into the host, so we can see which host functions each game needs.
//
// The host interface (the game engine's methods, the 121 host callbacks, the game
// options layout) follows the community's research published in libmcc
// (github.com/SpringContingency/libmcc). This file is our own code.
//
// Usage:  installation08.exe --game haloreach [--map 179 --map-file m10] [--difficulty normal]
//                     [--mcc "C:\...\Halo The Master Chief Collection"] [--tick 60]
//                     [--legacy-map-id N] [--insertion N] [--width 1920 --height 1080]
// The log is installation08.log next to installation08.exe.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dxgi.h>
#include <xinput.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <atomic>
#include <mutex>
#include <set>
#include <functional>
#include <algorithm>
#include <map>
#include <deque>
#include <vector>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>

typedef uint64_t XUID;

// ---------------------------------------------------------------- logging
static CRITICAL_SECTION g_log_lock;
static FILE *g_log;
static LARGE_INTEGER g_t0, g_freq;

// Lines go into a memory buffer that a background thread writes out twice a second
// (writing every line straight to disk, and to the console, slowed the game to a
// crawl under Wine, where the MCC folder is on a slow shared drive). A crash
// flushes the buffer at once.
static std::string g_log_buffer;

static void log_flush()
{
    std::string out;
    EnterCriticalSection(&g_log_lock);
    out.swap(g_log_buffer);
    LeaveCriticalSection(&g_log_lock);
    if (g_log && !out.empty())
    {
        fwrite(out.data(), 1, out.size(), g_log);
        fflush(g_log);
    }
}

static DWORD WINAPI log_writer_thread(LPVOID)
{
    for (;;) { Sleep(500); log_flush(); }
}

static void logf(const char *fmt, ...)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = double(now.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart);
    char line[2048];
    int n = snprintf(line, sizeof line, "%9.3f [%04lx] ", t, GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - n - 2, fmt, ap);
    va_end(ap);
    n += m < 0 ? 0 : std::min<int>(m, (int)sizeof line - n - 2);
    line[n++] = '\n';
    EnterCriticalSection(&g_log_lock);
    g_log_buffer.append(line, n);
    LeaveCriticalSection(&g_log_lock);
}

// A call is logged the first 20 times, then every 1000th time.
// Detailed logging (every host call, call chains) only with verbose_log = 1 in the
// .ini or --verbose 1; otherwise the log keeps the milestones and problems.
static bool g_verbose_log;
static bool should_log(std::atomic<uint32_t> &counter)
{
    if (!g_verbose_log) return false;
    uint32_t n = ++counter;
    return n <= 20 || n % 1000 == 0;
}

static std::string narrow(const wchar_t *w)
{
    if (!w) return "(null)";
    char buf[1024];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    buf[sizeof(buf) - 1] = 0;
    return buf;
}

static std::string module_offset(const void *address)
{
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (const char *)address, &module) && module)
    {
        GetModuleFileNameA(module, name, sizeof(name));
        const char *base = strrchr(name, '\\');
        char out[MAX_PATH + 32];
        snprintf(out, sizeof(out), "%s+0x%llx", base ? base + 1 : name,
                 (unsigned long long)((const char *)address - (const char *)module));
        return out;
    }
    char out[32];
    snprintf(out, sizeof(out), "%p", address);
    return out;
}

// ---------------------------------------------------------------- game interfaces
struct s_scenario_map_id
{
    int32_t builtin_map_id;
    uint16_t part1;
    uint16_t flags;
    uint64_t part2;
};
static_assert(sizeof(s_scenario_map_id) == 0x10, "map id");

struct s_game_player_options
{
    XUID xuid;
    uint64_t address;
    int32_t unknown_2, unknown_3, unknown_4, unknown_5;
};
static_assert(sizeof(s_game_player_options) == 32, "player options");

struct s_player_options
{
    uintptr_t address[17];
    int32_t player_count;
    s_game_player_options player_options[16];
    int32_t peer_count;
    int32_t unknown_2;
};
static_assert(sizeof(s_player_options) == 664, "players");

#pragma pack(push, 1)
struct s_game_options
{
    int16_t flags;
    char option[6];
    char network_option[2];
    char game_tick;
    char un_0;
    int32_t game_mode;                 // 1 campaign
    int32_t legacy_map_id;
    s_scenario_map_id map_id;
    s_scenario_map_id custom_campaign_map_id;
    int32_t difficulty;                // 0 easy .. 3 legendary
    int32_t pad_0;
    int32_t campaign_insertion_point;
    int32_t pad_1, pad_2;
    uint64_t skulls;                   // offset 72
    int64_t un_2;
    int64_t host_address;
    s_player_options player_options;
    int64_t un_3;
    char game_variant[117760];
    char un_4[1032];
    char map_variant[59392];
    char un_5[1032];
    int32_t un_6;
    int32_t padding_1;
    void *saved_game_state;
    const char *saved_film_path;
    int64_t un_7;
};
#pragma pack(pop)
static_assert(sizeof(s_game_options) == 0x2BF30, "game options");
static_assert(offsetof(s_game_options, skulls) == 72, "skulls offset");
static_assert(offsetof(s_game_options, map_id) == 20, "map id offset");
static_assert(offsetof(s_game_options, player_options) == 96, "players offset");
static_assert(offsetof(s_game_options, game_variant) == 768, "variant offset");
static_assert(offsetof(s_game_options, saved_game_state) == 0x2BF30 - 24, "saved game offset");

struct s_language_settings
{
    wchar_t audio[85];
    wchar_t text_1[85];
    wchar_t text_2[85];
};

struct i_game_engine
{
    virtual void free() = 0;
    virtual void initialize_graphics(ID3D11Device *, ID3D11DeviceContext *, IDXGISwapChain *, IDXGISwapChain *) = 0;
    virtual HANDLE initialize_game(void *game_manager, const s_game_options *) = 0;
    virtual void *post_message(int message, const void *parameter) = 0;
    virtual void preload_common_begin(ID3D11Device *) = 0;
    virtual void preload_level_begin(int map_id) = 0;
    virtual void unused_6() = 0;
    virtual void unused_7() = 0;
    virtual void unused_8() = 0;
    virtual void *post_command(const char *command) = 0; // "HS: <halo script>"
};
static i_game_engine *g_engine;
struct s_host_input_state;
namespace menu
{
static void on_end_frame(IDXGISwapChain *chain);
static bool filter_input(s_host_input_state *state);
static void loading_state(int state);
}

typedef uint64_t (*t_create_game_engine)(i_game_engine **);

// The game's own loader for game types and maps (libmcc's i_data_access), and what it
// returns (i_saved_game_file): each copies itself into s_game_options.
struct i_saved_game_file
{
    virtual void free_() = 0;
    virtual wchar_t *get_name() const = 0;
    virtual wchar_t *set_name(const wchar_t *) = 0;
    virtual wchar_t *get_description() const = 0;
    virtual wchar_t *set_description(const wchar_t *) = 0;
    virtual wchar_t *get_author(void *) const = 0;
    virtual wchar_t *set_author(const void *) = 0;
    virtual void copy_to_game_options(struct s_game_options *) const = 0;
};
struct i_data_access
{
    virtual void free_() = 0;
    virtual i_saved_game_file *map_variant_from_file(const char *buf, uint32_t len) = 0;
    virtual i_saved_game_file *map_variant_from_map_id(const s_scenario_map_id *id) = 0;
    virtual i_saved_game_file *map_variant_from_map_variant(const void *) = 0;
    virtual i_saved_game_file *game_variant_from_file(const char *buf, uint32_t len) = 0;
};
typedef uint64_t (*t_create_data_access)(i_data_access **);
typedef int (*t_set_library_settings)(const s_language_settings *);

// ---------------------------------------------------------------- host state
struct host_config
{
    std::wstring mcc_root, game, host_dir, map_file;
    bool scenario_dir = false;
    int preload = 0, watchdog = 0, auto_resume = 8, monitor = 0;
    float volume = 1.0f, mouse_sensitivity = 1.0f;
    int pad_mapping = 0;
    uint64_t address = 1;
    int flags = 8;
    int map = -1, legacy_map_id = -1, insertion = 0, difficulty = 1, tick = 60;
    int width = 1920, height = 1080;
    int window_x = CW_USEDEFAULT, window_y = CW_USEDEFAULT, window_max = 0;
    bool mcc_given = false;
    int borderless = -1;
};
static host_config g_cfg;
static int g_module = 6; // MCC module index: halo1 0, halo2 1, halo3 2, halo4 3, groundhog 4, odst 5, reach 6
static IDXGISwapChain *g_swapchain;

// Stand-in objects the game may ask the host for. Their vtables are all logging stubs.
struct stub_object { void **vtable; };
static stub_object g_event_manager;
alignas(16) static unsigned char g_player_profile[0x1000];
alignas(16) static unsigned char g_gamepad_mapping[0x100];

// ---------------------------------------------------------------- generic stubs
// Each table entry logs "<tag> #<index>" and returns 0. Specific host methods are
// replaced with real implementations below once their signatures are known.
enum { k_tag_host = 0, k_tag_event = 1 };
static const char *k_host_names[121] = {
    "begin_frame", "end_frame", "resize", "set_game_state", "restart_game", "save_game", "set_game_result",
    "pause_game", "_pause_game_", "set_game_objectives", "get_game_event_manager", "set_game_engine_variant",
    "set_scenario_map_variant", "_13", "set_player_look_control", "set_player_profile_game_specific",
    "get_map_info", "get_campaign_map_info", "get_multiplayer_map_info", "_19", "_20", "_21", "_22",
    "update_launch_timer", "_24", "_25", "_26", "_27", "_28", "_29", "_30", "_31", "get_video_setting",
    "get_audio_setting", "get_player_profile", "_35", "get_input_state", "get_input_state_gamepad",
    "update_input_time", "set_input_state", "_40", "network_sendto_unreliable", "network_sendto_reliable",
    "network_recvfrom", "network_send", "_45", "get_folder_path", "get_game_folder_path",
    "get_scenario_path_a", "get_scenario_path_w", "get_ugc_id", "get_game_setting", "validate_cache_file",
    "_53", "get_mcc_string", "use_custom_string_mapping", "insert_string", "get_string", "get_subtitle",
    "font_59", "font_60", "font_test_string", "font_precache_character", "font_get_texture", "font_test_char",
    "font_get_kerning_pair_offset", "font_set_66", "font_set_selection", "font_68", "_69", "_70",
    "get_player_skin", "draw_player_emblem", "get_player_emblem", "get_player_emblem_attribute",
    "get_player_weapon_offset", "_76", "_77", "_78", "_79", "_80", "_81", "_82", "_83", "_84", "_85", "_86",
    "_87", "get_player_xuid", "_89", "_90", "_91", "_92", "_93", "_94", "chud_95", "chud_96",
    "chud_blend_color", "_98", "_99", "_100", "_101", "_102", "_103", "_104", "_105", "_106", "_107", "_108",
    "_109", "_110", "_111", "_112", "_113", "_114", "_115", "get_player_gamepad_mapping", "_117", "_118",
    "_119", "_120"};

static std::atomic<uint32_t> g_counts[2][512];

template <int Tag, int Index>
static uint64_t generic_stub(void *self, uint64_t a, uint64_t b, uint64_t c)
{
    if (should_log(g_counts[Tag][Index]))
    {
        if (Tag == k_tag_host)
            logf("host.%s (#%d) a=%llx b=%llx c=%llx  from %s", Index < 121 ? k_host_names[Index] : "beyond_121",
                 Index, (unsigned long long)a, (unsigned long long)b, (unsigned long long)c,
                 module_offset(__builtin_return_address(0)).c_str());
        else
            logf("event_manager #%d a=%llx b=%llx  from %s", Index, (unsigned long long)a, (unsigned long long)b,
                 module_offset(__builtin_return_address(0)).c_str());
    }
    return 0;
}

template <int Tag, size_t... I>
static void fill_stubs(void **table, std::index_sequence<I...>)
{
    void *entries[] = {(void *)&generic_stub<Tag, (int)I>...};
    for (size_t i = 0; i < sizeof...(I); i++) table[i] = entries[i];
}

static void *g_host_vtable[512];
static void *g_event_vtable[512];
static stub_object g_host = {g_host_vtable};

#define LOG_CALL(index, ...) do { if (should_log(g_counts[k_tag_host][index])) logf(__VA_ARGS__); } while (0)

// ---------------------------------------------------------------- host methods
static void h_begin_frame(void *) { LOG_CALL(0, "host.begin_frame"); }

static void h_end_frame(void *, IDXGISwapChain *swapchain, UINT *flags)
{
    LOG_CALL(1, "host.end_frame swapchain=%p (ours %p) flags=%u", (void *)swapchain, (void *)g_swapchain,
             flags ? *flags : 0u);
    // The game presents the frame itself right after this call, with the flags the
    // host leaves here. (Presenting here too showed every other frame as black.)
    // The pause menu, when open, is drawn over the frame here.
    menu::on_end_frame(swapchain ? swapchain : g_swapchain);
}

static void commit_mission_map();
static std::atomic<int64_t> g_game_over_at; // when the game reported it has ended (state 5)
static void h_set_game_state(void *, int state)
{
    logf("host.set_game_state %d", state);
    if (state == 5) g_game_over_at = (int64_t)GetTickCount64();
    if (state == 0) commit_mission_map(); // a map starts loading
    menu::loading_state(state);
}

// Network: one machine, one player. Halo CE's multiplayer runs a server and a client
// that talk through these even when both are this machine, so a packet sent to our
// own address is handed back on the next receive (a loopback).
struct net_packet { std::vector<char> data; uint64_t from; uint32_t port; };
static std::mutex g_net_lock;
static std::deque<net_packet> g_net_queue;
static uint32_t net_send(uint64_t id, const char *buf, uint32_t len, uint32_t port)
{
    if (!buf || !len) return 0;
    if (g_module != 0) return len; // only Halo CE needs it; the others never get packets
    if (id != (uint64_t)g_cfg.address && id != 0) return len; // nobody else is out there
    std::lock_guard<std::mutex> l(g_net_lock);
    if (g_net_queue.size() > 4096) g_net_queue.pop_front();
    g_net_queue.push_back({std::vector<char>(buf, buf + len), (uint64_t)g_cfg.address, port});
    return len;
}
static uint32_t h_network_sendto_unreliable(void *, uint64_t id, const char *buf, uint32_t len, uint32_t port)
{
    LOG_CALL(41, "host.network_sendto_unreliable id=%llx len=%u port=%u", (unsigned long long)id, len, port);
    return net_send(id, buf, len, port);
}
static uint32_t h_network_sendto_reliable(void *, uint64_t id, const char *buf, uint32_t len, uint32_t port)
{
    LOG_CALL(42, "host.network_sendto_reliable id=%llx len=%u port=%u", (unsigned long long)id, len, port);
    return net_send(id, buf, len, port);
}
// recvfrom(buffer, size, sender out, port): the port is the one this side listens on
// (the server and the client are both here, on different ports).
static uint32_t h_network_recvfrom(void *, char *buf, uint32_t len, uint64_t *id, uint32_t port)
{
    std::lock_guard<std::mutex> l(g_net_lock);
    if (!buf) return 0;
    for (auto it = g_net_queue.begin(); it != g_net_queue.end(); ++it)
    {
        if (it->port != port) continue;
        uint32_t n = (uint32_t)std::min<size_t>(it->data.size(), len);
        memcpy(buf, it->data.data(), n);
        if (id) *id = it->from;
        g_net_queue.erase(it);
        LOG_CALL(43, "host.network_recvfrom port %u -> %u bytes (loopback)", port, n);
        return n;
    }
    return 0;
}
static void net_reset()
{
    std::lock_guard<std::mutex> l(g_net_lock);
    g_net_queue.clear();
}

static void h_restart_game(void *, int reason, const char *message)
{
    logf("host.restart_game reason=%d message=%s", reason, message ? message : "(null)");
}

// Saved games. The game hands the host its whole saved state (Reach: 10.9 MB) when a
// mission starts, at each checkpoint and when the game ends; MCC stores it and gives it back
// in the game options to continue. Stored as saves\<game>.sav, with a small
// <game>.sav.info beside it saying which mission and difficulty it belongs to.
static std::wstring save_path(const std::wstring &game) { return g_cfg.host_dir + L"\\saves\\" + game + L".sav"; }
static void on_game_saved();                 // defined with the menus
// "Quit without saving": saves made from then on are ignored, and the save the game
// had when this session started is put back.
static std::atomic<bool> g_discard_session;
static bool g_had_save_before;

static void keep_save_from_before()
{
    std::wstring sav = save_path(g_cfg.game);
    g_had_save_before = CopyFileW(sav.c_str(), (sav + L".before").c_str(), FALSE) &&
                        CopyFileW((sav + L".info").c_str(), (sav + L".info.before").c_str(), FALSE);
}

static void put_back_save_from_before()
{
    std::wstring sav = save_path(g_cfg.game);
    if (g_had_save_before)
    {
        CopyFileW((sav + L".before").c_str(), sav.c_str(), FALSE);
        CopyFileW((sav + L".info.before").c_str(), (sav + L".info").c_str(), FALSE);
        logf("this session's progress discarded; the earlier save is back");
    }
    else
    {
        DeleteFileW(sav.c_str());
        DeleteFileW((sav + L".info").c_str());
        logf("this session's progress discarded; there was no earlier save");
    }
}
static int current_map_for_save();
static std::string current_file_for_save();

static int g_mode_for_saves = 1;
static void h_save_game(void *, const char *buf, uint32_t len)
{
    if (g_mode_for_saves != 1) { logf("host.save_game: not a campaign, not kept (%u bytes)", len); return; }
    logf("host.save_game %u bytes", len);
    if (!buf || !len) return;
    if (g_discard_session) { logf("not kept (quitting without saving)"); return; }
    // Halo CE sometimes hands over a 1-byte "save" at a mission's start; keeping it
    // would replace a real saved game with nothing to continue from.
    if (len < 4096) { logf("not kept (too small to be a saved game)"); return; }
    CreateDirectoryW((g_cfg.host_dir + L"\\saves").c_str(), nullptr);
    std::wstring path = save_path(g_cfg.game), temp = path + L".new";
    bool ok = false;
    if (FILE *f = _wfopen(temp.c_str(), L"wb"))
    {
        ok = fwrite(buf, 1, len, f) == len;
        ok = (fclose(f) == 0) && ok;
    }
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        logf("could not write the saved game");
        return;
    }
    if (FILE *f = _wfopen((path + L".info").c_str(), L"w"))
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "map = %d\nfile = %s\ndifficulty = %d\nsaved = %04d-%02d-%02d %02d:%02d\n", current_map_for_save(),
                current_file_for_save().c_str(), g_cfg.difficulty, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
        fclose(f);
    }
    logf("saved game written (%s)", current_file_for_save().c_str());
    on_game_saved();
}


static void *h_get_game_event_manager(void *)
{
    LOG_CALL(10, "host.get_game_event_manager -> stub object");
    return &g_event_manager;
}

// The campaign's description: MCC hands the game the contents of the campaign file
// (<game>\maps\info\*.campaign, named halo3.campaign even in Reach), which lists the
// missions and their maps. Without it the game has no map name and crashes.
static std::string g_campaign_info;

static std::string read_file(const std::wstring &path)
{
    std::string out;
    if (FILE *f = _wfopen(path.c_str(), L"rb"))
    {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        fclose(f);
    }
    return out;
}

static void load_campaign_info()
{
    std::wstring dir = g_cfg.mcc_root + L"\\" + g_cfg.game + L"\\maps\\info\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.campaign").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { logf("no .campaign file in %s", narrow(dir.c_str()).c_str()); return; }
    std::wstring name = fd.cFileName;
    FindClose(h);
    g_campaign_info = read_file(dir + name);
    logf("campaign info: %s, %zu bytes", narrow(name.c_str()).c_str(), g_campaign_info.size());
}

static bool h_get_campaign_map_info(void *, const s_scenario_map_id *id, const char **info, uint32_t *size)
{
    logf("host.get_campaign_map_info map=%d part1=%u flags=%x part2=%llx -> %zu bytes  from %s",
         id ? id->builtin_map_id : -99, id ? id->part1 : 0, id ? id->flags : 0,
         id ? (unsigned long long)id->part2 : 0ull, g_campaign_info.size(),
         module_offset(__builtin_return_address(0)).c_str());
    if (g_campaign_info.empty())
    {
        if (info) *info = nullptr;
        if (size) *size = 0;
        return false;
    }
    if (info) *info = g_campaign_info.data();
    if (size) *size = (uint32_t)g_campaign_info.size();
    return true;
}

static bool h_get_multiplayer_map_info(void *, const s_scenario_map_id *id, const char **info, uint32_t *size)
{
    logf("host.get_multiplayer_map_info map=%d", id ? id->builtin_map_id : -99);
    if (info) *info = nullptr;
    if (size) *size = 0;
    return false;
}

struct s_general_video_settings
{
    int32_t width, height;
    char rest[16];
};

static bool h_get_video_setting(void *, unsigned char *settings)
{
    LOG_CALL(32, "host.get_video_setting");
    memset(settings, 0, 368);
    auto *general = reinterpret_cast<s_general_video_settings *>(settings);
    general->width = g_cfg.width;
    general->height = g_cfg.height;
    return true;
}

static uint64_t h_get_audio_setting(void *, unsigned char *settings)
{
    LOG_CALL(33, "host.get_audio_setting");
    memset(settings, 0, 788);
    float one = 1.0f;
    memcpy(settings + 4, &one, 4); // voice chat volume
    return 1;
}

static void *h_get_player_profile(void *, XUID xuid)
{
    LOG_CALL(34, "host.get_player_profile xuid=%llx", (unsigned long long)xuid);
    return g_player_profile;
}

// ---------------------------------------------------------------- input
// Player 1 plays with an Xbox-style controller (XInput, first one connected) or with
// the keyboard and mouse. The first int of the input state is "keyboard and mouse":
// the game fills in what it last saw and takes back whatever the host returns, so the
// host decides. Whichever device was used last wins, as in MCC.
static int g_input_connected = 1;
static HWND g_hwnd;

// Keys and mouse buttons held down in our window, from its own window messages (the
// way games normally read the keyboard), indexed by virtual-key code.
static std::atomic<uint8_t> g_keys[256];
static bool key_down(int vk) { return vk > 0 && vk < 256 && g_keys[vk].load(std::memory_order_relaxed) != 0; }
static void clear_keys() { for (auto &k : g_keys) k = 0; }
typedef DWORD(WINAPI *t_xinput_get_state)(DWORD, XINPUT_STATE *);
typedef DWORD(WINAPI *t_xinput_set_state)(DWORD, XINPUT_VIBRATION *);
static t_xinput_get_state g_xinput_get_state;
static t_xinput_set_state g_xinput_set_state;
static std::atomic<int> g_mouse_dx, g_mouse_dy, g_mouse_wheel;
static std::atomic<bool> g_mouse_captured;
static int g_last_is_km = 0;
static WORD g_last_pad_buttons;

static void load_xinput()
{
    const wchar_t *names[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (auto n : names)
    {
        HMODULE x = LoadLibraryW(n);
        if (!x) continue;
        g_xinput_get_state = (t_xinput_get_state)GetProcAddress(x, "XInputGetState");
        g_xinput_set_state = (t_xinput_set_state)GetProcAddress(x, "XInputSetState");
        logf("input: %s loaded", narrow(n).c_str());
        return;
    }
    logf("input: no XInput DLL, controllers unavailable");
}

struct s_host_gamepad_state
{
    int32_t buttons; // XInput wButtons (the game copies it into an XINPUT_GAMEPAD)
    uint8_t left_trigger, right_trigger;
    int16_t thumb_lx, thumb_ly, thumb_rx, thumb_ry;
};
struct s_host_mouse_state
{
    float dx, dy, x, y, wheel, wheel_scale;
    int32_t buttons; // left, right, middle, 4, 5
};
struct s_host_input_state
{
    int32_t is_km;
    uint8_t key_down[256];
    s_host_mouse_state mouse;
    s_host_gamepad_state gamepad;
};
static_assert(sizeof(s_host_input_state) == 0x130, "input state size");
static_assert(offsetof(s_host_input_state, gamepad) == 0x120, "gamepad offset");

static bool window_active() { return g_hwnd && GetForegroundWindow() == g_hwnd; }

static XINPUT_GAMEPAD remap_pad(const XINPUT_GAMEPAD &in);

// Local players (split-screen): how many, and who holds what. Player 1 has the
// keyboard and mouse; controllers go to players in order, or, with "keyboard as a
// player", player 1 is keyboard only and controller 1 goes to player 2.
static std::atomic<int> g_local_players{1};
static std::atomic<bool> g_keyboard_player{false};
static int pad_for_player(int player)
{
    if (player < 0 || player >= g_local_players) return -1;
    if (g_keyboard_player) return player == 0 ? -1 : player - 1;
    return player;
}
namespace menu { static bool filter_input(struct s_host_input_state *state); static bool menu_blocking(); }

static bool h_get_input_state(void *, int player, unsigned char *raw)
{
    LOG_CALL(36, "host.get_input_state player=%d", player);
    if (!raw) return false;
    auto *state = reinterpret_cast<s_host_input_state *>(raw);
    memset(raw + 4, 0, 0x130 - 4);
    if (!g_input_connected || player < 0 || player >= g_local_players) return false;
    int pad = pad_for_player(player);

    if (player > 0) // a split-screen player: a controller only
    {
        XINPUT_STATE xs = {};
        if (pad >= 0 && g_xinput_get_state && g_xinput_get_state(pad, &xs) == ERROR_SUCCESS)
        {
            const XINPUT_GAMEPAD p = remap_pad(xs.Gamepad);
            state->gamepad.buttons = p.wButtons;
            state->gamepad.left_trigger = p.bLeftTrigger;
            state->gamepad.right_trigger = p.bRightTrigger;
            state->gamepad.thumb_lx = p.sThumbLX;
            state->gamepad.thumb_ly = p.sThumbLY;
            state->gamepad.thumb_rx = p.sThumbRX;
            state->gamepad.thumb_ry = p.sThumbRY;
        }
        state->is_km = 0;
        if (menu::menu_blocking()) memset(raw + 4, 0, 0x130 - 4);
        return true;
    }

    bool pad_active = false, km_active = false;
    XINPUT_STATE xs = {};
    if (pad >= 0 && g_xinput_get_state && g_xinput_get_state(pad, &xs) == ERROR_SUCCESS)
    {
        const XINPUT_GAMEPAD &raw_pad = xs.Gamepad;
        const XINPUT_GAMEPAD p = remap_pad(raw_pad); // this game's button layout
        state->gamepad.buttons = p.wButtons; // the game reads XInput's own button bits
        state->gamepad.left_trigger = p.bLeftTrigger;
        state->gamepad.right_trigger = p.bRightTrigger;
        state->gamepad.thumb_lx = p.sThumbLX;
        state->gamepad.thumb_ly = p.sThumbLY;
        state->gamepad.thumb_rx = p.sThumbRX;
        state->gamepad.thumb_ry = p.sThumbRY;
        auto moved = [](int v) { return v > 12000 || v < -12000; };
        pad_active = raw_pad.wButtons != g_last_pad_buttons || p.bLeftTrigger > 60 || p.bRightTrigger > 60 ||
                     moved(p.sThumbLX) || moved(p.sThumbLY) || moved(p.sThumbRX) || moved(p.sThumbRY);
        g_last_pad_buttons = raw_pad.wButtons;
    }

    if (window_active())
    {
        for (int vk = 1; vk < 256; vk++)
            if (key_down(vk)) state->key_down[vk] = 1, km_active = true;
        int dx = g_mouse_dx.exchange(0), dy = g_mouse_dy.exchange(0), wheel = g_mouse_wheel.exchange(0);
        state->mouse.dx = dx * g_cfg.mouse_sensitivity;
        state->mouse.dy = dy * g_cfg.mouse_sensitivity;
        state->mouse.wheel = wheel / (float)WHEEL_DELTA;
        state->mouse.wheel_scale = 1.0f;
        POINT pt;
        if (GetCursorPos(&pt) && ScreenToClient(g_hwnd, &pt)) state->mouse.x = (float)pt.x, state->mouse.y = (float)pt.y;
        int mb = 0;
        if (state->key_down[VK_LBUTTON]) mb |= 1;
        if (state->key_down[VK_RBUTTON]) mb |= 2;
        if (state->key_down[VK_MBUTTON]) mb |= 4;
        if (state->key_down[VK_XBUTTON1]) mb |= 8;
        if (state->key_down[VK_XBUTTON2]) mb |= 16;
        state->mouse.buttons = mb;
        if (dx || dy || wheel) km_active = true;
    }
    else
    {
        g_mouse_dx = 0, g_mouse_dy = 0, g_mouse_wheel = 0;
    }

    if (pad_active) g_last_is_km = 0;
    else if (km_active) g_last_is_km = 1;
    state->is_km = g_last_is_km;
    menu::filter_input(state);
    return true;
}

static bool h_get_input_state_gamepad(void *, int player, unsigned char *state)
{
    LOG_CALL(37, "host.get_input_state_gamepad player=%d", player);
    if (state) memset(state, 0, 0x130);
    return false;
}

// Rumble for a player's controller.
static void h_set_input_state(void *, int player, const int16_t *rumble)
{
    LOG_CALL(39, "host.set_input_state player=%d", player);
    int pad = pad_for_player(player);
    if (pad < 0 || !rumble || !g_xinput_set_state) return;
    XINPUT_VIBRATION v = {(WORD)rumble[0], (WORD)rumble[1]};
    g_xinput_set_state(pad, &v);
}

// The player profile MCC keeps per user (libmcc's s_player_profile, 0xACC bytes).
// A zeroed one means zero volume (no sound) and an empty controller layout, so
// fill in MCC-like defaults.
template <typename T> static void put(unsigned char *base, size_t offset, T v) { memcpy(base + offset, &v, sizeof v); }

enum
{   // abstract game actions (libmcc e_game_abstract_button)
    a_jump, a_switchgrenade, a_actionreload, a_reload, a_switchweapon, a_melee, a_flashlight, a_throwgrenade,
    a_fire, a_crouch, a_zoom, a_zoomin, a_zoomout, a_swapweapon, a_sprint, a_bansheebomb, a_forward, a_back,
    a_left, a_right, a_showscores, a_vehicletrick1, a_vehicletrick2, a_equipment, a_secondaryfire,
    k_action_count = 66
};
enum
{   // abstract controller buttons (libmcc e_abstract_gamepad_button)
    g_lt, g_rt, g_up, g_down, g_left, g_right, g_start, g_select, g_ls, g_rs, g_lb, g_rb, g_a, g_b, g_x, g_y,
    g_none = 0xFF
};

static void init_profile_and_mappings()
{
    unsigned char *p = g_player_profile;
    memset(p, 0, sizeof(g_player_profile));
    put<float>(p, 0x0C, 0.5f);  // subtitle background opacity
    put<float>(p, 0x10, 0.5f);  // subtitle shadow opacity
    put<int32_t>(p, 0x14, 78);  // field of view
    put<int32_t>(p, 0x18, 78);  // vehicle field of view
    p[0x1B5] = 3;                // vertical look sensitivity
    p[0x1B6] = 3;                // horizontal look sensitivity
    put<float>(p, 0x1B8, 0.08f); // look axial dead zone
    put<float>(p, 0x1BC, 0.08f); // look radial dead zone
    put<float>(p, 0x1C0, 1.0f);  // zoom look sensitivity multiplier
    put<float>(p, 0x1C4, 1.0f);  // vehicle look sensitivity multiplier
    put<float>(p, 0x410, 1.0f);  // mouse sensitivity
    // Keyboard and mouse bindings: 66 entries of {action, up to 5 virtual keys}.
    struct binding { int action; int keys[5]; };
    static const binding kbm[] = {
        {a_jump, {VK_SPACE}},          {a_switchgrenade, {'G'}},     {a_actionreload, {'E'}},
        {a_reload, {'R'}},             {a_switchweapon, {'1', VK_TAB}}, {a_melee, {'Q', 'V'}},
        {a_flashlight, {'4'}},         {a_throwgrenade, {'F'}},      {a_fire, {VK_LBUTTON}},
        {a_crouch, {VK_LCONTROL, 'C'}}, {a_zoom, {VK_RBUTTON}},       {a_swapweapon, {'X'}},
        {a_forward, {'W'}},            {a_back, {'S'}},              {a_left, {'A'}},
        {a_right, {'D'}},              {a_showscores, {VK_BACK}},    {a_equipment, {VK_LSHIFT}},
    };
    for (int i = 0; i < k_action_count; i++) put<int32_t>(p, 0x42C + i * 0x18, i);
    for (const auto &b : kbm)
        for (int k = 0; k < 5; k++) put<int32_t>(p, 0x42C + b.action * 0x18 + 4 + k * 4, b.keys[k]);
    put<float>(p, 0xA5C, g_cfg.volume); // master volume
    put<float>(p, 0xA60, g_cfg.volume); // music volume
    put<float>(p, 0xA64, g_cfg.volume); // effects volume
    put<float>(p, 0xAC8, 1.0f);         // HUD scale

    // Controller layout: one controller button per action.
    memset(g_gamepad_mapping, g_none, k_action_count);
    unsigned char *m = g_gamepad_mapping;
    m[a_jump] = g_a;          m[a_melee] = g_b;         m[a_actionreload] = g_x;  m[a_reload] = g_x;
    m[a_switchweapon] = g_y;  m[a_swapweapon] = g_y;    m[a_equipment] = g_lb;    m[a_switchgrenade] = g_rb;
    m[a_throwgrenade] = g_lt; m[a_fire] = g_rt;         m[a_crouch] = g_ls;       m[a_zoom] = g_rs;
    m[a_flashlight] = g_up;   m[a_showscores] = g_select;
    m[a_vehicletrick1] = g_a; m[a_vehicletrick2] = g_b; m[a_secondaryfire] = g_lt;
    m[49 /* dual wield */] = g_lb;
    logf("profile: volume %.2f, default controller and keyboard bindings", g_cfg.volume);
}

static float h_update_input_time(void *, int player)
{
    LOG_CALL(38, "host.update_input_time player=%d", player);
    return 0.0f;
}

static bool copy_path(wchar_t *buf, size_t len, const std::wstring &path)
{
    if (!buf || path.size() + 1 > len) return false;
    wcscpy(buf, path.c_str());
    return true;
}

static std::wstring folder_for(int folder, bool game)
{
    switch (folder)
    {
    case 0: return g_cfg.host_dir + L"\\logs\\";
    case 1: return g_cfg.host_dir + L"\\config\\";
    case 2: return g_cfg.host_dir + L"\\temp\\";
    case 3: return game ? g_cfg.mcc_root + L"\\" + g_cfg.game + L"\\" : g_cfg.mcc_root + L"\\";
    }
    return g_cfg.host_dir + L"\\";
}

static bool h_get_folder_path(void *, int folder, wchar_t *buf, size_t len)
{
    std::wstring path = folder_for(folder, false);
    CreateDirectoryW(path.c_str(), nullptr);
    bool ok = copy_path(buf, len, path);
    LOG_CALL(46, "host.get_folder_path %d -> %s", folder, narrow(path.c_str()).c_str());
    return ok;
}

static bool h_get_game_folder_path(void *, int folder, wchar_t *buf, size_t len)
{
    std::wstring path = folder_for(folder, true);
    CreateDirectoryW(path.c_str(), nullptr);
    bool ok = copy_path(buf, len, path);
    LOG_CALL(47, "host.get_game_folder_path %d -> %s", folder, narrow(path.c_str()).c_str());
    return ok;
}

static std::wstring maps_dir() { return g_cfg.mcc_root + L"\\" + g_cfg.game + L"\\maps\\"; }

// get_scenario_path is a redirect hook, not a question: the game fills the buffer with
// the path it is about to use (kind 1: "maps\m10.map" and the like, kinds 3-6: other
// paths) and the host may rewrite it, for example to point at a mod's copy. Returning
// false with the buffer untouched means "use your own path". (Answering every call with
// one map file made every map the game looked up resolve to the same file.)
static std::wstring scenario_path() { return g_cfg.scenario_dir ? maps_dir() : maps_dir() + g_cfg.map_file + L".map"; }
static int g_scenario_mode; // 0 pass through, 1 map file, 2 maps folder

static std::string backtrace_here()
{
    void *frames[24];
    USHORT n = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
    std::string out;
    for (USHORT i = 0; i < n; i++) out += (i ? " <- " : "") + module_offset(frames[i]);
    return out;
}

static void note_map_loaded(const std::string &path);
static bool h_get_scenario_path_a(void *, int kind, char *buf, size_t len)
{
    std::string before = (buf && len) ? std::string(buf, strnlen(buf, len)) : std::string("(none)");
    if (g_scenario_mode == 0)
    {
        // Logged once per path (the game asks for the same paths thousands of times a second
        // while it waits), with the call chain the first time.
        ++g_counts[k_tag_host][48];
        static std::mutex lock;
        static std::set<std::string> seen;
        bool first;
        {
            std::lock_guard<std::mutex> l(lock);
            first = seen.insert(std::to_string(kind) + before).second;
        }
        if (first && g_verbose_log)
            logf("host.get_scenario_path_a kind=%d \"%s\" (kept)\n      call chain: %s", kind, before.c_str(),
                 backtrace_here().c_str());
        if (kind == 1)
        {
            // A map the game knows by an older file name than the one MCC ships.
            static const char *const aliases[][2] = {{"ff60_ruins.map", "ff60_airview.map"}};
            for (const auto &a : aliases)
            {
                size_t at = before.size() >= strlen(a[0]) ? before.size() - strlen(a[0]) : std::string::npos;
                if (at == std::string::npos || _stricmp(before.c_str() + at, a[0])) continue;
                std::string renamed = before.substr(0, at) + a[1];
                if (buf && renamed.size() + 1 <= len)
                {
                    strcpy(buf, renamed.c_str());
                    logf("host.get_scenario_path_a: %s -> %s", before.c_str(), renamed.c_str());
                    note_map_loaded(renamed);
                    return true;
                }
            }
            note_map_loaded(before);
        }
        return false;
    }
    std::string path = narrow(scenario_path().c_str());
    LOG_CALL(48, "host.get_scenario_path_a kind=%d \"%s\" -> %s", kind, before.c_str(), path.c_str());
    if (!buf || path.size() + 1 > len) return false;
    strcpy(buf, path.c_str());
    return true;
}

static bool h_get_scenario_path_w(void *, int kind, wchar_t *buf, size_t len)
{
    std::string before = (buf && len) ? narrow(buf) : std::string("(none)");
    if (g_scenario_mode == 0)
    {
        LOG_CALL(49, "host.get_scenario_path_w kind=%d \"%s\" (kept)", kind, before.c_str());
        return false;
    }
    LOG_CALL(49, "host.get_scenario_path_w kind=%d \"%s\" -> %s", kind, before.c_str(), narrow(scenario_path().c_str()).c_str());
    return copy_path(buf, len, scenario_path());
}

static bool h_get_game_setting(void *, int setting, bool *v1, uint64_t *v2)
{
    LOG_CALL(51, "host.get_game_setting %d", setting);
    if (v1) *v1 = false;
    if (v2) *v2 = 0;
    // 6: the font package MCC ships, maps\fonts\font_package_icon*.bin.
    if (setting == 6) { if (v1) *v1 = true; return true; }
    return false;
}

static bool h_validate_cache_file(void *, int a)
{
    LOG_CALL(52, "host.validate_cache_file %d", a);
    return true;
}

// #53: look up a UI text by its key ("UI_SKIP_CINE"). Halo CE uses the result as a
// C string straight away (and replaces "{PARAM}" in it with the button), so this must
// never return null; a key we have no text for comes back as itself.
static const char *h_get_ui_text(void *, const char *name)
{
    if (!name) return "";
    if (!strcmp(name, "UI_SKIP_CINE")) return "{PARAM} Skip";
    return name;
}

static uint64_t h_get_mcc_string(void *, const char *name, wchar_t *buf, size_t len)
{
    LOG_CALL(54, "host.get_mcc_string %s", name ? name : "(null)");
    if (!buf || len < 2) return 0;
    len /= 2; // may be a byte count (see get_player_xuid); never write past half of it
    size_t i = 0;
    for (; name && name[i] && i + 1 < len; i++) buf[i] = (wchar_t)(unsigned char)name[i];
    buf[i] = 0;
    return 1;
}

static bool h_false_55(void *) { LOG_CALL(55, "host.use_custom_string_mapping -> false"); return false; }

static bool h_get_string(void *, int datum, int string_id, wchar_t *buf, size_t len)
{
    LOG_CALL(57, "host.get_string datum=%x id=%x", datum, string_id);
    if (buf && len) buf[0] = 0;
    return false;
}

static bool h_get_subtitle(void *, const char *sound, const char *prefix, int index, wchar_t *buf, size_t len)
{
    LOG_CALL(58, "host.get_subtitle %s %s %d", sound ? sound : "", prefix ? prefix : "", index);
    if (buf && len) buf[0] = 0;
    return false;
}

static bool h_get_player_xuid(void *, XUID *xuid, wchar_t *name, int size, int player)
{
    LOG_CALL(88, "host.get_player_xuid player=%d", player);
    if (player < 0 || player >= g_local_players) return false;
    if (xuid) *xuid = 0x0009000000000001ull + (uint64_t)player;
    // "size" is in bytes for Halo CE (a 32-byte buffer) and may be in characters for
    // the others, so take it as bytes: the name fits either way. (wcsncpy padded the
    // rest with zeros, 64 bytes into Halo CE's 32, and broke its stack check.)
    if (name && size >= 4)
    {
        size_t room = (size_t)size / sizeof(wchar_t);
        wchar_t label[16];
        swprintf(label, 16, player == 0 ? L"Player" : L"Player %d", player + 1);
        size_t n = std::min(wcslen(label), room - 1);
        memcpy(name, label, n * sizeof(wchar_t));
        name[n] = 0;
    }
    return true;
}

static void *h_get_player_gamepad_mapping(void *, int player)
{
    LOG_CALL(116, "host.get_player_gamepad_mapping player=%d", player);
    // No table: the game keeps its own default controller layout (Reach's "Default").
    // Reach (and the games after it) keep their own default layout when the host gives
    // none. Halo 2 binds nothing from the controller without one, and its Back button
    // (MCC's "show scores") is what switches classic/Anniversary graphics.
    return (g_cfg.pad_mapping || g_module == 1) ? g_gamepad_mapping : nullptr;
}

// Event manager #147 (GetGUID) returns a pointer to a GUID the game copies; the
// others report events (achievements, stats, presence) and return bool or nothing.
static GUID g_session_guid = {0x6d636368, 0x6f73, 0x7421, {0x8a, 0x1f, 0x52, 0x65, 0x61, 0x63, 0x68, 0x21}};
static GUID *e_get_guid(void *)
{
    if (should_log(g_counts[k_tag_event][147])) logf("event_manager.GetGUID -> session guid");
    return &g_session_guid;
}

// ---------------------------------------------------------------- text (MCC's font cache)
// With MCC's font package (game setting 6) the game doesn't draw text from its own
// font files: it asks the host for each character. The host rasterizes the
// character, keeps it in 1024x1024 texture pages and tells the game where it is.
// The game's icons (button prompts and the like, U+E000..U+E2FF) come the other
// way: the game hands the host their pixels to keep in the same pages.
//
// What the game expects, worked out from haloreach.dll: a character's picture
// starts at the font's ascender line (the game places it at baseline - ascender),
// its left edge is at the pen position + horiBearingX, and coordinates in the
// pages are pixels. Sizes are the game's size times its scale (its x1/x2/x3 art).
#pragma pack(push, 1)
struct s_font_character
{
    wchar_t unicode;
    uint16_t pad;
    float scale;
    int16_t left, top, width, height;
    int16_t bearing_y, bearing_x, advance, vert_bearing_x;
    uint8_t texture;
    uint8_t valid;
};
#pragma pack(pop)
static_assert(sizeof(s_font_character) == 26, "font character size");

static ID3D11Device *g_device;
static ID3D11DeviceContext *g_context;

namespace fonts
{
constexpr int k_page = 1024;
constexpr int k_max_pages = 32;

struct page
{
    std::vector<uint8_t> rgba = std::vector<uint8_t>(k_page * k_page * 4, 0);
    int x = 1, y = 1, row_height = 0;
    int dirty_top = k_page, dirty_bottom = 0;
    ID3D11Texture2D *texture = nullptr;
    ID3D11ShaderResourceView *view = nullptr;
};

struct face
{
    HFONT font = nullptr;
    TEXTMETRICW metrics = {};
    std::wstring name;
};

static std::recursive_mutex g_lock;
static std::vector<page *> g_pages;
static std::map<std::string, s_font_character> g_chars;
static std::map<std::string, face> g_faces;
static HDC g_dc;
static std::set<std::wstring> g_families;
static std::atomic<uint32_t> g_rasterized, g_icons;

static std::string key(const char *name, int size, float scale)
{
    char buf[160];
    snprintf(buf, sizeof buf, "%s|%d|%g", name ? name : "", size, scale);
    return buf;
}
static std::string char_key(const char *name, int size, float scale, wchar_t c)
{
    return key(name, size, scale) + "|" + std::to_string((unsigned)c);
}

static int CALLBACK collect_family(const LOGFONTW *lf, const TEXTMETRICW *, DWORD, LPARAM)
{
    std::wstring n = lf->lfFaceName;
    for (auto &ch : n) ch = towlower(ch);
    g_families.insert(n);
    return 1;
}

static void scan_families()
{
    g_families.clear();
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(g_dc, &lf, collect_family, 0, 0);
}

static bool have_family(const std::wstring &name)
{
    std::wstring n = name;
    for (auto &ch : n) ch = towlower(ch);
    return g_families.count(n) != 0;
}

// Fonts MCC ships (if any are loose files) are registered for this process only.
static void register_font_files(const std::wstring &dir, int depth, int &count)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (fd.cFileName[0] == L'.') continue;
        std::wstring path = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (depth > 0 && _wcsicmp(fd.cFileName, L"maps") && _wcsicmp(fd.cFileName, L"easyanticheat"))
                register_font_files(path, depth - 1, count);
            continue;
        }
        const wchar_t *ext = wcsrchr(fd.cFileName, L'.');
        if (!ext || (_wcsicmp(ext, L".ttf") && _wcsicmp(ext, L".otf") && _wcsicmp(ext, L".ttc"))) continue;
        int added = AddFontResourceExW(path.c_str(), FR_PRIVATE, nullptr);
        logf("fonts: %s -> %d face(s)", narrow(path.c_str()).c_str(), added);
        count += added;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void init(const std::wstring &mcc_root)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_dc = CreateCompatibleDC(nullptr);
    int count = 0;
    register_font_files(mcc_root, 5, count);
    logf("fonts: %d face(s) registered from the MCC folder", count);
    scan_families();
}

// The game's font names and stand-ins for them on a stock Windows install.
static std::wstring resolve(const char *name, bool &bold)
{
    std::string n = name ? name : "";
    bold = n.find("Bold") != std::string::npos;
    std::vector<std::wstring> candidates;
    std::wstring wide(n.begin(), n.end());
    candidates.push_back(wide);
    if (n.rfind("TVNord", 0) == 0)
        candidates.insert(candidates.end(), {L"TV Nord Cond", L"TVNord Cond", L"Bahnschrift SemiCondensed",
                                             L"Bahnschrift Condensed", L"Bahnschrift", L"Arial Narrow"});
    else if (n == "Eurostile")
        candidates.insert(candidates.end(), {L"Eurostile LT Std", L"Eurostile Next", L"Bahnschrift", L"Segoe UI"});
    else if (n == "Fixedsys")
        candidates.insert(candidates.end(), {L"Consolas", L"Courier New"});
    else if (n.rfind("Blender", 0) == 0)
        candidates.insert(candidates.end(), {L"Blender Pro", L"Bahnschrift", L"Segoe UI"});
    candidates.insert(candidates.end(), {L"Bahnschrift", L"Segoe UI", L"Arial"});
    for (const auto &c : candidates)
        if (have_family(c)) return c;
    return L"Arial";
}

static face *get_face(const char *name, int size, float scale)
{
    std::string k = key(name, size, scale);
    auto it = g_faces.find(k);
    if (it != g_faces.end()) return &it->second;
    face f;
    bool bold = false;
    f.name = resolve(name, bold);
    int pixels = (int)lroundf(size * (scale > 0 ? scale : 1.0f));
    if (pixels < 4) pixels = 4;
    f.font = CreateFontW(-pixels, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                         CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, f.name.c_str());
    SelectObject(g_dc, f.font);
    GetTextMetricsW(g_dc, &f.metrics);
    if (g_verbose_log) logf("fonts: %s size %d scale %g -> %s %dpx (ascent %ld, descent %ld)", name ? name : "(null)", size, scale,
         narrow(f.name.c_str()).c_str(), pixels, f.metrics.tmAscent, f.metrics.tmDescent);
    return &(g_faces[k] = f);
}

// Room for a w x h picture in the pages; returns false when all pages are full.
static bool allocate(int w, int h, int &page_index, int &x, int &y)
{
    if (w > k_page - 2 || h > k_page - 2) return false;
    for (;;)
    {
        if (g_pages.empty()) g_pages.push_back(new page);
        page *p = g_pages.back();
        if (p->x + w + 1 > k_page) p->x = 1, p->y += p->row_height + 1, p->row_height = 0;
        if (p->y + h + 1 <= k_page)
        {
            page_index = (int)g_pages.size() - 1;
            x = p->x, y = p->y;
            p->x += w + 1;
            if (h > p->row_height) p->row_height = h;
            return true;
        }
        if ((int)g_pages.size() >= k_max_pages) return false;
        g_pages.push_back(new page);
    }
}

static void mark_dirty(page *p, int y, int h)
{
    if (y < p->dirty_top) p->dirty_top = y;
    if (y + h > p->dirty_bottom) p->dirty_bottom = y + h;
}

static const s_font_character *rasterize(const char *name, int size, float scale, wchar_t c)
{
    std::string k = char_key(name, size, scale, c);
    auto it = g_chars.find(k);
    if (it != g_chars.end()) return &it->second;
    if (c >= 0xE000 && c <= 0xE2FF) return nullptr; // icons come from the game

    face *f = get_face(name, size, scale);
    SelectObject(g_dc, f->font);
    s_font_character ch = {};
    ch.unicode = c;
    ch.scale = scale;
    int cell_height = f->metrics.tmAscent + f->metrics.tmDescent;
    GLYPHMETRICS gm = {};
    MAT2 identity = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    DWORD bytes = GetGlyphOutlineW(g_dc, c, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &identity);
    std::vector<uint8_t> bits;
    if (bytes != GDI_ERROR && bytes > 0)
    {
        bits.resize(bytes);
        if (GetGlyphOutlineW(g_dc, c, GGO_GRAY8_BITMAP, &gm, bytes, bits.data(), &identity) == GDI_ERROR) bits.clear();
    }
    else if (bytes == GDI_ERROR)
    {
        ABC abc = {};
        if (GetCharABCWidthsW(g_dc, c, c, &abc)) gm.gmCellIncX = (short)(abc.abcA + abc.abcB + abc.abcC);
    }
    int w = bits.empty() ? 0 : (int)gm.gmBlackBoxX;
    int h = bits.empty() ? 0 : cell_height;
    ch.bearing_x = (int16_t)(bits.empty() ? 0 : gm.gmptGlyphOrigin.x);
    ch.advance = (int16_t)gm.gmCellIncX;
    ch.bearing_y = (int16_t)f->metrics.tmAscent;
    ch.width = (int16_t)w;
    ch.height = (int16_t)h;
    ch.valid = 1;
    int page_index = 0, x = 0, y = 0;
    if (w > 0 && h > 0 && allocate(w, h, page_index, x, y))
    {
        page *p = g_pages[page_index];
        int pitch = ((int)gm.gmBlackBoxX + 3) & ~3;
        int top = f->metrics.tmAscent - gm.gmptGlyphOrigin.y;
        for (int row = 0; row < (int)gm.gmBlackBoxY; row++)
        {
            int ty = top + row;
            if (ty < 0 || ty >= h) continue;
            for (int col = 0; col < w; col++)
            {
                int v = bits[row * pitch + col];
                uint8_t a = (uint8_t)(v >= 64 ? 255 : v * 4);
                uint8_t *px = &p->rgba[((y + ty) * k_page + (x + col)) * 4];
                px[0] = px[1] = px[2] = 255;
                px[3] = a;
            }
        }
        mark_dirty(p, y, h);
        ch.left = (int16_t)x;
        ch.top = (int16_t)y;
        ch.texture = (uint8_t)page_index;
    }
    else
    {
        ch.width = ch.height = 0;
    }
    if (should_log(g_rasterized)) logf("fonts: '%lc' (U+%04X) %s %d -> %dx%d at page %d (%d,%d) advance %d", c >= 32 && c < 127 ? c : L'?', (unsigned)c, name ? name : "", size, ch.width, ch.height, ch.texture, ch.left, ch.top, ch.advance);
    return &(g_chars[k] = ch);
}
// A new game starts with an empty cache (its icons may differ).
static void reset()
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_chars.clear();
    for (page *p : g_pages)
    {
        std::fill(p->rgba.begin(), p->rgba.end(), 0);
        p->x = p->y = 1, p->row_height = 0;
        p->dirty_top = 0, p->dirty_bottom = k_page;
    }
}
} // namespace fonts

static bool h_font_test_string(void *, const wchar_t *text, int size, float scale, const char *name)
{
    LOG_CALL(61, "host.font_test_string size=%d scale=%g font=%s", size, scale, name ? name : "(null)");
    std::lock_guard<std::recursive_mutex> lock(fonts::g_lock);
    if (text)
        for (const wchar_t *p = text; *p; p++) fonts::rasterize(name, size, scale, *p);
    return true;
}

static bool h_font_precache_character(void *, wchar_t c, s_font_character *out, int size, float scale,
                                      const char *name)
{
    LOG_CALL(62, "host.font_precache_character U+%04X size=%d scale=%g font=%s", (unsigned)c, size, scale,
             name ? name : "(null)");
    std::lock_guard<std::recursive_mutex> lock(fonts::g_lock);
    const s_font_character *ch = fonts::rasterize(name, size, scale, c);
    if (!ch) return false;
    if (out) *out = *ch;
    return true;
}

static ID3D11ShaderResourceView *h_font_get_texture(void *, int index)
{
    LOG_CALL(63, "host.font_get_texture %d", index);
    std::lock_guard<std::recursive_mutex> lock(fonts::g_lock);
    if (index < 0 || index >= (int)fonts::g_pages.size() || !g_device) return nullptr;
    fonts::page *p = fonts::g_pages[index];
    if (!p->texture)
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = fonts::k_page;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {p->rgba.data(), fonts::k_page * 4, 0};
        if (FAILED(g_device->CreateTexture2D(&td, &init, &p->texture)) ||
            FAILED(g_device->CreateShaderResourceView(p->texture, nullptr, &p->view)))
        {
            logf("fonts: cannot create the texture for page %d", index);
            return nullptr;
        }
        p->dirty_top = fonts::k_page, p->dirty_bottom = 0;
        logf("fonts: page %d texture created", index);
    }
    else if (p->dirty_bottom > p->dirty_top && g_context)
    {
        // Called while the game draws text, on its rendering thread.
        D3D11_BOX box = {0, (UINT)p->dirty_top, 0, (UINT)fonts::k_page, (UINT)p->dirty_bottom, 1};
        g_context->UpdateSubresource(p->texture, 0, &box, &p->rgba[p->dirty_top * fonts::k_page * 4],
                                     fonts::k_page * 4, 0);
        p->dirty_top = fonts::k_page, p->dirty_bottom = 0;
    }
    return p->view;
}

static bool h_font_test_char(void *, wchar_t c, int size, float scale, const char *name)
{
    LOG_CALL(64, "host.font_test_char U+%04X size=%d scale=%g", (unsigned)c, size, scale);
    std::lock_guard<std::recursive_mutex> lock(fonts::g_lock);
    return fonts::g_chars.count(fonts::char_key(name, size, scale, c)) != 0;
}

static int h_font_kerning(void *, wchar_t c, wchar_t previous, int size, float scale, const char *name)
{
    return 0;
}

// An icon's pixels from the game: RGBA, 'pitch' bytes per row; origin_y is how far
// above the baseline its top sits.
static bool h_font_set_icon(void *, wchar_t c, int size, float scale, const char *name, int width, int height,
                            int origin_y, const uint8_t *data, int pitch)
{
    LOG_CALL(66, "host.font_set_icon U+%04X size=%d scale=%g %dx%d origin_y=%d", (unsigned)c, size, scale, width,
             height, origin_y);
    std::lock_guard<std::recursive_mutex> lock(fonts::g_lock);
    s_font_character ch = {};
    ch.unicode = c;
    ch.scale = scale;
    ch.width = (int16_t)width;
    ch.height = (int16_t)height;
    ch.advance = (int16_t)width;
    ch.bearing_y = (int16_t)origin_y;
    ch.valid = 1;
    int page_index = 0, x = 0, y = 0;
    if (data && width > 0 && height > 0 && fonts::allocate(width, height, page_index, x, y))
    {
        fonts::page *p = fonts::g_pages[page_index];
        for (int row = 0; row < height; row++)
            memcpy(&p->rgba[((y + row) * fonts::k_page + x) * 4], data + (size_t)row * pitch, (size_t)width * 4);
        fonts::mark_dirty(p, y, height);
        ch.left = (int16_t)x, ch.top = (int16_t)y, ch.texture = (uint8_t)page_index;
    }
    fonts::g_chars[fonts::char_key(name, size, scale, c)] = ch;
    fonts::g_icons++;
    return true;
}

static bool h_font_set_selection(void *, int size, float scale, const char *name, uint16_t *ascender,
                                 uint16_t *descender)
{
    LOG_CALL(67, "host.font_set_selection size=%d scale=%g font=%s", size, scale, name ? name : "(null)");
    std::lock_guard<std::recursive_mutex> lock(fonts::g_lock);
    fonts::face *f = fonts::get_face(name, size, scale);
    if (ascender) *ascender = (uint16_t)f->metrics.tmAscent;
    if (descender) *descender = (uint16_t)f->metrics.tmDescent;
    return true;
}

// The mission the game loads (it moves on to the next one by itself).
static std::mutex g_mission_lock;
static std::string g_current_map_file;
// The game asks for map files ahead of time (the next mission's, while the current
// one is still playing), so a requested map only becomes "the current mission" when
// the game then starts loading a map.
static std::string g_candidate_map_file, g_last_map_request;
static void note_map_loaded(const std::string &path)
{
    {
        std::lock_guard<std::mutex> l(g_mission_lock);
        if (path == g_last_map_request) return; // asked for again and again while loading
        g_last_map_request = path;
    }
    size_t slash = path.find_last_of("\\/");
    std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    if (name.size() <= 4 || _stricmp(name.c_str() + name.size() - 4, ".map")) return;
    name.resize(name.size() - 4);
    if (!_stricmp(name.c_str(), "shared") || !_stricmp(name.c_str(), "campaign") ||
        !_stricmp(name.c_str(), "mainmenu"))
        return;
    std::lock_guard<std::mutex> l(g_mission_lock);
    if (g_candidate_map_file != name) logf("map requested: %s", name.c_str());
    g_candidate_map_file = name;
}

static void commit_mission_map()
{
    std::lock_guard<std::mutex> l(g_mission_lock);
    if (g_candidate_map_file.empty() || g_candidate_map_file == g_current_map_file) return;
    g_current_map_file = g_candidate_map_file;
    logf("mission map: %s", g_current_map_file.c_str());
}

// ---------------------------------------------------------------- menus
// Two menus share this code:
//  - the launcher, shown when mcchost starts without --game: pick a game, a mission
//    and a difficulty, change settings, start.
//  - the pause menu: MCC draws its own pause menu, so when the player presses Start
//    (or Esc) the game tells the host (pause_game) and waits to be told to pause and
//    resume. The host pauses the game and draws this menu over its last frame.
// Both are drawn with Direct2D and read the controller, keyboard and mouse. While
// the pause menu is open the game sees no input; buttons still held when it closes
// are hidden from the game until they're released, so the Start or Esc that closes
// it doesn't reopen it.
//
// A game DLL can't be unloaded and loaded again in one process, so starting a game,
// restarting a mission and going back to the launcher each start a fresh mcchost
// with the right options, in the same place on screen.

struct host_settings
{
    int master = 100, music = 100, effects = 100; // percent
    float mouse = 1.0f;
    int invert_look = 0, invert_mouse = 0, fov = 78;
    std::string last_game;
    std::map<std::string, int> last_map; // per game
    std::map<std::string, int> ff_map;           // last Firefight map, per game
    std::map<std::string, std::string> ff_variant, cg_variant; // last game type (its file), per game
    int spops = 0;                               // last Spartan Ops mission (index)
    std::map<std::string, int> cg_map;           // last custom game / Forge map, per game
    int h2_classic = 0; // Halo 2 starts with classic graphics
    int h1_classic = 0; // Halo CE starts with classic graphics
    int button_style = 0; // 0 auto, 1 Xbox, 2 PlayStation
    int resolution = 0;   // index into k_resolutions (0: the screen's own)
    int windowed = 0;     // 0 fullscreen (borderless), 1 a window
    int players = 1;      // campaign split-screen players
    int keyboard_player = 0; // split-screen: the keyboard is player 1 on its own
    int split_session = 0;   // split-screen: list every player in the launch options
    int difficulty = 1;
};
static host_settings g_settings;


// ------------------------------------------------ controls (per game)
// Controller: each game has its own built-in button layout, so buttons are remapped
// before the game sees them: "this button now does what B does". That works the same
// in every game. Start is left alone so the pause menu is always reachable.
static const int k_pad_count = 15;
static const wchar_t *const k_pad_names[k_pad_count] = {
    L"A", L"B", L"X", L"Y", L"LB", L"RB", L"LT", L"RT", L"Left stick click", L"Right stick click",
    L"Back", L"D-pad up", L"D-pad down", L"D-pad left", L"D-pad right"};
static const WORD k_pad_bits[k_pad_count] = {
    XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
    XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0,
    XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_BACK,
    XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT};
enum { pad_lt = 6, pad_rt = 7 };
// Button names as printed on the controller in hand. "Auto" means PlayStation under
// Wine (the PS5, where prospero-win presents the DualSense as an Xbox pad) and Xbox
// on Windows.
static const wchar_t *const k_ps_names[k_pad_count] = {
    L"Cross", L"Circle", L"Square", L"Triangle", L"L1", L"R1", L"L2", L"R2", L"L3", L"R3",
    L"Create", L"D-pad up", L"D-pad down", L"D-pad left", L"D-pad right"};
static bool running_under_wine()
{
    static int cached = -1;
    if (cached < 0)
    {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        cached = nt && GetProcAddress(nt, "wine_get_version") ? 1 : 0;
    }
    return cached == 1;
}
static bool playstation_labels();
static const wchar_t *pad_name(int i) { return playstation_labels() ? k_ps_names[i] : k_pad_names[i]; }
// One of the menu's own buttons by its Xbox name: A, B, Start, Back.
static const wchar_t *btn(const wchar_t *xbox)
{
    if (!playstation_labels()) return xbox;
    if (!wcscmp(xbox, L"A")) return L"Cross";
    if (!wcscmp(xbox, L"B")) return L"Circle";
    if (!wcscmp(xbox, L"Start")) return L"Options";
    if (!wcscmp(xbox, L"Back")) return L"Create";
    return xbox;
}
struct pad_layout
{
    int source[k_pad_count]; // source[t]: the physical button that does what t does by default
    bool swap_sticks = false;
    pad_layout() { for (int i = 0; i < k_pad_count; i++) source[i] = i; }
    bool is_default() const
    {
        for (int i = 0; i < k_pad_count; i++)
            if (source[i] != i) return false;
        return !swap_sticks;
    }
};
static std::map<std::string, pad_layout> g_pad_layouts; // by game folder; missing = default
static pad_layout g_pad;                                // the running game's

static uint8_t pad_value(const XINPUT_GAMEPAD &p, int i)
{
    if (i == pad_lt) return p.bLeftTrigger;
    if (i == pad_rt) return p.bRightTrigger;
    return (p.wButtons & k_pad_bits[i]) ? 255 : 0;
}
static bool pad_pressed(const XINPUT_GAMEPAD &p, int i) { return pad_value(p, i) > XINPUT_GAMEPAD_TRIGGER_THRESHOLD; }

static XINPUT_GAMEPAD remap_pad(const XINPUT_GAMEPAD &in)
{
    const pad_layout l = g_pad;
    XINPUT_GAMEPAD out = in;
    out.wButtons = in.wButtons & XINPUT_GAMEPAD_START;
    out.bLeftTrigger = out.bRightTrigger = 0;
    for (int t = 0; t < k_pad_count; t++)
    {
        int src = (l.source[t] >= 0 && l.source[t] < k_pad_count) ? l.source[t] : t;
        uint8_t v = pad_value(in, src);
        if (t == pad_lt) out.bLeftTrigger = v;
        else if (t == pad_rt) out.bRightTrigger = v;
        else if (v > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) out.wButtons |= k_pad_bits[t];
    }
    if (l.swap_sticks)
    {
        std::swap(out.sThumbLX, out.sThumbRX);
        std::swap(out.sThumbLY, out.sThumbRY);
    }
    return out;
}

// Keyboard and mouse: the games read key bindings per action from the player profile,
// so these are real per-action bindings. Up to 5 keys each.
struct key_binding { int action; int keys[5]; };
static std::vector<key_binding> default_keys(int module)
{
    std::vector<key_binding> k = {
        {a_jump, {VK_SPACE}},          {a_switchgrenade, {'G'}},     {a_actionreload, {'E'}},
        {a_reload, {'R'}},             {a_switchweapon, {'1', VK_TAB}}, {a_melee, {'Q', 'V'}},
        {a_flashlight, {'4'}},         {a_throwgrenade, {'F'}},      {a_fire, {VK_LBUTTON}},
        {a_crouch, {VK_LCONTROL, 'C'}}, {a_zoom, {VK_RBUTTON}},       {a_swapweapon, {'X'}},
        {a_forward, {'W'}},            {a_back, {'S'}},              {a_left, {'A'}},
        {a_right, {'D'}},              {a_showscores, {VK_BACK}},    {a_equipment, {VK_LSHIFT}},
        {49 /* dual wield */, {'2'}},
    };
    if (module == 3) // Halo 4: sprint on Shift, armor ability moves to Z
    {
        for (auto &b : k)
            if (b.action == a_equipment) b.keys[0] = 'Z';
        k.push_back({a_sprint, {VK_LSHIFT}});
    }
    return k;
}
// by game folder, then action: only the actions changed from the defaults
static std::map<std::string, std::map<int, std::vector<int>>> g_key_overrides;

static std::vector<int> keys_for(const std::string &game, int module, int action)
{
    auto g = g_key_overrides.find(game);
    if (g != g_key_overrides.end())
    {
        auto a = g->second.find(action);
        if (a != g->second.end()) return a->second;
    }
    std::vector<int> out;
    for (const auto &b : default_keys(module))
        if (b.action == action)
            for (int k : b.keys)
                if (k) out.push_back(k);
    return out;
}

// The actions shown on a game's keyboard page, with that game's name for them.
struct key_action { int action; const wchar_t *label; };
static std::vector<key_action> key_actions(int module)
{
    std::vector<key_action> v = {
        {a_forward, L"Move forward"}, {a_back, L"Move back"}, {a_left, L"Strafe left"}, {a_right, L"Strafe right"},
        {a_jump, L"Jump"}, {a_crouch, L"Crouch"}, {a_fire, L"Fire"}, {a_zoom, L"Zoom"},
        {a_melee, L"Melee"}, {a_reload, L"Reload"}, {a_actionreload, L"Action / pick up"},
        {a_switchweapon, L"Switch weapon"}, {a_throwgrenade, L"Throw grenade"}, {a_switchgrenade, L"Switch grenade"},
    };
    switch (module)
    {
    case 0: v.push_back({a_flashlight, L"Flashlight"}); break;
    case 1: v.push_back({a_flashlight, L"Flashlight"}); v.push_back({49, L"Dual wield"}); break;
    case 2: v.push_back({a_equipment, L"Use equipment"}); break;
    case 5: v.push_back({a_flashlight, L"VISR"}); break;
    case 6: v.push_back({a_equipment, L"Armor ability"}); break;
    case 3: v.push_back({a_sprint, L"Sprint"}); v.push_back({a_equipment, L"Armor ability"}); break;
    }
    v.push_back({a_showscores, module <= 1 ? L"Scoreboard / switch graphics" : L"Scoreboard"});
    return v;
}

static std::wstring key_name(int vk)
{
    switch (vk)
    {
    case VK_LBUTTON: return L"Left click";
    case VK_RBUTTON: return L"Right click";
    case VK_MBUTTON: return L"Middle click";
    case VK_XBUTTON1: return L"Mouse 4";
    case VK_XBUTTON2: return L"Mouse 5";
    case VK_SPACE: return L"Space";
    case VK_LSHIFT: return L"Left Shift";
    case VK_RSHIFT: return L"Right Shift";
    case VK_LCONTROL: return L"Left Ctrl";
    case VK_RCONTROL: return L"Right Ctrl";
    case VK_LMENU: return L"Left Alt";
    case VK_RMENU: return L"Right Alt";
    case VK_TAB: return L"Tab";
    case VK_BACK: return L"Backspace";
    case VK_RETURN: return L"Enter";
    case VK_CAPITAL: return L"Caps Lock";
    case VK_UP: return L"Up";
    case VK_DOWN: return L"Down";
    case VK_LEFT: return L"Left";
    case VK_RIGHT: return L"Right";
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    wchar_t buf[64] = {};
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (scan && GetKeyNameTextW((LONG)(scan << 16), buf, 64) > 0) return buf;
    return L"Key " + std::to_wstring(vk);
}

static std::wstring keys_label(const std::vector<int> &keys)
{
    std::wstring s;
    for (int k : keys) s += (s.empty() ? L"" : L" / ") + key_name(k);
    return s.empty() ? L"None" : s;
}

static bool g_profile_changed; // tell the game to re-read the profile (message 5)

// Write a game's key bindings into the profile.
static void write_key_bindings(const std::string &game, int module)
{
    unsigned char *p = g_player_profile;
    for (int i = 0; i < k_action_count; i++)
    {
        put<int32_t>(p, 0x42C + i * 0x18, i);
        std::vector<int> keys = keys_for(game, module, i);
        for (int k = 0; k < 5; k++) put<int32_t>(p, 0x42C + i * 0x18 + 4 + k * 4, k < (int)keys.size() ? keys[k] : 0);
    }
    g_profile_changed = true;
}

// Make a game's controls the live ones (at game start, and after a change in game).
static void use_controls(const std::string &game, int module)
{
    auto it = g_pad_layouts.find(game);
    g_pad = it != g_pad_layouts.end() ? it->second : pad_layout();
    write_key_bindings(game, module);
}

// Picture sizes the games render at. 0 is the screen's own.
static const int k_resolutions[][2] = {{0, 0}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
static const int k_resolution_count = 6;
static std::wstring resolution_name(int i)
{
    if (i <= 0 || i >= k_resolution_count)
        return L"Screen (" + std::to_wstring(GetSystemMetrics(SM_CXSCREEN)) + L"x" + std::to_wstring(GetSystemMetrics(SM_CYSCREEN)) + L")";
    return std::to_wstring(k_resolutions[i][0]) + L"x" + std::to_wstring(k_resolutions[i][1]);
}
static bool g_size_given; // --width/--height on the command line win

static bool playstation_labels() { return g_settings.button_style == 2 || (g_settings.button_style == 0 && running_under_wine()); }

static std::wstring settings_path() { return g_cfg.host_dir + L"\\installation08.ini"; }

static void load_settings()
{
    FILE *f = _wfopen(settings_path().c_str(), L"r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f))
    {
        char key[64], value[400];
        if (sscanf(line, " %63[a-z0-9_] = %127[^\r\n]", key, value) != 2) continue;
        for (size_t n = strlen(value); n && (value[n - 1] == ' ' || value[n - 1] == '\t'); n--) value[n - 1] = 0;
        std::string k = key;
        float v = (float)atof(value);
        if (k == "master_volume") g_settings.master = (int)v;
        else if (k == "music_volume") g_settings.music = (int)v;
        else if (k == "effects_volume") g_settings.effects = (int)v;
        else if (k == "mouse_sensitivity") g_settings.mouse = v;
        else if (k == "invert_look") g_settings.invert_look = (int)v;
        else if (k == "invert_mouse") g_settings.invert_mouse = (int)v;
        else if (k == "field_of_view") g_settings.fov = (int)v;
        else if (k == "difficulty") g_settings.difficulty = (int)v;
        else if (k == "halo2_classic_graphics") g_settings.h2_classic = (int)v;
        else if (k == "halo1_classic_graphics") g_settings.h1_classic = (int)v;
        else if (k.rfind("firefight_map_", 0) == 0) g_settings.ff_map[k.substr(14)] = (int)v;
        else if (k.rfind("firefight_variant_", 0) == 0) g_settings.ff_variant[k.substr(18)] = value;
        else if (k == "spartan_ops_mission") g_settings.spops = (int)v;
        else if (k.rfind("custom_map_", 0) == 0) g_settings.cg_map[k.substr(11)] = (int)v;
        else if (k.rfind("custom_variant_", 0) == 0) g_settings.cg_variant[k.substr(15)] = value;
        else if (k == "button_labels") g_settings.button_style = (int)v;
        else if (k == "resolution") g_settings.resolution = (int)v;
        else if (k == "windowed") g_settings.windowed = (int)v;
        else if (k == "players") g_settings.players = (int)v;
        else if (k == "keyboard_player") g_settings.keyboard_player = (int)v;
        else if (k == "split_screen_session") g_settings.split_session = (int)v;
        else if (k == "verbose_log") g_verbose_log = v != 0;
        else if (k == "last_game") g_settings.last_game = value;
        else if (k.rfind("pad_", 0) == 0)
        {
            pad_layout l;
            int n = 0;
            for (const char *c = value; *c && n < k_pad_count; n++)
            {
                l.source[n] = atoi(c);
                if (l.source[n] < 0 || l.source[n] >= k_pad_count) l.source[n] = n;
                c = strchr(c, ',');
                if (!c) { n++; break; }
                c++;
            }
            l.swap_sticks = g_pad_layouts[k.substr(4)].swap_sticks;
            g_pad_layouts[k.substr(4)] = l;
        }
        else if (k.rfind("sticks_", 0) == 0) g_pad_layouts[k.substr(7)].swap_sticks = v != 0;
        else if (k.rfind("key_", 0) == 0 && k.rfind('_') > 4)
        {
            size_t u = k.rfind('_');
            std::vector<int> keys;
            if (strcmp(value, "none") != 0)
                for (const char *c = value; c && *c; c = strchr(c, ','), c = c ? c + 1 : nullptr)
                    if (atoi(c) > 0 && atoi(c) < 256 && keys.size() < 5) keys.push_back(atoi(c));
            g_key_overrides[k.substr(4, u - 4)][atoi(k.c_str() + u + 1)] = keys;
        }
        else if (k.rfind("last_map_", 0) == 0) g_settings.last_map[k.substr(9)] = (int)v;
    }
    fclose(f);
    logf("settings loaded from installation08.ini");
}

static void save_settings()
{
    FILE *f = _wfopen(settings_path().c_str(), L"w");
    if (!f) return;
    fprintf(f, "master_volume = %d\nmusic_volume = %d\neffects_volume = %d\nmouse_sensitivity = %.2f\n"
               "invert_look = %d\ninvert_mouse = %d\nfield_of_view = %d\ndifficulty = %d\n",
            g_settings.master, g_settings.music, g_settings.effects, g_settings.mouse, g_settings.invert_look,
            g_settings.invert_mouse, g_settings.fov, g_settings.difficulty);
    fprintf(f, "halo1_classic_graphics = %d\nhalo2_classic_graphics = %d\n", g_settings.h1_classic, g_settings.h2_classic);
    fprintf(f, "button_labels = %d\nresolution = %d\nwindowed = %d\nplayers = %d\nkeyboard_player = %d\n",
            g_settings.button_style, g_settings.resolution, g_settings.windowed, g_settings.players, g_settings.keyboard_player);
    for (const auto &m : g_settings.ff_map) fprintf(f, "firefight_map_%s = %d\n", m.first.c_str(), m.second);
    for (const auto &m : g_settings.ff_variant) fprintf(f, "firefight_variant_%s = %s\n", m.first.c_str(), m.second.c_str());
    fprintf(f, "spartan_ops_mission = %d\n", g_settings.spops);
    if (g_settings.split_session) fprintf(f, "split_screen_session = 1\n");
    for (const auto &m : g_settings.cg_map) fprintf(f, "custom_map_%s = %d\n", m.first.c_str(), m.second);
    for (const auto &m : g_settings.cg_variant) fprintf(f, "custom_variant_%s = %s\n", m.first.c_str(), m.second.c_str());
    if (!g_settings.last_game.empty()) fprintf(f, "last_game = %s\n", g_settings.last_game.c_str());
    if (g_verbose_log) fprintf(f, "verbose_log = 1\n");
    for (const auto &g : g_pad_layouts)
    {
        if (g.second.is_default()) continue;
        fprintf(f, "pad_%s = ", g.first.c_str());
        for (int i = 0; i < k_pad_count; i++) fprintf(f, i ? ",%d" : "%d", g.second.source[i]);
        fprintf(f, "\nsticks_%s = %d\n", g.first.c_str(), g.second.swap_sticks ? 1 : 0);
    }
    for (const auto &g : g_key_overrides)
        for (const auto &a : g.second)
        {
            fprintf(f, "key_%s_%d = ", g.first.c_str(), a.first);
            if (a.second.empty()) fprintf(f, "none");
            for (size_t i = 0; i < a.second.size(); i++) fprintf(f, i ? ",%d" : "%d", a.second[i]);
            fprintf(f, "\n");
        }
    for (const auto &m : g_settings.last_map) fprintf(f, "last_map_%s = %d\n", m.first.c_str(), m.second);
    fclose(f);
}

// Into the player profile. Volumes are read every frame; most of the rest (field of
// view, look inversion) only when the game re-reads the whole profile, which it does
// at load and after message 5, "settings changed" (sent when the pause menu closes).
static void apply_settings()
{
    g_profile_changed = true;
    unsigned char *p = g_player_profile;
    put<float>(p, 0xA5C, g_settings.master / 100.0f);
    put<float>(p, 0xA60, g_settings.music / 100.0f);
    put<float>(p, 0xA64, g_settings.effects / 100.0f);
    p[0x1D] = (unsigned char)g_settings.invert_look;
    p[0x1E] = (unsigned char)g_settings.invert_mouse;
    put<int32_t>(p, 0x14, g_settings.fov);
    put<int32_t>(p, 0x18, g_settings.fov);
    g_cfg.mouse_sensitivity = g_settings.mouse;
}

// ------------------------------------------------ games and missions
// Map ids are MCC's built-in ids (libmcc's numbering); files are the map each
// mission lives in, used to follow the game when it moves on to the next mission.
struct mission_entry { int map; const char *file; const wchar_t *name; };
struct game_entry
{
    const wchar_t *folder, *title;
    int module;
    bool tested;
    std::vector<mission_entry> missions;
    std::vector<mission_entry> firefight; // Firefight maps (Reach)
};
// Firefight game types: the ones MCC ships in each game's game_variants folder (file
// names without .bin).
struct firefight_type { const char *file; const wchar_t *name; };
static const std::vector<firefight_type> &firefight_types(int module)
{
    static const std::vector<firefight_type> reach = {
        {"hr_ff_standard_1set", L"Firefight"},          {"hr_ff_classic_1set", L"Classic Firefight"},
        {"hr_ff_scoreAttack_1round", L"Score Attack"},  {"hr_ff_gruntpocalypse_1round", L"Gruntpocalypse"},
        {"hr_ff_rocketfight_1set", L"Rocketfight"},     {"hr_ff_generatorDefense_1set", L"Generator Defense"},
    };
    static const std::vector<firefight_type> odst = {
        {"odst_ff_standard_1set", L"Firefight"},           {"odst_ff_classic", L"Classic Firefight"},
        {"odst_ff_gruntpocalypse_1round", L"Gruntpocalypse"}, {"odst_ff_robocalypse_1round", L"Robocalypse"},
        {"odst_ff_drone_wars_1round", L"Drone Wars"},      {"odst_ff_floodfight", L"Floodfight"},
        {"odst_ff_rocketfight_1set", L"Rocketfight"},      {"odst_ff_sniperfight_1set", L"Sniperfight"},
        {"odst_ff_fiestafight_1set", L"Fiestafight"},
    };
    static const std::vector<firefight_type> none;
    return module == 6 ? reach : module == 5 ? odst : none;
}

// Custom games: a multiplayer game type from game_variants on one of the game's
// multiplayer maps (built-in map variant, made from the map id). Reach for now.
static const std::vector<firefight_type> &custom_types(int module)
{
    static const std::vector<firefight_type> reach = {
        {"slayer_054", L"Slayer"},           {"slayer_pro_054", L"Slayer Pro"},     {"SWAT_054", L"SWAT"},
        {"slayer_classic_054", L"Classic Slayer"}, {"headhunter_054", L"Headhunter"}, {"juggernaut_054", L"Juggernaut"},
        {"infection_054", L"Infection"},     {"koth_054", L"King of the Hill"},    {"oddball_054", L"Oddball"},
        {"ctf_054", L"Capture the Flag"},    {"assault_054", L"Assault"},          {"territories_054", L"Territories"},
        {"stockpile_054", L"Stockpile"},     {"race_054", L"Race"},                {"rally_054", L"Rally"},
    };
    static const std::vector<firefight_type> h4 = {
        {"H4_SLAYER_132", L"Slayer"},            {"H4_SWAT_132", L"SWAT"},             {"H4_SLAYER_ESCALATION_132", L"Escalation Slayer"},
        {"H4_infinityrumble_8ffa_132", L"Rumble Pit"}, {"H4_REGICIDE_132", L"Regicide"},  {"H4_CTF_132", L"Capture the Flag"},
        {"H4_KOTH_132", L"King of the Hill"},    {"H4_ODDBALL_132", L"Oddball"},       {"H4_DOMINION_132", L"Dominion"},
        {"H4_EXTRACTION_132", L"Extraction"},    {"H4_FLOOD_132", L"Flood"},           {"H4_GOAL_132", L"Ricochet"},
        {"H4_Grifball_132", L"Grifball"},
    };
    static const std::vector<firefight_type> h1 = {
        {"01_slayer", L"Slayer"},           {"02_team_slayer", L"Team Slayer"},   {"03_classic_slayer", L"Classic Slayer"},
        {"09_classic_rockets", L"Rockets"}, {"10_classic_snipers", L"Snipers"},   {"11_ctf", L"Capture the Flag"},
        {"12_classic_ctf", L"Classic CTF"}, {"16_king", L"King of the Hill"},    {"17_crazy_king", L"Crazy King"},
        {"23_oddball", L"Oddball"},         {"29_juggernaut", L"Juggernaut"},     {"32_race", L"Race"},
        {"38_assault", L"Assault"},
    };
    static const std::vector<firefight_type> h2 = {
        {"01_slayer", L"Slayer"},           {"02_team_slayer", L"Team Slayer"},   {"03_rockets", L"Rockets"},
        {"04_swords", L"Swords"},           {"05_snipers", L"Snipers"},           {"23_fiesta", L"Fiesta"},
        {"40_swat", L"SWAT"},               {"10_ctf_classic", L"Capture the Flag"}, {"13_king", L"King of the Hill"},
        {"16_crazy_king", L"Crazy King"},   {"18_oddball", L"Oddball"},           {"26_ninjanaut", L"Ninjanaut"},
        {"30_single_bomb", L"Assault"},     {"34_3_plots", L"Territories"},
    };
    static const std::vector<firefight_type> h3 = {
        {"slayer-0_010", L"Slayer"},
        {"slayer-1_010", L"Slayer (2)"},
        {"slayer-2_010", L"Slayer (3)"},
        {"slayer-3_010", L"Slayer (4)"},
        {"slayer-4_010", L"Slayer (5)"},
        {"ctf-0_010", L"Capture the Flag"},
        {"ctf-1_010", L"Capture the Flag (2)"},
        {"ctf-2_010", L"Capture the Flag (3)"},
        {"ctf-3_010", L"Capture the Flag (4)"},
        {"assault-0_010", L"Assault"},
        {"assault-1_010", L"Assault (2)"},
        {"assault-2_010", L"Assault (3)"},
        {"assault-3_010", L"Assault (4)"},
        {"infection-0_010", L"Infection"},
        {"infection-1_010", L"Infection (2)"},
        {"infection-2_010", L"Infection (3)"},
        {"infection-3_010", L"Infection (4)"},
        {"juggernaut-0_010", L"Juggernaut"},
        {"juggernaut-1_010", L"Juggernaut (2)"},
        {"juggernaut-2_010", L"Juggernaut (3)"},
        {"king of the hill-0_010", L"King of the Hill"},
        {"king of the hill-1_010", L"King of the Hill (2)"},
        {"king of the hill-2_010", L"King of the Hill (3)"},
        {"oddball-0_010", L"Oddball"},
        {"oddball-1_010", L"Oddball (2)"},
        {"oddball-2_010", L"Oddball (3)"},
        {"oddball-3_010", L"Oddball (4)"},
        {"oddball-4_010", L"Oddball (5)"},
        {"territories-0_010", L"Territories"},
        {"territories-1_010", L"Territories (2)"},
        {"territories-2_010", L"Territories (3)"},
        {"vip-0_010", L"VIP"},
        {"vip-1_010", L"VIP (2)"},
        {"vip-2_010", L"VIP (3)"},
        {"vip-3_010", L"VIP (4)"},
        {"gun_game_default_010", L"Gun Game"},
    };
    static const std::vector<firefight_type> none;
    return module == 6 ? reach : module == 3 ? h4 : module == 0 ? h1 : module == 1 ? h2 : module == 2 ? h3 : none;
}
// Forge: the game's editing game type on any multiplayer map.
static const char *forge_type(int module)
{
    return module == 6 ? "00_basic_editing_054" : module == 3 ? "H4_BASIC_EDITING_132" : module == 2 ? "00_sandbox-0_010" : nullptr;
}
static const std::vector<mission_entry> &custom_maps(int module)
{
    static const std::vector<mission_entry> reach = {
        {190, "50_panopticon", L"Boardwalk"},   {191, "70_boneyard", L"Boneyard"},      {192, "45_launch_station", L"Countdown"},
        {193, "30_settlement", L"Powerhouse"},  {194, "52_ivory_tower", L"Reflection"}, {195, "35_island", L"Spire"},
        {196, "20_sword_slayer", L"Sword Base"}, {197, "45_aftship", L"Zealot"},         {198, "dlc_slayer", L"Anchor 9"},
        {199, "dlc_invasion", L"Breakpoint"},   {200, "dlc_medium", L"Tempest"},         {201, "condemned", L"Condemned"},
        {202, "trainingpreserve", L"Highlands"}, {203, "cex_beaver_creek", L"Battle Canyon"}, {204, "cex_damnation", L"Penance"},
        {205, "cex_timberland", L"Ridgeline"},  {206, "cex_prisoner", L"Solitary"},     {207, "cex_hangemhigh", L"High Noon"},
        {208, "cex_headlong", L"Breakneck"},    {209, "forge_halo", L"Forge World"},
    };
    // Halo 4: map ids and files from halo4.dll's own id -> map table.
    static const std::vector<mission_entry> h4 = {
        {114, "ca_warhouse", L"Adrift"},        {115, "ca_blood_cavern", L"Abandon"},  {116, "z05_cliffside", L"Complex"},
        {117, "ca_blood_crash", L"Exile"},      {118, "wraparound", L"Haven"},         {119, "ca_gore_valley", L"Longbow"},
        {120, "ca_canyon", L"Meltdown"},        {121, "z11_valhalla", L"Ragnarok"},    {122, "ca_tower", L"Solace"},
        {123, "ca_redoubt", L"Vortex"},         {124, "ca_forge_ravine", L"Ravine"},   {125, "ca_forge_bonanza", L"Impact"},
        {126, "ca_forge_erosion", L"Erosion"},  {127, "dlc_forge_island", L"Forge Island"}, {128, "dlc_dejunkyard", L"Wreckage"},
        {129, "zd_02_grind", L"Harvest"},       {130, "dlc_dejewel", L"Shatter"},      {131, "ca_port", L"Landfall"},
        {132, "ca_deadlycrossing", L"Monolith"}, {133, "ca_rattler", L"Skyline"},      {134, "ca_spiderweb", L"Daybreak"},
        {135, "ca_basin", L"Outcast"},          {136, "ca_highrise", L"Perdition"},    {137, "ca_creeper", L"Pitfall"},
        {138, "ca_dropoff", L"Vertigo"},
    };
    // Halo CE, Halo 2 and Halo 3: MCC's map ids (libmcc's list); files as each game names them.
    static const std::vector<mission_entry> h1 = {
        {10, "beavercreek", L"Battle Creek"}, {11, "sidewinder", L"Sidewinder"},   {12, "damnation", L"Damnation"},
        {13, "ratrace", L"Rat Race"},         {14, "prisoner", L"Prisoner"},       {15, "hangemhigh", L"Hang 'Em High"},
        {16, "chillout", L"Chill Out"},       {17, "carousel", L"Derelict"},       {18, "boardingaction", L"Boarding Action"},
        {19, "putput", L"Chiron TL-34"},      {20, "bloodgulch", L"Blood Gulch"},  {21, "wizard", L"Wizard"},
        {22, "longest", L"Longest"},          {23, "deathisland", L"Death Island"}, {24, "dangercanyon", L"Danger Canyon"},
        {25, "infinity", L"Infinity"},        {26, "timberland", L"Timberland"},   {27, "icefields", L"Ice Fields"},
        {28, "gephyrophobia", L"Gephyrophobia"},
    };
    static const std::vector<mission_entry> h2 = {
        {44, "lockout", L"Lockout"},          {45, "ascension", L"Ascension"},     {46, "midship", L"Midship"},
        {47, "cyclotron", L"Ivory Tower"},    {48, "beavercreek", L"Beaver Creek"}, {49, "burial_mounds", L"Burial Mounds"},
        {50, "colossus", L"Colossus"},        {51, "zanzibar", L"Zanzibar"},       {52, "coagulation", L"Coagulation"},
        {53, "headlong", L"Headlong"},        {54, "waterworks", L"Waterworks"},   {55, "foundation", L"Foundation"},
        {56, "containment", L"Containment"},  {57, "warlock", L"Warlock"},         {58, "deltatap", L"Sanctuary"},
        {59, "turf", L"Turf"},                {60, "backwash", L"Backwash"},       {61, "elongation", L"Elongation"},
        {62, "gemini", L"Gemini"},            {63, "dune", L"Relic"},              {64, "triplicate", L"Terminal"},
        {65, "derelict", L"Desolation"},      {66, "highplains", L"Tombstone"},    {67, "street_sweeper", L"District"},
        {68, "needle", L"Uplift"},
    };
    static const std::vector<mission_entry> h3 = {
        {79, "construct", L"Construct"},      {80, "salvation", L"Epitaph"},       {81, "guardian", L"Guardian"},
        {82, "deadlock", L"High Ground"},     {83, "isolation", L"Isolation"},     {84, "zanzibar", L"Last Resort"},
        {85, "chill", L"Narrows"},            {86, "shrine", L"Sandtrap"},         {87, "snowbound", L"Snowbound"},
        {88, "cyberdyne", L"The Pit"},        {89, "riverworld", L"Valhalla"},     {90, "warehouse", L"Foundry"},
        {91, "armory", L"Rat's Nest"},        {92, "bunkerworld", L"Standoff"},    {93, "sidewinder", L"Avalanche"},
        {94, "lockout", L"Blackout"},         {95, "ghosttown", L"Ghost Town"},    {96, "chillout", L"Cold Storage"},
        {97, "descent", L"Assembly"},         {98, "spacecamp", L"Orbital"},       {99, "sandbox", L"Sandbox"},
        {100, "fortress", L"Citadel"},        {101, "midship", L"Heretic"},        {102, "docks", L"Longshore"},
        {220, "s3d_waterfall", L"Waterfall"}, {221, "s3d_edge", L"Edge"},          {222, "s3d_turf", L"Icebox"},
    };
    static const std::vector<mission_entry> none;
    return module == 6 ? reach : module == 3 ? h4 : module == 0 ? h1 : module == 1 ? h2 : module == 2 ? h3 : none;
}

// Halo 4's Spartan Ops: 10 episodes of 5 missions, each a file in spops_variants and
// played on one of the Spartan Ops maps (map ids 140-154).
struct spops_mission { int episode; const char *file; int map; const wchar_t *map_name; };
static const std::vector<spops_mission> &spops_missions()
{
    enum { chopperbowl = 140, sniperalley, fortsw, temple, scurve, courtyard, complex_, valhalla, factory, mezzanine,
           caverns, vortex, breach, hillside, engine };
    static const std::vector<spops_mission> list = {
        {1, "e1_m1_chopperbowl_vehicle_destroy_132", chopperbowl, L"Chopperbowl"},
        {1, "e1_m3_fortsw_shutdown_132", fortsw, L"Fort"},
        {1, "e1_m4_sniperalley_destroy_132", sniperalley, L"Sniper Alley"},
        {1, "e1_m4_temple_escape_132", temple, L"Temple"},
        {1, "e1_m5_scurve_destroy_132", scurve, L"S-Curve"},
        {2, "e2_m1_cathedral_132", courtyard, L"Cathedral"},
        {2, "e2_m2_scurve_rescue_132", scurve, L"S-Curve"},
        {2, "e2_m3_forts_defend_132", fortsw, L"Fort"},
        {2, "e2_m4_sniperalley_run_132", sniperalley, L"Sniper Alley"},
        {2, "e2_m5_temple_retake_132", temple, L"Temple"},
        {3, "e3_m1_chopperbowl_swarm_132", chopperbowl, L"Chopperbowl"},
        {3, "e3_m2_complex_destroy_132", complex_, L"Complex"},
        {3, "e3_m3_valhalla_132", valhalla, L"Valhalla"},
        {3, "e3_m4_valhalla_132", valhalla, L"Valhalla"},
        {3, "e3_m5_cathedral_132", courtyard, L"Cathedral"},
        {4, "e4_m1_sniperalley_grab_132", sniperalley, L"Sniper Alley"},
        {4, "e4_m2_valhalla_132", valhalla, L"Valhalla"},
        {4, "e4_m3_fortsw_activate_132", fortsw, L"Fort"},
        {4, "e4_m4_scurve_capture_132", scurve, L"S-Curve"},
        {4, "e4_m5_complex_holdout_132", complex_, L"Complex"},
        {5, "e5_m1_cathedral_defend_132", courtyard, L"Cathedral"},
        {5, "e5_m1_temple_shutdown_132", temple, L"Temple"},
        {5, "e5_m2_temple_recover_132", temple, L"Temple"},
        {5, "e5_m4_scurve_defend_132", scurve, L"S-Curve"},
        {5, "e5_m5_chopperbowl_outnumbered_132", chopperbowl, L"Chopperbowl"},
        {6, "e6_m1_factory_132", factory, L"Lockup"},
        {6, "e6_m2_mezzanine_infiltrate_132", mezzanine, L"Mezzanine"},
        {6, "e6_m3_caverns_recon_132", caverns, L"Caverns"},
        {6, "e6_m4_vortex_132", vortex, L"Vortex"},
        {6, "e6_m5_breach_destroy_132", breach, L"Breach"},
        {7, "e7_m1_hillside_abort_132", hillside, L"Hillside"},
        {7, "e7_m2_engine_recovery_132", engine, L"Infinity"},
        {7, "e7_m3_engine_havok_132", engine, L"Infinity"},
        {7, "e7_m4_engine_ventcore_132", engine, L"Infinity"},
        {7, "e7_m5_engine_roadtrip_132", engine, L"Infinity"},
        {8, "e8_m1_hillside_retake_132", hillside, L"Hillside"},
        {8, "e8_m2_vortex_artillery_132", vortex, L"Vortex"},
        {8, "e8_m3_caverns_rescue_132", caverns, L"Caverns"},
        {8, "e8_m4_factory_132", factory, L"Lockup"},
        {8, "e8_m5_mezzanine_secure_132", mezzanine, L"Mezzanine"},
        {9, "e9_m1_vortex_traces_132", vortex, L"Vortex"},
        {9, "e9_m2_mezzanine_break_132", mezzanine, L"Mezzanine"},
        {9, "e9_m3_hillside_132", hillside, L"Hillside"},
        {9, "e9_m4_factory_132", factory, L"Lockup"},
        {9, "e9_m5_factory_132", factory, L"Lockup"},
        {10, "e10_m1_breach_intel_132", breach, L"Breach"},
        {10, "e10_m2_cavern_kill_132", caverns, L"Caverns"},
        {10, "e10_m3_hillside_blitzkrieg_132", hillside, L"Hillside"},
        {10, "e10_m4_caverns_outlast_132", caverns, L"Caverns"},
        {10, "e10_m5_breach_unlock_132", breach, L"Breach"},
    };
    return list;
}
static const std::vector<game_entry> &games()
{
    static const std::vector<game_entry> list = {
        {L"halo1", L"Halo: Combat Evolved Anniversary", 0, false,
         {{0, "a10", L"The Pillar of Autumn"}, {1, "a30", L"Halo"}, {2, "a50", L"The Truth and Reconciliation"},
          {3, "b30", L"The Silent Cartographer"}, {4, "b40", L"Assault on the Control Room"},
          {5, "c10", L"343 Guilty Spark"}, {6, "c20", L"The Library"}, {7, "c40", L"Two Betrayals"},
          {8, "d20", L"Keyes"}, {9, "d40", L"The Maw"}}},
        {L"halo2", L"Halo 2: Anniversary", 1, false,
         {{29, "00a_introduction", L"The Heretic"}, {30, "01a_tutorial", L"The Armory"},
          {31, "01b_spacestation", L"Cairo Station"}, {32, "03a_oldmombasa", L"Outskirts"},
          {33, "03b_newmombasa", L"Metropolis"}, {34, "04a_gasgiant", L"The Arbiter"},
          {35, "04b_floodlab", L"The Oracle"}, {36, "05a_deltaapproach", L"Delta Halo"},
          {37, "05b_deltatowers", L"Regret"}, {38, "06a_sentinelwalls", L"Sacred Icon"},
          {39, "06b_floodzone", L"Quarantine Zone"}, {40, "07a_highcharity", L"Gravemind"},
          {41, "07b_forerunnership", L"Uprising"}, {42, "08a_deltacliffs", L"High Charity"},
          {43, "08b_deltacontrol", L"The Great Journey"}}},
        {L"halo3", L"Halo 3", 2, true,
         {{69, "005_intro", L"Arrival"}, {70, "010_jungle", L"Sierra 117"}, {71, "020_base", L"Crow's Nest"},
          {72, "030_outskirts", L"Tsavo Highway"}, {73, "040_voi", L"The Storm"}, {74, "050_floodvoi", L"Floodgate"},
          {75, "070_waste", L"The Ark"}, {76, "100_citadel", L"The Covenant"}, {77, "110_hc", L"Cortana"},
          {78, "120_halo", L"Halo"}, {165, "130_epilogue", L"Epilogue"}}},
        {L"halo3odst", L"Halo 3: ODST", 5, false,
         {{166, "c100", L"Prepare to Drop"}, {167, "h100", L"Mombasa Streets"}, {168, "sc100", L"Tayari Plaza"},
          {169, "sc110", L"Uplift Reserve"}, {170, "sc120", L"Kizingo Boulevard"},
          {171, "sc130", L"ONI Alpha Site"}, {172, "sc140", L"NMPD HQ"}, {173, "sc150", L"Kikowani Station"},
          {174, "l200", L"Data Hive"}, {175, "l300", L"Coastal Highway"}, {176, "c200", L"Epilogue"}},
         // ODST's Firefight runs on the campaign maps, launched with the campaign map ids.
         {{168, "sc100", L"Crater"}, {169, "sc110", L"Lost Platoon"}, {170, "sc120", L"Rally Point"},
          {171, "sc130", L"Alpha Site"}, {172, "sc140", L"Security Zone"}, {173, "sc150", L"Windward"},
          {174, "l200", L"Chasm Ten"}, {175, "l300", L"Last Exit"}}},
        {L"haloreach", L"Halo: Reach", 6, true,
         {{178, "m05", L"Noble Actual"}, {179, "m10", L"Winter Contingency"}, {180, "m20", L"ONI: Sword Base"},
          {181, "m30", L"Nightfall"}, {182, "m35", L"Tip of the Spear"}, {183, "m45", L"Long Night of Solace"},
          {184, "m50", L"Exodus"}, {185, "m52", L"New Alexandria"}, {186, "m60", L"The Package"},
          {187, "m70", L"The Pillar of Autumn"}, {189, "m70_bonus", L"Lone Wolf"}},
         {{210, "ff50_park", L"Beachhead"}, {211, "ff45_corvette", L"Corvette"}, {212, "ff20_courtyard", L"Courtyard"},
          {213, "ff60_icecave", L"Glacier"}, {214, "ff70_holdout", L"Holdout"}, {215, "ff60_airview", L"Outpost"},
          {216, "ff10_prototype", L"Overlook"}, {217, "ff30_waterfront", L"Waterfront"},
          {218, "ff_unearthed", L"Unearthed"}, {219, "cex_ff_halo", L"Installation 04"}}},
        {L"halo4", L"Halo 4", 3, false,
         {{103, "m05_prologue", L"Prologue"}, {104, "m10_crash", L"Dawn"}, {105, "m020", L"Requiem"},
          {106, "m30_cryptum", L"Forerunner"}, {107, "m40_invasion", L"Infinity"}, {108, "m60_rescue", L"Reclaimer"},
          {109, "m70_liftoff", L"Shutdown"}, {110, "m80_delta", L"Composer"}, {111, "m90_sacrifice", L"Midnight"},
          {112, "m95_epilogue", L"Epilogue"}}},
    };
    return list;
}
static const game_entry *find_game(const std::wstring &folder)
{
    for (const auto &g : games())
        if (!_wcsicmp(g.folder, folder.c_str())) return &g;
    return nullptr;
}
static bool game_installed(const game_entry &g)
{
    std::wstring dll = g_cfg.mcc_root + L"\\" + g.folder + L"\\" + g.folder + L".dll";
    return GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES;
}
static const wchar_t *k_difficulty_names[] = {L"Easy", L"Normal", L"Heroic", L"Legendary"};
static const wchar_t *difficulty_name(int d) { return k_difficulty_names[std::max(0, std::min(3, d))]; }

static std::wstring g_objective_primary, g_objective_secondary;
static std::vector<std::wstring> g_launch_args;
static bool g_launcher; // no game loaded: the launcher

static const mission_entry *current_mission()
{
    const game_entry *g = find_game(g_cfg.game);
    if (!g) return nullptr;
    std::string file;
    {
        std::lock_guard<std::mutex> l(g_mission_lock);
        file = g_current_map_file;
    }
    for (const auto *list : {&g->missions, &g->firefight})
        for (const auto &m : *list)
            if (!file.empty() && !_stricmp(m.file, file.c_str())) return &m;
    for (const auto *list : {&g->missions, &g->firefight, &custom_maps(g->module)})
        for (const auto &m : *list)
            if (m.map == g_cfg.map) return &m;
    return nullptr;
}

// What to do when the current game (or the launcher) ends.
struct next_action
{
    enum { k_none, k_launcher, k_game, k_quit } kind = k_none;
    std::wstring game;
    int map = -1;
    std::string file;
    int difficulty = 1;
    bool resume = false; // continue from the saved game
    int mode = 1;        // 1 campaign, 2 Spartan Ops, 3 custom game, 5 Firefight
    int players = 1;     // local players (campaign split-screen)
    std::string variant; // Firefight / Spartan Ops: the game type's file, relative to the game's folder (no .bin)
};
static next_action g_next;
static int g_mode = 1;           // what's running: 1 campaign, 2 Spartan Ops, 5 Firefight
static std::string g_ff_variant; // and, for Firefight, its game type
static std::atomic<bool> g_ending;     // the game was asked to end
static int64_t g_ending_since;

// Switch to another game or mission (game empty: the launcher). The game running now
// is asked to end; when its thread has finished, its DLL is unloaded and the next
// one is loaded, all in this process.
static void relaunch(const std::wstring &game, int map, const std::string &file, int difficulty)
{
    next_action n;
    n.kind = game.empty() ? next_action::k_launcher : next_action::k_game;
    n.game = game, n.map = map, n.file = file, n.difficulty = difficulty;
    n.players = std::max(1, std::min(4, g_settings.players));
    if (!game.empty())
    {
        g_settings.last_game = narrow(game.c_str());
        g_settings.last_map[g_settings.last_game] = map;
        g_settings.difficulty = difficulty;
    }
    save_settings();
    g_next = n;
    logf("next: %s %s map %d", n.kind == next_action::k_launcher ? "launcher" : "game", narrow(game.c_str()).c_str(), map);
}

static void relaunch_mode(int mode, const std::wstring &game, int map, const std::string &file, const std::string &variant,
                          int difficulty)
{
    relaunch(game, map, file, difficulty);
    next_action n = g_next;
    n.mode = mode, n.variant = variant;
    g_next = n;
    logf("next: mode %d, game type %s", mode, variant.c_str());
}

static void request_quit()
{
    save_settings();
    g_next = next_action();
    g_next.kind = next_action::k_quit;
    logf("next: quit");
}

struct save_info
{
    bool exists = false;
    int map = -1, difficulty = 1;
    std::string file, saved;
};
static save_info read_save_info(const std::wstring &game)
{
    save_info info;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(save_path(game).c_str(), GetFileExInfoStandard, &fa)) return info;
    if (fa.nFileSizeHigh == 0 && fa.nFileSizeLow < 4096) return info; // not a real saved game
    FILE *f = _wfopen((save_path(game) + L".info").c_str(), L"r");
    if (!f) return info;
    char line[256];
    while (fgets(line, sizeof line, f))
    {
        char key[32], value[128] = {};
        if (sscanf(line, " %31[a-z] = %127[^\n]", key, value) < 2) continue;
        std::string k = key;
        if (k == "map") info.map = atoi(value);
        else if (k == "file") info.file = value;
        else if (k == "difficulty") info.difficulty = atoi(value);
        else if (k == "saved") info.saved = value;
    }
    fclose(f);
    info.exists = info.map >= 0;
    return info;
}

static void continue_from_save(const std::wstring &game)
{
    save_info info = read_save_info(game);
    if (!info.exists) return;
    relaunch(game, info.map, info.file, info.difficulty);
    g_next.resume = true;
    logf("next: continue from the saved game");
}

static int current_map_for_save()
{
    const mission_entry *m = current_mission();
    return m ? m->map : g_cfg.map;
}
static std::string current_file_for_save()
{
    const mission_entry *m = current_mission();
    return m ? std::string(m->file) : narrow(g_cfg.map_file.c_str());
}

// "Save and quit": Reach saves its state and hands it to the host whenever a game
// ends (as well as at each checkpoint), so ending the game and going back to the
// launcher is all it takes. (Its game_save_and_quit script command didn't save.)
static void start_save_and_quit()
{
    logf("menu: save and quit");
    relaunch(L"", 0, "", g_cfg.difficulty);
}
static void on_game_saved() {}

static void quit_without_saving()
{
    logf("menu: quit without saving");
    g_discard_session = true;
    relaunch(L"", 0, "", g_cfg.difficulty);
}

namespace menu
{
enum page_id { p_main, p_missions, p_settings, p_quit, p_games, p_setup, p_confirm, p_controls, p_pad, p_keys, p_firefight, p_spops, p_custom, p_forge, p_campaign, p_pick };
// A yes/no question: its title, a line of detail, and what Yes does.
static std::wstring g_confirm_title, g_confirm_note;
static std::function<void()> g_confirm_yes;
struct item
{
    std::wstring label, value; // value empty: not adjustable
    std::function<void()> activate;
    std::function<void(int)> adjust;
    std::wstring note;   // small text under the label
    bool enabled = true;
};

static std::recursive_mutex g_lock;
static std::atomic<bool> g_open, g_open_request;
static page_id g_page = p_main;
static int g_selected = 0;
static int g_difficulty = 1;
static std::atomic<int64_t> g_last_game_frame;

// Launcher choices
static const game_entry *g_pick_game;
static int g_pick_mission;
static std::vector<std::pair<page_id, int>> g_history; // for Back

// Input edges
enum { a_up, a_down, a_left, a_right, a_accept, a_back, a_close, k_actions };
static bool g_prev[k_actions];
static DWORD g_repeat_at[k_actions];
static POINT g_last_mouse;
static bool g_prev_click, g_prev_rclick;

// Held when the menu closed: hidden from the game until released.
static std::atomic<bool> g_masking;
static uint8_t g_masked_keys[256];
static WORD g_masked_buttons;

static D2D1_RECT_F g_item_rects[24];
static int g_item_count, g_scroll;

static int64_t now_ms() { return (int64_t)GetTickCount64(); }
static void set_mouse_capture(bool on) { if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 1, on ? 1 : 0, 0); }

static void go(page_id page, int selected = 0)
{
    g_history.push_back({g_page, g_selected});
    g_page = page, g_selected = selected, g_scroll = 0;
}

static void close_menu()
{
    if (!g_open || g_launcher) return;
    for (int vk = 1; vk < 256; vk++) g_masked_keys[vk] = key_down(vk) ? 1 : 0;
    XINPUT_STATE xs = {};
    g_masked_buttons = (g_xinput_get_state && g_xinput_get_state(0, &xs) == ERROR_SUCCESS) ? remap_pad(xs.Gamepad).wButtons : 0;
    g_masking = true;
    g_open = false;
    save_settings();
    if (g_engine && g_profile_changed)
    {
        g_engine->post_message(5, nullptr); // settings changed: the game re-reads the profile
        g_profile_changed = false;
        logf("menu: settings changed, told the game");
    }
    if (g_engine) g_engine->post_message(1, nullptr);
    if (g_last_is_km) set_mouse_capture(true);
    logf("menu: closed, game resumed");
}

static void open_menu()
{
    if (g_open) return;
    g_page = p_main, g_selected = 0, g_scroll = 0;
    g_history.clear();
    g_difficulty = g_cfg.difficulty;
    // Whatever opened the menu is still held: count it as already pressed.
    for (auto &p : g_prev) p = true;
    g_prev_click = g_prev_rclick = true;
    GetCursorPos(&g_last_mouse);
    if (g_engine) g_engine->post_message(0, nullptr);
    g_open = true;
    set_mouse_capture(false);
    logf("menu: opened, game paused");
}

static void start_launcher()
{
    g_launcher = true;
    g_open = true;
    g_page = p_games;
    g_selected = 0;
    // Start on the game played last.
    int index = 0;
    for (const auto &g : games())
    {
        if (!game_installed(g)) continue;
        if (narrow(g.folder) == g_settings.last_game) g_selected = index;
        index++;
    }
    g_difficulty = g_settings.difficulty;
    for (auto &p : g_prev) p = true;
    g_prev_click = g_prev_rclick = true;
}

// ---- Controls pages: which game they edit, and waiting for a button or key.
static std::string g_controls_game;
static int g_controls_module;
enum { listen_none, listen_pad, listen_key };
static int g_listen_kind, g_listen_index;
static int64_t g_listen_until;
static bool g_listen_held_pad[k_pad_count];
static WORD g_listen_held_buttons;
static uint8_t g_listen_held_keys[256];

static void open_controls(const std::wstring &game, int module)
{
    g_controls_game = narrow(game.c_str());
    g_controls_module = module;
    go(p_controls);
}

static void controls_changed()
{
    // The running game's controls take effect at once (keys when the menu closes).
    if (!g_launcher && narrow(g_cfg.game.c_str()) == g_controls_game) use_controls(g_controls_game, g_controls_module);
    save_settings();
}

static void start_listening(int kind, int index)
{
    g_listen_kind = kind, g_listen_index = index;
    g_listen_until = now_ms() + 6000;
    XINPUT_STATE xs = {};
    bool pad = g_xinput_get_state && g_xinput_get_state(0, &xs) == ERROR_SUCCESS;
    for (int i = 0; i < k_pad_count; i++) g_listen_held_pad[i] = pad && pad_pressed(xs.Gamepad, i);
    g_listen_held_buttons = pad ? xs.Gamepad.wButtons : 0;
    for (int vk = 1; vk < 256; vk++) g_listen_held_keys[vk] = key_down(vk) ? 1 : 0;
}

static void stop_listening()
{
    g_listen_kind = listen_none;
    for (auto &p : g_prev) p = true; // the button just pressed doesn't also work the menu
    g_prev_click = g_prev_rclick = true;
}

static void assign_pad(int target, int physical)
{
    pad_layout &l = g_pad_layouts[g_controls_game];
    int old = l.source[target];
    for (int u = 0; u < k_pad_count; u++)
        if (u != target && l.source[u] == physical) l.source[u] = old; // swap with whatever had it
    l.source[target] = physical;
    logf("controls: %s %ls -> %ls", g_controls_game.c_str(), k_pad_names[target], k_pad_names[physical]);
    controls_changed();
}

static void assign_key(int action, int vk)
{
    auto &over = g_key_overrides[g_controls_game];
    for (int a = 0; a < k_action_count; a++)
    {
        if (a == action) continue;
        std::vector<int> keys = keys_for(g_controls_game, g_controls_module, a);
        auto it = std::find(keys.begin(), keys.end(), vk);
        if (it == keys.end()) continue;
        keys.erase(it); // one key, one action
        over[a] = keys;
    }
    over[action] = {vk};
    logf("controls: %s action %d -> %ls", g_controls_game.c_str(), action, key_name(vk).c_str());
    controls_changed();
}

// While waiting: the first newly pressed button or key is the answer.
static void update_listening()
{
    if (now_ms() > g_listen_until) { stop_listening(); return; }
    XINPUT_STATE xs = {};
    bool pad = g_xinput_get_state && g_xinput_get_state(0, &xs) == ERROR_SUCCESS;
    WORD fresh = pad ? (WORD)(xs.Gamepad.wButtons & ~g_listen_held_buttons) : 0;
    if (pad) g_listen_held_buttons &= xs.Gamepad.wButtons;
    bool focused = window_active();
    auto fresh_key = [&](int vk) {
        bool d = focused && key_down(vk);
        if (!d) g_listen_held_keys[vk] = 0;
        return d && !g_listen_held_keys[vk];
    };
    if ((fresh & XINPUT_GAMEPAD_START) || fresh_key(VK_ESCAPE)) { stop_listening(); return; }
    if (g_listen_kind == listen_pad)
    {
        if (!pad) return;
        for (int i = 0; i < k_pad_count; i++)
        {
            bool d = pad_pressed(xs.Gamepad, i);
            if (!d) g_listen_held_pad[i] = false;
            else if (!g_listen_held_pad[i])
            {
                assign_pad(g_listen_index, i);
                stop_listening();
                return;
            }
        }
    }
    else
    {
        if (fresh & XINPUT_GAMEPAD_B) { stop_listening(); return; }
        for (int vk = 1; vk < 256; vk++)
        {
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_ESCAPE) continue;
            if (vk == VK_LWIN || vk == VK_RWIN) continue;
            if (fresh_key(vk))
            {
                assign_key(g_listen_index, vk);
                stop_listening();
                return;
            }
        }
    }
}

static bool game_file_exists(const game_entry *g, const char *sub, const char *file)
{
    std::string rel = std::string(sub) + "\\" + file + ".bin";
    return GetFileAttributesW((g_cfg.mcc_root + L"\\" + g->folder + L"\\" + std::wstring(rel.begin(), rel.end())).c_str()) !=
           INVALID_FILE_ATTRIBUTES;
}

// The Firefight game types whose files are installed (indexes into firefight_types()).
static std::vector<int> installed_firefight_types(const game_entry *g)
{
    std::vector<int> out;
    if (!g) return out;
    const auto &types = firefight_types(g->module);
    for (size_t i = 0; i < types.size(); i++)
        if (game_file_exists(g, "game_variants", types[i].file)) out.push_back((int)i);
    return out;
}

static std::vector<int> installed_custom_types(const game_entry *g)
{
    std::vector<int> out;
    if (!g) return out;
    const auto &types = custom_types(g->module);
    for (size_t i = 0; i < types.size(); i++)
        if (game_file_exists(g, "game_variants", types[i].file)) out.push_back((int)i);
    return out;
}

static std::vector<int> installed_spops()
{
    std::vector<int> out;
    const game_entry *g = find_game(L"halo4");
    if (!g) return out;
    for (size_t i = 0; i < spops_missions().size(); i++)
        if (game_file_exists(g, "spops_variants", spops_missions()[i].file)) out.push_back((int)i);
    return out;
}

// ---- Game types: the stock ones (named), the rest of the game's own folder,
// MCC's matchmaking set, and any the player saved in MCC on this PC.
struct variant_choice { std::string path; std::wstring name, group; };

static std::wstring pretty_variant_name(std::string f)
{
    // "h1_4v4_team_ctf_5caps_12min_pistols" -> "4v4 Team CTF 5caps 12min Pistols"
    for (const char *suffix : {"_054", "_010", "_132", "_031", "_012", "_tu", "_vanilla"})
    {
        size_t n = strlen(suffix);
        if (f.size() > n && !_stricmp(f.c_str() + f.size() - n, suffix)) f.resize(f.size() - n);
    }
    for (const char *prefix : {"h1_", "h2_", "h3_", "hr_", "h4_", "hce_", "odst_", "ff_"})
        if (f.size() > strlen(prefix) && !_strnicmp(f.c_str(), prefix, strlen(prefix))) f = f.substr(strlen(prefix));
    if (f.size() > 3 && isdigit((unsigned char)f[0]) && isdigit((unsigned char)f[1]) && f[2] == '_') f = f.substr(3);
    std::string out;
    for (size_t i = 0; i < f.size(); i++)
    {
        char c = f[i];
        if (c == '_' || c == '-') { out += ' '; continue; }
        if (i && isupper((unsigned char)c) && islower((unsigned char)f[i - 1])) out += ' ';
        out += c;
    }
    std::wstring w;
    bool start = true;
    std::string word;
    auto flush = [&]() {
        if (word.empty()) return;
        std::string lower = word;
        for (auto &ch : lower) ch = (char)tolower((unsigned char)ch);
        static const char *const upper[] = {"ctf", "ffa", "koth", "swat", "br", "ar", "dmr", "smg", "btb", "vip", "hc", "ts", "pc", "frg", "gen"};
        bool caps = false;
        for (const char *u : upper)
            if (lower == u) caps = true;
        if (caps)
            for (auto &ch : word) ch = (char)toupper((unsigned char)ch);
        else if (start || islower((unsigned char)word[0]))
            word[0] = (char)toupper((unsigned char)word[0]);
        if (!w.empty()) w += L' ';
        w += std::wstring(word.begin(), word.end());
        word.clear();
    };
    for (char c : out)
    {
        if (c == ' ') { flush(); start = false; continue; }
        word += c;
    }
    flush();
    return w.empty() ? L"Game type" : w;
}

static std::vector<std::string> list_bin_files(const std::wstring &dir)
{
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.bin").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string n = narrow(fd.cFileName);
        if (n.size() > 4) out.push_back(n.substr(0, n.size() - 4));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

static bool is_firefight_file(const std::string &f)
{
    std::string l = f;
    for (auto &c : l) c = (char)tolower((unsigned char)c);
    return l.rfind("ff_", 0) == 0 || l.find("_ff_") != std::string::npos;
}

// The player's own game types: copied into game_types\<game folder> next to the exe.
static std::vector<std::wstring> user_variant_dirs(const game_entry *g)
{
    std::vector<std::wstring> out;
    std::wstring dir = g_cfg.host_dir + L"\\game_types\\" + g->folder;
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) out.push_back(dir);
    return out;
}

static const std::vector<variant_choice> &game_variants_for(const game_entry *g, bool firefight)
{
    static std::map<std::wstring, std::vector<variant_choice>> cache;
    std::wstring key = std::wstring(g->folder) + (firefight ? L"#ff" : L"#mp");
    auto found = cache.find(key);
    if (found != cache.end()) return found->second;
    std::vector<variant_choice> &out = cache[key];
    std::set<std::string> seen;
    auto lower = [](std::string v) { for (auto &c : v) c = (char)tolower((unsigned char)c); return v; };
    const auto &stock = firefight ? firefight_types(g->module) : custom_types(g->module);
    for (const auto &t : stock)
        if (game_file_exists(g, "game_variants", t.file))
            out.push_back({std::string("game_variants\\") + t.file, t.name, L""}), seen.insert(lower(t.file));
    std::wstring base = g_cfg.mcc_root + L"\\" + g->folder + L"\\";
    const char *forge = forge_type(g->module);
    auto wanted = [&](const std::string &f) {
        if (seen.count(lower(f))) return false;
        if (forge && !_stricmp(f.c_str(), forge)) return false;
        if (firefight != is_firefight_file(f)) return false;
        if (!firefight && (lower(f).find("versus") != std::string::npos)) return true;
        return true;
    };
    for (const auto &f : list_bin_files(base + L"game_variants"))
        if (wanted(f)) out.push_back({"game_variants\\" + f, pretty_variant_name(f), L"More"}), seen.insert(lower(f));
    for (const auto &f : list_bin_files(base + L"hopper_game_variants"))
        if (wanted(f)) out.push_back({"hopper_game_variants\\" + f, pretty_variant_name(f), L"Matchmaking"}), seen.insert(lower(f));
    if (!firefight)
        for (const auto &dir : user_variant_dirs(g))
            for (const auto &f : list_bin_files(dir))
                out.push_back({narrow(dir.c_str()) + "\\" + f, pretty_variant_name(f), L"Your game types"});
    return out;
}

// ---- A list to choose one thing from (missions, maps, game types).
static std::wstring g_pick_title;
static std::vector<std::pair<std::wstring, std::wstring>> g_pick_list; // label, small note
static std::function<void(int)> g_pick_done;
static void open_picker(const std::wstring &title, std::vector<std::pair<std::wstring, std::wstring>> list, int current,
                        std::function<void(int)> done)
{
    g_pick_title = title, g_pick_list = std::move(list), g_pick_done = std::move(done);
    go(p_pick, std::max(0, current));
}

static int find_variant(const std::vector<variant_choice> &v, const std::string &path)
{
    for (size_t i = 0; i < v.size(); i++)
        if (!_stricmp(v[i].path.c_str(), path.c_str())) return (int)i;
    return 0;
}

static std::vector<std::pair<std::wstring, std::wstring>> variant_list(const std::vector<variant_choice> &v)
{
    std::vector<std::pair<std::wstring, std::wstring>> out;
    for (const auto &c : v) out.push_back({c.name, c.group});
    return out;
}
static std::vector<std::pair<std::wstring, std::wstring>> map_list(const std::vector<mission_entry> &v)
{
    std::vector<std::pair<std::wstring, std::wstring>> out;
    for (const auto &m : v) out.push_back({m.name, L""});
    return out;
}

static void pick_game(const game_entry *g)
{
    g_pick_game = g;
    g_pick_mission = 0;
    auto it = g_settings.last_map.find(narrow(g->folder));
    for (size_t i = 0; i < g->missions.size(); i++)
        if (it != g_settings.last_map.end() && g->missions[i].map == it->second) g_pick_mission = (int)i;
    go(p_setup, 0);
}

// What a game offers, for its line on the game list.
static std::wstring game_modes(const game_entry *g)
{
    static std::map<const game_entry *, std::wstring> cache; // the files don't change while we run
    auto it = cache.find(g);
    if (it != cache.end()) return it->second;
    std::wstring &s = cache[g];
    s = L"Campaign";
    if (!g->firefight.empty() && !installed_firefight_types(g).empty()) s += L"  •  Firefight";
    if (g->module == 3 && !installed_spops().empty()) s += L"  •  Spartan Ops";
    if (!custom_maps(g->module).empty() && !installed_custom_types(g).empty()) s += L"  •  Custom games";
    if (forge_type(g->module) && game_file_exists(g, "game_variants", forge_type(g->module))) s += L"  •  Forge";
    return s;
}

static std::wstring pct(int v) { return std::to_wstring(v) + L"%"; }
static void step(int &v, int dir, int lo, int hi, int by) { v = std::max(lo, std::min(hi, v + dir * by)); }

static void go_back_one()
{
    if (!g_history.empty())
        g_page = g_history.back().first, g_selected = g_history.back().second, g_history.pop_back();
}

// Players (split-screen) and who has the keyboard: on every mode's page.
static void add_player_items(std::vector<item> &items)
{
    {
        item pl{L"Players", std::to_wstring(std::max(1, std::min(4, g_settings.players))), nullptr, [](int d) {
                        g_settings.players = (std::max(1, std::min(4, g_settings.players)) - 1 + d + 4) % 4 + 1;
                        save_settings();
                    }};
            pl.activate = [] { g_settings.players = std::max(1, std::min(4, g_settings.players)) % 4 + 1; save_settings(); };
            pl.note = g_settings.players > 1 ? L"Split-screen" : L"";
            items.push_back(pl);
            if (g_settings.players > 1)
            {
                item kb{L"Player 1 uses", g_settings.keyboard_player ? L"Keyboard and mouse" : L"Controller 1 (or keyboard)",
                        [] { g_settings.keyboard_player ^= 1; save_settings(); },
                        [](int) { g_settings.keyboard_player ^= 1; save_settings(); }};
                items.push_back(kb);
            }
        }
}
static int player_item_count() { return g_settings.players > 1 ? 2 : 1; }

static std::vector<item> build_items()
{
    std::vector<item> items;
    switch (g_page)
    {
    case p_games:
        for (const auto &g : games())
        {
            if (!game_installed(g)) continue;
            const game_entry *gp = &g;
            item i{g.title, L"", [gp] { pick_game(gp); }, nullptr};
            i.note = game_modes(gp);
            items.push_back(i);
        }
        if (items.empty())
        {
            item i{L"No Halo games found", L"", nullptr, nullptr};
            if (g_cfg.mcc_given)
            {
                // Under Wine, Z: is the host's root: show the folder as the console sees it.
                std::wstring where = g_cfg.mcc_root;
                if (running_under_wine() && where.size() > 2 && (where[0] == L'Z' || where[0] == L'z') && where[1] == L':')
                {
                    where = where.substr(2);
                    for (auto &c : where) if (c == L'\\') c = L'/';
                }
                i.note = L"Copy your MCC folders (halo1, halo2, halo3, halo3odst, haloreach, halo4) into " + where;
            }
            else
                i.note = L"Put installation08.exe in the Halo The Master Chief Collection folder, or start it with --mcc <folder>";
            i.enabled = false;
            items.push_back(i);
        }
        items.push_back({L"Settings", L"", [] { go(p_settings); }, nullptr});
        items.push_back({L"Quit", L"", [] { go(p_quit, 1); }, nullptr});
        break;
    case p_pick:
        for (size_t i = 0; i < g_pick_list.size(); i++)
        {
            int index = (int)i;
            item it{g_pick_list[i].first, L"", [index] {
                        auto done = g_pick_done;
                        go_back_one();
                        if (done) done(index);
                    }, nullptr};
            // A group's name on its first entry only, so the list reads as sections.
            if (!g_pick_list[i].second.empty() && (i == 0 || g_pick_list[i - 1].second != g_pick_list[i].second))
                it.note = g_pick_list[i].second;
            items.push_back(it);
        }
        break;
    case p_setup: // a game's page: its modes
    {
        const game_entry *g = g_pick_game;
        save_info save = read_save_info(g->folder);
        if (save.exists)
        {
            std::wstring where;
            for (const auto &mm : g->missions)
                if (mm.map == save.map) where = mm.name;
            std::wstring note = (where.empty() ? L"Saved game" : where) + L"  •  " + difficulty_name(save.difficulty) +
                                (save.saved.empty() ? L"" : L"  •  " + std::wstring(save.saved.begin(), save.saved.end()));
            item c{L"Continue campaign", L"", [g, note] {
                       g_confirm_title = L"CONTINUE FROM LAST SAVE?";
                       g_confirm_note = note;
                       g_confirm_yes = [g] { continue_from_save(g->folder); };
                       go(p_confirm, 0);
                   }, nullptr};
            c.note = note;
            items.push_back(c);
        }
        items.push_back({L"Campaign", L"", [g] {
                             go(p_campaign, 2 + (g->module <= 1 ? 1 : 0) + player_item_count());
                         }, nullptr});
        if (!g->firefight.empty() && !installed_firefight_types(g).empty())
            items.push_back({L"Firefight", L"", [] { go(p_firefight, 3 + player_item_count()); }, nullptr});
        if (g->module == 3 && !installed_spops().empty())
            items.push_back({L"Spartan Ops", L"", [] { go(p_spops, 3 + player_item_count()); }, nullptr});
        if (!custom_maps(g->module).empty() && !installed_custom_types(g).empty())
            items.push_back({L"Custom game", L"", [] { go(p_custom, 2 + player_item_count()); }, nullptr});
        if (forge_type(g->module) && game_file_exists(g, "game_variants", forge_type(g->module)))
            items.push_back({L"Forge", L"", [] { go(p_forge, 1 + player_item_count()); }, nullptr});
        items.push_back({L"Controls", L"", [g] { open_controls(g->folder, g->module); }, nullptr});
        break;
    }
    case p_campaign:
    {
        const game_entry *g = g_pick_game;
        const mission_entry &m = g->missions[g_pick_mission];
        items.push_back({L"Mission", m.name, [g] {
                             open_picker(L"SELECT MISSION", map_list(g->missions), g_pick_mission,
                                         [](int i) { g_pick_mission = i; });
                         }, [g](int d) {
                             int n = (int)g->missions.size();
                             g_pick_mission = (g_pick_mission + d + n) % n;
                         }});
        items.push_back({L"Difficulty", difficulty_name(g_difficulty), [] { g_difficulty = (g_difficulty + 1) % 4; },
                         [](int d) { g_difficulty = (g_difficulty + d + 4) % 4; }});
        if (g->module <= 1) // Halo CE and Halo 2 have both graphics
        {
            int *classic = g->module == 0 ? &g_settings.h1_classic : &g_settings.h2_classic;
            item gr{L"Graphics", *classic ? L"Classic" : L"Anniversary", [classic] { *classic ^= 1; save_settings(); },
                    [classic](int) { *classic ^= 1; save_settings(); }};
            gr.note = std::wstring(btn(L"Back")) + L" switches between them in the game";
            items.push_back(gr);
        }
        add_player_items(items);
        items.push_back({L"Start campaign", L"", [g] {
                             const mission_entry &mm = g->missions[g_pick_mission];
                             relaunch(g->folder, mm.map, mm.file, g_difficulty);
                         }, nullptr});
        break;
    }
    case p_firefight:
    {
        const game_entry *g = g_pick_game;
        const auto &types = game_variants_for(g, true);
        if (types.empty() || g->firefight.empty()) break;
        std::string key = narrow(g->folder);
        int *mi = &g_settings.ff_map[key];
        int nm = (int)g->firefight.size();
        *mi = ((*mi % nm) + nm) % nm;
        int ti = find_variant(types, g_settings.ff_variant[key]);
        items.push_back({L"Map", g->firefight[*mi].name, [g, mi] {
                             open_picker(L"SELECT MAP", map_list(g->firefight), *mi, [mi](int i) { *mi = i; save_settings(); });
                         }, [mi, nm](int d) { *mi = (*mi + d + nm) % nm; save_settings(); }});
        item gt{L"Game type", types[ti].name, [g, key, ti] {
                    open_picker(L"SELECT GAME TYPE", variant_list(game_variants_for(g, true)), ti, [g, key](int i) {
                        g_settings.ff_variant[key] = game_variants_for(g, true)[i].path;
                        save_settings();
                    });
                }, [g, key, ti](int d) {
                    const auto &v = game_variants_for(g, true);
                    g_settings.ff_variant[key] = v[(ti + d + (int)v.size()) % (int)v.size()].path;
                    save_settings();
                }};
        gt.note = types[ti].group;
        items.push_back(gt);
        items.push_back({L"Difficulty", difficulty_name(g_difficulty), [] { g_difficulty = (g_difficulty + 1) % 4; },
                         [](int d) { g_difficulty = (g_difficulty + d + 4) % 4; }});
        add_player_items(items);
        items.push_back({L"Start Firefight", L"", [g, mi, ti] {
                             const mission_entry &m = g->firefight[*mi];
                             relaunch_mode(5, g->folder, m.map, m.file, game_variants_for(g, true)[ti].path, g_difficulty);
                         }, nullptr});
        break;
    }
    case p_custom:
    {
        const game_entry *g = g_pick_game;
        const auto &types = game_variants_for(g, false);
        const auto &maps = custom_maps(g->module);
        if (types.empty() || maps.empty()) break;
        std::string key = narrow(g->folder);
        int *mi = &g_settings.cg_map[key];
        int nm = (int)maps.size();
        *mi = ((*mi % nm) + nm) % nm;
        int ti = find_variant(types, g_settings.cg_variant[key]);
        items.push_back({L"Map", maps[*mi].name, [g, mi] {
                             open_picker(L"SELECT MAP", map_list(custom_maps(g->module)), *mi, [mi](int i) { *mi = i; save_settings(); });
                         }, [mi, nm](int d) { *mi = (*mi + d + nm) % nm; save_settings(); }});
        item gt{L"Game type", types[ti].name, [g, key, ti] {
                    open_picker(L"SELECT GAME TYPE", variant_list(game_variants_for(g, false)), ti, [g, key](int i) {
                        g_settings.cg_variant[key] = game_variants_for(g, false)[i].path;
                        save_settings();
                    });
                }, [g, key, ti](int d) {
                    const auto &v = game_variants_for(g, false);
                    g_settings.cg_variant[key] = v[(ti + d + (int)v.size()) % (int)v.size()].path;
                    save_settings();
                }};
        gt.note = types[ti].group;
        items.push_back(gt);
        add_player_items(items);
        items.push_back({L"Start game", L"", [g, mi, ti] {
                             const mission_entry &m = custom_maps(g->module)[*mi];
                             relaunch_mode(3, g->folder, m.map, m.file, game_variants_for(g, false)[ti].path, g_difficulty);
                         }, nullptr});
        break;
    }
    case p_forge:
    {
        const game_entry *g = g_pick_game;
        const auto &maps = custom_maps(g->module);
        if (maps.empty() || !forge_type(g->module)) break;
        int *mi = &g_settings.cg_map[narrow(g->folder) + "_forge"];
        int nm = (int)maps.size();
        *mi = ((*mi % nm) + nm) % nm;
        items.push_back({L"Map", maps[*mi].name, [g, mi] {
                             open_picker(L"SELECT MAP", map_list(custom_maps(g->module)), *mi, [mi](int i) { *mi = i; save_settings(); });
                         }, [mi, nm](int d) { *mi = (*mi + d + nm) % nm; save_settings(); }});
        add_player_items(items);
        items.push_back({L"Start Forge", L"", [g, mi] {
                             const mission_entry &m = custom_maps(g->module)[*mi];
                             relaunch_mode(3, g->folder, m.map, m.file, std::string("game_variants\\") + forge_type(g->module),
                                           g_difficulty);
                         }, nullptr});
        break;
    }
    case p_spops:
    {
        std::vector<int> have = installed_spops();
        if (have.empty()) break;
        const auto &all = spops_missions();
        int n = (int)have.size(), at = 0;
        for (int i = 0; i < n; i++)
            if (have[i] == g_settings.spops) at = i;
        g_settings.spops = have[at];
        const spops_mission &m = all[g_settings.spops];
        int chapter = 1;
        for (int i = 0; i < g_settings.spops; i++)
            if (all[i].episode == m.episode) chapter++;
        auto step = [have, n](int d) {
            int i = 0;
            for (int k = 0; k < n; k++)
                if (have[k] == g_settings.spops) i = k;
            g_settings.spops = have[(i + d + n) % n];
            save_settings();
        };
        auto step_episode = [have, n](int d) {
            const auto &all = spops_missions();
            int ep = all[g_settings.spops].episode + d;
            ep = ep < 1 ? 10 : ep > 10 ? 1 : ep;
            for (int k = 0; k < n; k++)
                if (all[have[k]].episode == ep) { g_settings.spops = have[k]; break; }
            save_settings();
        };
        items.push_back({L"Episode", std::to_wstring(m.episode), [step_episode] { step_episode(1); }, step_episode});
        item mi{L"Mission", std::to_wstring(chapter) + L"  \u2022  " + m.map_name, [step] { step(1); }, step};
        items.push_back(mi);
        items.push_back({L"Difficulty", difficulty_name(g_difficulty), [] { g_difficulty = (g_difficulty + 1) % 4; },
                         [](int d) { g_difficulty = (g_difficulty + d + 4) % 4; }});
        add_player_items(items);
        items.push_back({L"Start mission", L"", [] {
                             const spops_mission &m = spops_missions()[g_settings.spops];
                             char file[64];
                             snprintf(file, sizeof file, "spops_e%d", m.episode);
                             relaunch_mode(2, L"halo4", m.map, file, std::string("spops_variants\\") + m.file, g_difficulty);
                         }, nullptr});
        break;
    }
    case p_main:
        items.push_back({L"Resume", L"", [] { close_menu(); }, nullptr});
        if (g_mode != 1)
        {
            items.push_back({g_mode == 5 ? L"Restart Firefight" : g_mode == 3 ? L"Restart game" : L"Restart mission", L"", [] {
                                 relaunch_mode(g_mode, g_cfg.game, g_cfg.map, narrow(g_cfg.map_file.c_str()), g_ff_variant,
                                               g_difficulty);
                             }, nullptr});
            if (g_mode != 3)
            {
                item d{L"Difficulty", difficulty_name(g_difficulty), [] { g_difficulty = (g_difficulty + 1) % 4; },
                       [](int dir) { g_difficulty = (g_difficulty + dir + 4) % 4; }};
                d.note = L"Takes effect when you restart";
                items.push_back(d);
            }
            items.push_back({L"Settings", L"", [] { go(p_settings); }, nullptr});
            items.push_back({L"Controls", L"", [] { open_controls(g_cfg.game, g_module); }, nullptr});
            items.push_back({L"Return to main menu", L"", [] { relaunch(L"", 0, "", g_difficulty); }, nullptr});
            items.push_back({L"Quit to desktop", L"", [] { go(p_quit, 1); }, nullptr});
            break;
        }
        items.push_back({L"Revert to last checkpoint", L"", [] {
                             close_menu();
                             if (g_engine) g_engine->post_command("HS: game_revert");
                             logf("menu: revert to checkpoint");
                         }, nullptr});
        items.push_back({L"Restart mission", L"", [] {
                             const mission_entry *m = current_mission();
                             if (m) relaunch(g_cfg.game, m->map, m->file, g_difficulty);
                             else relaunch(g_cfg.game, g_cfg.map, narrow(g_cfg.map_file.c_str()), g_difficulty);
                         }, nullptr});
        if (find_game(g_cfg.game)) items.push_back({L"Select mission", L"", [] { go(p_missions); }, nullptr});
        {
            item d{L"Difficulty", difficulty_name(g_difficulty), [] { g_difficulty = (g_difficulty + 1) % 4; },
                   [](int dir) { g_difficulty = (g_difficulty + dir + 4) % 4; }};
            d.note = L"Takes effect when the mission restarts";
            items.push_back(d);
        }
        items.push_back({L"Settings", L"", [] { go(p_settings); }, nullptr});
        items.push_back({L"Controls", L"", [] { open_controls(g_cfg.game, g_module); }, nullptr});
        {
            item sq{L"Save and quit", L"", [] {
                        g_confirm_title = L"SAVE AND QUIT?";
                        g_confirm_note = L"Saves your progress and returns to the main menu";
                        g_confirm_yes = [] { start_save_and_quit(); };
                        go(p_confirm, 0);
                    }, nullptr};
            items.push_back(sq);
        }
        {
            item qn{L"Quit without saving", L"", [] {
                        g_confirm_title = L"QUIT WITHOUT SAVING?";
                        g_confirm_note = L"Returns to the main menu. Continue will pick up where you were before this session";
                        g_confirm_yes = [] { quit_without_saving(); };
                        go(p_confirm, 1);
                    }, nullptr};
            items.push_back(qn);
        }
        items.push_back({L"Return to main menu", L"", [] { relaunch(L"", 0, "", g_difficulty); }, nullptr});
        items.push_back({L"Quit to desktop", L"", [] { go(p_quit, 1); }, nullptr});
        break;
    case p_missions:
    {
        const game_entry *g = g_launcher ? g_pick_game : find_game(g_cfg.game);
        if (!g) break;
        for (size_t i = 0; i < g->missions.size(); i++)
        {
            const mission_entry *mp = &g->missions[i];
            int index = (int)i;
            (void)index;
            items.push_back({mp->name, L"", [g, mp] { relaunch(g->folder, mp->map, mp->file, g_difficulty); },
                                 nullptr});
        }
        break;
    }
    case p_settings:
    {
        auto adj = [](int *v, int lo, int hi, int by) {
            return [v, lo, hi, by](int d) { step(*v, d, lo, hi, by); apply_settings(); };
        };
        items.push_back({L"Master volume", pct(g_settings.master), nullptr, adj(&g_settings.master, 0, 100, 5)});
        items.push_back({L"Music volume", pct(g_settings.music), nullptr, adj(&g_settings.music, 0, 100, 5)});
        items.push_back({L"Effects volume", pct(g_settings.effects), nullptr, adj(&g_settings.effects, 0, 100, 5)});
        wchar_t sens[16];
        swprintf(sens, 16, L"%.1f", g_settings.mouse);
        items.push_back({L"Mouse sensitivity", sens, nullptr, [](int d) {
                             g_settings.mouse = std::max(0.1f, std::min(5.0f, g_settings.mouse + d * 0.1f));
                             apply_settings();
                         }});
        items.push_back({L"Invert look (controller)", g_settings.invert_look ? L"On" : L"Off", nullptr,
                         [](int) { g_settings.invert_look ^= 1; apply_settings(); }});
        items.push_back({L"Invert mouse", g_settings.invert_mouse ? L"On" : L"Off", nullptr,
                         [](int) { g_settings.invert_mouse ^= 1; apply_settings(); }});
        items.push_back({L"Field of view", std::to_wstring(g_settings.fov), nullptr, adj(&g_settings.fov, 65, 120, 1)});
        {
            const wchar_t *styles[] = {L"Auto", L"Xbox", L"PlayStation"};
            std::wstring v = styles[g_settings.button_style % 3];
            if (g_settings.button_style == 0) v += playstation_labels() ? L" (PlayStation)" : L" (Xbox)";
            item b{L"Button labels", v, nullptr, [](int d) {
                       g_settings.button_style = (g_settings.button_style + d + 3) % 3;
                       save_settings();
                   }};
            b.note = L"How this menu names controller buttons";
            items.push_back(b);
        }
        {
            item res{L"Resolution", resolution_name(g_settings.resolution), nullptr, [](int d) {
                         g_settings.resolution = (g_settings.resolution + d + k_resolution_count) % k_resolution_count;
                         save_settings();
                     }};
            res.note = L"Takes effect when Installation 08 next starts";
            items.push_back(res);
            item win{L"Display", g_settings.windowed ? L"Window" : L"Fullscreen", nullptr, [](int) {
                         g_settings.windowed ^= 1;
                         save_settings();
                     }};
            win.note = L"Takes effect when Installation 08 next starts";
            items.push_back(win);
        }
        for (auto &i : items)
            if (!i.activate) { auto a = i.adjust; i.activate = [a] { a(1); }; }
        break;
    }
    case p_controls:
    {
        item pad{L"Controller", L"", [] { go(p_pad); }, nullptr};
        pad.note = g_pad_layouts.count(g_controls_game) && !g_pad_layouts[g_controls_game].is_default() ? L"Custom layout" : L"Default layout";
        items.push_back(pad);
        items.push_back({L"Keyboard and mouse", L"", [] { go(p_keys); }, nullptr});
        break;
    }
    case p_pad:
    {
        const pad_layout l = g_pad_layouts.count(g_controls_game) ? g_pad_layouts[g_controls_game] : pad_layout();
        for (int t = 0; t < k_pad_count; t++)
        {
            bool waiting = g_listen_kind == listen_pad && g_listen_index == t;
            std::wstring value = waiting ? L"Press a button..." : pad_name(l.source[t]);
            item i{pad_name(t), value, [t] { start_listening(listen_pad, t); }, nullptr};
            if (t == 0) i.note = L"Select a button, then press the one you want to use in its place";
            items.push_back(i);
        }
        items.push_back({L"Swap sticks", l.swap_sticks ? L"On" : L"Off", [] {
                             g_pad_layouts[g_controls_game].swap_sticks ^= 1;
                             controls_changed();
                         }, [](int) {
                             g_pad_layouts[g_controls_game].swap_sticks ^= 1;
                             controls_changed();
                         }});
        items.push_back({L"Reset controller to default", L"", [] {
                             g_pad_layouts.erase(g_controls_game);
                             controls_changed();
                         }, nullptr});
        break;
    }
    case p_keys:
    {
        for (const auto &a : key_actions(g_controls_module))
        {
            bool waiting = g_listen_kind == listen_key && g_listen_index == a.action;
            std::wstring value = waiting ? L"Press a key..." : keys_label(keys_for(g_controls_game, g_controls_module, a.action));
            int action = a.action;
            item i{a.label, value, [action] { start_listening(listen_key, action); }, nullptr};
            items.push_back(i);
        }
        items[0].note = L"Select one, then press the key or mouse button you want for it";
        items.push_back({L"Reset keyboard to default", L"", [] {
                             g_key_overrides.erase(g_controls_game);
                             controls_changed();
                         }, nullptr});
        break;
    }
    case p_confirm:
    {
        item yes{L"Yes", L"", [] {
                     auto action = g_confirm_yes;
                     if (!g_history.empty()) g_page = g_history.back().first, g_selected = g_history.back().second,
                                             g_history.pop_back();
                     if (action) action();
                 }, nullptr};
        yes.note = g_confirm_note;
        items.push_back(yes);
        items.push_back({L"No", L"", [] { go_back_one(); }, nullptr});
        break;
    }
    case p_quit:
        items.push_back({L"Quit", L"", [] {
                             logf("menu: quit");
                             request_quit();
                         }, nullptr});
        items.push_back({L"Cancel", L"", [] { if (!g_history.empty()) g_page = g_history.back().first, g_selected = g_history.back().second, g_history.pop_back(); }, nullptr});
        break;
    }
    return items;
}

static void go_back()
{
    if (!g_history.empty())
    {
        g_page = g_history.back().first, g_selected = g_history.back().second, g_scroll = 0;
        g_history.pop_back();
    }
    else if (g_launcher) go(p_quit, 1);
    else close_menu();
}

// One frame of menu input.
static void update()
{
    if (g_listen_kind) { update_listening(); return; }
    bool now[k_actions] = {};
    XINPUT_STATE xs = {};
    if (g_xinput_get_state && g_xinput_get_state(0, &xs) == ERROR_SUCCESS)
    {
        const XINPUT_GAMEPAD &p = xs.Gamepad;
        now[a_up] |= (p.wButtons & XINPUT_GAMEPAD_DPAD_UP) || p.sThumbLY > 16000;
        now[a_down] |= (p.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || p.sThumbLY < -16000;
        now[a_left] |= (p.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || p.sThumbLX < -16000;
        now[a_right] |= (p.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || p.sThumbLX > 16000;
        now[a_accept] |= (p.wButtons & XINPUT_GAMEPAD_A) != 0;
        now[a_back] |= (p.wButtons & XINPUT_GAMEPAD_B) != 0;
        now[a_close] |= (p.wButtons & XINPUT_GAMEPAD_START) != 0;
    }
    bool focused = window_active();
    if (focused)
    {
        auto down = [](int vk) { return key_down(vk); };
        now[a_up] |= down(VK_UP) || down('W');
        now[a_down] |= down(VK_DOWN) || down('S');
        now[a_left] |= down(VK_LEFT) || down('A');
        now[a_right] |= down(VK_RIGHT) || down('D');
        now[a_accept] |= down(VK_RETURN) || down(VK_SPACE);
        now[a_back] |= down(VK_BACK);
        now[a_close] |= down(VK_ESCAPE);
    }
    DWORD t = GetTickCount();
    bool fire[k_actions] = {};
    for (int a = 0; a < k_actions; a++)
    {
        if (now[a] && !g_prev[a]) fire[a] = true, g_repeat_at[a] = t + 350;
        else if (now[a] && a <= a_right && (int)(t - g_repeat_at[a]) >= 0) fire[a] = true, g_repeat_at[a] = t + 90;
        g_prev[a] = now[a];
    }
    // In the launcher, Start/Esc is the same as Back.
    if (g_launcher && fire[a_close]) fire[a_back] = true, fire[a_close] = false;

    std::vector<item> items = build_items();
    int n = (int)items.size();
    if (n == 0) { go_back(); return; }
    if (fire[a_up]) g_selected = (g_selected + n - 1) % n;
    if (fire[a_down]) g_selected = (g_selected + 1) % n;
    if (g_selected >= n) g_selected = n - 1;
    if (g_selected < 0) g_selected = 0;

    // Mouse: hover selects, left click activates, right click steps a value back.
    POINT pt;
    bool click = false, rclick = false;
    if (focused && GetCursorPos(&pt))
    {
        bool moved = pt.x != g_last_mouse.x || pt.y != g_last_mouse.y;
        g_last_mouse = pt;
        POINT cp = pt;
        ScreenToClient(g_hwnd, &cp);
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        DXGI_SWAP_CHAIN_DESC sd;
        float sx = 1, sy = 1;
        if (g_swapchain && SUCCEEDED(g_swapchain->GetDesc(&sd)) && rc.right > 0 && rc.bottom > 0)
            sx = (float)sd.BufferDesc.Width / rc.right, sy = (float)sd.BufferDesc.Height / rc.bottom;
        float x = cp.x * sx, y = cp.y * sy;
        int hit = -1;
        for (int i = 0; i < g_item_count; i++)
            if (x >= g_item_rects[i].left && x <= g_item_rects[i].right && y >= g_item_rects[i].top &&
                y <= g_item_rects[i].bottom)
                hit = i + g_scroll;
        if (hit >= n) hit = -1;
        if (hit >= 0 && moved) g_selected = hit;
        bool l = key_down(VK_LBUTTON), r = key_down(VK_RBUTTON);
        click = l && !g_prev_click && hit >= 0;
        rclick = r && !g_prev_rclick && hit >= 0;
        if (click || rclick) g_selected = hit;
        g_prev_click = l, g_prev_rclick = r;
    }

    item &cur = items[g_selected];
    if ((fire[a_left] || rclick) && cur.adjust) cur.adjust(-1);
    else if (fire[a_right] && cur.adjust) cur.adjust(1);
    else if (fire[a_accept] || click) { if (cur.activate && cur.enabled) cur.activate(); }
    else if (fire[a_back]) go_back();
    else if (fire[a_close]) close_menu();
}

// ------------------------------------------------ drawing
static ID2D1Factory *g_d2d;
static IDWriteFactory *g_dwrite;
static ID2D1RenderTarget *g_rt;
static IDWriteTextFormat *g_title_format, *g_item_format, *g_small_format, *g_value_format, *g_huge_format;
static float g_formats_scale;

static IDWriteTextFormat *make_format(const wchar_t *family, float size, DWRITE_FONT_WEIGHT weight,
                                      DWRITE_TEXT_ALIGNMENT align)
{
    IDWriteTextFormat *f = nullptr;
    g_dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                               L"en-us", &f);
    if (f)
    {
        f->SetTextAlignment(align);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    return f;
}

static bool ensure_target(IDXGISwapChain *chain, float scale)
{
    if (!g_d2d)
    {
        D2D1_FACTORY_OPTIONS opts = {};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory), &opts,
                                     (void **)&g_d2d)))
            return false;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&g_dwrite);
    }
    if (!g_rt)
    {
        IDXGISurface *surface = nullptr;
        if (FAILED(chain->GetBuffer(0, __uuidof(IDXGISurface), (void **)&surface))) return false;
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        HRESULT hr = g_d2d->CreateDxgiSurfaceRenderTarget(surface, &props, &g_rt);
        surface->Release();
        if (FAILED(hr)) { logf("menu: cannot draw on the frames (%08lx)", (unsigned long)hr); return false; }
        g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    if (g_dwrite && g_formats_scale != scale)
    {
        for (auto f : {g_title_format, g_item_format, g_small_format, g_value_format, g_huge_format})
            if (f) f->Release();
        g_huge_format = make_format(L"Bahnschrift", 84 * scale, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
        g_title_format = make_format(L"Bahnschrift", 56 * scale, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
        g_item_format = make_format(L"Bahnschrift", 30 * scale, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
        g_value_format = make_format(L"Bahnschrift", 30 * scale, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
        g_small_format = make_format(L"Bahnschrift", 22 * scale, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
        g_formats_scale = scale;
    }
    return g_title_format && g_item_format && g_small_format && g_value_format && g_huge_format;
}

static void text(const std::wstring &s, IDWriteTextFormat *f, D2D1_RECT_F r, ID2D1Brush *b)
{
    g_rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, b, D2D1_DRAW_TEXT_OPTIONS_NONE);
}

static std::wstring page_title()
{
    switch (g_page)
    {
    case p_games: return L"";
    case p_setup: return g_pick_game ? g_pick_game->title : L"";
    case p_campaign: return L"CAMPAIGN";
    case p_pick: return g_pick_title;
    case p_main: return L"PAUSED";
    case p_missions: return L"SELECT MISSION";
    case p_settings: return L"SETTINGS";
    case p_quit: return L"QUIT TO DESKTOP?";
    case p_confirm: return g_confirm_title;
    case p_controls: return L"CONTROLS";
    case p_firefight: return L"FIREFIGHT";
    case p_spops: return L"SPARTAN OPS";
    case p_custom: return L"CUSTOM GAME";
    case p_forge: return L"FORGE";
    case p_pad: return L"CONTROLLER";
    case p_keys: return L"KEYBOARD AND MOUSE";
    }
    return L"";
}

static void draw(IDXGISwapChain *chain, bool opaque)
{
    DXGI_SWAP_CHAIN_DESC sd;
    if (FAILED(chain->GetDesc(&sd))) return;
    float w = (float)sd.BufferDesc.Width, h = (float)sd.BufferDesc.Height, s = h / 1080.0f;
    if (!ensure_target(chain, s)) return;

    ID2D1SolidColorBrush *white, *dim, *faint, *accent, *accent_fill, *shade, *panel;
    g_rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.78f, 0.84f, 0.9f, 0.8f), &dim);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.78f, 0.84f, 0.9f, 0.45f), &faint);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.36f, 0.78f, 1.0f, 1), &accent);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.36f, 0.78f, 1.0f, 0.22f), &accent_fill);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.55f), &shade);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.02f, 0.06f, 0.1f, 0.6f), &panel);

    g_rt->BeginDraw();
    float left = 140 * s, col = 760 * s, top = 170 * s;
    if (opaque || g_launcher)
    {
        // The launcher's backdrop: a dark blue gradient with a faint horizon line.
        ID2D1GradientStopCollection *stops = nullptr;
        D2D1_GRADIENT_STOP gs[3] = {{0.0f, D2D1::ColorF(0.03f, 0.07f, 0.12f, 1)},
                                    {0.6f, D2D1::ColorF(0.02f, 0.04f, 0.07f, 1)},
                                    {1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 1)}};
        g_rt->CreateGradientStopCollection(gs, 3, &stops);
        ID2D1LinearGradientBrush *bg = nullptr;
        if (stops)
            g_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(w * 0.3f, h)),
                                            stops, &bg);
        if (bg) g_rt->FillRectangle(D2D1::RectF(0, 0, w, h), bg), bg->Release();
        if (stops) stops->Release();
        if (g_launcher)
        {
            text(L"INSTALLATION 08", g_huge_format, D2D1::RectF(left, 60 * s, w - left, 170 * s), white);
            text(L"The Master Chief Collection, offline  \u2022  version 1.1", g_small_format, D2D1::RectF(left + 4 * s, 160 * s, w - left, 195 * s), faint);
            top = 240 * s;
        }
    }
    else
    {
        g_rt->FillRectangle(D2D1::RectF(0, 0, w, h), shade);
    }

    text(page_title(), g_title_format, D2D1::RectF(left, top, left + col + 400 * s, top + 70 * s), white);
    std::wstring sub;
    if (g_launcher)
    {
        if (g_page == p_setup) sub = L"";
        else if (g_page == p_campaign || g_page == p_missions || g_page == p_pick) sub = g_pick_game ? g_pick_game->title : L"";
        else if ((g_page == p_firefight || g_page == p_spops || g_page == p_custom || g_page == p_forge || g_page == p_controls ||
                  g_page == p_pad || g_page == p_keys) && g_pick_game)
            sub = g_pick_game->title;
        else if (g_page == p_games) sub = L"";
        else if ((g_page == p_controls || g_page == p_pad || g_page == p_keys) && g_pick_game) sub = g_pick_game->title;
    }
    else
    {
        const mission_entry *m = current_mission();
        const game_entry *g = find_game(g_cfg.game);
        sub = (g ? std::wstring(g->title) + L"  •  " : L"") + (m ? std::wstring(m->name) + L"  •  " : L"") +
              difficulty_name(g_cfg.difficulty);
    }
    text(sub, g_small_format, D2D1::RectF(left, top + 70 * s, w - left, top + 105 * s), dim);
    g_rt->FillRectangle(D2D1::RectF(left, top + 118 * s, left + col, top + 120 * s), accent);

    std::vector<item> items = build_items();
    float y0 = top + 145 * s, row = 54 * s;
    int visible = std::max(3, (int)((h - 160 * s - y0) / (row + 4 * s)));
    visible = std::min(visible, 24);
    if (g_selected < g_scroll) g_scroll = g_selected;
    if (g_selected >= g_scroll + visible) g_scroll = g_selected - visible + 1;
    if (g_scroll > std::max(0, (int)items.size() - visible)) g_scroll = std::max(0, (int)items.size() - visible);
    g_item_count = 0;
    float y = y0;
    for (int i = g_scroll; i < (int)items.size() && g_item_count < visible; i++)
    {
        float rh = items[i].note.empty() ? row : row + 26 * s;
        D2D1_RECT_F r = D2D1::RectF(left, y, left + col, y + rh);
        g_item_rects[g_item_count++] = r;
        bool sel = i == g_selected;
        if (sel)
        {
            g_rt->FillRectangle(r, accent_fill);
            g_rt->FillRectangle(D2D1::RectF(left, y, left + 5 * s, y + rh), accent);
        }
        ID2D1Brush *label_brush = !items[i].enabled ? (ID2D1Brush *)faint : sel ? (ID2D1Brush *)white : (ID2D1Brush *)dim;
        text(items[i].label, g_item_format, D2D1::RectF(left + 26 * s, y, left + col - 20 * s, y + row), label_brush);
        if (!items[i].note.empty())
            text(items[i].note, g_small_format, D2D1::RectF(left + 26 * s, y + row - 14 * s, left + col - 20 * s, y + rh - 6 * s), faint);
        if (!items[i].value.empty())
        {
            std::wstring v = sel && items[i].adjust ? L"◂  " + items[i].value + L"  ▸" : items[i].value;
            text(v, g_value_format, D2D1::RectF(left + 26 * s, y, left + col - 24 * s, y + row),
                 sel ? (ID2D1Brush *)accent : (ID2D1Brush *)dim);
        }
        y += rh + 4 * s;
    }
    if (g_scroll > 0) text(L"▴", g_small_format, D2D1::RectF(left + col + 10 * s, y0, left + col + 40 * s, y0 + 30 * s), dim);
    if (g_scroll + visible < (int)items.size()) text(L"▾", g_small_format, D2D1::RectF(left + col + 10 * s, y - 34 * s, left + col + 40 * s, y), dim);

    // Objectives, as MCC shows them on its pause screen.
    std::wstring primary, secondary;
    {
        std::lock_guard<std::mutex> l(g_mission_lock);
        primary = g_objective_primary, secondary = g_objective_secondary;
    }
    if (!g_launcher && g_page == p_main && (!primary.empty() || !secondary.empty()))
    {
        float ox = left + col + 120 * s, ow = w - ox - 140 * s;
        if (ow > 300 * s)
        {
            g_rt->FillRectangle(D2D1::RectF(ox, top + 145 * s, ox + ow, top + 470 * s), panel);
            text(L"OBJECTIVES", g_small_format, D2D1::RectF(ox + 30 * s, top + 160 * s, ox + ow, top + 200 * s), accent);
            text(primary + (secondary.empty() ? L"" : L"\n\n" + secondary), g_item_format,
                 D2D1::RectF(ox + 30 * s, top + 210 * s, ox + ow - 30 * s, top + 450 * s), white);
        }
    }

    std::wstring hints = g_launcher ? std::wstring(btn(L"A")) + L" / Enter  select      " + btn(L"B") + L" / Esc  back"
                                    : std::wstring(btn(L"A")) + L" / Enter  select      " + btn(L"B") + L" / Backspace  back      " +
                                          btn(L"Start") + L" / Esc  resume";
    if (g_selected < (int)items.size() && items[g_selected].adjust) hints += L"      ◂ ▸  change";
    if (g_listen_kind == listen_pad) hints = std::wstring(L"Press the button to use      ") + btn(L"Start") + L" / Esc  cancel";
    if (g_listen_kind == listen_key) hints = std::wstring(L"Press the key or mouse button to use      Esc / ") + btn(L"B") + L"  cancel";
    text(hints, g_small_format, D2D1::RectF(left, h - 110 * s, w - left, h - 70 * s), dim);

    HRESULT hr = g_rt->EndDraw();
    for (auto b : {white, dim, faint, accent, accent_fill, shade, panel}) b->Release();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) g_rt->Release(), g_rt = nullptr;
}

static void reset_for_game()
{
    std::lock_guard<std::recursive_mutex> l(g_lock);
    g_open = false, g_open_request = false, g_launcher = false, g_masking = false;
    g_page = p_main, g_selected = 0, g_scroll = 0;
    g_history.clear();
    g_last_game_frame = 0;
    g_listen_kind = listen_none;
}

// The loading screen. It covers the game's frames from the moment a game starts until
// the game says it is running (state 1), and again for each new map (state 0: a mission
// loading in the middle of a campaign, a restart). The games don't report how far they
// have got, so the bar is an estimate that fills quickly at first and slows, and jumps
// to full when the game reports the map has loaded (state 7). It also covers the wait
// while a game shuts down. It is drawn over the game's own frames, on the game's
// rendering thread; when the game stops presenting, its last frame stays on screen.
enum { load_off, load_loading, load_loaded, load_fading };
static std::atomic<int> g_load_phase{load_off};
static std::atomic<int64_t> g_load_started, g_load_loaded_at, g_load_fade_at;
static std::atomic<float> g_load_shown{0}; // the bar as last drawn, so it never goes back
static std::mutex g_load_text_lock;
static std::wstring g_load_title, g_load_subtitle;
static const int64_t k_load_hold_ms = 3000; // after state 7, at most this long before the fade
static const int64_t k_load_fade_ms = 350;

// What the screen says: the mission or map, and the game and what's being played.
static void update_loading_text()
{
    const game_entry *g = find_game(g_cfg.game);
    std::wstring game = g ? g->title : g_cfg.game, title, detail;
    if (g_mode == 2)
    {
        for (const auto &m : spops_missions())
            if (g_ff_variant.find(m.file) != std::string::npos)
                title = m.map_name, detail = L"Spartan Ops  •  Episode " + std::to_wstring(m.episode);
        if (detail.empty()) detail = L"Spartan Ops";
    }
    else
    {
        const mission_entry *m = current_mission();
        if (m) title = m->name;
        if (g_mode == 1)
        {
            int d = g_cfg.difficulty;
            detail = L"Campaign";
            if (d >= 0 && d < 4) detail += std::wstring(L"  •  ") + k_difficulty_names[d];
        }
        else
        {
            std::string v = g_ff_variant;
            size_t slash = v.find_last_of("\\/");
            if (slash != std::string::npos) v = v.substr(slash + 1);
            if (v.size() > 4 && !_stricmp(v.c_str() + v.size() - 4, ".bin")) v.resize(v.size() - 4);
            detail = g_mode == 5 ? L"Firefight" : L"Custom game";
            if (!v.empty()) detail += L"  •  " + pretty_variant_name(v);
        }
    }
    if (g_local_players > 1) detail += L"  •  " + std::to_wstring(g_local_players) + L" players";
    std::lock_guard<std::mutex> l(g_load_text_lock);
    g_load_title = title.empty() ? game : title;
    g_load_subtitle = title.empty() ? detail : game + L"  •  " + detail;
}

static void start_loading()
{
    if (g_load_phase != load_loading)
    {
        g_load_started = now_ms();
        g_load_shown = 0;
    }
    g_load_phase = load_loading;
    update_loading_text();
}

// From the game's set_game_state.
static void loading_state(int state)
{
    if (g_ending) return;
    if (state == 0) { start_loading(); return; }
    int phase = g_load_phase;
    if (state == 7 && phase == load_loading) g_load_loaded_at = now_ms(), g_load_phase = load_loaded;
    else if (state == 1 && (phase == load_loading || phase == load_loaded))
        g_load_fade_at = now_ms(), g_load_phase = load_fading;
    else if (state == 5 && phase != load_off) g_load_phase = load_off; // something went wrong; show the game
}

// Moves the phases on with time; returns false when there's nothing to draw.
static bool loading_visible(float *alpha, float *progress, bool *busy)
{
    int64_t t = now_ms();
    *busy = false;
    if (g_ending)
    {
        *alpha = 1, *progress = 0, *busy = true;
        return true;
    }
    int phase = g_load_phase;
    if (phase == load_loaded && t - g_load_loaded_at > k_load_hold_ms)
        g_load_fade_at = t, g_load_phase = phase = load_fading;
    if (phase == load_loading && t - g_load_started > 120000) g_load_phase = phase = load_off; // never told; show the game
    if (phase == load_off) return false;
    float p = 1;
    if (phase == load_loading)
    {
        float secs = (float)(t - g_load_started) / 1000.0f;
        p = 0.93f * (1.0f - expf(-secs / 5.0f));
    }
    float shown = g_load_shown;
    if (p < shown) p = shown;
    else if (p > shown + 0.04f && phase != load_loading) p = shown + 0.04f + (p - shown) * 0.25f; // ease to full
    g_load_shown = p;
    *progress = p;
    *alpha = 1;
    if (phase == load_fading)
    {
        // Let the bar reach the end first, then fade.
        int64_t since = t - g_load_fade_at;
        if (p < 0.999f) g_load_fade_at = t, since = 0;
        if (since >= k_load_fade_ms) { g_load_phase = load_off; return false; }
        *alpha = 1.0f - (float)since / (float)k_load_fade_ms;
    }
    return true;
}

static void draw_loading_screen(IDXGISwapChain *chain, float alpha, float progress, bool busy)
{
    DXGI_SWAP_CHAIN_DESC sd;
    if (!chain || FAILED(chain->GetDesc(&sd))) return;
    float w = (float)sd.BufferDesc.Width, h = (float)sd.BufferDesc.Height, s = h / 1080.0f;
    if (!ensure_target(chain, s)) return;
    std::wstring title, subtitle;
    {
        std::lock_guard<std::mutex> l(g_load_text_lock);
        title = g_load_title, subtitle = g_load_subtitle;
    }
    if (busy)
    {
        const next_action &n = g_next;
        if (n.kind == next_action::k_game)
        {
            const game_entry *g = find_game(n.game);
            subtitle = g ? g->title : n.game;
            title = L"Starting";
        }
        else if (n.kind == next_action::k_launcher) title = L"Returning to the menu", subtitle = L"";
        else title = L"Closing", subtitle = L"";
    }
    ID2D1SolidColorBrush *bg, *white, *dim, *accent, *track;
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.02f, 0.04f, 0.07f, alpha), &bg);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, alpha), &white);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.78f, 0.84f, 0.9f, 0.8f * alpha), &dim);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.36f, 0.78f, 1.0f, alpha), &accent);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.36f, 0.78f, 1.0f, 0.18f * alpha), &track);
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    if (alpha >= 1.0f) g_rt->Clear(D2D1::ColorF(0.02f, 0.04f, 0.07f, 1));
    else g_rt->FillRectangle(D2D1::RectF(0, 0, w, h), bg);
    float left = 140 * s, y = h - 320 * s, bar_w = std::min(w - 2 * left, 900 * s);
    text(busy ? L"PLEASE WAIT" : L"LOADING", g_small_format, D2D1::RectF(left, y, w - left, y + 36 * s), accent);
    text(title, g_title_format, D2D1::RectF(left, y + 36 * s, w - left, y + 110 * s), white);
    text(subtitle, g_item_format, D2D1::RectF(left, y + 110 * s, w - left, y + 160 * s), dim);
    float by = y + 185 * s, bh = 6 * s;
    g_rt->FillRectangle(D2D1::RectF(left, by, left + bar_w, by + bh), track);
    if (busy)
    {
        // No progress to show: a segment that sweeps along the bar.
        float seg = bar_w * 0.22f, phase = (float)(now_ms() % 1600) / 1600.0f;
        float x0 = left - seg + (bar_w + seg) * phase;
        g_rt->FillRectangle(D2D1::RectF(std::max(left, x0), by, std::min(left + bar_w, x0 + seg), by + bh), accent);
    }
    else
    {
        g_rt->FillRectangle(D2D1::RectF(left, by, left + bar_w * progress, by + bh), accent);
        // A soft glint moving along the filled part, so a slow load still looks alive.
        float filled = bar_w * progress, gw = 60 * s;
        if (filled > gw)
        {
            float gx = left + fmodf((float)now_ms() / 1000.0f * 420 * s, filled + gw) - gw;
            ID2D1SolidColorBrush *glint;
            g_rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.45f * alpha), &glint);
            g_rt->FillRectangle(D2D1::RectF(std::max(left, gx), by, std::min(left + filled, gx + gw), by + bh), glint);
            glint->Release();
        }
    }
    HRESULT hr = g_rt->EndDraw();
    for (auto b : {bg, white, dim, accent, track}) b->Release();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) g_rt->Release(), g_rt = nullptr;
}

// Shown as soon as a game is chosen, before its DLL loads.
static void draw_loading(const std::wstring &, const std::wstring &)
{
    std::lock_guard<std::recursive_mutex> l(g_lock);
    start_loading();
    float alpha, progress;
    bool busy;
    if (!g_swapchain || !loading_visible(&alpha, &progress, &busy)) return;
    draw_loading_screen(g_swapchain, alpha, progress, busy);
    g_swapchain->Present(1, 0);
}

// Called by the game at the end of each frame, on its rendering thread, just
// before it presents.
static void on_end_frame(IDXGISwapChain *chain)
{
    g_last_game_frame = now_ms();
    if (g_open_request.exchange(false))
    {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        open_menu();
    }
    if (!g_open)
    {
        float alpha, progress;
        bool busy;
        if (!loading_visible(&alpha, &progress, &busy)) return;
        std::lock_guard<std::recursive_mutex> l(g_lock);
        draw_loading_screen(chain ? chain : g_swapchain, alpha, progress, busy);
        return;
    }
    std::lock_guard<std::recursive_mutex> l(g_lock);
    update();
    if (g_open) draw(chain ? chain : g_swapchain, false);
}

// The launcher, and the pause menu if the game stops drawing frames while paused:
// the host draws the menu itself.
static void on_idle()
{
    if (g_launcher)
    {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        update();
        draw(g_swapchain, true);
        g_swapchain->Present(1, 0);
        return;
    }
    // The loading screen is drawn only on the game's own frames (on_end_frame): while a
    // game is loading its threads use the device, and drawing from here at the same
    // time crashed the games under DXVK.
    if (!g_open && !g_open_request) return;
    if (now_ms() - g_last_game_frame < 250 || !g_swapchain) return;
    std::lock_guard<std::recursive_mutex> l(g_lock);
    if (g_open_request.exchange(false)) open_menu();
    if (!g_open) return;
    update();
    if (g_open) { draw(g_swapchain, true); g_swapchain->Present(1, 0); }
}

// While the menu is open, and for held buttons after it closes, the game sees nothing.
static bool menu_blocking() { return g_open || g_open_request; }

static bool filter_input(s_host_input_state *state)
{
    if (g_open || g_open_request)
    {
        memset(reinterpret_cast<unsigned char *>(state) + 4, 0, sizeof(*state) - 4);
        return true;
    }
    if (!g_masking) return false;
    bool any = false;
    for (int vk = 1; vk < 256; vk++)
    {
        if (!g_masked_keys[vk]) continue;
        if (state->key_down[vk]) state->key_down[vk] = 0, any = true;
        else g_masked_keys[vk] = 0;
    }
    WORD held = (WORD)state->gamepad.buttons & g_masked_buttons;
    g_masked_buttons = held;
    state->gamepad.buttons &= ~held;
    if (!any && !held) g_masking = false;
    return false;
}
} // namespace menu

// The game asks for MCC's pause menu (Start or Esc).
static void h_pause_game(void *, int player)
{
    logf("host.pause_game player=%d", player);
    if (!menu::g_open) menu::g_open_request = true;
}

static void h_set_game_objectives(void *, const wchar_t *primary, const wchar_t *secondary)
{
    LOG_CALL(9, "host.set_game_objectives \"%s\" / \"%s\"", narrow(primary).c_str(), narrow(secondary).c_str());
    std::lock_guard<std::mutex> l(g_mission_lock);
    g_objective_primary = primary ? primary : L"";
    g_objective_secondary = secondary ? secondary : L"";
}

// HUD colours pass through MCC, which can tint them; give back the colour unchanged.
static uint32_t h_chud_blend_color(void *, int player, uint32_t argb)
{
    LOG_CALL(97, "host.chud_blend_color player=%d color=%08x", player, argb);
    return argb;
}

// #115: whether the host handles the classic/Anniversary graphics switch through its
// controller layout (Halo 2 then switches on MCC's "show scores" button, Back).
// Halo 2's Anniversary renderer draws one full-screen view, so split-screen stays on
// the classic graphics and Back doesn't switch.
static bool h_graphics_switch_on_back(void *) { return g_module == 1 && g_local_players <= 1; }

static void build_host_vtable()
{
    fill_stubs<k_tag_host>(g_host_vtable, std::make_index_sequence<512>{});
    fill_stubs<k_tag_event>(g_event_vtable, std::make_index_sequence<512>{});
    g_event_manager.vtable = g_event_vtable;
    g_event_vtable[147] = (void *)&e_get_guid;
    void **v = g_host_vtable;
    v[0] = (void *)&h_begin_frame;
    v[1] = (void *)&h_end_frame;
    v[3] = (void *)&h_set_game_state;
    v[4] = (void *)&h_restart_game;
    v[41] = (void *)&h_network_sendto_unreliable;
    v[42] = (void *)&h_network_sendto_reliable;
    v[43] = (void *)&h_network_recvfrom;
    v[5] = (void *)&h_save_game;
    v[7] = (void *)&h_pause_game;
    v[9] = (void *)&h_set_game_objectives;
    v[10] = (void *)&h_get_game_event_manager;
    v[17] = (void *)&h_get_campaign_map_info;
    v[18] = (void *)&h_get_multiplayer_map_info;
    v[32] = (void *)&h_get_video_setting;
    v[33] = (void *)&h_get_audio_setting;
    v[34] = (void *)&h_get_player_profile;
    v[36] = (void *)&h_get_input_state;
    v[37] = (void *)&h_get_input_state_gamepad;
    v[38] = (void *)&h_update_input_time;
    v[39] = (void *)&h_set_input_state;
    v[46] = (void *)&h_get_folder_path;
    v[47] = (void *)&h_get_game_folder_path;
    v[48] = (void *)&h_get_scenario_path_a;
    v[49] = (void *)&h_get_scenario_path_w;
    v[51] = (void *)&h_get_game_setting;
    v[52] = (void *)&h_validate_cache_file;
    v[53] = (void *)&h_get_ui_text;
    v[54] = (void *)&h_get_mcc_string;
    v[55] = (void *)&h_false_55;
    v[57] = (void *)&h_get_string;
    v[58] = (void *)&h_get_subtitle;
    v[88] = (void *)&h_get_player_xuid;
    v[97] = (void *)&h_chud_blend_color;
    v[115] = (void *)&h_graphics_switch_on_back;
    v[61] = (void *)&h_font_test_string;
    v[62] = (void *)&h_font_precache_character;
    v[63] = (void *)&h_font_get_texture;
    v[64] = (void *)&h_font_test_char;
    v[65] = (void *)&h_font_kerning;
    v[66] = (void *)&h_font_set_icon;
    v[67] = (void *)&h_font_set_selection;
    v[116] = (void *)&h_get_player_gamepad_mapping;
}

static bool find_file(const std::wstring &dir, const wchar_t *name, int depth, std::wstring &out)
{
    std::wstring direct = dir + L"\\" + name;
    if (GetFileAttributesW(direct.c_str()) != INVALID_FILE_ATTRIBUTES) { out = direct; return true; }
    if (depth <= 0) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        if (!_wcsicmp(fd.cFileName, L"maps") || !_wcsicmp(fd.cFileName, L"Content")) continue; // big, never holds DLLs
        found = find_file(dir + L"\\" + fd.cFileName, name, depth - 1, out);
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// Under Wine, a fresh prefix has Wine's own xaudio2_9.dll, which doesn't work with the
// games (and the profile asks for the native one). MCC ships Microsoft's XAudio 2.9 as
// XAudio2_9Redist.dll; it is put in the prefix's system32 as xaudio2_9.dll when that
// one is Wine's. Only under Wine: on Windows, system32 is never touched.
static void give_wine_native_xaudio(const std::wstring &redist)
{
    if (!running_under_wine()) return;
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring target = std::wstring(sys) + L"\\xaudio2_9.dll";
    bool replace = true;
    if (FILE *f = _wfopen(target.c_str(), L"rb"))
    {
        char head[0x60] = {};
        size_t got = fread(head, 1, sizeof(head), f);
        fclose(f);
        // Wine's placeholder DLLs say so at 0x40.
        replace = got >= 0x50 && !memcmp(head + 0x40, "Wine builtin DLL", 16);
    }
    if (!replace) return;
    BOOL ok = CopyFileW(redist.c_str(), target.c_str(), FALSE);
    logf("xaudio2_9.dll: Wine's own in %s; MCC's copied over it: %s", narrow(sys).c_str(), ok ? "done" : "failed");
}

static void preload_mcc_libraries()
{
    const wchar_t *libs[] = {L"bink2w64.dll", L"XAudio2_9Redist.dll"};
    std::wstring bin = g_cfg.mcc_root + L"\\mcc\\binaries\\win64";
    for (auto lib : libs)
    {
        std::wstring path;
        if (!find_file(bin, lib, 0, path) && !find_file(g_cfg.host_dir, lib, 0, path) &&
            !find_file(g_cfg.mcc_root, lib, 4, path))
        {
            logf("WARNING: %s not found under %s; videos will crash the game", narrow(lib).c_str(),
                 narrow(g_cfg.mcc_root.c_str()).c_str());
            continue;
        }
        HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        logf("preload %s -> %p (error %lu)", narrow(path.c_str()).c_str(), (void *)h, h ? 0ul : GetLastError());
        if (!_wcsicmp(lib, L"XAudio2_9Redist.dll")) give_wine_native_xaudio(path);
    }
}

static void list_folder(const std::wstring &pattern)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { logf("listing %s: nothing", narrow(pattern.c_str()).c_str()); return; }
    std::string line;
    int n = 0;
    do
    {
        if (fd.cFileName[0] == L'.') continue;
        line += narrow(fd.cFileName) + ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "\\ " : " ");
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    logf("listing %s (%d): %s", narrow(pattern.c_str()).c_str(), n, line.c_str());
}

static HMODULE g_game_module;
// ---------------------------------------------------------------- crashes
static LONG WINAPI on_crash(EXCEPTION_POINTERS *info);
static LONG WINAPI on_crash(EXCEPTION_POINTERS *info)
{
    EXCEPTION_RECORD *r = info->ExceptionRecord;
    logf("CRASH code=%08lx at %s", r->ExceptionCode, module_offset(r->ExceptionAddress).c_str());
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        logf("  %s address %llx", r->ExceptionInformation[0] ? "writing" : "reading",
             (unsigned long long)r->ExceptionInformation[1]);
    CONTEXT *c = info->ContextRecord;
    logf("  rax=%llx rbx=%llx rcx=%llx rdx=%llx r8=%llx r9=%llx rsp=%llx", c->Rax, c->Rbx, c->Rcx, c->Rdx, c->R8,
         c->R9, c->Rsp);
    uint64_t *stack = reinterpret_cast<uint64_t *>(c->Rsp);
    for (int i = 0; i < 48; i++)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)stack[i], &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE) continue;
        logf("  stack[%d] %s", i, module_offset((void *)stack[i]).c_str());
    }
    log_flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- window
// The mouse is captured (hidden and kept in the window) after a click in the window,
// and released with F8 or when the window loses focus.
static void capture_mouse(HWND hwnd, bool on)
{
    if (on == g_mouse_captured) return;
    g_mouse_captured = on;
    if (on)
    {
        RECT r;
        GetClientRect(hwnd, &r);
        MapWindowPoints(hwnd, nullptr, (POINT *)&r, 2);
        ClipCursor(&r);
        while (ShowCursor(FALSE) >= 0) {}
    }
    else
    {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {}
    }
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INPUT:
    {
        RAWINPUT ri;
        UINT size = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && g_mouse_captured)
        {
            if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
                g_mouse_dx += ri.data.mouse.lLastX, g_mouse_dy += ri.data.mouse.lLastY;
            if (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) g_mouse_wheel += (SHORT)ri.data.mouse.usButtonData;
        }
        break;
    }
    case WM_LBUTTONDOWN:
        g_keys[VK_LBUTTON] = 1;
        if (!menu::g_open) capture_mouse(hwnd, true);
        break;
    case WM_LBUTTONUP: g_keys[VK_LBUTTON] = 0; break;
    case WM_RBUTTONDOWN: g_keys[VK_RBUTTON] = 1; break;
    case WM_RBUTTONUP: g_keys[VK_RBUTTON] = 0; break;
    case WM_MBUTTONDOWN: g_keys[VK_MBUTTON] = 1; break;
    case WM_MBUTTONUP: g_keys[VK_MBUTTON] = 0; break;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        g_keys[GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2] = msg == WM_XBUTTONDOWN;
        return TRUE;
    case WM_KILLFOCUS:
        clear_keys();
        break;
    case WM_APP + 1:
        capture_mouse(hwnd, wp != 0);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) capture_mouse(hwnd, false);
        break;
    case WM_SIZE:
        if (g_mouse_captured) { capture_mouse(hwnd, false); capture_mouse(hwnd, true); }
        break;
    case WM_CLOSE:
        logf("window closed");
        request_quit();
        return 0;
    case WM_TIMER:
        KillTimer(hwnd, 1);
        if (g_engine && !menu::g_open) { logf("auto-resume: post_message(resume)"); g_engine->post_message(1, nullptr); }
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
    {
        bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        int vk = (int)wp & 0xFF;
        bool extended = (lp >> 24) & 1;
        UINT scan = (lp >> 16) & 0xFF;
        g_keys[vk] = down;
        // Left/right versions of Shift, Ctrl and Alt.
        if (vk == VK_SHIFT) g_keys[MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX) & 0xFF] = down;
        if (vk == VK_CONTROL) g_keys[extended ? VK_RCONTROL : VK_LCONTROL] = down;
        if (vk == VK_MENU) g_keys[extended ? VK_RMENU : VK_LMENU] = down;
        if (!down) break;
        if (msg == WM_SYSKEYDOWN && vk != VK_F4) return 0; // keep Alt from opening the window menu
    }
        if (wp == VK_F8) capture_mouse(hwnd, false);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- main
static int difficulty_from(const wchar_t *s)
{
    if (!_wcsicmp(s, L"easy")) return 0;
    if (!_wcsicmp(s, L"normal")) return 1;
    if (!_wcsicmp(s, L"heroic")) return 2;
    if (!_wcsicmp(s, L"legendary")) return 3;
    return _wtoi(s);
}

// The window and the Direct3D 11 device and swapchain the game (or the launcher) draws to.
static bool create_window_and_device(const std::wstring &title, ID3D11Device *&device, ID3D11DeviceContext *&context)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"installation08";
    RegisterClassW(&wc);
    // A screen no bigger than the game's picture (the PS5's 1920x1080 desktop, a TV)
    // gets a borderless window covering all of it; a title bar would push the picture
    // off the bottom. Otherwise a normal window. --borderless 1/0 decides by hand.
    int screen_w = GetSystemMetrics(SM_CXSCREEN), screen_h = GetSystemMetrics(SM_CYSCREEN);
    bool borderless = g_cfg.borderless >= 0 ? g_cfg.borderless != 0
                                            : (screen_w <= g_cfg.width && screen_h <= g_cfg.height);
    HWND hwnd;
    if (borderless)
    {
        hwnd = CreateWindowW(L"installation08", title.c_str(), WS_POPUP, 0, 0, screen_w, screen_h, nullptr, nullptr,
                             wc.hInstance, nullptr);
        ShowWindow(hwnd, SW_SHOWNORMAL);
        logf("borderless window %dx%d", screen_w, screen_h);
    }
    else
    {
        RECT rect = {0, 0, g_cfg.width, g_cfg.height};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = CreateWindowW(L"installation08", title.c_str(), WS_OVERLAPPEDWINDOW, g_cfg.window_x, g_cfg.window_y,
                             rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
        ShowWindow(hwnd, g_cfg.window_max ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    }
    g_hwnd = hwnd;
    RAWINPUTDEVICE rid = {0x01, 0x02, 0, hwnd}; // generic desktop, mouse
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) logf("raw mouse input unavailable: %lu", GetLastError());

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = g_cfg.width;
    sd.BufferDesc.Height = g_cfg.height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &sd,
                                               &g_swapchain, &device, nullptr, &context);
    if (FAILED(hr))
    {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 1, D3D11_SDK_VERSION, &sd,
                                           &g_swapchain, &device, nullptr, &context);
    }
    if (FAILED(hr)) { logf("D3D11CreateDeviceAndSwapChain failed: %08lx", (unsigned long)hr); return false; }
    logf("Direct3D 11 device %p, swapchain %p", (void *)device, (void *)g_swapchain);
    g_device = device;
    g_context = context;
    return true;
}

// MCC's folder: --mcc, the folder installation08.exe is in (or its parent), or Steam's
// libraries.
static bool has_games(const std::wstring &root)
{
    for (const auto &g : games())
        if (GetFileAttributesW((root + L"\\" + g.folder + L"\\" + g.folder + L".dll").c_str()) != INVALID_FILE_ATTRIBUTES)
            return true;
    return false;
}

static void find_mcc_root()
{
    if (g_cfg.mcc_given || has_games(g_cfg.mcc_root)) return;
    std::vector<std::wstring> candidates;
    std::wstring parent = g_cfg.host_dir.substr(0, g_cfg.host_dir.find_last_of(L'\\'));
    candidates.push_back(parent);
    wchar_t steam[MAX_PATH] = {};
    DWORD size = sizeof steam;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &size) ==
        ERROR_SUCCESS)
    {
        std::wstring base = steam;
        for (auto &c : base) if (c == L'/') c = L'\\';
        candidates.push_back(base + L"\\steamapps\\common\\Halo The Master Chief Collection");
        // Other Steam libraries are listed in libraryfolders.vdf as "path" "D:\\SteamLibrary".
        FILE *f = _wfopen((base + L"\\steamapps\\libraryfolders.vdf").c_str(), L"r");
        if (f)
        {
            char line[1024];
            while (fgets(line, sizeof line, f))
            {
                char *p = strstr(line, "\"path\"");
                if (!p) continue;
                p = strchr(p + 6, '"');
                if (!p) continue;
                std::string path;
                for (p++; *p && *p != '"'; p++)
                {
                    if (*p == '\\' && p[1] == '\\') p++;
                    path += *p;
                }
                int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
                std::wstring wide(n ? n - 1 : 0, L'\0');
                if (n) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], n);
                candidates.push_back(wide + L"\\steamapps\\common\\Halo The Master Chief Collection");
            }
            fclose(f);
        }
    }
    for (const auto &c : candidates)
        if (has_games(c)) { g_cfg.mcc_root = c; logf("MCC found at %s", narrow(c.c_str()).c_str()); return; }
    logf("no MCC games found next to installation08.exe or in Steam's libraries");
}

// ---------------------------------------------------------------- running games
static void pump_messages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT) request_quit();
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static next_action run_launcher()
{
    logf("launcher, MCC at %s", narrow(g_cfg.mcc_root.c_str()).c_str());
    SetWindowTextW(g_hwnd, L"Installation 08");
    g_cfg.game.clear();
    g_next = next_action();
    menu::start_launcher();
    while (g_next.kind == next_action::k_none)
    {
        pump_messages();
        if (IsIconic(g_hwnd)) { Sleep(50); continue; }
        menu::on_idle(); // draws and presents, paced by the display
    }
    menu::g_open = false;
    g_launcher = false;
    return g_next;
}

// Threads the game started (their start address is in its DLL) that are still running.
static int game_threads_running(HMODULE module)
{
    typedef LONG(NTAPI * t_query)(HANDLE, int, void *, ULONG, ULONG *);
    static auto query = (t_query)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    auto *base = reinterpret_cast<unsigned char *>(module);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + reinterpret_cast<IMAGE_DOS_HEADER *>(base)->e_lfanew);
    uintptr_t lo = (uintptr_t)base, hi = lo + nt->OptionalHeader.SizeOfImage;
    int count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    THREADENTRY32 te = {sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        uintptr_t start = 0;
        if (query && query(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, nullptr) == 0 &&
            start >= lo && start < hi)
            count++;
        CloseHandle(h);
    }
    CloseHandle(snap);
    return count;
}

static DWORD WINAPI free_library_thread(LPVOID module)
{
    FreeLibrary((HMODULE)module);
    return 0;
}

// When a game can't be unloaded, Installation 08 closes so the next game starts in a
// fresh process. The PS5 app (which runs one Windows program per start) reads this
// file when the program ends and starts it again instead of going back to Home.
static void ask_to_be_restarted()
{
    if (!running_under_wine()) return;
    if (FILE *f = _wfopen((g_cfg.host_dir + L"\\restart.flag").c_str(), L"wb")) fputs("1\n", f), fclose(f);
}

static next_action run_game(const next_action &what, ID3D11Device *device, ID3D11DeviceContext *context)
{
    g_cfg.game = what.game;
    g_cfg.map = what.map;
    g_cfg.map_file = std::wstring(what.file.begin(), what.file.end());
    g_cfg.difficulty = what.difficulty;
    g_mode = what.mode, g_mode_for_saves = what.mode;
    g_keyboard_player = g_settings.keyboard_player != 0;
    net_reset();
    g_ff_variant = what.variant;
    const game_entry *entry = find_game(g_cfg.game);
    if (entry)
    {
        g_module = entry->module;
        if (g_cfg.map < 0)
        {
            // Default: the first playable mission (Reach's first, Noble Actual, is a cutscene).
            const mission_entry &m = entry->missions[g_module == 6 && entry->missions.size() > 1 ? 1 : 0];
            g_cfg.map = m.map;
            g_cfg.map_file = std::wstring(m.file, m.file + strlen(m.file));
        }
    }
    if (g_cfg.map < 0) g_cfg.map = 0;
    {
        std::lock_guard<std::mutex> l(g_mission_lock);
        g_current_map_file.clear();
        g_candidate_map_file.clear();
        g_last_map_request.clear();
        g_objective_primary.clear(), g_objective_secondary.clear();
    }
    g_next = next_action();
    g_ending = false;
    g_game_over_at = 0;
    g_discard_session = false;
    keep_save_from_before();
    menu::reset_for_game();
    fonts::reset();
    SetWindowTextW(g_hwnd, (L"Installation 08 - " + std::wstring(entry ? entry->title : g_cfg.game.c_str())).c_str());
    const mission_entry *m = current_mission();
    menu::draw_loading(entry ? entry->title : g_cfg.game, m ? m->name : L"");
    logf("installation08: game=%s map=%d file=%s difficulty=%d tick=%d mcc=%s", narrow(g_cfg.game.c_str()).c_str(), g_cfg.map,
         narrow(g_cfg.map_file.c_str()).c_str(), g_cfg.difficulty, g_cfg.tick, narrow(g_cfg.mcc_root.c_str()).c_str());

    next_action back_to_launcher;
    back_to_launcher.kind = next_action::k_launcher;

    // 1. Load the game's DLL from its own folder.
    std::wstring game_dir = g_cfg.mcc_root + L"\\" + g_cfg.game;
    SetDllDirectoryW(game_dir.c_str());
    std::wstring dll = game_dir + L"\\" + g_cfg.game + L".dll";
    HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) { logf("cannot load %s: error %lu", narrow(dll.c_str()).c_str(), GetLastError()); return back_to_launcher; }
    logf("loaded %s at %p", narrow(dll.c_str()).c_str(), (void *)module);
    g_game_module = module;
    list_folder(game_dir + L"\\maps\\fonts\\*");
    auto set_library_settings = (t_set_library_settings)GetProcAddress(module, "SetLibrarySettings");
    auto create_game_engine = (t_create_game_engine)GetProcAddress(module, "CreateGameEngine");
    if (!create_game_engine) { logf("no CreateGameEngine export"); return back_to_launcher; }

    // 2. Language, as MCC sets it before creating the engine.
    if (set_library_settings)
    {
        static s_language_settings language;
        wcscpy(language.audio, L"en-US");
        wcscpy(language.text_1, L"en-US");
        wcscpy(language.text_2, L"en-US");
        int r = set_library_settings(&language);
        logf("SetLibrarySettings(en-US) -> %d", r);
    }

    // 3. The engine, drawing to our window.
    uint64_t r = create_game_engine(&g_engine);
    logf("CreateGameEngine -> %llu, engine=%p vtable=%s", (unsigned long long)r, (void *)g_engine,
         g_engine ? module_offset(*(void **)g_engine).c_str() : "-");
    if (!g_engine) return back_to_launcher;
    load_campaign_info();
    apply_settings();
    logf("engine.initialize_graphics ...");
    g_engine->initialize_graphics(device, context, g_swapchain, nullptr);
    logf("engine.initialize_graphics returned");

    // 4. What to play: one local player, campaign, the chosen mission.
    static s_game_options options;
    memset(&options, 0, sizeof(options));
    // Flag 8 means "this machine hosts the game's session". Without it Reach's launch
    // sequence takes the path for joining someone else's session (step 9) and never
    // starts the map; with it, a one-player game takes step 10, which starts at once.
    options.flags = (int16_t)g_cfg.flags;
    // Halo 2: flag 1 starts the Anniversary graphics; without it the game starts classic.
    // (Anniversary graphics are a campaign thing; multiplayer runs the classic engine.)
    bool split = what.players > 1;
    if (g_module == 1 && !g_settings.h2_classic && g_mode == 1 && !split) options.flags |= 1;
    use_controls(narrow(g_cfg.game.c_str()), g_module);
    // Halo CE reads the same two bits into its config: bit 0 clear sets CONFIG.LegacyMode
    // (classic graphics), bit 1 clear sets CONFIG.LegacySounds (classic sound). MCC's
    // default is the remastered sound, so that bit is always on.
    if (g_module == 0) options.flags |= 2 | (g_settings.h1_classic || g_mode != 1 ? 0 : 1);
    options.game_tick = (char)g_cfg.tick;
    options.game_mode = 1;
    // Halo 2 picks its engine from this field: a campaign map id (29-43) gets the
    // Anniversary campaign engine (new graphics and sound); anything else gets a bare
    // classic engine with no audio. So it carries the map id there.
    options.legacy_map_id = (g_cfg.legacy_map_id < 0 && g_module == 1) ? g_cfg.map : g_cfg.legacy_map_id;
    options.map_id = {g_cfg.map, 0, 0x8888, 0};
    // "No custom campaign": the game's own campaign, marked 0x1111 with the game's module
    // index (Reach checks for this before it would ask the host for a mod campaign).
    options.custom_campaign_map_id = {g_module, 1, 0x1111, 0};
    options.difficulty = g_cfg.difficulty;
    options.campaign_insertion_point = g_cfg.insertion;
    s_player_options &players = options.player_options;
    for (auto &p : players.player_options) p = {0, 0, -1, 0, -1, -1};
    // Local players (split-screen in campaign); all on this one machine.
    g_local_players = std::max(1, std::min(4, what.players));
    // Listing every local player here sends Halo 3 down its networked start, which
    // waits for them to join a session (and gives up after 45 s). So by default only
    // player 1 is listed and the others exist as local users (input, profile, name);
    // split_screen_session = 1 in the .ini lists them all instead.
    players.player_count = g_settings.split_session ? (int)g_local_players : 1;
    for (int i = 0; i < players.player_count; i++) players.player_options[i].xuid = 0x0009000000000001ull + (uint64_t)i;
    logf("local players: %d%s", (int)g_local_players, g_keyboard_player ? " (keyboard is player 1)" : "");
    players.peer_count = 1;
    if (g_cfg.address)
    {
        options.host_address = (int64_t)g_cfg.address;
        options.un_3 = (int64_t)g_cfg.address;
        players.address[0] = g_cfg.address;
        for (int i = 0; i < g_local_players; i++) players.player_options[i].address = g_cfg.address;
    }
    players.unknown_2 = -1;

    // Firefight: the same launch with mode 5, plus a game type (a file in
    // game_variants) and the map's built-in variant, both made by the game's own loader.
    if (g_mode == 5 || g_mode == 2 || g_mode == 3)
    {
        auto create_data_access = (t_create_data_access)GetProcAddress(module, "CreateDataAccess");
        i_data_access *data = nullptr;
        if (create_data_access) create_data_access(&data);
        std::vector<char> file;
        std::wstring variant(g_ff_variant.begin(), g_ff_variant.end());
        // A path of its own (the player's saved game types) or one inside the game's folder.
        std::wstring path = (variant.size() > 2 && variant[1] == L':' ? variant : game_dir + L"\\" + variant) + L".bin";
        if (FILE *f = _wfopen(path.c_str(), L"rb"))
        {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (size > 0 && size < (1 << 24))
            {
                file.resize((size_t)size);
                if (fread(file.data(), 1, file.size(), f) != file.size()) file.clear();
            }
            fclose(f);
        }
        i_saved_game_file *game_type = (data && !file.empty()) ? data->game_variant_from_file(file.data(), (uint32_t)file.size()) : nullptr;
        i_saved_game_file *map = data ? data->map_variant_from_map_id(&options.map_id) : nullptr;
        logf("firefight: data access %p, game type %s (%zu bytes) -> %p \"%s\", map %d -> %p", (void *)data,
             narrow(path.c_str()).c_str(), file.size(), (void *)game_type,
             game_type && game_type->get_name() ? narrow(game_type->get_name()).c_str() : "", g_cfg.map, (void *)map);
        if (!game_type || !map)
        {
            logf("firefight: could not set up the game type or map");
            return back_to_launcher;
        }
        game_type->copy_to_game_options(&options);
        map->copy_to_game_options(&options);
        options.game_mode = g_mode;
    }

    // Continuing: the saved state goes back to the game the way MCC passes it, its
    // size just before the pointer (Reach checks for exactly its 10.9 MB state).
    static std::vector<char> resume_state;
    resume_state.clear();
    if (what.resume)
    {
        if (FILE *f = _wfopen(save_path(g_cfg.game).c_str(), L"rb"))
        {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (size > 0)
            {
                resume_state.resize((size_t)size);
                if (fread(resume_state.data(), 1, resume_state.size(), f) != resume_state.size()) resume_state.clear();
            }
            fclose(f);
        }
        if (!resume_state.empty())
        {
            options.un_6 = (int32_t)resume_state.size();
            options.saved_game_state = resume_state.data();
            logf("continuing from the saved game (%zu bytes)", resume_state.size());
        }
        else logf("could not read the saved game; starting the mission");
    }

    logf("engine.initialize_game ...");
    HANDLE game_thread = g_engine->initialize_game(&g_host, &options);
    logf("engine.initialize_game -> thread handle %p", (void *)game_thread);
    if (g_cfg.auto_resume > 0) SetTimer(g_hwnd, 1, g_cfg.auto_resume * 1000, nullptr);

    bool engine_released = false, threads_finished = false;
    // 5. Run until the game's thread ends. A menu choice (or closing the window) sets
    // what comes next and asks the game to end.
    for (;;)
    {
        DWORD w = game_thread ? MsgWaitForMultipleObjects(1, &game_thread, FALSE, 30, QS_ALLINPUT)
                              : MsgWaitForMultipleObjects(0, nullptr, FALSE, 30, QS_ALLINPUT);
        if (game_thread && w == WAIT_OBJECT_0) { logf("game thread ended"); break; }
        menu::on_idle();
        pump_messages();
        if (g_next.kind != next_action::k_none && !g_ending)
        {
            g_ending = true;
            g_ending_since = (int64_t)GetTickCount64();
            menu::g_open = false;
            logf("asking the game to end");
            g_engine->post_message(1, nullptr); // resume, if paused
            g_engine->post_message(13, nullptr);
        }
        // Halo 2 reports that it has ended but keeps its threads running until the host
        // releases the engine (MCC does this next); Reach's thread ends by itself first.
        if (g_ending && !engine_released && g_game_over_at &&
            (int64_t)GetTickCount64() - g_game_over_at > 1500)
        {
            logf("the game has ended but its thread is still running: releasing the engine");
            engine_released = true;
            g_engine->free();
        }
        // Halo 2's handle never signals; what counts is that none of its threads remain.
        if (engine_released && game_threads_running(module) == 0)
        {
            logf("the game's threads have all finished");
            threads_finished = true;
            break;
        }
        if (g_ending && (int64_t)GetTickCount64() - g_ending_since > 20000)
        {
            logf("the game did not end within 20 seconds");
            break;
        }
    }
    KillTimer(g_hwnd, 1);
    next_action result = g_next;
    if (result.kind == next_action::k_none) result = back_to_launcher; // the game ended by itself (campaign over)
    if (g_discard_session.exchange(false)) put_back_save_from_before();
    bool thread_done = threads_finished || !game_thread || WaitForSingleObject(game_thread, 0) == WAIT_OBJECT_0;
    if (game_thread) CloseHandle(game_thread);

    if (result.kind == next_action::k_quit) return result;

    // 6. Unload the game so the next one (or the same one, for a new mission) starts
    // from a clean slate. The game's thread already gave up its own hold on the DLL
    // (FreeLibraryAndExitThread); its other threads should be finishing.
    for (int i = 0; i < 50 && game_threads_running(module) > 0; i++) Sleep(100);
    int left = game_threads_running(module);
    if (!thread_done || left > 0)
    {
        logf("the game left %d thread(s) running; closing (start Installation 08 again to continue)", left);
        result.kind = next_action::k_quit; // could not start one: give up
        ask_to_be_restarted();
        return result;
    }
    if (!engine_released) g_engine->free();
    g_engine = nullptr;
    g_game_module = nullptr;
    HANDLE unloader = CreateThread(nullptr, 0, free_library_thread, module, 0, nullptr);
    if (WaitForSingleObject(unloader, 15000) != WAIT_OBJECT_0)
    {
        logf("unloading the game did not finish; closing (start Installation 08 again to continue)");
        result.kind = next_action::k_quit;
        ask_to_be_restarted();
        return result;
    }
    CloseHandle(unloader);
    HMODULE still = GetModuleHandleW(dll.c_str());
    logf("game unloaded%s", still ? " (but the DLL is still loaded)" : "");
    if (still)
    {
        // Something else holds it: a second LoadLibrary would get the used copy.
        result.kind = next_action::k_quit;
        ask_to_be_restarted();
    }
    return result;
}

int wmain(int argc, wchar_t **argv)
{
    InitializeCriticalSection(&g_log_lock);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_cfg.host_dir = exe;
    g_cfg.host_dir = g_cfg.host_dir.substr(0, g_cfg.host_dir.find_last_of(L'\\'));
    g_cfg.mcc_root = g_cfg.host_dir;
    g_log = _wfopen((g_cfg.host_dir + L"\\installation08.log").c_str(), L"w");
    CreateThread(nullptr, 0, log_writer_thread, nullptr, 0, nullptr);

    load_settings();
    for (int i = 1; i < argc; i++) g_launch_args.push_back(argv[i]);
    for (int i = 1; i + 1 < argc; i += 2)
    {
        std::wstring k = argv[i];
        const wchar_t *v = argv[i + 1];
        if (k == L"--game") g_cfg.game = v;
        else if (k == L"--mcc") g_cfg.mcc_root = v, g_cfg.mcc_given = true;
        else if (k == L"--window-x") g_cfg.window_x = _wtoi(v);
        else if (k == L"--window-y") g_cfg.window_y = _wtoi(v);
        else if (k == L"--window-max") g_cfg.window_max = _wtoi(v);
        else if (k == L"--borderless") g_cfg.borderless = _wtoi(v);
        else if (k == L"--map") g_cfg.map = _wtoi(v);
        else if (k == L"--map-file") g_cfg.map_file = v;
        else if (k == L"--preload") g_cfg.preload = _wtoi(v);
        else if (k == L"--watchdog") g_cfg.watchdog = _wtoi(v);
        else if (k == L"--auto-resume") g_cfg.auto_resume = _wtoi(v);
        else if (k == L"--monitor") g_cfg.monitor = _wtoi(v);
        else if (k == L"--pad-mapping") g_cfg.pad_mapping = _wtoi(v);
        else if (k == L"--volume")
            g_cfg.volume = (float)_wtof(v), g_settings.master = g_settings.music = g_settings.effects = (int)(g_cfg.volume * 100);
        else if (k == L"--mouse-sensitivity") g_settings.mouse = (float)_wtof(v);
        else if (k == L"--address") g_cfg.address = _wcstoui64(v, nullptr, 0);
        else if (k == L"--input-connected") g_input_connected = _wtoi(v);
        else if (k == L"--flags") g_cfg.flags = (int)wcstol(v, nullptr, 0);
        else if (k == L"--scenario-path")
        {
            g_scenario_mode = !_wcsicmp(v, L"file") ? 1 : !_wcsicmp(v, L"folder") ? 2 : 0;
            g_cfg.scenario_dir = g_scenario_mode == 2;
        }
        else if (k == L"--legacy-map-id") g_cfg.legacy_map_id = _wtoi(v);
        else if (k == L"--insertion") g_cfg.insertion = _wtoi(v);
        else if (k == L"--difficulty") g_cfg.difficulty = difficulty_from(v);
        else if (k == L"--tick") g_cfg.tick = _wtoi(v);
        else if (k == L"--verbose") g_verbose_log = _wtoi(v) != 0;
        else if (k == L"--width") g_cfg.width = _wtoi(v), g_size_given = true;
        else if (k == L"--height") g_cfg.height = _wtoi(v), g_size_given = true;
        else { fprintf(stderr, "unknown option %s\n", narrow(k.c_str()).c_str()); return 2; }
    }
    find_mcc_root();
    // The games open some of their files by paths relative to MCC's folder ("halo4\\...",
    // ".\\sound\\pc\\..."), as MCC itself runs from there: the working folder must be it,
    // whatever folder Installation 08 was started in.
    if (!SetCurrentDirectoryW(g_cfg.mcc_root.c_str()))
        logf("cannot make %s the working folder: error %lu", narrow(g_cfg.mcc_root.c_str()).c_str(), GetLastError());
    SetUnhandledExceptionFilter(on_crash);
    // The games install their own crash handlers, so this one rarely runs; for a crash
    // that leaves no log, Windows' crash dumps (WER LocalDumps) show where it was.
    // Libraries the games load on first use (delay-loaded) that MCC ships in its own
    // binaries folder, not the games': Bink (videos) and XAudio 2.9 (Bink's sound).
    preload_mcc_libraries();

    // One window and Direct3D 11 device for the launcher and every game.
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    // The picture size: the settings' choice (by default the screen's own size), unless
    // given on the command line. Fullscreen unless the settings ask for a window.
    if (!g_size_given)
    {
        int ri = g_settings.resolution;
        if (ri > 0 && ri < k_resolution_count) g_cfg.width = k_resolutions[ri][0], g_cfg.height = k_resolutions[ri][1];
        else g_cfg.width = GetSystemMetrics(SM_CXSCREEN), g_cfg.height = GetSystemMetrics(SM_CYSCREEN);
        if (g_cfg.width < 640 || g_cfg.height < 480) g_cfg.width = 1920, g_cfg.height = 1080;
    }
    if (g_cfg.borderless < 0) g_cfg.borderless = g_settings.windowed ? 0 : 1;
    logf("picture %dx%d, %s", g_cfg.width, g_cfg.height, g_cfg.borderless ? "fullscreen" : "window");
    if (!create_window_and_device(L"Installation 08", device, context)) return 1;
    fonts::init(g_cfg.mcc_root);
    build_host_vtable();
    init_profile_and_mappings();
    apply_settings();
    load_xinput();

    next_action next;
    if (!g_cfg.game.empty())
    {
        next.kind = next_action::k_game;
        next.game = g_cfg.game, next.map = g_cfg.map, next.file = narrow(g_cfg.map_file.c_str());
        next.difficulty = g_cfg.difficulty;
    }
    else next.kind = next_action::k_launcher;

    for (;;)
    {
        if (next.kind == next_action::k_launcher) next = run_launcher();
        else if (next.kind == next_action::k_game) next = run_game(next, device, context);
        else break;
    }
    logf("installation08 exiting");
    save_settings();
    // A normal exit runs the game DLLs' shutdown code again; nothing is left to save,
    // so end the process outright.
    log_flush();
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
