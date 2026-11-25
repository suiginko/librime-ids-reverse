# rime-ids-reverse

librime 插件：**部件检字（IDS 模糊搜字）翻译器**，C++ 原生实现。

另提供一份单文件 **Lua 5.5** 实现 `ids_reverse.lua`，功能与 C++ 版对等，
直接复用同一份 `ids_data.bin` 和 `ids_reverse` 配置。两者二选一挂载。

包含两套模式：

- **I 模式**：`I` + 部件/拼音，例如 `I木口`、`Ikouhao`，由原生 processor/segmentor 触发。
- **O 模式**：`O` + 汉字、全拼、双拼混合检字，支持 `-` 减法、`+` 加法、回车字母直出、IDS 结构上屏。

C++ 版 O 模式为**插件自包含实现**，不需要 Lua，也不依赖任何外部输入法方案；
Lua 5.5 版见下文「Lua 5.5 版本」。

---

## 数据格式（IDS4 v3）

单文件 `ids_data.bin`（约 30MB），面向**查询延迟**设计：`mmap` 惰性映射、
稠密字 id、整数部件匹配、预计算叶子/原子部件、整数倒排键、每键连续 postings。

> IDS4 v3 必须包含 `PY_CHAR` section。旧 v2/v1/IDS3 数据文件会在加载时报错，
> 需要重新生成 `ids_data.bin`。

- **稠密 id**：103k+ 汉字映射到 `0..N-1`（`CP2ID`/`ID2CP`）。
- **CHAR**：每字 24 字节定长记录 `{freq, flags, char_token, depth, block,
  alts_off, leaf_off, atom_off}`，一次 cache line 读取评分所需全部热字段。
- **整数部件匹配**：15k+ 部件 token 表（`u16`）；alts 存成按长度分组的平坦
  `u16` 序列 + 每变体 `u32` 有序签名。
- **预计算派生数据**：
  - `LEAVES`：每字顶层部件 token 序列，替代运行时 UTF-8 IDS 拆字；
  - `ATOMS`：每字原子部件 token 序列，替代运行时 `std::map<string,int>` 分解；
  - `TOK_CANON`：token → 归一化 token 直查表；
  - `ATOMIC_SET`：原子部件 token 集合，生成器与运行时共用；
  - `LEAF_STRUCT`：顶层叶子多重集 hash → 字 id 的结构索引，用于“只包含这些
    部件”的精确结构候选，不受字频 head 和普通排序上限影响。
- **倒排**：
  - `KEY_LOOKUP`/`KEY_REC`：按 `token_id + 重数` 直接寻址，不再哈希字符串键；
  - `POST`：每个键的 `head` 与完整集合连续存放；完整集合大键用**位图**、
    其余用 id 升序 `u32`；位图阈值 `>1024`。
  - 单 term 查询直接使用字频序 `head`，不再遍历十万级完整倒排。
- **PY/TOKENS**：拼音→token、token 词表（含码点）。
- **PY_CHAR**：无调拼音→全部字符 id（含异读、按字频降序），供 O 模式部件选择；
  完整单音节候选不设上限。

区间（section）采用节表，全部定长、小端、8 字节对齐，并按冷热排序：
`CHAR → CP2ID → KEY_* → TOKENS → PY/PY_CHAR → LEAVES/ATOMS/LEAF_STRUCT →
ALTS/COMPSET → POST → COLD → VAR → ID2CP`。

---

## 组件注册

模块：`RIME_REGISTER_MODULE(ids_reverse)`（插件名 `librime-ids-reverse`）。

| 组件 | 类型 | 用途 |
|---|---|---|
| `ids_reverse_processor` | processor | search / hybrid 模式首次触发与按键处理 |
| `ids_reverse_segmentor` | segmentor | search 整段打标签、hybrid 动态分段 |
| `ids_reverse_translator` | translator | search 直接检字、hybrid 混合检字与部件候选 |
| `ids_reverse_filter` | filter | search 前缀抑制、hybrid 单字过滤与去重 |

