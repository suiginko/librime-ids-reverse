//
// ids_reverse_input.h — 输入编码层（Rime 拼写代数）。
//
// 把 `ids_reverse/algebra` 中的 Rime spelling algebra
// 规则应用到无调全拼音节表， 得到「输入码 →
// 标准拼音」映射；负责切分、精确查询、前缀查询和术语归一化。
//
#ifndef RIME_IDS_REVERSE_IDS_REVERSE_INPUT_H_
#define RIME_IDS_REVERSE_IDS_REVERSE_INPUT_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rime/algo/spelling.h>

namespace ids_reverse {

class IdsStore;
struct ReverseConfig;

class InputCodec {
 public:
  // 按配置构建（进程内缓存）。
  static std::shared_ptr<const InputCodec> Get(IdsStore& store,
                                               const ReverseConfig& config);

  // 直接以音节表和规则列表构建（供 Lua 对等实现参考及测试使用）。
  static std::shared_ptr<const InputCodec> Create(
      const std::vector<std::string>& syllables,
      const std::vector<std::string>& algebra);

  // 把一段输入编码切分为拼音音节或原样片段。
  std::vector<std::string> Segment(const std::string& input) const;

  // 完整编码 → 全部标准拼音（按普通/模糊/简拼优先级排序）。
  std::vector<std::string> DecodeExact(const std::string& code) const;

  // 编码前缀 → 可能的全部标准拼音（用于增量输入的部件候选）。
  std::vector<std::string> DecodePrefix(const std::string& prefix) const;

  // 术语归一化：是完整编码则转标准拼音，否则原样返回。
  std::string NormalizeTerm(const std::string& term) const;

  uint32_t id() const { return id_; }
  size_t size() const { return entries_.size(); }

  struct Entry {
    std::string pinyin;
    rime::SpellingType type = rime::kNormalSpelling;
    double credibility = 0.0;
  };

 private:
  InputCodec() = default;

  void Build(const std::vector<std::string>& syllables,
             const std::vector<std::string>& algebra);
  std::vector<std::string> SegmentChunk(const std::string& chunk) const;
  bool HasCodePrefix(const std::string& prefix) const;

  std::map<std::string, std::vector<Entry>> entries_;
  std::vector<std::string> codes_;  // 有序，供前缀查询
  size_t max_code_len_ = 0;
  uint32_t id_ = 0;

  // Segment 结果缓存；InputCodec 以 shared_ptr<const> 暴露。
  mutable std::map<std::string, std::vector<std::string>> segment_cache_;
};

}  // namespace ids_reverse

#endif  // RIME_IDS_REVERSE_IDS_REVERSE_INPUT_H_
