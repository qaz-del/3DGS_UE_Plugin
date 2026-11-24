# 手动验证：Mode 0 / Mode 1、深度、拾取、2×2

状态：源码已修改，尚未进行 Unreal 编译、着色器编译或运行验证。本文中的结果均为验收预期，不是实测结论。测试由用户操作；本次没有性能数值。工程关联 UE 5.8、DX12、SM6。

本文件的 `GS_PluginEditor`、`TestLvl` 和绝对路径是本地开发工程示例；源码仓库不分发该工程的 Content。部署到自己的工程后，请替换工程名称、路径和测试地图，再执行相同步骤。

## 1. 完整编译并确认模式

关闭 UE 编辑器。此次新增了反射类型，不使用 Live Coding。PowerShell 执行：

```powershell
& 'D:\software\UE_5.8\Engine\Build\BatchFiles\Build.bat' GS_PluginEditor Win64 Development '-Project=D:\UnrealProject\Guassian\GS_Plugin\GS_Plugin.uproject' -WaitMutex -NoHotReloadFromIDE
```

编译成功后重新打开工程，等待着色器编译结束。若编译失败，保留第一条 C++、UHT 或 Shader 错误；不要把后续连锁错误当根因。

模式固定为 `gps.Render.Mode 0`（普通解析高斯）、`gps.Render.Mode 1`（论文点采样）。2 仍不受支持。此次两种模式的深度/ID 路径都要求 SM6 与 64 位纹理原子操作；不支持时记录错误并跳过高斯视图，不静默给出错误深度。

先只使用一个高斯组件，地图使用 TestLvl。在**场景中的组件实例**上设置 `MaxRenderGaussians = 8000000`，不要只改蓝图默认值。固定 GaussianScaleMultiplier、源坐标变换、空间索引及质量参数。

依次输入（每行一条命令）：

```text
gps.Render.Supersampling 1
gps.Debug.Output 0
gps.Render.UseSceneDepthTest 0
gps.Render.UseHZBOcclusion 0
gps.Paper.AnimateSeed 0
gps.Paper.MaxWorkItems 16000000
gps.Paper.MaxPointsPerGaussian 128
gps.Debug.LogDispatch 1
gps.Render.Mode 0
```

日志应有 `GPS Frame: ... RenderGaussians=8000000 SampleGrid=1x1`。这是组件选择的总数；视锥裁剪后的处理数另见 GPU pass 名称。切换 `gps.Render.Mode 1` 应看到 `Paper renderer active`，GPU Visualizer 出现 `GPS Paper ...`。确认后执行 `gps.Debug.LogDispatch 0`，防止逐帧日志影响计时。

## 2. 采集当前版本的 0/1 基线

使用 Standalone Game 或固定分辨率的 PIE 独立窗口（建议 1920×1080）。记录运行方式、UE 版本、GPU、驱动、窗口实际尺寸和屏幕百分比。两种模式保持一致。

在 Sequencer 中建一条约 20 秒的相机路径，至少经过远景、近处立面、屏幕边缘三种位置；添加 Camera Cuts 并固定相机 FOV。两种模式重放**同一条序列**，不能手动飞两次代替相同路径。先预热两圈，再从相同起始帧采集一整圈，各重复三次。

```text
t.MaxFPS 0
r.VSync 0
r.ScreenPercentage 100
r.GPUCsvStatsEnabled 1
stat unit
stat gpu
stat RHI
csvprofile start
```

重放一整圈后：

```text
csvprofile stop
```

CSV 文件在项目 `Saved/Profiling/CSV` 下。分别保存 Mode 0 和 Mode 1 的文件，计算 Frame、Game、Render/Draw、GPU 的中位数与 P95，单位 ms。当前新增 pass 使用 UE 5.8 的 `RDG_EVENT_SCOPE_STAT`，CSV 中查找 GPU/GPS 项；不要把这些 GPU 项与 CPU 线程耗时相加。

在三个固定相机位置分别执行 `profilegpu`，保存 GPU Visualizer 的 pass 时间。**profilegpu 产生的暂停不纳入 CSV 的正常帧采样**。

