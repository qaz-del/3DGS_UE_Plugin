#include "PlyGaussianLoader.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformFileManager.h"

namespace
{
	static constexpr float SH_C0 = 0.28209479177387814f;
	static constexpr int32 GPSImportSampleModeSequential = 0;
	static constexpr int32 GPSImportSampleModeSpatialTile = 1;

	static TAutoConsoleVariable<int32> CVarGPSImportMaxGaussians(
		TEXT("gps.Import.MaxGaussians"),
		15000000,
		TEXT("Maximum number of gaussians to import from a PLY. 0 imports all vertices."));

	static TAutoConsoleVariable<int32> CVarGPSImportStride(
		TEXT("gps.Import.Stride"),
		1,
		TEXT("Import every Nth gaussian from a PLY. Useful for quick tests with very large files."));

	static TAutoConsoleVariable<int32> CVarGPSImportSampleMode(
		TEXT("gps.Import.SampleMode"),
		GPSImportSampleModeSpatialTile,
		TEXT("Gaussian import mode. 0 sequential chunks, 1 spatial tile chunks."));

	static TAutoConsoleVariable<int32> CVarGPSImportOutputChunkSize(
		TEXT("gps.Import.OutputChunkSize"),
		2000000,
		TEXT("Splits imported gaussians into multiple generated assets with at most this many gaussians per chunk. 0 keeps a single asset."));

	static TAutoConsoleVariable<int32> CVarGPSImportSpatialTileResolution(
		TEXT("gps.Import.SpatialTile.Resolution"),
		0,
		TEXT("Coarse tile resolution per axis for gps.Import.SampleMode 1. 0 chooses tile dimensions from MaxGaussians and OutputChunkSize."));

	static TAutoConsoleVariable<int32> CVarGPSImportSpatialTileMaxStatsSamples(
		TEXT("gps.Import.SpatialTile.MaxStatsSamples"),
		1000000,
		TEXT("Maximum candidate samples used to estimate spatial tile import bounds."));

	static TAutoConsoleVariable<int32> CVarGPSImportSpatialTileAutoMultiplier(
		TEXT("gps.Import.SpatialTile.AutoMultiplier"),
		16,
		TEXT("Multiplier applied to the target output chunk count when choosing automatic spatial tile count."));

	static TAutoConsoleVariable<int32> CVarGPSImportSpatialTileSelectedTileCount(
		TEXT("gps.Import.SpatialTile.SelectedTileCount"),
		0,
		TEXT("Number of connected-core spatial tiles to import in gps.Import.SampleMode 1. 0 selects enough tiles to approximate gps.Import.MaxGaussians."));

	static TAutoConsoleVariable<float> CVarGPSImportSpatialTileCenterWeight(
		TEXT("gps.Import.SpatialTile.CenterWeight"),
		4.0f,
		TEXT("Origin bias used when selecting spatial tiles. 0 selects by sampled tile density only."));

	static TAutoConsoleVariable<float> CVarGPSImportSpatialTileCoreDensityRatio(
		TEXT("gps.Import.SpatialTile.CoreDensityRatio"),
		0.02f,
		TEXT("Minimum sampled tile density ratio used by the connected-core selector. Lower values grow farther from the dense core."));

	static TAutoConsoleVariable<int32> CVarGPSImportFilterLargeGaussians(
		TEXT("gps.Import.FilterLargeGaussians"),
		1,
		TEXT("Rejects very large gaussians during spatial-tile import. This suppresses sky/background splats while keeping compact building detail."));

	static TAutoConsoleVariable<float> CVarGPSImportFilterCandidatePoolMultiplier(
		TEXT("gps.Import.Filter.CandidatePoolMultiplier"),
		1.5f,
		TEXT("Spatial tile candidate pool size multiplier used before large-gaussian filtering. Values above 1 leave room to reject background splats while still reaching MaxGaussians."));

	static TAutoConsoleVariable<float> CVarGPSImportFilterMaxScaleQuantile(
		TEXT("gps.Import.Filter.MaxScaleQuantile"),
		0.95f,
		TEXT("Quantile of sampled selected-tile max scale used as the large-gaussian cutoff before applying gps.Import.Filter.MaxScaleMultiplier."));

	static TAutoConsoleVariable<float> CVarGPSImportFilterMaxScaleMultiplier(
		TEXT("gps.Import.Filter.MaxScaleMultiplier"),
		1.15f,
		TEXT("Multiplier applied to the sampled selected-tile scale quantile. Larger values preserve more broad splats; smaller values reject more sky/background splats."));

	static TAutoConsoleVariable<float> CVarGPSImportSpatialTileScalePenalty(
		TEXT("gps.Import.SpatialTile.ScalePenalty"),
		0.75f,
		TEXT("Penalty applied to spatial tiles whose sampled gaussians are much larger than the global sampled median scale."));

	enum class EPlyScalarType : uint8
	{
		Invalid,
		Int8,
		UInt8,
		Int16,
		UInt16,
		Int32,
		UInt32,
		Float32,
		Float64
	};

	struct FPlyProperty
	{
		EPlyScalarType Type = EPlyScalarType::Invalid;
		FString Name;
	};

	struct FPlyGaussianRaw
	{
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
		float Scale0 = 0.0f;
		float Scale1 = 0.0f;
		float Scale2 = 0.0f;
		float Rot0 = 1.0f;
		float Rot1 = 0.0f;
		float Rot2 = 0.0f;
		float Rot3 = 0.0f;
		float OpacityLogit = 10.0f;
		float Fdc0 = 0.0f;
		float Fdc1 = 0.0f;
		float Fdc2 = 0.0f;
	};

	struct FSpatialTileStats
	{
		bool bValid = false;
		int32 Resolution = 4;
		FIntVector Dimensions = FIntVector(1, 1, 1);
		TSet<uint64> SelectedTileKeys;
		int32 StatSampleCount = 0;
		int32 OccupiedTileCount = 0;
		int32 SelectedTileCount = 0;
		int32 SelectedTileSampleCount = 0;
		float LargeGaussianMaxScale = 0.0f;
		bool bUseLargeGaussianFilter = false;
		FVector3f Min = FVector3f::ZeroVector;
		FVector3f Max = FVector3f::ZeroVector;
	};

	struct FSpatialTileCandidate
	{
		uint64 TileKey = 0;
		FIntVector TileCoord = FIntVector(0, 0, 0);
		int32 SampleCount = 0;
		double OriginDistance = 0.0;
		double MaxScaleSum = 0.0;
		double OpacitySum = 0.0;
		double Score = 0.0;
	};

	struct FSpatialImportSample
	{
		FVector3f Position = FVector3f::ZeroVector;
		float MaxScale = 0.0f;
		float Opacity = 1.0f;
	};

	struct FPlyGaussianField
	{
		int32 Offset = INDEX_NONE;
		EPlyScalarType Type = EPlyScalarType::Invalid;
	};

