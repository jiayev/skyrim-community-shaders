# Advanced Skin 分层材质 — 制作指南（中文）

> 面向 Modder / 美术制作人员。本指南基于当前版本的 `src/Features/Skin.h` 与
> `src/Features/Skin.cpp` 实现编写，所有键名、匹配规则与合并顺序均已对照代码核对。
> 若与游戏实际表现有出入，请以 `CS` 运行日志（`[Advanced Skin] ...`）为准。

Advanced Skin 的覆盖（Override）系统用 JSON 驱动。它把「每一格皮肤几何体最终使用什么
参数、什么额外贴图」拆成 **基材质（Base）→ Surface → Appearance → Local** 四个层级
依次合并，任何一层都可以只写你需要改动的部分。

---

## 1. 系统能做什么、不能做什么

### ✅ 系统能直接做

-   **覆盖参数**：粗糙度、F0、SSS、绒毛（Fuzz）、皮肤细节（Detail）等全部 `SkinProfile`
    数值字段（见第 4 节字段表），按 NIF / 形状 / 贴图 / 种族 / NPC / 槽位等条件精确命中。
-   **指定额外贴图**：RFAOS（额外粗糙度/反射率/AO/镜面）与 wetness（湿身遮罩）两张贴图，
    直接由 JSON 给出路径。
-   **分层继承**：把通用设定写在基材质 / Surface，把角色个性写在 Appearance，把单个
    NPC 某个身体部件的特例写在 Local，后写覆盖前写、子继承父。
-   **条件匹配**：选择器（selector）与分类器（classifier，`all/any/not` 递归逻辑），支持
    glob 通配符（`*`、`?`，`**` 视为连续 `*` 可跨路径分隔符）。
-   **热重载**：运行中改 JSON 即时生效（见第 2 节）。

### ❌ 系统 **不能** 直接做

-   **不会给几何体「分配」diffuse / normal 贴图**。 `diffuse`、`normal` 仍由
    ESP / NIF / `BSTextureSet`（纹理集）决定。JSON 里的 diffuse / normal 只能作为
    **选择条件**（`diffuse` / `diffusePrefix` / `diffuseGlob` 等）用来「命中」几何体，
    而不是把它们替换成别的贴图。
-   **不会写回引擎共享材质或 `BSTextureSet`**。JSON 只额外绑定 RFAOS / wetness 两个
    纹理槽，不动原材质。
-   **不会按「湿身常量」做逐 NPC 覆盖**。全局湿身参数（`ExtraSkinWetness`、`WetParams`、
    汗水阈值等）属于全局设置，不在 `SkinProfile` 里，无法通过覆盖 JSON 单独调整。

> 一句话：**贴图分配靠 ESP/NIF/TextureSet，JSON 只负责「参数 + RFAOS/wetness」以及
> “这些设定命中谁”。**

---

## 2. 文件路径、User 覆盖、热重载、游戏内 UI

### 文件位置

| 类型                           | 路径                                      |
| ------------------------------ | ----------------------------------------- |
| Mod 覆盖                       | `Data\Shaders\Skin\Overrides\*.json`      |
| 用户覆盖（后加载，优先级更高） | `Data\Shaders\Skin\Overrides\User\*.json` |

-   只读取扩展名为 `.json` 的普通文件（扩展名大小写不敏感）。
-   目录扫描后按「小写文件名」排序，**先加载 Mod 文件，再加载 `User` 文件**。
-   游戏内 UI（编辑界面）写入的用户覆盖会持久化到 `User\SkinOverrides.user.json`，
    且只保存 legacy 的 `nif` / `baseid` 两部分。

### 热重载

运行时每约 60 帧重新扫描一次目录；只要任一文件的修改时间集合发生变化，就重建整套
覆盖并让几何体缓存失效（`revision` 自增）。因此 **改完保存即可在游戏里看到效果，
无需重启**。

### 游戏内「快速模式」

