# osgVerse TODO
- 模块: 编译，系统，渲染，模型，动画，编辑器，插件，例程，脚本，其它
- 所属库：osgVerse的库/例程目录名，或者留空
- 任务类型：新增，优化，除虫
- 完成情况：百分比，已完成( 100% )，已废止( :x: )，或者留空
- 已完成的"新增"任务如果需要修改/除虫，则新建TODO列表行
- 已完成的"优化/除虫"任务如果需要再次优化/除虫，则在原列表行直接改动

#### 本次审视说明（2026-11-17）
- 范围：原 93 条 TODO、当前工作区与最近提交、关键源码与测试目录、README/AGENTS/HELP 文档；以代码证据为主，未做全平台运行时回归。
- 排序：有缺漏/未修复条目按优先级 P0→P3 排列；复核后未见明显问题的历史完成项统一放到表格末尾。P0=阻断构建或主流程，P1=核心功能/稳定性缺失，P2=重要完善，P3=覆盖度与体验，"-"=保留项。
- 重要变更：合并原第 23/32 行重复条目；多条原标 100% 但代码证据不支持的条目已明确降级并在备注注明"原100%"；新增 vksplat/3DGS 训练、MLT、Earth AI、Terrarium、EXR/Lottie、PerfProfiler 等文档漏记项。
- 当前未提交改动：本次文档更新，以及 `pipeline/Utilities.cpp`（CORE 下禁用 lightmapper 的开关）；其余修复均已进历史提交。

#### 更新说明（2026-11-22）
- many_lights_test 新增 1-6 渲染模式切换（HUD 提示按键）：forward/deferred × 无灯/多灯 × 无GI/GI 六种组合运行时可切（也可 `--mode N` 无头指定）；模式 1/2 验证中发现 **forward 输出不经过色调映射**的架构缺口，已列为 P1 计划项。
- 光照计划重排（见 AGENTS.md "Screen-Space Lighting Effects"）：① velocity buffer + 时域重投影 → ② forward 输出接入后处理 → ③ SSGI 半分辨率 → ④ 二次弹射/弯曲法线 → ⑤ 点光源接触阴影 → ⑥ 探针 GI → ⑦ JSON 内嵌 shader 再生成。

#### 本次审视说明（2026-11-20）
- 范围：光照/屏幕空间效果（前向光照簇、SSAO 应用位置、法线编码、SSGI 初版）、shader 目录重组、AGENTS.md 测试经验补充；以 RTX 4080 桌面 GL2 兼容模式运行验证为主（1280×720 截图逐像素 A/B）。
- 已完成并验证：Forward 光照簇、SSAO 间接光遮蔽、八面体法线、SSGI 初版（回退链第一级）、shaders 目录 deferred//forward/ 重组。
- 光照系统后续计划按 AGENTS.md "Screen-Space Lighting Effects" 一节推进：velocity buffer + 时域重投影 → SSGI 半分辨率/多弹射 → 探针 GI 回退级 → 点光源接触阴影。

