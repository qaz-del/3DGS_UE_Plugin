#include "GaussianPointSplatShaders.h"

#include "GaussianPointSplatLog.h"
#include "GaussianPointSplatPaperRenderer.h"
#include "GaussianPointSplatSceneProxy.h"
#include "GlobalShader.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "HAL/IConsoleManager.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHI.h"
#include "ScreenPass.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"
#include "SystemTextures.h"

DECLARE_GPU_STAT_NAMED(GPSAnalytic1, TEXT("GaussianPointSplatting Analytic Clear"));
DECLARE_GPU_STAT_NAMED(GPSAnalytic2, TEXT("GaussianPointSplatting Analytic Splat"));
DECLARE_GPU_STAT_NAMED(GPSAnalytic3, TEXT("GaussianPointSplatting Analytic Resolve"));

namespace
{
	TAutoConsoleVariable<int32> CVarGPSDebugLogDispatch(
		TEXT("gps.Debug.LogDispatch"), 0,
		TEXT("Logs each GaussianPointSplat compute dispatch. This can be noisy."));
	TAutoConsoleVariable<int32> CVarGPSDebugVisualizeSplatRadius(
		TEXT("gps.Debug.VisualizeSplatRadius"), 0,
		TEXT("Visualizes the regular renderer footprint. 0=off, 1=radius heatmap, 2=coverage alpha."));
	TAutoConsoleVariable<int32> CVarGPSRenderMode(
		TEXT("gps.Render.Mode"), 0,
		TEXT("Gaussian renderer mode. 0=regular splat, 1=paper point splat."));
	TAutoConsoleVariable<int32> CVarGPSRenderUseSceneDepthTest(
		TEXT("gps.Render.UseSceneDepthTest"), 1,
		TEXT("Tests both render modes against UE SceneDepth."));
	TAutoConsoleVariable<float> CVarGPSRenderSceneDepthBiasCm(
		TEXT("gps.Render.SceneDepthBiasCm"), 2.0f,
		TEXT("Positive depth bias in centimeters for SceneDepth tests in both modes."));
	TAutoConsoleVariable<int32> CVarGPSRenderUseHZBOcclusion(
		TEXT("gps.Render.UseHZBOcclusion"), 0,
		TEXT("Uses UE Furthest HZB to cull fully occluded regular splat footprints."));
	TAutoConsoleVariable<float> CVarGPSRenderHZBDepthBiasCm(
		TEXT("gps.Render.HZBDepthBiasCm"), 4.0f,
		TEXT("Positive depth bias in centimeters for regular-renderer HZB occlusion."));
	TAutoConsoleVariable<float> CVarGPSRenderAnalyticAlphaScale(
		TEXT("gps.Render.AnalyticAlphaScale"), 1.0f,
		TEXT("Scales alpha for continuous screen-space Gaussian splats."));
	TAutoConsoleVariable<float> CVarGPSRenderAnalyticCoveragePower(
		TEXT("gps.Render.AnalyticCoveragePower"), 0.85f,
		TEXT("Power applied to the regular Gaussian kernel; values below 1 broaden its tails."));

	constexpr uint32 ThreadGroupSize = 64;
	constexpr uint32 GPSAnalyticMaxDispatchGroups = 65535;

	struct FGPSLinearDispatch
	{
		FIntVector GroupCount = FIntVector(1, 1, 1);
		uint32 ThreadStride = ThreadGroupSize;
	};

	FGPSLinearDispatch MakeGPSLinearDispatch(uint64 ItemCount)
	{
		FGPSLinearDispatch Result;
		const uint64 TotalGroups = FMath::Max<uint64>(FMath::DivideAndRoundUp(ItemCount, static_cast<uint64>(ThreadGroupSize)), 1);
		const uint32 GroupsX = static_cast<uint32>(FMath::Min<uint64>(TotalGroups, GPSAnalyticMaxDispatchGroups));
		const uint64 GroupsY = FMath::DivideAndRoundUp(TotalGroups, static_cast<uint64>(GroupsX));
		check(GroupsY <= GPSAnalyticMaxDispatchGroups);
		Result.GroupCount = FIntVector(GroupsX, static_cast<int32>(GroupsY), 1);
		Result.ThreadStride = GroupsX * ThreadGroupSize;
		return Result;
	}

