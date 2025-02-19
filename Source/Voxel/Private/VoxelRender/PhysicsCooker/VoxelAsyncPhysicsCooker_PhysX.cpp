// Copyright 2021 Phyronnaz

#include "VoxelRender/PhysicsCooker/VoxelAsyncPhysicsCooker_PhysX.h"
#include "VoxelRender/VoxelProcMeshBuffers.h"
#include "VoxelRender/VoxelProceduralMeshComponent.h"
#include "VoxelPhysXHelpers.h"
#include "VoxelWorldRootComponent.h"


#include "PhysicsPublic.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsSettings.h"

#if ENGINE_MAJOR_VERSION < 5
#include "IPhysXCookingModule.h"
inline IPhysXCooking* GetPhysXCooking()
{
	static IPhysXCookingModule* PhysXCookingModule = nullptr;
	if (!PhysXCookingModule)
	{
		PhysXCookingModule = GetPhysXCookingModule();
	}
	return PhysXCookingModule->GetPhysXCooking();
}

static const FName PhysXFormat = FPlatformProperties::GetPhysicsFormat();

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

FVoxelAsyncPhysicsCooker_PhysX::FVoxelAsyncPhysicsCooker_PhysX(UVoxelProceduralMeshComponent* Component)
	: IVoxelAsyncPhysicsCooker(Component)
	, PhysXCooking(GetPhysXCooking())
{
}

class UMRMeshComponent
{
public:
	static void FinishCreatingPhysicsMeshes(UBodySetup& Body, const TArray<physx::PxConvexMesh*>& ConvexMeshes, const TArray<physx::PxConvexMesh*>& ConvexMeshesNegX, const TArray<physx::PxTriangleMesh*>& TriMeshes)
	{
		Body.FinishCreatingPhysicsMeshes_PhysX(ConvexMeshes, ConvexMeshesNegX, TriMeshes);
	}
};

bool FVoxelAsyncPhysicsCooker_PhysX::Finalize(
	UBodySetup& BodySetup,
	TVoxelSharedPtr<FVoxelSimpleCollisionData>& OutSimpleCollisionData,
	FVoxelProceduralMeshComponentMemoryUsage& OutMemoryUsage)
{
	VOXEL_FUNCTION_COUNTER();
	
	if (ErrorCounter.GetValue() > 0)
	{
		return false;
	}

	{
		VOXEL_SCOPE_COUNTER("FinishCreatingPhysicsMeshes");
		UMRMeshComponent::FinishCreatingPhysicsMeshes(BodySetup, {}, {}, CookResult.TriangleMeshes);
	}

	OutSimpleCollisionData = CookResult.SimpleCollisionData;
	OutMemoryUsage.TriangleMeshes = CookResult.TriangleMeshesMemoryUsage;

	return true;
}

