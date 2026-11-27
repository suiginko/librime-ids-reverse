//
// ids_reverse_search.cc — 混合检字实现。
//
#include "ids_reverse_search.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include "ids_store.h"
#include "ids_reverse_input.h"
#include "ids_text.h"

namespace ids_reverse {

namespace {

using rime::hash_map;
using rime::hash_set;

bool IsAsciiAlphaOnly(const std::string& s) {
  if (s.empty())
    return false;
  for (unsigned char c : s) {
    if (!std::isalpha(c) || c > 0x7F)
      return false;
  }
  return true;
}

std::string AsciiLower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

std::string StripSpace(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
      out.push_back(static_cast<char>(c));
  }
  return out;
}

bool StartsWith(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string NormalizeComponent(const std::string& raw,
                               const InputCodec& codec) {
  std::string s = StripSpace(raw);
  if (s.empty())
    return s;
  auto decoded = codec.DecodeExact(AsciiLower(s));
  if (!decoded.empty())
    return Canon(decoded.front());
  return Canon(s);
}

// ---------------------------------------------------------------------------
// 查询代数
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// IDS / 部件分解
// ---------------------------------------------------------------------------

// preserve_glyph=false 时返回归一化部件（用于匹配/打分）；为 true 时保留
// 数据中的原始字形（用于减法结果展示），例如 达 的 辶 不会被改写成 ⻌。
std::vector<std::string> TopLeavesById(IdsStore& store,
                                       uint32_t id,
                                       bool preserve_glyph = false) {
  std::vector<std::string> leaves;
  if (id == kInvalidId)
    return leaves;
  const uint16_t* tokens = nullptr;
  uint16_t n = 0;
  store.Leaves(id, &tokens, &n);
  for (uint16_t i = 0; i < n; ++i) {
    uint16_t t = preserve_glyph ? tokens[i] : store.CanonToken(tokens[i]);
    const std::string& s = store.TokenStr(t);
    if (!s.empty())
      leaves.push_back(s);
  }
  if (leaves.empty()) {
    uint32_t cp = store.CpOfId(id);
    if (cp) {
      std::string c = CpToUtf8(cp);
      leaves.push_back(preserve_glyph ? c : Canon(c));
    }
  }
  return leaves;
}

std::vector<std::string> TopLeavesOfComponent(IdsStore& store,
                                              const std::string& comp,
                                              bool preserve_glyph = false) {
  std::vector<std::string> chars = Utf8Chars(comp);
  if (chars.size() != 1)
    return {preserve_glyph ? comp : Canon(comp)};
  size_t pos = 0;
  uint32_t cp = DecodeNext(comp, &pos);
  uint32_t id = store.IdOfCp(cp);
  if (id == kInvalidId)
    return {preserve_glyph ? comp : Canon(comp)};
  std::vector<std::string> leaves = TopLeavesById(store, id, preserve_glyph);
  if (leaves.empty())
    leaves.push_back(preserve_glyph ? comp : Canon(comp));
  return leaves;
}

const hash_set<std::string>& AtomicComponents() {
  static const hash_set<std::string> kAtomic = {
      "木", "氵", "水", "火", "灬", "土", "金", "钅", "日", "月", "目", "口",
      "人", "亻", "手", "扌", "心", "忄", "女", "子", "艹", "言", "讠", "石",
      "田", "山", "刀", "刂", "力", "又", "十", "大", "小", "工", "弓", "广",
      "门", "马", "鸟", "鱼", "虫", "犭", "犬", "辶", "宀", "冖", "竹",
      "⺮", "米", "糸", "纟", "车", "足", "⻊", "雨", "页", "酉", "贝", "走",
      "身", "辰", "阝", "阜", "方", "斤", "止", "歹", "毛",
      "氏", "气", "白", "皮", "皿", "矢", "示", "礻", "禾", "立", "舟",
      "艮", "见", "角", "豆", "豕", "豸", "赤", "革", "风", "飞",
      "饣", "首", "骨", "鬼", "鼎", "鼓", "鼠", "鼻", "齐",
      "齿", "龙", "龟", "几", "八", "巴", "电", "习",
      // Canon() 归一后的代表字形
      "⺡", "⺣", "⺅", "龵", "⺖", "⺾", "⺬", "⺭", "⻐", "⻠", "⺰", "⺲",
      "⻌", "⻖", "⺩", "⺙", "⺹", "⻊", "⺫", "⺮", "⺉", "⺶", "㇒", "㇐",
      "㇑", "㇔", "㇏", "㇀", "乛", "亅"};
  return kAtomic;
}

std::vector<std::string> AtomicDecomposition(
    IdsStore& store,
    const std::string& comp,
    hash_map<std::string, std::vector<std::string>>* cache,
    int depth) {
  std::string ch = Canon(comp);
  auto hit = cache->find(ch);
  if (hit != cache->end())
    return hit->second;
  if (depth > 2 || AtomicComponents().count(ch)) {
    std::vector<std::string> r = {ch};
    (*cache)[ch] = r;
    return r;
  }
  std::vector<std::string> leaves = TopLeavesOfComponent(store, ch);
  if (leaves.size() <= 1) {
    std::vector<std::string> r = {ch};
    (*cache)[ch] = r;
    return r;
  }
  std::vector<std::string> out;
  for (const auto& leaf : leaves) {
    if (leaf == ch) {
      out.push_back(leaf);
    } else {
      std::vector<std::string> sub =
          AtomicDecomposition(store, leaf, cache, depth + 1);
      out.insert(out.end(), sub.begin(), sub.end());
    }
  }
  (*cache)[ch] = out;
  return out;
}

// 查询部件是否参与字形归一（CanonMap 的键或值）。只有这类查询才需要原始
// 字形优先排序；普通部件查询保持原有结构分排序。
const hash_set<std::string>& CanonEquivSet() {
  static const hash_set<std::string> kSet = [] {
    hash_set<std::string> set;
    for (const auto& kv : CanonMap()) {
      set.insert(kv.first);
      set.insert(kv.second);
    }
    return set;
  }();
  return kSet;
}

using LiteralNodeCache = hash_map<uint32_t, std::vector<uint16_t>>;

// 原始字形分解节点：从 LEAVES 的原始 token 出发递归展开，保留每一层的
// 原始字形。缓存键为 token + 深度，数据只读，可跨查询复用。
const std::vector<uint16_t>& CollectLiteralTokenNodes(IdsStore& store,
                                                      uint16_t token,
                                                      LiteralNodeCache* cache,
                                                      int depth) {
  uint32_t key =
      static_cast<uint32_t>(token) | (static_cast<uint32_t>(depth) << 16);
  auto hit = cache->find(key);
  if (hit != cache->end())
    return hit->second;

  std::vector<uint16_t> nodes;
  nodes.push_back(token);
  if (depth <= 2 && !store.IsAtomicToken(store.CanonToken(token))) {
    uint32_t cp = store.TokenCp(token);
    uint32_t id = cp ? store.IdOfCp(cp) : kInvalidId;
    if (id != kInvalidId) {
      const uint16_t* leaves = nullptr;
      uint16_t n = 0;
      store.Leaves(id, &leaves, &n);
      if (n > 1) {
        for (uint16_t i = 0; i < n; ++i) {
          uint16_t child = leaves[i];
          if (child == token)
            continue;
          const std::vector<uint16_t>& sub =
              CollectLiteralTokenNodes(store, child, cache, depth + 1);
          nodes.insert(nodes.end(), sub.begin(), sub.end());
        }
      }
    }
  }
  auto inserted = cache->emplace(key, std::move(nodes));
  return inserted.first->second;
}

struct LiteralQuery {
  std::map<uint16_t, int> counts;
  bool needed = false;
};

void BuildLiteralQuery(IdsStore& store,
                       const std::vector<std::string>& raw,
                       LiteralQuery* q) {
  for (const auto& item : raw) {
    for (const auto& ch : Utf8Chars(item)) {
      if (ch.empty())
        continue;
      if (ch.size() == 1 && static_cast<unsigned char>(ch[0]) < 0x80)
        continue;
      uint16_t t;
      if (store.FindToken(ch, &t))
        q->counts[t] += 1;
      if (CanonEquivSet().count(ch))
        q->needed = true;
    }
  }
}

int LiteralHits(IdsStore& store,
                uint32_t id,
                const LiteralQuery& q,
                LiteralNodeCache* cache) {
  const uint16_t* leaves = nullptr;
  uint16_t n = 0;
  store.Leaves(id, &leaves, &n);
  std::map<uint16_t, int> counts;
  for (uint16_t i = 0; i < n; ++i) {
    const std::vector<uint16_t>& nodes =
        CollectLiteralTokenNodes(store, leaves[i], cache, 0);
    for (uint16_t t : nodes)
      ++counts[t];
  }
  int hits = 0;
  for (const auto& kv : q.counts) {
    auto it = counts.find(kv.first);
    if (it != counts.end())
      hits += std::min(kv.second, it->second);
  }
  return hits;
}

// ---------------------------------------------------------------------------
// postings 读取 / 求交
// ---------------------------------------------------------------------------

std::vector<const KeyEntry*> KeysForTerm(IdsStore& store,
                                         const std::string& term,
                                         int count) {
  std::vector<const KeyEntry*> keys;
  auto add_token = [&](uint16_t t) {
    const KeyEntry* e = store.FindKeyByToken(t, count);
    if (!e && count > 1)
      e = store.FindKeyByToken(t, 1);
    if (e)
      keys.push_back(e);
  };
  uint16_t t;
  if (store.FindToken(term, &t)) {
    add_token(t);
    if (!keys.empty())
      return keys;
  }
  auto pit = store.py_map().find(term);
  if (pit != store.py_map().end()) {
    for (uint16_t token : pit->second)
      add_token(token);
    if (!keys.empty())
      return keys;
  }
  uint16_t rt = store.ResolveTerm(term);
  if (rt != kNoToken)
    add_token(rt);
  return keys;
}

// 完整求交：选 union 最小的 term 作为种子，遍历其全部 posting，再用
// FullContains 过滤其他 term。避免按 id 截断 posting 导致高 id 字符漏检。
std::vector<uint32_t> IntersectPostings(IdsStore& store,
                                        const std::vector<std::string>& terms,
                                        const std::vector<int>& counts) {
  if (terms.empty())
    return {};

  struct TermKeys {
    std::vector<const KeyEntry*> entries;
    uint64_t total = 0;
  };

  std::vector<TermKeys> term_keys;
  term_keys.reserve(terms.size());
  size_t seed_index = 0;
  uint64_t seed_size = std::numeric_limits<uint64_t>::max();

  for (size_t i = 0; i < terms.size(); ++i) {
    TermKeys tk;
    hash_set<const KeyEntry*> seen_keys;
    for (const KeyEntry* e : KeysForTerm(store, terms[i], counts[i])) {
      if (!seen_keys.insert(e).second)
        continue;
      if (!e || e->full_cnt == 0)
        continue;
      tk.entries.push_back(e);
      tk.total += e->full_cnt;
    }
    if (tk.entries.empty())
      return {};
    term_keys.push_back(std::move(tk));
    if (term_keys.back().total < seed_size) {
      seed_size = term_keys.back().total;
      seed_index = i;
    }
  }

  std::vector<uint32_t> result;
  hash_set<uint32_t> seed_seen;
  bool need_dedup = term_keys[seed_index].entries.size() > 1;

  // 非种子 term 的 id 升序游标。种子遍历本身按 id 升序，因此每个条目只需
  // 单调推进一次，避免对每个候选重复二分。
  std::vector<std::vector<uint32_t>> cursor(term_keys.size());
  for (size_t i = 0; i < term_keys.size(); ++i)
    cursor[i].assign(term_keys[i].entries.size(), 0);

  for (const KeyEntry* se : term_keys[seed_index].entries) {
    for (size_t i = 0; i < term_keys.size(); ++i)
      std::fill(cursor[i].begin(), cursor[i].end(), 0);

    store.ForEachFull(*se, [&](uint32_t id) {
      if (need_dedup && !seed_seen.insert(id).second)
        return true;

      for (size_t i = 0; i < term_keys.size(); ++i) {
        if (i == seed_index)
          continue;
        bool matched = false;
        const auto& entries = term_keys[i].entries;
        for (size_t j = 0; j < entries.size(); ++j) {
          const KeyEntry* other = entries[j];
          if (other->kind == 0) {
            const uint32_t* a = store.FullIds(*other);
            uint32_t& p = cursor[i][j];
            while (p < other->full_cnt && a[p] < id)
              ++p;
            if (p < other->full_cnt && a[p] == id) {
              matched = true;
              break;
            }
          } else if (store.FullContains(*other, id)) {
            matched = true;
            break;
          }
        }
        if (!matched)
          return true;
      }

      result.push_back(id);
      return true;
    });
  }

  return result;
}

// ---------------------------------------------------------------------------
// 黄金排序
// ---------------------------------------------------------------------------

void AddCount(std::map<std::string, int>* counts, const std::string& k) {
  (*counts)[k] += 1;
}

struct TokenCount {
  uint16_t token;
  int count;
};

void AddTokenCount(std::vector<TokenCount>* counts, uint16_t token) {
  for (auto& c : *counts) {
    if (c.token == token) {
      ++c.count;
      return;
    }
  }
  counts->push_back({token, 1});
}

int TokenCountOf(const std::vector<TokenCount>& counts, uint16_t token) {
  for (const auto& c : counts) {
    if (c.token == token)
      return c.count;
  }
  return 0;
}

const char* PairedScriptRadical(const std::string& raw) {
  if (raw == "丝")
    return "纟";
  if (raw == "纟")
    return "丝";
  if (raw == "糸")
    return "糹";
  if (raw == "糹")
    return "糸";
  if (raw == "金")
    return "釒";
  if (raw == "釒")
    return "金";
  if (raw == "食")
    return "飠";
  if (raw == "飠")
    return "食";
  if (raw == "言")
    return "訁";
  if (raw == "訁")
    return "言";
  return nullptr;
}

struct QueryAtoms {
  std::vector<uint16_t> raw;       // 未经 Canon 的原始输入 token
  std::vector<uint16_t> paired;    // 简繁成字↔偏旁同源偏好 token
  std::vector<uint16_t> norm;      // 已 Canon 的输入 token
  std::vector<TokenCount> counts;  // 输入原子部件计数
  std::vector<uint16_t> seq;       // 输入原子部件顺序
};

bool BuildQueryAtoms(IdsStore& store,
                     const std::vector<std::string>& normalized,
                     QueryAtoms* q) {
  for (const auto& raw : normalized) {
    uint16_t raw_tok = store.ResolveTerm(raw);
    if (raw_tok != kNoToken)
      q->raw.push_back(raw_tok);
    if (const char* paired_str = PairedScriptRadical(raw)) {
      uint16_t paired_tok = store.ResolveTerm(paired_str);
      if (paired_tok != kNoToken)
        q->paired.push_back(paired_tok);
    }
    std::string c = Canon(raw);
    uint16_t t = store.ResolveTerm(c);
    if (t == kNoToken)
      return false;
    uint32_t cp = store.TokenCp(t);
    uint32_t id = cp ? store.IdOfCp(cp) : kInvalidId;
    if (id == kInvalidId)
      return false;
    q->norm.push_back(store.CanonToken(t));
    const uint16_t* atoms = nullptr;
    uint16_t n = 0;
    store.Atomics(id, &atoms, &n);
    if (n == 0)
      return false;
    for (uint16_t i = 0; i < n; ++i) {
      AddTokenCount(&q->counts, atoms[i]);
      q->seq.push_back(atoms[i]);
    }
  }
  return !q->seq.empty();
}

int64_t ScoreHybridTokens(IdsStore& store, uint32_t id, const QueryAtoms& q) {
  const uint16_t* leaves = nullptr;
  uint16_t leaf_n = 0;
  store.Leaves(id, &leaves, &leaf_n);
  const uint16_t* atoms = nullptr;
  uint16_t atom_n = 0;
  store.Atomics(id, &atoms, &atom_n);

  // 单部件检字直接度分层天梯：
  // Tier 1: 纯单部件变体/镜像/自身（如 2010F 𠄏 = ⿿了）
  // Tier 2: 顶层直接包含（Leaves 显式包含该部件，如 辽, 孑, 孓, 钌, 𠮾, 𠆨, 叮, 𡤼）
  // Tier 3: 深层间接包含（如 㞌=尸子 间接包含 了，剥夺大区加分特权排在后面）
  if (q.norm.size() == 1) {
    uint16_t target_tok = q.norm[0];
    const HotRec& h = store.Hot(id);

    if (h.char_token == target_tok)
      return 50000000 + h.freq;

    bool direct_hit = false;
    int real_comp_count = 0;
    for (uint16_t i = 0; i < leaf_n; ++i) {
      uint32_t cp = store.TokenCp(leaves[i]);
      if ((cp >= 0x2FF0 && cp <= 0x2FFF) || cp <= 127)
        continue;
      ++real_comp_count;
      if (store.CanonToken(leaves[i]) == target_tok || leaves[i] == target_tok) {
        direct_hit = true;
      }
    }

    if (direct_hit && real_comp_count <= 1)
      return 40000000 + h.freq - h.depth * 1000;

    if (direct_hit) {
      int64_t b_bonus = (h.block == 0) ? 2000000 : ((h.block == 1) ? 1000000 : 0);
      return 20000000 + b_bonus + h.freq / 100 - h.depth * 1000;
    }

    return static_cast<int64_t>(h.freq / 100) - static_cast<int64_t>(h.depth) * 10000;
  }

  bool satisfied = true;
  int missing_count = 0;
  for (const auto& c : q.counts) {
    int avail = 0;
    for (uint16_t i = 0; i < atom_n; ++i) {
      if (atoms[i] == c.token) {
        ++avail;
      }
    }
    if (avail < c.count) {
      satisfied = false;
      missing_count += c.count - avail;
    }
  }

  int extra_count = 0;
  for (uint16_t i = 0; i < atom_n; ++i) {
    uint16_t t = atoms[i];
    // 只在每个 token 第一次出现时统计
    bool first = true;
    for (uint16_t j = 0; j < i; ++j) {
      if (atoms[j] == t) {
        first = false;
        break;
      }
    }
    if (!first)
      continue;
    int avail = 0;
    for (uint16_t j = 0; j < atom_n; ++j) {
      if (atoms[j] == t)
        ++avail;
    }
    int req = TokenCountOf(q.counts, t);
    if (avail > req)
      extra_count += avail - req;
  }

  int64_t ordered_bonus = 0;
  bool match_macro = (leaf_n == q.norm.size());
  if (match_macro) {
    for (size_t i = 0; i < q.norm.size(); ++i) {
      if (store.CanonToken(leaves[i]) != q.norm[i]) {
        match_macro = false;
        break;
      }
    }
  }
  bool match_macro_rev = false;
  if (!match_macro && leaf_n == q.norm.size()) {
    match_macro_rev = true;
    size_t n = q.norm.size();
    for (size_t i = 0; i < n; ++i) {
      if (store.CanonToken(leaves[i]) != q.norm[n - i - 1]) {
        match_macro_rev = false;
        break;
      }
    }
  }
  if (match_macro) {
    ordered_bonus = 20000000;
  } else if (match_macro_rev) {
    ordered_bonus = 10000000;
  } else if (q.seq.size() == atom_n) {
    bool seq_match = true;
    for (size_t i = 0; i < q.seq.size(); ++i) {
      if (q.seq[i] != atoms[i]) {
        seq_match = false;
        break;
      }
    }
    if (seq_match)
      ordered_bonus = 15000000;
  }

  int64_t score = 0;
  if (satisfied) {
    if (extra_count == 0)
      score = 50000000 + ordered_bonus;
    else
      score = 20000000 - extra_count * 500000 + ordered_bonus / 2;
  } else {
    score = -missing_count * 5000000 - extra_count * 500000;
  }

  int block = store.Hot(id).block;
  if (block == 0)
    score += 2000000;
  else if (block == 1)
    score += 1000000;

  int raw_match = 0;
  for (uint16_t rt : q.raw) {
    for (uint16_t i = 0; i < leaf_n; ++i) {
      if (leaves[i] == rt) {
        ++raw_match;
        break;
      }
    }
  }
  int paired_match = 0;
  for (uint16_t pt : q.paired) {
    for (uint16_t i = 0; i < leaf_n; ++i) {
      if (leaves[i] == pt) {
        ++paired_match;
        break;
      }
    }
  }
  score += static_cast<int64_t>(raw_match) * 100000;
  score += static_cast<int64_t>(paired_match) * 50000;
  return score;
}

int64_t ScoreHybridStrings(
    IdsStore& store,
    uint32_t id,
    const std::vector<std::string>& normalized,
    const std::map<std::string, int>& user_atomics,
    const std::vector<std::string>& user_atomic_seq,
    hash_map<std::string, std::vector<std::string>>* cache) {
  uint32_t cp = store.CpOfId(id);
  std::string ch = CpToUtf8(cp);
  std::vector<std::string> cand_leaves = TopLeavesById(store, id);
  std::vector<std::string> cand_atomic_list =
      AtomicDecomposition(store, ch, cache, 0);

  std::map<std::string, int> cand_atomics;
  for (const auto& a : cand_atomic_list)
    AddCount(&cand_atomics, a);

  bool satisfied = true;
  int missing_count = 0;
  for (const auto& kv : user_atomics) {
    auto it = cand_atomics.find(kv.first);
    int avail = it == cand_atomics.end() ? 0 : it->second;
    if (avail < kv.second) {
      satisfied = false;
      missing_count += kv.second - avail;
    }
  }

  int extra_count = 0;
  for (const auto& kv : cand_atomics) {
    auto it = user_atomics.find(kv.first);
    int req = it == user_atomics.end() ? 0 : it->second;
    if (kv.second > req)
      extra_count += kv.second - req;
  }

  int64_t ordered_bonus = 0;
  bool match_macro = cand_leaves.size() == normalized.size();
  if (match_macro) {
    for (size_t i = 0; i < normalized.size(); ++i) {
      if (cand_leaves[i] != normalized[i]) {
        match_macro = false;
        break;
      }
    }
  }
  bool match_macro_rev = false;
  if (!match_macro && cand_leaves.size() == normalized.size()) {
    match_macro_rev = true;
    size_t n = normalized.size();
    for (size_t i = 0; i < n; ++i) {
      if (cand_leaves[i] != normalized[n - i - 1]) {
        match_macro_rev = false;
        break;
      }
    }
  }
  if (match_macro) {
    ordered_bonus = 20000000;
  } else if (match_macro_rev) {
    ordered_bonus = 10000000;
  } else if (user_atomic_seq.size() == cand_atomic_list.size()) {
    bool seq_match = true;
    for (size_t i = 0; i < user_atomic_seq.size(); ++i) {
      if (user_atomic_seq[i] != cand_atomic_list[i]) {
        seq_match = false;
        break;
      }
    }
    if (seq_match)
      ordered_bonus = 15000000;
  }

  int64_t score = 0;
  if (satisfied) {
    if (extra_count == 0)
      score = 50000000 + ordered_bonus;
    else
      score = 20000000 - extra_count * 500000 + ordered_bonus / 2;
  } else {
    score = -missing_count * 5000000 - extra_count * 500000;
  }

  int block = store.Hot(id).block;
  if (block == 0)
    score += 2000000;
  else if (block == 1)
    score += 1000000;
  return score;
}

std::vector<uint32_t> RankHybridStrings(
    IdsStore& store,
    std::vector<uint32_t> cands,
    const std::vector<std::string>& normalized,
    const std::vector<std::string>& raw,
    const hash_set<uint32_t>* exact_ids,
    size_t score_limit) {
  std::map<std::string, int> user_atomics;
  std::vector<std::string> user_atomic_seq;
  hash_map<std::string, std::vector<std::string>> cache;
  std::vector<std::string> norm_input;
  for (const auto& item : normalized) {
    std::string c = Canon(item);
    norm_input.push_back(c);
    std::vector<std::string> atomics = AtomicDecomposition(store, c, &cache, 0);
    for (const auto& a : atomics) {
      AddCount(&user_atomics, a);
      user_atomic_seq.push_back(a);
    }
  }

  LiteralQuery literal;
  BuildLiteralQuery(store, raw, &literal);
  LiteralNodeCache literal_cache;

  size_t limit = std::min<size_t>(cands.size(), score_limit);
  struct Scored {
    uint32_t id;
    int64_t score;
    int literal;
    uint32_t freq;
    size_t index;
  };
  std::vector<Scored> scored;
  scored.reserve(limit);
  for (size_t i = 0; i < limit; ++i) {
    int64_t sc = ScoreHybridStrings(store, cands[i], norm_input, user_atomics,
                                    user_atomic_seq, &cache);
    if (exact_ids && exact_ids->count(cands[i]))
      sc = BoostExactStructureScore(sc);
    int literal_hits =
        literal.needed ? LiteralHits(store, cands[i], literal, &literal_cache)
                       : 0;
    scored.push_back({cands[i], sc, literal_hits, store.Hot(cands[i]).freq, i});
  }
  std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
    if (a.literal != b.literal)
      return a.literal > b.literal;
    if (a.score != b.score)
      return a.score > b.score;
    if (a.freq != b.freq)
      return a.freq > b.freq;
    return a.index < b.index;
  });
  std::vector<uint32_t> out;
  out.reserve(cands.size());
  for (const auto& it : scored)
    out.push_back(it.id);
  for (size_t i = limit; i < cands.size(); ++i)
    out.push_back(cands[i]);
  return out;
}

