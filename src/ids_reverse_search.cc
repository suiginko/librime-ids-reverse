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

// ---------------------------------------------------------------------------
// 双拼解码（只保留插件已支持的 flypy / zrm / quanpin）
// ---------------------------------------------------------------------------

std::string DecodeShuangpin(IdsStore& store,
                            const std::string& raw,
                            const std::string& mode) {
  std::string sp = AsciiLower(raw);
  if (sp.size() != 2 || !IsAsciiAlphaOnly(sp) || mode == "quanpin")
    return sp;

  static const hash_map<std::string, std::string> kZeroVowels = {
      {"aa", "a"},   {"ai", "ai"}, {"an", "an"}, {"ah", "ang"},
      {"ao", "ao"},  {"ee", "e"},  {"ei", "ei"}, {"en", "en"},
      {"eg", "eng"}, {"er", "er"}, {"oo", "o"},  {"ou", "ou"}};
  auto z = kZeroVowels.find(sp);
  if (z != kZeroVowels.end() && (store.valid_pinyins().count(z->second) ||
                                 store.py_map().count(z->second))) {
    return z->second;
  }

  auto init_of = [](char c) -> std::string {
    if (c == 'v')
      return "zh";
    if (c == 'i')
      return "ch";
    if (c == 'u')
      return "sh";
    return std::string(1, c);
  };

  static const hash_map<char, std::vector<std::string>> kFlypyFinals = {
      {'q', {"iu"}},
      {'w', {"ei"}},
      {'r', {"uan"}},
      {'t', {"ue", "ve"}},
      {'y', {"un"}},
      {'p', {"ie"}},
      {'d', {"ai"}},
      {'f', {"en"}},
      {'g', {"eng"}},
      {'h', {"ang"}},
      {'j', {"an"}},
      {'k', {"uai", "ing"}},
      {'l', {"uang", "iang"}},
      {'z', {"ou"}},
      {'x', {"ua", "ia"}},
      {'c', {"ao"}},
      {'v', {"ui", "v"}},
      {'b', {"in"}},
      {'n', {"iao"}},
      {'m', {"ian"}},
      {'s', {"iong", "ong"}},
      {'o', {"uo", "o"}},
      {'u', {"u"}},
      {'i', {"i"}},
      {'a', {"a"}},
      {'e', {"e"}}};

  static const hash_map<char, std::vector<std::string>> kZrmFinals = {
      {'q', {"iu"}},
      {'w', {"ua", "ia"}},
      {'r', {"uan"}},
      {'t', {"ue", "ve"}},
      {'y', {"uai", "ing"}},
      {'p', {"un"}},
      {'d', {"uang", "iang"}},
      {'f', {"en"}},
      {'g', {"eng"}},
      {'h', {"ang"}},
      {'j', {"an"}},
      {'k', {"ao"}},
      {'l', {"ai"}},
      {'z', {"ou"}},
      {'x', {"ia"}},
      {'c', {"ch"}},
      {'v', {"ui", "v"}},
      {'b', {"in"}},
      {'n', {"in"}},
      {'m', {"ian"}},
      {'s', {"iong", "ong"}},
      {'o', {"uo", "o"}},
      {'u', {"u"}},
      {'i', {"i"}},
      {'a', {"a"}},
      {'e', {"e"}}};

  const auto& finals = (mode == "zrm") ? kZrmFinals : kFlypyFinals;
  auto fit = finals.find(sp[1]);
  if (fit == finals.end())
    return sp;
  std::string init = init_of(sp[0]);
  for (const auto& fin : fit->second) {
    std::string py = init + fin;
    if ((init == "j" || init == "q" || init == "x" || init == "y") &&
        fin == "ve")
      py = init + "ue";
    else if ((init == "j" || init == "q" || init == "x" || init == "y") &&
             fin == "v")
      py = init + "u";
    if (store.valid_pinyins().count(py) || store.py_map().count(py))
      return py;
  }
  return sp;
}

