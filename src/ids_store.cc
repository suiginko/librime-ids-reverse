//
// ids_store.cc — IDS4 单文件数据加载与寻址（mmap 惰性映射）。
//
#include "ids_store.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ids_reverse {

const rime::hash_map<std::string, std::string>& CanonMap() {
  static const rime::hash_map<std::string, std::string> kMap = {
      {"氵", "⺡"}, {"水", "⺡"}, {"⺡", "⺡"}, {"氺", "⺡"}, {"灬", "⺣"},
      {"火", "⺣"}, {"⺣", "⺣"}, {"亻", "⺅"}, {"人", "⺅"}, {"𠆢", "⺅"},
      {"⺅", "⺅"}, {"扌", "龵"}, {"手", "龵"}, {"龵", "龵"}, {"忄", "⺖"},
      {"心", "⺖"}, {"⺖", "⺖"}, {"⺗", "⺖"}, {"⺼", "肉"}, {"月", "肉"},
      {"肉", "肉"}, {"⺝", "肉"}, {"艹", "⺾"}, {"艸", "⺾"}, {"⺾", "⺾"},
      {"⻀", "⺾"}, {"草", "⺾"}, {"礻", "⺬"}, {"示", "⺬"}, {"⺬", "⺬"},
      {"衤", "⺭"}, {"衣", "⺭"}, {"⺭", "⺭"}, {"𧘇", "⺭"}, {"釒", "⻐"},
      {"金", "⻐"}, {"钅", "⻐"}, {"⻐", "⻐"}, {"⻑", "⻐"}, {"飠", "⻠"},
      {"食", "⻠"}, {"饣", "⻠"}, {"⻠", "⻠"}, {"⻡", "⻠"}, {"𩙿", "⻠"},
      {"糹", "⺰"}, {"糸", "⺰"}, {"纟", "⺰"}, {"⺰", "⺰"}, {"⺱", "⺰"},
      {"犭", "⺲"}, {"犬", "⺲"}, {"⺲", "⺲"}, {"辶", "⻌"}, {"辵", "⻌"},
      {"⻌", "⻌"}, {"⻍", "⻌"}, {"阝", "⻖"}, {"阜", "⻖"}, {"邑", "⻖"},
      {"⻖", "⻖"}, {"⻗", "⻖"}, {"王", "⺩"}, {"⺩", "⺩"}, {"𤣩", "⺩"},
      {"玉", "⺩"}, {"⺆", "冂"}, {"冂", "冂"}, {"攵", "⺙"}, {"攴", "⺙"},
      {"⺙", "⺙"}, {"耂", "⺹"}, {"老", "⺹"}, {"⺹", "⺹"}, {"足", "⻊"},
      {"⻊", "⻊"}, {"𧾷", "⻊"}, {"四", "⺫"}, {"罒", "⺫"}, {"网", "⺫"},
      {"⺫", "⺫"}, {"竹", "⺮"}, {"⺮", "⺮"}, {"𥫗", "⺮"}, {"刀", "⺉"},
      {"刂", "⺉"}, {"⺈", "⺉"}, {"⺉", "⺉"}, {"羊", "⺶"}, {"⺶", "⺶"},
      {"⺷", "⺶"}, {"𦍌", "⺶"}, {"言", "讠"}, {"讠", "讠"}, {"訁", "讠"},
      {"号", "号"}, {"號", "号"}, {"牛", "牜"}, {"牜", "牜"}, {"⺧", "牜"},
      {"龍", "龙"}, {"龙", "龙"}, {"馬", "马"}, {"马", "马"}, {"鳥", "鸟"},
      {"鸟", "鸟"}, {"魚", "鱼"}, {"鱼", "鱼"}, {"車", "车"}, {"车", "车"},
      {"門", "门"}, {"门", "门"}, {"見", "见"}, {"见", "见"}, {"貝", "贝"},
      {"贝", "贝"}, {"頁", "页"}, {"页", "页"}, {"風", "风"}, {"风", "风"},
      {"飛", "飞"}, {"飞", "飞"}, {"黽", "黾"}, {"黾", "黾"}, {"齊", "齐"},
      {"齐", "齐"}, {"齒", "齿"}, {"齿", "齿"}, {"長", "长"}, {"长", "长"},
      {"撇", "㇒"}, {"丿", "㇒"}, {"㇒", "㇒"}, {"横", "㇐"}, {"一", "㇐"},
      {"㇐", "㇐"}, {"竖", "㇑"}, {"丨", "㇑"}, {"㇑", "㇑"}, {"点", "㇔"},
      {"丶", "㇔"}, {"㇔", "㇔"}, {"捺", "㇏"}, {"乀", "㇏"}, {"乁", "㇏"},
      {"㇏", "㇏"}, {"提", "㇀"}, {"㇀", "㇀"}, {"折", "乛"}, {"乛", "乛"},
      {"钩", "亅"}, {"鉤", "亅"}, {"亅", "亅"},
  };
  return kMap;
}

