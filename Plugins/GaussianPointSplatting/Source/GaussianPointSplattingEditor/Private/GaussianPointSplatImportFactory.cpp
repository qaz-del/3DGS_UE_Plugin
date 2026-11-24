#include "GaussianPointSplatImportFactory.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "GaussianPointSplatAsset.h"
#include "Misc/PackageName.h"
#include "Misc/FeedbackContext.h"
#include "PlyGaussianLoader.h"
#include "UObject/Package.h"

namespace
{
	UPackage* GetImportOuterPackage(UObject* InParent)
	{
		return InParent ? InParent->GetOutermost() : nullptr;
	}

	FString BuildChunkPackageName(const UPackage* RootPackage, const FString& RootAssetName, int32 ChunkIndex)
	{
		const FString RootPackageName = RootPackage ? RootPackage->GetName() : FString();
		const FString PackagePath = RootPackageName.StartsWith(TEXT("/"))
			? FPackageName::GetLongPackagePath(RootPackageName)
			: TEXT("/Game");
		const FString ChunkAssetName = FString::Printf(TEXT("%s_Chunk%03d"), *RootAssetName, ChunkIndex);
		return PackagePath / ChunkAssetName;
	}

	UGaussianPointSplatAsset* CreateGaussianAsset(
		UObject* Outer,
		UClass* InClass,
		FName Name,
		EObjectFlags Flags,
		TArray<FGaussianPointSplatSample>&& Gaussians,
		const FString& Filename,
		int32 SHDegree,
		bool& bCreatedNewAsset)
	{
		bCreatedNewAsset = false;
		UGaussianPointSplatAsset* Asset = FindObject<UGaussianPointSplatAsset>(Outer, *Name.ToString());
		if (!Asset)
		{
			Asset = NewObject<UGaussianPointSplatAsset>(Outer, InClass, Name, Flags);
			bCreatedNewAsset = true;
		}
		Asset->SetGaussianData(MoveTemp(Gaussians), Filename, SHDegree);
		Asset->MarkPackageDirty();
		return Asset;
	}
}

UGaussianPointSplatImportFactory::UGaussianPointSplatImportFactory()
{
	SupportedClass = UGaussianPointSplatAsset::StaticClass();
	bCreateNew = false;
	bEditorImport = true;
	Formats.Add(TEXT("ply;3D Gaussian Splatting PLY"));
}