Advanced Skin 设置面板 → **NIF Overrides** 区块提供：

-   **Pick Crosshair**（十字准星拾取）/ **Use Console Selection**（使用控制台选中）；
-   自动推导该目标全部 NIF 键与 BaseID 键，并显示目标是否含皮肤材质；
-   **New NIF Override** / **New BaseID Override** 打开覆盖编辑器，合并链会以
    `Chain: Default -> Race(...) -> ... override` 提示。
    这套 UI 适合快速定位键名，最终也写进 `User` 覆盖。正式发布仍建议整理成清晰的 JSON 文件。

---

## 3. 简单格式：`nif` 与 `baseid`

### `nif`（按 NIF / 形状键）

```json
{
    "nif": {
        "actors/character/actresscharacternord/characternord.nif": {
            "SkinMainRoughness": 0.62,
            "EnableSkinDetail": true
        }
    }
}
```

键会做规范化：转小写、`\` 转 `/`，并去掉开头的 `meshes/`、`data/meshes/` 或路径中
`/meshes/` 之前的部分。所以 `meshes\actors\...nif` 与 `actors/...nif` 等价。
值是一组 `SkinProfile` 字段（可只写要改的字段；`null` 在合并时被跳过）。

### `baseid`（按 NPC 基础 FormID）

```json
{
    "baseid": {
        "000A2C8E": {
            "Translucency": 0.2,
            "sssWidth": 0.28
        }
    }
}
```

键会做：可选的 `0x`/`0X` 前缀去掉、十六进制字母转大写。**这里保存的是 NPC
（`TESNPC`）的“原始 FormID”**（8 位十六进制，含插件在 `0x00000xxx` 之外的加载位时
会带高 2 位索引）。

> ⚠️ **加载顺序风险**：BaseID 是**原始 FormID**，不是 `PluginName:FormID` 这类稳定 ID。
> 一旦该 NPC 所在插件加载顺序变化（以及带轻量插件 `FE` 区段标记的 FormID），这个键
> 就会失效。发布前请明确记录目标 NPC 的插件与 FormID。

---

## 4. `SkinProfile` 字段表（完整）

任何需要写“参数”的地方（基材质 `parameters`、实例 `parameters`、
`profileOverrides`、legacy partial）都使用下列字段，全部可选，未写的字段沿用上一层的值。
默认值来自 `Skin.h` 中的 `SkinProfile` 默认构造。

| 字段（精确大小写）                  | 默认值 | 说明                                                                |
| ----------------------------------- | ------ | ------------------------------------------------------------------- |
| `SkinMainRoughness`                 | 0.7    | 第一层（角质层）微观粗糙度                                          |
| `SkinSecondRoughness`               | 0.35   | 第二层（表皮）反射平滑度，通常比第一层低 30–50%                     |
| `SkinSpecularTexMultiplier`         | 1.0    | 对原版镜面贴图的乘数（作用于第一层粗糙度）                          |
| `SecondarySpecularStrength`         | 0.15   | 第二高光强度                                                        |
| `F0`                                | 0.0278 | 菲涅尔反射率（F0）                                                  |
| `BaseColorMultiplier`               | 1.0    | 基色贴图乘数                                                        |
| `PhysicalMainRoughnessMultiplier`   | 1.3    | 物理主粗糙度贴图乘数                                                |
| `PhysicalSecondRoughnessMultiplier` | 0.75   | 物理次粗糙度贴图乘数                                                |
| `PhysicalSpecularStrength`          | 1.0    | 物理镜面强度（注意：与 `SkinSpecularTexMultiplier` 是两个不同字段） |
| `ExtraEdgeRoughness`                | 0.25   | 边缘额外粗糙度（近似面部绒毛）                                      |
| `EnableSkinDetail`                  | true   | 是否启用皮肤细节贴图                                                |
| `SkinDetailStrength`                | 0.25   | 皮肤细节强度                                                        |
| `SkinDetailTiling`                  | 10.0   | 皮肤细节平铺                                                        |
| `BodyTilingMultiplier`              | 2.0    | 身体平铺乘数（对齐脸部）                                            |
| `Translucency`                      | 0.1    | SSS 透射的半透明度                                                  |
| `sssWidth`                          | 0.2    | SSS 透射宽度                                                        |
| `UseSSS`                            | true   | 是否启用 SSS 透射                                                   |
| `FuzzStrength`                      | 1.0    | 绒毛强度                                                            |
| `FuzzRoughness`                     | 0.35   | 绒毛粗糙度                                                          |
| `FuzzF0`                            | 0.045  | 绒毛 F0                                                             |

> `SkinProfile` 里 **没有** 湿身字段；湿身强度、阈值、噪声参数等属于全局设置，
> 不能逐 NPC 覆盖。

---

## 5. 推荐分层模型：Base → Surface → Appearance → Local

### 5.1 顶层 sections（JSON 顶层键）

| 顶层键                | 含义                                                                         |
| --------------------- | ---------------------------------------------------------------------------- |
| `nif` / `baseid`      | legacy 兼容层（第 3 节），仍可读取                                           |
| `profiles`            | 命名参数池（供 legacy `rules` 的 `profile` 引用）                            |
| `materials`           | 二义：要么是「仅贴图 payload」（legacy），要么是带 `parameters` 的「基材质」 |
| `surfaceInstances`    | Surface 域实例                                                               |
| `appearanceInstances` | Appearance 域实例                                                            |
| `localInstances`      | Local 域实例                                                                 |
| `classifiers`         | 标签（tag）→ 递归分类表达式                                                  |
| `rules`               | legacy 统一规则（兼容层，低优先级）                                          |
| `bindings`            | 现代绑定（推荐）                                                             |
| `priority`            | 文件级默认优先级（可选，作用于该文件的 rules / bindings）                    |

> 注意：**没有**顶层 `baseMaterials` 键。基材质是 `materials` 里带 `parameters`
> 对象的条目；其中名为 `advanced-skin:default` 的条目会被当作全局根覆盖，
> 在一切层级之前先合并进 `DefaultProfile`。

### 5.2 三个域（Domain）

| 域         | 关注什么                                                              | 不负责 / 不关注                                  |
| ---------- | --------------------------------------------------------------------- | ------------------------------------------------ |
| Surface    | 网格表面：NIF、shape、diffuse/normal、armor、slot、shaderFeature、tag | 角色身份                                         |
| Appearance | 角色身份：baseid、referenceid、race、sex                              | 网格表面                                         |
| Local      | 角色 + 表面（两者都要有选择器字段）                                   | 仅单方面（缺少 actor 或 surface 选择器会被拒绝） |

`domain` 字符串（`bindings` 中使用）大小写不敏感：`surface` / `appearance` / `local`。

### 5.3 实例继承（material instance）

```json
{
    "surfaceInstances": {
        "skin:ube_body": {
            "parent": "advanced-skin:default",
            "parameters": { "SkinDetailTiling": 12.0 },
            "textures": {
                "rfaos": "textures/actors/character/female/femalebody_1_rfaos.dds",
                "wetness": "textures/actors/character/female/femalebody_1_wet.dds"
            }
        }
    }
}
```

-   `parent` 是**单亲**；Surface 实例可继承基材质或另一个 Surface 实例；
    Appearance 只能继承 Appearance；Local 只能继承 Local。
-   合并顺序是 **父→子**（根先应用，叶子最后应用，后者覆盖前者同名字段）。
-   校验：缺父、跨域父、成环、深度超过 32 都会让该实例被判无效并在日志告警；
    绑定到无效实例的 binding 会被丢弃。
-   `advanced-skin:default`（或任何基材质 id）在 Surface 域里是合法根，碰到它就应用其
    `parameters` 并停止向上走。

### 5.4 `bindings`（推荐的选择机制）

```json
{
    "bindings": [
        {
            "id": "surface:ube_body",
            "domain": "surface",
            "use": "skin:ube_body",
            "match": { "tag": "body:ube" }
        }
    ]
}
```

-   必填：`match`（对象）、`domain`（字符串）、`use`（字符串，指向同域实例）。
-   每个域独立选一个「赢家」：遍历排序后的 bindings，**最后一个命中的 binding** 胜出；
    三个域互不竞争。
-   Local 域的 binding **必须同时含至少一个 actor 选择器字段和一个 surface 选择器字段**，
    否则被忽略。

### 5.5 selector 字段（精确键名）

所有**已填**字段必须全部命中（AND 关系）。匹配大小写不敏感；路径用正斜杠。
未知字段或非字符串字段会使**整条 selector 失效**（该 rule/binding 被丢弃）。

| 字段            | 规范化                    | 匹配规则                                   |
| --------------- | ------------------------- | ------------------------------------------ |
| `nif`           | 去 meshes 前缀、小写、`/` | 等于 `context.nif` 或 `context.legacyNif`  |
| `nifGlob`       | 同上                      | glob 匹配 `nif` 或 `legacyNif`（`*`、`?`） |
| `shape`         | 小写                      | 等于几何体名                               |
| `shapeGlob`     | 小写                      | glob                                       |
| `baseid`        | 去 0x、大写               | 等于 NPC 原始 FormID                       |
| `referenceid`   | 同上                      | 等于已放置实例的 FormID                    |
| `race`          | 小写                      | 等于种族 EditorID                          |
| `normal`        | 贴图键规范化              | 等于法线贴图路径                           |
| `normalPrefix`  | 同上                      | 法线路径 `startswith`                      |
| `normalGlob`    | 同上                      | glob                                       |
| `diffuse`       | 同上                      | 等于漫反射贴图路径                         |
| `diffusePrefix` | 同上                      | 漫反射路径 `startswith`                    |
| `diffuseGlob`   | 同上                      | glob                                       |
| `armor`         | 小写                      | 等于 armor EditorID **或** 十六进制 FormID |
| `armorAddon`    | 小写                      | 等于 ArmorAddon EditorID **或** FormID     |
| `slot`          | 小写                      | 等于槽位名（见下）                         |
| `sex`           | 小写                      | `female` / `male`                          |
| `shaderFeature` | 小写                      | `facegen` / `facegenrgbtint` / 数值字符串  |
| `tag`           | 小写                      | `context.tags` 含该标签                    |

**槽位名**（BIPED 槽位 → 小写字符串，完整列表）：
`head, hair, body, hands, forearms, amulet, ring, feet, calves, shield, tail, longhair,
circlet, ears, modmouth, modneck, modchestprimary, modback, modmisc1, modpelvisprimary,
decapitatehead, decapitate, modpelvissecondary, modlegright, modlegleft, modfacejewelry,
modchestsecondary, modshoulder, modarmleft, modarmright, modmisc2, fx01, handtohandmelee,
onehandsword, onehanddagger, onehandaxe, onehandmace, twohandmelee, bow, staff, crossbow, quiver`

**shaderFeature**：仅脸部几何体会得到具名值 `facegen` / `facegenrgbtint`；其余材质显示为
数值字符串（如 `6`）。FaceGen 几何体通常没有 ArmorAddon，建议改用 shape / 源 NIF /
贴图 / shaderFeature 来分类。ARMO/ARMA/slot 身份取自拥有该几何体的激活 Biped 部件。

### 5.6 classifier（标签）

```json
{
    "classifiers": {
        "body:ube": {
            "any": [
                { "shape": "UBEBody" },
                { "nifGlob": "actors/character/**/ube*.nif" },
                { "normalPrefix": "textures/ube/" }
            ]
        }
    }
}
```

-   分类器是递归表达式：`all`（AND，数组）、`any`（OR，数组）、`not`（取反），
    或一个 selector 叶子（同一套 19 个 selector 字段）。
-   叶子没有命中任何字段时判为 false。
-   分类器可以引用其它标签，求值迭代到不动点；推导出的标签进入 `context.tags`，
    之后即可用 `"tag"` 选择。

### 5.7 priority 与排序（优先级）

bindings / rules 按以下元组**升序**排序（后面的优先级更高、胜出）：

1. 普通文件在前，`User` 文件在后；
2. `priority`（文件级默认，可被单条 `priority` 覆盖）；
3. selector 特异性（填了多少个字段）；
4. 规范化后的源文件名；
5. 文件内顺序（`sourceOrder`）。

每个域取**最后一个命中的 binding** 作赢家，三个域互不竞争。legacy 的 `nif`/`baseid`
则是「同一键 User 覆盖 Mod 值」（用户赢），无 priority/specificity 参与。

### 5.8 运行时固定合并顺序

```
DefaultProfile（+ advanced-skin:default 基材质）      ← 根
  → legacy NIF partial（Surface）
  → legacy rules → Surface 域
  → Surface 赢家实例链
  → legacy 种族档案（仅当 UI 绑定了 race→profile）
  → legacy BaseID partial（Appearance）
  → legacy rules → Appearance 域
  → Appearance 赢家实例链
  → legacy rules → Local 域
  → Local 赢家实例链