	template<typename TParameters>
	void BindPackedGaussianData(FRDGBuilder& GraphBuilder, TParameters* Parameters,
		const FGaussianPointSplatProxyData& Data, FRDGBufferRef GeometryBuffer,
		FRDGBufferRef ColorBuffer, FRDGBufferRef OpacityBuffer)
	{
		Parameters->GPSPositionMin = Data.PositionMin;
		Parameters->GPSPositionExtent = Data.PositionExtent;
		Parameters->GPSScaleLogMin = Data.ScaleLogMin;
		Parameters->GPSScaleLogExtent = Data.ScaleLogExtent;
		Parameters->GPSPackedGeometry = GraphBuilder.CreateSRV(GeometryBuffer);
		Parameters->GPSPackedColors = GraphBuilder.CreateSRV(ColorBuffer);
		Parameters->GPSPackedOpacities = GraphBuilder.CreateSRV(OpacityBuffer);
	}
}

class FGPSPointSplatCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGPSPointSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FGPSPointSplatCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER(uint32, NumGaussians)
		SHADER_PARAMETER(uint32, UseVisibleIndices)
		SHADER_PARAMETER(uint32, LinearDispatchThreadStride)
		SHADER_PARAMETER(float, PointRadiusPixels)
		SHADER_PARAMETER(uint32, DebugVisualizeSplatRadius)
		SHADER_PARAMETER(uint32, UseSceneDepthTest)
		SHADER_PARAMETER(uint32, UseHZBOcclusion)
		SHADER_PARAMETER(float, SceneDepthBiasCm)
		SHADER_PARAMETER(FVector2f, SceneDepthViewMin)
		SHADER_PARAMETER(FVector2f, SceneDepthViewSize)
		SHADER_PARAMETER(float, Supersampling)
		SHADER_PARAMETER(float, MinViewDepth)
		SHADER_PARAMETER(FMatrix44f, LocalToView)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<UlongType>, NearestUAV)
		SHADER_PARAMETER(float, HZBDepthBiasCm)
		SHADER_PARAMETER(float, AnalyticAlphaScale)
		SHADER_PARAMETER(float, AnalyticCoveragePower)
		SHADER_PARAMETER(FVector2f, HZBSize)
		SHADER_PARAMETER(FVector2f, ViewportUVToHZBBufferUV)
		SHADER_PARAMETER(float, GaussianScaleMultiplier)
		SHADER_PARAMETER(float, MaxSplatRadiusPixels)
		SHADER_PARAMETER(float, MinSplatRadiusPixels)
		SHADER_PARAMETER(FVector2f, OutputSize)
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
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FurthestHZBTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, AccumRTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, AccumGTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, AccumBTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, AccumATexture)
	END_SHADER_PARAMETER_STRUCT()
public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}
};

class FGPSAnalyticClearCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGPSAnalyticClearCS);
	SHADER_USE_PARAMETER_STRUCT(FGPSAnalyticClearCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, LinearDispatchThreadStride)
		SHADER_PARAMETER(FVector2f, OutputSize)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<UlongType>, NearestUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, AccumRTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, AccumGTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, AccumBTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, AccumATexture)
	END_SHADER_PARAMETER_STRUCT()
public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}
};

class FGPSAnalyticResolveCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGPSAnalyticResolveCS);
	SHADER_USE_PARAMETER_STRUCT(FGPSAnalyticResolveCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, LinearDispatchThreadStride)
		SHADER_PARAMETER(FVector2f, OutputSize)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, ResolvedAccumRTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, ResolvedAccumGTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, ResolvedAccumBTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, ResolvedAccumATexture)
		SHADER_PARAMETER(uint32, LayerIndex)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<UlongType>, NearestSRV)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SourceIDs)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, LayerColor)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<uint2>, LayerHit)
	END_SHADER_PARAMETER_STRUCT()
