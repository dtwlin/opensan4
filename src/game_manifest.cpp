#include "game_manifest.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>

#include "grp600c_defs.h"
#include "util/string_util.h"
#include "util/win32_platform.h"

namespace san4 {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

constexpr std::uint32_t rotr32(std::uint32_t x, unsigned n) {
  return (x >> n) | (x << (32u - n));
}

void sha256_transform(std::array<std::uint32_t, 8>& state,
                      const std::uint8_t block[64]) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    std::uint32_t s0 =
        rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    std::uint32_t s1 =
        rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

  for (int i = 0; i < 64; ++i) {
    std::uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    std::uint32_t ch = (e & f) ^ ((~e) & g);
    std::uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
    std::uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    std::uint32_t temp2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

constexpr ManifestEntry kManifest[] = {
    {"EDMUSIC.S4", 8706, "8d5df99f4984564fc129c09e5d32f92057cc43662c06a2bb30f888aa6a050ec2"},
    {"END.EXE", 65787, "faff2e045885b96f08bfaf6ae97f2143b5b4040cd58650c1bedb362bfc20f8b1"},
    {"ENDDAT.S4", 1120708, "6e66190af9e434b48a460d8b7ff2124f0b512209e4a1395198a42166af71b323"},
    {"ENDSTAFF.S4", 4002, "759e5d10b5c21bbe48812c22ab7dc0890b530792208b04296c95074a3c42e874"},
    {"FMDRV.COM", 5469, "803becf535bcf025447f60418573628dc4cdf69094ceaea5c16d377b27c7ddf0"},
    {"FONT.S4", 2880, "bf5ab619dae4ad8f58bd77f9031d3b4350c9b190385fcdf7c2b43f81fd281d9d"},
    {"GRAPH.S4", 15391, "be0ca58152a021e688b010f7c14b2f5b3b6a5c737ebdbc49287bf1981a479eb4"},
    {"GRP600C.EXE", 24849, "1f33eef9ddfd8d5437ceed8dd6bda1cf396c3ebdf7b95f590c9558f3acdf591d"},
    {"HEGRP.S4", 20804, "f27b9b26bc380d019a01a572946b1dbdd082bb5b2f5784dc80181b61c2b2d931"},
    {"HEXGRP.S4", 43793, "b9fd020901458d453a150be42b1f6a2c44f568efa296c437b3f7bccdaec5ba90"},
    {"HEXMAP.S4", 64465, "acae6e7f2696ffd2a137644a3f7bd5f76275827b21cd81e32db1269cb13965cb"},
    {"HIBACK.S4", 114563, "f8b0a3aeab8a2f9922fc8ecc771ef585e96a332f64418600d444a53b3b29dbeb"},
    {"HIGRP.S4", 54092, "ae9c46ce3573340a0516598c58e2ab0b28de9b4fed358250aad0cc221a6fc668"},
    {"HKGRP.S4", 34647, "99d479513bc976c99e5da6c7731bfff8ee954fcd4491ed74ff40500508aef2e6"},
    {"HKMAP.S4", 35112, "9b8b7f709c4f0fa2127c51db5cdb9ad8628cc8d290e3baf5024986fed3f3d47e"},
    {"HUNIT.S4", 17741, "17ddce26809816788f62c310748ad756127cb136fb41256de81cb7f80391abd5"},
    {"HWGRP.S4", 15872, "c1e78ca2e6d5edcd608c422ef7be1a719a725f69ff7f35d10325b39369e1e9bd"},
    {"KAODATA.S4", 530413, "fd1c968a058cd293ee62eaee3ea1a2bdf2a381438f36fb228df2e5f28d8c7356"},
    {"LOGO.S4", 9547, "f4681804b94d6670cb796f716f58422f7892d12b843ce7b4ff32fd65c38f838c"},
    {"MAIN.16P", 56980, "53fe6e53ef5c39956d3561d71ce0ce107bf17b14963bc6fab7f44af8cadaf04d"},
    {"MAIN.EXE", 598974, "78c54faa8714dfbd4910a15c07a57da09944ed1baaec261bea8478023f71234e"},
    {"MAINCG.S4", 59905, "786c8cc6b7b7cd1956af79513a12928efe63f32b3b5572d1508aa3e1aee01bf5"},
    {"MAINMAP.S4", 21804, "b60fab29583c38b2092db779c2dfa6c35b11f08fc4afa4a92e19bcb9edd4517e"},
    {"MESSAGE.S4", 56328, "c14869649065cae2a32ab2fea0d5db01240308316dd3bf5b36c68cba8ea566ca"},
    {"MONDATA.S4", 190464, "01df172a3f086e8e6ed8307190c16f1ff3add2190d7d9de5ebd927729856eff8"},
    {"MUSIC.S4", 38824, "e7afa4fc4bda06d698d87e0d17404d0115948114549c9b53c0652680e751c81f"},
    {"NAME.16P", 280028, "cdebf4d4bf6783cf59776ae9570b1c3a349ed2e4fd26a4425562c9f9594def27"},
    {"NBDATA.S4", 17496, "3bd2d1a30ea5f2e9b8ec2627213970ed9988af8df3062df439d25e2952e54369"},
    {"OPED.16P", 21448, "a77e7d308624f3bec2a8f5393b786ba218e6f761ad069ff7e1437a333c304531"},
    {"OPEN.EXE", 53731, "e779dab95a3392de27c839a345223c32a340422fe0a539e900575dc9d40b869c"},
    {"OPENDAT.S4", 530761, "587a54156a6b3443a22d56050fc226bf73aaa0c921329fba1f34aba381b58b3d"},
    {"OPMUSIC.S4", 13635, "80c5f3a08cf80efdd9f1473cd7871a2927e0e870e9a8ddd18d3a030a5764e09b"},
    {"PACKGRP.S4", 342738, "bb5d05b968853cfbe8a9bf1a980070bc327af55919898731c6fe70a00a8ff137"},
    {"PACKGRP2.S4", 70785, "5c886f1ab109ad061977a50c38d8ad34d1883d5c1ef3fc5995acfa2b27102e28"},
    {"PACKGRP3.S4", 119142, "20f7b5e3de94203c634e7a1c6fe4c26be9bc3de0fe07677225d17304efff6daa"},
    {"PREGRP.S4", 107541, "a4993d73b4197d8ab08d0e7b0346d45873e769d8683ca31f57091571ba1490c9"},
    {"PREMSG.S4", 3669, "881acf082a39a939d566e09e389a65a4958089417ced2672258ea6cc36972405"},
    {"REVGRP.S4", 71234, "415b50b64905309eebef0728041bb38f1bfb0e3d90e7ae9d8797dc60ea783a1b"},
    {"SAN486.COM", 1934, "8bb3d79630b02b8c64406a5fd85d3d4d53242a51f19e424f99b61e6a3e323d8d"},
    {"SNDATA.S4", 59930, "423e7cbcec2c2642a51aaeec3772e39d2492ff857d6f57d70386289dfab944c7"},
    {"TAIKI.S4", 36461, "91b83544940437b32a3b0a29fd87303fdbc75bd141ca03cdeec954e2b23ebd73"},
    {"TOSHICG.S4", 31679, "1add0e2347926c57693dc443bb1fad0ea10fb172e79c1bef52ca3cee7a2b113f"},
    {"TOSHIMAP.S4", 9514, "d536cfd56db861325487d35185df3046204df9a9aa1fbbe1d5f52c0cd043b13e"},
};

std::string trim_line(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
    s.pop_back();
  }
  return s;
}

}  // namespace

