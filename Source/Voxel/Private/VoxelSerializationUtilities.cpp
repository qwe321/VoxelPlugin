// Copyright 2021 Phyronnaz

#include "VoxelUtilities/VoxelSerializationUtilities.h"
#include "VoxelMaterial.h"
#include "VoxelMinimal.h"
#include "VoxelSettings.h"

#include "Serialization/LargeMemoryWriter.h"

THIRD_PARTY_INCLUDES_START
#include "ThirdParty/zlib/1.2.12/include/zlib.h"
THIRD_PARTY_INCLUDES_END

void FVoxelSerializationUtilities::CompressData(
	const uint8* const UncompressedData, 
	const int64 UncompressedDataNum, 
	TArray<uint8>& OutCompressedData,
	EVoxelCompressionLevel::Type InCompressionLevel)
{
	VOXEL_ASYNC_FUNCTION_COUNTER();
	
	const double TotalStartTime = FPlatformTime::Seconds();

	if (UncompressedDataNum == 0 || !ensure(UncompressedData))
	{
		OutCompressedData.Empty();
		return;
	}

	const auto GetCompressionLevel = [&]()
	{
		int32 CompressionLevel = InCompressionLevel;
		if (CompressionLevel == EVoxelCompressionLevel::VoxelDefault)
		{
			CompressionLevel = GetDefault<UVoxelSettings>()->DefaultCompressionLevel;
		}
		CompressionLevel = FMath::Clamp(CompressionLevel, -1, 9);
		static_assert(Z_NO_COMPRESSION == 0, "");
		static_assert(Z_BEST_COMPRESSION == 9, "");
		return CompressionLevel;
	};
	const int32 CompressionLevel = GetCompressionLevel();

	const int32 NumChunks = FVoxelUtilities::DivideCeil64(UncompressedDataNum, MaxChunkSize);
	check(0 < NumChunks && NumChunks < MaxNumChunks);

	struct FChunk
	{
		int64 Start = 0;
		int64 Size = 0;
		uLong CompressedSize = 0;
	};
	TArray<FChunk, TFixedAllocator<MaxNumChunks>> Chunks;

	// Fill chunks
	for (int32 ChunkIndex = 0; ChunkIndex < NumChunks; ChunkIndex++)
	{
		auto& NewChunk = Chunks.Emplace_GetRef();
		NewChunk.Start = ChunkIndex * MaxChunkSize;
		NewChunk.Size = FMath::Min(MaxChunkSize, UncompressedDataNum - ChunkIndex * MaxChunkSize);
	}
	check(Chunks.Last().Start + Chunks.Last().Size == UncompressedDataNum);

	// Compute estimated compressed size
	int64 TotalCompressedSizeBound = 0;
	for (auto& Chunk : Chunks)
	{
		Chunk.CompressedSize = compressBound(Chunk.Size);
		TotalCompressedSizeBound += Chunk.CompressedSize;
	}

	// Allocate memory
	TArray64<uint8> CompressedData;
	CompressedData.SetNumUninitialized(TotalCompressedSizeBound);

	FHeader Header;

	int64 TotalCompressedSize = 0;
	double CompressionTime = 0;

	// Compress chunks
	for (int32 ChunkIndex = 0; ChunkIndex < NumChunks; ChunkIndex++)
	{
		FChunk& Chunk = Chunks[ChunkIndex];
		
		const double StartTime = FPlatformTime::Seconds();
		const auto Result = compress2(CompressedData.GetData() + TotalCompressedSize, &Chunk.CompressedSize, UncompressedData + Chunk.Start, Chunk.Size, CompressionLevel);
		const double EndTime = FPlatformTime::Seconds();
		
		if (!ensureMsgf(Result == Z_OK, TEXT("Compression failed: %d"), Result))
		{
			CompressedData.Reset();
			return;
		}

		CompressionTime += EndTime - StartTime;
		TotalCompressedSize += Chunk.CompressedSize;

		Header.ChunksCompressedSize[ChunkIndex] = Chunk.CompressedSize;
	}
	check(TotalCompressedSize <= TotalCompressedSizeBound);
	checkf(TotalCompressedSize < MAX_int32 - sizeof(FHeader), TEXT("Compressed data overflow: %lld"), TotalCompressedSize);

	// Fill header
	Header.CompressedSize = TotalCompressedSize;
	Header.UncompressedSize = UncompressedDataNum;
	Header.NumChunks = NumChunks;

	// Write final data
	OutCompressedData.SetNumUninitialized(sizeof(FHeader) + TotalCompressedSize);
	FMemory::Memcpy(OutCompressedData.GetData(), &Header, sizeof(FHeader));
	FMemory::Memcpy(OutCompressedData.GetData() + sizeof(FHeader), CompressedData.GetData(), TotalCompressedSize);

	// Log time
	
	const double TotalEndTime = FPlatformTime::Seconds();
	
	const double UncompressedSizeMB = double(UncompressedDataNum) / double(1 << 20);
	const double CompressedSizeMB = double(TotalCompressedSize) / double(1 << 20);

	const double TotalTime = TotalEndTime - TotalStartTime;
	
	LOG_VOXEL(Log, TEXT("Compressed %f MB in %fs (%f MB/s). Compressed Size: %f MB (%f%%). Compression: %fs (%f%%). Num Chunks: %d."), 
		UncompressedSizeMB, 
		TotalTime, 
		UncompressedSizeMB / TotalTime, 
		CompressedSizeMB,
		100 * CompressedSizeMB / UncompressedSizeMB,
		CompressionTime,
		100 * CompressionTime / TotalTime,
		NumChunks);
}