public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}
};

IMPLEMENT_GLOBAL_SHADER(FGPSPointSplatCS, "/Plugin/GaussianPointSplatting/Private/GPSPointSplat.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGPSAnalyticClearCS, "/Plugin/GaussianPointSplatting/Private/GPSPointSplat.usf", "AnalyticClearCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGPSAnalyticResolveCS, "/Plugin/GaussianPointSplatting/Private/GPSPointSplat.usf", "AnalyticResolveCS", SF_Compute);

FScreenPassTexture FGaussianPointSplatRenderer::AddRenderPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FScreenPassTexture SceneColor,
	const FGaussianPointSplatProxyData& Data,
	FRDGTexture* SceneDepthTexture,
	FRDGTexture* FurthestHZBTexture,
	const FVector2f& HZBSize,
	const FVector2f& ViewportUVToHZBBufferUV,
	FGPSRenderFrame& Frame)
{
	if (!SceneColor.IsValid() || Data.NumGaussians == 0)
	{
		return SceneColor;
	}

	const FIntPoint Extent = Frame.SampleExtent;
	if (Extent.X <= 0 || Extent.Y <= 0)
	{
		UE_LOG(LogGaussianPointSplatting, Warning, TEXT("Skipped render pass because SceneColor extent is invalid: %dx%d"), Extent.X, Extent.Y);
		return SceneColor;
	}

	TArray<FIntPoint> VisibleRanges;
	int32 NumGaussians = Data.NumGaussians;
	bool bUseVisibleIndices = false;
	if (!Data.RootNodes.IsEmpty())
	{
		TArray<int32, TInlineAllocator<64>> Stack;
		for (int32 RootNode : Data.RootNodes)
		{
			Stack.Add(RootNode);
		}
		int32 VisibleCount = 0;
		while (!Stack.IsEmpty())
		{
			const FGaussianPointSplatRenderNode& Node = Data.Nodes[Stack.Pop(EAllowShrinking::No)];
			bool bFullyContained = false;
			if (Node.NumGaussians == 0 || !View.GetCullingFrustum().IntersectBox(Node.Bounds.Origin, Node.Bounds.BoxExtent, bFullyContained))
			{
				continue;
			}
			if (!bFullyContained && Node.LeftChild != INDEX_NONE && Node.RightChild != INDEX_NONE)
			{
				Stack.Add(Node.LeftChild);
				Stack.Add(Node.RightChild);
			}
			else
			{
				VisibleRanges.Add(FIntPoint(Node.FirstGaussian, Node.NumGaussians));
				VisibleCount += Node.NumGaussians;
			}
		}
		if (VisibleCount == 0)
		{
			return SceneColor;
		}
		bUseVisibleIndices = VisibleCount < NumGaussians;
		NumGaussians = VisibleCount;
	}

	TArray<uint32> VisibleIndices;
	if (bUseVisibleIndices)
	{
		VisibleIndices.Reserve(NumGaussians);
		for (const FIntPoint& Range : VisibleRanges)
		{
			for (int32 Index = Range.X; Index < Range.X + Range.Y; ++Index)
			{
				VisibleIndices.Add(static_cast<uint32>(Index));
			}
		}
	}

	auto RegisterStaticBuffer = [&GraphBuilder](const TCHAR* Name, const auto& Samples, TRefCountPtr<FRDGPooledBuffer>& CachedBuffer)
	{
		if (CachedBuffer.IsValid())
		{
			return GraphBuilder.RegisterExternalBuffer(CachedBuffer, Name);
		}
		FRDGBufferRef Buffer = CreateStructuredBuffer(GraphBuilder, Name, Samples);
		CachedBuffer = GraphBuilder.ConvertToExternalBuffer(Buffer);
		return Buffer;
	};
	FRDGBufferRef GeometryBuffer = RegisterStaticBuffer(TEXT("GPS.PackedGeometry"), Data.PackedGeometry, Data.GeometryBuffer);
	FRDGBufferRef ColorBuffer = RegisterStaticBuffer(TEXT("GPS.PackedColors"), Data.PackedColors, Data.ColorBuffer);
	FRDGBufferRef OpacityBuffer = RegisterStaticBuffer(TEXT("GPS.PackedOpacities"), Data.PackedOpacityWords, Data.OpacityBuffer);
	const uint32 DummyIndex = 0;
	FRDGBufferRef VisibleIndexBuffer = bUseVisibleIndices
		? CreateStructuredBuffer(GraphBuilder, TEXT("GPS.VisibleIndices"), TConstArrayView<uint32>(VisibleIndices))
		: CreateStructuredBuffer(GraphBuilder, TEXT("GPS.DummyVisibleIndex"), TConstArrayView<uint32>(&DummyIndex, 1));

	FRDGBufferRef SourceIDBuffer = RegisterStaticBuffer(TEXT("GPS.SourceIDs"), Data.SourceIDs, Data.SourceIDBuffer);
	const int32 RequestedMode = CVarGPSRenderMode.GetValueOnRenderThread();
	if (RequestedMode == 1)
	{
		if (FGaussianPointSplatPaperRenderer::IsSupported(View))
		{
			static bool bLoggedPaperModeActive = false;
			if (!bLoggedPaperModeActive)
			{
				UE_LOG(LogGaussianPointSplatting, Display, TEXT("Paper renderer active: SM6, Atomic64, PackedFormat=%s."),
					GPixelFormats[PF_R64_UINT].Supported ? TEXT("R64_UINT") : TEXT("R32G32_UINT"));
				bLoggedPaperModeActive = true;
			}
			FGaussianPointSplatPaperRenderer::AddPasses(GraphBuilder, View, SceneColor, Frame, SceneDepthTexture, Data,
				GeometryBuffer, ColorBuffer, OpacityBuffer, VisibleIndexBuffer, NumGaussians, bUseVisibleIndices, SourceIDBuffer);
			return SceneColor;
		}
		static bool bLoggedUnsupportedPaperMode = false;
		if (!bLoggedUnsupportedPaperMode)
		{
			UE_LOG(LogGaussianPointSplatting, Warning,
				TEXT("Paper renderer unavailable: Gaussians=%d FeatureLevel=%d Atomic64=%d R64=%d R32G32=%d; using regular renderer."),
				NumGaussians,
				static_cast<int32>(View.GetFeatureLevel()), GRHISupportsAtomicUInt64 ? 1 : 0,
				GPixelFormats[PF_R64_UINT].Supported ? 1 : 0, GPixelFormats[PF_R32G32_UINT].Supported ? 1 : 0);
			bLoggedUnsupportedPaperMode = true;
		}
	}
	else if (RequestedMode != 0)
	{
		static int32 LastUnknownMode = INDEX_NONE;
		if (LastUnknownMode != RequestedMode)
		{
			UE_LOG(LogGaussianPointSplatting, Warning, TEXT("Unknown gps.Render.Mode %d; using regular renderer (0)."), RequestedMode);
			LastUnknownMode = RequestedMode;
		}
	}

	FRDGTextureDesc NearestDesc = FRDGTextureDesc::Create2D(Extent,
		GPixelFormats[PF_R64_UINT].Supported ? PF_R64_UINT : PF_R32G32_UINT,
		FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV | ETextureCreateFlags::Atomic64Compatible);
	NearestDesc.AliasableFormats.Add(PF_R32G32_UINT);
	FRDGTextureRef Nearest = GraphBuilder.CreateTexture(NearestDesc, TEXT("GPS.Analytic.Nearest"));
	FRDGTextureDesc AccumDesc = FRDGTextureDesc::Create2D(
		Extent, PF_R32_UINT, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV);
	FRDGTextureRef AccumRTexture = GraphBuilder.CreateTexture(AccumDesc, TEXT("GPS.Analytic.AccumR"));
	FRDGTextureRef AccumGTexture = GraphBuilder.CreateTexture(AccumDesc, TEXT("GPS.Analytic.AccumG"));
	FRDGTextureRef AccumBTexture = GraphBuilder.CreateTexture(AccumDesc, TEXT("GPS.Analytic.AccumB"));
	FRDGTextureRef AccumATexture = GraphBuilder.CreateTexture(AccumDesc, TEXT("GPS.Analytic.AccumA"));
	const FGPSLinearDispatch PixelDispatch = MakeGPSLinearDispatch(static_cast<uint64>(Extent.X) * Extent.Y);
	const FGPSLinearDispatch GaussianDispatch = MakeGPSLinearDispatch(static_cast<uint64>(NumGaussians));
	const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());

	auto* Clear = GraphBuilder.AllocParameters<FGPSAnalyticClearCS::FParameters>();
	Clear->NearestUAV = GraphBuilder.CreateUAV(Nearest);
	Clear->LinearDispatchThreadStride = PixelDispatch.ThreadStride;
	Clear->OutputSize = FVector2f(static_cast<float>(Extent.X), static_cast<float>(Extent.Y));
	Clear->AccumRTexture = GraphBuilder.CreateUAV(AccumRTexture);
	Clear->AccumGTexture = GraphBuilder.CreateUAV(AccumGTexture);
	Clear->AccumBTexture = GraphBuilder.CreateUAV(AccumBTexture);
	Clear->AccumATexture = GraphBuilder.CreateUAV(AccumATexture);
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, GPSAnalytic1, "GaussianPointSplatting Analytic Clear");
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GaussianPointSplatting Analytic Clear"),
			TShaderMapRef<FGPSAnalyticClearCS>(ShaderMap), Clear, PixelDispatch.GroupCount);
	}

	auto* Splat = GraphBuilder.AllocParameters<FGPSPointSplatCS::FParameters>();
	Splat->View = View.ViewUniformBuffer;
	Splat->NumGaussians = NumGaussians;
	Splat->UseVisibleIndices = bUseVisibleIndices ? 1u : 0u;
	Splat->VisibleIndices = GraphBuilder.CreateSRV(VisibleIndexBuffer);
	Splat->LinearDispatchThreadStride = GaussianDispatch.ThreadStride;
	Splat->PointRadiusPixels = Data.PointRadiusPixels * Frame.Supersampling;
	Splat->DebugVisualizeSplatRadius = static_cast<uint32>(FMath::Clamp(CVarGPSDebugVisualizeSplatRadius.GetValueOnRenderThread(), 0, 2));
	Splat->UseSceneDepthTest = Frame.bUseSceneDepth && SceneDepthTexture ? 1u : 0u;
	Splat->NearestUAV = GraphBuilder.CreateUAV(Nearest);
	Splat->Supersampling = static_cast<float>(Frame.Supersampling);
	Splat->MinViewDepth = FMath::Max(View.NearClippingDistance, 0.001f);
	Splat->LocalToView = Data.LocalToWorld * FMatrix44f(View.ViewMatrices.GetViewMatrix());
	Splat->SceneDepthViewMin = FVector2f(Frame.SceneDepthViewRect.Min);
	Splat->SceneDepthViewSize = FVector2f(Frame.SceneDepthViewRect.Size());
	Splat->UseHZBOcclusion = CVarGPSRenderUseHZBOcclusion.GetValueOnRenderThread() != 0 && FurthestHZBTexture != nullptr && HZBSize.X > 0.0f && HZBSize.Y > 0.0f ? 1u : 0u;
	Splat->SceneDepthBiasCm = FMath::Max(CVarGPSRenderSceneDepthBiasCm.GetValueOnRenderThread(), 0.0f);
	Splat->HZBDepthBiasCm = FMath::Max(CVarGPSRenderHZBDepthBiasCm.GetValueOnRenderThread(), 0.0f);
	Splat->AnalyticAlphaScale = FMath::Max(CVarGPSRenderAnalyticAlphaScale.GetValueOnRenderThread(), 0.0f);
	Splat->AnalyticCoveragePower = FMath::Clamp(CVarGPSRenderAnalyticCoveragePower.GetValueOnRenderThread(), 0.2f, 2.0f);
	Splat->HZBSize = HZBSize;
	Splat->ViewportUVToHZBBufferUV = ViewportUVToHZBBufferUV;
	Splat->GaussianScaleMultiplier = Data.GaussianScaleMultiplier;
	Splat->MaxSplatRadiusPixels = Data.MaxSplatRadiusPixels * Frame.Supersampling;
	Splat->MinSplatRadiusPixels = Data.MinSplatRadiusPixels * Frame.Supersampling;
	Splat->OutputSize = FVector2f(static_cast<float>(Extent.X), static_cast<float>(Extent.Y));
	Splat->ViewRectMin = FVector2f(SceneColor.ViewRect.Min) * static_cast<float>(Frame.Supersampling);
	Splat->ViewRectSize = FVector2f(SceneColor.ViewRect.Size()) * static_cast<float>(Frame.Supersampling);
	Splat->LocalToClip = Data.LocalToWorld * FMatrix44f(View.ViewMatrices.GetWorldToClip());
	BindPackedGaussianData(GraphBuilder, Splat, Data, GeometryBuffer, ColorBuffer, OpacityBuffer);
	Splat->SceneDepthTexture = SceneDepthTexture ? SceneDepthTexture : GSystemTextures.GetBlackDummy(GraphBuilder);
	Splat->FurthestHZBTexture = FurthestHZBTexture ? FurthestHZBTexture : GSystemTextures.GetBlackDummy(GraphBuilder);
	Splat->AccumRTexture = GraphBuilder.CreateUAV(AccumRTexture);
	Splat->AccumGTexture = GraphBuilder.CreateUAV(AccumGTexture);
	Splat->AccumBTexture = GraphBuilder.CreateUAV(AccumBTexture);
	Splat->AccumATexture = GraphBuilder.CreateUAV(AccumATexture);
	if (CVarGPSDebugLogDispatch.GetValueOnRenderThread() != 0)
	{
		UE_LOG(LogGaussianPointSplatting, Display,
			TEXT("Dispatch regular splat: Gaussians=%d Extent=%dx%d ViewRect=%s SceneDepth=%u HZB=%u Groups=%dx%d"),
			NumGaussians, Extent.X, Extent.Y, *SceneColor.ViewRect.ToString(),
			Splat->UseSceneDepthTest, Splat->UseHZBOcclusion,
			GaussianDispatch.GroupCount.X, GaussianDispatch.GroupCount.Y);
	}
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, GPSAnalytic2, "GaussianPointSplatting Analytic Splat");
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GaussianPointSplatting Analytic Splat (%d)", NumGaussians),
			TShaderMapRef<FGPSPointSplatCS>(ShaderMap), Splat, GaussianDispatch.GroupCount);
	}

	auto* Resolve = GraphBuilder.AllocParameters<FGPSAnalyticResolveCS::FParameters>();
	Resolve->LinearDispatchThreadStride = PixelDispatch.ThreadStride;
	Resolve->OutputSize = FVector2f(static_cast<float>(Extent.X), static_cast<float>(Extent.Y));
	Resolve->ResolvedAccumRTexture = AccumRTexture;
	Resolve->ResolvedAccumGTexture = AccumGTexture;
	Resolve->ResolvedAccumBTexture = AccumBTexture;
	Resolve->ResolvedAccumATexture = AccumATexture;
	Resolve->LayerIndex = Frame.LayerIndex;
	Resolve->NearestSRV = Nearest;
	Resolve->SourceIDs = GraphBuilder.CreateSRV(SourceIDBuffer);
	Resolve->LayerColor = GraphBuilder.CreateUAV(Frame.LayerColor);
	Resolve->LayerHit = GraphBuilder.CreateUAV(Frame.LayerHit);
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, GPSAnalytic3, "GaussianPointSplatting Analytic Resolve");
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GaussianPointSplatting Analytic Resolve"),
			TShaderMapRef<FGPSAnalyticResolveCS>(ShaderMap), Resolve, PixelDispatch.GroupCount);
	}

	return SceneColor;
}
