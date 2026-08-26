你现在需要修改当前 `Mc_Injector` 项目，实现一套稳定的 **Hypixel Bed Wars 比赛状态识别、自己队伍识别，以及敌人接近己方床时的警告系统**。

请先完整阅读现有源码，尤其是：

- JVM/JNI bindings
- `GameBindings`
- `MappingProvider`
- 世界/实体扫描逻辑
- Bed scanner
- Sidebar / Scoreboard 相关逻辑
- TAB / `NetworkPlayerInfo` 相关逻辑
- Overlay snapshot / renderer
- 现有 telemetry / Controller 通信

以项目当前结构为准进行最小侵入式修改。

不要修改程序现有的注入方式、OpenGL Hook 方式或其他无关功能。

不要添加任何反作弊规避、隐藏模块、擦除 PE、反检测等功能。

---

# 一、最终目标

实现两套彼此独立、但共享 `ownTeam` 状态的功能：

```text
A. Sidebar Match Detector
   ↓
判断 Bed Wars 是否已经正式开始
   ↓
判断自己属于 RED / BLUE / ... 哪个队伍
```

以及：

```text
B. Bed Threat Detector
   ↓
扫描己方床附近 EntityPlayer
   ↓
读取玩家胸甲
   ↓
自己队色胸甲 → 队友
其他颜色胸甲 → 敌人
没有胸甲 → 潜在隐身敌人
   ↓
产生床警告
```

核心设计原则：

```text
Sidebar
→ 负责比赛状态和 ownTeam

世界实体
→ 负责床附近实际出现的玩家

胸甲颜色
→ 负责床附近玩家的敌我判断
```

不要使用 TAB 玩家是否存在来判断床附近威胁。

---

# 二、统一定义 BedWarsTeam

项目内部增加统一的队伍枚举，概念上至少包含：

```text
Unknown
Red
Blue
Green
Yellow
Aqua
White
Pink
Gray
```

之后：

- Sidebar parser
- 玩家胸甲识别
- Bed warning
- UI

全部使用这个统一枚举。

不要在各模块到处传播：

```text
"RED"
"Blue"
'R'
0xFF0000
```

等不同表示。

---

# 三、WorldClient 是整个比赛状态的生命周期边界

必须记录当前世界实例。

只要：

```text
currentWorld == null
```

或者：

```text
currentWorld != previousWorld
```

即当前 `WorldClient` 被卸载或换成新的世界，

立即执行：

```text
Hard Reset
```

清除至少：

```text
matchActive
ownTeam

Sidebar 稳定计数

当前床警告状态
床附近实体状态
威胁缓存

当前世界对应的 entity/global refs
ownBed 等世界相关信息
```

即：

```text
WorldClient A
↓
断线 / rejoin / 返回大厅 / 下一局
↓
WorldClient B
↓
所有上一世界比赛状态必须失效
```

不要专门解析：

```text
/rejoin
游戏结束
断线
```

这些文字或命令。

只把：

```text
WorldClient change
```

作为最可靠的 Hard Reset。

---

# 四、Sidebar 是正式比赛开始的唯一主要判据

不要使用：

```text
"Game starts..."
"Protect your bed..."
"游戏开始"
"BED WARS"
```

之类自然语言文本。

Hypixel 支持多语言。

也不要使用：

```text
TAB 中出现玩家
```

作为正式开局依据。

应该读取 Minecraft **右侧 Sidebar Scoreboard 最终格式化后的显示行**。

注意：

不要只读取某个原始 `Score.getPlayerName()`。

Minecraft Sidebar 的实际一行可能由：

```text
Score entry
+
ScorePlayerTeam prefix
+
ScorePlayerTeam suffix
```

组合产生。

需要解析与玩家屏幕最终看到的格式化内容等价的字符串。

---

# 五、识别 Bed Wars 队伍行

从 Sidebar 行中识别下面这些结构：