```

同名参数后写覆盖前写；纹理通道默认继承（inherit），只有 `Disabled` / `Path` 才改值。

---

## 6. RFAOS 与 wetness 贴图制作说明

Advanced Skin 额外绑定两张贴图（纹理槽 71 / 74），由 `HasRfaos` / `HasWetness`
两组显式 flag 控制，黑图始终作为后备绑定（**不以贴图尺寸推断是否存在**）。

### 纹理通道三态

| JSON 写法      | 效果                                     |
| -------------- | ---------------------------------------- |
| 不写该键       | **继承**（沿用上一层/默认值）            |
| `null` 或 `""` | **禁用**（清空路径，不绑 RFAOS/wetness） |
| 非空字符串     | **Path**（绑定该路径）                   |

路径会做规范化：小写、`\`→`/`，去掉开头 `data/` 或 `/textures/` 之前的部分，保留
`textures/...` 尾段。非字符串值会被忽略并告警。

### RFAOS（额外粗糙度/绒毛遮罩/AO/镜面）

`rfaos` 一张图打包四个通道，当前 shader 的精确通道如下：

| 通道  | 含义                            |
| ----- | ------------------------------- |
| **R** | Roughness（粗糙度）             |
| **G** | Fuzz mask（绒毛遮罩）           |
| **B** | Ambient Occlusion（环境光遮蔽） |
| **A** | Specular（镜面）                |

制作时请把粗糙度、绒毛遮罩、AO、镜面分别放入 RGBA 对应通道。

### wetness（湿身贴图：遮罩 / 法线 两种模式）

`wetness` 不只是遮罩，它按像素内容区分两种模式（由 `package/Shaders/Lighting.hlsl`
判断，逻辑见下）：

-   **模式 A（mask/height，由着色器求法线）**：当 `G` 与 `B` 都为 0，**或** `RGB` 为灰度且
    `A ≈ 1`（代码里 `A >= 0.99`）时，整张图被当作**单一的 mask/height 图**：
    -   `R` = 湿润 mask / 高度场，并喂给 `CalculateNormalFromHeight` 由着色器统一求法线；
    -   此时 `G`/`B` 不参与（保持 0 或与 R 相同）。
-   **模式 B（湿润法线 + mask）**：否则（`G`/`B` 非 0，或 RGB 非灰度、A 未满），则按法线图处理：
    -   `RGB` = 湿润（水流）法线；
    -   `A` = 湿润 mask（wet mask）。

决定逻辑的精确判断（摘自 `Lighting.hlsl`）：

```text
if (G == 0 && B == 0) 或 (R == G == B 且 A >= 0.99)：
    R 作为 mask/height，着色器用 CalculateNormalFromHeight 求法线
