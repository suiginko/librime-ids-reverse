-- 延迟基准：纯 Lua 5.5，不依赖 Rime 引擎。
--
-- 用法：
--   lua5.5 plugins/ids-reverse/tests/bench_ids_reverse.lua
--   lua5.5 plugins/ids-reverse/tests/bench_ids_reverse.lua <ids_data.bin>

local script = arg and arg[0] or "tests/bench_ids_reverse.lua"
local root = script:match("^(.*)[/\\]tests[/\\]") or "."
local bin = (arg and arg[1]) or (root .. "/data/ids_data.bin")
local module = root .. "/ids_reverse.lua"

local M = dofile(module)
local I = M._internal
local function now() return os.clock() end
local function ms(t) return t * 1000 end

local load_start = now()
local store, err = I.Store.load(bin, "")
local load_time = now() - load_start
assert(store, "failed to load " .. bin .. ": " .. tostring(err))
local full = I.get_codec(store, { algebra = {} })

local function percentile(sorted, p)
  local idx = math.max(1, math.min(#sorted, math.ceil(#sorted * p)))
  return sorted[idx]
end

local function bench(fn, cold_iter, warm_iter)
  fn()
  I.clear_query_caches()
  local t = now()
  fn()
  local first = now() - t

  local cold = {}
  for i = 1, cold_iter do
    I.clear_query_caches()
    local a = now()
    fn()
    cold[i] = ms(now() - a)
  end
  table.sort(cold)

  local warm = {}
  for i = 1, warm_iter do
    local a = now()
    fn()
    warm[i] = ms(now() - a)
  end
  table.sort(warm)

  local function avg(values)
    local sum = 0
    for _, v in ipairs(values) do sum = sum + v end
    return sum / #values
  end
  return {
    first = ms(first),
    cold_avg = avg(cold),
    cold_p95 = percentile(cold, 0.95),
    cold_max = cold[#cold],
    warm_avg = avg(warm),
    warm_p95 = percentile(warm, 0.95),
    warm_max = warm[#warm],
  }
end

local cases = {
  { "I 单字 木", function() I.search_components(store, "木", 60, full, nil) end },
  { "I 双部件 木口", function() I.search_components(store, "木口", 60, full, nil) end },
  { "I 拼音 kouhao", function() I.search_components(store, "kouhao", 60, full, nil) end },
  { "I 前缀 m", function() I.search_components(store, "m", 60, full, nil) end },
  { "O 单部件 木木", function() I.search_hybrid(store, { "木木" }, full, 60, nil) end },
  { "O 双部件 木口", function() I.search_hybrid(store, { "木口" }, full, 60, nil) end },
  { "O 减法 森木-", function() I.search_hybrid(store, { "森", "木", "-" }, full, 60, nil) end },
  { "O 加减 森木-氵+", function() I.search_hybrid(store, { "森", "木", "-", "氵", "+" }, full, 60, nil) end },
  { "O 排除 木木口!", function() I.search_hybrid(store, { "木木", "口", "!" }, full, 60, nil) end },
  { "O 递归 章日-十-", function() I.search_hybrid(store, { "章", "日", "-", "十", "-" }, full, 60, nil) end },
  { "O 异体基准 艹亏", function() I.search_hybrid(store, { "艹", "亏" }, full, 60, nil) end },
  { "O 单层表达式 森木-", function() I.search_hybrid(store, { "森木-" }, full, 60, nil) end },
  { "部件候选 mu", function() I.get_component_candidates(store, "mu", full, 80) end },
  { "部件候选 m", function() I.get_component_candidates(store, "m", full, 80) end },
}

print(string.format("数据加载: %.1f ms (%.1f MB), 字符 %d, token %d, key %d",
                    ms(load_time), #store.blob / 1024 / 1024,
                    store.num_chars, store.num_tokens, store.num_keys))
print()
print(string.format("%-22s %8s %9s %9s %9s %9s %9s %9s",
                    "查询", "首次", "冷均", "冷P95", "冷最大", "温均", "温P95", "温最大"))
print(string.rep("-", 94))
for _, case in ipairs(cases) do
  local r = bench(case[2], 40, 400)
  print(string.format("%-22s %8.3f %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f",
                      case[1], r.first, r.cold_avg, r.cold_p95, r.cold_max,
                      r.warm_avg, r.warm_p95, r.warm_max))
end
print("\n单位: ms，Lua 5.5.1，40 次冷查询 / 400 次温查询")
