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

ok(I.decode_shuangpin(store, "kz", "flypy") == "kou", "flypy 双拼 kz -> kou")
local segs = I.segment_alpha_string(store, "mukou", "quanpin")
ok(#segs == 2 and segs[1] == "mu" and segs[2] == "kou", "全拼切分 mukou")

local pending = I.parse_pending_components(store, "mukou", "quanpin")
ok(pending.valid and #pending.tokens == 2, "pending 多音节解析")

local function first_component(code)
  local ids = I.get_component_candidates(store, code, "quanpin", 20)
  return chars(ids), ids
end
local mu_chars = first_component("mu")
ok(contains(mu_chars, "木") and contains(mu_chars, "目") and contains(mu_chars, "母"),
   "mu 部件候选")

local function search(q, mode)
  return I.search_components(store, q, 60, mode or "quanpin", nil)
end
local function hybrid(components, mode)
  return I.search_hybrid(store, components, mode or "quanpin", 60, nil)
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

local sen_minus = hybrid({ "森", "-木" })
ok(utf8.char(store:cp_of_id(sen_minus[1])) == "林", "O 森-木 首选 林")

local da_minus = hybrid({ "达", "-大" })
ok(utf8.char(store:cp_of_id(da_minus[1])) == "辶", "O 达-大 首选 辶")

local excluded = chars(hybrid({ "木木", "!口" }))
ok(not contains(excluded, "啉"), "O 木木!口 排除 啉")
ok(#hybrid({ "木木", "!木" }) == 0, "O 木木!木 结果为空")

local zhang = hybrid({ "章", "-日十" })
ok(utf8.char(store:cp_of_id(zhang[1])) == "立", "O 章-日十 首选 立")

local chun = hybrid({ "春", "-日" })
ok(utf8.char(store:cp_of_id(chun[1])) == "𡗗", "O 春-日 首选 𡗗")

local pei = hybrid({ "佩", "-亻" })
ok(utf8.char(store:cp_of_id(pei[1])) == "𫥞", "O 佩-亻 首选 𫥞")

-- 括号分组：外层运算符作用于组内所有部件
local miao_group = hybrid({ "渺", "-(水少)" })
ok(utf8.char(store:cp_of_id(miao_group[1])) == "目", "O 渺-(水少) 首选 目")
local miao_group_raw = hybrid({ "渺-(水少)" })
ok(utf8.char(store:cp_of_id(miao_group_raw[1])) == "目",
   "O 渺-(水少) 表达式首选 目")

local sen_group = hybrid({ "森", "-(木木)" })
ok(utf8.char(store:cp_of_id(sen_group[1])) == "木", "O 森-(木木) 首选 木")

local grouped_excluded = chars(hybrid({ "木木", "!(口日)" }))
ok(not contains(grouped_excluded, "啉"), "O 木木!(口日) 排除 啉")
ok(not contains(grouped_excluded, "晽"), "O 木木!(口日) 排除 晽")

local group_steps = I.parse_algebra_steps({ "渺-(水少)" })
ok(#group_steps == 3 and group_steps[1].item == "渺"
   and group_steps[1].op == "+"
   and group_steps[2].item == "水" and group_steps[2].op == "-"
   and group_steps[3].item == "少" and group_steps[3].op == "-",
   "括号解析 渺-(水少)")

local excl_steps = I.parse_algebra_steps({ "木木", "!(口日)" })
ok(#excl_steps == 4 and excl_steps[3].item == "口"
   and excl_steps[3].op == "!" and excl_steps[4].item == "日"
   and excl_steps[4].op == "!", "括号解析 木木!(口日)")

ok(I.is_component_expression("渺-(水少)"), "括号表达式合法")
ok(not I.is_component_expression("渺-(水+少)"), "括号内运算符非法")
ok(not I.is_component_expression("渺-(水少"), "括号未闭合非法")

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
  shuangpin_scheme = "quanpin",
}
I.build_multi_component_candidates(store, { "mu", "kou" },
                                   { start = 0, _end = 5 }, cfg, nil)
local combo = {}
for i, cand in ipairs(emitted) do combo[i] = cand.text end
local combo_text = table.concat(combo, " ")
ok(contains(combo_text, "木口"), "Omukou 生成 木口 组合候选")

emitted = {}
I.build_multi_component_candidates(store, { "mu", "mu" },
                                   { start = 0, _end = 4 }, cfg, nil)
combo = {}
for i, cand in ipairs(emitted) do combo[i] = cand.text end
combo_text = table.concat(combo, " ")
ok(contains(combo_text, "木木"), "Omumu 生成 木木 组合候选")

local paren_flypy = I.parse_pending_components(store, "-(uv", "flypy")
ok(paren_flypy.valid and #paren_flypy.tokens == 1 and paren_flypy.tokens[1] == "shui",
   "括号 pending 小鹤双拼 -(uv -> shui")

local paren_quanpin = I.parse_pending_components(store, "-(shui", "quanpin")
ok(paren_quanpin.valid and #paren_quanpin.tokens == 1 and paren_quanpin.tokens[1] == "shui",
   "括号 pending 全拼 -(shui -> shui")

local paren_multi = I.parse_pending_components(store, "-(shuishao)", "quanpin")
ok(paren_multi.valid and #paren_multi.tokens == 2 and paren_multi.tokens[1] == "shui"
   and paren_multi.tokens[2] == "shao", "括号 pending 全拼 -(shuishao) -> shui shao")

_G.Candidate = saved_candidate
_G.yield = saved_yield

print(string.format("全部通过：%d 项", passed))
