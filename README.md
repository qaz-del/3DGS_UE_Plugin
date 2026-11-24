# Gaussian Point Splatting for Unreal Engine

基于 Unreal Engine RDG Compute Shader 的 3D Gaussian Splatting 插件，提供普通高斯渲染和参考 *Gaussian Point Splatting* 论文的随机点采样渲染，并支持 UE 场景深度遮挡、屏幕拾取和 2×2 超采样。

本仓库以本地 `GS_Plugin` 文件夹为根目录，采用**插件源码分发**。发布内容位于 `Plugins/GaussianPointSplatting`，不包含大型测试数据、导入后的高斯资产、预编译 DLL 或完整测试工程。下载数据后，在自己的 UE 工程中重新导入即可使用。

## 环境要求

- Unreal Engine **5.8**，Windows / Win64。
- DirectX 12、Shader Model 6，以及支持 **64 位纹理原子操作**的 GPU。当前深度和 ID 实现在两个渲染模式下均需要这些能力。
- 与 UE 版本匹配的 Visual Studio C++ 工具链和 Windows SDK。
- 建议使用 C++ 工程编译插件；不需要安装 CUDA Toolkit。

插件使用了 UE Renderer 内部接口，其他引擎版本需要自行适配，不能直接假定二进制兼容。

## 已实现功能

- 导入训练后的 `binary_little_endian` Gaussian PLY，生成 `UGaussianPointSplatAsset`；支持导入数量限制、空间选区和分块输出。
- 通过 `AGaussianPointSplatActor` / `UGaussianPointSplatComponent` 放置和变换高斯场景。
- **Mode 0**：解析高斯覆盖与加权颜色合成。
- **Mode 1**：Poisson 点数采样、采样密度修正、GPU 工作队列与间接派发、64 位原子最近点竞争。
- 持久化 GPU 压缩属性、独立空间索引及视锥裁剪。
- 两模式均支持 UE SceneDepth 测试，输出高斯深度、稳定源索引和覆盖率，并按像素深度合成多个组件。
- 按需异步 GPU 屏幕拾取，提供控制台命令及蓝图异步节点。
- 可切换 **1×1 / 2×2** 内部采样，以及颜色、深度、ID、覆盖率调试视图。
- GPU pass 计时标记，可使用 `stat gpu`、`profilegpu` 和 CSV Profiler 采集数据。

## 部署到自己的 UE 工程

1. 新建或打开 UE 5.8 C++ 工程，关闭编辑器。
2. 将本仓库的整个 `Plugins/GaussianPointSplatting` 文件夹复制到目标工程的 `Plugins` 文件夹中：

   ```text
   YourProject/
   ├── YourProject.uproject
   └── Plugins/
       └── GaussianPointSplatting/
           ├── GaussianPointSplatting.uplugin
           ├── Config/
           ├── Source/
           ├── Shaders/
           └── THIRD_PARTY_NOTICES.md
   ```

3. 重新生成项目文件，使用 **Development Editor / Win64** 完整编译。插件新增或修改反射类型时应关闭编辑器后编译。
4. 打开目标工程，在 Plugins 中确认 **Gaussian Point Splatting** 已启用；按提示重启。
5. 在 Project Settings 中使用 **DirectX 12**，启用 **D3D12 SM6**，等待着色器编译完成。

无需复制本地开发工程的 `GS_Plugin.uproject` 或其配置；`ModelContextProtocol`、`Terminal`、`EditorToolset` 等开发工具不属于插件运行依赖。

## 测试数据下载与导入

场景来源：

