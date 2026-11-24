#include "GaussianPointSplatPaperRenderer.h"

#include "GaussianPointSplatSceneProxy.h"
#include "GlobalShader.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "HAL/IConsoleManager.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHI.h"
#include "SceneView.h"
#include "ScreenPass.h"
#include "ShaderParameterStruct.h"
#include "SystemTextures.h"

DECLARE_GPU_STAT_NAMED(GPSPaper1, TEXT("GPS Paper Preprocess"));
DECLARE_GPU_STAT_NAMED(GPSPaper2, TEXT("GPS Paper Budget"));
DECLARE_GPU_STAT_NAMED(GPSPaper3, TEXT("GPS Paper Compact"));
DECLARE_GPU_STAT_NAMED(GPSPaper4, TEXT("GPS Paper Dispatch Args"));
DECLARE_GPU_STAT_NAMED(GPSPaper5, TEXT("GPS Paper Clear"));
DECLARE_GPU_STAT_NAMED(GPSPaper6, TEXT("GPS Paper Point Splat"));
DECLARE_GPU_STAT_NAMED(GPSPaper7, TEXT("GPS Paper Resolve"));

namespace
{
	TAutoConsoleVariable<int32> CVarGPSPaperMaxWorkItems(
		TEXT("gps.Paper.MaxWorkItems"),
		16000000,
		TEXT("Maximum compact point work items for the paper renderer. Excess work is thinned on the GPU."));

	TAutoConsoleVariable<int32> CVarGPSPaperMaxPointsPerGaussian(
		TEXT("gps.Paper.MaxPointsPerGaussian"),
		128,
		TEXT("Per-Gaussian point cap at 1x1 (1..128), multiplied by the sample-grid area at 2x2."));

	TAutoConsoleVariable<int32> CVarGPSPaperAnimateSeed(
		TEXT("gps.Paper.AnimateSeed"),
		0,
		TEXT("Advance the paper renderer random seed each frame. Default 0 keeps a static view stable until temporal accumulation is available."));

	constexpr uint32 GaussianGroupSize = 256;
	constexpr uint32 PixelGroupSize = 64;
	constexpr uint32 MaxDispatchGroups = 65535;

	struct FPaperDispatch
	{
		FIntVector Groups = FIntVector(1, 1, 1);
		uint32 Stride = 1;
	};

	FPaperDispatch MakeDispatch(uint64 ItemCount, uint32 GroupSize)
	{
		FPaperDispatch Result;
		const uint64 TotalGroups = FMath::Max<uint64>(FMath::DivideAndRoundUp(ItemCount, static_cast<uint64>(GroupSize)), 1);
		const uint32 GroupsX = static_cast<uint32>(FMath::Min<uint64>(TotalGroups, MaxDispatchGroups));
		const uint64 GroupsY = FMath::DivideAndRoundUp(TotalGroups, static_cast<uint64>(GroupsX));
		check(GroupsY <= MaxDispatchGroups);
		Result.Groups = FIntVector(GroupsX, static_cast<int32>(GroupsY), 1);
		Result.Stride = GroupsX * GroupSize;
		return Result;
	}
}

class FGPSPaperPreprocessCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGPSPaperPreprocessCS);
	SHADER_USE_PARAMETER_STRUCT(FGPSPaperPreprocessCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, GaussianCount)
		SHADER_PARAMETER(uint32, GaussianThreadStride)
		SHADER_PARAMETER(uint32, UseVisibleIndices)
		SHADER_PARAMETER(uint32, FrameSeed)
		SHADER_PARAMETER(uint32, MaxPointsPerGaussian)
		SHADER_PARAMETER(float, GaussianScaleMultiplier)
		SHADER_PARAMETER(float, MinClipW)
		SHADER_PARAMETER(float, Supersampling)
		SHADER_PARAMETER(FMatrix44f, LocalToView)
		SHADER_PARAMETER(FVector2f, ViewRectMin)
		SHADER_PARAMETER(FVector2f, ViewRectSize)
		SHADER_PARAMETER(FMatrix44f, LocalToClip)
		SHADER_PARAMETER(FVector4f, GPSPositionMin)
		SHADER_PARAMETER(FVector4f, GPSPositionExtent)
		SHADER_PARAMETER(float, GPSScaleLogMin)
		SHADER_PARAMETER(float, GPSScaleLogExtent)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, GPSPackedGeometry)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GPSPackedColors)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GPSPackedOpacities)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VisibleIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, Projected0)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, Projected1)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RawCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RawTotal)
	END_SHADER_PARAMETER_STRUCT()
