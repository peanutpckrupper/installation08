// Installation 08: a stand-in for MCC's game host, to run the Halo game DLLs from
// The Master Chief Collection on their own (offline, and LAN).
//
// It loads <game>\<game>.dll, starts the engine on a window and Direct3D 11 device of
// its own, and answers the calls the game makes back into the host.
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
// Online play (Private and Public lobbies through the Installation 08 server) is built
// in when the build names the server: -DI08_SERVER="host[:port]" (or -DI08_ONLINE=1 to
// use online_server in installation08.ini instead). Without it: LAN and local play only.
#ifndef I08_ONLINE
#ifdef I08_SERVER
#define I08_ONLINE 1
#else
#define I08_ONLINE 0
#endif
#endif
// Developer tools (Settings > Network test): -DI08_TEST_TOOLS=1.
#ifndef I08_TEST_TOOLS
#define I08_TEST_TOOLS 0
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dxgi.h>
#include <xinput.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <atomic>
#include <mutex>
#include <set>
#include <functional>
#include <algorithm>
#include <map>
#include <deque>
#include <thread>
#include <condition_variable>
#include <chrono>
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
#include "monocypher/monocypher.h"

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
static bool g_loading_screen = true; // loading_screen = 0 in installation08.ini turns it off
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
    // The file's bytes, as MCC writes them (Reach: a 29481-byte .mvar). The buffer is the object's own.
    virtual bool save_to_file(const char **buf, uint32_t *len) = 0;
    virtual bool save_to_file_(const char **buf, uint32_t *len) = 0;
};
// A map variant (Forge map) adds these (libmcc i_scenario_map_variant).
struct i_scenario_map_variant : i_saved_game_file
{
    virtual bool is_built_in() = 0;
    virtual bool is_built_in_2() = 0;
    virtual void unknown_12() = 0;
    virtual const int32_t *get_map_id() = 0; // s_scenario_map_id: the map's id first
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
static std::wstring g_show_result_path; // --show-result: open the post-game screen on a saved record
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

static HWND g_hwnd;

// F12 in a game: the frame as a PNG in screenshots\ next to the exe (written on its own
// thread so the game doesn't stall).
static void save_png(std::wstring path, std::vector<uint8_t> bgr, UINT w, UINT h)
{
    bool ok = false;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory *factory = nullptr;
    IWICStream *stream = nullptr;
    IWICBitmapEncoder *encoder = nullptr;
    IWICBitmapFrameEncode *frame = nullptr;
    IPropertyBag2 *props = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void **)&factory)) &&
        SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, &props)) &&
        SUCCEEDED(frame->Initialize(props)) && SUCCEEDED(frame->SetSize(w, h)))
    {
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        ok = SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) &&
             SUCCEEDED(frame->WritePixels(h, w * 3, (UINT)bgr.size(), bgr.data())) && SUCCEEDED(frame->Commit()) &&
             SUCCEEDED(encoder->Commit());
    }
    if (props) props->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    logf("screenshot %s: %s", ok ? "saved" : "NOT saved", narrow(path.c_str()).c_str());
}

static void capture_frame(IDXGISwapChain *swapchain)
{
    ID3D11Texture2D *back = nullptr;
    if (!swapchain || FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back))) return;
    D3D11_TEXTURE2D_DESC d;
    back->GetDesc(&d);
    bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    ID3D11Texture2D *staging = nullptr;
    back->GetDevice(&device);
    if (device) device->GetImmediateContext(&context);
    if ((bgra || rgba) && d.SampleDesc.Count == 1 && context)
    {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING, sd.BindFlags = 0, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ, sd.MiscFlags = 0;
        sd.MipLevels = 1, sd.ArraySize = 1;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &staging)))
        {
            context->CopyResource(staging, back);
            if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
            {
                std::vector<uint8_t> bgr((size_t)d.Width * d.Height * 3);
                for (UINT y = 0; y < d.Height; y++)
                {
                    const uint8_t *src = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
                    uint8_t *dst = bgr.data() + (size_t)y * d.Width * 3;
                    for (UINT x = 0; x < d.Width; x++, src += 4, dst += 3)
                        dst[0] = src[rgba ? 2 : 0], dst[1] = src[1], dst[2] = src[rgba ? 0 : 2];
                }
                context->Unmap(staging, 0);
                CreateDirectoryW((g_cfg.host_dir + L"\\screenshots").c_str(), nullptr);
                SYSTEMTIME t;
                GetLocalTime(&t);
                wchar_t name[96];
                swprintf(name, 96, L"%ls %04d-%02d-%02d %02d-%02d-%02d-%03d.png", g_cfg.game.c_str(), t.wYear, t.wMonth, t.wDay,
                         t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
                std::thread(save_png, g_cfg.host_dir + L"\\screenshots\\" + name, std::move(bgr), d.Width, d.Height).detach();
            }
        }
    }
    else logf("screenshot: picture format %d not supported", (int)d.Format);
    if (staging) staging->Release();
    if (context) context->Release();
    if (device) device->Release();
    back->Release();
}

static void h_end_frame(void *, IDXGISwapChain *swapchain, UINT *flags)
{
    static bool f12_was_down;
    bool f12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0 && GetForegroundWindow() == g_hwnd;
    if (f12 && !f12_was_down) capture_frame(swapchain ? swapchain : g_swapchain);
    f12_was_down = f12;
    LOG_CALL(1, "host.end_frame swapchain=%p (ours %p) flags=%u", (void *)swapchain, (void *)g_swapchain,
             flags ? *flags : 0u);
    // The game presents the frame itself right after this call, with the flags the
    // host leaves here. (Presenting here too showed every other frame as black.)
    // The pause menu, when open, is drawn over the frame here.
    menu::on_end_frame(swapchain ? swapchain : g_swapchain);
}

static void commit_mission_map();
static std::atomic<int64_t> g_game_over_at; // when the game reported it has ended (state 5)
// After a game: show the post-game screen (End game, or the game ended by itself), and go
// back to the lobby it was started from rather than the main menu.
static std::atomic<bool> g_show_postgame, g_return_to_lobby;
static void h_set_game_state(void *, int state)
{
    logf("host.set_game_state %d", state);
    if (state == 5) g_game_over_at = (int64_t)GetTickCount64();
    if (state == 0) commit_mission_map(); // a map starts loading
    menu::loading_state(state);
}

// Network. Halo CE's multiplayer runs a server and a client that talk through these
// even when both are this machine, so a packet sent to our own address is handed back
// on the next receive (a loopback). With LAN play (lan:: below) packets for another
// machine's address go to it over UDP.
struct net_packet { std::vector<char> data; uint64_t from; uint32_t port; int64_t at_us = 0; int64_t ready_us = 0; };
// Microseconds, for timing the network: how long packets wait for the game, the gaps between
// them, and the time to the other machine and back (in the 30-second traffic log line).
static int64_t now_us()
{
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)(c.QuadPart / f.QuadPart * 1000000 + c.QuadPart % f.QuadPart * 1000000 / f.QuadPart);
}
static std::mutex g_timing_lock;
static int64_t g_wait_max_us, g_wait_sum_us, g_wait_n;   // packet in -> the game reads it
static int64_t g_gap_max_us, g_last_game_in_us;          // longest silence between game packets
static int64_t g_rtt_min_us, g_rtt_max_us, g_rtt_sum_us, g_rtt_n;
static std::mutex g_net_lock;
static std::deque<net_packet> g_net_queue;
// ready_us: the packet is handed to the game from then on (0: straight away).
static void net_deliver(const char *buf, uint32_t len, uint64_t from, uint32_t port, int64_t ready_us = 0)
{
    std::lock_guard<std::mutex> l(g_net_lock);
    if (g_net_queue.size() > 4096) g_net_queue.pop_front();
    g_net_queue.push_back({std::vector<char>(buf, buf + len), from, port, now_us(), ready_us});
}

// Smoothing for co-op clients. Campaign co-op (and Spartan Ops and Firefight) in Halo 2
// onwards is "synchronous": the host runs the game and sends an update every tick, and a
// joined machine can only move on once the next update is in. Updates crossing the
// internet arrive a few milliseconds early or late, so the client got none in one frame
// and two in the next, which looked like constant stutter. Every game packet carries the
// time the host sent it, and the client hands each one to the game at that time plus a
// steady delay: the quickest trip seen lately plus enough to cover nearly all of the
// wobble (the 99th percentile, up to 80 ms). The delay follows the connection: up at once, down slowly.
static std::atomic<bool> g_smooth{false};   // this machine is a client in a synchronous game
namespace smooth
{
static const int k_ring = 256, k_recent = 128;
static int64_t g_trip[k_ring];   // arrival here minus the host's send time (clocks differ; only changes matter)
static int g_n, g_at;
static int64_t g_target_us, g_last_ready_us, g_last_stamp_us;
// For the log (under g_timing_lock).
static int64_t g_hold_sum_us, g_hold_max_us, g_n_held, g_n_late, g_target_min_us, g_target_max_us, g_target_sum_us;
static int64_t g_send_gap_max_us, g_send_gap_sum_us, g_send_gap_sq, g_send_gap_n, g_wobble_sum_us, g_wobble_max_us;
static void reset()
{
    g_n = g_at = 0;
    g_target_us = 10000;
    g_last_ready_us = g_last_stamp_us = 0;
}
static void reset_stats()
{
    g_hold_sum_us = g_hold_max_us = g_n_held = g_n_late = g_target_min_us = g_target_max_us = g_target_sum_us = 0;
    g_send_gap_max_us = g_send_gap_sum_us = g_send_gap_sq = g_send_gap_n = g_wobble_sum_us = g_wobble_max_us = 0;
}
// When to hand over a packet the host sent at `stamp` (its clock) that arrived `now` (ours).
// Receive thread only.
static int64_t ready_time(int64_t stamp, int64_t now)
{
    int64_t trip = now - stamp;
    g_trip[g_at] = trip;
    g_at = (g_at + 1) % k_ring;
    if (g_n < k_ring) g_n++;
    int64_t base = INT64_MAX;
    for (int i = 0; i < g_n; i++) base = std::min(base, g_trip[i]);
    int m = std::min(g_n, k_recent);
    int64_t extra[k_recent];
    for (int i = 0; i < m; i++) extra[i] = g_trip[(g_at - 1 - i + k_ring) % k_ring] - base;
    int k = m * 99 / 100;
    std::nth_element(extra, extra + k, extra + m);
    int64_t want = std::min<int64_t>(std::max<int64_t>(extra[k] + 2000, 3000), 80000);
    if (want > g_target_us) g_target_us = want;
    else g_target_us -= (g_target_us - want) / 64;
    int64_t ready = std::max(stamp + base + g_target_us, g_last_ready_us);
    g_last_ready_us = ready;
    {
        std::lock_guard<std::mutex> tl(g_timing_lock);
        if (ready > now) g_hold_sum_us += ready - now, g_hold_max_us = std::max(g_hold_max_us, ready - now), g_n_held++;
        else g_n_late++;
        g_target_min_us = g_target_min_us ? std::min(g_target_min_us, g_target_us) : g_target_us;
        g_target_max_us = std::max(g_target_max_us, g_target_us);
        g_target_sum_us += g_target_us;
        g_wobble_sum_us += trip - base, g_wobble_max_us = std::max(g_wobble_max_us, trip - base);
        if (g_last_stamp_us && stamp > g_last_stamp_us)
        {
            int64_t gap = stamp - g_last_stamp_us;
            g_send_gap_max_us = std::max(g_send_gap_max_us, gap);
            g_send_gap_sum_us += gap, g_send_gap_sq += (gap / 100) * (gap / 100), g_send_gap_n++;
        }
    }
    if (stamp > g_last_stamp_us) g_last_stamp_us = stamp;
    return ready;
}
} // namespace smooth

// ---------------------------------------------------------------- LAN play
// One machine hosts a game (Custom game > LAN), others find it on the network (LAN
// games on the main menu) or join it by IP address. Every machine has an address (a
// 64-bit number the games use to name machines); the host's player list gives each
// machine's address and its player's XUID, and the games send their packets to those
// addresses. Here an address is mapped to the machine's IP and port, and packets
// travel in UDP datagrams on port 27080:
//   header (magic, version, type, sender's address), then the message.
// Custom games, Forge and Firefight, in every game.
namespace lan
{
enum role_t { k_off, k_host, k_client };
static std::atomic<int> g_role{k_off};
static uint16_t g_port = 27080;
static SOCKET g_sock = INVALID_SOCKET;
static bool g_opened;
static bool g_port_taken; // another program has port 27080: joining works, hosting can't be found
static uint64_t g_my_id;
static std::string g_name;
static std::string g_problem; // shown in the launcher when LAN play could not start
static int64_t ms() { return (int64_t)GetTickCount64(); }

enum : uint16_t { t_game = 1, t_hello, t_welcome, t_start, t_end, t_find, t_here, t_lobby, t_bye, t_map,
                  t_save, t_save_query, t_save_status, t_frag, t_save_offer, t_start_ack,
                  t_sealed, t_hs_init, t_hs_cookie, t_hs_resp, t_hs_refuse, t_punch, t_ping, t_pong };
#pragma pack(push, 1)
struct header { uint32_t magic; uint16_t version, type; uint64_t from; };
struct seat { uint64_t xuid, address; };
// What a host's game is: sent to machines looking for games, and to those that joined.
struct lobby_msg
{
    char host[32];
    char game[16];     // the game's folder ("halo1")
    char map[48];      // names, for showing (UTF-8)
    char variant[64];
    uint8_t playing;   // the host is in a game now
    uint8_t count;     // players, host first
    char names[16][32];
    // The lobby (version 7): what the host has chosen, and whether each player can play it.
    uint8_t mode;       // 1 campaign, 2 Spartan Ops, 3 custom game or Forge, 5 Firefight
    uint8_t difficulty;
    char map_file[48];  // the map's file (no .map), to check it's installed
    uint32_t key;       // changes with game and map file: a player's answer is for this key
    uint8_t status[16]; // per player: 0 checking, 1 ready, 2 doesn't have the game, 3 doesn't have the map
    char detail[64];    // a line about the choice ("Heroic", "Continue: Requiem")
};
// A joining machine's hello: its name, and whether it can play the lobby's choice.
struct hello_msg
{
    char name[32];
    uint32_t key;
    uint8_t status;
};
struct start_msg
{
    char game[32];
    int32_t map;
    char file[64];
    int32_t mode;
    int32_t difficulty;
    char variant[260];
    uint32_t count;
    seat seats[16];
    uint32_t game_id;      // the same for the copies of one start message
    uint32_t map_size;     // a saved Forge map: sent first, as a t_map message with this game_id
    char map_name[64];     // its name (UTF-8)
    int32_t classic;       // campaign: the host's classic graphics choice (Halo CE, Halo 2)
    uint32_t save_size;    // co-op continue: the host's saved game, sent first in t_save pieces
    uint64_t save_hash;    //   (and its FNV-1a hash, to check the copy)
    uint32_t variant_size; // the game type's file, so every machine plays the same one
    char variant_data[61440]; // the biggest game types (Halo 4, Reach) are about 33 KB
};
// A saved Forge map for the next game (Reach's are 29 KB; MCC's limit is 58 KB).
struct map_msg
{
    uint32_t game_id;
    uint32_t size;
    char data[61440];
};
// Co-op continue: the host's saved game (4 to 11 MB) in pieces, sent before the start.
struct save_piece
{
    uint32_t game_id;
    uint32_t total;  // the whole save's size
    uint32_t index;  // which piece
    uint32_t pieces; // how many
};
static const uint32_t k_piece = 32000;
static const uint32_t k_most_pieces = 2048; // 64 MB
// Sent before any piece: the save's size and each piece's hash. A client fills in the
// pieces its own co-op save already has (usually all or most of them: every machine
// saved the same checkpoint), so only the others need to cross the network.
struct save_offer
{
    uint32_t game_id;
    uint32_t total;
    uint32_t pieces;
    char game[16];
    uint64_t hash[k_most_pieces];
};
// A client's answer to t_save_query: which pieces it has.
struct save_status
{
    uint32_t game_id;
    uint8_t have[k_most_pieces / 8];
};
#pragma pack(pop)
static const uint32_t k_magic = 0x4C383049; // "I08L"
static const uint16_t k_version = 16; // 15: keys made fresh for every join; 16: game packets carry the send time
static const size_t k_start_head = offsetof(start_msg, variant_data);

static std::mutex g_lock; // guards the lists below
struct peer { uint64_t id; sockaddr_in addr; std::string name; int64_t heard; uint32_t key = 0; uint8_t status = 0; };
// Client: can this machine play what the lobby says (status codes as in lobby_msg)?
static int (*g_check_lobby)(const char *game, const char *map_file);
// Client: this machine's own co-op save for a game ("" if none).
static std::string (*g_read_coop_save)(const char *game);
// Host: players the next start leaves out (they can't play it).
static std::vector<uint64_t> g_left_out;
static std::vector<peer> g_peers; // host: the machines that joined; client: the host
// Host: the start it's sending, and which machines said they have it.
static std::atomic<uint32_t> g_acking_id;
static std::set<uint64_t> g_start_acked;
static map_msg g_map_for_resend; // the saved Forge map sent with it, if any
static lobby_msg g_lobby_out;     // host: what it's offering
static lobby_msg g_lobby_in;      // client: the host's game
struct found_game { uint64_t id; sockaddr_in addr; lobby_msg info; int64_t seen; };
static std::vector<found_game> g_found;
static sockaddr_in g_join_addr;   // client: where the host is
static std::string g_join_text;
static std::string g_left_reason; // client: why it's no longer in a game's lobby
static std::atomic<bool> g_welcomed;
static std::atomic<int64_t> g_host_heard, g_searching_until;

static std::vector<seat> g_seats; // a networked game's players, host first; empty otherwise
static uint64_t g_my_xuid = 0x0009000000000001ull;
static std::atomic<uint64_t> g_sent, g_received, g_dropped;
// Game packets carry a number, one count per sender: a gap means packets were lost on
// the way (the network, or a full receive buffer), which nothing else would show.
static std::atomic<uint64_t> g_lost, g_late;
static uint32_t g_last_seq_from[16];
static uint64_t g_seq_peer[16];
static std::atomic<int> g_logged;
static std::mutex g_start_lock;
static start_msg g_start;
static std::atomic<bool> g_start_ready, g_end_ready;
static uint32_t g_last_game_id; // receive thread only
static map_msg g_map_in;        // the host's Forge map for a game (under g_start_lock)
static int64_t g_start_waiting; // main thread: since when a start waits for its map
// Co-op continue. The saved game the next co-op game starts from, on every machine
// (host: read from its file; client: received from the host). Empty: start the mission.
static std::vector<char> g_resume_data;
// Client, under g_start_lock: the save arriving from the host.
static uint32_t g_save_in_id;
static std::vector<char> g_save_in;
// Client: its own co-op save of the same checkpoint, which it continues from instead of
// the host's copy (each machine's save also holds things that are its own, such as which
// machine it is). For the start with this game_id.
static uint32_t g_save_own_id;
static std::vector<char> g_save_own;
static std::vector<uint8_t> g_save_in_have;
// Host, under g_lock: what each client last said it has.
struct save_report { uint64_t id; uint32_t game_id; std::vector<uint8_t> have; uint32_t answers; };
static std::vector<save_report> g_save_reports;

static uint64_t fnv64(const char *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)p[i]) * 0x100000001b3ull;
    return h;
}

static std::string ip_text(const sockaddr_in &a, bool port = true)
{
    char t[64];
    inet_ntop(AF_INET, &a.sin_addr, t, sizeof(t));
    return port ? std::string(t) + ":" + std::to_string(ntohs(a.sin_port)) : std::string(t);
}
static std::string field(const char *s, size_t n) { return std::string(s, strnlen(s, n)); }
static void put(char *dst, size_t n, const std::string &s) { memset(dst, 0, n); memcpy(dst, s.data(), std::min(n - 1, s.size())); }

// Big messages (a start with its game type, a Forge map, a piece of a saved game) go
// in pieces of at most k_frag bytes: the PS5's network stack (FreeBSD's) refuses to
// send a UDP datagram over 9216 bytes, and big datagrams get lost more on Wi-Fi.
#pragma pack(push, 1)
struct frag_head { uint32_t id, total, offset; uint16_t type, pad; };
#pragma pack(pop)
// 1150 bytes: with the headers and encryption a piece is about 1220 bytes, inside the
// ~1400 bytes an internet packet carries, so pieces never get split again on the way.
static const uint32_t k_frag = 1150;
static_assert(sizeof(lobby_msg) <= k_frag, "a lobby must fit in one piece: lobby search is never sealed");
// ---- Network test (Settings > Network test, for testing only). This machine's LAN
// traffic, both ways, gets internet-like delay, jitter and loss, to see how each game
// copes before real internet play exists. On one machine it covers both directions.
// A datagram bigger than the internet's ~1400-byte packets travels in pieces, and
// losing any piece loses it, so big datagrams are lost more often, as they would be.
struct sim_preset { const wchar_t *name; int delay_ms, jitter_ms; double loss; };
static const sim_preset k_sim[] = {
    {L"Off", 0, 0, 0},
    {L"Good internet (40 ms)", 20, 3, 0.001},
    {L"Average internet (100 ms)", 50, 10, 0.01},
    {L"Poor internet (200 ms)", 100, 25, 0.03},
    {L"Bad internet (300 ms)", 150, 40, 0.05},
};
static const int k_sim_count = (int)(sizeof(k_sim) / sizeof(k_sim[0]));
static std::atomic<int> g_sim{0};
static std::atomic<uint64_t> g_sim_lost;
struct sim_item { int64_t due; bool out; sockaddr_in addr; std::vector<char> data; };
static std::mutex g_sim_lock;
static std::condition_variable g_sim_cv;
static std::deque<sim_item> g_sim_queue[2]; // [0] arriving, [1] leaving; each in due order
static int64_t g_sim_last[2];
static uint64_t g_sim_rng = 0x2545F4914F6CDD1Dull;
static int64_t sim_now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static double sim_random()
{
    g_sim_rng ^= g_sim_rng << 13, g_sim_rng ^= g_sim_rng >> 7, g_sim_rng ^= g_sim_rng << 17;
    return (double)(g_sim_rng >> 11) * (1.0 / 9007199254740992.0);
}
static void wire_send(const sockaddr_in &to, const char *data, int len);
static void process_datagram(const sockaddr_in &from, const char *packet, int n);
static void sim_thread()
{
    std::unique_lock<std::mutex> l(g_sim_lock);
    for (;;)
    {
        int pick = -1;
        for (int q = 0; q < 2; q++)
            if (!g_sim_queue[q].empty() && (pick < 0 || g_sim_queue[q].front().due < g_sim_queue[pick].front().due)) pick = q;
        if (pick < 0) { g_sim_cv.wait(l); continue; }
        int64_t wait = g_sim_queue[pick].front().due - sim_now_us();
        if (wait > 0) { g_sim_cv.wait_for(l, std::chrono::microseconds(wait)); continue; }
        sim_item it = std::move(g_sim_queue[pick].front());
        g_sim_queue[pick].pop_front();
        l.unlock();
        if (it.out) wire_send(it.addr, it.data.data(), (int)it.data.size());
        else process_datagram(it.addr, it.data.data(), (int)it.data.size());
        l.lock();
    }
}
// True when the network test took the datagram (to deliver later, or lost it).
static bool sim_take(bool out, const sockaddr_in &addr, const char *data, int len)
{
    int p = g_sim;
    if (p <= 0 || p >= k_sim_count) return false;
    const sim_preset &sp = k_sim[p];
    static std::once_flag started;
    std::call_once(started, [] { std::thread(sim_thread).detach(); });
    std::lock_guard<std::mutex> l(g_sim_lock);
    int pieces = (len + 28 + 1399) / 1400;
    if (sim_random() >= std::pow(1.0 - sp.loss, pieces)) { g_sim_lost++; return true; }
    int64_t now = sim_now_us();
    int64_t due = now + (int64_t)sp.delay_ms * 1000 + (int64_t)((sim_random() * 2.0 - 1.0) * sp.jitter_ms * 1000);
    // The internet seldom reorders packets: jitter delays them, but they keep their order.
    int q = out ? 1 : 0;
    due = std::max(due, std::max(now, g_sim_last[q]));
    g_sim_last[q] = due;
    g_sim_queue[q].push_back({due, out, addr, std::vector<char>(data, data + len)});
    g_sim_cv.notify_one();
    return true;
}

// Traffic counts (bytes on the wire, with the 28 bytes of IP and UDP headers), for the
// upload a host needs, and the largest game packet each way.
static std::atomic<uint64_t> g_wire_out, g_wire_in, g_game_bytes_out, g_game_bytes_in;
static std::atomic<uint32_t> g_max_game_out, g_max_game_in, g_max_datagram_out;
static std::atomic<uint64_t> g_unrouted; // packets the game sent to a machine only the host can reach
static std::atomic<int64_t> g_stats_since;
static void note_max(std::atomic<uint32_t> &m, uint32_t v)
{
    uint32_t cur = m;
    while (v > cur && !m.compare_exchange_weak(cur, v)) {}
}

static void wire_send(const sockaddr_in &to, const char *data, int len);
static void put_wire(const sockaddr_in &to, const char *data, int len)
{
    g_wire_out += len + 28;
    note_max(g_max_datagram_out, (uint32_t)len);
    if (sim_take(true, to, data, len)) return;
    wire_send(to, data, len);
}

// ---- Security. Every packet between machines is encrypted and authenticated
// (XChaCha20-Poly1305 with keys from an X25519 exchange, using Monocypher). Only looking
// for lobbies on the home network and the handshake travel in the clear, and a machine
// is known by its keys, not by the address it claims. A LAN join has no shared secret
// (being on the home network is the invitation) and is accepted only from private
// addresses. An online join must carry the secret in the host's join code, so nobody
// without the code gets a single packet into a game.
static uint8_t g_priv[32], g_pub[32], g_cookie_secret[32];
static uint8_t g_online_code[10];        // host: its join code (80 random bits)
static uint8_t g_online_psk[32];         // host: the key made from it
static std::atomic<bool> g_online{false}; // host: open to the internet (with the code)
static uint8_t g_join_psk[32];           // client: the code's key, or zeros for a LAN join
static bool random_bytes(void *p, size_t n)
{
    return BCryptGenRandom(nullptr, (PUCHAR)p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}
// Home networks (and 100.64/10, which Tailscale-style private networks use).
static bool is_private(const sockaddr_in &a)
{
    uint32_t ip = ntohl(a.sin_addr.s_addr);
    return (ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE ||
           (ip >> 22) == 0x191;
}
static bool plain_type(uint16_t type)
{
    return type == t_find || type == t_here || type == t_hs_init || type == t_hs_cookie || type == t_hs_resp || type == t_hs_refuse ||
           type == t_punch;
}
static void auth_key(const uint8_t psk[32], uint8_t out[32])
{
    crypto_blake2b_keyed(out, 32, psk, 32, (const uint8_t *)"I08 auth 1", 10);
}
static void mac16(const uint8_t key[32], const char *tag, const void *a, size_t an, const void *b, size_t bn, const void *c,
                  size_t cn, uint8_t out[16])
{
    crypto_blake2b_ctx x;
    crypto_blake2b_keyed_init(&x, 16, key, 32);
    crypto_blake2b_update(&x, (const uint8_t *)tag, strlen(tag));
    crypto_blake2b_update(&x, (const uint8_t *)a, an);
    crypto_blake2b_update(&x, (const uint8_t *)b, bn);
    crypto_blake2b_update(&x, (const uint8_t *)c, cn);
    crypto_blake2b_final(&x, out);
}
#pragma pack(push, 1)
struct hs_init_msg { uint8_t pub[32], cookie[16], nonce[16], mac[16], pad[80]; }; // padded: replies are never bigger
struct hs_cookie_msg { uint8_t cookie[16], nonce[16]; };
struct hs_resp_msg { uint8_t pub[32], nonce[16], mac[16]; };
struct hs_refuse_msg { uint8_t nonce[16]; uint8_t reason; }; // 1 not hosting, 2 wrong code or not open to the internet
#pragma pack(pop)
struct session
{
    sockaddr_in addr;
    uint8_t tx[32], rx[32], peer_pub[32];
    uint8_t hs_nonce[16] = {0}; // the joiner's handshake that made it
    uint64_t tx_ctr = 0, rx_max = 0, rx_bits = 0;
    uint64_t peer_id = 0; // the machine address it uses inside: fixed once seen
    int64_t heard = 0;
    bool code = false;    // joined with a join code
};
static std::mutex g_sess_lock;
static std::vector<session> g_sessions;
static bool same_addr(const sockaddr_in &a, const sockaddr_in &b)
{
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}
static session *find_session(const sockaddr_in &a) // with g_sess_lock held
{
    for (auto &x : g_sessions)
        if (same_addr(x.addr, a)) return &x;
    return nullptr;
}
static void clear_sessions()
{
    std::lock_guard<std::mutex> l(g_sess_lock);
    for (auto &x : g_sessions) crypto_wipe(&x, sizeof(x));
    g_sessions.clear();
}
// Client: the handshake with the host it's joining.
struct pending_join { sockaddr_in addr; uint8_t cookie[16], nonce[16]; int64_t sent = 0, began = 0; bool active = false, said = false; };
static pending_join g_pending;
static void dropped_by_host(const char *why);

// The keys come from both machines' keys, the code and the joiner's random handshake
// nonce, so every join gets new ones: a machine that leaves and joins again starts its
// message counter from 1 under keys never used before.
static bool derive(const uint8_t peer_pub[32], const uint8_t client_pub[32], const uint8_t host_pub[32], const uint8_t psk[32],
                   const uint8_t hs_nonce[16], bool host, session &out)
{
    uint8_t shared[32], k[64], zero[32] = {0};
    crypto_x25519(shared, g_priv, peer_pub);
    if (crypto_verify32(shared, zero) == 0) return false; // a bad public key
    crypto_blake2b_ctx x;
    crypto_blake2b_init(&x, 64);
    crypto_blake2b_update(&x, shared, 32);
    crypto_blake2b_update(&x, client_pub, 32);
    crypto_blake2b_update(&x, host_pub, 32);
    crypto_blake2b_update(&x, psk, 32);
    crypto_blake2b_update(&x, hs_nonce, 16);
    crypto_blake2b_final(&x, k);
    memcpy(out.hs_nonce, hs_nonce, 16);
    memcpy(out.tx, host ? k + 32 : k, 32);
    memcpy(out.rx, host ? k : k + 32, 32);
    memcpy(out.peer_pub, peer_pub, 32);
    crypto_wipe(shared, sizeof shared);
    crypto_wipe(k, sizeof k);
    return true;
}
static void send_plain(const sockaddr_in &to, uint16_t type, const void *body, size_t len)
{
    char packet[sizeof(header) + 256];
    if (len > 256) return;
    header h = {k_magic, k_version, type, g_my_id};
    memcpy(packet, &h, sizeof(h));
    memcpy(packet + sizeof(h), body, len);
    put_wire(to, packet, (int)(sizeof(h) + len));
}
static void send_hs_init()
{
    hs_init_msg m = {};
    memcpy(m.pub, g_pub, 32);
    memcpy(m.cookie, g_pending.cookie, 16);
    memcpy(m.nonce, g_pending.nonce, 16);
    uint8_t ak[32];
    auth_key(g_join_psk, ak);
    mac16(ak, "init", m.pub, 32, m.cookie, 16, m.nonce, 16, m.mac);
    g_pending.sent = ms();
    send_plain(g_pending.addr, t_hs_init, &m, sizeof m);
}
// Client: start (or repeat, at most twice a second) the handshake with the host.
static void kick_handshake()
{
    if (g_role != k_client || !g_pending.active) return;
    if (ms() - g_pending.sent < 500) return;
    send_hs_init();
}
static void cookie_for(const sockaddr_in &a, int64_t epoch, uint8_t out[16])
{
    uint8_t msg[14];
    memcpy(msg, &a.sin_addr.s_addr, 4);
    memcpy(msg + 4, &a.sin_port, 2);
    memcpy(msg + 6, &epoch, 8);
    crypto_blake2b_keyed(out, 16, g_cookie_secret, 32, msg, sizeof msg);
}
// Host: at most 10 handshakes a second from one address, 200 from everyone.
static bool handshake_allowed(const sockaddr_in &a)
{
    static std::mutex lock;
    static std::map<uint32_t, std::pair<double, int64_t>> buckets;
    static double all = 200;
    static int64_t all_at = 0;
    std::lock_guard<std::mutex> l(lock);
    int64_t now = ms();
    all = std::min(200.0, all + (now - all_at) * 0.2), all_at = now;
    if (all < 1) return false;
    if (buckets.size() > 512) buckets.clear();
    auto &b = buckets[a.sin_addr.s_addr];
    if (!b.second) b.first = 20;
    b.first = std::min(20.0, b.first + (now - b.second) * 0.01), b.second = now;
    if (b.first < 1) return false;
    b.first -= 1, all -= 1;
    return true;
}
// Seal a datagram for a machine there is a secure connection with.
static bool seal(const sockaddr_in &to, const char *p, int n, std::vector<char> &out)
{
    std::lock_guard<std::mutex> l(g_sess_lock);
    session *s = find_session(to);
    if (!s) return false;
    uint64_t ctr = ++s->tx_ctr;
    out.resize(sizeof(header) + 8 + 16 + n);
    header h = {k_magic, k_version, t_sealed, g_my_id};
    memcpy(out.data(), &h, sizeof(h));
    memcpy(out.data() + sizeof(h), &ctr, 8);
    uint8_t nonce[24] = {0};
    memcpy(nonce, &ctr, 8);
    crypto_aead_lock((uint8_t *)out.data() + sizeof(h) + 24, (uint8_t *)out.data() + sizeof(h) + 8, s->tx, nonce,
                     (const uint8_t *)out.data(), sizeof(h) + 8, (const uint8_t *)p, (size_t)n);
    return true;
}
// Open a sealed datagram: false if it isn't genuine, is a repeat, or comes from nobody known.
static bool open_sealed(const sockaddr_in &from, const char *packet, int n, std::vector<char> &plain)
{
    const int head = (int)sizeof(header) + 8 + 16;
    if (n < head + (int)sizeof(header)) return false;
    uint64_t ctr;
    memcpy(&ctr, packet + sizeof(header), 8);
    std::lock_guard<std::mutex> l(g_sess_lock);
    session *s = find_session(from);
    if (!s || !ctr) return false;
    if (ctr <= s->rx_max)
    {
        uint64_t back = s->rx_max - ctr;
        if (back >= 64 || (s->rx_bits >> back) & 1) return false; // too old, or already seen
    }
    uint8_t nonce[24] = {0};
    memcpy(nonce, &ctr, 8);
    plain.resize((size_t)(n - head));
    if (crypto_aead_unlock((uint8_t *)plain.data(), (const uint8_t *)packet + sizeof(header) + 8, s->rx, nonce,
                           (const uint8_t *)packet, sizeof(header) + 8, (const uint8_t *)packet + head, (size_t)(n - head)))
        return false;
    if (ctr > s->rx_max)
    {
        uint64_t ahead = ctr - s->rx_max;
        s->rx_bits = ahead >= 64 ? 1 : (s->rx_bits << ahead) | 1;
        s->rx_max = ctr;
    }
    else s->rx_bits |= 1ull << (s->rx_max - ctr);
    s->heard = ms();
    // The machine address inside can't change: a joined machine can't speak for another.
    header inner;
    memcpy(&inner, plain.data(), sizeof(inner));
    if (!s->peer_id) s->peer_id = inner.from;
    else if (s->peer_id != inner.from) return false;
    return true;
}
// The handshake messages (receive thread).
static void handle_handshake(const sockaddr_in &from, uint16_t type, const char *body, int len)
{
    int role = g_role;
    if (type == t_hs_init)
    {
        if (len < (int)sizeof(hs_init_msg) || !handshake_allowed(from)) return;
        hs_init_msg m;
        memcpy(&m, body, sizeof m);
        if (role != k_host)
        {
            hs_refuse_msg r = {};
            memcpy(r.nonce, m.nonce, 16);
            r.reason = 1;
            send_plain(from, t_hs_refuse, &r, sizeof r);
            return;
        }
        uint8_t psk[32] = {0}, ak[32], mac[16];
        bool ok = false, code = false;
        if (is_private(from))
        {
            auth_key(psk, ak);
            mac16(ak, "init", m.pub, 32, m.cookie, 16, m.nonce, 16, mac);
            ok = crypto_verify16(mac, m.mac) == 0;
        }
        if (!ok && g_online)
        {
            memcpy(psk, g_online_psk, 32);
            auth_key(psk, ak);
            mac16(ak, "init", m.pub, 32, m.cookie, 16, m.nonce, 16, mac);
            ok = code = crypto_verify16(mac, m.mac) == 0;
        }
        if (!ok)
        {
            static std::atomic<int> logged;
            if (logged++ < 10)
                logf("lan: refused a join from %s: %s", ip_text(from).c_str(),
                     g_online ? "wrong join code" : "from outside the home network, and this lobby isn't online");
            hs_refuse_msg r = {};
            memcpy(r.nonce, m.nonce, 16);
            r.reason = 2;
            send_plain(from, t_hs_refuse, &r, sizeof r);
            return;
        }
        // A cookie proves the joiner receives at the address it claims (a forged address
        // gets nothing but a short reply, and no state is kept for it).
        int64_t epoch = ms() / 30000;
        uint8_t now_c[16], prev_c[16];
        cookie_for(from, epoch, now_c);
        cookie_for(from, epoch - 1, prev_c);
        if (crypto_verify16(m.cookie, now_c) && crypto_verify16(m.cookie, prev_c))
        {
            hs_cookie_msg c;
            memcpy(c.cookie, now_c, 16);
            memcpy(c.nonce, m.nonce, 16);
            send_plain(from, t_hs_cookie, &c, sizeof c);
            return;
        }
        bool fresh = true;
        {
            std::lock_guard<std::mutex> l(g_sess_lock);
            session *had = find_session(from);
            // The same handshake again (its answer was lost): answer again, keep the session.
            // A new handshake from the same machine (it left and is joining again) replaces it.
            if (had && crypto_verify32(had->peer_pub, m.pub) == 0 && crypto_verify16(had->hs_nonce, m.nonce) == 0) fresh = false;
            else
            {
                session ns;
                ns.addr = from;
                ns.code = code;
                ns.heard = ms();
                if (!derive(m.pub, m.pub, g_pub, psk, m.nonce, true, ns)) return;
                if (had) *had = ns;
                else if (g_sessions.size() < 24) g_sessions.push_back(ns);
                else return;
            }
        }
        hs_resp_msg r;
        memcpy(r.pub, g_pub, 32);
        memcpy(r.nonce, m.nonce, 16);
        mac16(ak, "resp", g_pub, 32, m.pub, 32, m.nonce, 16, r.mac);
        send_plain(from, t_hs_resp, &r, sizeof r);
        if (fresh) logf("lan: secure connection with %s (%s)", ip_text(from).c_str(), code ? "join code" : "home network");
        return;
    }
    if (role != k_client || !g_pending.active || !same_addr(from, g_pending.addr)) return;
    if (type == t_hs_cookie && len >= (int)sizeof(hs_cookie_msg))
    {
        hs_cookie_msg c;
        memcpy(&c, body, sizeof c);
        if (memcmp(c.nonce, g_pending.nonce, 16)) return;
        memcpy(g_pending.cookie, c.cookie, 16);
        send_hs_init();
    }
    else if (type == t_hs_resp && len >= (int)sizeof(hs_resp_msg))
    {
        hs_resp_msg r;
        memcpy(&r, body, sizeof r);
        if (memcmp(r.nonce, g_pending.nonce, 16)) return;
        uint8_t ak[32], mac[16];
        auth_key(g_join_psk, ak);
        mac16(ak, "resp", r.pub, 32, g_pub, 32, r.nonce, 16, mac);
        if (crypto_verify16(mac, r.mac)) return; // not the host the code names
        session ns;
        ns.addr = from;
        ns.heard = ms();
        if (!derive(r.pub, g_pub, r.pub, g_join_psk, g_pending.nonce, false, ns)) return;
        {
            std::lock_guard<std::mutex> l(g_sess_lock);
            g_sessions.clear();
            g_sessions.push_back(ns);
        }
        g_pending.active = false;
        logf("lan: secure connection with the host");
    }
    else if (type == t_hs_refuse && len >= (int)sizeof(hs_refuse_msg))
    {
        hs_refuse_msg r;
        memcpy(&r, body, sizeof r);
        if (memcmp(r.nonce, g_pending.nonce, 16)) return;
        g_pending.active = false;
        dropped_by_host(r.reason == 1 ? "That machine isn't hosting a game"
                                      : "The host turned the join down: check the join code (an internet join needs the host's code)");
    }
}

// ---- The online server. It matches a joiner with the host whose
// code it has, so they can connect directly, and passes their packets on when they can't
// (both are behind routers that won't let the other in). Its own messages start "I08S";
// relayed packets start "I08R" and an 8-byte tag naming the pair. A machine reached through
// it gets a stand-in address in 240.0.0.0/8, so the rest of this code treats it like any
// other; the packets themselves stay encrypted end to end, and the server can't read them.
static sockaddr_in g_server;                 // resolved when going online or joining with a code
struct relay_route { uint64_t tag; sockaddr_in fake; };
static std::mutex g_relay_lock;
static std::vector<relay_route> g_relays;
static bool is_relayed(const sockaddr_in &a) { return (ntohl(a.sin_addr.s_addr) >> 24) == 240; }
static sockaddr_in relay_address(uint64_t tag)
{
    std::lock_guard<std::mutex> l(g_relay_lock);
    for (const auto &r : g_relays)
        if (r.tag == tag) return r.fake;
    static uint32_t next = 1;
    relay_route r;
    r.tag = tag;
    r.fake = {};
    r.fake.sin_family = AF_INET;
    r.fake.sin_addr.s_addr = htonl(0xF0000000u | (next++ & 0xFFFFFF));
    r.fake.sin_port = htons(1);
    if (g_relays.size() >= 512) g_relays.erase(g_relays.begin());
    g_relays.push_back(r);
    return r.fake;
}
static bool relay_tag(const sockaddr_in &fake, uint64_t &tag)
{
    std::lock_guard<std::mutex> l(g_relay_lock);
    for (const auto &r : g_relays)
        if (r.fake.sin_addr.s_addr == fake.sin_addr.s_addr) return tag = r.tag, true;
    return false;
}
static bool from_server(const sockaddr_in &a)
{
    return g_server.sin_port && a.sin_addr.s_addr == g_server.sin_addr.s_addr && a.sin_port == g_server.sin_port;
}

static void wire_send(const sockaddr_in &to_in, const char *data, int len)
{
    sockaddr_in to = to_in;
    static thread_local std::vector<char> wrapped;
    if (is_relayed(to))
    {
        uint64_t tag;
        if (!relay_tag(to, tag) || !g_server.sin_port) return;
        wrapped.resize(12 + (size_t)len);
        memcpy(wrapped.data(), "I08R", 4);
        memcpy(wrapped.data() + 4, &tag, 8);
        memcpy(wrapped.data() + 12, data, (size_t)len);
        data = wrapped.data(), len += 12, to = g_server;
    }
    // Nowhere to send yet (joining with a code, before the server answers): the
    // handshake goes out again once the host's address is known.
    if (!to.sin_port) return;
    for (int tries = 0;; tries++)
    {
        if (sendto(g_sock, data, len, 0, (const sockaddr *)&to, sizeof(to)) != SOCKET_ERROR) break;
        int e = WSAGetLastError();
        // The PS5 (FreeBSD) says "no buffer space" when sending faster than the network
        // takes it, instead of waiting: wait a moment and send again.
        if (e == WSAENOBUFS && tries < 200) { Sleep(1); continue; }
        static std::atomic<int> logged;
        if (logged++ < 5) logf("lan: sending %d bytes failed (error %d)", len, e);
        g_dropped++;
        break;
    }
}
static void send_one(const sockaddr_in &to, uint16_t type, const void *body, size_t len)
{
    static thread_local std::vector<char> packet(sizeof(header) + k_frag + sizeof(frag_head) + 64);
    if (sizeof(header) + len > packet.size()) packet.resize(sizeof(header) + len);
    header h = {k_magic, k_version, type, g_my_id};
    memcpy(packet.data(), &h, sizeof(h));
    if (len) memcpy(packet.data() + sizeof(h), body, len);
    int total = (int)(sizeof(h) + len);
    if (plain_type(type)) { put_wire(to, packet.data(), total); return; }
    static thread_local std::vector<char> sealed;
    if (!seal(to, packet.data(), total, sealed))
    {
        // No secure connection yet: a client starts (or repeats) the handshake; the
        // message itself is dropped (hellos repeat every second).
        if (g_role == k_client && same_addr(to, g_join_addr)) kick_handshake();
        return;
    }
    put_wire(to, sealed.data(), (int)sealed.size());
}
static void raw_send(const sockaddr_in &to, uint16_t type, const void *body, size_t len)
{
    if (g_sock == INVALID_SOCKET) return;
    if (len <= k_frag) { send_one(to, type, body, len); return; }
    static std::atomic<uint32_t> next_id{1};
    uint32_t id = next_id++;
    std::vector<char> piece(sizeof(frag_head) + k_frag);
    for (uint32_t at = 0; at < len; at += k_frag)
    {
        uint32_t n = std::min<uint32_t>(k_frag, (uint32_t)len - at);
        frag_head fh = {id, (uint32_t)len, at, type, 0};
        memcpy(piece.data(), &fh, sizeof(fh));
        memcpy(piece.data() + sizeof(fh), (const char *)body + at, n);
        send_one(to, t_frag, piece.data(), sizeof(fh) + n);
    }
}

// Receiving end: put the pieces back together (receive thread only).
struct frag_buf { uint64_t from; uint32_t id; uint16_t type; std::vector<char> data; std::vector<bool> got; uint32_t have; int64_t started; };
static bool reassemble(uint64_t from, const char *body, int len, std::vector<char> &out, uint16_t &type)
{
    static std::vector<frag_buf> bufs;
    if (len < (int)sizeof(frag_head)) return false;
    frag_head fh;
    memcpy(&fh, body, sizeof(fh));
    uint32_t n = (uint32_t)len - sizeof(fh);
    if (fh.total > (1u << 20) || fh.offset % k_frag || fh.offset >= fh.total || n != std::min(k_frag, fh.total - fh.offset)) return false;
    int64_t now = (int64_t)GetTickCount64();
    bufs.erase(std::remove_if(bufs.begin(), bufs.end(), [now](const frag_buf &b) { return now - b.started > 10000; }), bufs.end());
    frag_buf *b = nullptr;
    for (auto &x : bufs)
        if (x.from == from && x.id == fh.id) b = &x;
    if (!b)
    {
        if (bufs.size() >= 256) bufs.erase(bufs.begin());
        bufs.push_back({from, fh.id, fh.type, std::vector<char>(fh.total), std::vector<bool>((fh.total + k_frag - 1) / k_frag), 0, now});
        b = &bufs.back();
    }
    if (b->data.size() != fh.total) return false;
    uint32_t index = fh.offset / k_frag;
    if (!b->got[index])
    {
        b->got[index] = true;
        b->have++;
        memcpy(b->data.data() + fh.offset, body + sizeof(fh), n);
    }
    if (b->have < b->got.size()) return false;
    out.swap(b->data);
    type = b->type;
    bufs.erase(bufs.begin() + (b - bufs.data()));
    return true;
}

static bool find_peer(uint64_t id, sockaddr_in *addr)
{
    std::lock_guard<std::mutex> l(g_lock);
    for (const auto &p : g_peers)
        if (p.id == id) { if (addr) *addr = p.addr; return true; }
    return false;
}

static void log_packet(const char *dir, uint64_t id, uint32_t port, uint32_t len)
{
    if (!g_verbose_log) return;
    int n = ++g_logged;
    if (n <= 20) logf("lan: %s %u bytes, game port %u, address %llx", dir, len, port, (unsigned long long)id);
    else if (n == 21) logf("lan: (further packets are counted, not logged)");
}

// A packet the game sends to another machine.
// A client leaving a game the host is still playing: its game traffic stops at once.
// The newer engines (Halo 2: Anniversary at least) don't end a client's game while the
// host is still talking to it; once the host goes quiet they see it as gone and end
// within a few seconds. Without this they took over 20 seconds.
static std::atomic<bool> g_traffic_cut;
static void send_game(uint64_t to, uint32_t port, const char *buf, uint32_t len)
{
    if (g_traffic_cut) return;
    sockaddr_in addr;
    if (!find_peer(to, &addr))
    {
        bool player = false;
        for (const auto &st : g_seats)
            if (st.address == to) player = true;
        if (player && g_role == k_client)
        {
            // Joined machines only know the host: this game talks between joined machines too.
            if (++g_unrouted <= 5)
                logf("lan: the game sent %u bytes (game port %u) to another joined machine (address %llx); only the host can reach it, so it was dropped",
                     len, port, (unsigned long long)to);
            return;
        }
        if (++g_dropped <= 10) logf("lan: no machine has address %llx; packet dropped", (unsigned long long)to);
        return;
    }
    char body[1516];
    if (len + 16 > sizeof(body)) { if (++g_dropped <= 10) logf("lan: packet of %u bytes too large; dropped", len); return; }
    // One count per receiving machine, so each sees its own numbers without gaps (one
    // count for all made every joined machine report the other's packets as lost).
    uint32_t seq;
    {
        static std::mutex seq_lock;
        static std::map<uint64_t, uint32_t> next_seq;
        std::lock_guard<std::mutex> l(seq_lock);
        seq = ++next_seq[to];
    }
    memcpy(body, &port, 4);
    memcpy(body + 4, &seq, 4);
    int64_t stamp = now_us(); // for smoothing on a co-op client
    memcpy(body + 8, &stamp, 8);
    memcpy(body + 16, buf, len);
    raw_send(addr, t_game, body, len + 16);
    g_sent++;
    g_game_bytes_out += len;
    note_max(g_max_game_out, len);
    log_packet("sent", to, port, len);
}

static bool same_machine(const sockaddr_in &a, const sockaddr_in &b)
{
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

// The client was told it's no longer in the host's game (or lost it).
static void dropped_by_host(const char *why)
{
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_peers.clear();
        g_left_reason = why;
    }
    g_welcomed = false;
    g_role = k_off;
    g_end_ready = true; // if it's in the game, leave it
    logf("lan: %s", why);
}

static void receive_thread()
{
    static std::vector<char> buffer(65536);
    char *packet = buffer.data();
    // It sleeps in recvfrom almost all the time; above normal lets it run soon after a
    // packet comes (time-critical was tried and felt worse in co-op).
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;)
    {
        sockaddr_in from = {};
        int from_len = sizeof(from);
        int n = recvfrom(g_sock, packet, (int)buffer.size(), 0, (sockaddr *)&from, &from_len);
        if (n == SOCKET_ERROR)
        {
            int e = WSAGetLastError();
            if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue; // a peer went away; a stray datagram
            logf("lan: receive failed (error %d); LAN play stopped", e);
            return;
        }
        g_wire_in += n + 28;
        if (sim_take(false, from, packet, n)) continue;
        process_datagram(from, packet, n);
    }
}

// One datagram from another machine (the receive thread, or the network test's thread).
static void server_message(const char *packet, int n);
static void process_datagram(const sockaddr_in &from_in, const char *packet, int n)
{
    static std::mutex one_at_a_time;
    std::lock_guard<std::mutex> guard(one_at_a_time);
    sockaddr_in from = from_in;
    if (n >= 4 && from_server(from))
    {
        if (!memcmp(packet, "I08S", 4)) { server_message(packet, n); return; }
        if (n < 12 || memcmp(packet, "I08R", 4)) return;
        uint64_t tag;
        memcpy(&tag, packet + 4, 8);
        from = relay_address(tag); // relayed: from the machine at the other end
        packet += 12, n -= 12;
    }
    if (n < (int)sizeof(header)) return;
    header h;
    memcpy(&h, packet, sizeof(h));
    if (h.magic != k_magic || h.version != k_version || h.from == g_my_id) return;
    static std::vector<char> opened;
    if (h.type == t_sealed)
    {
        if (!open_sealed(from, packet, n, opened))
        {
            // The host may have restarted: a client starts over.
            if (g_role == k_client && same_addr(from, g_join_addr)) kick_handshake();
            return;
        }
        packet = opened.data();
        n = (int)opened.size();
        memcpy(&h, packet, sizeof(h));
        if (h.magic != k_magic || h.version != k_version || h.type == t_sealed || h.from == g_my_id) return;
    }
    else if (!plain_type(h.type)) return; // nothing else travels in the clear
    else if (h.type != t_find && h.type != t_here)
    {
        handle_handshake(from, h.type, packet + sizeof(h), n - (int)sizeof(h));
        return;
    }
    const char *body = packet + sizeof(h);
    int len = n - (int)sizeof(h);
    static std::vector<char> whole;
    if (h.type == t_frag)
    {
        uint16_t inner = 0;
        if (!reassemble(h.from, body, len, whole, inner)) return;
        h.type = inner;
        body = whole.data();
        len = (int)whole.size();
    }
    int role = g_role;
    switch (h.type)
    {
    case t_game:
    {
        if (g_traffic_cut || len < 16 || !find_peer(h.from, nullptr)) break;
        uint32_t port, seq;
        int64_t stamp;
        memcpy(&port, body, 4);
        memcpy(&seq, body + 4, 4);
        memcpy(&stamp, body + 8, 8);
        {
            int slot = 0;
            while (slot < 15 && g_seq_peer[slot] && g_seq_peer[slot] != h.from) slot++;
            if (g_seq_peer[slot] != h.from) g_seq_peer[slot] = h.from, g_last_seq_from[slot] = seq - 1;
            uint32_t expect = g_last_seq_from[slot] + 1;
            if (seq == expect) g_last_seq_from[slot] = seq;
            else if ((int32_t)(seq - expect) > 0)
            {
                g_lost += seq - expect;
                if (g_verbose_log) logf("lan: %u game packet(s) from %llx lost on the way", seq - expect, (unsigned long long)h.from);
                g_last_seq_from[slot] = seq;
            }
            else g_late++;
        }
        {
            int64_t t = now_us();
            std::lock_guard<std::mutex> tl(g_timing_lock);
            if (g_last_game_in_us) g_gap_max_us = std::max(g_gap_max_us, t - g_last_game_in_us);
            g_last_game_in_us = t;
        }
        // Only the host sends to a client, so on a client every game packet is the host's.
        int64_t ready = (g_smooth && role == k_client) ? smooth::ready_time(stamp, now_us()) : 0;
        net_deliver(body + 16, (uint32_t)(len - 16), h.from, port, ready);
        g_received++;
        g_game_bytes_in += len - 16;
        note_max(g_max_game_in, (uint32_t)(len - 16));
        log_packet("received", h.from, port, (uint32_t)(len - 16));
        break;
    }
    case t_find: // someone is looking for games (on the home network only)
    {
        if (role != k_host || !is_private(from)) break;
        lobby_msg m;
        {
            std::lock_guard<std::mutex> l(g_lock);
            m = g_lobby_out;
        }
        raw_send(from, t_here, &m, sizeof(m));
        break;
    }
    case t_here:
    {
        if (len < (int)sizeof(lobby_msg) || ms() > g_searching_until || !is_private(from)) break;
        std::lock_guard<std::mutex> l(g_lock);
        found_game *f = nullptr;
        for (auto &g : g_found)
            if (g.id == h.from) f = &g;
        if (!f) g_found.push_back({}), f = &g_found.back();
        f->id = h.from, f->addr = from, f->seen = ms();
        memcpy(&f->info, body, sizeof(lobby_msg));
        break;
    }
    case t_hello: // a machine joining (or still there)
    {
        if (role != k_host)
        {
            raw_send(from, t_bye, nullptr, 0); // not hosting (any more)
            break;
        }
        std::string name = field(body, std::min(len, 32));
        hello_msg hm = {};
        if (len >= (int)sizeof(hello_msg)) memcpy(&hm, body, sizeof(hm));
        bool is_new = true, full = false;
        {
            std::lock_guard<std::mutex> l(g_lock);
            for (auto &p : g_peers)
                if (p.id == h.from) p.addr = from, p.name = name, p.heard = ms(), p.key = hm.key, p.status = hm.status, is_new = false;
            if (is_new)
            {
                if (g_peers.size() < 15) g_peers.push_back({h.from, from, name, ms(), hm.key, hm.status});
                else full = true;
            }
        }
        if (full) break;
        if (is_new) logf("lan: %s joined from %s (address %llx)", name.c_str(), ip_text(from).c_str(), (unsigned long long)h.from);
        char reply[32];
        put(reply, sizeof reply, g_name);
        raw_send(from, t_welcome, reply, sizeof(reply));
        break;
    }
    case t_welcome:
    {
        if (role != k_client || !same_machine(from, g_join_addr)) break;
        std::string name = field(body, std::min(len, 32));
        {
            std::lock_guard<std::mutex> l(g_lock);
            g_peers.assign(1, {h.from, from, name, ms()});
        }
        g_host_heard = ms();
        if (!g_welcomed.exchange(true))
            logf("lan: joined %s at %s (address %llx)", name.c_str(), ip_text(from).c_str(), (unsigned long long)h.from);
        break;
    }
    case t_lobby:
    {
        if (role != k_client || len < (int)sizeof(lobby_msg) || !find_peer(h.from, nullptr)) break;
        std::lock_guard<std::mutex> l(g_lock);
        memcpy(&g_lobby_in, body, sizeof(lobby_msg));
        g_host_heard = ms();
        break;
    }
    case t_start:
    {
        if (role != k_client || !find_peer(h.from, nullptr) || len < (int)k_start_head) break;
        // The host sends each start a few times; only the first copy counts.
        uint32_t id, size;
        memcpy(&id, body + offsetof(start_msg, game_id), sizeof(id));
        memcpy(&size, body + offsetof(start_msg, variant_size), sizeof(size));
        raw_send(from, t_start_ack, &id, sizeof(id)); // every copy: the host resends until it hears this
        if (id == g_last_game_id || size > sizeof(start_msg::variant_data) || (size_t)len < k_start_head + size) break;
        g_last_game_id = id;
        std::lock_guard<std::mutex> l(g_start_lock);
        memcpy(&g_start, body, k_start_head + size);
        g_start_ready = true;
        break;
    }
    case t_map:
    {
        if (role != k_client || !find_peer(h.from, nullptr) || len < (int)offsetof(map_msg, data)) break;
        uint32_t id, size;
        memcpy(&id, body + offsetof(map_msg, game_id), sizeof(id));
        memcpy(&size, body + offsetof(map_msg, size), sizeof(size));
        if (size > sizeof(map_msg::data) || (size_t)len < offsetof(map_msg, data) + size) break;
        std::lock_guard<std::mutex> l(g_start_lock);
        if (g_map_in.game_id == id && g_map_in.size == size) break;
        memcpy(&g_map_in, body, offsetof(map_msg, data) + size);
        break;
    }
    case t_start_ack:
    {
        if (role != k_host || len < 4) break;
        uint32_t id;
        memcpy(&id, body, 4);
        if (id && id == g_acking_id)
        {
            std::lock_guard<std::mutex> l(g_lock);
            g_start_acked.insert(h.from);
        }
        break;
    }
    case t_end:
        if (role == k_client && find_peer(h.from, nullptr)) g_end_ready = true;
        break;
    case t_save: // a piece of the host's saved game (co-op continue)
    {
        if (role != k_client || len < (int)sizeof(save_piece) || !find_peer(h.from, nullptr)) break;
        save_piece sp;
        memcpy(&sp, body, sizeof(sp));
        if (!sp.total || sp.total > k_piece * k_most_pieces || sp.pieces != (sp.total + k_piece - 1) / k_piece ||
            sp.index >= sp.pieces)
            break;
        uint32_t at = sp.index * k_piece, size = std::min(k_piece, sp.total - at);
        if ((size_t)len < sizeof(sp) + size) break;
        std::lock_guard<std::mutex> l(g_start_lock);
        if (g_save_in_id != sp.game_id || g_save_in.size() != sp.total)
        {
            g_save_in_id = sp.game_id;
            g_save_in.assign(sp.total, 0);
            g_save_in_have.assign(k_most_pieces / 8, 0);
        }
        memcpy(g_save_in.data() + at, body + sizeof(sp), size);
        g_save_in_have[sp.index / 8] |= (uint8_t)(1 << (sp.index % 8));
        break;
    }
    case t_save_query: // the host asks which pieces arrived
    {
        if (role != k_client || len < 4 || !find_peer(h.from, nullptr)) break;
        static save_status st;
        memset(&st, 0, sizeof(st));
        memcpy(&st.game_id, body, 4);
        {
            std::lock_guard<std::mutex> l(g_start_lock);
            if (g_save_in_id == st.game_id && !g_save_in_have.empty()) memcpy(st.have, g_save_in_have.data(), sizeof(st.have));
        }
        raw_send(from, t_save_status, &st, sizeof(st));
        break;
    }
    case t_save_status:
    {
        if (role != k_host || len < (int)sizeof(save_status)) break;
        save_status st;
        memcpy(&st, body, sizeof(st));
        std::lock_guard<std::mutex> l(g_lock);
        save_report *r = nullptr;
        for (auto &x : g_save_reports)
            if (x.id == h.from) r = &x;
        if (!r) g_save_reports.push_back({h.from, 0, {}, 0}), r = &g_save_reports.back();
        r->game_id = st.game_id;
        r->have.assign(st.have, st.have + sizeof(st.have));
        r->answers++;
        break;
    }
    case t_save_offer: // the host is about to send its saved game: use what's here
    {
        if (role != k_client || len < (int)offsetof(save_offer, hash) || !find_peer(h.from, nullptr)) break;
        static save_offer so;
        memset(&so, 0, sizeof(so));
        memcpy(&so, body, std::min<size_t>(len, sizeof(so)));
        if (!so.total || so.total > k_piece * k_most_pieces || so.pieces != (so.total + k_piece - 1) / k_piece ||
            (size_t)len < offsetof(save_offer, hash) + so.pieces * sizeof(uint64_t))
            break;
        {
            std::lock_guard<std::mutex> l(g_start_lock);
            if (g_save_in_id == so.game_id && g_save_in.size() == so.total) break; // already set up
        }
        std::string mine = g_read_coop_save ? g_read_coop_save(field(so.game, sizeof(so.game)).c_str()) : std::string();
        std::lock_guard<std::mutex> l(g_start_lock);
        g_save_in_id = so.game_id;
        g_save_in.assign(so.total, 0);
        g_save_in_have.assign(k_most_pieces / 8, 0);
        uint32_t same = 0;
        for (uint32_t i = 0; i < so.pieces; i++)
        {
            uint32_t at = i * k_piece, size = std::min(k_piece, so.total - at);
            if (at + size > mine.size() || fnv64(mine.data() + at, size) != so.hash[i]) continue;
            memcpy(g_save_in.data() + at, mine.data() + at, size);
            g_save_in_have[i / 8] |= (uint8_t)(1 << (i % 8));
            same++;
        }
        // Mostly the same: it's this machine's save of the host's checkpoint. Continue
        // from it, and tell the host it has everything.
        if (mine.size() == so.total && same * 2 >= so.pieces)
        {
            g_save_own_id = so.game_id;
            g_save_own.assign(mine.begin(), mine.end());
            for (uint32_t i = 0; i < so.pieces; i++) g_save_in_have[i / 8] |= (uint8_t)(1 << (i % 8));
            logf("lan: the host's co-op saved game: %u of %u piece(s) the same as this machine's; continuing from this machine's own", same,
                 so.pieces);
        }
        else
            logf("lan: the host's co-op saved game: %u of %u piece(s) already here; getting the rest", same, so.pieces);
        break;
    }
    case t_ping: // the other machine timing the way here and back
        if (len >= 8 && find_peer(h.from, nullptr)) raw_send(from, t_pong, body, 8);
        break;
    case t_pong:
        if (len >= 8 && find_peer(h.from, nullptr))
        {
            int64_t sent;
            memcpy(&sent, body, 8);
            int64_t rtt = now_us() - sent;
            if (rtt >= 0 && rtt < 10000000)
            {
                std::lock_guard<std::mutex> tl(g_timing_lock);
                g_rtt_min_us = g_rtt_n ? std::min(g_rtt_min_us, rtt) : rtt;
                g_rtt_max_us = std::max(g_rtt_max_us, rtt), g_rtt_sum_us += rtt, g_rtt_n++;
            }
        }
        break;
    case t_bye:
        if (role == k_host)
        {
            std::lock_guard<std::mutex> l(g_lock);
            for (size_t i = 0; i < g_peers.size(); i++)
                if (g_peers[i].id == h.from)
                {
                    logf("lan: %s left", g_peers[i].name.c_str());
                    g_peers.erase(g_peers.begin() + i);
                    break;
                }
        }
        else if (role == k_client && (find_peer(h.from, nullptr) || same_machine(from, g_join_addr)))
            dropped_by_host(g_welcomed ? "The host closed the game" : "That machine isn't hosting a game");
        break;
    }
}

// Where a "who's hosting?" goes: everywhere on this machine's networks.
static std::vector<sockaddr_in> broadcast_addresses()
{
    std::vector<sockaddr_in> out;
    sockaddr_in all = {};
    all.sin_family = AF_INET;
    all.sin_port = htons(g_port);
    all.sin_addr.s_addr = INADDR_BROADCAST;
    out.push_back(all);
    INTERFACE_INFO list[32];
    DWORD bytes = 0;
    if (WSAIoctl(g_sock, SIO_GET_INTERFACE_LIST, nullptr, 0, list, sizeof(list), &bytes, nullptr, nullptr) == 0)
        for (DWORD i = 0; i < bytes / sizeof(INTERFACE_INFO); i++)
        {
            const INTERFACE_INFO &f = list[i];
            if (!(f.iiFlags & IFF_UP) || (f.iiFlags & IFF_LOOPBACK) || f.iiAddress.Address.sa_family != AF_INET) continue;
            sockaddr_in b = all;
            b.sin_addr.s_addr = f.iiAddress.AddressIn.sin_addr.s_addr | ~f.iiNetmask.AddressIn.sin_addr.s_addr;
            if (b.sin_addr.s_addr != INADDR_BROADCAST) out.push_back(b);
        }
    return out;
}

// Every quarter second: joining machines say hello, the host tells them about its game
// and forgets machines it hasn't heard from, and the join page looks for games.
struct online_join;
struct public_game;
static int64_t g_list_asked;
static void ask_for_list(int page);
static bool resolve_server();
static std::mutex g_public_lock;
static void prune_public_list(int64_t t);
static void online_tick(int64_t t);
static bool g_ojoin_active();
static void tick_thread()
{
    int64_t last_hello = 0, last_lobby = 0, last_find = 0;
    hello_msg hello;
    uint64_t last_sent = 0, last_received = 0, last_lost = 0;
    int64_t last_stats = 0, qpc_start = 0, tick_start = 0;
    int64_t rate_at = 0, second_at = 0;
    uint64_t rate_out = 0, rate_in = 0, rate_game_out = 0, rate_game_in = 0, second_out = 0, peak_out = 0, peak_in = 0, second_in = 0;
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    for (;;)
    {
        Sleep(250);
        int64_t t = ms();
        int role = g_role;
        // With verbose_log, in a LAN game: traffic each second, and both clocks (a game
        // that thinks the other machine went quiet may be seeing a clock that drifts).
        if (g_verbose_log && !g_seats.empty() && t - last_stats >= 1000)
        {
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            if (!last_stats) qpc_start = q.QuadPart, tick_start = t;
            uint64_t s = g_sent, r = g_received, l = g_lost;
            logf("lan stats: sent %llu, received %llu, lost %llu in the last second; clocks: tick %lld ms, qpc %lld ms",
                 (unsigned long long)(s - last_sent), (unsigned long long)(r - last_received), (unsigned long long)(l - last_lost),
                 (long long)(t - tick_start), (long long)((q.QuadPart - qpc_start) * 1000 / qf.QuadPart));
            last_sent = s, last_received = r, last_lost = l, last_stats = t;
        }
        if (g_seats.empty()) last_stats = 0, last_sent = last_received = last_lost = 0;
        online_tick(t);
        if (role == k_client && g_pending.active && !g_pending.said && !g_ojoin_active() && t - g_pending.began > 8000)
        {
            g_pending.said = true;
            logf("lan: no answer from %s after 8 seconds (%s)", ip_text(g_pending.addr).c_str(),
                 g_pending.cookie[0] | g_pending.cookie[1] ? "it answered once, then went quiet"
                                                           : "nothing at all came back: its router may not be forwarding the port to it");
        }
        {
            std::lock_guard<std::mutex> l(g_sess_lock);
            g_sessions.erase(std::remove_if(g_sessions.begin(), g_sessions.end(),
                                            [t](const session &x) { return t - x.heard > 60000; }),
                             g_sessions.end());
        }
        if (!g_seats.empty() && g_sent + g_received > 0)
        {
            if (!rate_at) rate_at = second_at = t, rate_out = second_out = g_wire_out, rate_in = second_in = g_wire_in,
                          rate_game_out = g_sent, rate_game_in = g_received, peak_out = peak_in = 0;
            if (t - second_at >= 1000)
            {
                // Time the way to each other machine in the game and back, once a second.
                {
                    int64_t stamp = now_us();
                    std::vector<sockaddr_in> to;
                    {
                        std::lock_guard<std::mutex> l(g_lock);
                        for (const auto &p : g_peers) to.push_back(p.addr);
                    }
                    for (const auto &a : to) raw_send(a, t_ping, &stamp, 8);
                }
                uint64_t o = g_wire_out, i = g_wire_in;
                peak_out = std::max(peak_out, (o - second_out) * 1000 / (uint64_t)(t - second_at));
                peak_in = std::max(peak_in, (i - second_in) * 1000 / (uint64_t)(t - second_at));
                second_out = o, second_in = i, second_at = t;
            }
            if (t - rate_at >= 30000)
            {
                double secs = (t - rate_at) / 1000.0;
                uint64_t o = g_wire_out, i = g_wire_in, go = g_sent, gi = g_received;
                logf("lan traffic, last %.0f s: up %.0f kbit/s (busiest second %.0f), %.0f game packets/s; down %.0f kbit/s (busiest %.0f), %.0f game packets/s; %zu machine(s)",
                     secs, (o - rate_out) * 8 / secs / 1000, peak_out * 8 / 1000.0, (go - rate_game_out) / secs,
                     (i - rate_in) * 8 / secs / 1000, peak_in * 8 / 1000.0, (gi - rate_game_in) / secs, g_seats.size());
                {
                    std::lock_guard<std::mutex> tl(g_timing_lock);
                    logf("lan timing, last %.0f s: there and back %.0f ms (best %.0f, worst %.0f); longest gap between game packets %.0f ms; "
                         "packets waited for the game %.1f ms on average, %.0f ms at most",
                         secs, g_rtt_n ? g_rtt_sum_us / 1000.0 / g_rtt_n : 0.0, g_rtt_min_us / 1000.0, g_rtt_max_us / 1000.0,
                         g_gap_max_us / 1000.0, g_wait_n ? g_wait_sum_us / 1000.0 / g_wait_n : 0.0, g_wait_max_us / 1000.0);
                    if (g_smooth && (smooth::g_n_held || smooth::g_n_late))
                    {
                        using namespace smooth;
                        int64_t n = g_n_held + g_n_late;
                        double gap_avg = g_send_gap_n ? g_send_gap_sum_us / 1000.0 / g_send_gap_n : 0.0;
                        double gap_sd = g_send_gap_n ? sqrt(std::max(0.0, (double)g_send_gap_sq / g_send_gap_n - (gap_avg * 10) * (gap_avg * 10))) / 10.0 : 0.0;
                        logf("lan smoothing, last %.0f s: delay %.0f ms on average (%.0f to %.0f); packets held %.1f ms on average, %.0f ms at most; "
                             "%lld of %lld came too late to hold; arrival wobble %.1f ms on average, %.0f ms at most; host sent every %.1f ms (spread %.1f ms, longest %.0f ms)",
                             secs, g_target_sum_us / 1000.0 / n, g_target_min_us / 1000.0, g_target_max_us / 1000.0,
                             g_n_held ? g_hold_sum_us / 1000.0 / g_n_held : 0.0, g_hold_max_us / 1000.0, (long long)g_n_late, (long long)n,
                             g_wobble_sum_us / 1000.0 / n, g_wobble_max_us / 1000.0, gap_avg, gap_sd, g_send_gap_max_us / 1000.0);
                    }
                    smooth::reset_stats();
                    g_rtt_min_us = g_rtt_max_us = g_rtt_sum_us = g_rtt_n = 0;
                    g_gap_max_us = g_wait_max_us = g_wait_sum_us = g_wait_n = 0;
                }
                rate_at = t, rate_out = o, rate_in = i, rate_game_out = go, rate_game_in = gi, peak_out = peak_in = 0;
            }
        }
        else rate_at = 0;
        if (role == k_client)
        {
            if (t - last_hello >= 1000)
            {
                memset(&hello, 0, sizeof hello);
                put(hello.name, sizeof hello.name, g_name);
                lobby_msg in;
                {
                    std::lock_guard<std::mutex> l(g_lock);
                    in = g_lobby_in;
                }
                hello.key = in.key;
                hello.status = (uint8_t)(g_check_lobby && in.game[0] ? g_check_lobby(in.game, in.map_file) : 0);
                raw_send(g_join_addr, t_hello, &hello, sizeof(hello));
                last_hello = t;
            }
            if (g_welcomed && t - g_host_heard > 10000) dropped_by_host("Lost contact with the host");
        }
        if (role == k_host && t - last_lobby >= 1000)
        {
            last_lobby = t;
            std::vector<peer> peers;
            lobby_msg m;
            {
                std::lock_guard<std::mutex> l(g_lock);
                for (size_t i = 0; i < g_peers.size();)
                    if (t - g_peers[i].heard > 12000)
                    {
                        logf("lan: lost contact with %s", g_peers[i].name.c_str());
                        g_peers.erase(g_peers.begin() + i);
                    }
                    else i++;
                peers = g_peers;
                g_lobby_out.count = (uint8_t)std::min<size_t>(16, peers.size() + 1);
                put(g_lobby_out.host, 32, g_name);
                put(g_lobby_out.names[0], 32, g_name);
                g_lobby_out.status[0] = 1;
                for (size_t i = 0; i < peers.size() && i < 15; i++)
                {
                    put(g_lobby_out.names[i + 1], 32, peers[i].name);
                    g_lobby_out.status[i + 1] = peers[i].key == g_lobby_out.key ? peers[i].status : 0;
                }
                m = g_lobby_out;
            }
            for (const auto &p : peers) raw_send(p.addr, t_lobby, &m, sizeof(m));
        }
        if (I08_ONLINE && t < g_searching_until && t - g_list_asked >= 3000)
        {
            g_list_asked = t;
            if (g_server.sin_port || resolve_server()) ask_for_list(0);
            prune_public_list(t);
        }
        if (t < g_searching_until && t - last_find >= 1000)
        {
            last_find = t;
            for (const auto &a : broadcast_addresses()) raw_send(a, t_find, nullptr, 0);
            std::lock_guard<std::mutex> l(g_lock);
            g_found.erase(std::remove_if(g_found.begin(), g_found.end(), [t](const found_game &f) { return t - f.seen > 4000; }),
                          g_found.end());
        }
    }
}

// The socket and threads, the first time LAN play is used.
static bool open()
{
    if (g_opened) return g_sock != INVALID_SOCKET;
    g_opened = true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { logf("lan: no network (WSAStartup failed)"); g_problem = "no network"; return false; }
    if (g_name.empty())
    {
        wchar_t computer[64];
        DWORD n = 64;
        g_name = GetComputerNameW(computer, &n) ? narrow(computer) : "Player";
    }
    // A machine address nobody else will have; the top byte marks it as ours. New each
    // session: nothing kept needs it (a co-op save would, but they aren't kept), and a
    // file holding it could be copied to a second machine along with the folder.
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    uint64_t r = (uint64_t)t.QuadPart * 0x9E3779B97F4A7C15ull ^ ((uint64_t)GetCurrentProcessId() << 32) ^ GetTickCount64();
    for (char c : g_name) r = (r ^ (uint8_t)c) * 0x100000001b3ull;
    g_my_id = (0x08ull << 56) | (r & 0x00FFFFFFFFFFFFFFull);
    g_cfg.address = g_my_id;
    // This run's keys (the secure connections use new ones each time it starts).
    if (!random_bytes(g_priv, 32) || !random_bytes(g_cookie_secret, 32))
    {
        logf("lan: no secure random numbers (BCryptGenRandom failed); network play is off");
        g_problem = "no secure random numbers";
        return false;
    }
    crypto_x25519_public_key(g_pub, g_priv);

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) { g_problem = "no network"; return false; }
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(g_port);
    if (bind(g_sock, (sockaddr *)&local, sizeof(local)))
    {
        // Something else has the port: this machine can still join games.
        g_port_taken = true;
        local.sin_port = 0;
        if (bind(g_sock, (sockaddr *)&local, sizeof(local)))
        {
            logf("lan: cannot open a UDP port (error %d)", WSAGetLastError());
            g_problem = "can't open a network port";
            closesocket(g_sock);
            g_sock = INVALID_SOCKET;
            return false;
        }
        logf("lan: UDP port %u is in use (is Installation 08 already running?); joining only", g_port);
    }
    BOOL yes = TRUE;
    setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof(yes));
    int big = 1 << 20;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof(big));
    setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, (const char *)&big, sizeof(big));
    {
        int got = 0, gs = 0;
        int n1 = sizeof(got), n2 = sizeof(gs);
        getsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (char *)&got, &n1);
        getsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, (char *)&gs, &n2);
        logf("lan: socket buffers %d bytes in, %d out", got, gs);
    }
    std::thread(receive_thread).detach();
    std::thread(tick_thread).detach();
    logf("lan: network ready as \"%s\", address %llx, UDP port %u", g_name.c_str(), (unsigned long long)g_my_id,
         g_port_taken ? 0 : g_port);
    return true;
}

// ---- Online: the online server and the join code.
// Going online registers the lobby with the server under a number made from a new join
// code (80 random bits); the server never sees the code itself, only that number, so it
// can't make the key and can't join. A joiner types the code, asks the server for the
// host, and the two try to connect directly (each sends to the other's address, which
// opens both routers); if that fails within a few seconds, they go through the server.
// The online server's address comes from the build (-DI08_SERVER="host[:port]"), so it
// isn't kept in the source.
#ifndef I08_SERVER
#define I08_SERVER ""
#endif
static const char *k_default_server = I08_SERVER;
static const uint16_t k_server_port = 27090;
std::string g_server_name;                    // online_server in installation08.ini (host or host:port)
bool g_force_relay;                           // online_force_relay = 1: skip the direct try (testing)
static std::mutex g_online_lock;
static std::string g_online_status;           // shown with the join code
static std::atomic<int> g_online_gen{0};
static std::atomic<bool> g_registered{false};
static uint8_t g_room[8];                     // host: the lobby's number on the server
static int64_t g_registered_at;
static void set_status(const std::string &t)
{
    std::lock_guard<std::mutex> l(g_online_lock);
    g_online_status = t;
}
static std::string online_status()
{
    std::lock_guard<std::mutex> l(g_online_lock);
    return g_online_status;
}
static void code_keys(const uint8_t code[10], uint8_t room[8], uint8_t psk[32])
{
    crypto_blake2b_keyed(room, 8, code, 10, (const uint8_t *)"I08 room 1", 10);
    crypto_blake2b_keyed(psk, 32, code, 10, (const uint8_t *)"I08 code 2", 10);
}
static bool resolve_server()
{
    std::string name = g_server_name.empty() ? k_default_server : g_server_name;
    if (name.empty()) return false;
    uint16_t port = k_server_port;
    size_t colon = name.find(':');
    if (colon != std::string::npos) port = (uint16_t)atoi(name.c_str() + colon + 1), name.resize(colon);
    addrinfo hints = {}, *found = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &found) || !found) { logf("online: can't find the server %s", name.c_str()); return false; }
    sockaddr_in a = *(sockaddr_in *)found->ai_addr;
    freeaddrinfo(found);
    a.sin_port = htons(port);
    g_server = a;
    return true;
}
static void server_send(uint8_t type, const void *body, size_t len, size_t pad_to = 0)
{
    if (!g_server.sin_port || g_sock == INVALID_SOCKET) return;
    char m[1200] = {'I', '0', '8', 'S', 1, (char)type};
    if (len > sizeof m - 6) return;
    memcpy(m + 6, body, len);
    size_t total = std::max(6 + len, pad_to); // requests are never smaller than the answers
    sendto(g_sock, m, (int)total, 0, (const sockaddr *)&g_server, sizeof g_server);
}
// This machine's address on its own network (for players behind the same router).
static sockaddr_in local_address()
{
    sockaddr_in me = {};
    SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int len = sizeof me;
    if (probe != INVALID_SOCKET)
    {
        if (connect(probe, (const sockaddr *)&g_server, sizeof g_server) == 0) getsockname(probe, (sockaddr *)&me, &len);
        closesocket(probe);
    }
    me.sin_port = htons(g_port);
    return me;
}
// A public lobby's entry in the server's list: its code (anyone may join a public lobby)
// and what it's playing.
#pragma pack(push, 1)
struct public_entry
{
    uint8_t code[10];
    char host[32], game[16], map[48], variant[48];
    uint8_t mode, count, playing;
};
#pragma pack(pop)
static std::atomic<bool> g_public{false}; // host: listed in the server's public list
static void send_register()
{
    sockaddr_in me = local_address();
    uint8_t b[15 + sizeof(public_entry)] = {0};
    memcpy(b, g_room, 8);
    memcpy(b + 8, &me.sin_addr.s_addr, 4);
    memcpy(b + 12, &me.sin_port, 2);
    size_t n = 14;
    if (g_public)
    {
        public_entry e = {};
        memcpy(e.code, g_online_code, 10);
        {
            std::lock_guard<std::mutex> l(g_lock);
            memcpy(e.host, g_lobby_out.host, sizeof e.host);
            memcpy(e.game, g_lobby_out.game, sizeof e.game);
            memcpy(e.map, g_lobby_out.map, sizeof e.map);
            memcpy(e.variant, g_lobby_out.variant, sizeof e.variant);
            e.mode = g_lobby_out.mode;
            e.count = (uint8_t)(g_peers.size() + 1);
            e.playing = g_lobby_out.playing;
        }
        e.host[31] = e.game[15] = e.map[47] = e.variant[47] = 0;
        b[14] = 1;
        memcpy(b + 15, &e, sizeof e);
        n = sizeof b;
    }
    server_send(1, b, n, 32);
}
static void online_thread(int gen)
{
    set_status("Connecting to the online server...");
    if (!resolve_server())
    {
        set_status(g_server_name.empty() && !*k_default_server ? "No online server is set (online_server in installation08.ini)"
                                                               : "Can't find the online server. Check the internet connection");
        return;
    }
    int64_t last = 0;
    bool warned = false;
    while (g_online && gen == g_online_gen)
    {
        int64_t now = ms();
        // Every 10 seconds (every 2 until the server answers): keeps the lobby listed and
        // the router's door to the server open.
        if (now - last >= (!g_registered ? 2000 : g_public ? 5000 : 10000)) send_register(), last = now;
        if (!g_registered && !warned && now - g_registered_at > 8000)
        {
            warned = true;
            set_status("Can't reach the online server. Check the internet connection");
            logf("online: no answer from the server");
        }
        Sleep(250);
    }
}
static void set_online(bool on, bool listed = false)
{
    if (on && g_online && listed != g_public)
    {
        // Public to private: a new code (the old one was in the public list).
        if (!listed) set_online(false);
        else
        {
            g_public = true;
            send_register();
            logf("lan: lobby listed publicly");
            return;
        }
    }
    if (on == g_online) return;
    if (on)
    {
        g_public = listed;
        random_bytes(g_online_code, sizeof g_online_code);
        code_keys(g_online_code, g_room, g_online_psk);
        g_registered = false;
        g_registered_at = ms();
        set_status("Connecting to the online server...");
        g_online = true;
        std::thread(online_thread, ++g_online_gen).detach();
        logf("lan: lobby open to the internet with a join code");
        return;
    }
    g_online = false;
    g_public = false;
    ++g_online_gen;
    if (g_registered) server_send(3, g_room, 8, 32), server_send(3, g_room, 8, 32);
    g_registered = false;
    crypto_wipe(g_online_code, sizeof g_online_code);
    crypto_wipe(g_online_psk, 32);
    logf("lan: lobby no longer open to the internet");
}
// The join code: 16 letters and digits (no 0, O, I or L), in groups of four.
static const char k_b32[] = "123456789ABCDEFGHJKMNPQRSTUVWXYZ";
static std::string join_code()
{
    if (!g_online || !g_registered) return "";
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < sizeof g_online_code; i++)
    {
        acc = (acc << 8) | g_online_code[i], bits += 8;
        while (bits >= 5) out += k_b32[(acc >> (bits - 5)) & 31], bits -= 5;
    }
    std::string grouped;
    for (size_t i = 0; i < out.size(); i++) grouped += (i && i % 4 == 0 ? "-" : "") + std::string(1, out[i]);
    return grouped;
}

// Client: joining with a code goes through these steps (moved on by online_tick).
struct online_join
{
    bool active = false;
    int stage = 0; // 0 asking the server, 1 trying directly, 2 through the server
    uint8_t room[8], nonce[8];
    int64_t began = 0, stage_began = 0, asked = 0;
    sockaddr_in relay = {};
};
static online_join g_ojoin;
static bool join(const sockaddr_in &addr, const std::string &text, const uint8_t *psk);
static bool join_with_code_bytes(uint8_t code[10], const std::string &label);
[[maybe_unused]] static bool join_with_code(const std::string &text)
{
    std::string c;
    for (char ch : text)
    {
        if (!ch) break;
        if (ch == '-' || isspace((unsigned char)ch)) continue;
        char u = (char)toupper((unsigned char)ch);
        if (u == 'I' || u == 'L') u = '1'; // typed by mistake for a 1
        if (!strchr(k_b32, u)) { c.clear(); break; }
        c += u;
    }
    if (c.size() != 16)
    {
        logf("lan: that isn't a join code (%zu letters and numbers)", c.size());
        std::lock_guard<std::mutex> l(g_lock);
        g_left_reason = "That isn't a join code (16 letters and numbers)";
        return false;
    }
    uint8_t code[10] = {0};
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (char ch : c)
    {
        acc = (acc << 5) | (uint32_t)(strchr(k_b32, ch) - k_b32), bits += 5;
        if (bits >= 8 && n < sizeof code) code[n++] = (uint8_t)(acc >> (bits - 8)), bits -= 8;
    }
    return join_with_code_bytes(code, "an online lobby");
}
static bool join_with_code_bytes(uint8_t code[10], const std::string &label)
{
    uint8_t psk[32];
    if (!open()) return false;
    if (!resolve_server())
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_left_reason = g_server_name.empty() && !*k_default_server ? "No online server is set (online_server in installation08.ini)"
                                                                    : "Can't find the online server. Check the internet connection";
        return false;
    }
    online_join oj;
    code_keys(code, oj.room, psk);
    sockaddr_in none = {};
    bool ok = join(none, label, psk);
    // join() starts a handshake; with no address yet it waits for the server's answer
    crypto_wipe(psk, sizeof psk);
    crypto_wipe(code, 10);
    if (!ok) return false;
    g_pending.active = false; // no address yet: the server gives it
    random_bytes(oj.nonce, 8);
    oj.active = true;
    oj.began = oj.stage_began = ms();
    g_ojoin = oj;
    logf("online: asking the server for the lobby");
    return true;
}
// Client: the public lobbies the server listed (while Browse games is open).
struct public_game { public_entry e; int64_t seen; };
static std::vector<public_game> g_public_found;
static void ask_for_list(int page)
{
    uint8_t q[1] = {(uint8_t)page};
    server_send(5, q, 1, 1100); // padded: the answer is never bigger than the question
}
static void prune_public_list(int64_t t)
{
    std::lock_guard<std::mutex> l(g_public_lock);
    g_public_found.erase(std::remove_if(g_public_found.begin(), g_public_found.end(),
                                        [t](const public_game &x) { return t - x.seen > 10000; }),
                         g_public_found.end());
}
static std::vector<public_game> public_found()
{
    std::lock_guard<std::mutex> l(g_public_lock);
    return g_public_found;
}
static bool join_public(const public_game &g)
{
    uint8_t code[10];
    memcpy(code, g.e.code, 10);
    return join_with_code_bytes(code, field(g.e.host, sizeof g.e.host));
}
// Messages from the server (receive thread).
static void server_message(const char *packet, int n)
{
    if (n < 6 || packet[4] != 1) return;
    uint8_t type = (uint8_t)packet[5];
    const char *b = packet + 6;
    int len = n - 6;
    auto addr = [](const char *p) {
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        memcpy(&a.sin_addr.s_addr, p, 4);
        memcpy(&a.sin_port, p + 4, 2);
        return a;
    };
    if (type == 11 && len >= 14 && g_online && !memcmp(b, g_room, 8)) // registered
    {
        if (!g_registered)
        {
            g_registered = true;
            set_status(g_public ? "Listed: anyone can find this lobby in Browse games"
                                : "Ready: friends anywhere can join with this code. Players on your network can also join from Browse games");
            logf("online: registered with the server (seen as %s)", ip_text(addr(b + 8)).c_str());
#ifdef LOG_JOIN_CODE // local testing only: the code is a secret, and logs get shared
            logf("online: join code %s", join_code().c_str());
#endif
        }
    }
    else if (type == 13 && len >= 22 && g_online && !memcmp(b, g_room, 8) && g_role == k_host) // someone is joining
    {
        uint64_t tag;
        memcpy(&tag, b + 8, 8);
        sockaddr_in joiner = addr(b + 16);
        relay_address(tag); // ready in case they come through the server
        // Send toward them: it opens this router for their packets.
        for (int i = 0; i < 3; i++) send_plain(joiner, t_punch, nullptr, 0);
        logf("online: a player is joining from %s", ip_text(joiner).c_str());
    }
    else if (type == 12 && len >= 34 && g_ojoin.active && g_ojoin.stage == 0 && !memcmp(b, g_ojoin.nonce, 8)) // the host
    {
        uint64_t tag;
        memcpy(&tag, b + 8, 8);
        sockaddr_in pub = addr(b + 16), local = addr(b + 22), me = addr(b + 28);
        // Behind the same router: the host's address on this network.
        sockaddr_in direct = pub.sin_addr.s_addr == me.sin_addr.s_addr && local.sin_addr.s_addr ? local : pub;
        g_ojoin.relay = relay_address(tag);
        if (g_force_relay) direct = g_ojoin.relay, g_ojoin.stage = 2;
        g_join_addr = direct;
        g_pending.addr = direct;
        memset(g_pending.cookie, 0, 16);
        g_pending.sent = 0;
        g_pending.active = true;
        if (!g_force_relay) g_ojoin.stage = 1;
        g_ojoin.stage_began = ms();
        logf("online: the server found the lobby; connecting %s %s", g_force_relay ? "through the server, as" : "directly to",
             ip_text(direct).c_str());
        kick_handshake();
    }
    else if (type == 15 && len >= 3) // a page of the public list
    {
        int page = (uint8_t)b[0], pages = (uint8_t)b[1], count = (uint8_t)b[2];
        if (len < 3 + count * (int)sizeof(public_entry)) return;
        int64_t now = ms();
        std::lock_guard<std::mutex> l(g_public_lock);
        for (int i = 0; i < count; i++)
        {
            public_entry e;
            memcpy(&e, b + 3 + i * sizeof(public_entry), sizeof e);
            e.host[31] = e.game[15] = e.map[47] = e.variant[47] = 0;
            public_game *have = nullptr;
            for (auto &x : g_public_found)
                if (!memcmp(x.e.code, e.code, 10)) have = &x;
            if (have) have->e = e, have->seen = now;
            else if (g_public_found.size() < 200) g_public_found.push_back({e, now});
        }
        if (page + 1 < pages && page < 20) ask_for_list(page + 1);
    }
    else if (type == 14 && len >= 8 && g_ojoin.active && g_ojoin.stage == 0 && !memcmp(b, g_ojoin.nonce, 8))
    {
        g_ojoin.active = false;
        dropped_by_host("No online lobby has that code. Check it, and that the host's lobby is still Online");
    }
}
static bool g_ojoin_active() { return g_ojoin.active; }
// Client, four times a second: move the join along.
static void online_tick(int64_t t)
{
    if (!g_ojoin.active || g_role != k_client) return;
    if (g_ojoin.stage == 0)
    {
        if (t - g_ojoin.asked >= 1000)
        {
            uint8_t q[16];
            memcpy(q, g_ojoin.room, 8);
            memcpy(q + 8, g_ojoin.nonce, 8);
            server_send(2, q, sizeof q, 64);
            g_ojoin.asked = t;
        }
        if (t - g_ojoin.began > 10000)
        {
            g_ojoin.active = false;
            dropped_by_host("Can't reach the online server. Check the internet connection");
        }
        return;
    }
    bool connected;
    {
        std::lock_guard<std::mutex> l(g_sess_lock);
        connected = find_session(g_join_addr) != nullptr;
    }
    if (connected)
    {
        logf("online: connected %s", g_ojoin.stage == 1 ? "directly" : "through the server");
        g_ojoin.active = false;
        return;
    }
    kick_handshake();
    if (g_ojoin.stage == 1 && t - g_ojoin.stage_began > 3000)
    {
        g_ojoin.stage = 2;
        g_ojoin.stage_began = t;
        g_join_addr = g_ojoin.relay;
        g_pending.addr = g_ojoin.relay;
        memset(g_pending.cookie, 0, 16);
        g_pending.sent = 0;
        logf("online: no direct connection; going through the server");
        kick_handshake();
    }
    else if (g_ojoin.stage == 2 && t - g_ojoin.stage_began > 10000)
    {
        g_ojoin.active = false;
        dropped_by_host("Couldn't connect to the host, directly or through the server");
    }
}

// ---- Called from the menus (main thread).
static bool host_open()
{
    if (!open()) return false;
    if (g_port_taken)
    {
        g_problem = "port " + std::to_string(g_port) + " is in use, so others can't reach a game hosted here";
        return false;
    }
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_peers.clear();
        g_lobby_out = {};
    }
    g_problem.clear();
    clear_sessions();
    g_role = k_host;
    logf("lan: hosting a game");
    return true;
}

static void host_close()
{
    if (g_role != k_host) return;
    std::vector<peer> peers;
    {
        std::lock_guard<std::mutex> l(g_lock);
        peers.swap(g_peers);
    }
    for (int round = 0; round < 3; round++)
        for (const auto &p : peers) raw_send(p.addr, t_bye, nullptr, 0);
    g_role = k_off;
    clear_sessions();
    set_online(false);
    logf("lan: stopped hosting");
}

// The host's game, as the menu shows it.
static void set_lobby(const std::string &game, const std::string &map, const std::string &variant, bool playing, int mode = 0,
                      int difficulty = 0, const std::string &map_file = "", const std::string &detail = "")
{
    std::lock_guard<std::mutex> l(g_lock);
    put(g_lobby_out.game, sizeof g_lobby_out.game, game);
    put(g_lobby_out.map, sizeof g_lobby_out.map, map);
    put(g_lobby_out.variant, sizeof g_lobby_out.variant, variant);
    put(g_lobby_out.map_file, sizeof g_lobby_out.map_file, map_file);
    put(g_lobby_out.detail, sizeof g_lobby_out.detail, detail);
    g_lobby_out.playing = playing;
    g_lobby_out.mode = (uint8_t)mode;
    g_lobby_out.difficulty = (uint8_t)difficulty;
    uint32_t key = 2166136261u;
    for (char c : game + "/" + map_file) key = (key ^ (uint8_t)c) * 16777619u;
    g_lobby_out.key = key | 1;
}

// Host: the players who joined, with whether each can play the lobby's choice.
struct lobby_player { uint64_t id; std::string name; int status; };
static std::vector<lobby_player> lobby_players()
{
    std::lock_guard<std::mutex> l(g_lock);
    std::vector<lobby_player> out;
    for (const auto &p : g_peers) out.push_back({p.id, p.name, p.key == g_lobby_out.key ? p.status : 0});
    return out;
}

static void set_playing(bool playing)
{
    if (playing)
    {
        g_wire_out = g_wire_in = g_game_bytes_out = g_game_bytes_in = 0;
        g_max_game_out = g_max_game_in = g_max_datagram_out = 0;
        g_unrouted = 0, g_sim_lost = 0;
        g_stats_since = ms();
        if (g_sim > 0)
            logf("lan: network test on: %ls (%d ms each way, +/- %d ms, %.1f%% loss), both directions on this machine",
                 k_sim[g_sim].name, k_sim[g_sim].delay_ms, k_sim[g_sim].jitter_ms, k_sim[g_sim].loss * 100);
    }
    std::lock_guard<std::mutex> l(g_lock);
    g_lobby_out.playing = playing;
}

static bool join(const sockaddr_in &addr, const std::string &text, const uint8_t *psk = nullptr)
{
    if (!open()) return false;
    if (g_role == k_host) host_close();
    clear_sessions();
    if (psk) memcpy(g_join_psk, psk, 32);
    else memset(g_join_psk, 0, 32);
    g_pending.addr = addr;
    memset(g_pending.cookie, 0, 16);
    random_bytes(g_pending.nonce, 16);
    g_pending.sent = 0;
    g_pending.began = ms();
    g_pending.said = false;
    g_pending.active = true;
    g_ojoin.active = false;
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_peers.clear();
        g_lobby_in = {};
        g_left_reason.clear();
    }
    g_join_addr = addr;
    g_join_text = text;
    g_welcomed = false;
    g_start_ready = false;
    g_end_ready = false;
    g_role = k_client;
    logf("lan: joining %s (%s)%s", text.c_str(), ip_text(addr).c_str(), psk ? " with a join code" : "");
    kick_handshake();
    return true;
}

// An address someone typed: "192.168.1.20", a machine's name, or either with ":port".
static bool join_text(const std::string &text)
{
    std::string host = text;
    uint16_t port = g_port;
    size_t colon = host.find(':');
    if (colon != std::string::npos) port = (uint16_t)atoi(host.c_str() + colon + 1), host.resize(colon);
    if (!open()) return false;
    addrinfo hints = {}, *found = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (host.empty() || getaddrinfo(host.c_str(), nullptr, &hints, &found) || !found)
    {
        logf("lan: cannot find %s", text.c_str());
        std::lock_guard<std::mutex> l(g_lock);
        g_left_reason = "Can't find " + text;
        return false;
    }
    sockaddr_in a = *(sockaddr_in *)found->ai_addr;
    a.sin_port = htons(port ? port : g_port);
    freeaddrinfo(found);
    return join(a, text);
}

static void leave()
{
    if (g_role != k_client) return;
    sockaddr_in to = g_join_addr;
    g_role = k_off;
    g_welcomed = false;
    for (int round = 0; round < 3; round++) raw_send(to, t_bye, nullptr, 0);
    g_pending.active = false;
    g_ojoin.active = false;
    clear_sessions();
    std::lock_guard<std::mutex> l(g_lock);
    g_peers.clear();
    logf("lan: left the game");
}

static void search()
{
    if (!open()) return;
    g_searching_until = ms() + 3000;
}

static std::vector<found_game> found()
{
    std::lock_guard<std::mutex> l(g_lock);
    std::vector<found_game> out = g_found;
    std::sort(out.begin(), out.end(), [](const found_game &a, const found_game &b) { return strcmp(a.info.host, b.info.host) < 0; });
    return out;
}

static std::vector<std::string> joined_names()
{
    std::lock_guard<std::mutex> l(g_lock);
    std::vector<std::string> out;
    for (const auto &p : g_peers) out.push_back(p.name);
    return out;
}

static lobby_msg host_lobby()
{
    std::lock_guard<std::mutex> l(g_lock);
    return g_lobby_in;
}

static std::string left_reason()
{
    std::lock_guard<std::mutex> l(g_lock);
    return g_left_reason;
}

// From the command line: --lan host | --lan online | --lan <address> | --lan <join code>.
static void start(const std::wstring &what)
{
    std::string t = narrow(what.c_str());
    if (!_wcsicmp(what.c_str(), L"host")) host_open();
#if I08_ONLINE
    else if (!_wcsicmp(what.c_str(), L"online") || !_wcsicmp(what.c_str(), L"private")) { if (host_open()) set_online(true); }
    else if (!_wcsicmp(what.c_str(), L"public")) { if (host_open()) set_online(true, true); }
    else if (t.find('.') == std::string::npos && t.find('-') != std::string::npos) join_with_code(t);
#else
    else if (!_wcsicmp(what.c_str(), L"online") || !_wcsicmp(what.c_str(), L"private") || !_wcsicmp(what.c_str(), L"public") ||
             (t.find('.') == std::string::npos && t.find('-') != std::string::npos))
        g_problem = "online play isn't in this build: LAN only";
#endif
    else if (!join_text(t)) g_problem = "can't find the host " + t;
}

// Leaving Installation 08: tell the others.
static void shutdown()
{
    if (g_role == k_host) host_close();
    else leave();
}

static void log_totals()
{
    if (g_sent || g_received)
    {
        logf("lan: %llu packets sent, %llu received, %llu dropped here, %llu lost on the way, %llu out of order",
             (unsigned long long)g_sent.load(), (unsigned long long)g_received.load(), (unsigned long long)g_dropped.load(),
             (unsigned long long)g_lost.load(), (unsigned long long)g_late.load());
        double secs = std::max(1.0, (ms() - g_stats_since) / 1000.0);
        logf("lan: traffic over %.0f s: up %.1f MB (%.0f kbit/s average), down %.1f MB (%.0f kbit/s); game packets up to %u bytes sent, %u received; largest datagram sent %u bytes",
             secs, g_wire_out / 1e6, g_wire_out * 8 / secs / 1000, g_wire_in / 1e6, g_wire_in * 8 / secs / 1000,
             g_max_game_out.load(), g_max_game_in.load(), g_max_datagram_out.load());
        if (g_unrouted) logf("lan: %llu game packet(s) were for another joined machine and were dropped", (unsigned long long)g_unrouted.load());
        if (g_sim > 0) logf("lan: network test (%ls) lost %llu datagram(s) on purpose", k_sim[g_sim].name, (unsigned long long)g_sim_lost.load());
    }
    g_wire_out = g_wire_in = g_game_bytes_out = g_game_bytes_in = 0;
    g_max_game_out = g_max_game_in = g_max_datagram_out = 0;
    g_unrouted = 0;
    g_sim_lost = 0;
    {
        std::lock_guard<std::mutex> tl(g_timing_lock);
        g_last_game_in_us = g_gap_max_us = g_wait_max_us = g_wait_sum_us = g_wait_n = 0;
        g_rtt_min_us = g_rtt_max_us = g_rtt_sum_us = g_rtt_n = 0;
    }
    g_stats_since = ms();
    g_sent = g_received = g_dropped = g_lost = g_late = 0;
    memset(g_seq_peer, 0, sizeof g_seq_peer);
    g_logged = 0;
}
} // namespace lan

static uint32_t net_send(uint64_t id, const char *buf, uint32_t len, uint32_t port)
{
    if (!buf || !len) return 0;
    // Outside a LAN game only Halo CE needs its own packets back (its server and client
    // talk through here); the other games are left as they were.
    if (g_module != 0 && lan::g_seats.empty()) return len;
    uint64_t me = g_cfg.address;
    uint64_t to = id ? id : (lan::g_seats.empty() ? me : lan::g_seats[0].address);
    if (to == me) net_deliver(buf, len, me, port);
    else if (lan::g_role != lan::k_off) lan::send_game(to, port, buf, len);
    return len; // nobody else is out there
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
        if (it->ready_us && it->ready_us > now_us()) return 0; // held for smoothing; keeps the order
        uint32_t n = (uint32_t)std::min<size_t>(it->data.size(), len);
        memcpy(buf, it->data.data(), n);
        if (id) *id = it->from;
        if (it->from != g_cfg.address)
        {
            int64_t w = now_us() - std::max(it->at_us, it->ready_us);
            std::lock_guard<std::mutex> t(g_timing_lock);
            g_wait_max_us = std::max(g_wait_max_us, w), g_wait_sum_us += w, g_wait_n++;
        }
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
// LAN co-op checkpoints, kept apart from the solo saved game (every machine in the game
// keeps one, so any of them can host the next session).
static std::wstring coop_save_path(const std::wstring &game) { return g_cfg.host_dir + L"\\saves\\" + game + L".coop.sav"; }
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
// Continuing a LAN co-op game from a checkpoint. Off: Halo 3 won't (started from a saved
// game on every machine it never goes online; on the host only, it ignores the save),
// and the others are untested. The saving, sending and loading are kept for a game that can.
static const bool k_coop_continue = false;
static std::atomic<int> g_game_run; // counts games started, for things done once a game
// The match result. Every game builds the same record at the end of a game (0x5D138
// bytes, MCC's own layout) and hands it to the host: MCC makes its carnage report from
// it. Kept here for the post-game screen; each one is also saved to results\ next to the
// exe while the layout is being worked out.
static const size_t k_result_size = 0x5D138;
static std::mutex g_result_lock;
static std::vector<uint8_t> g_result;      // the last game's record (empty: none yet)
static std::atomic<int64_t> g_result_at;  // when it came
static void h_set_game_result(void *, const uint8_t *result)
{
    if (!result) { logf("host.set_game_result: no record"); return; }
    {
        std::lock_guard<std::mutex> l(g_result_lock);
        g_result.assign(result, result + k_result_size);
    }
    g_result_at = (int64_t)GetTickCount64();
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[96];
    swprintf(name, 96, L"%ls-%04d%02d%02d-%02d%02d%02d.bin", g_cfg.game.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond);
    std::wstring dir = g_cfg.host_dir + L"\\results";
    CreateDirectoryW(dir.c_str(), nullptr);
    if (FILE *f = _wfopen((dir + L"\\" + name).c_str(), L"wb"))
    {
        fwrite(result, 1, k_result_size, f);
        fclose(f);
    }
    // What's known so far (from Halo 2's builder): 16 players at 0x20 (0x1C8 each: present,
    // xuid at +8) and their stats at 0x1CE4 (0x5448 each: present, then values).
    logf("host.set_game_result: saved results\\%ls; game %d, value %llu", name, *(const int32_t *)(result + 8),
         (unsigned long long)*(const uint64_t *)(result + 0x10));
    for (int i = 0; i < 16; i++)
    {
        const uint8_t *id = result + 0x20 + i * 0x1C8, *st2 = result + 0x1CE4 + i * 0x5448;
        if (!id[0] && !st2[0]) continue;
        logf("  player %d: present %d, xuid %016llx, stats present %d, +0x28 %d, +0x20 %d, +0x7c %d, +0x64 %d", i, id[0],
             (unsigned long long)*(const uint64_t *)(id + 8), *(const int32_t *)st2, *(const int32_t *)(st2 + 0x28),
             *(const int32_t *)(st2 + 0x20), *(const int32_t *)(st2 + 0x7c), *(const int32_t *)(st2 + 0x64));
    }
}

static void h_save_game(void *, const char *buf, uint32_t len)
{
    if (g_mode_for_saves != 1) { logf("host.save_game: not a campaign, not kept (%u bytes)", len); return; }
    bool coop = !lan::g_seats.empty();
    if (coop && !k_coop_continue) { logf("host.save_game %u bytes (LAN co-op): not kept", len); return; }
    logf("host.save_game %u bytes%s", len, coop ? " (LAN co-op)" : "");
    if (!buf || !len) return;
    if (!coop && g_discard_session) { logf("not kept (quitting without saving)"); return; }
    // Halo CE sometimes hands over a 1-byte "save" at a mission's start; keeping it
    // would replace a real saved game with nothing to continue from.
    if (len < 4096) { logf("not kept (too small to be a saved game)"); return; }
    CreateDirectoryW((g_cfg.host_dir + L"\\saves").c_str(), nullptr);
    std::wstring path = coop ? coop_save_path(g_cfg.game) : save_path(g_cfg.game), temp = path + L".new";
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
    logf("%s written (%s)", coop ? "co-op saved game" : "saved game", current_file_for_save().c_str());
    // Once a game: where the machines' addresses are in a co-op save (to check that
    // continuing gives the game back the machines it expects).
    static int s_scanned_for = -1;
    if (coop && s_scanned_for != g_game_run)
    {
        s_scanned_for = g_game_run;
        std::vector<std::pair<std::string, uint64_t>> look;
        for (size_t i = 0; i < lan::g_seats.size(); i++)
            look.push_back({"machine " + std::to_string(i), lan::g_seats[i].address});
        if (!lan::g_seats.empty()) look.push_back({"session", lan::g_seats[0].address ^ 0x4930384C414E0001ull});
        std::string out;
        for (const auto &l : look)
        {
            int count = 0;
            std::string at;
            for (uint32_t o = 0; o + 8 <= len; o++)
            {
                if ((uint8_t)buf[o] != (uint8_t)l.second) continue;
                uint64_t v;
                memcpy(&v, buf + o, 8);
                if (v != l.second) continue;
                if (count++ < 4)
                {
                    char t[24];
                    snprintf(t, sizeof t, "%s%x", at.empty() ? "" : " ", o);
                    at += t;
                }
            }
            out += "; " + l.first + (l.second == lan::g_my_id ? " (this one)" : "") + ": " + std::to_string(count) + (count ? " at " + at : "");
        }
        logf("co-op save: addresses in it%s", out.c_str());
    }
    if (!coop) on_game_saved();
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

// The graphics choices in MCC's video settings, as the games read them (bytes 8 to
// 0x16 of s_general_video_settings; quality 0 low, 1 medium, 2 high; frame rate limit
// 0: 60, 1: 120, 2: 180, 3: 240, 4: none). Filled from the settings (apply_settings).
static unsigned char g_video_choices[24];
static std::atomic<bool> g_subtitles{false};
static bool h_get_video_setting(void *, unsigned char *settings)
{
    LOG_CALL(32, "host.get_video_setting");
    memset(settings, 0, 368);
    memcpy(settings, g_video_choices, sizeof g_video_choices);
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
static std::atomic<int> g_kbm_player{0}; // split-screen: which player has the keyboard and mouse
// XInput's slots (0-3) aren't always filled from 0 (a PC can put the only controller in
// slot 2), so players get the connected controllers in slot order. With fewer controllers
// than players, the keyboard is player 1's and the controllers go to players 2 and on.
static std::atomic<int> g_pad_slots[4];
static std::atomic<int> g_pad_count;
static std::atomic<int64_t> g_pads_checked;
static void refresh_pads();
// Players take the connected controllers in order.
static int pad_for_player(int player)
{
    if (player < 0 || player >= g_local_players) return -1;
    refresh_pads();
    return player < g_pad_count ? g_pad_slots[player].load() : -1;
}
// Which slots have a controller: checked twice a second (XInput on an empty slot is slow).
static void refresh_pads()
{
    int64_t now = (int64_t)GetTickCount64();
    if (now - g_pads_checked < 500 && g_pads_checked) return;
    g_pads_checked = now;
    int n = 0;
    for (int slot = 0; slot < 4; slot++)
    {
        XINPUT_STATE xs = {};
        if (g_xinput_get_state && g_xinput_get_state(slot, &xs) == ERROR_SUCCESS) g_pad_slots[n++] = slot;
    }
    if (n != g_pad_count) logf("input: %d controller(s) connected", n);
    g_pad_count = n;
}
namespace menu { static bool filter_input(struct s_host_input_state *state); }

// ---- Floating-point consistency (LAN co-op runs the same simulation on every machine,
// so every machine must compute exactly alike).
static unsigned fp_mxcsr()
{
    unsigned v = 0;
    __asm__ volatile("stmxcsr %0" : "=m"(v));
    return v;
}
static unsigned fp_x87()
{
    unsigned short v = 0;
    __asm__ volatile("fnstcw %0" : "=m"(v));
    return v;
}
static void log_fp_state(const char *where)
{
    static std::mutex m;
    static std::map<std::string, std::pair<unsigned, unsigned>> seen;
    unsigned a = fp_mxcsr(), b = fp_x87();
    std::lock_guard<std::mutex> l(m);
    auto it = seen.find(where);
    if (it != seen.end() && it->second == std::make_pair(a, b)) return;
    seen[where] = {a, b};
    logf("fp: %s: MXCSR %04x, x87 control %04x", where, a, b);
}
// What the CPU offers and what the system lets programs use (the C runtime's math
// picks its code path from these: FMA3 when it can).
static bool g_wine_math; // the games' C runtime math is Wine's (PS5 without Microsoft's ucrtbase.dll)
static void log_cpu()
{
    unsigned a, b, c, d;
    char brand[49] = {};
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000), "c"(0));
    if (a >= 0x80000004)
        for (unsigned i = 0; i < 3; i++)
        {
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000002 + i), "c"(0));
            memcpy(brand + i * 16, &a, 4), memcpy(brand + i * 16 + 4, &b, 4), memcpy(brand + i * 16 + 8, &c, 4),
                memcpy(brand + i * 16 + 12, &d, 4);
        }
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    bool fma = c >> 12 & 1, avx = c >> 28 & 1, osxsave = c >> 27 & 1;
    unsigned long long xcr0 = 0;
    if (osxsave)
    {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        xcr0 = ((unsigned long long)hi << 32) | lo;
    }
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    bool avx2 = b >> 5 & 1, avx512 = b >> 16 & 1;
    logf("cpu: %s; FMA3 %d, AVX %d, AVX2 %d, AVX-512 %d, OSXSAVE %d, XCR0 %llx", brand, fma, avx, avx2, avx512, osxsave, xcr0);
    // Which C runtime the games' math comes from: Wine's own copy says so in its header.
    HMODULE ucrt = LoadLibraryW(L"ucrtbase.dll");
    bool wine_copy = false;
    if (ucrt)
    {
        const char *base = (const char *)ucrt;
        wine_copy = memcmp(base + 0x40, "Wine builtin DLL", 16) == 0 || memcmp(base + 0x40, "Wine placeholder DLL", 20) == 0;
        g_wine_math = wine_copy;
        // Every machine uses the same (non-FMA) math code path, whatever its CPU offers.
        typedef int(__cdecl * t_set_fma3)(int);
        t_set_fma3 set_fma3 = (t_set_fma3)GetProcAddress(ucrt, "_set_FMA3_enable");
        typedef int(__cdecl * t_get_fma3)();
        t_get_fma3 get_fma3 = (t_get_fma3)GetProcAddress(ucrt, "_get_FMA3_enable");
        int before = get_fma3 ? get_fma3() : -1;
        int set = set_fma3 ? set_fma3(0) : -1;
        int after = get_fma3 ? get_fma3() : -1;
        logf("cpu: C runtime ucrtbase.dll is %s; FMA3 math %d, set(0) -> %d, now %d", wine_copy ? "Wine's" : "Microsoft's", before,
             set, after);
    }
    log_fp_state("main thread");
}

static bool h_get_input_state(void *, int player, unsigned char *raw)
{
    static bool fp_logged;
    if (!fp_logged) fp_logged = true, log_fp_state("game thread");
    LOG_CALL(36, "host.get_input_state player=%d", player);
    if (!raw) return false;
    auto *state = reinterpret_cast<s_host_input_state *>(raw);
    memset(raw + 4, 0, 0x130 - 4);
    if (!g_input_connected || player < 0 || player >= g_local_players) return false;
    // Controllers only (Installation 08 is made for the PS5): each player has the
    // connected controller in their position.
    int pad = pad_for_player(player);
    XINPUT_STATE xs = {};
    if (pad >= 0 && g_xinput_get_state && g_xinput_get_state(pad, &xs) == ERROR_SUCCESS)
    {
        const XINPUT_GAMEPAD p = remap_pad(xs.Gamepad); // this game's button layout
        state->gamepad.buttons = p.wButtons;
        state->gamepad.left_trigger = p.bLeftTrigger;
        state->gamepad.right_trigger = p.bRightTrigger;
        state->gamepad.thumb_lx = p.sThumbLX;
        state->gamepad.thumb_ly = p.sThumbLY;
        state->gamepad.thumb_rx = p.sThumbRX;
        state->gamepad.thumb_ry = p.sThumbRY;
    }
    state->is_km = 0;
    g_mouse_dx = 0, g_mouse_dy = 0, g_mouse_wheel = 0;
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
    // 5: subtitles.
    if (setting == 5) { if (v1) *v1 = g_subtitles; return true; }
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
    if (xuid) *xuid = player == 0 ? lan::g_my_xuid : 0x0009000000000001ull + (uint64_t)player;
    // "size" is in bytes for Halo CE (a 32-byte buffer) and may be in characters for
    // the others, so take it as bytes: the name fits either way. (wcsncpy padded the
    // rest with zeros, 64 bytes into Halo CE's 32, and broke its stack check.)
    if (name && size >= 4)
    {
        size_t room = (size_t)size / sizeof(wchar_t);
        wchar_t label[16];
        swprintf(label, 16, player == 0 ? L"Player" : L"Player %d", player + 1);
        if (player == 0 && !lan::g_name.empty())
        {
            int n = MultiByteToWideChar(CP_UTF8, 0, lan::g_name.c_str(), -1, label, 16);
            if (n <= 0) wcscpy(label, L"Player");
            label[15] = 0;
        }
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
    int mouse_speed = 30; // 1-100, on a logarithmic scale (see apply_settings)
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
    int keyboard_player = 0; // (before 1.3: the keyboard is player 1 on its own)
    int kbm_player = 0;      // split-screen: which player has the keyboard and mouse (0 = player 1)
    int split_session = 0;   // split-screen: list every player in the launch options
    int coop_continue_all = 0; // LAN co-op continue: every machine loads the saved game (0: the host only)
    int difficulty = 1;
    std::string lan_address; // the last address joined by hand
    std::string lan_name;    // the player's name: in the games and in LAN lobbies (default: the computer's name)
    int name_asked = 0;      // the first-start "your name" question has been shown
    std::map<std::string, std::string> look;      // "<game>_<slot>": armor or color, by MCC's item id
    std::string service_tag;                      // up to 4 letters and digits
    // Halo 4: primary, secondary, grenade, armor ability, tactical, support (the game's numbers).
    // Until changed: Assault Rifle, Magnum, Frag, Hologram, Mobility, Ammo.
    int loadout[5][6] = {{1, 5, 0, 7, 1, 11}, {1, 5, 0, 7, 1, 11}, {1, 5, 0, 7, 1, 11}, {1, 5, 0, 7, 1, 11}, {1, 5, 0, 7, 1, 11}};
    bool loadout_set[5] = {};
    std::map<std::string, std::string> map_variant; // "<game>_<map file>": the saved map last chosen (its file name)
    std::map<std::string, float> opt;               // the MCC settings below (k_options), by key
};
static host_settings g_settings;

// MCC's own settings (controls, video, audio, accessibility), as they reach the games:
// fields of the player profile (s_player_profile) and of the video settings. Defaults
// are MCC's (MCC-Win64-Shipping.exe+0x1c9868 builds its default profile).
enum opt_kind
{
    o_bool,     // profile byte: 0 or 1
    o_not,      // profile byte holding the opposite ("disabled" flags)
    o_byte,     // profile byte: the number
    o_int,      // profile int32: the number
    o_float,    // profile float: the number / scale
    o_video,    // video settings byte: the number
    o_subtitle, // subtitles: profile byte 0 and the game setting
};
enum opt_page { op_controls, op_video, op_audio, op_access };
struct opt_def
{
    const char *key;
    opt_page page;
    const wchar_t *label;
    opt_kind kind;
    int offset;
    float lo, hi, step, def;
    std::vector<const wchar_t *> names; // what each number is called (empty: the number)
    float scale;                        // o_float: profile value = shown value / scale
    const wchar_t *unit;                // after the number ("%")
    const wchar_t *note;
};
static const std::vector<const wchar_t *> k_off_on = {L"Off", L"On"};
static const std::vector<const wchar_t *> k_quality = {L"Low", L"Medium", L"High"};
static const std::vector<opt_def> k_options = {
    // Controls
    {"look_horizontal", op_controls, L"Look sensitivity (horizontal)", o_byte, 0x1B6, 1, 10, 1, 4, {}, 1, L"", L"Controller"},
    {"look_vertical", op_controls, L"Look sensitivity (vertical)", o_byte, 0x1B5, 1, 10, 1, 4, {}, 1, L"", L"Controller"},
    {"look_acceleration", op_controls, L"Look acceleration", o_byte, 0x1B7, 0, 10, 1, 4, {}, 1, L"", L"Controller"},
    {"axial_dead_zone", op_controls, L"Axial dead zone", o_float, 0x1B8, 0, 50, 1, 12, {}, 100, L"%", L"Controller: how far a stick moves before it counts, along each axis"},
    {"radial_dead_zone", op_controls, L"Radial dead zone", o_float, 0x1BC, 0, 50, 1, 12, {}, 100, L"%", L"Controller: how far a stick moves before it counts, in any direction"},
    {"zoom_sensitivity", op_controls, L"Zoomed look sensitivity", o_float, 0x1C0, 10, 200, 10, 100, {}, 100, L"%", L"Look speed while zoomed in, against normal"},
    {"vehicle_sensitivity", op_controls, L"Vehicle look sensitivity", o_float, 0x1C4, 10, 200, 10, 100, {}, 100, L"%", L"Look speed in vehicles, against normal"},
    {"invert_flight", op_controls, L"Invert flight controls", o_bool, 0x21, 0, 1, 1, 0, k_off_on, 1, L"", L""},
    {"vibration", op_controls, L"Vibration", o_not, 0x1F, 0, 1, 1, 1, k_off_on, 1, L"", L""},
    {"auto_center", op_controls, L"Look auto-centering", o_bool, 0x23, 0, 1, 1, 0, k_off_on, 1, L"", L"Levels your view when you move without looking up or down"},
    {"hold_to_zoom", op_controls, L"Zoom", o_bool, 0x28, 0, 1, 1, 1, {L"Toggle", L"Hold"}, 1, L"", L""},
    {"crouch_lock", op_controls, L"Crouch", o_bool, 0x24, 0, 1, 1, 0, {L"Hold", L"Toggle"}, 1, L"", L""},
    {"clench_protection", op_controls, L"Clench protection", o_bool, 0x26, 0, 1, 1, 0, k_off_on, 1, L"", L"Stops a hard squeeze of the grip from pressing other buttons"},
    {"double_jump_jetpack", op_controls, L"Double-press jump for jetpack", o_bool, 0x1D9, 0, 1, 1, 0, k_off_on, 1, L"", L"Halo: Reach and Halo 4"},
    {"dual_wield_swap", op_controls, L"Swap dual-wield triggers", o_bool, 0x1DB, 0, 1, 1, 0, k_off_on, 1, L"", L"Halo 2 and Halo 3"},
    // Video
    {"vehicle_fov", op_video, L"Vehicle field of view", o_int, 0x18, 65, 120, 1, 78, {}, 1, L"", L"Field of view in vehicles"},
    {"centered_crosshair", op_video, L"Crosshair", o_bool, 0x1C, 0, 1, 1, 0, {L"Default", L"Centered"}, 1, L"", L"Where the crosshair sits on the screen"},
    {"lock_aspect", op_video, L"Lock maximum aspect ratio", o_bool, 0x39, 0, 1, 1, 1, k_off_on, 1, L"", L"Keeps the picture at 16:9 on wider screens"},
    {"hud_scale", op_video, L"HUD scale", o_float, 0xAC8, 50, 100, 5, 100, {}, 1, L"%", L""},
    {"frame_rate", op_video, L"Frame rate limit", o_video, 0x15, 0, 4, 1, 0, {L"60", L"120", L"180", L"240", L"None"}, 1, L"",
     L"Higher limits need a fast enough machine (60 suits the PS5)"},
    {"texture_quality", op_video, L"Texture quality", o_video, 0x08, 0, 2, 1, 2, k_quality, 1, L"", L"Takes effect from the next game you start"},
    {"texture_filtering", op_video, L"Texture filtering", o_video, 0x09, 0, 2, 1, 2, k_quality, 1, L"", L"Takes effect from the next game you start"},
    {"lighting_quality", op_video, L"Lighting quality", o_video, 0x0A, 0, 2, 1, 2, k_quality, 1, L"", L"Takes effect from the next game you start"},
    {"effects_quality", op_video, L"Effects quality", o_video, 0x0B, 0, 2, 1, 2, k_quality, 1, L"", L"Takes effect from the next game you start"},
    {"details_quality", op_video, L"Details quality", o_video, 0x0D, 0, 2, 1, 2, k_quality, 1, L"", L"Takes effect from the next game you start"},
    {"post_processing", op_video, L"Post-processing quality", o_video, 0x0E, 0, 2, 1, 2, k_quality, 1, L"", L"Takes effect from the next game you start"},
    {"water_quality", op_video, L"Water quality", o_video, 0x0F, 0, 2, 1, 2, k_quality, 1, L"", L"Takes effect from the next game you start"},
    {"anti_aliasing", op_video, L"Anti-aliasing", o_video, 0x11, 0, 1, 1, 1, k_off_on, 1, L"", L"Takes effect from the next game you start"},
    {"motion_blur", op_video, L"Motion blur", o_video, 0x12, 0, 1, 1, 1, k_off_on, 1, L"", L"Takes effect from the next game you start"},
    // Audio
    {"female_voice", op_audio, L"Spartan voice", o_bool, 0x27, 0, 1, 1, 0, {L"Male", L"Female"}, 1, L"", L"Halo: Reach"},
    // Accessibility
    {"subtitles", op_access, L"Subtitles", o_subtitle, 0x00, 0, 1, 1, 0, k_off_on, 1, L"", L""},
    {"speaker_names", op_access, L"Speaker names in subtitles", o_bool, 0x07, 0, 1, 1, 1, k_off_on, 1, L"", L""},
    {"subtitle_background", op_access, L"Subtitle background opacity", o_float, 0x0C, 0, 100, 10, 50, {}, 100, L"%", L""},
    {"subtitle_shadow", op_access, L"Subtitle shadow opacity", o_float, 0x10, 0, 100, 10, 50, {}, 100, L"%", L""},
    {"colorblind_mode", op_access, L"Color blind mode", o_int, 0xAB4, 0, 3, 1, 0, {L"Off", L"Protanopia", L"Deuteranopia", L"Tritanopia"}, 1, L"", L""},
    {"colorblind_strength", op_access, L"Color blind filter strength", o_float, 0xAB8, 0, 100, 10, 100, {}, 100, L"%", L""},
    {"colorblind_brightness", op_access, L"Color blind filter brightness", o_float, 0xABC, 0, 100, 5, 50, {}, 100, L"%", L""},
    {"colorblind_contrast", op_access, L"Color blind filter contrast", o_float, 0xAC0, 0, 100, 5, 25, {}, 100, L"%", L""},
};
static float opt_value(const opt_def &o)
{
    auto it = g_settings.opt.find(o.key);
    float v = it == g_settings.opt.end() ? o.def : it->second;
    return std::max(o.lo, std::min(o.hi, v));
}


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
// Where quitting takes you: the PS5's home screen from the Installation 08 app (its
// Wine prefix lives under /data/installation08), Prospero Win's game list from
// Prospero Win, or the desktop on a PC (Windows, or Wine on Linux).
enum quit_target { quit_desktop, quit_prospero, quit_home };
static quit_target where_quit_goes()
{
    if (!running_under_wine()) return quit_desktop;
    // The PS5's display driver is loaded in every program with a window there.
    // (Wine passes the app's HOME on as WINE_HOST_HOME.)
    wchar_t buf[512] = {};
    bool ps5 = GetModuleHandleW(L"wineps5.drv") || GetEnvironmentVariableW(L"WINE_PS5_VIEW", buf, 512);
    if (!ps5) return quit_desktop;
    std::wstring where = g_cfg.mcc_root;
    DWORD n = GetEnvironmentVariableW(L"WINE_HOST_HOME", buf, 512);
    if (n && n < 512) where += L"|" + std::wstring(buf);
    for (auto &c : where) c = c == L'\\' ? L'/' : towlower(c);
    quit_target t = where.find(L"/data/installation08/") != std::wstring::npos ? quit_home : quit_prospero;
    static bool logged = false;
    if (!logged)
        logged = true, logf("quit goes to %s (wineps5.drv %s, from %s)", t == quit_home ? "the home screen" : "Prospero Win",
                            GetModuleHandleW(L"wineps5.drv") ? "loaded" : "not loaded", narrow(where.c_str()).c_str());
    return t;
}
static const wchar_t *quit_item()
{
    switch (where_quit_goes())
    {
    case quit_home: return L"Quit to home screen";
    case quit_prospero: return L"Quit to Prospero Win";
    default: return L"Quit to desktop";
    }
}
static const wchar_t *quit_title()
{
    switch (where_quit_goes())
    {
    case quit_home: return L"QUIT TO HOME SCREEN?";
    case quit_prospero: return L"QUIT TO PROSPERO WIN?";
    default: return L"QUIT TO DESKTOP?";
    }
}
static bool playstation_labels();
static const wchar_t *pad_name(int i) { return playstation_labels() ? k_ps_names[i] : k_pad_names[i]; }
// One of the menu's own buttons by its Xbox name: A, B, Start, Back.
static const wchar_t *btn(const wchar_t *xbox)
{
    if (!playstation_labels()) return xbox;
    if (!wcscmp(xbox, L"A")) return L"Cross";
    if (!wcscmp(xbox, L"B")) return L"Circle";
    if (!wcscmp(xbox, L"X")) return L"Square";
    if (!wcscmp(xbox, L"Y")) return L"Triangle";
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
        else if (k == "lan_address") g_settings.lan_address = value;
        else if (k == "lan_name" || k == "player_name") g_settings.lan_name = value;
        else if (k == "name_asked") g_settings.name_asked = (int)v;
        else if (k == "music_volume") g_settings.music = (int)v;
        else if (k == "effects_volume") g_settings.effects = (int)v;
        else if (k == "mouse_sensitivity") g_settings.mouse = v;
        else if (k == "mouse_sensitivity_2") g_settings.mouse_speed = std::max(1, std::min(100, (int)v));
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
        else if (k == "keyboard_mouse_player") g_settings.kbm_player = std::max(0, std::min(3, (int)v));
        else if (k == "split_screen_session") g_settings.split_session = (int)v;
        else if (k == "coop_continue_all") g_settings.coop_continue_all = (int)v;
        else if (k == "verbose_log") g_verbose_log = v != 0;
        else if (k == "online_server") lan::g_server_name = value;
        else if (k == "online_force_relay") lan::g_force_relay = v != 0;
        // Test builds only: a release ignores a test left on by an earlier test build.
        else if (k == "network_test" && I08_TEST_TOOLS) lan::g_sim = std::max(0, std::min(lan::k_sim_count - 1, (int)v));
        else if (k == "loading_screen") g_loading_screen = v != 0;
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
        else if (k.rfind("look_", 0) == 0) g_settings.look[k.substr(5)] = value;
        else if (k == "service_tag") g_settings.service_tag = value;
        else if (k.rfind("opt_", 0) == 0) g_settings.opt[k.substr(4)] = v;
        else if (k.rfind("map_variant_", 0) == 0) g_settings.map_variant[k.substr(12)] = value;
        else if (k.rfind("loadout_", 0) == 0 && k.size() == 9 && k[8] >= '1' && k[8] <= '5')
        {
            int n = k[8] - '1', i = 0;
            for (const char *c = value; c && *c && i < 6; c = strchr(c, ','), c = c ? c + 1 : nullptr) g_settings.loadout[n][i++] = atoi(c);
            g_settings.loadout_set[n] = true;
        }
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
    fprintf(f, "mouse_sensitivity_2 = %d\n", g_settings.mouse_speed);
    fprintf(f, "halo1_classic_graphics = %d\nhalo2_classic_graphics = %d\n", g_settings.h1_classic, g_settings.h2_classic);
    fprintf(f, "keyboard_mouse_player = %d\n", g_settings.kbm_player);
    fprintf(f, "button_labels = %d\nresolution = %d\nwindowed = %d\nplayers = %d\nkeyboard_player = %d\n",
            g_settings.button_style, g_settings.resolution, g_settings.windowed, g_settings.players, g_settings.keyboard_player);
    for (const auto &m : g_settings.ff_map) fprintf(f, "firefight_map_%s = %d\n", m.first.c_str(), m.second);
    for (const auto &m : g_settings.ff_variant) fprintf(f, "firefight_variant_%s = %s\n", m.first.c_str(), m.second.c_str());
    fprintf(f, "spartan_ops_mission = %d\n", g_settings.spops);
    if (!g_settings.lan_address.empty()) fprintf(f, "lan_address = %s\n", g_settings.lan_address.c_str());
    if (!g_settings.lan_name.empty()) fprintf(f, "player_name = %s\n", g_settings.lan_name.c_str());
    if (g_settings.name_asked) fprintf(f, "name_asked = 1\n");
    if (g_settings.split_session) fprintf(f, "split_screen_session = 1\n");
    if (g_settings.coop_continue_all) fprintf(f, "coop_continue_all = 1\n");
    for (const auto &m : g_settings.cg_map) fprintf(f, "custom_map_%s = %d\n", m.first.c_str(), m.second);
    for (const auto &m : g_settings.cg_variant) fprintf(f, "custom_variant_%s = %s\n", m.first.c_str(), m.second.c_str());
    if (!g_settings.last_game.empty()) fprintf(f, "last_game = %s\n", g_settings.last_game.c_str());
    if (g_verbose_log) fprintf(f, "verbose_log = 1\n");
    if (!lan::g_server_name.empty()) fprintf(f, "online_server = %s\n", lan::g_server_name.c_str());
    if (lan::g_sim > 0) fprintf(f, "network_test = %d\n", lan::g_sim.load());
    if (!g_loading_screen) fprintf(f, "loading_screen = 0\n");
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
    for (const auto &m : g_settings.look)
        if (!m.second.empty()) fprintf(f, "look_%s = %s\n", m.first.c_str(), m.second.c_str());
    if (!g_settings.service_tag.empty()) fprintf(f, "service_tag = %s\n", g_settings.service_tag.c_str());
    for (const auto &o : g_settings.opt) fprintf(f, "opt_%s = %g\n", o.first.c_str(), o.second);
    for (const auto &m : g_settings.map_variant)
        if (!m.second.empty()) fprintf(f, "map_variant_%s = %s\n", m.first.c_str(), m.second.c_str());
    for (int n = 0; n < 5; n++)
        if (g_settings.loadout_set[n])
        {
            const int *l = g_settings.loadout[n];
            fprintf(f, "loadout_%d = %d,%d,%d,%d,%d,%d\n", n + 1, l[0], l[1], l[2], l[3], l[4], l[5]);
        }
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
    g_cfg.mouse_sensitivity = 0.000002f * powf(5000.0f, (std::max(1, std::min(100, g_settings.mouse_speed)) - 1) / 99.0f);
    p[0x1DE] = 1; // color correction, as MCC sets it
    for (const auto &o : k_options)
    {
        float v = opt_value(o);
        switch (o.kind)
        {
        case o_bool: case o_byte: p[o.offset] = (unsigned char)(int)v; break;
        case o_not: p[o.offset] = v ? 0 : 1; break;
        case o_int: put<int32_t>(p, o.offset, (int32_t)v); break;
        case o_float: put<float>(p, o.offset, v / o.scale); break;
        case o_video: g_video_choices[o.offset] = (unsigned char)(int)v; break;
        case o_subtitle: p[o.offset] = v ? 1 : 0; g_subtitles = v != 0; break;
        }
    }
}

// ------------------------------------------------ armor, colors, service tag, loadouts
// MCC hands every game the player's look in the profile, each piece as MCC's own item
// number (libmcc's e_customization_item); the game turns it into its own (Reach:
// haloreach.dll+0x3b2f4, Halo 3: halo3.dll+0x2f628c). Item numbers and ids are MCC's.
struct look_item { uint16_t id; const char *name; };
static const look_item k_look_items[] = {
#include "customization_items.inc"
};
enum { ls_colors, ls_armor, ls_elite, ls_other };
struct look_slot
{
    const char *key;      // in the .ini: look_<game>_<key>
    const wchar_t *label;
    int section;
    int offset;           // in the profile
    std::vector<const char *> families; // item id prefixes (lower case)
    int index_offset = -1; // colors: also the position in the game's list (Halo CE to Halo 3 read that)
};
static const std::vector<look_slot> &look_slots(int module)
{
    static const std::vector<look_slot> reach = {
        {"primary", L"Primary color", ls_colors, 0x98, {"hr_color_"}, 0x2C},
        {"secondary", L"Secondary color", ls_colors, 0x9C, {"hr_color_"}, 0x30},
        {"helmet", L"Helmet", ls_armor, 0x40, {"hr_helmet_"}},
        {"left_shoulder", L"Left shoulder", ls_armor, 0x44, {"hr_leftshoulder_"}},
        {"right_shoulder", L"Right shoulder", ls_armor, 0x48, {"hr_rightshoulder_"}},
        {"chest", L"Chest", ls_armor, 0x4C, {"hr_chest_"}},
        {"wrist", L"Wrist", ls_armor, 0x50, {"hr_wrist_"}},
        {"utility", L"Utility", ls_armor, 0x54, {"hr_utility_"}},
        {"knees", L"Knee guards", ls_armor, 0x6C, {"hr_kneeguards_"}},
        {"visor", L"Visor color", ls_armor, 0x70, {"hr_visorcolor_"}},
        {"effect", L"Armor effect", ls_armor, 0x68, {"hr_armoreffect_"}},
        {"body", L"Gender", ls_armor, 0x64, {"hr_spartan_"}},
        {"elite", L"Elite armor", ls_elite, 0x8C, {"hr_elite_"}},
        {"elite_effect", L"Elite armor effect", ls_elite, 0x90, {"hr_elitearmoreffect_"}},
        {"voice", L"Firefight voice", ls_other, 0x94, {"hr_firefightvoice_"}},
    };
    static const std::vector<look_slot> h3 = {
        {"primary", L"Primary color", ls_colors, 0x98, {"h3_color"}, 0x2C},
        {"secondary", L"Secondary color", ls_colors, 0x9C, {"h3_color"}, 0x30},
        {"detail", L"Detail color", ls_colors, 0xA0, {"h3_color"}, 0x34},
        {"helmet", L"Helmet", ls_armor, 0x40, {"h3_helmet_", "h3e_helmet_"}},
        {"left_shoulder", L"Left shoulder", ls_armor, 0x44, {"h3_leftshoulder_", "h3e_leftshoulder_"}},
        {"right_shoulder", L"Right shoulder", ls_armor, 0x48, {"h3_rightshoulder_", "h3e_rightshoulder_"}},
        {"chest", L"Chest", ls_armor, 0x4C, {"h3_chest_", "h3e_chest_"}},
        {"arms", L"Arms", ls_armor, 0x58, {"h3_arms_", "h3e_arms_"}},
        {"legs", L"Legs", ls_armor, 0x5C, {"h3_legs_", "h3e_legs_"}},
        {"backpack", L"Backpack", ls_armor, 0x60, {"h3_backpack_", "h3e_backpack_"}},
        {"body", L"Body suit", ls_armor, 0x64, {"h3e_techsuit_"}},
        {"visor", L"Visor color", ls_armor, 0x70, {"h3_visor_"}},
        {"elite_helmet", L"Elite helmet", ls_elite, 0x74, {"h3e_elitehelmet_"}},
        {"elite_left_shoulder", L"Elite left shoulder", ls_elite, 0x78, {"h3e_eliteleftshoulder_"}},
        {"elite_right_shoulder", L"Elite right shoulder", ls_elite, 0x7C, {"h3e_eliterightshoulder_"}},
        {"elite_chest", L"Elite chest", ls_elite, 0x80, {"h3e_elitechest_"}},
        {"elite_arms", L"Elite arms", ls_elite, 0x84, {"h3e_elitearms_"}},
        {"elite_legs", L"Elite legs", ls_elite, 0x88, {"h3e_elitelegs_"}},
    };
    static const std::vector<look_slot> h4 = {
        {"primary", L"Primary color", ls_colors, 0x98, {"h4_color"}, 0x2C},
        {"secondary", L"Secondary color", ls_colors, 0x9C, {"h4_color"}, 0x30},
        {"helmet", L"Helmet", ls_armor, 0x40, {"h4_helmet_"}},
        {"left_shoulder", L"Left shoulder", ls_armor, 0x44, {"h4_leftshoulder_"}},
        {"right_shoulder", L"Right shoulder", ls_armor, 0x48, {"h4_rightshoulder_"}},
        {"chest", L"Chest", ls_armor, 0x4C, {"h4_chest_"}},
        {"arms", L"Arms", ls_armor, 0x58, {"h4_arms_"}},
        {"legs", L"Legs", ls_armor, 0x5C, {"h4_legs_"}},
        {"visor", L"Visor color", ls_armor, 0x70, {"h4_visor_"}},
        {"effect", L"Armor effect", ls_armor, 0x68, {"h4_armoreffect_"}},
        {"body", L"Gender", ls_armor, 0x64, {"h4_male", "h4_female"}},
    };
    static const std::vector<look_slot> h1 = {
        {"primary", L"Color", ls_colors, 0x98, {"h1_color_"}, 0x2C},
    };
    static const std::vector<look_slot> h2 = {
        {"primary", L"Primary color", ls_colors, 0x98, {"h2_color_"}, 0x2C},
        {"secondary", L"Secondary color", ls_colors, 0x9C, {"h2_color_"}, 0x30},
        {"detail", L"Detail color", ls_colors, 0xA0, {"h2_color_"}, 0x34},
    };
    static const std::vector<look_slot> odst = {
        {"primary", L"Primary color", ls_colors, 0x98, {"odst_color_"}, 0x2C},
        {"secondary", L"Secondary color", ls_colors, 0x9C, {"odst_color_"}, 0x30},
    };
    static const std::vector<look_slot> none;
    switch (module)
    {
    case 0: return h1;
    case 1: return h2;
    case 2: return h3;
    case 3: return h4;
    case 5: return odst;
    case 6: return reach;
    }
    return none;
}
// Spartan or Elite in multiplayer (profile 0x38): Halo 2, Halo 3 and Reach.
static bool look_has_species(int module) { return module == 1 || module == 2 || module == 6; }

static bool starts_with_nocase(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if (tolower((unsigned char)*s) != *prefix) return false;
    return true;
}
// The items a slot offers, in MCC's order.
static std::vector<const look_item *> look_choices(const look_slot &slot)
{
    std::vector<const look_item *> out;
    for (const auto &it : k_look_items)
        for (const char *f : slot.families)
            if (starts_with_nocase(it.name, f)) { out.push_back(&it); break; }
    return out;
}

// "HR_Helmet_MarkVB_Base" -> "Mark VB": without the game and the piece, split into words.
static std::wstring look_name(const char *id)
{
    std::string s = id;
    size_t a = s.find('_'), b = a == std::string::npos ? std::string::npos : s.find('_', a + 1);
    std::string rest = b != std::string::npos ? s.substr(b + 1) : a != std::string::npos ? s.substr(a + 1) : s;
    std::vector<std::string> words;
    std::string w;
    for (size_t i = 0; i < rest.size(); i++)
    {
        char c = rest[i];
        if (c == '_') { if (!w.empty()) words.push_back(w), w.clear(); continue; }
        if (!w.empty())
        {
            char p = w.back();
            bool next_lower = i + 1 < rest.size() && islower((unsigned char)rest[i + 1]);
            if ((islower((unsigned char)p) && isupper((unsigned char)c)) ||
                (isupper((unsigned char)p) && isupper((unsigned char)c) && next_lower && w.size() > 1))
                words.push_back(w), w.clear();
        }
        w += c;
    }
    if (!w.empty()) words.push_back(w);
    std::string out;
    for (size_t i = 0; i < words.size(); i++)
    {
        std::string x = words[i];
        bool lower_any = false, upper_any = false;
        for (char c : x) lower_any |= islower((unsigned char)c) != 0, upper_any |= isupper((unsigned char)c) != 0;
        // Kept in capitals: codes ("CQB", "UAHUL", "XV27", "SWFT") and a few known abbreviations.
        static const char *k_caps[] = {"EVA", "EOD", "ODST", "CIO", "SPI", "EVAC", "VI", "VB", "CQC", "GRD", "JFO", "UNSC", "AA", "HUL", "CNM", "CNMI", "MKVI"};
        bool vowel = false, digit = false, known = false;
        for (char c : x) vowel |= strchr("AEIOUaeiou", c) != nullptr, digit |= isdigit((unsigned char)c) != 0;
        for (const char *k : k_caps) known |= x == k;
        bool code = known || digit || (x.size() <= 4 && !vowel) || x.rfind("UA", 0) == 0 || x.rfind("CBRN", 0) == 0 ||
                    x.rfind("FC", 0) == 0;
        if ((upper_any && !lower_any && !code) || (lower_any && !upper_any))
        {
            for (auto &c : x) c = (char)tolower((unsigned char)c);
            x[0] = (char)toupper((unsigned char)x[0]);
        }
        if (_stricmp(x.c_str(), "Base") == 0 && words.size() > 1) continue;
        out += (out.empty() ? "" : " ") + x;
    }
    bool digits = !out.empty();
    for (char c : out) digits &= isdigit((unsigned char)c) != 0;
    if (digits) out = "Color " + out;
    std::wstring wide;
    for (char c : out) wide += (wchar_t)(unsigned char)c;
    return wide;
}

static std::string look_key(const std::string &game, const char *slot) { return game + "_" + slot; }

// Halo 4's loadouts: what each number is, read off the game's own loadout screen. Both weapons share one list, and the tactical
// package and support upgrade share another. Numbers the game shows no name for
// (campaign-only items) are left out.
struct loadout_item { int index; const wchar_t *name; };
struct loadout_field { const char *key; const wchar_t *label; int offset; std::vector<loadout_item> items; };
static const std::vector<loadout_field> &loadout_fields()
{
    static const std::vector<loadout_item> weapons = {
        {1, L"Assault Rifle"}, {43, L"Battle Rifle"}, {0, L"DMR"}, {30, L"Storm Rifle"}, {34, L"Covenant Carbine"},
        {24, L"Light Rifle"}, {44, L"Suppressor"}, {5, L"Magnum"}, {2, L"Plasma Pistol"}, {39, L"Boltshot"},
        {6, L"Needler"}, {9, L"Shotgun"}, {25, L"Scattershot"}, {37, L"SAW"}, {10, L"Sniper Rifle"}, {35, L"Beam Rifle"},
        {26, L"Binary Rifle"}, {28, L"Railgun"}, {36, L"Sticky Detonator"}, {41, L"Concussion Rifle"},
        {42, L"Fuel Rod Cannon"}, {8, L"Rocket Launcher"}, {11, L"Spartan Laser"}, {4, L"Energy Sword"},
        {12, L"Gravity Hammer"}};
    static const std::vector<loadout_item> perks = {
        {1, L"Mobility"}, {6, L"Shielding"}, {8, L"Resupply"}, {2, L"Grenadier"}, {7, L"AA Efficiency"}, {9, L"Firepower"},
        {16, L"Wheelman"}, {19, L"Fast Track"}, {18, L"Requisition"}, {11, L"Ammo"}, {3, L"Dexterity"}, {0, L"Sensor"},
        {5, L"Awareness"}, {20, L"Explosives"}, {4, L"Ordnance Priority"}, {13, L"Stability"}, {15, L"Drop Recon"},
        {14, L"Nemesis"}, {17, L"Gunner"}, {12, L"Stealth"}, {23, L"Survivor"}, {24, L"Recharge"}, {25, L"Resistor"}};
    static const std::vector<loadout_field> f = {
        {"primary", L"Primary weapon", 0x08, weapons},
        {"secondary", L"Secondary weapon", 0x0C, weapons},
        {"grenade", L"Grenade", 0x1C, {{0, L"Frag Grenade"}, {1, L"Plasma Grenade"}, {2, L"Pulse Grenade"}}},
        {"ability", L"Armor ability", 0x18,
         {{17, L"Promethean Vision"}, {7, L"Hologram"}, {18, L"Thruster Pack"}, {4, L"Active Camo"}, {12, L"Hardlight Shield"},
          {1, L"Jet Pack"}, {20, L"Regeneration Field"}, {14, L"Autosentry"}}},
        {"tactical", L"Tactical package", 0x00, perks},
        {"support", L"Support upgrade", 0x04, perks},
    };
    return f;
}
static int loadout_position(const loadout_field &f, int v)
{
    for (size_t i = 0; i < f.items.size(); i++)
        if (f.items[i].index == v) return (int)i;
    return -1;
}
static std::wstring loadout_value_name(const loadout_field &f, int v)
{
    int at = loadout_position(f, v);
    return at >= 0 ? f.items[at].name : L"Item " + std::to_wstring(v);
}

// Into the profile, before a game starts (and when changed from its pause menu).
static void apply_look(const std::string &game, int module)
{
    unsigned char *p = g_player_profile;
    put<int32_t>(p, 0x2C, 0);
    put<int32_t>(p, 0x30, 0);
    put<int32_t>(p, 0x34, 0);
    p[0x38] = 0;
    memset(p + 0x40, 0, 0x16C); // armor, colors, poses, skins
    memset(p + 0x1AC, 0, 8);    // service tag
    int set = 0;
    for (const auto &slot : look_slots(module))
    {
        auto it = g_settings.look.find(look_key(game, slot.key));
        if (it == g_settings.look.end() || it->second.empty()) continue;
        auto choices = look_choices(slot);
        for (size_t i = 0; i < choices.size(); i++)
            if (_stricmp(choices[i]->name, it->second.c_str()) == 0)
            {
                put<int32_t>(p, slot.offset, choices[i]->id);
                if (slot.index_offset >= 0) put<int32_t>(p, slot.index_offset, (int32_t)i);
                set++;
            }
    }
    if (look_has_species(module))
    {
        auto it = g_settings.look.find(look_key(game, "species"));
        p[0x38] = it != g_settings.look.end() && it->second == "elite" ? 1 : 0;
    }
    std::wstring tag;
    for (char c : g_settings.service_tag)
        if (isalnum((unsigned char)c) && tag.size() < 4) tag += (wchar_t)toupper((unsigned char)c);
    memcpy(p + 0x1AC, tag.c_str(), tag.size() * sizeof(wchar_t));
    // Halo 4's five loadouts: 0x3C each from 0x1E4, their names at +0x20.
    memset(p + 0x1E4, 0, 5 * 0x3C);
    if (module == 3)
        for (int n = 0; n < 5; n++)
        {
            unsigned char *l = p + 0x1E4 + n * 0x3C;
            const auto &fields = loadout_fields();
            for (size_t i = 0; i < fields.size(); i++) put<int32_t>(l, fields[i].offset, g_settings.loadout[n][i]);
            std::wstring name = L"Loadout " + std::to_wstring(n + 1);
            memcpy(l + 0x20, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
        }
    g_profile_changed = true;
    logf("look: %d pieces set for %s%s%s", set, game.c_str(), tag.empty() ? "" : ", service tag ",
         tag.empty() ? "" : narrow(tag.c_str()).c_str());
}

// ------------------------------------------------ settings changed inside a game
// Look inversion changed in a game's own menu (host #14), and each game's own block of
// profile settings (host #15, 256 bytes; MCC keeps one per game and player). Both are
// kept: the block in profiles\<game>.bin, restored into the profile at 0x310.
static std::atomic<bool> g_look_inverted_changed, g_game_data_changed;
static std::mutex g_game_data_lock;
static unsigned char g_game_data[0x100];

static std::wstring game_data_path(const std::wstring &game) { return g_cfg.host_dir + L"\\profiles\\" + game + L".bin"; }

static void load_game_data(const std::wstring &game)
{
    std::lock_guard<std::mutex> l(g_game_data_lock);
    memset(g_game_data, 0, sizeof g_game_data);
    bool got = false;
    if (FILE *f = _wfopen(game_data_path(game).c_str(), L"rb"))
    {
        got = fread(g_game_data, 1, sizeof g_game_data, f) == sizeof g_game_data;
        fclose(f);
    }
    if (!got) memset(g_game_data, 0, sizeof g_game_data);
    memcpy(g_player_profile + 0x310, g_game_data, sizeof g_game_data);
    logf("game settings block for %s: %s", narrow(game.c_str()).c_str(), got ? "restored" : "none kept yet");
}

static void h_set_player_look_control(void *, int player, bool inverted)
{
    LOG_CALL(14, "host.set_player_look_control player %d inverted %d", player, inverted ? 1 : 0);
    if (player != 0) return;
    g_player_profile[0x1D] = inverted ? 1 : 0;
    if (g_settings.invert_look != (inverted ? 1 : 0))
    {
        g_settings.invert_look = inverted ? 1 : 0;
        g_look_inverted_changed = true;
    }
}

static void h_set_player_profile_game_specific(void *, int player, const unsigned char *data)
{
    LOG_CALL(15, "host.set_player_profile_game_specific player %d", player);
    if (player != 0 || !data) return;
    std::lock_guard<std::mutex> l(g_game_data_lock);
    if (memcmp(g_game_data, data, sizeof g_game_data) == 0) return;
    memcpy(g_game_data, data, sizeof g_game_data);
    memcpy(g_player_profile + 0x310, data, sizeof g_game_data);
    g_game_data_changed = true;
}

// On the main thread, while a game runs and after it: write down what changed.
static void keep_game_changes()
{
    if (g_look_inverted_changed.exchange(false))
    {
        save_settings();
        logf("look inversion changed in the game: %d, kept", g_settings.invert_look);
    }
    if (g_game_data_changed.exchange(false))
    {
        unsigned char copy[0x100];
        {
            std::lock_guard<std::mutex> l(g_game_data_lock);
            memcpy(copy, g_game_data, sizeof copy);
        }
        CreateDirectoryW((g_cfg.host_dir + L"\\profiles").c_str(), nullptr);
        std::wstring path = game_data_path(g_cfg.game), temp = path + L".new";
        bool ok = false;
        if (FILE *f = _wfopen(temp.c_str(), L"wb"))
        {
            ok = fwrite(copy, 1, sizeof copy, f) == sizeof copy;
            ok = (fclose(f) == 0) && ok;
        }
        if (ok) ok = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        logf("game settings block changed in the game: %s", ok ? "kept" : "could not write it");
    }
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
    // H2A: the base game type of each kind (the rest are listed from the folders).
    static const std::vector<firefight_type> h2a = {
        {"H2A_100_100_Slayer_BASE_TeamSlayer_137", L"Team Slayer"}, {"H2A_100_200_Slayer_FFA_137", L"Slayer"},
        {"H2A_100_250_Slayer_BR_137", L"Team BRs"},                 {"H2A_100_150_Slayer_Pro_137", L"Slayer Pro"},
        {"H2A_100_750_Slayer_SWAT_137", L"SWAT"},                   {"H2A_200_200_CTF_Classic_Flag_137", L"Capture the Flag"},
        {"H2A_200_100_CTF_BASE_MultiFlag_137", L"Multi Flag CTF"},  {"H2A_200_500_CTF_One_Flag_137", L"One Flag CTF"},
        {"H2A_300_400_KOTH_BASE_King_137", L"King of the Hill"},    {"H2A_300_200_KOTH_Crazy_King_137", L"Crazy King"},
        {"H2A_400_100_ODDBALL_BASE_137", L"Oddball"},               {"H2A_500_100_JUGGERNAUT_BASE_137", L"Juggernaut"},
        {"H2A_600_100_ASSAULT_BASE_MultiBomb_137", L"Assault"},     {"H2A_700_100_TERRITORIES_BASE_3Plots_137", L"Territories"},
        {"H2A_800_100_INFECTION_BASE_137", L"Infection"},           {"H2A_900_100_RACE_BASE_137", L"Race"},
        {"H2A_950_100_RICOCHET_BASE_137", L"Ricochet"},
    };
    static const std::vector<firefight_type> none;
    return module == 6 ? reach : module == 3 ? h4 : module == 0 ? h1 : module == 1 ? h2 : module == 2 ? h3 : module == 4 ? h2a : none;
}
// Forge: the game's editing game type on any multiplayer map.
static const char *forge_type(int module)
{
    return module == 6 ? "00_basic_editing_054" : module == 3 ? "H4_BASIC_EDITING_132" : module == 2 ? "00_sandbox-0_010"
         : module == 4 ? "H2A_001_001_basic_editing_137" : nullptr;
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
    // H2A multiplayer: map ids from groundhog.dll's own id -> map switch. The three Forge
    // maps are empty skyboxes until a map variant (Skyward, Awash, Nebula layouts) is chosen.
    static const std::vector<mission_entry> h2a = {
        {158, "ca_coagulation", L"Bloodline"}, {155, "ca_lockout", L"Lockdown"},   {177, "ca_relic", L"Remnant"},
        {160, "ca_sanctuary", L"Shrine"},      {157, "ca_zanzibar", L"Stonetown"}, {159, "ca_warlock", L"Warlord"},
        {156, "ca_ascension", L"Zenith"},      {161, "ca_forge_skybox01", L"Skyward"},
        {162, "ca_forge_skybox02", L"Awash"},  {163, "ca_forge_skybox03", L"Nebula"},
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
    return module == 6 ? reach : module == 3 ? h4 : module == 0 ? h1 : module == 1 ? h2 : module == 2 ? h3 : module == 4 ? h2a : none;
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
        // Halo 2: Anniversary's multiplayer: its own engine (Halo 4's), in MCC's groundhog folder.
        {L"groundhog", L"Halo 2: Anniversary multiplayer", 4, false, {}},
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

// ------------------------------------------------ Forge maps (map variants)
// Saved in map_variants\<game>\<map file>\<name>.mvar next to the exe. To save, the
// host posts message 11; the game answers with its current map variant through host
// #12 (Reach: haloreach.dll+0xf9a7, Halo 3: halo3.dll+0xb97f), whose file bytes it
// gives with save_to_file. The variant then belongs to the host, which frees it.
static std::wstring map_variant_dir(const std::wstring &game, const std::string &map_file)
{
    std::wstring file(map_file.begin(), map_file.end());
    return g_cfg.host_dir + L"\\map_variants\\" + game + L"\\" + file;
}
static std::vector<std::wstring> saved_map_variants(const std::wstring &game, const std::string &map_file)
{
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((map_variant_dir(game, map_file) + L"\\*.mvar").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring n = fd.cFileName;
        out.push_back(n.substr(0, n.size() - 5));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::wstring &a, const std::wstring &b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return out;
}
static std::wstring file_safe(std::wstring s)
{
    for (auto &c : s)
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
    while (!s.empty() && s[0] == L' ') s.erase(0, 1);
    return s.size() > 60 ? s.substr(0, 60) : s;
}

static std::mutex g_forge_lock;
static std::wstring g_map_variant_path; // the saved map this game started with (or last saved to)
static std::wstring g_forge_status;     // for the pause menu
static std::atomic<int> g_forge_save{0}; // asked for: 1 save (over the loaded map), 2 save as a new map
static std::atomic<int64_t> g_forge_asked_at;

static void h_set_scenario_map_variant(void *, i_scenario_map_variant *variant)
{
    int want = g_forge_save.exchange(0);
    LOG_CALL(12, "host.set_scenario_map_variant %p%s", (void *)variant, want ? " (asked to save)" : "");
    if (!variant) return;
    if (want)
    {
        const char *buf = nullptr;
        uint32_t len = 0;
        bool ok = variant->save_to_file(&buf, &len) && buf && len;
        const wchar_t *vn = variant->get_name();
        std::wstring name = file_safe(vn ? std::wstring(vn, wcsnlen(vn, 64)) : L"");
        if (name.empty())
        {
            const mission_entry *m = current_mission();
            name = m ? m->name : L"Map";
        }
        std::wstring dir = map_variant_dir(g_cfg.game, narrow(g_cfg.map_file.c_str())), path;
        {
            std::lock_guard<std::mutex> l(g_forge_lock);
            path = want == 1 ? g_map_variant_path : L"";
        }
        if (path.empty())
        {
            for (int n = 1; n < 1000; n++)
            {
                std::wstring p = dir + L"\\" + name + (n > 1 ? L" " + std::to_wstring(n) : L"") + L".mvar";
                if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) { path = p; break; }
            }
        }
        if (ok && !path.empty())
        {
            CreateDirectoryW((g_cfg.host_dir + L"\\map_variants").c_str(), nullptr);
            CreateDirectoryW((g_cfg.host_dir + L"\\map_variants\\" + g_cfg.game).c_str(), nullptr);
            CreateDirectoryW(dir.c_str(), nullptr);
            std::wstring temp = path + L".new";
            ok = false;
            if (FILE *f = _wfopen(temp.c_str(), L"wb"))
            {
                ok = fwrite(buf, 1, len, f) == len;
                ok = (fclose(f) == 0) && ok;
            }
            if (ok) ok = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
            else DeleteFileW(temp.c_str());
        }
        std::wstring shown = path.substr(path.find_last_of(L'\\') + 1);
        if (shown.size() > 5) shown.resize(shown.size() - 5);
        {
            std::lock_guard<std::mutex> l(g_forge_lock);
            if (ok) g_map_variant_path = path;
            g_forge_status = ok ? L"Saved as “" + shown + L"”" : L"Could not save the map";
        }
        logf("forge: map variant \"%s\" (%u bytes) %s %s", narrow(name.c_str()).c_str(), len, ok ? "saved to" : "NOT saved,",
             narrow(path.c_str()).c_str());
    }
    variant->free_();
}

// Pause menu: ask the game for its map (it answers on its own thread).
static void request_forge_save(bool as_new)
{
    {
        std::lock_guard<std::mutex> l(g_forge_lock);
        g_forge_status = L"Saving…";
    }
    g_forge_save = as_new ? 2 : 1;
    g_forge_asked_at = (int64_t)GetTickCount64();
    if (g_engine) g_engine->post_message(11, nullptr);
    logf("forge: asked the game for its map variant (%s)", as_new ? "new map" : "save");
}
static std::wstring forge_status()
{
    std::lock_guard<std::mutex> l(g_forge_lock);
    if (g_forge_save && (int64_t)GetTickCount64() - g_forge_asked_at > 5000)
    {
        g_forge_save = 0;
        g_forge_status = L"The game didn't hand over the map";
    }
    return g_forge_status;
}
static std::wstring loaded_map_variant_name()
{
    std::lock_guard<std::mutex> l(g_forge_lock);
    std::wstring s = g_map_variant_path.substr(g_map_variant_path.find_last_of(L'\\') + 1);
    return s.size() > 5 ? s.substr(0, s.size() - 5) : L"";
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
    std::wstring map_variant; // custom games and Forge: a saved map's file (empty: the map as it comes)
    int classic = -1;         // LAN campaign: the host's graphics choice (-1: this machine's setting)
    bool coop = false;        // with resume: continue the co-op saved game (with the machines that joined)
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
                          int difficulty, const std::wstring &map_variant = L"")
{
    relaunch(game, map, file, difficulty);
    next_action n = g_next;
    n.mode = mode, n.variant = variant, n.map_variant = map_variant;
    g_next = n;
    logf("next: mode %d, game type %s%s%s", mode, variant.c_str(), map_variant.empty() ? "" : ", map variant ",
         narrow(map_variant.c_str()).c_str());
}

// ---------------------------------------------------------------- LAN play: starting games
namespace lan
{
static std::wstring widen(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// Co-op continue, on the host: get the saved game to every machine that joined. First
// an offer with each piece's hash (a client keeps the pieces its own co-op save already
// has), then the missing pieces in batches, each followed by "which pieces have you
// got?". The sending is paced, and slows down when pieces go missing: a PS5 on a wired
// network can send far faster than a handheld on Wi-Fi can take, and a flood also
// drowns the small messages that keep the lobby alive. Gives up (false) when nothing
// arrives for 15 seconds, or after 2 minutes.
static void pump_while_waiting()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) TranslateMessage(&msg), DispatchMessageW(&msg);
}
static bool send_save(const std::vector<peer> &peers, const std::string &data, uint32_t game_id, const std::wstring &game)
{
    uint32_t pieces = (uint32_t)((data.size() + k_piece - 1) / k_piece);
    std::vector<std::vector<bool>> need(peers.size(), std::vector<bool>(pieces, true));
    static std::vector<char> buf(sizeof(save_piece) + k_piece);
    static save_offer offer;
    memset(&offer, 0, sizeof(offer));
    offer.game_id = game_id;
    offer.total = (uint32_t)data.size();
    offer.pieces = pieces;
    put(offer.game, sizeof(offer.game), std::string(game.begin(), game.end()));
    for (uint32_t i = 0; i < pieces; i++)
    {
        uint32_t at = i * k_piece;
        offer.hash[i] = fnv64(data.data() + at, std::min<uint32_t>(k_piece, (uint32_t)data.size() - at));
    }
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_save_reports.clear();
    }
    const size_t offer_size = offsetof(save_offer, hash) + pieces * sizeof(uint64_t);
    const uint32_t k_batch = 48;      // pieces between questions: about 1.5 MB
    double rate = 4e6;                // bytes a second; between 0.5 and 16 MB/s
    int64_t start = ms(), progress_at = start;
    size_t best = SIZE_MAX, sent_total = 0, last_batch = 0, last_missing = 0;
    for (int round = 1;; round++)
    {
        // Ask which pieces each machine has (the first time, offer the hashes too).
        std::vector<uint32_t> before(peers.size(), 0);
        auto answers = [&](size_t p) -> uint32_t {
            for (const auto &x : g_save_reports)
                if (x.id == peers[p].id && x.game_id == game_id) return x.answers;
            return 0;
        };
        {
            std::lock_guard<std::mutex> l(g_lock);
            for (size_t p = 0; p < peers.size(); p++) before[p] = answers(p);
        }
        for (int ask = 0; ask < 8; ask++)
        {
            for (size_t p = 0; p < peers.size(); p++)
            {
                if (round == 1 || answers(p) == 0) raw_send(peers[p].addr, t_save_offer, &offer, offer_size);
                raw_send(peers[p].addr, t_save_query, &game_id, 4);
            }
            bool all = false;
            for (int w = 0; w < 10 && !all; w++)
            {
                Sleep(15);
                pump_while_waiting();
                std::lock_guard<std::mutex> l(g_lock);
                all = true;
                for (size_t p = 0; p < peers.size(); p++)
                    if (answers(p) == before[p]) all = false;
            }
            if (all) break;
        }
        size_t missing = 0;
        {
            std::lock_guard<std::mutex> l(g_lock);
            for (size_t p = 0; p < peers.size(); p++)
            {
                const save_report *r = nullptr;
                for (const auto &x : g_save_reports)
                    if (x.id == peers[p].id && x.game_id == game_id) r = &x;
                for (uint32_t i = 0; i < pieces; i++)
                {
                    if (r && (r->have[i / 8] >> (i % 8) & 1)) need[p][i] = false;
                    missing += need[p][i];
                }
            }
        }
        if (!missing)
        {
            logf("lan: sent the co-op saved game (%zu bytes, %zu sent) to %zu machine(s) in %lld ms, %d round(s)", data.size(),
                 sent_total, peers.size(), (long long)(ms() - start), round);
            return true;
        }
        if (round == 1)
            logf("lan: sending the co-op saved game: %zu of %u piece(s) needed", missing, pieces * (unsigned)peers.size());
        int64_t now = ms();
        if (missing < best) best = missing, progress_at = now;
        if (now - progress_at > 15000 || now - start > 120000)
        {
            logf("lan: sending the co-op saved game: %zu piece(s) still missing after %d round(s)", missing, round);
            return false;
        }
        // Did the last batch get through? Go faster if so, slower if not.
        if (round > 2 && last_batch)
        {
            size_t arrived = last_missing > missing ? last_missing - missing : 0;
            if (arrived * 10 >= last_batch * 9) rate = std::min(rate * 1.5, 16e6);
            else rate = std::max(rate / 2, 0.5e6);
        }
        // The next batch, paced.
        size_t batch = 0;
        int64_t t0 = ms();
        size_t bytes = 0;
        for (size_t p = 0; p < peers.size(); p++)
            for (uint32_t i = 0; i < pieces && batch < k_batch; i++)
            {
                if (!need[p][i]) continue;
                save_piece sp = {game_id, (uint32_t)data.size(), i, pieces};
                uint32_t at = i * k_piece, size = std::min<uint32_t>(k_piece, (uint32_t)data.size() - at);
                memcpy(buf.data(), &sp, sizeof(sp));
                memcpy(buf.data() + sizeof(sp), data.data() + at, size);
                raw_send(peers[p].addr, t_save, buf.data(), sizeof(sp) + size);
                batch++;
                bytes += size;
                int64_t due = t0 + (int64_t)(bytes * 1000.0 / rate);
                while (ms() < due) { Sleep(1); pump_while_waiting(); }
            }
        sent_total += bytes;
        last_batch = batch;
        last_missing = missing;
        if (round % 10 == 0)
            logf("lan: sending the co-op saved game, round %d: %zu piece(s) still missing, %.1f MB/s", round, missing, rate / 1e6);
    }
}

// The host, as a Halo CE custom game starts: if machines have joined, make the
// player list (this machine first) and send it, with what to play and the game type's
// file, to each of them.
static void host_game_starting(const next_action &what)
{
    if (g_role != k_host) return; // a client's list came from the host (poll)
    g_seats.clear();
    g_my_xuid = 0x0009000000000001ull;
    if (what.mode != 1 && what.mode != 2 && what.mode != 3 && what.mode != 5)
    {
        logf("lan: mode %d isn't played over LAN; playing alone", what.mode);
        return;
    }
    g_resume_data.clear();
    if (what.resume && !what.coop)
    {
        logf("lan: continuing the solo saved game is played alone");
        return;
    }
    std::vector<peer> peers;
    {
        std::lock_guard<std::mutex> l(g_lock);
        for (const auto &p : g_peers)
            if (std::find(g_left_out.begin(), g_left_out.end(), p.id) == g_left_out.end()) peers.push_back(p);
            else logf("lan: %s can't play this and is left out", p.name.c_str());
    }
    g_left_out.clear();
    if (peers.empty()) { logf("lan: nobody who joined can play this; playing alone"); return; }
    g_seats.push_back({g_my_xuid, g_my_id});
    size_t most = what.mode == 1 || what.mode == 2 ? 4 : 16; // campaign and Spartan Ops: 4 players
    if (peers.size() + 1 > most) logf("lan: co-op takes 4 players; the first %zu who joined play", most - 1);
    for (size_t i = 0; i < peers.size() && g_seats.size() < most; i++)
        g_seats.push_back({0x0009000000000001ull + 0x10 * (i + 1), peers[i].id});
    static start_msg m; // big: not on the stack
    memset(&m, 0, sizeof(m));
    strncpy(m.game, narrow(what.game.c_str()).c_str(), sizeof(m.game) - 1);
    m.map = what.map;
    strncpy(m.file, what.file.c_str(), sizeof(m.file) - 1);
    m.mode = what.mode;
    m.difficulty = what.difficulty;
    {
        const game_entry *ge = find_game(what.game);
        m.classic = !ge ? -1 : ge->module == 0 ? g_settings.h1_classic : ge->module == 1 ? g_settings.h2_classic : -1;
    }
    // The game type: its name for the others, and its file so they play the same one.
    std::string variant = what.variant;
    size_t slash = variant.find_last_of("\\/");
    strncpy(m.variant, variant.substr(slash == std::string::npos ? 0 : slash + 1).c_str(), sizeof(m.variant) - 1);
    std::wstring wv = widen(variant);
    std::wstring path = (wv.size() > 2 && wv[1] == L':' ? wv : g_cfg.mcc_root + L"\\" + what.game + L"\\" + wv) + L".bin";
    if (FILE *f = _wfopen(path.c_str(), L"rb"))
    {
        m.variant_size = (uint32_t)fread(m.variant_data, 1, sizeof(m.variant_data), f);
        if (fgetc(f) != EOF) m.variant_size = 0; // too big to send; the others use their own copy
        fclose(f);
    }
    m.count = (uint32_t)g_seats.size();
    for (size_t i = 0; i < g_seats.size(); i++) m.seats[i] = g_seats[i];
    static uint32_t next_id = GetTickCount() | 1;
    m.game_id = next_id++;
    // Co-op continue: everyone starts from the host's last co-op checkpoint, so it goes
    // to each of them first. If that fails, everyone starts the mission instead.
    if (what.resume && what.coop)
    {
        std::string bytes = read_file(coop_save_path(what.game));
        if (bytes.size() < 4096 || bytes.size() > (size_t)k_piece * k_most_pieces)
            logf("lan: the co-op saved game could not be read; starting the mission");
        else if (!g_settings.coop_continue_all)
        {
            // Only the host loads it; the others join the way they join any game (the
            // game itself may hand them the state, as Halo 3 did for players joining a
            // co-op game in progress).
            g_resume_data.assign(bytes.begin(), bytes.end());
            logf("lan: continuing the co-op saved game here; the other machines join it");
        }
        else if (!send_save(peers, bytes, m.game_id, what.game))
            logf("lan: the co-op saved game didn't reach everyone; starting the mission");
        else
        {
            g_resume_data.assign(bytes.begin(), bytes.end());
            m.save_size = (uint32_t)bytes.size();
            m.save_hash = fnv64(bytes.data(), bytes.size());
        }
    }
    // A saved Forge map goes first, in a message of its own.
    g_map_for_resend.size = 0;
    if (!what.map_variant.empty())
    {
        map_msg &map = g_map_for_resend;
        memset(&map, 0, offsetof(map_msg, data));
        map.game_id = m.game_id;
        if (FILE *f = _wfopen(what.map_variant.c_str(), L"rb"))
        {
            map.size = (uint32_t)fread(map.data, 1, sizeof(map.data), f);
            if (fgetc(f) != EOF) map.size = 0; // too big to send
            fclose(f);
        }
        if (map.size)
        {
            std::wstring name = what.map_variant.substr(what.map_variant.find_last_of(L'\\') + 1);
            if (name.size() > 5) name.resize(name.size() - 5);
            put(m.map_name, sizeof(m.map_name), narrow(name.c_str()));
            m.map_size = map.size;
            for (int round = 0; round < 3; round++)
            {
                for (const auto &p : peers) raw_send(p.addr, t_map, &map, offsetof(map_msg, data) + map.size);
                Sleep(20);
            }
            logf("lan: sent the saved map %s (%u bytes)", m.map_name, map.size);
        }
        else logf("lan: the saved map could not be read to send; the others play the map as it comes");
    }
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_start_acked.clear();
    }
    g_acking_id = m.game_id;
    // Sent until each machine says it has it (up to 10 seconds): one lost piece of the
    // start (with its game type, up to 60 KB) would leave that machine behind, and the
    // game would wait for it before starting without it.
    int64_t began = ms();
    size_t missing = peers.size();
    for (int round = 0; missing && ms() - began < 10000; round++)
    {
        missing = 0;
        for (const auto &p : peers)
        {
            {
                std::lock_guard<std::mutex> l(g_lock);
                if (g_start_acked.count(p.id)) continue;
            }
            missing++;
            if (round > 0 && m.map_size && g_map_for_resend.size)
                raw_send(p.addr, t_map, &g_map_for_resend, offsetof(map_msg, data) + g_map_for_resend.size);
            raw_send(p.addr, t_start, &m, k_start_head + m.variant_size);
        }
        if (missing) Sleep(round < 4 ? 30 : 250);
    }
    g_acking_id = 0;
    logf("lan: told %zu machine(s) to start %s, map %d (%s), game type %s (%u bytes); %s after %lld ms",
         peers.size(), m.game, m.map, m.file, m.variant, m.variant_size, missing ? "not all confirmed" : "all confirmed",
         (long long)(ms() - began));
    for (const auto &p : peers)
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (!g_start_acked.count(p.id)) logf("lan: %s never confirmed the start (left behind)", p.name.c_str());
    }
}

// After a game, on both sides: the host tells the others it's over.
static void game_ended()
{
    g_traffic_cut = false;
    if (g_role == k_host && !g_seats.empty())
    {
        std::vector<peer> peers;
        {
            std::lock_guard<std::mutex> l(g_lock);
            peers = g_peers;
        }
        for (int round = 0; round < 3; round++)
            for (const auto &p : peers) raw_send(p.addr, t_end, nullptr, 0);
    }
    g_seats.clear();
    g_my_xuid = 0x0009000000000001ull;
    g_end_ready = false;
}

// On the main thread: a client starts the host's game, or leaves it when the host ends it.
static void poll(bool in_game)
{
    if (in_game)
    {
        // (the host may also have gone away: then this machine isn't a client any more)
        if (!g_seats.empty() && g_role != k_host && g_end_ready.exchange(false) && g_next.kind == next_action::k_none)
        {
            logf("lan: the host ended the game");
            g_show_postgame = true, g_return_to_lobby = true;
            g_next.kind = next_action::k_launcher;
        }
        return;
    }
    if (g_role != k_client || !g_start_ready) { g_start_waiting = 0; return; }
    static start_msg m;
    static map_msg map;
    {
        std::lock_guard<std::mutex> l(g_start_lock);
        memcpy(&m, &g_start, k_start_head + std::min<uint32_t>(g_start.variant_size, sizeof(m.variant_data)));
        // A saved Forge map comes just before the start; give it a moment if it's late.
        map.size = 0;
        if (m.map_size && g_map_in.game_id == m.game_id && g_map_in.size == m.map_size)
            memcpy(&map, &g_map_in, offsetof(map_msg, data) + g_map_in.size);
    }
    if (m.map_size && !map.size)
    {
        if (!g_start_waiting) g_start_waiting = ms();
        if (ms() - g_start_waiting < 3000) return;
        logf("lan: the host's saved map didn't arrive; playing the map as it comes");
    }
    // Co-op continue: the host's saved game arrived before the start (the host checked).
    g_resume_data.clear();
    if (m.save_size)
    {
        std::lock_guard<std::mutex> l(g_start_lock);
        if (g_save_own_id == m.game_id && !g_save_own.empty())
            g_resume_data.swap(g_save_own);
        else if (g_save_in_id == m.game_id && g_save_in.size() == m.save_size && fnv64(g_save_in.data(), g_save_in.size()) == m.save_hash)
            g_resume_data.swap(g_save_in);
        g_save_own.clear();
        g_save_own_id = 0;
        g_save_in.clear();
        g_save_in_have.clear();
    }
    if (m.save_size && g_resume_data.empty())
    {
        g_start_ready = false;
        logf("lan: the host's co-op saved game didn't arrive complete; not starting");
        std::lock_guard<std::mutex> l(g_lock);
        g_left_reason = "The host's saved game didn't arrive; ask the host to start again";
        return;
    }
    g_start_waiting = 0;
    g_start_ready = false;
    g_end_ready = false;
    m.game[sizeof(m.game) - 1] = m.file[sizeof(m.file) - 1] = m.variant[sizeof(m.variant) - 1] = 0;
    std::wstring game(m.game, m.game + strlen(m.game));
    const game_entry *g = find_game(game);
    if (!g || !game_installed(*g))
    {
        logf("lan: the host started %s, which isn't installed here", m.game);
        std::lock_guard<std::mutex> l(g_lock);
        g_left_reason = "The host started a game that isn't installed here";
        return;
    }
    uint32_t n = std::min<uint32_t>(m.count, 16);
    g_seats.assign(m.seats, m.seats + n);
    bool found = false;
    for (const auto &seat : g_seats)
        if (seat.address == g_my_id) g_my_xuid = seat.xuid, found = true;
    if (!found)
    {
        logf("lan: the host started a game without this machine in it");
        g_seats.clear();
        return;
    }
    // The host's game type file, kept in lan_game_types next to the exe.
    std::string variant = std::string("game_variants\\") + m.variant;
    if (m.variant_size)
    {
        std::wstring dir = g_cfg.host_dir + L"\\lan_game_types";
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\" + game;
        CreateDirectoryW(dir.c_str(), nullptr);
        std::string name = m.variant;
        for (auto &c : name)
            if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        std::wstring path = dir + L"\\" + widen(name) + L".bin";
        if (FILE *f = _wfopen(path.c_str(), L"wb"))
        {
            fwrite(m.variant_data, 1, m.variant_size, f);
            fclose(f);
            variant = narrow((dir + L"\\" + widen(name)).c_str());
        }
    }
    // The host's saved Forge map, kept in lan_map_variants next to the exe.
    std::wstring map_path;
    if (map.size)
    {
        m.map_name[sizeof(m.map_name) - 1] = 0;
        std::wstring dir = g_cfg.host_dir + L"\\lan_map_variants";
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\" + game;
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring name = file_safe(widen(m.map_name));
        std::wstring path = dir + L"\\" + (name.empty() ? L"map" : name) + L".mvar";
        if (FILE *f = _wfopen(path.c_str(), L"wb"))
        {
            bool ok = fwrite(map.data, 1, map.size, f) == map.size;
            ok = (fclose(f) == 0) && ok;
            if (ok) map_path = path;
        }
        logf("lan: the host's saved map %s (%u bytes)%s", m.map_name, map.size, map_path.empty() ? " could not be written" : "");
    }
    next_action a;
    a.kind = next_action::k_game;
    a.map_variant = map_path;
    a.game = game;
    a.map = m.map;
    a.file = m.file;
    a.difficulty = m.difficulty;
    a.mode = m.mode;
    a.classic = m.classic;
    a.variant = variant;
    a.players = 1;
    a.resume = a.coop = !g_resume_data.empty();
    g_next = a;
    if (a.resume) logf("lan: continuing the host's co-op saved game (%zu bytes)", g_resume_data.size());
    logf("lan: the host started %s, map %d (%s), game type %s, %u players", m.game, m.map, m.file, variant.c_str(), n);
}

// For the launcher's header.
static std::wstring status()
{
    if (g_role == k_host)
    {
        size_t n;
        {
            std::lock_guard<std::mutex> l(g_lock);
            n = g_peers.size();
        }
        return std::wstring(lan::g_online ? L"  •  Hosting online: " : L"  •  Hosting a LAN game: ") + std::to_wstring(n) + (n == 1 ? L" player joined" : L" players joined");
    }
    if (g_role == k_client)
        return g_welcomed ? L"  •  In a LAN game" : L"  •  Looking for " + widen(g_join_text) + L"…";
    if (!g_problem.empty()) return L"  •  LAN: " + widen(g_problem);
    return L"";
}
} // namespace lan

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
static save_info read_save_info(const std::wstring &game, bool coop = false)
{
    save_info info;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    std::wstring path = coop ? coop_save_path(game) : save_path(game);
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return info;
    if (fa.nFileSizeHigh == 0 && fa.nFileSizeLow < 4096) return info; // not a real saved game
    FILE *f = _wfopen((path + L".info").c_str(), L"r");
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
enum page_id { p_main, p_missions, p_settings, p_quit, p_games, p_confirm, p_controls, p_pad, p_keys, p_firefight, p_spops, p_custom, p_forge, p_campaign, p_pick, p_lan, p_ip, p_joined, p_look, p_loadouts, p_loadout, p_text,
               p_set_controls, p_set_video, p_set_audio, p_set_access, p_lobby, p_customize, p_postgame };
// A yes/no question: its title, a line of detail, and what Yes does.
static std::wstring g_confirm_title, g_confirm_note;
static std::function<void()> g_confirm_yes;
struct item
{
    std::wstring label, value; // value empty: not adjustable
    std::function<void()> activate;
    std::function<void(int)> adjust;
    std::wstring note;   // what it does: shown at the bottom while it's selected
    bool enabled = true;
    bool heading = false; // a small section title; skipped when moving up and down
    std::wstring tile;    // main page: a game's tile (its folder); empty for other items
    std::wstring banner;  // a game's folder: its banner in the side panel
    int mode = -1;        // main menu: the mode it opens (a page_id)
    std::vector<std::pair<std::wstring, std::wstring>> roster; // side panel rows (a found lobby's players)
};

static std::recursive_mutex g_lock;
static std::atomic<bool> g_open, g_open_request;
static bool read_postgame();
static std::wstring g_pg_outcome, g_pg_title; // the post-game screen's title and its game
static page_id g_page = p_main;
static void open_name_entry();
static int g_selected = 0;
static int g_difficulty = 1;
static std::atomic<int64_t> g_last_game_frame;

// Launcher choices
static const game_entry *g_pick_game;
static int g_pick_mission;
static std::vector<std::pair<page_id, int>> g_history; // for Back

// Input edges
enum { a_up, a_down, a_left, a_right, a_accept, a_back, a_close, a_delete, k_actions };
// The splash screen when the launcher first opens: the name, then a fade into the menu.
static int64_t g_splash_start; // GetTickCount64() when it began; 0: none
static const int k_splash_hold = 3500, k_splash_out = 600, k_splash_in = 450;
static bool splash_blocks_input()
{
    return g_splash_start && (int64_t)GetTickCount64() - g_splash_start < k_splash_hold + k_splash_out;
}
static bool g_prev[k_actions];
static DWORD g_repeat_at[k_actions];
static POINT g_last_mouse;
static bool g_prev_click, g_prev_rclick;

// Held when the menu closed: hidden from the game until released.
static std::atomic<bool> g_masking;
static uint8_t g_masked_keys[256];
static WORD g_masked_buttons;

static D2D1_RECT_F g_item_rects[32];
static int g_item_count, g_scroll;
static int g_last_tile, g_last_bar; // (the old main page's tiles and bar)

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

// Hosting: which game and page (Custom game, Forge or Firefight) the LAN game is for.

static void start_launcher()
{
    static bool first = true;
    if (first) g_splash_start = (int64_t)GetTickCount64(), first = false;
    g_launcher = true;
    g_open = true;
    g_page = p_games;
    g_selected = 0;
    g_difficulty = g_settings.difficulty;
    for (auto &p : g_prev) p = true;
    g_prev_click = g_prev_rclick = true;
    // In a LAN game: back to its page (the host's Custom game page, or the lobby).
    g_history.clear();
    bool to_lobby = (lan::g_role == lan::k_host || g_return_to_lobby) && g_pick_game;
    if (to_lobby)
    {
        g_history.push_back({p_games, 0});
        g_page = p_lobby;
        g_selected = 0;
    }
    else if (lan::g_role == lan::k_client)
    {
        g_history.push_back({p_games, 0});
        g_page = p_joined;
        g_selected = 0;
    }
    // The first start: ask the player's name (once; Back keeps the computer's name).
    else if (!g_settings.name_asked)
    {
        g_settings.name_asked = 1;
        save_settings();
        open_name_entry();
    }
    if ((to_lobby || lan::g_role == lan::k_client) && g_show_postgame && read_postgame())
    {
        g_history.push_back({g_page, 0});
        g_page = p_postgame;
        g_selected = 0;
    }
    g_show_postgame = false, g_return_to_lobby = false;
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
    go(p_pad);
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
    for (const char *suffix : {"_054", "_010", "_132", "_031", "_012", "_137", "_tu", "_vanilla"})
    {
        size_t n = strlen(suffix);
        if (f.size() > n && !_stricmp(f.c_str() + f.size() - n, suffix)) f.resize(f.size() - n);
    }
    for (const char *prefix : {"h1_", "h2_", "h3_", "hr_", "h4_", "h2a_", "hce_", "odst_", "ff_"})
        if (f.size() > strlen(prefix) && !_strnicmp(f.c_str(), prefix, strlen(prefix))) f = f.substr(strlen(prefix));
    if (f.size() > 3 && isdigit((unsigned char)f[0]) && isdigit((unsigned char)f[1]) && f[2] == '_') f = f.substr(3);
    // H2A's numbered game types: "100_250_Slayer_BR" -> "Slayer BR"; "BASE" says nothing.
    while (f.size() > 4 && isdigit((unsigned char)f[0]) && isdigit((unsigned char)f[1]) && isdigit((unsigned char)f[2]) && f[3] == '_')
        f = f.substr(4);
    for (size_t at; (at = f.find("_BASE")) != std::string::npos;) f.erase(at, 5);
    std::string out;
    for (size_t i = 0; i < f.size(); i++)
    {
        char c = f[i];
        if (c == '_' || c == '-') { out += ' '; continue; }
        if (i && isupper((unsigned char)c) && islower((unsigned char)f[i - 1])) out += ' ';
        if (i && isalpha((unsigned char)c) && isdigit((unsigned char)f[i - 1]))
        {
            size_t run = 0;
            while (i + run < f.size() && isalpha((unsigned char)f[i + run])) run++;
            if (run >= 3) out += ' '; // "150points", "15min" (not "4v4", "12p")
        }
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
        bool shouting = word.size() > 4;
        for (char ch : word) shouting &= !islower((unsigned char)ch);
        if (caps)
            for (auto &ch : word) ch = (char)toupper((unsigned char)ch);
        else if (shouting) // "ODDBALL" -> "Oddball"
        {
            for (auto &ch : word) ch = (char)tolower((unsigned char)ch);
            word[0] = (char)toupper((unsigned char)word[0]);
        }
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
    // Where the current choice is once group headings are added above the groups.
    int row = std::max(0, current);
    for (int i = 0; i <= current && i < (int)g_pick_list.size(); i++)
        if (!g_pick_list[i].second.empty() && (i == 0 || g_pick_list[i - 1].second != g_pick_list[i].second)) row++;
    go(p_pick, row);
}

static int find_variant(const std::vector<variant_choice> &v, const std::string &path)
{
    for (size_t i = 0; i < v.size(); i++)
        if (!_stricmp(v[i].path.c_str(), path.c_str())) return (int)i;
    return 0;
}

// Game type categories, for sorting the Multiplayer list: by team size where the file
// name says (MCC's matchmaking types: "h2a_4v4_team_...", "..._8v8_..."), and by kind.
static const std::vector<std::wstring> &category_order()
{
    static const std::vector<std::wstring> order = {
        L"All game types", L"1v1", L"2v2", L"4v4", L"8v8 Big Team", L"Free for all",
        L"Slayer", L"Capture the Flag", L"King of the Hill", L"Oddball", L"Juggernaut", L"Assault", L"Territories",
        L"Headhunter", L"VIP", L"Infection", L"Race", L"Ricochet", L"Grifball", L"Extraction", L"Dominion",
        L"Regicide", L"Stockpile", L"Invasion", L"Gun Game", L"Other", L"Your game types"};
    return order;
}
static std::vector<std::wstring> variant_categories(const variant_choice &v)
{
    std::vector<std::wstring> out = {L"All game types"};
    if (v.group == L"Your game types") out.push_back(v.group);
    std::string f = v.path.substr(v.path.find_last_of('\\') + 1) + " " + narrow(v.name.c_str());
    for (auto &c : f) c = (char)tolower((unsigned char)c);
    auto has = [&](const char *w) { return f.find(w) != std::string::npos; };
    if (has("1v1") || has("h2h") || has("head_to_head") || has("headtohead") || has("head to head")) out.push_back(L"1v1");
    if (has("2v2")) out.push_back(L"2v2");
    if (has("4v4")) out.push_back(L"4v4");
    if (has("8v8") || has("btb") || has("big_team") || has("bigteam") || has("big team")) out.push_back(L"8v8 Big Team");
    if (has("ffa") || has("12p") || has("free for all")) out.push_back(L"Free for all");
    const char *kind =
        has("grif") || has("boomball") ? "Grifball"
      : has("infection") || has("hunted") || has("cadre") || has("flood") || has("zombie") || has("infect") ? "Infection"
      : has("rocketrace") || has("race") ? "Race"
      : has("ricochet") ? "Ricochet"
      : has("juggernaut") || has("naut") ? "Juggernaut"
      : has("headhunter") ? "Headhunter"
      : has("oddball") || has("ball") ? "Oddball"
      : has("koth") || has("king") || has("hill") ? "King of the Hill"
      : has("ctf") || has("flag") ? "Capture the Flag"
      : has("assault") || has("bomb") ? "Assault"
      : has("territor") || has("plots") || has("land_grab") || has("land grab") || has("incursion") || has("contention") ||
        has("gold_rush") || has("gold rush") || has("control_issues") || has("control issues") ? "Territories"
      : has("vip") ? "VIP"
      : has("extraction") ? "Extraction"
      : has("dominion") ? "Dominion"
      : has("regicide") ? "Regicide"
      : has("stockpile") ? "Stockpile"
      : has("invasion") ? "Invasion"
      : has("gun_game") || has("gungame") || has("gun game") ? "Gun Game"
      : has("slayer") || has("swat") || has("sniper") || has("snipe") || has("fiesta") || has("rocket") || has("sword") ||
        has("elimination") || has("phantom") || has("shotty") || has("shotgun") || has("duals") || has("1kill") ||
        has("minute") || has("mosh") || has("h2h") || has("arsenal") ? "Slayer"
      : "Other";
    out.push_back(std::wstring(kind, kind + strlen(kind)));
    return out;
}
static bool in_category(const variant_choice &v, const std::wstring &cat)
{
    for (const auto &c : variant_categories(v))
        if (c == cat) return true;
    return false;
}
// The categories a game's list has, in order.
static std::vector<std::wstring> categories_for(const std::vector<variant_choice> &types)
{
    std::set<std::wstring> have;
    for (const auto &t : types)
        for (const auto &c : variant_categories(t)) have.insert(c);
    std::vector<std::wstring> out;
    for (const auto &c : category_order())
        if (have.count(c)) out.push_back(c);
    return out;
}
static std::map<std::string, std::wstring> g_cg_category; // per game: the Category row's choice

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
            (void)0; // who uses what is in the lobby's panel
        }
}

static item heading_item(const std::wstring &label)
{
    item h{label, L"", nullptr, nullptr};
    h.enabled = false;
    h.heading = true;
    return h;
}

// ---- LAN games: finding and joining them, and hosting one.
static bool hosting_here(const game_entry *g, page_id page)
{
    return lan::g_role == lan::k_host && page == p_lobby;
}


// In the LAN lobby, a mode's items describe the game the host will start: tell the
// players. True when building the lobby (the Start button then starts it for everyone).
static page_id g_lobby_mode = p_campaign;
static bool add_lan_items(std::vector<item> &items, const game_entry *g, page_id page, const std::wstring &map,
                          const std::wstring &variant, const std::string &map_file = "")
{
    if (lan::g_role != lan::k_host || g_page != p_lobby) return false;
    int mode = page == p_campaign ? 1 : page == p_spops ? 2 : page == p_firefight ? 5 : 3;
    std::wstring detail = variant;
    lan::set_lobby(narrow(g->folder), narrow(map.c_str()), narrow(variant.c_str()), false, mode, g_difficulty, map_file,
                   narrow(detail.c_str()));
    return true;
}

static std::wstring lobby_title(const game_entry *g, int mode);
static int lan_mode_page(const lan::lobby_msg &m);
static std::wstring lobby_line(const lan::lobby_msg &m)
{
    std::wstring line;
    std::wstring folder = lan::widen(lan::field(m.game, sizeof m.game));
    const game_entry *g = folder.empty() ? nullptr : find_game(folder);
    if (g) line = lobby_title(g, lan_mode_page(m));
    std::string map = lan::field(m.map, sizeof m.map), variant = lan::field(m.variant, sizeof m.variant);
    if (!map.empty()) line += (line.empty() ? L"" : L"  •  ") + lan::widen(map);
    if (!variant.empty()) line += (line.empty() ? L"" : L"  •  ") + lan::widen(variant);
    int n = std::max(1, (int)m.count);
    line += (line.empty() ? L"" : L"  •  ") + std::to_wstring(n) + (n == 1 ? L" player" : L" players");
    if (m.playing) line += L"  •  playing now";
    return line;
}

// The address being typed on the Join by IP page.
static std::string g_ip_text;
static std::mutex g_typed_lock;
static std::wstring g_typed;
static void on_char(wchar_t c)
{
    std::lock_guard<std::mutex> l(g_typed_lock);
    if (g_typed.size() < 64) g_typed += c;
}

static std::string guess_ip_start()
{
    if (!g_settings.lan_address.empty()) return g_settings.lan_address;
    // This machine's own network: "192.168.1." for 192.168.1.20.
    if (!lan::open()) return "";
    INTERFACE_INFO list[32];
    DWORD bytes = 0;
    if (WSAIoctl(lan::g_sock, SIO_GET_INTERFACE_LIST, nullptr, 0, list, sizeof(list), &bytes, nullptr, nullptr) == 0)
        for (DWORD i = 0; i < bytes / sizeof(INTERFACE_INFO); i++)
        {
            const INTERFACE_INFO &f = list[i];
            if (!(f.iiFlags & IFF_UP) || (f.iiFlags & IFF_LOOPBACK) || f.iiAddress.Address.sa_family != AF_INET) continue;
            std::string ip = lan::ip_text(f.iiAddress.AddressIn, false);
            size_t dot = ip.find_last_of('.');
            if (dot != std::string::npos) return ip.substr(0, dot + 1);
        }
    return "";
}

static void step_ip(int d)
{
    size_t dot = g_ip_text.find_last_of('.');
    std::string head = dot == std::string::npos ? "" : g_ip_text.substr(0, dot + 1);
    std::string tail = dot == std::string::npos ? g_ip_text : g_ip_text.substr(dot + 1);
    int v = tail.empty() ? 0 : atoi(tail.c_str());
    if (tail.empty() && d < 0) v = 0;
    else v = (v + d + 256) % 256;
    g_ip_text = head + std::to_string(v);
}

static void join_typed()
{
    if (g_ip_text.empty()) return;
    if (lan::join_text(g_ip_text))
    {
        g_settings.lan_address = g_ip_text;
        save_settings();
        go(p_joined);
    }
}

static const wchar_t *mode_name(int mode);
static int lan_mode_page(const lan::lobby_msg &m);
static void open_text(const std::wstring &title, const std::wstring &note, const std::string &text, size_t max,
                      std::function<void(const std::string &)> done, bool name = false, const std::wstring &label = L"");
static void build_lan_items(std::vector<item> &items)
{
    lan::search();
    std::string reason = lan::left_reason();
    if (!reason.empty())
    {
        item r{lan::widen(reason), L"", nullptr, nullptr};
        r.enabled = false;
        items.push_back(r);
    }
    auto found = lan::found();
    for (const auto &f : found)
    {
        lan::found_game game = f;
        std::wstring mode = mode_name(lan_mode_page(f.info));
        item it{lan::widen(lan::field(f.info.host, sizeof f.info.host)), f.info.playing ? mode + L"  •  playing" : mode, [game] {
                    if (lan::join(game.addr, lan::field(game.info.host, sizeof game.info.host))) go(p_joined);
                }, nullptr};
        it.note = lobby_line(f.info);
        it.banner = lan::widen(lan::field(f.info.game, sizeof f.info.game));
        for (int i = 0; i < f.info.count && i < 16; i++)
            it.roster.push_back({lan::widen(lan::field(f.info.names[i], 32)), i == 0 ? L"Host" : L""});
        items.push_back(it);
    }
    for (const auto &pg : lan::public_found())
    {
        if (!I08_ONLINE) break;
        lan::public_game g = pg;
        bool on_lan = false;
        for (const auto &f : found) on_lan |= lan::field(f.info.host, sizeof f.info.host) == lan::field(g.e.host, sizeof g.e.host);
        if (on_lan) continue;
        std::wstring mode = mode_name(g.e.mode == 1 ? p_campaign : g.e.mode == 2 ? p_spops : g.e.mode == 5 ? p_firefight : p_custom);
        item it{lan::widen(lan::field(g.e.host, sizeof g.e.host)), mode + L"  •  Public" + (g.e.playing ? L"  •  playing" : L""), [g] {
                    if (lan::join_public(g)) go(p_joined);
                }, nullptr};
        std::wstring folder = lan::widen(lan::field(g.e.game, sizeof g.e.game));
        const game_entry *ge = folder.empty() ? nullptr : find_game(folder);
        std::wstring line = ge ? std::wstring(ge->title) : folder;
        if (g.e.map[0]) line += L"  •  " + lan::widen(lan::field(g.e.map, sizeof g.e.map));
        if (g.e.variant[0]) line += L"  •  " + lan::widen(lan::field(g.e.variant, sizeof g.e.variant));
        line += L"  •  " + std::to_wstring(g.e.count) + (g.e.count == 1 ? L" player" : L" players");
        it.note = line;
        it.banner = folder;
        items.push_back(it);
    }
    if (found.empty() && lan::public_found().empty())
    {
        item none{L"Looking for lobbies…", L"", nullptr, nullptr};
        none.enabled = false;
        items.push_back(none);
    }
#if I08_ONLINE
    item code{L"Join with a code", L"", [] {
                  open_text(L"ENTER THE JOIN CODE", L"The host's lobby shows it when the lobby is set to Private. The dashes are added for you",
                            "", 16, [](const std::string &text) {
                                if (lan::join_with_code(text)) go(p_joined);
                            }, false, L"Code");
              }, nullptr};
    code.note = L"For a friend's private lobby, anywhere";
    items.push_back(code);
#endif
    item ip{L"Join by IP address", L"", [] {
                g_ip_text = guess_ip_start();
                {
                    std::lock_guard<std::mutex> l(g_typed_lock);
                    g_typed.clear();
                }
                go(p_ip, 0);
            }, nullptr};
    ip.note = L"For a lobby on your network that doesn't show up in the list";
    items.push_back(ip);
}

static void build_ip_items(std::vector<item> &items)
{
    {
        std::wstring typed;
        {
            std::lock_guard<std::mutex> l(g_typed_lock);
            typed.swap(g_typed);
        }
        for (wchar_t c : typed)
        {
            if (c == 8) { if (!g_ip_text.empty()) g_ip_text.pop_back(); }
            else if (((c >= L'0' && c <= L'9') || c == L'.' || c == L':') && g_ip_text.size() < 60) g_ip_text += (char)c;
        }
    }
    bool blink = (GetTickCount64() / 500) % 2;
    item a{L"Address", lan::widen(g_ip_text) + (blink ? L"_" : L" "), [] { join_typed(); }, [](int d) { step_ip(d); }};
    a.note = L"Type it, or change the last number with left and right";
    items.push_back(a);
    item j{L"Join", L"", [] { join_typed(); }, nullptr};
    std::string reason = lan::left_reason();
    j.note = reason.empty() ? L"For machines on your network. The host sets its lobby to LAN" : lan::widen(reason);
    items.push_back(j);
}

static int check_lobby(const char *game, const char *map_file);
static std::wstring status_text(int status, const std::wstring &game);
static void build_joined_items(std::vector<item> &items)
{
    if (lan::g_role != lan::k_client)
    {
        std::string reason = lan::left_reason();
        item r{reason.empty() ? L"Not in a LAN game" : lan::widen(reason), L"", nullptr, nullptr};
        r.enabled = false;
        items.push_back(r);
        items.push_back({L"Back", L"", [] { go_back_one(); }, nullptr});
        return;
    }
    lan::lobby_msg m = lan::host_lobby();
    if (!lan::g_welcomed)
    {
        item look{L"Looking for " + lan::widen(lan::g_join_text) + L"…", L"", nullptr, nullptr};
        look.note = L"Connecting securely";
        look.enabled = false;
        items.push_back(look);
    }
    else
    {
        std::string host = lan::field(m.host, sizeof m.host);
        item top{lan::widen(host.empty() ? lan::g_join_text : host), L"", nullptr, nullptr};
        std::wstring folder = lan::widen(lan::field(m.game, sizeof m.game));
        const game_entry *g = folder.empty() ? nullptr : find_game(folder);
        int mine = m.game[0] ? check_lobby(m.game, m.map_file) : 0;
        top.value = L"Host";
        top.note = m.playing ? L"The host is playing a game; you'll be in the next one"
                 : mine >= 2 ? L"You can't play this: " + status_text(mine, g ? g->title : folder)
                 : m.game[0] ? L"Waiting for the host to start" : L"The host is choosing what to play";
        top.enabled = false;
        items.push_back(top);
        auto info = [&](const wchar_t *label, const std::wstring &value) {
            item i{label, value, nullptr, nullptr};
            i.enabled = false;
            items.push_back(i);
        };
        if (m.game[0])
        {
            info(L"Game", g ? lobby_title(g, lan_mode_page(m)) : folder);
            info(L"Mode", mode_name(lan_mode_page(m)));
            if (m.map[0]) info(m.mode == 1 ? L"Mission" : L"Map", lan::widen(lan::field(m.map, sizeof m.map)));
            if (m.mode == 1 || m.mode == 2 || m.mode == 5) info(L"Difficulty", difficulty_name(m.difficulty));
            if (m.mode != 1 && m.variant[0]) info(L"Game type", lan::widen(lan::field(m.variant, sizeof m.variant)));
        }
    }
    item leave{L"Leave lobby", L"", [] { lan::leave(); go_back_one(); }, nullptr};
    leave.note = L"Back to your main menu";
    items.push_back(leave);
}

// ---- Typing a short text (the service tag): keyboard, or left and right on the last letter.
static std::wstring g_text_title, g_text_note;
static std::string g_text;
static size_t g_text_max = 4;
static bool g_text_name; // a player name: upper and lower case, and spaces
static std::wstring g_text_label;
static std::function<void(const std::string &)> g_text_done;

static void open_text(const std::wstring &title, const std::wstring &note, const std::string &text, size_t max,
                      std::function<void(const std::string &)> done, bool name, const std::wstring &label)
{
    g_text_title = title, g_text_note = note, g_text = text.substr(0, max), g_text_max = max, g_text_done = std::move(done);
    g_text_name = name;
    g_text_label = !label.empty() ? label : name ? L"Name" : L"Tag";
    {
        std::lock_guard<std::mutex> l(g_typed_lock);
        g_typed.clear();
    }
    go(p_text, 1); // the keyboard's first row
}
static void text_finish()
{
    auto done = g_text_done;
    std::string text = g_text;
    go_back_one();
    if (done) done(text);
}
static void build_text_items(std::vector<item> &items)
{
    {
        std::wstring typed;
        {
            std::lock_guard<std::mutex> l(g_typed_lock);
            typed.swap(g_typed);
        }
        for (wchar_t c : typed)
        {
            if (c == 8) { if (!g_text.empty()) g_text.pop_back(); }
            else if (c < 128 && (isalnum((int)c) || (g_text_name && c == ' ')) && g_text.size() < g_text_max)
            {
                char t = g_text_name ? (char)c : (char)toupper((int)c);
                if (g_text_label == L"Code" && !strchr(lan::k_b32, t)) continue; // a join code has no 0 or O
                g_text += t;
            }
        }
    }
    // An on-screen keyboard for a controller (the PS5 has no keyboard popup): a row of
    // characters per item, left and right to pick one, A to type it.
    static int col = 0;
    static bool lower = false;
    bool blink = (GetTickCount64() / 500) % 2;
    bool code = g_text_label == L"Code";
    std::string shown = g_text;
    if (code) // in groups of four, as the host's lobby shows it
    {
        shown.clear();
        for (size_t i = 0; i < g_text.size(); i++) shown += (i && i % 4 == 0 ? "-" : "") + std::string(1, g_text[i]);
    }
    item t{g_text_label, lan::widen(shown) + (blink ? L"_" : L" "), [] { text_finish(); }, nullptr};
    t.note = g_text_note;
    items.push_back(t);
    const char *rows[] = {"ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "0123456789"};
    const char *code_rows[] = {"ABCDEFGHJKMN", "PQRSTUVWXYZ", "123456789"};
    if (code) for (int r = 0; r < 3; r++) rows[r] = code_rows[r];
    for (int r = 0; r < 3; r++)
    {
        std::string row = rows[r];
        if (lower && g_text_name && r < 2)
            for (char &c : row) c = (char)tolower((unsigned char)c);
        int n = (int)row.size();
        int at = std::min(col, n - 1);
        std::wstring label;
        for (int i = 0; i < n; i++)
        {
            if (i) label += L"  ";
            if (i == at && g_selected == (int)items.size()) label += std::wstring(L"[") + (wchar_t)row[i] + L"]";
            else label += (wchar_t)row[i];
        }
        item k{label, L"", [row, n] {
                   if (g_text.size() < g_text_max) g_text += row[std::min(col, n - 1)];
               },
               [n](int d) { col = (std::min(col, n - 1) + d + n) % n; }};
        k.enabled = g_text.size() < g_text_max;
        k.note = L"Left and right to pick, A to type";
        items.push_back(k);
    }
    if (g_text_name)
    {
        item sp{L"Space", L"", [] { if (g_text.size() < g_text_max && !g_text.empty() && g_text.back() != ' ') g_text += ' '; }, nullptr};
        sp.enabled = g_text.size() < g_text_max;
        items.push_back(sp);
        items.push_back({lower ? L"Capital letters" : L"Small letters", L"", [] { lower = !lower; }, nullptr});
    }
    items.push_back({L"Delete a letter", L"", [] { if (!g_text.empty()) g_text.pop_back(); }, nullptr});
    items.push_back({L"Done", L"", [] { text_finish(); }, nullptr});
}

// ---- The player's name: in the games (player 1) and in LAN lobbies.
static std::string clean_name(const std::string &in)
{
    std::string out;
    for (char c : in)
        if ((unsigned char)c < 128 && (isalnum((unsigned char)c) || c == ' ') && out.size() < 15 && !(c == ' ' && (out.empty() || out.back() == ' ')))
            out += c;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}
static std::string default_name()
{
    wchar_t computer[64];
    DWORD n = 64;
    std::string c = GetComputerNameW(computer, &n) ? clean_name(narrow(computer)) : "";
    return c.empty() ? "Player" : c;
}
static void open_name_entry()
{
    std::string now = clean_name(g_settings.lan_name);
    if (now.empty()) now = default_name();
    open_text(L"ENTER YOUR PLAYER NAME",
              L"Shown in the games and to other players on your network. Up to 15 letters, numbers and spaces",
              now, 15,
              [](const std::string &text) {
                  std::string name = clean_name(text);
                  if (name.empty()) return;
                  g_settings.lan_name = name;
                  lan::g_name = name;
                  save_settings();
                  logf("player name: %s", name.c_str());
              },
              true);
}

// ---- Armor and colors, the service tag, and Halo 4's loadouts.
static void look_changed(const game_entry *g)
{
    save_settings();
    if (!g_launcher && g && !_wcsicmp(g->folder, g_cfg.game.c_str())) apply_look(narrow(g_cfg.game.c_str()), g_module);
}

static void build_look_items(std::vector<item> &items)
{
    const game_entry *g = g_pick_game;
    if (!g) return;
    std::string game = narrow(g->folder);
    {
        std::string tag = g_settings.service_tag;
        item t{L"Service tag", tag.empty() ? L"None" : lan::widen(tag), [g] {
                   open_text(L"SERVICE TAG", L"Up to four letters and numbers", g_settings.service_tag, 4,
                             [g](const std::string &text) {
                                 g_settings.service_tag = text;
                                 look_changed(g);
                             });
               }, nullptr};
        t.note = L"Shown over your head and on the scoreboard, in every game";
        items.push_back(t);
    }
    if (look_has_species(g->module))
    {
        std::string key = look_key(game, "species");
        bool elite = g_settings.look[key] == "elite";
        auto flip = [g, key] {
            g_settings.look[key] = g_settings.look[key] == "elite" ? "" : "elite";
            look_changed(g);
        };
        item s{L"Multiplayer species", elite ? L"Elite" : L"Spartan", flip, [flip](int) { flip(); }};
        s.note = L"Where the game type allows it";
        items.push_back(s);
    }
    int section = -1;
    static const wchar_t *k_sections[] = {L"Colors", L"Armor", L"Elite", L"Other"};
    for (const auto &slot : look_slots(g->module))
    {
        if (slot.section != section) items.push_back(heading_item(k_sections[section = slot.section]));
        std::string key = look_key(game, slot.key);
        auto choices = look_choices(slot);
        int at = 0; // 0: the game's default; i + 1: choices[i]
        const std::string &cur = g_settings.look[key];
        for (size_t i = 0; i < choices.size(); i++)
            if (!_stricmp(choices[i]->name, cur.c_str())) at = (int)i + 1;
        int n = (int)choices.size() + 1;
        auto set = [g, key, choices](int i) {
            g_settings.look[key] = i > 0 && i <= (int)choices.size() ? choices[i - 1]->name : "";
            look_changed(g);
        };
        item it{slot.label, at ? look_name(choices[at - 1]->name) : L"Not set", [slot, at, choices, set] {
                    std::vector<std::pair<std::wstring, std::wstring>> list;
                    list.push_back({L"Not set (the game's own)", L""});
                    for (const auto *c : choices) list.push_back({look_name(c->name), L""});
                    std::wstring title = slot.label;
                    for (auto &c : title) c = towupper(c);
                    open_picker(title, list, at, set);
                }, [at, n, set](int d) { set((at + d + n) % n); }};
        it.note = std::to_wstring(n - 1) + L" to choose from";
        items.push_back(it);
    }
}

static int g_loadout_edit; // the loadout being edited (0-4)
static std::wstring loadout_line(int n)
{
    const auto &f = loadout_fields();
    const int *l = g_settings.loadout[n];
    return loadout_value_name(f[0], l[0]) + L"  •  " + loadout_value_name(f[1], l[1]) + L"  •  " +
           loadout_value_name(f[3], l[3]);
}
static void build_loadouts_items(std::vector<item> &items)
{
    for (int n = 0; n < 5; n++)
    {
        item it{L"Loadout " + std::to_wstring(n + 1), L"", [n] { g_loadout_edit = n; go(p_loadout, 0); }, nullptr};
        it.note = loadout_line(n);
        items.push_back(it);
    }
    item about{L"Custom games whose game type allows personal loadouts offer these", L"", nullptr, nullptr};
    about.enabled = false;
    items.push_back(about);
}
static void build_loadout_items(std::vector<item> &items)
{
    int n = g_loadout_edit;
    const auto &fields = loadout_fields();
    for (size_t i = 0; i < fields.size(); i++)
    {
        const loadout_field &f = fields[i];
        int v = g_settings.loadout[n][i], count = (int)f.items.size(), at = loadout_position(f, v);
        auto set = [n, i](int position) {
            g_settings.loadout[n][i] = loadout_fields()[i].items[position].index;
            g_settings.loadout_set[n] = true;
            look_changed(find_game(L"halo4"));
        };
        item it{f.label, loadout_value_name(f, v), [i, at, set] {
                    const loadout_field &f = loadout_fields()[i];
                    std::vector<std::pair<std::wstring, std::wstring>> list;
                    for (const auto &e : f.items) list.push_back({e.name, L""});
                    std::wstring title = f.label;
                    for (auto &c : title) c = towupper(c);
                    open_picker(title, list, std::max(0, at), set);
                }, [at, count, set](int d) { set(((at < 0 ? 0 : at + d) + count) % count); }};
        if (i < 2) it.note = L"Any weapon can go in either slot";
        items.push_back(it);
    }
}

// ---- Custom game and Forge pages: a Forge map saved on the chosen map.
static std::string map_variant_key(const game_entry *g, const mission_entry &m)
{
    std::string file = m.file;
    for (auto &c : file) c = (char)tolower((unsigned char)c);
    return narrow(g->folder) + "_" + file;
}
static std::wstring chosen_map_variant(const game_entry *g, const mission_entry &m)
{
    if (!forge_type(g->module)) return L"";
    auto it = g_settings.map_variant.find(map_variant_key(g, m));
    if (it == g_settings.map_variant.end() || it->second.empty()) return L"";
    std::wstring path = it->second.rfind("shipped:", 0) == 0
                      ? lan::widen(it->second.substr(8))
                      : map_variant_dir(g->folder, m.file) + L"\\" + lan::widen(it->second) + L".mvar";
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ? L"" : path;
}
// The map variants a game ships for a map (H2A: the classic layouts, and the Forge maps'
// layouts), as (name, path). Matched by the map's name in the file name.
static std::vector<std::pair<std::wstring, std::wstring>> shipped_map_variants(const game_entry *g, const mission_entry &m)
{
    std::vector<std::pair<std::wstring, std::wstring>> out;
    if (g->module != 4) return out;
    std::string word;
    for (const wchar_t *c = m.name; *c; c++) word += (char)towlower(*c);
    for (const wchar_t *dir : {L"map_variants", L"hopper_map_variants"})
    {
        std::wstring base = g_cfg.mcc_root + L"\\" + g->folder + L"\\" + dir;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((base + L"\\*.mvar").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring n = fd.cFileName;
            std::string lower = narrow(n.c_str());
            for (auto &ch : lower) ch = (char)tolower((unsigned char)ch);
            if (lower.find(word) == std::string::npos) continue;
            // "h2a_arena_skyward_fortHank" -> "Fort Hank (Arena)"; "bloodline_classic" -> "Classic".
            std::string stem = narrow(n.substr(0, n.size() - 5).c_str()), rest, kind;
            if (!_strnicmp(stem.c_str(), "h2a_", 4)) stem = stem.substr(4);
            size_t start = 0;
            for (int t = 0; start <= stem.size(); t++)
            {
                size_t end = stem.find('_', start);
                std::string tok = stem.substr(start, end == std::string::npos ? std::string::npos : end - start);
                std::string lt = tok;
                for (auto &ch : lt) ch = (char)tolower((unsigned char)ch);
                static const std::map<std::string, std::string> kinds = {
                    {"arena", "Arena"}, {"inf", "Infection"}, {"race", "Race"}, {"rocketrace", "Rocket Race"},
                    {"grif", "Grifball"}, {"hcs", "HCS"}};
                if (lt == word) {}
                else if (t == 0 && kinds.count(lt)) kind = kinds.at(lt);
                else rest += (rest.empty() ? "" : "_") + tok;
                if (end == std::string::npos) break;
                start = end + 1;
            }
            std::wstring name = rest.empty() ? (kind.empty() ? L"Matchmaking layout" : lan::widen(kind)) : pretty_variant_name(rest);
            if (!rest.empty() && !kind.empty()) name += L" (" + lan::widen(kind) + L")";
            out.push_back({name, base + L"\\" + n});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });
    return out;
}

static void add_map_variant_item(std::vector<item> &items, const game_entry *g, const mission_entry &m)
{
    if (!forge_type(g->module)) return;
    std::string key = map_variant_key(g, m);
    std::vector<std::wstring> list = saved_map_variants(g->folder, m.file);
    auto shipped = shipped_map_variants(g, m);
    // Choices: Original, the ones saved in Forge (stored by name), the game's own (stored
    // as "shipped:" and the file's path).
    std::vector<std::pair<std::wstring, std::wstring>> choices = {{L"Original", L""}};
    std::vector<std::string> values = {""};
    for (const auto &v : list) choices.push_back({v, L"Saved in Forge"}), values.push_back(narrow(v.c_str()));
    for (const auto &v : shipped) choices.push_back({v.first, L"Comes with the game"}), values.push_back("shipped:" + narrow(v.second.c_str()));
    std::string cur = g_settings.map_variant[key];
    int at = 0;
    for (size_t i = 0; i < values.size(); i++)
        if (!_stricmp(values[i].c_str(), cur.c_str())) at = (int)i;
    int n = (int)values.size();
    auto set = [key, values](int i) {
        g_settings.map_variant[key] = i >= 0 && i < (int)values.size() ? values[i] : "";
        save_settings();
    };
    item it{L"Map variant", choices[at].first, [choices, at, set] { open_picker(L"SELECT MAP VARIANT", choices, at, set); },
            [at, n, set](int d) { set((at + d + n) % n); }};
    it.note = !shipped.empty() && at == 0 && m.map >= 161 && m.map <= 163 ? L"An empty Forge map: pick a layout, or build one"
            : n == 1 ? L"Maps you save in Forge (pause menu, Save map) appear here"
                     : L"The map as it comes, a layout that comes with the game, or one you saved in Forge";
    items.push_back(it);
}

static bool forge_session() { return g_mode == 3 && forge_type(g_module) && g_ff_variant.find(forge_type(g_module)) != std::string::npos; }

// A mode's page (campaign, Firefight, Spartan Ops, custom game, Forge): its choices and
// Start. The LAN lobby shows the same items for the mode the host picked.
static void build_mode_items(page_id page, std::vector<item> &items)
{
    switch (page)
    {
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
        bool hosting = add_lan_items(items, g, p_campaign, m.name, std::wstring(L"Campaign, ") + difficulty_name(g_difficulty), m.file);
        if (!hosting) add_player_items(items);
        // Co-op continue: the last co-op checkpoint, with whoever has joined now.
        if (k_coop_continue && hosting && !lan::joined_names().empty())
        {
            save_info cs = read_save_info(g->folder, true);
            if (cs.exists)
            {
                std::wstring where = lan::widen(cs.file);
                for (const auto &mm : g->missions)
                    if (mm.map == cs.map) where = mm.name;
                item c{L"Continue co-op", where, [g] {
                           save_info i = read_save_info(g->folder, true);
                           if (!i.exists) return;
                           relaunch(g->folder, i.map, i.file, i.difficulty);
                           g_next.resume = g_next.coop = true;
                           logf("next: continue the co-op saved game");
                       }, nullptr};
                c.note = std::wstring(difficulty_name(cs.difficulty)) + L", saved " + lan::widen(cs.saved);
                items.push_back(c);
            }
        }
        items.push_back({hosting && !lan::joined_names().empty() ? L"Start LAN co-op" : L"Start campaign", L"", [g] {
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
        bool hosting = add_lan_items(items, g, p_firefight, g->firefight[*mi].name, types[ti].name, g->firefight[*mi].file);
        if (!hosting) add_player_items(items);
        items.push_back({hosting && !lan::joined_names().empty() ? L"Start LAN Firefight" : L"Start Firefight", L"", [g, mi, ti] {
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
        items.push_back({L"Map", maps[*mi].name, [g, mi] {
                             open_picker(L"SELECT MAP", map_list(custom_maps(g->module)), *mi, [mi](int i) { *mi = i; save_settings(); });
                         }, [mi, nm](int d) { *mi = (*mi + d + nm) % nm; save_settings(); }});
        // Category, then the game types in it.
        auto cats = categories_for(types);
        std::wstring &cat = g_cg_category[key];
        if (std::find(cats.begin(), cats.end(), cat) == cats.end()) cat = cats[0];
        std::vector<int> shown;
        for (size_t i = 0; i < types.size(); i++)
            if (in_category(types[i], cat)) shown.push_back((int)i);
        int ti = find_variant(types, g_settings.cg_variant[key]);
        int si = 0;
        bool found_it = false;
        for (size_t i = 0; i < shown.size(); i++)
            if (shown[i] == ti) si = (int)i, found_it = true;
        if (!found_it) ti = shown[0], si = 0, g_settings.cg_variant[key] = types[ti].path;
        int ci = (int)(std::find(cats.begin(), cats.end(), cat) - cats.begin());
        item ct{L"Category", cat, [cats, ci, key] {
                    std::vector<std::pair<std::wstring, std::wstring>> list;
                    for (const auto &c : cats) list.push_back({c, L""});
                    open_picker(L"SELECT CATEGORY", list, ci, [cats, key](int i) { g_cg_category[key] = cats[i]; });
                }, [cats, ci, key](int d) {
                    int n = (int)cats.size();
                    g_cg_category[key] = cats[(ci + d + n) % n];
                }};
        ct.note = L"Team size (4v4, 8v8 and so on) or kind of game: which game types to list";
        items.push_back(ct);
        item gt{L"Game type", types[ti].name, [g, key, shown, si] {
                    std::vector<std::pair<std::wstring, std::wstring>> list;
                    const auto &v = game_variants_for(g, false);
                    for (int i : shown) list.push_back({v[i].name, v[i].group});
                    open_picker(L"SELECT GAME TYPE", list, si, [g, key, shown](int i) {
                        g_settings.cg_variant[key] = game_variants_for(g, false)[shown[i]].path;
                        save_settings();
                    });
                }, [g, key, shown, si](int d) {
                    const auto &v = game_variants_for(g, false);
                    int n = (int)shown.size();
                    g_settings.cg_variant[key] = v[shown[(si + d + n) % n]].path;
                    save_settings();
                }};
        gt.note = std::to_wstring(shown.size()) + (shown.size() == 1 ? L" game type" : L" game types") +
                  (types[ti].group.empty() ? L"" : L"  \u2022  " + types[ti].group);
        items.push_back(gt);
        add_map_variant_item(items, g, maps[*mi]);
        bool hosting = add_lan_items(items, g, p_custom, maps[*mi].name, types[ti].name, maps[*mi].file);
        if (!hosting) add_player_items(items);
        items.push_back({hosting && !lan::joined_names().empty() ? (lan::g_online ? L"Start online game" : L"Start LAN game") : L"Start game", L"", [g, mi, ti] {
                             const mission_entry &m = custom_maps(g->module)[*mi];
                             relaunch_mode(3, g->folder, m.map, m.file, game_variants_for(g, false)[ti].path, g_difficulty,
                                           chosen_map_variant(g, m));
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
        add_map_variant_item(items, g, maps[*mi]);
        bool hosting = add_lan_items(items, g, p_forge, maps[*mi].name, L"Forge", maps[*mi].file);
        if (!hosting) add_player_items(items);
        items.push_back({hosting && !lan::joined_names().empty() ? L"Start LAN Forge" : L"Start Forge", L"", [g, mi] {
                             const mission_entry &m = custom_maps(g->module)[*mi];
                             relaunch_mode(3, g->folder, m.map, m.file, std::string("game_variants\\") + forge_type(g->module),
                                           g_difficulty, chosen_map_variant(g, m));
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
        bool hosting = add_lan_items(items, g_pick_game, p_spops, m.map_name, L"Spartan Ops");
        if (!hosting) add_player_items(items);
        items.push_back({hosting && !lan::joined_names().empty() ? L"Start LAN co-op" : L"Start mission", L"", [] {
                             const spops_mission &m = spops_missions()[g_settings.spops];
                             char file[64];
                             snprintf(file, sizeof file, "spops_e%d", m.episode);
                             relaunch_mode(2, L"halo4", m.map, file, std::string("spops_variants\\") + m.file, g_difficulty);
                         }, nullptr});
        break;
    }
    default:
        break;
    }
}

// ---- The LAN lobby.
// Can this machine play a lobby's choice? (Runs on the LAN thread for the hello.)
static bool map_file_installed(const game_entry *g, const std::string &file)
{
    if (file.empty()) return true;
    std::wstring f = lan::widen(file) + L".map";
    std::wstring base = g_cfg.mcc_root + L"\\" + g->folder + L"\\";
    for (const wchar_t *dir : {L"maps\\", L"h2_maps_win64_dx11\\"})
        if (GetFileAttributesW((base + dir + f).c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    return false;
}
static std::string read_coop_save(const char *game)
{
    std::wstring g;
    for (const char *c = game; *c; c++)
    {
        if (!isalnum((unsigned char)*c) && *c != '_') return std::string(); // a folder name only
        g += (wchar_t)*c;
    }
    return g.empty() ? std::string() : read_file(coop_save_path(g));
}
static int check_lobby(const char *game, const char *map_file)
{
    const game_entry *g = find_game(lan::widen(lan::field(game, 16)));
    if (!g || !game_installed(*g)) return 2;
    return map_file_installed(g, lan::field(map_file, 48)) ? 1 : 3;
}
static std::wstring status_text(int status, const std::wstring &game)
{
    switch (status)
    {
    case 1: return L"Ready";
    case 2: return L"Doesn't have " + game;
    case 3: return L"Doesn't have this map (install its part of " + game + L")";
    }
    return L"Checking\u2026";
}

// The modes a game offers. The game files don't change while we run, so it's kept.
static const std::vector<std::pair<page_id, const wchar_t *>> &lobby_modes(const game_entry *g)
{
    static std::map<const game_entry *, std::vector<std::pair<page_id, const wchar_t *>>> cache;
    auto it = cache.find(g);
    if (it != cache.end()) return it->second;
    auto &out = cache[g];
    if (!g->missions.empty()) out.push_back({p_campaign, L"Campaign"});
    if (!g->firefight.empty() && !installed_firefight_types(g).empty()) out.push_back({p_firefight, L"Firefight"});
    if (g->module == 3 && !installed_spops().empty()) out.push_back({p_spops, L"Spartan Ops"});
    if (!custom_maps(g->module).empty() && !installed_custom_types(g).empty()) out.push_back({p_custom, L"Custom game"});
    if (forge_type(g->module) && game_file_exists(g, "game_variants", forge_type(g->module))) out.push_back({p_forge, L"Forge"});
    return out;
}
static bool game_has_mode(const game_entry *g, page_id mode)
{
    for (const auto &m : lobby_modes(g))
        if (m.first == mode) return true;
    return false;
}
// The installed games that have a mode, in the menu's order.
static std::vector<const game_entry *> games_with_mode(page_id mode)
{
    std::vector<const game_entry *> out;
    for (const auto &g : games())
        if (game_installed(g) && game_has_mode(&g, mode)) out.push_back(&g);
    return out;
}

static const wchar_t *mode_name(int mode)
{
    switch (mode)
    {
    case p_campaign: return L"Campaign";
    case p_custom: return L"Multiplayer";
    case p_firefight: return L"Firefight";
    case p_spops: return L"Spartan Ops";
    case p_forge: return L"Forge";
    }
    return L"";
}
static const wchar_t *mode_blurb(int mode)
{
    switch (mode)
    {
    case p_campaign: return L"Play a campaign on your own, in split-screen, or in co-op with up to 4 players on your network.";
    case p_custom: return L"Custom games on any map with any game type, in split-screen or with others on your network.";
    case p_firefight: return L"Hold out against waves of enemies, on your own or with friends.";
    case p_spops: return L"Halo 4's episodes of co-op missions, on your own or with friends.";
    case p_forge: return L"Build and edit maps, on your own or together, and save them for custom games.";
    }
    return L"";
}
// A LAN lobby's mode (lobby_msg.mode) as the page it's played from.
static int lan_mode_page(const lan::lobby_msg &m)
{
    switch (m.mode)
    {
    case 1: return p_campaign;
    case 2: return p_spops;
    case 5: return p_firefight;
    case 3: return lan::field(m.variant, sizeof m.variant) == "Forge" ? p_forge : p_custom;
    }
    return -1;
}

// A game's name in a lobby. In Multiplayer and Forge, classic Halo 2 is "Halo 2" and H2A's
// multiplayer is "Halo 2: Anniversary", as MCC names them there.
static std::wstring lobby_title(const game_entry *g, int mode)
{
    if (!g) return L"";
    if (mode == p_custom || mode == p_forge)
    {
        if (!_wcsicmp(g->folder, L"halo2")) return L"Halo 2";
        if (!_wcsicmp(g->folder, L"groundhog")) return L"Halo 2: Anniversary";
    }
    return g->title;
}

static void pick_lobby_game(const game_entry *g)
{
    g_pick_game = g;
    g_pick_mission = 0;
    auto it = g_settings.last_map.find(narrow(g->folder));
    for (size_t i = 0; i < g->missions.size(); i++)
        if (it != g_settings.last_map.end() && g->missions[i].map == it->second) g_pick_mission = (int)i;
    if (!game_has_mode(g, g_lobby_mode)) g_lobby_mode = p_campaign;
}

// A mode's lobby, on the game played last when it has the mode. Offline until the
// player sets Network to LAN.
static void open_mode_lobby(page_id mode)
{
    auto list = games_with_mode(mode);
    if (list.empty()) return;
    const game_entry *g = list[0];
    for (const auto *x : list)
        if (narrow(x->folder) == g_settings.last_game) g = x;
    g_lobby_mode = mode;
    pick_lobby_game(g);
    g_lobby_mode = mode;
    go(p_lobby, 0);
}
static void request_mode(page_id mode)
{
    if (lan::g_role != lan::k_client) { open_mode_lobby(mode); return; }
    g_confirm_title = L"LEAVE THE LOBBY?";
    g_confirm_note = L"You're in " + lan::widen(lan::g_join_text) + L"'s lobby. Leave it to play something else";
    g_confirm_yes = [mode] { lan::leave(); open_mode_lobby(mode); };
    go(p_confirm, 1);
}

// Network: Offline, LAN or Online. LAN opens the lobby to others on the home network;
// Online also to friends anywhere who have its join code.
static bool g_focus_network; // the lobby's rows change with it: keep the cursor on Network
static int network_now() { return lan::g_role != lan::k_host ? 0 : !lan::g_online ? 1 : lan::g_public ? 3 : 2; }
static void set_network(int want)
{
    g_focus_network = true;
    if (want > 0)
    {
        if (lan::g_role != lan::k_host)
        {
            if (lan::g_role == lan::k_client) lan::leave();
            if (!lan::host_open()) return;
            logf("menu: lobby open on the network");
        }
        lan::set_online(want >= 2, want == 3);
        return;
    }
    if (lan::g_role != lan::k_host) return;
    if (!lan::joined_names().empty())
    {
        g_confirm_title = L"SET THE LOBBY TO OFFLINE?";
        g_confirm_note = L"The players who joined go back to their menus";
        g_confirm_yes = [] { lan::host_close(); };
        go(p_confirm, 1);
        return;
    }
    lan::host_close();
}

static void build_lobby_items(std::vector<item> &items)
{
    if (!g_pick_game) return;
    const game_entry *g = g_pick_game;
    page_id mode = g_lobby_mode;
    bool hosting = lan::g_role == lan::k_host;
    // The game: the installed ones that have this mode.
    auto list = games_with_mode(mode);
    std::vector<std::pair<std::wstring, std::wstring>> names;
    int at = 0;
    for (size_t i = 0; i < list.size(); i++)
    {
        if (list[i] == g) at = (int)i;
        names.push_back({lobby_title(list[i], mode), L""});
    }
    // Hosting: the mode can change without closing the lobby, so the group stays together.
    if (hosting)
    {
        std::vector<page_id> modes;
        for (page_id m : {p_campaign, p_custom, p_firefight, p_spops, p_forge})
            if (!games_with_mode(m).empty()) modes.push_back(m);
        int mi = 0;
        for (size_t i = 0; i < modes.size(); i++)
            if (modes[i] == mode) mi = (int)i;
        auto switch_to = [](page_id m) {
            g_lobby_mode = m;
            if (!g_pick_game || !game_has_mode(g_pick_game, m))
            {
                auto with = games_with_mode(m);
                if (!with.empty()) pick_lobby_game(with[0]);
            }
            g_lobby_mode = m;
        };
        item md{L"Mode", mode_name(mode), [modes, mi, switch_to] { switch_to(modes[(mi + 1) % modes.size()]); },
                [modes, mi, switch_to](int d) {
                    int n = (int)modes.size();
                    switch_to(modes[(mi + d + n) % n]);
                }};
        md.note = L"Players stay in the lobby when you change it";
        items.push_back(md);
    }
    item gi{L"Game", lobby_title(g, mode), [names, at, list] {
                open_picker(L"SELECT GAME", names, at, [list](int i) { pick_lobby_game(list[i]); });
            }, nullptr};
    if (list.size() > 1)
        gi.adjust = [at, list](int d) {
            int n = (int)list.size();
            pick_lobby_game(list[(at + d + n) % n]);
        };
    items.push_back(gi);
    size_t first_mode_item = items.size();
    build_mode_items(mode, items);
    // Network goes just above the Start button.
    size_t starts = items.size();
    while (starts > first_mode_item && (items[starts - 1].label.rfind(L"Start", 0) == 0 || items[starts - 1].label == L"Continue co-op"))
        starts--;
    int now = network_now();
    const wchar_t *net_names[] = {L"Local", L"LAN", L"Private", L"Public"};
    const int choices = I08_ONLINE ? 4 : 2;
    item net{L"Lobby", net_names[now], [now, choices] { set_network((now + 1) % choices); },
             [now, choices](int d) { set_network((now + d + choices) % choices); }};
    net.note = !lan::g_problem.empty() ? lan::widen(lan::g_problem)
             : now == 1 ? L"LAN: players on your network can join from Browse games"
             : now == 2 ? L"Private: friends anywhere join with the join code below; players on your network, from Browse games"
             : now == 3 ? L"Public: listed in Browse games, so anyone can join"
                        : I08_ONLINE ? L"Local: just this machine. LAN: your network. Private: friends anywhere, with a join code. Public: anyone"
                                     : L"Local: just this machine. LAN: players on your network can join from Browse games";
    items.insert(items.begin() + starts, net);
    if (now == 2)
    {
        std::string code = lan::join_code();
        std::string status = lan::online_status();
        item jc{L"Join code", !code.empty() ? lan::widen(code) : status.rfind("Connecting", 0) == 0 ? L"Getting it ready…" : L"Not available",
                [code] {
                    if (code.empty() || !OpenClipboard(g_hwnd)) return;
                    EmptyClipboard();
                    std::wstring w = lan::widen(code);
                    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t)))
                    {
                        memcpy(GlobalLock(h), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
                        GlobalUnlock(h);
                        SetClipboardData(CF_UNICODETEXT, h);
                    }
                    CloseClipboard();
                    logf("menu: join code copied");
                }, nullptr};
        jc.note = lan::widen(status) + (code.empty() ? L"" : L". Give the code only to people you want in the game");
        items.insert(items.begin() + starts + 1, jc);
    }
    if (g_focus_network && g_page == p_lobby) g_selected = (int)starts, g_focus_network = false;
    // Players who can't play the choice: Start asks first.
    std::vector<uint64_t> cannot;
    std::wstring cannot_names;
    if (hosting)
        for (const auto &pl : lan::lobby_players())
            if (pl.status >= 2)
            {
                cannot.push_back(pl.id);
                cannot_names += (cannot_names.empty() ? L"" : L", ") + lan::widen(pl.name);
            }
    if (!cannot.empty())
        for (size_t i = starts + 1; i < items.size(); i++)
        {
            auto start = items[i].activate;
            items[i].activate = [start, cannot, cannot_names] {
                g_confirm_title = L"START WITHOUT THEM?";
                g_confirm_note = cannot_names + L" can't play this and will stay in the lobby";
                g_confirm_yes = [start, cannot] {
                    lan::g_left_out = cannot;
                    start();
                };
                go(p_confirm, 1);
            };
            items[i].note = cannot_names + L" can't play this";
        }
    // Campaign, offline: carry on from the last save.
    if (mode == p_campaign && !hosting)
    {
        save_info save = read_save_info(g->folder);
        if (save.exists)
        {
            std::wstring where;
            for (const auto &mm : g->missions)
                if (mm.map == save.map) where = mm.name;
            item c{L"Continue from last save", where, [g] { continue_from_save(g->folder); }, nullptr};
            c.note = std::wstring(difficulty_name(save.difficulty)) +
                     (save.saved.empty() ? L"" : L"  •  saved " + std::wstring(save.saved.begin(), save.saved.end()));
            items.push_back(c);
        }
    }
}

static std::vector<item> build_items()
{
    lan::g_check_lobby = check_lobby;
    lan::g_read_coop_save = read_coop_save;
    std::vector<item> items;
    switch (g_page)
    {
    case p_games:
    {
        if (lan::g_role == lan::k_client)
        {
            item y{L"Return to lobby", L"", [] { go(p_joined); }, nullptr};
            y.note = L"You're in " + lan::widen(lan::g_join_text) + L"'s lobby.";
            items.push_back(y);
        }
        bool any = false;
        const game_entry *last = nullptr;
        for (const auto &g : games())
            if (game_installed(g))
            {
                any = true;
                if (narrow(g.folder) == g_settings.last_game) last = &g;
            }
        if (!any)
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
                i.note = L"Copy your MCC folders (halo1, halo2, halo3, halo3odst, haloreach, halo4) into " + where + L".";
            }
            else
                i.note = L"Put installation08.exe in the Halo The Master Chief Collection folder, or start it with --mcc <folder>.";
            i.enabled = false;
            items.push_back(i);
        }
        // Continue: the last game's campaign save. Read at most once a second.
        if (last && lan::g_role != lan::k_client)
        {
            static int64_t read_at;
            static const game_entry *read_for;
            static save_info save;
            if (read_for != last || now_ms() - read_at > 1000) save = read_save_info(last->folder), read_for = last, read_at = now_ms();
            if (save.exists)
            {
                std::wstring where;
                for (const auto &mm : last->missions)
                    if (mm.map == save.map) where = mm.name;
                const game_entry *g = last;
                item c{L"Continue", L"", [g] { continue_from_save(g->folder); }, nullptr};
                c.banner = last->folder;
                c.note = std::wstring(last->title) + (where.empty() ? L"" : L"\n" + where) + L"\n" + difficulty_name(save.difficulty) +
                         (save.saved.empty() ? L"" : L"  •  saved " + std::wstring(save.saved.begin(), save.saved.end()));
                items.push_back(c);
            }
        }
        for (page_id m : {p_campaign, p_custom, p_firefight, p_spops, p_forge})
        {
            if (games_with_mode(m).empty()) continue;
            item it{mode_name(m), L"", [m] { request_mode(m); }, nullptr};
            it.mode = m;
            it.note = mode_blurb(m);
            items.push_back(it);
        }
        {
            lan::search();
            size_t open = lan::found().size();
            item b{L"Browse games", open ? std::to_wstring(open) + L" open" : L"", [] { go(p_lan); }, nullptr};
            b.note = I08_ONLINE ? L"Join a lobby on your network, a public one online, or a friend's private one with its code: campaign co-op, custom "
                                  L"games, Firefight, Spartan Ops or Forge."
                                : L"Join a lobby on your network: campaign co-op, custom games, Firefight, Spartan Ops or Forge.";
            items.push_back(b);
        }
        {
            item c{L"Customize", L"", [] { go(p_customize); }, nullptr};
            c.note = L"Your name, and your armor, colors and loadouts in each game.";
            items.push_back(c);
        }
        {
            item st{L"Settings", L"", [] { go(p_settings); }, nullptr};
            st.note = L"Controls, video, audio and accessibility.";
            items.push_back(st);
        }
        {
            item q{L"Quit", L"", [] { go(p_quit, 1); }, nullptr};
            q.note = where_quit_goes() == quit_prospero ? L"Back to Prospero Win's game list."
                     : where_quit_goes() == quit_home ? L"Close Installation 08 and go back to the home screen."
                                                      : L"Close Installation 08.";
            items.push_back(q);
        }
        break;
    }
    case p_postgame:
    {
        item c{L"Continue", L"", [] { go_back_one(); }, nullptr};
        items.push_back(c);
        break;
    }
    case p_customize:
    {
        std::string name = lan::g_name.empty() ? default_name() : lan::g_name;
        item nm{L"Your name", lan::widen(name), [] { open_name_entry(); }, nullptr};
        nm.note = L"In the games, and in LAN lobbies.";
        items.push_back(nm);
        bool heading = false;
        for (const auto &g : games())
        {
            if (!game_installed(g) || look_slots(g.module).empty()) continue;
            if (!heading) items.push_back(heading_item(L"Armor and colors")), heading = true;
            const game_entry *gp = &g;
            item it{g.title, L"", [gp] { g_pick_game = gp; go(p_look, 0); }, nullptr};
            it.banner = g.folder;
            it.note = L"Your armor, colors and service tag in " + std::wstring(g.title) + L".";
            items.push_back(it);
        }
        if (const game_entry *h4 = find_game(L"halo4"))
            if (game_installed(*h4))
            {
                items.push_back(heading_item(L"Loadouts"));
                item lo{L"Halo 4 loadouts", L"", [h4] { g_pick_game = h4; go(p_loadouts, 0); }, nullptr};
                lo.banner = h4->folder;
                lo.note = L"Five loadouts for custom games whose game type allows your own.";
                items.push_back(lo);
            }
        break;
    }
    case p_lan:
        build_lan_items(items);
        break;
    case p_ip:
        build_ip_items(items);
        break;
    case p_joined:
        build_joined_items(items);
        break;
    case p_lobby:
        build_lobby_items(items);
        break;
    case p_text:
        build_text_items(items);
        break;
    case p_look:
        build_look_items(items);
        break;
    case p_loadouts:
        build_loadouts_items(items);
        break;
    case p_loadout:
        build_loadout_items(items);
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
            // A group's name above its first entry, so the list reads as sections.
            if (!g_pick_list[i].second.empty() && (i == 0 || g_pick_list[i - 1].second != g_pick_list[i].second))
                items.push_back(heading_item(g_pick_list[i].second));
            items.push_back(it);
        }
        break;
    case p_campaign:
    case p_firefight:
    case p_custom:
    case p_forge:
    case p_spops:
        build_mode_items(g_page, items);
        break;
    case p_main:
        items.push_back({L"Resume", L"", [] { close_menu(); }, nullptr});
        if (g_mode != 1)
        {
            if (!(lan::g_role == lan::k_client && !lan::g_seats.empty()))
            items.push_back({g_mode == 5 ? L"Restart Firefight" : g_mode == 3 ? L"Restart game" : L"Restart mission", L"", [] {
                                 std::wstring map_variant;
                                 {
                                     std::lock_guard<std::mutex> l(g_forge_lock);
                                     map_variant = g_map_variant_path;
                                 }
                                 relaunch_mode(g_mode, g_cfg.game, g_cfg.map, narrow(g_cfg.map_file.c_str()), g_ff_variant,
                                               g_difficulty, map_variant);
                             }, nullptr});
            if (forge_session())
            {
                std::wstring loaded = loaded_map_variant_name(), status = forge_status();
                item save{L"Save map", status, [] { request_forge_save(false); }, nullptr};
                save.note = loaded.empty() ? L"Saves this map as a new map variant, for Forge and custom games"
                                           : L"Saves over \u201C" + loaded + L"\u201D";
                items.push_back(save);
                if (!loaded.empty())
                {
                    item copy{L"Save as a new map", L"", [] { request_forge_save(true); }, nullptr};
                    copy.note = L"Keeps \u201C" + loaded + L"\u201D as it was";
                    items.push_back(copy);
                }
            }
            if (g_mode != 3)
            {
                item d{L"Difficulty", difficulty_name(g_difficulty), [] { g_difficulty = (g_difficulty + 1) % 4; },
                       [](int dir) { g_difficulty = (g_difficulty + dir + 4) % 4; }};
                d.note = L"Takes effect when you restart";
                items.push_back(d);
            }
            items.push_back({L"Settings", L"", [] { go(p_settings); }, nullptr});
            items.push_back({L"Controls", L"", [] { open_controls(g_cfg.game, g_module); }, nullptr});
            if (!look_slots(g_module).empty() && find_game(g_cfg.game))
            {
                item look{L"Armor and colors", L"", [] { g_pick_game = find_game(g_cfg.game); go(p_look, 0); }, nullptr};
                look.note = L"Changes show from the next game you start";
                items.push_back(look);
            }
            if (!(lan::g_role == lan::k_client && !lan::g_seats.empty()))
            {
                item eg{L"End game", L"", [] {
                            g_show_postgame = true, g_return_to_lobby = true;
                            relaunch(L"", 0, "", g_difficulty);
                        }, nullptr};
                eg.note = lan::g_seats.empty() ? L"Ends the match: see the results, then back to the lobby"
                                               : L"Ends the match for everyone: see the results, then back to the lobby";
                items.push_back(eg);
            }
            items.push_back({L"Return to main menu", L"", [] {
                                 g_show_postgame = false, g_return_to_lobby = false;
                                 relaunch(L"", 0, "", g_difficulty);
                             }, nullptr});
            items.push_back({quit_item(), L"", [] { go(p_quit, 1); }, nullptr});
            break;
        }
        if (!lan::g_seats.empty())
        {
            // LAN co-op: the host chooses the mission; nobody's saved game changes.
            bool host = lan::g_role == lan::k_host;
            if (host)
            {
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
                item d{L"Difficulty", difficulty_name(g_difficulty), [] { g_difficulty = (g_difficulty + 1) % 4; },
                       [](int dir) { g_difficulty = (g_difficulty + dir + 4) % 4; }};
                d.note = L"Takes effect when the mission restarts";
                items.push_back(d);
            }
            items.push_back({L"Settings", L"", [] { go(p_settings); }, nullptr});
            items.push_back({L"Controls", L"", [] { open_controls(g_cfg.game, g_module); }, nullptr});
            item back{host ? L"End co-op" : L"Leave co-op", L"", [] {
                           logf("menu: end co-op");
                           relaunch(L"", 0, "", g_difficulty);
                       }, nullptr};
            back.note = host ? L"Ends the game for everyone. Co-op starts from the mission's beginning next time"
                             : L"Leaves the game on this machine";
            items.push_back(back);
            items.push_back({quit_item(), L"", [] { go(p_quit, 1); }, nullptr});
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
        if (!look_slots(g_module).empty() && find_game(g_cfg.game))
        {
            item look{L"Armor and colors", L"", [] { g_pick_game = find_game(g_cfg.game); go(p_look, 0); }, nullptr};
            look.note = L"Changes show from the next game you start";
            items.push_back(look);
        }
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
        items.push_back({quit_item(), L"", [] { go(p_quit, 1); }, nullptr});
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
        items.push_back({L"Controls", L"", [] { go(p_set_controls); }, nullptr});
        if (g_launcher)
        {
            item bl{L"Button layout", L"", [] {
                        std::vector<std::pair<std::wstring, std::wstring>> list;
                        std::vector<const game_entry *> which;
                        for (const auto &g : games())
                            if (game_installed(g)) list.push_back({g.title, L""}), which.push_back(&g);
                        if (which.empty()) return;
                        open_picker(L"BUTTON LAYOUT", list, 0, [which](int i) { open_controls(which[i]->folder, which[i]->module); });
                    }, nullptr};
            bl.note = L"Change which button or key does what, for each game";
            items.push_back(bl);
        }
        items.push_back({L"Video", L"", [] { go(p_set_video); }, nullptr});
        items.push_back({L"Audio", L"", [] { go(p_set_audio); }, nullptr});
        items.push_back({L"Accessibility", L"", [] { go(p_set_access); }, nullptr});
        items.back().note = L"Most changes apply when this menu closes; a few, from the next game you start";
        if (g_launcher && I08_TEST_TOOLS)
        {
            item nt{L"Network test", lan::k_sim[lan::g_sim].name, nullptr, [](int d) {
                        lan::g_sim = (lan::g_sim + d + lan::k_sim_count) % lan::k_sim_count;
                        save_settings();
                    }};
            nt.note = L"For testing: LAN games on this machine get internet-like delay and lost packets, both ways. Set it on one machine only";
            items.push_back(nt);
        }
        break;
    case p_set_controls:
    case p_set_video:
    case p_set_audio:
    case p_set_access:
    {
        auto adj = [](int *v, int lo, int hi, int by) {
            return [v, lo, hi, by](int d) { step(*v, d, lo, hi, by); apply_settings(); };
        };
        opt_page which = g_page == p_set_controls ? op_controls : g_page == p_set_video ? op_video : g_page == p_set_audio ? op_audio : op_access;
        if (which == op_audio)
        {
            items.push_back({L"Master volume", pct(g_settings.master), nullptr, adj(&g_settings.master, 0, 100, 5)});
            items.push_back({L"Music volume", pct(g_settings.music), nullptr, adj(&g_settings.music, 0, 100, 5)});
            items.push_back({L"Effects volume", pct(g_settings.effects), nullptr, adj(&g_settings.effects, 0, 100, 5)});
        }
        if (which == op_controls)
        {
            items.push_back({L"Invert look", g_settings.invert_look ? L"On" : L"Off", nullptr,
                             [](int) { g_settings.invert_look ^= 1; apply_settings(); }});
        }
        if (which == op_video)
            items.push_back({L"Field of view", std::to_wstring(g_settings.fov), nullptr, adj(&g_settings.fov, 65, 120, 1)});
        for (const auto &o : k_options)
        {
            if (o.page != which) continue;
            float v = opt_value(o);
            std::wstring shown;
            if (!o.names.empty()) shown = o.names[std::max(0, std::min((int)o.names.size() - 1, (int)v))];
            else shown = std::to_wstring((int)std::lround(v)) + o.unit;
            const opt_def *od = &o;
            item i{o.label, shown, nullptr, [od](int d) {
                       float x = opt_value(*od) + d * od->step;
                       if (!od->names.empty() && od->hi - od->lo + 1 == (float)od->names.size())
                       {
                           // lists wrap around
                           int n = (int)od->names.size();
                           x = (float)((((int)x - (int)od->lo) % n + n) % n + (int)od->lo);
                       }
                       g_settings.opt[od->key] = std::max(od->lo, std::min(od->hi, x));
                       apply_settings();
                       save_settings();
                   }};
            i.note = o.note;
            items.push_back(i);
        }
        if (which == op_controls)
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
        if (which == op_video)
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
        items.push_back({L"Restore defaults", L"", [which] {
                             for (const auto &o : k_options)
                                 if (o.page == which) g_settings.opt.erase(o.key);
                             if (which == op_audio) g_settings.master = g_settings.music = g_settings.effects = 100;
                             if (which == op_controls) g_settings.invert_look = g_settings.invert_mouse = 0, g_settings.mouse = 1.0f;
                             if (which == op_video) g_settings.fov = 78;
                             apply_settings();
                             save_settings();
                         }, nullptr});
        for (auto &i : items)
            if (!i.activate && i.adjust) { auto a = i.adjust; i.activate = [a] { a(1); }; }
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
    if (g_page == p_joined) lan::leave();
    if (hosting_here(g_pick_game, g_page))
    {
        if (!lan::joined_names().empty())
        {
            g_confirm_title = L"CLOSE THE LAN GAME?";
            g_confirm_note = L"The players who joined go back to their menus";
            g_confirm_yes = [] { lan::host_close(); go_back_one(); };
            go(p_confirm, 1);
            return;
        }
        lan::host_close();
    }
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
    // Any connected controller works the menus.
    for (int slot = 0; slot < 4; slot++)
    {
        XINPUT_STATE xs = {};
        if (!g_xinput_get_state || g_xinput_get_state(slot, &xs) != ERROR_SUCCESS) continue;
        const XINPUT_GAMEPAD &p = xs.Gamepad;
        now[a_up] |= (p.wButtons & XINPUT_GAMEPAD_DPAD_UP) || p.sThumbLY > 16000;
        now[a_down] |= (p.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || p.sThumbLY < -16000;
        now[a_left] |= (p.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || p.sThumbLX < -16000;
        now[a_right] |= (p.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || p.sThumbLX > 16000;
        now[a_accept] |= (p.wButtons & XINPUT_GAMEPAD_A) != 0;
        now[a_back] |= (p.wButtons & XINPUT_GAMEPAD_B) != 0;
        now[a_close] |= (p.wButtons & XINPUT_GAMEPAD_START) != 0;
        now[a_delete] |= (p.wButtons & XINPUT_GAMEPAD_X) != 0;
    }
    bool focused = window_active();
    if (focused)
    {
        auto down = [](int vk) { return key_down(vk); };
        bool typing = g_page == p_text; // letters are typed, not moves
        now[a_up] |= down(VK_UP) || (down('W') && !typing);
        now[a_down] |= down(VK_DOWN) || (down('S') && !typing);
        now[a_left] |= down(VK_LEFT) || (down('A') && !typing);
        now[a_right] |= down(VK_RIGHT) || (down('D') && !typing);
        now[a_accept] |= down(VK_RETURN) || (down(VK_SPACE) && !typing);
        now[a_back] |= down(VK_BACK) && g_page != p_ip && !typing;
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
    // The splash: any button skips to its fade; nothing else happens until the menu shows.
    if (splash_blocks_input())
    {
        int64_t since = (int64_t)GetTickCount64() - g_splash_start;
        if ((fire[a_accept] || fire[a_back] || fire[a_close]) && since < k_splash_hold) g_splash_start -= k_splash_hold - since;
        return;
    }
    // X (Square): delete a letter while typing a name or tag.
    if (fire[a_delete] && g_page == p_text)
    {
        if (!g_text.empty()) g_text.pop_back();
        return;
    }

    std::vector<item> items = build_items();
    int n = (int)items.size();
    if (n == 0) { go_back(); return; }
    if (g_selected >= n) g_selected = n - 1;
    if (g_selected < 0) g_selected = 0;
    // The main page: game tiles in a row (left and right), the other items in a bar below.
    int tiles = 0;
    while (tiles < n && !items[tiles].tile.empty()) tiles++;
    if (tiles > 0 && tiles < n)
    {
        int bar = n - tiles;
        if (g_selected < tiles)
        {
            if (fire[a_left] && g_selected > 0) g_selected--;
            if (fire[a_right] && g_selected < tiles - 1) g_selected++;
            if (fire[a_down]) g_last_tile = g_selected, g_selected = tiles + std::min(g_last_bar, bar - 1);
        }
        else
        {
            int b = g_selected - tiles;
            if (fire[a_left]) b = (b + bar - 1) % bar;
            if (fire[a_right]) b = (b + 1) % bar;
            g_selected = tiles + b;
            if (fire[a_up]) g_last_bar = b, g_selected = std::min(g_last_tile, tiles - 1);
        }
        fire[a_up] = fire[a_down] = fire[a_left] = fire[a_right] = false;
    }
    // Up and down skip section headings, and lines that only show something (disabled, with
    // nothing to explain), unless the page has nothing else.
    auto skip = [&](int i) { return items[i].heading || (!items[i].enabled && items[i].note.empty()); };
    bool any = false;
    for (int i = 0; i < n; i++) any |= !skip(i);
    if (any)
    {
        for (int dir : {fire[a_up] ? -1 : 0, fire[a_down] ? 1 : 0})
            if (dir)
                for (int k = 0; k < n; k++)
                {
                    g_selected = (g_selected + dir + n) % n;
                    if (!skip(g_selected)) break;
                }
        for (int k = 0; k < n && skip(g_selected); k++) g_selected = (g_selected + 1) % n;
    }

    bool click = false, rclick = false; // (no mouse: controllers, and the keyboard as a fallback)
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
static IDWriteTextFormat *g_title_format, *g_item_format, *g_small_format, *g_value_format, *g_huge_format, *g_tile_format;
static IDWriteTextFormat *g_body_format, *g_small_right_format, *g_glyph_format, *g_tag_format;
static float g_formats_scale;

// The menus' font, Titillium Web, is inside the exe (resources 201-203), so the menus look
// the same on a PC and on the PS5, whose Wine has none of Windows' fonts. DirectWrite reads
// it straight from the exe's memory through a font file loader: nothing is written to disk.
// If that fails, the system's Bahnschrift (or Wine's substitute) is used.
static IDWriteFontCollection *g_font_collection;
static const wchar_t *g_font_family = L"Bahnschrift";
static bool font_resource(int id, const void **data, UINT64 *size)
{
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RT_RCDATA
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    *data = mem ? LockResource(mem) : nullptr;
    *size = res ? SizeofResource(nullptr, res) : 0;
    return *data && *size;
}
// One font file: a view of its resource.
struct font_stream final : IDWriteFontFileStream
{
    LONG refs = 1;
    const uint8_t *data = nullptr;
    UINT64 size = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteFontFileStream)) { *out = this; AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&refs);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE ReadFileFragment(const void **start, UINT64 offset, UINT64 bytes, void **context) override
    {
        *context = nullptr;
        if (offset > size || bytes > size - offset) { *start = nullptr; return E_FAIL; }
        *start = data + offset;
        return S_OK;
    }
    void STDMETHODCALLTYPE ReleaseFileFragment(void *) override {}
    HRESULT STDMETHODCALLTYPE GetFileSize(UINT64 *out) override { *out = size; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetLastWriteTime(UINT64 *out) override { *out = 0; return S_OK; }
};
// Makes a font file's stream from its key, the resource number.
struct font_file_loader final : IDWriteFontFileLoader
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteFontFileLoader)) { *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }  // static: never freed
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateStreamFromKey(const void *key, UINT32 key_size, IDWriteFontFileStream **out) override
    {
        *out = nullptr;
        if (key_size != sizeof(int)) return E_INVALIDARG;
        int id;
        memcpy(&id, key, sizeof id);
        const void *data;
        UINT64 size;
        if (!font_resource(id, &data, &size)) return E_FAIL;
        auto *st = new font_stream;
        st->data = (const uint8_t *)data, st->size = size;
        *out = st;
        return S_OK;
    }
};
static font_file_loader g_font_file_loader;
static const int k_font_ids[] = {201, 202, 203};
// The collection's files: one reference per resource.
struct font_files final : IDWriteFontFileEnumerator
{
    LONG refs = 1;
    IDWriteFactory *factory = nullptr;
    size_t at = 0;
    IDWriteFontFile *current = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteFontFileEnumerator)) { *out = this; AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&refs);
        if (!r)
        {
            if (current) current->Release();
            delete this;
        }
        return r;
    }
    HRESULT STDMETHODCALLTYPE MoveNext(BOOL *has) override
    {
        if (current) current->Release(), current = nullptr;
        *has = FALSE;
        if (at >= sizeof k_font_ids / sizeof k_font_ids[0]) return S_OK;
        int id = k_font_ids[at++];
        HRESULT hr = factory->CreateCustomFontFileReference(&id, sizeof id, &g_font_file_loader, &current);
        if (FAILED(hr)) return hr;
        *has = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentFontFile(IDWriteFontFile **out) override
    {
        if (!current) return E_FAIL;
        current->AddRef();
        *out = current;
        return S_OK;
    }
};
struct font_loader final : IDWriteFontCollectionLoader
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteFontCollectionLoader)) { *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }  // static: never freed
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateEnumeratorFromKey(IDWriteFactory *factory, const void *, UINT32,
                                                      IDWriteFontFileEnumerator **out) override
    {
        auto *e = new font_files;
        e->factory = factory;
        *out = e;
        return S_OK;
    }
};
static void load_menu_font()
{
    static bool tried;
    if (tried || !g_dwrite) return;
    tried = true;
    for (int id : k_font_ids)
    {
        const void *data;
        UINT64 size;
        if (!font_resource(id, &data, &size)) { logf("menu: font %d missing from the exe", id); return; }
    }
    static font_loader loader;
    HRESULT hr = g_dwrite->RegisterFontFileLoader(&g_font_file_loader);
    if (SUCCEEDED(hr)) hr = g_dwrite->RegisterFontCollectionLoader(&loader);
    if (FAILED(hr)) { logf("menu: font loader refused (%08lx)", (unsigned long)hr); return; }
    int key = 8;
    hr = g_dwrite->CreateCustomFontCollection(&loader, &key, sizeof key, &g_font_collection);
    if (FAILED(hr) || !g_font_collection) { logf("menu: font collection failed (%08lx)", (unsigned long)hr); g_font_collection = nullptr; return; }
    UINT32 index = 0;
    BOOL exists = FALSE;
    g_font_collection->FindFamilyName(L"Titillium Web", &index, &exists);
    if (!exists)
    {
        logf("menu: font collection has no Titillium Web");
        g_font_collection->Release();
        g_font_collection = nullptr;
        return;
    }
    g_font_family = L"Titillium Web";
    logf("menu: font Titillium Web");
}

static IDWriteTextFormat *make_format(const wchar_t *family, float size, DWRITE_FONT_WEIGHT weight,
                                      DWRITE_TEXT_ALIGNMENT align,
                                      DWRITE_PARAGRAPH_ALIGNMENT para = DWRITE_PARAGRAPH_ALIGNMENT_CENTER)
{
    IDWriteTextFormat *f = nullptr;
    g_dwrite->CreateTextFormat(family, g_font_collection, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                               L"en-us", &f);
    if (f)
    {
        f->SetTextAlignment(align);
        f->SetParagraphAlignment(para);
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
        for (auto f : {g_title_format, g_item_format, g_small_format, g_value_format, g_huge_format, g_tile_format,
                       g_body_format, g_small_right_format, g_glyph_format, g_tag_format})
            if (f) f->Release();
        load_menu_font();
        const wchar_t *fam = g_font_family;
        g_huge_format = make_format(fam, 88 * scale, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
        g_title_format = make_format(fam, 54 * scale, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
        g_item_format = make_format(fam, 30 * scale, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
        g_value_format = make_format(fam, 28 * scale, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
        g_small_format = make_format(fam, 22 * scale, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
        g_tile_format = make_format(fam, 36 * scale, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
                                    DWRITE_PARAGRAPH_ALIGNMENT_FAR);
        g_body_format = make_format(fam, 23 * scale, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
                                    DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        g_small_right_format = make_format(fam, 22 * scale, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
        g_glyph_format = make_format(fam, 19 * scale, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        g_tag_format = make_format(fam, 15 * scale, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        g_formats_scale = scale;
    }
    return g_title_format && g_item_format && g_small_format && g_value_format && g_huge_format && g_tile_format &&
           g_body_format && g_small_right_format && g_glyph_format && g_tag_format;
}

static void text(const std::wstring &s, IDWriteTextFormat *f, D2D1_RECT_F r, ID2D1Brush *b)
{
    g_rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, b, D2D1_DRAW_TEXT_OPTIONS_NONE);
}

static std::wstring upper(const std::wstring &t)
{
    std::wstring u = t;
    for (auto &c : u) c = towupper(c);
    return u;
}
static std::wstring page_title()
{
    switch (g_page)
    {
    case p_games: return L"";
    case p_campaign: return L"CAMPAIGN";
    case p_pick: return g_pick_title;
    case p_main: return L"PAUSED";
    case p_missions: return L"SELECT MISSION";
    case p_settings: return L"SETTINGS";
    case p_set_controls: return L"CONTROLS";
    case p_set_video: return L"VIDEO";
    case p_set_audio: return L"AUDIO";
    case p_set_access: return L"ACCESSIBILITY";
    case p_lobby: return upper(mode_name(g_lobby_mode));
    case p_customize: return L"CUSTOMIZE";
    case p_postgame: return g_pg_outcome;
    case p_quit: return quit_title();
    case p_confirm: return g_confirm_title;
    case p_controls: return L"CONTROLS";
    case p_firefight: return L"FIREFIGHT";
    case p_spops: return L"SPARTAN OPS";
    case p_custom: return L"CUSTOM GAME";
    case p_forge: return L"FORGE";
    case p_pad: return L"CONTROLLER";
    case p_keys: return L"KEYBOARD AND MOUSE";
    case p_lan: return L"BROWSE GAMES";
    case p_ip: return L"JOIN BY IP ADDRESS";
    case p_joined:
    {
        lan::lobby_msg m = lan::host_lobby();
        int mode = lan::g_welcomed && m.game[0] ? lan_mode_page(m) : -1;
        return mode >= 0 ? upper(mode_name(mode)) : L"JOINING";
    }
    case p_look: return L"ARMOR AND COLORS";
    case p_loadouts: return L"LOADOUTS";
    case p_loadout: return L"LOADOUT " + std::to_wstring(g_loadout_edit + 1);
    case p_text: return g_text_title;
    }
    return L"";
}

// A game's tile: two short lines of its name, and its colours (top, bottom).
struct tile_look { const wchar_t *folder, *name; float top[3], bottom[3]; };
static const tile_look &look_for(const std::wstring &folder)
{
    static const tile_look looks[] = {
        {L"halo1", L"HALO\nCOMBAT EVOLVED", {0.24f, 0.42f, 0.30f}, {0.04f, 0.09f, 0.07f}},
        {L"halo2", L"HALO 2", {0.20f, 0.33f, 0.56f}, {0.03f, 0.06f, 0.13f}},
        {L"groundhog", L"HALO 2\nANNIVERSARY", {0.36f, 0.42f, 0.52f}, {0.05f, 0.07f, 0.11f}},
        {L"halo3", L"HALO 3", {0.56f, 0.41f, 0.17f}, {0.11f, 0.07f, 0.03f}},
        {L"halo3odst", L"HALO 3\nODST", {0.38f, 0.40f, 0.45f}, {0.07f, 0.07f, 0.09f}},
        {L"haloreach", L"HALO\nREACH", {0.55f, 0.22f, 0.19f}, {0.11f, 0.04f, 0.04f}},
        {L"halo4", L"HALO 4", {0.16f, 0.45f, 0.52f}, {0.03f, 0.09f, 0.11f}},
    };
    static const tile_look other = {L"", L"", {0.3f, 0.32f, 0.36f}, {0.06f, 0.06f, 0.07f}};
    for (const auto &l : looks)
        if (!_wcsicmp(l.folder, folder.c_str())) return l;
    return other;
}

static void fill_gradient(D2D1_RECT_F r, D2D1_COLOR_F a, D2D1_COLOR_F b)
{
    ID2D1GradientStopCollection *stops = nullptr;
    D2D1_GRADIENT_STOP gs[2] = {{0.0f, a}, {1.0f, b}};
    g_rt->CreateGradientStopCollection(gs, 2, &stops);
    ID2D1LinearGradientBrush *br = nullptr;
    if (stops)
        g_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.left, r.bottom)),
                                        stops, &br);
    if (br) g_rt->FillRectangle(r, br), br->Release();
    if (stops) stops->Release();
}

// ---- The post-game screen: the match's result, from the record the game hands the host
// (host.set_game_result). Layout, the same in every game: 16 players at 0x20 (0x1C8 each:
// present, xuid +8, team +0x28, place +0x30 from 0, score +0x34) and their stats at
// 0x1CE4 (0x5448 each: present, won +8, kills +0x20, deaths +0x28, suicides +0x30,
// seconds played +0x38). The start and end times are at +8 and +0x10.
struct pg_player
{
    std::wstring name;
    int team = 0, place = 0, score = 0, kills = 0, deaths = 0, suicides = 0, seconds = 0;
    bool won = false, me = false;
};
struct postgame
{
    bool valid = false, teams = false, any_winner = false, i_won = false;
    std::wstring game, map, variant, outcome;
    int seconds = 0;
    std::vector<pg_player> players;
};
static postgame g_postgame;
static std::wstring g_pg_game, g_pg_map, g_pg_variant, g_pg_folder; // what the game was, noted when it started

// A player's name from their xuid: this machine's players, then the LAN game's seats in
// the lobby's order.
static std::wstring name_for_xuid(uint64_t xuid)
{
    // (the game is over, so the local player count is no longer set: try all four)
    for (int i = 0; i < 4; i++)
    {
        uint64_t x = i == 0 ? lan::g_my_xuid : 0x0009000000000001ull + (uint64_t)i;
        if (x == xuid && (lan::g_seats.empty() || i == 0))
            return i == 0 ? lan::widen(lan::g_name.empty() ? default_name() : lan::g_name) : L"Player " + std::to_wstring(i + 1);
    }
    std::vector<std::string> names;
    if (lan::g_role == lan::k_client)
    {
        lan::lobby_msg m = lan::host_lobby();
        for (int i = 0; i < m.count && i < 16; i++) names.push_back(lan::field(m.names[i], 32));
    }
    else if (lan::g_role == lan::k_host)
    {
        names.push_back(lan::g_name);
        for (const auto &n : lan::joined_names()) names.push_back(n);
    }
    for (size_t i = 0; i < lan::g_seats.size() && i < names.size(); i++)
        if (lan::g_seats[i].xuid == xuid) return lan::widen(names[i]);
    return L"Player";
}

static bool read_postgame()
{
    std::vector<uint8_t> r;
    {
        std::lock_guard<std::mutex> l(g_result_lock);
        r = g_result;
    }
    g_postgame = postgame();
    if (r.size() < k_result_size) return false;
    auto i32 = [&](size_t o) { int32_t v; memcpy(&v, r.data() + o, 4); return v; };
    auto u64 = [&](size_t o) { uint64_t v; memcpy(&v, r.data() + o, 8); return v; };
    postgame &pg = g_postgame;
    pg.game = g_pg_game, pg.map = g_pg_map, pg.variant = g_pg_variant;
    int64_t start = (int64_t)u64(8), end = (int64_t)u64(0x10);
    pg.seconds = end > start && end - start < 24 * 3600 ? (int)(end - start) : 0;
    uint64_t mine = lan::g_my_xuid;
    std::set<int> teams;
    for (int i = 0; i < 16; i++)
    {
        size_t id = 0x20 + (size_t)i * 0x1C8, st = 0x1CE4 + (size_t)i * 0x5448;
        if (!r[id]) continue;
        pg_player pl;
        uint64_t x = u64(id + 8);
        pl.name = name_for_xuid(x);
        pl.team = i32(id + 0x28), pl.place = i32(id + 0x30), pl.score = i32(id + 0x34);
        if (r[st])
            pl.won = i32(st + 8) != 0, pl.kills = i32(st + 0x20), pl.deaths = i32(st + 0x28), pl.suicides = i32(st + 0x30),
            pl.seconds = i32(st + 0x38);
        pl.me = x == mine;
        if (pl.won) pg.any_winner = true;
        if (pl.me && pl.won) pg.i_won = true;
        teams.insert(pl.team);
        pg.players.push_back(pl);
    }
    if (pg.players.empty()) return false;
    // A team game: some players share a team.
    pg.teams = teams.size() < pg.players.size();
    std::sort(pg.players.begin(), pg.players.end(), [&](const pg_player &a, const pg_player &b) {
        if (pg.teams && a.team != b.team) return a.team < b.team;
        if (a.place != b.place) return a.place < b.place;
        return a.score > b.score;
    });
    pg.outcome = !pg.any_winner ? L"GAME OVER" : pg.i_won ? L"VICTORY" : L"DEFEAT";
    g_pg_outcome = pg.outcome, g_pg_title = pg.game;
    pg.valid = true;
    logf("postgame: %zu players, %s, %s", pg.players.size(), pg.teams ? "teams" : "free for all", narrow(pg.outcome.c_str()).c_str());
    return true;
}

static const wchar_t *k_team_names[8] = {L"Red", L"Blue", L"Green", L"Orange", L"Purple", L"Gold", L"Brown", L"Pink"};
static const float k_team_colors[8][3] = {{0.80f, 0.20f, 0.18f}, {0.20f, 0.42f, 0.85f}, {0.25f, 0.65f, 0.25f},
                                          {0.92f, 0.52f, 0.12f}, {0.55f, 0.30f, 0.75f}, {0.85f, 0.70f, 0.15f},
                                          {0.50f, 0.34f, 0.20f}, {0.90f, 0.45f, 0.65f}};

static void draw_postgame(float s, float left, float right, float top, float bottom, float ease, ID2D1SolidColorBrush *white,
                          ID2D1SolidColorBrush *dim, ID2D1SolidColorBrush *faint, ID2D1SolidColorBrush *accent,
                          ID2D1SolidColorBrush *panel, ID2D1SolidColorBrush *line, ID2D1SolidColorBrush *green)
{
    const postgame &pg = g_postgame;
    float tw = std::min(right - left, 1180 * s);
    float x = left, y = top + 10 * s, row = 50 * s;
    // Columns: place, player, score, kills, deaths, K/D.
    float c_place = x + 18 * s, c_name = x + 90 * s, c_score = x + tw - 420 * s, c_kills = x + tw - 300 * s,
          c_deaths = x + tw - 190 * s, c_kd = x + tw - 80 * s;
    auto right_text = [&](const std::wstring &t, float cx, float yy, ID2D1Brush *b) {
        text(t, g_small_right_format, D2D1::RectF(cx - 140 * s, yy, cx + 40 * s, yy + row), b);
    };
    // Header row.
    g_rt->FillRectangle(D2D1::RectF(x, y, x + tw, y + 40 * s), panel);
    text(L"PLACE", g_small_format, D2D1::RectF(c_place, y, c_place + 120 * s, y + 40 * s), accent);
    text(L"PLAYER", g_small_format, D2D1::RectF(c_name, y, c_name + 300 * s, y + 40 * s), accent);
    for (auto &h : std::vector<std::pair<const wchar_t *, float>>{{L"SCORE", c_score}, {L"KILLS", c_kills}, {L"DEATHS", c_deaths}, {L"K/D", c_kd}})
        text(h.first, g_small_right_format, D2D1::RectF(h.second - 140 * s, y, h.second + 40 * s, y + 40 * s), accent);
    y += 48 * s;
    int last_team = -1000;
    for (size_t i = 0; i < pg.players.size() && y + row < bottom; i++)
    {
        const pg_player &pl = pg.players[i];
        if (pg.teams && pl.team != last_team)
        {
            last_team = pl.team;
            int t = std::max(0, std::min(7, pl.team));
            int total = 0;
            for (const auto &o : pg.players)
                if (o.team == pl.team) total += o.score;
            ID2D1SolidColorBrush *tc = nullptr;
            g_rt->CreateSolidColorBrush(D2D1::ColorF(k_team_colors[t][0], k_team_colors[t][1], k_team_colors[t][2], 0.9f * ease), &tc);
            if (tc)
            {
                g_rt->FillRectangle(D2D1::RectF(x, y + 6 * s, x + 6 * s, y + 40 * s), tc);
                tc->Release();
            }
            text(upper(k_team_names[t]) + L" TEAM", g_small_format, D2D1::RectF(c_place, y, c_name + 400 * s, y + 46 * s), white);
            right_text(std::to_wstring(total), c_score, y - 2 * s, white);
            y += 46 * s;
        }
        if (pl.me) g_rt->FillRectangle(D2D1::RectF(x, y, x + tw, y + row), panel);
        g_rt->FillRectangle(D2D1::RectF(x, y + row - 1 * s, x + tw, y + row), line);
        static const wchar_t *ord[] = {L"1st", L"2nd", L"3rd"};
        std::wstring place = pl.place >= 0 && pl.place < 3 ? ord[pl.place] : std::to_wstring(pl.place + 1) + L"th";
        ID2D1Brush *ink = pl.me ? (ID2D1Brush *)white : (ID2D1Brush *)dim;
        text(place, g_item_format, D2D1::RectF(c_place, y, c_place + 80 * s, y + row), pl.place == 0 ? (ID2D1Brush *)accent : ink);
        text(pl.name, g_item_format, D2D1::RectF(c_name, y, c_score - 150 * s, y + row), ink);
        right_text(std::to_wstring(pl.score), c_score, y, ink);
        right_text(std::to_wstring(pl.kills), c_kills, y, ink);
        right_text(std::to_wstring(pl.deaths), c_deaths, y, ink);
        wchar_t kd[16];
        swprintf(kd, 16, L"%.2f", pl.deaths ? (double)pl.kills / pl.deaths : (double)pl.kills);
        right_text(kd, c_kd, y, ink);
        y += row;
    }
    // The match, on the right.
    float px = x + tw + 60 * s;
    if (right - px > 300 * s)
    {
        g_rt->FillRectangle(D2D1::RectF(px, top + 10 * s, right, bottom), panel);
        float cy = top + 10 * s;
        const game_entry *g = find_game(g_pg_folder);
        if (g)
        {
            const tile_look &lk = look_for(g->folder);
            D2D1_RECT_F br = D2D1::RectF(px, cy, right, cy + 170 * s);
            fill_gradient(br, D2D1::ColorF(lk.top[0], lk.top[1], lk.top[2], ease), D2D1::ColorF(lk.bottom[0], lk.bottom[1], lk.bottom[2], ease));
            text(lk.name, g_tile_format, D2D1::RectF(px + 28 * s, br.top + 16 * s, right - 24 * s, br.bottom - 22 * s), white);
            g_rt->FillRectangle(D2D1::RectF(px, br.bottom - 3 * s, right, br.bottom), accent);
            cy = br.bottom;
        }
        cy += 20 * s;
        auto info = [&](const wchar_t *label, const std::wstring &value) {
            if (value.empty()) return;
            text(label, g_small_format, D2D1::RectF(px + 28 * s, cy, right - 28 * s, cy + 30 * s), accent);
            text(value, g_item_format, D2D1::RectF(px + 28 * s, cy + 28 * s, right - 28 * s, cy + 70 * s), white);
            cy += 80 * s;
        };
        info(L"GAME TYPE", pg.variant);
        info(L"MAP", pg.map);
        if (pg.seconds)
        {
            wchar_t t[16];
            swprintf(t, 16, L"%d:%02d", pg.seconds / 60, pg.seconds % 60);
            info(L"LENGTH", t);
        }
    }
}

// The panel on the right: the chosen game, who's in the lobby, what the selection does.
struct side_panel
{
    bool show = false;
    std::wstring banner, title, heading, body;
    std::vector<std::pair<std::wstring, std::wstring>> rows;
};
static side_panel build_panel(const std::vector<item> &items)
{
    side_panel sp;
    if (!g_launcher) return sp;
    const item *sel = g_selected >= 0 && g_selected < (int)items.size() ? &items[g_selected] : nullptr;
    switch (g_page)
    {
    case p_games:
        if (!sel) break;
        sp.show = true;
        sp.banner = sel->banner;
        sp.title = sel->label;
        sp.body = sel->note;
        if (sel->mode >= 0)
        {
            sp.heading = L"GAMES";
            for (const auto *g : games_with_mode((page_id)sel->mode)) sp.rows.push_back({lobby_title(g, sel->mode), L""});
        }
        break;
    case p_lobby:
    {
        if (!g_pick_game) break;
        sp.show = true;
        sp.banner = g_pick_game->folder;
        sp.title = lobby_title(g_pick_game, g_lobby_mode);
        std::string me = lan::g_name.empty() ? default_name() : lan::g_name;
        if (lan::g_role == lan::k_host)
        {
            auto players = lan::lobby_players();
            sp.rows.push_back({lan::widen(me), L"Host"});
            for (const auto &pl : players) sp.rows.push_back({lan::widen(pl.name), status_text(pl.status, g_pick_game->title)});
            sp.heading = L"PLAYERS  " + std::to_wstring(players.size() + 1);
            sp.body = players.empty() ? L"Waiting for players. On their main menu, they choose Browse games, then your name."
                                      : L"Everyone returns to this lobby after each game.";
        }
        else
        {
            int n = std::max(1, std::min(4, g_settings.players));
            refresh_pads();
            int pads = g_pad_count;
            // As the game will hand them out: player 1 has the keyboard and mouse (and a
            // controller too when there's one for everyone); the others take controllers in order.
            auto device = [&](int i) -> std::wstring { return i < pads ? L"Controller " + std::to_wstring(i + 1) : L"No controller"; };
            sp.rows.push_back({lan::widen(me), device(0)});
            for (int i = 1; i < n; i++) sp.rows.push_back({L"Player " + std::to_wstring(i + 1), device(i)});
            sp.heading = L"PLAYERS";
            sp.body = I08_ONLINE ? L"Local: just this machine. Set Lobby to LAN, Private or Public to let others join."
                                 : L"Local: just this machine. Set Lobby to LAN to let players on your network join.";
        }
        break;
    }
    case p_joined:
    {
        lan::lobby_msg m = lan::host_lobby();
        sp.show = true;
        std::wstring folder = lan::widen(lan::field(m.game, sizeof m.game));
        const game_entry *g = folder.empty() ? nullptr : find_game(folder);
        sp.banner = folder;
        sp.title = lobby_title(g, lan_mode_page(m));
        if (lan::g_welcomed)
        {
            sp.heading = L"PLAYERS  " + std::to_wstring(std::max(1, (int)m.count));
            for (int i = 0; i < m.count && i < 16; i++)
                sp.rows.push_back({lan::widen(lan::field(m.names[i], 32)), i == 0 ? L"Host" : status_text(m.status[i], g ? g->title : folder)});
            int mine = m.game[0] ? check_lobby(m.game, m.map_file) : 0;
            sp.body = m.playing ? L"The host is playing a game; you'll be in the next one."
                    : mine >= 2 ? L"You can't play this: " + status_text(mine, g ? g->title : folder) + L"."
                    : m.game[0] ? L"Waiting for the host to start." : L"The host is choosing what to play.";
        }
        else sp.body = L"Connecting to " + lan::widen(lan::g_join_text) + L"…";
        break;
    }
    case p_lan:
        sp.show = true;
        if (sel && !sel->banner.empty() && sel->roster.empty()) // a public lobby
        {
            sp.banner = sel->banner;
            sp.title = sel->label;
            sp.body = sel->note + L"\n\nA public lobby: anyone can join.";
        }
        else if (sel && !sel->roster.empty())
        {
            sp.banner = sel->banner;
            sp.title = sel->label;
            sp.heading = L"PLAYERS  " + std::to_wstring(sel->roster.size());
            sp.rows = sel->roster;
            sp.body = sel->note;
        }
        else if (lan::found().empty() && lan::public_found().empty())
            sp.body = I08_ONLINE ? L"No lobbies yet. A lobby shows up here when its host sets Lobby to LAN (players on the same network) or "
                                   L"Public (anyone). For a friend's Private lobby, use Join with a code. "
                                   L"Every machine needs the same version of Installation 08."
                                 : L"No lobbies yet. A lobby shows up here when its host, on the same network, sets Lobby to LAN. "
                                   L"Every machine needs the same version of Installation 08.";
        else if (sel) sp.body = sel->note;
        break;
    case p_customize:
        if (!sel || sel->banner.empty()) break;
        sp.show = true;
        sp.banner = sel->banner;
        sp.body = sel->note;
        break;
    default:
        break;
    }
    return sp;
}

// Button prompts: PlayStation symbols on the PS5 (or when chosen), Xbox letters otherwise.
static float text_width(const std::wstring &t, IDWriteTextFormat *f, float s)
{
    IDWriteTextLayout *layout = nullptr;
    float w = 0;
    g_dwrite->CreateTextLayout(t.c_str(), (UINT32)t.size(), f, 4000 * s, 200 * s, &layout);
    if (layout)
    {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(layout->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace;
        layout->Release();
    }
    return w;
}
// One button at (x, cy); returns its width.
static float draw_button(const wchar_t *xbox, float x, float cy, float s)
{
    bool ps = playstation_labels();
    auto brush = [](float r, float g, float b, float a = 1) {
        ID2D1SolidColorBrush *br = nullptr;
        g_rt->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), &br);
        return br;
    };
    bool start = !wcscmp(xbox, L"Start"), back = !wcscmp(xbox, L"Back");
    if (start || back)
    {
        const wchar_t *name = ps ? (start ? L"OPTIONS" : L"CREATE") : (start ? L"MENU" : L"VIEW");
        float w = text_width(name, g_tag_format, s) + 22 * s, hh = 26 * s;
        ID2D1SolidColorBrush *ring = brush(0.78f, 0.84f, 0.9f, 0.7f), *ink = brush(0.9f, 0.93f, 0.96f);
        if (ring && ink)
        {
            g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, cy - hh / 2, x + w, cy + hh / 2), hh / 2, hh / 2), ring, 1.5f * s);
            text(name, g_tag_format, D2D1::RectF(x, cy - hh / 2, x + w, cy + hh / 2), ink);
        }
        for (auto b : {ring, ink}) if (b) b->Release();
        return w;
    }
    float r = 15 * s, cx = x + r;
    // Each button's colour as printed on the controller.
    float col[3] = {0.85f, 0.88f, 0.92f};
    int which = !wcscmp(xbox, L"A") ? 0 : !wcscmp(xbox, L"B") ? 1 : !wcscmp(xbox, L"X") ? 2 : 3;
    static const float ps_col[4][3] = {{0.49f, 0.70f, 0.95f}, {1.0f, 0.42f, 0.42f}, {0.91f, 0.61f, 0.84f}, {0.31f, 0.82f, 0.69f}};
    static const float xb_col[4][3] = {{0.42f, 0.75f, 0.29f}, {0.90f, 0.33f, 0.29f}, {0.24f, 0.56f, 0.88f}, {0.95f, 0.76f, 0.19f}};
    for (int i = 0; i < 3; i++) col[i] = ps ? ps_col[which][i] : xb_col[which][i];
    ID2D1SolidColorBrush *mark = brush(col[0], col[1], col[2]), *ring = brush(0.78f, 0.84f, 0.9f, 0.55f);
    if (!mark || !ring)
    {
        for (auto b : {mark, ring}) if (b) b->Release();
        return 2 * r;
    }
    if (ps)
    {
        g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), ring, 1.5f * s);
        float k = r * 0.42f, t = 2.2f * s;
        switch (which)
        {
        case 0:
            g_rt->DrawLine(D2D1::Point2F(cx - k, cy - k), D2D1::Point2F(cx + k, cy + k), mark, t);
            g_rt->DrawLine(D2D1::Point2F(cx - k, cy + k), D2D1::Point2F(cx + k, cy - k), mark, t);
            break;
        case 1: g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), k * 1.05f, k * 1.05f), mark, t); break;
        case 2: g_rt->DrawRectangle(D2D1::RectF(cx - k * 0.9f, cy - k * 0.9f, cx + k * 0.9f, cy + k * 0.9f), mark, t); break;
        default:
        {
            ID2D1PathGeometry *tri = nullptr;
            ID2D1GeometrySink *sink = nullptr;
            if (SUCCEEDED(g_d2d->CreatePathGeometry(&tri)) && SUCCEEDED(tri->Open(&sink)))
            {
                sink->BeginFigure(D2D1::Point2F(cx, cy - k * 1.05f), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine(D2D1::Point2F(cx + k * 1.1f, cy + k * 0.8f));
                sink->AddLine(D2D1::Point2F(cx - k * 1.1f, cy + k * 0.8f));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                g_rt->DrawGeometry(tri, mark, t);
            }
            if (sink) sink->Release();
            if (tri) tri->Release();
        }
        }
    }
    else
    {
        g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), mark);
        ID2D1SolidColorBrush *ink = brush(0.04f, 0.06f, 0.08f);
        if (ink) text(xbox, g_glyph_format, D2D1::RectF(cx - r, cy - r - 1 * s, cx + r, cy + r - 1 * s), ink), ink->Release();
    }
    mark->Release(), ring->Release();
    return 2 * r;
}
// A small filled arrow (the font has no arrow characters): dir 0 left, 1 right, 2 up, 3 down.
static void draw_arrow(float cx, float cy, float r, int dir, ID2D1Brush *ink)
{
    ID2D1PathGeometry *geo = nullptr;
    ID2D1GeometrySink *sink = nullptr;
    if (SUCCEEDED(g_d2d->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sink)))
    {
        D2D1_POINT_2F a, b, c;
        switch (dir)
        {
        case 0: a = D2D1::Point2F(cx - r, cy), b = D2D1::Point2F(cx + r * 0.6f, cy - r), c = D2D1::Point2F(cx + r * 0.6f, cy + r); break;
        case 1: a = D2D1::Point2F(cx + r, cy), b = D2D1::Point2F(cx - r * 0.6f, cy - r), c = D2D1::Point2F(cx - r * 0.6f, cy + r); break;
        case 2: a = D2D1::Point2F(cx, cy - r), b = D2D1::Point2F(cx - r, cy + r * 0.6f), c = D2D1::Point2F(cx + r, cy + r * 0.6f); break;
        default: a = D2D1::Point2F(cx, cy + r), b = D2D1::Point2F(cx - r, cy - r * 0.6f), c = D2D1::Point2F(cx + r, cy - r * 0.6f); break;
        }
        sink->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(b);
        sink->AddLine(c);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        g_rt->FillGeometry(geo, ink);
    }
    if (sink) sink->Release();
    if (geo) geo->Release();
}

// A row of prompts: a button (or "dpad", or "" for words alone) and what it does.
static void draw_hints(const std::vector<std::pair<std::wstring, std::wstring>> &hints, float x, float cy, float s, ID2D1Brush *ink)
{
    for (const auto &h : hints)
    {
        if (h.first == L"dpad")
        {
            // The d-pad's left and right.
            draw_arrow(x + 8 * s, cy, 7 * s, 0, ink);
            draw_arrow(x + 30 * s, cy, 7 * s, 1, ink);
            x += 48 * s;
        }
        else if (!h.first.empty()) x += draw_button(h.first.c_str(), x, cy, s) + 10 * s;
        float w = text_width(h.second, g_small_format, s);
        text(h.second, g_small_format, D2D1::RectF(x, cy - 20 * s, x + w + 4 * s, cy + 20 * s), ink);
        x += w + 34 * s;
    }
}

static float g_hl_y = -1; // the selection's highlight, gliding to the selected row

static void draw(IDXGISwapChain *chain, bool opaque)
{
    DXGI_SWAP_CHAIN_DESC sd;
    if (FAILED(chain->GetDesc(&sd))) return;
    float w = (float)sd.BufferDesc.Width, h = (float)sd.BufferDesc.Height;
    float s = std::min(h / 1080.0f, w / 1600.0f); // fits narrow windows too
    if (!ensure_target(chain, s)) return;

    ID2D1SolidColorBrush *white, *dim, *faint, *accent, *accent_fill, *shade, *panel, *line, *green, *warn;
    g_rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.78f, 0.84f, 0.9f, 0.8f), &dim);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.78f, 0.84f, 0.9f, 0.45f), &faint);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.36f, 0.78f, 1.0f, 1), &accent);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.36f, 0.78f, 1.0f, 0.16f), &accent_fill);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.6f), &shade);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.02f, 0.06f, 0.1f, 0.6f), &panel);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.78f, 0.84f, 0.9f, 0.14f), &line);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.50f, 0.85f, 0.55f, 1), &green);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.72f, 0.38f, 1), &warn);

    g_rt->BeginDraw();
    float left = std::max(48 * s, std::min(140 * s, w * 0.07f));
    float right = w - left;
    float top = 120 * s;
    if (opaque || g_launcher)
    {
        // The backdrop: a dark blue gradient.
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
    }
    else g_rt->FillRectangle(D2D1::RectF(0, 0, w, h), shade);

    std::vector<item> items = build_items();
    int n = (int)items.size();
    side_panel sp = build_panel(items);

    // A new page fades and slides in; the selection's highlight glides between rows.
    static int last_page = -1;
    static bool last_launcher;
    static int64_t changed_at;
    int64_t tnow = now_ms();
    if ((int)g_page != last_page || last_launcher != g_launcher)
        last_page = (int)g_page, last_launcher = g_launcher, changed_at = tnow, g_hl_y = -1;
    float ft = std::min(1.0f, (float)(tnow - changed_at) / 220.0f);
    float ease = 1 - (1 - ft) * (1 - ft) * (1 - ft);
    float slide = (1 - ease) * 36 * s;
    auto fade = [&](float o) {
        for (auto b : {white, dim, faint, accent, accent_fill, panel, line, green, warn}) b->SetOpacity(o);
    };

    // Header: the name on the main page; elsewhere the page's title, and where it is.
    fade(ease);
    if (g_launcher && g_page == p_games)
    {
        text(L"INSTALLATION 08", g_title_format, D2D1::RectF(left, 52 * s, right, 130 * s), white);
        g_rt->FillRectangle(D2D1::RectF(left + 2 * s, 130 * s, left + 72 * s, 133 * s), accent);
        text(lan::status().empty() ? L"The Master Chief Collection, offline" : lan::status().substr(5), g_small_format,
             D2D1::RectF(left + 2 * s, 140 * s, right, 172 * s), faint);
        top = 200 * s;
    }
    else
    {
        std::wstring sub;
        if (g_launcher)
        {
            if (g_page == p_lobby) sub = lan::g_role != lan::k_host ? L"Local lobby" : !lan::g_online ? L"LAN lobby" : lan::g_public ? L"Public lobby" : L"Private lobby";
            else if (g_page == p_postgame) sub = g_pg_title;
            else if (g_page == p_joined)
                sub = lan::g_join_text.empty() ? L"" : L"Hosted by " + lan::widen(lan::g_join_text);
            else if ((g_page == p_look || g_page == p_loadouts || g_page == p_loadout || g_page == p_controls ||
                      g_page == p_pad || g_page == p_keys || g_page == p_missions) && g_pick_game)
                sub = g_pick_game->title;
        }
        else
        {
            const mission_entry *m = current_mission();
            const game_entry *g = find_game(g_cfg.game);
            sub = (g ? std::wstring(g->title) : L"") + (m && g_mode == 1 ? L"  •  " + std::wstring(m->name) : L"") +
                  (g_mode != 3 ? L"  •  " + std::wstring(difficulty_name(g_cfg.difficulty)) : L"");
        }
        text(sub, g_small_format, D2D1::RectF(left + slide, 60 * s, right, 92 * s), faint);
        text(page_title(), g_title_format, D2D1::RectF(left + slide, 88 * s, right, 160 * s), white);
        top = 190 * s;
    }

    // Footer: what the selected item does, then the buttons.
    fade(1);
    float foot = h - 110 * s;
    g_rt->FillRectangle(D2D1::RectF(left, foot, right, foot + 1.5f * s), line);
    std::vector<std::pair<std::wstring, std::wstring>> hints;
    if (g_listen_kind == listen_pad) hints = {{L"", L"Press the button to use"}, {L"Start", L"Cancel"}};
    else if (g_listen_kind == listen_key) hints = {{L"", L"Press the key or mouse button to use"}, {L"B", L"Cancel (or Esc)"}};
    else
    {
        hints = g_page == p_postgame ? std::vector<std::pair<std::wstring, std::wstring>>{{L"A", L"Continue"}}
                                     : std::vector<std::pair<std::wstring, std::wstring>>{{L"A", L"Select"}, {L"B", L"Back"}};
        if (g_page == p_text) hints.push_back({L"X", L"Delete"});
        if (g_selected < n && items[g_selected].adjust) hints.push_back({L"dpad", L"Change"});
        if (!g_launcher) hints.push_back({L"Start", L"Resume"});
    }
    draw_hints(hints, left, foot + 42 * s, s, dim);
    bool panel_room = sp.show && right - left > 1000 * s;
    bool note_in_panel = (panel_room && (g_page == p_games || g_page == p_lan || g_page == p_customize)) || g_page == p_postgame;
    if (g_selected < n && !note_in_panel)
        text(items[g_selected].note, g_small_format, D2D1::RectF(left, foot - 50 * s, right, foot - 12 * s), dim);

    // A list: one line per item, a value on the right; scrolls when long.
    fade(ease);
    for (auto &r : g_item_rects) r = D2D1::RectF(-1, -1, -1, -1);
    if (g_page == p_postgame && g_launcher)
    {
        draw_postgame(s, left + slide, right, top, foot - 40 * s, ease, white, dim, faint, accent, panel, line, green);
        g_item_count = 0;
    }
    else
    {
        float col = panel_room ? std::min((right - left) * 0.5f, 820 * s) : std::min(right - left, 900 * s);
        float lx = left + slide;
        float row = 58 * s, gap = 4 * s;
        float y0 = top + 10 * s, bottom = foot - 64 * s;
        auto height = [&](int i) { return items[i].heading ? 46 * s : row; };
        if (g_selected < g_scroll) g_scroll = g_selected;
        // (headings above the selection come into view with it)
        while (g_scroll > 0 && items[g_scroll - 1].heading && g_scroll - 1 <= g_selected) g_scroll--;
        for (;;)
        {
            float used = 0;
            for (int i = g_scroll; i <= g_selected && i < n; i++) used += height(i) + gap;
            if (used <= bottom - y0 || g_scroll >= g_selected) break;
            g_scroll++;
        }
        // Lay the rows out, then draw the highlight under them, then the rows.
        struct placed { int i; D2D1_RECT_F r; };
        std::vector<placed> rows;
        g_item_count = 0;
        float y = y0;
        int last = g_scroll;
        for (int i = g_scroll; i < n; i++)
        {
            float rh = height(i);
            if (y + rh > bottom) break;
            last = i;
            rows.push_back({i, D2D1::RectF(lx, y, lx + col, y + rh)});
            if (g_item_count < 32) g_item_rects[g_item_count++] = D2D1::RectF(left, y, left + col, y + rh);
            y += rh + gap;
        }
        for (const auto &pr : rows)
            if (pr.i == g_selected && !items[pr.i].heading)
            {
                float target = pr.r.top;
                if (g_hl_y < 0 || std::fabs(target - g_hl_y) < 0.5f) g_hl_y = target;
                else g_hl_y += (target - g_hl_y) * 0.35f;
                D2D1_RECT_F hl = D2D1::RectF(lx, g_hl_y, lx + col, g_hl_y + row);
                g_rt->FillRectangle(hl, accent_fill);
                g_rt->FillRectangle(D2D1::RectF(lx, hl.top, lx + 4 * s, hl.bottom), accent);
            }
        for (const auto &pr : rows)
        {
            int i = pr.i;
            D2D1_RECT_F r = pr.r;
            if (items[i].heading)
            {
                text(upper(items[i].label), g_small_format, D2D1::RectF(r.left + 26 * s, r.top + 12 * s, r.right, r.bottom), accent);
                continue;
            }
            bool sel = i == g_selected;
            ID2D1Brush *label_brush = !items[i].enabled ? (ID2D1Brush *)faint : sel ? (ID2D1Brush *)white : (ID2D1Brush *)dim;
            text(items[i].label, g_item_format, D2D1::RectF(r.left + 26 * s, r.top, r.right - 20 * s, r.bottom), label_brush);
            if (!items[i].value.empty())
            {
                bool arrows = sel && items[i].adjust;
                float vr = r.right - (arrows ? 46 * s : 24 * s);
                text(items[i].value, g_value_format, D2D1::RectF(r.left + 26 * s, r.top, vr, r.bottom),
                     sel ? (ID2D1Brush *)accent : (ID2D1Brush *)dim);
                if (arrows)
                {
                    float vw = std::min(text_width(items[i].value, g_value_format, s), vr - r.left - 60 * s);
                    float my = (r.top + r.bottom) / 2;
                    draw_arrow(vr - vw - 18 * s, my, 7 * s, 0, accent);
                    draw_arrow(r.right - 26 * s, my, 7 * s, 1, accent);
                }
            }
        }
        if (g_scroll > 0) draw_arrow(lx + col + 24 * s, y0 + 16 * s, 8 * s, 2, dim);
        if (last < n - 1) draw_arrow(lx + col + 24 * s, y - 18 * s, 8 * s, 3, dim);

        // The side panel.
        if (panel_room)
        {
            float px = left + col + 80 * s + slide, pr = right + slide * 0.5f, py = y0, pb = bottom;
            g_rt->FillRectangle(D2D1::RectF(px, py, pr, pb), panel);
            float cy = py;
            if (!sp.banner.empty())
            {
                const tile_look &lk = look_for(sp.banner);
                D2D1_RECT_F br = D2D1::RectF(px, py, pr, py + 190 * s);
                fill_gradient(br, D2D1::ColorF(lk.top[0], lk.top[1], lk.top[2], ease),
                              D2D1::ColorF(lk.bottom[0], lk.bottom[1], lk.bottom[2], ease));
                text(lk.name, g_tile_format, D2D1::RectF(px + 28 * s, br.top + 16 * s, pr - 24 * s, br.bottom - 22 * s), white);
                g_rt->FillRectangle(D2D1::RectF(px, br.bottom - 3 * s, pr, br.bottom), accent);
                cy = br.bottom;
            }
            cy += 22 * s;
            float tx = px + 28 * s, tr = pr - 28 * s;
            if (!sp.title.empty())
            {
                text(sp.title, g_item_format, D2D1::RectF(tx, cy, tr, cy + 44 * s), white);
                cy += 54 * s;
            }
            if (!sp.heading.empty())
            {
                text(sp.heading, g_small_format, D2D1::RectF(tx, cy, tr, cy + 30 * s), accent);
                cy += 38 * s;
            }
            for (const auto &rw : sp.rows)
            {
                if (cy + 42 * s > pb - 70 * s) break;
                g_rt->FillRectangle(D2D1::RectF(tx, cy, tr, cy + 1 * s), line);
                text(rw.first, g_small_format, D2D1::RectF(tx, cy, tr - 160 * s, cy + 42 * s), white);
                ID2D1Brush *st = rw.second == L"Ready" ? (ID2D1Brush *)green
                               : rw.second.rfind(L"Doesn't", 0) == 0 ? (ID2D1Brush *)warn : (ID2D1Brush *)faint;
                text(rw.second, g_small_right_format, D2D1::RectF(tx + 120 * s, cy, tr, cy + 42 * s), st);
                cy += 42 * s;
            }
            if (!sp.body.empty() && cy < pb - 40 * s)
                text(sp.body, g_body_format, D2D1::RectF(tx, cy + (sp.rows.empty() ? 0 : 18 * s), tr, pb - 20 * s), dim);
        }

        // Objectives, as MCC shows them on its pause screen.
        std::wstring primary, secondary;
        {
            std::lock_guard<std::mutex> l(g_mission_lock);
            primary = g_objective_primary, secondary = g_objective_secondary;
        }
        if (!g_launcher && g_page == p_main && (!primary.empty() || !secondary.empty()))
        {
            float ox = left + col + 100 * s, ow = right - ox;
            if (ow > 300 * s)
            {
                g_rt->FillRectangle(D2D1::RectF(ox, y0, ox + ow, y0 + 320 * s), panel);
                text(L"OBJECTIVES", g_small_format, D2D1::RectF(ox + 30 * s, y0 + 15 * s, ox + ow, y0 + 55 * s), accent);
                text(primary + (secondary.empty() ? L"" : L"\n\n" + secondary), g_body_format,
                     D2D1::RectF(ox + 30 * s, y0 + 65 * s, ox + ow - 30 * s, y0 + 300 * s), white);
            }
        }
    }
    fade(1);

    // The player's name, top right, in the launcher (once it's been asked).
    if (g_launcher && g_page != p_text && g_settings.name_asked && !lan::g_name.empty())
    {
        text(lan::widen(lan::g_name), g_value_format, D2D1::RectF(left, 66 * s, right, 110 * s), white);
        if (!g_settings.service_tag.empty())
        {
            g_small_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            text(lan::widen(g_settings.service_tag), g_small_format, D2D1::RectF(left, 106 * s, right, 136 * s), faint);
            g_small_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }

    // The splash: the name alone, then it fades out and the menu fades in.
    if (g_launcher && g_splash_start)
    {
        int64_t t = (int64_t)GetTickCount64() - g_splash_start;
        if (t >= k_splash_hold + k_splash_out + k_splash_in) g_splash_start = 0;
        else
        {
            float cover = t < k_splash_hold + k_splash_out ? 1.0f : 1.0f - (float)(t - k_splash_hold - k_splash_out) / k_splash_in;
            float a = t < 400 ? t / 400.0f : t < k_splash_hold ? 1.0f : t < k_splash_hold + k_splash_out ? 1.0f - (float)(t - k_splash_hold) / k_splash_out : 0.0f;
            ID2D1SolidColorBrush *black = nullptr, *ink = nullptr, *bar = nullptr;
            g_rt->CreateSolidColorBrush(D2D1::ColorF(0.01f, 0.02f, 0.04f, cover), &black);
            g_rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, a), &ink);
            g_rt->CreateSolidColorBrush(D2D1::ColorF(0.36f, 0.78f, 1.0f, a), &bar);
            if (black) g_rt->FillRectangle(D2D1::RectF(0, 0, w, h), black);
            if (a > 0 && ink && bar)
            {
                float cy = h / 2;
                // The accent line grows out from the middle as the name appears.
                float grow = std::min(1.0f, t / 700.0f);
                float half = 220 * s * (1 - (1 - grow) * (1 - grow));
                g_huge_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                text(L"INSTALLATION 08", g_huge_format, D2D1::RectF(0, cy - 110 * s, w, cy - 10 * s), ink);
                g_rt->FillRectangle(D2D1::RectF(w / 2 - half, cy + 6 * s, w / 2 + half, cy + 9 * s), bar);
                g_huge_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
            for (auto b : {black, ink, bar})
                if (b) b->Release();
        }
    }

    HRESULT hr = g_rt->EndDraw();
    for (auto b : {white, dim, faint, accent, accent_fill, shade, panel, line, green, warn}) b->Release();
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
    if (!g_loading_screen) return false;
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

// Split-screen view values the games ask for (MCC-Win64-Shipping.exe host functions
// #84-#86 return these constants; a generic stub would leave the float register as junk).
static float h_splitscreen_84(void *, int n)
{
    static const float v[4] = {-0.38f, -0.58f, -0.79f, -1.0f};
    return n >= 0 && n < 4 ? v[n] : 0.0f;
}
static float h_splitscreen_85(void *, int n)
{
    static const float v[4] = {0.76666665f, 0.6666667f, 0.5882353f, 0.5f};
    return n >= 0 && n < 4 ? v[n] : 1.0f;
}
static float h_splitscreen_86(void *) { return 0.2f; }

static void build_host_vtable()
{
    fill_stubs<k_tag_host>(g_host_vtable, std::make_index_sequence<512>{});
    fill_stubs<k_tag_event>(g_event_vtable, std::make_index_sequence<512>{});
    g_event_manager.vtable = g_event_vtable;
    g_event_vtable[147] = (void *)&e_get_guid;
    void **v = g_host_vtable;
    v[0] = (void *)&h_begin_frame;
    v[84] = (void *)&h_splitscreen_84;
    v[85] = (void *)&h_splitscreen_85;
    v[86] = (void *)&h_splitscreen_86;
    v[1] = (void *)&h_end_frame;
    v[3] = (void *)&h_set_game_state;
    v[4] = (void *)&h_restart_game;
    v[41] = (void *)&h_network_sendto_unreliable;
    v[42] = (void *)&h_network_sendto_reliable;
    v[43] = (void *)&h_network_recvfrom;
    v[5] = (void *)&h_save_game;
    v[6] = (void *)&h_set_game_result;
    v[12] = (void *)&h_set_scenario_map_variant;
    v[14] = (void *)&h_set_player_look_control;
    v[15] = (void *)&h_set_player_profile_game_specific;
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
static DWORD g_main_thread;
static std::wstring g_lan_arg; // --lan host | --lan <host address>
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
    logf("  thread %lu%s", GetCurrentThreadId(), GetCurrentThreadId() == g_main_thread ? " (Installation 08's main thread)" : "");
    // Code addresses on the stack: the callers, among other things. Large frames (DXVK's)
    // push the callers far down, so look a long way.
    uint64_t *stack = reinterpret_cast<uint64_t *>(c->Rsp);
    int shown = 0;
    for (int i = 0; i < 2048 && shown < 40; i++)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (IsBadReadPtr(&stack[i], 8)) break;
        if (!VirtualQuery((void *)stack[i], &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE || !(mbi.Protect & 0xF0)) continue;
        logf("  stack[%d] %s", i, module_offset((void *)stack[i]).c_str());
        shown++;
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
    case WM_CHAR:
        menu::on_char((wchar_t)wp);
        return 0;
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

static HMODULE g_pending_unload;
static std::wstring g_pending_unload_dll;
static bool finish_unload();
static next_action run_launcher()
{
    logf("launcher, MCC at %s", narrow(g_cfg.mcc_root.c_str()).c_str());
    SetWindowTextW(g_hwnd, L"Installation 08");
    g_cfg.game.clear();
    g_next = next_action();
    // --show-result <results\\game-....bin>: the post-game screen for a saved record.
    if (!g_show_result_path.empty())
    {
        std::string data = read_file(g_show_result_path);
        std::wstring base = g_show_result_path.substr(g_show_result_path.find_last_of(L"\\/") + 1);
        std::wstring folder = base.substr(0, base.find(L'-'));
        const game_entry *e = find_game(folder);
        if (data.size() >= k_result_size && e)
        {
            {
                std::lock_guard<std::mutex> l(g_result_lock);
                g_result.assign(data.begin(), data.begin() + k_result_size);
            }
            menu::g_pg_folder = folder, menu::g_pg_game = menu::lobby_title(e, menu::p_custom);
            menu::g_pg_map = L"(saved record)", menu::g_pg_variant = base;
            menu::g_lobby_mode = menu::p_custom;
            menu::pick_lobby_game(e);
            menu::g_lobby_mode = menu::p_custom;
            g_show_postgame = true, g_return_to_lobby = true;
        }
        else logf("--show-result: can't use %s", narrow(g_show_result_path.c_str()).c_str());
        g_show_result_path.clear();
    }
    menu::start_launcher();
    int64_t launcher_since = (int64_t)GetTickCount64();
    int frames = 0;
    while (g_next.kind == next_action::k_none)
    {
        pump_messages();
        lan::poll(false);
        if (g_pending_unload && ++frames > 120 && (int64_t)GetTickCount64() - launcher_since > 2000 && !finish_unload())
            g_next.kind = next_action::k_quit;
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

// A finished game's DLL stays loaded while the menu draws for a while: the PS5's
// graphics layer (DXVK) can release objects the game attached to its buffers several
// frames later, and if the game's code is gone by then, that crashes (seen after a
// three-machine H2A game, on the menu's first frame). Unloaded after the menu has drawn
// about two seconds, or before the next game loads, whichever comes first.
static bool finish_unload()
{
    if (!g_pending_unload) return true;
    HMODULE module = g_pending_unload;
    g_pending_unload = nullptr;
    if (g_context)
    {
        g_context->ClearState();
        g_context->Flush();
        D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
        ID3D11Query *done = nullptr;
        if (g_device && SUCCEEDED(g_device->CreateQuery(&qd, &done)))
        {
            g_context->End(done);
            g_context->Flush();
            BOOL finished = FALSE;
            for (int i = 0; i < 400 && g_context->GetData(done, &finished, sizeof(finished), 0) != S_OK; i++) Sleep(5);
            done->Release();
        }
    }
    HANDLE unloader = CreateThread(nullptr, 0, free_library_thread, module, 0, nullptr);
    if (WaitForSingleObject(unloader, 15000) != WAIT_OBJECT_0)
    {
        logf("unloading the game did not finish; closing (start Installation 08 again to continue)");
        ask_to_be_restarted();
        return false;
    }
    CloseHandle(unloader);
    HMODULE still = GetModuleHandleW(g_pending_unload_dll.c_str());
    logf("game unloaded%s", still ? " (but the DLL is still loaded)" : "");
    if (still)
    {
        // Something else holds it: a second LoadLibrary would get the used copy.
        ask_to_be_restarted();
        return false;
    }
    return true;
}

static next_action run_game(const next_action &what, ID3D11Device *device, ID3D11DeviceContext *context)
{
    if (!finish_unload())
    {
        next_action quit;
        quit.kind = next_action::k_quit;
        return quit;
    }
    g_cfg.game = what.game;
    g_cfg.map = what.map;
    g_cfg.map_file = std::wstring(what.file.begin(), what.file.end());
    g_cfg.difficulty = what.difficulty;
    g_mode = what.mode, g_mode_for_saves = what.mode;
    g_kbm_player = g_settings.kbm_player;
    net_reset();
    g_ff_variant = what.variant;
    {
        std::lock_guard<std::mutex> l(g_forge_lock);
        g_map_variant_path = what.map_variant;
        g_forge_status.clear();
    }
    g_forge_save = 0;
    {
        const game_entry *e = find_game(what.game);
        if (e) g_module = e->module;
    }
    {
        const game_entry *e = find_game(what.game);
        int mode = what.mode == 3 ? ((forge_type(g_module) && what.variant.find(forge_type(g_module)) != std::string::npos) ? menu::p_forge : menu::p_custom)
                 : what.mode == 2 ? menu::p_spops : what.mode == 5 ? menu::p_firefight : menu::p_campaign;
        menu::g_pg_game = e ? menu::lobby_title(e, mode) : what.game;
        menu::g_pg_folder = what.game;
        menu::g_pg_map.clear();
        if (e)
            for (const auto &m : what.mode == 5 ? e->firefight : custom_maps(e->module))
                if (m.map == what.map) menu::g_pg_map = m.name;
        std::string v = what.variant.substr(what.variant.find_last_of('\\') + 1);
        menu::g_pg_variant = v.empty() ? L"" : menu::pretty_variant_name(v);
        for (const auto &t : custom_types(g_module))
            if (!_stricmp(t.file, v.c_str())) menu::g_pg_variant = t.name;
    }
    lan::host_game_starting(what);
    lan::set_playing(true);
    const game_entry *entry = find_game(g_cfg.game);
    if (entry)
    {
        g_module = entry->module;
        if (g_cfg.map < 0)
        {
            // Default: the first playable mission (Reach's first, Noble Actual, is a cutscene).
            if (!entry->missions.empty())
            {
                const mission_entry &m = entry->missions[g_module == 6 && entry->missions.size() > 1 ? 1 : 0];
                g_cfg.map = m.map;
                g_cfg.map_file = std::wstring(m.file, m.file + strlen(m.file));
            }
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
    lan::g_traffic_cut = false;
    g_show_postgame = false, g_return_to_lobby = false;
    {
        std::lock_guard<std::mutex> l(g_result_lock);
        g_result.clear();
    }
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
    apply_look(narrow(g_cfg.game.c_str()), g_module);
    load_game_data(g_cfg.game);
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
    int h1_classic = what.classic >= 0 ? what.classic : g_settings.h1_classic;
    int h2_classic = what.classic >= 0 ? what.classic : g_settings.h2_classic;
    if (g_module == 1 && !h2_classic && g_mode == 1 && !split) options.flags |= 1;
    use_controls(narrow(g_cfg.game.c_str()), g_module);
    // Halo CE reads the same two bits into its config: bit 0 clear sets CONFIG.LegacyMode
    // (classic graphics), bit 1 clear sets CONFIG.LegacySounds (classic sound). MCC's
    // default is the remastered sound, so that bit is always on.
    if (g_module == 0) options.flags |= 2 | (h1_classic || g_mode != 1 ? 0 : 1);
    options.game_tick = (char)g_cfg.tick;
    // A client in a synchronous game (everything but Halo CE in campaign, Spartan Ops and
    // Firefight) smooths the host's updates (smooth:: above).
    {
        bool on = lan::g_role == lan::k_client && !lan::g_seats.empty() && g_module != 0 && (g_mode == 1 || g_mode == 2 || g_mode == 5);
        g_smooth = false;
        {
            std::lock_guard<std::mutex> tl(g_timing_lock);
            smooth::reset();
            smooth::reset_stats();
        }
        g_smooth = on;
        if (on) logf("lan co-op: smoothing the host's updates");
    }
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
    logf("local players: %d", (int)g_local_players);
    players.peer_count = 1;
    if (g_cfg.address)
    {
        options.host_address = (int64_t)g_cfg.address;
        options.un_3 = (int64_t)g_cfg.address;
        players.address[0] = g_cfg.address;
        for (int i = 0; i < g_local_players; i++) players.player_options[i].address = g_cfg.address;
    }
    // LAN play: everyone's machine and player, host first, the same on every machine;
    // only the host has flag 8. un_3 is the server to join: Halo CE's client reads it
    // when flag 8 is clear (halo1.dll+0x93c71) and sends its join requests there.
    if (!lan::g_seats.empty())
    {
        int n = (int)std::min<size_t>(lan::g_seats.size(), 16);
        players.player_count = n;
        players.peer_count = n;
        for (int i = 0; i < n; i++)
        {
            players.player_options[i] = {lan::g_seats[i].xuid, lan::g_seats[i].address, -1, 0, -1, -1};
            players.address[i] = lan::g_seats[i].address;
        }
        if (g_module == 0)
        {
            // Halo CE (tested this way): both fields hold the server's address.
            options.host_address = (int64_t)lan::g_seats[0].address;
            options.un_3 = (int64_t)lan::g_seats[0].address;
        }
        else
        {
            // As MCC fills them in (MCC-Win64-Shipping.exe+0x46be1f..0x46c25d):
            //  +0x50  the session's id, the same on every machine
            //  +0x58  this machine's own address (Halo 3 takes it as its network identity)
            //  +0x2f8 the host's address, on the other machines only
            //  each player's unknown_4 is its machine's index in the address list, and the
            //  list's unknown_2 is this machine's index.
            int me = 0;
            for (int i = 0; i < n; i++)
            {
                players.player_options[i].unknown_4 = i;
                if (lan::g_seats[i].address == lan::g_my_id) me = i;
            }
            players.unknown_2 = me;
            options.un_2 = (int64_t)(lan::g_seats[0].address ^ 0x4930384C414E0001ull);
            options.host_address = (int64_t)lan::g_my_id;
            options.un_3 = lan::g_role == lan::k_host ? 0 : (int64_t)lan::g_seats[0].address;
        }
        if (lan::g_role == lan::k_host) options.flags |= 8;
        else options.flags &= ~8;
        g_local_players = 1;
        std::string list;
        for (int i = 0; i < n; i++)
            list += (i ? ", " : "") + std::to_string(i) + ": xuid " + std::to_string(lan::g_seats[i].xuid & 0xFFFF) + " at " +
                    std::to_string(lan::g_seats[i].address & 0xFFFF);
        logf("lan: %s a game for %d machines (%s), flags %x", lan::g_role == lan::k_host ? "hosting" : "joining", n, list.c_str(),
             (unsigned)(uint16_t)options.flags);
    }
    if (lan::g_seats.empty() || g_module == 0) players.unknown_2 = -1;

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
        i_saved_game_file *map = nullptr;
        // A saved Forge map, or the map as it comes.
        if (data && !what.map_variant.empty())
        {
            std::string bytes = read_file(what.map_variant);
            if (!bytes.empty()) map = data->map_variant_from_file(bytes.data(), (uint32_t)bytes.size());
            const int32_t *id = map ? static_cast<i_scenario_map_variant *>(map)->get_map_id() : nullptr;
            logf("map variant %s (%zu bytes) -> %p, its map %d (playing on %d)", narrow(what.map_variant.c_str()).c_str(),
                 bytes.size(), (void *)map, id ? *id : -1, g_cfg.map);
            if (!map)
            {
                logf("the saved map could not be read; playing the map as it comes");
                std::lock_guard<std::mutex> l(g_forge_lock);
                g_map_variant_path.clear();
            }
        }
        if (!map && data) map = data->map_variant_from_map_id(&options.map_id);
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

    // LAN co-op (campaign): a networked game checks the map variant in the options,
    // which a solo campaign never fills. The map's own variant, as for Firefight.
    if (g_mode == 1 && !lan::g_seats.empty())
    {
        auto create_data_access = (t_create_data_access)GetProcAddress(module, "CreateDataAccess");
        i_data_access *data = nullptr;
        if (create_data_access) create_data_access(&data);
        i_saved_game_file *map = data ? data->map_variant_from_map_id(&options.map_id) : nullptr;
        logf("lan co-op: map variant for map %d -> %p", g_cfg.map, (void *)map);
        if (map) map->copy_to_game_options(&options);
    }

    // Continuing: the saved state goes back to the game the way MCC passes it, its
    // size just before the pointer (Reach checks for exactly its 10.9 MB state).
    static std::vector<char> resume_state;
    resume_state.clear();
    if (what.resume && !lan::g_seats.empty())
    {
        // Co-op continue: the same saved game on every machine (lan::host_game_starting
        // read it on the host and sent it; a client received it).
        resume_state.assign(lan::g_resume_data.begin(), lan::g_resume_data.end());
        if (!resume_state.empty())
        {
            options.un_6 = (int32_t)resume_state.size();
            options.saved_game_state = resume_state.data();
            logf("continuing the co-op saved game (%zu bytes)", resume_state.size());
        }
        else logf("starting the co-op mission from its beginning");
    }
    else if (what.resume)
    {
        if (FILE *f = _wfopen((what.coop ? coop_save_path(g_cfg.game) : save_path(g_cfg.game)).c_str(), L"rb"))
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

    g_game_run++;
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
        lan::poll(true);
        keep_game_changes();
        menu::on_idle();
        pump_messages();
        if (g_next.kind != next_action::k_none && !g_ending)
        {
            g_ending = true;
            g_ending_since = (int64_t)GetTickCount64();
            menu::g_open = false;
            logf("asking the game to end");
            if (lan::g_role == lan::k_client && !lan::g_seats.empty() && !g_show_postgame)
            {
                logf("lan: leaving the host's game; its game traffic stops here");
                lan::g_traffic_cut = true;
            }
            g_engine->post_message(1, nullptr); // resume, if paused
            g_engine->post_message(13, nullptr);
        }
        // The game ended by itself (the mission or co-op session is over, or the host
        // left a LAN game): nothing asked it to, so treat it as asked. Halo 3, Halo 4 and
        // Reach end their thread on their own before this; Halo 2 waits for the engine
        // to be released below, and without this it waited forever.
        if (!g_ending && g_game_over_at && (int64_t)GetTickCount64() - g_game_over_at > 3000)
        {
            logf("the game ended by itself; going back to the launcher");
            g_show_postgame = true, g_return_to_lobby = true;
            g_ending = true;
            g_ending_since = (int64_t)GetTickCount64();
            menu::g_open = false;
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
        if (g_ending && (int64_t)GetTickCount64() - g_ending_since > (lan::g_traffic_cut ? 40000 : 20000))
        {
            logf("the game did not end within %d seconds", lan::g_traffic_cut ? 40 : 20);
            break;
        }
    }
    KillTimer(g_hwnd, 1);
    keep_game_changes();
    g_smooth = false;
    lan::log_totals();
    lan::game_ended();
    lan::set_playing(false);
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
    // Let the graphics driver finish with everything the game drew and let go of what
    // it made before its code goes away: some of the game's objects are released by the
    // driver later (on the PS5, after the unload, which crashed the launcher).
    if (context)
    {
        context->ClearState();
        // Two blank frames: the PS5's graphics layer (DXVK) lets go of what a game
        // released at the end of a frame. Halo 4 attaches objects of its own to its
        // buffers (as private data), released then; after the DLL is gone that crashed.
        if (g_swapchain)
            for (int i = 0; i < 2; i++)
            {
                ID3D11Texture2D *back = nullptr;
                if (SUCCEEDED(g_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back)) && back)
                {
                    ID3D11RenderTargetView *rtv = nullptr;
                    if (device && SUCCEEDED(device->CreateRenderTargetView(back, nullptr, &rtv)) && rtv)
                    {
                        const float black[4] = {0, 0, 0, 1};
                        context->ClearRenderTargetView(rtv, black);
                        rtv->Release();
                    }
                    back->Release();
                }
                g_swapchain->Present(0, 0);
            }
        context->ClearState();
        context->Flush();
        D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
        ID3D11Query *done = nullptr;
        if (device && SUCCEEDED(device->CreateQuery(&qd, &done)))
        {
            context->End(done);
            context->Flush();
            BOOL finished = FALSE;
            for (int i = 0; i < 400 && context->GetData(done, &finished, sizeof(finished), 0) != S_OK; i++) Sleep(5);
            done->Release();
        }
    }
    g_pending_unload = module;
    g_pending_unload_dll = dll;
    logf("game stopped; its DLL is unloaded once the menu has drawn for a moment");
    return result;
}

// Before ending the process: let the graphics driver finish this process's work and
// let go of the window and swap chain, so the next start does not wait on them.
static void release_graphics(ID3D11Device *device, ID3D11DeviceContext *context)
{
    if (g_hwnd) ShowWindow(g_hwnd, SW_HIDE);
    if (menu::g_rt) menu::g_rt->Release(), menu::g_rt = nullptr;
    if (context)
    {
        context->ClearState();
        context->Flush();
        D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
        ID3D11Query *done = nullptr;
        if (device && SUCCEEDED(device->CreateQuery(&qd, &done)))
        {
            context->End(done);
            BOOL finished = FALSE;
            for (int i = 0; i < 200 && context->GetData(done, &finished, sizeof(finished), 0) != S_OK; i++) Sleep(5);
            done->Release();
        }
    }
    if (g_swapchain) g_swapchain->Release(), g_swapchain = nullptr;
    if (context) context->Release();
    if (device) device->Release();
    if (g_hwnd) DestroyWindow(g_hwnd), g_hwnd = nullptr;
    logf("graphics released");
}

// Another Installation 08 still running (or still closing): worth knowing about.
static void note_other_instances()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = {sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (pe.th32ProcessID != GetCurrentProcessId() && !_wcsicmp(pe.szExeFile, L"installation08.exe"))
        {
            logf("another installation08.exe is still running (process %lu); waiting for it to close", (unsigned long)pe.th32ProcessID);
            if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pe.th32ProcessID))
            {
                DWORD w = WaitForSingleObject(h, 15000);
                logf("  %s", w == WAIT_OBJECT_0 ? "it closed" : "it is still there; carrying on");
                CloseHandle(h);
            }
        }
    CloseHandle(snap);
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
        else if (k == L"--show-result") g_show_result_path = v;
        else if (k == L"--window-x") g_cfg.window_x = _wtoi(v);
        else if (k == L"--window-y") g_cfg.window_y = _wtoi(v);
        else if (k == L"--window-max") g_cfg.window_max = _wtoi(v);
        else if (k == L"--borderless") g_cfg.borderless = _wtoi(v);
        else if (k == L"--map") g_cfg.map = _wtoi(v);
        else if (k == L"--map-file") g_cfg.map_file = v;
        else if (k == L"--preload") g_cfg.preload = _wtoi(v);
        else if (k == L"--auto-resume") g_cfg.auto_resume = _wtoi(v);
        else if (k == L"--monitor") g_cfg.monitor = _wtoi(v);
        else if (k == L"--pad-mapping") g_cfg.pad_mapping = _wtoi(v);
        else if (k == L"--volume")
            g_cfg.volume = (float)_wtof(v), g_settings.master = g_settings.music = g_settings.effects = (int)(g_cfg.volume * 100);
        else if (k == L"--mouse-sensitivity") g_settings.mouse = (float)_wtof(v);
        else if (k == L"--address") g_cfg.address = _wcstoui64(v, nullptr, 0);
        else if (k == L"--lan") g_lan_arg = v;
        else if (k == L"--lan-name") lan::g_name = narrow(v);
        else if (k == L"--lan-port") lan::g_port = (uint16_t)_wtoi(v);
        else if (k == L"--online-server") lan::g_server_name = narrow(v);
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
    g_main_thread = GetCurrentThreadId();
    if (lan::g_name.empty()) lan::g_name = menu::clean_name(g_settings.lan_name);
    if (lan::g_name.empty()) lan::g_name = menu::default_name();
    if (!g_lan_arg.empty()) lan::start(g_lan_arg);
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
    logf(I08_ONLINE ? "Installation 08 1.3" : "Installation 08 1.3 (LAN only)");
    log_cpu();
    logf("picture %dx%d, %s", g_cfg.width, g_cfg.height, g_cfg.borderless ? "fullscreen" : "window");
    note_other_instances();
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
    lan::shutdown();
    logf("installation08 exiting");
    save_settings();
    if (!g_game_module) release_graphics(device, context); // (a game still running keeps them)
    // A normal exit runs the game DLLs' shutdown code again; nothing is left to save,
    // so end the process outright.
    log_flush();
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