std::string Canon(const std::string& c) {
  if (c.empty())
    return c;
  if (c[0] == '{' && c[c.size() - 1] == '}') {
    std::string inner = c.substr(1, c.size() - 2);
    auto it = CanonMap().find(inner);
    return it != CanonMap().end() ? it->second : inner;
  }
  auto it = CanonMap().find(c);
  return it != CanonMap().end() ? it->second : c;
}

const rime::hash_map<std::string, int>& BlockAliasMap() {
  static const rime::hash_map<std::string, int> kMap = {
      {"基本", 0},      {"基本区", 0},      {"basic", 0},     {"base", 0},
      {"cjk", 0},       {"扩a", 1},         {"exta", 1},      {"ext-a", 1},
      {"a", 1},         {"扩b", 2},         {"extb", 2},      {"ext-b", 2},
      {"b", 2},         {"扩c", 3},         {"extc", 3},      {"ext-c", 3},
      {"c", 3},         {"扩d", 4},         {"extd", 4},      {"ext-d", 4},
      {"d", 4},         {"扩e", 5},         {"exte", 5},      {"ext-e", 5},
      {"e", 5},         {"扩f", 6},         {"extf", 6},      {"ext-f", 6},
      {"f", 6},         {"扩g", 7},         {"extg", 7},      {"ext-g", 7},
      {"g", 7},         {"扩h", 8},         {"exth", 8},      {"ext-h", 8},
      {"h", 8},         {"扩i", 9},         {"exti", 9},      {"ext-i", 9},
      {"i", 9},         {"扩j", 10},        {"extj", 10},     {"ext-j", 10},
      {"j", 10},        {"兼容区", 11},     {"兼容", 11},     {"compat", 11},
      {"兼容补充", 12}, {"compat-sup", 12}, {"部首补充", 13}, {"rad-sup", 13},
      {"康熙部首", 14}, {"kangxi", 14},     {"笔画", 15},     {"strokes", 15},
  };
  return kMap;
}

// ---------------------------------------------------------------------------
// MappedFile：POSIX mmap；Windows 退化为整读缓冲。
// ---------------------------------------------------------------------------

struct IdsStore::MappedFile {
  const uint8_t* data = nullptr;
  size_t size = 0;
#if !defined(_WIN32)
  int fd = -1;
  void* addr = nullptr;
  ~MappedFile() {
    if (addr)
      ::munmap(addr, size);
    if (fd >= 0)
      ::close(fd);
  }
#else
  std::string buf;
  ~MappedFile() = default;
#endif
};

