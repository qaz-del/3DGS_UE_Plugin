#include "GaussianPointSplatViewExtension.h"

#include "GaussianPointSplatLog.h"
#include "GaussianPointSplatPaperRenderer.h"
#include "GaussianPointSplatPicking.h"
#include "GaussianPointSplatSceneProxy.h"
#include "GaussianPointSplatShaders.h"
#include "GlobalShader.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "HAL/IConsoleManager.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphUtils.h"
#include "ScreenPass.h"
#include "SceneRendering.h"
#include "SceneTextures.h"
#include "ShaderParameterStruct.h"

DECLARE_GPU_STAT_NAMED(GPSFrame1, TEXT("GPS Frame Composite"));

namespace
{
	TAutoConsoleVariable<int32> CVarGPSSupersampling(TEXT("gps.Render.Supersampling"), 1,
		TEXT("Sample grid factor for both modes: 1=1x1, 2=2x2. Does not change the viewport resolution."));
	TAutoConsoleVariable<int32> CVarGPSOutput(TEXT("gps.Debug.Output"), 0,
		TEXT("0=color, 1=linear view depth, 2=stable component/source ID, 3=coverage."));
	TAutoConsoleVariable<float> CVarGPSDepthAlpha(TEXT("gps.Render.DepthAlphaThreshold"), 0.05f,
		TEXT("Minimum visible layer contribution for representative depth/ID. Does not alter color."));
}

class FGPSFrameCompositeCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGPSFrameCompositeCS);
	SHADER_USE_PARAMETER_STRUCT(FGPSFrameCompositeCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, LayerCount)
		SHADER_PARAMETER(uint32, Supersampling)
		SHADER_PARAMETER(uint32, DebugOutput)
		SHADER_PARAMETER(float, DepthAlphaThreshold)
		SHADER_PARAMETER(FIntPoint, OutputExtent)
		SHADER_PARAMETER(FIntPoint, ViewMin)
		SHADER_PARAMETER(FIntPoint, ViewSize)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float4>, LayerColor)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<uint2>, LayerHit)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ComponentIDs)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, SceneColor)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputColor)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputDepth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint2>, OutputID)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputCoverage)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FGPSFrameCompositeCS, "/Plugin/GaussianPointSplatting/Private/GPSFrame.usf", "CompositeCS", SF_Compute);

FGaussianPointSplatViewExtension::FGaussianPointSplatViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

bool FGaussianPointSplatViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	return true;
}

void FGaussianPointSplatViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	if (Pass == EPostProcessingPass::AfterDOF && FGaussianPointSplatSceneProxy::GetProxyCount() > 0)
	{
		InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FGaussianPointSplatViewExtension::PostProcessPass_RenderThread));
	}
}

