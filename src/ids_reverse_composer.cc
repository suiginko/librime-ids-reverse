//
// ids_reverse_composer.cc — ids_reverse 原生 processor / segmentor / filter。
//
#include "ids_reverse_composer.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

#include <rime/candidate.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/key_table.h>
#include <rime/menu.h>
#include <rime/schema.h>
#include <rime/segmentation.h>

#include "ids_reverse_config.h"
#include "ids_reverse_group.h"
#include "ids_reverse_search.h"
#include "ids_store.h"
#include "ids_text.h"

namespace ids_reverse {

namespace {

using rime::an;
using rime::Candidate;
using rime::Context;
using rime::New;
using rime::Segment;
using rime::Segmentation;

// 判断单字是否有异体字；数据尚未加载时保持原有行为。
bool HasVariants(const std::string& text) {
  IdsStore& store = IdsStore::Instance();
  if (!store.loaded())
    return true;
  size_t pos = 0;
  uint32_t cp = DecodeNext(text, &pos);
  if (!cp || pos != text.size())
    return false;
  uint32_t id = store.IdOfCp(cp);
  if (id == kInvalidId)
    return false;
  const uint32_t* variants = nullptr;
  uint16_t n = 0;
  store.Variants(id, &variants, &n);
  return n > 0;
}

std::string CurrentPinyinMode(Context* ctx, const ReverseConfig& config) {
  std::string mode = config.shuangpin_scheme;
  if (ctx) {
    if (ctx->get_option("shuangpin_flypy") ||
        ctx->get_option("double_pinyin")) {
      mode = "flypy";
    } else if (ctx->get_option("shuangpin_zrm")) {
      mode = "zrm";
    } else if (ctx->get_option("pinyin_quanpin")) {
      mode = "quanpin";
    }
  }
  return mode;
}

// 括号闭合且内部拼音都已消歧后，求值并作为一个部件加入状态。
bool TryCommitClosedGroup(Context* ctx, const ReverseConfig& config) {
  if (!ctx || !IsReverseActive(ctx, config.hybrid.prefix))
    return false;
  HybridState st = ReadHybridState(ctx, config.hybrid.prefix);
  PendingGroup group = ParseSingleGroup(st.pending);
  if (!group.valid || !group.closed)
    return false;
  size_t run_start = 0, run_end = 0;
  if (FindLastAsciiRun(st.pending, &run_start, &run_end))
    return false;
  if (!EnsureIdsStoreLoaded(config))
    return false;
  IdsStore& store = IdsStore::Instance();
  AlgebraEvaluation eval;
  if (!EvaluateAlgebraExpression(store, group.inner,
                                 CurrentPinyinMode(ctx, config), &eval))
    return false;
  std::string component =
      FormatGroupComponent(group.op, eval.components, eval.excludes);
  const std::string& input = ctx->input();
  std::string base = input.substr(0, input.size() - st.pending.size());
  if (component.empty()) {
    ctx->set_input(base);
    return true;
  }
  st.components.push_back(component);
  st.codes.push_back(st.pending);
  SaveHybridState(ctx, st.components, st.codes);
  ctx->set_input(base + st.pending + "'");
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Processor
// ---------------------------------------------------------------------------

IdsReverseProcessor::IdsReverseProcessor(const rime::Ticket& ticket)
    : Processor(ticket) {
  LoadReverseConfig(ticket, &name_space_, &config_);
  if (!config_.variant_replace_key.empty()) {
    has_variant_replace_key_ =
        variant_replace_key_.Parse(config_.variant_replace_key);
    if (!has_variant_replace_key_) {
      LOG(WARNING) << "ids_reverse: invalid variant_replace_key: '"
                   << config_.variant_replace_key << "'";
    }
  }
  Context* ctx = engine_ ? engine_->context() : nullptr;
  if (ctx) {
    select_before_ = ctx->select_notifier().connect(
        [this](Context* c) { OnSelectBefore(c); }, boost::signals2::at_front);
    select_after_ = ctx->select_notifier().connect(
        [this](Context* c) { OnSelectAfter(c); }, boost::signals2::at_back);
    update_connection_ =
        ctx->update_notifier().connect([this](Context* c) { OnUpdate(c); });
  }
}

IdsReverseProcessor::~IdsReverseProcessor() {
  select_before_.disconnect();
  select_after_.disconnect();
  update_connection_.disconnect();
}

void IdsReverseProcessor::OnSelectBefore(Context* ctx) {
  if (!IsReverseActive(ctx, config_.hybrid.prefix))
    return;
  if (ctx->composition().empty())
    return;
  an<Candidate> cand = ctx->GetSelectedCandidate();
  if (!cand)
    return;
  an<Candidate> genuine = Candidate::GetGenuineCandidate(cand);
  const std::string& type = genuine ? genuine->type() : cand->type();
  has_selection_ = true;
  selected_is_result_ = (type == "ids_reverse" || type == "hanzi_result");

  selected_text_ = cand->text();
  selected_component_ = genuine ? genuine->text() : cand->text();
  // 选择期间临时关闭自动上屏，交给 OnSelectAfter 统一处理。
  saved_auto_commit_ = ctx->get_option("_auto_commit");
  if (saved_auto_commit_)
    ctx->set_option("_auto_commit", false);
}

void IdsReverseProcessor::OnSelectAfter(Context* ctx) {
  if (!has_selection_)
    return;
  has_selection_ = false;
  const bool restore_auto_commit = saved_auto_commit_;
  saved_auto_commit_ = false;

  if (selected_is_result_) {
    engine_->CommitText(selected_text_);
    ctx->set_property(kVariantsProperty, "");
    ctx->set_property(kVariantsResultProperty, "");
    ctx->Clear();
  } else {
    ctx->set_property(kVariantsProperty, "");
    ctx->set_property(kVariantsResultProperty, "");
    HybridState st = ReadHybridState(ctx, config_.hybrid.prefix);
    std::string text = selected_component_;
    PendingGroup group = ParseSingleGroup(st.pending);
    if (group.valid) {
      // 括号内：只把当前拼音段替换成所选汉字，不提交为状态部件。
      size_t run_start = 0, run_end = 0;
      if (!text.empty() && FindLastAsciiRun(st.pending, &run_start, &run_end)) {
        std::string new_pending =
            st.pending.substr(0, run_start) + text + st.pending.substr(run_end);
        std::string base =
            ctx->input().substr(0, ctx->input().size() - st.pending.size());
        ctx->set_input(base + new_pending);
        TryCommitClosedGroup(ctx, config_);
      }
    } else {
      if (!text.empty() && !st.pending.empty() && st.pending[0] == '-' &&
          text[0] != '-') {
        text = "-" + text;
      } else if (!text.empty() && !st.pending.empty() && st.pending[0] == '+' &&
                 text[0] != '+') {
        text = "+" + text;
      } else if (!text.empty() && !st.pending.empty() && st.pending[0] == '!' &&
                 text[0] != '!') {
        text = "!" + text;
      }
      std::string base =
          ctx->input().substr(0, ctx->input().size() - st.pending.size());
      st.components.push_back(text);
      st.codes.push_back(text);
      SaveHybridState(ctx, st.components, st.codes);
      ctx->set_input(base + text + "'");
    }
  }

  if (restore_auto_commit && !ctx->get_option("_auto_commit"))
    ctx->set_option("_auto_commit", true);
}

void IdsReverseProcessor::OnUpdate(Context* ctx) {
  if (!IsReverseActive(ctx, config_.hybrid.prefix))
    return;
  if (!ctx->composition().empty())
    ctx->composition().back().prompt = config_.hybrid.tips;
}

bool IdsReverseProcessor::HandleIdsHotkey(const rime::KeyEvent& key_event,
                                          Context* ctx) {
  if (!ctx || ctx->composition().empty())
    return false;
  if (!IsReverseActive(ctx, config_.hybrid.prefix))
    return false;
  const int code = key_event.keycode();
  const bool ctrl = key_event.ctrl();
  const bool alt = key_event.alt();
  const bool shift = key_event.shift();
  const bool is_return = (code == XK_Return || code == XK_KP_Enter);

  int digit = 0;
  if (code >= XK_1 && code <= XK_6)
    digit = code - XK_0;

  bool action = false;
  int ids_digit = 0;
  if (ctrl && !shift && !alt && is_return)
    action = true;
  else if (alt && !shift && !ctrl && is_return)
    action = true;
  else if (alt && !ctrl && (code == XK_i || code == XK_I))
    action = true;
  else if (ctrl && shift && (code == XK_i || code == XK_I))
    action = true;
  else if (ctrl && !shift && !alt && digit) {
    action = true;
    ids_digit = digit;
  } else if (alt && !shift && !ctrl && digit) {
    action = true;
    ids_digit = digit;
  }
  if (!action)
    return false;

  Segment& seg = ctx->composition().back();
  if (!seg.menu)
    return false;
  size_t index = seg.selected_index;
  if (ids_digit) {
    int page_size = engine_->schema() ? engine_->schema()->page_size() : 5;
    if (page_size <= 0)
      page_size = 5;
    size_t page_start =
        (seg.selected_index / static_cast<size_t>(page_size)) * page_size;
    index = page_start + static_cast<size_t>(ids_digit) - 1;
  }
  seg.menu->Prepare(index + 1);
  an<Candidate> cand = seg.GetCandidateAt(index);
  if (!cand || cand->text().empty())
    return false;
  if (!EnsureIdsStoreLoaded(config_))
    return false;
  IdsStore& store = IdsStore::Instance();
  std::string ids = IdsOfText(store, cand->text());
  if (ids.empty())
    return false;
  engine_->CommitText(ids);
  ctx->Clear();
  return true;
}

bool IdsReverseProcessor::HandleSelectionKey(int digit, Context* ctx) {
  if (!ctx || ctx->composition().empty())
    return false;
  Segment& seg = ctx->composition().back();
  if (!seg.menu)
    return false;
  int page_size = engine_->schema() ? engine_->schema()->page_size() : 5;
  if (page_size <= 0)
    page_size = 5;
  size_t page_start =
      (seg.selected_index / static_cast<size_t>(page_size)) * page_size;
  size_t target = page_start + static_cast<size_t>(digit) - 1;
  size_t count = seg.menu->Prepare(target + 1);
  if (target >= count)
    return true;
  ctx->Select(target);
  return true;
}

bool IdsReverseProcessor::MatchesVariantReplaceKey(
    const rime::KeyEvent& key_event) const {
  if (!has_variant_replace_key_)
    return false;
  const int modifier = key_event.modifier() & ~kLockMask;
  const int target_modifier = variant_replace_key_.modifier() & ~kLockMask;
  if (modifier != target_modifier)
    return false;
  int keycode = key_event.keycode();
  int target_keycode = variant_replace_key_.keycode();
  if (keycode == target_keycode)
    return true;
  // 容忍前端以大小写不同的 keysym 报告 Shift+V / Shift+v。
  auto lower = [](int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
  };
  return lower(keycode) == lower(target_keycode);
}

bool IdsReverseProcessor::TryReplaceVariant(Context* ctx) {
  if (!ctx || !IsReverseActive(ctx, config_.hybrid.prefix))
    return false;
  if (ctx->get_property(kVariantsProperty).empty())
    return false;
  if (!ctx->HasMenu() || ctx->composition().empty())
    return false;

  HybridState st = ReadHybridState(ctx, config_.hybrid.prefix);
  const std::string& trigger = config_.variant_trigger;
  if (!EndsWith(st.pending, trigger))
    return false;

  Segment& seg = ctx->composition().back();
  if (!seg.menu)
    return false;
  size_t index = seg.selected_index;
  seg.menu->Prepare(index + 1);
  an<Candidate> cand = seg.GetCandidateAt(index);
  if (!cand || cand->text().empty() || cand->type() != "ids_reverse")
    return false;

  const std::string base = ctx->get_property(kVariantsProperty);
  const std::string variant = cand->text();
  if (variant == base)
    return true;  // 无异体字提示候选：消费按键，不改变状态。

  std::string pending_query =
      st.pending.substr(0, st.pending.size() - trigger.size());
  const bool result_replacement = pending_query.empty();
  std::string component = variant;
  if (!pending_query.empty() &&
      (pending_query[0] == '-' || pending_query[0] == '+' ||
       pending_query[0] == '!')) {
    component = pending_query.substr(0, 1) + variant;
  }

  ctx->set_property(kVariantsProperty, "");
  ctx->set_property(kVariantsResultProperty, variant);

  if (result_replacement) {
    // 替换的是结果候选（O艹癸 → 葵 → /）：保留前置部件文本用于显示，
    // 异体字由 translator 作为当前结果候选输出。
    std::string expression;
    for (const auto& existing : st.components)
      expression += existing;
    expression += component;
    SaveHybridState(ctx, {}, {});
    ctx->Clear();
    ctx->set_input(config_.hybrid.prefix + expression);
  } else {
    // 替换的是待输入原字（O葵/）：把异体字直接作为已加入的部件，
    // 这样后续输入 - / + / ! 时能正常作用于它。
    std::vector<std::string> components = st.components;
    components.push_back(component);
    std::vector<std::string> codes = components;
    std::string input = config_.hybrid.prefix;
    for (const auto& code : codes) {
      input += code;
      input += "'";
    }
    SaveHybridState(ctx, components, codes);
    ctx->Clear();
    ctx->set_input(input);
  }
  return true;
}

rime::ProcessResult IdsReverseProcessor::ProcessKeyEvent(
    const rime::KeyEvent& key_event) {
  if (key_event.release())
    return rime::kNoop;
  Context* ctx = engine_ ? engine_->context() : nullptr;
  if (!ctx)
    return rime::kNoop;

  if (HandleIdsHotkey(key_event, ctx))
    return rime::kAccepted;

  const std::string repr = key_event.repr();
  const int code = key_event.keycode();
  const bool ctrl = key_event.ctrl();
  const bool alt = key_event.alt();
  const bool shift = key_event.shift();

  if (!IsReverseActive(ctx, config_.hybrid.prefix)) {
    if (!ctx->get_option("ascii_mode") && !ctrl && !alt &&
        !ctx->IsComposing()) {
      if (repr == config_.search.prefix) {
        ctx->PushInput(config_.search.prefix);
        return rime::kAccepted;
      }
      if (repr == config_.hybrid.prefix) {
        ctx->PushInput(config_.hybrid.prefix);
        return rime::kAccepted;
      }
    }
    return rime::kNoop;
  }

  if (code == XK_Escape || repr == "Escape") {
    ctx->set_property(kVariantsProperty, "");
    ctx->set_property(kVariantsResultProperty, "");
    ctx->Clear();
    return rime::kAccepted;
  }

  if (code == XK_Return || code == XK_KP_Enter || repr == "Return") {
    HybridState st = ReadHybridState(ctx, config_.hybrid.prefix);
    if (st.components.empty() && st.pending.empty()) {
      engine_->CommitText(config_.hybrid.prefix);
      ctx->Clear();
      return rime::kAccepted;
    }
    // 多字部件候选或结果候选：回车直接确认当前候选。
    // 单字部件仍保留“把编码作为一个部件”的旧行为。
    if (ctx->HasMenu()) {
      an<Candidate> cand = ctx->GetSelectedCandidate();
      if (cand) {
        an<Candidate> genuine = Candidate::GetGenuineCandidate(cand);
        const std::string& type = cand->type();
        const std::string& genuine_type = genuine ? genuine->type() : type;
        bool is_result = type == "ids_reverse" || type == "hanzi_result" ||
                         genuine_type == "ids_reverse" ||
                         genuine_type == "hanzi_result";
        bool is_multi_component =
            (type == "hanzi_component" || genuine_type == "hanzi_component") &&
            Utf8Length(cand->text()) > 1;
        if (is_result || is_multi_component) {
          ctx->ConfirmCurrentSelection();
          return rime::kAccepted;
        }
        // 括号内单字部件也用回车确认，交给 OnSelectAfter 替换当前拼音段。
        if (ParseSingleGroup(st.pending).valid) {
          ctx->ConfirmCurrentSelection();
          return rime::kAccepted;
        }
      }
    }
    if (ParseSingleGroup(st.pending).valid)
      return rime::kAccepted;
    if (!st.pending.empty() && st.pending != "-" && st.pending != "+" &&
        st.pending != "!") {
      if (!ctx->composition().empty())
        ctx->composition().pop_back();
      st.components.push_back(st.pending);
      st.codes.push_back(st.pending);
      SaveHybridState(ctx, st.components, st.codes);
      ctx->PushInput("'");
      return rime::kAccepted;
    }
    if (ctx->HasMenu() && ctx->GetSelectedCandidate()) {
      ctx->ConfirmCurrentSelection();
      return rime::kAccepted;
    }
    return rime::kNoop;
  }

  if (code == XK_BackSpace || repr == "BackSpace") {
    ctx->set_property(kVariantsResultProperty, "");
    HybridState st = ReadHybridState(ctx, config_.hybrid.prefix);
    if (!st.pending.empty()) {
      const size_t pending_chars = Utf8Length(st.pending);
      size_t pending_bytes = LastUtf8CharBytes(st.pending);
      if (pending_bytes == 0)
        pending_bytes = 1;
      if (pending_chars == 1 && !st.components.empty()) {
        ctx->PopInput(pending_bytes);
        while (!ctx->composition().empty() &&
               ctx->composition().back().start ==
                   ctx->composition().back().end) {
          ctx->composition().pop_back();
        }
        if (!ctx->composition().empty()) {
          Segment& sep = ctx->composition().back();
          sep.tags.clear();
          sep.tags.insert(config_.hybrid.tag);
          sep.tags.insert(kResultSegmentTag);
          sep.status = Segment::kVoid;
        }
        ctx->set_input(ctx->input());
        return rime::kAccepted;
      }
      ctx->PopInput(pending_bytes);
    } else if (!st.components.empty()) {
      const size_t code_len = st.codes.back().size();
      st.components.pop_back();
      st.codes.pop_back();
      SaveHybridState(ctx, st.components, st.codes);
      if (!ctx->composition().empty())
        ctx->composition().pop_back();
      if (!ctx->composition().empty())
        ctx->composition().back().status = Segment::kGuess;
      ctx->PopInput(code_len + 1);
    } else {
      ctx->Clear();
    }
    return rime::kAccepted;
  }

  if (!ctrl && !alt && code >= XK_1 && code <= XK_6 &&
      !(shift && code == XK_1)) {
    if (ctx->HasMenu()) {
      HandleSelectionKey(code - XK_0, ctx);
      return rime::kAccepted;
    }
    return rime::kNoop;
  }
  if (!ctrl && !alt && (code == XK_semicolon || repr == "semicolon")) {
    if (ctx->HasMenu()) {
      HandleSelectionKey(2, ctx);
      return rime::kAccepted;
    }
    return rime::kNoop;
  }
  if (code == XK_space || repr == "space") {
    if (!key_event.shift() && ctx->HasMenu() && ctx->GetSelectedCandidate()) {
      ctx->ConfirmCurrentSelection();
      return rime::kAccepted;
    }
    return rime::kNoop;
  }

  bool is_letter = !ctrl && !alt && repr.size() == 1 &&
                   std::isalpha(static_cast<unsigned char>(repr[0]));
  bool is_variant =
      !ctrl && !alt && (code == XK_slash || repr == "/" || repr == "slash");
  bool is_minus =
      !ctrl && !alt && (code == XK_minus || repr == "minus" || repr == "-");
  bool is_plus = !ctrl && !alt &&
                 (code == XK_plus || code == XK_equal || repr == "plus" ||
                  repr == "equal" || repr == "+" || repr == "=");
  bool is_exclude = !ctrl && !alt &&
                    (code == XK_exclam || repr == "!" || repr == "exclam" ||
                     (code == XK_1 && shift));
  bool is_lparen = !ctrl && !alt &&
                   (code == XK_parenleft || repr == "(" || repr == "parenleft");
  bool is_rparen =
      !ctrl && !alt &&
      (code == XK_parenright || repr == ")" || repr == "parenright");

  if (is_variant) {
    ctx->set_property(kVariantsResultProperty, "");
    // 异体字触发：以菜单当前选中候选为基准，写入 property
    // 后推入触发符驱动重译。
    if (!ctx->HasMenu() || ctx->composition().empty())
      return rime::kNoop;
    Segment& seg = ctx->composition().back();
    if (!seg.menu)
      return rime::kNoop;
    size_t index = seg.selected_index;
    seg.menu->Prepare(index + 1);
    an<Candidate> cand = seg.GetCandidateAt(index);
    if (!cand || cand->text().empty())
      return rime::kNoop;
    an<Candidate> genuine = Candidate::GetGenuineCandidate(cand);
    const std::string& outer_type = cand->type();
    const std::string& genuine_type = genuine ? genuine->type() : outer_type;
    bool is_result =
        outer_type == "ids_reverse" || outer_type == "hanzi_result" ||
        genuine_type == "ids_reverse" || genuine_type == "hanzi_result";
    // 单字部件候选也允许直接查异体字，例如 O葵/。
    bool is_single_component = (outer_type == "hanzi_component" ||
                                genuine_type == "hanzi_component") &&
                               Utf8Length(cand->text()) == 1;
    if (!is_result && !is_single_component) {
      ctx->set_property(kVariantsProperty, "");
      return rime::kNoop;
    }
    const std::string base =
        is_result && genuine ? genuine->text() : cand->text();

    // 单字部件加入状态后，结果菜单显示的是“包含该部件的字”。
    // 若高亮结果没有异体字，则回退为查询该部件本身：
    // O葵 → 空格 → / 应显示 葵 的异体字，而不是当前结果。
    if (is_result) {
      HybridState st = ReadHybridState(ctx, config_.hybrid.prefix);
      if (st.pending.empty() && st.components.size() == 1 &&
          Utf8Length(st.components[0]) == 1 && st.components[0] != base &&
          !HasVariants(base)) {
        const std::string component = st.components[0];
        ctx->set_property(kVariantsProperty, component);
        SaveHybridState(ctx, {}, {});
        ctx->Clear();
        ctx->set_input(config_.hybrid.prefix + component +
                       config_.variant_trigger);
        return rime::kAccepted;
      }
    }

    ctx->set_property(kVariantsProperty, base);
    ctx->PushInput(config_.variant_trigger);
    return rime::kAccepted;
  }

  if (MatchesVariantReplaceKey(key_event) && TryReplaceVariant(ctx))
    return rime::kAccepted;

  if (is_letter || is_minus || is_plus || is_exclude || is_lparen ||
      is_rparen) {
    // 异体字替换后继续输入：确保异体字已作为独立部件存在，
    // 让后续运算作用于它，而不是把它留作 pending 原始输入。
    const std::string variant_result =
        ctx->get_property(kVariantsResultProperty);
    if (!variant_result.empty()) {
      HybridState st = ReadHybridState(ctx, config_.hybrid.prefix);
      bool already_component = false;
      if (!st.components.empty()) {
        const std::string& last = st.components.back();
        const std::string base =
            (!last.empty() &&
             (last[0] == '-' || last[0] == '+' || last[0] == '!'))
                ? last.substr(1)
                : last;
        already_component = (base == variant_result);
      }
      ctx->set_property(kVariantsResultProperty, "");
      ctx->set_property(kVariantsProperty, "");
      if (!already_component) {
        // 结果替换：保留原始表达式作为显示用 code，让 preedit 维持
        // O艹癸𦮙；实际参与检字运算的部件是异体字本身。
        std::string display = st.pending;
        if (display.empty())
          display = variant_result;
        SaveHybridState(ctx, {variant_result}, {display});
        ctx->Clear();
        ctx->set_input(config_.hybrid.prefix + display + "'");
      }
    }
    char ch = repr[0];
    if (is_minus)
      ch = '-';
    else if (is_plus)
      ch = '+';
    else if (is_exclude)
      ch = '!';
    else if (is_lparen)
      ch = '(';
    else if (is_rparen)
      ch = ')';
    if (ch >= 'A' && ch <= 'Z')
      ch = static_cast<char>(ch - 'A' + 'a');
    if (ctx->caret_pos() != ctx->input().size())
      ctx->set_caret_pos(ctx->input().size());
    if (!ctx->composition().empty() &&
        ctx->composition().back().HasTag(config_.hybrid.tag)) {
      ctx->ClearNonConfirmedComposition();
    }
    ctx->set_property(kVariantsProperty, "");
    ctx->PushInput(std::string(1, ch));
    if (is_rparen)
      TryCommitClosedGroup(ctx, config_);
    return rime::kAccepted;
  }

  return rime::kNoop;
}

// ---------------------------------------------------------------------------
// Segmentor
// ---------------------------------------------------------------------------

IdsReverseSegmentor::IdsReverseSegmentor(const rime::Ticket& ticket)
    : Segmentor(ticket) {
  LoadReverseConfig(ticket, &name_space_, &config_);
}

bool IdsReverseSegmentor::Proceed(Segmentation* segmentation) {
  if (!segmentation)
    return false;
  Context* ctx = engine_ ? engine_->context() : nullptr;

  const std::string& input = segmentation->input();
  size_t start = segmentation->GetCurrentStartPosition();

  // search 模式：整段输入打标签，交给 translator 直接检索。
  if (start == 0 && (!ctx || !ctx->get_option("ascii_mode")) &&
      StartsWith(input, config_.search.prefix)) {
    Segment seg(0, static_cast<int>(input.size()));
    seg.tags.insert(config_.search.tag);
    seg.prompt = config_.search.tips;
    if (!segmentation->AddSegment(seg))
      return true;
    if (!segmentation->empty())
      segmentation->back().prompt = config_.search.tips;
    return false;
  }

  if (!IsReverseActive(ctx, config_.hybrid.prefix))
    return true;

  if (start >= input.size())
    return false;

  size_t prefix_end = (start == 0) ? config_.hybrid.prefix.size() : start + 1;
  bool is_prefix =
      (start == 0) || (start < input.size() && input[start] == '\'');

  Segment seg;
  if (is_prefix && prefix_end < input.size()) {
    seg = Segment(static_cast<int>(start), static_cast<int>(prefix_end));
    // 保留 hybrid 前缀在组字区显示；引号分隔符仍隐藏。
    if (start != 0)
      seg.tags.insert("phony");
    seg.status = Segment::kConfirmed;
  } else if (is_prefix) {
    seg = Segment(static_cast<int>(start), static_cast<int>(input.size()));
    if (start == 0) {
      seg.tags.insert(config_.hybrid.tag);
    } else {
      seg.tags.insert(config_.hybrid.tag);
      seg.tags.insert(kResultSegmentTag);
    }
  } else {
    size_t next_quote = input.find('\'', start + 1);
    char first_char = (start < input.size()) ? input[start] : '\0';
    if (first_char == '-' || first_char == '+' || first_char == '!') {
      bool has_code_after =
          (start + 1 < input.size()) &&
          (next_quote == std::string::npos || next_quote > start + 1);
      if (has_code_after) {
        seg = Segment(static_cast<int>(start), static_cast<int>(start + 1));
        seg.status = Segment::kConfirmed;
        seg.tags.insert("confirmed_component");
      } else if (start + 1 == input.size()) {
        seg = Segment(static_cast<int>(start), static_cast<int>(input.size()));
        seg.tags.insert(config_.hybrid.tag);
        seg.tags.insert(kResultSegmentTag);
      } else {
        seg = Segment(static_cast<int>(start), static_cast<int>(start + 1));
        seg.status = Segment::kConfirmed;
        seg.tags.insert("confirmed_component");
      }
    } else if (next_quote != std::string::npos) {
      seg = Segment(static_cast<int>(start), static_cast<int>(next_quote));
      seg.status = Segment::kConfirmed;
      seg.tags.insert("confirmed_component");
    } else {
      seg = Segment(static_cast<int>(start), static_cast<int>(input.size()));
      seg.tags.insert("abc");
      seg.tags.insert("hanzi_component");
    }
  }

  seg.prompt = config_.hybrid.tips;
  segmentation->AddSegment(seg);
  return false;
}

// ---------------------------------------------------------------------------
// Filter / Cleaner
// ---------------------------------------------------------------------------

namespace {

class HybridFilterTranslation : public rime::PrefetchTranslation {
 public:
  HybridFilterTranslation(an<rime::Translation> translation, Context* ctx)
      : PrefetchTranslation(translation), ctx_(ctx) {}

  an<Candidate> Peek() override {
    if (exhausted())
      return nullptr;
    if (!cache_.empty())
      return cache_.front();
    if (!Replenish()) {
      // 不要把未过滤的底层候选回退出去。
      set_exhausted(true);
      return nullptr;
    }
    return cache_.front();
  }

 protected:
  bool Replenish() override {
    // 防止底层翻译器（预测、句子等）产生海量候选时长时间扫描。
    constexpr size_t kMaxScan = 2000;
    size_t scanned = 0;
    while (cache_.empty()) {
      if (scanned++ >= kMaxScan)
        break;
      an<Candidate> next = translation_->Peek();
      if (!next) {
        translation_->Next();
        if (translation_->exhausted())
          break;
        continue;
      }
      translation_->Next();
      an<Candidate> genuine = Candidate::GetGenuineCandidate(next);
      const std::string& type = genuine ? genuine->type() : next->type();
      if (type == "ids_reverse" || type == "hanzi_result") {
        cache_.push_back(next);
        continue;
      }
      if (!ctx_ || ctx_->composition().empty()) {
        cache_.push_back(next);
        continue;
      }
      Segment& seg = ctx_->composition().back();
      const bool component_stage = seg.HasTag("hanzi_component");
      if (!component_stage)
        continue;
      if (next->end() == seg.end && IsComponentExpression(next->text()) &&
          seen_.insert(next->text()).second) {
        cache_.push_back(New<rime::ShadowCandidate>(next, "hanzi_component"));
      }
    }
    return !cache_.empty();
  }

 private:
  Context* ctx_;
  std::set<std::string> seen_;
};

}  // namespace

IdsReverseFilter::IdsReverseFilter(const rime::Ticket& ticket)
    : Filter(ticket) {
  LoadReverseConfig(ticket, &name_space_, &config_);
}

bool IdsReverseFilter::AppliesToSegment(Segment* segment) {
  suppress_ = false;
  Context* ctx = engine_ ? engine_->context() : nullptr;
  if (!ctx || !segment || ctx->get_option("ascii_mode"))
    return true;
  if (ctx->input() != config_.search.prefix)
    return true;
  if (segment->start != 0 ||
      segment->end != static_cast<size_t>(config_.search.prefix.size())) {
    return true;
  }
  suppress_ = true;
  return true;
}

an<rime::Translation> IdsReverseFilter::Apply(an<rime::Translation> translation,
                                              rime::CandidateList* candidates) {
  (void)candidates;
  Context* ctx = engine_ ? engine_->context() : nullptr;
  if (!ctx)
    return translation;

  if (suppress_) {
    suppress_ = false;
    return New<rime::FifoTranslation>();
  }

  if (!IsReverseActive(ctx, config_.hybrid.prefix))
    return translation;

  // 初始 O / 前缀段既不是部件选择段，也不是结果段。
  // 直接返回空翻译，避免遍历完整词典或预测翻译器的海量候选。
  if (ctx->composition().empty())
    return New<rime::FifoTranslation>();
  Segment& seg = ctx->composition().back();
  if (!seg.HasTag("hanzi_component") && !seg.HasTag(kResultSegmentTag))
    return New<rime::FifoTranslation>();

  return New<HybridFilterTranslation>(translation, ctx);
}

}  // namespace ids_reverse