```text
§cR ...     → RED
§9B ...     → BLUE
§aG ...     → GREEN
§eY ...     → YELLOW
§bA ...     → AQUA
§fW ...     → WHITE
§dP ...     → PINK
§7S ...     → GRAY
```

为了兼容可能存在的表示差异，Gray 可以允许：

```text
§7S ...
§7G ...
```

但必须结合灰色格式码判断，避免把 Green 的 `G` 当成 Gray。

---

# 六、不要解析完整队名

不要依赖：

```text
Red
Blue
Green
Yellow
```

这些英文词。

更不要依赖它们的中文翻译。

例如：

```text
§9B Blue: ...
```

和：

```text
§9B 蓝队: ...
```

对于程序应该是完全相同的：

```text
BLUE
```

主要依据：

```text
颜色 formatting code
+
队伍 marker
```

---

# 七、允许中间存在额外 Minecraft formatting code

不要写一个非常脆弱的：

```text
startsWith("§9B ")
```

然后假设格式永远严格如此。

格式化文本中可能出现：

```text
§r
§l
§o
```

等 formatting codes。

parser 应能够跳过无关格式控制码，同时正确识别真正的：

```text
team color
+
team marker
```

但不要把普通文字中的字母 `R/B/G/...` 当成队伍。

---

# 八、正式开局条件

每次扫描 Sidebar：

记录出现的**不同** Bed Wars team。

例如：

```text
RED
BLUE
GREEN
YELLOW
```

应该使用集合/bitmask 语义。

不要简单：

```text
teamRows++
```

因为重复行不能算两个队。

同时寻找：

```text
YOU
```

注意：

只有当 `YOU` 出现在一个已经被识别为合法 Bed Wars team 的行中才有效。

最终：

```text
recognizedDistinctTeams >= 2
AND
exactly one recognized team row contains "YOU"
```

才认为这一次 Sidebar snapshot 是：

```text
Valid Bed Wars Match State
```

---

# 九、根据 YOU 判断自己的队伍

例如：

```text
§cR Red: ...
§9B Blue: ... YOU
§aG Green: ...
§eY Yellow: ...
```

直接得到：

```text
ownTeam = BLUE
```

不要再：

- 根据出生点猜颜色；
- 根据用户名 prefix 猜；
- 根据 TAB `[B]` 猜；
- 解析 `Blue` 这个词。

规则就是：

```text
YOU 所在的合法 Bed Wars 队伍行
=
ownTeam
```

---

# 十、开局状态需要轻微防抖

不要单次读取符合条件就立即激活。

因为 Scoreboard packet 更新期间可能存在瞬态数据。

推荐：

```text
UNKNOWN

第一次满足：
candidateCount = 1

第二次连续满足相同 ownTeam：
candidateCount = 2

达到阈值：
MATCH_ACTIVE
```

2～3 个连续稳定 snapshot 即可。

不要让这个防抖产生明显延迟。

---

# 十一、ACTIVE 状态

成功确认以后保存：

```text
matchActive = true
ownTeam = X
```

之后继续监控 Sidebar。

如果 Sidebar 再次稳定检测到同一个：

```text
ownTeam
```

正常保持。

如果偶尔一次 Sidebar 不完整：

```text
只有一个 team
YOU 暂时没读到
```

不要立即：

```text
matchActive = false
```

Scoreboard 更新过程中可能短暂不完整。

---

# 十二、比赛结束

优先使用：

```text
WorldClient change
```

Hard Reset。

如果 Hypixel 在正式切换 WorldClient 前就把 Sidebar 清空，可以增加 Soft Reset：

```text
连续多个扫描周期
都不存在 Bed Wars team matrix
且不存在 YOU
```

才：

```text
matchActive = false
ownTeam = Unknown
```

不要因为一次 Sidebar snapshot 不完整而 Reset。

---

# 十三、断线重连和 /rejoin

必须保证以下流程自然成立：