	struct FPlyGaussianReadLayout
	{
		int32 VertexStride = 0;
		FPlyGaussianField X;
		FPlyGaussianField Y;
		FPlyGaussianField Z;
		FPlyGaussianField Scale0;
		FPlyGaussianField Scale1;
		FPlyGaussianField Scale2;
		FPlyGaussianField Rot0;
		FPlyGaussianField Rot1;
		FPlyGaussianField Rot2;
		FPlyGaussianField Rot3;
		FPlyGaussianField Opacity;
		FPlyGaussianField Fdc0;
		FPlyGaussianField Fdc1;
		FPlyGaussianField Fdc2;
	};

	class FLargePlyFileReader
	{
	public:
		explicit FLargePlyFileReader(TUniquePtr<IFileHandle>&& InHandle)
			: Handle(MoveTemp(InHandle))
			, FileSize(Handle ? Handle->Size() : 0)
			, Position(Handle ? Handle->Tell() : 0)
		{
		}

		bool IsValid() const
		{
			return Handle.IsValid() && FileSize >= 0 && Position >= 0;
		}

		bool AtEnd() const
		{
			return !IsValid() || Position >= FileSize;
		}

		bool IsError() const
		{
			return bError;
		}

		bool Read(void* Destination, int64 NumBytes)
		{
			if (!IsValid() || NumBytes < 0 || (NumBytes > 0 && !Destination))
			{
				bError = true;
				return false;
			}

			if (NumBytes == 0)
			{
				return true;
			}

			if (Position > FileSize || NumBytes > FileSize - Position)
			{
				bError = true;
				return false;
			}

			if (!Handle->Read(static_cast<uint8*>(Destination), NumBytes))
			{
				bError = true;
				return false;
			}

			Position = Handle->Tell();
			if (Position < 0)
			{
				bError = true;
				return false;
			}

			return true;
		}

	private:
		TUniquePtr<IFileHandle> Handle;
		int64 FileSize = 0;
		int64 Position = 0;
		bool bError = false;
	};

	EPlyScalarType ParseScalarType(const FString& Type)
	{
		if (Type == TEXT("char") || Type == TEXT("int8")) return EPlyScalarType::Int8;
		if (Type == TEXT("uchar") || Type == TEXT("uint8")) return EPlyScalarType::UInt8;
		if (Type == TEXT("short") || Type == TEXT("int16")) return EPlyScalarType::Int16;
		if (Type == TEXT("ushort") || Type == TEXT("uint16")) return EPlyScalarType::UInt16;
		if (Type == TEXT("int") || Type == TEXT("int32")) return EPlyScalarType::Int32;
		if (Type == TEXT("uint") || Type == TEXT("uint32")) return EPlyScalarType::UInt32;
		if (Type == TEXT("float") || Type == TEXT("float32")) return EPlyScalarType::Float32;
		if (Type == TEXT("double") || Type == TEXT("float64")) return EPlyScalarType::Float64;
		return EPlyScalarType::Invalid;
	}

	int32 GetScalarByteSize(EPlyScalarType Type)
	{
		switch (Type)
		{
		case EPlyScalarType::Int8:
		case EPlyScalarType::UInt8:
			return 1;
		case EPlyScalarType::Int16:
		case EPlyScalarType::UInt16:
			return 2;
		case EPlyScalarType::Int32:
		case EPlyScalarType::UInt32:
		case EPlyScalarType::Float32:
			return 4;
		case EPlyScalarType::Float64:
			return 8;
		default:
			return 0;
		}
	}

	void AssignGaussianField(FPlyGaussianReadLayout& Layout, const FString& PropertyName, FPlyGaussianField Field)
	{
		if (PropertyName == TEXT("x")) Layout.X = Field;
		else if (PropertyName == TEXT("y")) Layout.Y = Field;
		else if (PropertyName == TEXT("z")) Layout.Z = Field;
		else if (PropertyName == TEXT("scale_0")) Layout.Scale0 = Field;
		else if (PropertyName == TEXT("scale_1")) Layout.Scale1 = Field;
		else if (PropertyName == TEXT("scale_2")) Layout.Scale2 = Field;
		else if (PropertyName == TEXT("rot_0")) Layout.Rot0 = Field;
		else if (PropertyName == TEXT("rot_1")) Layout.Rot1 = Field;
		else if (PropertyName == TEXT("rot_2")) Layout.Rot2 = Field;
		else if (PropertyName == TEXT("rot_3")) Layout.Rot3 = Field;
		else if (PropertyName == TEXT("opacity")) Layout.Opacity = Field;
		else if (PropertyName == TEXT("f_dc_0")) Layout.Fdc0 = Field;
		else if (PropertyName == TEXT("f_dc_1")) Layout.Fdc1 = Field;
		else if (PropertyName == TEXT("f_dc_2")) Layout.Fdc2 = Field;
	}

	bool BuildGaussianReadLayout(const TArray<FPlyProperty>& Properties, FPlyGaussianReadLayout& OutLayout, FString& OutError)
	{
		OutLayout = FPlyGaussianReadLayout();
		int32 ByteOffset = 0;
		for (const FPlyProperty& Property : Properties)
		{
			const int32 ByteSize = GetScalarByteSize(Property.Type);
			if (ByteSize <= 0)
			{
				OutError = FString::Printf(TEXT("Unsupported byte size for PLY property '%s'."), *Property.Name);
				return false;
			}

			FPlyGaussianField Field;
			Field.Offset = ByteOffset;
			Field.Type = Property.Type;
			AssignGaussianField(OutLayout, Property.Name, Field);
			ByteOffset += ByteSize;
		}

		OutLayout.VertexStride = ByteOffset;
		if (OutLayout.VertexStride <= 0)
		{
			OutError = TEXT("PLY vertex element has no readable scalar properties.");
			return false;
		}
		return true;
	}

	template <typename T>
	T ReadUnalignedValue(const uint8* Data)
	{
		T Value;
		FMemory::Memcpy(&Value, Data, sizeof(T));
		return Value;
	}

	bool ReadScalarAsFloatFromBytes(const uint8* VertexData, const FPlyGaussianField& Field, float& OutValue)
	{
		if (!VertexData || Field.Offset == INDEX_NONE)
		{
			return false;
		}

		const uint8* FieldData = VertexData + Field.Offset;
		switch (Field.Type)
		{
		case EPlyScalarType::Int8:
			OutValue = static_cast<float>(ReadUnalignedValue<int8>(FieldData));
			return true;
		case EPlyScalarType::UInt8:
			OutValue = static_cast<float>(ReadUnalignedValue<uint8>(FieldData));
			return true;
		case EPlyScalarType::Int16:
			OutValue = static_cast<float>(ReadUnalignedValue<int16>(FieldData));
			return true;
		case EPlyScalarType::UInt16:
			OutValue = static_cast<float>(ReadUnalignedValue<uint16>(FieldData));
			return true;
		case EPlyScalarType::Int32:
			OutValue = static_cast<float>(ReadUnalignedValue<int32>(FieldData));
			return true;
		case EPlyScalarType::UInt32:
			OutValue = static_cast<float>(ReadUnalignedValue<uint32>(FieldData));
			return true;
		case EPlyScalarType::Float32:
			OutValue = ReadUnalignedValue<float>(FieldData);
			return true;
		case EPlyScalarType::Float64:
			OutValue = static_cast<float>(ReadUnalignedValue<double>(FieldData));
			return true;
		default:
			return false;
		}
	}