namespace {

inline uint64_t Rd64(const uint8_t* p) {
  return static_cast<uint64_t>(Rd32(p)) |
         (static_cast<uint64_t>(Rd32(p + 4)) << 32);
}

std::string ToLowerAscii(const std::string& s) {
  std::string r = s;
  for (auto& ch : r) {
    if (ch >= 'A' && ch <= 'Z')
      ch = static_cast<char>(ch + 32);
  }
  return r;
}

void Trim(std::string* s) {
  size_t b = 0, e = s->size();
  while (b < e && (s->at(b) == ' ' || s->at(b) == '\t' || s->at(b) == '\r'))
    ++b;
  while (e > b && (s->at(e - 1) == ' ' || s->at(e - 1) == '\t' ||
                   s->at(e - 1) == '\r' || s->at(e - 1) == '\n'))
    --e;
  *s = s->substr(b, e - b);
}

}  // namespace

IdsStore& IdsStore::Instance() {
  static IdsStore kInstance;
  return kInstance;
}

IdsStore::~IdsStore() = default;

bool IdsStore::Parse() {
  if (size_ < 32 || std::memcmp(base_, "IDS4", 4) != 0)
    return false;
  if (Rd16(base_ + 4) != 3)
    return false;  // 仅支持 IDS4 v3（含必需 PY_CHAR section）。
  max_cp_ = Rd32(base_ + 8);
  num_chars_ = Rd32(base_ + 12);
  uint32_t nkeys = Rd32(base_ + 16);
  uint32_t nsec = Rd32(base_ + 20);
  const uint8_t* st = base_ + 32;
  if (size_ < 32 + static_cast<size_t>(nsec) * 24)
    return false;

  const uint8_t* tokens_sec = nullptr;
  uint64_t tokens_len = 0;
  const uint8_t* py_sec = nullptr;
  uint64_t py_len = 0;
  const uint8_t* pychar_sec = nullptr;
  uint64_t pychar_len = 0;
  const uint8_t* keyrec_sec = nullptr;
  uint64_t keyrec_len = 0;
  const uint8_t* atomset_sec = nullptr;
  uint64_t atomset_len = 0;
  const uint8_t* leaf_struct_sec = nullptr;
  uint64_t leaf_struct_len = 0;

  for (uint32_t i = 0; i < nsec; ++i) {
    const uint8_t* e = st + static_cast<size_t>(i) * 24;
    uint32_t type = Rd32(e);
    uint64_t off = Rd64(e + 8);
    uint64_t len = Rd64(e + 16);
    if (off + len > size_ || off > size_)
      return false;
    const uint8_t* p = base_ + off;
    switch (type) {
      case kSecCp2Id:
        cp2id_ = reinterpret_cast<const uint32_t*>(p);
        break;
      case kSecId2Cp:
        id2cp_ = reinterpret_cast<const uint32_t*>(p);
        break;
      case kSecHot:
        hot_ = reinterpret_cast<const HotRec*>(p);
        break;
      case kSecColdIdx:
        cold_idx_ = reinterpret_cast<const ColdIdxRec*>(p);
        break;
      case kSecAlts:
        alts_ = p;
        break;
      case kSecLeaves:
        leaves_ = p;
        break;
      case kSecAtoms:
        atoms_ = p;
        break;
      case kSecCompSet:
        compset_ = p;
        break;
      case kSecCold:
        cold_ = p;
        break;
      case kSecTokens:
        tokens_sec = p;
        tokens_len = len;
        break;
      case kSecPy:
        py_sec = p;
        py_len = len;
        break;
      case kSecPyChar:
        pychar_sec = p;
        pychar_len = len;
        break;
      case kSecKeyLookup:
        key_lookup_ = reinterpret_cast<const uint32_t*>(p);
        break;
      case kSecKeyRec:
        keyrec_sec = p;
        keyrec_len = len;
        break;
      case kSecPost:
        post_ = p;
        break;
      case kSecVarOff:
        var_off_ = reinterpret_cast<const uint32_t*>(p);
        break;
      case kSecVar:
        var_ = p;
        break;
      case kSecTokCanon:
        tok_canon_ = reinterpret_cast<const uint16_t*>(p);
        break;
      case kSecAtomicSet:
        atomset_sec = p;
        atomset_len = len;
        break;
      case kSecLeafStruct:
        leaf_struct_sec = p;
        leaf_struct_len = len;
        break;
      default:
        break;
    }
  }
  if (!cp2id_ || !id2cp_ || !hot_ || !cold_idx_ || !alts_ || !leaves_ ||
      !atoms_ || !compset_ || !cold_ || !post_ || !key_lookup_ || !tokens_sec ||
      !py_sec || !pychar_sec || !keyrec_sec || !leaf_struct_sec || !tok_canon_)
    return false;

  // LEAF_STRUCT: u32 count + count * {u32 hash, u32 id}
  if (leaf_struct_len < 4)
    return false;
  leaf_struct_ = leaf_struct_sec;
  leaf_struct_count_ = Rd32(leaf_struct_sec);
  if (4 + static_cast<uint64_t>(leaf_struct_count_) * 8 > leaf_struct_len)
    return false;
  if (leaf_struct_count_ != num_chars_)
    return false;

  // TOKENS
  {
    const uint8_t* p = tokens_sec;
    const uint8_t* end = tokens_sec + tokens_len;
    if (p + 2 > end)
      return false;
    num_tokens_ = Rd16(p);
    p += 2;
    token_str_.reserve(num_tokens_);
    token_cp_.reserve(num_tokens_);
    for (uint16_t t = 0; t < num_tokens_; ++t) {
      if (p + 2 > end)
        return false;
      uint16_t len = Rd16(p);
      p += 2;
      if (p + len + 4 > end)
        return false;
      std::string s(reinterpret_cast<const char*>(p), len);
      p += len;
      uint32_t cp = Rd32(p);
      p += 4;
      token_index_[s] = t;
      token_str_.push_back(std::move(s));
      token_cp_.push_back(cp);
    }
  }

  // PY
  {
    const uint8_t* p = py_sec;
    const uint8_t* end = py_sec + py_len;
    if (p + 2 > end)
      return false;
    uint16_t np = Rd16(p);
    p += 2;
    for (uint16_t k = 0; k < np; ++k) {
      if (p + 2 > end)
        return false;
      uint16_t len = Rd16(p);
      p += 2;
      if (p + len + 2 > end)
        return false;
      std::string py(reinterpret_cast<const char*>(p), len);
      p += len;
      uint16_t m = Rd16(p);
      p += 2;
      if (p + static_cast<size_t>(m) * 2 > end)
        return false;
      std::vector<uint16_t> ids;
      ids.reserve(m);
      for (uint16_t j = 0; j < m; ++j) {
        ids.push_back(Rd16(p));
        p += 2;
      }
      valid_pinyins_.insert(py);
      py_map_[py] = std::move(ids);
    }
  }

  // PY_CHAR: pinyin -> all character ids（O 模式候选，含异读）。
  {
    const uint8_t* p = pychar_sec;
    const uint8_t* end = pychar_sec + pychar_len;
    if (p + 2 > end)
      return false;
    uint16_t np = Rd16(p);
    p += 2;
    for (uint16_t k = 0; k < np; ++k) {
      if (p + 2 > end)
        return false;
      uint16_t len = Rd16(p);
      p += 2;
      if (p + len + 2 > end)
        return false;
      std::string py(reinterpret_cast<const char*>(p), len);
      p += len;
      uint16_t m = Rd16(p);
      p += 2;
      if (p + static_cast<size_t>(m) * 4 > end)
        return false;
      std::vector<uint32_t> ids;
      ids.reserve(m);
      for (uint16_t j = 0; j < m; ++j) {
        ids.push_back(Rd32(p));
        p += 4;
      }
      py_char_map_[py] = std::move(ids);
    }
  }

  // KEY_REC
  key_entries_.reserve(nkeys);
  {
    const uint8_t* p = keyrec_sec;
    const uint8_t* end = keyrec_sec + keyrec_len;
    while (p + 20 <= end) {
      KeyRec rec;
      rec.mult = p[0];
      rec.entry.kind = p[1];
      rec.entry.head_off = Rd32(p + 4);
      rec.entry.head_cnt = Rd32(p + 8);
      rec.entry.full_off = Rd32(p + 12);
      rec.entry.full_cnt = Rd32(p + 16);
      key_entries_.push_back(rec);
      p += 20;
    }
    if (static_cast<uint32_t>(key_entries_.size()) != nkeys)
      return false;
  }

  // ATOMIC_SET
  if (atomset_sec) {
    const uint8_t* p = atomset_sec;
    const uint8_t* end = atomset_sec + atomset_len;
    if (p + 2 <= end) {
      uint16_t n = Rd16(p);
      p += 2;
      if (p + static_cast<size_t>(n) * 2 <= end) {
        for (uint16_t i = 0; i < n; ++i) {
          atomic_tokens_.insert(Rd16(p));
          p += 2;
        }
      }
    }
  }

  return true;
}

