#pragma once

#include "CoreMinimal.h"
#include "GaussianPointSplatTypes.h"
#include "Templates/Function.h"

using FPlyGaussianChunkCallback = TFunctionRef<bool(int32 ChunkIndex, TArray<FGaussianPointSplatSample>&& Gaussians)>;

struct FPlyGaussianLoadResult
{
	TArray<FGaussianPointSplatSample> Gaussians;
	TArray<TArray<FGaussianPointSplatSample>> GaussianChunks;
	int64 ImportedGaussianCount = 0;
	int32 OutputChunkCount = 0;
	int64 SourceVertexCount = 0;
	int32 SHDegree = 0;
	int32 ImportStride = 1;
	int32 ImportMaxGaussians = 0;
	int32 ImportSampleMode = 0;
	int32 ImportOutputChunkSize = 0;
	int32 ImportSpatialTileResolution = 0;
	int32 ImportSpatialTileCount = 0;
	int32 ImportSpatialTileSelectedCount = 0;
	int32 ImportSpatialTileStatSampleCount = 0;
	int32 ImportSpatialTileSelectedSampleCount = 0;
	int64 ImportSpatialTileRejectedCandidateCount = 0;
	int64 ImportLargeGaussianRejectedCandidateCount = 0;
	float ImportLargeGaussianMaxScale = 0.0f;
	FIntVector ImportSpatialTileDimensions = FIntVector(0, 0, 0);
	bool bSpatialTileSample = false;
	bool bImportLargeGaussianFilter = false;
	FString Error;

	int32 GetImportedGaussianCount() const
	{
		if (ImportedGaussianCount > 0)
		{
			return static_cast<int32>(FMath::Min<int64>(ImportedGaussianCount, TNumericLimits<int32>::Max()));
		}

		if (GaussianChunks.IsEmpty())
		{
			return Gaussians.Num();
		}

		int64 TotalCount = 0;
		for (const TArray<FGaussianPointSplatSample>& Chunk : GaussianChunks)
		{
			TotalCount += Chunk.Num();
		}
		return static_cast<int32>(FMath::Min<int64>(TotalCount, TNumericLimits<int32>::Max()));
	}

	int32 GetOutputChunkCount() const
	{
		if (OutputChunkCount > 0)
		{
			return OutputChunkCount;
		}

		return GaussianChunks.IsEmpty() ? (Gaussians.IsEmpty() ? 0 : 1) : GaussianChunks.Num();
	}
};

class FPlyGaussianLoader
{
public:
	static bool LoadBinaryLittleEndian(const FString& Filename, FPlyGaussianLoadResult& OutResult);
	static bool LoadBinaryLittleEndianStreamed(const FString& Filename, FPlyGaussianLoadResult& OutResult, FPlyGaussianChunkCallback OnChunkLoaded);
};