	bool DecodeGaussianPositionFromBytes(const FPlyGaussianReadLayout& Layout, const TArray<uint8>& VertexBytes, FVector3f& OutPosition)
	{
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
		ReadScalarAsFloatFromBytes(VertexBytes.GetData(), Layout.X, X);
		ReadScalarAsFloatFromBytes(VertexBytes.GetData(), Layout.Y, Y);
		ReadScalarAsFloatFromBytes(VertexBytes.GetData(), Layout.Z, Z);
		OutPosition = FVector3f(X, Y, Z);
		return true;
	}

	void DecodeGaussianRawFromBytes(const FPlyGaussianReadLayout& Layout, const TArray<uint8>& VertexBytes, FPlyGaussianRaw& OutRaw)
	{
		OutRaw = FPlyGaussianRaw();
		ReadScalarAsFloatFromBytes(VertexBytes.GetData(), Layout.X, OutRaw.X);
		ReadScalarAsFloatFromBytes(VertexBytes.GetData(), Layout.Y, OutRaw.Y);
		ReadScalarAsFloatFromBytes(VertexBytes.GetData(), Layout.Z, OutRaw.Z);
		ReadScalarAsFloatFromBytes(VertexBytes.GetData(), Layout.Scale0, OutRaw.Scale0);
		ReadScalarAsFloatFromBytes(VertexBytes.GetData(), Layout.Scale1, OutRaw.Scale1);
		ReadScalarAsFloatFromBytes(VertexBytes.GetData(), Layout.Scale2, OutRaw.Scale2);
		ReadScalarAsFloatFromBytes(VertexBytes.GetData(), Layout.Rot0, OutRaw.Rot0);
		ReadScalarAsFloatFromBytes(VertexBytes.GetData(), Layout.Rot1, OutRaw.Rot1);
		ReadScalarAsFloatFromBytes(VertexBytes.GetData(), Layout.Rot2, OutRaw.Rot2);
		ReadScalarAsFloatFromBytes(VertexBytes.GetData(), Layout.Rot3, OutRaw.Rot3);
		ReadScalarAsFloatFromBytes(VertexBytes.GetData(), Layout.Opacity, OutRaw.OpacityLogit);
		ReadScalarAsFloatFromBytes(VertexBytes.GetData(), Layout.Fdc0, OutRaw.Fdc0);
		ReadScalarAsFloatFromBytes(VertexBytes.GetData(), Layout.Fdc1, OutRaw.Fdc1);
		ReadScalarAsFloatFromBytes(VertexBytes.GetData(), Layout.Fdc2, OutRaw.Fdc2);
	}

	bool ReadHeader(FLargePlyFileReader& Reader, FString& OutHeader)
	{
		TArray<ANSICHAR> HeaderBytes;
		HeaderBytes.Reserve(4096);

		while (!Reader.AtEnd())
		{
			ANSICHAR Character = 0;
			if (!Reader.Read(&Character, 1))
			{
				return false;
			}

			HeaderBytes.Add(Character);
			if (Character == '\n')
			{
				FUTF8ToTCHAR HeaderSoFar(HeaderBytes.GetData(), HeaderBytes.Num());
				const FString HeaderString(HeaderSoFar.Length(), HeaderSoFar.Get());
				if (HeaderString.Contains(TEXT("\nend_header")) || HeaderString.StartsWith(TEXT("end_header")))
				{
					HeaderBytes.Add(0);
					FUTF8ToTCHAR ConvertedHeader(HeaderBytes.GetData());
					OutHeader = FString(ConvertedHeader.Length(), ConvertedHeader.Get());
					return true;
				}
			}

			if (HeaderBytes.Num() > 1024 * 1024)
			{
				return false;
			}
		}

		return false;
	}

	bool ParseHeader(const FString& Header, int64& OutVertexCount, TArray<FPlyProperty>& OutProperties, int32& OutSHDegree, FString& OutError)
	{
		OutVertexCount = 0;
		OutSHDegree = 0;
		OutProperties.Reset();

		if (!Header.StartsWith(TEXT("ply")))
		{
			OutError = TEXT("The file is not a PLY file.");
			return false;
		}

		if (!Header.Contains(TEXT("format binary_little_endian")))
		{
			OutError = TEXT("Only binary_little_endian PLY is supported by this first importer template.");
			return false;
		}

		TArray<FString> Lines;
		Header.ParseIntoArrayLines(Lines);

		bool bInVertexElement = false;
		int32 FRestCount = 0;

		for (const FString& Line : Lines)
		{
			TArray<FString> Tokens;
			Line.ParseIntoArrayWS(Tokens);
			if (Tokens.IsEmpty())
			{
				continue;
			}

			if (Tokens[0] == TEXT("element"))
			{
				bInVertexElement = Tokens.Num() >= 3 && Tokens[1] == TEXT("vertex");
				if (bInVertexElement)
				{
					OutVertexCount = FCString::Atoi64(*Tokens[2]);
				}
				continue;
			}

			if (bInVertexElement && Tokens[0] == TEXT("property"))
			{
				if (Tokens.Num() >= 3 && Tokens[1] != TEXT("list"))
				{
					FPlyProperty Property;
					Property.Type = ParseScalarType(Tokens[1]);
					Property.Name = Tokens[2];
					if (Property.Type == EPlyScalarType::Invalid)
					{
						OutError = FString::Printf(TEXT("Unsupported PLY scalar type '%s' for property '%s'."), *Tokens[1], *Tokens[2]);
						return false;
					}
					if (Property.Name.StartsWith(TEXT("f_rest_")))
					{
						++FRestCount;
					}
					OutProperties.Add(Property);
				}
				else
				{
					OutError = TEXT("List properties inside the vertex element are not supported.");
					return false;
				}
			}
		}

		if (OutVertexCount <= 0)
		{
			OutError = TEXT("PLY header does not contain a positive vertex count.");
			return false;
		}

		OutSHDegree = FRestCount >= 45 ? 3 : 0;
		return true;
	}

	float Sigmoid(float Value)
	{
		return 1.0f / (1.0f + FMath::Exp(-Value));
	}

	float DecodeGaussianScale(float LogScale)
	{
		if (!FMath::IsFinite(LogScale))
		{
			return TNumericLimits<float>::Max();
		}
		return FMath::Exp(FMath::Clamp(LogScale, -30.0f, 30.0f));
	}

	float ComputeGaussianMaxScale(const FPlyGaussianRaw& Raw)
	{
		return FMath::Max3(
			DecodeGaussianScale(Raw.Scale0),
			DecodeGaussianScale(Raw.Scale1),
			DecodeGaussianScale(Raw.Scale2));
	}

	float ComputeScaleQuantile(TArray<float>& SortedScales, float Quantile)
	{
		SortedScales.RemoveAll([](float Value)
		{
			return !FMath::IsFinite(Value) || Value <= 0.0f;
		});
		if (SortedScales.IsEmpty())
		{
			return 0.0f;
		}

		SortedScales.Sort();
		const float ClampedQuantile = FMath::Clamp(Quantile, 0.0f, 1.0f);
		const int32 Index = FMath::Clamp(
			FMath::CeilToInt(static_cast<float>(SortedScales.Num()) * ClampedQuantile) - 1,
			0,
			SortedScales.Num() - 1);
		return SortedScales[Index];
	}

