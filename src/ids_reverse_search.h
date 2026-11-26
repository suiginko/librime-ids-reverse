//
// ids_reverse_search.h — 混合检字（部件/拼音/输入码 + 加减法）。
//
#ifndef RIME_IDS_REVERSE_IDS_REVERSE_SEARCH_H_
#define RIME_IDS_REVERSE_IDS_REVERSE_SEARCH_H_

#include <cstdint>
#include <string>
#include <vector>

#include <rime/common.h>

namespace ids_reverse {

class IdsStore;
class InputCodec;

// components 为已选部件文本（可能带前导 - / +），按输入顺序排列。
// rpn=true 时 components 中的 "+"/"-"/"!" 视为后缀运算符层。
std::vector<uint32_t> SearchHybrid(IdsStore& store,
                                   const std::vector<std::string>& components,
                                   const InputCodec& codec,
                                   size_t max_results,
                                   const rime::hash_set<int>* disabled_blocks,
                                   bool rpn = false);

// O 模式减法待选：返回最后一个已选部件中可被减去的下级部件。
// 基本部件在前，中间复合部件在后；已去重并按 IDS 展开顺序排列。
// components 为当前状态中的部件层（可能带前导 - / + / !）。
std::vector<std::string> GetSubtractableParts(
    IdsStore& store,
    const std::vector<std::string>& components);

// pending 编码对应的部件单字候选（自包含，不依赖外部拼音翻译器）。
// 完整单音节返回全部字符（不按 limit 截断）；前缀匹配仍受 limit 限制。
std::vector<uint32_t> GetComponentCandidates(IdsStore& store,
                                             const std::string& code,
                                             const InputCodec& codec,
                                             size_t limit);

// 标准拼音对应的部件单字候选（用于已解码的音节，不再当作输入码处理）。
std::vector<uint32_t> GetPinyinCandidates(IdsStore& store,
                                          const std::string& pinyin,
                                          size_t limit);

// 只计算括号表达式中的部件代数（+ / - / !、嵌套括号），不查词典。
struct AlgebraEvaluation {
  std::vector<std::string> components;
  std::vector<std::string> excludes;
};

bool EvaluateAlgebraExpression(IdsStore& store,
                               const std::string& expression,
                               const InputCodec& codec,
                               AlgebraEvaluation* out);

}  // namespace ids_reverse

#endif  // RIME_IDS_REVERSE_IDS_REVERSE_SEARCH_H_