std::vector<uint32_t> RankHybrid(IdsStore& store,
                                 std::vector<uint32_t> cands,
                                 const std::vector<std::string>& normalized,
                                 const std::vector<std::string>& raw,
                                 const hash_set<uint32_t>* exact_ids,
                                 size_t score_limit) {
  if (cands.size() <= 1 || normalized.empty())
    return cands;

  QueryAtoms q;
  if (!BuildQueryAtoms(store, normalized, &q))
    return RankHybridStrings(store, std::move(cands), normalized, raw,
                             exact_ids, score_limit);

  LiteralQuery literal;
  BuildLiteralQuery(store, raw, &literal);
  LiteralNodeCache literal_cache;

  size_t limit = std::min<size_t>(cands.size(), score_limit);
  struct Scored {
    uint32_t id;
    int64_t score;
    int literal;
    uint32_t freq;
    size_t index;
  };
  std::vector<Scored> scored;
  scored.reserve(limit);
  for (size_t i = 0; i < limit; ++i) {
    int64_t sc = ScoreHybridTokens(store, cands[i], q);
    if (exact_ids && exact_ids->count(cands[i]))
      sc = BoostExactStructureScore(sc);
    int literal_hits =
        literal.needed ? LiteralHits(store, cands[i], literal, &literal_cache)
                       : 0;
    scored.push_back({cands[i], sc, literal_hits, store.Hot(cands[i]).freq, i});
  }
  std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
    if (a.literal != b.literal)
      return a.literal > b.literal;
    if (a.score != b.score)
      return a.score > b.score;
    if (a.freq != b.freq)
      return a.freq > b.freq;
    return a.index < b.index;
  });
  std::vector<uint32_t> out;
  out.reserve(cands.size());
  for (const auto& it : scored)
    out.push_back(it.id);
  for (size_t i = limit; i < cands.size(); ++i)
    out.push_back(cands[i]);
  return out;
}