两种模式共用同一个配置命名空间 `ids_reverse`。共享项放在顶层，
只有 `tag`、`prefix`、`tips` 用 `search_` / `hybrid_` 前缀区分。

---

## Lua 5.5 版本

单文件实现位于插件根目录：

```text
plugins/ids-reverse/ids_reverse.lua
```

### 依赖

- `librime` 需要启用 `librime-lua` 插件（Lua 5.5 运行时）。
- 不需要编译 `librime-ids-reverse.so`。
- 继续使用现有的 `ids_data.bin`，无需生成新数据。

### 部署

Rime 的 Lua `require` 只搜索用户数据目录或共享数据目录下的 `lua/`。
源码虽然放在插件根目录，运行时仍需复制到：

```text
<用户数据目录>/lua/ids_reverse.lua
```

或：

```text
<共享数据目录>/lua/ids_reverse.lua
```

CMake 安装规则会把该文件放到共享数据目录的 `lua/` 下。

### 组件引用

单文件返回 `processor` / `segmentor` / `translator` / `filter` 四个子表：

```yaml
engine:
  processors:
    - lua_processor@*ids_reverse*processor@ids_reverse
    - ascii_composer
    - key_binder
    - speller
    - punctuator
    - selector
    - navigator
    - express_editor

  segmentors:
    - lua_segmentor@*ids_reverse*segmentor@ids_reverse
    - abc_segmentor
    - fallback_segmentor

  translators:
    - echo_translator
    - lua_translator@*ids_reverse*translator@ids_reverse

  filters:
    - lua_filter@*ids_reverse*filter@ids_reverse
    - uniquifier
```

`@ids_reverse` 用于让组件读取现有的 `ids_reverse` 配置节点。配置项与 C++ 版完全一致。

参考：

- `examples/ids_reverse.lua.schema.yaml`
- `examples/attach.lua.patch.yaml`

### 与 C++ 版的差异

- Lua 版会在首次检字时把约 30MB 的 `ids_data.bin` 读入内存；C++ 版使用
  `mmap` 惰性映射。桌面端正常使用无影响。
- Lua 的 `select_notifier` 没有 `at_front`，因此 O 模式会临时关闭
  `_auto_commit`，由 Lua 组件自己处理结果上屏和部件状态。
- 两个版本共用 `ids_reverse` 配置与 property 名称，但不要同时挂载两套组件。

### 测试

```sh
lua5.5 plugins/ids-reverse/tests/test_ids_reverse.lua
```

测试会覆盖数据加载、全拼/双拼、I 模式、O 模式加减排除、异体字和多部件组合。
如果 `ids_data.bin` 不在默认位置，可作为参数传入：

```sh
lua5.5 plugins/ids-reverse/tests/test_ids_reverse.lua path/to/ids_data.bin
```

延迟基准：

```sh
lua5.5 plugins/ids-reverse/tests/bench_ids_reverse.lua
```

参考数据（Lua 5.5.1，桌面开发机）：

| 场景 | 冷查询 | 缓存命中 |
|---|---:|---:|
| I 单字 `木` | ~0.4–0.8 ms | ~0.001 ms |
| I 双部件 `木口` | ~10–17 ms | ~0.001 ms |
| I 拼音 `kouhao` | ~0.7–1.2 ms | ~0.001 ms |
| O 单部件 `木木` | ~1.4–2.7 ms | ~0.001 ms |
| O 双部件 `木口` | ~9–10 ms | ~0.001 ms |
| O 减法 `森-木` | ~1.4–2.6 ms | ~0.001 ms |
| O 排除 `木木!口` | ~1.7–3.1 ms | ~0.001 ms |
| 部件候选 `mu` | ~0.03–0.06 ms | ~0.03–0.06 ms |

首次加载数据约 25–45 ms。I 模式双部件交叉检索是当前最慢的冷查询路径。

