#include "GaussianPointSplatSceneProxy.h"

#include "GaussianPointSplatAsset.h"
#include "GaussianPointSplatComponent.h"
#include "GaussianPointSplatLog.h"
#include "GaussianPointSplatSpatialIndex.h"
#include "HAL/IConsoleManager.h"
#include "MeshElementCollector.h"
#include "SceneManagement.h"

namespace
{
	uint32 QuantizeUnit(float Value, uint32 MaxValue)
	{
		return static_cast<uint32>(FMath::RoundToInt(FMath::Clamp(Value, 0.0f, 1.0f) * MaxValue));
	}

	uint32 PackRotation(const FVector4f& Rotation)
	{
		float Components[4] = { Rotation.X, Rotation.Y, Rotation.Z, Rotation.W };
		int32 Largest = 0;
		for (int32 Index = 1; Index < 4; ++Index)
		{
			if (FMath::Abs(Components[Index]) > FMath::Abs(Components[Largest]))
			{
				Largest = Index;
			}
		}
		const float Sign = Components[Largest] < 0.0f ? -1.0f : 1.0f;
		uint32 Packed = static_cast<uint32>(Largest) << 30u;
		int32 PackedIndex = 0;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			if (Index != Largest)
			{
				const float Unit = Components[Index] * Sign * 0.70710678118f + 0.5f;
				Packed |= QuantizeUnit(Unit, 1023u) << (PackedIndex * 10);
				++PackedIndex;
			}
		}
		return Packed;
	}

	FCriticalSection GGaussianPointSplatProxyLock;
	TSet<const FGaussianPointSplatSceneProxy*> GGaussianPointSplatProxies;

	TAutoConsoleVariable<int32> CVarGPSDebugDrawBounds(
		TEXT("gps.Debug.DrawBounds"),
		0,
		TEXT("Draws GaussianPointSplat scene proxy world bounds as a cyan wire box."));
}