std::string NormalizeComponent(IdsStore& store,
                               const std::string& raw,
                               const std::string& mode) {
  std::string s = StripSpace(raw);
  if (s.empty())
    return s;
  std::string lower = AsciiLower(s);
  bool known = store.FindKey(s) != nullptr || store.FindKey(lower) != nullptr ||
               store.py_map().count(s) || store.py_map().count(lower) ||
               store.ResolveTerm(s) != kNoToken;
  if (!known && lower.size() == 2 && IsAsciiAlphaOnly(lower)) {
    std::string decoded = DecodeShuangpin(store, lower, mode);
    if (decoded != lower && (store.valid_pinyins().count(decoded) ||
                             store.py_map().count(decoded))) {
      return Canon(decoded);
    }
  }
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
      "门", "马", "鸟", "鱼", "虫", "犭", "犬", "辶", "宀", "冖", "穴", "竹",
      "⺮", "米", "糸", "纟", "车", "足", "⻊", "雨", "页", "酉", "贝", "走",
      "身", "辛", "辰", "邑", "阝", "阜", "方", "斤", "止", "歹", "殳", "毛",
      "氏", "气", "白", "皮", "皿", "矢", "示", "礻", "禾", "立", "羽", "舟",
      "艮", "见", "角", "谷", "豆", "豕", "豸", "赤", "革", "音", "风", "飞",
      "食", "饣", "首", "香", "骨", "鬼", "黾", "鼎", "鼓", "鼠", "鼻", "齐",
      "齿", "龙", "龟",
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

struct QueryAtoms {
  std::vector<uint16_t> norm;      // 已 Canon 的输入 token
  std::vector<TokenCount> counts;  // 输入原子部件计数
  std::vector<uint16_t> seq;       // 输入原子部件顺序
};

bool BuildQueryAtoms(IdsStore& store,
                     const std::vector<std::string>& normalized,
                     QueryAtoms* q) {
  for (const auto& raw : normalized) {
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
    const hash_set<uint32_t>* exact_ids,
    size_t score_limit) {
  std::map<std::string, int> user_atomics;
  std::vector<std::string> user_atomic_seq;
  hash_map<std::string, std::vector<std::string>> cache;
  std::vector<std::string> norm_input;
  for (const auto& raw : normalized) {
    std::string c = Canon(raw);
    norm_input.push_back(c);
    std::vector<std::string> atomics = AtomicDecomposition(store, c, &cache, 0);
    for (const auto& a : atomics) {
      AddCount(&user_atomics, a);
      user_atomic_seq.push_back(a);
    }
  }

  size_t limit = std::min<size_t>(cands.size(), score_limit);
  struct Scored {
    uint32_t id;
    int64_t score;
    size_t index;
  };
  std::vector<Scored> scored;
  scored.reserve(limit);
  for (size_t i = 0; i < limit; ++i) {
    int64_t sc = ScoreHybridStrings(store, cands[i], norm_input, user_atomics,
                                    user_atomic_seq, &cache);
    if (exact_ids && exact_ids->count(cands[i]))
      sc = BoostExactStructureScore(sc);
    scored.push_back({cands[i], sc, i});
  }
  std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
    if (a.score != b.score)
      return a.score > b.score;
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
                                 const hash_set<uint32_t>* exact_ids,
                                 size_t score_limit) {
  if (cands.size() <= 1 || normalized.empty())
    return cands;

  QueryAtoms q;
  if (!BuildQueryAtoms(store, normalized, &q))
    return RankHybridStrings(store, std::move(cands), normalized, exact_ids,
                             score_limit);

  size_t limit = std::min<size_t>(cands.size(), score_limit);
  struct Scored {
    uint32_t id;
    int64_t score;
    size_t index;
  };
  std::vector<Scored> scored;
  scored.reserve(limit);
  for (size_t i = 0; i < limit; ++i) {
    int64_t sc = ScoreHybridTokens(store, cands[i], q);
    if (exact_ids && exact_ids->count(cands[i]))
      sc = BoostExactStructureScore(sc);
    scored.push_back({cands[i], sc, i});
  }
  std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
    if (a.score != b.score)
      return a.score > b.score;
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

struct AlgebraStep {
  char op = '+';
  std::string item;
};

// 单字部件保留原字形，供递归扣除读取其 IDS 结构；拼音编码仍走常规规范化。
std::string NormalizeAlgebraItem(IdsStore& store,
                                 const std::string& raw,
                                 const std::string& mode) {
  std::string s = StripSpace(raw);
  if (Utf8Chars(s).size() == 1)
    return s;
  return NormalizeComponent(store, s, mode);
}

// 流式解析操作符：支持任意次 + / - / ! 交替；汉字串按字符拆分为独立运算项。
// ! 是一次性排除：处理完一个 ! 项后恢复为正向，避免影响后续新选的部件。
// () 只做分组：括号内不允许运算符，括号前的运算符统一作用于组内所有部件。
std::vector<AlgebraStep> ParseAlgebraSteps(
    const std::vector<std::string>& components) {
  std::vector<AlgebraStep> steps;
  char curr_op = '+';
  std::vector<char> groups;
  for (const auto& raw : components) {
    std::string s = StripSpace(raw);
    // 上一条以 ! 结尾时，新条目默认恢复正向；显式运算符仍可覆盖。
    if (groups.empty() && curr_op == '!' && !s.empty() && s[0] != '+' &&
        s[0] != '-' && s[0] != '!' && s[0] != '(') {
      curr_op = '+';
    }
    size_t i = 0;
    while (i < s.size()) {
      char c = s[i];
      if (c == '(') {
        groups.push_back(curr_op);
        ++i;
      } else if (c == ')') {
        if (!groups.empty()) {
          char outer = groups.back();
          groups.pop_back();
          if (!groups.empty() && groups.back() == '!')
            curr_op = '!';
          else if (outer == '!')
            curr_op = '+';
          else
            curr_op = outer;
        }
        ++i;
      } else if (c == '+' || c == '-' || c == '!') {
        // 括号内不允许运算符；这里防御性忽略，不改变组内默认运算符。
        if (groups.empty())
          curr_op = c;
        ++i;
      } else {
        size_t start = i;
        while (i < s.size() && s[i] != '+' && s[i] != '-' && s[i] != '!' &&
               s[i] != '(' && s[i] != ')')
          ++i;
        std::string item = s.substr(start, i - start);
        if (item.empty())
          continue;
        if (IsAsciiAlphaOnly(item)) {
          steps.push_back({curr_op, item});
        } else {
          for (const auto& ch : Utf8Chars(item))
            steps.push_back({curr_op, ch});
        }
        // ! 在顶层是一次性的；作为括号默认运算符时对整组生效。
        if (curr_op == '!' && groups.empty())
          curr_op = '+';
      }
    }
  }
  return steps;
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
                                     const std::string& mode,
                                     size_t max_results,
                                     const rime::hash_set<int>* disabled_blocks,
                                     const ExcludeEntries& exclude_entries) {
  std::vector<std::string> normalized;
  for (const auto& raw : leaves) {
    std::string n = NormalizeComponent(store, raw, mode);
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
  cands = RankHybrid(store, std::move(cands), normalized,
                     exact_set.empty() ? nullptr : &exact_set, score_limit);
  if (cands.size() > max_results)
    cands.resize(max_results);
  return cands;
}

}  // namespace

namespace {

struct AlgebraEvalState {
  std::vector<std::string> components;
  std::vector<std::string> excludes;
  bool has_sub = false;
};

void ApplyEvalItem(IdsStore& store,
                   AlgebraEvalState* st,
                   char op,
                   const std::string& raw,
                   const std::string& mode) {
  std::string item = NormalizeAlgebraItem(store, raw, mode);
  if (item.empty())
    return;
  if (op == '+') {
    st->components.push_back(item);
  } else if (op == '!') {
    st->excludes.push_back(item);
  } else {
    st->has_sub = true;
    st->components = ApplySingleSubtraction(store, st->components, item);
  }
}

void MergeEvalGroup(IdsStore& store,
                    AlgebraEvalState* st,
                    char op,
                    const AlgebraEvalState& inner) {
  st->has_sub = st->has_sub || inner.has_sub;
  for (const auto& item : inner.components) {
    if (op == '+') {
      st->components.push_back(item);
    } else if (op == '!') {
      st->excludes.push_back(item);
    } else {
      st->has_sub = true;
      st->components = ApplySingleSubtraction(store, st->components, item);
    }
  }
  for (const auto& e : inner.excludes)
    st->excludes.push_back(e);
}

bool IsEvalSeparator(char c) {
  return c == '\'' || c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
         c == '.' || c == '_';
}

size_t EvalAlgebraSeq(IdsStore& store,
                      const std::string& s,
                      size_t i,
                      const std::string& mode,
                      bool stop_at_rparen,
                      AlgebraEvalState* st) {
  char op = '+';
  while (i < s.size()) {
    char c = s[i];
    if (c == ')') {
      if (stop_at_rparen)
        ++i;
      break;
    }
    if (c == '(') {
      AlgebraEvalState inner;
      size_t next = EvalAlgebraSeq(store, s, i + 1, mode, true, &inner);
      MergeEvalGroup(store, st, op, inner);
      if (op == '!')
        op = '+';
      i = next;
      continue;
    }
    if (c == '+' || c == '-' || c == '!') {
      op = c;
      ++i;
      continue;
    }
    if (IsEvalSeparator(c)) {
      ++i;
      continue;
    }
    size_t start = i;
    while (i < s.size() && s[i] != '+' && s[i] != '-' && s[i] != '!' &&
           s[i] != '(' && s[i] != ')' && !IsEvalSeparator(s[i]))
      ++i;
    std::string item = s.substr(start, i - start);
    if (item.empty())
      continue;
    if (IsAsciiAlphaOnly(item)) {
      ApplyEvalItem(store, st, op, item, mode);
      if (op == '!')
        op = '+';
    } else {
      for (const auto& ch : Utf8Chars(item)) {
        ApplyEvalItem(store, st, op, ch, mode);
        if (op == '!')
          op = '+';
      }
    }
  }
  return i;
}

}  // namespace

bool EvaluateAlgebraExpression(IdsStore& store,
                               const std::string& expression,
                               const std::string& mode,
                               AlgebraEvaluation* out) {
  if (!out)
    return false;
  AlgebraEvalState st;
  EvalAlgebraSeq(store, expression, 0, mode, false, &st);
  out->components = std::move(st.components);
  out->excludes = std::move(st.excludes);
  return true;
}

namespace {

struct RpnValue {
  std::vector<std::string> components;
  std::vector<std::string> excludes;
  bool has_sub = false;
  bool plus_after_sub = false;
};

// 逆波兰后缀求值。非运算符层先按中缀表达式解析为一个操作数，因此括号结果、
// 多字组合部件等都能直接入栈。运算符栈不足时返回 false。
bool EvaluateRpnComponents(IdsStore& store,
                           const std::vector<std::string>& tokens,
                           const std::string& mode,
                           AlgebraEvaluation* out,
                           bool* pure_sub,
                           bool* has_sub) {
  if (!out || !pure_sub || !has_sub)
    return false;

  std::vector<RpnValue> stack;
  std::vector<std::string> global_excludes;

  auto push_operand = [&](const std::string& text) {
    AlgebraEvaluation value;
    EvaluateAlgebraExpression(store, text, mode, &value);
    RpnValue item;
    item.components = std::move(value.components);
    item.excludes = std::move(value.excludes);
    stack.push_back(std::move(item));
  };

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
      push_operand(token);
    }
  }

  bool result_has_sub = false;
  bool result_plus_after_sub = false;
  out->components.clear();
  for (const auto& value : stack) {
    out->components.insert(out->components.end(), value.components.begin(),
                           value.components.end());
    global_excludes.insert(global_excludes.end(), value.excludes.begin(),
                           value.excludes.end());
    result_has_sub = result_has_sub || value.has_sub;
    result_plus_after_sub = result_plus_after_sub || value.plus_after_sub;
  }
  out->excludes = std::move(global_excludes);
  *has_sub = result_has_sub;
  *pure_sub = (stack.size() == 1 && result_has_sub && !result_plus_after_sub);
  return true;
}

}  // namespace

std::vector<uint32_t> SearchHybrid(IdsStore& store,
                                   const std::vector<std::string>& components,
                                   const std::string& mode,
                                   size_t max_results,
                                   const rime::hash_set<int>* disabled_blocks,
                                   bool rpn) {
  if (components.empty())
    return {};

  std::vector<std::string> current;
  std::vector<std::string> excludes;
  bool pure_sub = true;
  bool has_sub = false;

  if (rpn) {
    AlgebraEvaluation eval;
    bool rpn_pure_sub = false;
    bool rpn_has_sub = false;
    if (!EvaluateRpnComponents(store, components, mode, &eval, &rpn_pure_sub,
                               &rpn_has_sub))
      return {};
    current = std::move(eval.components);
    excludes = std::move(eval.excludes);
    pure_sub = rpn_pure_sub;
    has_sub = rpn_has_sub;
  } else {
    std::vector<AlgebraStep> steps = ParseAlgebraSteps(components);
    for (const auto& step : steps) {
      std::string item = NormalizeAlgebraItem(store, step.item, mode);
      if (item.empty())
        continue;
      if (step.op == '+') {
        if (!current.empty() && has_sub)
          pure_sub = false;
        current.push_back(item);
      } else if (step.op == '!') {
        excludes.push_back(item);
      } else {
        has_sub = true;
        current = ApplySingleSubtraction(store, current, item);
      }
    }
  }

  ExcludeEntries exclude_entries = BuildExcludeEntries(store, excludes);

  // 纯减法：先给出剩余部件本身，再同步检索包含这些部件的字。
  // 例如 O达-大：首选 辶，随后列出 边/达/近/连/道 等。
  if (pure_sub && has_sub && !current.empty()) {
    std::vector<uint32_t> results = PureSubtractionResults(store, current);
    RemoveExcluded(store, exclude_entries, &results);
    std::vector<uint32_t> expanded = SearchByLeaves(
        store, current, mode, max_results, disabled_blocks, exclude_entries);

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

  return SearchByLeaves(store, current, mode, max_results, disabled_blocks,
                        exclude_entries);
}

std::vector<uint32_t> GetComponentCandidates(IdsStore& store,
                                             const std::string& raw_code,
                                             const std::string& mode,
                                             size_t limit) {
  std::string code = StripSpace(raw_code);
  while (!code.empty() &&
         (code.front() == '-' || code.front() == '+' || code.front() == '!'))
    code.erase(code.begin());
  if (code.empty())
    return {};
  code = AsciiLower(code);
  if (!IsAsciiAlphaOnly(code))
    return {};

  const auto& py_char_map = store.py_char_map();
  std::string lookup = code;
  if (!py_char_map.count(lookup) && lookup.size() == 2) {
    std::string decoded = DecodeShuangpin(store, lookup, mode);
    if (decoded != lookup)
      lookup = decoded;
  }

  // v3 数据：完整字符拼音索引优先。精确单音节不设上限。
  if (!py_char_map.empty()) {
    auto exact = py_char_map.find(lookup);
    if (exact != py_char_map.end())
      return exact->second;
    // 前缀匹配保留上限，避免一次列出上万字。
    std::vector<uint32_t> ids;
    hash_set<uint32_t> seen;
    for (const auto& kv : py_char_map) {
      if (!StartsWith(kv.first, lookup))
        continue;
      for (uint32_t id : kv.second) {
        if (seen.insert(id).second)
          ids.push_back(id);
      }
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

  // 兼容旧数据（理论上 v3 不允许缺少 PY_CHAR）。
  std::vector<uint16_t> tokens;
  auto pit = store.py_map().find(lookup);
  if (pit != store.py_map().end()) {
    tokens = pit->second;
  } else {
    hash_set<uint16_t> seen;
    for (const auto& kv : store.py_map()) {
      if (!StartsWith(kv.first, lookup))
        continue;
      for (uint16_t t : kv.second) {
        if (seen.insert(t).second)
          tokens.push_back(t);
      }
    }
  }

  std::vector<uint32_t> ids;
  hash_set<uint32_t> seen_ids;
  for (uint16_t t : tokens) {
    uint32_t cp = store.TokenCp(t);
    if (!cp)
      continue;
    uint32_t id = store.IdOfCp(cp);
    if (id == kInvalidId || !seen_ids.insert(id).second)
      continue;
    ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) {
    return store.Hot(a).freq > store.Hot(b).freq;
  });
  if (limit > 0 && ids.size() > limit)
    ids.resize(limit);
  return ids;
}

}  // namespace ids_reverse