// ---------------------------------------------------------------------------
// 加减代数与递归扣除
// ---------------------------------------------------------------------------

// 单字部件保留原字形，供递归扣除读取其 IDS 结构；拼音编码仍走常规规范化。
std::string NormalizeAlgebraItem(IdsStore& store,
                                 const std::string& raw,
                                 const InputCodec& codec) {
  std::string s = StripSpace(raw);
  if (Utf8Chars(s).size() == 1)
    return s;
  return NormalizeComponent(s, codec);
}

// 从 base 的深层部件中递归扣除 neg；成功返回剩余叶子，失败返回 nullopt。
std::optional<std::vector<std::string>> SubtractCharacter(
    IdsStore& store,
    const std::string& base,
    const std::string& neg,
    int depth,
    bool from_leaf = false) {
  std::string norm_base = Canon(base);
  std::string norm_neg = Canon(neg);
  if (norm_base == norm_neg)
    return std::vector<std::string>{};
  // 用户输入/当前项按原始字形判断原子性，保留 龍 等传统复合字的可拆分性；
  // 递归进入分解叶子时则按归一化字形判断，避免把 辵/⻍ 等变体继续拆成笔画。
  bool atomic = AtomicComponents().count(base) != 0;
  if (!atomic && from_leaf)
    atomic = AtomicComponents().count(norm_base) != 0;
  if (depth > 3 || atomic)
    return std::nullopt;

  std::vector<std::string> leaves =
      TopLeavesOfComponent(store, base, /*preserve_glyph=*/true);
  if (leaves.size() <= 1 && !leaves.empty() && Canon(leaves[0]) == norm_base)
    return std::nullopt;

  // 1. 直接叶子命中。保留原始字形，但比较使用归一化部件。
  for (size_t i = 0; i < leaves.size(); ++i) {
    if (Canon(leaves[i]) == norm_neg) {
      leaves.erase(leaves.begin() + static_cast<std::ptrdiff_t>(i));
      return leaves;
    }
  }

  // 2. 递归深入复合子部件展开。
  std::vector<std::string> new_leaves;
  bool sub_done = false;
  for (const auto& leaf : leaves) {
    if (!sub_done) {
      auto sub = SubtractCharacter(store, leaf, norm_neg, depth + 1, true);
      if (sub) {
        new_leaves.insert(new_leaves.end(), sub->begin(), sub->end());
        sub_done = true;
        continue;
      }
    }
    new_leaves.push_back(leaf);
  }
  if (sub_done)
    return new_leaves;
  return std::nullopt;
}

