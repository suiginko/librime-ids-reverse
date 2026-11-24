-- ids_reverse.lua — ids-reverse 插件的 Lua 5.5 实现。
--
-- 单文件提供四个 Rime 组件：
--   lua_processor@*ids_reverse*processor@ids_reverse
--   lua_segmentor@*ids_reverse*segmentor@ids_reverse
--   lua_translator@*ids_reverse*translator@ids_reverse
--   lua_filter@*ids_reverse*filter@ids_reverse
--
-- 直接读取 C++ 版使用的 IDS4 单文件数据 ids_data.bin。

local M = {}

-- ---------------------------------------------------------------------------
-- 常量与通用工具
-- ---------------------------------------------------------------------------

local SEC_CP2ID = 1
local SEC_ID2CP = 2
local SEC_HOT = 3
local SEC_COLD_IDX = 4
local SEC_ALTS = 5
local SEC_LEAVES = 6
local SEC_ATOMS = 7
local SEC_COMPSET = 8
local SEC_COLD = 9
local SEC_TOKENS = 10
local SEC_PY = 11
local SEC_KEY_LOOKUP = 12
local SEC_KEY_REC = 13
local SEC_POST = 14
local SEC_VAR_OFF = 15
local SEC_VAR = 16
local SEC_TOK_CANON = 17
local SEC_ATOMIC_SET = 18
local SEC_LEAF_STRUCT = 19

local NO_TOKEN = 0xFFFF
local INVALID_ID = 0xFFFFFFFF

local EXACT_STRUCTURE_BONUS = 20000000
local EXACT_STRUCTURE_FLOOR = 70000000

local function boost_exact_structure(score)
  score = score + EXACT_STRUCTURE_BONUS
  if score < EXACT_STRUCTURE_FLOOR then score = EXACT_STRUCTURE_FLOOR end
  return score
end

local HYBRID_STATE_PROPERTY = "ids_reverse_components"
local VARIANTS_PROPERTY = "ids_reverse_variants"
local VARIANTS_RESULT_PROPERTY = "ids_reverse_variants_result"
local RESULT_SEGMENT_TAG = "ids_reverse_result"
local SAVED_AUTO_COMMIT_PROPERTY = "ids_reverse_saved_auto_commit"

local BLOCK_NAMES = {
  [0] = "基本", [1] = "扩A", [2] = "扩B", [3] = "扩C", [4] = "扩D",
  [5] = "扩E", [6] = "扩F", [7] = "扩G", [8] = "扩H", [9] = "扩I",
  [10] = "扩J", [11] = "兼容区", [12] = "兼容补充", [13] = "部首补充",
  [14] = "康熙部首", [15] = "笔画", [16] = "CJK",
}

local IDC_LABELS = {
  [0] = "[左右]", [1] = "[上下]", [2] = "[左中右]", [3] = "[上中下]",
  [4] = "[全包]", [5] = "[上包]", [6] = "[下包]", [7] = "[左包]",
  [8] = "[左上包]", [9] = "[右上包]", [10] = "[左下包]", [11] = "[交叉]",
  [12] = "[右包]", [13] = "[右下包]", [14] = "[水平翻转]", [15] = "[旋转]",
}

local function log_error(msg)
  if _G.log and _G.log.error then _G.log.error(msg) end
end

local function starts_with(s, prefix)
  return prefix ~= "" and #s >= #prefix and s:sub(1, #prefix) == prefix
end

local function ends_with(s, suffix)
  return suffix ~= "" and #s >= #suffix and s:sub(#s - #suffix + 1) == suffix
end

local function ascii_lower(s)
  return (s:gsub("[A-Z]", function(c) return string.char(c:byte() + 32) end))
end

local function to_lower_ascii(s)
  return (s:gsub("[A-Z]", function(c) return string.char(c:byte() + 32) end))
end

local function is_ascii_alpha_only(s)
  if s == "" then return false end
  return s:match("^[A-Za-z]+$") ~= nil
end

local function has_whitespace(s)
  return s:find("[ \t\r\n]") ~= nil
end

local function trim(s)
  return (s:gsub("^[ \t\r]+", ""):gsub("[ \t\r\n]+$", ""))
end

local function join(parts, sep)
  return table.concat(parts, sep)
end

-- ---------------------------------------------------------------------------
-- UTF-8 / Canon / IDS 文本
-- ---------------------------------------------------------------------------

-- 与 C++ DecodeNext 一致：返回码点和下一个 0-based 字节位置。
local function decode_next(s, i)
  local n = #s
  if i >= n then return 0, i end
  local b1 = s:byte(i + 1)
  if b1 < 0x80 then return b1, i + 1 end
  if b1 < 0xE0 then
    if i + 1 >= n then return 0, i end
    local b2 = s:byte(i + 2)
    return (b1 - 0xC0) * 0x40 + (b2 - 0x80), i + 2
  end
  if b1 < 0xF0 then
    if i + 2 >= n then return 0, i end
    local b2, b3 = s:byte(i + 2, i + 3)
    return (b1 - 0xE0) * 0x1000 + (b2 - 0x80) * 0x40 + (b3 - 0x80), i + 3
  end
  if i + 3 >= n then return 0, i end
  local b2, b3, b4 = s:byte(i + 2, i + 4)
  return (b1 - 0xF0) * 0x40000 + (b2 - 0x80) * 0x1000
       + (b3 - 0x80) * 0x40 + (b4 - 0x80), i + 4
end

local function cp_to_utf8(cp)
  if cp < 0x80 then
    return string.char(cp)
  elseif cp < 0x800 then
    return string.char(0xC0 | (cp >> 6), 0x80 | (cp & 0x3F))
  elseif cp < 0x10000 then
    return string.char(0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F),
                       0x80 | (cp & 0x3F))
  end
  return string.char(0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F),
                     0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F))
end