std::vector<uint32_t> IdsStore::LookupLeafStructure(
    const std::vector<uint16_t>& tokens) const {
  std::vector<uint32_t> out;
  if (tokens.empty() || !leaf_struct_ || leaf_struct_count_ == 0)
    return out;

  std::vector<uint16_t> want;
  want.reserve(tokens.size());
  for (uint16_t t : tokens)
    want.push_back(CanonToken(t));
  std::sort(want.begin(), want.end());

  uint32_t hash = LeafStructHash(want.data(), want.size());
  uint32_t lo = 0;
  uint32_t hi = leaf_struct_count_;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (LeafStructHashAt(mid) < hash)
      lo = mid + 1;
    else
      hi = mid;
  }

  std::vector<uint16_t> got;
  for (uint32_t i = lo; i < leaf_struct_count_; ++i) {
    if (LeafStructHashAt(i) != hash)
      break;
    uint32_t id = LeafStructIdAt(i);
    const uint16_t* leaves = nullptr;
    uint16_t n = 0;
    Leaves(id, &leaves, &n);
    if (n != want.size())
      continue;
    got.clear();
    got.reserve(n);
    for (uint16_t j = 0; j < n; ++j)
      got.push_back(CanonToken(leaves[j]));
    std::sort(got.begin(), got.end());
    if (got == want)
      out.push_back(id);
  }
  return out;
}