std::vector<std::string> ApplySingleSubtraction(
    IdsStore& store,
    const std::vector<std::string>& current,
    const std::string& neg) {
  // 优先从当前已有的复合字符中向下递归抠除。
  for (size_t i = 0; i < current.size(); ++i) {
    auto sub = SubtractCharacter(store, current[i], neg, 0);
    if (!sub)
      continue;
    std::vector<std::string> out;
    out.reserve(current.size() + sub->size());
    out.insert(out.end(), current.begin(),
               current.begin() + static_cast<std::ptrdiff_t>(i));
    out.insert(out.end(), sub->begin(), sub->end());
    out.insert(out.end(), current.begin() + static_cast<std::ptrdiff_t>(i + 1),
               current.end());
    return out;
  }

  // 若无法从字内抠除，尝试从当前部件列表中字面剔除。
  for (size_t i = 0; i < current.size(); ++i) {
    if (Canon(current[i]) == Canon(neg)) {
      std::vector<std::string> out = current;
      out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
      return out;
    }
  }
  return current;
}

std::vector<uint32_t> PureSubtractionResults(
    IdsStore& store,
    const std::vector<std::string>& components) {
  std::vector<uint32_t> results;
  hash_set<std::string> seen_comp;
  hash_set<uint32_t> seen_id;
  for (const auto& c : components) {
    std::string key = Canon(c);
    if (!seen_comp.insert(key).second)
      continue;
    if (Utf8Chars(c).size() != 1)
      continue;
    size_t pos = 0;
    uint32_t cp = DecodeNext(c, &pos);
    uint32_t id = store.IdOfCp(cp);
    if (id == kInvalidId) {
      // 极少数原始字形可能不在字符表中；此时回退到归一化代表字形。
      const std::string canonical = Canon(c);
      if (canonical != c) {
        size_t cpos = 0;
        cp = DecodeNext(canonical, &cpos);
        id = store.IdOfCp(cp);
      }
    }
    if (id != kInvalidId && seen_id.insert(id).second)
      results.push_back(id);
  }
  return results;
}

