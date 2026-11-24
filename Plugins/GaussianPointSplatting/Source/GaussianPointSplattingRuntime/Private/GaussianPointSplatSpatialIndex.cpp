#include "GaussianPointSplatSpatialIndex.h"

#include "GaussianPointSplatAsset.h"
#include "GaussianPointSplatComponent.h"
#include "GaussianPointSplatLog.h"
#include "UObject/UObjectIterator.h"

namespace
{
	uint32 ExpandSpatialIndexMortonBits(uint32 Value)
	{
		Value = (Value * 0x00010001u) & 0xFF0000FFu;
		Value = (Value * 0x00000101u) & 0x0F00F00Fu;
		Value = (Value * 0x00000011u) & 0xC30C30C3u;
		Value = (Value * 0x00000005u) & 0x49249249u;
		return Value;
	}

	uint32 MortonCode(const FVector3f& Position, const FBox& Bounds)
	{
		const FVector Size = Bounds.GetSize();
		const FVector Relative = FVector(Position) - Bounds.Min;
		const uint32 X = static_cast<uint32>(FMath::FloorToInt(1023.0 * FMath::Clamp(Relative.X / FMath::Max(Size.X, UE_DOUBLE_SMALL_NUMBER), 0.0, 1.0)));
		const uint32 Y = static_cast<uint32>(FMath::FloorToInt(1023.0 * FMath::Clamp(Relative.Y / FMath::Max(Size.Y, UE_DOUBLE_SMALL_NUMBER), 0.0, 1.0)));
		const uint32 Z = static_cast<uint32>(FMath::FloorToInt(1023.0 * FMath::Clamp(Relative.Z / FMath::Max(Size.Z, UE_DOUBLE_SMALL_NUMBER), 0.0, 1.0)));
		const uint32 XBits = ExpandSpatialIndexMortonBits(X);
		const uint32 YBits = ExpandSpatialIndexMortonBits(Y);
		const uint32 ZBits = ExpandSpatialIndexMortonBits(Z);
		return XBits * 4u | YBits * 2u | ZBits;
	}

	FBox GaussianBounds(const FGaussianPointSplatSample& Gaussian)
	{
		const double Radius = 3.0 * FMath::Max3(FMath::Abs(Gaussian.Scale.X), FMath::Abs(Gaussian.Scale.Y), FMath::Abs(Gaussian.Scale.Z));
		return FBox(FVector(Gaussian.Position) - FVector(Radius), FVector(Gaussian.Position) + FVector(Radius));
	}

	int32 BuildNode(FGaussianPointSplatSpatialSection& Section, int32 First, int32 Count, int32 LeafSize)
	{
		const int32 NodeIndex = Section.Nodes.AddDefaulted();
		Section.Nodes[NodeIndex].FirstGaussian = First;
		Section.Nodes[NodeIndex].NumGaussians = Count;
		Section.Nodes[NodeIndex].bLeaf = Count <= LeafSize;

		if (Count <= LeafSize)
		{
			FBox Bounds(EForceInit::ForceInit);
			float MaxScale = 0.0f;
			for (int32 Index = First; Index < First + Count; ++Index)
			{
				const FGaussianPointSplatSample& Gaussian = Section.SourceAsset->Gaussians[Section.OrderedIndices[Index]];
				Bounds += GaussianBounds(Gaussian);
				MaxScale = FMath::Max(MaxScale, FMath::Max3(FMath::Abs(Gaussian.Scale.X), FMath::Abs(Gaussian.Scale.Y), FMath::Abs(Gaussian.Scale.Z)));
			}
			Section.Nodes[NodeIndex].LocalBounds = Bounds;
			Section.Nodes[NodeIndex].MaxGaussianScale = MaxScale;
			return NodeIndex;
		}

		const int32 LeafCount = FMath::DivideAndRoundUp(Count, LeafSize);
		const int32 LeftCount = (LeafCount / 2) * LeafSize;
		const int32 Left = BuildNode(Section, First, LeftCount, LeafSize);
		const int32 Right = BuildNode(Section, First + LeftCount, Count - LeftCount, LeafSize);
		Section.Nodes[NodeIndex].LeftChild = Left;
		Section.Nodes[NodeIndex].RightChild = Right;
		Section.Nodes[NodeIndex].LocalBounds = Section.Nodes[Left].LocalBounds + Section.Nodes[Right].LocalBounds;
		Section.Nodes[NodeIndex].MaxGaussianScale = FMath::Max(Section.Nodes[Left].MaxGaussianScale, Section.Nodes[Right].MaxGaussianScale);
		return NodeIndex;
	}