public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}
};

class FGPSPaperScaleCountsCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGPSPaperScaleCountsCS);
	SHADER_USE_PARAMETER_STRUCT(FGPSPaperScaleCountsCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, GaussianCount)
		SHADER_PARAMETER(uint32, GaussianThreadStride)
		SHADER_PARAMETER(uint32, FrameSeed)
		SHADER_PARAMETER(uint32, MaxWorkItems)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ReadRawCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ReadRawTotal)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, FinalCounts)
	END_SHADER_PARAMETER_STRUCT()
public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}
};

class FGPSPaperCompactCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGPSPaperCompactCS);
	SHADER_USE_PARAMETER_STRUCT(FGPSPaperCompactCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, GaussianCount)
		SHADER_PARAMETER(uint32, GaussianThreadStride)
		SHADER_PARAMETER(uint32, MaxWorkItems)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ReadFinalCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, WorkTotal)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, WorkIndices)
	END_SHADER_PARAMETER_STRUCT()
public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}
};

class FGPSPaperBuildArgsCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGPSPaperBuildArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FGPSPaperBuildArgsCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, MaxWorkItems)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ReadWorkTotal)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()
public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}
};

class FGPSPaperClearCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGPSPaperClearCS);
	SHADER_USE_PARAMETER_STRUCT(FGPSPaperClearCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, PixelThreadStride)
		SHADER_PARAMETER(FVector2f, OutputSize)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<UlongType>, PackedSamplesUAV)
	END_SHADER_PARAMETER_STRUCT()
public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}
};

class FGPSPaperSplatCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGPSPaperSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FGPSPaperSplatCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER(uint32, UseSceneDepthTest)
		SHADER_PARAMETER(float, SceneDepthBiasCm)
		SHADER_PARAMETER(FVector2f, SceneDepthViewMin)
		SHADER_PARAMETER(FVector2f, SceneDepthViewSize)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER(uint32, MaxWorkItems)
		SHADER_PARAMETER(uint32, UseVisibleIndices)
		SHADER_PARAMETER(uint32, FrameSeed)
		SHADER_PARAMETER(FVector2f, ViewRectMin)
		SHADER_PARAMETER(FVector2f, ViewRectSize)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ReadWorkTotal)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, ReadWorkIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ReadProjected0)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ReadProjected1)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VisibleIndices)
		SHADER_PARAMETER(FVector4f, GPSPositionMin)
		SHADER_PARAMETER(FVector4f, GPSPositionExtent)
		SHADER_PARAMETER(float, GPSScaleLogMin)
		SHADER_PARAMETER(float, GPSScaleLogExtent)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, GPSPackedGeometry)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GPSPackedColors)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GPSPackedOpacities)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<UlongType>, PackedSamplesUAV)
		RDG_BUFFER_ACCESS(DispatchArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()
public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}
};

class FGPSPaperResolveCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGPSPaperResolveCS);
	SHADER_USE_PARAMETER_STRUCT(FGPSPaperResolveCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, PixelThreadStride)
		SHADER_PARAMETER(FVector2f, OutputSize)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<UlongType>, PackedSamplesSRV)
		SHADER_PARAMETER(uint32, LayerIndex)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SourceIDs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GPSPackedColors)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GPSPackedOpacities)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, LayerColor)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<uint2>, LayerHit)
	END_SHADER_PARAMETER_STRUCT()
public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}
};

IMPLEMENT_GLOBAL_SHADER(FGPSPaperPreprocessCS, "/Plugin/GaussianPointSplatting/Private/GPSPaperSplat.usf", "PaperPreprocessCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGPSPaperScaleCountsCS, "/Plugin/GaussianPointSplatting/Private/GPSPaperSplat.usf", "PaperScaleCountsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGPSPaperCompactCS, "/Plugin/GaussianPointSplatting/Private/GPSPaperSplat.usf", "PaperCompactCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGPSPaperBuildArgsCS, "/Plugin/GaussianPointSplatting/Private/GPSPaperSplat.usf", "PaperBuildArgsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGPSPaperClearCS, "/Plugin/GaussianPointSplatting/Private/GPSPaperSplat.usf", "PaperClearCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGPSPaperSplatCS, "/Plugin/GaussianPointSplatting/Private/GPSPaperSplat.usf", "PaperSplatCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGPSPaperResolveCS, "/Plugin/GaussianPointSplatting/Private/GPSPaperSplat.usf", "PaperResolveCS", SF_Compute);

