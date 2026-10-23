#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include "xiaole_sprites.h"

// Avatar sprite sets: the palette frames, lip-sync patches and layout the expression
// engine (xiaole_frame.h) draws from. The builtin set is the compiled-in rabbit
// (xiaole_sprites.h, exported by assets/xiaole/export_firmware.py); an avatar pack is
// the same data as a blob in the assets partition (assets/xiaole/export_pack.py), named
// by index.json's "avatar_pack". No LVGL or ESP-IDF dependency: the firmware, the host
// simulator and the host test all parse packs with this code.
//
// Pack format "XLAV" v1, little-endian, read byte by byte (asset payloads in the mmap
// start at odd offsets, so nothing here dereferences a wider type):
//   0  char[4]  "XLAV"
//   4  u16      version = 1
//   6  u16      header_size = 64 (offset of the palette)
//   8  u8       frame_w = 64, frame_h = 64, mood_count = 21, palette_count (1..255)
//  12  u8       left_eye_x, right_eye_x, eye_top, eye_rows, brow_row
//  17  u8       mouth_x, mouth_y, mouth_w, mouth_h, mouth_levels = 2
//  22  u8       ear_rows, content_top, content_bottom
//  25  u8       face_index, ink_index, highlight_index   (palette entries the engine paints eyes with)
//  28  u8       pupil_style (0 small, 1 normal, 2 big), u8[3] reserved
//  32  char[16] id (hex), 48 char[16] name (UTF-8, NUL padded)
//  64  u32[palette_count] ARGB8888
//      u8[mood_count][64*64] frames in kMoods order
//      u8[mouth_levels][mouth_w*mouth_h] mouth patches (index 0 keeps the face pixel)
namespace xiaole {

inline constexpr int kFrameSize = 64;  // Palette frames are 64x64.
inline constexpr int kMoodCount = static_cast<int>(std::size(kFrames));
inline constexpr int kMouthLevels = 2;
inline constexpr int kAvatarPackHeaderSize = 64;
inline constexpr int kAvatarPackVersion = 1;
inline constexpr int kAvatarPaletteMax = 256;
inline constexpr int kAvatarMouthMax = 16 * 16;
static_assert(kMoodCount == static_cast<int>(std::size(kMoods)), "mood table mismatch");

// Where the expression engine draws: eye boxes, brow rows, mouth box, ear rows.
struct AvatarLayout {
    int left_eye_x = kLeftEyeX;
    int right_eye_x = kRightEyeX;
    int eye_top = kEyeTop;
    int eye_rows = kEyeRows;
    int brow_row = kBrowRow;
    int mouth_x = kMouthX;
    int mouth_y = kMouthY;
    int mouth_w = kMouthW;
    int mouth_h = kMouthH;
    int ear_rows = kEarRows;
    int content_top = kContentTop;
    int content_bottom = kContentBottom;
    int pupil_style = kBuiltinPupilStyle;  // shifts Frame::pupil_size: 0 small, 1 as is, 2 big
};

struct SpriteSet {
    AvatarLayout layout;
    uint32_t palette[kAvatarPaletteMax] = {};
    int palette_count = 0;
    const uint8_t* frames[kMoodCount] = {};
    const uint8_t* mouth[kMouthLevels] = {};
    uint32_t face = 0, ink = 0, highlight = 0;  // procedural eyes: erase, pupil/brow, highlight
    char id[17] = {};
    char name[33] = {};
    bool builtin = true;