FGaussianPointSplatSceneProxy::FGaussianPointSplatSceneProxy(const UGaussianPointSplatComponent* Component)
	: FPrimitiveSceneProxy(Component)
{
	check(Component);
	check(Component->Asset);

	Data.PickOwner.ComponentID = Component->GetUniqueID();
	Data.SourceToLocal = FTransform(Component->SourceRotation, Component->SourceOffset, FVector(FMath::Max(Component->SourceUniformScale, 0.000001f))).ToMatrixWithScale();
	Data.PickOwner.SourceToWorld = Data.SourceToLocal * Component->GetComponentTransform().ToMatrixWithScale();
	Data.PickOwner.Component = const_cast<UGaussianPointSplatComponent*>(Component);
	Data.PickOwner.RootAsset = Component->Asset.Get();
	Data.PickOwner.RootRevision = Component->Asset->DataRevision;
	uint32 FlatOffset = 0;
	auto AddPickSource = [&](UGaussianPointSplatAsset* Asset)
	{
		if (!Asset) return;
		Data.PickOwner.Sources.Add({Asset, Asset->DataRevision, FlatOffset, static_cast<uint32>(Asset->Gaussians.Num())});
		FlatOffset += Asset->Gaussians.Num();
	};
	AddPickSource(Component->Asset);
	for (UGaussianPointSplatAsset* Child : Component->Asset->ChildChunks) AddPickSource(Child);

	Data.LocalToWorld = FMatrix44f(Component->GetComponentTransform().ToMatrixWithScale());
	Data.PointRadiusPixels = Component->PointRadiusPixels;
	Data.GaussianScaleMultiplier = Component->GaussianScaleMultiplier;
	Data.MaxSplatRadiusPixels = Component->MaxSplatRadiusPixels;
	Data.MinSplatRadiusPixels = Component->MinSplatRadiusPixels;

	const int32 TotalGaussians = Component->Asset->GetTotalGaussianCount();
	const int32 NumToCopy = FMath::Max(0, FMath::Min(TotalGaussians, Component->MaxRenderGaussians));
	const FQuat SourceRotation = Component->SourceRotation.Quaternion();
	const FVector SourceOffset = Component->SourceOffset;
	const float SourceUniformScale = FMath::Max(Component->SourceUniformScale, 0.000001f);
	FBox SourceTransformedBounds(EForceInit::ForceInit);
	TArray<FVector4f> Positions;
	TArray<FVector4f> Colors;
	TArray<FVector4f> Scales;
	TArray<FVector4f> Rotations;
	Positions.Reserve(NumToCopy);
	Data.SourceIDs.Reserve(NumToCopy);
	Colors.Reserve(NumToCopy);
	Scales.Reserve(NumToCopy);
	Rotations.Reserve(NumToCopy);
	int64 SourceGaussianOffset = 0;

	auto AppendGaussian = [&](const FGaussianPointSplatSample& Gaussian, uint32 SourceID)
	{
		const FVector TransformedPositionD = SourceRotation.RotateVector(FVector(Gaussian.Position) * SourceUniformScale) + SourceOffset;
		const FVector3f TransformedPosition(TransformedPositionD);
		const FVector3f TransformedScale = Gaussian.Scale * SourceUniformScale;
		const FQuat GaussianRotation(
			static_cast<double>(Gaussian.Rotation.X),
			static_cast<double>(Gaussian.Rotation.Y),
			static_cast<double>(Gaussian.Rotation.Z),
			static_cast<double>(Gaussian.Rotation.W));
		const FQuat TransformedRotation = (SourceRotation * GaussianRotation).GetNormalized();

		Positions.Add(FVector4f(TransformedPosition, 1.0f));
		Data.SourceIDs.Add(SourceID);
		SourceTransformedBounds += FVector(TransformedPosition);
		Colors.Add(FVector4f(
			FMath::Clamp(Gaussian.Color.R, 0.0f, 1.0f),
			FMath::Clamp(Gaussian.Color.G, 0.0f, 1.0f),
			FMath::Clamp(Gaussian.Color.B, 0.0f, 1.0f),
			FMath::Clamp(Gaussian.Opacity, 0.0f, 1.0f)));
		Scales.Add(FVector4f(
			FMath::Max(TransformedScale.X, 0.0001f),
			FMath::Max(TransformedScale.Y, 0.0001f),
			FMath::Max(TransformedScale.Z, 0.0001f),
			0.0f));
		Rotations.Add(FVector4f(
			static_cast<float>(TransformedRotation.X),
			static_cast<float>(TransformedRotation.Y),
			static_cast<float>(TransformedRotation.Z),
			static_cast<float>(TransformedRotation.W)));
	};

	auto TransformLeafBounds = [&](const FBox& NodeLocalBounds)
	{
		FBox Transformed(EForceInit::ForceInit);
		for (int32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
		{
			const FVector Corner(
				(CornerIndex & 1) ? NodeLocalBounds.Max.X : NodeLocalBounds.Min.X,
				(CornerIndex & 2) ? NodeLocalBounds.Max.Y : NodeLocalBounds.Min.Y,
				(CornerIndex & 4) ? NodeLocalBounds.Max.Z : NodeLocalBounds.Min.Z);
			Transformed += SourceRotation.RotateVector(Corner * SourceUniformScale) + SourceOffset;
		}
		return FBoxSphereBounds(Transformed);
	};

	const bool bUseSpatialIndex = Component->SpatialIndex && Component->SpatialIndex->IsValidFor(Component->Asset);
	if (Component->SpatialIndex && !bUseSpatialIndex)
	{
		UE_LOG(LogGaussianPointSplatting, Warning, TEXT("Spatial index is stale for %s; refresh it to enable BVH culling."), *Component->GetPathName());
	}
	if (bUseSpatialIndex)
	{
		int64 ProcessedGaussians = 0;
		for (const FGaussianPointSplatSpatialSection& Section : Component->SpatialIndex->Sections)
		{
			uint32 SectionOffset = 0;
			for (const FGPSPickSource& Source : Data.PickOwner.Sources)
			{
				if (Source.Asset.Get() == Section.SourceAsset.Get()) { SectionOffset = Source.First; break; }
			}
			const int64 SectionSourceStart = ProcessedGaussians;
			const int32 NodeBase = Data.Nodes.Num();
			Data.RootNodes.Add(NodeBase);
			for (const FGaussianPointSplatBVHNode& Node : Section.Nodes)
			{
				if (!Node.bLeaf)
				{
					continue;
				}
				const int64 TargetSelected = (ProcessedGaussians + Node.NumGaussians) * NumToCopy / TotalGaussians;
				const int32 SelectedCount = static_cast<int32>(TargetSelected - Positions.Num());
				for (int32 SelectedIndex = 0; SelectedIndex < SelectedCount; ++SelectedIndex)
				{
					const int32 OrderedIndex = Node.FirstGaussian + static_cast<int64>(SelectedIndex) * Node.NumGaussians / SelectedCount;
					AppendGaussian(Section.SourceAsset->Gaussians[Section.OrderedIndices[OrderedIndex]],
						SectionOffset + Section.OrderedIndices[OrderedIndex]);
				}
				ProcessedGaussians += Node.NumGaussians;
			}
			for (const FGaussianPointSplatBVHNode& SourceNode : Section.Nodes)
			{
				FGaussianPointSplatRenderNode& Node = Data.Nodes.AddDefaulted_GetRef();
				const int64 FirstSource = SectionSourceStart + SourceNode.FirstGaussian;
				const int64 EndSource = FirstSource + SourceNode.NumGaussians;
				Node.FirstGaussian = static_cast<int32>(FirstSource * NumToCopy / TotalGaussians);
				Node.NumGaussians = static_cast<int32>(EndSource * NumToCopy / TotalGaussians) - Node.FirstGaussian;
				const double ExtraRadius = 3.0 * SourceNode.MaxGaussianScale * FMath::Max(Data.GaussianScaleMultiplier - 1.0f, 0.0f);
				Node.LocalBounds = TransformLeafBounds(SourceNode.LocalBounds.ExpandBy(ExtraRadius));
				Node.Bounds = Node.LocalBounds.TransformBy(Component->GetComponentTransform());
				Node.LeftChild = SourceNode.LeftChild == INDEX_NONE ? INDEX_NONE : NodeBase + SourceNode.LeftChild;
				Node.RightChild = SourceNode.RightChild == INDEX_NONE ? INDEX_NONE : NodeBase + SourceNode.RightChild;
			}
		}
	}
	else
	{
		auto AppendAssetGaussians = [&](const UGaussianPointSplatAsset* SourceAsset)
		{
			if (!SourceAsset)
			{
				return;
			}

			const int64 ChunkEnd = SourceGaussianOffset + SourceAsset->Gaussians.Num();
			while (Positions.Num() < NumToCopy)
			{
				const int64 SourceIndex = static_cast<int64>(Positions.Num()) * TotalGaussians / NumToCopy;
				if (SourceIndex >= ChunkEnd)
				{
					break;
				}
				AppendGaussian(SourceAsset->Gaussians[static_cast<int32>(SourceIndex - SourceGaussianOffset)], static_cast<uint32>(SourceIndex));
			}
			SourceGaussianOffset = ChunkEnd;
		};

		AppendAssetGaussians(Component->Asset);
		for (const UGaussianPointSplatAsset* ChildChunk : Component->Asset->ChildChunks)
		{
			AppendAssetGaussians(ChildChunk);
			if (Positions.Num() >= NumToCopy)
			{
				break;
			}
		}
	}
	if (NumToCopy < TotalGaussians)
	{
		UE_LOG(LogGaussianPointSplatting, Warning,
			TEXT("Gaussian coverage limited: Component=%s AssetGaussians=%d RenderGaussians=%d. Check the placed actor instance's MaxRenderGaussians override."),
			*Component->GetPathName(), TotalGaussians, NumToCopy);
	}

	if (SourceTransformedBounds.IsValid)
	{
		Data.Bounds = FBoxSphereBounds(SourceTransformedBounds).TransformBy(Component->GetComponentTransform());
	}
	else
	{
		Data.Bounds = Component->Asset->GetCombinedLocalBounds().TransformBy(Component->GetComponentTransform());
	}

	Data.NumGaussians = Positions.Num();
	if (Data.NumGaussians > 0)
	{
		Data.PositionMin = FVector4f(FVector3f(SourceTransformedBounds.Min), 0.0f);
		const FVector3f Extent(SourceTransformedBounds.GetSize());
		Data.PositionExtent = FVector4f(FMath::Max(Extent.X, 1.0e-6f), FMath::Max(Extent.Y, 1.0e-6f), FMath::Max(Extent.Z, 1.0e-6f), 0.0f);
		float MinLogScale = TNumericLimits<float>::Max();
		float MaxLogScale = -TNumericLimits<float>::Max();
		for (const FVector4f& Scale : Scales)
		{
			MinLogScale = FMath::Min3(MinLogScale, FMath::Loge(Scale.X), FMath::Min(FMath::Loge(Scale.Y), FMath::Loge(Scale.Z)));
			MaxLogScale = FMath::Max3(MaxLogScale, FMath::Loge(Scale.X), FMath::Max(FMath::Loge(Scale.Y), FMath::Loge(Scale.Z)));
		}
		Data.ScaleLogMin = MinLogScale;
		Data.ScaleLogExtent = FMath::Max(MaxLogScale - MinLogScale, 1.0e-6f);
		Data.PackedGeometry.Reserve(Data.NumGaussians);
		Data.PackedColors.Reserve(Data.NumGaussians);
		Data.PackedOpacityWords.SetNumZeroed(FMath::DivideAndRoundUp(Data.NumGaussians, 4));
		for (int32 Index = 0; Index < Data.NumGaussians; ++Index)
		{
			const FVector4f& Position = Positions[Index];
			const FVector4f& Scale = Scales[Index];
			const FVector4f& Color = Colors[Index];
			const uint32 X = QuantizeUnit((Position.X - Data.PositionMin.X) / Data.PositionExtent.X, (1u << 22u) - 1u);
			const uint32 Y = QuantizeUnit((Position.Y - Data.PositionMin.Y) / Data.PositionExtent.Y, (1u << 21u) - 1u);
			const uint32 Z = QuantizeUnit((Position.Z - Data.PositionMin.Z) / Data.PositionExtent.Z, (1u << 21u) - 1u);
			const uint64 PackedPosition = static_cast<uint64>(X) | (static_cast<uint64>(Y) << 22u) | (static_cast<uint64>(Z) << 43u);
			const uint32 SX = QuantizeUnit((FMath::Loge(Scale.X) - MinLogScale) / Data.ScaleLogExtent, 1023u);
			const uint32 SY = QuantizeUnit((FMath::Loge(Scale.Y) - MinLogScale) / Data.ScaleLogExtent, 1023u);
			const uint32 SZ = QuantizeUnit((FMath::Loge(Scale.Z) - MinLogScale) / Data.ScaleLogExtent, 1023u);
			Data.PackedGeometry.Add(FUintVector4(static_cast<uint32>(PackedPosition), static_cast<uint32>(PackedPosition >> 32u), SX | (SY << 10u) | (SZ << 20u), PackRotation(Rotations[Index])));
			Data.PackedColors.Add(QuantizeUnit(Color.X, 1023u) | (QuantizeUnit(Color.Y, 1023u) << 10u) | (QuantizeUnit(Color.Z, 1023u) << 20u));
			Data.PackedOpacityWords[Index / 4] |= QuantizeUnit(Color.W, 255u) << ((Index % 4) * 8);
		}
	}

	FScopeLock Lock(&GGaussianPointSplatProxyLock);
	GGaussianPointSplatProxies.Add(this);

	UE_LOG(LogGaussianPointSplatting, Verbose,
		TEXT("Created scene proxy: RenderGaussians=%d AssetGaussians=%d BoundsOrigin=%s BoundsExtent=%s Component=%s"),
		Data.NumGaussians, TotalGaussians, *Data.Bounds.Origin.ToString(),
		*Data.Bounds.BoxExtent.ToString(), *Component->GetPathName());
}

FGaussianPointSplatSceneProxy::~FGaussianPointSplatSceneProxy()
{
	FScopeLock Lock(&GGaussianPointSplatProxyLock);
	GGaussianPointSplatProxies.Remove(this);
}

void FGaussianPointSplatSceneProxy::OnTransformChanged(FRHICommandListBase& RHICmdList)
{
	const FMatrix NewWorld = GetLocalToWorld();
	Data.PickOwner.SourceToWorld = Data.SourceToLocal * NewWorld;
	Data.LocalToWorld = FMatrix44f(NewWorld);
	for (FGaussianPointSplatRenderNode& Node : Data.Nodes) Node.Bounds = Node.LocalBounds.TransformBy(NewWorld);
	Data.Bounds = GetBounds();
}

SIZE_T FGaussianPointSplatSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<SIZE_T>(&UniquePointer);
}

