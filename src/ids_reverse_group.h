//
// ids_reverse_group.h — O 模式括号表达式编辑辅助函数。
//
#ifndef RIME_IDS_REVERSE_IDS_REVERSE_GROUP_H_
#define RIME_IDS_REVERSE_IDS_REVERSE_GROUP_H_

#include <cstddef>
#include <string>
#include <vector>

namespace ids_reverse {

// pending 形如 [op]? ( ... )? 时解析为单个括号表达式。
struct PendingGroup {
  bool valid = false;
  bool closed = false;
  char op = 0;
  std::string inner;
};

PendingGroup ParseSingleGroup(const std::string& pending);

// 若 code 是已闭合的单个括号表达式，返回去掉右括号后的文本。
// 供退格重新打开括号编辑，而不是一次删除整个括号层。
bool ReopenClosedGroup(const std::string& code, std::string* reopened);

// 查找最后一个连续 ASCII 字母段；end 为开区间。
bool FindLastAsciiRun(const std::string& s, size_t* start, size_t* end);

std::string ReplaceLastAsciiRun(const std::string& s, const std::string& text);

std::string FormatGroupComponent(char op,
                                 const std::vector<std::string>& components,
                                 const std::vector<std::string>& excludes);

}  // namespace ids_reverse

#endif  // RIME_IDS_REVERSE_IDS_REVERSE_GROUP_H_