void FVoxelAsyncPhysicsCooker_PhysX::CookMesh()
{
	if (CollisionTraceFlag != ECollisionTraceFlag::CTF_UseComplexAsSimple)
	{
		CreateSimpleCollision();
	}
	if (CollisionTraceFlag != ECollisionTraceFlag::CTF_UseSimpleAsComplex)
	{
		CreateTriMesh();
	}
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

void FVoxelAsyncPhysicsCooker_PhysX::CreateTriMesh()
{
	VOXEL_ASYNC_FUNCTION_COUNTER();

	TArray<FVector> Vertices;
	TArray<FTriIndices> Indices;
	TArray<uint16> MaterialIndices;

	// Copy data from buffers
	{
		VOXEL_ASYNC_SCOPE_COUNTER("Copy data from buffers");

		{
			int32 NumIndices = 0;
			int32 NumVertices = 0;
			for (auto& Buffer : Buffers)
			{
				NumIndices += Buffer->GetNumIndices();
				NumVertices += Buffer->GetNumVertices();
			}
			VOXEL_ASYNC_SCOPE_COUNTER("Reserve");
			Vertices.Reserve(NumVertices);
			Indices.Reserve(NumIndices);
			MaterialIndices.Reserve(NumIndices);
		}

		int32 VertexOffset = 0;
		for (int32 SectionIndex = 0; SectionIndex < Buffers.Num(); SectionIndex++)
		{
			auto& Buffer = *Buffers[SectionIndex];
			const auto Get = [](auto& Array, int32 Index) -> auto&
			{
#if VOXEL_DEBUG
				return Array[Index];
#else
				return Array.GetData()[Index];
#endif
			};

			// Copy vertices
			{
				auto& PositionBuffer = Buffer.VertexBuffers.PositionVertexBuffer;

				const int32 Offset = Vertices.Num();
				check(PositionBuffer.GetNumVertices() <= uint32(Vertices.GetSlack()));
				Vertices.AddUninitialized(PositionBuffer.GetNumVertices());

				VOXEL_ASYNC_SCOPE_COUNTER("Copy vertices");
				for (uint32 Index = 0; Index < PositionBuffer.GetNumVertices(); Index++)
				{
					Get(Vertices, Offset + Index) = PositionBuffer.VertexPosition(Index);
				}
			}

			// Copy triangle data
			{
				auto& IndexBuffer = Buffer.IndexBuffer;

				ensure(Indices.Num() == MaterialIndices.Num());
				const int32 Offset = Indices.Num();
				ensure(IndexBuffer.GetNumIndices() % 3 == 0);
				const int32 NumTriangles = IndexBuffer.GetNumIndices() / 3;

				check(NumTriangles <= Indices.GetSlack());
				check(NumTriangles <= MaterialIndices.GetSlack());
				Indices.AddUninitialized(NumTriangles);
				MaterialIndices.AddUninitialized(NumTriangles);

				{
					VOXEL_ASYNC_SCOPE_COUNTER("Copy triangles");
					const auto Lambda = [&](const auto* RESTRICT Data)
					{
						for (int32 Index = 0; Index < NumTriangles; Index++)
						{
							// Need to add base offset for indices
							FTriIndices TriIndices;
							TriIndices.v0 = Data[3 * Index + 0] + VertexOffset;
							TriIndices.v1 = Data[3 * Index + 1] + VertexOffset;
							TriIndices.v2 = Data[3 * Index + 2] + VertexOffset;
							checkVoxelSlow(3 * Index + 2 < IndexBuffer.GetNumIndices());
							Get(Indices, Offset + Index) = TriIndices;
						}
					};
					if (IndexBuffer.Is32Bit())
					{
						Lambda(IndexBuffer.GetData_32());
					}
					else
					{
						Lambda(IndexBuffer.GetData_16());
					}
				}
				// Also store material info
				{
					VOXEL_ASYNC_SCOPE_COUNTER("Copy material info");
					for (int32 Index = 0; Index < NumTriangles; Index++)
					{
						Get(MaterialIndices, Offset + Index) = SectionIndex;
					}
				}
			}

			VertexOffset = Vertices.Num();
		}
	}

	physx::PxTriangleMesh* TriangleMesh = nullptr;

	constexpr bool bFlipNormals = true; // Always true due to the order of the vertices (clock wise vs not)
	const bool bSuccess = PhysXCooking->CreateTriMesh(
		PhysXFormat,
		GetCookFlags(),
		Vertices,
		Indices,
		MaterialIndices,
		bFlipNormals,
		TriangleMesh);
	
	CookResult.TriangleMeshes.Add(TriangleMesh);

	if (TriangleMesh)
	{
		CookResult.TriangleMeshesMemoryUsage += FVoxelPhysXHelpers::GetAllocatedSize(*TriangleMesh);
	}
	
	if (!bSuccess)
	{
		// Happens sometimes
		LOG_VOXEL(Warning, TEXT("Failed to cook TriMesh. Num vertices: %d; Num triangles: %d"), Vertices.Num(), Indices.Num());
		ErrorCounter.Increment();
	}
}

void FVoxelAsyncPhysicsCooker_PhysX::CreateSimpleCollision()
{
	VOXEL_ASYNC_FUNCTION_COUNTER();
	
	if (Buffers.Num() == 1 && Buffers[0]->GetNumVertices() < 4) return;

	CookResult.SimpleCollisionData = MakeVoxelShared<FVoxelSimpleCollisionData>();

	FVoxelSimpleCollisionData& SimpleCollisionData = *CookResult.SimpleCollisionData;
	SimpleCollisionData.Bounds = FBox(ForceInit);

	if (bSimpleCubicCollision)
	{
	    VOXEL_ASYNC_SCOPE_COUNTER("BoxElems");
        TArray<FKBoxElem>& BoxElems = SimpleCollisionData.BoxElems;
        for (auto& Buffer : Buffers)
        {
            for (FBox Cube : Buffer->CollisionCubes)
            {
                Cube = Cube.TransformBy(LocalToRoot);
                SimpleCollisionData.Bounds += Cube;

                FKBoxElem& BoxElem = BoxElems.Emplace_GetRef();

				BoxElem.Center = Cube.GetCenter();
				BoxElem.X = Cube.GetExtent().X * 2;
				BoxElem.Y = Cube.GetExtent().Y * 2;
				BoxElem.Z = Cube.GetExtent().Z * 2;
			}
		}
	}
	else
    {
		VOXEL_ASYNC_SCOPE_COUNTER("ConvexElems");
        TArray<FKConvexElem>& ConvexElems = SimpleCollisionData.ConvexElems;

        FBox Box(ForceInit);
        for (auto& Buffer : Buffers)
        {
            auto& PositionBuffer = Buffer->VertexBuffers.PositionVertexBuffer;
            for (uint32 Index = 0; Index < PositionBuffer.GetNumVertices(); Index++)
            {
                Box += PositionBuffer.VertexPosition(Index);
            }
        }

        const int32 ChunkSize = MESHER_CHUNK_SIZE << LOD;
        const FIntVector Size =
            FVoxelUtilities::ComponentMax(
                FIntVector(1),
                FVoxelUtilities::CeilToInt(Box.GetSize() / ChunkSize * NumConvexHullsPerAxis));

        if (!ensure(Size.GetMax() <= 64)) return;

        ConvexElems.SetNum(Size.X * Size.Y * Size.Z);

        for (auto& Buffer : Buffers)
        {
            auto& PositionBuffer = Buffer->VertexBuffers.PositionVertexBuffer;
            for (uint32 Index = 0; Index < PositionBuffer.GetNumVertices(); Index++)
            {
                const FVector Vertex = PositionBuffer.VertexPosition(Index);

                FIntVector MainPosition;
                const auto Lambda = [&](int32 OffsetX, int32 OffsetY, int32 OffsetZ)
                {
                    const FVector Offset = FVector(OffsetX, OffsetY, OffsetZ) * (1 << LOD); // 1 << LOD: should be max distance between the vertices
                    FIntVector Position = FVoxelUtilities::FloorToInt((Vertex + Offset - Box.Min) / ChunkSize * NumConvexHullsPerAxis);
                    Position = FVoxelUtilities::Clamp(Position, FIntVector(0), Size - 1);

                    // Avoid adding too many duplicates by checking we're not in the center
                    if (OffsetX == 0 && OffsetY == 0 && OffsetZ == 0)
                    {
                        MainPosition = Position;
                    }
                    else
                    {
                        if (Position == MainPosition)
                        {
                            return;
                        }
                    }
                    ConvexElems[Position.X + Size.X * Position.Y + Size.X * Size.Y * Position.Z].VertexData.Add(Vertex);
                };
                Lambda(0, 0, 0);
                // Iterate possible neighbors to avoid holes between hulls
                Lambda(+1, 0, 0);
                Lambda(-1, 0, 0);
                Lambda(0, +1, 0);
                Lambda(0, -1, 0);
                Lambda(0, 0, +1);
                Lambda(0, 0, -1);
            }
        }

        constexpr int32 Threshold = 8;

        // Merge the hulls until they are big enough
        // This moves the vertices to the end
        for (int32 Index = 0; Index < ConvexElems.Num() - 1; Index++)
        {
            auto& Element = ConvexElems[Index];
            if (Element.VertexData.Num() < Threshold)
            {
                ConvexElems[Index + 1].VertexData.Append(Element.VertexData);
                Element.VertexData.Reset();
            }
        }

        // Remove all empty hulls
        ConvexElems.RemoveAll([](auto& Element) { return Element.VertexData.Num() == 0; });
        if (!ensure(ConvexElems.Num() > 0)) return;

        // Then merge backwards while the last hull isn't big enough
        while (ConvexElems.Last().VertexData.Num() < Threshold && ConvexElems.Num() > 1)
        {
            ConvexElems[ConvexElems.Num() - 2].VertexData.Append(ConvexElems.Last().VertexData);
            ConvexElems.Pop();
        }
		
        // Transform from component space to root component space, as the root is going to hold the convex meshes,
		// and update bounds
        for (auto& Element : ConvexElems)
        {
            ensure(Element.VertexData.Num() >= 4);
            for (auto& Vertex : Element.VertexData)
            {
                Vertex = LocalToRoot.TransformPosition(Vertex);
            }
            Element.UpdateElemBox();
            SimpleCollisionData.Bounds += Element.ElemBox;
        }

		// Finally, create the physx data
	    for (FKConvexElem& Element : ConvexElems)
	    {
			VOXEL_ASYNC_SCOPE_COUNTER("CreateConvex");
	    	
			PxConvexMesh* Mesh = nullptr;
		    const EPhysXCookingResult Result = PhysXCooking->CreateConvex(PhysXFormat, GetCookFlags(), Element.VertexData, Mesh);
	    	
		    switch (Result)
		    {
		    case EPhysXCookingResult::Failed:
		    {
			    LOG_VOXEL(Warning, TEXT("Failed to cook convex"));
			    ErrorCounter.Increment();
			    break;
		    }
		    case EPhysXCookingResult::SucceededWithInflation:
		    {
			    LOG_VOXEL(Warning, TEXT("Cook convex failed but succeeded with inflation"));
			    break;
		    }
		    case EPhysXCookingResult::Succeeded: break;
		    default: ensure(false);
		    }

	    	SimpleCollisionData.ConvexMeshes.Add(Mesh);
	    }
    }
}

EPhysXMeshCookFlags FVoxelAsyncPhysicsCooker_PhysX::GetCookFlags() const
{
	EPhysXMeshCookFlags CookFlags = EPhysXMeshCookFlags::Default;
	if (!bCleanCollisionMesh)
	{
		CookFlags |= EPhysXMeshCookFlags::DeformableMesh;
	}
	// TODO try and bench CookFlags |= EPhysXMeshCookFlags::DisableActiveEdgePrecompute;
	// TODO: option/check perf
	CookFlags |= EPhysXMeshCookFlags::FastCook;
	return CookFlags;
}
#endif