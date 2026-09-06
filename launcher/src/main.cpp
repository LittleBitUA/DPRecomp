// DPLauncher — Deadly Premonition rexglue port launcher
// Win32 + GDI+ native window with PLAY / Settings / Exit buttons.
// Derived from the Silent Hill: Downpour launcher (DPourLauncher); the
// Downpour-only flows (ISO/title-update installer, UE3 Coalesced editor,
// background music controls) were removed for this port.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <digitalv.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <commctrl.h>
#include <shlobj.h>
#include <winhttp.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <thread>
#include <string>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <vector>
#include <map>
#include <memory>
#include <unordered_map>
#include <cwchar>
#include <algorithm>
#include <set>
#include <atomic>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "winhttp.lib")

using namespace Gdiplus;

// Defined at file scope (outside namespace) so wWinMain can reach them too.
void SaveLauncherLanguageSidecar();
void LoadLauncherLanguageFromToml();
// v1.1.6: launcher.ini access from inside the anonymous namespace (Settings
// dialog Save handler) needs these symbols pre-declared. The definitions
// live further down at file scope, after the anon ns closes.
extern std::unordered_map<std::string, std::string> g_launcher_ini;
void WriteLauncherIni();
void MaybeShareShaderCache();
void ApplySteamDeckPresetIfDetected();
void GenerateKeyPromptOverlay();
void EnsureBundledAssets();
std::string GetPromptStyle();
bool IsKnownPromptStyle(const std::string& v);
static void ReadLauncherIni();
extern std::unordered_map<std::string, std::string> g_launcher_ini;

namespace {

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr wchar_t kWindowClass[] = L"DPLauncherWindow";
// Base title used by message boxes and other "what is this window" queries.
// The actual top-level window title built in wWinMain composes this with
// kLauncherVersion + author suffix so the taskbar always shows the shipping
// version (e.g. "Deadly Premonition v0.2.0 | «Little Bit»"). Bumping
// kLauncherVersion is the SINGLE source of truth — title updates flow from
// there. Don't add a separate version string here.
constexpr wchar_t kWindowTitle[] = L"Deadly Premonition Recompilation";
constexpr wchar_t kWindowTitleAuthorSuffix[] = L" | «Little Bit»";
// Game-side file names next to the launcher.
constexpr wchar_t kGameExeName[] = L"deadlyprem.exe";
constexpr wchar_t kGameTomlName[] = L"deadlyprem.toml";

// Resource IDs (must match resources.rc).
#define IDR_BANNER 200
#define IDR_LOGO   201
#define IDR_FONT   203  // Cinema Calligraphy (the game's UI font) for the key prompts
#define IDR_MUSIC  202

ULONG_PTR g_gdiplus_token = 0;
std::unique_ptr<Bitmap> g_banner_bitmap;
std::unique_ptr<Bitmap> g_logo_bitmap;
HFONT g_button_font = nullptr;
HFONT g_hint_font = nullptr;
HFONT g_title_font = nullptr;
std::wstring g_music_temp_path;

// ===== i18n =====
enum LangId { kLangEn = 0, kLangUk = 1 };
LangId g_lang = kLangEn;

std::wstring Widen(const std::string& s);
bool LooksNumeric(const std::string& s);

static const std::map<std::string, std::wstring>& UkTable() {
  static const std::map<std::string, std::wstring> t = {
      // Buttons / hint.
      {"PLAY",              L"ГРАТИ"},
      {"Update available: ",L"Доступне оновлення: "},
      {" — click to install",L" — натисніть, щоб встановити"},
      {"Download and install update?",
                            L"Завантажити та встановити оновлення?"},
      {"Latest version: ", L"Остання версія: "},
      {"Current version: ",L"Поточна версія: "},
      {"Downloading update",L"Завантаження оновлення"},
      {"Update failed",     L"Помилка оновлення"},
      {"Could not download the update zip. Check your internet connection.",
                            L"Не вдалося завантажити zip-архів оновлення. Перевірте інтернет-зʼєднання."},
      {"The latest release contains no DPRecomp zip asset.",
                            L"У останньому релізі відсутній zip-архів DPRecomp."},
      {"Update — preparing installer...",
                            L"Оновлення — підготовка інсталятора..."},
      {"Previous update did not finish",
                            L"Попереднє оновлення не завершилось"},
      {"The auto-updater logged an error on the last attempt. Open the diagnostic log? (No = delete log and continue.)",
                            L"Авто-оновлювач залишив помилку у журналі при минулій спробі. Відкрити діагностичний лог? (Ні — видалити лог і продовжити.)"},
      {"Yes",               L"Так"},
      {"No",                L"Ні"},
      {"Settings",          L"Налаштування"},
      {"Exit",              L"Вихід"},
      {"Save && Close",     L"Зберегти і закрити"},
      {"Cancel",            L"Скасувати"},
      {"D-Pad: Select Option   A: Confirm",
                            L"D-Pad: Вибір   A: Підтвердити"},
      // Tabs.
      {"Graphics",          L"Графіка"},
      {"Advanced",          L"Додатково"},
      {"Mouse",             L"Миша"},
      {"Controls",          L"Керування"},
      {"Debug",             L"Діагностика"},
      // Graphics cvars.
      {"Render Target Path",        L"Шлях рендеру"},
      {"ROV (recommended)",         L"ROV (рекомендовано)"},
      {"RTV (compatibility)",       L"RTV (сумісність)"},
      {"Auto (SDK default)",        L"Авто (за замовчуванням SDK)"},
      {"Internal Resolution Scale", L"Внутрішнє суперсемплування"},
      {"1x — 1280x720 internal",    L"1x — 1280x720 внутрішньо"},
      {"2x — 2560x1440 internal (recommended)",
                                    L"2x — 2560x1440 внутрішньо (рекомендовано)"},
      {"3x — 3840x2160 internal",   L"3x — 3840x2160 внутрішньо"},
      {"4x — 5120x2880 internal (slowest)",
                                    L"4x — 5120x2880 внутрішньо (найповільніше)"},
      {"Native 2x MSAA",            L"Нативний 2x MSAA"},
      {"Anisotropic Filtering",     L"Анізотропна фільтрація"},
      {"Game default",              L"Як у грі"},
      {"Off",                       L"Вимк."},
      {"Post-process Anti-Aliasing", L"Згладжування (FXAA)"},
      {"Off (sharp, more aliasing)", L"Вимкнено (чітко, видно aliasing)"},
      {"FXAA (recommended)",        L"FXAA (рекомендовано)"},
      {"FXAA Extreme (heavier blur, hides specks)",
                                    L"FXAA Extreme (сильніший blur, ховає точки)"},
      {"Upscaler / Sharpener",      L"Апскейлер / Різкість"},
      {"Bilinear (off)",            L"Білінійний"},
      {"AMD CAS (sharpening)",      L"AMD CAS"},
      {"AMD FSR 1 (spatial)",       L"AMD FSR 1"},
      {"AMD FSR 2 (temporal)",      L"AMD FSR 2"},
      {"AMD FSR 3 (temporal+)",     L"AMD FSR 3"},
      {"FSR Quality Mode",          L"Режим якості FSR"},
      {"Auto",                      L"Авто"},
      {"Native AA",                 L"Native AA"},
      {"Quality",                   L"Якість"},
      {"Balanced",                  L"Збалансований"},
      {"Performance",               L"Швидкодія"},
      {"Ultra Performance",         L"Макс. швидкодія"},
      {"FSR Softness (0 = sharpest)", L"М'якість FSR (0 = різко)"},
      {"CAS Extra Sharpness",       L"CAS: додаткова різкість"},
      {"Preserve Aspect (Letterbox)", L"Зберегти пропорції"},
      {"60 FPS (ehw patch)",        L"60 FPS (патч ehw)"},
      {"VSync",                     L"Вертикальна синхр."},
      {"Fullscreen",                L"Повноекранний режим"},
      {"Window Width (0 = auto)",   L"Ширина вікна (0 = авто)"},
      {"Window Height (0 = auto)",  L"Висота вікна (0 = авто)"},
      {"Monitor",                   L"Монітор"},
      {"Default (primary)",         L"Типово (основний)"},
      {"Allow VRR / Tearing",       L"Дозволити VRR / tearing"},
      {"Output Dithering",          L"Дизеринг виводу"},
      // Advanced.
      {"Launcher Language",         L"Мова лаунчера"},
      {"English",                   L"Англійська"},
      {"Ukrainian",                 L"Українська"},
      {"Input Backend",             L"Бекенд вводу"},
      {"SDL (recommended, DualSense support)",
                                    L"SDL (рекомендовано, підтримка DualSense)"},
      {"XInput (Xbox controllers only)",
                                    L"XInput (лише Xbox-геймпади)"},
      {"Controller Mappings (SDL)",
                                    L"Мапінги геймпадів (SDL)"},
      {"Game Language",             L"Мова гри"},
      {"German (Deutsch)",          L"Німецька"},
      {"French (Francais)",         L"Французька"},
      {"Spanish (Espanol)",         L"Іспанська"},
      {"Italian (Italiano)",        L"Італійська"},
      {"GPU Adapter",               L"Відеоадаптер"},
      {"Auto (first physical GPU)", L"Авто (перший фізичний GPU)"},
      {"Adapter 0",                 L"Адаптер 0"},
      {"Adapter 1",                 L"Адаптер 1"},
      {"Adapter 2",                 L"Адаптер 2"},
      {"Async Shader Compilation",  L"Асинхронні шейдери"},
      {"Texture Cache Soft Limit (MB)",
                                    L"Кеш текстур: м'який (МБ)"},
      {"Texture Cache Hard Limit (MB)",
                                    L"Кеш текстур: жорсткий (МБ)"},
      {"Mute Game Audio",           L"Вимкнути звук гри"},
      // Mouse.
      {"Mouse & Keyboard Mode",     L"Миша + клавіатура"},
      {"Mouse Camera Hook (direct)",
                                    L"Хук камери (миша)"},
      {"Camera Hook Sensitivity",   L"Чутливість хука камери"},
      {"Camera Hook Invert Y",      L"Хук: інверсія Y"},
      {"Mouse as Right Stick",
                                    L"Миша як правий стік"},
      {"Mouse Sensitivity",         L"Чутливість миші"},
      {"Stick Scale (units per pixel)", L"Масштаб стіка (од./піксель)"},
      {"Deadzone Floor (stick units)", L"Мін. мертва зона (стік)"},
      {"Invert Mouse Y",            L"Інверсія миші по Y"},
      // DualSense.
      {"DualSense Adaptive Triggers", L"DualSense: адаптивні курки"},
      {"Right Trigger Effect Mode", L"Правий курок: режим ефекту"},
      {"Left Trigger Effect Mode",  L"Лівий курок: режим ефекту"},
      {"Off (pass-through)",        L"Вимк. (без ефекту)"},
      {"Feedback (constant resistance)",
                                    L"Feedback (постійний опір)"},
      {"Weapon (click point — gun trigger feel)",
                                    L"Weapon (клацання спуску)"},
      {"Vibration (buzz on pull)",  L"Vibration (вібрація)"},
      {"Right Trigger Start Position", L"Правий курок: початок"},
      {"Right Trigger End Position", L"Правий курок: кінець"},
      {"Right Trigger Strength",    L"Правий курок: сила"},
      {"Left Trigger Start Position", L"Лівий курок: початок"},
      {"Left Trigger End Position", L"Лівий курок: кінець"},
      {"Left Trigger Strength",     L"Лівий курок: сила"},
      // Keybinds (Director's Cut layout).
      {"A button (action / fire)",
                                    L"A (дія / постріл)"},
      {"B button",                  L"B"},
      {"X button",                  L"X"},
      {"Y button",                  L"Y"},
      {"Left Trigger",              L"LT (лівий курок)"},
      {"Right Trigger (aim)",       L"RT (прицілювання)"},
      {"Left Shoulder",             L"LB (лівий бампер)"},
      {"Right Shoulder",            L"RB (правий бампер)"},
      {"Left Stick Press",          L"Натиск лівого стіку"},
      {"Right Stick Press",         L"Натиск правого стіку"},
      {"Move Forward",              L"Рух уперед"},
      {"Move Backward",             L"Рух назад"},
      {"Strafe Left",               L"Крок ліворуч"},
      {"Strafe Right",              L"Крок праворуч"},
      {"D-Pad Up",                  L"D-Pad угору"},
      {"D-Pad Down",                L"D-Pad униз"},
      {"D-Pad Left",                L"D-Pad ліворуч"},
      {"D-Pad Right",               L"D-Pad праворуч"},
      {"Back",                      L"Back"},
      {"Start (pause menu)",        L"Start (пауза)"},
      // Debug.
      {"Log Level",                 L"Рівень логування"},
      {"Off (no logs)",             L"Вимк. (без логів)"},
      {"Error",                     L"Error (лише помилки)"},
      {"Warn",                      L"Warn (попередження)"},
      {"Info (recommended)",        L"Info (рекомендовано)"},
      {"Debug (verbose)",           L"Debug (детально)"},
      {"Trace (very verbose)",      L"Trace (дуже детально)"},
      {"Memexport Readback (keep on)", L"Читання memexport (лишити)"},
      {"Occlusion Queries",         L"Occlusion-запити (відсікання)"},
      {"PSO Missing Policy",        L"Якщо шейдер ще не готовий"},
      {"Block (wait per draw, budgeted — recommended)",
                                    L"Чекати (рекомендовано)"},
      {"Skip (no block, pop-in on miss)",
                                    L"Пропустити (об'єкт зникне)"},
      {"Sync (inline compile, longest stutter)",
                                    L"Синхронно (найдовший фриз)"},
      {"PSO Block Budget (ms)", L"Макс. очікування шейдера (мс)"},
      {"No PSO Wait At Frame End",  L"Не чекати шейдери в кінці кадру"},
      {"D3D12 Debug Layer (slow)",  L"Debug-шар D3D12 (повільно)"},
      {"Shader Storage Cache",      L"Зберігати шейдери на диску"},
      {"Share Shader Cache",        L"Надсилати кеш шейдерів"},
      {"Help other players?",       L"Допомогти іншим гравцям?"},
      {"Steam Deck preset",         L"Пресет Steam Deck"},
      {"Steam Overlay",             L"Оверлей Steam"},
      {"Button Prompts",            L"Підказки кнопок"},
      {"Texture Dump (textures\\dump)", L"Дамп текстур (textures\\dump)"},
      {"Keyboard (keys from your bindings)", L"Клавіатура (ваші клавіші)"},
      {"Xbox (original icons)",     L"Xbox (рідні іконки)"},
      {"PlayStation - solid (DualShock / DualSense)", L"PlayStation - суцільні (DualShock / DualSense)"},
      {"PlayStation - solid with ring", L"PlayStation - суцільні з кільцем"},
      {"PlayStation - outline",     L"PlayStation - контурні"},
      {"On (Steam default)",        L"Увімк. (типово в Steam)"},
      {"Off (fixes black screen / tinted quarter frame)",
                                    L"Вимк. (лікує чорний екран)"},
      {"On - send my shader cache (anonymous) to the project",
                                    L"Увімк. (анонімно, після гри)"},
      {"Shader Compile Indicator",  L"Індикатор компіляції PSO"},
      {"Shader Indicator: Verbose", L"Індикатор: детально"},
      {"PSO Library (disk cache)",  L"Бібліотека PSO (на диску)"},
      {"Camera Hook: Direct Yaw",   L"Хук: прямий поворот"},
      {"Camera Hook: Catch-up",     L"Хук: наздоганяння"},
      {"Camera Auto-center Hold (ms)", L"Затримка автоцентру (мс)"},
      {"Key Stick Ramp (ms)",       L"Розгін стіка з клавіш (мс)"},
      {"Auto-shake (hold A + D)",   L"Автотряска (тримати A + D)"},
      {"Auto-shake Rate (Hz)",      L"Частота автотряски (Гц)"},
      {"Steam Deck detected - the community-tested Deck preset was applied (RTV, 1x, 2x MSAA, 16x AF, FXAA + CAS, 30 FPS, VSync, fullscreen 1280x800). You can change anything in Settings; this will not be applied again.",
       L"Виявлено Steam Deck - застосовано перевірений спільнотою пресет (RTV, 1x, 2x MSAA, 16x AF, FXAA + CAS, 30 FPS, VSync, повний екран 1280x800). Усе можна змінити в Settings; повторно не застосовуватиметься."},
      {"Share your shader cache with the project?\n\nWhen enabled, the launcher sends the shader cache the game builds while you play (only shader microcode and pipeline descriptions - no personal data, no save games) to the developers. Merged caches ship with the next release, so people who play after you get fewer stutters in new scenes.\n\nYou can change this later in Settings -> Advanced -> Share Shader Cache.",
       L"Поділитися кешем шейдерів із проєктом?\n\nЯкщо увімкнути, лаунчер надсилатиме розробникам кеш шейдерів, який гра будує під час твоєї гри (лише мікрокод шейдерів і описи pipeline - без персональних даних і сейвів). Злиті кеші виходять у наступному релізі, тож ті, хто гратиме після тебе, матимуть менше підлагувань у нових сценах.\n\nЗмінити можна пізніше: Settings -> Advanced -> Share Shader Cache."},
      {"Log Files To Keep",         L"Кількість лог-файлів"},
      {"Log File Size Limit (MB)",  L"Ліміт розміру лог-файлу (МБ)"},
      // Misc.
      {"Launcher language changed. Restart to apply.",
                                    L"Мова лаунчера змінена. Перезапустіть для застосування."},
      {"Failed to launch deadlyprem.exe.\nMake sure it exists next to PlayDeadlyPremonition.exe.",
                                    L"Не вдалося запустити deadlyprem.exe.\nПереконайтеся, що він поруч із PlayDeadlyPremonition.exe."},
      {"Game data not found.\nExpected assets\\default.xex next to the launcher.\n"
       "Copy the extracted game files into the assets folder and try again.",
                                    L"Файли гри не знайдено.\nОчікується assets\\default.xex поруч із лаунчером.\n"
                                    L"Скопіюйте розпаковані файли гри в теку assets і спробуйте знову."},
  };
  return t;
}

std::wstring TrW(const std::string& en) {
  if (g_lang == kLangUk) {
    const auto& t = UkTable();
    auto it = t.find(en);
    if (it != t.end()) return it->second;
  }
  return Widen(en);
}

const wchar_t* TrC(const char* en) {
  thread_local std::wstring cache;
  cache = TrW(en);
  return cache.c_str();
}

struct Button {
  RECT rect;
  std::wstring text;
  bool hovered = false;
  bool pressed = false;
  int id = 0;
};

constexpr int kBtnPlay = 1;
constexpr int kBtnSettings = 2;
constexpr int kBtnExit = 3;
constexpr int kBtnUpdate = 4;

// Embedded launcher version. Bump on every release. The boot-time GitHub
// API probe compares this to the latest release `tag_name` to decide whether
// to show the "Update available" banner. Keep resources.rc in sync.
constexpr const wchar_t* kLauncherVersion = L"v1.2.0";
// v1.1: opt-in shader cache sharing. When the user enables "Share Shader
// Cache" (launcher.ini: launcher_share_shader_cache = on) the launcher zips
// userdata\cache\shaders\shareable\*.xsh / *.xpso (game shader microcode +
// pipeline descriptions only, no personal data) and posts it to this Discord
// webhook whenever the cache changed since the last upload. Empty = feature
// hidden and disabled.
constexpr const wchar_t* kShaderCacheWebhookUrl = L"";
constexpr const char* kGithubReleaseUrl =
    "https://github.com/LittleBitUA/DPRecomp/releases/latest";

// Background-probed update state. Written once by the worker thread, then
// read every paint frame from the UI thread. The atomic flag is the
// happens-before fence; the tag string + asset URL are only read after the
// flag flips to true so the relaxed-vs-acquire pairing is sufficient.
std::atomic<bool> g_update_available{false};
std::wstring g_update_tag;       // e.g. "v1.0.1"
std::wstring g_update_asset_url; // direct https URL to the zip asset

// Translations for the update-flow strings.
struct UpdateStrings {
  std::wstring banner_text;       // "Update available: v1.0.1 — click to install"
  std::wstring confirm_title;     // dialog title
  std::wstring confirm_body;      // "Download X MB and install update?"
  std::wstring progress_title;    // "Downloading update"
  std::wstring failure_title;     // "Update failed"
  std::wstring failure_body;      // "Could not download the update zip."
  std::wstring no_asset_body;     // "No DPRecomp zip was found in the release."
};
UpdateStrings BuildUpdateStrings();

std::vector<Button> g_buttons;
int g_focused_button = 0;

std::wstring GetExeDir() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  PathRemoveFileSpecW(path);
  return path;
}

Bitmap* LoadBitmapFromResource(int res_id) {
  HRSRC hres = FindResourceW(nullptr, MAKEINTRESOURCEW(res_id), RT_RCDATA);
  if (!hres) return nullptr;
  DWORD size = SizeofResource(nullptr, hres);
  HGLOBAL hmem = LoadResource(nullptr, hres);
  if (!hmem) return nullptr;
  void* data = LockResource(hmem);
  if (!data) return nullptr;

  HGLOBAL hbuf = GlobalAlloc(GMEM_MOVEABLE, size);
  if (!hbuf) return nullptr;
  void* buf = GlobalLock(hbuf);
  memcpy(buf, data, size);
  GlobalUnlock(hbuf);

  IStream* stream = nullptr;
  if (CreateStreamOnHGlobal(hbuf, TRUE, &stream) != S_OK) {
    GlobalFree(hbuf);
    return nullptr;
  }
  Bitmap* bmp = Bitmap::FromStream(stream);
  stream->Release();
  if (bmp && bmp->GetLastStatus() != Ok) {
    delete bmp;
    return nullptr;
  }
  return bmp;
}

