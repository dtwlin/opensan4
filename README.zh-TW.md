# OpenSan4 — 《三國志 IV》（DOS 版）原生 Linux 與 Windows 11 執行引擎暨 4x HD 高畫質素材包產生器

**Language / 語言:** [English](README.md) | 繁體中文（臺灣）

**OpenSan4**（`opensan4` — Linux 版為 `san4-linux`、Windows 11 版為 `san4.exe`，以及素材包產生器 `san4-hdpack-gen` / `san4-hdpack-gen.exe`）是一套以無塵室（Clean-Room）方式全新開發、採開放原始碼（`GPL-2.0-or-later`）授權的原生 `x86_64` Linux 與 Windows 11 執行引擎，並內附自備遊戲檔案（BYOG, Bring Your Own Game）的 `4x` HD 高畫質素材包產生器，專為《三國志 IV》（`SAN4`，1994 年 MS-DOS 繁體中文版）打造。本專案無須透過 DOSBox，即可在輕量級 C++20 行程中直接執行原版 16 位元 DOS 遊戲執行檔，同時提供現代化的操控體驗、流暢的 24 位元全彩畫面淡入淡出轉場、多執行緒像素放大演算法，以及涵蓋原版遊戲 100% 圖檔封裝的完整 `4x`（`3200x2400`）32 位元 RGBA 高畫質圖像支援。

## 法律聲明與自備遊戲檔案（BYOG）須知

- **不包含任何專有遊戲資產**：本儲存庫**僅**包含無塵室開發之執行引擎與素材產生器原始碼、建置指令稿及合成單元測試。本儲存庫與預設組態編譯產出之 `san4-linux`、`san4.exe`、`san4-hdpack-gen`、`san4-hdpack-gen.exe` 二進位執行檔中，均未散佈或內嵌任何光榮特庫摩（Koei Co., Ltd. / Koei Tecmo Games Co., Ltd.）之執行檔（`.EXE`、`.COM`）、劇本與資料檔（`.S4`）、字型檔（`.16P`）、音樂、圖像或存檔。（若您在本機自行啟用選用的 `--embed-game-dir`／`-DSAN4_EMBED_GAME_DIR` 模式，將您合法擁有的 `43` 個原版 DOS 檔案封裝成個人單一執行檔，該編譯產出之 `san4` 執行檔因內含您的原版遊戲資產，僅供您個人本機使用——**嚴禁公開散佈或轉發**。）
- **非官方關聯聲明**：*Romance of the Three Kingdoms*（《三國志》）為光榮特庫摩遊戲（Koei Tecmo Games Co., Ltd.）之註冊商標。本專案為獨立開發之相容性與數位保存執行環境，與光榮特庫摩無任何隸屬、合作或背書關係。
- **自備原版遊戲檔案（Bring Your Own Game）**：若要執行預設本機建置、在本機產生 HD 高畫質素材包並啟動遊戲，您必須自行準備合法擁有的《三國志 IV》MS-DOS 繁體中文版 `SAN4` 目錄（共 `43` 個原始檔案，於 CMake 設定階段與執行期啟動時皆會驗證檔案大小與 SHA-256 雜湊值）。

## 核心特色

- **免 DOSBox 原生架構**：
  - 採用 `libx86emu` 16 位元 CPU 指令模擬，搭配零複製（Zero-Copy）`x86emu_set_page()` 之 4 MB LIM EMS 4.0（`INT 67h`）頁框切換機制（`0xD0000..0xDFFFF`），原生支援 Microsoft C 7.0 `MOVE` 覆疊模組（Overlays）。
  - 內建原生 VESA `0x0102`（`800x600` 16 色 4 位元平面）VGA 硬體狀態機（完整實作 `Write Modes 0..3`、`Read Modes 0..1`、`16` 位元 `OUT DX, AX` 連接埠拆解，以及原子化垂直同步／影格鎖存）。
  - 提供不分大小寫的虛擬檔案系統（`INT 21h`），並支援選用的 `--save-dir` 寫入時複製（Copy-on-Write）存檔隔離目錄，確保原版遊戲資料夾維持完整不變。