	void BuildSection(UGaussianPointSplatAsset* Asset, int32 LeafSize, FGaussianPointSplatSpatialSection& Section)
	{
		Section.SourceAsset = Asset;
		Section.SourceRevision = Asset->DataRevision;
		const int32 Count = Asset->Gaussians.Num();
		Section.OrderedIndices.SetNumUninitialized(Count);
		if (Count == 0)
		{
			return;
		}

		FBox Centers(EForceInit::ForceInit);
		for (const FGaussianPointSplatSample& Gaussian : Asset->Gaussians)
		{
			Centers += FVector(Gaussian.Position);
		}
		TArray<uint32> MortonCodes;
		MortonCodes.SetNumUninitialized(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Section.OrderedIndices[Index] = Index;
			MortonCodes[Index] = MortonCode(Asset->Gaussians[Index].Position, Centers);
		}
		Section.OrderedIndices.Sort([&MortonCodes](int32 Left, int32 Right)
		{
			return MortonCodes[Left] == MortonCodes[Right] ? Left < Right : MortonCodes[Left] < MortonCodes[Right];
		});
		Section.Nodes.Reserve(2 * FMath::DivideAndRoundUp(Count, LeafSize));
		BuildNode(Section, 0, Count, LeafSize);
	}
}

void UGaussianPointSplatSpatialIndex::RefreshIndex()
{
	if (!SourceAsset || SourceAsset->IsEmpty())
	{
		UE_LOG(LogGaussianPointSplatting, Warning, TEXT("Spatial index refresh requires a nonempty SourceAsset."));
		return;
	}
	if (SourceAsset->Gaussians.IsEmpty() || SourceAsset->ChildChunks.ContainsByPredicate([](const TObjectPtr<UGaussianPointSplatAsset>& Child)
		{ return !Child || Child->Gaussians.IsEmpty() || !Child->ChildChunks.IsEmpty(); }))
	{
		UE_LOG(LogGaussianPointSplatting, Warning, TEXT("Spatial index refresh requires nonempty root and child assets."));
		return;
	}

	const int32 ClampedLeafSize = FMath::Clamp(LeafSize, 256, 65536);
	TArray<FGaussianPointSplatSpatialSection> NewSections;
	NewSections.Reserve(1 + SourceAsset->ChildChunks.Num());
	auto AddSection = [&](UGaussianPointSplatAsset* Asset)
	{
		if (Asset && !Asset->Gaussians.IsEmpty())
		{
			BuildSection(Asset, ClampedLeafSize, NewSections.AddDefaulted_GetRef());
		}
	};
	AddSection(SourceAsset);
	for (UGaussianPointSplatAsset* Child : SourceAsset->ChildChunks)
	{
		AddSection(Child);
	}

	Sections = MoveTemp(NewSections);
	LeafSize = ClampedLeafSize;
	BuiltLeafSize = ClampedLeafSize;
	TotalIndexedGaussians = SourceAsset->GetTotalGaussianCount();
	MarkPackageDirty();
	UE_LOG(LogGaussianPointSplatting, Display, TEXT("Refreshed spatial index %s: %d Gaussians in %d source assets."),
		*GetPathName(), TotalIndexedGaussians, Sections.Num());
	for (TObjectIterator<UGaussianPointSplatComponent> It; It; ++It)
	{
		if (!It->IsTemplate() && It->IsRegistered() && It->SpatialIndex == this)
		{
			It->MarkRenderStateDirty();
		}
	}
}

bool UGaussianPointSplatSpatialIndex::IsValidFor(const UGaussianPointSplatAsset* Asset) const
{
	if (!Asset || SourceAsset != Asset || BuiltLeafSize != FMath::Clamp(LeafSize, 256, 65536) ||
		TotalIndexedGaussians != Asset->GetTotalGaussianCount() ||
		Sections.Num() != 1 + Asset->ChildChunks.Num())
	{
		return false;
	}

	for (int32 SectionIndex = 0; SectionIndex < Sections.Num(); ++SectionIndex)
	{
		const UGaussianPointSplatAsset* Expected = SectionIndex == 0 ? Asset : Asset->ChildChunks[SectionIndex - 1].Get();
		const FGaussianPointSplatSpatialSection& Section = Sections[SectionIndex];
		if (!Expected || Section.SourceAsset != Expected || Section.SourceRevision != Expected->DataRevision ||
			Section.OrderedIndices.Num() != Expected->Gaussians.Num() || Section.Nodes.IsEmpty() ||
			Section.Nodes[0].FirstGaussian != 0 || Section.Nodes[0].NumGaussians != Expected->Gaussians.Num())
		{
			return false;
		}
	}
	return true;
}
