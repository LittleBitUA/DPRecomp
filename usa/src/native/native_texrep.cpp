// [NEW FABLE VERSION] 2026-09-23
// Texture replacement for the native renderer: the file side (see
// native_texrep.h). PNGs are decoded with WIC, which ships with Windows, since
// the SDK's stb_image copy is private to its GPU plugin.
#include "native_texrep.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/hash.h>  // xxhash with XXH_INLINE_ALL (XXH64 is not exported by the runtime)

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace dp::native::texrep {

namespace {

struct File {
  std::filesystem::path path;
  bool overlay = false;
};

struct Registry {
  std::mutex mutex;
  bool scanned = false;
  std::unordered_map<uint64_t, File> files;
  std::unordered_map<uint64_t, std::unique_ptr<Image>> images;
};

Registry& Get() {
  static Registry registry;
  return registry;
}

// The SDK's texture_path / texture_replacement cvars live in the GPU plugin,
// which the native renderer does not load; read them by name when registered.
void ScanLocked(Registry& r) {
  r.scanned = true;
  const std::string enabled = rex::cvar::GetFlagByName("texture_replacement");
  if (enabled == "false" || enabled == "0") {
    REXLOG_INFO("Native texture replacement: disabled (texture_replacement = false)");
    return;
  }
  const std::string configured = rex::cvar::GetFlagByName("texture_path");
  const std::filesystem::path root =
      configured.empty() ? rex::filesystem::GetExecutableFolder() / "textures" : std::filesystem::path(configured);
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) return;
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const std::filesystem::path& p = entry.path();
    std::string ext = p.extension().string();
    for (char& c : ext) c = char(::tolower(static_cast<unsigned char>(c)));
    if (ext != ".png") continue;
    const std::string stem = p.stem().string();
    uint64_t hash = 0;
    bool overlay = false;
    if (!ParseName(stem, &hash, &overlay)) continue;
    // Same precedence as the SDK: a full replacement wins over an overlay.
    auto existing = r.files.find(hash);
    if (existing != r.files.end() && !existing->second.overlay && overlay) continue;
    r.files[hash] = File{p, overlay};
  }
  if (!r.files.empty()) {
    REXLOG_INFO("Native texture replacement: {} file(s) in {}", r.files.size(), root.string());
  }
}

bool DecodePng(const std::filesystem::path& path, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
  using Microsoft::WRL::ComPtr;
  // The guest thread may not have COM yet; any apartment works for WIC.
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool co_owned = SUCCEEDED(co);
  bool ok = false;
  {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT w = 0, h = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.wstring().c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        // 32bppRGBA is straight (not premultiplied) alpha, as stb_image returns it.
        SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                        0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(converter->GetSize(&w, &h)) && w && h) {
      rgba.resize(size_t(w) * h * 4);
      if (SUCCEEDED(converter->CopyPixels(nullptr, w * 4, UINT(rgba.size()), rgba.data()))) {
        width = w;
        height = h;
        ok = true;
      }
    }
  }
  if (co_owned) CoUninitialize();
  return ok;
}

}  // namespace

bool Active() {
  Registry& r = Get();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (!r.scanned) ScanLocked(r);
  return !r.files.empty();
}

uint64_t Hash(const uint8_t* data, size_t size, uint32_t width, uint32_t height, uint32_t format) {
  return XXH64(data, size, HashSeed(width, height, format));
}

const Image* Find(uint64_t hash, const GuestDecoder& decode_guest) {
  Registry& r = Get();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (!r.scanned) ScanLocked(r);
  if (auto cached = r.images.find(hash); cached != r.images.end()) return cached->second.get();
  auto file = r.files.find(hash);
  if (file == r.files.end()) return nullptr;
  std::unique_ptr<Image> image;
  std::vector<uint8_t> pixels;
  uint32_t w = 0, h = 0;
  if (!DecodePng(file->second.path, pixels, w, h)) {
    REXLOG_WARN("Native texture replacement: failed to decode {}", file->second.path.string());
  } else {
    bool ok = true;
    if (file->second.overlay) {
      std::vector<uint8_t> guest;
      uint32_t gw = 0, gh = 0;
      ok = decode_guest && decode_guest(guest, gw, gh) && gw && gh;
      if (ok) {
        ComposeOverlay(guest.data(), gw, gh, pixels.data(), w, h);
      } else {
        REXLOG_WARN("Native texture replacement: overlay {} needs a decodable guest texture (format not supported)",
                    file->second.path.filename().string());
      }
    }
    if (ok) {
      image = std::make_unique<Image>();
      image->width = w;
      image->height = h;
      image->levels.push_back(std::move(pixels));
      BuildMips(*image);
      REXLOG_INFO("Native texture replacement: {:016X} <- {} ({}x{}, {} mips{})", hash,
                  file->second.path.filename().string(), w, h, image->levels.size(),
                  file->second.overlay ? ", overlay" : "");
    }
  }
  const Image* result = image.get();
  r.images.emplace(hash, std::move(image));
  return result;
}

}  // namespace dp::native::texrep