	bool ReadGaussianVertexBytes(
		FLargePlyFileReader& Reader,
		const FPlyGaussianReadLayout& Layout,
		int64 VertexIndex,
		TArray<uint8>& VertexBytes,
		FString& OutError)
	{
		if (VertexBytes.Num() != Layout.VertexStride)
		{
			VertexBytes.SetNumUninitialized(Layout.VertexStride);
		}

		if (!Reader.Read(VertexBytes.GetData(), Layout.VertexStride))
		{
			OutError = FString::Printf(TEXT("Unexpected EOF while reading vertex %lld."), VertexIndex);
			return false;
		}

		return true;
	}

	bool ReadGaussianRaw(
		FLargePlyFileReader& Reader,
		const FPlyGaussianReadLayout& Layout,
		int64 VertexIndex,
		TArray<uint8>& VertexBytes,
		FPlyGaussianRaw& OutRaw,
		FString& OutError)
	{
		if (!ReadGaussianVertexBytes(Reader, Layout, VertexIndex, VertexBytes, OutError))
		{
			return false;
		}

		DecodeGaussianRawFromBytes(Layout, VertexBytes, OutRaw);
		return true;
	}

	FGaussianPointSplatSample BuildGaussianSample(const FPlyGaussianRaw& Raw)
	{
		FGaussianPointSplatSample Gaussian;
		Gaussian.Position = FVector3f(Raw.X, Raw.Y, Raw.Z);
		Gaussian.Scale = FVector3f(DecodeGaussianScale(Raw.Scale0), DecodeGaussianScale(Raw.Scale1), DecodeGaussianScale(Raw.Scale2));
		Gaussian.Rotation = FVector4f(Raw.Rot1, Raw.Rot2, Raw.Rot3, Raw.Rot0);
		Gaussian.Opacity = FMath::Clamp(Sigmoid(Raw.OpacityLogit), 0.0f, 1.0f);
		Gaussian.Color = FLinearColor(
			FMath::Clamp(0.5f + SH_C0 * Raw.Fdc0, 0.0f, 1.0f),
			FMath::Clamp(0.5f + SH_C0 * Raw.Fdc1, 0.0f, 1.0f),
			FMath::Clamp(0.5f + SH_C0 * Raw.Fdc2, 0.0f, 1.0f),
			Gaussian.Opacity);
		return Gaussian;
	}

	bool OpenPlyDataReader(
		const FString& Filename,
		TUniquePtr<FLargePlyFileReader>& OutReader,
		int64& OutVertexCount,
		TArray<FPlyProperty>& OutProperties,
		int32& OutSHDegree,
		FString& OutError)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		TUniquePtr<IFileHandle> FileHandle(PlatformFile.OpenRead(*Filename));
		if (!FileHandle)
		{
			OutError = FString::Printf(TEXT("Could not open '%s'."), *Filename);
			return false;
		}
		OutReader = MakeUnique<FLargePlyFileReader>(MoveTemp(FileHandle));
		if (!OutReader->IsValid())
		{
			OutError = FString::Printf(TEXT("Could not initialize a large-file reader for '%s'."), *Filename);
			return false;
		}

		FString Header;
		if (!ReadHeader(*OutReader, Header))
		{
			OutError = TEXT("Could not read PLY header.");
			return false;
		}