#### TODO列表
| 模块      | 所属库        | 类型     | 完成情况   | 具体内容描述  | 备注 |
|-----------|-------------|----------|---------|-------------|------|
| 系统      |              | 新增     |         | 自动化回归测试与持续集成 | P0；`tests/CMakeLists.txt` 的 NEW_TEST/NEW_EXAMPLE 只生成手工程序，无 `enable_testing()/add_test()`，仓库无 GitHub Actions/其他 CI 配置；多平台提交无法自动验收 |
| 系统      |              | 新增     |         | 编译器与平台矩阵自动构建 | P1；近期连续修 VS2015/VS2017/Windows lottie/MLT 问题，说明矩阵缺少 CI 闸门；至少覆盖 MSVC 2015/2017/最新版、MinGW、GCC/Clang、WASM、Android |
| 渲染      | pipeline     | 除虫     | 85%     | 多 Slave（Across Screen）与 CompositeViewer 支持 | P1；原两行 75% 已合并；`applyStagesToView(View*)` 和 `applications/viewer_composite` 已可用，resize 处理 slave 投影偏移；遗留 `Pipeline.cpp:1146` FIXME：前向天空盒/前向场景被禁用；TAA 场景也需移除天空盒 |
| 渲染      | pipeline     | 除虫     |         | Viewer 的 DrawThreadPerContext / ThreadPerCamera 多线程绘制 | P1；实际未支持，`viewer_composite/viewer_main.cpp:204`、`viewer_main.cpp:280`、`pipeline_test.cpp:331` 均强制 SingleThreaded，原因是共享阴影/RTT 纹理未同步 |
| 渲染      | pipeline     | 优化     | 90%     | 后处理抗锯齿 FXAA/TAA | P1；原 50% 已过时；TAA 已实现抖动投影、历史缓冲、重投影、邻域 clamp、camera-cut 重置与低性能回退 FXAA；无速度缓冲，运动物体拖影；与天空盒同时启用有限制 |
| 渲染      | pipeline     | 新增     |         | ShadowModule 支持 PSM/LispSM/TSM 提升阴影质量 | P1；原 100% 降级为 0%：`ShadowModule.h:26-30` 技术枚举仅 Default/BandPCF/PoissonPCF/VSM/ESM/EVSM，全仓无 PSM/LispSM/TSM |
| 渲染      | pipeline     | 优化     | 50%     | ShadowModule 支持 EVSM/PCSS 软阴影 | P1；原 100% 降级；EVSM 与 blur 链已实现，PCSS 全仓未实现 |
| 渲染      | pipeline     | 新增     | 40%     | ShadowModule 支持多光源阴影（点光源和平行光） | P1；当前级联/阴影图主要围绕单一方向光组织，点光源立方体阴影图链路未见；已有 `many_lights_test.cpp` 但只验证多光源前向渲染压力 |
| 渲染      | pipeline     | 除虫     | 40%     | NodeSelector 会被物体遮挡 | P1；原 100% 降级；`NodeSelector.cpp:133` 的 `Depth(ALWAYS)` 关键代码被注释，仅 ColorMask 处理不足以穿墙 |
| 渲染      | pipeline     | 除虫     | 60%     | 非 MRT 流水线且 GBuffer 支持 PBR 透明体/前向光照 | P1；原 100% 降级；`PipelineStandard.cpp:821`、`:882` 在 MRT 不可用时直接 fatal，前向路径可渲染透明物但未见完整非 MRT GBuffer 落地 |
| 渲染      | pipeline     | 除虫     | 65%     | SkyBox 大坐标显示/裁切与 tex2d 接缝 | P1；原 100% 降级；eye 平移+far 缩放与 CLAMP_TO_EDGE 已有，但 equirect 采样 atan 跳变仍可能出缝，未见边界混合；`COMPLEX_SKYBOX` 未定义 |
| 渲染      | pipeline     | 新增     | 60%     | OpenXR 标准 VR 设备渲染 | P1；原 100% 降级；RenderCallbackXR 主体和示例存在，但 Emscripten 禁用、Android/EGL-GLES session 未实现、XCB/Wayland 仍 TODO |
| 插件      | osgdb_mediastream | 除虫 | 50%   | 流媒体推流/拉流与 WebRTC 云渲染 | P1；原 100% 降级；ZLMediaKit 有 SDP/WebSocket 示例，但 `ReaderWriterZLMedia.cpp:177-187` 写接口 `NOT_IMPLEMENTED`，README 仍标 TBD |
| 系统      |              | 新增     |         | GPU/显存预算、资源回收与生命周期治理 | P1；`PerfAnalyzer` 能报 VRAM_PRESSURE、`releaseGLObjects` 零散存在，但无显存预算、LRU/eviction、资源池、卸载回收策略；大模型/大场景仿真必备 |
| 系统      |              | 新增     |         | 并发规范与竞态验证（TSAN/锁审计） | P1；仅 `IncrementalCompiler` 等局部锁与 QuickThread，结合上条多线程绘制不可用，缺乏系统性并发约束 |
| 系统      |              | 新增     |         | 结构化日志、指标、追踪与崩溃上报 | P1；现有 OSG_NOTICE/WARN、ConsoleHandler、GL debug 回调与 backward-cpp 是零散能力，缺统一级别/标签、指标导出、追踪与远程告警 |
| 渲染      | pipeline     | 除虫     | 75%     | GLSL 1.2 / Core Profile / WebGL 的 shader 兼容层 | P2；原 100% 降级；ShaderLibrary 的 VERSE_* 宏覆盖主体，poi_symbols 两个 shader 已于 2026-11-17 改用 VERSE_TEX2D 与 VERSE_INSTANCE_ID 适配（含 GLSL120 的 gl_InstanceIDARB 回退）；`particles.vert.glsl:33` 仍裸用 gl_InstanceID，同类低端卡/WebGL1 风险待处理 |
| 编译      |              | 新增     | 80%     | backward-cpp 自动打印崩溃堆栈 | P2；原 100% 降级；头文件已引入且部分测试启用，但 viewer/editor 等主程序未见统一接入，不是"自动全局" |
| 数据读写  | readerwriter | 优化     | 60%     | 自动检查输入几何体正确性并优化 | P2；原 100% 降级；有 `MeshInternalOptimizer` 与固定功能优化，但无统一自动入口，Web 插件中相关调用被注释 |
| 辅助工具  | helpers      | 优化     | 80%     | Unity 导出插件第一版 | P2；原 100% 降级；几何/PBR 已实现，但 `SceneExporter.cs:113` 坐标手性 FIXME，camera/collider 仍 TODO |
| 插件      | osgdb_pbrlayout | 除虫   | 80%     | 伪插件自动替换 PBR 贴图顺序 | P2；原 100% 降级；ORM 分量只取其一有 FIXME（ReaderWriterPBRLayout.cpp:169），奇数参数串可能越界读 p[j+1]（:194） |
| 渲染      | pipeline     | 除虫     |         | HBAO 法线花斑、小物件 AO 噪声、简单几何光照异常边界线 | P2；三条均未见修复代码，当前 AO 是 SSAO（`std_ssao.frag.glsl`），Bloom 也仍沿用旧模糊方案 |
| 渲染      | pipeline     | 新增     |         | 支持后处理 Atmospheric Scattering 天空盒 | P2；未见实现，可与 SkyBox 接缝/大坐标问题合并推进 |
| 渲染      | pipeline     | 优化     |         | 大坐标模型阴影 bias 按坡度动态调整 PolygonOffset | P2；未见 slope-scale depth bias/PolygonOffset 联动 |
| 渲染      | pipeline     | 除虫     |         | clearStagesFromView() 后移动视角再恢复，阴影错误 | P2；`Pipeline.cpp:1079-1113` 有清理逻辑，但未见回归测试，状态仍按未验证处理 |
| 渲染      | pipeline     | 新增     |         | 集群渲染、多机帧同步、网格与融合带调整 | P2；尚未开始，大型仿真/CAVE 必备能力 |
| 渲染      | pipeline     | 新增     | 80%     | 支持自定义扩展 Pipeline::Stage | P2；`tests/pipeline_test.cpp:218-308` 已演示 GBuffer/中间 Stage/显示 Stage/VR 扩展及 HistoryBufferCallback，可提升进度 |
| 渲染/例程 | pipeline     | 新增     | 30%     | MultiEffectNode 独立测试案例 | P2；类与序列化包装齐全，但测试中只 include 未实例化 |
| 渲染      | pipeline     | 新增     |         | NV HBAO 自定义 Stage 演示 | P2；全仓无 HBAO 实现，先补 AO 算法本体再谈演示 |
| 模型/动画 | animation    | 优化     | 70%     | 粒子系统（基于计算着色器） | P2；原 80% 更正；已有 CPU LUT/CPU 顶点属性/GPU 几何着色器三条路径，无 compute 路径；compute 只用于 NIS/3DGS 等其他模块 |
| 模型/动画 | animation    | 新增     |         | PhysX 刚体、步行漫游与驾驶 | P2；当前是 Bullet 路线，未见 PhysX 接入 |
| 脚本      | script       | 新增     | 85%     | 使用 Serialization 映射 OSG 与 osgVerse 核心函数 | P2；原 80% 可上调；wrappers 数量充足，scripting_test 有反射/脚本示例；继续补 osgVerse 新库（MLT/AI/3DGS）映射 |
| 脚本      | script       | 新增     |         | SWIG 接入 C# 和 Python | P2；明确未开始；无 .i 文件与构建集成，`swig_interface_test.cpp` 实为 wrapper 序列化测试，命名易误导 |
| 数据读写  | readerwriter | 优化     | 85%     | FBX/GLTF 导入角色与动画并显示 | P2；原 80% 可上调；GLTF 骨骼/蒙皮/动画与 FBX 单层动画已通并有 player_animation_test；FBX 多层动画、blend-shape 动画未实现且无独立 FBX 播放测试 |
| 数据读写  | readerwriter | 新增     |         | MeshSync 流传输修改模型，Blender 与编辑器动态切换编辑 | P2；未开始 |
| 数据读写  | readerwriter | 新增     |         | 倾斜摄影数据自动修改处理框架 | P2；已有顶层/相邻层合并，自动处理框架未落地 |
| 辅助工具  | helpers      | 新增     |         | Blender 导出插件第一版 | P2；未开始 |
| 辅助工具  | helpers      | 新增     |         | 3dsmax 导出插件第一版 | P2；未开始 |
| 编译      |              | 新增     | 50%     | UWP 编译流程 | P2；可编译通过，缺测试例子 |
| 编译      |              | 新增     | 50%     | Android 编译流程与 Android Studio 集成 | P2；库可编译，缺测试例子 |
| 编译      |              | 新增     |         | macOS 与 iOS 编译流程 | P1；成熟引擎必需的多端交付缺口，原表为空 |
| 系统      |              | 新增     |         | 统一错误码、异常策略与失败恢复 | P2；仅少量 catch，SaveSceneGLTF 等处仍以警告代替错误，缺统一错误模型与失败注入测试 |
| 数据读写  |              | 新增     |         | 数据 schema 版本、迁移与向后兼容测试 | P2；wrappers 有 OSG 编译期版本适配，但场景/配置格式无版本治理 |
| 系统      |              | 新增     |         | 自动化性能基线与帧时间回归门禁 | P2；PerfProfiler/PerfAnalyzer 与 perf_profile_test 已提供采集手段，但无基线数据与门禁 |
| 系统      |              | 新增     |         | 发布流程、CHANGELOG、SBOM 与许可证门禁 | P2；有 LICENSE/THIRDPARTY_LICENSES，无 CHANGELOG、发布脚本、SBOM 与自动合规检查 |
| 系统      |              | 新增     |         | 网络状态同步与多人仿真（replication/authority/rollback） | P2；传输层有 WebRTC/WebSocket，但没有仿真状态同步机制，限制多人仿真场景 |
| 渲染      | pipeline     | 新增     | 40%     | OpenXR 手柄绑定、手势与触觉交互 | P2；已有 ActionSet/姿态输入，但缺 hand tracker 调用、haptic 与一致性测试 |
| 编译      |              | 新增     |         | Docker 自动编译依赖库、OSG 与 osgVerse 多版本程序包 | P2；未开始 |
| 编译      |              | 新增     |         | 手机浏览器支持测试（微信/Chrome/Opera） | P3；未开始 |
| 编译      |              | 新增     | 90%     | 支持 GL3 Core Profile | P3；原 100% 保留主体能力，但 Windows 下 lightmapper 裸 GL 符号问题见首行，暂不标满 |
| 编译/渲染 |              | 新增     | 95%     | GLES2 / GLES3 与 GLSL 1.2 兼容运行 | P3；原 100% 微调，poi_symbols 兼容问题已修复（见 shader 兼容层行） |
| 渲染      | pipeline     | 优化     | 60%     | 通过脚本管理标准与自定义 Pipeline | P3；原 100% 降级；C++ 侧已接线（`PipelineStandard.cpp:1686`、`viewer_main.cpp:335`、`qt_viewer/viewer_main.cpp:78` 调 `Pipeline::load`），脚本层暴露程度仍需确认 |
| 例程      | ai           | 新增     | 20%     | TrellisPipeline 测试程序 | P3；`ggml_related_test.cpp` 只 include，未调用 generate |
| 例程      | ai           | 新增     |         | ObjectTracker 多目标跟踪测试 | P3；实现存在但无测试 |
| 例程      | animation    | 新增     |         | VectorSmoother 与 TimeOut 数据滤波/时间控制测试 | P3；实现存在但无测试 |
| 例程      | modeling     | 新增     |         | FFDModeler 与 BSplineVolume 自由变形测试 | P3；实现存在但无测试 |
| 例程      | modeling     | 新增     |         | MeshDeformer 网格变形/蒙皮测试 | P3；实现存在但无测试 |
| 例程      | modeling     | 新增     |         | MathExpression 数学表达式解析求值测试 | P3；实现存在但无测试 |
| 例程      | modeling     | 新增     |         | PointCloudQuery 点云查询测试 | P3；实现存在但无测试 |
| 例程      | modeling     | 新增     | 10%     | DynamicPointLine/Polyline/Polygon 动态线面绘制测试 | P3；mesh_process_test 只 include 未使用 |
| 例程      | modeling     | 新增     | 50%     | 重写 TextureMapping 和 ConvexHull 测试程序 | P3；texture_reprojection_test 已覆盖贴图重投影，ConvexHull 仍只在 deprecated 测试 |
| 例程      | pipeline     | 新增     |         | TsfFramework Windows TSF 输入法测试 | P3；实现存在但无测试 |
| 例程      | readerwriter | 新增     |         | GraphicsWindowWin32NV 窗口实现测试 | P3；实现存在但无测试 |
| 例程      | readerwriter | 新增     | 40%     | FeatureDefinition 与 EncodedFrameObject 测试 | P3；FeatureDefinition 在 poi_cluster_test 中有使用，EncodedFrameObject 全仓无使用 |
| 例程      | ui           | 新增     | 40%     | SpiderEditor 节点图编辑器测试 | P3；控件存在，但 uniform_serializer.cpp 的 createSpiderNode 仍返回 -1 |
| 例程      | ui           | 新增     | 50%     | Timeline 时间轴控件测试 | P3；scene_editor 菜单已接入，无独立测试 |
| 例程      | ui           | 新增     | 40%     | VirtualKeyboard 与 FileDialog 测试 | P3；实现存在，compressing_test 中 FileDialog 调用被注释 |
| 例程      | ui           | 新增     | 60%     | 常用 UI 组件（Label/Button/CheckBox/ComboBox/InputField/Slider/ListView）演示 | P3；imgui_test 仅覆盖 Button/Checkbox 等部分 |
| 模型/动画 | animation    | 新增     | 80%     | TweenAnimation 路径/纤程动画验证 | P3；原备注"待测试"；已有手工交互测试且 CMake 仍标 FIXME，缺自动断言 |
| 渲染/例程 | pipeline     | 新增     | 70%     | 多光源测试案例 | P3；`tests/many_lights_test.cpp` 已存在，可上调进度，待补压力基线断言 |
| 系统      |              | 新增     |         | 统一的输入设备抽象与手柄映射配置 | P3；现有窗口/UI 输入分散，XR 输入亦缺绑定配置 |
| 渲染      | pipeline     | 优化     | 100%     | Forward 渲染路径接入光照簇（与 deferred 共享 LightCluster） | P1；`std_forward_render.frag.glsl` 新增 `VERSE_LIGHT_CLUSTER` 分支与 `getClusterIndex()`，`LightModule::applyToStateSet()` 返回下一可用纹理单元；many_lights/forward_pbr A/B 验证与基线差异仅 HUD 文本与容差噪声 |
| 渲染      | pipeline     | 优化     | 100%     | SSAO 应用位置审查：只遮蔽间接光 | P1；漫反射 `min(materialAO, SSAO)`、镜面 GTSO（Jimenez 2016）；A/B 与 HEAD 基线几乎逐像素一致（5/1,024,000 像素），负向对照 116,576 像素证明测试敏感 |
| 渲染      | pipeline     | 优化     | 100%     | GBuffer 法线八面体编码 | P1；新增 `normal_encoding.module.glsl` 共享模块，GBuffer/lighting/shadow combine/forward 全链路同步 |
| 渲染      | pipeline     | 新增     | 100%     | SSGI 单弹射初版（间接光回退链第一级） | P1；`std_ssgi(+_blur).frag.glsl`：SSDO 风格 16 采样，输出 rgb=辐亮度×覆盖率（预乘）、a=覆盖率；lighting ambient 拆 3 MRT（Color/IblAmbient/IblSpecular）；tonemapping 合成 `ssgi.rgb + (1-覆盖率)×IBL 漫反射`；曝光计量同步纳入；`GIRadius<=0` 默认屏幕空间半径（40px）适配任意场景尺度； GLES2 与 debugShadowCombination 跳过；质量优化见下 |
| 渲染      | pipeline     | 优化     | 100%     | assets/shaders 目录整理：deferred/ 与 forward/ 子目录 | P2；27 个延迟阶段 shader 入 deferred/、3 个 forward shader 入 forward/，共享模块（common_quad/IBL 预计算/skybox）留根；子目录 include 改 `../` 相对路径（OSG glsl 插件按包含文件所在目录解析，裸文件名会找不到）；全部 C++ 加载点同步后五个测试程序验证通过 |
| 渲染      | pipeline     | 新增     | 100%     | many_lights_test 六模式交互切换（HUD 提示按 1-6） | P1；1=forward+无灯+无GI，2=forward+多灯+无GI，3=deferred+无灯+无GI，4=deferred+多灯+无GI，5=deferred+无灯+GI，6=deferred+多灯+GI；灯光瞬时开关=置零 LightDrawable 颜色（nodeMask 摘除会被 LightGlobalManager 的 cull 回调/prune 延迟数帧，不可用）、GI=stage 相机 nodeMask + 消费端重绑零纹理、forward=FORWARD_SCENE_MASK + createForwardStateSet；六模式截图 + 按键注入验证（key3 ≈ mode3，avg 1.149）均符合预期；支持 `--mode N` 无头驱动 |
| 渲染      | pipeline     | 新增     |         | Forward 输出接入后处理链（色调映射/曝光） | P1；模式 1/2 验证发现：forward 相机在 display 阶段之后直接画后备缓冲，输出未做色调映射与曝光（高光溢出、与 deferred 亮度不可比）；方案：forward 场景画进后处理链读取的缓冲，或在 forward pass 后追加 tonemap 阶段 |
| 渲染      | pipeline     | 新增     |         | Velocity buffer + 屏幕空间效果时域重投影 | P1；SSAO/SSGI 16 采样有噪声且运动闪烁；TAA 已有历史链但无逐像素运动矢量（运动物体拖影同源）；GBuffer 输出 VelocityBuffer（经上一帧视图矩阵的裁剪空间位置），SSAO/SSGI 时域累积 + TAA 正确消拖影；计划详见 AGENTS.md "Screen-Space Lighting Effects" |
| 渲染      | pipeline     | 优化     | 30%     | SSGI 质量与性能优化 | P1；半分辨率 + 深度/法线联合双边上采样、第二次弹射（读 SsgiBlurredBuffer 可直接叠加）、弯曲法线输出、GI 强度随场景亮度自动调参；当前为 16 采样全分辨率初版 |
| 渲染      | pipeline     | 新增     |         | 间接光回退链第二级：探针 GI（DDGI 风格） | P2；回退链插槽已在 tonemapping 落地（screen-space → IBL），探针用于覆盖率低的像素替代/增强 IBL 漫反射回退 |
| 渲染      | pipeline     | 新增     |         | 点光源延迟接触阴影（screen-space raymarch） | P2；在 lighting 阶段对深度缓冲光线步进，叠加在现有 cluster lighting 之上，低成本补足点光源阴影缺口（另见多光源阴影行） |
| 渲染      | pipeline     | 除虫     |         | standard_pipeline*.json 内嵌 shader 与 .glsl 源漂移 | P2；JSON 仍是旧版（2-MRT ambient、RGB 法线、无 GI、旧目录名），每次 shader 改动漂移加大；从 .glsl 再生成或取消内嵌 |
| —— | —— | —— | —— | **以下为复核后未见明显缺漏的历史完成项（未做运行时回归）** | —— |
| 编译      |              | 新增     | 100%     | 支持 Static 编译流程 | 优先级：-；选项联动与静态插件注册机制齐全 |
| 编译      |              | 新增     | 100%     | 支持 MinGW 编译流程 | 优先级：-；CMake 与 Setup.sh 分支齐备 |
| 编译      |              | 新增     | 100%     | 解决 ept 插件和 laszip 的 Ubuntu 编译问题 | 优先级：- |
| 编译      |              | 新增     | 100%     | 支持摩尔线程、景嘉微、Quadro、树莓派、RISC-V 编译和测试运行 | 优先级：- |
| 编译      |              | 新增     | 100%     | 支持 Emscripten / WebAssembly 编译流程并输出浏览器端 | 优先级：-；仅 earth_demo 子目录未启用，不影响主流程 |
| 编译      |              | 新增     | 100%     | 通过 Emscripten 支持 WebGL1/2 接口 | 优先级：- |
| 编译      |              | 除虫     | 100%     | VS2017 下 MLT 三处编译错误：Layer 移动构造、std::tuple 缺失、is_signed_v 默认实参 ICE | 优先级：-；已在 aa8ebcd4 修复并经 cl.exe 全翻译单元验证 |
| 编译      |              | 除虫     | 100%     | VS2015 / Windows lottie 插件 / headless SDL 等近期构建修复 | 优先级：-；见 c934e3a7、2b3b5dee、389b040e |
| 编译      | 3rdparty/thorvg | 除虫   | 100%     | VS2017 构建 verse_core 时 tvgStr.cpp:99 的 NAN 表达式报 C2143 | 优先级：-；已用 `std::numeric_limits<float>::quiet_NaN()/infinity()` 替换 C 宏，`osgVerseDependency` 与 `osgVersePipeline` 均构建通过 |
| 编译/渲染 |              | 新增     | 100%     | 支持 Angel 并通过自定义方式切换不同底层（DX/Vulkan） | 优先级：- |
| 渲染      | pipeline     | 优化     | 100%     | 不用 NodeMask 管理 Deferred 场景 | 优先级：- |
| 渲染      | pipeline     | 除虫     | 100%     | 低帧率下 Deferred 比 Forward 慢一拍 | 优先级：- |
| 渲染      | pipeline     | 除虫     | 100%     | 加入自定义管线后全屏/窗口切换阴影与场景显示错误 | 优先级：- |
| 渲染      | pipeline     | 除虫     | 100%     | OSG 3.1.1 Texture2DArray 渲染报错 | 优先级：- |
| 渲染      | pipeline     | 除虫     | 100%     | OSG 3.1.1 缩放窗口后多层阴影 resize 问题 | 优先级：- |
| 渲染      | pipeline     | 除虫     | 100%     | 摩尔 S50/S80 上延迟与非延迟场景深度融合错误 | 优先级：- |
| 渲染      | pipeline     | 优化     | 100%     | 后处理 Bloom 模糊方法锯齿问题 | 优先级：-；新发现的小物件 Bloom 噪声另立行跟踪 |
| 渲染      | pipeline     | 新增     | 100%     | PBR 透明体前向光照通路 | 优先级：-；完整非 MRT GBuffer 另立行跟踪 |
| 渲染      | pipeline     | 优化     | 100%     | PBR Lighting 通过 LightManager 光源数据表计算多种光照 | 优先级：- |
| 渲染      | pipeline     | 除虫     | 100%     | Sponza 法线贴图共享与 metallic 闪烁 | 优先级：- |
| 渲染      | pipeline     | 除虫     | 100%     | 全屏/窗口切换或缩放后多层阴影显示不正确 | 优先级：- |
| 渲染      | pipeline     | 优化     | 100%     | 明确 metallic/roughness 表达与导出方式 | 优先级：- |
| 插件      | plugins      | 新增     | 100%     | libhv 支持多种网络协议替代 curl | 优先级：-；http/https/ftp/ftps 与 POST 均已接线 |
| 插件      | plugins      | 新增     | 100%     | leveldb 数据库读写与 osgb 存储 | 优先级：- |
| 数据读写  | readerwriter | 新增     | 100%     | KTX 纹理读取/写入与纹理压缩 | 优先级：- |
| 数据读写  | readerwriter | 新增     | 100%     | 默认常见图片格式读取（WASM 等场合免插件） | 优先级：- |
| 数据读写  | readerwriter | 新增     | 100%     | 倾斜摄影相邻层/顶层合并与 3dtiles 读取 | 优先级：- |
| 模型/动画 | animation    | 优化     | 100%     | PlayerAnimation 支持直接输入骨骼和动画数据 | 优先级：- |
| 模型/动画 | animation    | 优化     | 100%     | PlayerAnimation 支持外部数据驱动 | 优先级：- |
| 插件      | osgdb_mlt    | 新增     | 100%     | MapLibre Tile（MLT）与 pmtiles 数据读取插件 | 优先级：-；README.md 已列，VS2017 编译问题已修；原 TODO 漏记 |
| 渲染/模型 | plugins/3dgs | 新增     | 100%     | 3D Gaussian Splatting 读取、渲染、简化，以及 vksplat/GaussianTrainer 训练 | 优先级：-；原 TODO 漏记，见 README.md、tests/gaussian_* 与提交 54ad54a7 |
| 系统      | earth_explorer | 新增   | 100%     | Earth Explorer 多供应商 AI 助手与工具调用 | 优先级：-；原 TODO 漏记，见 HELP.zh-CN.md |
| 数据读写  | osgdb_terrain | 新增    | 100%     | Terrarium 高程数据与 OSG 插件体系集成 | 优先级：-；原 TODO 漏记，见 HELP.zh-CN.md |
| 插件      | osgdb_exr/osgdb_lottie | 新增 | 100% | EXR 与 Lottie 动画插件 | 优先级：-；原 TODO 漏记，Windows lottie 构建已修 |
| 系统      | pipeline     | 新增     | 100%     | PerfProfiler / PerfAnalyzer 相机与节点统计 | 优先级：-；性能基线与门禁另立行 |
| 编译/例程 |              | 新增     | 100%     | headless SDL 渲染支持 | 优先级：-；见 389b040e |
| 例程      | modeling     | 新增     | 100%     | AnnotationCluster 标注聚类测试 | 优先级：-；poi_cluster_test 已实例化 |
| 例程      | pipeline     | 新增     | 100%     | NISUpscaler NVIDIA 图像上采样测试 | 优先级：-；forward_pbr_test 已调用 |
| 例程      | ui           | 新增     | 100%     | ImGuiManager 的 3D/RTT 模式（addToTexture）测试 | 优先级：-；imgui_test 已覆盖 |
