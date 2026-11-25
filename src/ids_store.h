//
// ids_store.h — ids_reverse 插件数据层。
// 加载 IDS4 单文件（mmap 惰性映射），提供稠密 id / 热字段 / 整数部件匹配。
//
#ifndef RIME_IDS_REVERSE_IDS_STORE_H_
#define RIME_IDS_REVERSE_IDS_STORE_H_

#include <cstdint>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rime/common.h>

namespace ids_reverse {

namespace fs = std::filesystem;

// 偏旁等价归一表（与生成脚本的 C2 表完全一致）。
const rime::hash_map<std::string, std::string>& CanonMap();
std::string Canon(const std::string& c);

// 区块别名表。
const rime::hash_map<std::string, int>& BlockAliasMap();

constexpr uint16_t kNoToken = 0xFFFF;
constexpr uint32_t kInvalidId = 0xFFFFFFFF;

// IDS4 section type ids（必须与 generate_data_v4.py 一致）。
enum SectionType {
  kSecCp2Id = 1,
  kSecId2Cp = 2,
  kSecHot = 3,
  kSecColdIdx = 4,
  kSecAlts = 5,
  kSecLeaves = 6,
  kSecAtoms = 7,
  kSecCompSet = 8,
  kSecCold = 9,
  kSecTokens = 10,
  kSecPy = 11,
  kSecKeyLookup = 12,
  kSecKeyRec = 13,
  kSecPost = 14,
  kSecVarOff = 15,
  kSecVar = 16,
  kSecTokCanon = 17,
  kSecAtomicSet = 18,
  kSecLeafStruct = 19,
  kSecPyChar = 20,
};

// 顶层部件多重集精确匹配的排序保底。普通 ALTS 伪精确匹配最高约 5700 万，
// 普通混合叶序匹配最高约 6700 万，因此精确结构至少保底 7000 万。
constexpr int64_t kExactStructureBonus = 20000000;
constexpr int64_t kExactStructureFloor = 70000000;

inline int64_t BoostExactStructureScore(int64_t score) {
  score += kExactStructureBonus;
  return score < kExactStructureFloor ? kExactStructureFloor : score;
}

inline uint32_t LeafStructHash(const uint16_t* tokens, size_t n) {
  uint32_t h = 2166136261u;
  auto mix = [&h](uint8_t v) {
    h ^= v;
    h *= 16777619u;
  };
  mix(static_cast<uint8_t>(n & 0xFF));
  mix(static_cast<uint8_t>((n >> 8) & 0xFF));
  for (size_t i = 0; i < n; ++i) {
    uint16_t t = tokens[i];
    mix(static_cast<uint8_t>(t & 0xFF));
    mix(static_cast<uint8_t>((t >> 8) & 0xFF));
  }
  return h;
}

// 每字热记录（24 字节）：按 id 直索引。
struct HotRec {
  uint32_t freq = 0;
  uint32_t flags = 0;  // bit L 置位表示存在长度为 L 的 alts 变体
  uint16_t char_token = kNoToken;
  uint8_t depth = 0;
  uint8_t block = 0;
  uint32_t alts_off = 0;
  uint32_t leaf_off = 0;
  uint32_t atom_off = 0;
};
static_assert(sizeof(HotRec) == 24, "HotRec must match IDS4 CharRec");

struct ColdIdxRec {
  uint32_t cs_off = 0;
  uint32_t cold_off = 0;
};
static_assert(sizeof(ColdIdxRec) == 8, "ColdIdxRec must match IDS4");

struct KeyEntry {
  uint32_t head_off = 0;  // POST 区字节偏移
  uint32_t head_cnt = 0;
  uint32_t full_off = 0;  // POST 区字节偏移
  uint32_t full_cnt = 0;
  uint8_t kind = 0;  // 0 = 升序 u32 数组; 1 = 位图
};

struct KeyRec {
  uint8_t mult = 1;
  KeyEntry entry;
};

// 小端读取（未对齐安全）。
inline uint16_t Rd16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t Rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// 简单 LRU 缓存（供切分 / matcher / 查询结果缓存使用）。
template <class Key, class Val>
class LruCache {
 public:
  explicit LruCache(size_t cap) : cap_(cap == 0 ? 1 : cap) {}