```text
正在比赛
ownTeam = BLUE
matchActive = true
      ↓
断线 / rejoin
      ↓
旧 WorldClient 被卸载
      ↓
Hard Reset
      ↓
新的 WorldClient
      ↓
matchActive = false
ownTeam = Unknown
      ↓
服务器重新发送 Scoreboard
      ↓
再次检测 Sidebar
      ↓
§9B ... YOU
      ↓
ownTeam = BLUE
matchActive = true
```

不需要重新注入 Agent，只要 Minecraft 进程本身没有退出。

但所有旧世界的 Java 对象引用必须失效并重新取得。

---

# 十四、床警告的启用条件

只有：

```text
matchActive == true
```

并且：

```text
ownTeam != Unknown
```

并且：

```text
ownBed 已知
```

时才启用 Bed Threat Detector。

如果项目当前还不能可靠确定：

```text
ownBed
```

不要猜。

把床警告保持 inactive，并在日志中明确说明：

```text
Bed warning unavailable: own bed not resolved
```

本任务重点是：

```text
已知 ownBed 后如何判断附近玩家威胁
```

不要把“床归属识别”与本任务混在一起写成大量不稳定 heuristics。

---

# 十五、床附近玩家来源

床警告必须使用当前世界中的：

```text
EntityPlayer
```

或项目已有等效实体集合。

这里不能只依赖：

```text
TAB / NetworkPlayerInfo
```

因为床警告关心的是：

```text
谁实际上出现在床附近
```

因此需要实体的：

```text
position
equipment
```

信息。

---

# 十六、首先排除本地玩家

扫描到玩家实体后：

```text
if entity == localPlayer
    ignore
```

不能让自己触发床警告。

---

# 十七、距离判断

只对进入己方床警戒范围的玩家进行装备判定。

概念：

```text
distance(entity.position, ownBed.position)
<= bedWarningRadius
```

警戒半径使用项目现有配置体系。

不要硬编码到散落的业务逻辑中。

如果项目已有 Bed ESP / bed marker 距离配置，应尽量复用统一配置。

---

# 十八、胸甲是核心敌我依据

对床附近 `EntityPlayer` 读取**客户端目前实际收到的胸甲 ItemStack**。

不要根据屏幕像素颜色判断。

需要读取：

```text
EntityPlayer
↓
armor equipment
↓
chestplate ItemStack
```

如果存在胸甲：

确认是不是皮革护甲。

对于 Hypixel Bed Wars 队伍皮革装备：

```text
ItemArmor.getColor(ItemStack)
```

或者等价方式读取：

```text
display.color
```

获得实际 RGB。

---

# 十九、附魔不能影响队伍判断

附魔和皮革染色颜色是不同的数据。

例如：

```text
Leather Chestplate
├─ display.color
└─ ench
```

因此：

```text
Protection I
Protection II
附魔闪光
```

不能让胸甲颜色识别失效。

不要根据最终渲染出来的：

```text
glint 后的视觉颜色
```

判断。

必须读取：

```text
ItemArmor / ItemStack 内部的实际 leather color
```

---

# 二十、不要未经验证硬编码错误 RGB

建立一个统一：

```text
BedWarsTeam
↔
LeatherArmorColor
```

映射层。

但实现时必须确认 Hypixel 当前实际发送的皮革护甲 RGB。

不要因为：

```text
RED
```

就未经验证假定一定是：

```text
0xFF0000
```

推荐开发过程中增加一次诊断日志：

```text
ownTeam = BLUE
local chestplate rawColor = 0xXXXXXX
```

通过本地玩家/明确队友的胸甲确认 Hypixel 实际 palette。

确认后再作为 canonical palette。

颜色逻辑统一放在一个函数/模块中。

不要在 renderer、scanner、UI 中分别硬编码 RGB。

---

# 二十一、胸甲分类规则

对于进入己方床警戒区的玩家：

## 情况 A：存在可识别的皮革胸甲

读取：

```text
armorTeam
```

如果：

```text
armorTeam == ownTeam
```

分类：

```text
TEAMMATE
```

不触发床警告。

---