local function utf8_chars(s)
  local out = {}
  local i, n = 0, #s
  while i < n do
    local old = i
    local cp
    cp, i = decode_next(s, i)
    if i == old or cp == 0 then break end
    out[#out + 1] = s:sub(old + 1, i)
  end
  return out
end

local function utf8_length(s)
  return #utf8_chars(s)
end

local function last_utf8_char_bytes(s)
  if s == "" then return 0 end
  local start = #s
  local steps = 0
  while start > 1 and steps < 4 and (s:byte(start) & 0xC0) == 0x80 do
    start = start - 1
    steps = steps + 1
  end
  return #s - start + 1
end

local function is_idc(cp)
  return cp >= 0x2FF0 and cp <= 0x2FFF
end

local function is_component_char(cp)
  if is_idc(cp) then return false end
  return (cp >= 0x2E80 and cp <= 0x2FDF)
      or (cp >= 0x31C0 and cp <= 0x31EF)
      or cp == 0x3007
      or (cp >= 0x3400 and cp <= 0x4DBF)
      or (cp >= 0x4E00 and cp <= 0x9FFF)
      or (cp >= 0xF900 and cp <= 0xFAFF)
      or (cp >= 0x20000 and cp <= 0x2FA1F)
      or (cp >= 0x30000 and cp <= 0x323AF)
end

local CANON_MAP = {
  ["氵"] = "⺡", ["水"] = "⺡", ["⺡"] = "⺡", ["氺"] = "⺡", ["灬"] = "⺣",
  ["火"] = "⺣", ["⺣"] = "⺣", ["亻"] = "⺅", ["人"] = "⺅", ["𠆢"] = "⺅",
  ["⺅"] = "⺅", ["扌"] = "龵", ["手"] = "龵", ["龵"] = "龵", ["忄"] = "⺖",
  ["心"] = "⺖", ["⺖"] = "⺖", ["⺗"] = "⺖", ["⺼"] = "肉", ["月"] = "肉",
  ["肉"] = "肉", ["⺝"] = "肉", ["艹"] = "⺾", ["艸"] = "⺾", ["⺾"] = "⺾",
  ["⻀"] = "⺾", ["草"] = "⺾", ["礻"] = "⺬", ["示"] = "⺬", ["⺬"] = "⺬",
  ["衤"] = "⺭", ["衣"] = "⺭", ["⺭"] = "⺭", ["𧘇"] = "⺭", ["釒"] = "⻐",
  ["金"] = "⻐", ["钅"] = "⻐", ["⻐"] = "⻐", ["⻑"] = "⻐", ["飠"] = "⻠",
  ["食"] = "⻠", ["饣"] = "⻠", ["⻠"] = "⻠", ["⻡"] = "⻠", ["𩙿"] = "⻠",
  ["糹"] = "⺰", ["糸"] = "⺰", ["纟"] = "⺰", ["⺰"] = "⺰", ["⺱"] = "⺰",
  ["犭"] = "⺲", ["犬"] = "⺲", ["⺲"] = "⺲", ["辶"] = "⻌", ["辵"] = "⻌",
  ["⻌"] = "⻌", ["⻍"] = "⻌", ["阝"] = "⻖", ["阜"] = "⻖", ["邑"] = "⻖",
  ["⻖"] = "⻖", ["⻗"] = "⻖", ["王"] = "⺩", ["⺩"] = "⺩", ["𤣩"] = "⺩",
  ["玉"] = "⺩", ["⺆"] = "冂", ["冂"] = "冂", ["攵"] = "⺙", ["攴"] = "⺙",
  ["⺙"] = "⺙", ["耂"] = "⺹", ["老"] = "⺹", ["⺹"] = "⺹", ["足"] = "⻊",
  ["⻊"] = "⻊", ["𧾷"] = "⻊", ["四"] = "⺫", ["罒"] = "⺫", ["网"] = "⺫",
  ["⺫"] = "⺫", ["竹"] = "⺮", ["⺮"] = "⺮", ["𥫗"] = "⺮", ["刀"] = "⺉",
  ["刂"] = "⺉", ["⺈"] = "⺉", ["⺉"] = "⺉", ["羊"] = "⺶", ["⺶"] = "⺶",
  ["⺷"] = "⺶", ["𦍌"] = "⺶", ["言"] = "讠", ["讠"] = "讠", ["訁"] = "讠",
  ["号"] = "号", ["號"] = "号", ["牛"] = "牜", ["牜"] = "牜", ["⺧"] = "牜",
  ["龍"] = "龙", ["龙"] = "龙", ["馬"] = "马", ["马"] = "马", ["鳥"] = "鸟",
  ["鸟"] = "鸟", ["魚"] = "鱼", ["鱼"] = "鱼", ["車"] = "车", ["车"] = "车",
  ["門"] = "门", ["门"] = "门", ["見"] = "见", ["见"] = "见", ["貝"] = "贝",
  ["贝"] = "贝", ["頁"] = "页", ["页"] = "页", ["風"] = "风", ["风"] = "风",
  ["飛"] = "飞", ["飞"] = "飞", ["黽"] = "黾", ["黾"] = "黾", ["齊"] = "齐",
  ["齐"] = "齐", ["齒"] = "齿", ["齿"] = "齿", ["長"] = "长", ["长"] = "长",
  ["撇"] = "㇒", ["丿"] = "㇒", ["㇒"] = "㇒", ["横"] = "㇐", ["一"] = "㇐",
  ["㇐"] = "㇐", ["竖"] = "㇑", ["丨"] = "㇑", ["㇑"] = "㇑", ["点"] = "㇔",
  ["丶"] = "㇔", ["㇔"] = "㇔", ["捺"] = "㇏", ["乀"] = "㇏", ["乁"] = "㇏",
  ["㇏"] = "㇏", ["提"] = "㇀", ["㇀"] = "㇀", ["折"] = "乛", ["乛"] = "乛",
  ["钩"] = "亅", ["鉤"] = "亅", ["亅"] = "亅",
}

local BLOCK_ALIAS_MAP = {
  ["基本"] = 0, ["基本区"] = 0, ["basic"] = 0, ["base"] = 0, ["cjk"] = 0,
  ["扩a"] = 1, ["exta"] = 1, ["ext-a"] = 1, ["a"] = 1,
  ["扩b"] = 2, ["extb"] = 2, ["ext-b"] = 2, ["b"] = 2,
  ["扩c"] = 3, ["extc"] = 3, ["ext-c"] = 3, ["c"] = 3,
  ["扩d"] = 4, ["extd"] = 4, ["ext-d"] = 4, ["d"] = 4,
  ["扩e"] = 5, ["exte"] = 5, ["ext-e"] = 5, ["e"] = 5,
  ["扩f"] = 6, ["extf"] = 6, ["ext-f"] = 6, ["f"] = 6,
  ["扩g"] = 7, ["extg"] = 7, ["ext-g"] = 7, ["g"] = 7,
  ["扩h"] = 8, ["exth"] = 8, ["ext-h"] = 8, ["h"] = 8,
  ["扩i"] = 9, ["exti"] = 9, ["ext-i"] = 9, ["i"] = 9,
  ["扩j"] = 10, ["extj"] = 10, ["ext-j"] = 10, ["j"] = 10,
  ["兼容区"] = 11, ["兼容"] = 11, ["compat"] = 11,
  ["兼容补充"] = 12, ["compat-sup"] = 12,
  ["部首补充"] = 13, ["rad-sup"] = 13,
  ["康熙部首"] = 14, ["kangxi"] = 14,
  ["笔画"] = 15, ["strokes"] = 15,
}

local function canon(c)
  if c == "" then return c end
  if c:sub(1, 1) == "{" and c:sub(-1) == "}" then
    local inner = c:sub(2, -2)
    return CANON_MAP[inner] or inner
  end
  return CANON_MAP[c] or c
end

local function block_name(block)
  return BLOCK_NAMES[block] or "基本"
end

local function format_ids(raw_ids, format)
  if format == "unicode" or format == nil or format == "" then
    return raw_ids
  end
  local out = {}
  local i, n = 0, #raw_ids
  while i < n do
    local old = i
    local cp
    cp, i = decode_next(raw_ids, i)
    if i == old then break end
    if is_idc(cp) then
      if format == "bracket" then
        out[#out + 1] = IDC_LABELS[cp - 0x2FF0] or raw_ids:sub(old + 1, i)
      elseif format ~= "plain" then
        out[#out + 1] = raw_ids:sub(old + 1, i)
      end
    else
      out[#out + 1] = raw_ids:sub(old + 1, i)
    end
  end
  return table.concat(out)
end

local function is_component_expression(text, max_chars)
  max_chars = max_chars or 32
  if text == "" or max_chars == 0 or has_whitespace(text) then return false end
  local count = 0
  local has_component = false
  local depth = 0
  local i, n = 0, #text
  while i < n do
    local cp
    cp, i = decode_next(text, i)
    if cp == 0 then return false end
    if cp == 0x28 or cp == 0x29 then
      if cp == 0x28 then
        depth = depth + 1
      else
        if depth == 0 then return false end
        depth = depth - 1
      end
      count = count + 1
    elseif cp == 0x2B or cp == 0x2D or cp == 0x21 then
      -- 括号内不允许再写运算符；出现即视为非法表达式。
      if depth > 0 then return false end
      count = count + 1
    else
      if not is_component_char(cp) then return false end
      has_component = true
      count = count + 1
    end
    if count > max_chars then return false end
  end
  return has_component and depth == 0
end

local function is_component_code_point(cp)
  return is_component_char(cp)
end

-- ---------------------------------------------------------------------------
-- IDS4 数据读取
-- ---------------------------------------------------------------------------

local function read_file(path)
  local f, err = io.open(path, "rb")
  if not f then return nil, err end
  local data = f:read("a")
  f:close()
  if not data or data == "" then return nil, "empty file" end
  return data
end

local function rd16(blob, off)
  return (string.unpack("<I2", blob, off + 1))
end

local function rd32(blob, off)
  return (string.unpack("<I4", blob, off + 1))
end

local function rd64(blob, off)
  return (string.unpack("<I8", blob, off + 1))
end

local function rd8(blob, off)
  return string.byte(blob, off + 1)
end

local function leaf_struct_hash(tokens)
  local h = 2166136261
  local n = #tokens
  h = (h ~ (n & 0xFF)) * 16777619 & 0xFFFFFFFF
  h = (h ~ ((n >> 8) & 0xFF)) * 16777619 & 0xFFFFFFFF
  for i = 1, n do
    local t = tokens[i]
    h = (h ~ (t & 0xFF)) * 16777619 & 0xFFFFFFFF
    h = (h ~ ((t >> 8) & 0xFF)) * 16777619 & 0xFFFFFFFF
  end
  return h
end

local TOKEN_ARRAY_FORMAT = {}
local U32_ARRAY_FORMAT = {}

local function array_format(cache, n, unit)
  local fmt = cache[n]
  if not fmt then
    fmt = "<" .. string.rep(unit, n)
    cache[n] = fmt
  end
  return fmt
end

local function read_token_array(blob, off)
  local n = rd16(blob, off)
  local out = {}
  if n == 0 then return out end
  local values = { string.unpack(array_format(TOKEN_ARRAY_FORMAT, n, "I2"),
                                 blob, off + 3) }
  for i = 1, n do out[i] = values[i] end
  return out
end

local Store = {}
Store.__index = Store

local function is_absolute_path(path)
  return path:sub(1, 1) == "/" or path:sub(1, 1) == "\\"
      or path:match("^%a:[/\\]") ~= nil
end

local function path_join(a, b)
  local sep = package.config:sub(1, 1)
  if a:sub(-1) == "/" or a:sub(-1) == "\\" then return a .. b end
  return a .. sep .. b
end

local function resolve_data_path(filename)
  if is_absolute_path(filename) then return filename end
  local ud = _G.rime_api and _G.rime_api.get_user_data_dir()
  local sd = _G.rime_api and _G.rime_api.get_shared_data_dir()
  local dirs = {}
  if ud then dirs[#dirs + 1] = ud end
  if sd then dirs[#dirs + 1] = sd end
  for _, dir in ipairs(dirs) do
    local p = path_join(dir, filename)
    local f = io.open(p, "rb")
    if f then f:close(); return p end
  end
  if ud then return path_join(ud, filename) end
  return filename
end

function Store.load(path, custom_path)
  local blob, err = read_file(path)
  if not blob then return nil, err or ("cannot open " .. path) end
  if #blob < 32 or blob:sub(1, 4) ~= "IDS4" then
    return nil, "invalid IDS4 file"
  end

  local self = setmetatable({}, Store)
  self.path = path
  self.custom_path = custom_path
  self.blob = blob
  self.size = #blob

  local magic, version, flags, max_cp, num_chars, nkeys, nsec =
      string.unpack("<c4HHIIII", blob, 1)
  if version ~= 2 then
    return nil, "old IDS4 data (version " .. tostring(version) ..
                "), please regenerate ids_data.bin"
  end
  self.version = version
  self.flags = flags
  self.max_cp = max_cp
  self.num_chars = num_chars
  self.num_keys = nkeys
  self.num_tokens = 0
  self.sections = {}

  if #blob < 32 + nsec * 24 then return nil, "truncated section table" end
  for i = 0, nsec - 1 do
    local type, pad, off, len = string.unpack("<I4I4I8I8", blob, 33 + i * 24)
    if off + len > #blob or off > #blob then return nil, "bad section" end
    self.sections[type] = { off = off, len = len }
  end

  local required = {
    SEC_CP2ID, SEC_ID2CP, SEC_HOT, SEC_COLD_IDX, SEC_ALTS, SEC_LEAVES,
    SEC_ATOMS, SEC_COMPSET, SEC_COLD, SEC_POST, SEC_KEY_LOOKUP, SEC_TOKENS,
    SEC_PY, SEC_KEY_REC, SEC_LEAF_STRUCT,
  }
  for _, t in ipairs(required) do
    if not self.sections[t] then return nil, "missing section " .. t end
  end

  self.cp2id_off = self.sections[SEC_CP2ID].off
  self.id2cp_off = self.sections[SEC_ID2CP].off
  self.hot_off = self.sections[SEC_HOT].off
  self.cold_idx_off = self.sections[SEC_COLD_IDX].off
  self.alts_off = self.sections[SEC_ALTS].off
  self.leaves_off = self.sections[SEC_LEAVES].off
  self.atoms_off = self.sections[SEC_ATOMS].off
  self.compset_off = self.sections[SEC_COMPSET].off
  self.cold_off = self.sections[SEC_COLD].off
  self.post_off = self.sections[SEC_POST].off
  self.key_lookup_off = self.sections[SEC_KEY_LOOKUP].off
  self.key_rec_off = self.sections[SEC_KEY_REC].off
  if self.sections[SEC_VAR_OFF] then
    self.var_off_off = self.sections[SEC_VAR_OFF].off
  end
  if self.sections[SEC_VAR] then
    self.var_off_data = self.sections[SEC_VAR].off
  end
  if self.sections[SEC_TOK_CANON] then
    self.tok_canon_off = self.sections[SEC_TOK_CANON].off
  end
  if self.sections[SEC_ATOMIC_SET] then
    self.atomic_set_off = self.sections[SEC_ATOMIC_SET].off
  end
  self.leaf_struct_off = self.sections[SEC_LEAF_STRUCT].off
  self.leaf_struct_count = rd32(blob, self.leaf_struct_off)
  if 4 + self.leaf_struct_count * 8 > self.sections[SEC_LEAF_STRUCT].len then
    return nil, "bad LEAF_STRUCT section"
  end

  self.hot_cache = {}
  self.cold_cache = {}
  self.compset_cache = {}
  self.leaves_cache = {}
  self.atoms_cache = {}
  self.variants_cache = {}

  self:parse_tokens()
  self:parse_py()
  self:parse_key_recs()
  self:parse_atomic_set()
  self.custom_disabled_blocks = {}
  if custom_path and custom_path ~= "" then
    self:apply_custom_rules(custom_path)
  end
  return self
end

function Store:id_of_cp(cp)
  if cp == 0 or cp >= self.max_cp then return INVALID_ID end
  local v = rd32(self.blob, self.cp2id_off + cp * 4)
  if v == 0 then return INVALID_ID end
  return v - 1
end

function Store:cp_of_id(id)
  if id >= self.num_chars then return 0 end
  return rd32(self.blob, self.id2cp_off + id * 4)
end

function Store:hot(id)
  if id >= self.num_chars then
    return { freq = 0, flags = 0, char_token = NO_TOKEN, depth = 0,
             block = 0, alts_off = 0, leaf_off = 0, atom_off = 0 }
  end
  local hit = self.hot_cache[id]
  if hit then return hit end
  local freq, flags, char_token, depth, block, alts_off, leaf_off, atom_off =
      string.unpack("<I4I4I2I1I1I4I4I4", self.blob, self.hot_off + id * 24 + 1)
  local rec = { freq = freq, flags = flags, char_token = char_token,
                depth = depth, block = block, alts_off = alts_off,
                leaf_off = leaf_off, atom_off = atom_off }
  self.hot_cache[id] = rec
  return rec
end

function Store:cold_idx(id)
  local off = self.cold_idx_off + id * 8
  local cs_off, cold_off = string.unpack("<I4I4", self.blob, off + 1)
  return cs_off, cold_off
end

function Store:cold(id)
  if id >= self.num_chars then return "", "" end
  local hit = self.cold_cache[id]
  if hit then return hit[1], hit[2] end
  local _, cold_off = self:cold_idx(id)
  local p = self.cold_off + cold_off
  local py_len = rd16(self.blob, p)
  local py = self.blob:sub(p + 3, p + 2 + py_len)
  p = p + 2 + py_len
  local ids_len = rd16(self.blob, p)
  local ids = self.blob:sub(p + 3, p + 2 + ids_len)
  self.cold_cache[id] = { py, ids }
  return py, ids
end

function Store:comp_set(id)
  if id >= self.num_chars then return {} end
  local hit = self.compset_cache[id]
  if hit then return hit end
  local cs_off = self:cold_idx(id)
  local out = read_token_array(self.blob, self.compset_off + cs_off)
  self.compset_cache[id] = out
  return out
end

function Store:leaves(id)
  if id >= self.num_chars then return {} end
  local hit = self.leaves_cache[id]
  if hit then return hit end
  local h = self:hot(id)
  local out = read_token_array(self.blob, self.leaves_off + h.leaf_off)
  self.leaves_cache[id] = out
  return out
end

function Store:lookup_leaf_structure(tokens)
  local out = {}
  if not tokens or #tokens == 0 then return out end
  local want = {}
  for i = 1, #tokens do want[i] = self:canon_token(tokens[i]) end
  table.sort(want)
  local hash = leaf_struct_hash(want)
  local lo, hi = 0, self.leaf_struct_count
  while lo < hi do
    local mid = lo + (hi - lo) // 2
    local h = rd32(self.blob, self.leaf_struct_off + 4 + mid * 8)
    if h < hash then lo = mid + 1 else hi = mid end
  end
  local got = {}
  for i = lo, self.leaf_struct_count - 1 do
    local base = self.leaf_struct_off + 4 + i * 8
    if rd32(self.blob, base) ~= hash then break end
    local id = rd32(self.blob, base + 4)
    local lv = self:leaves(id)
    if #lv == #want then
      got = {}
      for j = 1, #lv do got[j] = self:canon_token(lv[j]) end
      table.sort(got)
      local same = true
      for j = 1, #want do
        if got[j] ~= want[j] then same = false; break end
      end
      if same then out[#out + 1] = id end
    end
  end
  return out
end

function Store:atomics(id)
  if id >= self.num_chars then return {} end
  local hit = self.atoms_cache[id]
  if hit then return hit end
  local h = self:hot(id)
  local out = read_token_array(self.blob, self.atoms_off + h.atom_off)
  self.atoms_cache[id] = out
  return out
end

function Store:parse_tokens()
  local sec = self.sections[SEC_TOKENS]
  local p = sec.off
  local endp = sec.off + sec.len
  if p + 2 > endp then return false end
  local n = rd16(self.blob, p)
  p = p + 2
  self.num_tokens = n
  self.token_str = {}
  self.token_cp = {}
  self.token_index = {}
  for t = 0, n - 1 do
    if p + 2 > endp then return false end
    local len = rd16(self.blob, p)
    p = p + 2
    if p + len + 4 > endp then return false end
    local s = self.blob:sub(p + 1, p + len)
    p = p + len
    local cp = rd32(self.blob, p)
    p = p + 4
    self.token_str[t] = s
    self.token_cp[t] = cp
    self.token_index[s] = t
  end
  if self.sections[SEC_TOK_CANON] then
    self.tok_canon = {}
    for t = 0, n - 1 do
      self.tok_canon[t] = rd16(self.blob, self.tok_canon_off + t * 2)
    end
  end
  return true
end

function Store:parse_py()
  local sec = self.sections[SEC_PY]
  local p = sec.off
  local endp = sec.off + sec.len
  if p + 2 > endp then return false end
  local np = rd16(self.blob, p)
  p = p + 2
  self.py_map = {}
  self.valid_pinyins = {}
  for _ = 1, np do
    if p + 2 > endp then return false end
    local len = rd16(self.blob, p)
    p = p + 2
    if p + len + 2 > endp then return false end
    local py = self.blob:sub(p + 1, p + len)
    p = p + len
    local m = rd16(self.blob, p)
    p = p + 2
    local ids = {}
    for j = 1, m do
      ids[j] = rd16(self.blob, p)
      p = p + 2
    end
    self.py_map[py] = ids
    self.valid_pinyins[py] = true
  end
  return true
end

function Store:parse_key_recs()
  self.key_lookup = {}
  for t = 0, self.num_tokens do
    self.key_lookup[t] = rd32(self.blob, self.key_lookup_off + t * 4)
  end
  self.key_recs = {}
  local p = self.key_rec_off
  local endp = self.key_rec_off + self.sections[SEC_KEY_REC].len
  local i = 0
  while p + 20 <= endp do
    local mult, kind, _, head_off, head_cnt, full_off, full_cnt =
        string.unpack("<I1I1I2I4I4I4I4", self.blob, p + 1)
    self.key_recs[i] = { mult = mult, kind = kind, head_off = head_off,
                         head_cnt = head_cnt, full_off = full_off,
                         full_cnt = full_cnt }
    p = p + 20
    i = i + 1
  end
end

function Store:parse_atomic_set()
  self.atomic_tokens = {}
  if not self.atomic_set_off then return end
  local sec = self.sections[SEC_ATOMIC_SET]
  local p = sec.off
  local endp = sec.off + sec.len
  if p + 2 > endp then return end
  local n = rd16(self.blob, p)
  p = p + 2
  if p + n * 2 > endp then return end
  for _ = 1, n do
    self.atomic_tokens[rd16(self.blob, p)] = true
    p = p + 2
  end
end

function Store:token_str_of(t)
  return self.token_str[t] or ""
end

function Store:token_cp_of(t)
  return self.token_cp[t] or 0
end

function Store:canon_token(t)
  if self.tok_canon and self.tok_canon[t] ~= nil then return self.tok_canon[t] end
  return t
end

function Store:find_token(s)
  return self.token_index[s]
end

function Store:resolve_term(s)
  local t = self:find_token(s)
  if t ~= nil then return t end
  local c = canon(s)
  if c ~= s then return self:find_token(c) end
  return nil
end

function Store:is_atomic_token(t)
  return self.atomic_tokens[t] == true
end

function Store:find_key_by_token(token, mult)
  if token >= self.num_tokens then return nil end
  local b = self.key_lookup[token] or 0
  local e = self.key_lookup[token + 1] or b
  for i = b, e - 1 do
    local rec = self.key_recs[i]
    if rec and rec.mult == mult then return rec end
  end
  return nil
end

function Store:find_key(k)
  if k == "" then return nil end
  local mult = 1
  local base = k
  local star = k:find("*", 1, true)
  if star and star < #k then
    local suffix = k:sub(star + 1)
    if suffix:match("^%d+$") then
      local m = tonumber(suffix)
      if m and m > 1 then
        mult = m
        base = k:sub(1, star - 1)
      end
    end
  end
  local token = self:find_token(base)
  if token == nil then return nil end
  return self:find_key_by_token(token, mult)
end

function Store:has_key(k)
  return self:find_key(k) ~= nil
end

function Store:head_ids(entry, count)
  local n = count or entry.head_cnt
  local out = {}
  if n == 0 then return out end
  local values = { string.unpack(array_format(U32_ARRAY_FORMAT, n, "I4"),
                                 self.blob,
                                 self.post_off + entry.head_off + 1) }
  for i = 1, n do out[i] = values[i] end
  return out
end

function Store:full_contains(entry, id)
  if entry.kind == 0 then
    local lo, hi = 0, entry.full_cnt - 1
    local base = self.post_off + entry.full_off
    while lo <= hi do
      local mid = (lo + hi) // 2
      local v = rd32(self.blob, base + mid * 4)
      if v == id then return true end
      if v < id then lo = mid + 1 else hi = mid - 1 end
    end
    return false
  end
  local base = self.post_off + entry.full_off
  local word = rd64(self.blob, base + (id >> 6) * 8)
  return ((word >> (id & 63)) & 1) ~= 0
end

function Store:for_each_full(entry, f)
  if entry.kind == 0 then
    local base = self.post_off + entry.full_off
    local remaining = entry.full_cnt
    local pos = base
    local chunk = 256
    while remaining > 0 do
      local n = remaining < chunk and remaining or chunk
      local values = { string.unpack(array_format(U32_ARRAY_FORMAT, n, "I4"),
                                     self.blob, pos + 1) }
      for i = 1, n do
        if f(values[i]) == false then return end
      end
      pos = pos + n * 4
      remaining = remaining - n
    end
  else
    local base = self.post_off + entry.full_off
    local words = (self.num_chars + 63) >> 6
    for w = 0, words - 1 do
      local word = rd64(self.blob, base + w * 8)
      if word ~= 0 then
        for bit = 0, 63 do
          if ((word >> bit) & 1) ~= 0 then
            local id = w * 64 + bit
            if id >= self.num_chars then break end
            if f(id) == false then return end
          end
        end
      end
    end
  end
end

function Store:variants(id)
  if id >= self.num_chars or not self.var_off_off then return {} end
  local hit = self.variants_cache[id]
  if hit then return hit end
  local off = rd32(self.blob, self.var_off_off + id * 4)
  local p = self.var_off_data + off
  local n = rd16(self.blob, p)
  local out = {}
  for i = 1, n do
    out[i] = rd32(self.blob, p + 2 + (i - 1) * 4)
  end
  self.variants_cache[id] = out
  return out
end

local function remove_token_from_list(list, token)
  for i = #list, 1, -1 do
    if list[i] == token then table.remove(list, i) end
  end
end

function Store:apply_custom_rules(path)
  local f = io.open(path, "r")
  if not f then return end

  local function add_token_pair(list, comp)
    local t = self:find_token(comp)
    if t ~= nil then
      local found = false
      for _, v in ipairs(list) do if v == t then found = true; break end end
      if not found then list[#list + 1] = t end
    end
    local c = canon(comp)
    if c ~= comp then
      t = self:find_token(c)
      if t ~= nil then
        local found = false
        for _, v in ipairs(list) do if v == t then found = true; break end end
        if not found then list[#list + 1] = t end
      end
    end
  end

  local function remove_token_pair(list, comp)
    local t = self:find_token(comp)
    if t ~= nil then remove_token_from_list(list, t) end
    local c = canon(comp)
    if c ~= comp then
      t = self:find_token(c)
      if t ~= nil then remove_token_from_list(list, t) end
    end
  end

  for raw_line in f:lines() do
    local line = raw_line
    local hash = line:find("#", 1, true)
    if hash then line = line:sub(1, hash - 1) end
    line = trim(line)
    if line ~= "" then
      local tokens = {}
      for tok in line:gmatch("%S+") do tokens[#tokens + 1] = tok end
      if #tokens >= 2 and
         (tokens[1] == "-block" or tokens[1] == "-区" or tokens[1] == "-分区") then
        for k = 2, #tokens do
          local b = BLOCK_ALIAS_MAP[to_lower_ascii(tokens[k])]
          if b ~= nil then self.custom_disabled_blocks[b] = true end
        end
      elseif #tokens == 1 and tokens[1]:sub(1, 1) == "-" then
        local del_py = to_lower_ascii(tokens[1]:sub(2))
        self.py_map[del_py] = nil
        self.valid_pinyins[del_py] = nil
      elseif #tokens >= 2 then
        local comp = tokens[1]
        local has_del = false
        local has_override = false
        for k = 2, #tokens do
          local tok = tokens[k]
          if tok:sub(1, 1) == "-" then has_del = true
          elseif tok:sub(1, 1) == "=" then has_override = true end
        end
        if has_del then
          for k = 2, #tokens do
            local tok = tokens[k]
            if tok:sub(1, 1) == "-" then
              local del_py = to_lower_ascii(tok:sub(2))
              local list = self.py_map[del_py]
              if list then
                remove_token_pair(list, comp)
                if #list == 0 then self.py_map[del_py] = nil end
              end
            end
          end
        elseif has_override then
          local keys = {}
          for py in pairs(self.py_map) do keys[#keys + 1] = py end
          for _, py in ipairs(keys) do
            remove_token_pair(self.py_map[py], comp)
          end
          for k = 2, #tokens do
            local tok = tokens[k]
            local py = tok:sub(1, 1) == "=" and tok:sub(2) or tok
            py = to_lower_ascii(py)
            if py ~= "" then
              self.py_map[py] = self.py_map[py] or {}
              add_token_pair(self.py_map[py], comp)
              self.valid_pinyins[py] = true
            end
          end
        else
          for k = 2, #tokens do
            local py = to_lower_ascii(tokens[k])
            if py ~= "" then
              self.py_map[py] = self.py_map[py] or {}
              add_token_pair(self.py_map[py], comp)
              self.valid_pinyins[py] = true
            end
          end
        end
      end
    end
  end
  f:close()
end

local store_instance = nil

local function ensure_store(config)
  local data = resolve_data_path(config.data_path)
  local custom = ""
  if config.custom_path and config.custom_path ~= "" then
    custom = resolve_data_path(config.custom_path)
  end
  if store_instance and store_instance.path == data
     and (store_instance.custom_path or "") == custom then
    local s = store_instance
    for b in pairs(s.custom_disabled_blocks) do
      config.disabled_blocks[b] = true
      config.disabled_blocks_enabled = true
    end
    return s
  end
  local s, err = Store.load(data, custom)
  if not s then
    log_error("ids_reverse: cannot load data file: '" .. data .. "': " ..
              tostring(err))
    return nil
  end
  store_instance = s
  for b in pairs(s.custom_disabled_blocks) do
    config.disabled_blocks[b] = true
    config.disabled_blocks_enabled = true
  end
  return s
end

-- ---------------------------------------------------------------------------
-- 拼音：全拼切分与双拼解码
-- ---------------------------------------------------------------------------

local ZERO_INITIALS = {
  ["a"] = true, ["o"] = true, ["e"] = true, ["ai"] = true, ["ao"] = true,
  ["an"] = true, ["ang"] = true, ["ou"] = true, ["er"] = true, ["en"] = true,
  ["eng"] = true, ["ei"] = true,
}
local INITIALS2 = { ["zh"] = true, ["ch"] = true, ["sh"] = true }
local INITIALS1 = {
  ["b"] = true, ["p"] = true, ["m"] = true, ["f"] = true, ["d"] = true,
  ["t"] = true, ["n"] = true, ["l"] = true, ["g"] = true, ["k"] = true,
  ["h"] = true, ["j"] = true, ["q"] = true, ["x"] = true, ["r"] = true,
  ["z"] = true, ["c"] = true, ["s"] = true, ["y"] = true, ["w"] = true,
}

local FLYPY_FINALS = {
  q = "iu", w = "ei", e = "e", r = "uan", y = "un", u = "u", i = "i",
  p = "ie", a = "a", d = "ai", f = "en", g = "eng", h = "ang", j = "an",
  z = "ou", c = "ao", b = "in", n = "iao", m = "ian",
}
local ZRM_FINALS = {
  q = "iu", w = "ia", r = "uan", p = "un", u = "u", i = "i", f = "en",
  g = "eng", h = "ang", j = "an", k = "ao", l = "ai", z = "ou", c = "ao",
  b = "in", m = "ian",
}

local function sp_init(c)
  if c == "v" then return "zh" end
  if c == "i" then return "ch" end
  if c == "u" then return "sh" end
  return nil
end

local function sp_initials(c)
  if c == "v" then return "zh" end
  if c == "i" then return "ch" end
  if c == "u" then return "sh" end
  if c == "a" then return "a" end
  if c == "o" then return "o" end
  if c == "e" then return "e" end
  return nil
end

local function is_in(value, list)
  for i = 1, #list do
    if value == list[i] then return true end
  end
  return false
end

local function decode_flypy_syllable(code)
  if #code ~= 2 then return "" end
  local c1, c2 = code:sub(1, 1), code:sub(2, 2)
  if c1 == "a" then
    if c2 == "a" then return "a" end
    if c2 == "i" or c2 == "n" or c2 == "o" then return "a" .. c2 end
    if c2 == "h" then return "ang" end
  elseif c1 == "o" then
    if c2 == "o" then return "o" end
    if c2 == "u" then return "ou" end
  elseif c1 == "e" then
    if c2 == "e" then return "e" end
    if c2 == "i" or c2 == "n" or c2 == "r" then return "e" .. c2 end
    if c2 == "g" then return "eng" end
  end
  local init = sp_init(c1) or c1
  local final
  if c2 == "k" then
    final = is_in(init, { "zh", "ch", "sh", "g", "k", "h" }) and "uai" or "ing"
  elseif c2 == "l" then
    final = is_in(init, { "zh", "ch", "sh", "g", "k", "h", "w" }) and "uang" or "iang"
  elseif c2 == "x" then
    final = is_in(init, { "zh", "ch", "sh", "g", "k", "h" }) and "ua" or "ia"
  elseif c2 == "s" then
    final = is_in(init, { "j", "q", "x" }) and "iong" or "ong"
  elseif c2 == "t" then
    final = "ue"
  elseif c2 == "v" then
    if is_in(init, { "n", "l" }) then return init .. "v" end
    if is_in(init, { "j", "q", "x", "y" }) then return init .. "u" end
    final = "ui"
  elseif c2 == "o" then
    final = is_in(init, { "b", "p", "m", "f" }) and "o" or "uo"
  else
    final = FLYPY_FINALS[c2]
  end
  if not final then return "" end
  if is_in(init, { "j", "q", "x", "y" }) and final == "ue" then
    return init .. "ue"
  end
  if is_in(init, { "l", "n" }) and final == "ue" then
    return init .. "ve"
  end
  return init .. final
end

local function decode_zrm_syllable(code)
  if #code ~= 2 then return "" end
  local c1, c2 = code:sub(1, 1), code:sub(2, 2)
  if c1 == "a" then
    if c2 == "a" then return "a" end
    if c2 == "i" or c2 == "n" or c2 == "o" then return "a" .. c2 end
    if c2 == "h" then return "ang" end
  elseif c1 == "o" then
    if c2 == "o" then return "o" end
    if c2 == "u" then return "ou" end
  elseif c1 == "e" then
    if c2 == "e" then return "e" end
    if c2 == "i" or c2 == "n" or c2 == "r" then return "e" .. c2 end
    if c2 == "g" then return "eng" end
  end
  local init = sp_init(c1) or c1
  local final
  if c2 == "y" then
    final = is_in(init, { "zh", "ch", "sh", "g", "k", "h" }) and "uai" or "ing"
  elseif c2 == "d" then
    final = is_in(init, { "zh", "ch", "sh", "g", "k", "h" }) and "uang" or "iang"
  elseif c2 == "w" then
    final = is_in(init, { "zh", "ch", "sh", "g", "k", "h" }) and "ua" or "ia"
  elseif c2 == "s" then
    final = is_in(init, { "j", "q", "x" }) and "iong" or "ong"
  elseif c2 == "t" then
    final = "ue"
  elseif c2 == "v" then
    if is_in(init, { "n", "l" }) then return init .. "v" end
    if is_in(init, { "j", "q", "x", "y" }) then return init .. "u" end
    final = "ui"
  elseif c2 == "o" then
    final = is_in(init, { "b", "p", "m", "f" }) and "o" or "uo"
  else
    final = ZRM_FINALS[c2]
  end
  if not final then return "" end
  return init .. final
end

local function has_separator(s)
  return s:find("['/._ \t\r\n]") ~= nil
end

local function split_separator(s)
  local parts = {}
  for part in s:gmatch("[^'/%._ \t\r\n]+") do parts[#parts + 1] = part end
  return parts
end

local function decode_shuangpin_string(store, s, scheme)
  local decoder = scheme == "zrm" and decode_zrm_syllable or decode_flypy_syllable
  local parts = has_separator(s) and split_separator(s) or { s }
  local res = {}
  for _, part in ipairs(parts) do
    local n = #part
    local pair_count = n // 2
    for i = 0, pair_count - 1 do
      local syl = decoder(part:sub(i * 2 + 1, i * 2 + 2))
      if syl ~= "" and store.valid_pinyins[syl] then
        res[#res + 1] = syl
      else
        return nil
      end
    end
    if n % 2 == 1 then
      local rem = part:sub(n, n)
      res[#res + 1] = sp_initials(rem) or rem
    end
  end
  return res
end

local function segment_pinyin_optimal(store, s)
  local n = #s
  if n == 0 then return {} end
  local memo = {}
  for i = 0, n do memo[i] = { set = false, cost = 0, segs = {} } end

  local function dfs(idx)
    if idx >= n then
      memo[idx].set = true
      memo[idx].cost = 0
      memo[idx].segs = {}
      return
    end
    if memo[idx].set then return end
    local best_cost = 1000000000
    local best_segs = {}
    local max_len = math.min(6, n - idx)
    local found = false
    for l = max_len, 1, -1 do
      local sub = s:sub(idx + 1, idx + l)
      local is_valid = store.valid_pinyins[sub] == true
                    or store.py_map[sub] ~= nil
      local is_single_init = (l == 1 and INITIALS1[sub])
                          or (l == 2 and INITIALS2[sub])
      if is_valid or is_single_init then
        local step_cost
        if is_valid then
          step_cost = 10 - l
          if ZERO_INITIALS[sub] and idx > 0 then
            local prev = s:sub(idx, idx)
            if prev == "n" or prev == "g" or prev == "r" then
              step_cost = step_cost + 15
            end
          end
        else
          step_cost = 30 + (idx < 2 and 0 or idx * 2)
        end
        dfs(idx + l)
        local total = step_cost + memo[idx + l].cost
        if total < best_cost then
          best_cost = total
          found = true
          best_segs = { sub }
          for _, seg in ipairs(memo[idx + l].segs) do
            best_segs[#best_segs + 1] = seg
          end
        end
      end
    end
    if not found then
      local sub = s:sub(idx + 1, idx + 1)
      dfs(idx + 1)
      best_cost = 100 + memo[idx + 1].cost
      best_segs = { sub }
      for _, seg in ipairs(memo[idx + 1].segs) do
        best_segs[#best_segs + 1] = seg
      end
    end
    memo[idx].set = true
    memo[idx].cost = best_cost
    memo[idx].segs = best_segs
  end

  dfs(0)
  return memo[0].segs
end

local seg_cache = {}
local prefix_cache = {}
local matcher_cache = {}
local rank_cache = {}
local query_cache = {}
local term_cache = {}
local hybrid_cache = {}

local function cache_get(cache, key)
  return cache[key]
end

local function cache_put(cache, key, value, limit)
  local n = cache["\0n"] or 0
  if cache[key] == nil then n = n + 1 end
  if limit and n > limit then
    for k in pairs(cache) do
      if k ~= "\0n" then cache[k] = nil end
    end
    n = 1
  end
  cache["\0n"] = n
  cache[key] = value
end

local function clear_query_caches()
  for _, cache in ipairs({ seg_cache, prefix_cache, matcher_cache, rank_cache,
                           query_cache, term_cache, hybrid_cache }) do
    for k in pairs(cache) do cache[k] = nil end
  end
end

local function segment_alpha_string(store, chunk, mode)
  if chunk == "" then return {} end
  local key = chunk .. ":" .. mode
  local hit = cache_get(seg_cache, key)
  if hit then return hit end
  local py_map = store.py_map
  if py_map[chunk] and (mode == "quanpin" or #chunk == 2) then
    local r = { chunk }
    cache_put(seg_cache, key, r, 2048)
    return r
  end
  if mode == "flypy" or mode == "zrm" then
    local segs = segment_pinyin_optimal(store, chunk)
    if #segs > 0 then
      local all_valid = true
      for _, seg in ipairs(segs) do
        if not store.valid_pinyins[seg] then all_valid = false; break end
      end
      if all_valid then
        cache_put(seg_cache, key, segs, 2048)
        return segs
      end
    end
    local sp = decode_shuangpin_string(store, chunk, mode)
    if sp and #sp > 0 then
      cache_put(seg_cache, key, sp, 2048)
      return sp
    end
    if #segs > 0 then
      cache_put(seg_cache, key, segs, 2048)
      return segs
    end
    local r = { chunk }
    cache_put(seg_cache, key, r, 2048)
    return r
  end
  local segs = segment_pinyin_optimal(store, chunk)
  if #segs > 0 then
    local has_single_frag = false
    for _, seg in ipairs(segs) do
      if #seg == 1 and not store.valid_pinyins[seg] then
        has_single_frag = true
        break
      end
    end
    if (has_single_frag or #chunk <= 4) and #chunk >= 2 and #chunk % 2 == 0 then
      local sp = decode_shuangpin_string(store, chunk, "flypy")
      if sp then
        local all_valid = #sp > 0
        for _, seg in ipairs(sp) do
          if not store.valid_pinyins[seg] then all_valid = false; break end
        end
        if all_valid then
          cache_put(seg_cache, key, sp, 2048)
          return sp
        end
      end
      sp = decode_shuangpin_string(store, chunk, "zrm")
      if sp then
        local all_valid = #sp > 0
        for _, seg in ipairs(sp) do
          if not store.valid_pinyins[seg] then all_valid = false; break end
        end
        if all_valid then
          cache_put(seg_cache, key, sp, 2048)
          return sp
        end
      end
    end
    cache_put(seg_cache, key, segs, 2048)
    return segs
  end
  local r = { chunk }
  cache_put(seg_cache, key, r, 2048)
  return r
end

-- ---------------------------------------------------------------------------
-- 通用查询：term / posting / 打分
-- ---------------------------------------------------------------------------

local function make_matcher(tokens)
  local m = { list = tokens, set = {} }
  for _, t in ipairs(tokens) do m.set[t] = true end
  return m
end

local function matcher_has(m, t)
  return m.set[t] == true
end

local function get_comps_by_prefix(store, pref)
  if pref == "" then return nil end
  local hit = cache_get(prefix_cache, pref)
  if hit then
    if #hit == 0 then return nil end
    return hit
  end
  local res = {}
  local seen = {}
  local pref_len = #pref
  for py, tokens in pairs(store.py_map) do
    if #py >= pref_len and py:sub(1, pref_len) == pref then
      for _, t in ipairs(tokens) do
        if not seen[t] then
          seen[t] = true
          res[#res + 1] = t
        end
      end
    end
  end
  if #res > 1 then
    table.sort(res, function(a, b)
      local function freq(t)
        local cp = store:token_cp_of(t)
        local id = cp ~= 0 and store:id_of_cp(cp) or INVALID_ID
        return id == INVALID_ID and 0 or store:hot(id).freq
      end
      return freq(a) > freq(b)
    end)
  end
  cache_put(prefix_cache, pref, res, 128)
  if #res == 0 then return nil end
  return res
end

local function get_term_matcher(store, term)
  local hit = cache_get(matcher_cache, term)
  if hit then return hit end
  local v = {}
  local py = store.py_map[term]
  if py and #py > 0 then
    for _, t in ipairs(py) do v[#v + 1] = t end
  else
    local pref = get_comps_by_prefix(store, term)
    if pref and #pref > 0 then
      for _, t in ipairs(pref) do v[#v + 1] = t end
    else
      local t = store:find_token(term)
      if t ~= nil then v[#v + 1] = t end
      local c = canon(term)
      if c ~= term then
        t = store:find_token(c)
        if t ~= nil then v[#v + 1] = t end
      end
    end
  end
  table.sort(v)
  local uniq = {}
  local last
  for _, t in ipairs(v) do
    if t ~= last then uniq[#uniq + 1] = t; last = t end
  end
  local m = make_matcher(uniq)
  cache_put(matcher_cache, term, m, 128)
  return m
end

local function get_direct_comp_rank(store, term)
  local hit = cache_get(rank_cache, term)
  if hit then return hit end
  local rank = {}
  local r = 1
  local py = store.py_map[term]
  if py then
    for _, t in ipairs(py) do
      local cp = store:token_cp_of(t)
      if cp > 0 and not rank[cp] then
        rank[cp] = r
        r = r + 1
      end
    end
  else
    local cp = 0
    local t = store:resolve_term(term)
    if t ~= nil then cp = store:token_cp_of(t) end
    if cp == 0 then
      cp = decode_next(term, 0)
    end
    if cp > 0 and not rank[cp] then rank[cp] = r end
  end
  cache_put(rank_cache, term, rank, 64)
  return rank
end

local function parse_query(store, raw_query, mode)
  local lower = ascii_lower(raw_query)
  local items = {}
  if has_separator(lower) then
    for _, part in ipairs(split_separator(lower)) do
      if part ~= "" then
        local segs = segment_alpha_string(store, part, mode)
        for _, s in ipairs(segs) do items[#items + 1] = s end
      end
    end
  elseif is_ascii_alpha_only(lower) then
    items = segment_alpha_string(store, lower, mode)
  else
    local cur = {}
    for _, ch in ipairs(utf8_chars(lower)) do
      if #ch == 1 and ch:byte() >= 97 and ch:byte() <= 122 then
        cur[#cur + 1] = ch
      else
        if #cur > 0 then
          local segs = segment_alpha_string(store, table.concat(cur), mode)
          for _, s in ipairs(segs) do items[#items + 1] = s end
          cur = {}
        end
        items[#items + 1] = ch
      end
    end
    if #cur > 0 then
      local segs = segment_alpha_string(store, table.concat(cur), mode)
      for _, s in ipairs(segs) do items[#items + 1] = s end
    end
  end
  local pq = { items = items, order = {}, counts = {} }
  for _, item in ipairs(items) do
    if pq.counts[item] == nil then
      pq.counts[item] = 1
      pq.order[#pq.order + 1] = item
    else
      pq.counts[item] = pq.counts[item] + 1
    end
  end
  return pq
end

local function bipartite_match(comps, total, matchers, used, idx)
  if idx > total then return true end
  for j = 1, total do
    if not used[j] and matcher_has(matchers[j], comps[idx]) then
      used[j] = true
      if bipartite_match(comps, total, matchers, used, idx + 1) then
        return true
      end
      used[j] = false
    end
  end
  return false
end

local function check_direct_match(store, alts_off, total, matchers,
                                  all_singleton, target_sig)
  if total == 0 or total > 64 then return 0 end
  local blob = store.blob
  local p = store.alts_off + alts_off
  local ng = rd8(blob, p)
  p = p + 1
  local best = 0
  for _ = 1, ng do
    local L = rd8(blob, p)
    p = p + 1
    local nv = rd16(blob, p)
    p = p + 2
    if L ~= total then
      p = p + nv * (L * 2 + 4)
    else
      for _ = 1, nv do
        local tok = p
        local ordered = false
        if not all_singleton or rd32(blob, tok + L * 2) == target_sig then
          ordered = true
          for i = 1, total do
            if not matcher_has(matchers[i], rd16(blob, tok + (i - 1) * 2)) then
              ordered = false
              break
            end
          end
        end
        if ordered then return 2 end
        if best < 1 then
          if total == 2 then
            local c0 = rd16(blob, tok)
            local c1 = rd16(blob, tok + 2)
            local m0, m1 = matchers[1], matchers[2]
            if (matcher_has(m0, c0) and matcher_has(m1, c1))
               or (matcher_has(m0, c1) and matcher_has(m1, c0)) then
              best = 1
            end
          elseif total == 3 then
            local c0 = rd16(blob, tok)
            local c1 = rd16(blob, tok + 2)
            local c2 = rd16(blob, tok + 4)
            local m0, m1, m2 = matchers[1], matchers[2], matchers[3]
            if (matcher_has(m0, c0) and
                ((matcher_has(m1, c1) and matcher_has(m2, c2)) or
                 (matcher_has(m1, c2) and matcher_has(m2, c1))))
               or (matcher_has(m0, c1) and
                ((matcher_has(m1, c0) and matcher_has(m2, c2)) or
                 (matcher_has(m1, c2) and matcher_has(m2, c0))))
               or (matcher_has(m0, c2) and
                ((matcher_has(m1, c0) and matcher_has(m2, c1)) or
                 (matcher_has(m1, c1) and matcher_has(m2, c0)))) then
              best = 1
            end
          else
            local comps = {}
            for i = 1, total do comps[i] = rd16(blob, tok + (i - 1) * 2) end
            local used = {}
            if bipartite_match(comps, total, matchers, used, 1) then best = 1 end
          end
        end
        p = p + L * 2 + 4
      end
    end
  end
  return best
end

local function score_candidate(store, id, matchers, order_count)
  local h = store:hot(id)
  local freq = h.freq
  local depth = h.depth
  local total = #matchers
  if order_count == 1 and total == 1 then
    local ct = h.char_token
    if ct ~= NO_TOKEN and matcher_has(matchers[1], ct) then
      return 30000000 + freq
    end
  end
  local mt = 0
  if total >= 1 and total <= 31 and (h.flags & (1 << total)) ~= 0 then
    local all_singleton = true
    for _, m in ipairs(matchers) do
      if #m.list ~= 1 then all_singleton = false; break end
    end
    local target_sig = 2166136261
    if all_singleton then
      for _, m in ipairs(matchers) do
        local v = m.list[1]
        target_sig = target_sig ~ (v & 0xFF)
        target_sig = (target_sig * 16777619) & 0xFFFFFFFF
        target_sig = target_sig ~ ((v >> 8) & 0xFF)
        target_sig = (target_sig * 16777619) & 0xFFFFFFFF
      end
    end
    mt = check_direct_match(store, h.alts_off, total, matchers,
                            all_singleton, target_sig)
  end
  local common_bonus = freq >= 1900000 and 5000000 or 0
  if mt == 2 then return 50000000 + common_bonus + freq end
  if mt == 1 then return 40000000 + common_bonus + freq end
  return freq - depth * 1000
end

local function add_posting_cps(store, key, limit, quota, cp_list, seen,
                               disabled_blocks)
  local e = store:find_key(key)
  if not e or e.full_cnt == 0 then return end
  local read = math.min(e.head_cnt, quota)
  local head = store:head_ids(e, read)
  local added = 0
  for _, id in ipairs(head) do
    if not seen[id] then
      seen[id] = true
      local keep = true
      if disabled_blocks and disabled_blocks[store:hot(id).block] then
        keep = false
      end
      if keep then
        cp_list[#cp_list + 1] = id
        added = added + 1
        if #cp_list >= limit or added >= quota then return end
      end
    end
  end
  if #cp_list >= limit or read < e.head_cnt then return end
  store:for_each_full(e, function(id)
    if seen[id] then return true end
    seen[id] = true
    local keep = true
    if disabled_blocks and disabled_blocks[store:hot(id).block] then
      keep = false
    end
    if keep then
      cp_list[#cp_list + 1] = id
      added = added + 1
      if #cp_list >= limit then return false end
    end
    return true
  end)
end

local function add_single_cp(store, cp_list, seen, limit, ch, disabled_blocks)
  local cp = decode_next(ch, 0)
  local id = cp ~= 0 and store:id_of_cp(cp) or INVALID_ID
  if id == INVALID_ID or seen[id] then return false end
  seen[id] = true
  if disabled_blocks and disabled_blocks[store:hot(id).block] then
    return false
  end
  cp_list[#cp_list + 1] = id
  return #cp_list >= limit
end

local function get_candidates_for_term(store, term, count, max_chars,
                                       custom_keys, disabled_blocks,
                                       is_multi_term)
  local cp_list = {}
  local seen = {}
  local limit = max_chars == 0 and 200000 or max_chars
  local cn_term = canon(term)

  if count == 1 and not is_multi_term then
    if (store:has_key(cn_term) or store:has_key(term))
       and not (term:sub(1, 1) == "{") then
      if add_single_cp(store, cp_list, seen, limit, cn_term, disabled_blocks) then
        return cp_list
      end
      if cn_term ~= term then
        if add_single_cp(store, cp_list, seen, limit, term, disabled_blocks) then
          return cp_list
        end
      end
    end
    local pit = store.py_map[term]
    if pit then
      for _, t in ipairs(pit) do
        local comp = store:token_str_of(t)
        if comp:sub(1, 1) ~= "{" then
          if add_single_cp(store, cp_list, seen, limit, comp, disabled_blocks) then
            return cp_list
          end
        end
      end
    end
  end

  if custom_keys and #custom_keys > 0 then
    local quota = #custom_keys <= 2 and limit
                  or math.max(60, limit // #custom_keys)
    for _, k in ipairs(custom_keys) do
      add_posting_cps(store, k, limit, quota, cp_list, seen, disabled_blocks)
      if #cp_list >= limit then break end
    end
    if #cp_list < limit then
      for _, k in ipairs(custom_keys) do
        add_posting_cps(store, k, limit, limit, cp_list, seen, disabled_blocks)
        if #cp_list >= limit then break end
      end
    end
  else
    local has_cn = store:has_key(cn_term)
    local has_term = store:has_key(term)
    local has_cn_n = count > 1 and store:has_key(cn_term .. "*" .. count)
    local has_term_n = count > 1 and store:has_key(term .. "*" .. count)
    if has_cn or has_term or has_cn_n or has_term_n then
      local key = nil
      if count > 1 then
        if has_cn_n then key = cn_term .. "*" .. count
        elseif has_term_n then key = term .. "*" .. count end
      end
      if not key then
        if has_cn then key = cn_term else key = term end
      end
      add_posting_cps(store, key, limit, limit, cp_list, seen, disabled_blocks)
    else
      local pit = store.py_map[term]
      if pit then
        local comps = pit
        local quota = #comps <= 2 and limit
                      or math.max(60, limit // #comps)
        local function emit(q)
          for _, t in ipairs(comps) do
            local key = count > 1 and (store:token_str_of(t) .. "*" .. count)
                                    or store:token_str_of(t)
            if store:has_key(key) then
              add_posting_cps(store, key, limit, q, cp_list, seen,
                              disabled_blocks)
            end
            if #cp_list >= limit then return end
          end
        end
        emit(quota)
        if #cp_list < limit then emit(limit) end
      else
        local pref = get_comps_by_prefix(store, term)
        if pref and #pref > 0 then
          local quota = math.max(60, limit // math.max(1, #pref))
          for _, t in ipairs(pref) do
            local key = count > 1 and (store:token_str_of(t) .. "*" .. count)
                                    or store:token_str_of(t)
            if store:has_key(key) then
              add_posting_cps(store, key, limit, quota, cp_list, seen,
                              disabled_blocks)
            end
            if #cp_list >= limit then break end
          end
        end
      end
    end
  end
  return cp_list
end

local function build_term_info(store, term, mult)
  local info = { term = term, mult = mult, is_pref = false, comps = {},
                 tokens = {}, post_keys = {}, data_size = 0 }
  local cn_term = canon(term)
  local comps = {}
  local is_pref = false
  local pit = store.py_map[term]
  if pit then
    for _, t in ipairs(pit) do comps[#comps + 1] = store:token_str_of(t) end
  elseif store:has_key(cn_term) or store:has_key(term) then
    comps[#comps + 1] = cn_term
    if cn_term ~= term then comps[#comps + 1] = term end
  else
    local pref = get_comps_by_prefix(store, term)
    if pref then
      for _, t in ipairs(pref) do comps[#comps + 1] = store:token_str_of(t) end
      is_pref = true
    else
      comps[#comps + 1] = cn_term
      if cn_term ~= term then comps[#comps + 1] = term end
    end
  end
  info.is_pref = is_pref
  info.comps = comps
  local tokens, seen_tok = {}, {}
  for _, c in ipairs(comps) do
    local t = store:resolve_term(c)
    if t ~= nil and not seen_tok[t] then
      seen_tok[t] = true
      tokens[#tokens + 1] = t
    end
  end
  table.sort(tokens)
  info.tokens = tokens

  local max_keys = is_pref and 25 or 250
  local seen_post = {}
  for _, c in ipairs(comps) do
    local cn = canon(c)
    local suffix = mult > 1 and ("*" .. mult) or ""
    local k1 = c .. suffix
    local k2 = cn .. suffix
    local use_k
    if store:has_key(k2) then use_k = k2
    elseif store:has_key(k1) then use_k = k1
    elseif mult > 1 then
      if store:has_key(cn) then use_k = cn
      elseif store:has_key(c) then use_k = c end
    end
    if use_k and not seen_post[use_k] then
      seen_post[use_k] = true
      info.post_keys[#info.post_keys + 1] = use_k
      if #info.post_keys >= max_keys then break end
    end
  end
  local sz = 0
  for _, k in ipairs(info.post_keys) do
    local e = store:find_key(k)
    if e then sz = sz + e.full_cnt * 4 end
  end
  info.data_size = sz
  return info
end

local function get_candidates_for_term_full(store, info, disabled_blocks)
  local ids, seen = {}, {}
  for _, key in ipairs(info.post_keys) do
    local e = store:find_key(key)
    if e and e.full_cnt > 0 then
      local head = store:head_ids(e)
      for _, id in ipairs(head) do
        if not seen[id] then
          seen[id] = true
          if not (disabled_blocks and disabled_blocks[store:hot(id).block]) then
            ids[#ids + 1] = id
          end
        end
      end
      store:for_each_full(e, function(id)
        if seen[id] then return true end
        seen[id] = true
        if not (disabled_blocks and disabled_blocks[store:hot(id).block]) then
          ids[#ids + 1] = id
        end
        return true
      end)
    end
  end
  return ids
end

local function disabled_cache_key(disabled)
  if not disabled or next(disabled) == nil then return "" end
  local ks = {}
  for b in pairs(disabled) do ks[#ks + 1] = b end
  table.sort(ks)
  return ":blk=" .. table.concat(ks, ",")
end

local function lookup_exact_query(store, pq, disabled_blocks)
  local tokens = {}
  for _, term in ipairs(pq.order) do
    local t = store:resolve_term(term)
    if t == nil then return {} end
    for _ = 1, pq.counts[term] do tokens[#tokens + 1] = t end
  end
  if #tokens == 0 then return {} end
  local ids = store:lookup_leaf_structure(tokens)
  if disabled_blocks then
    local filtered = {}
    for _, id in ipairs(ids) do
      if not disabled_blocks[store:hot(id).block] then
        filtered[#filtered + 1] = id
      end
    end
    ids = filtered
  end
  return ids
end

local function merge_exact_first(exact, broad)
  local exact_set, seen, merged = {}, {}, {}
  for _, id in ipairs(exact) do
    if not exact_set[id] then
      exact_set[id] = true
      seen[id] = true
      merged[#merged + 1] = id
    end
  end
  for _, id in ipairs(broad) do
    if not seen[id] then
      seen[id] = true
      merged[#merged + 1] = id
    end
  end
  return merged, exact_set
end

local function search_components(store, query_str, max_results, mode,
                                 disabled_blocks)
  if query_str == "" then return {} end
  local cache_key = query_str .. ":" .. max_results .. ":" .. mode
                      .. disabled_cache_key(disabled_blocks)
  local hit = cache_get(query_cache, cache_key)
  if hit then return hit end

  local pq = parse_query(store, query_str, mode)
  if #pq.order == 0 then return {} end

  local matchers = {}
  for _, item in ipairs(pq.items) do
    matchers[#matchers + 1] = get_term_matcher(store, item)
  end
  local order_count = #pq.order

  if order_count == 1 then
    local term = pq.order[1]
    local direct_rank = get_direct_comp_rank(store, term)
    local is_pref = not store.valid_pinyins[term]
                    and get_comps_by_prefix(store, term) ~= nil
    local limit = is_pref and math.max(250, max_results * 3)
                         or math.max(480, max_results * 8)
    local cands = get_candidates_for_term(store, term, pq.counts[term], limit,
                                          nil, disabled_blocks, false)
    local exact = lookup_exact_query(store, pq, disabled_blocks)
    local exact_set
    cands, exact_set = merge_exact_first(exact, cands)
    if #cands == 0 then
      cache_put(query_cache, cache_key, {}, 256)
      return {}
    end
    local scored = {}
    for _, id in ipairs(cands) do
      local sc = score_candidate(store, id, matchers, order_count)
      if pq.counts[term] == 1 and #matchers == 1 and direct_rank then
        local cp = store:cp_of_id(id)
        local r = direct_rank[cp]
        if r then
          sc = 40000000 + store:hot(id).freq - r * 10
        end
      end
      if exact_set[id] then sc = boost_exact_structure(sc) end
      scored[#scored + 1] = { id = id, score = sc }
    end
    table.sort(scored, function(a, b) return a.score > b.score end)
    local results = {}
    for i = 1, math.min(#scored, max_results) do results[i] = scored[i].id end
    cache_put(query_cache, cache_key, results, 256)
    return results
  end

  local terms_info = {}
  for _, term in ipairs(pq.order) do
    local mult = pq.counts[term]
    local k = term .. ":" .. mult
    local sp = cache_get(term_cache, k)
    if not sp then
      sp = build_term_info(store, term, mult)
      cache_put(term_cache, k, sp, 200)
    end
    if #sp.comps > 0 then terms_info[#terms_info + 1] = sp end
  end
  if #terms_info == 0 then return {} end
  table.sort(terms_info, function(a, b) return a.data_size < b.data_size end)

  local has_any_pref = false
  for _, t in ipairs(terms_info) do
    if t.is_pref then has_any_pref = true; break end
  end

  local base = nil
  for _, t in ipairs(terms_info) do
    if #t.post_keys > 0 then base = t; break end
  end
  if not base then return {} end

  local current
  if has_any_pref then
    current = get_candidates_for_term(store, base.term, base.mult, 1500,
                                      base.post_keys, disabled_blocks, true)
  else
    current = get_candidates_for_term_full(store, base, disabled_blocks)
  end
  if #current == 0 then return {} end

  for _, t in ipairs(terms_info) do
    if #current == 0 then break end
    if t ~= base then
      local filtered = {}
      for _, id in ipairs(current) do
        local cs = store:comp_set(id)
        local cn = #cs
        local matched = false
        local a, b = 1, 1
        local toks = t.tokens
        while a <= cn and b <= #toks do
          if cs[a] == toks[b] then matched = true; break
          elseif cs[a] < toks[b] then a = a + 1
          else b = b + 1 end
        end
        if not matched then
          for _, k in ipairs(t.post_keys) do
            local e = store:find_key(k)
            if e and store:full_contains(e, id) then matched = true; break end
          end
        end
        if matched then filtered[#filtered + 1] = id end
      end
      current = filtered
    end
  end
  local exact = lookup_exact_query(store, pq, disabled_blocks)
  local exact_set
  current, exact_set = merge_exact_first(exact, current)
  if #current == 0 then return {} end

  local score_limit = has_any_pref and 500 or 1500
  local exact_count = 0
  for _ in pairs(exact_set) do exact_count = exact_count + 1 end
  score_limit = score_limit + exact_count
  local scored = {}
  for i = 1, math.min(#current, score_limit) do
    local sc = score_candidate(store, current[i], matchers, order_count)
    if exact_set[current[i]] then sc = boost_exact_structure(sc) end
    scored[#scored + 1] = { id = current[i], score = sc }
  end
  table.sort(scored, function(a, b) return a.score > b.score end)
  local results = {}
  for i = 1, math.min(#scored, max_results) do results[i] = scored[i].id end
  cache_put(query_cache, cache_key, results, 256)
  return results
end

-- ---------------------------------------------------------------------------
-- 混合检字：O 模式查询代数、分解、排序
-- ---------------------------------------------------------------------------

local SEARCH_ZERO_VOWELS = {
  aa = "a", ai = "ai", an = "an", ah = "ang", ao = "ao", ee = "e",
  ei = "ei", en = "en", eg = "eng", er = "er", oo = "o", ou = "ou",
}
local SEARCH_FLYPY_FINALS = {
  q = { "iu" }, w = { "ei" }, r = { "uan" }, t = { "ue", "ve" },
  y = { "un" }, p = { "ie" }, d = { "ai" }, f = { "en" }, g = { "eng" },
  h = { "ang" }, j = { "an" }, k = { "uai", "ing" }, l = { "uang", "iang" },
  z = { "ou" }, x = { "ua", "ia" }, c = { "ao" }, v = { "ui", "v" },
  b = { "in" }, n = { "iao" }, m = { "ian" }, s = { "iong", "ong" },
  o = { "uo", "o" }, u = { "u" }, i = { "i" }, a = { "a" }, e = { "e" },
}
local SEARCH_ZRM_FINALS = {
  q = { "iu" }, w = { "ua", "ia" }, r = { "uan" }, t = { "ue", "ve" },
  y = { "uai", "ing" }, p = { "un" }, d = { "uang", "iang" }, f = { "en" },
  g = { "eng" }, h = { "ang" }, j = { "an" }, k = { "ao" }, l = { "ai" },
  z = { "ou" }, x = { "ia" }, c = { "ch" }, v = { "ui", "v" }, b = { "in" },
  n = { "in" }, m = { "ian" }, s = { "iong", "ong" }, o = { "uo", "o" },
  u = { "u" }, i = { "i" }, a = { "a" }, e = { "e" },
}

local function strip_space(s)
  return (s:gsub("[ \t\r\n]", ""))
end

local function decode_shuangpin(store, raw, mode)
  local sp = ascii_lower(raw)
  if #sp ~= 2 or not is_ascii_alpha_only(sp) or mode == "quanpin" then
    return sp
  end
  local z = SEARCH_ZERO_VOWELS[sp]
  if z and (store.valid_pinyins[z] or store.py_map[z]) then return z end
  local function init_of(c)
    if c == "v" then return "zh" end
    if c == "i" then return "ch" end
    if c == "u" then return "sh" end
    return c
  end
  local finals = mode == "zrm" and SEARCH_ZRM_FINALS or SEARCH_FLYPY_FINALS
  local fit = finals[sp:sub(2, 2)]
  if not fit then return sp end
  local init = init_of(sp:sub(1, 1))
  for _, fin in ipairs(fit) do
    local py = init .. fin
    if (init == "j" or init == "q" or init == "x" or init == "y") and fin == "ve" then
      py = init .. "ue"
    elseif (init == "j" or init == "q" or init == "x" or init == "y") and fin == "v" then
      py = init .. "u"
    end
    if store.valid_pinyins[py] or store.py_map[py] then return py end
  end
  return sp
end

local function normalize_component(store, raw, mode)
  local s = strip_space(raw)
  if s == "" then return s end
  local lower = ascii_lower(s)
  local known = store:find_key(s) ~= nil or store:find_key(lower) ~= nil
             or store.py_map[s] ~= nil or store.py_map[lower] ~= nil
             or store:resolve_term(s) ~= nil
  if not known and #lower == 2 and is_ascii_alpha_only(lower) then
    local decoded = decode_shuangpin(store, lower, mode)
    if decoded ~= lower and (store.valid_pinyins[decoded] or store.py_map[decoded]) then
      return canon(decoded)
    end
  end
  return canon(s)
end

local ATOMIC_COMPONENTS = {
  ["木"] = true, ["氵"] = true, ["水"] = true, ["火"] = true, ["灬"] = true,
  ["土"] = true, ["金"] = true, ["钅"] = true, ["日"] = true, ["月"] = true,
  ["目"] = true, ["口"] = true, ["人"] = true, ["亻"] = true, ["手"] = true,
  ["扌"] = true, ["心"] = true, ["忄"] = true, ["女"] = true, ["子"] = true,
  ["艹"] = true, ["言"] = true, ["讠"] = true, ["石"] = true, ["田"] = true,
  ["山"] = true, ["刀"] = true, ["刂"] = true, ["力"] = true, ["又"] = true,
  ["十"] = true, ["大"] = true, ["小"] = true, ["工"] = true, ["弓"] = true,
  ["广"] = true, ["门"] = true, ["马"] = true, ["鸟"] = true, ["鱼"] = true,
  ["虫"] = true, ["犭"] = true, ["犬"] = true, ["辶"] = true, ["宀"] = true,
  ["冖"] = true, ["穴"] = true, ["竹"] = true, ["⺮"] = true, ["米"] = true,
  ["糸"] = true, ["纟"] = true, ["车"] = true, ["足"] = true, ["⻊"] = true,
  ["雨"] = true, ["页"] = true, ["酉"] = true, ["贝"] = true, ["走"] = true,
  ["身"] = true, ["辛"] = true, ["辰"] = true, ["邑"] = true, ["阝"] = true,
  ["阜"] = true, ["方"] = true, ["斤"] = true, ["止"] = true, ["歹"] = true,
  ["殳"] = true, ["毛"] = true, ["氏"] = true, ["气"] = true, ["白"] = true,
  ["皮"] = true, ["皿"] = true, ["矢"] = true, ["示"] = true, ["礻"] = true,
  ["禾"] = true, ["立"] = true, ["羽"] = true, ["舟"] = true, ["艮"] = true,
  ["见"] = true, ["角"] = true, ["谷"] = true, ["豆"] = true, ["豕"] = true,
  ["豸"] = true, ["赤"] = true, ["革"] = true, ["音"] = true, ["风"] = true,
  ["飞"] = true, ["食"] = true, ["饣"] = true, ["首"] = true, ["香"] = true,
  ["骨"] = true, ["鬼"] = true, ["黾"] = true, ["鼎"] = true, ["鼓"] = true,
  ["鼠"] = true, ["鼻"] = true, ["齐"] = true, ["齿"] = true, ["龙"] = true,
  ["龟"] = true,
  ["⺡"] = true, ["⺣"] = true, ["⺅"] = true, ["龵"] = true, ["⺖"] = true,
  ["⺾"] = true, ["⺬"] = true, ["⺭"] = true, ["⻐"] = true, ["⻠"] = true,
  ["⺰"] = true, ["⺲"] = true, ["⻌"] = true, ["⻖"] = true, ["⺩"] = true,
  ["⺙"] = true, ["⺹"] = true, ["⺫"] = true, ["⺉"] = true, ["⺶"] = true,
  ["㇒"] = true, ["㇐"] = true, ["㇑"] = true, ["㇔"] = true, ["㇏"] = true,
  ["㇀"] = true, ["乛"] = true, ["亅"] = true,
}

local function top_leaves_by_id(store, id, preserve_glyph)
  local leaves = {}
  if id == INVALID_ID then return leaves end
  local tokens = store:leaves(id)
  for _, t in ipairs(tokens) do
    local tt = preserve_glyph and t or store:canon_token(t)
    local s = store:token_str_of(tt)
    if s ~= "" then leaves[#leaves + 1] = s end
  end
  if #leaves == 0 then
    local cp = store:cp_of_id(id)
    if cp ~= 0 then
      local c = cp_to_utf8(cp)
      leaves[1] = preserve_glyph and c or canon(c)
    end
  end
  return leaves
end

local function top_leaves_of_component(store, comp, preserve_glyph)
  local chars = utf8_chars(comp)
  if #chars ~= 1 then
    return { preserve_glyph and comp or canon(comp) }
  end
  local cp = decode_next(comp, 0)
  local id = store:id_of_cp(cp)
  if id == INVALID_ID then
    return { preserve_glyph and comp or canon(comp) }
  end
  local leaves = top_leaves_by_id(store, id, preserve_glyph)
  if #leaves == 0 then
    leaves[1] = preserve_glyph and comp or canon(comp)
  end
  return leaves
end

local function atomic_decomposition(store, comp, cache, depth)
  local ch = canon(comp)
  local hit = cache[ch]
  if hit then return hit end
  if depth > 2 or ATOMIC_COMPONENTS[ch] then
    local r = { ch }
    cache[ch] = r
    return r
  end
  local leaves = top_leaves_of_component(store, ch)
  if #leaves <= 1 then
    local r = { ch }
    cache[ch] = r
    return r
  end
  local out = {}
  for _, leaf in ipairs(leaves) do
    if leaf == ch then
      out[#out + 1] = leaf
    else
      local sub = atomic_decomposition(store, leaf, cache, depth + 1)
      for _, x in ipairs(sub) do out[#out + 1] = x end
    end
  end
  cache[ch] = out
  return out
end

local function keys_for_term(store, term, count)
  local keys = {}
  local function add_token(t)
    local e = store:find_key_by_token(t, count)
    if not e and count > 1 then e = store:find_key_by_token(t, 1) end
    if e then keys[#keys + 1] = e end
  end
  local t = store:find_token(term)
  if t ~= nil then
    add_token(t)
    if #keys > 0 then return keys end
  end
  local pit = store.py_map[term]
  if pit then
    for _, token in ipairs(pit) do add_token(token) end
    if #keys > 0 then return keys end
  end
  local rt = store:resolve_term(term)
  if rt ~= nil then add_token(rt) end
  return keys
end

local function intersect_postings(store, terms, counts)
  if #terms == 0 then return {} end
  local term_keys = {}
  local seed_index, seed_size = 1, math.maxinteger
  for i, term in ipairs(terms) do
    local tk = { entries = {}, total = 0 }
    local seen_keys = {}
    for _, e in ipairs(keys_for_term(store, term, counts[i])) do
      if not seen_keys[e] then
        seen_keys[e] = true
        if e and e.full_cnt > 0 then
          tk.entries[#tk.entries + 1] = e
          tk.total = tk.total + e.full_cnt
        end
      end
    end
    if #tk.entries == 0 then return {} end
    term_keys[i] = tk
    if tk.total < seed_size then
      seed_size = tk.total
      seed_index = i
    end
  end

  local result = {}
  local seed_seen = {}
  local need_dedup = #term_keys[seed_index].entries > 1
  local cursor = {}
  for i = 1, #term_keys do
    cursor[i] = {}
    for j = 1, #term_keys[i].entries do cursor[i][j] = 0 end
  end

  for _, se in ipairs(term_keys[seed_index].entries) do
    for i = 1, #term_keys do
      for j = 1, #cursor[i] do cursor[i][j] = 0 end
    end
    store:for_each_full(se, function(id)
      if need_dedup and seed_seen[id] then return true end
      if need_dedup then seed_seen[id] = true end
      for i = 1, #term_keys do
        if i ~= seed_index then
          local matched = false
          local entries = term_keys[i].entries
          for j = 1, #entries do
            local other = entries[j]
            if other.kind == 0 then
              local base = store.post_off + other.full_off
              local p = cursor[i][j]
              while p < other.full_cnt
                    and rd32(store.blob, base + p * 4) < id do
                p = p + 1
              end
              cursor[i][j] = p
              if p < other.full_cnt
                 and rd32(store.blob, base + p * 4) == id then
                matched = true
                break
              end
            elseif store:full_contains(other, id) then
              matched = true
              break
            end
          end
          if not matched then return true end
        end
      end
      result[#result + 1] = id
      return true
    end)
  end
  return result
end

local function add_count(map, key)
  map[key] = (map[key] or 0) + 1
end

local function add_token_count(counts, token)
  for _, c in ipairs(counts) do
    if c.token == token then c.count = c.count + 1; return end
  end
  counts[#counts + 1] = { token = token, count = 1 }
end

local function token_count_of(counts, token)
  for _, c in ipairs(counts) do
    if c.token == token then return c.count end
  end
  return 0
end

local function build_query_atoms(store, normalized)
  local q = { norm = {}, counts = {}, seq = {} }
  for _, raw in ipairs(normalized) do
    local c = canon(raw)
    local t = store:resolve_term(c)
    if t == nil then return nil end
    local cp = store:token_cp_of(t)
    local id = cp ~= 0 and store:id_of_cp(cp) or INVALID_ID
    if id == INVALID_ID then return nil end
    q.norm[#q.norm + 1] = store:canon_token(t)
    local atoms = store:atomics(id)
    if #atoms == 0 then return nil end
    for _, atom in ipairs(atoms) do
      add_token_count(q.counts, atom)
      q.seq[#q.seq + 1] = atom
    end
  end
  if #q.seq == 0 then return nil end
  return q
end

local function score_hybrid_tokens(store, id, q)
  local leaves = store:leaves(id)
  local atoms = store:atomics(id)
  local satisfied = true
  local missing_count = 0
  for _, c in ipairs(q.counts) do
    local avail = 0
    for _, atom in ipairs(atoms) do
      if atom == c.token then avail = avail + 1 end
    end
    if avail < c.count then
      satisfied = false
      missing_count = missing_count + c.count - avail
    end
  end

  local extra_count = 0
  local seen_tok = {}
  for _, t in ipairs(atoms) do
    if not seen_tok[t] then
      seen_tok[t] = true
      local avail = 0
      for _, atom in ipairs(atoms) do
        if atom == t then avail = avail + 1 end
      end
      local req = token_count_of(q.counts, t)
      if avail > req then extra_count = extra_count + avail - req end
    end
  end

  local ordered_bonus = 0
  local match_macro = #leaves == #q.norm
  if match_macro then
    for i = 1, #q.norm do
      if store:canon_token(leaves[i]) ~= q.norm[i] then
        match_macro = false
        break
      end
    end
  end
  local match_macro_rev = false
  if not match_macro and #leaves == #q.norm then
    match_macro_rev = true
    local n = #q.norm
    for i = 1, n do
      if store:canon_token(leaves[i]) ~= q.norm[n - i + 1] then
        match_macro_rev = false
        break
      end
    end
  end
  if match_macro then
    ordered_bonus = 20000000
  elseif match_macro_rev then
    ordered_bonus = 10000000
  elseif #q.seq == #atoms then
    local seq_match = true
    for i = 1, #q.seq do
      if q.seq[i] ~= atoms[i] then seq_match = false; break end
    end
    if seq_match then ordered_bonus = 15000000 end
  end

  local score
  if satisfied then
    if extra_count == 0 then
      score = 50000000 + ordered_bonus
    else
      score = 20000000 - extra_count * 500000 + ordered_bonus // 2
    end
  else
    score = -missing_count * 5000000 - extra_count * 500000
  end
  local block = store:hot(id).block
  if block == 0 then score = score + 2000000
  elseif block == 1 then score = score + 1000000 end
  return score
end

local function score_hybrid_strings(store, id, normalized, user_atomics,
                                    user_atomic_seq, cache)
  local cp = store:cp_of_id(id)
  local ch = cp_to_utf8(cp)
  local cand_leaves = top_leaves_by_id(store, id)
  local cand_atomic_list = atomic_decomposition(store, ch, cache, 0)
  local cand_atomics = {}
  for _, a in ipairs(cand_atomic_list) do add_count(cand_atomics, a) end

  local satisfied = true
  local missing_count = 0
  for k, v in pairs(user_atomics) do
    local avail = cand_atomics[k] or 0
    if avail < v then
      satisfied = false
      missing_count = missing_count + v - avail
    end
  end
  local extra_count = 0
  for k, v in pairs(cand_atomics) do
    local req = user_atomics[k] or 0
    if v > req then extra_count = extra_count + v - req end
  end

  local ordered_bonus = 0
  local match_macro = #cand_leaves == #normalized
  if match_macro then
    for i = 1, #normalized do
      if cand_leaves[i] ~= normalized[i] then
        match_macro = false
        break
      end
    end
  end
  local match_macro_rev = false
  if not match_macro and #cand_leaves == #normalized then
    match_macro_rev = true
    local n = #normalized
    for i = 1, n do
      if cand_leaves[i] ~= normalized[n - i + 1] then
        match_macro_rev = false
        break
      end
    end
  end
  if match_macro then
    ordered_bonus = 20000000
  elseif match_macro_rev then
    ordered_bonus = 10000000
  elseif #user_atomic_seq == #cand_atomic_list then
    local seq_match = true
    for i = 1, #user_atomic_seq do
      if user_atomic_seq[i] ~= cand_atomic_list[i] then
        seq_match = false
        break
      end
    end
    if seq_match then ordered_bonus = 15000000 end
  end

  local score
  if satisfied then
    if extra_count == 0 then
      score = 50000000 + ordered_bonus
    else
      score = 20000000 - extra_count * 500000 + ordered_bonus // 2
    end
  else
    score = -missing_count * 5000000 - extra_count * 500000
  end
  local block = store:hot(id).block
  if block == 0 then score = score + 2000000
  elseif block == 1 then score = score + 1000000 end
  return score
end

local function rank_hybrid_strings(store, cands, normalized, exact_ids,
                                   score_limit)
  local user_atomics = {}
  local user_atomic_seq = {}
  local cache = {}
  local norm_input = {}
  for _, raw in ipairs(normalized) do
    local c = canon(raw)
    norm_input[#norm_input + 1] = c
    local atomics = atomic_decomposition(store, c, cache, 0)
    for _, a in ipairs(atomics) do
      add_count(user_atomics, a)
      user_atomic_seq[#user_atomic_seq + 1] = a
    end
  end

  local limit = math.min(#cands, score_limit or 500)
  local scored = {}
  for i = 1, limit do
    local sc = score_hybrid_strings(store, cands[i], norm_input, user_atomics,
                                    user_atomic_seq, cache)
    if exact_ids and exact_ids[cands[i]] then
      sc = boost_exact_structure(sc)
    end
    scored[#scored + 1] = { id = cands[i], score = sc, index = i }
  end
  table.sort(scored, function(a, b)
    if a.score ~= b.score then return a.score > b.score end
    return a.index < b.index
  end)
  local out = {}
  for _, it in ipairs(scored) do out[#out + 1] = it.id end
  for i = limit + 1, #cands do out[#out + 1] = cands[i] end
  return out
end

local function rank_hybrid(store, cands, normalized, exact_ids, score_limit)
  if #cands <= 1 or #normalized == 0 then return cands end
  local q = build_query_atoms(store, normalized)
  if not q then
    return rank_hybrid_strings(store, cands, normalized, exact_ids, score_limit)
  end
  local limit = math.min(#cands, score_limit or 500)
  local scored = {}
  for i = 1, limit do
    local sc = score_hybrid_tokens(store, cands[i], q)
    if exact_ids and exact_ids[cands[i]] then
      sc = boost_exact_structure(sc)
    end
    scored[#scored + 1] = { id = cands[i], score = sc, index = i }
  end
  table.sort(scored, function(a, b)
    if a.score ~= b.score then return a.score > b.score end
    return a.index < b.index
  end)
  local out = {}
  for _, it in ipairs(scored) do out[#out + 1] = it.id end
  for i = limit + 1, #cands do out[#out + 1] = cands[i] end
  return out
end

local function normalize_algebra_item(store, raw, mode)
  local s = strip_space(raw)
  if #utf8_chars(s) == 1 then return s end
  return normalize_component(store, s, mode)
end

local function parse_algebra_steps(components)
  local steps = {}
  local curr_op = "+"
  local groups = {}
  for _, raw in ipairs(components) do
    local s = strip_space(raw)
    -- 上一条以 ! 结尾时，新条目默认恢复正向；显式运算符仍可覆盖。
    -- 括号内不允许运算符，因此只在顶层做一次性的恢复。
    if #groups == 0 and curr_op == "!" and s ~= ""
       and s:sub(1, 1) ~= "+" and s:sub(1, 1) ~= "-" and s:sub(1, 1) ~= "!"
       and s:sub(1, 1) ~= "(" then
      curr_op = "+"
    end
    local i = 1
    while i <= #s do
      local c = s:sub(i, i)
      if c == "(" then
        groups[#groups + 1] = curr_op
        i = i + 1
      elseif c == ")" then
        if #groups > 0 then
          local outer = groups[#groups]
          groups[#groups] = nil
          if #groups > 0 and groups[#groups] == "!" then
            curr_op = "!"
          elseif outer == "!" then
            curr_op = "+"
          else
            curr_op = outer
          end
        end
        i = i + 1
      elseif c == "+" or c == "-" or c == "!" then
        -- 括号内不允许运算符；这里防御性忽略，不改变组内默认运算符。
        if #groups == 0 then curr_op = c end
        i = i + 1
      else
        local start = i
        while i <= #s and s:sub(i, i) ~= "+" and s:sub(i, i) ~= "-"
              and s:sub(i, i) ~= "!" and s:sub(i, i) ~= "("
              and s:sub(i, i) ~= ")" do
          i = i + 1
        end
        local item = s:sub(start, i - 1)
        if item ~= "" then
          if is_ascii_alpha_only(item) then
            steps[#steps + 1] = { op = curr_op, item = item }
          else
            for _, ch in ipairs(utf8_chars(item)) do
              steps[#steps + 1] = { op = curr_op, item = ch }
            end
          end
        end
        -- ! 在顶层是一次性的；作为括号默认运算符时对整组生效。
        if curr_op == "!" and #groups == 0 then curr_op = "+" end
      end
    end
  end
  return steps
end

local function subtract_character(store, base, neg, depth, from_leaf)
  local norm_base = canon(base)
  local norm_neg = canon(neg)
  if norm_base == norm_neg then return {} end
  local atomic = ATOMIC_COMPONENTS[base] == true
  if not atomic and from_leaf then atomic = ATOMIC_COMPONENTS[norm_base] == true end
  if depth > 3 or atomic then return nil end

  local leaves = top_leaves_of_component(store, base, true)
  if #leaves <= 1 and #leaves > 0 and canon(leaves[1]) == norm_base then
    return nil
  end

  for i = 1, #leaves do
    if canon(leaves[i]) == norm_neg then
      table.remove(leaves, i)
      return leaves
    end
  end

  local new_leaves = {}
  local sub_done = false
  for _, leaf in ipairs(leaves) do
    if not sub_done then
      local sub = subtract_character(store, leaf, norm_neg, depth + 1, true)
      if sub then
        for _, x in ipairs(sub) do new_leaves[#new_leaves + 1] = x end
        sub_done = true
      else
        new_leaves[#new_leaves + 1] = leaf
      end
    else
      new_leaves[#new_leaves + 1] = leaf
    end
  end
  if sub_done then return new_leaves end
  return nil
end

local function apply_single_subtraction(store, current, neg)
  for i = 1, #current do
    local sub = subtract_character(store, current[i], neg, 0, false)
    if sub then
      local out = {}
      for j = 1, i - 1 do out[#out + 1] = current[j] end
      for _, x in ipairs(sub) do out[#out + 1] = x end
      for j = i + 1, #current do out[#out + 1] = current[j] end
      return out
    end
  end
  for i = 1, #current do
    if canon(current[i]) == canon(neg) then
      local out = {}
      for j = 1, #current do
        if j ~= i then out[#out + 1] = current[j] end
      end
      return out
    end
  end
  return current
end

-- ---------------------------------------------------------------------------
-- 括号表达式求值（只算部件，不查词典）
-- ---------------------------------------------------------------------------

local function find_last_ascii_run(s)
  local last_start, last_end
  local i = 1
  while i <= #s do
    local b = s:byte(i)
    if b and ((b >= 0x41 and b <= 0x5A) or (b >= 0x61 and b <= 0x7A)) then
      local start = i
      while i <= #s do
        local c = s:byte(i)
        if not c or not ((c >= 0x41 and c <= 0x5A) or (c >= 0x61 and c <= 0x7A)) then
          break
        end
        i = i + 1
      end
      last_start, last_end = start, i - 1
    else
      i = i + 1
    end
  end
  if last_start then return last_start, last_end end
  return nil
end

local function replace_last_ascii_run(s, text)
  local start_pos, end_pos = find_last_ascii_run(s)
  if not start_pos then return s end
  return s:sub(1, start_pos - 1) .. text .. s:sub(end_pos + 1)
end

-- pending 形如 [op]? ( ... )? 时返回 { op, inner, closed }；否则返回 nil。
local function parse_single_group(pending)
  if pending == "" then return nil end
  local i = 1
  local op = ""
  local first = pending:sub(1, 1)
  if first == "-" or first == "+" or first == "!" then
    op = first
    i = 2
  end
  if pending:sub(i, i) ~= "(" then return nil end
  local open_pos = i
  local depth = 0
  local close_pos
  local j = i
  while j <= #pending do
    local c = pending:sub(j, j)
    if c == "(" then
      depth = depth + 1
    elseif c == ")" then
      depth = depth - 1
      if depth == 0 then
        close_pos = j
        break
      end
    end
    j = j + 1
  end
  if close_pos then
    if close_pos ~= #pending then return nil end
    return { op = op, inner = pending:sub(open_pos + 1, close_pos - 1),
             closed = true }
  end
  return { op = op, inner = pending:sub(open_pos + 1), closed = false }
end

local function format_group_component(op, components, excludes)
  local out = table.concat(components)
  for _, e in ipairs(excludes) do out = out .. "!" .. e end
  if out == "" then return "" end
  if op ~= "" then out = op .. out end
  return out
end

local function apply_eval_item(store, st, op, raw, mode)
  local item = normalize_algebra_item(store, raw, mode)
  if item == "" then return end
  if op == "+" then
    st.components[#st.components + 1] = item
  elseif op == "!" then
    st.excludes[#st.excludes + 1] = item
  else
    st.has_sub = true
    st.components = apply_single_subtraction(store, st.components, item)
  end
end

local function merge_eval_group(store, st, op, inner)
  st.has_sub = st.has_sub or inner.has_sub
  for _, item in ipairs(inner.components) do
    if op == "+" then
      st.components[#st.components + 1] = item
    elseif op == "!" then
      st.excludes[#st.excludes + 1] = item
    else
      st.has_sub = true
      st.components = apply_single_subtraction(store, st.components, item)
    end
  end
  for _, e in ipairs(inner.excludes) do
    st.excludes[#st.excludes + 1] = e
  end
end

local function is_eval_separator(c)
  return c == "'" or c == " " or c == "\t" or c == "\r" or c == "\n"
         or c == "." or c == "_"
end

local function eval_algebra_seq(store, s, i, mode, stop)
  local st = { components = {}, excludes = {}, has_sub = false }
  local op = "+"
  while i <= #s do
    local c = s:sub(i, i)
    if c == ")" then
      if stop then i = i + 1 end
      break
    elseif c == "(" then
      local new_i, inner = eval_algebra_seq(store, s, i + 1, mode, true)
      merge_eval_group(store, st, op, inner)
      if op == "!" then op = "+" end
      i = new_i
    elseif c == "+" or c == "-" or c == "!" then
      op = c
      i = i + 1
    elseif is_eval_separator(c) then
      i = i + 1
    else
      local start = i
      while i <= #s do
        local ch = s:sub(i, i)
        if ch == "+" or ch == "-" or ch == "!" or ch == "(" or ch == ")"
           or is_eval_separator(ch) then
          break
        end
        i = i + 1
      end
      local item = s:sub(start, i - 1)
      if item ~= "" then
        if is_ascii_alpha_only(item) then
          apply_eval_item(store, st, op, item, mode)
          if op == "!" then op = "+" end
        else
          for _, ch in ipairs(utf8_chars(item)) do
            apply_eval_item(store, st, op, ch, mode)
            if op == "!" then op = "+" end
          end
        end
      end
    end
  end
  return i, st
end

local function evaluate_algebra_expression(store, expression, mode)
  local _, st = eval_algebra_seq(store, expression, 1, mode, false)
  return st.components, st.excludes
end

local function pure_subtraction_results(store, components)
  local results = {}
  local seen_comp, seen_id = {}, {}
  for _, c in ipairs(components) do
    local key = canon(c)
    if not seen_comp[key] then
      seen_comp[key] = true
      if #utf8_chars(c) == 1 then
        local cp = decode_next(c, 0)
        local id = store:id_of_cp(cp)
        if id == INVALID_ID then
          local canonical = canon(c)
          if canonical ~= c then
            cp = decode_next(canonical, 0)
            id = store:id_of_cp(cp)
          end
        end
        if id ~= INVALID_ID and not seen_id[id] then
          seen_id[id] = true
          results[#results + 1] = id
        end
      end
    end
  end
  return results
end

local function build_exclude_entries(store, excludes)
  local out = { postings = {}, self_ids = {} }
  local seen_keys, seen_ids = {}, {}
  local function add_id(id)
    if id ~= INVALID_ID and not seen_ids[id] then
      seen_ids[id] = true
      out.self_ids[#out.self_ids + 1] = id
    end
  end
  local function add_term_id(term)
    if #utf8_chars(term) ~= 1 then return end
    local cp = decode_next(term, 0)
    if cp ~= 0 then add_id(store:id_of_cp(cp)) end
  end
  for _, term in ipairs(excludes) do
    for _, e in ipairs(keys_for_term(store, term, 1)) do
      if not seen_keys[e] then
        seen_keys[e] = true
        out.postings[#out.postings + 1] = e
      end
    end
    add_term_id(term)
    local cn = canon(term)
    if cn ~= term then add_term_id(cn) end
    local pit = store.py_map[term]
    if pit then
      for _, t in ipairs(pit) do
        local cp = store:token_cp_of(t)
        if cp ~= 0 then add_id(store:id_of_cp(cp)) end
      end
    end
  end
  table.sort(out.self_ids)
  local uniq = {}
  local last
  for _, id in ipairs(out.self_ids) do
    if id ~= last then uniq[#uniq + 1] = id; last = id end
  end
  out.self_ids = uniq
  return out
end

local function contains_excluded(store, entries, id)
  local lo, hi = 1, #entries.self_ids
  while lo <= hi do
    local mid = (lo + hi) // 2
    local v = entries.self_ids[mid]
    if v == id then return true end
    if v < id then lo = mid + 1 else hi = mid - 1 end
  end
  for _, e in ipairs(entries.postings) do
    if store:full_contains(e, id) then return true end
  end
  return false
end

local function remove_excluded(store, entries, ids)
  if not entries or (#entries.postings == 0 and #entries.self_ids == 0) then
    return ids
  end
  local out = {}
  for _, id in ipairs(ids) do
    if not contains_excluded(store, entries, id) then out[#out + 1] = id end
  end
  return out
end

local function lookup_exact_structure(store, normalized, disabled_blocks)
  local tokens = {}
  for _, n in ipairs(normalized) do
    local t = store:resolve_term(n)
    if t == nil then return {} end
    tokens[#tokens + 1] = t
  end
  if #tokens == 0 then return {} end
  local ids = store:lookup_leaf_structure(tokens)
  if disabled_blocks then
    local filtered = {}
    for _, id in ipairs(ids) do
      if not disabled_blocks[store:hot(id).block] then
        filtered[#filtered + 1] = id
      end
    end
    ids = filtered
  end
  return ids
end

local function search_by_leaves(store, leaves, mode, max_results,
                                disabled_blocks, exclude_entries)
  local normalized = {}
  for _, raw in ipairs(leaves) do
    local n = normalize_component(store, raw, mode)
    if n ~= "" then normalized[#normalized + 1] = n end
  end
  if #normalized == 0 then return {} end

  local order, counts = {}, {}
  for _, n in ipairs(normalized) do
    local found = false
    for i = 1, #order do
      if order[i] == n then
        counts[i] = counts[i] + 1
        found = true
        break
      end
    end
    if not found then
      order[#order + 1] = n
      counts[#counts + 1] = 1
    end
  end

  local cands
  if #order == 1 then
    local entries = keys_for_term(store, order[1], counts[1])
    if #entries == 1 then
      cands = store:head_ids(entries[1])
    else
      cands = {}
      local seen = {}
      for _, e in ipairs(entries) do
        for _, id in ipairs(store:head_ids(e)) do
          if not seen[id] then seen[id] = true; cands[#cands + 1] = id end
        end
      end
    end
  else
    cands = intersect_postings(store, order, counts)
  end

  if disabled_blocks then
    local filtered = {}
    for _, id in ipairs(cands) do
      if not disabled_blocks[store:hot(id).block] then
        filtered[#filtered + 1] = id
      end
    end
    cands = filtered
  end
  cands = remove_excluded(store, exclude_entries, cands)

  local exact = lookup_exact_structure(store, normalized, disabled_blocks)
  exact = remove_excluded(store, exclude_entries, exact)
  local exact_set
  cands, exact_set = merge_exact_first(exact, cands)
  local exact_count = 0
  for _ in pairs(exact_set) do exact_count = exact_count + 1 end
  local score_limit = 500
  if exact_count > 0 then
    score_limit = math.min(#cands, 500 + exact_count)
  end
  cands = rank_hybrid(store, cands, normalized,
                      exact_count > 0 and exact_set or nil, score_limit)
  if #cands > max_results then
    local r = {}
    for i = 1, max_results do r[i] = cands[i] end
    cands = r
  end
  return cands
end

local function search_hybrid(store, components, mode, max_results,
                             disabled_blocks)
  if #components == 0 then return {} end
  local cache_key = table.concat(components, "\1") .. ":" .. mode .. ":"
                    .. max_results .. disabled_cache_key(disabled_blocks)
  local hit = cache_get(hybrid_cache, cache_key)
  if hit then return hit end
  local steps = parse_algebra_steps(components)
  local current, excludes = {}, {}
  local pure_sub = true
  local has_sub = false
  for _, step in ipairs(steps) do
    local item = normalize_algebra_item(store, step.item, mode)
    if item ~= "" then
      if step.op == "+" then
        if #current > 0 and has_sub then pure_sub = false end
        current[#current + 1] = item
      elseif step.op == "!" then
        excludes[#excludes + 1] = item
      else
        has_sub = true
        current = apply_single_subtraction(store, current, item)
      end
    end
  end

  local exclude_entries = build_exclude_entries(store, excludes)

  if pure_sub and has_sub and #current > 0 then
    local results = pure_subtraction_results(store, current)
    results = remove_excluded(store, exclude_entries, results)
    local expanded = search_by_leaves(store, current, mode, max_results,
                                      disabled_blocks, exclude_entries)
    local seen, merged = {}, {}
    for _, id in ipairs(results) do
      if #merged >= max_results then break end
      if not seen[id] then seen[id] = true; merged[#merged + 1] = id end
    end
    for _, id in ipairs(expanded) do
      if #merged >= max_results then break end
      if not seen[id] then seen[id] = true; merged[#merged + 1] = id end
    end
    cache_put(hybrid_cache, cache_key, merged, 256)
    return merged
  end
  if #current == 0 then
    local empty = {}
    cache_put(hybrid_cache, cache_key, empty, 256)
    return empty
  end
  local results = search_by_leaves(store, current, mode, max_results,
                                   disabled_blocks, exclude_entries)
  cache_put(hybrid_cache, cache_key, results, 256)
  return results
end

local function get_component_candidates(store, raw_code, mode, limit)
  local code = strip_space(raw_code)
  while code ~= "" and (code:sub(1, 1) == "-" or code:sub(1, 1) == "+"
                        or code:sub(1, 1) == "!") do
    code = code:sub(2)
  end
  if code == "" then return {} end
  code = ascii_lower(code)
  if not is_ascii_alpha_only(code) then return {} end
  local lookup = code
  if not store.py_map[lookup] and #lookup == 2 then
    local decoded = decode_shuangpin(store, lookup, mode)
    if decoded ~= lookup then lookup = decoded end
  end
  local tokens
  local pit = store.py_map[lookup]
  if pit then
    tokens = pit
  else
    tokens = {}
    local seen = {}
    for py, list in pairs(store.py_map) do
      if starts_with(py, lookup) then
        for _, t in ipairs(list) do
          if not seen[t] then seen[t] = true; tokens[#tokens + 1] = t end
        end
      end
    end
  end
  local ids, seen_ids = {}, {}
  for _, t in ipairs(tokens) do
    local cp = store:token_cp_of(t)
    if cp ~= 0 then
      local id = store:id_of_cp(cp)
      if id ~= INVALID_ID and not seen_ids[id] then
        seen_ids[id] = true
        ids[#ids + 1] = id
      end
    end
  end
  table.sort(ids, function(a, b) return store:hot(a).freq > store:hot(b).freq end)
  if #ids > limit then
    local r = {}
    for i = 1, limit do r[i] = ids[i] end
    ids = r
  end
  return ids
end

-- ---------------------------------------------------------------------------
-- 候选注释与 IDS 文本
-- ---------------------------------------------------------------------------

local function build_comment(store, id, show_pinyin, show_ids, show_unicode,
                             show_block, ids_format)
  local py, ids = store:cold(id)
  local h = store:hot(id)
  local parts = {}
  if show_pinyin and py ~= "" then parts[#parts + 1] = py end
  if show_ids and ids ~= "" then
    parts[#parts + 1] = format_ids(ids, ids_format)
  end
  local cp = store:cp_of_id(id)
  if show_unicode then parts[#parts + 1] = string.format("%04X", cp) end
  if show_block then parts[#parts + 1] = block_name(h.block) end
  return join(parts, " ")
end

local function ids_of_text(store, text)
  local parts = {}
  for _, ch in ipairs(utf8_chars(text)) do
    local cp = decode_next(ch, 0)
    local id = store:id_of_cp(cp)
    if id == INVALID_ID then
      parts[#parts + 1] = ch
    else
      local _, ids = store:cold(id)
      parts[#parts + 1] = ids ~= "" and ids or ch
    end
  end
  return join(parts, " ")
end

-- ---------------------------------------------------------------------------
-- 配置与状态
-- ---------------------------------------------------------------------------

local function default_config()
  return {
    search = { prefix = "I", tag = "ids_reverse_search", tips = "〔部件检字〕" },
    hybrid = { prefix = "O", tag = "ids_reverse_hybrid", tips = "〔汉字部件检字〕" },
    data_path = "ids_data.bin",
    custom_path = "",
    shuangpin_scheme = "flypy",
    ids_format = "unicode",
    variant_trigger = "/",
    variant_replace_key = "Shift+V",
    max_results = 60,
    show_pinyin = true,
    show_ids = true,
    show_unicode = true,
    show_block = true,
    has_sw_pinyin = false,
    has_sw_ids = false,
    has_sw_unicode = false,
    has_sw_block = false,
    disabled_blocks = {},
    disabled_blocks_enabled = false,
  }
end

local function config_name_space(env)
  local ns = env.name_space or ""
  if ns == "" or ns:sub(1, 1) == "*" or ns == "processor"
     or ns == "segmentor" or ns == "translator" or ns == "filter" then
    return "ids_reverse"
  end
  return ns
end

local function load_config(env)
  local cfg = default_config()
  local ns = config_name_space(env)
  cfg.ns = ns
  local conf = env.engine and env.engine.schema and env.engine.schema.config
  if not conf then return cfg end
  local function get_s(key, dst)
    local v = conf:get_string(ns .. "/" .. key)
    if v and v ~= "" then return v end
    return dst
  end

  cfg.search.prefix = get_s("search_prefix", cfg.search.prefix)
  cfg.search.tag = get_s("search_tag", cfg.search.tag)
  cfg.search.tips = get_s("search_tips", cfg.search.tips)
  cfg.hybrid.prefix = get_s("hybrid_prefix", cfg.hybrid.prefix)
  cfg.hybrid.tag = get_s("hybrid_tag", cfg.hybrid.tag)
  cfg.hybrid.tips = get_s("hybrid_tips", cfg.hybrid.tips)
  cfg.data_path = get_s("data_file", cfg.data_path)
  cfg.custom_path = get_s("custom_file", cfg.custom_path)
  cfg.shuangpin_scheme = get_s("shuangpin_scheme", cfg.shuangpin_scheme)
  cfg.ids_format = get_s("ids_format", cfg.ids_format)
  cfg.variant_trigger = get_s("variant_trigger", cfg.variant_trigger)
  cfg.variant_replace_key = get_s("variant_replace_key", cfg.variant_replace_key)

  local max_results = conf:get_int(ns .. "/max_results")
  if max_results and max_results > 0 then cfg.max_results = max_results end
  local v
  v = conf:get_bool(ns .. "/show_pinyin")
  if v ~= nil then cfg.show_pinyin = v end
  v = conf:get_bool(ns .. "/show_ids")
  if v ~= nil then cfg.show_ids = v end
  v = conf:get_bool(ns .. "/show_unicode")
  if v ~= nil then cfg.show_unicode = v end
  v = conf:get_bool(ns .. "/show_block")
  if v ~= nil then cfg.show_block = v end

  if cfg.shuangpin_scheme ~= "zrm" and cfg.shuangpin_scheme ~= "quanpin" then
    cfg.shuangpin_scheme = "flypy"
  end
  if cfg.variant_trigger == "" then cfg.variant_trigger = "/" end
  if cfg.variant_replace_key == "" then cfg.variant_replace_key = "Shift+V" end
  if cfg.max_results <= 0 then cfg.max_results = 60 end

  local switches = conf:get_list("switches")
  if switches then
    for i = 0, switches.size - 1 do
      local item = switches:get_at(i)
      local map = item and item:get_map()
      if map then
        local value = map:get_value("name")
        local name = value and value:get_string()
        if name == "show_pinyin" then cfg.has_sw_pinyin = true
        elseif name == "show_ids" then cfg.has_sw_ids = true
        elseif name == "show_unicode" then cfg.has_sw_unicode = true
        elseif name == "show_block" then cfg.has_sw_block = true end
      end
    end
  end

  local function add_block(name)
    local b = BLOCK_ALIAS_MAP[to_lower_ascii(name)]
    if b ~= nil then
      cfg.disabled_blocks[b] = true
      cfg.disabled_blocks_enabled = true
    end
  end
  local list = conf:get_list(ns .. "/disabled_blocks")
  if list then
    for i = 0, list.size - 1 do
      local value = list:get_value_at(i)
      local name = value and value:get_string()
      if name then add_block(name) end
    end
  end
  local str = conf:get_string(ns .. "/disabled_blocks")
  if str and str ~= "" then
    for name in str:gmatch("[^,%s]+") do add_block(name) end
  end
  return cfg
end

local function is_reverse_active(ctx, prefix)
  if not ctx or prefix == "" then return false end
  if ctx:get_option("ascii_mode") then return false end
  return starts_with(ctx.input, prefix)
end

local function read_hybrid_state(ctx, prefix)
  local st = { components = {}, codes = {}, pending = "" }
  local prop = ctx:get_property(HYBRID_STATE_PROPERTY) or ""
  local pos = 1
  while pos <= #prop do
    local nl = prop:find("\n", pos, true)
    if not nl then nl = #prop + 1 end
    local line = prop:sub(pos, nl - 1)
    if line:sub(-1) == "\r" then line = line:sub(1, -2) end
    local tab = line:find("\t", 1, true)
    if tab then
      st.components[#st.components + 1] = line:sub(1, tab - 1)
      st.codes[#st.codes + 1] = line:sub(tab + 1)
    end
    pos = nl + 1
  end
  local expected = prefix
  for _, code in ipairs(st.codes) do expected = expected .. code .. "'" end
  local input = ctx.input
  if not starts_with(input, expected) then
    st.components = {}
    st.codes = {}
    st.pending = #input > #prefix and input:sub(#prefix + 1) or ""
    return st
  end
  st.pending = input:sub(#expected + 1)
  return st
end

local function save_hybrid_state(ctx, components, codes)
  local out = {}
  for i = 1, math.min(#components, #codes) do
    out[#out + 1] = components[i] .. "\t" .. codes[i] .. "\n"
  end
  ctx:set_property(HYBRID_STATE_PROPERTY, table.concat(out))
end

local function pinyin_mode(config, ctx)
  local mode = config.shuangpin_scheme
  if ctx then
    if ctx:get_option("shuangpin_flypy") or ctx:get_option("double_pinyin") then
      mode = "flypy"
    elseif ctx:get_option("shuangpin_zrm") then
      mode = "zrm"
    elseif ctx:get_option("pinyin_quanpin") then
      mode = "quanpin"
    end
  end
  return mode
end

local function has_variants(text)
  if not store_instance then return true end
  local pos = 0
  local cp
  cp, pos = decode_next(text, 0)
  if cp == 0 or pos ~= #text then return false end
  local id = store_instance:id_of_cp(cp)
  if id == INVALID_ID then return false end
  return #store_instance:variants(id) > 0
end

local function enter_o_mode(ctx)
  if ctx:get_option("_auto_commit") then
    ctx:set_property(SAVED_AUTO_COMMIT_PROPERTY, "1")
    ctx:set_option("_auto_commit", false)
  end
end

local function leave_o_mode(ctx)
  if ctx:get_property(SAVED_AUTO_COMMIT_PROPERTY) == "1" then
    ctx:set_property(SAVED_AUTO_COMMIT_PROPERTY, "")
    ctx:set_option("_auto_commit", true)
  end
end

local function selected_segment(ctx)
  local comp = ctx.composition
  if comp:empty() then return nil end
  local segs = comp:toSegmentation()
  local n = segs.size
  if n <= 0 then return nil end
  local last = segs:get_at(n - 1)
  if n >= 2 and last and last.start == last._end then
    return segs:get_at(n - 2)
  end
  return last
end

local function prepare_selected_candidate(seg)
  if not seg or not seg.menu then return nil end
  seg.menu:prepare(seg.selected_index + 1)
  return seg:get_candidate_at(seg.selected_index)
end

-- 括号闭合且内部拼音都已消歧后，求值并作为一个部件加入状态。
local function try_commit_closed_group(ctx, env)
  if not is_reverse_active(ctx, env.config.hybrid.prefix) then return false end
  local st = read_hybrid_state(ctx, env.config.hybrid.prefix)
  local group = parse_single_group(st.pending)
  if not group or not group.closed then return false end
  if find_last_ascii_run(st.pending) then return false end
  local store = ensure_store(env.config)
  if not store then return false end
  local components, excludes = evaluate_algebra_expression(
      store, group.inner, pinyin_mode(env.config, ctx))
  local component = format_group_component(group.op, components, excludes)
  local base = ctx.input:sub(1, #ctx.input - #st.pending)
  if component == "" then
    ctx.input = base
    return true
  end
  st.components[#st.components + 1] = component
  st.codes[#st.codes + 1] = st.pending
  save_hybrid_state(ctx, st.components, st.codes)
  ctx.input = base .. st.pending .. "'"
  return true
end

-- ---------------------------------------------------------------------------
-- Processor
-- ---------------------------------------------------------------------------

local function handle_selection_key(digit, ctx, env)
  if ctx.composition:empty() then return false end
  local seg = ctx.composition:back()
  if not seg.menu then return false end
  local page_size = env.engine.schema and env.engine.schema.page_size or 5
  if not page_size or page_size <= 0 then page_size = 5 end
  local page_start = (seg.selected_index // page_size) * page_size
  local target = page_start + digit - 1
  local count = seg.menu:prepare(target + 1)
  if target >= count then return true end
  ctx:select(target)
  return true
end

local function handle_ids_hotkey(key_event, ctx, env)
  if ctx.composition:empty() then return false end
  if not is_reverse_active(ctx, env.config.hybrid.prefix) then return false end
  local code = key_event.keycode
  local ctrl, alt, shift = key_event:ctrl(), key_event:alt(), key_event:shift()
  local is_return = code == 0xFF0D or code == 0xFF8D
  local digit = 0
  if code >= 0x31 and code <= 0x36 then digit = code - 0x30 end

  local action = false
  local ids_digit = 0
  if ctrl and not shift and not alt and is_return then action = true
  elseif alt and not shift and not ctrl and is_return then action = true
  elseif alt and not ctrl and (code == 0x69 or code == 0x49) then action = true
  elseif ctrl and shift and (code == 0x69 or code == 0x49) then action = true
  elseif ctrl and not shift and not alt and digit ~= 0 then
    action = true; ids_digit = digit
  elseif alt and not shift and not ctrl and digit ~= 0 then
    action = true; ids_digit = digit
  end
  if not action then return false end

  local seg = ctx.composition:back()
  if not seg.menu then return false end
  local index = seg.selected_index
  if ids_digit ~= 0 then
    local page_size = env.engine.schema and env.engine.schema.page_size or 5
    if not page_size or page_size <= 0 then page_size = 5 end
    local page_start = (seg.selected_index // page_size) * page_size
    index = page_start + ids_digit - 1
  end
  seg.menu:prepare(index + 1)
  local cand = seg:get_candidate_at(index)
  if not cand or cand.text == "" then return false end
  local store = ensure_store(env.config)
  if not store then return false end
  local ids = ids_of_text(store, cand.text)
  if ids == "" then return false end
  env.engine:commit_text(ids)
  leave_o_mode(ctx)
  ctx:clear()
  return true
end

local function matches_variant_replace_key(key_event, env)
  local target = env.variant_key
  if not target or target.keycode == 0 then return false end
  local modifier = key_event.modifier & ~2
  local target_modifier = target.modifier & ~2
  if modifier ~= target_modifier then return false end
  local code = key_event.keycode
  local target_code = target.keycode
  if code == target_code then return true end
  local function lower(c)
    if c >= 0x41 and c <= 0x5A then return c + 0x20 end
    return c
  end
  return lower(code) == lower(target_code)
end

local function try_replace_variant(ctx, env)
  if not ctx or not is_reverse_active(ctx, env.config.hybrid.prefix) then
    return false
  end
  if ctx:get_property(VARIANTS_PROPERTY) == "" then return false end
  if not ctx:has_menu() or ctx.composition:empty() then return false end

  local st = read_hybrid_state(ctx, env.config.hybrid.prefix)
  local trigger = env.config.variant_trigger
  if not ends_with(st.pending, trigger) then return false end

  local seg = ctx.composition:back()
  if not seg.menu then return false end
  seg.menu:prepare(seg.selected_index + 1)
  local cand = seg:get_candidate_at(seg.selected_index)
  if not cand or cand.text == "" or cand.type ~= "ids_reverse" then return false end

  local base = ctx:get_property(VARIANTS_PROPERTY)
  local variant = cand.text
  if variant == base then return true end

  local pending_query = st.pending:sub(1, #st.pending - #trigger)
  local result_replacement = pending_query == ""
  local component = variant
  if pending_query ~= "" and
     (pending_query:sub(1, 1) == "-" or pending_query:sub(1, 1) == "+"
      or pending_query:sub(1, 1) == "!") then
    component = pending_query:sub(1, 1) .. variant
  end

  ctx:set_property(VARIANTS_PROPERTY, "")
  ctx:set_property(VARIANTS_RESULT_PROPERTY, variant)

  if result_replacement then
    local expression = table.concat(st.components)
    expression = expression .. component
    save_hybrid_state(ctx, {}, {})
    ctx:clear()
    ctx.input = env.config.hybrid.prefix .. expression
  else
    local components = {}
    for _, c in ipairs(st.components) do components[#components + 1] = c end
    components[#components + 1] = component
    local codes = {}
    for _, c in ipairs(components) do codes[#codes + 1] = c end
    local input = env.config.hybrid.prefix
    for _, code in ipairs(codes) do input = input .. code .. "'" end
    save_hybrid_state(ctx, components, codes)
    ctx:clear()
    ctx.input = input
  end
  return true
end

local function on_select(ctx, env)
  if not is_reverse_active(ctx, env.config.hybrid.prefix) then return end
  local seg = selected_segment(ctx)
  if not seg then return end
  local cand = prepare_selected_candidate(seg)
  if not cand then return end

  local genuine = cand:get_genuine()
  local ctype = cand.type
  local gtype = genuine and genuine.type or ctype
  local is_result = ctype == "ids_reverse" or ctype == "hanzi_result"
                    or gtype == "ids_reverse" or gtype == "hanzi_result"
  local text = cand.text
  local component = genuine and genuine.text or text

  ctx:set_property(VARIANTS_PROPERTY, "")
  ctx:set_property(VARIANTS_RESULT_PROPERTY, "")

  if is_result then
    env.engine:commit_text(text)
    leave_o_mode(ctx)
    ctx:clear()
    return
  end

  local st = read_hybrid_state(ctx, env.config.hybrid.prefix)

  -- 括号内：只把当前拼音段替换成所选汉字，不提交为状态部件。
  local group = parse_single_group(st.pending)
  if group then
    local run_start = find_last_ascii_run(st.pending)
    if run_start and component ~= "" then
      local new_pending = replace_last_ascii_run(st.pending, component)
      local base = ctx.input:sub(1, #ctx.input - #st.pending)
      ctx.input = base .. new_pending
      try_commit_closed_group(ctx, env)
    end
    return
  end

  if st.pending ~= "" and component ~= "" then
    local first = st.pending:sub(1, 1)
    if (first == "-" or first == "+" or first == "!")
       and component:sub(1, 1) ~= first then
      component = first .. component
    end
  end
  local base = ctx.input:sub(1, #ctx.input - #st.pending)
  st.components[#st.components + 1] = component
  st.codes[#st.codes + 1] = component
  save_hybrid_state(ctx, st.components, st.codes)
  ctx.input = base .. component .. "'"
end

local function on_update(ctx, env)
  if not is_reverse_active(ctx, env.config.hybrid.prefix) then return end
  if not ctx.composition:empty() then
    ctx.composition:back().prompt = env.config.hybrid.tips
  end
end

local function processor_init(env)
  env.config = load_config(env)
  env.conns = {}
  local ctx = env.engine.context
  env.conns[#env.conns + 1] = ctx.select_notifier:connect(
    function(c) on_select(c, env) end)
  env.conns[#env.conns + 1] = ctx.update_notifier:connect(
    function(c) on_update(c, env) end)
  if _G.KeyEvent and env.config.variant_replace_key ~= "" then
    env.variant_key = _G.KeyEvent(env.config.variant_replace_key)
  end
end

local function processor_fini(env)
  if env.conns then
    for _, c in ipairs(env.conns) do c:disconnect() end
    env.conns = nil
  end
end

local function processor_func(key_event, env)
  if key_event:release() then return 2 end
  local ctx = env.engine.context
  if not ctx then return 2 end

  if handle_ids_hotkey(key_event, ctx, env) then return 1 end

  local repr = key_event:repr()
  local code = key_event.keycode
  local ctrl, alt, shift = key_event:ctrl(), key_event:alt(), key_event:shift()

  if not is_reverse_active(ctx, env.config.hybrid.prefix) then
    if not ctx:get_option("ascii_mode") and not ctrl and not alt
       and not ctx:is_composing() then
      if repr == env.config.search.prefix then
        ctx:push_input(env.config.search.prefix)
        return 1
      end
      if repr == env.config.hybrid.prefix then
        ctx:push_input(env.config.hybrid.prefix)
        enter_o_mode(ctx)
        return 1
      end
    end
    return 2
  end

  enter_o_mode(ctx)

  if code == 0xFF1B or repr == "Escape" then
    ctx:set_property(VARIANTS_PROPERTY, "")
    ctx:set_property(VARIANTS_RESULT_PROPERTY, "")
    leave_o_mode(ctx)
    ctx:clear()
    return 1
  end

  if code == 0xFF0D or code == 0xFF8D or repr == "Return" then
    local st = read_hybrid_state(ctx, env.config.hybrid.prefix)
    if #st.components == 0 and st.pending == "" then
      env.engine:commit_text(env.config.hybrid.prefix)
      leave_o_mode(ctx)
      ctx:clear()
      return 1
    end
    if ctx:has_menu() then
      local cand = ctx:get_selected_candidate()
      if cand then
        local genuine = cand:get_genuine()
        local ctype = cand.type
        local gtype = genuine and genuine.type or ctype
        local is_result = ctype == "ids_reverse" or ctype == "hanzi_result"
                          or gtype == "ids_reverse" or gtype == "hanzi_result"
        local is_multi_component =
            (ctype == "hanzi_component" or gtype == "hanzi_component")
            and utf8_length(cand.text) > 1
        if is_result or is_multi_component then
          ctx:confirm_current_selection()
          return 1
        end
        -- 括号内单字部件也用回车确认，交给 on_select 替换当前拼音段。
        if parse_single_group(st.pending) then
          ctx:confirm_current_selection()
          return 1
        end
      end
    end
    if parse_single_group(st.pending) then return 1 end
    if st.pending ~= "" and st.pending ~= "-" and st.pending ~= "+"
       and st.pending ~= "!" then
      if not ctx.composition:empty() then ctx.composition:pop_back() end
      st.components[#st.components + 1] = st.pending
      st.codes[#st.codes + 1] = st.pending
      save_hybrid_state(ctx, st.components, st.codes)
      ctx:push_input("'")
      return 1
    end
    if ctx:has_menu() and ctx:get_selected_candidate() then
      ctx:confirm_current_selection()
      return 1
    end
    return 2
  end

  if code == 0xFF08 or repr == "BackSpace" then
    ctx:set_property(VARIANTS_RESULT_PROPERTY, "")
    local st = read_hybrid_state(ctx, env.config.hybrid.prefix)
    if st.pending ~= "" then
      local pending_chars = utf8_length(st.pending)
      local pending_bytes = last_utf8_char_bytes(st.pending)
      if pending_bytes == 0 then pending_bytes = 1 end
      if pending_chars == 1 and #st.components > 0 then
        ctx:pop_input(pending_bytes)
        while not ctx.composition:empty()
              and ctx.composition:back().start == ctx.composition:back()._end do
          ctx.composition:pop_back()
        end
        if not ctx.composition:empty() then
          local sep = ctx.composition:back()
          sep.tags = Set({ env.config.hybrid.tag, RESULT_SEGMENT_TAG })
          sep.status = "kVoid"
        end
        ctx.input = ctx.input
        return 1
      end
      ctx:pop_input(pending_bytes)
    elseif #st.components > 0 then
      local code_len = #st.codes[#st.codes]
      st.components[#st.components] = nil
      st.codes[#st.codes] = nil
      save_hybrid_state(ctx, st.components, st.codes)
      if not ctx.composition:empty() then ctx.composition:pop_back() end
      if not ctx.composition:empty() then
        ctx.composition:back().status = "kGuess"
      end
      ctx:pop_input(code_len + 1)
    else
      leave_o_mode(ctx)
      ctx:clear()
    end
    return 1
  end

  if not ctrl and not alt and code >= 0x31 and code <= 0x36
     and not (shift and code == 0x31) then
    if ctx:has_menu() then
      handle_selection_key(code - 0x30, ctx, env)
      return 1
    end
    return 2
  end
  if not ctrl and not alt and (code == 0x3B or repr == "semicolon") then
    if ctx:has_menu() then
      handle_selection_key(2, ctx, env)
      return 1
    end
    return 2
  end
  if code == 0x20 or repr == "space" then
    if not shift and ctx:has_menu() and ctx:get_selected_candidate() then
      ctx:confirm_current_selection()
      return 1
    end
    return 2
  end

  local is_letter = not ctrl and not alt and #repr == 1
                    and repr:match("^[A-Za-z]$") ~= nil
  local is_variant = not ctrl and not alt
                     and (code == 0x2F or repr == "/" or repr == "slash")
  local is_minus = not ctrl and not alt
                    and (code == 0x2D or repr == "minus" or repr == "-")
  local is_plus = not ctrl and not alt
                  and (code == 0x2B or code == 0x3D or repr == "plus"
                       or repr == "equal" or repr == "+" or repr == "=")
  local is_exclude = not ctrl and not alt
                     and (code == 0x21 or repr == "!" or repr == "exclam"
                          or (code == 0x31 and shift))
  local is_lparen = not ctrl and not alt
                    and (code == 0x28 or repr == "(" or repr == "parenleft")
  local is_rparen = not ctrl and not alt
                    and (code == 0x29 or repr == ")" or repr == "parenright")

  if is_variant then
    ctx:set_property(VARIANTS_RESULT_PROPERTY, "")
    if not ctx:has_menu() or ctx.composition:empty() then return 2 end
    local seg = ctx.composition:back()
    if not seg.menu then return 2 end
    seg.menu:prepare(seg.selected_index + 1)
    local cand = seg:get_candidate_at(seg.selected_index)
    if not cand or cand.text == "" then return 2 end
    local genuine = cand:get_genuine()
    local otype = cand.type
    local gtype = genuine and genuine.type or otype
    local is_result = otype == "ids_reverse" or otype == "hanzi_result"
                      or gtype == "ids_reverse" or gtype == "hanzi_result"
    local is_single_component = (otype == "hanzi_component"
                                 or gtype == "hanzi_component")
                                and utf8_length(cand.text) == 1
    if not is_result and not is_single_component then
      ctx:set_property(VARIANTS_PROPERTY, "")
      return 2
    end
    local base = is_result and genuine and genuine.text or cand.text
    if is_result then
      local st = read_hybrid_state(ctx, env.config.hybrid.prefix)
      if st.pending == "" and #st.components == 1
         and utf8_length(st.components[1]) == 1
         and st.components[1] ~= base and not has_variants(base) then
        local component = st.components[1]
        ctx:set_property(VARIANTS_PROPERTY, component)
        save_hybrid_state(ctx, {}, {})
        ctx:clear()
        ctx.input = env.config.hybrid.prefix .. component
                    .. env.config.variant_trigger
        return 1
      end
    end
    ctx:set_property(VARIANTS_PROPERTY, base)
    ctx:push_input(env.config.variant_trigger)
    return 1
  end

  if matches_variant_replace_key(key_event, env) and try_replace_variant(ctx, env) then
    return 1
  end

  if is_letter or is_minus or is_plus or is_exclude
     or is_lparen or is_rparen then
    local variant_result = ctx:get_property(VARIANTS_RESULT_PROPERTY)
    if variant_result ~= "" then
      local st = read_hybrid_state(ctx, env.config.hybrid.prefix)
      local already_component = false
      if #st.components > 0 then
        local last = st.components[#st.components]
        local base = (last:sub(1, 1) == "-" or last:sub(1, 1) == "+"
                      or last:sub(1, 1) == "!") and last:sub(2) or last
        already_component = base == variant_result
      end
      ctx:set_property(VARIANTS_RESULT_PROPERTY, "")
      ctx:set_property(VARIANTS_PROPERTY, "")
      if not already_component then
        local display = st.pending
        if display == "" then display = variant_result end
        save_hybrid_state(ctx, { variant_result }, { display })
        ctx:clear()
        ctx.input = env.config.hybrid.prefix .. display .. "'"
      end
    end

    local ch
    if is_minus then ch = "-"
    elseif is_plus then ch = "+"
    elseif is_exclude then ch = "!"
    elseif is_lparen then ch = "("
    elseif is_rparen then ch = ")"
    else ch = repr:sub(1, 1) end
    if ch:match("^[A-Z]$") then ch = ascii_lower(ch) end
    if ctx.caret_pos ~= #ctx.input then ctx.caret_pos = #ctx.input end
    if not ctx.composition:empty()
       and ctx.composition:back():has_tag(env.config.hybrid.tag) then
      ctx:clear_non_confirmed_composition()
    end
    ctx:set_property(VARIANTS_PROPERTY, "")
    ctx:push_input(ch)
    if is_rparen then try_commit_closed_group(ctx, env) end
    return 1
  end

  return 2
end

-- ---------------------------------------------------------------------------
-- Segmentor
-- ---------------------------------------------------------------------------

local function segmentor_init(env)
  env.config = load_config(env)
end

local function segmentor_func(segmentation, env)
  if not segmentation then return false end
  local ctx = env.engine.context
  local input = segmentation.input
  local start = segmentation:get_current_start_position()

  if start == 0 and (not ctx or not ctx:get_option("ascii_mode"))
     and starts_with(input, env.config.search.prefix) then
    local seg = Segment(0, #input)
    seg.tags = Set({ env.config.search.tag })
    seg.prompt = env.config.search.tips
    if not segmentation:add_segment(seg) then return true end
    if not segmentation:empty() then
      segmentation:back().prompt = env.config.search.tips
    end
    return false
  end

  if not is_reverse_active(ctx, env.config.hybrid.prefix) then return true end
  if start >= #input then return false end

  local prefix_end = (start == 0) and #env.config.hybrid.prefix or start + 1
  local is_prefix = (start == 0)
                    or (start < #input and input:sub(start + 1, start + 1) == "'")

  local seg
  if is_prefix and prefix_end < #input then
    seg = Segment(start, prefix_end)
    if start ~= 0 then seg.tags = Set({ "phony" }) end
    seg.status = "kConfirmed"
  elseif is_prefix then
    seg = Segment(start, #input)
    if start == 0 then
      seg.tags = Set({ env.config.hybrid.tag })
    else
      seg.tags = Set({ env.config.hybrid.tag, RESULT_SEGMENT_TAG })
    end
  else
    local next_quote = input:find("'", start + 2, true)
    local first_char = start < #input and input:sub(start + 1, start + 1) or ""
    if first_char == "-" or first_char == "+" or first_char == "!" then
      local has_code_after = (start + 1 < #input)
                             and (next_quote == nil or next_quote > start + 2)
      if has_code_after then
        seg = Segment(start, start + 1)
        seg.status = "kConfirmed"
        seg.tags = Set({ "confirmed_component" })
      elseif start + 1 == #input then
        seg = Segment(start, #input)
        seg.tags = Set({ env.config.hybrid.tag, RESULT_SEGMENT_TAG })
      else
        seg = Segment(start, start + 1)
        seg.status = "kConfirmed"
        seg.tags = Set({ "confirmed_component" })
      end
    elseif next_quote then
      seg = Segment(start, next_quote - 1)
      seg.status = "kConfirmed"
      seg.tags = Set({ "confirmed_component" })
    else
      seg = Segment(start, #input)
      seg.tags = Set({ "abc", "hanzi_component" })
    end
  end
  seg.prompt = env.config.hybrid.tips
  segmentation:add_segment(seg)
  return false
end

-- ---------------------------------------------------------------------------
-- Translator
-- ---------------------------------------------------------------------------

local function show_options(config, ctx)
  local show_pinyin = config.show_pinyin
  local show_ids = config.show_ids
  local show_unicode = config.show_unicode
  local show_block = config.show_block
  if ctx then
    if config.has_sw_pinyin then show_pinyin = ctx:get_option("show_pinyin") end
    if config.has_sw_ids then show_ids = ctx:get_option("show_ids") end
    if config.has_sw_unicode then show_unicode = ctx:get_option("show_unicode") end
    if config.has_sw_block then show_block = ctx:get_option("show_block") end
  end
  return show_pinyin, show_ids, show_unicode, show_block
end

local function build_search_candidates(store, results, preedit, start_pos,
                                       end_pos, config, ctx)
  local show_pinyin, show_ids, show_unicode, show_block = show_options(config, ctx)
  local emitted = 0
  for _, id in ipairs(results) do
    local comment = build_comment(store, id, show_pinyin, show_ids,
                                  show_unicode, show_block, config.ids_format)
    local ch = cp_to_utf8(store:cp_of_id(id))
    local cand = Candidate("ids_reverse", start_pos, end_pos, ch, comment)
    cand.preedit = preedit
    cand.quality = 100.0
    yield(cand)
    emitted = emitted + 1
  end
  return emitted
end

local function query_search(input, segment, env)
  local start_pos, end_pos = segment.start, segment._end
  local query_raw = input
  local prefix = env.config.search.prefix
  if prefix ~= "" then
    if not starts_with(input, prefix) then return end
    query_raw = input:sub(#prefix + 1)
  end
  if query_raw == "" then
    local cand = Candidate("ids_reverse", start_pos, end_pos, "",
                           env.config.search.tips)
    cand.preedit = input .. " " .. env.config.search.tips
    cand.quality = 100.0
    yield(cand)
    return
  end
  local store = ensure_store(env.config)
  if not store then return end
  local mode = pinyin_mode(env.config, env.engine.context)
  local results = search_components(store, query_raw, env.config.max_results,
                                    mode, env.config.disabled_blocks_enabled
                                          and env.config.disabled_blocks or nil)
  if #results == 0 then return end
  build_search_candidates(store, results, prefix == "" and "" or input,
                          start_pos, end_pos, env.config, env.engine.context)
end

local function build_result_candidates(store, results, segment, pending,
                                       config, ctx)
  local show_pinyin, show_ids, show_unicode, show_block = show_options(config, ctx)
  local preedit = "\t"
  if pending == "-" then preedit = "-\t"
  elseif pending == "+" then preedit = "+\t"
  elseif pending == "!" then preedit = "!\t" end
  for _, id in ipairs(results) do
    local text = cp_to_utf8(store:cp_of_id(id))
    local comment = build_comment(store, id, show_pinyin, show_ids,
                                  show_unicode, show_block, config.ids_format)
    local cand = Candidate("ids_reverse", segment.start, segment._end, text,
                           comment)
    cand.preedit = preedit
    cand.quality = 100.0
    yield(cand)
  end
end

local function build_component_candidates(store, results, segment, config)
  for _, id in ipairs(results) do
    if not (config.disabled_blocks_enabled
            and config.disabled_blocks[store:hot(id).block]) then
      local text = cp_to_utf8(store:cp_of_id(id))
      local cand = Candidate("hanzi_component", segment.start, segment._end,
                             text, "")
      cand.quality = 100.0
      yield(cand)
    end
  end
end

local function build_multi_component_candidates(store, tokens, segment, config,
                                                ctx)
  local per_token_limit = 4
  local max_candidates = 40
  local disabled = config.disabled_blocks_enabled and config.disabled_blocks
                   or nil

  local options = {}
  for _, token in ipairs(tokens) do
    local opts = {}
    if not is_ascii_alpha_only(token) and utf8_length(token) == 1 then
      local cp = decode_next(token, 0)
      local id = cp ~= 0 and store:id_of_cp(cp) or INVALID_ID
      if id == INVALID_ID then return end
      if disabled and disabled[store:hot(id).block] then return end
      opts[1] = { text = token, rank = 0, freq = store:hot(id).freq }
    else
      local ids = get_component_candidates(store, token,
                                           pinyin_mode(config, ctx), 80)
      for _, id in ipairs(ids) do
        if not (disabled and disabled[store:hot(id).block]) then
          local cp = store:cp_of_id(id)
          if cp ~= 0 then
            opts[#opts + 1] = { text = cp_to_utf8(cp), rank = #opts,
                                freq = store:hot(id).freq }
            if #opts >= per_token_limit then break end
          end
        end
      end
    end
    if #opts == 0 then return end
    options[#options + 1] = opts
  end

  local function better(a, b)
    if a.rank_sum ~= b.rank_sum then return a.rank_sum < b.rank_sum end
    if a.freq_sum ~= b.freq_sum then return a.freq_sum > b.freq_sum end
    return a.text < b.text
  end

  local combos = { { text = "", rank_sum = 0, freq_sum = 0 } }
  for _, opts in ipairs(options) do
    local next_combos = {}
    for _, base in ipairs(combos) do
      for _, opt in ipairs(opts) do
        next_combos[#next_combos + 1] = {
          text = base.text .. opt.text,
          rank_sum = base.rank_sum + opt.rank,
          freq_sum = base.freq_sum + opt.freq,
        }
      end
    end
    table.sort(next_combos, better)
    if #next_combos > max_candidates * 4 then
      local trimmed = {}
      for i = 1, max_candidates * 4 do trimmed[i] = next_combos[i] end
      next_combos = trimmed
    end
    combos = next_combos
  end
  table.sort(combos, better)

  local limit = max_candidates
  if config.max_results > 0 then
    limit = math.min(limit, config.max_results)
  end
  local seen = {}
  local emitted = 0
  for _, combo in ipairs(combos) do
    if emitted >= limit then break end
    if combo.text ~= "" and not seen[combo.text] then
      seen[combo.text] = true
      local cand = Candidate("hanzi_component", segment.start, segment._end,
                             combo.text, "多部件")
      cand.quality = 100.0
      yield(cand)
      emitted = emitted + 1
    end
  end
end

local function build_variant_candidates(store, base_id, base, preedit, segment,
                                        config, ctx)
  local show_pinyin, show_ids, show_unicode, show_block = show_options(config, ctx)
  local variants = base_id ~= INVALID_ID and store:variants(base_id) or {}
  if #variants == 0 then
    local cand = Candidate("ids_reverse", segment.start, segment._end, base,
                           "〔无异体字〕")
    cand.preedit = preedit
    cand.quality = 100.0
    yield(cand)
    return
  end
  for _, vid in ipairs(variants) do
    if vid < store.num_chars then
      local text = cp_to_utf8(store:cp_of_id(vid))
      local comment = build_comment(store, vid, show_pinyin, show_ids,
                                    show_unicode, show_block, config.ids_format)
      local cand = Candidate("ids_reverse", segment.start, segment._end, text,
                             comment)
      cand.preedit = preedit
      cand.quality = 100.0
      yield(cand)
    end
  end
end

local function parse_pending_components(store, pending, mode)
  local out = { valid = false, tokens = {} }
  if pending == "" then return out end
  local i = 1
  local first = pending:sub(1, 1)
  if first == "-" or first == "+" or first == "!" then i = 2 end
  local ascii = {}
  local function flush_ascii()
    if #ascii == 0 then return true end
    local s = table.concat(ascii)
    ascii = {}
    local segs = segment_alpha_string(store, s, mode)
    for _, seg in ipairs(segs) do
      if seg ~= "" then out.tokens[#out.tokens + 1] = seg end
    end
    return #segs > 0
  end
  while i <= #pending do
    local c = pending:sub(i, i)
    if c == "-" or c == "+" then return out end
    if c == "'" or c == " " or c == "\t" or c == "\r" or c == "\n"
       or c == "." or c == "_" then
      if not flush_ascii() then return out end
      i = i + 1
    elseif c:byte() < 0x80 then
      if not c:match("^[A-Za-z]$") then return out end
      ascii[#ascii + 1] = c
      i = i + 1
    else
      if not flush_ascii() then return out end
      local start = i
      local cp
      cp, i = decode_next(pending, i - 1)
      i = i + 1
      if i - 1 == start - 1 or cp == 0 or not is_component_code_point(cp) then
        return out
      end
      out.tokens[#out.tokens + 1] = pending:sub(start, i - 1)
    end
  end
  if not flush_ascii() then return out end
  if #out.tokens > 8 then return out end
  out.valid = #out.tokens > 0
  return out
end

local function query_hybrid(segment, env)
  local ctx = env.engine.context
  if not ctx or not is_reverse_active(ctx, env.config.hybrid.prefix) then return end
  local has_hybrid = segment:has_tag(env.config.hybrid.tag)
  local has_component = segment:has_tag("hanzi_component")
  if not has_hybrid and not has_component then return end
  local store = ensure_store(env.config)
  if not store then return end

  local st = read_hybrid_state(ctx, env.config.hybrid.prefix)
  local pending = st.pending
  local variant_pending = ends_with(pending, env.config.variant_trigger)
  local variant_result = ctx:get_property(VARIANTS_RESULT_PROPERTY)
  local variant_result_stage = false
  if not variant_pending and variant_result ~= "" then
    if ends_with(pending, variant_result) then
      variant_result_stage = true
    elseif pending == "" and #st.components > 0 then
      local last = st.components[#st.components]
      local base = (last:sub(1, 1) == "-" or last:sub(1, 1) == "+"
                    or last:sub(1, 1) == "!") and last:sub(2) or last
      variant_result_stage = base == variant_result
    end
  end

  if variant_result_stage then
    local cp = decode_next(variant_result, 0)
    local id = cp ~= 0 and store:id_of_cp(cp) or INVALID_ID
    local comment = ""
    if id ~= INVALID_ID then
      local show_pinyin, show_ids, show_unicode, show_block =
          show_options(env.config, ctx)
      comment = build_comment(store, id, show_pinyin, show_ids, show_unicode,
                              show_block, env.config.ids_format)
    end
    local preedit = pending == "" and "\t" or ""
    local cand = Candidate("ids_reverse", segment.start, segment._end,
                           variant_result, comment)
    cand.preedit = preedit
    cand.quality = 100.0
    yield(cand)
    return
  end

  if variant_pending then
    local base = ctx:get_property(VARIANTS_PROPERTY)
    if base ~= "" then
      local cp = decode_next(base, 0)
      local base_id = cp ~= 0 and store:id_of_cp(cp) or INVALID_ID
      local query = pending:sub(1, #pending - #env.config.variant_trigger)
      local preedit = env.config.variant_trigger .. "\t"
      if query ~= "" then
        preedit = base .. env.config.variant_trigger .. "\t"
      end
      build_variant_candidates(store, base_id, base, preedit, segment,
                               env.config, ctx)
    end
    return
  end

  -- 括号编辑：只消歧最后一个拼音段，选中后写回 pending。
  local group = parse_single_group(pending)
  if group and has_component then
    local run_start, run_end = find_last_ascii_run(pending)
    if run_start then
      local run = pending:sub(run_start, run_end)
      local mode = pinyin_mode(env.config, ctx)
      local tokens = segment_alpha_string(store, run, mode)
      if #tokens >= 2 then
        build_multi_component_candidates(store, tokens, segment, env.config, ctx)
      elseif #tokens == 1 then
        local results = get_component_candidates(store, tokens[1], mode, 80)
        build_component_candidates(store, results, segment, env.config)
      end
    end
    return
  end

  if (pending == "" or pending == "-" or pending == "+" or pending == "!")
     and #st.components > 0 then
    local results = search_hybrid(store, st.components,
                                  pinyin_mode(env.config, ctx),
                                  env.config.max_results,
                                  env.config.disabled_blocks_enabled
                                    and env.config.disabled_blocks or nil)
    build_result_candidates(store, results, segment, pending, env.config, ctx)
  elseif pending ~= "" and pending ~= "-" and pending ~= "+" and pending ~= "!"
         and has_component then
    local parsed = parse_pending_components(store, pending,
                                            pinyin_mode(env.config, ctx))
    if parsed.valid and #parsed.tokens >= 2 then
      build_multi_component_candidates(store, parsed.tokens, segment,
                                       env.config, ctx)
    elseif parsed.valid and #parsed.tokens == 1 then
      if not is_ascii_alpha_only(parsed.tokens[1]) then
        local cand = Candidate("hanzi_component", segment.start, segment._end,
                               parsed.tokens[1], "")
        cand.quality = 100.0
        yield(cand)
      else
        local results = get_component_candidates(store, parsed.tokens[1],
                                                 pinyin_mode(env.config, ctx), 80)
        build_component_candidates(store, results, segment, env.config)
      end
    elseif is_component_expression(pending) then
      local cand = Candidate("hanzi_component", segment.start, segment._end,
                             pending, "部件表达式")
      cand.quality = 100.0
      yield(cand)
    else
      local results = get_component_candidates(store, pending,
                                               pinyin_mode(env.config, ctx), 80)
      build_component_candidates(store, results, segment, env.config)
    end
  end
end

local function translator_init(env)
  env.config = load_config(env)
end

local function translator_func(input, segment, env)
  if segment:has_tag(env.config.search.tag) then
    query_search(input, segment, env)
  else
    query_hybrid(segment, env)
  end
end

-- ---------------------------------------------------------------------------
-- Filter
-- ---------------------------------------------------------------------------

local function filter_init(env)
  env.config = load_config(env)
end

local function filter_func(input, env)
  local ctx = env.engine.context
  if not ctx or ctx:get_option("ascii_mode") then
    for cand in input:iter() do yield(cand) end
    return
  end

  if ctx.input == env.config.search.prefix then return end

  if not is_reverse_active(ctx, env.config.hybrid.prefix) then
    for cand in input:iter() do yield(cand) end
    return
  end

  if ctx.composition:empty() then return end
  local seg = ctx.composition:back()
  if not seg:has_tag("hanzi_component") and not seg:has_tag(RESULT_SEGMENT_TAG) then
    return
  end

  local seen = {}
  local scanned = 0
  for cand in input:iter() do
    scanned = scanned + 1
    if scanned > 2000 then break end
    local genuine = cand:get_genuine()
    local ctype = genuine and genuine.type or cand.type
    if ctype == "ids_reverse" or ctype == "hanzi_result" then
      yield(cand)
    elseif ctx.composition:empty() then
      yield(cand)
    else
      local seg2 = ctx.composition:back()
      if seg2:has_tag("hanzi_component") then
        if cand._end == seg2._end and is_component_expression(cand.text)
           and not seen[cand.text] then
          seen[cand.text] = true
          yield(ShadowCandidate(cand, "hanzi_component"))
        end
      end
    end
  end
end

-- ---------------------------------------------------------------------------
-- 导出
-- ---------------------------------------------------------------------------

M.processor = { init = processor_init, func = processor_func, fini = processor_fini }
M.segmentor = { init = segmentor_init, func = segmentor_func }
M.translator = { init = translator_init, func = translator_func }
M.filter = { init = filter_init, func = filter_func }

-- 供纯 Lua 测试使用的内部接口；不影响 Rime 组件。
M._internal = {
  Store = Store,
  store = function() return store_instance end,
  search_components = search_components,
  search_hybrid = search_hybrid,
  get_component_candidates = get_component_candidates,
  build_comment = build_comment,
  ids_of_text = ids_of_text,
  format_ids = format_ids,
  canon = canon,
  utf8_chars = utf8_chars,
  utf8_length = utf8_length,
  segment_alpha_string = segment_alpha_string,
  decode_shuangpin = decode_shuangpin,
  normalize_component = normalize_component,
  parse_pending_components = parse_pending_components,
  parse_algebra_steps = parse_algebra_steps,
  parse_single_group = parse_single_group,
  find_last_ascii_run = find_last_ascii_run,
  replace_last_ascii_run = replace_last_ascii_run,
  evaluate_algebra_expression = evaluate_algebra_expression,
  format_group_component = format_group_component,
  is_component_expression = is_component_expression,
  build_multi_component_candidates = build_multi_component_candidates,
  build_variant_candidates = build_variant_candidates,
  clear_query_caches = clear_query_caches,
  leaf_struct_hash = leaf_struct_hash,
}

return M
