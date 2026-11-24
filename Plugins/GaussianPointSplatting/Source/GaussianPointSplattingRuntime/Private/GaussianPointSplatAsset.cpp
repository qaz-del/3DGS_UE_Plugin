#include "GaussianPointSplatAsset.h"

#include "GaussianPointSplatLog.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

namespace
{
	TAutoConsoleVariable<int32> CVarGPSImportBuildSpatialBVH(
		TEXT("gps.Import.BuildSpatialBVH"),
		0,
		TEXT("Builds spatially sorted chunks and a CPU-side Gaussian BVH when Gaussian assets are imported or rebuilt."));

	struct FGaussianMortonEntry
	{
		uint32 MortonCode = 0;
		int32 OriginalIndex = 0;
	};

	uint32 ExpandMortonBits(uint32 Value)
	{
		Value = (Value * 0x00010001u) & 0xFF0000FFu;
		Value = (Value * 0x00000101u) & 0x0F00F00Fu;
		Value = (Value * 0x00000011u) & 0xC30C30C3u;
		Value = (Value * 0x00000005u) & 0x49249249u;
		return Value;
	}

	uint32 QuantizeMortonCoordinate(double NormalizedValue)
	{
		const int32 Quantized = FMath::Clamp(FMath::FloorToInt(NormalizedValue * 1023.0), 0, 1023);
		return static_cast<uint32>(Quantized);
	}

	uint32 CalculateMortonCode(const FVector& Position, const FBox& Bounds)
	{
		const FVector BoundsSize = Bounds.GetSize();
		const double X = BoundsSize.X > KINDA_SMALL_NUMBER ? (Position.X - Bounds.Min.X) / BoundsSize.X : 0.0;
		const double Y = BoundsSize.Y > KINDA_SMALL_NUMBER ? (Position.Y - Bounds.Min.Y) / BoundsSize.Y : 0.0;
		const double Z = BoundsSize.Z > KINDA_SMALL_NUMBER ? (Position.Z - Bounds.Min.Z) / BoundsSize.Z : 0.0;
		const uint32 QuantizedX = QuantizeMortonCoordinate(X);
		const uint32 QuantizedY = QuantizeMortonCoordinate(Y);
		const uint32 QuantizedZ = QuantizeMortonCoordinate(Z);
		return (ExpandMortonBits(QuantizedX) << 2) | (ExpandMortonBits(QuantizedY) << 1) | ExpandMortonBits(QuantizedZ);
	}

	FBox CalculateGaussianLocalBounds(const FGaussianPointSplatSample& Gaussian)
	{
		const FVector Center(Gaussian.Position);
		const float MaxScale = FMath::Max3(FMath::Abs(Gaussian.Scale.X), FMath::Abs(Gaussian.Scale.Y), FMath::Abs(Gaussian.Scale.Z));
		const double Radius = FMath::Max(static_cast<double>(MaxScale) * 3.0, 0.0001);
		return FBox(Center - FVector(Radius), Center + FVector(Radius));
	}

