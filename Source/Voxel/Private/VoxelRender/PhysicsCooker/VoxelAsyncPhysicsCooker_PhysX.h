// Copyright 2021 Phyronnaz

#pragma once

#include "CoreMinimal.h"
#include "VoxelAsyncPhysicsCooker.h"

#if ENGINE_MAJOR_VERSION < 5
#include "IPhysXCooking.h"
struct FKConvexElem;
class IPhysXCooking;

class FVoxelAsyncPhysicsCooker_PhysX : public IVoxelAsyncPhysicsCooker
{
	GENERATED_VOXEL_ASYNC_WORK_BODY(FVoxelAsyncPhysicsCooker_PhysX)

public:
	explicit FVoxelAsyncPhysicsCooker_PhysX(UVoxelProceduralMeshComponent* Component);

private:
	//~ Begin IVoxelAsyncPhysicsCooker Interface
	virtual bool Finalize(
		UBodySetup& BodySetup,
		TVoxelSharedPtr<FVoxelSimpleCollisionData>& OutSimpleCollisionData,
		FVoxelProceduralMeshComponentMemoryUsage& OutMemoryUsage) override;
	virtual void CookMesh() override;
	//~ End IVoxelAsyncPhysicsCooker Interface
	
private:
	void CreateTriMesh();
	void CreateSimpleCollision();
	EPhysXMeshCookFlags GetCookFlags() const;

	IPhysXCooking* const PhysXCooking;
	FThreadSafeCounter ErrorCounter;

	struct FCookResult
	{
		TVoxelSharedPtr<FVoxelSimpleCollisionData> SimpleCollisionData;

	    TArray<physx::PxTriangleMesh*> TriangleMeshes;
		uint64 TriangleMeshesMemoryUsage = 0;
	};
	FCookResult CookResult;
};
#endif