void ExtractMusicToTemp() {
  HRSRC hres = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_MUSIC), RT_RCDATA);
  if (!hres) return;
  DWORD size = SizeofResource(nullptr, hres);
  HGLOBAL hmem = LoadResource(nullptr, hres);
  if (!hmem) return;
  void* data = LockResource(hmem);
  if (!data) return;

  wchar_t temp_dir[MAX_PATH];
  GetTempPathW(MAX_PATH, temp_dir);
  wchar_t temp_file[MAX_PATH];
  GetTempFileNameW(temp_dir, L"dpm", 0, temp_file);
  std::wstring ogg_path = std::wstring(temp_file) + L".ogg";

  HANDLE h = CreateFileW(ogg_path.c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  BOOL ok = WriteFile(h, data, size, &written, nullptr);
  CloseHandle(h);
  DeleteFileW(temp_file);  // remove the placeholder file the API generated.
  if (!ok || written != size) {
    DeleteFileW(ogg_path.c_str());
    return;  // Partial/failed write — don't expose truncated audio to MCI.
  }
  g_music_temp_path = ogg_path;
}

// v1.1.6: launcher background-music volume (0..100 percent) and mute toggle.
// User reported the music played at full volume by default which was too loud;
// dropped default to 25%. Persisted in launcher.ini so the choice survives
// across releases (toml is reserved for game-side cvars). MCI's setaudio
// expects 0..1000 internally so we multiply by 10 in ApplyMusicVolume.
static int g_music_volume = 25;
static bool g_music_muted = false;
static bool g_music_open  = false;

void ApplyMusicVolume() {
  if (!g_music_open) return;
  int percent = g_music_muted ? 0 : g_music_volume;
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  wchar_t cmd[64];
  swprintf_s(cmd, L"setaudio bgmusic volume to %d", percent * 10);
  mciSendStringW(cmd, nullptr, 0, nullptr);
}

void StartMusic() {
  if (g_music_temp_path.empty()) return;
  std::wstring open_cmd = L"open \"" + g_music_temp_path + L"\" type mpegvideo alias bgmusic";
  if (mciSendStringW(open_cmd.c_str(), nullptr, 0, nullptr) != 0) {
    // Fallback: try alias type detection.
    std::wstring fallback = L"open \"" + g_music_temp_path + L"\" alias bgmusic";
    mciSendStringW(fallback.c_str(), nullptr, 0, nullptr);
  }
  g_music_open = true;
  ApplyMusicVolume();
  mciSendStringW(L"play bgmusic repeat", nullptr, 0, nullptr);
}

void StopMusic() {
  mciSendStringW(L"stop bgmusic", nullptr, 0, nullptr);
  mciSendStringW(L"close bgmusic", nullptr, 0, nullptr);
  if (!g_music_temp_path.empty()) {
    DeleteFileW(g_music_temp_path.c_str());
    g_music_temp_path.clear();
  }
}

void LoadAssets() {
  g_banner_bitmap.reset(LoadBitmapFromResource(IDR_BANNER));
  g_logo_bitmap.reset(LoadBitmapFromResource(IDR_LOGO));
}

void CreateFonts() {
  LOGFONTW lf = {};
  lf.lfHeight = -22;
  lf.lfWeight = FW_BOLD;
  lf.lfQuality = CLEARTYPE_QUALITY;
  wcscpy_s(lf.lfFaceName, L"Bahnschrift");
  g_button_font = CreateFontIndirectW(&lf);

  lf.lfHeight = -14;
  lf.lfWeight = FW_NORMAL;
  wcscpy_s(lf.lfFaceName, L"Segoe UI");
  g_hint_font = CreateFontIndirectW(&lf);

  lf.lfHeight = -28;
  lf.lfWeight = FW_BOLD;
  wcscpy_s(lf.lfFaceName, L"Bahnschrift");
  g_title_font = CreateFontIndirectW(&lf);
}

void LayoutButtons(int client_w, int client_h) {
  g_buttons.clear();
  const int default_w = 140;
  const int btn_h = 44;
  const int gap = 10;
  const int margin_right = 24;
  const int margin_bottom = 24;

  const char* labels[] = {"PLAY", "Settings", "Exit"};
  const int widths[] = {default_w, default_w, default_w};
  const int ids[] = {kBtnPlay, kBtnSettings, kBtnExit};
  const int count = 3;

  int total_w = (gap * (count - 1));
  for (int i = 0; i < count; ++i) total_w += widths[i];
  int x = client_w - margin_right - total_w;
  int y = client_h - margin_bottom - btn_h;

  for (int i = 0; i < count; ++i) {
    Button b;
    b.rect = {x, y, x + widths[i], y + btn_h};
    b.text = TrW(labels[i]);
    b.id = ids[i];
    g_buttons.push_back(b);
    x += widths[i] + gap;
  }

  // Update banner (if a newer GitHub release was found). Modern pill-shaped
  // notification at top-center. Painted via DrawUpdateBanner (rounded corners
  // + accent gradient + subtle glow) instead of the generic DrawButton.
  if (g_update_available.load(std::memory_order_acquire)) {
    const int banner_h = 44;
    const int banner_w = 540;  // wide enough for "Update available: vX.Y.Z — click to install"
    const int banner_y = 36;
    int banner_x = (client_w - banner_w) / 2;
    if (banner_x < 24) banner_x = 24;
    Button b;
    b.rect = {banner_x, banner_y, banner_x + banner_w, banner_y + banner_h};
    b.text = TrW("Update available: ") + g_update_tag +
             TrW(" — click to install");
    b.id = kBtnUpdate;
    g_buttons.push_back(b);
  }
}

// GDI+ FontFamily ctor fails (status != Ok) when the requested face isn't
// installed — unlike GDI's CreateFont which substitutes silently. Bahnschrift
// (Win10+) and Segoe UI (Win7+) are not on stock Wine: DrawString then
// renders nothing → blank buttons / blank title on Linux. Walk a list of
// candidates and finally fall back to GDI+'s GenericSansSerif which is
// guaranteed available on any platform.
std::unique_ptr<FontFamily> MakeFontFamilyWithFallback(
    std::initializer_list<const wchar_t*> candidates) {
  for (const wchar_t* name : candidates) {
    auto ff = std::make_unique<FontFamily>(name);
    if (ff->GetLastStatus() == Ok && ff->IsAvailable()) {
      return ff;
    }
  }
  // GenericSansSerif() returns a borrowed singleton; FontFamily's copy ctor is
  // private. Look up its family name and construct a fresh, owned instance.
  WCHAR generic_name[LF_FACESIZE] = {};
  if (auto* generic = FontFamily::GenericSansSerif()) {
    generic->GetFamilyName(generic_name);
  }
  if (generic_name[0] == L'\0') {
    wcscpy_s(generic_name, L"Microsoft Sans Serif");  // ships with Wine too
  }
  return std::make_unique<FontFamily>(generic_name);
}

// Build a rounded-rectangle path with the given corner radius. Used by
// DrawUpdateBanner — GDI+ has no native rounded-rect primitive, so we
// stitch four arcs and two line edges.
static void BuildRoundedRectPath(GraphicsPath& path, int x, int y, int w,
                                  int h, int radius) {
  const int d = radius * 2;
  path.Reset();
  path.AddArc(x,             y,             d, d, 180, 90);
  path.AddArc(x + w - d,     y,             d, d, 270, 90);
  path.AddArc(x + w - d,     y + h - d,     d, d,   0, 90);
  path.AddArc(x,             y + h - d,     d, d,  90, 90);
  path.CloseFigure();
}

// Custom paint for the GitHub update notification. Designed to sit on the
// Downpour title-screen art without standing out as a Windows-toast pill —
// uses the game's muted blue-grey + aged-blood palette:
//   • near-black fill ~92 % opacity (sits over banner art without blocking)
//   • thin 1 px maroon border with a faint blood-red outer glow
//   • white text matching the PLAY / Settings buttons exactly
//   • rounded full-pill corners (radius = half-height) for the only round
//     element on screen — that alone marks it as "this is a notification"
static void DrawUpdateBanner(Graphics& g, const Button& btn, bool focused) {
  const int x = btn.rect.left;
  const int y = btn.rect.top;
  const int w = btn.rect.right - btn.rect.left;
  const int h = btn.rect.bottom - btn.rect.top;
  const int radius = h / 2;

  const bool active = btn.hovered || btn.pressed || focused;

  // Outer aged-blood glow — concentric layers, deeper red when active.
  for (int i = 5; i >= 1; --i) {
    GraphicsPath glow;
    BuildRoundedRectPath(glow, x - i, y - i, w + 2 * i, h + 2 * i,
                         radius + i);
    BYTE alpha = (BYTE)((active ? 28 : 16) / i);
    SolidBrush glow_brush(Color(alpha, 120, 22, 22));
    g.FillPath(&glow_brush, &glow);
  }

  // Main fill — near-black, slightly translucent so the banner art bleeds
  // through and the pill belongs to the scene, not floats above it.
  GraphicsPath shape;
  BuildRoundedRectPath(shape, x, y, w, h, radius);
  Color fill_top    = active ? Color(245, 36, 36, 40) : Color(230, 22, 22, 26);
  Color fill_bottom = active ? Color(245, 22, 22, 26) : Color(230, 14, 14, 18);
  LinearGradientBrush fill(Rect(x, y, w, h), fill_top, fill_bottom,
                           LinearGradientModeVertical);
  g.FillPath(&fill, &shape);

  // Hairline maroon border. Slightly brighter (closer to neutral grey) on
  // hover so the affordance reads as "this is interactive" without going
  // full primary-button white.
  Pen border(active ? Color(220, 200, 200, 200) : Color(180, 140,  56,  56),
             1.0f);
  g.DrawPath(&border, &shape);

  // Text — same colour and style as the PLAY / Settings buttons. No icon
  // glyph (the Unicode ⬇ rendered with non-uniform advance width on the
  // Bahnschrift fallback chain, which threw StringAlignmentCenter off by
  // ~15 px). The pill shape itself signals "this is a notification".
  auto ff = MakeFontFamilyWithFallback({L"Bahnschrift", L"Segoe UI", L"Tahoma"});
  Font text_font(ff.get(), 15, FontStyleBold, UnitPixel);
  StringFormat fmt;
  fmt.SetAlignment(StringAlignmentCenter);
  fmt.SetLineAlignment(StringAlignmentCenter);
  SolidBrush text_brush(Color(255, 230, 230, 230));
  RectF rf((REAL)x, (REAL)y, (REAL)w, (REAL)h);
  g.DrawString(btn.text.c_str(), -1, &text_font, rf, &fmt, &text_brush);
}

void DrawButton(Graphics& g, const Button& btn, bool focused) {
  Color fill_color(220, 20, 20, 20);
  Color border_color(255, 200, 200, 200);
  Color text_color(255, 230, 230, 230);

  if (btn.pressed) {
    fill_color = Color(255, 60, 60, 60);
  } else if (btn.hovered || focused) {
    fill_color = Color(240, 50, 50, 50);
    border_color = Color(255, 240, 240, 240);
  }

  Rect r(btn.rect.left, btn.rect.top, btn.rect.right - btn.rect.left,
         btn.rect.bottom - btn.rect.top);
  SolidBrush bg(fill_color);
  g.FillRectangle(&bg, r);
  Pen pen(border_color, 1.5f);
  g.DrawRectangle(&pen, r);

  auto ff = MakeFontFamilyWithFallback({L"Bahnschrift", L"Segoe UI", L"Tahoma"});
  Font font(ff.get(), 18, FontStyleBold, UnitPixel);
  StringFormat fmt;
  fmt.SetAlignment(StringAlignmentCenter);
  fmt.SetLineAlignment(StringAlignmentCenter);
  RectF rf((REAL)r.X, (REAL)r.Y, (REAL)r.Width, (REAL)r.Height);
  SolidBrush text_brush(text_color);
  g.DrawString(btn.text.c_str(), -1, &font, rf, &fmt, &text_brush);
}

