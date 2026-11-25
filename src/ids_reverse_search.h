//
// ids_reverse_search.h — 混合检字（部件/拼音/双拼 + 加减法）。
//
#ifndef RIME_IDS_REVERSE_IDS_REVERSE_SEARCH_H_
#define RIME_IDS_REVERSE_IDS_REVERSE_SEARCH_H_

#include <cstdint>
#include <string>
#include <vector>

#include <rime/common.h>

namespace ids_reverse {

class IdsStore;

// components 为已选部件文本（可能带前导 - / +），按输入顺序排列。
// rpn=true 时 components 中的 "+"/"-"/"!" 视为后缀运算符层。
std::vector<uint32_t> SearchHybrid(IdsStore& store,
                                   const std::vector<std::string>& components,
                                   const std::string& mode,
                                   size_t max_results,
                                   const rime::hash_set<int>* disabled_blocks,
                                   bool rpn = false);

// pending 编码对应的部件单字候选（自包含，不依赖外部拼音翻译器）。
// 完整单音节返回全部字符（不按 limit 截断）；前缀匹配仍受 limit 限制。
std::vector<uint32_t> GetComponentCandidates(IdsStore& store,
                                             const std::string& code,
                                             const std::string& mode,
                                             size_t limit);

// 只计算括号表达式中的部件代数（+ / - / !、嵌套括号），不查词典。
struct AlgebraEvaluation {
  std::vector<std::string> components;
  std::vector<std::string> excludes;
};

bool EvaluateAlgebraExpression(IdsStore& store,
                               const std::string& expression,
                               const std::string& mode,
                               AlgebraEvaluation* out);

}  // namespace ids_reverse

#endif  // RIME_IDS_REVERSE_IDS_REVERSE_SEARCH_H_
