#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace san4 {

// GRP600C.EXE (PC-98 -> VESA 800x600x16 planar graphics driver) function codes (AH)
// and internal CS-relative state variable offsets verified from GRP600C.EXE.
inline constexpr uint8_t kGrpInit = 0x00;
inline constexpr uint8_t kGrpReset = 0x01;
inline constexpr uint8_t kGrpClearScreen = 0x04;
inline constexpr uint8_t kGrpSetPlaneDisable = 0x05;
inline constexpr uint8_t kGrpGetPlaneDisable = 0x06;
inline constexpr uint8_t kGrpSetActivePlanes = 0x07;
inline constexpr uint8_t kGrpSetPlanarRop = 0x08;
inline constexpr uint8_t kGrpSetTilePattern = 0x09;
inline constexpr uint8_t kGrpDrawPixel = 0x0A;
inline constexpr uint8_t kGrpReadPixel = 0x0B;
inline constexpr uint8_t kGrpDrawLine = 0x0C;
// BX,CX,SI,DI = x0,y0,x1,y1 (any corner order). A zero-width or zero-height
// box draws a line. Otherwise AL&3: 0 = outline, 2 = tile-pattern fill
// (CS:0x00AB, planar ROP), 1/3 = solid fill. Clipped to the AH=0x1D rect.
inline constexpr uint8_t kGrpDrawBox = 0x0D;
// BX,CX = seed point, DL = game color.
inline constexpr uint8_t kGrpFloodFill = 0x0E;
[[deprecated("use kGrpDrawBox")]] inline constexpr uint8_t kGrpDrawBoxOutline =
    kGrpDrawBox;
[[deprecated("0x0E is FloodFill; box fills are kGrpDrawBox")]]
inline constexpr uint8_t kGrpFillBox = kGrpFloodFill;
inline constexpr uint8_t kGrpBlitPlanar = 0x0F;
inline constexpr uint8_t kGrpReadPlanar = 0x10;
inline constexpr uint8_t kGrpGetGlyph16 = 0x11;
inline constexpr uint8_t kGrpGetGlyph8 = 0x12;
inline constexpr uint8_t kGrpBlitMono = 0x13;
inline constexpr uint8_t kGrpSetPaletteSingle = 0x14;
inline constexpr uint8_t kGrpSetPaletteAll = 0x15;
inline constexpr uint8_t kGrpSmoothFade = 0x16;
inline constexpr uint8_t kGrpCopyRect = 0x17;
inline constexpr uint8_t kGrpSwapPlanarRect = 0x18;
inline constexpr uint8_t kGrpReadRect = 0x18;
inline constexpr uint8_t kGrpBlitNpk = 0x19;
inline constexpr uint8_t kGrpSetMonoRop = 0x1B;
inline constexpr uint8_t kGrpSetGaiji = 0x1C;
inline constexpr uint8_t kGrpSetClipRect = 0x1D;
inline constexpr uint8_t kGrpKbhit = 0x1E;             // INT 16h AH=11h -> AX=0/1
inline constexpr uint8_t kGrpFilterKeyChar = 0x1F;     // BX=key -> AX (E0h/F0h prefixes dropped)
inline constexpr uint8_t kGrpGetScreenDims = 0x20;     // -> AX=800, BX=600
inline constexpr uint8_t kGrpGetPalette = 0x21;        // BX=first, CX=count -> ES:DI (CS:0x198D)
inline constexpr uint8_t kGrpInitKanjiDriver = 0x22;   // open + IOCTL the DOS/V font device
inline constexpr uint8_t kGrpCallKanjiDriver = 0x23;   // far call into the font device entry
inline constexpr uint8_t kGrpGetKeyBlocking = 0x24;    // INT 16h AH=10h, BX = AH=12h shift flags
inline constexpr uint8_t kGrpSetTextColors = 0x25;     // AL = bg<<4|fg -> AH=0x1B with AL=0

// GRP600C.EXE internal CS-relative state offsets (CS = GRP600C TSR code segment):
inline constexpr uint16_t kGrpEntryOff = 0x0018;              // CS:0x0018 far entry point
// AH=0x08 planar / primitive state:
inline constexpr uint16_t kGrpCsPlanarRopOffset = 0x0094;     // u16_le: 0=PSET, 1=OR, 2=AND, 3=XOR, 4=color-0 transparent, 5=NOT
inline constexpr uint16_t kGrpCsPlanarFgSlotOffset = 0x0096;  // u16_le: slot_map[BL]
inline constexpr uint16_t kGrpCsPlanarBgSlotOffset = 0x0098;  // u16_le: slot_map[BH]