	void SortGaussiansByMortonOrder(TArray<FGaussianPointSplatSample>& Gaussians, const FBox& Bounds)
	{
		const int32 NumGaussians = Gaussians.Num();
		if (NumGaussians <= 1 || !Bounds.IsValid)
		{
			return;
		}

		TArray<FGaussianMortonEntry> Entries;
		Entries.SetNumUninitialized(NumGaussians);
		for (int32 Index = 0; Index < NumGaussians; ++Index)
		{
			Entries[Index].MortonCode = CalculateMortonCode(FVector(Gaussians[Index].Position), Bounds);
			Entries[Index].OriginalIndex = Index;
		}

		Entries.Sort(
			[](const FGaussianMortonEntry& Left, const FGaussianMortonEntry& Right)
			{
				if (Left.MortonCode != Right.MortonCode)
				{
					return Left.MortonCode < Right.MortonCode;
				}
				return Left.OriginalIndex < Right.OriginalIndex;
			});

		TArray<int32> NewToOld;
		NewToOld.SetNumUninitialized(NumGaussians);
		for (int32 NewIndex = 0; NewIndex < NumGaussians; ++NewIndex)
		{
			NewToOld[NewIndex] = Entries[NewIndex].OriginalIndex;
		}
		Entries.Empty();

		TBitArray<> Visited;
		Visited.Init(false, NumGaussians);
		for (int32 StartIndex = 0; StartIndex < NumGaussians; ++StartIndex)
		{
			if (Visited[StartIndex])
			{
				continue;
			}

			int32 CurrentIndex = StartIndex;
			FGaussianPointSplatSample Temp = MoveTemp(Gaussians[StartIndex]);
			while (true)
			{
				Visited[CurrentIndex] = true;
				const int32 SourceIndex = NewToOld[CurrentIndex];
				if (SourceIndex == StartIndex)
				{
					Gaussians[CurrentIndex] = MoveTemp(Temp);
					break;
				}

				Gaussians[CurrentIndex] = MoveTemp(Gaussians[SourceIndex]);
				CurrentIndex = SourceIndex;
			}
		}
	}

	int32 BuildBVHNodeRecursive(const TArray<FGaussianPointSplatChunk>& Chunks, int32 FirstChunk, int32 NumChunks, TArray<FGaussianPointSplatBVHNode>& OutNodes)
	{
		const int32 NodeIndex = OutNodes.AddDefaulted();
		FGaussianPointSplatBVHNode& Node = OutNodes[NodeIndex];
		Node.FirstChunk = FirstChunk;
		Node.NumChunks = NumChunks;
		Node.bLeaf = NumChunks <= 1;

		FBox NodeBounds(EForceInit::ForceInit);
		for (int32 ChunkIndex = FirstChunk; ChunkIndex < FirstChunk + NumChunks; ++ChunkIndex)
		{
			NodeBounds += Chunks[ChunkIndex].LocalBounds;
		}

		Node.LocalBounds = NodeBounds;
		Node.FirstGaussian = Chunks[FirstChunk].FirstGaussian;
		const FGaussianPointSplatChunk& LastChunk = Chunks[FirstChunk + NumChunks - 1];
		Node.NumGaussians = LastChunk.FirstGaussian + LastChunk.NumGaussians - Node.FirstGaussian;

		if (!Node.bLeaf)
		{
			const int32 LeftChunkCount = NumChunks / 2;
			const int32 RightChunkCount = NumChunks - LeftChunkCount;
			const int32 LeftChild = BuildBVHNodeRecursive(Chunks, FirstChunk, LeftChunkCount, OutNodes);
			const int32 RightChild = BuildBVHNodeRecursive(Chunks, FirstChunk + LeftChunkCount, RightChunkCount, OutNodes);
			OutNodes[NodeIndex].LeftChild = LeftChild;
			OutNodes[NodeIndex].RightChild = RightChild;
		}

		return NodeIndex;
	}
}

void UGaussianPointSplatAsset::SetGaussianData(TArray<FGaussianPointSplatSample>&& InGaussians, const FString& InSourceFile, int32 InSHDegree)
{
	Gaussians = MoveTemp(InGaussians);
	++DataRevision;
	SourceFile = InSourceFile;
	SHDegree = InSHDegree;
	ChildChunks.Reset();
	RebuildBoundsAndChunks();
}

int32 UGaussianPointSplatAsset::GetGaussianCount() const
{
	return NumGaussians;
}

int32 UGaussianPointSplatAsset::GetTotalGaussianCount() const
{
	int64 TotalCount = NumGaussians;
	for (const UGaussianPointSplatAsset* ChildChunk : ChildChunks)
	{
		if (ChildChunk)
		{
			TotalCount += ChildChunk->GetTotalGaussianCount();
		}
	}

	return static_cast<int32>(FMath::Min<int64>(TotalCount, TNumericLimits<int32>::Max()));
}