FScreenPassTexture FGaussianPointSplatViewExtension::PostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessMaterialInputs& Inputs)
{
	FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor), Inputs.OverrideOutput);
	if (!SceneColor.IsValid()) return SceneColor;
	FGaussianPointSplatPicking::Poll();
	TArray<const FGaussianPointSplatSceneProxy*> Proxies;
	FGaussianPointSplatSceneProxy::ForEachProxy([&](const FGaussianPointSplatSceneProxy& Proxy)
	{
		if (&Proxy.GetScene() == InView.Family->Scene && Proxy.IsShown(&InView) && Proxy.GetData().NumGaussians > 0)
			Proxies.Add(&Proxy);
	});
	Proxies.Sort([](const FGaussianPointSplatSceneProxy& A, const FGaussianPointSplatSceneProxy& B)
	{
		return A.GetData().PickOwner.ComponentID < B.GetData().PickOwner.ComponentID;
	});
	if (!FGaussianPointSplatPaperRenderer::IsSupported(InView))
	{
		static bool bLoggedUnsupported = false;
		if (!bLoggedUnsupported)
		{
			UE_LOG(LogGaussianPointSplatting, Error, TEXT("Gaussian depth/ID rendering requires SM6 and 64-bit texture atomics; view skipped."));
			bLoggedUnsupported = true;
		}
		return SceneColor;
	}
	if (Proxies.Num() > GMaxTextureArrayLayers)
	{
		UE_LOG(LogGaussianPointSplatting, Error, TEXT("Gaussian component count %d exceeds texture layer limit %d."), Proxies.Num(), GMaxTextureArrayLayers);
		return SceneColor;
	}
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(InView);
	const FSceneTextures* SceneTextures = ViewInfo.GetSceneTexturesChecked();
	FRDGTextureRef SceneDepthTexture = SceneTextures ? (SceneTextures->Depth.Resolve ? SceneTextures->Depth.Resolve : SceneTextures->Depth.Target) : nullptr;
	FRDGTextureRef FurthestHZBTexture = ViewInfo.HZB;
	FVector2f HZBSize = FVector2f::ZeroVector;
	FVector2f ViewportUVToHZBBufferUV = FVector2f::ZeroVector;
	if (FurthestHZBTexture && ViewInfo.HZBMipmap0Size.X > 0 && ViewInfo.HZBMipmap0Size.Y > 0)
	{
		HZBSize = FVector2f(ViewInfo.HZBMipmap0Size);
		ViewportUVToHZBBufferUV = FVector2f(ViewInfo.ViewRect.Size()) / (2.0f * HZBSize);
	}

	FGPSRenderFrame Frame;
	Frame.LayerCount = Proxies.Num();
	Frame.Supersampling = FMath::Clamp(CVarGPSSupersampling.GetValueOnRenderThread(), 1, 2);
	const FIntPoint Extent = SceneColor.Texture->Desc.Extent;
	Frame.SampleExtent = Extent * static_cast<int32>(Frame.Supersampling);
	if (Frame.SampleExtent.X > GMaxTextureDimensions || Frame.SampleExtent.Y > GMaxTextureDimensions)
	{
		UE_LOG(LogGaussianPointSplatting, Warning, TEXT("Gaussian 2x2 sample grid exceeds texture dimension limit; using 1x1."));
		Frame.Supersampling = 1;
		Frame.SampleExtent = Extent;
	}
	const IConsoleVariable* LogDispatch = IConsoleManager::Get().FindConsoleVariable(TEXT("gps.Debug.LogDispatch"));
	if (LogDispatch && LogDispatch->GetInt() != 0)
	{
		int64 Total = 0;
		for (const FGaussianPointSplatSceneProxy* Proxy : Proxies) Total += Proxy->GetData().NumGaussians;
		UE_LOG(LogGaussianPointSplatting, Display, TEXT("GPS Frame: Components=%d RenderGaussians=%lld SampleGrid=%ux%u SampleExtent=%dx%d ColorView=%s DepthView=%s"),
			Proxies.Num(), Total, Frame.Supersampling, Frame.Supersampling, Frame.SampleExtent.X, Frame.SampleExtent.Y,
			*SceneColor.ViewRect.ToString(), *ViewInfo.ViewRect.ToString());
	}
	Frame.SceneDepthViewRect = ViewInfo.ViewRect;
	const IConsoleVariable* DepthTest = IConsoleManager::Get().FindConsoleVariable(TEXT("gps.Render.UseSceneDepthTest"));
	const IConsoleVariable* DepthBias = IConsoleManager::Get().FindConsoleVariable(TEXT("gps.Render.SceneDepthBiasCm"));
	Frame.bUseSceneDepth = DepthTest && DepthTest->GetInt() != 0;
	if (Frame.bUseSceneDepth && !SceneDepthTexture)
	{
		static bool bLoggedMissingDepth = false;
		if (!bLoggedMissingDepth)
		{
			UE_LOG(LogGaussianPointSplatting, Warning, TEXT("UE SceneDepth is unavailable for this view; Gaussian SceneDepth testing is skipped."));
			bLoggedMissingDepth = true;
		}
	}
	Frame.SceneDepthBiasCm = DepthBias ? FMath::Max(DepthBias->GetFloat(), 0.0f) : 2.0f;
	const ETextureCreateFlags Flags = TexCreate_ShaderResource | TexCreate_UAV;
	const uint16 Layers = static_cast<uint16>(FMath::Max(Proxies.Num(), 1));
	Frame.LayerColor = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2DArray(Frame.SampleExtent, PF_FloatRGBA, FClearValueBinding::None, Flags, Layers), TEXT("GPS.Layers.Color"));
	Frame.LayerHit = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2DArray(Frame.SampleExtent, PF_R32G32_UINT, FClearValueBinding::None, Flags, Layers), TEXT("GPS.Layers.Hit"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Frame.LayerColor), FLinearColor::Transparent);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Frame.LayerHit), 0u);
	TArray<FGPSPickOwner> Owners;
	TArray<uint32> ComponentIDs;
	for (const FGaussianPointSplatSceneProxy* Proxy : Proxies)
	{
		const FGaussianPointSplatProxyData& Data = Proxy->GetData();
		Owners.Add(Data.PickOwner);
		ComponentIDs.Add(Data.PickOwner.ComponentID);
		FGaussianPointSplatRenderer::AddRenderPass(GraphBuilder, InView, SceneColor, Data,
			SceneDepthTexture, FurthestHZBTexture, HZBSize, ViewportUVToHZBBufferUV, Frame);
		++Frame.LayerIndex;
	}
	if (ComponentIDs.IsEmpty()) ComponentIDs.Add(0u);
	FRDGBufferRef IDs = CreateStructuredBuffer(GraphBuilder, TEXT("GPS.ComponentIDs"), TConstArrayView<uint32>(ComponentIDs));
	FRDGTextureDesc OutputDesc = SceneColor.Texture->Desc;
	OutputDesc.Flags |= TexCreate_UAV;
	FRDGTextureRef Output = GraphBuilder.CreateTexture(OutputDesc, TEXT("GPS.SceneColor"));
	Frame.Depth = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Extent, PF_R32_FLOAT, FClearValueBinding::None, Flags), TEXT("GPS.Depth"));
	Frame.ID = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Extent, PF_R32G32_UINT, FClearValueBinding::None, Flags), TEXT("GPS.ID"));
	Frame.Coverage = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Extent, PF_R32_FLOAT, FClearValueBinding::None, Flags), TEXT("GPS.Coverage"));
	auto* Composite = GraphBuilder.AllocParameters<FGPSFrameCompositeCS::FParameters>();
	Composite->LayerCount = Frame.LayerCount;
	Composite->Supersampling = Frame.Supersampling;
	Composite->DebugOutput = FMath::Clamp(CVarGPSOutput.GetValueOnRenderThread(), 0, 3);
	Composite->DepthAlphaThreshold = FMath::Clamp(CVarGPSDepthAlpha.GetValueOnRenderThread(), 1.0f/255.0f, 1.0f);
	Composite->OutputExtent = Extent;
	Composite->ViewMin = SceneColor.ViewRect.Min;
	Composite->ViewSize = SceneColor.ViewRect.Size();
	Composite->LayerColor = Frame.LayerColor;
	Composite->LayerHit = Frame.LayerHit;
	Composite->ComponentIDs = GraphBuilder.CreateSRV(IDs);
	Composite->SceneColor = SceneColor.Texture;
	Composite->OutputColor = GraphBuilder.CreateUAV(Output);
	Composite->OutputDepth = GraphBuilder.CreateUAV(Frame.Depth);
	Composite->OutputID = GraphBuilder.CreateUAV(Frame.ID);
	Composite->OutputCoverage = GraphBuilder.CreateUAV(Frame.Coverage);
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, GPSFrame1, "GPS Frame Composite");
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GPS Frame Composite (%u layers, %ux%u)", Frame.LayerCount, Frame.Supersampling, Frame.Supersampling),
			TShaderMapRef<FGPSFrameCompositeCS>(GetGlobalShaderMap(InView.GetFeatureLevel())), Composite,
			FIntVector(FMath::DivideAndRoundUp(Extent.X, 8), FMath::DivideAndRoundUp(Extent.Y, 8), 1));
	}
	FGaussianPointSplatPicking::AddPasses(GraphBuilder, InView, SceneColor, Frame, Owners);
	FScreenPassTexture Result(Output, SceneColor.ViewRect);
	if (Inputs.OverrideOutput.IsValid() && Output != Inputs.OverrideOutput.Texture)
	{
		AddDrawTexturePass(GraphBuilder, InView, Result, Inputs.OverrideOutput);
		return Inputs.OverrideOutput;
	}
	return Result;
}
