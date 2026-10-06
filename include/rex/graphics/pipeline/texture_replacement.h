/**
 * @file        graphics/pipeline/texture_replacement.h
 * @brief       Texture packs: the game's textures replaced by files, and dumped
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace rex::graphics::texture_replacement {

// A texture is known by the hash of its data in the game's memory (with its
// format and size) - the same every run. dump_textures writes each 2D texture
// the game loads as <dump folder>/<HASH>.png (16 hex digits), showing what the
// game's shaders see; a replacement is a file named by the hash - optionally
// followed by _anything, for the modder's own notes - in a mod's textures
// folder (or <executable folder>/textures):
//   <HASH>.png (or .jpg, .tga, .bmp) - 8-bit RGBA, mipmaps generated;
//   <HASH>.dds - BC1, BC2, BC3, BC4, BC5, BC7 or 8-bit RGBA/BGRA, with its
//                own mipmaps (or none).
// Replacements may be any size (larger for more detail) and are sampled as the
// dumps show, channel for channel (the game's swizzles still apply).

enum class ImageFormat : uint32_t {
  kRGBA8,
  kBC1,
  kBC2,
  kBC3,
  kBC4,
  kBC5,
  kBC7,
};

struct Image {
  struct Mip {
    uint32_t width;
    uint32_t height;
    size_t offset;
    // Bytes per row of texels (or of 4x4 blocks), and the number of those
    // rows.
    uint32_t row_pitch;
    uint32_t row_count;
  };
  ImageFormat format = ImageFormat::kRGBA8;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<Mip> mips;
  std::vector<uint8_t> data;
};

// Whether replacements are used (texture_replacement) and any were found.
bool IsEnabled();

// (Re)scans the replacement folders. Called at startup and by
// RequestReload's consumer.
void Rescan();

// texture_replacement_reload: scan again, and reload the textures.
void RequestReload();
// Whether a reload was requested since the last call (clears it).
bool ConsumeReloadRequest();

// The replacement file for a texture, if any.
std::optional<std::filesystem::path> Find(uint64_t hash);

// Companion maps: <HASH>_normal, _orm, _height, _emissive (any notes go
// before the kind: <HASH>_wall_normal.png), in the order of
// material_shaders::CompanionMap - the material shaders sample them along
// with the texture. The file for one, if any.
constexpr uint32_t kCompanionKindCount = 4;
std::optional<std::filesystem::path> FindCompanion(uint64_t hash, uint32_t kind);

// Reads an image file (with mipmaps). False if it can't be read.
bool LoadImageFile(const std::filesystem::path& path, Image& image_out);

// The hash of a texture's data in guest memory, with what identifies it
// besides.
uint64_t HashGuestTexture(const void* data, size_t size, uint32_t format, uint32_t width,
                          uint32_t height, uint32_t pitch, bool tiled, uint32_t endianness);

// Whether dump_textures is set.
bool IsDumping();
// Whether dump_textures is set and this texture hasn't been written yet.
bool ShouldDump(uint64_t hash);
// Writes a texture's dump (8-bit RGBA, as the game's shaders see it).
void Dump(uint64_t hash, uint32_t width, uint32_t height, const uint8_t* rgba, size_t row_pitch);

}  // namespace rex::graphics::texture_replacement
