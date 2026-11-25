//
// ids_reverse_config.h — ids_reverse 统一配置与混合检字状态。
//
#ifndef RIME_IDS_REVERSE_IDS_REVERSE_CONFIG_H_
#define RIME_IDS_REVERSE_IDS_REVERSE_CONFIG_H_

#include <string>
#include <vector>

#include <rime/common.h>

namespace rime {
class Context;
struct Ticket;
}  // namespace rime

namespace ids_reverse {

// 单个检字模式的配置：仅 tag / prefix / tips 按模式区分。
struct ReverseModeConfig {
  std::string prefix;
  std::string tag;
  std::string tips;
};

// ids_reverse 命名空间下的全部配置。
struct ReverseConfig {
  ReverseModeConfig search{"I", "ids_reverse_search", "〔部件检字〕"};
  ReverseModeConfig hybrid{"O", "ids_reverse_hybrid", "〔汉字部件检字〕"};

  std::string data_path = "ids_data.bin";
  std::string custom_path;
  // 输入编码使用 Rime 拼写代数（spelling algebra）规则。
  // 留空表示无规则：输入码就是全拼本身。
  std::vector<std::string> algebra;
  // O 模式部件代数表示法：infix（中缀，默认）或 rpn（逆波兰后缀）。
  std::string algebra_notation = "infix";
  std::string ids_format = "unicode";
  std::string variant_trigger = "/";
  std::string variant_replace_key = "Shift+V";
  int max_results = 60;
  bool show_pinyin = true;
  bool show_ids = true;
  bool show_unicode = true;
  bool show_block = true;
  bool has_sw_pinyin = false;
  bool has_sw_ids = false;
  bool has_sw_unicode = false;
  bool has_sw_block = false;
  rime::hash_set<int> disabled_blocks;
  bool disabled_blocks_enabled = false;
};

inline bool IsRpnNotation(const ReverseConfig& config) {
  return config.algebra_notation == "rpn";
}

// 混合检字过程中保存在 Context property 中的状态。
constexpr char kHybridStateProperty[] = "ids_reverse_components";
// 异体字查询的基准字（/ 触发时由 processor 写入，translator 读取）。
constexpr char kVariantsProperty[] = "ids_reverse_variants";
// 结果替换后的异体字（Shift+V 替换结果时写入，作为当前结果候选）。
constexpr char kVariantsResultProperty[] = "ids_reverse_variants_result";
// 混合检字结果段使用的内部标签（与可配置 tag 区分）。
constexpr char kResultSegmentTag[] = "ids_reverse_result";

bool StartsWith(const std::string& s, const std::string& prefix);
bool EndsWith(const std::string& s, const std::string& suffix);

std::string DefaultReverseNamespace(const rime::Ticket& ticket);

// 从 <name_space>/ 读取 ids_reverse 统一配置。
void LoadReverseConfig(const rime::Ticket& ticket,
                       std::string* name_space,
                       ReverseConfig* config);

// 当前输入是否以指定前缀开头且未处于 ascii 模式。
bool IsReverseActive(rime::Context* ctx, const std::string& prefix);

struct HybridState {
  std::vector<std::string> components;
  std::vector<std::string> codes;
  std::string pending;
};

HybridState ReadHybridState(rime::Context* ctx, const std::string& prefix);
void SaveHybridState(rime::Context* ctx,
                     const std::vector<std::string>& components,
                     const std::vector<std::string>& codes);

// 按统一配置加载 IDS4 数据，并把 custom 文件中的禁用分区合并到配置。
bool EnsureIdsStoreLoaded(const ReverseConfig& config);
void MergeCustomDisabledBlocks(rime::hash_set<int>* disabled, bool* enabled);

}  // namespace ids_reverse

#endif  // RIME_IDS_REVERSE_IDS_REVERSE_CONFIG_H_