bool IdsStore::SmallContains(const KeyEntry& e, uint32_t id) const {
  const uint32_t* a = reinterpret_cast<const uint32_t*>(post_ + e.full_off);
  return std::binary_search(a, a + e.full_cnt, id);
}

bool IdsStore::Load(const fs::path& data_path, const fs::path& custom_path) {
  if (loaded_)
    return true;

  auto file = std::make_shared<MappedFile>();
#if !defined(_WIN32)
  file->fd = ::open(data_path.c_str(), O_RDONLY);
  if (file->fd < 0)
    return false;
  struct stat sb;
  if (::fstat(file->fd, &sb) != 0 || sb.st_size <= 0) {
    ::close(file->fd);
    return false;
  }
  file->size = static_cast<size_t>(sb.st_size);
  file->addr = ::mmap(nullptr, file->size, PROT_READ, MAP_PRIVATE, file->fd, 0);
  if (file->addr == MAP_FAILED) {
    file->addr = nullptr;
    ::close(file->fd);
    return false;
  }
  file->data = static_cast<const uint8_t*>(file->addr);
#else
  {
    std::ifstream f(data_path, std::ios::binary);
    if (!f)
      return false;
    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    if (sz <= 0)
      return false;
    file->buf.resize(static_cast<size_t>(sz));
    f.seekg(0, std::ios::beg);
    f.read(&file->buf[0], sz);
    file->data = reinterpret_cast<const uint8_t*>(file->buf.data());
    file->size = static_cast<size_t>(sz);
  }
#endif

  file_ = file;
  base_ = file->data;
  size_ = file->size;

  if (!Parse()) {
    file_.reset();
    base_ = nullptr;
    size_ = 0;
    return false;
  }
  if (!custom_path.empty())
    ApplyCustomRules(custom_path);
  loaded_ = true;
  return true;
}

