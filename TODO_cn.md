# osgVerse TODO
- 模块: 编译，系统，渲染，模型，动画，编辑器，插件，例程，脚本，其它
- 所属库：osgVerse的库/例程目录名，或者留空
- 任务类型：新增，优化，除虫
- 完成情况：百分比，已完成( 100% )，已废止( :x: )，或者留空
- 已完成的"新增"任务如果需要修改/除虫，则新建TODO列表行
- 已完成的"优化/除虫"任务如果需要再次优化/除虫，则在原列表行直接改动

#### TODO列表
| 模块      | 所属库        | 类型     | 完成情况   | 具体内容描述  | 备注 |
|-----------|-------------|----------|---------|-------------|------|
| 编译      |              | 新增     |         | 支持Docker自动编译依赖库，OSG，以及osgVerse的多版本程序包 | |
| 编译      |              | 新增     | 50%     | 支持UWP编译流程 | 可以编译通过，需要提供测试例子 |
| 编译      |              | 新增     | 50%     | 支持Android编译流程，支持直接纳入Android Studio | 目前已经可以编译Android库，需要提供测试例子 |
| 编译      |              | 新增     |         | 支持Apple Mac OSX和IOS编译流程 | |
| 编译      |              | 新增     |         | 测试手机端浏览器的支持情况，微信，Chrome，Opera等 | |
| 渲染      | pipeline     | 新增     |         | 支持集群渲染，多机自动帧同步，网格调整，融合带调整 | |
| 渲染      | pipeline     | 除虫     |         | BrdfLUT处理的结果没有考虑滤波问题 | https://github.com/xarray/osgverse/issues/7 |
| 渲染      | pipeline     | 新增     |         | SkyBox中支持后处理的Atmospheric Scattering天空盒 | |
| 渲染      | pipeline     | 除虫     |         | 对于大坐标模型，NodeSelector无法正确显示 | |
| 渲染      | pipeline     | 新增     |         | ShadowModule支持来自多个光源的阴影，包括点光源和平行光 | |
| 渲染      | pipeline     | 优化     |         | 对于大坐标模型，阴影bias需要根据坡度值动态修改PolygonOffset | |
| 渲染      | pipeline     | 优化     | 75%     | 目前没办法处理多Slave（Across Screen）和CompositeViewer的情况 | 多Slave可处理但不能显示Forward天空盒 |
| 渲染      | pipeline     | 优化     |         | 目前没办法处理Viewer多线程DrawThreadPerContext和ThreadPerCamera | |
| 渲染      | pipeline     | 除虫     |         | 处理某些模型时，HBAO法线出现大量花斑 | |
| 渲染      | pipeline     | 除虫     |         | 对于小物件，AO的效果噪声比较严重，且Bloom结果不好 | |
| 渲染      | pipeline     | 除虫     |         | 对于简单几何体，光照容易产生异常边界线，效果不好 | |
| 渲染/例程  | pipeline     | 新增     |         | 增加一个测试案例演示如何增加多个光源，并测试多光源渲染压力 | |
| 渲染      | pipeline     | 除虫     |         | 如果clearStagesFromView()删除管线，移动视角后再恢复，则阴影错误 | |
| 渲染      | pipeline     | 新增     | 50%     | 支持后处理抗锯齿方案，FXAA/TAA | 目前已经支持FXAA |
| 渲染/例程  | pipeline     | 新增     | 40%     | 支持自己扩展Pipeline::Stage，增加一个测试案例演示使用NV HBAO | |
| 渲染      | pipeline     | 优化     | 75%    | 目前没办法处理多Slave（Across Screen）和CompositeViewer的情况 | 多Slave可处理但不能显示Forward天空盒 |
| 辅助工具  | helpers       | 新增     |         | 实现Blender的导出插件第一版(静态模型，PBR材质) | |
| 辅助工具  | helpers       | 新增     |         | 实现3dsmax的导出插件第一版(静态模型，PBR材质) | |
| 数据读写  | readerwriter  | 新增     |         | 支持MeshSync流传输修改模型，实现Blender和编辑器的动态模型切换编辑 | |
| 数据读写  | readerwriter  | 新增     |         | 支持倾斜摄影数据的自动修改处理框架 | |
| 数据读写  | readerwriter  | 优化     | 80%     | FBX和GLTF插件支持导入角色和角色动画并显示 | 已实现GLTF/FBX导入角色，剩余部分测试工作 |
| 模型/动画 | animation     | 优化     | 80%     | 支持初步的粒子系统，考虑基于计算着色器 | |
| 模型/动画 | animation     | 优化     |         | 支持PhysX引擎的刚体，步行漫游，驾驶 | |
| 脚本      | script       | 新增     | 80%     | 使用Serialization映射OSG和osgVerse的核心函数 | OSG函数已经全部映射完成 |
| 脚本      | script       | 新增     |         | 考虑SWIG的方式接入C#和Python等脚本语言 | |
| 例程      | ai           | 新增     |         | 编写测试程序验证TrellisPipeline，演示基于AI的3D模型生成 | 对应ai/TrellisPipeline.h |
| 例程      | ai           | 新增     |         | 编写测试程序验证ObjectTracker，演示AI推理输出的多目标跟踪 | 对应ai/Utilities.h |
| 例程      | animation    | 新增     |         | 编写测试程序验证VectorSmoother和TimeOut，演示数据滤波和时间控制 | 对应animation/Utilities.h |
| 例程      | modeling     | 新增     |         | 编写测试程序验证FFDModeler和BSplineVolume，演示自由变形 | 对应modeling/FFDModeler.h |
| 例程      | modeling     | 新增     |         | 编写测试程序验证MeshDeformer，演示网格变形/蒙皮 | 对应modeling/MeshDeformer.h |
| 例程      | modeling     | 新增     |         | 编写测试程序验证AnnotationCluster，演示标注聚类显示 | 对应modeling/AnnotationCluster.h |
| 例程      | modeling     | 新增     |         | 编写测试程序验证MathExpression，演示数学表达式解析求值 | 对应modeling/Math.h |
| 例程      | modeling     | 新增     |         | 编写测试程序验证PointCloudQuery，演示点云查询 | 对应modeling/Math.h |
| 例程      | modeling     | 新增     |         | 编写测试程序验证DynamicPointLine/Polyline/Polygon，演示动态线/面绘制 | 对应modeling/DynamicGeometry.h |
| 例程      | modeling     | 新增     |         | 重新编写TextureMapping和ConvexHull的测试程序 | 原测试已废弃(deprecated) |
| 例程      | pipeline     | 新增     |         | 编写测试程序验证NISUpscaler，演示NVIDIA图像上采样 | 对应pipeline/NISUpscaler.h |
| 例程      | pipeline     | 新增     |         | 编写测试程序验证TsfFramework，演示Windows TSF文本输入法 | 对应pipeline/TsfFramework.h |
| 例程      | pipeline     | 新增     |         | 编写测试程序验证MultiEffectNode，演示多效果节点 | 对应pipeline/MultiEffectNode.h |
| 例程      | readerwriter | 新增     |         | 编写测试程序验证GraphicsWindowWin32NV窗口实现 | 对应readerwriter/GraphicsWindowWin32NV.h |
| 例程      | readerwriter | 新增     |         | 编写测试程序验证FeatureDefinition和EncodedFrameObject | 对应readerwriter/FeatureDefinition.h |
| 例程      | ui           | 新增     |         | 编写测试程序验证SpiderEditor，演示节点图编辑器 | 对应ui/ImGuiComponents.h |
| 例程      | ui           | 新增     |         | 编写测试程序验证Timeline，演示时间轴控件 | 对应ui/ImGuiComponents.h |
| 例程      | ui           | 新增     |         | 编写测试程序验证VirtualKeyboard和FileDialog | 对应ui/ImGuiComponents.h |
| 例程      | ui           | 新增     |         | 编写测试程序验证常用UI组件(Label/Button/CheckBox/ComboBox/InputField/Slider/ListView等) | |
| 例程      | ui           | 新增     |         | 编写测试程序验证ImGuiManager的3D/RTT模式(addToTexture等) | 对应ui/ImGui3D.cpp |
| 编译      |              | 新增     | 100%     | 支持backward-cpp自动打印崩溃时的程序堆栈 | |
| 编译      |              | 新增     | 100%     | 支持Static编译流程 | |
| 编译      |              | 新增     | 100%     | 支持MinGW编译流程 | |
| 编译      |              | 除虫     | 100%     | 解决ept插件和laszip的Ubuntu编译问题 | |
| 编译/渲染 |              | 新增     | 100%      | 支持GL3 Core Profile | |
| 编译/渲染 |              | 新增     | 100%      | 支持GLES2 / GLES3 | |
| 编译/渲染 |              | 新增     | 100%      | 支持Angel并通过自定义的方式来切换不同底层(DX/Vulkan) | |
| 编译/渲染 |              | 新增     | 100%      | 支持GLSL 1.2，可以运行在虚拟机和低端国产显卡上 | |
| 编译      |              | 新增     | 100%     | 支持摩尔线程，景嘉微，Quadro，树莓派，RISC-V的编译和测试运行 | |
| 编译      |              | 新增     | 100%     | 支持Emscripten / WebAssembly编译流程，可以输出到浏览器端 | |
| 编译      |              | 新增     | 100%     | 通过Emscripten支持WebGL1/2接口 | |
| 编译      |              | 新增     | 100%     | 支持流媒体推流和拉流，支持WebRTC云渲染 | |
| 渲染      | pipeline     | 优化     | 100%     | 不要用NodeMask来管理Deferred场景，避免影响用户代码 | |
| 渲染      | pipeline     | 除虫     | 100%     | 帧速率较低时，会明显感受到Deferred场景比Forward慢一拍 | |
| 渲染      | pipeline     | 除虫     | 100%     | SkyBox对于大坐标场景显示错误，并且被裁切 | |
| 渲染      | pipeline     | 除虫     | 100%     | SkyBox用tex2d纹理时，会有一条明显的接缝边界线 | |
| 渲染      | pipeline     | 除虫     | 100%     | NodeSelector会被物体遮挡 | |
| 渲染      | pipeline     | 优化     | 100%     | ShadowModule支持PSM/LispSM/TSM来提升阴影质量 | |
| 渲染      | pipeline     | 优化     | 100%     | ShadowModule支持EVSM/PCSS软阴影 | |
| 渲染      | pipeline     | 优化     | 100%     | 需要明确贴图metallic和roughness是如何表达的，软件如何导出 | |
| 渲染      | pipeline     | 除虫     | 100%     | 解决Sponza法线贴图不能共享以及matallic闪烁的问题 | |
| 渲染      | pipeline     | 新增     | 100%     | 考虑初步支持OpenXR标准的VR设备渲染 | |
| 渲染      | pipeline     | 除虫     | 100%     | 全屏/窗口切换或者缩放窗口大小后，多层阴影显示不正确 | |
| 渲染      | pipeline     | 除虫     | 100%     | 加入自定义管线后，全屏/窗口切换，阴影和场景显示错误 | |
| 渲染      | pipeline     | 新增     | 100%     | 支持非MRT的流水线，且GBuffer Pass可以支持PBR透明体和前向光照 | |
| 渲染      | pipeline     | 优化     | 100%     | 在PBR Lighting过程中通过LightManager光源数据表计算多种光照结果 | |
| 渲染      | pipeline     | 除虫     | 100%     | 发现OSG 311版本中渲染报错，和Texture2DArray有关 | |
| 渲染      | pipeline     | 除虫     | 100%     | 发现311版本中缩放窗口后多层阴影的问题，因为没有实现resize() | |
| 渲染      | pipeline     | 除虫     | 100%     | 发现摩尔S50/S80显卡上延迟和非延迟场景的深度融合错误 | |
| 渲染      | pipeline     | 优化     | 100%     | 后处理Bloom的模糊处理方法不理想，目前可能会产生锯齿 | |
| 渲染      | pipeline     | 新增     | 100%     | 通过脚本的方式来管理标准和自定义的Pipeline | |
| 插件      | plugins      | 新增     | 100%     | 支持伪插件方式自动替换PBR贴图顺序(?.D4,S3,N3,X1M1R1.pbrlayout) | |
| 插件      | plugins      | 新增     | 100%     | 使用libhv支持多种网络协议，替代curl插件 | |
| 插件      | plugins      | 新增     | 100%     | 使用leveldb支持数据库读写操作，可以用数据库来存储osgb文件 | |
| 辅助工具  | helpers      | 新增     | 100%     | 实现Unity的导出插件第一版(静态模型，PBR材质) | |
| 数据读写  | readerwriter | 新增     | 100%     | 自动检查输入几何体的正确性，尝试优化输入的几何数据 | |
| 数据读写  | readerwriter | 新增     | 100%     | 支持KTX纹理格式的读取和写入，支持纹理压缩功能 | |
| 数据读写  | readerwriter | 新增     | 100%     | 支持默认常见图片格式的读取，不需要额外插件，主要用于WASM等场合 | |
| 数据读写  | readerwriter | 新增     | 100%     | 支持倾斜摄影模型的相邻层合并，顶层合并，3dtiles读取 | |
| 模型/动画 | animation    | 优化     | 100%     | PlayerAnimation支持直接输入角色骨骼和动画数据 | |
| 模型/动画 | animation    | 优化     | 100%     | PlayerAnimation支持外部数据驱动 | |
| 模型/动画 | animation    | 优化     | 100%     | 支持EaseMotion，路径动画，DoTween形式的纤程动画 | 已实现TweenAnimation，待测试 |
|<img width=40/>|         |<img width=40/>|<img width=40/>| |