void FVoxelSerializationUtilities::CompressData(FLargeMemoryWriter& UncompressedData, TArray<uint8>& CompressedData, EVoxelCompressionLevel::Type CompressionLevel)
{
	// Tell and not TotalSize: TotalSize returns the total memory allocated by the writer, which might be bigger if AllocatedMemory is too big
	CompressData(UncompressedData.GetData(), UncompressedData.Tell(), CompressedData, CompressionLevel);
}

bool FVoxelSerializationUtilities::DecompressData(const TArray<uint8>& CompressedData, TArray64<uint8>& UncompressedData)
{
	VOXEL_ASYNC_FUNCTION_COUNTER();
	
	const double TotalStartTime = FPlatformTime::Seconds();
	
	if (CompressedData.Num() == 0)
	{
		UncompressedData.Empty();
		return false;
	}

	int32 Flag;
	FMemory::Memcpy(&Flag, CompressedData.GetData(), sizeof(Flag));

	if (Flag == -1)
	{
		// New 64 bit archive

		if (!ensure(CompressedData.Num() >= sizeof(FHeader)))
		{
			UncompressedData.Empty();
			return false;
		}
		
		FHeader Header;
		FMemory::Memcpy(&Header, CompressedData.GetData(), sizeof(FHeader));

		check(Header.LegacyFlag == -1);
		if (!ensureMsgf(Header.Magic == FHeader().Magic, TEXT("Magic was %x"), Header.Magic))
		{
			UncompressedData.Empty();
			return false;
		}
		
		if (!ensureMsgf(Header.CompressedSize == CompressedData.Num() - sizeof(FHeader), TEXT("Archive is saying its size is %lld, but it's %lld"), Header.CompressedSize, CompressedData.Num() - sizeof(FHeader)))
		{
			UncompressedData.Empty();
			return false;
		}
		
		if (!ensureMsgf(Header.NumChunks <= MaxNumChunks, TEXT("Header.NumChunks was %u"), Header.NumChunks))
		{
			UncompressedData.Empty();
			return false;
		}

		// Allocate memory
		UncompressedData.SetNumUninitialized(Header.UncompressedSize);

		int64 TotalCompressedSize = 0;
		int64 TotalUncompressedSize = 0;

		double DecompressionTime = 0;

		// Decompress all chunks
		for (uint32 ChunkIndex = 0; ChunkIndex < Header.NumChunks; ChunkIndex++)
		{
			const uint32 ChunkCompressedSize = Header.ChunksCompressedSize[ChunkIndex];
			if (!ensureMsgf(TotalCompressedSize + ChunkCompressedSize <= Header.CompressedSize, TEXT("Decompression overflow: Compressed size = %lld, Already processed = %lld, Chunk = %u"),
				Header.CompressedSize, TotalCompressedSize, ChunkCompressedSize))
			{
				UncompressedData.Empty();
				return false;
			}

			uLong UncompressedSize = FMath::Min<int64>(MaxChunkSize, Header.UncompressedSize - TotalUncompressedSize);

			const double StartTime = FPlatformTime::Seconds();
			const auto Result = uncompress(
				UncompressedData.GetData() + TotalUncompressedSize, &UncompressedSize, 
				CompressedData.GetData() + sizeof(FHeader) + TotalCompressedSize, ChunkCompressedSize);
			const double EndTime = FPlatformTime::Seconds();

			if (!ensureMsgf(Result == Z_OK, TEXT("Decompression failed: %d"), Result))
			{
				UncompressedData.Empty();
				return false;
			}

			TotalCompressedSize += ChunkCompressedSize;
			TotalUncompressedSize += UncompressedSize;

			DecompressionTime += EndTime - StartTime;
		}

		if (!ensureMsgf(TotalCompressedSize == Header.CompressedSize, TEXT("Compressed size mismatch: read %lld, but %lld in header"), TotalCompressedSize, Header.CompressedSize))
		{
			UncompressedData.Empty();
			return false;
		}
		if (!ensureMsgf(TotalUncompressedSize == Header.UncompressedSize, TEXT("Uncompressed size mismatch: read %lld, but %lld in header"), TotalUncompressedSize, Header.UncompressedSize))
		{
			UncompressedData.Empty();
			return false;
		}

		// Log

		const double TotalEndTime = FPlatformTime::Seconds();
		
		const double UncompressedSizeMB = double(TotalUncompressedSize) / double(1 << 20);
		const double CompressedSizeMB = double(TotalCompressedSize) / double(1 << 20);

		const double TotalTime = TotalEndTime - TotalStartTime;
	
		LOG_VOXEL(Log, TEXT("Decompressed %f MB in %fs (%f MB/s). Compressed Size: %f MB (%f%%). Decompression: %fs (%f%%). Num Chunks: %d."),
			UncompressedSizeMB,
			TotalTime,
			UncompressedSizeMB / TotalTime,
			CompressedSizeMB,
			100 * CompressedSizeMB / UncompressedSizeMB,
			DecompressionTime,
			100 * DecompressionTime / TotalTime,
			Header.NumChunks);

		return true;
	}
	else
	{
		const ECompressionFlags CompressionFlags = ECompressionFlags(CompressedData.Last());

		int32 UncompressedSize;
		FMemory::Memcpy(&UncompressedSize, CompressedData.GetData(), sizeof(UncompressedSize));
		UncompressedData.SetNum(UncompressedSize);
		const uint8* CompressionStart = CompressedData.GetData() + sizeof(UncompressedSize);
		const int32 CompressionSize = CompressedData.Num() - 1 - sizeof(UncompressedSize);

		bool bSuccess = false;
		ECompressionFlags NewCompressionFlags = (ECompressionFlags)(CompressionFlags & COMPRESS_OptionsFlagsMask);
		switch (CompressionFlags & COMPRESS_DeprecatedFormatFlagsMask)
		{
		case COMPRESS_ZLIB:
			bSuccess = FCompression::UncompressMemory(NAME_Zlib, UncompressedData.GetData(), UncompressedSize, CompressionStart, CompressionSize, NewCompressionFlags);
			break;
		case COMPRESS_GZIP:
			bSuccess = FCompression::UncompressMemory(NAME_Gzip, UncompressedData.GetData(), UncompressedSize, CompressionStart, CompressionSize, NewCompressionFlags);
			break;
		case COMPRESS_Custom:
			bSuccess = FCompression::UncompressMemory(TEXT("Oodle"), UncompressedData.GetData(), UncompressedSize, CompressionStart, CompressionSize, NewCompressionFlags);
			break;
		default:
			ensure(false);
		}

		return bSuccess;
	}
}

void FVoxelSerializationUtilities::TestCompression(int64 Size, EVoxelCompressionLevel::Type CompressionLevel)
{
	LOG_VOXEL(Log, TEXT("Testing compression on %fMB"), double(Size) / double(1 << 20));
	
	TArray64<uint8> Data;
	Data.SetNumUninitialized(Size);

	const FRandomStream Random(0);
	for (int64 Index = 0; Index < Data.Num(); Index++)
	{
		Data[Index] = Random.GetUnsignedInt();
	}

	TArray<uint8> CompressedData;
	CompressData(Data.GetData(), Data.Num(), CompressedData, CompressionLevel);

	TArray64<uint8> UncompressedData;
	DecompressData(CompressedData, UncompressedData);

	check(Data.Num() == UncompressedData.Num());

	for (int64 Index = 0; Index < Data.Num(); Index++)
	{
		check(Data[Index] == UncompressedData[Index]);
	}
}