UObject* UGaussianPointSplatImportFactory::FactoryCreateFile(
	UClass* InClass,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	const FString& Filename,
	const TCHAR* Parms,
	FFeedbackContext* Warn,
	bool& bOutOperationCanceled)
{
	bOutOperationCanceled = false;

	FPlyGaussianLoadResult LoadResult;
	UGaussianPointSplatAsset* Asset = nullptr;
	int32 CreatedAssetCount = 0;
	const UPackage* RootPackage = GetImportOuterPackage(InParent);
	const FString RootAssetName = InName.ToString();
	auto CreateImportedChunkAsset = [&](int32 ChunkIndex, TArray<FGaussianPointSplatSample>&& ChunkGaussians) -> bool
	{
		bool bCreatedNewAsset = false;
		UGaussianPointSplatAsset* ChunkAsset = nullptr;
		if (ChunkIndex == 0)
		{
			Asset = CreateGaussianAsset(InParent, InClass, InName, Flags, MoveTemp(ChunkGaussians), Filename, LoadResult.SHDegree, bCreatedNewAsset);
			ChunkAsset = Asset;
		}
		else
		{
			if (!Asset)
			{
				LoadResult.Error = FString::Printf(TEXT("Cannot create Gaussian chunk %d before the root asset."), ChunkIndex);
				return false;
			}

			const FString ChunkPackageName = BuildChunkPackageName(RootPackage, RootAssetName, ChunkIndex);
			const FString ChunkAssetName = FPackageName::GetLongPackageAssetName(ChunkPackageName);
			UPackage* ChunkPackage = CreatePackage(*ChunkPackageName);
			ChunkAsset = CreateGaussianAsset(
				ChunkPackage,
				InClass,
				FName(*ChunkAssetName),
				Flags,
				MoveTemp(ChunkGaussians),
				Filename,
				LoadResult.SHDegree,
				bCreatedNewAsset);
			Asset->ChildChunks.Add(ChunkAsset);
			if (bCreatedNewAsset)
			{
				FAssetRegistryModule::AssetCreated(ChunkAsset);
			}
		}

		++CreatedAssetCount;
		if (Warn && ChunkAsset)
		{
			Warn->Logf(
				ELogVerbosity::Display,
				TEXT("Imported Gaussian chunk %d: Asset=%s Gaussians=%d"),
				ChunkIndex,
				*ChunkAsset->GetPathName(),
				ChunkAsset->NumGaussians);
		}
		return true;
	};

	if (!FPlyGaussianLoader::LoadBinaryLittleEndianStreamed(Filename, LoadResult, CreateImportedChunkAsset))
	{
		if (Warn)
		{
			Warn->Logf(ELogVerbosity::Error, TEXT("Gaussian PLY import failed: %s"), *LoadResult.Error);
		}
		return nullptr;
	}

	if (!Asset)
	{
		if (!LoadResult.GaussianChunks.IsEmpty())
		{
			for (int32 ChunkIndex = 0; ChunkIndex < LoadResult.GaussianChunks.Num(); ++ChunkIndex)
			{
				if (!CreateImportedChunkAsset(ChunkIndex, MoveTemp(LoadResult.GaussianChunks[ChunkIndex])))
				{
					if (Warn)
					{
						Warn->Logf(ELogVerbosity::Error, TEXT("Gaussian PLY import failed: %s"), *LoadResult.Error);
					}
					return nullptr;
				}
			}
		}
		else if (!LoadResult.Gaussians.IsEmpty())
		{
			if (!CreateImportedChunkAsset(0, MoveTemp(LoadResult.Gaussians)))
			{
				if (Warn)
				{
					Warn->Logf(ELogVerbosity::Error, TEXT("Gaussian PLY import failed: %s"), *LoadResult.Error);
				}
				return nullptr;
			}
		}
	}

	if (!Asset)
	{
		if (Warn)
		{
			Warn->Logf(ELogVerbosity::Error, TEXT("Gaussian PLY import failed: no gaussians were imported."));
		}
		return nullptr;
	}

	Asset->MarkPackageDirty();

	if (Warn)
	{
		Warn->Logf(
			ELogVerbosity::Display,
			TEXT("Imported Gaussian PLY '%s': %d/%lld gaussians, %d generated assets, root %d gaussians, %d chunks, %d BVH nodes, spatial sort %s, SH degree %d, stride %d, max %d, sample mode %d, output chunk size %d, spatial tile %s, tile dims %dx%dx%d, occupied tiles %d, selected tiles %d, stats samples %d, selected samples %d, rejected candidates %lld, large gaussian filter %s, max scale %.6f, large rejected %lld."),
			*Filename,
			LoadResult.GetImportedGaussianCount(),
			LoadResult.SourceVertexCount,
			CreatedAssetCount,
			Asset->NumGaussians,
			Asset->NumChunks,
			Asset->NumBVHNodes,
			Asset->bSpatiallySortedForBVH ? TEXT("true") : TEXT("false"),
			Asset->SHDegree,
			LoadResult.ImportStride,
			LoadResult.ImportMaxGaussians,
			LoadResult.ImportSampleMode,
			LoadResult.ImportOutputChunkSize,
			LoadResult.bSpatialTileSample ? TEXT("true") : TEXT("false"),
			LoadResult.ImportSpatialTileDimensions.X,
			LoadResult.ImportSpatialTileDimensions.Y,
			LoadResult.ImportSpatialTileDimensions.Z,
			LoadResult.ImportSpatialTileCount,
			LoadResult.ImportSpatialTileSelectedCount,
			LoadResult.ImportSpatialTileStatSampleCount,
			LoadResult.ImportSpatialTileSelectedSampleCount,
			LoadResult.ImportSpatialTileRejectedCandidateCount,
			LoadResult.bImportLargeGaussianFilter ? TEXT("true") : TEXT("false"),
			LoadResult.ImportLargeGaussianMaxScale,
			LoadResult.ImportLargeGaussianRejectedCandidateCount);
	}

	return Asset;
}