同机 C++ 版冷查询大致为：I 双部件约 1 ms、O 单部件约 0.02 ms。Lua 版
冷查询约为 C++ 的 10–50 倍，但 O 模式通常在 3 ms 内，结果缓存命中后低于
0.001 ms；I 模式双部件首次查询约 10–17 ms。

---

## 配置

```yaml
engine:
  processors:
    - ids_reverse_processor
    - ascii_composer
    - key_binder
    - speller
    - punctuator
    - selector
    - navigator
    - express_editor
  segmentors:
    - ids_reverse_segmentor
    - abc_segmentor
    - fallback_segmentor
  translators:
    - echo_translator
    - ids_reverse_translator
  filters:
    - ids_reverse_filter
    - uniquifier

ids_reverse:
  data_file: build/ids_data.bin
  custom_file: ids_custom.txt
  max_results: 60
  shuangpin_scheme: flypy        # flypy | zrm | quanpin
  show_pinyin: true
  show_ids: true
  show_unicode: true
  show_block: true
  ids_format: unicode            # unicode | bracket | plain
  variant_trigger: "/"           # O 模式异体字查询触发符，可自定义
  variant_replace_key: "Shift+V" # 用选中异体字替换 preedit 原字
  # disabled_blocks: [扩A, 扩展B]

  search_prefix: "I"
  search_tag: ids_reverse_search
  search_tips: "〔部件检字〕"

  hybrid_prefix: "O"
  hybrid_tag: ids_reverse_hybrid
  hybrid_tips: "〔汉字部件检字〕"
```

两种模式始终同时由同一套组件处理。若两个前缀相同，search 模式优先。

---

## I 模式（search）

- `I` + 部件/拼音，例如 `I木口`、`Ikouhao`。
- 由 `ids_reverse_processor` 接收首个大写 `I`，`ids_reverse_segmentor`
  为输入打上 `search_tag`，不再依赖 `recognizer` 或 `matcher`。
- 单独的 `I` 会由 `ids_reverse_filter` 清空候选，只保留提示。
- 使用 `search_prefix`、`search_tag`、`search_tips`。

### IDS 展示格式

| `ids_format` | 效果 |
|---|---|
| `unicode` | `⿰鼠吾` |
| `bracket` | `[左右]鼠吾` |
| `plain` | `鼠吾` |

---

## O 模式（hybrid）

- `O` + 拼音/双拼：例如 `Omu`、`Omukou`、`Okz`。选中部件后，拼音编码会被
  替换为所选汉字并保留在组字区，例如 `Oshao` → 选 `少` → `O少`。
- O 模式不接受直接输入汉字；组字区中的汉字只来自拼音/双拼候选选择（或异体字替换）。
- 多部件连写：`Omukou`、`Omu'kou` 会按音节生成 `木口`、`目口`、`木扣` 等
  组合部件候选；选中一次即可加入多个部件，例如 `Omukou` → 选 `木口` →
  结果含 `杏`、`呆` 等，`Omumu` → 选 `木木` → 结果 `林`。单音节编码
  （如 `Omu`）仍显示单字部件菜单，便于在 `木/目/母` 之间消歧。
- 要一次加入多个部件，请在编码阶段连续输入多个音节，不要先选单字；例如直接
  输入 `Omukou`，而不是先选 `木` 再输入 `kou`。
- 组合候选默认每个音节取前 4 个、最多 40 个；多字组合在状态中作为一层，
  退格一次撤销整组。
- 连续拼音连写中暂不支持内部 `+`/`-`/`!`；多步运算仍按“先选部件，再输入运算符”
  的方式。括号内运算见下文。
- 选择部件：空格、数字 `1~5`、分号 `;`。
- 回车：
  - 只有 `O`：上屏 `O`；
  - 正在输入单音节编码：把编码作为部件继续检字；
  - 已有多字组合候选或结果：确认当前候选。
- `-` 减法：`O佩-亻`、`O春-日`、`O森-木`、`O龍-立月`。
  纯减法结果保留数据中的原始部件字形，例如 `O达-大` 得到 `辶`；匹配时仍会将
  `辶 / 辵 / ⻌ / ⻍` 视为同一部件。