- **完整 `4x`（`3200x2400`）32 位元 RGBA HD 高畫質素材引擎與 `san4-hdpack-gen`**：
  - **無塵室 BYOG 素材包產生器（`san4-hdpack-gen`）**：直接從您的本機原版遊戲資料夾解碼所有 `SAN4` 圖像格式（`LS11`、`KAODATA.S4`、`MONDATA.S4`、`NPK016`／`NPK16` 與原始位元平面精靈圖表），透過工作執行緒池於約 3 秒內產生完整的 `4x` 32 位元 RGBA HD 高畫質素材包（共 `4,558` 張 PNG 圖檔與二進位索引，涵蓋全部 `13,770` 項遊戲圖像資產），管線結合 Bayer／棋盤格去網點（De-dithering）、多階段 `xBR` 向量化放大、人物眼神高光保留、雙邊濾波陰影柔化，以及非銳化遮罩（Unsharp Mask）明度增強。
  - **極速啟動與嚴格記憶體上限**：啟動時僅需 `< 10 ms` 即可延遲載入 `hd_manifest.tsv` 與二進位索引表，無須預先解碼所有 PNG 圖檔；執行期間依需求串流解碼紋理至容量上限為 `256 MB` 的 LRU 快取中。
  - **`3200x2400` 螢幕空間 HD 畫布與硬體級精確遮擋**：支援無數量上限的同畫面 `16x16` HD 戰略／都市／野戰／攻城圖磚（`MAINCG.S4`、`TOSHICG.S4`、`HEXGRP.S4`、`HKGRP.S4`）、基於來源雜湊且帶透明遮罩的部隊／單挑／特效精靈圖（`HUNIT.S4`、`HIGRP.S4`、`HWGRP.S4`、`HEGRP.S4`）、`MONDATA.S4` 四部件動態組合武將頭像（由 `320` 個部件即時拼合 `8,192` 張臉孔）、`GRAPH.S4` 動態按鈕合成（`1,407` 個按鈕）、`tile_dict.bin` `16x16` 圖格動態合成、`AH=0x10`／`AH=0x18` 彈出視窗背景還原、支援重疊區域的 `AH=0x17` 地圖捲動複製、硬體級精確的 `vram_write8` 對話框遮擋判定，以及銳利平滑的 `4x` 反鋸齒 `.16P` Big5 繁體中文字型渲染（`AH=0x13`）。
  - **動態場景調色盤 CG 與平滑 3D LUT 色彩分級**：當動態調色盤 `NPK016` 插畫（`OPENDAT.S4`、`ENDDAT.S4`、`PREGRP.S4`、`HIBACK.S4`、`HEGRP.S4`）淡入完成時，自動升級為符合即時場景調色盤的高畫質圖層，並透過快取的 `17x17x17` 三線性內插 3D LUT 即時處理四季變化、戰場色調與平滑轉場色彩演繹。
  - **OpenGL 3.3 Core 硬體 GPU 渲染管線與自動 CPU 備援機制**：在具備 OpenGL 3.3+ 硬體顯示卡（Linux 與 Windows 11 上之 NVIDIA、AMD、Intel GPU）的電腦上，`san4-linux`／`san4.exe` 會自動將全部 5 種基礎像素放大演算法（`nearest`、`sharp2x`、`scale2x`、`xbr2x`、`xbr4x`）、`3200x2400` HD 圖層合成以及 `17x17x17` 3D LUT 三線性色彩分級卸載至位元級精確的 GLSL `#version 330 core` 片段著色器執行——執行檔本身零靜態連結任何 OpenGL 函式庫（執行期透過 `SDL_GL_GetProcAddress` 動態載入），且在無硬體 GPU 或偵測到軟體光柵化驅動程式（`llvmpipe`、`softpipe`、`swrast`、`lavapipe`、`SVGA3D`、`GDI Generic`、`Microsoft Basic Render Driver`、`WARP`）時，會自動無縫退回多執行緒 CPU 搭配 `SDL_Renderer` 管線。
- **流暢度、現代化操控與立體聲 OPL2 音效**：
  - 採用無節流 `6 ms` CPU 突發排程與靜止輸入／計時器輪詢偵測（待機 CPU 使用率 `< 2%`）、60–144 Hz 24 位元平滑淡入淡出轉場，以及不改變背景音樂速度的獨立遊戲加速功能（按 `F4` 切換 `1x / 2x / 4x`，或按住 `Tab` 鍵啟動 `16x` 極速快轉）。
  - 具備 `1:1` `800x600` 零延遲作業系統原生滑鼠追蹤與支援高 DPI 縮放的硬體彩色游標、滑鼠滾輪清單捲動、滑鼠中鍵拖曳／`WASD` 地圖平移，以及取樣精確的 `44.1 kHz` 立體聲 `DBOPL` YM3812 FM 音源合成。

## 相依套件

在 Debian / Ubuntu 系統上安裝建置相依套件：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config libsdl2-dev libx86emu-dev libpng-dev
# 選用（建議安裝，以支援 Clang + LTO + PGO + BOLT 發行版最佳化建置）：
sudo apt-get install -y clang lld llvm bolt
```

需求 SDL 版本為 `2.0.12` 以上；若使用早於 `2.0.18` 的 SDL 版本，則無法啟用垂直同步（VSync），影格將一律由宿主端進行時間步調控制。

若您在無 root 權限的機器上建置（且系統已具備 `libpng-dev`），可執行 `./scripts/bootstrap_deps.sh`，將 `libsdl2-dev`、`libx86emu` 以及上游 `dosbox` 的 `dbopl.{h,cpp}` 自動下載並解壓縮至未納入版本控制的本機 `third_party/` 與 `src/opl/` 目錄中。

## 編譯建置

### 標準建置（自備原版遊戲檔案）

將 `-DSAN4_GAME_DIR` 指向您的原版 `SAN4` 目錄。CMake 會驗證全部 `43` 個原始遊戲檔案，並同時建置 `san4-linux` 與 `san4-hdpack-gen`：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSAN4_WARNINGS_AS_ERRORS=ON -DSAN4_GAME_DIR=/path/to/SAN4
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Clang + LTO + 剖析導向最佳化（PGO）+ `llvm-bolt` 建置

`san4-linux` 原生完整支援 Clang 連結時最佳化（`FULL` 與 `ThinLTO`）、兩階段 **LLVM IR 剖析導向最佳化（PGO, Profile-Guided Optimization）**，以及 **`llvm-bolt` 連結後二進位最佳化**（包含 `ext-tsp` 基本區塊佈局、`cdsort` 快取導向函式重排、冷熱程式碼分離與相同程式碼摺疊）。在僅安裝具版本號之 LLVM 套件（例如 `clang-21`、`lld-21`、`llvm-21`）而無未帶版本號符號連結的 Debian/Ubuntu 主機上，`-DSAN4_USE_CLANG=ON` 會自動尋找配對的 `clang`／`clang++`，解析同目錄下的 `llvm-ar`／`llvm-ranlib`、`ld.lld`、`llvm-profdata` 與 `llvm-strip`，並於設定階段執行 bitcode 靜態封存檔與 AVX2 多版本目標函式連結探測。當 Debian/Ubuntu 主機未預先安裝 `llvm-bolt` 時，`scripts/ensure_bolt.sh` 會自動免 root 下載並解壓縮對應版本的 `bolt-<major>` 與 `libbolt-<major>-dev` 套件至 `third_party/bolt-<major>/`：

```bash
# 單一指令完成 Clang Full-LTO + IR PGO + llvm-bolt 建置（使用暫存 --save-dir
# 自動執行決定性 HD 4x、經典 xBR 與合成相容性訓練工作負載）：
./scripts/build_clang_lto.sh --pgo-bolt --clean --test \
  --game-dir /path/to/SAN4 --hd-pack /path/to/hd_pack -DSAN4_WARNINGS_AS_ERRORS=ON