void OnPaint(HWND hwnd) {
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(hwnd, &ps);

  RECT rc;
  GetClientRect(hwnd, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;

  HDC mem_dc = CreateCompatibleDC(hdc);
  HBITMAP mem_bm = CreateCompatibleBitmap(hdc, w, h);
  HBITMAP old_bm = (HBITMAP)SelectObject(mem_dc, mem_bm);

  {
    Graphics g(mem_dc);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // Background banner — fill window.
    if (g_banner_bitmap) {
      UINT bw = g_banner_bitmap->GetWidth();
      UINT bh = g_banner_bitmap->GetHeight();
      // Scale to fill window preserving aspect (cover).
      float scale = (float)w / (float)bw;
      float scale_h = (float)h / (float)bh;
      if (scale_h > scale) scale = scale_h;
      int dw = (int)(bw * scale);
      int dh = (int)(bh * scale);
      int dx = (w - dw) / 2;
      int dy = (h - dh) / 2;
      g.DrawImage(g_banner_bitmap.get(), dx, dy, dw, dh);
    } else {
      SolidBrush bg(Color(255, 12, 14, 18));
      g.FillRectangle(&bg, 0, 0, w, h);
    }

    // Soft darkening only at very bottom for button area, smooth fade.
    LinearGradientBrush grad(Point(0, h - 80), Point(0, h),
                             Color(0, 0, 0, 0), Color(160, 0, 0, 0));
    g.FillRectangle(&grad, 0, h - 80, w, 80);

    // Logo — top-left, large, above "HE HAS TO PAY" graffiti.
    if (g_logo_bitmap) {
      UINT lw = g_logo_bitmap->GetWidth();
      UINT lh = g_logo_bitmap->GetHeight();
      const int target_w = (int)(w * 0.42f);  // ~540 px at 1280-wide window
      float scale = (float)target_w / (float)lw;
      int dw = (int)(lw * scale);
      int dh = (int)(lh * scale);
      int dx = (int)(w * 0.04f);              // ~50 px from left
      int dy = (int)(h * 0.06f);              // ~45 px from top
      g.DrawImage(g_logo_bitmap.get(), dx, dy, dw, dh);
    }

    // Bottom-left hint.
    {
      auto ff = MakeFontFamilyWithFallback({L"Segoe UI", L"Tahoma", L"Arial"});
      Font font(ff.get(), 14, FontStyleRegular, UnitPixel);
      SolidBrush brush(Color(200, 220, 220, 220));
      std::wstring hint = TrW("D-Pad: Select Option   A: Confirm");
      g.DrawString(hint.c_str(), -1, &font,
                   PointF(20.0f, (REAL)(h - 32)), &brush);
    }
    // Top-right corner: small version label.
    {
      auto ff = MakeFontFamilyWithFallback({L"Segoe UI", L"Tahoma", L"Arial"});
      Font font(ff.get(), 12, FontStyleRegular, UnitPixel);
      SolidBrush brush(Color(150, 200, 200, 200));
      StringFormat fmt;
      fmt.SetAlignment(StringAlignmentFar);
      RectF rf(0.0f, 12.0f, (REAL)w - 20.0f, 20.0f);
      std::wstring corner_text =
          std::wstring(L"DPLauncher ") + kLauncherVersion + L"  —  github.com/LittleBitUA/DPRecomp";
      g.DrawString(corner_text.c_str(),
                   -1, &font, rf, &fmt, &brush);
    }

    // Buttons (with the update banner painted via a separate pill renderer).
    for (size_t i = 0; i < g_buttons.size(); ++i) {
      const bool focused = (int)i == g_focused_button;
      if (g_buttons[i].id == kBtnUpdate) {
        DrawUpdateBanner(g, g_buttons[i], focused);
      } else {
        DrawButton(g, g_buttons[i], focused);
      }
    }
  }

  BitBlt(hdc, 0, 0, w, h, mem_dc, 0, 0, SRCCOPY);

  SelectObject(mem_dc, old_bm);
  DeleteObject(mem_bm);
  DeleteDC(mem_dc);

  EndPaint(hwnd, &ps);
}

bool PointInRect(int x, int y, const RECT& r) {
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// Shared dark background brush for the update dialogs (was also used by the
// Downpour ISO-extraction dialog, which this launcher no longer has).
static HBRUSH g_extract_bg_brush = nullptr;

// PLAY: verify the extracted game data is present, then start the runtime.
// Deadly Premonition has no title update and this launcher ships no ISO
// installer — the user copies the extracted game files into assets/ by hand.
void LaunchGame(HWND hwnd) {
  std::wstring exe_dir = GetExeDir();
  std::wstring marker  = exe_dir + L"\\assets\\default.xex";
  // No game data yet: start the game anyway - its built-in first-run installer
  // asks for the user's disc image, extracts it into assets\ and then starts
  // the build matching the disc region (2026-09-06; the previous "game data
  // not found" error box is gone).

  // Region pick (2026-09-06): the PAL and USA discs ship different executables,
  // so the release carries two recompiled builds. Choose by the size of the
  // user's default.xex (PAL 10,113,024 bytes; USA 10,080,256 bytes). Unknown
  // sizes fall back to the PAL build, which also hosts the first-run installer.
  std::wstring exe = exe_dir + L"\\" + kGameExeName;
  {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(marker.c_str(), GetFileExInfoStandard, &fad)) {
      const unsigned long long size =
          (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
      if (size == 10080256ull) {
        std::wstring usa = exe_dir + L"\\deadlyprem_usa.exe";
        if (GetFileAttributesW(usa.c_str()) != INVALID_FILE_ATTRIBUTES) {
          exe = usa;
        }
      }
    }
  }
  std::wstring args = L"--game_data_root assets";
  std::wstring cmdline = L"\"" + exe + L"\" " + args;

  // v1.1 (DPRecomp #13): optional Steam overlay opt-out for users who added
  // the launcher to Steam. The child inherits our environment.
  {
    ReadLauncherIni();
    auto it = g_launcher_ini.find("launcher_steam_overlay");
    if (it != g_launcher_ini.end() && it->second == "off") {
      SetEnvironmentVariableW(L"SteamNoOverlayUIDrawing", L"1");
    }
  }

  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi = {};
  std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
  cmd_buf.push_back(0);

  if (CreateProcessW(exe.c_str(), cmd_buf.data(), nullptr, nullptr, FALSE,
                     0, nullptr, exe_dir.c_str(), &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    PostMessage(hwnd, WM_CLOSE, 0, 0);
  } else {
    MessageBoxW(hwnd,
                TrC("Failed to launch deadlyprem.exe.\nMake sure it exists next to PlayDeadlyPremonition.exe."),
                kWindowTitle, MB_ICONERROR | MB_OK);
  }
}

void OpenSettings(HWND hwnd);
static void RunUpdateFlow(HWND hwnd);

void HandleButton(HWND hwnd, int id) {
  switch (id) {
    case kBtnPlay:
      LaunchGame(hwnd);
      break;
    case kBtnSettings:
      OpenSettings(hwnd);
      break;
    case kBtnExit:
      PostMessage(hwnd, WM_CLOSE, 0, 0);
      break;
    case kBtnUpdate:
      RunUpdateFlow(hwnd);
      break;
  }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      RECT rc;
      GetClientRect(hwnd, &rc);
      LayoutButtons(rc.right - rc.left, rc.bottom - rc.top);
      return 0;
    }
    case WM_SIZE: {
      LayoutButtons(LOWORD(lp), HIWORD(lp));
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    // Posted by ProbeReleaseAsync when a newer GitHub release is found.
    // Re-runs LayoutButtons on the UI thread so the update banner button
    // gets appended to g_buttons before the next WM_PAINT.
    case WM_APP + 0: {
      RECT rc;
      GetClientRect(hwnd, &rc);
      LayoutButtons(rc.right - rc.left, rc.bottom - rc.top);
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      OnPaint(hwnd);
      return 0;
    case WM_MOUSEMOVE: {
      int x = GET_X_LPARAM(lp);
      int y = GET_Y_LPARAM(lp);
      bool any_change = false;
      for (auto& b : g_buttons) {
        bool h = PointInRect(x, y, b.rect);
        if (h != b.hovered) {
          b.hovered = h;
          any_change = true;
        }
      }
      if (any_change) InvalidateRect(hwnd, nullptr, FALSE);
      TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
      TrackMouseEvent(&tme);
      return 0;
    }
    case WM_MOUSELEAVE: {
      for (auto& b : g_buttons) b.hovered = false;
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      int x = GET_X_LPARAM(lp);
      int y = GET_Y_LPARAM(lp);
      for (auto& b : g_buttons) {
        if (PointInRect(x, y, b.rect)) {
          b.pressed = true;
          SetCapture(hwnd);
          InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      int x = GET_X_LPARAM(lp);
      int y = GET_Y_LPARAM(lp);
      ReleaseCapture();
      int clicked = 0;
      for (auto& b : g_buttons) {
        bool was = b.pressed;
        b.pressed = false;
        if (was && PointInRect(x, y, b.rect)) clicked = b.id;
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      if (clicked) HandleButton(hwnd, clicked);
      return 0;
    }
    case WM_KEYDOWN: {
      if (wp == VK_LEFT || wp == VK_UP) {
        g_focused_button = (g_focused_button + (int)g_buttons.size() - 1) % (int)g_buttons.size();
        InvalidateRect(hwnd, nullptr, FALSE);
      } else if (wp == VK_RIGHT || wp == VK_DOWN || wp == VK_TAB) {
        g_focused_button = (g_focused_button + 1) % (int)g_buttons.size();
        InvalidateRect(hwnd, nullptr, FALSE);
      } else if (wp == VK_RETURN || wp == VK_SPACE) {
        if (g_focused_button >= 0 && g_focused_button < (int)g_buttons.size()) {
          HandleButton(hwnd, g_buttons[g_focused_button].id);
        }
      } else if (wp == VK_ESCAPE) {
        PostMessage(hwnd, WM_CLOSE, 0, 0);
      }
      return 0;
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===== Settings dialog =====
// Simple cvar list editor for deadlyprem.toml.

enum CvarCategory {
  kCatGraphics = 0,
  kCatAdvanced = 1,
  kCatMouse = 2,
  kCatControls = 3,
  kCatDebug = 4,
  // Auto-managed cvars not surfaced in the Settings UI. Saved/loaded
  // alongside the rest, but never get a label or control built for them.
  kCatHidden = 5,
};

struct CvarRow {
  std::string key;
  std::string display_name;
  std::string description;
  enum Kind { kBool, kInt, kFloat, kString, kEnum } kind;
  std::string value;
  CvarCategory category = kCatGraphics;
  // For ints / floats:
  double min_val = 0;
  double max_val = 0;
  // For enum:
  std::vector<std::pair<std::string, std::string>> options;  // value -> label
};

std::vector<CvarRow> g_cvars;

// Preserves raw lines for TOML keys we don't know about (e.g. cvars added by
// the SDK's F4 overlay or hand-edited by power users). Without this, SaveToml
// would silently drop them on every round-trip.
std::vector<std::string> g_unknown_toml_lines;

void AddCvar(const char* key, const char* display, CvarCategory cat,
             CvarRow::Kind kind, const std::string& def,
             std::vector<std::pair<std::string, std::string>> options = {},
             double min_v = 0, double max_v = 0, const char* desc = "") {
  CvarRow r;
  r.key = key;
  r.display_name = display;
  r.category = cat;
  r.kind = kind;
  r.value = def;
  r.options = std::move(options);
  r.min_val = min_v;
  r.max_val = max_v;
  r.description = desc;
  g_cvars.push_back(r);
}

// Cvar catalog for deadlyprem.toml.
//
// Every key below exists in the upstream ReXGlue nightly 0.10 SDK
// (REXCVAR_DEFINE_* under rexglue-sdk/src) — verified 2026-09-05 — except
// `launcher_language`, which is launcher-only and stored in launcher.ini.
// Defaults mirror DPRecomp/deadlyprem.toml.dist-default; keys the dist file
// doesn't mention carry the SDK's own default so seeding them is a no-op for
// the game. The keybind layout is the Director's Cut PC keymap and must be
// preserved as-is.
std::string Narrow(const std::wstring& s);

// v1.1.1: the "monitor" cvar is an SDL display index (1-based, 0 = let the
// OS decide). SDL on Windows lists the primary display first and the rest in
// EnumDisplayMonitors order, so we mirror that to build a drop-down instead
// of a bare 0..16 slider.
static std::vector<std::pair<std::string, std::string>> BuildMonitorOptions(
    const std::string& current) {
  struct Mon { bool primary; int w, h; std::wstring dev; };
  std::vector<Mon> mons;
  EnumDisplayMonitors(nullptr, nullptr,
      [](HMONITOR hm, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hm, &mi)) {
          auto* v = reinterpret_cast<std::vector<Mon>*>(lp);
          v->push_back({(mi.dwFlags & MONITORINFOF_PRIMARY) != 0,
                        mi.rcMonitor.right - mi.rcMonitor.left,
                        mi.rcMonitor.bottom - mi.rcMonitor.top, mi.szDevice});
        }
        return TRUE;
      }, reinterpret_cast<LPARAM>(&mons));
  std::stable_partition(mons.begin(), mons.end(), [](const Mon& m) { return m.primary; });
  std::vector<std::pair<std::string, std::string>> opts;
  opts.push_back({"0", "Default (primary)"});
  for (size_t i = 0; i < mons.size(); ++i) {
    std::wstring dev = mons[i].dev;
    const size_t slash = dev.find_last_of(L'\\');
    if (slash != std::wstring::npos) dev = dev.substr(slash + 1);
    std::string label = std::to_string(i + 1) + ": " + Narrow(dev.c_str()) + "  " +
                        std::to_string(mons[i].w) + "x" + std::to_string(mons[i].h) +
                        (mons[i].primary ? "  (primary)" : "");
    opts.push_back({std::to_string(i + 1), label});
  }
  bool known = false;
  for (const auto& o : opts) known = known || o.first == current;
  if (!known && !current.empty()) opts.push_back({current, current + ": (not connected)"});
  return opts;
}

void DefineCvars() {
  g_cvars.clear();
  using K = CvarRow;

  // ===== GRAPHICS =====
  AddCvar("render_target_path_d3d12", "Render Target Path", kCatGraphics,
          K::kEnum, "rov",
          {{"rov", "ROV (recommended)"},
           {"rtv", "RTV (compatibility)"},
           {"",    "Auto (SDK default)"}});
  // SDK uses integer scales of the 1280x720 guest resolution (range 1..8);
  // the launcher exposes 1..4. Requires restart.
  AddCvar("resolution_scale", "Internal Resolution Scale", kCatGraphics,
          K::kEnum, "2",
          {{"1", "1x — 1280x720 internal"},
           {"2", "2x — 2560x1440 internal (recommended)"},
           {"3", "3x — 3840x2160 internal"},
           {"4", "4x — 5120x2880 internal (slowest)"}});
  AddCvar("native_2x_msaa", "Native 2x MSAA", kCatGraphics, K::kBool, "true");
  // SDK default 3 (= force 4x). Hot-reloadable.
  AddCvar("anisotropic_override", "Anisotropic Filtering", kCatGraphics,
          K::kEnum, "3",
          {{"-1", "Game default"}, {"0", "Off"},
           {"1", "1x"}, {"2", "2x"}, {"3", "4x"}, {"4", "8x"}, {"5", "16x"}});
  AddCvar("swap_post_effect", "Post-process Anti-Aliasing", kCatGraphics,
          K::kEnum, "fxaa",
          {{"none", "Off (sharp, more aliasing)"},
           {"fxaa", "FXAA (recommended)"},
           {"fxaa_extreme", "FXAA Extreme (heavier blur, hides specks)"}});
  // Presenter output effect (FidelityFX build). Requires restart.
  AddCvar("present_effect", "Upscaler / Sharpener", kCatGraphics,
          K::kEnum, "fsr3",
          {{"bilinear", "Bilinear (off)"},
           {"cas",      "AMD CAS (sharpening)"},
           {"fsr",      "AMD FSR 1 (spatial)"},
           {"fsr2",     "AMD FSR 2 (temporal)"},
           {"fsr3",     "AMD FSR 3 (temporal+)"}});
  AddCvar("present_fsr_quality_mode", "FSR Quality Mode", kCatGraphics,
          K::kEnum, "nativeaa",
          {{"auto", "Auto"},
           {"nativeaa", "Native AA"},
           {"quality", "Quality"},
           {"balanced", "Balanced"},
           {"performance", "Performance"},
           {"ultra_performance", "Ultra Performance"}});
  // RCAS sharpness for present_effect = fsr/fsr2/fsr3: sharpness = 1 - r * 0.5
  // (SDK default 0.2 = 0.9, reported "way too sharp" on DP 2026-09-06; the
  // dist toml pins 1.6 = 0.2). Hot-reloadable in-game via F4 too.
  AddCvar("present_fsr_sharpness_reduction", "FSR Softness (0 = sharpest)",
          kCatGraphics, K::kFloat, "1.6", {}, 0.0, 2.0);
  // Extra sharpening blend for present_effect = cas only.
  AddCvar("present_cas_additional_sharpness", "CAS Extra Sharpness",
          kCatGraphics, K::kFloat, "0.0", {}, 0.0, 1.0);
  AddCvar("present_letterbox", "Preserve Aspect (Letterbox)", kCatGraphics,
          K::kBool, "true");
  // DP1 2026-09-06: ehw's 60 FPS patch (DPRecomp #3) ported to the PAL XEX as
  // mid-asm hooks; the cvar toggles them at runtime (hot-reload).
  AddCvar("dp_60fps", "60 FPS (ehw patch)", kCatGraphics, K::kBool, "true");
  // v1.1: PSO cache (Downpour port). Indicator badge while pipelines compile;
  // driver PSO blob library so the second launch skips the driver compiles.
  AddCvar("show_shader_compile_indicator", "Shader Compile Indicator",
          kCatGraphics, K::kBool, "true");
  AddCvar("shader_compile_indicator_verbose", "Shader Indicator: Verbose",
          kCatGraphics, K::kBool, "false");
  AddCvar("d3d12_pso_library_enable", "PSO Library (disk cache)",
          kCatGraphics, K::kBool, "true");
  AddCvar("vsync", "VSync", kCatGraphics, K::kBool, "true");
  // dist-default ships windowed for bring-up.
  AddCvar("fullscreen", "Fullscreen", kCatGraphics, K::kBool, "false");
  // Ported from the Downpour launcher (2026-09-05). Nightly SDK: window size
  // 0 = app default (kRequiresRestart), monitor index 0 = default.
  AddCvar("window_width", "Window Width (0 = auto)", kCatGraphics,
          K::kInt, "0", {}, 0, 8192);
  AddCvar("window_height", "Window Height (0 = auto)", kCatGraphics,
          K::kInt, "0", {}, 0, 8192);
  AddCvar("monitor", "Monitor", kCatGraphics, K::kEnum, "0",
          BuildMonitorOptions("0"));
  // SDK default true. Downpour shipped false after an NVIDIA report where
  // vsync was ignored while tearing was allowed - flip here if that recurs.
  AddCvar("d3d12_allow_variable_refresh_rate_and_tearing",
          "Allow VRR / Tearing", kCatGraphics,
          K::kBool, "true");
  AddCvar("present_dither", "Output Dithering", kCatGraphics,
          K::kBool, "false");

  // ===== ADVANCED =====
  // Launcher-only: persisted in launcher.ini, never written to toml (the SDK
  // would log it as an unknown cvar).
  AddCvar("launcher_language", "Launcher Language", kCatAdvanced,
          K::kEnum, "en",
          {{"en", "English"}, {"uk", "Ukrainian"}});
  // v1.1 (DPRecomp #13): launcher-only, persisted in launcher.ini. "off" sets
  // SteamNoOverlayUIDrawing=1 for the game process, which stops the Steam
  // overlay (GameOverlayRenderer64.dll) from drawing into our swap chain when
  // the launcher was added to Steam as a non-Steam game.
  AddCvar("launcher_steam_overlay", "Steam Overlay", kCatAdvanced,
          K::kEnum, "on",
          {{"on", "On (Steam default)"}, {"off", "Off (fixes black screen / tinted quarter frame)"}});
  // v1.1: opt-in shader cache sharing (launcher-only, see kShaderCacheWebhookUrl).
  if (kShaderCacheWebhookUrl[0] != L'\0') {
    AddCvar("launcher_share_shader_cache", "Share Shader Cache", kCatAdvanced,
            K::kEnum, "off",
            {{"off", "Off"}, {"on", "On - send my shader cache (anonymous) to the project"}});
  }
  AddCvar("input_backend", "Input Backend", kCatAdvanced,
          K::kEnum, "sdl",
          {{"sdl", "SDL (recommended, DualSense support)"},
           {"xinput", "XInput (Xbox controllers only)"}});
  // v1.1.1: texture dump / replacement (rexglue-sdk texture/replacement.cpp).
  AddCvar("texture_dump", "Texture Dump (textures\\dump)", kCatAdvanced, K::kBool, "false");
  AddCvar("texture_replacement", "", kCatHidden, K::kBool, "true");
  AddCvar("texture_path", "", kCatHidden, K::kString, "");
  AddCvar("hid_mappings_file", "Controller Mappings (SDL)", kCatAdvanced,
          K::kString, "gamecontrollerdb.txt");
  // Ported from the Downpour launcher (2026-09-05). Xbox 360 XLanguage IDs
  // (SDK cvar user_language, UINT32): the PAL disc carries EN/DE/FR/ES/IT.
  AddCvar("user_language", "Game Language", kCatAdvanced,
          K::kEnum, "1",
          {{"1", "English"},
           {"3", "German (Deutsch)"},
           {"4", "French (Francais)"},
           {"5", "Spanish (Espanol)"},
           {"6", "Italian (Italiano)"}});
  // d3d12_adapter is INT32 (-1 = any physical adapter); numeric enum so the
  // negative default doesn't go through the trackbar.
  AddCvar("d3d12_adapter", "GPU Adapter", kCatAdvanced,
          K::kEnum, "-1",
          {{"-1", "Auto (first physical GPU)"},
           {"0", "Adapter 0"},
           {"1", "Adapter 1"},
           {"2", "Adapter 2"}});
  AddCvar("async_shader_compilation", "Async Shader Compilation", kCatAdvanced,
          K::kBool, "true");
  // SDK defaults (384/768 MB); the nightly has no VRAM auto-tune, so these
  // are exact. Raise on 8 GB+ cards if textures pop at 3x-4x scale.
  AddCvar("texture_cache_memory_limit_soft", "Texture Cache Soft Limit (MB)",
          kCatAdvanced, K::kInt, "384", {}, 64, 4096);
  AddCvar("texture_cache_memory_limit_hard", "Texture Cache Hard Limit (MB)",
          kCatAdvanced, K::kInt, "768", {}, 128, 8192);
  AddCvar("audio_mute", "Mute Game Audio", kCatAdvanced, K::kBool, "false");

  // ===== MOUSE =====
  AddCvar("mnk_mode", "Mouse & Keyboard Mode", kCatMouse, K::kBool, "true");
  // v1.1.1: launcher-only (launcher.ini). Draws the bound keys onto the
  // button-prompt atlas through the runtime texture overlay (see
  // GenerateKeyPromptOverlay).
  AddCvar("launcher_prompt_style", "Button Prompts", kCatControls, K::kEnum, "keyboard",
          {{"keyboard", "Keyboard (keys from your bindings)"},
           {"xbox", "Xbox (original icons)"},
           {"ps_fullsolid", "PlayStation - solid (DualShock / DualSense)"},
           {"ps_solid", "PlayStation - solid with ring"},
           {"ps_outline", "PlayStation - outline"}});
  // DP1 2026-09-06: direct camera control via a game hook (no stick
  // emulation). When on, the stick mapping below is bypassed.
  AddCvar("dp_mouse_camera", "Mouse Camera Hook (direct)",
          kCatMouse, K::kBool, "true");
  AddCvar("dp_mouse_camera_sensitivity", "Camera Hook Sensitivity",
          kCatMouse, K::kFloat, "0.003", {}, 0.0005, 0.05);
  AddCvar("dp_mouse_camera_invert_y", "Camera Hook Invert Y",
          kCatMouse, K::kBool, "false");
  // v1.1 (DPRecomp #11): position control instead of a one-frame stick-like
  // nudge. "direct" turns the camera yaw state itself, "catch-up" keeps the
  // angle the camera has not reached yet pending, "hold" = idle time after
  // which the pending angle is released to the game's auto-centering.
  AddCvar("dp_mouse_camera_direct", "Camera Hook: Direct Yaw",
          kCatMouse, K::kBool, "true");
  AddCvar("dp_mouse_camera_catchup", "Camera Hook: Catch-up",
          kCatMouse, K::kBool, "true");
  AddCvar("dp_mouse_camera_hold_ms", "Camera Auto-center Hold (ms)",
          kCatMouse, K::kInt, "120", {}, 0, 2000);
  AddCvar("mnk_mouse", "Mouse as Right Stick", kCatMouse,
          K::kBool, "true");
  AddCvar("mnk_sensitivity", "Mouse Sensitivity",
          kCatMouse, K::kFloat, "1.0", {}, 0.05, 5.0);
  // DP1 SDK port (2026-09-06) of the Downpour direct mouse mapping: stick =
  // floor + |delta| * sensitivity * stick_scale. The floor sits above the
  // game's inner right-stick deadzone so small mouse moves are not discarded.
  AddCvar("mnk_stick_scale", "Stick Scale (units per pixel)",
          kCatMouse, K::kFloat, "300.0", {}, 10.0, 5000.0);
  AddCvar("mnk_deadzone_floor", "Deadzone Floor (stick units)",
          kCatMouse, K::kInt, "8689", {}, 0, 32767);
  AddCvar("mnk_invert_y", "Invert Mouse Y", kCatMouse, K::kBool, "false");
  // v1.1: stick-shake QTEs on a keyboard - hold A + D to oscillate the stick.
  AddCvar("mnk_key_stick_ramp_ms", "Key Stick Ramp (ms)", kCatMouse, K::kFloat, "60.0", {}, 0.0, 300.0);
  AddCvar("mnk_auto_shake", "Auto-shake (hold A + D)", kCatMouse, K::kBool, "false");
  AddCvar("mnk_auto_shake_hz", "Auto-shake Rate (Hz)", kCatMouse, K::kFloat, "12.0", {}, 2.0, 30.0);

  // ===== DUALSENSE ADAPTIVE TRIGGERS =====
  // Applied by the SDL input driver on every DualSense connect event; other
  // controllers ignore these. Defaults = SDK defaults.
  AddCvar("dualsense_adaptive_triggers", "DualSense Adaptive Triggers",
          kCatControls, K::kBool, "true");
  AddCvar("dualsense_rt_mode", "Right Trigger Effect Mode",
          kCatControls, K::kEnum, "weapon",
          {{"off", "Off (pass-through)"},
           {"feedback", "Feedback (constant resistance)"},
           {"weapon", "Weapon (click point — gun trigger feel)"},
           {"vibration", "Vibration (buzz on pull)"}});
  AddCvar("dualsense_rt_start", "Right Trigger Start Position",
          kCatControls, K::kInt, "3", {}, 0, 9);
  AddCvar("dualsense_rt_end", "Right Trigger End Position",
          kCatControls, K::kInt, "6", {}, 0, 9);
  AddCvar("dualsense_rt_strength", "Right Trigger Strength",
          kCatControls, K::kInt, "5", {}, 0, 8);
  AddCvar("dualsense_lt_mode", "Left Trigger Effect Mode",
          kCatControls, K::kEnum, "feedback",
          {{"off", "Off (pass-through)"},
           {"feedback", "Feedback (constant resistance)"},
           {"weapon", "Weapon (click point — gun trigger feel)"},
           {"vibration", "Vibration (buzz on pull)"}});
  AddCvar("dualsense_lt_start", "Left Trigger Start Position",
          kCatControls, K::kInt, "2", {}, 0, 9);
  AddCvar("dualsense_lt_end", "Left Trigger End Position",
          kCatControls, K::kInt, "0", {}, 0, 9);
  AddCvar("dualsense_lt_strength", "Left Trigger Strength",
          kCatControls, K::kInt, "4", {}, 0, 8);

  // ===== KEYBINDS (Director's Cut PC layout, from deadlyprem.toml.dist-default) =====
  // "E,Space+LMB" = interact on E, or fire when Space (aim) and LMB are held
  // together. Space+LMB / WheelUp / WheelDown rely on the DP1 combo/wheel port
  // in the SDK.
  AddCvar("keybind_a", "A button (action / fire)",
          kCatControls, K::kString, "E,Space+LMB");
  AddCvar("keybind_b", "B button",
          kCatControls, K::kString, "R");
  AddCvar("keybind_x", "X button",
          kCatControls, K::kString, "Shift");
  AddCvar("keybind_y", "Y button",
          kCatControls, K::kString, "F");
  AddCvar("keybind_left_trigger", "Left Trigger",
          kCatControls, K::kString, "Control");
  AddCvar("keybind_right_trigger", "Right Trigger (aim)",
          kCatControls, K::kString, "Space");
  AddCvar("keybind_left_shoulder", "Left Shoulder",
          kCatControls, K::kString, "Z");
  AddCvar("keybind_right_shoulder", "Right Shoulder",
          kCatControls, K::kString, "X");
  AddCvar("keybind_lstick_press", "Left Stick Press",
          kCatControls, K::kString, "C");
  // Not in the dist file — SDK default.
  AddCvar("keybind_rstick_press", "Right Stick Press",
          kCatControls, K::kString, "K");
  AddCvar("keybind_lstick_up", "Move Forward",
          kCatControls, K::kString, "W");
  AddCvar("keybind_lstick_down", "Move Backward",
          kCatControls, K::kString, "S");
  AddCvar("keybind_lstick_left", "Strafe Left",
          kCatControls, K::kString, "A");
  AddCvar("keybind_lstick_right", "Strafe Right",
          kCatControls, K::kString, "D");
  AddCvar("keybind_dpad_up", "D-Pad Up",
          kCatControls, K::kString, "WheelUp");
  AddCvar("keybind_dpad_down", "D-Pad Down",
          kCatControls, K::kString, "WheelDown");
  // Not in the dist file — SDK defaults.
  AddCvar("keybind_dpad_left", "D-Pad Left",
          kCatControls, K::kString, "Shift+Left");
  AddCvar("keybind_dpad_right", "D-Pad Right",
          kCatControls, K::kString, "Shift+Right");
  AddCvar("keybind_back", "Back",
          kCatControls, K::kString, "M");
  AddCvar("keybind_start", "Start (pause menu)",
          kCatControls, K::kString, "Return");

  // ===== HIDDEN: pinned by dist-default, not shown in the UI =====
  // GPU emulation plugin (rexgpu-xenosrd.dll next to the exe).
  AddCvar("gpu_plugin", "", kCatHidden, K::kString, "xenos");
  // DP's game logic ticks per vblank — keep 60 Hz.
  AddCvar("video_mode_refresh_rate", "", kCatHidden, K::kFloat, "60.0",
          {}, 24.0, 240.0);
  // Correctness pin carried from the Downpour soft-fork (true = rainbow noise).
  AddCvar("gpu_allow_invalid_fetch_constants", "", kCatHidden, K::kBool, "false");

  // ===== DEBUG / DIAGNOSTICS =====
  AddCvar("log_level", "Log Level", kCatDebug,
          K::kEnum, "info",
          {{"off",   "Off (no logs)"},
           {"error", "Error"},
           {"warn",  "Warn"},
           {"info",  "Info (recommended)"},
           {"debug", "Debug (verbose)"},
           {"trace", "Trace (very verbose)"}});
  // dist-default pins this on; exposed so it can be A/B'd, but keep it on.
  AddCvar("readback_memexport", "Memexport Readback (keep on)",
          kCatDebug, K::kBool, "true");
  AddCvar("occlusion_query_enable", "Occlusion Queries",
          kCatDebug, K::kBool, "true");
  // Ported from the Downpour launcher's Debug tab (2026-09-05), limited to
  // cvars that exist in the nightly SDK.
  // DP1 SDK port of the Downpour PSO handling (2026-09-06): 'block' waits per
  // draw (budgeted) so geometry doesn't pop in while shaders compile.
  AddCvar("pso_missing_policy", "PSO Missing Policy", kCatDebug,
          K::kEnum, "block",
          {{"block", "Block (wait per draw, budgeted — recommended)"},
           {"skip",  "Skip (no block, pop-in on miss)"},
           {"sync",  "Sync (inline compile, longest stutter)"}});
  AddCvar("d3d12_pso_block_per_draw_budget_ms", "PSO Block Budget (ms)",
          kCatDebug, K::kFloat, "8.0", {}, 0.5, 500.0);
  AddCvar("d3d12_pso_no_block_at_submission_end", "No PSO Wait At Frame End",
          kCatDebug, K::kBool, "true");
  AddCvar("d3d12_debug", "D3D12 Debug Layer (slow)", kCatDebug,
          K::kBool, "false");
  AddCvar("store_shaders", "Shader Storage Cache", kCatDebug,
          K::kBool, "true");
  AddCvar("log_max_files", "Log Files To Keep", kCatDebug,
          K::kInt, "20", {}, 1, 100);
  AddCvar("log_max_file_size_mb", "Log File Size Limit (MB)", kCatDebug,
          K::kInt, "5", {}, 1, 100);
}

// Seeds perf-related cvars into an existing deadlyprem.toml that pre-dates the
// auto-config logic. Only appends MISSING keys — never overwrites a user's
// explicit value. Runs once at launcher startup.
UpdateStrings BuildUpdateStrings() {
  UpdateStrings s;
  s.banner_text     = TrW("Update available: ") + g_update_tag +
                      TrW(" — click to install");
  s.confirm_title   = TrW("Update available: ") + g_update_tag;
  s.confirm_body    = TrW("Download and install update?");
  s.progress_title  = TrW("Downloading update");
  s.failure_title   = TrW("Update failed");
  s.failure_body    = TrW("Could not download the update zip. Check your internet connection.");
  s.no_asset_body   = TrW("The latest release contains no DPRecomp zip asset.");
  return s;
}

// ============================================================================
// Update probe — background WinHTTP GET against the GitHub releases API.
// ============================================================================

// Small slice extractor: locate "<key>":  ... <value> in a flat JSON blob.
// Returns the substring between the FIRST '"' after the key and the matching
// closing '"'. Only safe for string values without escaped quotes — adequate
// for GitHub's tag_name and browser_download_url fields.
static std::string ExtractJsonString(const std::string& body, const char* key) {
  std::string needle = std::string("\"") + key + "\"";
  size_t p = body.find(needle);
  if (p == std::string::npos) return {};
  p = body.find(':', p + needle.size());
  if (p == std::string::npos) return {};
  p = body.find('"', p);
  if (p == std::string::npos) return {};
  size_t q = body.find('"', p + 1);
  if (q == std::string::npos) return {};
  return body.substr(p + 1, q - p - 1);
}

// Locate the first object inside "assets":[ { ... } ] whose browser_download_url
// ends in ".zip" and carries the "DPRecomp" substring (release zips must be
// named that way for the auto-updater to pick them up).
static std::string FindZipAssetUrl(const std::string& body) {
  size_t assets_p = body.find("\"assets\"");
  if (assets_p == std::string::npos) return {};
  size_t scan = assets_p;
  while (true) {
    size_t url = body.find("\"browser_download_url\"", scan);
    if (url == std::string::npos) break;
    size_t colon = body.find(':', url);
    if (colon == std::string::npos) break;
    size_t qa = body.find('"', colon);
    size_t qb = body.find('"', qa + 1);
    if (qa == std::string::npos || qb == std::string::npos) break;
    std::string val = body.substr(qa + 1, qb - qa - 1);
    scan = qb + 1;
    if (val.size() >= 4 &&
        val.compare(val.size() - 4, 4, ".zip") == 0 &&
        val.find("DPRecomp") != std::string::npos) {
      return val;
    }
  }
  return {};
}

// HTTPS GET into a buffer. Synchronous; intended to be called from a worker
// thread. Returns empty string on any error.
static std::string HttpsGet(const wchar_t* host, const wchar_t* path) {
  HINTERNET session = WinHttpOpen(L"DPLauncher/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return {};
  HINTERNET conn = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!conn) { WinHttpCloseHandle(session); return {}; }
  HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr,
                                     WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE);
  if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return {}; }
  bool ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr);
  std::string body;
  if (ok) {
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
      std::string chunk(avail, '\0');
      DWORD got = 0;
      if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0) break;
      body.append(chunk.data(), got);
    }
  }
  WinHttpCloseHandle(req);
  WinHttpCloseHandle(conn);
  WinHttpCloseHandle(session);
  return body;
}

// v1.1.6 fix: proper semver-style comparison. Pre-v1.1.6 the probe was a
// raw string equality check — any time the local version differed from the
// GitHub tag the banner showed "Update available", even if local was AHEAD
// (e.g. dev builds, or users who downgraded). Parses "vX.Y.Z" into integer
// tuples and compares element-wise.
static bool IsLatestNewer(const std::wstring& latest, const std::wstring& current) {
  auto parse = [](std::wstring v) -> std::vector<int> {
    if (!v.empty() && (v[0] == L'v' || v[0] == L'V')) v.erase(0, 1);
    std::vector<int> parts;
    size_t i = 0;
    while (i < v.size()) {
      size_t j = i;
      while (j < v.size() && iswdigit(v[j])) ++j;
      if (j == i) break;
      try { parts.push_back(std::stoi(v.substr(i, j - i))); }
      catch (...) { break; }
      i = j;
      if (i < v.size() && v[i] == L'.') ++i; else break;
    }
    return parts;
  };
  std::vector<int> a = parse(latest), b = parse(current);
  size_t n = std::max(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    int va = i < a.size() ? a[i] : 0;
    int vb = i < b.size() ? b[i] : 0;
    if (va != vb) return va > vb;
  }
  return false;  // equal or current is longer with extra zeros — not newer
}

// Spawn the background release-check on launcher boot. Repaints the main
// window if a newer tag is found.
static void ProbeReleaseAsync(HWND main_hwnd) {
  std::thread([main_hwnd] {
    std::string body = HttpsGet(L"api.github.com",
                                L"/repos/LittleBitUA/DPRecomp/releases/latest");
    if (body.empty()) return;
    std::string tag = ExtractJsonString(body, "tag_name");
    if (tag.empty()) return;
    std::wstring wtag(tag.begin(), tag.end());
    if (!IsLatestNewer(wtag, kLauncherVersion)) return;  // up to date or local is ahead
    std::string asset = FindZipAssetUrl(body);
    if (asset.empty()) return;  // release exists but no zip asset
    g_update_tag = wtag;
    g_update_asset_url = std::wstring(asset.begin(), asset.end());
    g_update_available.store(true, std::memory_order_release);
    // Drive a UI-thread relayout: SendMessage would deadlock if the UI
    // thread is also waiting; PostMessage queues into the message loop.
    if (main_hwnd) PostMessage(main_hwnd, WM_APP + 0, 0, 0);
  }).detach();
}

// ============================================================================
// Update download — chunked WinHTTPS GET with progress dialog.
// ============================================================================

constexpr int kIdUpdateProgress = 800;
constexpr int kIdUpdateStatus   = 801;
constexpr int kIdUpdateCancel   = 802;

constexpr int kIdConfirmBody    = 900;

// ============================================================================
// Update confirm dialog — dark-themed Yes/No to replace the default
// Windows MessageBox (which is white on Win11 and clashes with the rest of
// the launcher UI). Same look as the Settings + progress dialogs.
// ============================================================================

struct UpdateConfirmState {
  std::wstring body_text;
  HWND dlg = nullptr;
};

static INT_PTR CALLBACK UpdateConfirmDlgProc(HWND hwnd, UINT msg, WPARAM wp,
                                             LPARAM lp) {
  auto* st = reinterpret_cast<UpdateConfirmState*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<UpdateConfirmState*>(lp);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(st));
      st->dlg = hwnd;

      BOOL dark = TRUE;
      DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
      if (!g_extract_bg_brush)
        g_extract_bg_brush = CreateSolidBrush(RGB(24, 26, 30));

      HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

      RECT cr;
      GetClientRect(hwnd, &cr);
      const int dlg_w  = cr.right - cr.left;
      const int dlg_h  = cr.bottom - cr.top;
      const int margin = 16;
      const int btn_w  = 100;
      const int btn_h  = 28;
      const int btn_gap = 8;
      const int btn_y  = dlg_h - margin - btn_h;
      const int btn_total = btn_w * 2 + btn_gap;
      const int btn_x_start = (dlg_w - btn_total) / 2;

      HWND body = CreateWindowW(L"STATIC", st->body_text.c_str(),
                                WS_VISIBLE | WS_CHILD | SS_LEFT | SS_NOPREFIX,
                                margin, margin,
                                dlg_w - 2 * margin, btn_y - 2 * margin,
                                hwnd, (HMENU)(intptr_t)kIdConfirmBody,
                                nullptr, nullptr);
      SendMessageW(body, WM_SETFONT, (WPARAM)font, TRUE);

      HWND yes_btn = CreateWindowW(L"BUTTON", TrC("Yes"),
                                   WS_VISIBLE | WS_CHILD | WS_TABSTOP |
                                       BS_DEFPUSHBUTTON,
                                   btn_x_start, btn_y, btn_w, btn_h, hwnd,
                                   (HMENU)(intptr_t)IDYES, nullptr, nullptr);
      SendMessageW(yes_btn, WM_SETFONT, (WPARAM)font, TRUE);

      HWND no_btn = CreateWindowW(L"BUTTON", TrC("No"),
                                  WS_VISIBLE | WS_CHILD | WS_TABSTOP,
                                  btn_x_start + btn_w + btn_gap, btn_y,
                                  btn_w, btn_h, hwnd,
                                  (HMENU)(intptr_t)IDNO, nullptr, nullptr);
      SendMessageW(no_btn, WM_SETFONT, (WPARAM)font, TRUE);

      SetFocus(yes_btn);
      return FALSE;  // we set focus ourselves
    }
    case WM_COMMAND: {
      WORD id = LOWORD(wp);
      if (id == IDYES || id == IDNO || id == IDCANCEL) {
        EndDialog(hwnd, id == IDCANCEL ? IDNO : id);
        return TRUE;
      }
      return FALSE;
    }
    case WM_CLOSE:
      EndDialog(hwnd, IDNO);
      return TRUE;
    case WM_CTLCOLORSTATIC: {
      HDC hdc = (HDC)wp;
      SetTextColor(hdc, RGB(230, 230, 230));
      SetBkColor(hdc, RGB(24, 26, 30));
      return (INT_PTR)g_extract_bg_brush;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORBTN: {
      return (INT_PTR)g_extract_bg_brush;
    }
  }
  return FALSE;
}

