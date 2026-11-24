//
// ids_reverse_group.cc — O 模式括号表达式编辑辅助函数。
//
#include "ids_reverse_group.h"

namespace ids_reverse {

namespace {

bool IsAsciiLetter(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

}  // namespace

PendingGroup ParseSingleGroup(const std::string& pending) {
  PendingGroup out;
  if (pending.empty())
    return out;
  size_t i = 0;
  char op = 0;
  if (pending[0] == '-' || pending[0] == '+' || pending[0] == '!') {
    op = pending[0];
    i = 1;
  }
  if (i >= pending.size() || pending[i] != '(')
    return out;
  size_t open_pos = i;
  int depth = 0;
  size_t close_pos = std::string::npos;
  for (size_t j = i; j < pending.size(); ++j) {
    char c = pending[j];
    if (c == '(') {
      ++depth;
    } else if (c == ')') {
      --depth;
      if (depth == 0) {
        close_pos = j;
        break;
      }
    }
  }
  out.valid = true;
  out.op = op;
  if (close_pos != std::string::npos) {
    if (close_pos + 1 != pending.size())
      return PendingGroup();
    out.closed = true;
    out.inner = pending.substr(open_pos + 1, close_pos - open_pos - 1);
  } else {
    out.inner = pending.substr(open_pos + 1);
  }
  return out;
}

bool FindLastAsciiRun(const std::string& s, size_t* start, size_t* end) {
  bool found = false;
  size_t last_start = 0;
  size_t last_end = 0;
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (IsAsciiLetter(c)) {
      size_t run_start = i;
      while (i < s.size() && IsAsciiLetter(static_cast<unsigned char>(s[i])))
        ++i;
      last_start = run_start;
      last_end = i;
      found = true;
    } else {
      ++i;
    }
  }
  if (found) {
    *start = last_start;
    *end = last_end;
  }
  return found;
}

std::string ReplaceLastAsciiRun(const std::string& s, const std::string& text) {
  size_t start = 0, end = 0;
  if (!FindLastAsciiRun(s, &start, &end))
    return s;
  std::string out;
  out.reserve(s.size() - (end - start) + text.size());
  out.append(s, 0, start);
  out += text;
  out.append(s, end, std::string::npos);
  return out;
}

std::string FormatGroupComponent(char op,
                                 const std::vector<std::string>& components,
                                 const std::vector<std::string>& excludes) {
  std::string out;
  for (const auto& c : components)
    out += c;
  for (const auto& e : excludes) {
    out += '!';
    out += e;
  }
  if (out.empty())
    return out;
  if (op)
    out.insert(out.begin(), op);
  return out;
}

}  // namespace ids_reverse