# 標準單階段 Clang Full-LTO 建置（加上 --thin 可改用 ThinLTO）：
./scripts/build_clang_lto.sh --test -DSAN4_WARNINGS_AS_ERRORS=ON -DSAN4_GAME_DIR=/path/to/SAN4

# 等效的直接 CMake 呼叫（亦可使用：cmake --preset clang-lto -DSAN4_GAME_DIR=/path/to/SAN4，
# 或使用 cmake --preset clang-lto-bolt-relocs 以附加 -Wl,--emit-relocs）：
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSAN4_USE_CLANG=ON -DSAN4_ENABLE_LTO=ON -DSAN4_LTO_MODE=FULL -DSAN4_GAME_DIR=/path/to/SAN4
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### 公開 CI／純引擎建置（無須遊戲檔案）

專案貢獻者與 CI 執行環境無須擁有原版遊戲檔案，即可在開啟嚴格編譯警告（`-Wall -Wextra -Wpedantic -Werror`）下編譯 `san4-linux`、`san4-hdpack-gen` 與 `san4_tests`，並在 ASan + UBSan、ThreadSanitizer（TSan）以及 Clang `llvm-cov` 原始碼級別程式碼覆蓋率檢測下執行全部 `75` 組合成相容性、編解碼器、放大演算法、GPU 像素一致性、硬體模擬、並行處理、CLI／宿主環境、LTO／SIMD、安全性與二進位整潔度測試套件：

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -DSAN4_CI_NO_GAME=ON -DSAN4_ENABLE_SANITIZERS=ON -DSAN4_WARNINGS_AS_ERRORS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

# ThreadSanitizer
cmake -S . -B build-tsan -DSAN4_CI_NO_GAME=ON -DSAN4_ENABLE_TSAN=ON -DSAN4_WARNINGS_AS_ERRORS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure

# Clang 原始碼級別程式碼覆蓋率報告（llvm-cov）
./scripts/run_coverage.sh --min-line-pct 90
```

### CMake 建置選項

- `-DSAN4_GAME_DIR=/path/to/SAN4`：使用者自備之 DOS 版 `SAN4` 目錄路徑（除非啟用 `SAN4_CI_NO_GAME=ON` 或已指定 `SAN4_EMBED_GAME_DIR`，否則將於 CMake 設定階段進行驗證）。
- `-DSAN4_EMBED_GAME_DIR=/path/to/SAN4`：選用的個人單一執行檔內嵌模式（預設為空字串 `""`）。建置時會驗證全部 `43` 個原版 DOS `SAN4` 檔案，透過 `zlib` 壓縮輸出至建置目錄 `${CMAKE_BINARY_DIR}/generated/embedded_game_data.cpp`（絕不寫入原始碼目錄），並直接連結為記憶體內虛擬檔案系統（In-Memory VFS）封裝進 `san4-linux`／`san4.exe`，執行時完全無須外部 `SAN4` 資料夾。**使用此選項編譯之執行檔僅限個人使用，嚴禁散佈。**
- `-DSAN4_CI_NO_GAME=ON`：在無本機遊戲檔案的情況下建置引擎、素材包產生器與合成測試套件（預設為 `OFF`）。
- `-DSAN4_USE_CLANG=ON`：當未明確指定 `CMAKE_C_COMPILER`／`CMAKE_CXX_COMPILER` 時，自動探索配對的 `clang` 與 `clang++`（優先搜尋無版本號執行檔，再依序由 `clang-23` 往下搜尋至 `clang-15`；預設為 `OFF`）。
- `-DSAN4_ENABLE_LTO=ON|OFF`：啟用連結時最佳化（Link-Time Optimization）。使用 Clang 且處於非 `Debug`、非 Sanitizer 組態時預設為 `ON`，其餘情況預設為 `OFF`（亦可搭配 GCC 明確設為 `ON` 以啟用 `-flto=auto`，或在 Clang 下明確設為 `OFF`）。若與 Sanitizer 旗標同時開啟，系統會自動發出警告並略過 LTO。
- `-DSAN4_LTO_MODE=FULL|THIN`：當 Clang 啟用 `SAN4_ENABLE_LTO=ON` 時，選擇完整 LTO（`-flto`，預設值）或 ThinLTO（`-flto=thin`）。
- `-DSAN4_PGO_MODE=OFF|GENERATE|USE`：選擇 Clang LLVM IR 剖析導向最佳化階段（預設為 `OFF`；與各類 Sanitizer 選項互斥，且需要 Clang 與 `llvm-profdata`）。可透過 `SAN4_PGO_PROFILE_DIR`（`GENERATE` 階段輸出原始 `.profraw` 之目錄）與 `SAN4_PGO_PROFILE_FILE`（`USE` 階段讀取合併後 `.profdata` 之檔案路徑）進行控制。
- `-DSAN4_BOLT_RELOCS=ON|OFF`：在連結 `san4-linux` 時輸出連結後重定位區段（`-Wl,--emit-relocs`），供 `llvm-bolt` 二進位最佳化使用（預設為 `OFF`）。
- `-DSAN4_ENABLE_AVX2=ON|OFF`：相容性切換開關（預設為 `OFF`）。`simd_blend.cpp` 中的 AVX2 合成器核心在 `x86_64` 架構下永遠會參與編譯，並於執行期自動偵測 CPU 指令集進行分派（`__builtin_cpu_supports("avx2")`）；全域編譯參數刻意不加上 `-mavx2`，以確保產出的執行檔可在基礎 `x86_64` CPU 上順利執行。
- `-DSAN4_WARNINGS_AS_ERRORS=ON`：將所有編譯器警告視為錯誤（`-Werror`）。
- `-DSAN4_ENABLE_SANITIZERS=ON`：為建置目標加入 AddressSanitizer 與 UndefinedBehaviorSanitizer 檢測（與 `SAN4_ENABLE_TSAN` 互斥）。
- `-DSAN4_ENABLE_TSAN=ON`：為建置目標加入 ThreadSanitizer 執行緒競爭檢測（與 `SAN4_ENABLE_SANITIZERS` 互斥）。
- `-DSAN4_ENABLE_COVERAGE=ON`：為建置目標加入 Clang 原始碼級別程式碼覆蓋率檢測（`-fprofile-instr-generate -fcoverage-mapping`；需要 Clang，且與各類 Sanitizer、PGO 及 `SAN4_STATIC` 選項互斥）。
- `-DSAN4_STATIC=ON`：可攜式靜態建置模式，從 `SAN4_STATIC_PREFIX` 將 SDL2、libx86emu、libpng、zlib、`libstdc++` 與 `libgcc` 靜態連結至執行檔中（預設為 `OFF`；與各類 Sanitizer 選項不相容）。詳見[可攜式靜態建置](#可攜式靜態建置)。
- `-DSAN4_STATIC_PREFIX=<DIR>`：由 `scripts/build_static_deps.sh` 建立之靜態相依套件前綴目錄（預設為 `third_party/static`；若執行 `./scripts/build_static.sh --clang-lto` 則為 `third_party/static-clang-lto`）。若該目錄缺少任何標頭檔或靜態封存檔，設定階段會列出缺少的檔案並立即中止。

### 可攜式靜態建置

只需建置一次 `san4-linux` 與 `san4-hdpack-gen`，即可直接複製到任何具備相容 glibc 版本的 `x86_64` Linux 電腦上執行——目標機器完全不需要安裝 SDL2、libx86emu、libpng、zlib 或 C++ 執行期函式庫，也不使用 `RPATH` 或 `LD_LIBRARY_PATH`：

```bash
# Clang + Full-LTO + IR PGO（含 libx86emu.a）+ llvm-bolt 可攜式靜態建置：
./scripts/build_static.sh --clang-lto --pgo-bolt --clean --test \
  --game-dir /path/to/SAN4 --hd-pack /path/to/hd_pack -DSAN4_WARNINGS_AS_ERRORS=ON

# 單階段 Clang + Full-LTO 可攜式靜態建置：
./scripts/build_static.sh --clang-lto --test -DSAN4_GAME_DIR=/path/to/SAN4   # 或加上 -DSAN4_CI_NO_GAME=ON

