//
// ids_reverse_input.cc — Rime 拼写代数驱动的输入编码层。
//
#include "ids_reverse_input.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <utility>

#include <rime/algo/algebra.h>
#include <rime/common.h>
#include <rime/config.h>

#include "ids_reverse_config.h"
#include "ids_store.h"

namespace ids_reverse {

namespace {

using rime::hash_set;

std::string AsciiLower(const std::string& s) {
  std::string r = s;
  for (char& ch : r) {
    if (ch >= 'A' && ch <= 'Z')
      ch = static_cast<char>(ch + 32);
  }
  return r;
}

bool HasSeparator(const std::string& s) {
  for (unsigned char c : s) {
    if (c == '\'' || c == '/' || c == '.' || c == '_' || c == ' ' ||
        c == '\t' || c == '\r' || c == '\n')
      return true;
  }
  return false;
}

std::vector<std::string> SplitSeparator(const std::string& s) {
  std::vector<std::string> parts;
  std::string cur;
  for (unsigned char c : s) {
    if (c == '\'' || c == '/' || c == '.' || c == '_' || c == ' ' ||
        c == '\t' || c == '\r' || c == '\n') {
      if (!cur.empty()) {
        parts.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(static_cast<char>(c));
    }
  }
  if (!cur.empty())
    parts.push_back(cur);
  return parts;
}

double TypePenalty(rime::SpellingType type) {
  switch (type) {
    case rime::kNormalSpelling:
      return 0.0;
    case rime::kFuzzySpelling:
      return 6.0;
    case rime::kAbbreviation:
      return 12.0;
    case rime::kCompletion:
      return 18.0;
    case rime::kAmbiguousSpelling:
      return 24.0;
    default:
      return 48.0;
  }
}

bool EntryBetter(const InputCodec::Entry& a, const InputCodec::Entry& b) {
  if (a.type != b.type)
    return a.type < b.type;
  if (a.credibility != b.credibility)
    return a.credibility > b.credibility;
  return a.pinyin < b.pinyin;
}

// 一个编码对应多个拼音时，只在存在唯一普通拼写时直接采用；否则保留编码原文，
// 交给后续前缀匹配处理（例如简拼 "m" 应展开为所有 m 音节）。
std::string PreferredToken(const std::string& code,
                           const std::vector<InputCodec::Entry>& entries) {
  const InputCodec::Entry* normal = nullptr;
  int normal_count = 0;
  for (const auto& e : entries) {
    if (e.type == rime::kNormalSpelling) {
      ++normal_count;
      normal = &e;
    }
  }
  if (normal_count == 1)
    return normal->pinyin;
  if (entries.size() == 1)
    return entries.front().pinyin;
  return code;
}

uint32_t Fingerprint(const std::vector<std::string>& algebra,
                     const std::vector<std::string>& syllables,
                     uint32_t num_chars) {
  uint32_t h = 2166136261u;
  auto feed = [&](const std::string& s) {
    for (unsigned char c : s) {
      h ^= c;
      h *= 16777619u;
    }
    h ^= 0x1F;
    h *= 16777619u;
  };
  for (const auto& rule : algebra)
    feed(rule);
  for (const auto& syllable : syllables)
    feed(syllable);
  feed(std::to_string(num_chars));
  return h ? h : 1;
}

}  // namespace

std::shared_ptr<const InputCodec> InputCodec::Create(
    const std::vector<std::string>& syllables,
    const std::vector<std::string>& algebra) {
  std::shared_ptr<InputCodec> codec(new InputCodec());
  codec->Build(syllables, algebra);
  return codec;
}

std::shared_ptr<const InputCodec> InputCodec::Get(IdsStore& store,
                                                  const ReverseConfig& config) {
  static std::mutex mutex;
  static std::map<uint32_t, std::shared_ptr<const InputCodec>> cache;

  std::vector<std::string> syllables(store.valid_pinyins().begin(),
                                     store.valid_pinyins().end());
  std::sort(syllables.begin(), syllables.end());
  uint32_t id = Fingerprint(config.algebra, syllables, store.num_chars());

  std::lock_guard<std::mutex> lock(mutex);
  auto it = cache.find(id);
  if (it != cache.end())
    return it->second;
  auto codec = Create(syllables, config.algebra);
  cache[id] = codec;
  return codec;
}

bool InputCodec::HasCodePrefix(const std::string& prefix) const {
  if (prefix.empty())
    return false;
  auto it = std::lower_bound(codes_.begin(), codes_.end(), prefix);
  return it != codes_.end() && it->size() >= prefix.size() &&
         it->compare(0, prefix.size(), prefix) == 0;
}

void InputCodec::Build(const std::vector<std::string>& syllables,
                       const std::vector<std::string>& algebra) {
  entries_.clear();
  codes_.clear();
  max_code_len_ = 0;
  id_ = Fingerprint(algebra, syllables, 0);

  rime::Script script;
  for (const auto& py : syllables) {
    if (!py.empty())
      script.AddSyllable(py);
  }

  if (!algebra.empty()) {
    auto list = rime::New<rime::ConfigList>();
    for (const auto& rule : algebra)
      list->Append(rime::New<rime::ConfigValue>(rule));
    rime::Projection projection;
    if (projection.Load(list)) {
      projection.Apply(&script);
    } else {
      LOG(ERROR) << "ids_reverse: invalid spelling algebra; "
                    "falling back to full pinyin.";
    }
  }

  for (const auto& kv : script) {
    if (kv.first.empty())
      continue;
    std::map<std::string, Entry> best;
    for (const auto& spelling : kv.second) {
      if (spelling.str.empty())
        continue;
      Entry entry{spelling.str, spelling.properties.type,
                  spelling.properties.credibility};
      auto it = best.find(entry.pinyin);
      if (it == best.end() || EntryBetter(entry, it->second))
        best[entry.pinyin] = std::move(entry);
    }
    if (best.empty())
      continue;
    std::vector<Entry>& out = entries_[kv.first];
    out.reserve(best.size());
    for (auto& item : best)
      out.push_back(std::move(item.second));
    std::sort(out.begin(), out.end(), EntryBetter);
    codes_.push_back(kv.first);
    max_code_len_ = std::max(max_code_len_, kv.first.size());
  }
}

std::vector<std::string> InputCodec::Segment(const std::string& input) const {
  if (input.empty())
    return {};

  auto cached = segment_cache_.find(input);
  if (cached != segment_cache_.end())
    return cached->second;

  std::vector<std::string> result;
  if (HasSeparator(input)) {
    for (const auto& part : SplitSeparator(input)) {
      std::vector<std::string> sub = Segment(part);
      result.insert(result.end(), sub.begin(), sub.end());
    }
  } else {
    result = SegmentChunk(AsciiLower(input));
  }

  if (segment_cache_.size() > 4096)
    segment_cache_.clear();
  segment_cache_[input] = result;
  return result;
}

std::vector<std::string> InputCodec::SegmentChunk(
    const std::string& chunk) const {
  size_t n = chunk.size();
  if (n == 0)
    return {};

  // 整段输入必须是「某个编码序列的前缀」，否则严格按代数拒绝。
  // 例如 flypy 下 "kou" 不是任何编码序列的前缀，应返回空。
  std::vector<char> prefix_ok(n + 1, 0);
  prefix_ok[n] = 1;
  for (size_t i = n; i-- > 0;) {
    size_t max_len = std::min(max_code_len_, n - i);
    for (size_t len = 1; len <= max_len; ++len) {
      auto it = entries_.find(chunk.substr(i, len));
      if (it != entries_.end() && prefix_ok[i + len]) {
        prefix_ok[i] = 1;
        break;
      }
    }
    if (!prefix_ok[i] && HasCodePrefix(chunk.substr(i)))
      prefix_ok[i] = 1;
  }
  if (!prefix_ok[0])
    return {};

  struct Node {
    double cost = 1e18;
    size_t next = 0;
    std::string token;
    bool set = false;
  };
  std::vector<Node> memo(n + 1);
  memo[n].set = true;
  memo[n].cost = 0.0;

  const double kRawCost = 1000.0;

  for (size_t i = n; i-- > 0;) {
    Node best;
    // 无法匹配时保留单字符，供后续前缀匹配使用。
    if (memo[i + 1].set) {
      best.cost = kRawCost + memo[i + 1].cost;
      best.next = i + 1;
      best.token = chunk.substr(i, 1);
      best.set = true;
    }

    size_t max_len = std::min(max_code_len_, n - i);
    for (size_t len = max_len; len >= 1; --len) {
      auto it = entries_.find(chunk.substr(i, len));
      if (it == entries_.end())
        continue;
      if (!memo[i + len].set)
        continue;
      const Entry& entry = it->second.front();
      double cost = (10.0 - static_cast<double>(len)) +
                    TypePenalty(entry.type) - entry.credibility +
                    memo[i + len].cost;
      if (!best.set || cost < best.cost) {
        best.cost = cost;
        best.next = i + len;
        best.token = PreferredToken(it->first, it->second);
        best.set = true;
      }
      if (len == 1)
        break;
    }

    // 尾部未完成的编码前缀：整段剩余输入作为一个片段。
    if (memo[n].set && HasCodePrefix(chunk.substr(i))) {
      double cost = 50.0 + memo[n].cost;
      if (!best.set || cost < best.cost) {
        best.cost = cost;
        best.next = n;
        best.token = chunk.substr(i);
        best.set = true;
      }
    }

    if (best.set)
      memo[i] = std::move(best);
  }

  std::vector<std::string> tokens;
  size_t pos = 0;
  while (pos < n && memo[pos].set && memo[pos].next > pos) {
    if (!memo[pos].token.empty())
      tokens.push_back(memo[pos].token);
    pos = memo[pos].next;
  }
  if (pos < n) {
    // 理论上不会发生；保底返回原文。
    tokens.clear();
    tokens.push_back(chunk);
  }
  return tokens;
}

std::vector<std::string> InputCodec::DecodeExact(
    const std::string& code) const {
  auto it = entries_.find(AsciiLower(code));
  if (it == entries_.end())
    return {};
  std::vector<std::string> out;
  out.reserve(it->second.size());
  for (const auto& entry : it->second)
    out.push_back(entry.pinyin);
  return out;
}

std::vector<std::string> InputCodec::DecodePrefix(
    const std::string& prefix) const {
  std::string lower = AsciiLower(prefix);
  std::vector<std::string> out;
  if (lower.empty())
    return out;

  hash_set<std::string> seen;
  size_t matched = 0;
  auto it = std::lower_bound(codes_.begin(), codes_.end(), lower);
  for (; it != codes_.end(); ++it) {
    if (it->size() < lower.size() || it->compare(0, lower.size(), lower) != 0)
      break;
    auto entry_it = entries_.find(*it);
    if (entry_it == entries_.end())
      continue;
    for (const auto& entry : entry_it->second) {
      if (seen.insert(entry.pinyin).second)
        out.push_back(entry.pinyin);
    }
    if (++matched >= 2048)
      break;
  }
  return out;
}

std::string InputCodec::NormalizeTerm(const std::string& term) const {
  auto decoded = DecodeExact(term);
  if (!decoded.empty())
    return decoded.front();
  return term;
}

}  // namespace ids_reverse