## 情况 B：存在胸甲，而且明确是其他 Bed Wars 队色

例如：

```text
ownTeam = BLUE
player chestplate = RED
```

分类：

```text
ENEMY
```

立即触发床警告。

---

# 二十二、没有胸甲必须按威胁处理

这是非常重要的设计要求。

如果：

```text
chestplate == null
```

不要判断：

```text
不是敌人
```

而应该：

```text
UNKNOWN_THREAT
```

并触发床警告。

原因：

Hypixel Bed Wars 中敌方 Invis 场景下，客户端可能看不到敌人的正常护甲。

因此：

```text
Player entity
+
在己方床附近
+
没有可确认的 own-team chestplate
```

必须 fail-safe：

```text
触发警告
```

---

# 二十三、无法识别胸甲颜色也按威胁处理

以下情况：

```text
不是预期皮革胸甲
颜色读取失败
颜色不属于任何已知 Bed Wars team
NBT/ItemStack 暂时异常
```

统一：

```text
UNKNOWN_THREAT
```

床警告：

```text
ON
```

设计原则：

```text
只有明确证明是队友
才能免除床警告
```

而不是：

```text
只有明确证明是敌人才警告
```

即：

```text
confirmed teammate
→ safe

everything else
→ threat
```

---

# 二十四、最终床警告判定表

逻辑必须等价于：

```text
本地玩家
→ IGNORE

其他玩家，不在床范围
→ IGNORE

其他玩家，在床范围：

    胸甲颜色 == ownTeam
        → TEAMMATE
        → NO WARNING

    胸甲颜色 == other valid BedWarsTeam
        → ENEMY
        → WARNING

    没有胸甲
        → UNKNOWN_THREAT
        → WARNING

    胸甲无法解析
        → UNKNOWN_THREAT
        → WARNING
```

---

# 二十五、装备同步需要防抖，但不能导致 Invis 漏报

Hypixel / Minecraft 网络同步可能短暂发生：

```text
一帧 chestplate == null
下一帧恢复
```

因此内部可以保存：

```text
entityId
lastArmorState
lastSeenTime
```

用于 UI 稳定。

但是：

**不要为了减少误报而要求“连续几秒没有胸甲才警告”。**

这样会削弱 Invis 警告价值。

推荐：

```text
第一次发现 null / enemy color
→ 立即形成临时 Threat
→ 可以立即提示

连续确认后
→ 提升为 stable threat
```

而解除警告可以比触发稍微慢一点：

```text
连续数次明确检测为 own-team chestplate
→ 解除
```

这种设计比：

```text
Threat 也延迟几秒
```

更安全。

---

# 二十六、Entity ID 只能作为当前世界中的短期身份

如果实现会缓存：

```text
entityId → threat state
```

必须注意：

```text
Entity ID
```

只在当前世界生命周期内有意义。

WorldClient change 后：

```text
entityThreatCache.clear()
```

绝对不能把：

```text
entityId 123
```

从上一局带到下一局。

---

# 二十七、玩家重新出现时重新判断

不要因为一个 entityId 曾经被判断为：

```text
TEAMMATE
```

就永久信任。

每次装备状态发生变化都要重新计算。

例如：

```text
实体进入范围
→ own team chestplate
→ TEAMMATE

之后装备 packet 更新
→ chestplate absent
→ UNKNOWN_THREAT
```

应允许状态变化。

床警告是：

```text
实时世界状态判断
```

而不是永久身份数据库。

---

# 二十八、不要依赖 TAB 来豁免床警告

即使：

```text
TAB cache
```

说某用户名此前属于自己队，

只要床附近这个实体当前：

```text
没有自己队颜色的胸甲
```

本功能仍然按照上述 fail-safe 逻辑处理。

原因：

床警告的目标不是建立绝对身份数据库，而是：

```text
宁可对无法确认身份的床附近玩家发出警告
也不要漏掉 Invis enemy
```

所以在 Bed Threat Detector 中：

```text
胸甲状态
```

是主要即时证据。