- 纯减法会同时检索**包含剩余部件的字**：剩余部件本身排在最前，随后按字频与结构
  得分列出包含它的字。例如 `O达-大` 首选 `辶`，后面继续列出 `边 / 达 / 近 /
  连 / 道` 等；`O森-木` 首选 `林`，随后列出 `森 / 淋 / 琳 / 霖` 等。
  多个剩余部件按“同时包含”求交，与多项检索一致。
- `+` / `=` 加法：`O佩-亻+木`、`O泰-氺+日`、`O森-木+氵`。
- `()` 分组：括号内可以逐个输入拼音并选择部件，`(` 不会立即提交，选中的汉字
  会写回组字区；输入 `)` 关闭括号时对括号内表达式求值，再把结果作为一个部件
  加入状态。例如：
  - `Oshao` → 选 `少` → `O少`
  - `(mu` → 选 `沐` → `O少(沐`
  - `-mu` → 选 `木` → `O少(沐-木`
  - `)` → 组内 `沐-木` 求值为 `氵`，`少 + 氵` 首选 `沙`
  括号前的 `-` / `+` / `!` 统一作用于括号内表达式的结果；括号内可以使用
  `+` / `-` / `!` 和嵌套括号。例如 `O渺-(shui` 会先给出 `shui` 的部件候选，
  选好并输入 `)` 后按减法处理。
  括号内还没选完拼音时输入 `)` 不会丢失输入：会继续显示剩余拼音的部件候选，
  全部选完后自动提交括号结果。
- `!` 排除：`O木木!口` 排除包含 `口` 的字，例如结果中不再出现 `啉`。
  `!` 与 `-` / `+` 一样是已选部件之间的运算符：先选 `木木`，输入 `!`，再输入
  `kou` 正常选择 `口`。排除按深层部件判断。
  排除项本身也会被排除，例如 `O刀口!召` 的结果中不会出现 `召`。
  `!` 一次性生效：排除一个部件后，后续选中的部件自动恢复为正向；继续排除请再次
  输入 `!`。
- 连续拼音表达式（如 `Omumu!kou`）不支持，行为与 `Omumu-kou` 一致：先选中正向
  部件，再输入 `!kou`。
- 支持任意多步加减交替与深层复合部件递归扣除，例如 `O龍-立月`、`O章-日十`、`O森-木+氵-氵`。
- 退格：按层回退编码、部件和结果。
- `Ctrl+Enter` / `Alt+Enter` / `Alt+I` / `Ctrl+Shift+I`：上屏当前候选的 IDS 结构。
- `Ctrl+1~6` / `Alt+1~6`：上屏指定序号候选的 IDS 结构。
- `7/8/9/0`、方向键、PageUp/PageDown、逗号、句号放行给原有按键绑定。
- `/`（触发符，可通过 `variant_trigger` 自定义）：**异体字查询**。以菜单当前选中
  候选为基准，从 `ids_variants.txt` 编译进 `ids_data.bin` 的变体表中查询其异体字。
  例如 `Ocao` 选 `艹`、`kui` 选 `亏` 得到 `芌`，按 `/` 显示其异体字 `芋`；
  `Ocao` 选 `艹`、`yu` 选 `于` 得到 `芋`，按 `/` 显示 `芌 芋`。单字部件候选
  也支持直接查询，例如 `Okui` 选 `葵` 后按 `/` 显示 `葵` 的异体字
  `𦮙 𦷡 𫶙`，不需要先把 `葵` 加入状态。若已用空格把单字部件加入状态，而当前
  高亮结果没有异体字，则回退查询该部件本身。无变体时显示 `〔无异体字〕` 提示候选
  （选中即上屏基准字）。