		return ParseHeader(Header, OutVertexCount, OutProperties, OutSHDegree, OutError);
	}

	int32 QuantizeVoxelAxis(float Value, float MinValue, float MaxValue, int32 Resolution)
	{
		const float Range = MaxValue - MinValue;
		if (Range <= KINDA_SMALL_NUMBER)
		{
			return 0;
		}

		const float Normalized = (Value - MinValue) / Range;
		return FMath::Clamp(FMath::FloorToInt(Normalized * static_cast<float>(Resolution)), 0, Resolution - 1);
	}

	uint64 Morton3DFromTile(int32 X, int32 Y, int32 Z)
	{
		uint64 Code = 0;
		for (int32 Bit = 0; Bit < 21; ++Bit)
		{
			Code |= (static_cast<uint64>((X >> Bit) & 1) << (3 * Bit + 2));
			Code |= (static_cast<uint64>((Y >> Bit) & 1) << (3 * Bit + 1));
			Code |= (static_cast<uint64>((Z >> Bit) & 1) << (3 * Bit));
		}
		return Code;
	}

	FIntVector ChooseSpatialTileDimensions(const FBox& Bounds, int32 TargetTileCount, int32 ForcedResolution)
	{
		static constexpr int32 MaxTileDimension = 1024;
		if (ForcedResolution > 0)
		{
			const int32 Resolution = FMath::Clamp(ForcedResolution, 1, MaxTileDimension);
			return FIntVector(Resolution, Resolution, Resolution);
		}

		const int64 ClampedTarget = FMath::Clamp<int64>(TargetTileCount, 1, static_cast<int64>(MaxTileDimension) * MaxTileDimension);
		FIntVector Dimensions(1, 1, 1);
		const FVector BoundsSize = Bounds.GetSize();
		const FVector Size(FMath::Abs(BoundsSize.X), FMath::Abs(BoundsSize.Y), FMath::Abs(BoundsSize.Z));
		while (static_cast<int64>(Dimensions.X) * Dimensions.Y * Dimensions.Z < ClampedTarget)
		{
			const double XScore = Dimensions.X < MaxTileDimension ? Size.X / static_cast<double>(Dimensions.X) : -1.0;
			const double YScore = Dimensions.Y < MaxTileDimension ? Size.Y / static_cast<double>(Dimensions.Y) : -1.0;
			const double ZScore = Dimensions.Z < MaxTileDimension ? Size.Z / static_cast<double>(Dimensions.Z) : -1.0;
			if (XScore < 0.0 && YScore < 0.0 && ZScore < 0.0)
			{
				break;
			}

			if (XScore >= YScore && XScore >= ZScore)
			{
				++Dimensions.X;
			}
			else if (YScore >= ZScore)
			{
				++Dimensions.Y;
			}
			else
			{
				++Dimensions.Z;
			}
		}
		return Dimensions;
	}

	FIntVector ComputeSpatialTileCoord(const FVector3f& Position, const FSpatialTileStats& Stats)
	{
		return FIntVector(
			QuantizeVoxelAxis(Position.X, Stats.Min.X, Stats.Max.X, FMath::Max(Stats.Dimensions.X, 1)),
			QuantizeVoxelAxis(Position.Y, Stats.Min.Y, Stats.Max.Y, FMath::Max(Stats.Dimensions.Y, 1)),
			QuantizeVoxelAxis(Position.Z, Stats.Min.Z, Stats.Max.Z, FMath::Max(Stats.Dimensions.Z, 1)));
	}

	uint64 ComputeSpatialTileKeyFromCoord(const FIntVector& Coord)
	{
		return Morton3DFromTile(Coord.X, Coord.Y, Coord.Z);
	}

	uint64 ComputeSpatialTileKey(const FVector3f& Position, const FSpatialTileStats& Stats)
	{
		return ComputeSpatialTileKeyFromCoord(ComputeSpatialTileCoord(Position, Stats));
	}

	double ComputeSampleOriginDistance(const FVector3f& Position, const FSpatialTileStats& Stats)
	{
		const FVector3f OriginExtent(
			FMath::Max(FMath::Abs(Stats.Min.X), FMath::Abs(Stats.Max.X)),
			FMath::Max(FMath::Abs(Stats.Min.Y), FMath::Abs(Stats.Max.Y)),
			FMath::Max(FMath::Abs(Stats.Min.Z), FMath::Abs(Stats.Max.Z)));
		const double X = OriginExtent.X > KINDA_SMALL_NUMBER ? FMath::Abs(static_cast<double>(Position.X) / OriginExtent.X) : 0.0;
		const double Y = OriginExtent.Y > KINDA_SMALL_NUMBER ? FMath::Abs(static_cast<double>(Position.Y) / OriginExtent.Y) : 0.0;
		const double Z = OriginExtent.Z > KINDA_SMALL_NUMBER ? FMath::Abs(static_cast<double>(Position.Z) / OriginExtent.Z) : 0.0;
		const double NormalizedDistance = FMath::Sqrt(X * X + Y * Y + Z * Z) / FMath::Sqrt(3.0);
		return FMath::Clamp(NormalizedDistance, 0.0, 1.0);
	}

	void SelectSpatialTiles(const TArray<FSpatialImportSample>& Samples, int64 CandidateCount, int64 TileCandidatePoolCount, FSpatialTileStats& InOutStats)
	{
		TMap<uint64, FSpatialTileCandidate> CandidateByKey;
		TArray<float> GlobalSampleScales;
		GlobalSampleScales.Reserve(Samples.Num());
		for (const FSpatialImportSample& Sample : Samples)
		{
			GlobalSampleScales.Add(Sample.MaxScale);
			const FIntVector TileCoord = ComputeSpatialTileCoord(Sample.Position, InOutStats);
			const uint64 TileKey = ComputeSpatialTileKeyFromCoord(TileCoord);
			FSpatialTileCandidate& Candidate = CandidateByKey.FindOrAdd(TileKey);
			if (Candidate.SampleCount == 0)
			{
				Candidate.TileKey = TileKey;
				Candidate.TileCoord = TileCoord;
			}
			++Candidate.SampleCount;
			Candidate.OriginDistance += ComputeSampleOriginDistance(Sample.Position, InOutStats);
			Candidate.MaxScaleSum += Sample.MaxScale;
			Candidate.OpacitySum += Sample.Opacity;
		}

		InOutStats.OccupiedTileCount = CandidateByKey.Num();
		if (CandidateByKey.IsEmpty())
		{
			return;
		}

		TArray<FSpatialTileCandidate> Candidates;
		Candidates.Reserve(CandidateByKey.Num());
		int32 MaxSampleCount = 0;
		for (TPair<uint64, FSpatialTileCandidate>& CandidatePair : CandidateByKey)
		{
			FSpatialTileCandidate& Candidate = CandidatePair.Value;
			Candidate.OriginDistance = Candidate.SampleCount > 0 ? Candidate.OriginDistance / static_cast<double>(Candidate.SampleCount) : 1.0;
			MaxSampleCount = FMath::Max(MaxSampleCount, Candidate.SampleCount);
		}

		const double OriginWeight = FMath::Max(0.0f, CVarGPSImportSpatialTileCenterWeight.GetValueOnAnyThread());
		const double ScalePenalty = FMath::Max(0.0f, CVarGPSImportSpatialTileScalePenalty.GetValueOnAnyThread());
		const double ReferenceScale = FMath::Max(static_cast<double>(ComputeScaleQuantile(GlobalSampleScales, 0.5f)), 0.000001);
		for (TPair<uint64, FSpatialTileCandidate>& CandidatePair : CandidateByKey)
		{
			FSpatialTileCandidate& Candidate = CandidatePair.Value;
			const double MeanMaxScale = Candidate.SampleCount > 0 ? Candidate.MaxScaleSum / static_cast<double>(Candidate.SampleCount) : ReferenceScale;
			const double MeanOpacity = Candidate.SampleCount > 0 ? Candidate.OpacitySum / static_cast<double>(Candidate.SampleCount) : 1.0;
			const double RelativeScale = MeanMaxScale / ReferenceScale;
			const double ScaleQuality = 1.0 / (1.0 + ScalePenalty * RelativeScale);
			const double OpacityQuality = 0.25 + 0.75 * FMath::Clamp(MeanOpacity, 0.0, 1.0);
			Candidate.Score = static_cast<double>(Candidate.SampleCount) * ScaleQuality * OpacityQuality / (1.0 + OriginWeight * Candidate.OriginDistance * Candidate.OriginDistance);
			Candidates.Add(Candidate);
		}

		Candidates.Sort([](const FSpatialTileCandidate& A, const FSpatialTileCandidate& B)
		{
			if (!FMath::IsNearlyEqual(A.Score, B.Score))
			{
				return A.Score > B.Score;
			}
			return A.TileKey < B.TileKey;
		});

		const int32 ForcedSelectedTileCount = FMath::Max(0, CVarGPSImportSpatialTileSelectedTileCount.GetValueOnAnyThread());
		const bool bImportAllTiles = ForcedSelectedTileCount <= 0 && (TileCandidatePoolCount <= 0 || CandidateCount <= 0 || TileCandidatePoolCount >= CandidateCount);
		const int32 TargetSelectedTileCount = ForcedSelectedTileCount > 0
			? FMath::Min(ForcedSelectedTileCount, Candidates.Num())
			: (bImportAllTiles ? Candidates.Num() : 0);
		const int32 TargetSelectedSampleCount = !bImportAllTiles && ForcedSelectedTileCount <= 0
			? FMath::Max(1, FMath::CeilToInt(static_cast<double>(Samples.Num()) * static_cast<double>(TileCandidatePoolCount) / static_cast<double>(CandidateCount)))
			: 0;

		if (bImportAllTiles)
		{
			for (const FSpatialTileCandidate& Candidate : Candidates)
			{
				InOutStats.SelectedTileKeys.Add(Candidate.TileKey);
				++InOutStats.SelectedTileCount;
				InOutStats.SelectedTileSampleCount += Candidate.SampleCount;
			}
			return;
		}

		const double CoreDensityRatio = FMath::Clamp(CVarGPSImportSpatialTileCoreDensityRatio.GetValueOnAnyThread(), 0.0f, 1.0f);
		const int32 MinCoreSampleCount = FMath::Max(1, FMath::FloorToInt(static_cast<double>(MaxSampleCount) * CoreDensityRatio));
		const FSpatialTileCandidate* SeedCandidate = nullptr;
		for (const FSpatialTileCandidate& Candidate : Candidates)
		{
			if (Candidate.SampleCount < MinCoreSampleCount)
			{
				continue;
			}
			if (!SeedCandidate
				|| Candidate.OriginDistance < SeedCandidate->OriginDistance
				|| (FMath::IsNearlyEqual(Candidate.OriginDistance, SeedCandidate->OriginDistance) && Candidate.Score > SeedCandidate->Score))
			{
				SeedCandidate = &Candidate;
			}
		}

		if (!SeedCandidate && !Candidates.IsEmpty())
		{
			SeedCandidate = &Candidates[0];
		}
		if (!SeedCandidate)
		{
			return;
		}

		TSet<uint64> FrontierKeys;
		auto AddSelectedTile = [&InOutStats, &FrontierKeys](const FSpatialTileCandidate& Candidate, const FSpatialTileStats& Stats)
		{
			InOutStats.SelectedTileKeys.Add(Candidate.TileKey);
			++InOutStats.SelectedTileCount;
			InOutStats.SelectedTileSampleCount += Candidate.SampleCount;

			for (int32 DX = -1; DX <= 1; ++DX)
			{
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					for (int32 DZ = -1; DZ <= 1; ++DZ)
					{
						if (DX == 0 && DY == 0 && DZ == 0)
						{
							continue;
						}

						const FIntVector NeighborCoord(Candidate.TileCoord.X + DX, Candidate.TileCoord.Y + DY, Candidate.TileCoord.Z + DZ);
						if (NeighborCoord.X < 0 || NeighborCoord.Y < 0 || NeighborCoord.Z < 0
							|| NeighborCoord.X >= Stats.Dimensions.X
							|| NeighborCoord.Y >= Stats.Dimensions.Y
							|| NeighborCoord.Z >= Stats.Dimensions.Z)
						{
							continue;
						}
						FrontierKeys.Add(ComputeSpatialTileKeyFromCoord(NeighborCoord));
					}
				}
			}
		};

		AddSelectedTile(*SeedCandidate, InOutStats);
		while (true)
		{
			if (TargetSelectedTileCount > 0 && InOutStats.SelectedTileCount >= TargetSelectedTileCount)
			{
				break;
			}
			if (TargetSelectedSampleCount > 0 && InOutStats.SelectedTileSampleCount >= TargetSelectedSampleCount)
			{
				break;
			}

			const FSpatialTileCandidate* BestFrontierCandidate = nullptr;
			uint64 BestFrontierKey = 0;
			for (uint64 FrontierKey : FrontierKeys)
			{
				if (InOutStats.SelectedTileKeys.Contains(FrontierKey))
				{
					continue;
				}

				const FSpatialTileCandidate* Candidate = CandidateByKey.Find(FrontierKey);
				if (!Candidate || Candidate->SampleCount < MinCoreSampleCount)
				{
					continue;
				}

				if (!BestFrontierCandidate
					|| Candidate->Score > BestFrontierCandidate->Score
					|| (FMath::IsNearlyEqual(Candidate->Score, BestFrontierCandidate->Score) && Candidate->OriginDistance < BestFrontierCandidate->OriginDistance))
				{
					BestFrontierCandidate = Candidate;
					BestFrontierKey = FrontierKey;
				}
			}

			if (!BestFrontierCandidate)
			{
				break;
			}

			FrontierKeys.Remove(BestFrontierKey);
			AddSelectedTile(*BestFrontierCandidate, InOutStats);
		}
	}

	void ReserveLoadedGaussians(FPlyGaussianLoadResult& OutResult, int64 ExpectedImportedCount, int32 OutputChunkSize, bool bStreamChunks)
	{
		OutResult.ImportOutputChunkSize = OutputChunkSize;
		if (OutputChunkSize <= 0)
		{
			OutResult.Gaussians.Reserve(static_cast<int32>(ExpectedImportedCount));
			return;
		}

		if (bStreamChunks)
		{
			const int32 InitialChunkReserve = static_cast<int32>(FMath::Min<int64>(ExpectedImportedCount, OutputChunkSize));
			OutResult.Gaussians.Reserve(InitialChunkReserve);
			return;
		}

		const int64 ExpectedChunkCount64 = FMath::DivideAndRoundUp(ExpectedImportedCount, static_cast<int64>(OutputChunkSize));
		const int32 ExpectedChunkCount = static_cast<int32>(FMath::Min<int64>(ExpectedChunkCount64, TNumericLimits<int32>::Max()));
		OutResult.GaussianChunks.Reserve(FMath::Max(ExpectedChunkCount, 1));
	}

	bool FlushLoadedGaussianChunk(FPlyGaussianLoadResult& OutResult, FPlyGaussianChunkCallback* OnChunkLoaded)
	{
		if (!OnChunkLoaded || OutResult.ImportOutputChunkSize <= 0 || OutResult.Gaussians.IsEmpty())
		{
			return true;
		}

		const int32 ChunkIndex = OutResult.OutputChunkCount;
		TArray<FGaussianPointSplatSample> ChunkGaussians = MoveTemp(OutResult.Gaussians);
		OutResult.Gaussians.Reset();
		OutResult.Gaussians.Reserve(OutResult.ImportOutputChunkSize);
		if (!(*OnChunkLoaded)(ChunkIndex, MoveTemp(ChunkGaussians)))
		{
			OutResult.Error = FString::Printf(TEXT("Failed to create imported Gaussian chunk %d."), ChunkIndex);
			return false;
		}

		++OutResult.OutputChunkCount;
		return true;
	}

	bool AppendLoadedGaussian(FPlyGaussianLoadResult& OutResult, FGaussianPointSplatSample&& Gaussian, FPlyGaussianChunkCallback* OnChunkLoaded)
	{
		if (OutResult.ImportOutputChunkSize <= 0)
		{
			OutResult.Gaussians.Add(MoveTemp(Gaussian));
			++OutResult.ImportedGaussianCount;
			return true;
		}

		if (OnChunkLoaded)
		{
			OutResult.Gaussians.Add(MoveTemp(Gaussian));
			++OutResult.ImportedGaussianCount;
			if (OutResult.Gaussians.Num() >= OutResult.ImportOutputChunkSize)
			{
				return FlushLoadedGaussianChunk(OutResult, OnChunkLoaded);
			}
			return true;
		}

		if (OutResult.GaussianChunks.IsEmpty() || OutResult.GaussianChunks.Last().Num() >= OutResult.ImportOutputChunkSize)
		{
			TArray<FGaussianPointSplatSample>& NewChunk = OutResult.GaussianChunks.AddDefaulted_GetRef();
			NewChunk.Reserve(OutResult.ImportOutputChunkSize);
		}

		OutResult.GaussianChunks.Last().Add(MoveTemp(Gaussian));
		++OutResult.ImportedGaussianCount;
		OutResult.OutputChunkCount = OutResult.GaussianChunks.Num();
		return true;
	}

	bool CollectSpatialTileStats(
		const FString& Filename,
		int64 VertexCount,
		int32 ImportStride,
		int64 CandidateCount,
		int64 ExpectedImportedCount,
		int32 OutputChunkSize,
		FSpatialTileStats& OutStats,
		FString& OutError)
	{
		TUniquePtr<FLargePlyFileReader> Reader;
		int64 ParsedVertexCount = 0;
		int32 ParsedSHDegree = 0;
		TArray<FPlyProperty> Properties;
		if (!OpenPlyDataReader(Filename, Reader, ParsedVertexCount, Properties, ParsedSHDegree, OutError))
		{
			return false;
		}

		if (ParsedVertexCount != VertexCount)
		{
			OutError = TEXT("PLY vertex count changed between spatial tile passes.");
			return false;
		}

		FPlyGaussianReadLayout ReadLayout;
		if (!BuildGaussianReadLayout(Properties, ReadLayout, OutError))
		{
			return false;
		}

		const int32 MaxStatsSamples = FMath::Max(CVarGPSImportSpatialTileMaxStatsSamples.GetValueOnAnyThread(), 1024);
		const int64 StatStride = FMath::Max<int64>((CandidateCount + MaxStatsSamples - 1) / MaxStatsSamples, 1);
		TArray<FSpatialImportSample> Samples;
		Samples.Reserve(static_cast<int32>(FMath::Min<int64>(CandidateCount, MaxStatsSamples)));
		TArray<uint8> VertexBytes;
		VertexBytes.SetNumUninitialized(ReadLayout.VertexStride);

		int64 CandidateIndex = 0;
		for (int64 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
		{
			if (!ReadGaussianVertexBytes(*Reader, ReadLayout, VertexIndex, VertexBytes, OutError))
			{
				return false;
			}

			if ((VertexIndex % ImportStride) != 0)
			{
				continue;
			}

			if ((CandidateIndex % StatStride) == 0)
			{
				FPlyGaussianRaw Raw;
				DecodeGaussianRawFromBytes(ReadLayout, VertexBytes, Raw);

				FSpatialImportSample& Sample = Samples.AddDefaulted_GetRef();
				Sample.Position = FVector3f(Raw.X, Raw.Y, Raw.Z);
				Sample.MaxScale = ComputeGaussianMaxScale(Raw);
				Sample.Opacity = FMath::Clamp(Sigmoid(Raw.OpacityLogit), 0.0f, 1.0f);
			}
			++CandidateIndex;
		}

		if (Samples.IsEmpty())
		{
			OutError = TEXT("Not enough samples to build spatial tile import bounds.");
			return false;
		}

		FBox SampleBounds(EForceInit::ForceInit);
		for (const FSpatialImportSample& Sample : Samples)
		{
			SampleBounds += FVector(Sample.Position);
		}

		if (!SampleBounds.IsValid)
		{
			OutError = TEXT("Invalid sampled bounds for spatial tile import.");
			return false;
		}

		const int32 ForcedResolution = FMath::Clamp(CVarGPSImportSpatialTileResolution.GetValueOnAnyThread(), 0, 1024);
		const int32 AutoTileMultiplier = FMath::Max(1, CVarGPSImportSpatialTileAutoMultiplier.GetValueOnAnyThread());
		const bool bUseLargeGaussianFilter = CVarGPSImportFilterLargeGaussians.GetValueOnAnyThread() != 0 && ExpectedImportedCount < CandidateCount;
		const double CandidatePoolMultiplier = bUseLargeGaussianFilter
			? FMath::Max(1.0f, CVarGPSImportFilterCandidatePoolMultiplier.GetValueOnAnyThread())
			: 1.0;
		const double TileCandidatePoolCountD = static_cast<double>(ExpectedImportedCount) * CandidatePoolMultiplier;
		const int64 TileCandidatePoolCount = FMath::Min<int64>(
			CandidateCount,
			static_cast<int64>(TileCandidatePoolCountD + 0.999999));
		const int64 TargetOutputChunkCount64 = OutputChunkSize > 0
			? FMath::DivideAndRoundUp(TileCandidatePoolCount, static_cast<int64>(OutputChunkSize))
			: 1;
		const int64 TargetTileCount64 = TargetOutputChunkCount64 * AutoTileMultiplier;
		const int32 TargetTileCount = static_cast<int32>(FMath::Clamp<int64>(TargetTileCount64, 1, 1024 * 1024));
		OutStats.Min = FVector3f(SampleBounds.Min);
		OutStats.Max = FVector3f(SampleBounds.Max);
		OutStats.Dimensions = ChooseSpatialTileDimensions(SampleBounds, TargetTileCount, ForcedResolution);
		OutStats.Resolution = FMath::Max(FMath::Max(OutStats.Dimensions.X, OutStats.Dimensions.Y), OutStats.Dimensions.Z);
		OutStats.StatSampleCount = Samples.Num();
		SelectSpatialTiles(Samples, CandidateCount, TileCandidatePoolCount, OutStats);
		if (OutStats.SelectedTileKeys.IsEmpty())
		{
			OutError = TEXT("No spatial tiles were selected for import.");
			return false;
		}
		if (bUseLargeGaussianFilter)
		{
			TArray<float> SelectedScales;
			SelectedScales.Reserve(Samples.Num());
			for (const FSpatialImportSample& Sample : Samples)
			{
				if (OutStats.SelectedTileKeys.Contains(ComputeSpatialTileKey(Sample.Position, OutStats)))
				{
					SelectedScales.Add(Sample.MaxScale);
				}
			}

			const float ScaleQuantile = ComputeScaleQuantile(SelectedScales, CVarGPSImportFilterMaxScaleQuantile.GetValueOnAnyThread());
			const float ScaleMultiplier = FMath::Max(1.0f, CVarGPSImportFilterMaxScaleMultiplier.GetValueOnAnyThread());
			OutStats.LargeGaussianMaxScale = ScaleQuantile > 0.0f ? ScaleQuantile * ScaleMultiplier : 0.0f;
			OutStats.bUseLargeGaussianFilter = OutStats.LargeGaussianMaxScale > 0.0f;
		}
		OutStats.bValid = true;
		return true;
	}
	bool LoadBinaryLittleEndianInternal(const FString& Filename, FPlyGaussianLoadResult& OutResult, FPlyGaussianChunkCallback* OnChunkLoaded)
	{
		OutResult = FPlyGaussianLoadResult();

		int64 VertexCount = 0;
		int32 SHDegree = 0;
		TArray<FPlyProperty> Properties;
		TUniquePtr<FLargePlyFileReader> Reader;
		if (!OpenPlyDataReader(Filename, Reader, VertexCount, Properties, SHDegree, OutResult.Error))
		{
			return false;
		}

		FPlyGaussianReadLayout ReadLayout;
		if (!BuildGaussianReadLayout(Properties, ReadLayout, OutResult.Error))
		{
			return false;
		}
		TArray<uint8> VertexBytes;
		VertexBytes.SetNumUninitialized(ReadLayout.VertexStride);

		OutResult.SourceVertexCount = VertexCount;
		OutResult.SHDegree = SHDegree;
		const int32 ImportStride = FMath::Max(1, CVarGPSImportStride.GetValueOnAnyThread());
		const int32 MaxGaussians = FMath::Max(0, CVarGPSImportMaxGaussians.GetValueOnAnyThread());
		const int32 ConfiguredSampleMode = FMath::Clamp(CVarGPSImportSampleMode.GetValueOnAnyThread(), GPSImportSampleModeSequential, GPSImportSampleModeSpatialTile);
		const bool bSpatialTileSample = ConfiguredSampleMode == GPSImportSampleModeSpatialTile;
		const int32 ConfiguredOutputChunkSize = FMath::Max(0, CVarGPSImportOutputChunkSize.GetValueOnAnyThread());
		const int32 OutputChunkSize = bSpatialTileSample
			? (ConfiguredOutputChunkSize > 0 ? ConfiguredOutputChunkSize : 2000000)
			: ConfiguredOutputChunkSize;
		const int64 CandidateCount = (VertexCount + ImportStride - 1) / ImportStride;
		OutResult.ImportStride = ImportStride;
		OutResult.ImportMaxGaussians = MaxGaussians;
		OutResult.ImportSampleMode = ConfiguredSampleMode;
		OutResult.ImportOutputChunkSize = OutputChunkSize;
		OutResult.ImportSpatialTileResolution = bSpatialTileSample ? FMath::Clamp(CVarGPSImportSpatialTileResolution.GetValueOnAnyThread(), 0, 1024) : 0;
		OutResult.bSpatialTileSample = bSpatialTileSample;

		const int64 ExpectedImportedCount = MaxGaussians > 0 ? FMath::Min<int64>(MaxGaussians, CandidateCount) : CandidateCount;
		if (ExpectedImportedCount > TNumericLimits<int32>::Max() && OutputChunkSize <= 0)
		{
			OutResult.Error = FString::Printf(
				TEXT("Import would create %lld gaussians in one asset, which exceeds UE TArray's int32 index range. Set gps.Import.MaxGaussians or keep gps.Import.OutputChunkSize above 0."),
				ExpectedImportedCount);
			return false;
		}
		ReserveLoadedGaussians(OutResult, ExpectedImportedCount, OutputChunkSize, OnChunkLoaded != nullptr);

		FSpatialTileStats SpatialTileStats;
		if (bSpatialTileSample)
		{
			if (!CollectSpatialTileStats(Filename, VertexCount, ImportStride, CandidateCount, ExpectedImportedCount, OutputChunkSize, SpatialTileStats, OutResult.Error))
			{
				return false;
			}

			OutResult.ImportSpatialTileResolution = SpatialTileStats.Resolution;
			OutResult.ImportSpatialTileDimensions = SpatialTileStats.Dimensions;
			OutResult.ImportSpatialTileStatSampleCount = SpatialTileStats.StatSampleCount;
			OutResult.ImportSpatialTileCount = SpatialTileStats.OccupiedTileCount;
			OutResult.ImportSpatialTileSelectedCount = SpatialTileStats.SelectedTileCount;
			OutResult.ImportSpatialTileSelectedSampleCount = SpatialTileStats.SelectedTileSampleCount;
			OutResult.ImportLargeGaussianMaxScale = SpatialTileStats.LargeGaussianMaxScale;
			OutResult.bImportLargeGaussianFilter = SpatialTileStats.bUseLargeGaussianFilter;

			Reader.Reset();
			int64 ParsedVertexCount = 0;
			int32 ParsedSHDegree = 0;
			if (!OpenPlyDataReader(Filename, Reader, ParsedVertexCount, Properties, ParsedSHDegree, OutResult.Error))
			{
				return false;
			}
			if (ParsedVertexCount != VertexCount)
			{
				OutResult.Error = TEXT("PLY vertex count changed between spatial tile passes.");
				return false;
			}
		}

		int64 ImportedCount = 0;
		int64 CandidateIndex = 0;
		for (int64 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
		{
			if (bSpatialTileSample && MaxGaussians > 0 && ImportedCount >= MaxGaussians)
			{
				break;
			}

			if (bSpatialTileSample)
			{
				if (!ReadGaussianVertexBytes(*Reader, ReadLayout, VertexIndex, VertexBytes, OutResult.Error))
				{
					return false;
				}

				if ((VertexIndex % ImportStride) != 0)
				{
					continue;
				}

				FVector3f Position = FVector3f::ZeroVector;
				DecodeGaussianPositionFromBytes(ReadLayout, VertexBytes, Position);

				const uint64 TileKey = ComputeSpatialTileKey(Position, SpatialTileStats);
				if (!SpatialTileStats.SelectedTileKeys.Contains(TileKey))
				{
					++OutResult.ImportSpatialTileRejectedCandidateCount;
					++CandidateIndex;
					continue;
				}

				FPlyGaussianRaw Raw;
				DecodeGaussianRawFromBytes(ReadLayout, VertexBytes, Raw);
				if (SpatialTileStats.bUseLargeGaussianFilter && ComputeGaussianMaxScale(Raw) > SpatialTileStats.LargeGaussianMaxScale)
				{
					++OutResult.ImportLargeGaussianRejectedCandidateCount;
					++CandidateIndex;
					continue;
				}

				FGaussianPointSplatSample Gaussian = BuildGaussianSample(Raw);
				if (!AppendLoadedGaussian(OutResult, MoveTemp(Gaussian), OnChunkLoaded))
				{
					return false;
				}
				++ImportedCount;
				++CandidateIndex;
				if (MaxGaussians > 0 && ImportedCount >= MaxGaussians)
				{
					break;
				}
				continue;
			}

			FPlyGaussianRaw Raw;
			if (!ReadGaussianRaw(*Reader, ReadLayout, VertexIndex, VertexBytes, Raw, OutResult.Error))
			{
				return false;
			}

			if ((VertexIndex % ImportStride) == 0)
			{
				bool bShouldImport = MaxGaussians == 0;
				if (MaxGaussians > 0)
				{
					if (CandidateCount > MaxGaussians)
					{
						const int64 TargetImportedCount = ((CandidateIndex + 1) * MaxGaussians) / CandidateCount;
						bShouldImport = ImportedCount < TargetImportedCount;
					}
					else
					{
						bShouldImport = ImportedCount < MaxGaussians;
					}
				}

				if (bShouldImport)
				{
					FGaussianPointSplatSample Gaussian = BuildGaussianSample(Raw);
					if (!AppendLoadedGaussian(OutResult, MoveTemp(Gaussian), OnChunkLoaded))
					{
						return false;
					}
					++ImportedCount;
					if (MaxGaussians > 0 && ImportedCount >= MaxGaussians)
					{
						break;
					}
				}

				++CandidateIndex;
			}
		}

		return FlushLoadedGaussianChunk(OutResult, OnChunkLoaded);
	}
}

bool FPlyGaussianLoader::LoadBinaryLittleEndian(const FString& Filename, FPlyGaussianLoadResult& OutResult)
{
	return LoadBinaryLittleEndianInternal(Filename, OutResult, nullptr);
}

bool FPlyGaussianLoader::LoadBinaryLittleEndianStreamed(const FString& Filename, FPlyGaussianLoadResult& OutResult, FPlyGaussianChunkCallback OnChunkLoaded)
{
	return LoadBinaryLittleEndianInternal(Filename, OutResult, &OnChunkLoaded);
}