void IdsStore::ApplyCustomRules(const fs::path& path) {
  std::ifstream in(path);
  if (!in)
    return;

  auto add_token_pair = [&](std::vector<uint16_t>* v, const std::string& comp) {
    uint16_t t;
    if (FindToken(comp, &t) && std::find(v->begin(), v->end(), t) == v->end())
      v->push_back(t);
    std::string c = Canon(comp);
    if (c != comp && FindToken(c, &t) &&
        std::find(v->begin(), v->end(), t) == v->end())
      v->push_back(t);
  };
  auto remove_token_pair = [&](std::vector<uint16_t>* v,
                               const std::string& comp) {
    uint16_t t;
    if (FindToken(comp, &t))
      v->erase(std::remove(v->begin(), v->end(), t), v->end());
    std::string c = Canon(comp);
    if (c != comp && FindToken(c, &t))
      v->erase(std::remove(v->begin(), v->end(), t), v->end());
  };

  std::string line;
  while (std::getline(in, line)) {
    size_t hash = line.find('#');
    if (hash != std::string::npos)
      line.erase(hash);
    Trim(&line);
    if (line.empty())
      continue;
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < line.size()) {
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
      size_t j = i;
      while (j < line.size() && line[j] != ' ' && line[j] != '\t')
        ++j;
      if (j > i)
        tokens.push_back(line.substr(i, j - i));
      i = j;
    }
    if (tokens.size() >= 2 &&
        (tokens[0] == "-block" || tokens[0] == "-区" || tokens[0] == "-分区")) {
      for (size_t k = 1; k < tokens.size(); ++k) {
        auto it = BlockAliasMap().find(ToLowerAscii(tokens[k]));
        if (it != BlockAliasMap().end())
          custom_disabled_blocks_.insert(it->second);
      }
    } else if (tokens.size() == 1 && !tokens[0].empty() &&
               tokens[0][0] == '-') {
      std::string del_py = ToLowerAscii(tokens[0].substr(1));
      py_map_.erase(del_py);
      valid_pinyins_.erase(del_py);
    } else if (tokens.size() >= 2) {
      const std::string& comp = tokens[0];
      bool has_del = false;
      bool has_override = false;
      for (size_t k = 1; k < tokens.size(); ++k) {
        if (tokens[k].empty())
          continue;
        if (tokens[k][0] == '-')
          has_del = true;
        else if (tokens[k][0] == '=')
          has_override = true;
      }
      if (has_del) {
        for (size_t k = 1; k < tokens.size(); ++k) {
          if (!tokens[k].empty() && tokens[k][0] == '-') {
            std::string del_py = ToLowerAscii(tokens[k].substr(1));
            auto it = py_map_.find(del_py);
            if (it != py_map_.end()) {
              remove_token_pair(&it->second, comp);
              if (it->second.empty())
                py_map_.erase(it);
            }
          }
        }
      } else if (has_override) {
        for (auto& kv : py_map_)
          remove_token_pair(&kv.second, comp);
        for (size_t k = 1; k < tokens.size(); ++k) {
          std::string py =
              (tokens[k][0] == '=') ? tokens[k].substr(1) : tokens[k];
          py = ToLowerAscii(py);
          if (py.empty())
            continue;
          add_token_pair(&py_map_[py], comp);
          valid_pinyins_.insert(py);
        }
      } else {
        for (size_t k = 1; k < tokens.size(); ++k) {
          std::string py = ToLowerAscii(tokens[k]);
          if (py.empty())
            continue;
          add_token_pair(&py_map_[py], comp);
          valid_pinyins_.insert(py);
        }
      }
    }
  }
  in.close();
}

}  // namespace ids_reverse