---

# 二十九、建议的整体状态结构

逻辑上可以维护类似：

```text
MatchState
├─ worldIdentity
├─ active
├─ ownTeam
├─ stableSidebarCount
└─ ownBed

BedThreatState
├─ entityId
├─ distanceToBed
├─ armorPresent
├─ armorColor
├─ armorTeam
├─ classification
│  ├─ Teammate
│  ├─ Enemy
│  └─ UnknownThreat
└─ lastSeen
```

不要求完全使用这些具体类型名。

以现有项目命名风格为准。

---

# 三十、Renderer 不要重新做判断

Renderer 只消费已经计算好的 snapshot。

不要在 OpenGL `SwapBuffers` render path 里重新：

```text
访问复杂 JNI
扫描 Scoreboard
扫描所有 Entity
解析 NBT
```

应继续遵守项目现有的工作线程 / snapshot 架构：

```text
JNI/game sampling
↓
计算 MatchState / ThreatState
↓
保存轻量 snapshot
↓
Renderer 只读 snapshot
```

避免把重型 JNI 查询塞进渲染线程。

---

# 三十一、建议增加诊断信息

开发版至少提供这些诊断字段：

```text
World generation / world changed

Sidebar:
recognized teams = R,B,G,Y
YOU team = BLUE
stable count = 2
matchActive = true

Own team:
BLUE

Own bed:
known / unknown
position

Bed threat:
entity id
distance
chestplate present
raw leather RGB
normalized team
classification
```

但不要每帧疯狂刷日志。

只在：

```text
状态变化
新实体进入警戒区
胸甲分类变化
World reset
Match activated/deactivated
```

时输出。

---

# 三十二、必须正确区分“检测失败”和“没有威胁”

例如：

```text
JNI exception
无法取得 ItemStack
无法解析胸甲
```

绝对不要默默返回：

```text
SAFE
```

床附近实体的装备数据发生读取异常：

```text
UNKNOWN_THREAT
```

至少在当次检测中按照威胁处理，并记录诊断信息。

这是 fail-safe 设计。

---

# 三十三、不要破坏现有异常处理

所有：

```text
CallObjectMethod
CallBooleanMethod
CallIntMethod
GetObjectField
```

等 JNI 操作必须遵守项目已有的 exception clearing/checking 规范。

不能让一个胸甲读取失败留下：

```text
pending Java exception
```

污染后续 JNI 操作。

Global/local reference 生命周期也必须保持项目现有规范。

---

# 三十四、MappingProvider

如果当前 mapping registry 中缺少以下必要类型/成员：

```text
Scoreboard
Score
ScorePlayerTeam

Sidebar Objective
formatted sidebar line

EntityPlayer armor/equipment
ItemStack
Item
ItemArmor
ItemArmor.getColor
```

需要为项目实际支持的：

```text
Vanilla / MCP
Forge / SRG
Lunar legacy
Lunar MCP
```

等 profile 补齐 mapping。

不要只让其中一个 profile 工作。

Mapping 缺失时：

```text
fail closed
```

并返回清晰 diagnostic。

不要猜 field/method。

---

# 三十五、测试要求：Sidebar parser

至少添加纯逻辑测试。

例如：

```text
§cR Red: ✔
§9B Blue: ✔ YOU
§aG Green: ✔
§eY Yellow: ✔
```

结果必须是：

```text
valid = true
ownTeam = BLUE
teamsSeen = 4
```

中文：

```text
§cR 红队: ✔
§9B 蓝队: ✔ YOU
§aG 绿队: ✔
§eY 黄队: ✔
```

必须得到完全相同结果：

```text
valid = true
ownTeam = BLUE
```

---

# 三十六、Sidebar parser 负面测试

以下情况不能判定为开局：

只有：

```text
YOU
```

结果：

```text
false
```

只有：

```text
§cR ...
```

结果：

```text
false
```

两个队但没有 YOU：

```text
§cR ...
§9B ...
```

结果：

```text
false
```