  Val* Find(const Key& k) {
    auto it = map_.find(k);
    if (it == map_.end())
      return nullptr;
    order_.splice(order_.begin(), order_, it->second.order_it);
    return &it->second.val;
  }

  Val& Ensure(const Key& k) {
    auto it = map_.find(k);
    if (it != map_.end()) {
      order_.splice(order_.begin(), order_, it->second.order_it);
      return it->second.val;
    }
    if (map_.size() >= cap_) {
      Key oldest = order_.back();
      order_.pop_back();
      map_.erase(oldest);
    }
    auto res = map_.emplace(k, Entry{Val(), order_.end()});
    order_.push_front(k);
    res.first->second.order_it = order_.begin();
    return res.first->second.val;
  }

  void Put(const Key& k, Val v) {
    auto it = map_.find(k);
    if (it != map_.end()) {
      it->second.val = std::move(v);
      order_.splice(order_.begin(), order_, it->second.order_it);
      return;
    }
    if (map_.size() >= cap_) {
      Key oldest = order_.back();
      order_.pop_back();
      map_.erase(oldest);
    }
    auto res = map_.emplace(k, Entry{std::move(v), order_.end()});
    order_.push_front(k);
    res.first->second.order_it = order_.begin();
  }

  void Erase(const Key& k) {
    auto it = map_.find(k);
    if (it == map_.end())
      return;
    order_.erase(it->second.order_it);
    map_.erase(it);
  }

  void Clear() {
    map_.clear();
    order_.clear();
  }

 private:
  struct Entry {
    Val val;
    typename std::list<Key>::iterator order_it;
  };
  rime::hash_map<Key, Entry> map_;
  std::list<Key> order_;
  size_t cap_;
};

// IDS4 数据仓库：mmap 惰性映射 + 稠密 id 直索引。
class IdsStore {
 public:
  static IdsStore& Instance();

  bool Load(const fs::path& data_path, const fs::path& custom_path);
  bool loaded() const { return loaded_; }

  uint32_t num_chars() const { return num_chars_; }
  uint32_t max_cp() const { return max_cp_; }

  uint32_t IdOfCp(uint32_t cp) const {
    if (cp == 0 || cp >= max_cp_)
      return kInvalidId;
    uint32_t v = cp2id_[cp];
    return v == 0 ? kInvalidId : v - 1;
  }
  uint32_t CpOfId(uint32_t id) const {
    return id < num_chars_ ? id2cp_[id] : 0;
  }

  const HotRec& Hot(uint32_t id) const {
    static const HotRec kEmpty;
    return id < num_chars_ ? hot_[id] : kEmpty;
  }

  // ALTS / COMPSET / COLD 记录指针。
  const uint8_t* Alts(uint32_t id) const { return alts_ + hot_[id].alts_off; }
  const uint8_t* CompSetRec(uint32_t id) const {
    return compset_ + cold_idx_[id].cs_off;
  }
  // 直接取字典序升序的部件 id 列表。
  void CompSet(uint32_t id, const uint16_t** out, uint16_t* n) const {
    const uint8_t* p = CompSetRec(id);
    *n = Rd16(p);
    *out = reinterpret_cast<const uint16_t*>(p + 2);
  }

  // 预计算顶层叶子（原始字形 token）。
  void Leaves(uint32_t id, const uint16_t** out, uint16_t* n) const {
    if (id >= num_chars_) {
      *out = nullptr;
      *n = 0;
      return;
    }
    const uint8_t* p = leaves_ + hot_[id].leaf_off;
    *n = Rd16(p);
    *out = reinterpret_cast<const uint16_t*>(p + 2);
  }

  // 预计算原子部件（保持 AtomicDecomposition 顺序）。
  void Atomics(uint32_t id, const uint16_t** out, uint16_t* n) const {
    if (id >= num_chars_) {
      *out = nullptr;
      *n = 0;
      return;
    }
    const uint8_t* p = atoms_ + hot_[id].atom_off;
    *n = Rd16(p);
    *out = reinterpret_cast<const uint16_t*>(p + 2);
  }

