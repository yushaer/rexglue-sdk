/**
 * @file        graphics/pipeline/texture_replacement.cpp
 * @brief       Texture packs: the game's textures replaced by files, and dumped
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/pipeline/texture_replacement.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>
#include <xxhash.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#include <stb_image.h>

#ifdef _WIN32
#include <wincodec.h>
#include <wrl/client.h>
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/pipeline/material_shaders.h>
#include <rex/logging.h>

// Texture packs are read once, at startup: replacing the textures - and the
// maps material shaders sample - of a running game isn't supported.
REXCVAR_DEFINE_BOOL(texture_replacement, true, "GPU/Textures",
                    "Replace the game's textures with those in mods' textures folders (and "
                    "<executable folder>/textures)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(dump_textures, "", "GPU/Textures",
                      "Write each 2D texture the game loads to this folder as <hash>.png, for "
                      "making replacements")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::texture_replacement {

namespace {

std::mutex index_mutex;
std::unordered_map<uint64_t, std::filesystem::path> replacements;
// Companion maps by (hash, kind).
std::unordered_map<uint64_t, std::filesystem::path> companions[kCompanionKindCount];
std::atomic<bool> scanned{false};

// The companion kind a file name's last _suffix names, if any.
std::optional<uint32_t> ParseCompanionKind(const std::filesystem::path& path) {
  std::string stem = path.stem().u8string();
  size_t separator = stem.find_last_of('_');
  if (separator == std::string::npos || separator < 16) {
    return std::nullopt;
  }
  std::string suffix = stem.substr(separator + 1);
  std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  static const char* const kSuffixes[kCompanionKindCount] = {"normal", "orm", "height",
                                                             "emissive"};
  for (uint32_t kind = 0; kind < kCompanionKindCount; ++kind) {
    if (suffix == kSuffixes[kind]) {
      return kind;
    }
  }
  return std::nullopt;
}

// texture_replacement as it was at startup (it applies after restarting).
bool Enabled() {
  static const bool enabled = REXCVAR_GET(texture_replacement);
  return enabled;
}

std::mutex dump_mutex;
std::unordered_set<uint64_t> dumped;

std::filesystem::path Utf8Path(std::string_view text) {
  return std::filesystem::u8path(text.begin(), text.end());
}

// The texture hashes a file name starts with: one, or several joined by + (an
// image standing in for several textures - like the same picture at the sizes
// the game streams it at), then notes after a separator if any. None if the
// name isn't like that.
std::vector<uint64_t> ParseHashNames(const std::filesystem::path& path) {
  std::string stem = path.stem().u8string();
  std::vector<uint64_t> hashes;
  size_t position = 0;
  while (true) {
    if (stem.size() < position + 16) {
      return {};
    }
    uint64_t hash = 0;
    for (size_t i = position; i < position + 16; ++i) {
      char c = char(std::toupper(uint8_t(stem[i])));
      uint32_t digit;
      if (c >= '0' && c <= '9') {
        digit = uint32_t(c - '0');
      } else if (c >= 'A' && c <= 'F') {
        digit = uint32_t(c - 'A' + 10);
      } else {
        return {};
      }
      hash = (hash << 4) | digit;
    }
    hashes.push_back(hash);
    position += 16;
    if (position >= stem.size() || stem[position] != '+') {
      break;
    }
    ++position;
  }
  if (position < stem.size() && stem[position] != '_' && stem[position] != '-' &&
      stem[position] != ' ' && stem[position] != '.') {
    return {};
  }
  return hashes;
}

bool IsImageExtension(std::string extension) {
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return extension == ".png" || extension == ".dds" || extension == ".jpg" ||
         extension == ".jpeg" || extension == ".tga" || extension == ".bmp";
}

// Not under index_mutex (Rescan takes it).
void EnsureScanned() {
  if (!scanned.load(std::memory_order_acquire)) {
    Rescan();
  }
}

uint32_t BlockBytes(ImageFormat format) {
  switch (format) {
    case ImageFormat::kBC1:
    case ImageFormat::kBC4:
      return 8;
    case ImageFormat::kBC2:
    case ImageFormat::kBC3:
    case ImageFormat::kBC5:
    case ImageFormat::kBC7:
      return 16;
    default:
      return 0;
  }
}

// The mip chain's layout, packed.
void LayOutMips(Image& image, uint32_t mip_count) {
  image.mips.clear();
  size_t offset = 0;
  uint32_t block_bytes = BlockBytes(image.format);
  for (uint32_t level = 0; level < mip_count; ++level) {
    Image::Mip mip;
    mip.width = std::max(image.width >> level, 1u);
    mip.height = std::max(image.height >> level, 1u);
    mip.offset = offset;
    if (block_bytes) {
      mip.row_pitch = std::max((mip.width + 3) / 4, 1u) * block_bytes;
      mip.row_count = std::max((mip.height + 3) / 4, 1u);
    } else {
      mip.row_pitch = mip.width * 4;
      mip.row_count = mip.height;
    }
    offset += size_t(mip.row_pitch) * mip.row_count;
    image.mips.push_back(mip);
  }
}

uint32_t FullMipCount(uint32_t width, uint32_t height) {
  uint32_t count = 1;
  while ((width | height) > 1) {
    width = std::max(width >> 1, 1u);
    height = std::max(height >> 1, 1u);
    ++count;
  }
  return count;
}

// 8-bit RGBA mipmaps by 2x2 box filtering.
void GenerateMips(Image& image) {
  LayOutMips(image, FullMipCount(image.width, image.height));
  image.data.resize(image.mips.back().offset +
                    size_t(image.mips.back().row_pitch) * image.mips.back().row_count);
  for (size_t level = 1; level < image.mips.size(); ++level) {
    const Image::Mip& source = image.mips[level - 1];
    const Image::Mip& target = image.mips[level];
    for (uint32_t y = 0; y < target.height; ++y) {
      for (uint32_t x = 0; x < target.width; ++x) {
        uint32_t sum[4] = {};
        for (uint32_t sy = 0; sy < 2; ++sy) {
          for (uint32_t sx = 0; sx < 2; ++sx) {
            uint32_t px = std::min(x * 2 + sx, source.width - 1);
            uint32_t py = std::min(y * 2 + sy, source.height - 1);
            const uint8_t* texel =
                image.data.data() + source.offset + size_t(py) * source.row_pitch + px * 4;
            for (uint32_t c = 0; c < 4; ++c) {
              sum[c] += texel[c];
            }
          }
        }
        uint8_t* out = image.data.data() + target.offset + size_t(y) * target.row_pitch + x * 4;
        for (uint32_t c = 0; c < 4; ++c) {
          out[c] = uint8_t((sum[c] + 2) / 4);
        }
      }
    }
  }
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& data_out) {
  FILE* file = rex::filesystem::OpenFile(path, "rb");
  if (!file) {
    return false;
  }
  std::error_code error;
  data_out.resize(size_t(std::filesystem::file_size(path, error)));
  bool read = !error && !data_out.empty() &&
              fread(data_out.data(), 1, data_out.size(), file) == data_out.size();
  fclose(file);
  return read;
}

uint32_t ReadU32(const uint8_t* data) {
  uint32_t value;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

constexpr uint32_t MakeFourCC(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) |
         (uint32_t(uint8_t(d)) << 24);
}

bool LoadDds(const std::vector<uint8_t>& file, Image& image) {
  if (file.size() < 128 || ReadU32(file.data()) != MakeFourCC('D', 'D', 'S', ' ')) {
    return false;
  }
  const uint8_t* header = file.data() + 4;
  uint32_t flags = ReadU32(header + 4);
  image.height = ReadU32(header + 8);
  image.width = ReadU32(header + 12);
  uint32_t mip_count = (flags & 0x20000) ? std::max(ReadU32(header + 24), 1u) : 1;
  const uint8_t* pixel_format = header + 72;
  uint32_t pf_flags = ReadU32(pixel_format + 4);
  uint32_t four_cc = ReadU32(pixel_format + 8);
  uint32_t bit_count = ReadU32(pixel_format + 12);
  uint32_t r_mask = ReadU32(pixel_format + 16), g_mask = ReadU32(pixel_format + 20),
           b_mask = ReadU32(pixel_format + 24), a_mask = ReadU32(pixel_format + 28);
  size_t data_offset = 128;
  bool swap_red_blue = false;
  bool format_known = true;
  if ((pf_flags & 0x4) && four_cc == MakeFourCC('D', 'X', '1', '0')) {
    if (file.size() < 148) {
      return false;
    }
    uint32_t dxgi_format = ReadU32(file.data() + 128);
    data_offset = 148;
    switch (dxgi_format) {
      case 70:  // BC1_TYPELESS
      case 71:  // BC1_UNORM
      case 72:  // BC1_UNORM_SRGB
        image.format = ImageFormat::kBC1;
        break;
      case 73:
      case 74:
      case 75:
        image.format = ImageFormat::kBC2;
        break;
      case 76:
      case 77:
      case 78:
        image.format = ImageFormat::kBC3;
        break;
      case 79:
      case 80:
        image.format = ImageFormat::kBC4;
        break;
      case 82:
      case 83:
        image.format = ImageFormat::kBC5;
        break;
      case 97:
      case 98:
      case 99:
        image.format = ImageFormat::kBC7;
        break;
      case 27:  // R8G8B8A8_TYPELESS
      case 28:  // R8G8B8A8_UNORM
      case 29:  // R8G8B8A8_UNORM_SRGB
        image.format = ImageFormat::kRGBA8;
        break;
      case 87:  // B8G8R8A8_UNORM
      case 90:
      case 91:
        image.format = ImageFormat::kRGBA8;
        swap_red_blue = true;
        break;
      default:
        format_known = false;
    }
  } else if (pf_flags & 0x4) {
    if (four_cc == MakeFourCC('D', 'X', 'T', '1')) {
      image.format = ImageFormat::kBC1;
    } else if (four_cc == MakeFourCC('D', 'X', 'T', '2') ||
               four_cc == MakeFourCC('D', 'X', 'T', '3')) {
      image.format = ImageFormat::kBC2;
    } else if (four_cc == MakeFourCC('D', 'X', 'T', '4') ||
               four_cc == MakeFourCC('D', 'X', 'T', '5')) {
      image.format = ImageFormat::kBC3;
    } else if (four_cc == MakeFourCC('A', 'T', 'I', '1') ||
               four_cc == MakeFourCC('B', 'C', '4', 'U')) {
      image.format = ImageFormat::kBC4;
    } else if (four_cc == MakeFourCC('A', 'T', 'I', '2') ||
               four_cc == MakeFourCC('B', 'C', '5', 'U')) {
      image.format = ImageFormat::kBC5;
    } else {
      format_known = false;
    }
  } else if ((pf_flags & 0x40) && bit_count == 32) {
    image.format = ImageFormat::kRGBA8;
    if (r_mask == 0x00FF0000 && g_mask == 0x0000FF00 && b_mask == 0x000000FF) {
      swap_red_blue = true;
    } else if (!(r_mask == 0x000000FF && g_mask == 0x0000FF00 && b_mask == 0x00FF0000)) {
      format_known = false;
    }
    if (!(pf_flags & 0x1) || !a_mask) {
      // No alpha - opaque.
      a_mask = 0;
    }
  } else {
    format_known = false;
  }
  if (!format_known || !image.width || !image.height) {
    return false;
  }
  LayOutMips(image, std::min(mip_count, FullMipCount(image.width, image.height)));
  size_t size = image.mips.back().offset +
                size_t(image.mips.back().row_pitch) * image.mips.back().row_count;
  if (file.size() < data_offset + size) {
    // Missing mips - keep what's there.
    while (image.mips.size() > 1 &&
           file.size() < data_offset + image.mips.back().offset +
                             size_t(image.mips.back().row_pitch) * image.mips.back().row_count) {
      image.mips.pop_back();
    }
    size = image.mips.back().offset +
           size_t(image.mips.back().row_pitch) * image.mips.back().row_count;
    if (file.size() < data_offset + size) {
      return false;
    }
  }
  image.data.assign(file.begin() + data_offset, file.begin() + data_offset + size);
  if (image.format == ImageFormat::kRGBA8) {
    for (size_t i = 0; i + 3 < image.data.size(); i += 4) {
      if (swap_red_blue) {
        std::swap(image.data[i], image.data[i + 2]);
      }
      if ((pf_flags & 0x40) && !a_mask) {
        image.data[i + 3] = 255;
      }
    }
    if (image.mips.size() == 1 && (image.width > 1 || image.height > 1)) {
      GenerateMips(image);
    }
  }
  return true;
}

}  // namespace

bool IsEnabled() {
  if (!Enabled()) {
    return false;
  }
  EnsureScanned();
  std::lock_guard<std::mutex> lock(index_mutex);
  bool any_companion = false;
  for (const auto& kind_companions : companions) {
    any_companion |= !kind_companions.empty();
  }
  return !replacements.empty() || any_companion;
}

void Rescan() {
  std::unordered_map<uint64_t, std::filesystem::path> found;
  std::unordered_map<uint64_t, std::filesystem::path> found_companions[kCompanionKindCount];
  std::vector<std::filesystem::path> folders;
  for (const std::filesystem::path& mod_folder : material_shaders::GetModFolders()) {
    folders.push_back(mod_folder / "textures");
  }
  folders.push_back(rex::filesystem::GetExecutableFolder() / "textures");
  std::error_code error;
  for (const std::filesystem::path& folder : folders) {
    if (!std::filesystem::is_directory(folder, error)) {
      continue;
    }
    uint32_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, error)) {
      if (!entry.is_regular_file(error) || !IsImageExtension(entry.path().extension().u8string())) {
        continue;
      }
      // Earlier (higher priority) folders win.
      std::optional<uint32_t> companion_kind = ParseCompanionKind(entry.path());
      for (uint64_t hash : ParseHashNames(entry.path())) {
        if (companion_kind) {
          if (found_companions[*companion_kind].emplace(hash, entry.path()).second) {
            ++count;
          }
        } else if (found.emplace(hash, entry.path()).second) {
          ++count;
        }
      }
    }
    if (count) {
      REXGPU_INFO("Texture replacements: {} in {}", count, folder.u8string());
    }
  }
  std::lock_guard<std::mutex> lock(index_mutex);
  replacements = std::move(found);
  for (uint32_t kind = 0; kind < kCompanionKindCount; ++kind) {
    companions[kind] = std::move(found_companions[kind]);
  }
  scanned.store(true, std::memory_order_release);
}

std::optional<std::filesystem::path> FindCompanion(uint64_t hash, uint32_t kind) {
  if (kind >= kCompanionKindCount || !Enabled()) {
    return std::nullopt;
  }
  EnsureScanned();
  std::lock_guard<std::mutex> lock(index_mutex);
  auto it = companions[kind].find(hash);
  if (it == companions[kind].end()) {
    return std::nullopt;
  }
  return it->second;
}


std::optional<std::filesystem::path> Find(uint64_t hash) {
  EnsureScanned();
  std::lock_guard<std::mutex> lock(index_mutex);
  auto it = replacements.find(hash);
  if (it == replacements.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool LoadImageFile(const std::filesystem::path& path, Image& image_out) {
  std::vector<uint8_t> file;
  if (!ReadFile(path, file)) {
    REXGPU_WARN("Texture replacement {} could not be read", path.u8string());
    return false;
  }
  image_out = Image();
  std::string extension = path.extension().u8string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  if (extension == ".dds") {
    if (!LoadDds(file, image_out)) {
      REXGPU_WARN("Texture replacement {}: unsupported DDS (BC1-5, BC7 or 8-bit RGBA only)",
                  path.u8string());
      return false;
    }
    return true;
  }
  int width, height, channels;
  stbi_uc* pixels = stbi_load_from_memory(file.data(), int(file.size()), &width, &height,
                                          &channels, 4);
  if (!pixels) {
    REXGPU_WARN("Texture replacement {}: {}", path.u8string(), stbi_failure_reason());
    return false;
  }
  image_out.format = ImageFormat::kRGBA8;
  image_out.width = uint32_t(width);
  image_out.height = uint32_t(height);
  image_out.data.assign(pixels, pixels + size_t(width) * height * 4);
  stbi_image_free(pixels);
  GenerateMips(image_out);
  return true;
}

uint64_t HashGuestTexture(const void* data, size_t size, uint32_t format, uint32_t width,
                          uint32_t height, uint32_t pitch, bool tiled, uint32_t endianness) {
  uint64_t seed = uint64_t(format) | (uint64_t(width) << 8) | (uint64_t(height) << 22) |
                  (uint64_t(pitch) << 36) | (uint64_t(tiled) << 50) |
                  (uint64_t(endianness) << 51);
  return XXH3_64bits_withSeed(data, size, seed);
}

bool IsDumping() { return !REXCVAR_GET(dump_textures).empty(); }

bool ShouldDump(uint64_t hash) {
  if (REXCVAR_GET(dump_textures).empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(dump_mutex);
  return !dumped.count(hash);
}

void Dump(uint64_t hash, uint32_t width, uint32_t height, const uint8_t* rgba, size_t row_pitch) {
  {
    std::lock_guard<std::mutex> lock(dump_mutex);
    if (!dumped.insert(hash).second) {
      return;
    }
  }
  std::filesystem::path folder = Utf8Path(REXCVAR_GET(dump_textures));
  if (folder.is_relative()) {
    folder = rex::filesystem::GetExecutableFolder() / folder;
  }
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  std::filesystem::path path = folder / Utf8Path(fmt::format("{:016X}.png", hash));
  if (std::filesystem::exists(path, error)) {
    return;
  }
#ifdef _WIN32
  // BGRA for the PNG encoder.
  std::vector<uint8_t> bgra(size_t(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* source = rgba + size_t(y) * row_pitch;
    uint8_t* target = bgra.data() + size_t(y) * width * 4;
    for (uint32_t x = 0; x < width; ++x) {
      target[x * 4 + 0] = source[x * 4 + 2];
      target[x * 4 + 1] = source[x * 4 + 1];
      target[x * 4 + 2] = source[x * 4 + 0];
      target[x * 4 + 3] = source[x * 4 + 3];
    }
  }
  HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool written = false;
  {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    written =
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(width, height)) &&
        SUCCEEDED(frame->SetPixelFormat(&pixel_format)) &&
        IsEqualGUID(pixel_format, GUID_WICPixelFormat32bppBGRA) &&
        SUCCEEDED(frame->WritePixels(height, width * 4, UINT(bgra.size()), bgra.data())) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
  }
  if (SUCCEEDED(com_result)) {
    CoUninitialize();
  }
  if (!written) {
    REXGPU_WARN("Texture dump: could not write {}", path.u8string());
  }
#endif
}

}  // namespace rex::graphics::texture_replacement