    const uint8_t* frame(int mood) const { return frames[mood < 0 || mood >= kMoodCount ? 0 : mood]; }
};

// Bounded copy with a terminating NUL (strncpy's truncation warnings aside).
inline void CopyText(char* dst, size_t dst_size, const char* src, size_t src_max) {
    size_t n = 0;
    while (n < src_max && n + 1 < dst_size && src[n] != '\0')
        ++n;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

inline const SpriteSet& BuiltinSprites() {
    static const SpriteSet set = [] {
        SpriteSet s;
        s.palette_count = static_cast<int>(std::size(kPalette));
        for (int i = 0; i < s.palette_count; ++i)
            s.palette[i] = kPalette[i];
        for (int i = 0; i < kMoodCount; ++i)
            s.frames[i] = kFrames[i];
        for (int i = 0; i < kMouthLevels; ++i)
            s.mouth[i] = kMouthOverlays[i];
        s.face = kPalette[kBuiltinFaceIndex];
        s.ink = kPalette[kBuiltinInkIndex];
        s.highlight = kPalette[kBuiltinHighlightIndex];
        CopyText(s.id, sizeof(s.id), kBuiltinAvatarId, sizeof(kBuiltinAvatarId));
        CopyText(s.name, sizeof(s.name), kBuiltinAvatarName, sizeof(kBuiltinAvatarName));
        s.builtin = true;
        return s;
    }();
    return set;
}

inline uint16_t ReadU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t ReadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Parses a pack in place: `out` points into `data`, which must stay mapped while the
// set is in use. Returns false with `*error` set (static text) when the pack is not
// usable; `out` is untouched then.
inline bool LoadAvatarPack(const void* data, size_t size, SpriteSet& out, const char** error) {
    const char* why = nullptr;
    auto fail = [&](const char* text) {
        why = text;
        return false;
    };
    const auto* p = static_cast<const uint8_t*>(data);
    SpriteSet s;
    if (!p || size < static_cast<size_t>(kAvatarPackHeaderSize))
        fail("pack too short");
    else if (std::memcmp(p, "XLAV", 4) != 0)
        fail("not an XLAV pack");
    else if (ReadU16(p + 4) != kAvatarPackVersion)
        fail("unsupported pack version");
    else if (ReadU16(p + 6) != kAvatarPackHeaderSize)
        fail("unexpected header size");
    else if (p[8] != kFrameSize || p[9] != kFrameSize)
        fail("frames are not 64x64");
    else if (p[10] != kMoodCount)
        fail("mood count is not 21");
    else if (p[11] == 0)
        fail("empty palette");
    if (why) {
        if (error) *error = why;
        return false;
    }
    s.palette_count = p[11];
    AvatarLayout& L = s.layout;
    L.left_eye_x = p[12];
    L.right_eye_x = p[13];
    L.eye_top = p[14];
    L.eye_rows = p[15];
    L.brow_row = p[16];
    L.mouth_x = p[17];
    L.mouth_y = p[18];
    L.mouth_w = p[19];
    L.mouth_h = p[20];
    const int mouth_levels = p[21];
    L.ear_rows = p[22];
    L.content_top = p[23];
    L.content_bottom = p[24];
    const int face = p[25], ink = p[26], highlight = p[27];
    L.pupil_style = p[28];
    const size_t mouth_bytes = static_cast<size_t>(L.mouth_w) * L.mouth_h;
    const size_t need = static_cast<size_t>(kAvatarPackHeaderSize) + 4u * s.palette_count +
                        static_cast<size_t>(kMoodCount) * kFrameSize * kFrameSize + mouth_levels * mouth_bytes;
    if (size < need)
        fail("pack truncated");
    else if (mouth_levels != kMouthLevels || mouth_bytes == 0 || mouth_bytes > static_cast<size_t>(kAvatarMouthMax))
        fail("bad mouth patches");
    else if (L.eye_rows < 4 || L.eye_rows > 12 || L.brow_row < 1 || L.brow_row >= L.eye_top ||
             L.eye_top + L.eye_rows > kFrameSize || L.left_eye_x < 1 || L.right_eye_x < 1 ||
             L.left_eye_x + 7 > kFrameSize || L.right_eye_x + 7 > kFrameSize || L.left_eye_x + 6 > L.right_eye_x)
        fail("eye layout out of range");
    else if (L.mouth_w == 0 || L.mouth_h == 0 || L.mouth_x + L.mouth_w > kFrameSize || L.mouth_y + L.mouth_h > kFrameSize)
        fail("mouth layout out of range");
    else if (L.ear_rows > kFrameSize || L.content_top < kContentTop || L.content_bottom > kContentBottom ||
             L.content_top > L.content_bottom)
        fail("content rows out of range");  // kContentTop/Bottom are what the screen layout was checked against
    else if (face >= s.palette_count || ink >= s.palette_count || highlight >= s.palette_count)
        fail("eye colour index out of range");
    else if (L.pupil_style > 2)
        fail("bad pupil style");
    if (why) {
        if (error) *error = why;
        return false;
    }
    const uint8_t* cursor = p + kAvatarPackHeaderSize;
    for (int i = 0; i < s.palette_count; ++i, cursor += 4)
        s.palette[i] = ReadU32(cursor);
    for (int i = 0; i < kMoodCount; ++i, cursor += kFrameSize * kFrameSize)
        s.frames[i] = cursor;
    for (int i = 0; i < kMouthLevels; ++i, cursor += mouth_bytes)
        s.mouth[i] = cursor;
    // Every index must name a palette entry: a stray byte would read past the palette.
    for (int i = 0; i < kMoodCount; ++i)
        for (int k = 0; k < kFrameSize * kFrameSize; ++k)
            if (s.frames[i][k] >= s.palette_count) {
                if (error) *error = "frame index out of range";
                return false;
            }
    for (int i = 0; i < kMouthLevels; ++i)
        for (size_t k = 0; k < mouth_bytes; ++k)
            if (s.mouth[i][k] >= s.palette_count) {
                if (error) *error = "mouth index out of range";
                return false;
            }
    s.face = s.palette[face];
    s.ink = s.palette[ink];
    s.highlight = s.palette[highlight];
    CopyText(s.id, sizeof(s.id), reinterpret_cast<const char*>(p + 32), 16);
    CopyText(s.name, sizeof(s.name), reinterpret_cast<const char*>(p + 48), 16);
    s.builtin = false;
    out = s;
    return true;
}

}  // namespace xiaole