struct ExcludeEntries {
  std::vector<const KeyEntry*> postings;  // 部件分解中包含排除项的字
  std::vector<uint32_t> self_ids;         // 排除项本身对应的字

  bool empty() const { return postings.empty() && self_ids.empty(); }
};

ExcludeEntries BuildExcludeEntries(IdsStore& store,
                                   const std::vector<std::string>& excludes) {
  ExcludeEntries out;
  hash_set<const KeyEntry*> seen_keys;
  hash_set<uint32_t> seen_ids;
  auto add_id = [&](uint32_t id) {
    if (id != kInvalidId && seen_ids.insert(id).second)
      out.self_ids.push_back(id);
  };
  auto add_term_id = [&](const std::string& term) {
    if (Utf8Chars(term).size() != 1)
      return;
    size_t pos = 0;
    uint32_t cp = DecodeNext(term, &pos);
    if (cp)
      add_id(store.IdOfCp(cp));
  };

  for (const auto& term : excludes) {
    for (const KeyEntry* e : KeysForTerm(store, term, 1)) {
      if (!seen_keys.insert(e).second)
        continue;
      out.postings.push_back(e);
    }

    // 排除项本身也要被排除；倒排只覆盖“包含该部件的字”。
    add_term_id(term);
    std::string canon = Canon(term);
    if (canon != term)
      add_term_id(canon);

    // 拼音排除时，同时排除该拼音对应部件本身的字。
    auto pit = store.py_map().find(term);
    if (pit != store.py_map().end()) {
      for (uint16_t t : pit->second) {
        uint32_t cp = store.TokenCp(t);
        if (cp)
          add_id(store.IdOfCp(cp));
      }
    }
  }
  std::sort(out.self_ids.begin(), out.self_ids.end());
  out.self_ids.erase(std::unique(out.self_ids.begin(), out.self_ids.end()),
                     out.self_ids.end());
  return out;
}