else：
    RGB = 湿润法线，A = wet mask
```

制作建议：只需一层均匀湿润/水膜时，导出一张灰度（R）遮罩并让 A≈1 即可，简单且省事；
需要定向水流、涟漪等局部法线细节时，改用「法线 RGB + A 遮罩」模式。

### 加载失败

显式路径解析不到（松散发文件或 BSA 都找不到）时，日志输出
`[Advanced Skin] Failed to load configured RFAOS texture: ...`，并静默回退到黑图
（“看起来没有 RFAOS”而非报错崩溃）。打字错误请以此日志排查。

> 兼容性提示：默认**关闭** `_rfaos.dds` / `_wet.dds` 文件名自动发现
> （`EnableLegacyExtraTextureDiscovery`）。该发现会在 `_s.dds` / `_n.dds` / `_msn.dds`
> 旁边找 `_rfaos.dds` / `_wet.dds`，且**不写回共享材质/BSTextureSet**。正式作品建议
> 使用显式 Surface/Local 实例纹理。

---

## 7. 完整有效 JSON 案例

> 以下均为**严格 JSON，无注释**，可直接使用。

### 7.1 UBE 通用身体（自定义 Base Material + Surface 材质）

```json
{
    "priority": 10,
    "materials": {
        "my_ube:base": {
            "parameters": {
                "SkinMainRoughness": 0.62,
                "SkinSecondRoughness": 0.3,
                "F0": 0.028,
                "BaseColorMultiplier": 1.05,
                "UseSSS": true,
                "Translucency": 0.12,
                "sssWidth": 0.2
            }
        }
    },
    "surfaceInstances": {
        "skin:ube_body": {
            "parent": "my_ube:base",
            "parameters": {
                "SkinDetailTiling": 12.0,
                "BodyTilingMultiplier": 2.0
            },
            "textures": {
                "rfaos": "textures/skin/ube_body_rfaos.dds",
                "wetness": "textures/skin/ube_body_wet.dds"
            }
        }
    },
    "classifiers": {
        "body:ube": {
            "any": [
                { "shape": "UBEBody" },
                { "nifGlob": "actors/character/**/ube*.nif" },
                { "normalPrefix": "textures/ube/" }
            ]
        }
    },
    "bindings": [
        {
            "id": "surface:ube_body",
            "domain": "surface",
            "use": "skin:ube_body",
            "match": { "tag": "body:ube" }
        }
    ]
}
```

> 这里把 UBE 专属参数放在自定义基材质 `my_ube:base`（任意名字皆可），由
> `skin:ube_body` 通过 `parent` 继承，**不会**影响非 UBE 皮肤。不要写进
> `advanced-skin:default`——那会作为全局根覆盖波及所有皮肤。

### 7.1.1 如何让系统自动识别 UBE / tag 如何生效

**tag 不是写在 NIF 里的元数据。** tag 是 classifier 在运行时，对每一格几何体的
`GeometryContext`（NIF、shape、diffuse/normal 路径、armor、slot、race、sex 等事实）
**动态推导**出来的标签，然后进入 `context.tags`。所谓“识别 UBE”，本质就是：写一个
classifier，把“什么算 UBE”表达成对 `GeometryContext` 的可判定条件；再用一个只匹配
`tag` 的 Surface binding 把命中者接到 UBE 实例上。

**契约式发布（provider JSON）**：UBE 作者只需发布一个 provider JSON，定义对第三方
稳定的识别契约。推荐按可靠度排序：

1. **`normalPrefix` / `diffusePrefix`（首选，最稳定）**：规定 canonical 的贴图目录前缀，
   例如所有 UBE body 的法线必须位于 `textures/ube/` 之下。
2. **`nifGlob`（次选）**：规定 NIF 路径通配，如 `actors/character/**/ube*.nif`。
3. **`shapeGlob`（再次）**：规定几何体命名通配，如 `UBEBody`。

注意：**不要靠逐 NIF 列举**（`nifGlob` 比一堆 `nif` 条目更通用），也**不要只靠
`slot: "body"`**——`body` 槽位同样会命中非 UBE 身体，无法区分 UBE 与其它身形。

```json
{
    "classifiers": {
        "body:ube": {
            "any": [
                { "normalPrefix": "textures/ube/" },
                { "diffusePrefix": "textures/ube/" },
                { "nifGlob": "actors/character/**/ube*.nif" },
                { "shapeGlob": "UBEBody" }
            ]
        }
    },
    "surfaceInstances": {
        "skin:ube_body": {
            "parent": "advanced-skin:default",
            "textures": {
                "rfaos": "textures/skin/ube_body_rfaos.dds",
                "wetness": "textures/skin/ube_body_wet.dds"
            }
        }
    },
    "bindings": [
        {
            "id": "surface:ube_body",
            "domain": "surface",
            "use": "skin:ube_body",
            "match": { "tag": "body:ube" }
        }
    ]
}
```

**第三方扩展如何接入**：

-   若第三方 UBE skin / 装备扩展**保持 canonical 路径约定**（贴图/NIF/shape 都沿用同一
    约定），那么 provider 的 classifier 会自动命中，无需对方做任何事。
-   若对方**换了路径**，它应**新增自己的 binding**，把 `use` 指向既有的
    `skin:ube_body` 实例（或自建实例继承之），而**不要在不同文件里重复定义同名的
    `body:ube` classifier**。

> ⚠️ 重要：同名 tag 的 classifier 在**后加载的文件会替换**先加载的定义（parser 用
> `parsedClassifiers[tag] = expression` 直接赋值，不是合并）。因此不同文件重复定义
> `body:ube` 会造成“最后一个生效”，前面的识别逻辑被覆盖，导致行为难以预期。
> 正确做法是：provider 只定义一次 `body:ube`，其他人用**新的 binding**去 hook 它。

**如果 UBE 没有稳定约定会怎样**：如果某个 UBE 完全没有稳定的路径 / shape / 贴图命名
约定，系统**无法凭拓扑“猜出”它是 UBE**——分类只基于 `GeometryContext` 里可观察的
事实，没有约定就没有可判定的条件。这种情况下必须**先建立约定**（例如统一
`textures/ube/` 前缀）；未来才可能通过 NIF ExtraData 或显式 registry 之类的机制补充
识别，但当前版本不支持。

**如何验证**：

1. 游戏内用 **Pick Crosshair** 选中目标，查看实际推导出的 **NIF keys**（以及是否含
   皮肤材质），确认你的 `nifGlob`/prefix 与真实值对应。
2. 用明显的临时参数（例如把 `SkinMainRoughness` 设成 0 或 1）观察命中与否，再改回。
3. 当前 UI **不显示 classifier 派生的 tag 列表**——只能通过参数/纹理是否生效来间接
   验证 tag 是否命中。

### 7.2 Lydia（BaseID `000A2C8E`）— Appearance 参数 + Local 身体贴图

```json
{
    "priority": 10,
    "appearanceInstances": {
        "appearance:lydia": {
            "parameters": {
                "SkinMainRoughness": 0.55,
                "SkinDetailStrength": 0.4,
                "Translucency": 0.2,
                "sssWidth": 0.28,
                "FuzzStrength": 1.2
            }
        }
    },
    "surfaceInstances": {
        "skin:ube_body": {
            "parent": "advanced-skin:default",
            "textures": {
                "rfaos": "textures/skin/ube_body_rfaos.dds",
                "wetness": "textures/skin/ube_body_wet.dds"
            }
        }
    },
    "localInstances": {
        "lydia:ube_body": {
            "textures": {
                "rfaos": "textures/skin/lydia_body_rfaos.dds"
            }
        }
    },
    "classifiers": {
        "body:ube": {
            "any": [{ "shape": "UBEBody" }, { "normalPrefix": "textures/ube/" }]
        }
    },
    "bindings": [
        {
            "id": "appearance:lydia",
            "domain": "appearance",
            "use": "appearance:lydia",
            "match": { "baseid": "000A2C8E" }
        },
        {
            "id": "surface:ube_body",
            "domain": "surface",
            "use": "skin:ube_body",
            "match": { "tag": "body:ube" }
        },
        {
            "id": "local:lydia_ube_body",
            "domain": "local",
            "use": "lydia:ube_body",
            "match": { "baseid": "000A2C8E", "tag": "body:ube" }
        }
    ]
}
```

说明：Lydia 的头与身体共享同一套 Appearance 参数；`skin:ube_body` 直接继承内置 root
`advanced-skin:default`（不需要在本文件里重定义它，全局 `DefaultProfile` 已作为根），
身体贴图仍各自从 Surface 链继承；`local:lydia_ube_body` 只把 Lydia 的 UBE 身体 RFAOS
换成专属贴图（省略 `wetness` 即“继承”，不会禁用）。

---

## 8. 故障排查

| 现象                     | 可能原因                                     | 处理                                                                             |
| ------------------------ | -------------------------------------------- | -------------------------------------------------------------------------------- |
| 覆盖完全不生效           | JSON 不是严格 JSON；文件未放对目录；名字拼错 | 用 JSON 校验器；确认在 `Overrides` 或 `Overrides\User`；看日志 `[Advanced Skin]` |
| 某条 rule/binding 被丢弃 | selector 有未知字段或非字符串字段            | 只用上表 19 个字段；检查拼写                                                     |
| Local binding 不生效     | 缺 actor 或 surface 选择器字段               | 同时给 `baseid`/`race`/`sex` 与 `nif`/`shape`/`tag` 等                           |
| 实例无效                 | 缺父、跨域父、成环、深度>32                  | 检查 `parent` 域归属与层级                                                       |
| BaseID 不命中            | 加载顺序变了 / FormID 不匹配                 | 重新确认 NPC 原始 FormID（见第 3 节风险）                                        |
| NIF 覆盖偶发失效         | 源 NIF 被覆盖、同签名多来源导致 origin 为空  | 优先用 shape / diffuse / tag 分类；核对源 NIF                                    |
| RFAOS/wetness “没有”     | 路径打错、贴图缺失                           | 看日志 `Failed to load configured ...`；核对 `textures/` 尾段                    |
| wetness 强度不随 JSON 变 | 湿身参数是全局设置，不在 SkinProfile         | 改全局设置而不是覆盖 JSON                                                        |
| 改 JSON 不生效           | 热重载约 60 帧一次；或缓存未失效             | 等 1–2 秒；必要时重开面板/重新加载几何体                                         |

## 9. 发布检查表

-   [ ] JSON 通过严格 JSON 校验（无注释、无尾逗号）。
-   [ ] 文件放入 `Data\Shaders\Skin\Overrides\`（不要放 `User`，`User` 是用户覆盖）。
-   [ ] 目标几何体的 diffuse / normal 已由 ESP/NIF/`BSTextureSet` 正确分配。
-   [ ] `rfaos` / `wetness` 路径以 `textures/` 开头、正斜杠、无 `data/` 前缀。
-   [ ] 确认 NPC BaseID 为**原始 FormID** 并记录插件与加载顺序依赖。
-   [ ] 每条 binding 的 `domain`、`use` 指向同域实例；Local binding 含 actor+surface 选择器。
-   [ ] 实例 `parent` 同域、无环、深度 ≤ 32。
-   [ ] selector 只使用文档列出的 19 个合法字段。
-   [ ] 用日志确认 `[Advanced Skin] Loaded ... surface/... appearance/... local material(s) ...`
        且无 `[Advanced Skin]` 警告/错误。
-   [ ] 在游戏内用 Pick Crosshair 验证命中与合并链（`Chain: ...`）。