std::string sha256_hex(std::span<const std::uint8_t> data) {
  std::array<std::uint32_t, 8> state = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
  };

  std::size_t offset = 0;
  while (offset + 64 <= data.size()) {
    sha256_transform(state, data.data() + offset);
    offset += 64;
  }

  std::uint8_t tail[128] = {};
  std::size_t rem = data.size() - offset;
  if (rem > 0) {
    std::copy_n(data.data() + offset, rem, tail);
  }
  tail[rem] = 0x80u;
  std::size_t pad_blocks = (rem < 56) ? 1 : 2;
  std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8u;
  for (int i = 0; i < 8; ++i) {
    tail[pad_blocks * 64 - 1 - i] =
        static_cast<std::uint8_t>((bit_len >> (i * 8)) & 0xFFu);
  }
  for (std::size_t b = 0; b < pad_blocks; ++b) {
    sha256_transform(state, tail + b * 64);
  }

  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (std::uint32_t word : state) {
    oss << std::setw(8) << word;
  }
  return oss.str();
}

std::span<const ManifestEntry> immutable_game_manifest() {
  return kManifest;
}

ManifestCheckResult resolve_and_verify_game_dir(
    const std::string& cli_game_dir,
    const std::filesystem::path& exe_path) {
  ManifestCheckResult res;
  std::vector<std::filesystem::path> candidates;

  if (!cli_game_dir.empty()) {
    candidates.push_back(util::utf8_to_path(cli_game_dir));
  }
  if (auto env_dir = win32_platform::get_env_path("SAN4_GAME_DIR");
      env_dir && !env_dir->empty()) {
    candidates.push_back(std::move(*env_dir));
  }

  const std::filesystem::path effective_exe_path =
      !exe_path.empty() ? exe_path : win32_platform::get_executable_path("");
  if (!effective_exe_path.empty()) {
    std::error_code ec;
    const auto exe_dir =
        std::filesystem::weakly_canonical(effective_exe_path, ec).parent_path();
    if (!exe_dir.empty()) {
      const auto hint_file = exe_dir / "san4_game_dir.txt";
      if (std::filesystem::exists(hint_file, ec)) {
        std::ifstream ifs(hint_file);
        std::string line;
        if (std::getline(ifs, line)) {
          line = trim_line(line);
          if (!line.empty()) {
            candidates.push_back(util::utf8_to_path(line));
          }
        }
      }
      candidates.push_back(exe_dir / "SAN4");
      candidates.push_back(exe_dir / "game" / "SAN4");
      const auto parent_dir = exe_dir.parent_path();
      if (!parent_dir.empty() && parent_dir != exe_dir) {
        candidates.push_back(parent_dir / "SAN4");
        candidates.push_back(parent_dir / "game" / "SAN4");
      }
      if (std::filesystem::exists(exe_dir / "SAN486.COM", ec) && !ec) {
        candidates.push_back(exe_dir);
      }
      if (!parent_dir.empty() && parent_dir != exe_dir &&
          std::filesystem::exists(parent_dir / "SAN486.COM", ec) && !ec) {
        candidates.push_back(parent_dir);
      }
    }
  }
#ifdef _WIN32
  if (auto local_appdata = win32_platform::get_env_path("LOCALAPPDATA");
      local_appdata && !local_appdata->empty()) {
    candidates.push_back(*local_appdata / "san4" / "game");
  }
  if (auto appdata = win32_platform::get_env_path("APPDATA");
      appdata && !appdata->empty()) {
    candidates.push_back(*appdata / "san4" / "game");
  }
#else
  if (auto xdg = win32_platform::get_env_path("XDG_DATA_HOME");
      xdg && !xdg->empty()) {
    candidates.push_back(*xdg / "san4" / "game");
  } else if (auto home = win32_platform::get_env_path("HOME");
             home && !home->empty()) {
    candidates.push_back(*home / ".local" / "share" / "san4" / "game");
  }
#endif
  candidates.emplace_back("SAN4");
  candidates.emplace_back("game/SAN4");
  candidates.emplace_back("../game/SAN4");
  {
    std::error_code cwd_ec;
    if (std::filesystem::exists(std::filesystem::path(".") / "SAN486.COM",
                                cwd_ec) &&
        !cwd_ec) {
      candidates.emplace_back(".");
    }
  }

  std::error_code ec;
  std::filesystem::path first_existing_dir;
  for (const auto& cand : candidates) {
    if (std::filesystem::is_directory(cand, ec) && !ec) {
      const auto canon = std::filesystem::weakly_canonical(cand, ec);
      if (first_existing_dir.empty()) {
        first_existing_dir = canon;
      }
      if (!cli_game_dir.empty()) {
        res.resolved_dir = canon;
        break;
      }
      for (const auto& entry : std::filesystem::directory_iterator(canon, ec)) {
        if (entry.is_regular_file(ec)) {
          const std::string uname =
              util::ascii_upper(util::path_to_utf8(entry.path().filename()));
          if (uname == "SAN486.COM" || uname == "MAIN.EXE" ||
              uname == "GRP600C.EXE") {
            res.resolved_dir = canon;
            break;
          }
        }
      }
      if (!res.resolved_dir.empty()) {
        break;
      }
    }
  }
  if (res.resolved_dir.empty() && !first_existing_dir.empty()) {
    res.resolved_dir = std::move(first_existing_dir);
  }

  if (res.resolved_dir.empty()) {
    res.errors.push_back(
        "No SAN4 game directory found. Pass --game-dir /path/to/SAN4 or set SAN4_GAME_DIR.");
    return res;
  }

  std::unordered_map<std::string, std::filesystem::path> upper_to_path;
  for (const auto& entry : std::filesystem::directory_iterator(res.resolved_dir, ec)) {
    if (entry.is_regular_file(ec)) {
      upper_to_path[util::ascii_upper(
          util::path_to_utf8(entry.path().filename()))] = entry.path();
    }
  }

  for (const auto& exp : kManifest) {
    auto it = upper_to_path.find(exp.filename);
    if (it == upper_to_path.end()) {
      res.errors.push_back(std::string("Missing required game file: ") + exp.filename);
      continue;
    }
    auto fsize = std::filesystem::file_size(it->second, ec);
    if (ec || fsize != exp.size) {
      res.errors.push_back(std::string("Size mismatch on ") + exp.filename);
      continue;
    }
    // NBDATA.S4 (17,496 bytes) stores user-created custom officers and is
    // updated in-place by MAIN.EXE while keeping its exact fixed byte size.
    if (std::string_view(exp.filename) == "NBDATA.S4") {
      continue;
    }
    std::ifstream ifs(it->second, std::ios::binary);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(fsize));
    if (!ifs.read(reinterpret_cast<char*>(buf.data()),
                  static_cast<std::streamsize>(buf.size()))) {
      res.errors.push_back(std::string("Failed to read ") + exp.filename);
      continue;
    }
    if (sha256_hex(buf) != exp.sha256_hex) {
      res.errors.push_back(std::string("SHA-256 mismatch on ") + exp.filename);
    }
  }

  res.ok = res.errors.empty();
  return res;
}

bool has_expected_grp600c_dispatch_signature(
    std::span<const std::uint8_t> grp_image) {
  if (grp_image.size() < static_cast<std::size_t>(kGrpEntryOff) + 5u) {
    return false;
  }
  return grp_image[kGrpEntryOff + 0u] == 0xE9u &&
         grp_image[kGrpEntryOff + 1u] == 0x14u &&
         grp_image[kGrpEntryOff + 2u] == 0x23u &&
         grp_image[kGrpEntryOff + 3u] == 0x90u &&
         grp_image[kGrpEntryOff + 4u] == 0x90u;
}

}  // namespace san4