- 异体字替换：在异体字菜单中选中候选后按 `variant_replace_key`（默认 `Shift+V`），
  会用该异体字替换 preedit 中的原字，而不是直接上屏。例如 `Okui` 选 `葵` 后按
  `/` 选 `𦷡` 再按 `Shift+V`，preedit 变为 `O𦷡`；`Ocao` 选 `艹`、`gui` 选 `癸`
  得到 `葵` 后按 `/`，再选 `𦷡` 并替换，preedit 变为 `O艹癸𦷡`。替换后异体字会
  作为当前结果/部件存在：按空格直接上屏；继续输入 `-` 等运算符时，会以该异体字为
  部件继续运算，例如 `O葵/` 替换后输入 `-cao`。
- 使用 `hybrid_prefix`、`hybrid_tag`、`hybrid_tips`。
- **纯结构优先**：只包含查询部件的字由 `LEAF_STRUCT` 结构索引直接置顶，不受字频
  head 影响。例如 `O了` 的 `𠄏`（`⿿了`，旋转操作符不参与部件匹配）、`O匕匕`
  的 `𫧇`。

O 模式自带的部件候选来自 `ids_data.bin` 的 `PY_CHAR`，因此即使当前方案没有拼音词典，
也可以完成 `O` + 拼音/双拼选部件。候选包含该读音的全部字符（包括 `pypinyin` 异读），
完整单音节不设数量上限；仅前缀/未完成拼音保留上限。

---

## 挂接到现有方案

参考 `examples/attach.patch.yaml`。核心是追加：

```yaml
patch:
  engine/processors/@before 0: ids_reverse_processor
  engine/segmentors/@before 0: ids_reverse_segmentor
  engine/translators/+:
    - ids_reverse_translator
  engine/filters/@before 0: ids_reverse_filter

  ids_reverse:
    data_file: build/ids_data.bin
    custom_file: ids_custom.txt
    max_results: 60
    shuangpin_scheme: flypy
    show_pinyin: true
    show_ids: true
    show_unicode: true
    show_block: true
    ids_format: unicode
    variant_trigger: "/"
    variant_replace_key: "Shift+V"
    search_prefix: "I"
    search_tag: ids_reverse_search
    search_tips: "〔部件检字〕"
    hybrid_prefix: "O"
    hybrid_tag: ids_reverse_hybrid
    hybrid_tips: "〔汉字部件检字〕"
```

I 模式和 O 模式都不再依赖 `recognizer/patterns/ids_reverse` 和 `matcher`。
现有方案中的 `matcher`/`recognizer` 可以保留，用于其他规则。

或者直接使用 `examples/ids_reverse.schema.yaml` 作为独立检字方案。

---

## 生成数据

```sh
cd plugins/ids-reverse/data
python3 generate_data_v4.py    # 依赖 pypinyin
```

生成器会读取：

- `IDS_raw.TXT`
- `ids_zitools.txt`
- `ids_lv1.txt`
- `EquivalentUnifiedIdeograph.txt`
- `excel_sheet2_pinyin.json`（部件读音补充表）
- `ids_variants.txt`（异体字表，`原始汉字\t异体字（空格分隔）`）
- `wanxiang_charset.dict.yaml`（可选的本地字符集来源，不存在则跳过）

产出 `ids_data.bin`（IDS4 v3，约 30MB）。该文件被 `.gitignore` 忽略，需要本地生成或从发布包获取。

> IDS4 v3 与 v2/v1、旧 IDS3 数据文件不兼容。升级插件后必须重新生成 `ids_data.bin`；
> 读取到旧版本号时会直接加载失败。

数据检查：

```sh
python3 dump_ids_v4.py ids_data.bin
```

---

## 构建

```sh
# 在 librime 源码根目录（已启用外部插件模式：ENABLE_EXTERNAL_PLUGINS=ON）
cmake -S . -B build
cmake --build build --target rime-ids-reverse
# 产出：build/lib/rime-plugins/librime-ids-reverse.so
```

---

## 部署

将新建的 `librime.so` 与 `librime-ids-reverse.so` 一起安装到运行方：

- 插件 `.so` 放到 `librime.so` 同级的 `rime-plugins/` 目录；
- 确保前端（fcitx5-rime / ibus-rime / Weasel / LyraIME）加载的是这次新构建的 librime；
- `ids_data.bin` 放入用户数据目录的 `build/` 下，或使用绝对路径。