bool FGaussianPointSplatPaperRenderer::IsSupported(const FSceneView& View)
{
	return View.GetFeatureLevel() >= ERHIFeatureLevel::SM6 && GRHISupportsAtomicUInt64 &&
		(GPixelFormats[PF_R64_UINT].Supported || GPixelFormats[PF_R32G32_UINT].Supported);
}

void FGaussianPointSplatPaperRenderer::AddPasses(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FScreenPassTexture& SceneColor,
	FGPSRenderFrame& Frame,
	FRDGTextureRef SceneDepthTexture,
	const FGaussianPointSplatProxyData& Data,
	FRDGBufferRef GeometryBuffer,
	FRDGBufferRef ColorBuffer,
	FRDGBufferRef OpacityBuffer,
	FRDGBufferRef VisibleIndexBuffer,
	uint32 NumGaussians,
	bool bUseVisibleIndices,
	FRDGBufferRef SourceIDBuffer)
{
	check(IsSupported(View));
	const uint32 MaxWorkItems = static_cast<uint32>(FMath::Clamp(CVarGPSPaperMaxWorkItems.GetValueOnRenderThread(), 1000000, 64000000));
	const uint32 MaxPointsPerGaussian = static_cast<uint32>(FMath::Clamp(CVarGPSPaperMaxPointsPerGaussian.GetValueOnRenderThread(), 1, 128)) * Frame.Supersampling * Frame.Supersampling;
	const uint32 FrameSeed = (CVarGPSPaperAnimateSeed.GetValueOnRenderThread() != 0 ? View.Family->FrameNumber : 0u) + 1337u;
	const FPaperDispatch GaussianDispatch = MakeDispatch(NumGaussians, GaussianGroupSize);
	const FIntPoint Extent = Frame.SampleExtent;
	const FPaperDispatch PixelDispatch = MakeDispatch(static_cast<uint64>(Extent.X) * Extent.Y, PixelGroupSize);
	const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());

	FRDGBufferRef Projected0 = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), NumGaussians), TEXT("GPS.Paper.Projected0"));
	FRDGBufferRef Projected1 = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), NumGaussians), TEXT("GPS.Paper.Projected1"));
	FRDGBufferRef RawCounts = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumGaussians), TEXT("GPS.Paper.RawCounts"));
	FRDGBufferRef FinalCounts = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumGaussians), TEXT("GPS.Paper.FinalCounts"));
	FRDGBufferRef RawTotal = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("GPS.Paper.RawTotal"));
	FRDGBufferRef WorkTotal = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("GPS.Paper.WorkTotal"));
	FRDGBufferRef WorkIndices = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(2 * sizeof(uint32), MaxWorkItems), TEXT("GPS.Paper.WorkIndices"));
	FRDGBufferRef IndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(), TEXT("GPS.Paper.IndirectArgs"));
	const EPixelFormat PackedFormat = GPixelFormats[PF_R64_UINT].Supported ? PF_R64_UINT : PF_R32G32_UINT;
	FRDGTextureDesc PackedDesc = FRDGTextureDesc::Create2D(Extent, PackedFormat, FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV | ETextureCreateFlags::Atomic64Compatible);
	PackedDesc.AliasableFormats.Add(PF_R32G32_UINT);
	FRDGTextureRef PackedSamples = GraphBuilder.CreateTexture(PackedDesc, TEXT("GPS.Paper.PackedSamples"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(RawTotal), 0);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(WorkTotal), 0);

	auto* Preprocess = GraphBuilder.AllocParameters<FGPSPaperPreprocessCS::FParameters>();
	Preprocess->GaussianCount = NumGaussians;
	Preprocess->GaussianThreadStride = GaussianDispatch.Stride;
	Preprocess->UseVisibleIndices = bUseVisibleIndices ? 1u : 0u;
	Preprocess->FrameSeed = FrameSeed;
	Preprocess->MaxPointsPerGaussian = MaxPointsPerGaussian;
	Preprocess->GaussianScaleMultiplier = Data.GaussianScaleMultiplier;
	Preprocess->Supersampling = static_cast<float>(Frame.Supersampling);
	Preprocess->LocalToView = Data.LocalToWorld * FMatrix44f(View.ViewMatrices.GetViewMatrix());
	Preprocess->MinClipW = FMath::Max(View.NearClippingDistance, 0.001f);
	Preprocess->ViewRectMin = FVector2f(SceneColor.ViewRect.Min) * static_cast<float>(Frame.Supersampling);
	Preprocess->ViewRectSize = FVector2f(SceneColor.ViewRect.Size()) * static_cast<float>(Frame.Supersampling);
	Preprocess->LocalToClip = Data.LocalToWorld * FMatrix44f(View.ViewMatrices.GetWorldToClip());
	Preprocess->GPSPositionMin = Data.PositionMin;
	Preprocess->GPSPositionExtent = Data.PositionExtent;
	Preprocess->GPSScaleLogMin = Data.ScaleLogMin;
	Preprocess->GPSScaleLogExtent = Data.ScaleLogExtent;
	Preprocess->GPSPackedGeometry = GraphBuilder.CreateSRV(GeometryBuffer);
	Preprocess->GPSPackedColors = GraphBuilder.CreateSRV(ColorBuffer);
	Preprocess->GPSPackedOpacities = GraphBuilder.CreateSRV(OpacityBuffer);
	Preprocess->VisibleIndices = GraphBuilder.CreateSRV(VisibleIndexBuffer);
	Preprocess->Projected0 = GraphBuilder.CreateUAV(Projected0);
	Preprocess->Projected1 = GraphBuilder.CreateUAV(Projected1);
	Preprocess->RawCounts = GraphBuilder.CreateUAV(RawCounts);
	Preprocess->RawTotal = GraphBuilder.CreateUAV(RawTotal);
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, GPSPaper1, "GPS Paper Preprocess");
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GPS Paper Preprocess (%u)", NumGaussians),
			TShaderMapRef<FGPSPaperPreprocessCS>(ShaderMap), Preprocess, GaussianDispatch.Groups);
	}

	auto* Scale = GraphBuilder.AllocParameters<FGPSPaperScaleCountsCS::FParameters>();
	Scale->GaussianCount = NumGaussians;
	Scale->GaussianThreadStride = GaussianDispatch.Stride;
	Scale->FrameSeed = FrameSeed;
	Scale->MaxWorkItems = MaxWorkItems;
	Scale->ReadRawCounts = GraphBuilder.CreateSRV(RawCounts);
	Scale->ReadRawTotal = GraphBuilder.CreateSRV(RawTotal);
	Scale->FinalCounts = GraphBuilder.CreateUAV(FinalCounts);
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, GPSPaper2, "GPS Paper Budget");
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GPS Paper Budget"),
			TShaderMapRef<FGPSPaperScaleCountsCS>(ShaderMap), Scale, GaussianDispatch.Groups);
	}

	auto* Compact = GraphBuilder.AllocParameters<FGPSPaperCompactCS::FParameters>();
	Compact->GaussianCount = NumGaussians;
	Compact->GaussianThreadStride = GaussianDispatch.Stride;
	Compact->MaxWorkItems = MaxWorkItems;
	Compact->ReadFinalCounts = GraphBuilder.CreateSRV(FinalCounts);
	Compact->WorkTotal = GraphBuilder.CreateUAV(WorkTotal);
	Compact->WorkIndices = GraphBuilder.CreateUAV(WorkIndices);
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, GPSPaper3, "GPS Paper Compact");
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GPS Paper Compact"),
			TShaderMapRef<FGPSPaperCompactCS>(ShaderMap), Compact, GaussianDispatch.Groups);
	}

	auto* Args = GraphBuilder.AllocParameters<FGPSPaperBuildArgsCS::FParameters>();
	Args->MaxWorkItems = MaxWorkItems;
	Args->ReadWorkTotal = GraphBuilder.CreateSRV(WorkTotal);
	Args->IndirectArgs = GraphBuilder.CreateUAV(IndirectArgs, PF_R32_UINT);
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, GPSPaper4, "GPS Paper Dispatch Args");
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GPS Paper Dispatch Args"),
			TShaderMapRef<FGPSPaperBuildArgsCS>(ShaderMap), Args, FIntVector(1, 1, 1));
	}

	auto* Clear = GraphBuilder.AllocParameters<FGPSPaperClearCS::FParameters>();
	Clear->PixelThreadStride = PixelDispatch.Stride;
	Clear->OutputSize = FVector2f(static_cast<float>(Extent.X), static_cast<float>(Extent.Y));
	Clear->PackedSamplesUAV = GraphBuilder.CreateUAV(PackedSamples);
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, GPSPaper5, "GPS Paper Clear");
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GPS Paper Clear"),
			TShaderMapRef<FGPSPaperClearCS>(ShaderMap), Clear, PixelDispatch.Groups);
	}

	auto* Splat = GraphBuilder.AllocParameters<FGPSPaperSplatCS::FParameters>();
	Splat->View = View.ViewUniformBuffer;
	Splat->UseSceneDepthTest = Frame.bUseSceneDepth && SceneDepthTexture ? 1u : 0u;
	Splat->SceneDepthBiasCm = Frame.SceneDepthBiasCm;
	Splat->SceneDepthViewMin = FVector2f(Frame.SceneDepthViewRect.Min);
	Splat->SceneDepthViewSize = FVector2f(Frame.SceneDepthViewRect.Size());
	Splat->SceneDepthTexture = SceneDepthTexture ? SceneDepthTexture : GSystemTextures.GetBlackDummy(GraphBuilder);
	Splat->MaxWorkItems = MaxWorkItems;
	Splat->UseVisibleIndices = bUseVisibleIndices ? 1u : 0u;
	Splat->FrameSeed = FrameSeed;
	Splat->ViewRectMin = FVector2f(SceneColor.ViewRect.Min) * static_cast<float>(Frame.Supersampling);
	Splat->ViewRectSize = FVector2f(SceneColor.ViewRect.Size()) * static_cast<float>(Frame.Supersampling);
	Splat->ReadWorkTotal = GraphBuilder.CreateSRV(WorkTotal);
	Splat->ReadWorkIndices = GraphBuilder.CreateSRV(WorkIndices);
	Splat->ReadProjected0 = GraphBuilder.CreateSRV(Projected0);
	Splat->ReadProjected1 = GraphBuilder.CreateSRV(Projected1);
	Splat->VisibleIndices = GraphBuilder.CreateSRV(VisibleIndexBuffer);
	Splat->GPSPositionMin = Data.PositionMin;
	Splat->GPSPositionExtent = Data.PositionExtent;
	Splat->GPSScaleLogMin = Data.ScaleLogMin;
	Splat->GPSScaleLogExtent = Data.ScaleLogExtent;
	Splat->GPSPackedGeometry = GraphBuilder.CreateSRV(GeometryBuffer);
	Splat->GPSPackedColors = GraphBuilder.CreateSRV(ColorBuffer);
	Splat->GPSPackedOpacities = GraphBuilder.CreateSRV(OpacityBuffer);
	Splat->PackedSamplesUAV = GraphBuilder.CreateUAV(PackedSamples);
	Splat->DispatchArgs = IndirectArgs;
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, GPSPaper6, "GPS Paper Point Splat");
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GPS Paper Point Splat"),
			TShaderMapRef<FGPSPaperSplatCS>(ShaderMap), Splat, IndirectArgs, 0);
	}

	auto* Resolve = GraphBuilder.AllocParameters<FGPSPaperResolveCS::FParameters>();
	Resolve->PixelThreadStride = PixelDispatch.Stride;
	Resolve->OutputSize = FVector2f(static_cast<float>(Extent.X), static_cast<float>(Extent.Y));
	Resolve->PackedSamplesSRV = PackedSamples;
	Resolve->LayerIndex = Frame.LayerIndex;
	Resolve->SourceIDs = GraphBuilder.CreateSRV(SourceIDBuffer);
	Resolve->GPSPackedColors = GraphBuilder.CreateSRV(ColorBuffer);
	Resolve->GPSPackedOpacities = GraphBuilder.CreateSRV(OpacityBuffer);
	Resolve->LayerColor = GraphBuilder.CreateUAV(Frame.LayerColor);
	Resolve->LayerHit = GraphBuilder.CreateUAV(Frame.LayerHit);
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, GPSPaper7, "GPS Paper Resolve");
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GPS Paper Resolve"),
			TShaderMapRef<FGPSPaperResolveCS>(ShaderMap), Resolve, PixelDispatch.Groups);
	}
}