struct UpdateDownloadState {
  std::wstring asset_url;
  std::wstring dest_path;
  HWND dlg = nullptr;
  HANDLE thread = nullptr;
  std::atomic<bool> cancel{false};
  std::atomic<uint64_t> bytes_done{0};
  std::atomic<uint64_t> bytes_total{0};
  std::atomic<bool> finished{false};
  std::atomic<bool> success{false};
};

// Parse https://host/path → host + path. Returns false on malformed input.
static bool SplitHttpsUrl(const std::wstring& url, std::wstring& host,
                          std::wstring& path) {
  constexpr const wchar_t* prefix = L"https://";
  if (url.compare(0, 8, prefix) != 0) return false;
  size_t host_start = 8;
  size_t slash = url.find('/', host_start);
  if (slash == std::wstring::npos) {
    host = url.substr(host_start);
    path = L"/";
  } else {
    host = url.substr(host_start, slash - host_start);
    path = url.substr(slash);
  }
  return !host.empty();
}

static DWORD WINAPI UpdateDownloadThread(LPVOID lp) {
  auto* st = static_cast<UpdateDownloadState*>(lp);
  std::wstring host, path;
  if (!SplitHttpsUrl(st->asset_url, host, path)) {
    st->finished.store(true, std::memory_order_release);
    PostMessageW(st->dlg, WM_USER + 2, 0, 0);
    return 1;
  }
  // GitHub asset URLs redirect to objects.githubusercontent.com — follow
  // redirects automatically (WinHTTP does so by default; chain depth 5).
  HINTERNET session = WinHttpOpen(L"DPLauncher/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  HINTERNET conn = session ? WinHttpConnect(session, host.c_str(),
                                            INTERNET_DEFAULT_HTTPS_PORT, 0)
                           : nullptr;
  HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE)
                       : nullptr;
  bool ok = req &&
            WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr);
  if (ok) {
    // Query Content-Length to drive the progress bar.
    DWORD len_size = sizeof(uint64_t);
    uint64_t content_len = 0;
    DWORD len_dword = 0;
    DWORD dword_size = sizeof(len_dword);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &len_dword,
                            &dword_size, WINHTTP_NO_HEADER_INDEX)) {
      content_len = len_dword;
    }
    st->bytes_total.store(content_len, std::memory_order_release);

    HANDLE f = CreateFileW(st->dest_path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      DWORD avail = 0;
      bool stream_ok = true;
      while (!st->cancel.load(std::memory_order_acquire) &&
             WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
        std::vector<char> buf(avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), avail, &got) || got == 0) {
          stream_ok = false;
          break;
        }
        DWORD written = 0;
        if (!WriteFile(f, buf.data(), got, &written, nullptr) || written != got) {
          stream_ok = false;
          break;
        }
        st->bytes_done.fetch_add(got, std::memory_order_relaxed);
        PostMessageW(st->dlg, WM_USER + 1, 0, 0);
      }
      CloseHandle(f);
      if (stream_ok && !st->cancel.load(std::memory_order_acquire)) {
        st->success.store(true, std::memory_order_release);
      } else {
        DeleteFileW(st->dest_path.c_str());
      }
    }
  }
  if (req) WinHttpCloseHandle(req);
  if (conn) WinHttpCloseHandle(conn);
  if (session) WinHttpCloseHandle(session);
  st->finished.store(true, std::memory_order_release);
  PostMessageW(st->dlg, WM_USER + 2, 0, 0);
  return 0;
}

static INT_PTR CALLBACK UpdateDownloadDlgProc(HWND hwnd, UINT msg, WPARAM wp,
                                              LPARAM lp) {
  auto* st = reinterpret_cast<UpdateDownloadState*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<UpdateDownloadState*>(lp);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(st));
      st->dlg = hwnd;

      BOOL dark = TRUE;
      DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
      if (!g_extract_bg_brush)
        g_extract_bg_brush = CreateSolidBrush(RGB(24, 26, 30));

      HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
      RECT cr;
      GetClientRect(hwnd, &cr);
      const int dlg_w  = cr.right - cr.left;
      const int dlg_h  = cr.bottom - cr.top;
      const int margin = 16;
      const int row_h  = 22;
      const int btn_w  = 110;
      const int btn_h  = 28;
      const int inner_w = dlg_w - 2 * margin;
      const int btn_x  = dlg_w - margin - btn_w;
      const int btn_y  = dlg_h - margin - btn_h;

      HWND status = CreateWindowW(L"STATIC",
                                  TrC("Update — preparing installer..."),
                                  WS_VISIBLE | WS_CHILD | SS_LEFT |
                                      SS_NOPREFIX | SS_ENDELLIPSIS,
                                  margin, margin, inner_w, row_h, hwnd,
                                  (HMENU)(intptr_t)kIdUpdateStatus,
                                  nullptr, nullptr);
      SendMessageW(status, WM_SETFONT, (WPARAM)font, TRUE);

      HWND bar = CreateWindowExW(0, PROGRESS_CLASS, nullptr,
                                  WS_VISIBLE | WS_CHILD | PBS_SMOOTH,
                                  margin, margin + row_h + 8, inner_w, row_h,
                                  hwnd, (HMENU)(intptr_t)kIdUpdateProgress,
                                  nullptr, nullptr);
      SendMessageW(bar, PBM_SETRANGE32, 0, 1000);

      HWND cancel = CreateWindowW(L"BUTTON", TrC("Cancel"),
                                   WS_VISIBLE | WS_CHILD | WS_TABSTOP,
                                   btn_x, btn_y, btn_w, btn_h, hwnd,
                                   (HMENU)(intptr_t)kIdUpdateCancel,
                                   nullptr, nullptr);
      SendMessageW(cancel, WM_SETFONT, (WPARAM)font, TRUE);

      st->thread = CreateThread(nullptr, 0, UpdateDownloadThread, st, 0, nullptr);
      return TRUE;
    }
    case WM_USER + 1: {  // progress tick
      uint64_t done  = st->bytes_done.load();
      uint64_t total = st->bytes_total.load();
      int permille = total > 0
                         ? static_cast<int>((done * 1000ULL) / total)
                         : 0;
      SendDlgItemMessageW(hwnd, kIdUpdateProgress, PBM_SETPOS, permille, 0);
      wchar_t buf[160];
      _snwprintf_s(buf, _TRUNCATE, L"%llu / %llu MB",
                   (unsigned long long)(done / (1024ULL * 1024ULL)),
                   (unsigned long long)(total / (1024ULL * 1024ULL)));
      SetDlgItemTextW(hwnd, kIdUpdateStatus, buf);
      return TRUE;
    }
    case WM_USER + 2: {  // download done (success or fail)
      WaitForSingleObject(st->thread, INFINITE);
      CloseHandle(st->thread);
      st->thread = nullptr;
      EndDialog(hwnd, st->success.load() ? IDOK : IDCANCEL);
      return TRUE;
    }
    case WM_COMMAND: {
      if (LOWORD(wp) == kIdUpdateCancel) {
        st->cancel.store(true, std::memory_order_release);
      }
      return TRUE;
    }
    case WM_CLOSE: {
      st->cancel.store(true, std::memory_order_release);
      return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
      HDC hdc = (HDC)wp;
      SetTextColor(hdc, RGB(220, 220, 220));
      SetBkColor(hdc, RGB(24, 26, 30));
      return (INT_PTR)g_extract_bg_brush;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORBTN: {
      return (INT_PTR)g_extract_bg_brush;
    }
  }
  return FALSE;
}

// Self-update PowerShell script. Hidden window via -WindowStyle Hidden when
// launched. Waits for launcher.exe to exit, extracts the zip, snapshots user
// data into a backup zip in %TEMP% (v1.1.6+ safety net), copies the host
// shell files over the existing install, leaves user data alone (assets/,
// user/, logs/, deadlyprem.toml, deadlyprem.toml.backup, launcher.ini), then
// relaunches the new PlayDeadlyPremonition.exe. The backup is named
// `dp1_user_backup_v<OLD>.zip` and survives across runs so a user can hand-
// restore if anything ever wipes their saves.
// Path the auto-update PowerShell script writes its step-by-step log to. The
// log lives in %TEMP% (matches where the script + zip stage). On a clean
// success the script removes the log; if it's still present at next launcher
// boot, the update was interrupted (corrupt zip, perms, disk-full, etc.) and
// the user is offered the log via `CheckPreviousUpdateLog`.
static std::wstring GetUpdateLogPath() {
  wchar_t buf[MAX_PATH];
  if (!GetTempPathW(MAX_PATH, buf)) return L"";
  return std::wstring(buf) + L"dp1_update.log";
}

// Path the auto-update PowerShell script packs user data into BEFORE applying
// the update (Compress-Archive of user/, deadlyprem.toml, launcher.ini next to
// the launcher). Named by the running (pre-update) launcher version so each
// release leaves its own backup zip in %TEMP%. Worst case (update wipes data)
// the user can manually extract this zip back over the install dir.
static std::wstring GetUpdateBackupPath() {
  wchar_t buf[MAX_PATH];
  if (!GetTempPathW(MAX_PATH, buf)) return L"";
  return std::wstring(buf) + L"dp1_user_backup_" + kLauncherVersion + L".zip";
}