# 系統預設編譯器（GCC）可攜式靜態建置：
./scripts/build_static.sh --test -DSAN4_GAME_DIR=/path/to/SAN4
# => 產出 build-static/san4-linux、build-static/san4-hdpack-gen、build-static/THIRD_PARTY_LICENSES.txt
```

`scripts/build_static_deps.sh`（會自動執行，免 root 且具冪等性）會將鎖定版本並通過 SHA-256 驗證的上游原始碼套件下載至 `third_party/static/`，並編譯為靜態 PIC 封存檔：包含 SDL `2.30.12`、libx86emu `3.5`、libpng `1.6.59`、zlib `1.3.2`（後三者與預設 Debian 動態建置所連結的版本一致，因此輸出的遊戲畫面截圖與動態建置位元組完全一致）。傳入 `--clang-lto`（或 `--clang-lto --thin-lto`）時，系統會將 LLVM bitcode `.a` 封存檔（透過 `llvm-ar` + `-flto` 建置，並對 `libx86emu` 套用 `-fno-strict-aliasing`）輸出至獨立隔離的 `third_party/static-clang-lto/` 前綴目錄——在共用壓縮檔快取的同時不會覆寫 `third_party/static/` 中供 GCC 使用的 ELF 封存檔——使 `ld.lld` 能跨越 `san4-linux` 與 `libx86emu` 執行全程式連結時最佳化（Whole-Program LTO）。SDL 各後端所需的發行版 `-dev` 標頭套件會透過 `apt-get download` 擷取至暫存 sysroot，僅供編譯期使用。隨後 `scripts/build_static.sh` 會以 `-DSAN4_STATIC=ON` 設定並建置 `build-static/`，最後執行 `scripts/verify_static_binary.sh`：只要 `DT_NEEDED` 出現 glibc 核心函式庫以外的任何動態函式庫，或含有 `RPATH`／`RUNPATH`，驗證即會失敗。靜態建置的 `ctest` 亦包含此項檢查，並額外執行清空環境變數（Empty Environment）啟動測試。

內嵌與外部載入範圍說明：

- **靜態內嵌於執行檔中**：SDL2、libx86emu、libpng、zlib、`libstdc++`、`libgcc`。編譯產出的執行檔僅需宿主系統提供 `libc.so.6`、`libm.so.6` 與 `ld-linux` 動態載入器。
- **執行期由宿主系統動態載入（絕不靜態內嵌）**：顯示器、GPU 與音訊堆疊。SDL 會在執行期依系統環境透過 `dlopen()` 動態載入存在的元件，包括 X11（`libX11` 與 `Xext`、`Xcursor`、`Xi`、`Xfixes`、`Xrandr`、`Xss`）、Wayland（`wayland-client`／`-egl`／`-cursor`、`xkbcommon`、`libdecor`）、KMSDRM（`libdrm`、`gbm`）、透過 GLX／EGL 存取之 OpenGL／GLES（Mesa 或顯示卡原廠驅動程式）、PulseAudio、PipeWire、ALSA、D-Bus 與 udev。由於這些底層堆疊與本機核心驅動程式、視窗合成器及音訊伺服器緊密綁定，必須與目標機器相符，因此無法靜態連結。無頭模式（`--headless-script`）則使用 SDL 內建的 `dummy` 驅動程式，完全不需要上述任何套件。
- **glibc**：採動態連結。在特定主機上建置的執行檔，目標系統至少須具備該建置主機的 glibc 版本（`scripts/verify_static_binary.sh` 會印出精確的最低 `GLIBC_` 符號版本需求）。若需支援較舊的 Linux 發行版，請在舊版發行版的容器或 chroot 環境中執行 `scripts/build_static.sh`（例如使用 `debian:bookworm` 以對應 glibc `2.36`，或 `ubuntu:20.04` 以對應 `2.31`）；建置指令稿僅需 `cmake`、`gcc`／`g++`（或 `clang`／`lld`）、`make`、`curl`、`python3` 與 `apt-get`。
- **為何不採用完全 `-static` 連結**：完全靜態連結 glibc 的執行檔無法安全地 `dlopen()` 宿主系統的 X11、Mesa 或音訊函式庫（這會載入第二份版本不符的 `libc.so.6`）。實測顯示，以 `-static` 連結的 `san4-linux` 雖可通過無頭腳本重播，但在開啟視窗時會立即崩潰，因此刻意不提供此模式。

重新散佈靜態執行檔時，等同於一併散佈其中內嵌的第三方元件：請務必隨附 [LICENSE](LICENSE) 與 `build-static/THIRD_PARTY_LICENSES.txt`（詳見 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)）。所有內嵌元件皆採用寬鬆且與 GPL 相容之授權條款。

### Windows 11 獨立可攜式靜態建置（`san4.exe` 與 `san4-hdpack-gen.exe`）

支援跨平台交叉編譯（或於 MSYS2 MinGW-w64 環境下原生建置）完全獨立的 `x86_64` Windows 11 PE32+ 執行檔（`san4.exe` 與 `san4-hdpack-gen.exe`），**完全不依賴任何非系統內建之外部 DLL**——已將 `SDL2 2.30.12`、`libx86emu 3.5`、`libpng 1.6.59`、`zlib 1.3.2`、`libstdc++`、`libgcc` 與 `libwinpthread` 全數靜態封裝進 `.exe` 執行檔中：

```bash
# 單一指令執行 Clang + LLD Full-LTO Windows 11 靜態交叉建置並封裝可攜式 .zip：
./scripts/build_win64_static.sh --clang-lto --clean --package-zip --test \
  -DSAN4_GAME_DIR=/path/to/SAN4 -DSAN4_WARNINGS_AS_ERRORS=ON