  // 顶层部件多重集索引：hash -> id 区间，调用方负责校验 LEAVES。
  uint32_t leaf_struct_count() const { return leaf_struct_count_; }
  uint32_t LeafStructHashAt(uint32_t i) const {
    return i < leaf_struct_count_ ? Rd32(leaf_struct_ + 4 + i * 8) : 0;
  }
  uint32_t LeafStructIdAt(uint32_t i) const {
    return i < leaf_struct_count_ ? Rd32(leaf_struct_ + 8 + i * 8) : kInvalidId;
  }
  // 查询顶层叶子多重集：内部完成 canonical 化、排序、hash 查找和 LEAVES 校验。
  std::vector<uint32_t> LookupLeafStructure(
      const std::vector<uint16_t>& tokens) const;

  // 供 translator 直接解码 COLD：返回 py/ids 长度与指针。
  void Cold(uint32_t id,
            const uint8_t** py,
            uint16_t* py_len,
            const uint8_t** ids,
            uint16_t* ids_len) const {
    const uint8_t* p = cold_ + cold_idx_[id].cold_off;
    *py_len = Rd16(p);
    *py = p + 2;
    p = *py + *py_len;
    *ids_len = Rd16(p);
    *ids = p + 2;
  }

  // tokens
  uint16_t NTokens() const { return num_tokens_; }
  const std::string& TokenStr(uint16_t id) const {
    static const std::string kEmpty;
    return id < num_tokens_ ? token_str_[id] : kEmpty;
  }
  uint32_t TokenCp(uint16_t id) const {
    return id < num_tokens_ ? token_cp_[id] : 0;
  }
  uint16_t CanonToken(uint16_t id) const {
    return (tok_canon_ && id < num_tokens_) ? tok_canon_[id] : id;
  }
  bool FindToken(const std::string& s, uint16_t* out) const {
    auto it = token_index_.find(s);
    if (it == token_index_.end())
      return false;
    *out = it->second;
    return true;
  }
  bool IsAtomicToken(uint16_t t) const { return atomic_tokens_.count(t) != 0; }

  // 找一个 term 的 token id：先原文，再 Canon 归一。
  uint16_t ResolveTerm(const std::string& s) const {
    uint16_t t;
    if (FindToken(s, &t))
      return t;
    std::string c = Canon(s);
    if (c != s && FindToken(c, &t))
      return t;
    return kNoToken;
  }

  // 异体字查询：返回 id 对应的变体字 id 数组（无变体时 n=0）。
  void Variants(uint32_t id, const uint32_t** out, uint16_t* n) const {
    *n = 0;
    *out = nullptr;
    if (!var_ || !var_off_ || id >= num_chars_)
      return;
    const uint8_t* p = var_ + var_off_[id];
    *n = Rd16(p);
    *out = reinterpret_cast<const uint32_t*>(p + 2);
  }