static bool WriteUpdateScript(const std::wstring& script_path,
                              const std::wstring& zip_path,
                              const std::wstring& staging_dir,
                              const std::wstring& install_dir) {
  std::wstring log_path = GetUpdateLogPath();
  std::wstring backup_path = GetUpdateBackupPath();
  std::wstring script;
  // Stop-on-first-error so the catch block sees the actual failure instead of
  // PowerShell hiding it behind a non-zero exit code.
  script += L"$ErrorActionPreference = 'Stop'\r\n";
  script += L"$zip = '" + zip_path + L"'\r\n";
  script += L"$staging = '" + staging_dir + L"'\r\n";
  script += L"$dest = '" + install_dir + L"'\r\n";
  script += L"$log = '" + log_path + L"'\r\n";
  script += L"$backup = '" + backup_path + L"'\r\n";
  script += L"function L($m) {\r\n";
  script += L"  $line = '[' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + '] ' + $m\r\n";
  script += L"  Add-Content -Path $log -Value $line -Encoding UTF8 -ErrorAction SilentlyContinue\r\n";
  script += L"}\r\n";
  // Truncate / create log fresh at the top so a previous run's log doesn't
  // bleed into this one's diagnosis.
  script += L"Set-Content -Path $log -Value '' -Encoding UTF8 -ErrorAction SilentlyContinue\r\n";
  script += L"L ('update v' + '" + std::wstring(kLauncherVersion) + L"' + ' starting; zip=' + $zip + ' dest=' + $dest)\r\n";
  script += L"try {\r\n";
  script += L"  L 'wait: launcher to release file locks'\r\n";
  script += L"  for ($i = 0; $i -lt 40; $i++) {\r\n";
  script += L"    $p = Get-Process -Name 'PlayDeadlyPremonition' -ErrorAction SilentlyContinue\r\n";
  script += L"    if (-not $p) { break }\r\n";
  script += L"    Start-Sleep -Milliseconds 250\r\n";
  script += L"  }\r\n";
  script += L"  Start-Sleep -Milliseconds 500\r\n";
  script += L"  if (Test-Path $staging) {\r\n";
  script += L"    L \"stage: clean previous $staging\"\r\n";
  script += L"    Remove-Item -Recurse -Force $staging\r\n";
  script += L"  }\r\n";
  script += L"  L \"stage: Expand-Archive '$zip' -> '$staging'\"\r\n";
  script += L"  Expand-Archive -Path $zip -DestinationPath $staging -Force\r\n";
  script += L"  $root = $staging\r\n";
  script += L"  $inner = Get-ChildItem $staging -Directory | Select-Object -First 1\r\n";
  script += L"  if ($inner -and (Test-Path \"$($inner.FullName)\\PlayDeadlyPremonition.exe\")) { $root = $inner.FullName }\r\n";
  script += L"  L \"stage: zip root resolved to $root\"\r\n";
  // Pre-update safety net: snapshot the install dir's user data into a zip
  // in %TEMP%. If the update mis-behaves (data loss reports from real users
  // have surfaced even though the copy step below explicitly never touches
  // user/), the backup is right there for the user to restore by hand.
  // Failure to back up is logged but NOT fatal — we still want the update
  // to apply for users where compression itself fails (long paths, antivirus,
  // disk-full).
  script += L"  L 'backup: snapshot user data before copy'\r\n";
  script += L"  $items = @()\r\n";
  script += L"  foreach ($n in @('user','deadlyprem.toml','launcher.ini','deadlyprem.toml.backup')) {\r\n";
  script += L"    $p = Join-Path $dest $n\r\n";
  script += L"    if (Test-Path $p) { $items += $p }\r\n";
  script += L"  }\r\n";
  script += L"  if ($items.Count -gt 0) {\r\n";
  script += L"    try {\r\n";
  script += L"      if (Test-Path $backup) { Remove-Item -Force $backup -ErrorAction SilentlyContinue }\r\n";
  script += L"      Compress-Archive -LiteralPath $items -DestinationPath $backup -Force\r\n";
  script += L"      L ('backup: ok ' + $backup + ' (' + (Get-Item $backup).Length + ' bytes)')\r\n";
  script += L"    } catch {\r\n";
  script += L"      L ('backup FAILED (continuing without backup): ' + $_.ToString())\r\n";
  script += L"    }\r\n";
  script += L"  } else {\r\n";
  script += L"    L 'backup: no user data found, skipping'\r\n";
  script += L"  }\r\n";
  // Host shell files for the DPRecomp nightly layout (runtime + GPU plugin +
  // FidelityFX backend). Each is guarded by Test-Path below, so zips missing
  // an entry stay harmless. Keep in sync with the release zip contents.
  script += L"  $copy = @('PlayDeadlyPremonition.exe','deadlyprem.exe','rexruntimerd.dll','rexgpu-xenosrd.dll','TracyClientrd.dll','amd_fidelityfx_dx12drel.dll','deadlyprem_usa.exe','gamecontrollerdb.txt','CONTROLS.txt','CONTROLS_EN.txt','README.txt')\r\n";
  script += L"  foreach ($f in $copy) {\r\n";
  script += L"    $src = Join-Path $root $f\r\n";
  script += L"    if (Test-Path $src) {\r\n";
  script += L"      L \"copy: $f\"\r\n";
  script += L"      Copy-Item -Force $src (Join-Path $dest $f)\r\n";
  script += L"    } else {\r\n";
  script += L"      L \"skip: $f (not in zip)\"\r\n";
  script += L"    }\r\n";
  script += L"  }\r\n";
  // v1.2: whole folders shipped in the zip (icon sets, texture replacements).
  script += L"  foreach ($d in @('prompts','textures')) {\r\n";
  script += L"    $s = Join-Path $root $d\r\n";
  script += L"    if (Test-Path $s) {\r\n";
  script += L"      $t = Join-Path $dest $d\r\n";
  script += L"      New-Item -ItemType Directory -Force -Path $t | Out-Null\r\n";
  script += L"      L \"copy: $d/*\"\r\n";
  script += L"      Copy-Item -Recurse -Force \"$s\\*\" $t\r\n";
  script += L"    }\r\n";
  script += L"  }\r\n";
  script += L"  $shareable_src = Join-Path $root 'userdata\\cache\\shaders\\shareable'\r\n";
  script += L"  if (Test-Path $shareable_src) {\r\n";
  script += L"    $shareable_dst = Join-Path $dest 'userdata\\cache\\shaders\\shareable'\r\n";
  script += L"    New-Item -ItemType Directory -Force -Path $shareable_dst | Out-Null\r\n";
  script += L"    L 'copy: cache/shaders/shareable/*'\r\n";
  script += L"    Copy-Item -Recurse -Force \"$shareable_src\\*\" $shareable_dst\r\n";
  script += L"  }\r\n";
  script += L"  L 'cleanup: remove staging + zip'\r\n";
  script += L"  Remove-Item -Recurse -Force $staging -ErrorAction SilentlyContinue\r\n";
  script += L"  Remove-Item -Force $zip -ErrorAction SilentlyContinue\r\n";
  script += L"  L 'update: success -- relaunching'\r\n";
  script += L"  Start-Process -FilePath (Join-Path $dest 'PlayDeadlyPremonition.exe') -WorkingDirectory $dest\r\n";
  // Clean removal of the log marks a successful run for the next launcher boot.
  script += L"  Remove-Item -Force $log -ErrorAction SilentlyContinue\r\n";
  script += L"} catch {\r\n";
  script += L"  L ('FAILED: ' + $_.ToString())\r\n";
  script += L"  L ('stack: ' + $_.ScriptStackTrace)\r\n";
  script += L"  L 'update: aborted -- launching previous build'\r\n";
  // On failure: leave log on disk so the next launcher boot can surface it.
  // Best-effort relaunch of whatever exe is in place so the user isn't stranded.
  script += L"  Start-Process -FilePath (Join-Path $dest 'PlayDeadlyPremonition.exe') -WorkingDirectory $dest -ErrorAction SilentlyContinue\r\n";
  script += L"}\r\n";
  script += L"Remove-Item -Force $PSCommandPath -ErrorAction SilentlyContinue\r\n";
  std::ofstream f(script_path, std::ios::binary);
  if (!f) return false;
  // PowerShell reads UTF-16 LE with BOM cleanly; emit as such.
  unsigned char bom[2] = {0xFF, 0xFE};
  f.write(reinterpret_cast<const char*>(bom), 2);
  f.write(reinterpret_cast<const char*>(script.data()),
          script.size() * sizeof(wchar_t));
  return f.good();
}

// Boot-time check: if the auto-updater PS script left its log file behind in
// %TEMP%, the previous update did NOT complete cleanly (the script removes
// the log on success). Offer the user a dark Yes/No dialog: Yes opens the
// log in Notepad for diagnostic, No deletes the log and continues silently.
// Either branch leaves the launcher in a usable state — the previous exe
// was already relaunched by the script's catch block.
static void CheckPreviousUpdateLog() {
  std::wstring log_path = GetUpdateLogPath();
  if (log_path.empty()) return;
  std::ifstream probe(log_path, std::ios::binary);
  if (!probe.good()) return;
  probe.close();

  UpdateConfirmState st;
  st.body_text = TrW("The auto-updater logged an error on the last attempt. "
                     "Open the diagnostic log? (No = delete log and continue.)") +
                 L"\r\n\r\n" + log_path;

  std::vector<BYTE> ctmpl(512, 0);
  auto* cdt = reinterpret_cast<DLGTEMPLATE*>(ctmpl.data());
  cdt->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME |
               DS_CENTER | DS_SETFONT;
  cdt->dwExtendedStyle = 0;
  cdt->cdit = 0;
  cdt->x = 0; cdt->y = 0;
  cdt->cx = 260; cdt->cy = 140;
  WORD* cp = reinterpret_cast<WORD*>(cdt + 1);
  *cp++ = 0; *cp++ = 0;
  std::wstring title = TrW("Previous update did not finish");
  for (wchar_t c : title) *cp++ = (WORD)c;
  *cp++ = 0;
  *cp++ = 9;
  const wchar_t face[] = L"Segoe UI";
  for (const wchar_t* c = face; *c; ++c) *cp++ = (WORD)*c;
  *cp++ = 0;

  INT_PTR rc = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), cdt,
                                       nullptr, UpdateConfirmDlgProc,
                                       reinterpret_cast<LPARAM>(&st));
  if (rc == IDYES) {
    ShellExecuteW(nullptr, L"open", L"notepad.exe", log_path.c_str(), nullptr,
                  SW_SHOWNORMAL);
  } else {
    DeleteFileW(log_path.c_str());
  }
}

// Triggered by the update banner click. Confirms, downloads, generates the
// update script, spawns PowerShell hidden, and exits the launcher.
static void RunUpdateFlow(HWND hwnd) {
  UpdateStrings s = BuildUpdateStrings();

  // Dark-themed confirm dialog (replaces the default white Win32 MessageBox).
  UpdateConfirmState confirm_state;
  confirm_state.body_text = s.confirm_body + L"\r\n\r\n" +
                            TrW("Latest version: ") + g_update_tag + L"\r\n" +
                            TrW("Current version: ") + kLauncherVersion;
  {
    std::vector<BYTE> ctmpl(512, 0);
    auto* cdt = reinterpret_cast<DLGTEMPLATE*>(ctmpl.data());
    cdt->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME |
                 DS_CENTER | DS_SETFONT;
    cdt->dwExtendedStyle = 0;
    cdt->cdit = 0;
    cdt->x = 0;
    cdt->y = 0;
    cdt->cx = 240;
    cdt->cy = 110;
    WORD* cp = reinterpret_cast<WORD*>(cdt + 1);
    *cp++ = 0;
    *cp++ = 0;
    const wchar_t* ctitle = s.confirm_title.c_str();
    while (*ctitle) { *cp++ = (WORD)*ctitle++; }
    *cp++ = 0;
    *cp++ = 9;
    const wchar_t cface[] = L"Segoe UI";
    for (const wchar_t* c = cface; *c; ++c) *cp++ = (WORD)*c;
    *cp++ = 0;

    INT_PTR confirm_rc = DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
                                                 cdt, hwnd,
                                                 UpdateConfirmDlgProc,
                                                 reinterpret_cast<LPARAM>(&confirm_state));
    if (confirm_rc != IDYES) return;
  }

  wchar_t temp_dir[MAX_PATH];
  if (!GetTempPathW(MAX_PATH, temp_dir)) return;
  std::wstring zip_path = std::wstring(temp_dir) + L"dp1_update.zip";
  std::wstring staging  = std::wstring(temp_dir) + L"dp1_update_staging";
  std::wstring script   = std::wstring(temp_dir) + L"dp1_update.ps1";

  // Run the progress dialog modally.
  UpdateDownloadState st;
  st.asset_url = g_update_asset_url;
  st.dest_path = zip_path;

  // Build a tiny in-memory dialog template (centred, 380x110 client px).
  std::vector<BYTE> tmpl(512, 0);
  auto* dt = reinterpret_cast<DLGTEMPLATE*>(tmpl.data());
  dt->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME |
              DS_CENTER | DS_SETFONT;
  dt->dwExtendedStyle = 0;
  dt->cdit = 0;
  dt->x = 0;
  dt->y = 0;
  dt->cx = 240;   // 240 DLU ≈ 380 px
  dt->cy = 60;    // 60 DLU ≈ 90 px
  // Menu, class, and title strings (UTF-16 nul-terminated).
  WORD* p = reinterpret_cast<WORD*>(dt + 1);
  *p++ = 0;       // no menu
  *p++ = 0;       // default class
  // Title:
  const wchar_t* title = s.progress_title.c_str();
  while (*title) { *p++ = (WORD)*title++; }
  *p++ = 0;
  // DS_SETFONT extras:
  *p++ = 9;
  const wchar_t face[] = L"Segoe UI";
  for (const wchar_t* c = face; *c; ++c) *p++ = (WORD)*c;
  *p++ = 0;

  INT_PTR rc = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), dt, hwnd,
                                       UpdateDownloadDlgProc,
                                       reinterpret_cast<LPARAM>(&st));
  if (rc != IDOK) {
    DeleteFileW(zip_path.c_str());
    MessageBoxW(hwnd, s.failure_body.c_str(), s.failure_title.c_str(),
                MB_OK | MB_ICONERROR);
    return;
  }

  std::wstring install_dir = GetExeDir();
  if (!WriteUpdateScript(script, zip_path, staging, install_dir)) {
    MessageBoxW(hwnd, s.failure_body.c_str(), s.failure_title.c_str(),
                MB_OK | MB_ICONERROR);
    return;
  }

  // powershell.exe -WindowStyle Hidden -ExecutionPolicy Bypass -File <script>
  std::wstring cmd = L"powershell.exe -NoProfile -WindowStyle Hidden "
                     L"-ExecutionPolicy Bypass -File \"" + script + L"\"";
  STARTUPINFOW si{}; si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    // Exit immediately so the script can replace files without lock contention.
    PostMessage(hwnd, WM_CLOSE, 0, 0);
  } else {
    MessageBoxW(hwnd, s.failure_body.c_str(), s.failure_title.c_str(),
                MB_OK | MB_ICONERROR);
  }
}

void EnsurePerfDefaultsInToml() {
  // Make sure all launcher-managed cvars are registered so we can iterate
  // them below. DefineCvars is idempotent (clears g_cvars on entry).
  if (g_cvars.empty()) DefineCvars();

  std::wstring path = GetExeDir() + L"\\deadlyprem.toml";
  std::ifstream in(path);
  if (!in) return;
  std::vector<std::string> existing;
  std::vector<std::pair<std::string, std::string>> existing_lines;  // key, raw line
  std::string line;
  while (std::getline(in, line)) {
    size_t a = line.find_first_not_of(" \t\r\n");
    if (a == std::string::npos || line[a] == '#') continue;
    auto eq = line.find('=', a);
    if (eq == std::string::npos) continue;
    std::string k = line.substr(a, eq - a);
    while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
    existing.push_back(k);
    existing_lines.push_back({k, line});
  }
  in.close();

  // Detect duplicate keys (toml++ throws parse_error on duplicates, which
  // tanks the game's LoadConfig and silently reverts every cvar to default).
  // If found, rewrite the file with duplicates removed (keep first occurrence).
  std::sort(existing.begin(), existing.end());
  bool has_duplicates =
      std::adjacent_find(existing.begin(), existing.end()) != existing.end();
  if (has_duplicates) {
    std::set<std::string> seen;
    std::ofstream out(path, std::ios::trunc);
    if (out) {
      out << "# Auto-deduplicated by DPLauncher\n";
      for (const auto& [k, raw] : existing_lines) {
        if (seen.insert(k).second) {
          out << raw << "\n";
        }
      }
    }
    // Reload existing after dedupe so the missing() check below is accurate.
    existing.clear();
    for (const auto& [k, raw] : existing_lines) {
      if (std::find(existing.begin(), existing.end(), k) == existing.end()) {
        existing.push_back(k);
      }
    }
  }

  auto missing = [&](const char* k) {
    return std::find(existing.begin(), existing.end(), k) == existing.end();
  };

  // Format a cvar's registered default for toml output. Strings and enums
  // need to be double-quoted; numerics and bools go in bare.
  // Numeric enums (resolution_scale, anisotropic_override — INT32 cvars in
  // the SDK) must go in bare, same rule as SaveToml.
  auto format_default = [](const CvarRow& c) -> std::string {
    if (c.kind == CvarRow::kString ||
        (c.kind == CvarRow::kEnum && !LooksNumeric(c.value)))
      return std::string("\"") + c.value + "\"";
    return c.value;
  };

  struct Seed { std::string key; std::string value; };
  std::vector<Seed> add;

  // Auto-seed from g_cvars: any launcher-known cvar missing from toml gets
  // its registered default written back. This makes the SDK F4 SaveConfig
  // wipe self-heal — every time the in-game overlay drops a cvar that
  // matches the SDK default (launcher_language, mnk_*, keybind_*, dualsense_*,
  // colour_grade_*, every Debug-tab toggle, …), the next launcher boot
  // restores it. Was the root cause of the "language reset to Ukrainian
  // after F4" and "my keybinds disappeared" reports.
  //
  // Two cvars are deliberately excluded: texture_cache_memory_limit_soft /
  // _hard. SDK D3D12Provider auto-tunes both from DedicatedVideoMemory at
  // startup; pinning the AddCvar default would override the VRAM-aware
  // sizing on fresh installs. The user can still set them via launcher
  // Settings → those values DO get written to toml and respected.
  static const std::set<std::string> kNoAutoSeed = {
      // Launcher-only setting: lives in launcher.ini, the SDK never sees it.
      "launcher_language",
      "launcher_steam_overlay",
      "launcher_share_shader_cache",
      "launcher_prompt_style",
  };
  for (const auto& c : g_cvars) {
    if (kNoAutoSeed.count(c.key)) continue;
    if (!missing(c.key.c_str())) continue;
    add.push_back({c.key, format_default(c)});
  }

  // No vendor / VRAM auto-tuning here (the Downpour launcher flipped AMD to
  // RTV and low-VRAM GPUs to 1x): seeded values must match
  // deadlyprem.toml.dist-default exactly during bring-up.

  if (add.empty()) return;

  std::ofstream out(path, std::ios::app);
  if (!out) return;
  out << "\n# Auto-tuned defaults (seeded by launcher — self-heals F4 SaveConfig wipes)\n";
  for (const auto& s : add) out << s.key << " = " << s.value << "\n";
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) return L"";
  int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring out(len, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
  if (!out.empty() && out.back() == 0) out.pop_back();
  return out;
}

std::string Narrow(const std::wstring& s) {
  if (s.empty()) return "";
  int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(len, 0);
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), len, nullptr, nullptr);
  if (!out.empty() && out.back() == 0) out.pop_back();
  return out;
}

std::string Trim(std::string s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  size_t b = s.find_last_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  return s.substr(a, b - a + 1);
}

void LoadTomlValues(const std::wstring& toml_path) {
  g_unknown_toml_lines.clear();
  std::ifstream f(toml_path);
  if (!f) return;
  std::string line;
  while (std::getline(f, line)) {
    std::string l = Trim(line);
    if (l.empty() || l[0] == '#' || l[0] == '[') continue;
    auto eq = l.find('=');
    if (eq == std::string::npos) continue;
    std::string key = Trim(l.substr(0, eq));
    std::string val = Trim(l.substr(eq + 1));
    auto hash = val.find('#');
    if (hash != std::string::npos) val = Trim(val.substr(0, hash));
    // Strip surrounding quotes — TOML allows both "..." (basic string)
    // and '...' (literal string). The SDK F4 overlay writes single quotes.
    // Loop to clean up legacy toml's where an earlier launcher bug wrote
    // double-wrapped values like "'E'" — strip both layers in one pass.
    while (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') &&
           val.back() == val.front()) {
      val = val.substr(1, val.size() - 2);
    }
    bool matched = false;
    for (auto& c : g_cvars) {
      if (c.key == key) {
        c.value = val;
        matched = true;
        break;
      }
    }
    // game_data_root is hardcoded in SaveToml — skip to avoid duplicate emit.
    if (!matched && key != "game_data_root") {
      g_unknown_toml_lines.push_back(line);
    }
  }
  // Rebuild the monitor drop-down around the loaded value so an index of a
  // display that is not connected right now is kept, not silently reset.
  for (auto& c : g_cvars)
    if (c.key == "monitor") c.options = BuildMonitorOptions(c.value);
}

