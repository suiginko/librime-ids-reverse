-- 纯 Lua 测试：不依赖 Rime 引擎。
--
-- 用法：
--   lua5.5 plugins/ids-reverse/tests/test_ids_reverse.lua
--   lua5.5 plugins/ids-reverse/tests/test_ids_reverse.lua <ids_data.bin>

local script = arg and arg[0] or "tests/test_ids_reverse.lua"
local root = script:match("^(.*)[/\\]tests[/\\]") or "."
local bin = (arg and arg[1]) or (root .. "/data/ids_data.bin")
local module = root .. "/ids_reverse.lua"

local M = dofile(module)
local I = M._internal
local store, err = I.Store.load(bin, "")
assert(store, "failed to load " .. bin .. ": " .. tostring(err))

local full = I.get_codec(store, { algebra = {} })
local flypy_rules = {
  "xform/^zh/v/", "xform/^ch/i/", "xform/^sh/u/",
  "xform/iu$/q/", "xform/(.)ei$/$1w/", "xform/uan$/r/",
  "xform/[uv]e$/t/", "xform/un$/y/", "xform/uo$/o/", "xform/ie$/p/",
  "xform/(.)iao$/$1n/", "xform/(.)iang$/$1l/", "xform/(.)iong$/$1s/",
  "xform/(.)uai$/$1k/", "xform/(.)ing$/$1k/", "xform/(.)ian$/$1m/",
  "xform/(.)uang$/$1l/", "xform/(.)ai$/$1d/", "xform/(.)en$/$1f/",
  "xform/(.)eng$/$1g/", "xform/(.)ang$/$1h/", "xform/(.)an$/$1j/",
  "xform/(.)ou$/$1z/", "xform/(.)ao$/$1c/", "xform/(.)in$/$1b/",
  "xform/(.)ia$/$1x/", "xform/(.)ua$/$1x/", "xform/(.)ong$/$1s/",
  "xform/(.)ui$/$1v/",
}
local flypy = I.get_codec(store, { algebra = flypy_rules })

local passed = 0
local function ok(cond, msg)
  if not cond then error("FAIL: " .. msg, 2) end
  passed = passed + 1
  print("ok " .. passed .. " - " .. msg)
end

local function chars(ids)
  local out = {}
  for i, id in ipairs(ids) do out[i] = utf8.char(store:cp_of_id(id)) end
  return table.concat(out)
end

local function contains(text, ch)
  return text:find(ch, 1, true) ~= nil
end

local function index_of(ids, ch)
  local cp = utf8.codepoint(ch)
  for i, id in ipairs(ids) do
    if store:cp_of_id(id) == cp then return i end
  end
  return nil
end

ok(store.num_chars > 100000, "字符数量")
ok(store.num_tokens > 10000, "部件 token 数量")
ok(store.num_keys > 10000, "倒排 key 数量")

local wood = utf8.codepoint("木")
local wood_id = store:id_of_cp(wood)
ok(wood_id ~= 0xFFFFFFFF, "木 的稠密 id")
ok(store:cp_of_id(wood_id) == wood, "id/cp 双向一致")

ok(I.canon("辶") == "⻌", "Canon 归一")
ok(I.format_ids("⿰木木", "bracket") == "[左右]木木", "IDS bracket 格式")
ok(I.format_ids("⿰木木", "plain") == "木木", "IDS plain 格式")
ok(I.build_comment(store, wood_id, true, true, true, true, "unicode")
   :find("6728", 1, true) ~= nil, "候选注释包含 Unicode")
ok(I.ids_of_text(store, "木") ~= "", "IDS 结构输出")

