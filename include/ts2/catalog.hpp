#pragma once
#include "ts2/win32_abi.hpp"
#include <stdint.h>

// ============================================================================
// Object catalog for the build-mode browser: data comes from ts2catalog.bin / ts2thumbs.bin, produced by
// tools/catalog/build_catalog.py (base game + expansions + custom content "mods").
// Both files live in the game's working directory (next to ts2lang.cfg).
// ============================================================================
namespace ts2::catalog {

struct Item {
    uint32_t    guid;
    uint32_t    group;
    uint16_t    price;
    uint8_t     cat;        // index into the category list (0 is the pseudo category "All")
    uint8_t     flags;      // bit0 = custom content (mod)
    uint32_t    thumb;      // thumbnail index, 0xFFFFFFFF = none
    const char* nameEn;
    const char* namePt;
    const char* src;        // package file of a mod object ("" for the base game)
};

constexpr uint32_t kNoThumb = 0xFFFFFFFFu;
constexpr uint8_t  kFlagMod = 1;

enum Source : int { SRC_ALL = 0, SRC_BASE = 1, SRC_MODS = 2 };

bool Load();                       // loads once; false if the files are missing (see LoadError)
bool Available();
const char* LoadError();

uint32_t Count();
const Item& At(uint32_t i);
uint32_t ModCount();

int  CategoryCount();
const char* CategoryName(int c);   // in the current HUD language
const char* ItemName(const Item& it);

// Matches every word of `text` (accent/case-insensitive, EN + PT names + mod file name) AND the category AND the source.
// Returns the number of matches (written to `out`, at most `max`; pass out=nullptr just to count).
uint32_t Query(const char* text, int cat, int src, uint32_t* out, uint32_t max);

// 64x64 thumbnail as a D3D9 texture (nullptr while it is not loaded yet or when the item has none).
void* Thumb(d3d9::IDirect3DDevice9* dev, uint32_t thumbIdx);
void  ReleaseTextures();

} // namespace ts2::catalog