bool ContainsExcluded(IdsStore& store,
                      const ExcludeEntries& entries,
                      uint32_t id) {
  if (std::binary_search(entries.self_ids.begin(), entries.self_ids.end(), id))
    return true;
  for (const KeyEntry* e : entries.postings) {
    if (store.FullContains(*e, id))
      return true;
  }
  return false;
}

void RemoveExcluded(IdsStore& store,
                    const ExcludeEntries& entries,
                    std::vector<uint32_t>* ids) {
  if (!ids || entries.empty())
    return;
  ids->erase(std::remove_if(ids->begin(), ids->end(),
                            [&](uint32_t id) {
                              return ContainsExcluded(store, entries, id);
                            }),
             ids->end());
}

// 按部件叶子列表检索“包含这些部件”的字。
// 普通多项检索与纯减法后的扩展检索共用，保证两者排序/过滤一致。
std::vector<uint32_t> SearchByLeaves(IdsStore& store,
                                     const std::vector<std::string>& leaves,
                                     const InputCodec& codec,
                                     size_t max_results,
                                     const rime::hash_set<int>* disabled_blocks,
                                     const ExcludeEntries& exclude_entries) {
  std::vector<std::string> normalized;
  for (const auto& raw : leaves) {
    std::string n = NormalizeComponent(raw, codec);
    if (!n.empty())
      normalized.push_back(n);
  }
  if (normalized.empty())
    return {};

  std::vector<std::string> order;
  std::vector<int> counts;
  order.reserve(normalized.size());
  counts.reserve(normalized.size());
  for (const auto& n : normalized) {
    bool found = false;
    for (size_t i = 0; i < order.size(); ++i) {
      if (order[i] == n) {
        counts[i] += 1;
        found = true;
        break;
      }
    }
    if (!found) {
      order.push_back(n);
      counts.push_back(1);
    }
  }

  std::vector<uint32_t> cands;
  if (order.size() == 1) {
    // 单 term 只需字频头部；完整倒排可能有十万级 id，且首个 500 候选本应按
    // 字频选取，直接用 HEAD 既更快也更符合排序目标。
    std::vector<const KeyEntry*> entries =
        KeysForTerm(store, order[0], counts[0]);
    if (entries.size() == 1) {
      const KeyEntry* e = entries[0];
      const uint32_t* head = store.HeadIds(*e);
      cands.assign(head, head + e->head_cnt);
    } else {
      hash_set<uint32_t> seen;
      for (const KeyEntry* e : entries) {
        const uint32_t* head = store.HeadIds(*e);
        for (uint32_t i = 0; i < e->head_cnt; ++i) {
          if (seen.insert(head[i]).second)
            cands.push_back(head[i]);
        }
      }
    }
  } else {
    cands = IntersectPostings(store, order, counts);
  }
  if (disabled_blocks) {
    cands.erase(std::remove_if(cands.begin(), cands.end(),
                               [&](uint32_t id) {
                                 return disabled_blocks->count(static_cast<int>(
                                            store.Hot(id).block)) > 0;
                               }),
                cands.end());
  }
  RemoveExcluded(store, exclude_entries, &cands);
  if (order.size() >= 2 && cands.size() > 500) {
    // IntersectPostings 按 id（Unicode 码位）升序返回，扩A（0x3400..0x4DBF）
    // 会排在基本区（0x4E00..0x9FFF）之前；先按字频截取前 500，避免高频基本字
    // 被 score_limit = 500 截掉。
    std::partial_sort(cands.begin(), cands.begin() + 500, cands.end(),
                      [&](uint32_t a, uint32_t b) {
                        uint32_t fa = store.Hot(a).freq;
                        uint32_t fb = store.Hot(b).freq;
                        if (fa != fb)
                          return fa > fb;
                        return a < b;
                      });
    cands.resize(500);
  }

  // 顶层叶子多重集精确匹配：不依赖字频 head，也不受普通候选打分上限影响。
  std::vector<uint16_t> struct_tokens;
  bool struct_ok = true;
  for (const auto& n : normalized) {
    uint16_t t = store.ResolveTerm(n);
    if (t == kNoToken) {
      struct_ok = false;
      break;
    }
    struct_tokens.push_back(t);
  }
  std::vector<uint32_t> exact;
  if (struct_ok)
    exact = store.LookupLeafStructure(struct_tokens);
  if (disabled_blocks) {
    exact.erase(std::remove_if(exact.begin(), exact.end(),
                               [&](uint32_t id) {
                                 return disabled_blocks->count(static_cast<int>(
                                            store.Hot(id).block)) > 0;
                               }),
                exact.end());
  }
  RemoveExcluded(store, exclude_entries, &exact);

  hash_set<uint32_t> exact_set;
  exact_set.reserve(exact.size());
  std::vector<uint32_t> merged;
  merged.reserve(exact.size() + cands.size());
  for (uint32_t id : exact) {
    if (exact_set.insert(id).second)
      merged.push_back(id);
  }
  hash_set<uint32_t> seen = exact_set;
  for (uint32_t id : cands) {
    if (seen.insert(id).second)
      merged.push_back(id);
  }
  cands.swap(merged);

  size_t score_limit = 500;
  if (!exact_set.empty())
    score_limit = std::min<size_t>(cands.size(), 500 + exact_set.size());
  cands = RankHybrid(store, std::move(cands), normalized, leaves,
                     exact_set.empty() ? nullptr : &exact_set, score_limit);
  if (cands.size() > max_results)
    cands.resize(max_results);
  return cands;
}

}  // namespace