local kz = flypy:decode_exact("kz")
ok(#kz == 1 and kz[1] == "kou", "flypy 双拼 kz -> kou")
local mn_segs = I.segment_alpha_string(store, "mn", flypy)
ok(#mn_segs == 1 and mn_segs[1] == "miao", "flypy mn -> miao")
local mn_pending = I.parse_pending_components(store, "mn", flypy)
ok(mn_pending.valid and #mn_pending.tokens == 1
   and mn_pending.tokens[1] == "miao", "pending mn -> miao")
local miao_ids = I.get_component_candidates(store, "miao", full, 0)
local miao_chars = chars(miao_ids)
ok(contains(miao_chars, "渺") and contains(miao_chars, "妙")
   and contains(miao_chars, "苗"), "miao 部件候选")
local segs = I.segment_alpha_string(store, "mukou", full)
ok(#segs == 2 and segs[1] == "mu" and segs[2] == "kou", "全拼切分 mukou")

-- 拼写代数
local mukz = I.segment_alpha_string(store, "mukz", flypy)
ok(#mukz == 2 and mukz[1] == "mu" and mukz[2] == "kou", "flypy 切分 mukz")
local strict_kou = I.segment_alpha_string(store, "kou", flypy)
ok(not (#strict_kou == 1 and strict_kou[1] == "kou"),
   "严格 algebra 不接受未保留的全拼")

local abbrev = I.build_codec(store, { "abbrev/^([a-z]).+$/$1/" })
local abbrev_m = abbrev:decode_exact("m")
ok(#abbrev_m > 10, "abbrev 展开多个拼音")
local abbrev_seg = abbrev:segment("m")
ok(#abbrev_seg == 1 and abbrev_seg[1] == "m", "abbrev 歧义编码保留原文")

local derive = I.build_codec(store, { "derive/^([jqxy])u$/$1v/" })
local d1 = derive:decode_exact("jv")
local d2 = derive:decode_exact("ju")
ok(#d1 == 1 and d1[1] == "ju", "derive 新编码")
ok(#d2 == 1 and d2[1] == "ju", "derive 保留原编码")

local fuzzy = I.build_codec(store, { "fuzz/^zh/z/" })
local fz = fuzzy:decode_exact("za")
local has_zha = false
for _, py in ipairs(fz) do if py == "zha" then has_zha = true end end
ok(has_zha, "fuzz 生成模糊编码")

local fake = { valid_pinyins = { ["abc"] = true }, num_chars = 0 }
local xlit = I.build_codec(fake, { "xlit/abc/xyz/" })
local xl = xlit:decode_exact("xyz")
ok(#xl == 1 and xl[1] == "abc", "xlit 字符映射")
local reorder = I.build_codec(fake, { "reorder/cba/" })
local ro = reorder:decode_exact("cba")
ok(#ro == 1 and ro[1] == "abc", "reorder 重排")
local erase = I.build_codec(fake, { "erase/^abc$/" })
ok(#erase:decode_exact("abc") == 0, "erase 删除整条编码")

local kou_chars = chars(I.get_pinyin_candidates(store, "kou", 0))
ok(contains(kou_chars, "口"), "标准拼音候选 口")

local mukz_tokens = flypy:segment("mukz")
ok(#mukz_tokens == 2 and mukz_tokens[1] == "mu" and mukz_tokens[2] == "kou",
   "flypy 多音节解码")
local kz_chars = chars(I.get_component_candidates(store, "kz", flypy, 0))
ok(contains(kz_chars, "口"), "flypy 输入码 kz 候选包含 口")
local mn_chars = chars(I.get_component_candidates(store, "mn'", flypy, 0))
ok(contains(mn_chars, "妙"), "flypy 输入码 mn 候选包含 妙")

local pending = I.parse_pending_components(store, "mukou", full)
ok(pending.valid and #pending.tokens == 2, "pending 多音节解析")

local function first_component(code)
  local ids = I.get_component_candidates(store, code, full, 20)
  return chars(ids), ids
end
local mu_chars = first_component("mu")
ok(contains(mu_chars, "木") and contains(mu_chars, "目") and contains(mu_chars, "母"),
   "mu 部件候选")

ok(store.version == 3, "数据版本为 IDS4 v3")
local sha_ids = I.get_component_candidates(store, "sha", full, 0)
local sha_chars = chars(sha_ids)
ok(#sha_ids > 100, "sha 候选数量 > 100")
ok(contains(sha_chars, "啥") and contains(sha_chars, "砂")
   and contains(sha_chars, "鲨") and contains(sha_chars, "纱"),
   "sha 包含 啥/砂/鲨/纱")
ok(contains(sha_chars, "挲"), "sha 包含异读 挲")
local sh_prefix = I.get_component_candidates(store, "sh", full, 10)
ok(#sh_prefix == 10, "前缀候选保留上限")

local function search(q, codec)
  return I.search_components(store, q, 60, codec or full, nil)
end
local function hybrid(components, codec)
  return I.search_hybrid(store, components, codec or full, 60, nil)
end

local wood_mouth = chars(search("木口"))
ok(contains(wood_mouth, "杏"), "I 木口 包含 杏")
ok(#search("kouhao") > 0, "I kouhao")
ok(#search("老六") > 0, "I 老六")
ok(#search("木木木") > 0, "I 木木木")
ok(#search("氵工") > 0, "I 氵工")

local bi_tokens = { store:find_token("匕"), store:find_token("匕") }
local bi_exact = store:lookup_leaf_structure(bi_tokens)
ok(contains(chars(bi_exact), "𫧇"), "结构索引 匕匕 包含 𫧇")
ok(contains(chars(bi_exact), "比"), "结构索引 匕匕 包含 比")
ok(not contains(chars(bi_exact), "昆"), "结构索引 匕匕 排除 昆")

local le_exact = store:lookup_leaf_structure({ store:find_token("了") })
ok(contains(chars(le_exact), "𠄏"), "结构索引 了 包含 𠄏")

local i_bi = search("匕匕")
ok(contains(chars(i_bi), "𫧇"), "I 匕匕 包含 𫧇")

local i_le = search("了")
ok(index_of(i_le, "𠄏") ~= nil,
   "I 了 包含 𠄏")

ok(I.format_ids("⿿了", "bracket") == "[旋转]了", "旋转操作符 bracket 格式")
ok(I.format_ids("⿿了", "plain") == "了", "旋转操作符 plain 格式")

local lin = hybrid({ "木木" })
ok(utf8.char(store:cp_of_id(lin[1])) == "林", "O 木木 首选 林")
ok(contains(chars(lin), "啉"), "O 木木 包含 啉")

local o_le = hybrid({ "了" })
local o_le_pos = index_of(o_le, "𠄏")
ok(o_le_pos ~= nil and o_le_pos <= 3, "O 了 优先展示 𠄏")

local o_bi = hybrid({ "匕匕" })
ok(contains(chars(o_bi), "𫧇"), "O 匕匕 包含 𫧇")

-- 原始字形优先：同结构下先显示原文精确匹配，字频仅作最后兜底。
local o_miao = hybrid({ "纟", "目", "少" })
local o_miao_sim = index_of(o_miao, "缈")
local o_miao_trad = index_of(o_miao, "緲")
ok(o_miao_sim ~= nil and utf8.char(store:cp_of_id(o_miao[1])) == "缈",
   "O 纟目少 首选 缈")
ok(o_miao_sim ~= nil and o_miao_trad ~= nil and o_miao_sim < o_miao_trad,
   "O 纟目少 缈 在 緲 前")

local o_xiang = hybrid({ "纟", "目" })
ok(utf8.char(store:cp_of_id(o_xiang[1])) == "缃", "O 纟目 首选 缃")

local o_sha = hybrid({ "纟", "少" })
ok(utf8.char(store:cp_of_id(o_sha[1])) == "纱", "O 纟少 首选 纱")

local o_long = hybrid({ "龙" })
local o_long_pos = index_of(o_long, "拢")
local o_long_trad = index_of(o_long, "龍")
ok(o_long_pos ~= nil and
   (o_long_trad == nil or o_long_pos < o_long_trad),
   "O 龙 优先原始字形 拢")

local o_yan = hybrid({ "讠" })
local o_yan_pos = index_of(o_yan, "计")
local o_yan_trad = index_of(o_yan, "訁")
ok(o_yan_pos ~= nil and
   (o_yan_trad == nil or o_yan_pos < o_yan_trad),
   "O 讠 优先原始字形 计")

-- 逆波兰后缀表达式
local sen_minus = hybrid({ "森", "木", "-" })
ok(utf8.char(store:cp_of_id(sen_minus[1])) == "林", "O 森木- 首选 林")

local da_minus = hybrid({ "达", "大", "-" })
ok(utf8.char(store:cp_of_id(da_minus[1])) == "辶", "O 达大- 首选 辶")

local excluded = chars(hybrid({ "木木", "口", "!" }))
ok(not contains(excluded, "啉"), "O 木木口! 排除 啉")
ok(#hybrid({ "木木", "木", "!" }) == 0, "O 木木木! 结果为空")

local zhang = hybrid({ "章", "日", "-", "十", "-" })
ok(utf8.char(store:cp_of_id(zhang[1])) == "立", "O 章日-十- 首选 立")

local chun = hybrid({ "春", "日", "-" })
ok(utf8.char(store:cp_of_id(chun[1])) == "𡗗", "O 春日- 首选 𡗗")

local pei = hybrid({ "佩", "亻", "-" })
ok(utf8.char(store:cp_of_id(pei[1])) == "𫥞", "O 佩亻- 首选 𫥞")

local miao_minus_water = hybrid({ "渺", "氵", "-" })
ok(utf8.char(store:cp_of_id(miao_minus_water[1])) == "眇", "O 渺氵- 首选 眇")

-- 一个状态层是同一个操作数：汉字串展开后作为整体参与后续运算。
local multi_operand = hybrid({ "章", "日十", "-" })
ok(utf8.char(store:cp_of_id(multi_operand[1])) == "立",
   "O 章 日十 - 首选 立")

local miao_group = hybrid({ "渺", "水", "少", "+", "-" })
ok(utf8.char(store:cp_of_id(miao_group[1])) == "目", "O 渺水少+- 首选 目")
local miao_group_raw = hybrid({ "渺水少+-" })
ok(utf8.char(store:cp_of_id(miao_group_raw[1])) == "目",
   "O 渺水少+- 单层表达式首选 目")

local sen_group = hybrid({ "森", "木", "木", "+", "-" })
ok(utf8.char(store:cp_of_id(sen_group[1])) == "木", "O 森木木+- 首选 木")

local grouped_excluded = chars(hybrid({ "木木", "口", "日", "+", "!" }))
ok(not contains(grouped_excluded, "啉"), "O 木木口日+! 排除 啉")
ok(not contains(grouped_excluded, "晽"), "O 木木口日+! 排除 晽")

local rpn_add = hybrid({ "木", "木", "+" })
ok(utf8.char(store:cp_of_id(rpn_add[1])) == "林", "RPN 木木+ 首选 林")
local rpn_chain = hybrid({ "森", "木", "-", "氵", "+" })
ok(utf8.char(store:cp_of_id(rpn_chain[1])) == "淋", "RPN 森木-氵+ 首选 淋")
-- 结尾减法且栈内仍有多个操作数时，显示栈顶的中间结果。
local rpn_partial_sub = hybrid({ "渺", "沙", "少", "-" })
ok(#rpn_partial_sub > 0, "RPN 渺沙少- 不消失")
ok(utf8.char(store:cp_of_id(rpn_partial_sub[1])) == "氵",
   "RPN 渺沙少- 首选 氵")
local rpn_full_sub = hybrid({ "渺", "沙", "少", "-", "-" })
ok(utf8.char(store:cp_of_id(rpn_full_sub[1])) == "眇",
   "RPN 渺沙少-- 首选 眇")
local rpn_two_operands = hybrid({ "木", "木" })
ok(utf8.char(store:cp_of_id(rpn_two_operands[1])) == "林",
   "RPN 木木 仍按同时包含")
local rpn_excluded = chars(hybrid({ "木", "木", "口", "!" }))
ok(not contains(rpn_excluded, "啉"), "RPN 木木口! 排除 啉")
local rpn_miao = hybrid({ "渺", "水", "少", "+", "-" })
ok(utf8.char(store:cp_of_id(rpn_miao[1])) == "目", "RPN 渺水少+- 首选 目")
ok(#hybrid({ "木", "-" }) == 0, "RPN 运算符不足返回空")
ok(#hybrid({ "木-" }) == 0, "RPN 单层表达式运算符不足返回空")

-- 单层原始表达式按 RPN 求值
local raw_expr = hybrid({ "森木-" })
ok(utf8.char(store:cp_of_id(raw_expr[1])) == "林", "RPN 单层 森木- 首选 林")

local comps, excludes = I.evaluate_rpn_expression(store, { "森", "木", "-" }, full)
ok(#comps == 1 and comps[1] == "林" and #excludes == 0,
   "RPN 求值 森木- -> 林")
comps, excludes = I.evaluate_rpn_expression(store, { "木", "木", "口", "!" }, full)
ok(#comps == 2 and #excludes == 1 and excludes[1] == "口",
   "RPN 求值 木木口! 排除 口")

ok(I.is_component_expression("木木口!"), "RPN 部件表达式合法")
ok(I.is_component_expression("木木"), "纯部件表达式合法")
ok(not I.is_component_expression("木木(口)"), "括号表达式非法")
ok(I.is_component_expression("木木口"), "无运算符部件串仍合法")

local shao_shui = hybrid({ "少", "氵" })
ok(utf8.char(store:cp_of_id(shao_shui[1])) == "沙", "O 少+氵 首选 沙")

local qian_id = store:id_of_cp(utf8.codepoint("芌"))
local yu_id = store:id_of_cp(utf8.codepoint("芋"))
local qian_variants = store:variants(qian_id)
ok(#qian_variants == 1 and qian_variants[1] == yu_id, "芌 的异体字包含 芋")
local yu_variants = store:variants(yu_id)
ok(#yu_variants >= 2, "芋 有多个异体字")

-- 多部件组合候选
local saved_candidate = _G.Candidate
local saved_yield = _G.yield
_G.Candidate = function(typ, start_pos, end_pos, text, comment)
  return { type = typ, start = start_pos, _end = end_pos, text = text,
           comment = comment }
end
local emitted = {}
_G.yield = function(cand) emitted[#emitted + 1] = cand end
local cfg = {
  max_results = 60,
  disabled_blocks_enabled = false,
  disabled_blocks = {},
  algebra = {},
}
I.build_multi_component_candidates(store, { "mu", "kou" },
                                   { start = 0, _end = 5 }, cfg, nil)
local combo = {}
for i, cand in ipairs(emitted) do combo[i] = cand.text end
local combo_text = table.concat(combo, " ")
ok(contains(combo_text, "木口"), "Omukou 生成 木口 组合候选")

emitted = {}
local flypy_cfg = {
  max_results = 60,
  disabled_blocks_enabled = false,
  disabled_blocks = {},
  algebra = flypy_rules,
}
I.build_multi_component_candidates(store, flypy:segment("mukz"),
                                   { start = 0, _end = 4 }, flypy_cfg, nil)
combo = {}
for i, cand in ipairs(emitted) do combo[i] = cand.text end
combo_text = table.concat(combo, " ")
ok(contains(combo_text, "木口"), "flypy mukz 生成 木口 组合候选")

emitted = {}
I.build_multi_component_candidates(store, { "mu", "mu" },
                                   { start = 0, _end = 4 }, cfg, nil)
combo = {}
for i, cand in ipairs(emitted) do combo[i] = cand.text end
combo_text = table.concat(combo, " ")
ok(contains(combo_text, "木木"), "Omumu 生成 木木 组合候选")

_G.Candidate = saved_candidate
_G.yield = saved_yield

print(string.format("全部通过：%d 项", passed))