FBoxSphereBounds UGaussianPointSplatAsset::GetCombinedLocalBounds() const
{
	FBox CombinedBounds(EForceInit::ForceInit);
	if (NumGaussians > 0)
	{
		CombinedBounds += LocalBounds.GetBox();
	}

	for (const UGaussianPointSplatAsset* ChildChunk : ChildChunks)
	{
		if (ChildChunk && ChildChunk->GetTotalGaussianCount() > 0)
		{
			CombinedBounds += ChildChunk->GetCombinedLocalBounds().GetBox();
		}
	}

	if (CombinedBounds.IsValid)
	{
		return FBoxSphereBounds(CombinedBounds);
	}

	return LocalBounds;
}

void UGaussianPointSplatAsset::RebuildBoundsAndChunks(int32 ChunkSize)
{
	const double StartTimeSeconds = FPlatformTime::Seconds();
	NumGaussians = Gaussians.Num();
	Chunks.Reset();
	BVHNodes.Reset();
	NumChunks = 0;
	NumBVHNodes = 0;
	BVHRootNode = INDEX_NONE;
	BVHLeafChunkSize = FMath::Max(1, ChunkSize);
	bSpatiallySortedForBVH = false;

	FBox Bounds(EForceInit::ForceInit);
	for (const FGaussianPointSplatSample& Gaussian : Gaussians)
	{
		Bounds += CalculateGaussianLocalBounds(Gaussian);
	}

	if (Bounds.IsValid)
	{
		LocalBounds = FBoxSphereBounds(Bounds);
	}
	else
	{
		LocalBounds = FBoxSphereBounds(FSphere(FVector::ZeroVector, 1.0));
	}

	const bool bBuildSpatialBVH = CVarGPSImportBuildSpatialBVH.GetValueOnAnyThread() != 0;
	if (bBuildSpatialBVH && NumGaussians > 1 && Bounds.IsValid)
	{
		SortGaussiansByMortonOrder(Gaussians, Bounds);
		++DataRevision;
		bSpatiallySortedForBVH = true;
	}

	if (bBuildSpatialBVH)
	{
		ChunkSize = BVHLeafChunkSize;
		for (int32 FirstGaussian = 0; FirstGaussian < NumGaussians; FirstGaussian += ChunkSize)
		{
			const int32 ChunkCount = FMath::Min(ChunkSize, NumGaussians - FirstGaussian);
			FBox ChunkBounds(EForceInit::ForceInit);
			for (int32 Index = FirstGaussian; Index < FirstGaussian + ChunkCount; ++Index)
			{
				ChunkBounds += CalculateGaussianLocalBounds(Gaussians[Index]);
			}

			FGaussianPointSplatChunk Chunk;
			Chunk.FirstGaussian = FirstGaussian;
			Chunk.NumGaussians = ChunkCount;
			Chunk.LocalBounds = ChunkBounds;
			Chunks.Add(Chunk);
		}

		NumChunks = Chunks.Num();
		if (!Chunks.IsEmpty())
		{
			BVHNodes.Reserve(Chunks.Num() * 2 - 1);
			BVHRootNode = BuildBVHNodeRecursive(Chunks, 0, Chunks.Num(), BVHNodes);
			NumBVHNodes = BVHNodes.Num();
		}
	}

	const double ElapsedMs = (FPlatformTime::Seconds() - StartTimeSeconds) * 1000.0;
	UE_LOG(
		LogGaussianPointSplatting,
		Verbose,
		TEXT("Rebuilt Gaussian asset BVH: Gaussians=%d Chunks=%d BVHNodes=%d Root=%d ChunkSize=%d SpatialSort=%s ElapsedMs=%.2f Source=%s"),
		NumGaussians,
		NumChunks,
		NumBVHNodes,
		BVHRootNode,
		BVHLeafChunkSize,
		bSpatiallySortedForBVH ? TEXT("true") : TEXT("false"),
		ElapsedMs,
		*SourceFile);
}