  // keys / postings（按 token + 重数直接寻址）
  const KeyEntry* FindKeyByToken(uint16_t token, int mult) const {
    if (token >= num_tokens_)
      return nullptr;
    uint32_t b = key_lookup_[token];
    uint32_t e = key_lookup_[token + 1];
    for (uint32_t i = b; i < e; ++i) {
      if (key_entries_[i].mult == mult)
        return &key_entries_[i].entry;
    }
    return nullptr;
  }
  const KeyEntry* FindKey(const std::string& k) const {
    if (k.empty())
      return nullptr;
    int mult = 1;
    std::string base = k;
    size_t star = k.rfind('*');
    if (star != std::string::npos && star + 1 < k.size()) {
      int m = 0;
      bool ok = true;
      for (size_t i = star + 1; i < k.size(); ++i) {
        char c = k[i];
        if (c < '0' || c > '9') {
          ok = false;
          break;
        }
        m = m * 10 + (c - '0');
      }
      if (ok && m > 1) {
        mult = m;
        base = k.substr(0, star);
      }
    }
    uint16_t token;
    if (!FindToken(base, &token))
      return nullptr;
    return FindKeyByToken(token, mult);
  }
  const uint32_t* HeadIds(const KeyEntry& e) const {
    return reinterpret_cast<const uint32_t*>(post_ + e.head_off);
  }
  const uint32_t* FullIds(const KeyEntry& e) const {
    return reinterpret_cast<const uint32_t*>(post_ + e.full_off);
  }
  bool FullContains(const KeyEntry& e, uint32_t id) const {
    if (e.kind == 0)
      return SmallContains(e, id);
    const uint64_t* w = reinterpret_cast<const uint64_t*>(post_ + e.full_off) +
                        (static_cast<size_t>(id) >> 6);
    return (*w >> (id & 63)) & 1ULL;
  }
  template <class F>
  void ForEachFull(const KeyEntry& e, F&& f) const {
    if (e.kind == 0) {
      const uint32_t* a = reinterpret_cast<const uint32_t*>(post_ + e.full_off);
      for (uint32_t i = 0; i < e.full_cnt; ++i)
        if (!f(a[i]))
          return;
    } else {
      const uint64_t* base =
          reinterpret_cast<const uint64_t*>(post_ + e.full_off);
      for (uint32_t id = 0; id < num_chars_; ++id) {
        if ((base[id >> 6] >> (id & 63)) & 1ULL)
          if (!f(id))
            return;
      }
    }
  }

  // pinyin
  const std::map<std::string, std::vector<uint16_t>>& py_map() const {
    return py_map_;
  }
  // O 模式部件候选：无调拼音 -> 全字符 id（按字频降序）。
  const std::map<std::string, std::vector<uint32_t>>& py_char_map() const {
    return py_char_map_;
  }
  const rime::hash_set<std::string>& valid_pinyins() const {
    return valid_pinyins_;
  }

  const rime::hash_set<int>& custom_disabled_blocks() const {
    return custom_disabled_blocks_;
  }

 private:
  IdsStore() = default;
  ~IdsStore();
  IdsStore(const IdsStore&) = delete;
  IdsStore& operator=(const IdsStore&) = delete;

  bool Parse();
  void ApplyCustomRules(const fs::path& path);
  bool SmallContains(const KeyEntry& e, uint32_t id) const;

  struct MappedFile;
  std::shared_ptr<MappedFile> file_;  // mmap / 文件缓冲的所有权
  const uint8_t* base_ = nullptr;
  size_t size_ = 0;

  const uint32_t* cp2id_ = nullptr;
  const uint32_t* id2cp_ = nullptr;
  const HotRec* hot_ = nullptr;
  const ColdIdxRec* cold_idx_ = nullptr;
  const uint8_t* alts_ = nullptr;
  const uint8_t* leaves_ = nullptr;
  const uint8_t* atoms_ = nullptr;
  const uint8_t* compset_ = nullptr;
  const uint8_t* cold_ = nullptr;
  const uint8_t* post_ = nullptr;
  const uint32_t* var_off_ = nullptr;
  const uint8_t* var_ = nullptr;
  const uint32_t* key_lookup_ = nullptr;
  const uint16_t* tok_canon_ = nullptr;
  const uint8_t* leaf_struct_ = nullptr;
  uint32_t leaf_struct_count_ = 0;

  uint32_t num_chars_ = 0;
  uint32_t max_cp_ = 0;
  uint32_t num_tokens_ = 0;

  std::vector<std::string> token_str_;
  std::vector<uint32_t> token_cp_;
  rime::hash_map<std::string, uint16_t> token_index_;
  rime::hash_set<uint16_t> atomic_tokens_;
  std::vector<KeyRec> key_entries_;

  std::map<std::string, std::vector<uint16_t>> py_map_;
  std::map<std::string, std::vector<uint32_t>> py_char_map_;
  rime::hash_set<std::string> valid_pinyins_;
  rime::hash_set<int> custom_disabled_blocks_;
  bool loaded_ = false;
};

}  // namespace ids_reverse

#endif  // RIME_IDS_REVERSE_IDS_STORE_H_