YOU 在普通非队伍行：

```text
§cR ...
§9B ...
Some unrelated YOU text
```

结果：

```text
false
```

---

# 三十七、床威胁逻辑测试

假设：

```text
ownTeam = BLUE
```

测试：

```text
BLUE leather chestplate
→ TEAMMATE
→ warning false
```

```text
RED leather chestplate
→ ENEMY
→ warning true
```

```text
GREEN leather chestplate
→ ENEMY
→ warning true
```

```text
no chestplate
→ UNKNOWN_THREAT
→ warning true
```

```text
unrecognized chestplate/color
→ UNKNOWN_THREAT
→ warning true
```

---

# 三十八、附魔测试

构造：

```text
BLUE leather chestplate
+ enchantment
```

必须仍然：

```text
armorTeam = BLUE
TEAMMATE
```

附魔绝对不能改变颜色判定。

---

# 三十九、生命周期测试

模拟：

```text
World A
↓
Sidebar → BLUE YOU
↓
MATCH_ACTIVE / BLUE
↓
enemy warning active
```

然后：

```text
World A → null
```

必须立即：

```text
matchActive = false
ownTeam = Unknown
threat cache empty
```

然后：

```text
World B
↓
Sidebar → RED YOU
```

必须重新得到：

```text
matchActive = true
ownTeam = RED
```

不能残留上一局 BLUE。

---

# 四十、最终完整运行链

最终架构应该形成：

```text
                   WorldClient
                       │
          ┌────────────┴─────────────┐
          │                          │
          ▼                          ▼
     Sidebar scanner            Entity scanner
          │                          │
          ▼                          │
识别 ≥2 个 BW team                  │
+ YOU                                │
          │                          │
          ▼                          │
    MATCH_ACTIVE                     │
    ownTeam = X                      │
          │                          │
          └─────────────┐            │
                        ▼            ▼
                    ownTeam      ownBed radius
                         \          /
                          \        /
                           ▼      ▼
                       chestplate check
                              │
             ┌────────────────┼─────────────────┐
             ▼                ▼                 ▼
      own-team color     other team color      absent/
                                             unknown
             │                │                 │
             ▼                ▼                 ▼
         TEAMMATE           ENEMY        UNKNOWN_THREAT
             │                │                 │
             ▼                └────────┬────────┘
       no warning                     ▼
                                  BED WARNING
```

---

# 四十一、最重要的最终原则

请确保最终实现严格符合：

```text
Sidebar
=
开局状态 + ownTeam
```

```text
WorldClient
=
比赛生命周期 / reset 边界
```

```text
床附近 EntityPlayer
=
实际威胁对象
```

```text
自己的队伍颜色胸甲
=
明确队友证据
```

```text
其他颜色胸甲
=
敌人
```

```text
没有胸甲 / 无法识别
=
潜在 Invis / Unknown Threat
=
仍然报警
```

即床警告采用：

```text
Only confirmed teammate is safe.
```

而不是：

```text
Only confirmed enemy is dangerous.
```

---

# 四十二、完成后需要报告

完成修改后请输出：

1. 修改了哪些文件；
2. 增加了哪些 mapping；
3. Sidebar 最终如何取得格式化行；
4. `WorldClient` change 如何检测；
5. Sidebar parser 如何避免语言依赖；
6. `ownTeam` 如何确定；
7. 胸甲 ItemStack 如何取得；
8. leather RGB 如何取得；
9. 附魔为什么不会影响判定；
10. Null chestplate 如何处理；
11. Bed warning 的防抖策略；
12. `/rejoin` / 断线重连如何 Reset；
13. 添加了哪些测试；
14. 实际运行了哪些测试以及结果；
15. 尚未能在本机验证的运行时条件。

如果源码当前结构与本说明中的类名不同，以源码实际情况为准。

不要为了照抄本文而破坏现有架构。

优先复用当前：

```text
bindings
snapshot
worker thread
renderer
MappingProvider
diagnostic
```

等设计。