bool LooksNumeric(const std::string& s) {
  if (s.empty()) return false;
  size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  if (i >= s.size()) return false;
  bool seen_dot = false;
  for (; i < s.size(); ++i) {
    if (s[i] == '.') {
      if (seen_dot) return false;
      seen_dot = true;
      continue;
    }
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

void SaveToml(const std::wstring& toml_path) {
  // Backup existing toml so a partial write or hand-edit typo is recoverable.
  std::error_code ec;
  std::filesystem::copy_file(toml_path, toml_path + L".backup",
                             std::filesystem::copy_options::overwrite_existing,
                             ec);
  // (copy_file failure is ignored — first launch has no toml to back up.)

  std::ofstream f(toml_path, std::ios::trunc);
  if (!f) {
    MessageBoxW(nullptr,
                (L"Failed to write settings to:\n" + toml_path +
                 L"\n\nCheck file permissions and disk space.").c_str(),
                L"DPLauncher — Save error", MB_OK | MB_ICONERROR);
    return;
  }
  f << "# Auto-generated by DPLauncher\n";
  for (const auto& c : g_cvars) {
    // Launcher-only settings live in launcher.ini (the SDK never reads it).
    // Writing them to toml would trigger SDK "unknown cvar" warnings.
    if (c.key == "launcher_language" || c.key == "launcher_steam_overlay" ||
        c.key == "launcher_share_shader_cache" || c.key == "launcher_prompt_style") continue;
    f << c.key << " = ";
    switch (c.kind) {
      case CvarRow::kBool:
      case CvarRow::kInt:
        f << c.value;
        break;
      case CvarRow::kFloat:
        f << c.value;
        // TOML separates int and float types. If the user typed "1" in a float
        // field, append ".0" so a strict parser still treats it as a float.
        if (!c.value.empty() &&
            c.value.find('.') == std::string::npos &&
            c.value.find('e') == std::string::npos &&
            c.value.find('E') == std::string::npos) {
          f << ".0";
        }
        break;
      case CvarRow::kEnum:
        // Enum values can be either numeric (uint cvars like user_language)
        // or strings (render_target_path_d3d12). Pick by value content.
        if (LooksNumeric(c.value))
          f << c.value;
        else
          f << "\"" << c.value << "\"";
        break;
      case CvarRow::kString:
      default:
        f << "\"" << c.value << "\"";
        break;
    }
    f << "\n";
  }
  // Game data root — always points at assets/.
  f << "game_data_root = \"./assets\"\n";
  // Preserve any TOML keys we don't know about (SDK F4-overlay-only cvars,
  // user hand-edits) so round-trip through the launcher is non-destructive.
  for (const auto& raw : g_unknown_toml_lines) {
    f << raw << "\n";
  }
}

INT_PTR CALLBACK SettingsDlgProc(HWND, UINT, WPARAM, LPARAM);

std::wstring g_toml_path;

void OpenSettings(HWND parent) {
  if (g_cvars.empty()) DefineCvars();
  g_toml_path = GetExeDir() + L"\\" + kGameTomlName;
  LoadTomlValues(g_toml_path);
  // launcher_language is not a game cvar — mirror the active UI language
  // (loaded from launcher.ini at boot) into its row so the combo shows it.
  for (auto& c : g_cvars) {
    if (c.key == "launcher_language") {
      c.value = (g_lang == kLangUk) ? "uk" : "en";
    } else if (c.key == "launcher_steam_overlay") {
      ReadLauncherIni();
      auto it = g_launcher_ini.find("launcher_steam_overlay");
      c.value = (it != g_launcher_ini.end() && it->second == "off") ? "off" : "on";
    } else if (c.key == "launcher_share_shader_cache") {
      ReadLauncherIni();
      auto it = g_launcher_ini.find("launcher_share_shader_cache");
      c.value = (it != g_launcher_ini.end() && it->second == "on") ? "on" : "off";
    } else if (c.key == "launcher_prompt_style") {
      ReadLauncherIni();
      c.value = GetPromptStyle();
    }
  }

  struct DlgTemplate {
    DLGTEMPLATE t;
    WORD menu = 0;
    WORD wclass = 0;
    WCHAR title[32] = L"Deadly Premonition";
    short fontSize = 9;
    WCHAR fontName[16] = L"Segoe UI";
  };
  DlgTemplate tmpl{};
  tmpl.t.style = DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
  tmpl.t.dwExtendedStyle = 0;
  tmpl.t.cdit = 0;
  tmpl.t.x = 0; tmpl.t.y = 0;
  tmpl.t.cx = 460; tmpl.t.cy = 460;

  DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &tmpl.t, parent, SettingsDlgProc, 0);
}

// Settings dialog: TabControl with 3 pages (Graphics / Advanced / Controls).
// Each page hosts native Win32 controls (checkbox/edit/combo) on a dark theme.

constexpr int kIdStart = 1000;
constexpr int kIdSave = 100;
constexpr int kIdCancel = 101;
constexpr int kIdTab = 200;
// v1.1.6 sliders: trackbar IDs live above kIdStart range. The dialog proc
// uses (id - kSliderIdStart) to look up the matching cvar.
constexpr int kSliderIdStart = 10000;
// Re-entrancy guard for the EDIT <-> trackbar bidirectional sync. Set while
// we programmatically update one widget so the other's change handler can
// skip the round-trip and not loop.
static bool g_slider_syncing = false;

constexpr int kCatCount = 5;

struct CtrlEntry {
  HWND label = nullptr;
  HWND ctrl = nullptr;
  // v1.1.6 slider widget: for kFloat (and ranged kInt) cvars we now build a
  // trackbar paired with the numeric edit field. The trackbar is the visual
  // / draggable surface; the edit is the precise-value display. Save still
  // reads the value from `ctrl` (the edit), so the trackbar is a UI affordance
  // only — drag updates edit text, typed text updates trackbar position.
  HWND slider = nullptr;
  double slider_min = 0.0;
  double slider_max = 1.0;
  int cvar_index = -1;
};

static std::vector<CtrlEntry> g_ctrl_entries;

void ShowCategoryControls(int category) {
  for (auto& e : g_ctrl_entries) {
    if (e.cvar_index < 0) continue;
    int cat = (int)g_cvars[e.cvar_index].category;
    int cmd = (cat == category) ? SW_SHOW : SW_HIDE;
    if (e.label) ShowWindow(e.label, cmd);
    if (e.ctrl) ShowWindow(e.ctrl, cmd);
    if (e.slider) ShowWindow(e.slider, cmd);
  }
}

static HBRUSH g_dlg_bg_brush = nullptr;
static HBRUSH g_ctrl_bg_brush = nullptr;
static HFONT g_dlg_font = nullptr;

static WNDPROC g_orig_tab_proc = nullptr;
LRESULT CALLBACK TabSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND: {
      HDC hdc = (HDC)wp;
      RECT r;
      GetClientRect(hwnd, &r);
      FillRect(hdc, &r, g_dlg_bg_brush);
      return 1;
    }
    case WM_PAINT: {
      // Let default paint draw the items (we own-draw via WM_DRAWITEM in
      // parent), but pre-fill the background as dark so any gaps don't flash
      // white.
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      FillRect(hdc, &ps.rcPaint, g_dlg_bg_brush);
      EndPaint(hwnd, &ps);
      // Force the tab items to repaint over the dark fill.
      InvalidateRect(hwnd, nullptr, FALSE);
      return CallWindowProcW(g_orig_tab_proc, hwnd, WM_PAINT, 0, 0);
    }
  }
  return CallWindowProcW(g_orig_tab_proc, hwnd, msg, wp, lp);
}

int BuildPageControls(HWND hwnd, HFONT font, int dlg_w, int category) {
  // Count items in this category — switch to a 2-column grid when there are
  // many (keybinds tab has ~20 items, single-column would scroll).
  int item_count = 0;
  for (auto& c : g_cvars) if ((int)c.category == category) ++item_count;
  const bool two_columns = item_count > 12;
  const int columns = two_columns ? 2 : 1;
  const int row_h = 30;
  const int margin_x = 18;
  const int top_y = 16;
  const int col_gap = 18;
  // Each column is half the usable width when in 2-col mode.
  const int col_w = two_columns
                        ? (dlg_w - 2 * margin_x - col_gap) / 2
                        : (dlg_w - 2 * margin_x);
  const int label_w = two_columns ? 232 : 240;
  const int ctrl_w = two_columns ? (col_w - label_w - 8) : 160;
  const int rows_per_col = (item_count + columns - 1) / columns;
  int idx_in_cat = -1;
  int max_y = top_y;
  for (size_t i = 0; i < g_cvars.size(); ++i) {
    auto& c = g_cvars[i];
    if ((int)c.category != category) continue;
    ++idx_in_cat;
    int col = two_columns ? (idx_in_cat / rows_per_col) : 0;
    int row = two_columns ? (idx_in_cat % rows_per_col) : idx_in_cat;
    int y = top_y + row * row_h;
    int col_x = margin_x + col * (col_w + col_gap);
    if (y + row_h > max_y) max_y = y + row_h;
    std::wstring name = TrW(c.display_name);
    HWND label = CreateWindowW(L"STATIC", name.c_str(),
                               WS_CHILD | SS_LEFT | SS_NOPREFIX,
                               col_x, y + 6, label_w, row_h - 4,
                               hwnd, (HMENU)(intptr_t)(2000 + i),
                               nullptr, nullptr);
    SendMessageW(label, WM_SETFONT, (WPARAM)font, TRUE);

    int x = col_x + label_w + 8;
    HWND ctrl = nullptr;
    HMENU id = (HMENU)(intptr_t)(kIdStart + i);
    switch (c.kind) {
      case CvarRow::kBool: {
        ctrl = CreateWindowW(L"BUTTON", L"",
                             WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP,
                             x, y + 6, 20, row_h - 6, hwnd, id, nullptr, nullptr);
        bool checked = (c.value == "true" || c.value == "1");
        SendMessageW(ctrl, BM_SETCHECK,
                     checked ? BST_CHECKED : BST_UNCHECKED, 0);
        break;
      }
      case CvarRow::kEnum: {
        ctrl = CreateWindowW(L"COMBOBOX", nullptr,
                             WS_CHILD | CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                             x, y + 2, ctrl_w, 260, hwnd, id, nullptr, nullptr);
        int sel = 0;
        int widest = ctrl_w;
        HDC measure_hdc = GetDC(ctrl);
        HFONT prev = (HFONT)SelectObject(measure_hdc, font);
        for (size_t j = 0; j < c.options.size(); ++j) {
          std::wstring label = TrW(c.options[j].second);
          SendMessageW(ctrl, CB_ADDSTRING, 0, (LPARAM)label.c_str());
          if (c.options[j].first == c.value) sel = (int)j;
          SIZE sz = {};
          if (GetTextExtentPoint32W(measure_hdc, label.c_str(),
                                    (int)label.size(), &sz)) {
            if (sz.cx + 24 > widest) widest = sz.cx + 24;
          }
        }
        SelectObject(measure_hdc, prev);
        ReleaseDC(ctrl, measure_hdc);
        SendMessageW(ctrl, CB_SETDROPPEDWIDTH, widest, 0);
        SendMessageW(ctrl, CB_SETCURSEL, sel, 0);
        break;
      }
      case CvarRow::kInt:
      case CvarRow::kFloat:
      case CvarRow::kString:
      default: {
        // v1.1.6 slider: when the cvar has a real numeric range (kFloat or
        // kInt with min < max), pair the EDIT with a horizontal trackbar to
        // the left. Drag updates the EDIT text, typed text updates the
        // trackbar position. Save logic is unchanged (reads from EDIT).
        //
        // Exclusion list: cvars where the exact integer value matters (pixel
        // resolutions, fps caps, …). A slider quantises 0..1000 ticks which
        // makes typing "1920" much faster than dragging.
        static const std::set<std::string> kNoSliderKeys = {
            "window_width", "window_height",
        };
        const bool has_slider =
            (c.kind == CvarRow::kFloat || c.kind == CvarRow::kInt) &&
            c.min_val < c.max_val &&
            kNoSliderKeys.find(c.key) == kNoSliderKeys.end();
        const int edit_w = has_slider ? 72 : ctrl_w;
        const int slider_gap = 6;
        const int slider_w = has_slider ? (ctrl_w - edit_w - slider_gap) : 0;
        HWND slider_hwnd = nullptr;
        if (has_slider && slider_w > 40) {
          HMENU sid = (HMENU)(intptr_t)(kSliderIdStart + i);
          slider_hwnd = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                        WS_CHILD | WS_TABSTOP | TBS_HORZ |
                                            TBS_NOTICKS | TBS_BOTH,
                                        x, y + 4, slider_w, row_h - 8,
                                        hwnd, sid, nullptr, nullptr);
          SendMessageW(slider_hwnd, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
          SendMessageW(slider_hwnd, TBM_SETPAGESIZE, 0, 50);
          double v = atof(c.value.c_str());
          double span = c.max_val - c.min_val;
          double frac = span > 0.0 ? (v - c.min_val) / span : 0.0;
          if (frac < 0.0) frac = 0.0;
          if (frac > 1.0) frac = 1.0;
          int pos = (int)(frac * 1000.0 + 0.5);
          SendMessageW(slider_hwnd, TBM_SETPOS, TRUE, pos);
        }
        int edit_x = has_slider ? (x + slider_w + slider_gap) : x;
        ctrl = CreateWindowW(L"EDIT", Widen(c.value).c_str(),
                             WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                             edit_x, y + 4, edit_w, row_h - 8, hwnd, id, nullptr, nullptr);
        if (slider_hwnd) SendMessageW(slider_hwnd, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(ctrl, WM_SETFONT, (WPARAM)font, TRUE);
        CtrlEntry e;
        e.label = label;
        e.ctrl = ctrl;
        e.slider = slider_hwnd;
        e.slider_min = c.min_val;
        e.slider_max = c.max_val;
        e.cvar_index = (int)i;
        g_ctrl_entries.push_back(e);
        continue;  // skip the default entry-push below
      }
    }
    SendMessageW(ctrl, WM_SETFONT, (WPARAM)font, TRUE);

    CtrlEntry e;
    e.label = label;
    e.ctrl = ctrl;
    e.cvar_index = (int)i;
    g_ctrl_entries.push_back(e);
  }
  return max_y;
}

INT_PTR CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG: {
      BOOL dark = TRUE;
      DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
      if (!g_dlg_bg_brush) g_dlg_bg_brush = CreateSolidBrush(RGB(24, 26, 30));
      if (!g_ctrl_bg_brush) g_ctrl_bg_brush = CreateSolidBrush(RGB(40, 42, 48));
      if (!g_dlg_font) {
        LOGFONTW lf = {};
        lf.lfHeight = -14;
        lf.lfWeight = FW_NORMAL;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        g_dlg_font = CreateFontIndirectW(&lf);
      }
      g_ctrl_entries.clear();
      HFONT font = g_dlg_font;

      RECT cr;
      GetClientRect(hwnd, &cr);
      int dlg_w = cr.right - cr.left;

      // Tab control at top.
      const int tab_h = 32;
      HWND tab = CreateWindowW(WC_TABCONTROLW, nullptr,
                                WS_VISIBLE | WS_CHILD | WS_TABSTOP | TCS_TABS |
                                    TCS_OWNERDRAWFIXED,
                                10, 10, dlg_w - 20, tab_h,
                                hwnd, (HMENU)(intptr_t)kIdTab, nullptr, nullptr);
      SendMessageW(tab, WM_SETFONT, (WPARAM)font, TRUE);
      // Strip the visual theme so the white tab strip background goes away.
      SetWindowTheme(tab, L"", L"");
      // Subclass to paint the tab strip background dark instead of white.
      g_orig_tab_proc =
          (WNDPROC)SetWindowLongPtrW(tab, GWLP_WNDPROC, (LONG_PTR)TabSubclassProc);
      TCITEMW ti = {};
      ti.mask = TCIF_TEXT;
      const char* tab_names[] = {"Graphics", "Advanced", "Mouse", "Controls", "Debug"};
      std::wstring tab_titles[kCatCount];
      for (int i = 0; i < kCatCount; ++i) {
        tab_titles[i] = TrW(tab_names[i]);
        ti.pszText = (LPWSTR)tab_titles[i].c_str();
        TabCtrl_InsertItem(tab, i, &ti);
      }

      // Build all category page controls; they overlap, switched via Show.
      int max_y = 0;
      for (int cat = 0; cat < kCatCount; ++cat) {
        int y = BuildPageControls(hwnd, font, dlg_w, cat);
        if (y > max_y) max_y = y;
      }

      // Offset all by tab area + some breathing room.
      const int content_top_offset = tab_h + 30;
      for (auto& e : g_ctrl_entries) {
        for (HWND h : {e.label, e.ctrl, e.slider}) {
          if (!h) continue;
          RECT r;
          GetWindowRect(h, &r);
          POINT p = {r.left, r.top};
          ScreenToClient(hwnd, &p);
          MoveWindow(h, p.x, p.y + content_top_offset,
                     r.right - r.left, r.bottom - r.top, FALSE);
        }
      }
      max_y += content_top_offset;

      // Show only first category.
      ShowCategoryControls(0);

      // Save / Cancel buttons at bottom.
      const int btn_y = max_y + 20;
      std::wstring save_text = TrW("Save && Close");
      std::wstring cancel_text = TrW("Cancel");
      HWND save_btn = CreateWindowW(L"BUTTON", save_text.c_str(),
                                    WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON | WS_TABSTOP,
                                    dlg_w - 240, btn_y, 140, 34,
                                    hwnd, (HMENU)(intptr_t)kIdSave, nullptr, nullptr);
      SendMessageW(save_btn, WM_SETFONT, (WPARAM)font, TRUE);
      HWND cancel_btn = CreateWindowW(L"BUTTON", cancel_text.c_str(),
                                      WS_VISIBLE | WS_CHILD | WS_TABSTOP,
                                      dlg_w - 100, btn_y, 90, 34,
                                      hwnd, (HMENU)(intptr_t)kIdCancel, nullptr, nullptr);
      SendMessageW(cancel_btn, WM_SETFONT, (WPARAM)font, TRUE);

      // Resize dialog window so client fits max_y + buttons + bottom margin.
      RECT wr;
      GetWindowRect(hwnd, &wr);
      int frame_h = (wr.bottom - wr.top) - cr.bottom;
      int frame_w = (wr.right - wr.left) - dlg_w;
      int new_client_h = btn_y + 32 + 20;        // buttons + bottom margin
      int new_window_h = new_client_h + frame_h;
      int new_window_w = (wr.right - wr.left);   // keep current window width
      // Re-center on screen.
      int sx = (GetSystemMetrics(SM_CXSCREEN) - new_window_w) / 2;
      int sy = (GetSystemMetrics(SM_CYSCREEN) - new_window_h) / 2;
      if (sy < 0) sy = 0;
      SetWindowPos(hwnd, nullptr, sx, sy, new_window_w, new_window_h, SWP_NOZORDER);
      (void)frame_w;
      return TRUE;
    }
    case WM_NOTIFY: {
      LPNMHDR nm = (LPNMHDR)lp;
      if (nm->idFrom == kIdTab && nm->code == TCN_SELCHANGE) {
        int sel = TabCtrl_GetCurSel(nm->hwndFrom);
        ShowCategoryControls(sel);
        InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;
    }
    case WM_DRAWITEM: {
      DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lp;
      if (di->CtlID == kIdTab) {
        HDC hdc = di->hDC;
        RECT r = di->rcItem;
        bool selected = (di->itemState & ODS_SELECTED) != 0;
        HBRUSH bg = selected ? g_ctrl_bg_brush : g_dlg_bg_brush;
        FillRect(hdc, &r, bg);
        // Selected tab gets a subtle bottom accent.
        if (selected) {
          RECT accent = {r.left, r.bottom - 2, r.right, r.bottom};
          static HBRUSH accent_brush = CreateSolidBrush(RGB(170, 170, 180));
          FillRect(hdc, &accent, accent_brush);
        }
        SetTextColor(hdc, selected ? RGB(245, 245, 245) : RGB(180, 180, 185));
        SetBkMode(hdc, TRANSPARENT);
        wchar_t text[64] = {};
        TCITEMW ti = {};
        ti.mask = TCIF_TEXT;
        ti.pszText = text;
        ti.cchTextMax = 64;
        TabCtrl_GetItem(GetDlgItem(hwnd, kIdTab), di->itemID, &ti);
        DrawTextW(hdc, text, -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return TRUE;
      }
      return FALSE;
    }
    case WM_CTLCOLORDLG:
      return (INT_PTR)g_dlg_bg_brush;
    case WM_CTLCOLORSTATIC: {
      HDC hdc = (HDC)wp;
      SetTextColor(hdc, RGB(230, 230, 230));
      SetBkColor(hdc, RGB(24, 26, 30));
      SetBkMode(hdc, TRANSPARENT);
      return (INT_PTR)g_dlg_bg_brush;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
      HDC hdc = (HDC)wp;
      SetTextColor(hdc, RGB(235, 235, 235));
      SetBkColor(hdc, RGB(40, 42, 48));
      return (INT_PTR)g_ctrl_bg_brush;
    }
    case WM_HSCROLL: {
      // v1.1.6 slider drag: map [0..1000] position back to the cvar's
      // [min..max] range and write the formatted value into the paired EDIT.
      HWND src = (HWND)lp;
      if (!src || g_slider_syncing) break;
      for (auto& e : g_ctrl_entries) {
        if (e.slider != src) continue;
        LRESULT pos = SendMessageW(e.slider, TBM_GETPOS, 0, 0);
        double frac = (double)pos / 1000.0;
        double v = e.slider_min + frac * (e.slider_max - e.slider_min);
        if (e.cvar_index < 0 || e.cvar_index >= (int)g_cvars.size()) break;
        auto& c = g_cvars[e.cvar_index];
        wchar_t buf[64];
        if (c.kind == CvarRow::kInt) {
          int iv = (int)(v + (v >= 0 ? 0.5 : -0.5));
          swprintf_s(buf, L"%d", iv);
        } else {
          // %.4g auto-trims trailing zeros: 0.7 stays "0.7", 150.0 -> "150",
          // 0.05 stays "0.05". Avoids "0.500000" noise.
          swprintf_s(buf, L"%.4g", v);
        }
        g_slider_syncing = true;
        SetWindowTextW(e.ctrl, buf);
        g_slider_syncing = false;
        break;
      }
      return 0;
    }
    case WM_COMMAND: {
      // v1.1.6 slider: EDIT typed by user -> sync trackbar position.
      WORD notif = HIWORD(wp);
      HWND src = (HWND)lp;
      if (notif == EN_CHANGE && src && !g_slider_syncing) {
        for (auto& e : g_ctrl_entries) {
          if (e.ctrl != src || !e.slider) continue;
          wchar_t tbuf[64] = {};
          GetWindowTextW(e.ctrl, tbuf, 63);
          double v = _wtof(tbuf);
          double span = e.slider_max - e.slider_min;
          double frac = span > 0.0 ? (v - e.slider_min) / span : 0.0;
          if (frac < 0.0) frac = 0.0;
          if (frac > 1.0) frac = 1.0;
          int pos = (int)(frac * 1000.0 + 0.5);
          g_slider_syncing = true;
          SendMessageW(e.slider, TBM_SETPOS, TRUE, pos);
          g_slider_syncing = false;
          break;
        }
      }
      WORD id = LOWORD(wp);
      if (id == kIdSave) {
        for (auto& e : g_ctrl_entries) {
          if (e.cvar_index < 0 || !e.ctrl) continue;
          auto& c = g_cvars[e.cvar_index];
          switch (c.kind) {
            case CvarRow::kBool:
              c.value = (SendMessageW(e.ctrl, BM_GETCHECK, 0, 0) == BST_CHECKED)
                            ? "true" : "false";
              break;
            case CvarRow::kEnum: {
              int sel = (int)SendMessageW(e.ctrl, CB_GETCURSEL, 0, 0);
              if (sel >= 0 && sel < (int)c.options.size())
                c.value = c.options[sel].first;
              break;
            }
            case CvarRow::kInt:
            case CvarRow::kFloat:
            case CvarRow::kString:
            default: {
              int len = GetWindowTextLengthW(e.ctrl);
              std::wstring buf(len + 1, L'\0');
              GetWindowTextW(e.ctrl, buf.data(), len + 1);
              buf.resize(len);
              c.value = Narrow(buf.c_str());
              break;
            }
          }
        }
        // Detect launcher_language change to prompt for restart.
        std::string old_lang = (g_lang == kLangUk) ? "uk" : "en";
        std::string new_lang = old_lang;
        for (const auto& c : g_cvars) {
          if (c.key == "launcher_language") {
            new_lang = c.value;
            break;
          }
        }
        for (const auto& c : g_cvars) {
          if (c.key == "launcher_steam_overlay") {
            g_launcher_ini["launcher_steam_overlay"] = (c.value == "off") ? "off" : "on";
          } else if (c.key == "launcher_share_shader_cache") {
            g_launcher_ini["launcher_share_shader_cache"] = (c.value == "on") ? "on" : "off";
          } else if (c.key == "launcher_prompt_style") {
            g_launcher_ini["launcher_prompt_style"] = IsKnownPromptStyle(c.value) ? c.value : "keyboard";
            g_launcher_ini.erase("launcher_key_prompts");
          }
        }
        SaveToml(g_toml_path);
        WriteLauncherIni();
        GenerateKeyPromptOverlay();
        if (new_lang != old_lang) {
          // Update g_lang AND the launcher.ini sidecar (the only place the
          // launcher language is persisted).
          g_lang = (new_lang == "uk") ? kLangUk : kLangEn;
          SaveLauncherLanguageSidecar();
          MessageBoxW(hwnd,
                      TrC("Launcher language changed. Restart to apply."),
                      kWindowTitle,
                      MB_OK | MB_ICONINFORMATION);
        } else {
          // Even on no change, keep the sidecar in sync. Cheap and idempotent.
          SaveLauncherLanguageSidecar();
        }
        EndDialog(hwnd, IDOK);
        return TRUE;
      } else if (id == kIdCancel || id == IDCANCEL) {
        EndDialog(hwnd, IDCANCEL);
        return TRUE;
      }
      return FALSE;
    }
    case WM_CLOSE:
      EndDialog(hwnd, IDCANCEL);
      return TRUE;
  }
  return FALSE;
}

}  // namespace