| 路径 | 需要记录的 pass |
| --- | --- |
| Mode 0 | Analytic Clear / Splat / Resolve、GPS Frame Composite |
| Mode 1 | Paper Preprocess / Budget / Compact / Dispatch Args / Clear / Point Splat / Resolve、GPS Frame Composite |

显存先用 `stat RHI` 作概览；需要资源级占用时用 RenderDoc/PIX 捕获相同三个视点，另用 GPU 内存采样记录整段路径的峰值。区分全设备使用量、进程使用量和某个捕获帧的资源占用，不把单帧截图称为整段峰值。

在 Mode 1 捕获中读取 `GPS.Paper.RawTotal` 和 `GPS.Paper.WorkTotal`：前者是每高斯点数上限处理后、全局预算处理前的点数；实际输出最多为 `min(WorkTotal, MaxWorkItems)`。RawTotal 超过预算时会随机削减，必须和时间、画质一起记录。

修改前的 Source/Shaders 已保存在工作区 `Artifacts/GPS_Baseline_20261123`，但没有采到修改前的运行数据。上述采集是**当前版本两模式基线**；如需前后回归，在独立工程副本使用备份源码和相同地图/配置比较，避免覆盖当前实现。

## 3. UE SceneDepth 遮挡（两个模式都测）

在镜头与高斯建筑之间放一个 UE Cube，使用 Opaque 材质，覆盖画面一部分。在建筑后面再放一个 Cube。测试不使用透明材质。

```text
gps.Render.UseSceneDepthTest 0
gps.Render.UseSceneDepthTest 1
```

关闭时保留原来的高斯覆盖 UE 物体行为；开启时，前方 Cube 应遮挡后方高斯，高斯应遮挡后方 Cube 的对应覆盖区域，其他区域仍可见。空白/天空处不应导致全场消失。

先 Mode 0，再 Mode 1。分别在 `r.ScreenPercentage 50`、`100` 下重复，并改变编辑器视口大小；遮挡边界应与 Cube 对齐。高斯深度使用视空间 Z（厘米），比较前会转换 UE 的 DeviceZ。可用 `gps.Render.SceneDepthBiasCm 0` 和 `2` 检查接触处偏差；该偏差不应导致整幅画面平移。

## 4. 深度、ID、覆盖率输出

```text
gps.Debug.Output 1
gps.Debug.Output 2
gps.Debug.Output 3
gps.Debug.Output 0
```

1=深度（近亮远暗，空白黑），2=ID 伪彩色，3=覆盖率，0=正常颜色。Mode 1 的 1×1 覆盖率是单次随机可见性，不是去噪后的透明度。

RenderDoc 中可检查原始 RDG 纹理：

| 纹理 | 格式与含义 |
| --- | --- |
| GPS.Depth | R32_FLOAT，代表高斯中心的正视空间深度，cm；无命中为 0 |
| GPS.ID | R32G32_UINT：当前会话组件 ID、原始资产平铺索引；无命中为 `{0, 0xffffffff}` |
| GPS.Coverage | R32_FLOAT，0..1，2×2 时四个子像素平均覆盖率 |
| GPS.Layers.Color / Hit | 子像素分辨率、每组件一层，供跨组件合成 |

`gps.Render.DepthAlphaThreshold` 默认 0.05，只控制代表深度/ID 所需的可见贡献，不改变颜色。多个子像素都有命中时取最近的合格深度及其 ID，不平均深度或 ID。

输出目前是 RDG 纹理与拾取接口，不是可直接拖入材质的 UTextureRenderTarget，也不写回 UE 主 SceneDepth。

## 5. 跨组件遮挡

先降低每个组件的 MaxRenderGaussians（例如各 100 万）完成功能检查，再恢复目标数据量。复制高斯 Actor，沿相机方向错开，使两者局部重叠、前后关系明确。

两个模式分别检查：前方组件应覆盖后方组件的对应区域；隐藏前方组件后，后方组件显露；移动组件到相反前后位置后，遮挡关系应交换。隐藏/显示会重新注册组件，结果不能随注册顺序反转。PIE 中移动 Actor 时，图像和拾取中心应跟随移动。