# 或在無本機遊戲檔案下執行純引擎／CI 建置（亦支援 --thin-lto 或 --gcc）：
./scripts/build_win64_static.sh --clang-lto --test -DSAN4_CI_NO_GAME=ON -DSAN4_WARNINGS_AS_ERRORS=ON
# => 產出 build-win64/san4.exe、build-win64/san4-hdpack-gen.exe、build-win64/THIRD_PARTY_LICENSES.txt、
#    build-win64/san4-win64-portable.zip
```

- **免 Root 自動部署 MinGW-w64 工具鏈**：若系統未安裝全域 `/usr/x86_64-w64-mingw32` sysroot，`scripts/ensure_mingw64.sh` 會自動免 root 下載並解壓縮 Debian/Ubuntu 的 `mingw-w64-x86-64-dev`、`gcc-mingw-w64-x86-64-posix` 與 `g++-mingw-w64-x86-64-posix` 套件至未納入版本控制的 `third_party/mingw64/`。
- **Windows 11 子系統、應用程式資訊清單（Manifest）與安全性強化**：
  - `san4.exe` 連結為 `IMAGE_SUBSYSTEM_WINDOWS_GUI` 圖形介面子系統（在「檔案總管」中雙擊執行時不會閃現黑底主控台視窗）；同時若從 `wt.exe`（Windows 終端機）、PowerShell 或 `cmd.exe` 帶入 `--help` 或命令列參數執行，亦會自動掛接回父層主控台輸出訊息。
  - `san4.exe` 與 `san4-hdpack-gen.exe` 皆內嵌 Windows 11 應用程式資訊清單（`src/win32/san4.manifest`），宣告 `PerMonitorV2` 多螢幕高 DPI 感知與 `<activeCodePage>UTF-8</activeCodePage>`，透過寬字元 `CommandLineToArgvW` 將命令列參數解析為 UTF-8，並啟用決定性 PE32+ 安全連結旗標（`HIGH_ENTROPY_VA`、`DYNAMIC_BASE`、`NX_COMPAT`、`--no-insert-timestamp` 以及 `16 MB` 堆疊保留空間）。
  - 在 Windows 11 上預設啟用 SDL2 `direct3d11` 硬體加速渲染器與 `WASAPI` 音訊後端（支援音訊裝置熱插拔自動恢復）、採用完整支援 Unicode 的 `std::filesystem::path` 檔案讀寫以處理繁體中文路徑（例如 `D:\遊戲\三國志IV\SAN4`），並在 `SAN4` 目錄或存檔為唯讀屬性時自動切換至 `%LOCALAPPDATA%\SAN4\saves` 儲存進度（Linux 上則對應至 `$XDG_DATA_HOME/san4/saves` 或 `~/.local/share/san4/saves`）。
  - `scripts/verify_win64_binary.sh`（亦已整合至 `ctest`）會深度解析 PE32+ 標頭、`.rsrc` 資訊清單資源樹、LLVM bitcode 靜態封存檔（確認全部 4 個靜態相依套件與內部函式庫均為 `100%` bitcode）以及 COFF 匯入表；一旦偵測到匯入任何非作業系統內建之 DLL（如 `SDL2.dll`、`libpng16-16.dll`、`zlib1.dll`、`libstdc++-6.dll`、`libgcc_s_seh-1.dll`、`libwinpthread-1.dll`），驗證將立即失敗。
- **Windows 11 可攜式資料夾結構**：將 `san4-win64-portable.zip` 解壓縮並與您的原版 `SAN4\` 資料夾（及選用的 `hd_pack\` 資料夾）並排放置，直接雙擊 `san4.exe` 即可開始遊玩：

  ```text
  san4-win64\
  ├── san4.exe                  (獨立封裝之 Windows 11 GUI 遊戲執行引擎)
  ├── san4-hdpack-gen.exe       (獨立封裝之 Windows 11 命令列 HD 素材包產生器)
  ├── SAN4\                     (使用者自備之 43 個原版 DOS 遊戲檔案；亦可透過 SAN4_GAME_DIR / --game-dir 指定)
  ├── hd_pack\                  (與 san4.exe 並排存在時自動載入；亦可透過 --hd-pack <DIR> 指定)
  ├── LICENSE
  ├── THIRD_PARTY_LICENSES.txt
  ├── README.md
  └── README.zh-TW.md
  ```

### 個人單一執行檔獨立封裝建置（執行時無須外部 `SAN4` 遊戲資料夾）

若您希望將自己合法擁有的原版遊戲檔案直接封裝成單一獨立的 `san4.exe`（Windows 11）或 `san4-linux`（Linux），方便在個人電腦上隨處複製並直接雙擊執行——完全不需要額外附帶 `SAN4` 原版資料夾——可在建置時傳入 `--embed-game-dir /path/to/SAN4`（或 `-DSAN4_EMBED_GAME_DIR=/path/to/SAN4`）：

```bash
# Windows 11 單一 san4.exe 獨立封裝建置（僅需複製 build-win64/san4.exe 即可直接雙擊遊玩）：
./scripts/build_win64_static.sh --clang-lto --clean --package-zip --test \
  --embed-game-dir /path/to/SAN4 -DSAN4_WARNINGS_AS_ERRORS=ON

# Linux 可攜式靜態單一執行檔獨立封裝建置：
./scripts/build_static.sh --clang-lto --clean --test \
  --embed-game-dir /path/to/SAN4 -DSAN4_WARNINGS_AS_ERRORS=ON

# 直接透過 CMake 建置：
cmake -S . -B build-standalone -DCMAKE_BUILD_TYPE=Release \
  -DSAN4_EMBED_GAME_DIR=/path/to/SAN4 -DSAN4_WARNINGS_AS_ERRORS=ON