// launcher.ini schema: tiny key=value file next to PlayDeadlyPremonition.exe. Holds
// settings the SDK does not know about (launcher_language so far). Replaces
// the older launcher.lang sidecar (which was a single-value text file). The
// SDK never reads or writes this file — survives any F4 SaveConfig wipe of
// deadlyprem.toml.
//
// Supported keys (treated case-insensitively):
//   launcher_language = en|uk

std::unordered_map<std::string, std::string> g_launcher_ini;

static std::wstring LauncherIniPath() {
  return GetExeDir() + L"\\launcher.ini";
}

static std::wstring LegacyLauncherLangPath() {
  return GetExeDir() + L"\\launcher.lang";
}

static void ReadLauncherIni() {
  g_launcher_ini.clear();
  std::ifstream f(LauncherIniPath());
  if (!f) return;
  std::string line;
  while (std::getline(f, line)) {
    auto a = line.find_first_not_of(" \t\r\n");
    if (a == std::string::npos || line[a] == '#' || line[a] == ';' || line[a] == '[') continue;
    auto eq = line.find('=', a);
    if (eq == std::string::npos) continue;
    std::string key = line.substr(a, eq - a);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    std::string value = line.substr(eq + 1);
    auto b = value.find_first_not_of(" \t");
    if (b != std::string::npos) value = value.substr(b);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r' || value.back() == '\n'))
      value.pop_back();
    for (auto& c : key) c = char(::tolower(c));
    g_launcher_ini[key] = value;
  }
}

void WriteLauncherIni() {
  std::ofstream o(LauncherIniPath(), std::ios::trunc);
  if (!o) return;
  o << "; DPLauncher persistent settings (read by launcher only — SDK never\n"
       "; touches this file, so settings survive F4 SaveConfig wipes).\n";
  for (const auto& [k, v] : g_launcher_ini) {
    o << k << " = " << v << "\n";
  }
}

// Read `launcher_language` from launcher.ini (preferred) → deadlyprem.toml →
// legacy launcher.lang sidecar. The toml read is kept as a one-time migration
// helper: if the user has the language set in toml but launcher.ini is empty,
// we'll pick it up and then write it to launcher.ini on the next Save.
void LoadLauncherLanguageFromToml() {
  std::wstring exe_dir = GetExeDir();
  std::string val;

  // Pass 1: launcher.ini.
  ReadLauncherIni();
  auto it = g_launcher_ini.find("launcher_language");
  if (it != g_launcher_ini.end()) val = it->second;

  // Pass 2: deadlyprem.toml (migration fallback).
  if (val.empty()) {
    std::ifstream f(exe_dir + L"\\deadlyprem.toml");
    if (f) {
      std::string line;
      while (std::getline(f, line)) {
        size_t a = line.find_first_not_of(" \t\r\n");
        if (a == std::string::npos || line[a] == '#') continue;
        auto eq = line.find('=', a);
        if (eq == std::string::npos) continue;
        std::string key = line.substr(a, eq - a);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
          key.pop_back();
        if (key != "launcher_language") continue;
        val = line.substr(eq + 1);
        size_t b = val.find_first_not_of(" \t");
        if (b != std::string::npos) val = val.substr(b);
        auto hash = val.find('#');
        if (hash != std::string::npos) val = val.substr(0, hash);
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t' ||
                                val.back() == '\r' || val.back() == '\n'))
          val.pop_back();
        // Strip surrounding TOML quotes — accept BOTH "..." (basic) and
        // '...' (literal). SDK F4 SaveConfig writes single-quoted strings,
        // earlier launcher versions only stripped doubles — leaving val as
        // "'en'" which then failed both the "en" and "uk" comparisons and
        // fell through to the legacy launcher.lang sidecar. That sidecar
        // could carry "uk" from an earlier install, making the launcher
        // display Ukrainian for users who picked English. Loop in case of
        // double-wrapping from legacy round-trip ("'en'").
        while (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') &&
               val.back() == val.front()) {
          val = val.substr(1, val.size() - 2);
        }
        break;
      }
    }
  }

  // Pass 3: legacy launcher.lang sidecar (delete on success — one-shot migrate).
  if (val.empty() || (val != "uk" && val != "en")) {
    std::ifstream s(LegacyLauncherLangPath());
    if (s) {
      std::getline(s, val);
      while (!val.empty() && (val.back() == ' ' || val.back() == '\t' ||
                              val.back() == '\r' || val.back() == '\n'))
        val.pop_back();
      s.close();
      // Migrate then remove.
      if (val == "uk" || val == "en") {
        g_launcher_ini["launcher_language"] = val;
        WriteLauncherIni();
        DeleteFileW(LegacyLauncherLangPath().c_str());
      }
    }
  }

  if (val == "uk") g_lang = kLangUk;
}

// Persist the launcher language to launcher.ini so it survives any SDK F4
// SaveConfig (which strips unknown toml keys).
void SaveLauncherLanguageSidecar() {
  g_launcher_ini["launcher_language"] = (g_lang == kLangUk ? "uk" : "en");
  WriteLauncherIni();
}

// v1.1: Steam Deck preset (community-tested settings, DPRecomp discussions).
// Detection: Steam sets SteamDeck=1 in the environment of games it launches
// on the Deck (works under Proton), and the Deck APU reports as
// "AMD Custom GPU 0405" (LCD) / "AMD Custom GPU 0932" (OLED). Applied once,
// recorded in launcher.ini (deck_preset_applied), so the user can change any
// value afterwards without it being re-applied.
static bool IsSteamDeck() {
  wchar_t buf[8] = {};
  if (GetEnvironmentVariableW(L"SteamDeck", buf, 8) && buf[0] == L'1') return true;
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
  for (UINT i = 0;; ++i) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) continue;
    std::wstring name(desc.Description);
    if (name.find(L"AMD Custom GPU 0405") != std::wstring::npos ||
        name.find(L"AMD Custom GPU 0932") != std::wstring::npos ||
        name.find(L"Van Gogh") != std::wstring::npos) {
      return true;
    }
  }
  return false;
}

void ApplySteamDeckPresetIfDetected() {
  ReadLauncherIni();
  if (g_launcher_ini.count("deck_preset_applied")) return;
  if (!IsSteamDeck()) return;
  if (g_cvars.empty()) DefineCvars();
  const std::wstring toml = GetExeDir() + L"\\" + kGameTomlName;
  LoadTomlValues(toml);
  static const std::pair<const char*, const char*> kPreset[] = {
      {"render_target_path_d3d12", "rtv"}, {"resolution_scale", "1"},
      {"native_2x_msaa", "true"},          {"anisotropic_override", "5"},
      {"swap_post_effect", "fxaa"},        {"present_effect", "cas"},
      {"present_fsr_quality_mode", "auto"}, {"dp_60fps", "false"},
      {"vsync", "true"},                   {"fullscreen", "true"},
      {"window_width", "1280"},            {"window_height", "800"},
      {"monitor", "0"},                    {"d3d12_allow_variable_refresh_rate_and_tearing", "false"},
      {"present_dither", "false"},
  };
  for (auto& c : g_cvars) {
    for (const auto& kv : kPreset) {
      if (c.key == kv.first) c.value = kv.second;
    }
  }
  SaveToml(toml);
  g_launcher_ini["deck_preset_applied"] = "1";
  WriteLauncherIni();
  MessageBoxW(nullptr,
              TrC("Steam Deck detected - the community-tested Deck preset was applied (RTV, 1x, "
                  "2x MSAA, 16x AF, FXAA + CAS, 30 FPS, VSync, fullscreen 1280x800). You can "
                  "change anything in Settings; this will not be applied again."),
              TrC("Steam Deck preset"), MB_OK | MB_ICONINFORMATION);
}

// v1.1.1: keyboard key caps on the button-prompt atlas. The game draws its
// A/B/X/Y/LB/RB/LT/RT/stick prompts from one 256x256 DXT5 atlas (guest hash
// 2D1098B531AA9CA8, 8x8 cells of 32 px). We write
// textures\2D1098B531AA9CA8.overlay.png (4x, 1024x1024) with opaque key caps
// in the cells of the buttons that have keyboard binds; the runtime decodes the
// original atlas, upscales it and punches the overlay through where alpha > 0
// (rexglue-sdk src/graphics/pipeline/texture/replacement.cpp). D-pad, arrows
// and everything else stay original and no game asset is redistributed.
static int GetPngEncoderClsid(CLSID* clsid) {
  UINT num = 0, size = 0;
  GetImageEncodersSize(&num, &size);
  if (size == 0) return -1;
  std::vector<uint8_t> buf(size);
  ImageCodecInfo* info = reinterpret_cast<ImageCodecInfo*>(buf.data());
  GetImageEncoders(num, size, info);
  for (UINT i = 0; i < num; ++i) {
    if (wcscmp(info[i].MimeType, L"image/png") == 0) {
      *clsid = info[i].Clsid;
      return int(i);
    }
  }
  return -1;
}

// "E,Space+LMB" -> first alternative -> "E"; "Shift+Left" -> "SHIFT+<-".
static std::wstring PromptKeyLabel(const std::string& bind) {
  std::string first = bind.substr(0, bind.find(','));
  std::wstring out;
  size_t pos = 0;
  while (pos <= first.size()) {
    size_t plus = first.find('+', pos);
    std::string tok = first.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
    while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
    while (!tok.empty() && tok.back() == ' ') tok.pop_back();
    std::string lower = tok;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::wstring label;
    if (lower == "space") label = L"SPACE";
    else if (lower == "return" || lower == "enter") label = L"ENTER";
    else if (lower == "control" || lower == "ctrl" || lower == "lcontrol") label = L"CTRL";
    else if (lower == "shift" || lower == "lshift") label = L"SHIFT";
    else if (lower == "alt" || lower == "menu") label = L"ALT";
    else if (lower == "escape") label = L"ESC";
    else if (lower == "tab") label = L"TAB";
    else if (lower == "backspace") label = L"BKSP";
    else if (lower == "capslock") label = L"CAPS";
    else if (lower == "wheelup") label = L"WHL\u2191";
    else if (lower == "wheeldown") label = L"WHL\u2193";
    else if (lower == "left") label = L"\u2190";
    else if (lower == "right") label = L"\u2192";
    else if (lower == "up") label = L"\u2191";
    else if (lower == "down") label = L"\u2193";
    else {
      std::string up = tok;
      std::transform(up.begin(), up.end(), up.begin(), ::toupper);
      if (up.size() > 6) up.resize(6);
      label = Widen(up);
    }
    if (!label.empty()) {
      if (!out.empty()) out += L"+";
      out += label;
    }
    if (plus == std::string::npos) break;
    pos = plus + 1;
  }
  return out.empty() ? L"?" : out;
}

// One prompt atlas: guest hash + size, overlay scale, and the button cells in
// guest pixel coordinates. Add a line per texture that shows gamepad buttons
// (find them with the Texture Dump switch: textures\dump\<hash>_<w>x<h>_<fmt>.png).
struct PromptCell { const char* key; int x, y, w, h; };
struct PromptAtlas {
  const wchar_t* hash;
  int scale;  // overlay scale vs the guest texture (4 = crisp 128 px caps)
  std::vector<PromptCell> cells;
};
static const PromptAtlas kPromptAtlases[] = {
    // Main button-prompt atlas 256x256 (8x8 cells of 32 px).
    {L"2D1098B531AA9CA8", 4,
     {{"keybind_a", 0, 0, 32, 32},               {"keybind_b", 32, 0, 32, 32},
      {"keybind_x", 64, 0, 32, 32},              {"keybind_y", 96, 0, 32, 32},
      {"@move", 32, 32, 32, 32},                 {"@mouse", 64, 32, 32, 32},
      {"keybind_left_shoulder", 32, 64, 32, 32}, {"keybind_right_shoulder", 64, 64, 32, 32},
      {"keybind_left_trigger", 96, 64, 32, 32},  {"keybind_right_trigger", 128, 64, 32, 32},
      {"keybind_lstick_press", 160, 64, 32, 32}, {"keybind_rstick_press", 192, 64, 32, 32},
      // bottom row: stick-shake prompts "< stick >" (two animation frames each);
      // the stick icon is replaced, the animated arrows stay original.
      {"@shake_l", 12, 229, 34, 24}, {"@shake_l", 82, 229, 34, 24},
      {"@shake_r", 140, 229, 34, 24}, {"@shake_r", 210, 229, 34, 24}}},
    // DATA LOAD page 1024x1024: LB / RB tabs top-right.
    {L"51AA9FBD20140C6D", 2,
     {{"keybind_left_shoulder", 824, 1, 32, 30}, {"keybind_right_shoulder", 856, 1, 32, 30}}},
};

// v1.2: prompt icon sets (prompts\ps_*, Zacksly CC BY 3.0) and the title
// texture (textures\) travel inside the launcher exe and are written out
// when their folder is missing - the 1.1 in-launcher updater only copies a
// fixed list of files, so this is how 1.1 installs get them. A folder that
// exists is never touched (the user may have removed or changed files).
struct BundledAsset { int res_id; const wchar_t* rel; };
static const BundledAsset kBundledAssets[] = {
    {300, L"prompts\\ps_fullsolid\\Circle.png"},
    {301, L"prompts\\ps_fullsolid\\Create.png"},
    {302, L"prompts\\ps_fullsolid\\Cross.png"},
    {303, L"prompts\\ps_fullsolid\\L1.png"},
    {304, L"prompts\\ps_fullsolid\\L2.png"},
    {305, L"prompts\\ps_fullsolid\\Left Stick All.png"},
    {306, L"prompts\\ps_fullsolid\\Left Stick Click.png"},
    {307, L"prompts\\ps_fullsolid\\Left Stick Left-Right.png"},
    {308, L"prompts\\ps_fullsolid\\Options.png"},
    {309, L"prompts\\ps_fullsolid\\R1.png"},
    {310, L"prompts\\ps_fullsolid\\R2.png"},
    {311, L"prompts\\ps_fullsolid\\Right Stick Click.png"},
    {312, L"prompts\\ps_fullsolid\\Right Stick Left-Right.png"},
    {313, L"prompts\\ps_fullsolid\\Right Stick Up-Down.png"},
    {314, L"prompts\\ps_fullsolid\\Right Stick.png"},
    {315, L"prompts\\ps_fullsolid\\Square.png"},
    {316, L"prompts\\ps_fullsolid\\Triangle.png"},
    {317, L"prompts\\ps_solid\\Circle.png"},
    {318, L"prompts\\ps_solid\\Create.png"},
    {319, L"prompts\\ps_solid\\Cross.png"},
    {320, L"prompts\\ps_solid\\L1.png"},
    {321, L"prompts\\ps_solid\\L2.png"},
    {322, L"prompts\\ps_solid\\Left Stick All.png"},
    {323, L"prompts\\ps_solid\\Left Stick Click.png"},
    {324, L"prompts\\ps_solid\\Left Stick Left-Right.png"},
    {325, L"prompts\\ps_solid\\Options.png"},
    {326, L"prompts\\ps_solid\\R1.png"},
    {327, L"prompts\\ps_solid\\R2.png"},
    {328, L"prompts\\ps_solid\\Right Stick Click.png"},
    {329, L"prompts\\ps_solid\\Right Stick Left-Right.png"},
    {330, L"prompts\\ps_solid\\Right Stick Up-Down.png"},
    {331, L"prompts\\ps_solid\\Right Stick.png"},
    {332, L"prompts\\ps_solid\\Square.png"},
    {333, L"prompts\\ps_solid\\Triangle.png"},
    {334, L"prompts\\ps_outline\\Circle.png"},
    {335, L"prompts\\ps_outline\\Create.png"},
    {336, L"prompts\\ps_outline\\Cross.png"},
    {337, L"prompts\\ps_outline\\L1.png"},
    {338, L"prompts\\ps_outline\\L2.png"},
    {339, L"prompts\\ps_outline\\Left Stick All.png"},
    {340, L"prompts\\ps_outline\\Left Stick Click.png"},
    {341, L"prompts\\ps_outline\\Left Stick Left-Right.png"},
    {342, L"prompts\\ps_outline\\Options.png"},
    {343, L"prompts\\ps_outline\\R1.png"},
    {344, L"prompts\\ps_outline\\R2.png"},
    {345, L"prompts\\ps_outline\\Right Stick Click.png"},
    {346, L"prompts\\ps_outline\\Right Stick Left-Right.png"},
    {347, L"prompts\\ps_outline\\Right Stick Up-Down.png"},
    {348, L"prompts\\ps_outline\\Right Stick.png"},
    {349, L"prompts\\ps_outline\\Square.png"},
    {350, L"prompts\\ps_outline\\Triangle.png"},
    {351, L"prompts\\LICENSE-zacksly.txt"},
    {352, L"prompts\\README.txt"},
    {353, L"textures\\6E42BC7CF738BCF0.png"},
    {354, L"textures\\README.txt"},
};