Mode 1 在每个子像素使用最近点，因此跨组件最近点竞争完整保留。Mode 0 保留组件内部的加权混合，组件之间按每像素代表深度从前到后合成；多个组件内部高斯彼此交错的透明层仍是近似，不能作为逐高斯排序的精确结果验收。

## 6. 屏幕拾取：先用控制台

进入 PIE/Standalone，让屏幕中心对准一个覆盖充分的建筑表面，输入：

```text
gps.Pick
gps.Pick 0.25 0.5
```

参数是当前本地玩家受约束视口的归一化 UV，左上 0,0，右下趋近 1,1。默认中心 0.5,0.5；1.0 不属于有效像素范围。普通编辑器透视视口没有本地 PlayerController，不能执行此测试。

日志应有 `GPS Pick: Hit=1 Component=... Asset=... Sample=... FlatID=... DepthCm=... Coverage=...`。黄色小球标记选中高斯的**中心**，不是鼠标射线的物理表面交点。未覆盖的像素应为 Hit=0；Mode 1 单次随机采样的空洞也可能不命中。

对同一个静止相机/像素重复拾取，固定随机种子时结果应稳定。遮挡 Cube 前的像素不应拾取到 Cube 后的高斯。跨组件重叠处应拾取近处可见组件。不同模式/分辨率选到的邻近高斯可能不同，稳定 ID 指向的是源数据身份，不保证所有渲染设置都选择同一个高斯。

蓝图可搜索异步节点 **Pick Gaussian at Screen UV**。输入 Get Player Controller、UV（先用 0.5,0.5）、MinCoverage=0.25；在 Completed 分支读取 Result。Component + SourceAsset + SampleIndex 用于后续交互。无立体渲染支持；分屏使用对应本地玩家。鼠标 UV 要扣除分屏/黑边后的视口偏移，不能直接使用带 UMG DPI 缩放的坐标。

GPU 查询按需回读 16 字节，结果异步返回。组件销毁、资产重新导入或源资产变化时应安全返回失败；没有匹配游戏视图时约 5 秒超时，Error 非空。组件数字 ID 仅在当前会话有效；SourceAsset + SampleIndex 在资产版本不变时保持身份。

## 7. 2×2 超采样

先恢复单组件、同一相机和正常颜色：

```text
gps.Debug.Output 0
gps.Render.Mode 1
gps.Render.Supersampling 1
gps.Render.Supersampling 2
```

2×2 是宽高各两倍的内部采样图，viewport 尺寸保持不变。建筑尺寸与遮挡位置应一致；四个子像素分别深度测试/竞争，颜色合成后噪声与边缘锯齿通常减少。切换 1→2→1 不应崩溃、残留旧 ID 或改变投影位置。Mode 0 也重复测试。

默认工作项总预算仍为 1600 万，不会随超采样自动改变。Mode 1 每高斯点数上限按采样面积从 128 扩大到 512；工作项改为 uint2，避免点序号挤占高斯 ID 位。若 RawTotal 超过预算，先记录削减程度；显存允许时另测：

```text
gps.Paper.MaxWorkItems 64000000
```

该队列为 8 字节/项，1600 万约 128 MB，6400 万约 512 MB，另有其他显存资源。将不同预算的结果分开记录，不能直接称为同预算对比。

在 2×2 下重复 SceneDepth、ID、跨组件、gps.Pick 测试，并重放基线相机路径采集性能。记录四组：Mode 0/1 各自 1×1/2×2。2×2 不保证达到某个固定加速比；本次没有时域累积，静止图像也不会逐帧收敛。

测试结束可恢复：

```text
gps.Debug.Output 0
gps.Debug.LogDispatch 0
gps.Render.Supersampling 1
gps.Render.UseSceneDepthTest 1
gps.Render.UseHZBOcclusion 0
gps.Paper.AnimateSeed 0
gps.Paper.MaxWorkItems 16000000
gps.Render.Mode 1
```