namespace {

struct RpnValue {
  std::vector<std::string> components;
  std::vector<std::string> excludes;
  bool has_sub = false;
  bool plus_after_sub = false;
};

struct RpnEvaluation {
  std::vector<std::string> components;
  std::vector<std::string> excludes;
};

bool EvaluateRpnLayers(IdsStore& store,
                       const std::vector<std::string>& layers,
                       const InputCodec& codec,
                       RpnEvaluation* out,
                       bool* pure_sub,
                       bool* has_sub);

bool IsOperandSeparator(char c) {
  return c == '\'' || c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
         c == '.' || c == '_';
}

// 把一个状态层展开成一个操作数：ASCII 编码归一化为单个部件；汉字串按字符
// 拆分为同一操作数的多个部件；若层内自带后缀运算符，则递归按 RPN 求值。
bool ExpandRpnOperand(IdsStore& store,
                      const std::string& raw,
                      const InputCodec& codec,
                      RpnValue* out) {
  if (!out)
    return false;
  std::string s;
  for (char c : StripSpace(raw)) {
    if (!IsOperandSeparator(c))
      s.push_back(c);
  }
  if (s.empty())
    return false;

  if (IsAsciiAlphaOnly(s)) {
    std::string item = NormalizeAlgebraItem(store, s, codec);
    if (item.empty())
      return false;
    out->components.push_back(item);
    return true;
  }

  std::vector<std::string> tokens;
  bool has_operator = false;
  size_t i = 0;
  while (i < s.size()) {
    char c = s[i];
    if (c == '+' || c == '-' || c == '!') {
      tokens.emplace_back(1, c);
      has_operator = true;
      ++i;
      continue;
    }
    if (static_cast<unsigned char>(c) < 0x80)
      return false;  // 括号、数字等非部件 ASCII 不再属于表达式。
    size_t start = i;
    uint32_t cp = DecodeNext(s, &i);
    if (i == start || cp == 0)
      return false;
    tokens.push_back(s.substr(start, i - start));
  }
  if (tokens.empty())
    return false;

  if (!has_operator) {
    out->components = std::move(tokens);
    return true;
  }

  RpnEvaluation inner;
  bool inner_pure_sub = false;
  bool inner_has_sub = false;
  if (!EvaluateRpnLayers(store, tokens, codec, &inner, &inner_pure_sub,
                         &inner_has_sub))
    return false;
  out->components = std::move(inner.components);
  out->excludes = std::move(inner.excludes);
  out->has_sub = inner_has_sub;
  out->plus_after_sub = inner_has_sub && !inner_pure_sub;
  return true;
}

// 逆波兰后缀求值。运算符栈不足时返回 false。
bool EvaluateRpnLayers(IdsStore& store,
                       const std::vector<std::string>& tokens,
                       const InputCodec& codec,
                       RpnEvaluation* out,
                       bool* pure_sub,
                       bool* has_sub) {
  if (!out || !pure_sub || !has_sub)
    return false;

  std::vector<RpnValue> stack;
  std::vector<std::string> global_excludes;

  for (const auto& token : tokens) {
    if (token == "+" || token == "-") {
      if (stack.size() < 2)
        return false;
      RpnValue b = std::move(stack.back());
      stack.pop_back();
      RpnValue a = std::move(stack.back());
      stack.pop_back();
      if (token == "+") {
        if (a.has_sub || b.has_sub)
          a.plus_after_sub = true;
        a.has_sub = a.has_sub || b.has_sub;
        a.plus_after_sub = a.plus_after_sub || b.plus_after_sub;
        a.components.insert(a.components.end(), b.components.begin(),
                            b.components.end());
        a.excludes.insert(a.excludes.end(), b.excludes.begin(),
                          b.excludes.end());
      } else {
        for (const auto& item : b.components)
          a.components = ApplySingleSubtraction(store, a.components, item);
        a.has_sub = true;
        a.plus_after_sub = a.plus_after_sub || b.plus_after_sub;
        a.excludes.insert(a.excludes.end(), b.excludes.begin(),
                          b.excludes.end());
      }
      stack.push_back(std::move(a));
    } else if (token == "!") {
      if (stack.empty())
        return false;
      RpnValue value = std::move(stack.back());
      stack.pop_back();
      global_excludes.insert(global_excludes.end(), value.components.begin(),
                             value.components.end());
      global_excludes.insert(global_excludes.end(), value.excludes.begin(),
                             value.excludes.end());
    } else {
      RpnValue item;
      if (!ExpandRpnOperand(store, token, codec, &item))
        return false;
      stack.push_back(std::move(item));
    }
  }

  bool result_has_sub = false;
  bool result_plus_after_sub = false;
  out->components.clear();
  for (const auto& value : stack) {
    global_excludes.insert(global_excludes.end(), value.excludes.begin(),
                           value.excludes.end());
    result_has_sub = result_has_sub || value.has_sub;
    result_plus_after_sub = result_plus_after_sub || value.plus_after_sub;
  }

  // 结尾是减法且栈内仍有多个操作数时，候选显示栈顶（刚算出的差值），
  // 而不是把互不相容的栈值按“同时包含”求交导致候选为空。
  const bool last_is_sub = !tokens.empty() && tokens.back() == "-";
  if (last_is_sub && stack.size() > 1) {
    const RpnValue& top = stack.back();
    out->components = top.components;
    *has_sub = top.has_sub;
    *pure_sub = top.has_sub && !top.plus_after_sub;
  } else {
    for (const auto& value : stack) {
      out->components.insert(out->components.end(), value.components.begin(),
                             value.components.end());
    }
    *has_sub = result_has_sub;
    *pure_sub = (stack.size() == 1 && result_has_sub && !result_plus_after_sub);
  }
  out->excludes = std::move(global_excludes);
  return true;
}

}  // namespace