void EnsureBundledAssets() {
  const std::wstring root = GetExeDir();
  std::set<std::wstring> present_dirs;
  for (const auto& a : kBundledAssets) {
    const std::wstring rel(a.rel);
    const std::wstring dir = root + L"\\" + rel.substr(0, rel.find_last_of(L'\\'));
    if (present_dirs.count(dir)) continue;
    if (PathIsDirectoryW(dir.c_str())) { present_dirs.insert(dir); continue; }
    // First asset of a missing folder: create it and write every asset of it.
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    for (const auto& b : kBundledAssets) {
      const std::wstring brel(b.rel);
      if (root + L"\\" + brel.substr(0, brel.find_last_of(L'\\')) != dir) continue;
      HRSRC hres = FindResourceW(nullptr, MAKEINTRESOURCEW(b.res_id), RT_RCDATA);
      if (!hres) continue;
      HGLOBAL hmem = LoadResource(nullptr, hres);
      if (!hmem) continue;
      const DWORD size = SizeofResource(nullptr, hres);
      const void* data = LockResource(hmem);
      if (!data || !size) continue;
      std::ofstream f(root + L"\\" + brel, std::ios::binary | std::ios::trunc);
      if (f) f.write(static_cast<const char*>(data), size);
    }
    present_dirs.insert(dir);
  }
}

// launcher.ini: launcher_prompt_style = keyboard | xbox | ps_* (a folder under
// prompts\ with the icon PNGs, see prompts\README.txt). Migrates the nightly
// key launcher_key_prompts (on/off).
bool IsKnownPromptStyle(const std::string& v) {
  return v == "keyboard" || v == "xbox" || v == "ps_fullsolid" || v == "ps_solid" || v == "ps_outline";
}

std::string GetPromptStyle() {
  auto it = g_launcher_ini.find("launcher_prompt_style");
  if (it != g_launcher_ini.end()) {
    return IsKnownPromptStyle(it->second) ? it->second : "keyboard";
  }
  auto legacy = g_launcher_ini.find("launcher_key_prompts");
  if (legacy != g_launcher_ini.end() && legacy->second == "off") return "xbox";
  return "keyboard";
}

void GenerateKeyPromptOverlay() {
  const std::wstring dir = GetExeDir() + L"\\textures";
  ReadLauncherIni();
  const std::string style = GetPromptStyle();
  if (g_cvars.empty()) DefineCvars();
  LoadTomlValues(GetExeDir() + L"\\" + kGameTomlName);
  auto val = [](const char* key) -> std::string {
    for (const auto& c : g_cvars) if (c.key == key) return c.value;
    return std::string();
  };
  // Icon sets (PlayStation) live in prompts\<style>\; keyboard caps only make
  // sense with the keyboard driver on. Anything else = original Xbox icons.
  const bool icon_set = style.rfind("ps_", 0) == 0;
  const std::wstring icon_dir = GetExeDir() + L"\\prompts\\" + Widen(style);
  bool enabled = (style == "keyboard" && val("mnk_mode") == "true") ||
                 (icon_set && PathIsDirectoryW(icon_dir.c_str()));
  if (!enabled) {
    for (const auto& atlas : kPromptAtlases) {
      DeleteFileW((dir + L"\\" + atlas.hash + L".overlay.png").c_str());
    }
    return;
  }
  CreateDirectoryW(dir.c_str(), nullptr);
  CLSID png;
  if (GetPngEncoderClsid(&png) < 0) return;
  // The game's own UI font (Cinema Calligraphy), embedded as RCDATA and loaded
  // as a private font so nothing has to be installed. Segoe UI as fallback.
  PrivateFontCollection private_fonts;
  bool game_font = false;
  if (HRSRC hres = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_FONT), RT_RCDATA)) {
    if (HGLOBAL hmem = LoadResource(nullptr, hres)) {
      const DWORD size = SizeofResource(nullptr, hres);
      void* data = LockResource(hmem);
      if (data && size && private_fonts.AddMemoryFont(data, INT(size)) == Ok &&
          private_fonts.GetFamilyCount() > 0) {
        game_font = true;
      }
    }
  }
  FontFamily fallback(L"Segoe UI");
  FontFamily game_family;
  int found = 0;
  if (game_font) private_fonts.GetFamilies(1, &game_family, &found);
  const FontFamily& family = (game_font && found > 0) ? game_family : fallback;
  const FontStyle font_style = (game_font && found > 0) ? FontStyleRegular : FontStyleBold;

  // Movement keys as one label ("WASD") when they are single keys.
  std::wstring move_label;
  {
    std::wstring u = PromptKeyLabel(val("keybind_lstick_up")), l = PromptKeyLabel(val("keybind_lstick_left"));
    std::wstring d = PromptKeyLabel(val("keybind_lstick_down")), r = PromptKeyLabel(val("keybind_lstick_right"));
    move_label = (u.size() == 1 && l.size() == 1 && d.size() == 1 && r.size() == 1) ? (u + l + d + r) : L"MOVE";
  }

  for (const auto& atlas : kPromptAtlases) {
    const int S = atlas.scale;
    int gw = 0, gh = 0;
    for (const auto& c : atlas.cells) {
      gw = std::max(gw, c.x + c.w);
      gh = std::max(gh, c.y + c.h);
    }
    // Round the overlay up to the guest texture size (power of two >= extent).
    int size = 1;
    while (size < std::max(gw, gh)) size <<= 1;
    Bitmap bmp(size * S, size * S, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.Clear(Color(0, 0, 0, 0));
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    auto draw_cap = [&](const RectF& r, const std::wstring& label) {
      const float rad = std::min(r.Width, r.Height) * 0.16f;
      auto add_round = [&](GraphicsPath& path, const RectF& q) {
        path.AddArc(q.X, q.Y, rad * 2, rad * 2, 180, 90);
        path.AddArc(q.X + q.Width - rad * 2, q.Y, rad * 2, rad * 2, 270, 90);
        path.AddArc(q.X + q.Width - rad * 2, q.Y + q.Height - rad * 2, rad * 2, rad * 2, 0, 90);
        path.AddArc(q.X, q.Y + q.Height - rad * 2, rad * 2, rad * 2, 90, 90);
        path.CloseFigure();
      };
      GraphicsPath cap;
      add_round(cap, r);
      SolidBrush lip(Color(255, 150, 150, 150));
      g.FillPath(&lip, &cap);
      RectF face(r.X, r.Y, r.Width, r.Height - r.Height * 0.09f);
      GraphicsPath face_path;
      add_round(face_path, face);
      SolidBrush fill(Color(255, 238, 238, 238));
      g.FillPath(&fill, &face_path);
      Pen pen(Color(255, 35, 35, 35), std::max(1.0f, 0.4f * S));
      g.DrawPath(&pen, &cap);
      const size_t n = label.size();
      const float base = std::min(r.Width, r.Height);
      const float fsize = base * (n <= 1 ? 0.62f : n <= 2 ? 0.50f : n <= 4 ? 0.30f : n <= 6 ? 0.22f : 0.16f);
      Font font(&family, fsize, font_style, UnitPixel);
      StringFormat sf;
      sf.SetAlignment(StringAlignmentCenter);
      sf.SetLineAlignment(StringAlignmentCenter);
      SolidBrush text(Color(255, 25, 25, 25));
      g.DrawString(label.c_str(), -1, &font, face, &sf, &text);
    };

    // Mouse glyph on a cap: body, button split, a big highlighted wheel and
    // up/down arrows beside it ("scroll / move it forward-back").
    auto draw_mouse = [&](const RectF& r) {
      draw_cap(r, L"");
      RectF face(r.X, r.Y, r.Width, r.Height - r.Height * 0.09f);
      const float h = face.Height * 0.74f;
      const float w = std::min(h * 0.62f, face.Width * 0.55f);
      const float arrows_w = w * 0.42f;
      const float total_w = w + arrows_w * 1.15f;
      const float x = face.X + (face.Width - total_w) * 0.5f;
      const float y = face.Y + (face.Height - h) * 0.5f;
      const float stroke = std::max(1.0f, 0.5f * S);
      GraphicsPath body;
      body.AddArc(x, y, w, w * 0.9f, 180, 180);                       // top dome
      body.AddArc(x, y + h - w * 0.9f, w, w * 0.9f, 0, 180);          // bottom dome
      body.CloseFigure();
      SolidBrush white(Color(255, 255, 255, 255));
      g.FillPath(&white, &body);
      Pen pen(Color(255, 35, 35, 35), stroke);
      g.DrawPath(&pen, &body);
      const float split_y = y + h * 0.40f;
      g.DrawLine(&pen, x, split_y, x + w, split_y);                    // buttons bottom
      g.DrawLine(&pen, x + w * 0.5f, y, x + w * 0.5f, split_y);        // left/right split
      // Wheel: large, yellow like the atlas arrows, dark outline.
      const float ww = w * 0.30f, wh = h * 0.30f;
      RectF wheel(x + w * 0.5f - ww * 0.5f, y + h * 0.06f, ww, wh);
      GraphicsPath wheel_path;
      wheel_path.AddArc(wheel.X, wheel.Y, ww, ww, 180, 180);
      wheel_path.AddArc(wheel.X, wheel.Y + wh - ww, ww, ww, 0, 180);
      wheel_path.CloseFigure();
      SolidBrush yellow(Color(255, 242, 211, 27));
      g.FillPath(&yellow, &wheel_path);
      g.DrawPath(&pen, &wheel_path);
      // Up / down arrows to the right of the mouse.
      SolidBrush dark(Color(255, 35, 35, 35));
      const float ax = x + w + arrows_w * 0.15f;
      const float aw = arrows_w, ah = h * 0.26f;
      const float cy = y + h * 0.5f;
      PointF up[3] = {PointF(ax, cy - h * 0.10f), PointF(ax + aw, cy - h * 0.10f), PointF(ax + aw * 0.5f, cy - h * 0.10f - ah)};
      PointF down[3] = {PointF(ax, cy + h * 0.10f), PointF(ax + aw, cy + h * 0.10f), PointF(ax + aw * 0.5f, cy + h * 0.10f + ah)};
      g.FillPolygon(&dark, up, 3);
      g.FillPolygon(&dark, down, 3);
    };

    // Icon set cell: erase the original (alpha == 1 = "erase" for the runtime
    // overlay compositor), then the PNG scaled to fit, centred.
    auto draw_icon = [&](const RectF& r, const wchar_t* name) -> bool {
      const std::wstring path = icon_dir + L"\\" + name + L".png";
      Bitmap icon(path.c_str());
      if (icon.GetLastStatus() != Ok || icon.GetWidth() == 0) return false;
      SolidBrush erase(Color(1, 0, 0, 0));
      g.SetCompositingMode(CompositingModeSourceCopy);
      g.FillRectangle(&erase, r);
      g.SetCompositingMode(CompositingModeSourceOver);
      const float sc = std::min(r.Width / float(icon.GetWidth()), r.Height / float(icon.GetHeight()));
      const float w = icon.GetWidth() * sc, h = icon.GetHeight() * sc;
      g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
      g.DrawImage(&icon, RectF(r.X + (r.Width - w) * 0.5f, r.Y + (r.Height - h) * 0.5f, w, h));
      return true;
    };
    static const std::pair<const char*, const wchar_t*> kIconNames[] = {
        {"keybind_a", L"Cross"},                {"keybind_b", L"Circle"},
        {"keybind_x", L"Square"},               {"keybind_y", L"Triangle"},
        {"keybind_left_shoulder", L"L1"},       {"keybind_right_shoulder", L"R1"},
        {"keybind_left_trigger", L"L2"},        {"keybind_right_trigger", L"R2"},
        {"keybind_lstick_press", L"Left Stick Click"}, {"keybind_rstick_press", L"Right Stick Click"},
        {"@move", L"Left Stick All"},           {"@mouse", L"Right Stick"},
        {"@shake_l", L"Left Stick Left-Right"}, {"@shake_r", L"Right Stick Left-Right"},
    };

    for (const auto& c : atlas.cells) {
      const std::string key(c.key);
      const float m = 1.0f * S;
      RectF r(float(c.x * S) + m, float(c.y * S) + m, float(c.w * S) - 2 * m, float(c.h * S) - 2 * m);
      if (icon_set) {
        for (const auto& n : kIconNames) {
          if (key == n.first) {
            // full cell (no margin) so the erase covers the original icon
            RectF full(float(c.x * S), float(c.y * S), float(c.w * S), float(c.h * S));
            draw_icon(full, n.second);
            break;
          }
        }
        continue;
      }
      if (key == "@shake_l") {
        // two caps side by side: the left and right movement keys
        RectF a(r.X, r.Y, r.Width * 0.5f - m * 0.5f, r.Height);
        RectF d(r.X + r.Width * 0.5f + m * 0.5f, r.Y, r.Width * 0.5f - m * 0.5f, r.Height);
        draw_cap(a, PromptKeyLabel(val("keybind_lstick_left")));
        draw_cap(d, PromptKeyLabel(val("keybind_lstick_right")));
        continue;
      }
      if (key == "@mouse" || key == "@shake_r") {
        draw_mouse(r);
        continue;
      }
      std::wstring label;
      if (key == "@move") label = move_label;
      else {
        const std::string bind = val(c.key);
        if (bind.empty()) continue;
        label = PromptKeyLabel(bind);
      }
      draw_cap(r, label);
    }
    bmp.Save((dir + L"\\" + atlas.hash + L".overlay.png").c_str(), &png, nullptr);
  }
}

// v1.1: opt-in shader cache sharing. Fingerprints the shareable storage files
// (name:size:mtime), skips when unchanged since the last upload, otherwise
// hands the job to a hidden PowerShell script (Compress-Archive + curl.exe
// multipart POST to the Discord webhook) so the launcher never blocks. The
// fingerprint is recorded before the upload; a failed upload is retried the
// next time the cache changes.
void MaybeShareShaderCache() {
  if (kShaderCacheWebhookUrl[0] == L'\0') return;
  ReadLauncherIni();
  // First run: ask once. The answer is recorded either way so the question
  // never comes back; Settings -> Advanced can flip it later.
  if (g_launcher_ini.find("launcher_share_shader_cache") == g_launcher_ini.end()) {
    const int answer = MessageBoxW(
        nullptr,
        TrC("Share your shader cache with the project?\n\nWhen enabled, the launcher sends the shader cache the game builds while you play (only shader microcode and pipeline descriptions - no personal data, no save games) to the developers. Merged caches ship with the next release, so people who play after you get fewer stutters in new scenes.\n\nYou can change this later in Settings -> Advanced -> Share Shader Cache."),
        TrC("Help other players?"), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
    g_launcher_ini["launcher_share_shader_cache"] = (answer == IDYES) ? "on" : "off";
    WriteLauncherIni();
  }
  auto en = g_launcher_ini.find("launcher_share_shader_cache");
  if (en == g_launcher_ini.end() || en->second != "on") return;
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::path(GetExeDir()) / L"userdata" / L"cache" / L"shaders" / L"shareable";
  if (!fs::is_directory(dir, ec)) return;
  std::string fp;
  std::string summary;
  unsigned long long total = 0;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::wstring ext = e.path().extension().wstring();
    if (ext != L".xsh" && ext != L".xpso") continue;
    const auto size = (unsigned long long)e.file_size(ec);
    const auto mtime = (long long)e.last_write_time(ec).time_since_epoch().count();
    const std::string name = Narrow(e.path().filename().c_str());
    fp += name + ":" + std::to_string(size) + ":" + std::to_string(mtime) + ";";
    summary += name + " " + std::to_string(size / 1024) + "KB, ";
    total += size;
  }
  if (fp.empty() || total < 4096) return;
  // Cheap 64-bit FNV-1a of the descriptor string.
  unsigned long long h = 1469598103934665603ull;
  for (unsigned char c : fp) { h ^= c; h *= 1099511628211ull; }
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", h);
  auto done = g_launcher_ini.find("shader_cache_shared_fp");
  if (done != g_launcher_ini.end() && done->second == hex) return;
  g_launcher_ini["shader_cache_shared_fp"] = hex;
  WriteLauncherIni();

  wchar_t tmp[MAX_PATH];
  if (!GetTempPathW(MAX_PATH, tmp)) return;
  const std::wstring script = std::wstring(tmp) + L"dp1_share_cache.ps1";
  const std::wstring zip = std::wstring(tmp) + L"DPRecomp_shadercache_" + Widen(hex) + L".zip";
  std::string content = "DPRecomp shader cache | launcher " + Narrow(kLauncherVersion) +
                        " | " + summary + "total " + std::to_string(total / 1024) + " KB | fp " + hex;
  std::string ps;
  ps += "$ErrorActionPreference = 'Stop'\r\n";
  ps += "$src = '" + Narrow(dir.wstring().c_str()) + "'\r\n";
  ps += "$zip = '" + Narrow(zip.c_str()) + "'\r\n";
  ps += "Remove-Item -LiteralPath $zip -ErrorAction SilentlyContinue\r\n";
  ps += "Compress-Archive -Path (Join-Path $src '*.xsh'), (Join-Path $src '*.xpso') -DestinationPath $zip -Force\r\n";
  ps += "$json = '{\"content\":\"" + content + "\"}'\r\n";
  ps += "& curl.exe -s -S -f -F \"payload_json=$json\" -F \"file=@$zip\" '" +
        Narrow(kShaderCacheWebhookUrl) + "' | Out-Null\r\n";
  ps += "Remove-Item -LiteralPath $zip -ErrorAction SilentlyContinue\r\n";
  {
    std::ofstream f(script, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << ps;
  }
  std::wstring cmd = L"powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File \"" +
                     script + L"\"";
  STARTUPINFOW si{}; si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                     nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
  INITCOMMONCONTROLSEX icc = {sizeof(icc),
                              ICC_TAB_CLASSES | ICC_STANDARD_CLASSES |
                                  ICC_PROGRESS_CLASS | ICC_BAR_CLASSES};
  InitCommonControlsEx(&icc);

  GdiplusStartupInput gdip_input;
  GdiplusStartup(&g_gdiplus_token, &gdip_input, nullptr);

  LoadLauncherLanguageFromToml();
  // Sync the sidecar at startup so even if the user never opens Settings,
  // their language choice survives the next SDK F4 SaveConfig wipe.
  SaveLauncherLanguageSidecar();
  EnsurePerfDefaultsInToml();
  ApplySteamDeckPresetIfDetected();
  EnsureBundledAssets();
  GenerateKeyPromptOverlay();
  MaybeShareShaderCache();
  // If %TEMP%\dp1_update.log exists, the previous auto-updater run did not
  // complete — offer the user the log for diagnostic. Runs BEFORE the main
  // window is created so the dialog is the first thing they see.
  CheckPreviousUpdateLog();
  LoadAssets();
  CreateFonts();
  DefineCvars();
  // Background music is optional: this build embeds no IDR_MUSIC resource
  // (the Downpour track was game-branded), so ExtractMusicToTemp leaves the
  // temp path empty and StartMusic/StopMusic are no-ops. Dropping an OGG back
  // into resources.rc re-enables it without code changes.
  ExtractMusicToTemp();
  StartMusic();

  WNDCLASSEXW wc = {sizeof(wc)};
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = kWindowClass;
  wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                0, 0, LR_DEFAULTSIZE | LR_DEFAULTCOLOR);
  wc.hIconSm = wc.hIcon;
  RegisterClassExW(&wc);

  DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);

  RECT rc = {0, 0, kWindowWidth, kWindowHeight};
  AdjustWindowRect(&rc, style, FALSE);
  int win_w = rc.right - rc.left;
  int win_h = rc.bottom - rc.top;
  int sx = (GetSystemMetrics(SM_CXSCREEN) - win_w) / 2;
  int sy = (GetSystemMetrics(SM_CYSCREEN) - win_h) / 2;

  // Compose full title at runtime so a single kLauncherVersion bump updates
  // the taskbar caption, Alt-Tab tooltip, and Win11 jump-list entry. The
  // standalone kWindowTitle constant is used by MessageBox error popups
  // (where a short title fits better).
  std::wstring full_title =
      std::wstring(kWindowTitle) + L" " + kLauncherVersion + kWindowTitleAuthorSuffix;
  HWND hwnd = CreateWindowExW(0, kWindowClass, full_title.c_str(), style,
                              sx, sy, win_w, win_h,
                              nullptr, nullptr, hInst, nullptr);
  if (!hwnd) return 1;

  ShowWindow(hwnd, nCmdShow);
  UpdateWindow(hwnd);

  // Fire-and-forget GitHub release probe. Repaints the window with the
  // update banner if a newer tag is found. Failures are silent — no need
  // to surface network issues at the launcher boot.
  ProbeReleaseAsync(hwnd);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  StopMusic();
  if (g_button_font) DeleteObject(g_button_font);
  if (g_hint_font) DeleteObject(g_hint_font);
  if (g_title_font) DeleteObject(g_title_font);
  g_banner_bitmap.reset();
  g_logo_bitmap.reset();
  GdiplusShutdown(g_gdiplus_token);
  return 0;
}