uint32 FGaussianPointSplatSceneProxy::GetMemoryFootprint() const
{
	const SIZE_T AllocatedSize = Data.PickOwner.Sources.GetAllocatedSize() + Data.SourceIDs.GetAllocatedSize() + Data.PackedGeometry.GetAllocatedSize() + Data.PackedColors.GetAllocatedSize() + Data.PackedOpacityWords.GetAllocatedSize() + Data.Nodes.GetAllocatedSize() + Data.RootNodes.GetAllocatedSize();
	return static_cast<uint32>(sizeof(*this) + AllocatedSize);
}

FPrimitiveViewRelevance FGaussianPointSplatSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bDynamicRelevance = true;
	Result.bRenderInMainPass = true;
	Result.bUsesLightingChannels = false;
	Result.bShadowRelevance = false;
	return Result;
}

void FGaussianPointSplatSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	const bool bDrawBounds = CVarGPSDebugDrawBounds.GetValueOnAnyThread() != 0;
	if (!bDrawBounds)
	{
		return;
	}

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if ((VisibilityMap & (1 << ViewIndex)) == 0)
		{
			continue;
		}

		FPrimitiveDrawInterface* PDI = Collector.GetPDI(ViewIndex);
		if (PDI)
		{
			DrawWireBox(PDI, Data.Bounds.GetBox(), FColor::Cyan, SDPG_World);
		}
	}
}

int32 FGaussianPointSplatSceneProxy::GetProxyCount()
{
	FScopeLock Lock(&GGaussianPointSplatProxyLock);
	return GGaussianPointSplatProxies.Num();
}

void FGaussianPointSplatSceneProxy::ForEachProxy(TFunctionRef<void(const FGaussianPointSplatSceneProxy&)> Callback)
{
	FScopeLock Lock(&GGaussianPointSplatProxyLock);
	for (const FGaussianPointSplatSceneProxy* Proxy : GGaussianPointSplatProxies)
	{
		if (Proxy)
		{
			Callback(*Proxy);
		}
	}
}