std::vector<uint32_t> SearchHybrid(IdsStore& store,
                                   const std::vector<std::string>& components,
                                   const InputCodec& codec,
                                   size_t max_results,
                                   const rime::hash_set<int>* disabled_blocks) {
  if (components.empty())
    return {};

  RpnEvaluation eval;
  bool pure_sub = false;
  bool has_sub = false;
  if (!EvaluateRpnLayers(store, components, codec, &eval, &pure_sub, &has_sub))
    return {};
  std::vector<std::string> current = std::move(eval.components);
  std::vector<std::string> excludes = std::move(eval.excludes);

  ExcludeEntries exclude_entries = BuildExcludeEntries(store, excludes);

  // 纯减法：先给出剩余部件本身，再同步检索包含这些部件的字。
  // 例如 O达大-：首选 辶，随后列出 边/达/近/连/道 等。
  if (pure_sub && has_sub && !current.empty()) {
    std::vector<uint32_t> results = PureSubtractionResults(store, current);
    RemoveExcluded(store, exclude_entries, &results);
    std::vector<uint32_t> expanded = SearchByLeaves(
        store, current, codec, max_results, disabled_blocks, exclude_entries);

    hash_set<uint32_t> seen;
    seen.reserve(results.size() + expanded.size());
    std::vector<uint32_t> merged;
    merged.reserve(
        std::min<size_t>(max_results, results.size() + expanded.size()));
    for (uint32_t id : results) {
      if (merged.size() >= max_results)
        break;
      if (seen.insert(id).second)
        merged.push_back(id);
    }
    for (uint32_t id : expanded) {
      if (merged.size() >= max_results)
        break;
      if (seen.insert(id).second)
        merged.push_back(id);
    }
    return merged;
  }
  if (current.empty())
    return {};

  return SearchByLeaves(store, current, codec, max_results, disabled_blocks,
                        exclude_entries);
}

std::vector<uint32_t> GetPinyinCandidates(IdsStore& store,
                                          const std::string& pinyin,
                                          size_t limit) {
  const auto& py_char_map = store.py_char_map();
  if (!py_char_map.empty()) {
    auto it = py_char_map.find(pinyin);
    if (it == py_char_map.end())
      return {};
    if (limit > 0 && it->second.size() > limit) {
      std::vector<uint32_t> ids(it->second.begin(), it->second.begin() + limit);
      return ids;
    }
    return it->second;
  }

  // 兼容旧数据（理论上 v3 不允许缺少 PY_CHAR）。
  auto it = store.py_map().find(pinyin);
  if (it == store.py_map().end())
    return {};
  std::vector<uint32_t> ids;
  hash_set<uint32_t> seen;
  for (uint16_t t : it->second) {
    uint32_t cp = store.TokenCp(t);
    uint32_t id = cp ? store.IdOfCp(cp) : kInvalidId;
    if (id != kInvalidId && seen.insert(id).second)
      ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) {
    uint32_t fa = store.Hot(a).freq;
    uint32_t fb = store.Hot(b).freq;
    if (fa != fb)
      return fa > fb;
    return a < b;
  });
  if (limit > 0 && ids.size() > limit)
    ids.resize(limit);
  return ids;
}

std::vector<uint32_t> GetComponentCandidates(IdsStore& store,
                                             const std::string& raw_code,
                                             const InputCodec& codec,
                                             size_t limit) {
  std::string code = StripSpace(raw_code);
  code.erase(
      std::remove_if(
          code.begin(), code.end(),
          [](char c) { return c == '\'' || c == '/' || c == '.' || c == '_'; }),
      code.end());
  while (!code.empty() &&
         (code.front() == '-' || code.front() == '+' || code.front() == '!'))
    code.erase(code.begin());
  if (code.empty())
    return {};
  code = AsciiLower(code);
  if (!IsAsciiAlphaOnly(code))
    return {};

  // 完整编码优先；否则按编码前缀展开（增量输入）。
  std::vector<std::string> lookups = codec.DecodeExact(code);
  bool exact = !lookups.empty();
  if (!exact)
    lookups = codec.DecodePrefix(code);
  if (lookups.empty())
    return {};
  bool unbounded = exact && lookups.size() == 1;

  const auto& py_char_map = store.py_char_map();
  std::vector<uint32_t> ids;
  hash_set<uint32_t> seen;

  auto add_id = [&](uint32_t id) {
    if (id != kInvalidId && seen.insert(id).second)
      ids.push_back(id);
  };

  if (!py_char_map.empty()) {
    for (const auto& lookup : lookups) {
      auto it = py_char_map.find(lookup);
      if (it == py_char_map.end())
        continue;
      for (uint32_t id : it->second)
        add_id(id);
    }
  } else {
    // 兼容旧数据（理论上 v3 不允许缺少 PY_CHAR）。
    for (const auto& lookup : lookups) {
      auto it = store.py_map().find(lookup);
      if (it == store.py_map().end())
        continue;
      for (uint16_t t : it->second) {
        uint32_t cp = store.TokenCp(t);
        if (cp)
          add_id(store.IdOfCp(cp));
      }
    }
  }

  if (ids.empty())
    return {};

  std::sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) {
    uint32_t fa = store.Hot(a).freq;
    uint32_t fb = store.Hot(b).freq;
    if (fa != fb)
      return fa > fb;
    return a < b;
  });
  if (!unbounded && limit > 0 && ids.size() > limit)
    ids.resize(limit);
  return ids;
}
}  // namespace ids_reverse