---

## 验证

I 模式：

- `I木口`
- `Ikouhao`
- `I老六`
- `I木木木`
- `I氵工`
- `I匕匕` → 结果包含 `𫧇`

O 模式：

- `O木木` → `林`
- `O匕匕` → 结果包含 `𫧇`
- `O了` → `𠄏` 优先展示（`⿿` 为旋转操作符，不作为部件）
- `O沐木` → `淋`
- `Omukou` → 组合候选 `木口`，选中后结果含 `呆`（如 `杏`、`呆`）
- `Omumu` → 组合候选 `木木`，选中后得到 `林`
- `Oshao` → 选 `少`；`(mu` → 选 `沐`；`-mu` → 选 `木`；`)` → 首选 `沙`
- `Omiao` → 选 `渺`；`-(shui` → 选 `水`；`)` → 按减法处理
- `Osha` → 精确音节 `sha` 的全部候选（含 啥 / 砂 / 鲨 / 纱 / 挲 等，不截断）
- `Osen` → 选 `森`；`(mumu` → 选 `木木`；`)` → 首选 `木`
- `Omu` → 选 `木`；`Omu` → 选 `木`；`!(kouri` → 选 `口日`；`)` → 排除含 `口` / `日` 的字
- `O佩-亻` → `𫥞`，随后列出包含 `𫥞` 的字
- `O春-日` → `𡗗`，随后列出 `春 / 蠢 / 惷` 等
- `O森-木` → `林`，随后列出 `森 / 淋 / 琳 / 霖` 等
- `O龍-立月` → `𳑳`，随后列出包含 `𳑳` 的字
- `O章-日十` → `立`，随后列出 `亲 / 位 / 咅 / 垃` 等
- `O达-大` → `辶`，随后列出 `边 / 达 / 近 / 连 / 道` 等
- `O佩-亻+木` → `𣑲`
- `O泰-氺+日` → `春`
- `O森-木+氵` → `淋`
- `O森-木+氵-氵` → 多步交替运算
- `O渺-(水少)` → `目`
- `O森-(木木)` → `木`
- `O木木!(口日)` → 排除含 `口` / `日` 的字
- `O木木!口` → 结果包含 `木木`，但排除含 `口` 的字（不出现 `啉`）
- `O木木!木` → 正向结果都含 `木`，排除后为空
- `O刀口!召` → 结果不出现 `召` 本身，也不出现包含 `召` 的字
- `O艹亏` → `芌`，按 `/` → 显示异体字 `芋`
- `O艹于` → `芋`，按 `/` → 显示异体字 `芌 芋`
- `O葵` → 单字部件候选，直接按 `/` → 显示异体字 `𦮙 𦷡 𫶙`
- `O葵/` → 选 `𦷡` → `Shift+V` → preedit `O𦷡`
- `O艹癸` → 选 `艹癸` → 结果 `葵` → `/` → 选 `𦷡` → `Shift+V` → preedit `O艹癸𦷡`
- `Omu` + 回车 → 以 `mu` 作为部件继续检字
- `Ctrl+Enter` → 上屏当前候选 IDS

性能目标：单字查询 <0.1ms、多项查询 <1ms。

IDS4 开发机基准（1,278 条单部件/双部件/加减查询混合，含冷热数据）：
旧实现约 0.50–0.57 ms/条，IDS4 约 0.058 ms/条（约 9×）。

---

## 备注

- O 模式已完全由 C++ 实现，不再需要 `ids_dict.bin` / `ids_offset.bin` /
  `ids_info.bin` / `ids_info_idx.bin`，也不再需要 Lua 组件。
- `ids_search.lua` 与 `ids_page_processor.lua` 为历史文件，不再与 IDS4 磁盘格式对应
  （`ids_page_processor.lua` 仍可用于翻页键处理）。
- `data/ids_custom.txt` 是可选的自定义读音/禁用分区示例，默认不启用任何规则。