- [St. Sebastian church (30 M Gaussians)](https://www.youtube.com/watch?v=bMXM9ep6Y5I)

以上链接为 YouTube 场景介绍页面。**PLY 数据文件的直接下载地址：待补充。**

<!-- 找到 PLY 数据下载链接后填在此处，并补充文件版本及复现实验采用的导入参数。 -->

请使用包含位置、缩放、旋转、不透明度和颜色系数的训练后 Gaussian PLY，而不是只含 XYZ 的普通点云。当前导入器支持 `binary_little_endian`，不支持 ASCII PLY。数据单独下载，不提交到 Git 仓库。

初次运行可先导入少量高斯。在 UE 控制台中设置以下参数，**再执行导入**：

```text
gps.Import.MaxGaussians 1000000
gps.Import.Stride 1
gps.Import.SampleMode 0
gps.Import.OutputChunkSize 500000
```

`MaxGaussians` 限制导入数量；`0` 表示不设数量上限。`OutputChunkSize` 控制每个生成资产的高斯数量。`SampleMode 0` 为顺序模式，`1` 为空间选区模式；空间模式会选择部分区域，并可能过滤过大的背景高斯，结果不等同于完整场景。

在 Content Browser 中导入 `.ply`，建议将生成资产放在 `/Game/GaussianData/`。导入器会生成一个根资产及若干 `_Chunk...` 子资产，保存时需要保留整组资产。

在关卡中放置 **Gaussian Point Splat Actor**，或给 Actor 添加 **Gaussian Point Splat Component**。将组件的 `Asset` 指向**根资产**，调整 `SourceUniformScale`、`SourceRotation`、`SourceOffset` 以匹配 UE 场景。需要视锥裁剪时，点击组件上的 **Create or Refresh Spatial Index**，然后保存生成的空间索引。

如需 800 万高斯测试，导入前设置 `gps.Import.MaxGaussians 8000000`，并将关卡中组件实例的 `MaxRenderGaussians` 设置为 `8000000`。源数据不足或导入筛选会导致实际数量低于上限，具体以日志为准。修改导入参数不会自动缩小已经生成的资产，需要重新导入。

## 常用命令

控制台命令每行单独执行：

```text
gps.Render.Mode 0
gps.Render.Mode 1
gps.Render.UseSceneDepthTest 1
gps.Render.Supersampling 1
gps.Render.Supersampling 2
```

`Mode 0` 为普通高斯，`Mode 1` 为论文点采样；不支持 Mode 2。超采样默认是 1×1，设置为 `2` 后使用四个子像素，输出画面分辨率不变。

| 命令 | 用途 |
| --- | --- |
| `gps.Debug.Output 0` | 正常颜色 |
| `gps.Debug.Output 1` | 高斯代表深度 |
| `gps.Debug.Output 2` | ID 伪彩色 |
| `gps.Debug.Output 3` | 覆盖率 |
| `gps.Pick` | 在 PIE / Standalone 中拾取屏幕中心 |
| `gps.Pick 0.25 0.5` | 使用本地玩家视口的归一化 UV 拾取 |
| `gps.Paper.MaxWorkItems 16000000` | 论文模式的采样工作预算 |
| `gps.Paper.AnimateSeed 0` | 固定随机种子 |

蓝图可使用 **Pick Gaussian at Screen UV** 异步节点，返回组件、源资产、样本索引、视空间深度、覆盖率和高斯中心位置。完整验证流程见 [Testing.md](Plugins/GaussianPointSplatting/Testing.md)。

## 大数据与 `.uasset` 的处理

导入后的 `.uasset` 也是高斯数据，不是插件源码，**同样不上传**。根资产、分块资产和空间索引都可由 PLY 在本地重新生成。本地测试地图和蓝图可能引用这些资产，因此本仓库也不分发现有测试工程的 Content。

磁盘占用、CPU 内存和 GPU 显存是三个不同指标。当前 GPU 压缩属性约 21 字节/高斯，加上源 ID 映射约 25 字节/高斯；这**不会压缩现有 `.uasset` 文件或 UObject 内保存的样本数组**。例如 800 万高斯的这部分静态 GPU 数据约 200 MB，另有投影缓存、采样队列、纹理与 UE 场景资源。

如果希望减小本地资产和加载内存，应在导入前降低 `gps.Import.MaxGaussians`，而不只是降低组件的 `MaxRenderGaussians`。后者主要限制渲染数量，不会缩小已经导入的资产。分块会减小单文件大小，但不会自动降低总数据量；根资产对子块的引用也不等于按需流式加载。

替换为较小数据集后，通过 UE 检查引用并删除不再使用的整组旧高斯资产及空间索引；不要只手动删除部分 `_Chunk` 文件。`.gitignore` 只防止提交，不会释放本地磁盘或运行内存。

## 上传 GitHub 的内容

以 `GS_Plugin` 为 Git 仓库根目录，上传以下内容：

```text
README.md
.gitignore
Plugins/GaussianPointSplatting/
├── GaussianPointSplatting.uplugin
├── Config/
├── Source/
├── Shaders/
├── THIRD_PARTY_NOTICES.md
└── 功能与测试说明文档
```

根目录 `.gitignore` 已按这种“只发布插件”的方式配置：保留 README、插件源码、着色器及必要插件配置，排除本地宿主工程的 `GS_Plugin.uproject`、`Source/`、`Config/`、`Content/`，以及所有 `.uasset` / `.umap`、PLY 数据、编译产物、缓存、IDE 文件与性能捕获。插件自身的 `Plugins/GaussianPointSplatting/Source/` 和 `Config/` 会保留。

本地宿主工程配置中有开发工具插件依赖、测试地图引用和本机配置，因此不作为发布入口。使用者按前述部署步骤，在自己的 UE 工程里编译插件。

建议通过 Git 或 GitHub Desktop 提交。GitHub 网页直接拖拽文件不会按 `.gitignore` 自动筛除本地大文件；不要把磁盘上的整个文件夹直接打包后上传。

在仓库初始化后，可以显式暂存源码：

```bash
git add README.md .gitignore
git add Plugins/GaussianPointSplatting
git status --short
git diff --cached --stat
```

`.gitignore` 不影响已经被 Git 跟踪的文件。如果以前已经暂存大资产，需要先取消这些文件的跟踪；如果已写入提交历史，仅新增一次删除提交不会清除历史中的大文件。

## 实现边界与参考资料

当前颜色使用 DC / SH0，尚未保留和计算高阶 SH；没有时域累积、移动相机重投影或高斯自身遮挡剔除。Mode 0 的加权合成及跨组件代表深度排序仍是透明层近似，不是标准逐高斯排序的完整 3DGS 实现。

高斯深度是渲染/交互用的代表深度。当前通过 AfterDOF 合成并读取 UE SceneDepth，不写回 UE 主深度，也不提供物理碰撞、网格或阴影。随机点采样图像可能存在噪声，2×2 会增加采样与显存开销；本仓库不承诺固定帧率或论文中的大场景性能。

- [Gaussian Point Splatting, ACM TOG 2026](https://doi.org/10.1145/3811272)
- [渲染实现说明](Plugins/GaussianPointSplatting/PaperRenderer.md)
- [空间索引](Plugins/GaussianPointSplatting/SpatialIndex.md)
- [GPU 数据压缩](Plugins/GaussianPointSplatting/GaussianCompression.md)
- [上游参考实现版权声明](Plugins/GaussianPointSplatting/THIRD_PARTY_NOTICES.md)