cmake --build build-standalone -j
```

- **運作機制**：於建置階段，`cmake/generate_embedded_vfs.py` 會嚴格驗證全部 `43` 個官方原版 DOS 檔案（自動排除 `SAVEDAT.S4` 與任何無關檔案），以 `zlib` 高壓縮率封裝（約 `3.6 MB`）產生不含任何主機路徑字串的 `${CMAKE_BINARY_DIR}/generated/embedded_game_data.cpp`，並僅連結至 `san4.exe`／`san4-linux`（`san4-hdpack-gen` 與 `san4_tests` 則維持不內嵌狀態）。
- **遊戲存檔與新武將自動持久化**：啟動時於記憶體中一次性解壓縮並驗證 IEEE CRC-32（耗時 `< 5 ms`）。劇本進度存檔（`SAVEDAT.S4`）與自創登錄新武將資料（`NBDATA.S4`，首次寫入時自動以寫入時複製 Copy-on-Write 建立）會自動儲存至 Windows 的 `%LOCALAPPDATA%\SAN4\saves`（Linux 則為 `$XDG_DATA_HOME/san4/saves` 或 `~/.local/share/san4/saves`），並優先覆寫內嵌之出廠預設值。若在執行檔旁並排放置選用的 `hd_pack` 資料夾（或傳入 `--hd-pack`），仍會自動啟用 `4x` HD 高畫質渲染。
- **僅限個人本機使用聲明**：使用 `--embed-game-dir`／`-DSAN4_EMBED_GAME_DIR` 編譯產出的執行檔已內嵌您的專有原版 DOS 遊戲檔案，僅供您個人本機合法自用，**嚴禁公開散佈或轉發**。

## 產生與使用完整 `4x` HD 高畫質素材包

使用您的原版 `SAN4` 目錄，將完整的 `4x`（`3200x2400`）32 位元 RGBA HD 高畫質素材包輸出至儲存庫以外的資料夾：

```bash
./build/san4-hdpack-gen --game-dir /path/to/SAN4 --out-dir /path/to/hd_pack
```

- `--jobs <1..128>`：手動指定工作執行緒數量（預設自動依硬體邏輯核心數配置）。
- 啟動遊戲時加上 `--hd-pack /path/to/hd_pack`（若未明確指定 `--scale`，將自動選用 `--scale xbr4x` `3200x2400` 模式；若欲停用可傳入 `--no-hd-pack`）。
- 自訂美術素材覆寫（Custom Artist Overrides）：將任何自製的 `img_<W>x<H>_<HASH16>.png`（支援 `1x`、`2x` 或 `4x` 之 32 位元 RGBA 圖檔，可先透過 `--dump-sprites`／`F9` 擷取原始圖塊）直接放入 `/path/to/hd_pack/` 最上層目錄，並在遊戲中按下 `F8` 即可即時重新載入——鬆散圖檔會自動優先覆寫 `hd_manifest.tsv` 中的預設紀錄。

## 執行遊戲與操作熱鍵

```bash
./build/san4-linux --game-dir /path/to/SAN4 --hd-pack /path/to/hd_pack
```

| 輸入方式／熱鍵 | 功能說明 |
| :--- | :--- |
| **滑鼠左鍵／右鍵** | 確認／取消（零延遲作業系統原生滑鼠游標 + 觸控板輕點鎖存支援） |
| **滑鼠滾輪** | 清單向上／向下捲動（`Shift + 滾輪` = `PgUp`／`PgDn` 翻頁） |
| **滑鼠中鍵拖曳／`WASD`** | 平移戰略地圖與戰術戰場地圖 |
| **`F4`** | 循環切換遊戲進行速度（`1x`／`2x`／`4x`；FM 背景音樂維持 `1.0x` 原速） |
| **按住 `Tab`** | 按住期間啟動 `16x` 極速快轉（Turbo） |
| **`F5`** | 循環切換畫面放大演算法（`nearest` → `sharp2x` → `scale2x` → `xbr2x` → `xbr4x`） |
| **`F8`** | 即時重新載入 32 位元 RGBA `--hd-pack` 素材包目錄 |
| **`F9`** | 切換即時匯出 4 位元精靈圖 PNG（`--dump-sprites`） |
| **`F11`／`Alt+Enter`** | 切換無邊框桌面全螢幕／視窗模式 |

### 命令列選項（CLI Options）

所有接受參數值的旗標皆同時支援 `--flag <VALUE>` 與 `--flag=<VALUE>` 兩種寫法：

- `--game-dir <DIR>`：指定您的 DOS 版 `SAN4` 遊戲目錄路徑（亦可透過環境變數 `SAN4_GAME_DIR` 設定）。
- `--save-dir <DIR>`：將 `SAVEDAT.S4` 等存檔寫入獨立目錄，使 `--game-dir` 維持唯讀不變。
- `--hd-pack <DIR>`／`--no-hd-pack`：啟用（或明確停用）32 位元 RGBA HD 高畫質素材包渲染。當啟用 `--hd-pack` 且未指定 `--scale` 時，預設採用 `xbr4x`（`3200x2400`）；但若偵測到處於軟體渲染器（CPU 光柵化）且可繪製區域高度不超過 `1440` 像素時，則預設採用 `xbr2x`（`1600x1200`，透過 `4x`→`2x` 超取樣保留 HD 細節）。在您手動按下 `F5` 切換模式前，調整視窗大小時皆會自動重新評估此項預設值。
- `--scale <nearest|sharp2x|scale2x|xbr2x|xbr4x>`：選擇初始像素放大演算法（未啟用 HD 素材包時預設為 `sharp2x`；啟用 HD 素材包時之預設行為請參閱 `--hd-pack`）。明確傳入的 `--scale` 永遠優先套用。
- `--renderer <auto|gpu|cpu>`：渲染後端與畫面呈現設定檔。`auto`（預設值）在 Linux 與 Windows 11 上會自動探測 OpenGL 3.3 Core 環境（`GL_MAX_TEXTURE_SIZE >= 4096`），當確認為硬體 GPU 時即啟用雙階段 GLSL GPU 像素放大暨 HD／3D LUT 合成後端（`backend=opengl33`）；若系統不支援 OpenGL 3.3 或偵測到軟體光柵化驅動程式（`llvmpipe`、`softpipe`、`swrast`、`lavapipe`、`SVGA3D`、`GDI Generic`、`Microsoft Basic Render Driver`、`WARP`），則自動退回多執行緒 CPU 放大／合成器搭配 `SDL_Renderer`（`backend=sdl`，Windows 11 優先選用 `direct3d11`，Linux 優先選用 `opengles2`）。`gpu` 可強制請求 OpenGL 3.3 Core 後端（若不支援 OpenGL 3.3 則退回 `SDL_Renderer` 硬體設定檔）；`cpu` 則略過 OpenGL 探測並強制使用 CPU + `SDL_Renderer` 軟體設定檔。僅在硬體設定檔且螢幕更新率至少達 `55 Hz` 時才會啟用垂直同步（VSync）；其餘情況一律以 `60 Hz` 時間期限進行影格步調控制。啟動時會輸出一行診斷紀錄：`[san4-render] backend=<opengl33|sdl> driver=<name> gl_renderer=<str> profile=<hw|sw> vsync=<0|1> scale=<mode> simd=<avx2|scalar>`。無頭模式則維持使用單純的 SDL 渲染器。
- `--window-scale <1..8>`：選擇初始 `4:3` 視窗縮放倍率（預設為 `2`，對應 `1600x1200`）。
- `--speed <1|2|4>`：選擇初始遊戲進行速度倍率。
- `--fullscreen`／`--windowed`：啟動時進入無邊框桌面全螢幕模式或視窗模式（預設為 `--windowed`）。
- `--skip-intro`：略過 `OPEN.EXE` 片頭動畫，直接進入 `MAIN.EXE` 標題畫面。
- `--dump-sprites <DIR>`：將繪製過的唯一精靈圖塊匯出為 PNG 圖檔，方便製作自訂美術覆寫素材。
- `--no-mouse-warp`：停用當遊戲呼叫 `INT 33h AX=0004h` 時自動吸附作業系統滑鼠游標的行為。
- `--profile-frames <TSV>`：輸出逐影格效能計時檔案（欄位包含 `frame, guest_instrs, run_frame_ms, budget_capped, yield_reason, upscale_ms, composite_ms, upload_ms, present_ms, dirty_tiles, dirty_px`），並於結束時印出 `GRP600C`（`INT F1h`）繪圖中斷呼叫統計。互動模式下，主執行緒每從模擬執行緒取得一個影格便寫入一列（未取得的影格不予計入；若該影格無須上傳紋理或重繪，則不會執行呈現且 `present_ms = 0`）。無頭模式下，每模擬一個影格皆會執行呈現並寫入一列。
- `--threads <0..64>`：設定共用的工作執行緒池大小，供畫面放大、HD 圖層合成，以及（僅限互動模式）動態調色盤 CG 之背景重新放大使用。`0`（預設值）會自動選取 `clamp(硬體執行緒數, 4, 16)`。無頭模式同樣會使用該執行緒池進行放大與合成；其輸出與單執行緒執行結果位元完全一致（Bit-Identical），因此螢幕截圖不會因執行緒數量不同而產生任何差異。
- `--deterministic`（須搭配 `--headless-script`）：使無頭模式重播具備完全位元一致性。每個影格的客體 CPU 突發執行改以指令數上限（`budget_sec × 13.2M`，對應無頭模式 `15 ms` 預算為 `198000` 條指令）取代實體時間計時，且 DOS `INT 21h AH=2Ch` 系統時間改由客體 BIOS 滴答計數推導，而非讀取宿主系統時鐘。如此一來，同一份腳本執行兩次將產出位元組完全一致的螢幕截圖，讓修改前後的像素比對達到絕對精確。預設為關閉；未開啟時，無頭模式的時間推進（以及截圖中的動畫相位）會隨宿主電腦速度而異。
- `--headless-script <S>`：依序執行以 `;` 分隔的自動化指令並在完成後結束程式（腳本結尾請加上 `quit`）。無頭模式預設強制使用 SDL 的 `dummy` 視訊驅動程式，因此即使系統具備顯示環境也不會彈出視窗（若需覆寫可設定 `SDL_VIDEODRIVER` 環境變數），且僅接受來自腳本的輸入（忽略宿主滑鼠、鍵盤與關閉視窗事件），並將 `SIGINT`／`SIGTERM` 維持為系統預設終止行為。

## 授權條款（License）

OpenSan4（`san4-linux`／`san4.exe` 與 `san4-hdpack-gen`／`san4-hdpack-gen.exe`）採用 **GNU 通用公共授權條款第 2.0 版或更新版本（`GPL-2.0-or-later`）** 釋出；完整條款請參閱 [LICENSE](LICENSE)，第三方元件授權資訊請參閱 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)（包含源自 DOSBox 之 `DBOPL` `GPL-2.0-or-later`、`libx86emu` `HPND-sell-variant`、`SDL2` `Zlib`、`libpng` `libpng-2.0`、`zlib` `Zlib`）。