// AH=0x1B 1-bit monochrome blit state:
inline constexpr uint16_t kGrpCsMonoRopOffset = 0x009A;       // u16_le: 0=opaque fg/bg, 4=transparent 0-bits
inline constexpr uint16_t kGrpCsMonoFgSlotOffset = 0x009C;    // u16_le: slot_map[BL]
inline constexpr uint16_t kGrpCsMonoBgSlotOffset = 0x009E;    // u16_le: slot_map[BH]

// AH=0x05 / AH=0x07 plane masks & active plane count:
inline constexpr uint16_t kGrpCsPlaneDisableOffset = 0x00A0;  // u8: VGA-plane write-disable mask
inline constexpr uint16_t kGrpCsEffectiveDisableOffset = 0x00A1;
inline constexpr uint16_t kGrpCsActivePlanesOffset = 0x00A2;  // u16_le: 3 or 4 (init 4)

// The tile-pattern and mono-background offsets below, like several function
// codes above (e.g. kGrpKbhit), are not referenced by the runtime: they
// document the driver's binary layout for the planned GRP600C HLE work.
// AH=0x09 tile pattern (BX = byte width <= 1, CX = rows <= 16, DS:SI = data)
// used by AH=0x0D (AL&3)==2 fills:
inline constexpr uint16_t kGrpCsTileByteWidthOffset = 0x00A4;  // u16_le: bytes per row per plane
inline constexpr uint16_t kGrpCsTileHeightOffset = 0x00A6;     // u16_le: rows (row = y % tile_h)
inline constexpr uint16_t kGrpCsTileBytesOffset = 0x00A8;      // u16_le: width * rows * active planes
inline constexpr uint16_t kGrpCsTilePatternOffset = 0x00AB;    // u8[64]: rows of one byte per active plane
inline constexpr std::size_t kGrpTilePatternBytes = 64;

// AH=0x1B mono blit background per-plane fill masks:
inline constexpr uint16_t kGrpCsMonoBgPlaneMasksOffset = 0x01B7;  // u8[4]

// AH=0x1D inclusive clipping rectangle (init 0, 0, 799, 599):
inline constexpr uint16_t kGrpCsClipX0Offset = 0x1940;        // i16_le
inline constexpr uint16_t kGrpCsClipY0Offset = 0x1942;        // i16_le
inline constexpr uint16_t kGrpCsClipX1Offset = 0x1944;        // i16_le
inline constexpr uint16_t kGrpCsClipY1Offset = 0x1946;        // i16_le

// AH=0x0D corner-normalized, not yet clipped box (x0 <= x1, y0 <= y1):
inline constexpr uint16_t kGrpCsBoxX0Offset = 0x1948;         // i16_le
inline constexpr uint16_t kGrpCsBoxY0Offset = 0x194A;         // i16_le
inline constexpr uint16_t kGrpCsBoxX1Offset = 0x194C;         // i16_le
inline constexpr uint16_t kGrpCsBoxY1Offset = 0x194E;         // i16_le

// GRP600C CS:0x2B35 smooth palette fade & cache offsets:
inline constexpr uint16_t kGrpFadePaletteTableOffset = 0x0187;
inline constexpr uint16_t kGrpPalCacheAOff = 0x198D;
inline constexpr uint16_t kGrpPalCacheBOff = 0x19CF;
inline constexpr uint16_t kGrpSlotMapOffset = 0x24E8;
inline constexpr uint16_t kGrpSlotMapOff = 0x24E8;
inline constexpr uint16_t kGrpMinSegSpan = 0x24F8;
inline constexpr uint8_t kGrpMaxPaletteFadeSteps = 16;

// GRP600C CS:0x24E8 game-color -> VGA-plane-slot permutation (Plane 1 <-> Plane 2 swap):
inline constexpr std::array<uint8_t, 16> kGrpSlotMap = {
    0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15,
};

// Inverse permutation (VGA-plane-slot -> game-color, self-inverse):
inline constexpr std::array<uint8_t, 16> kGrpInverseSlotMap = {
    0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15,
};

}  // namespace san4
