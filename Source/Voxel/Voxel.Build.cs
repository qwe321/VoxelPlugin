// Copyright 2021 Phyronnaz

using System.IO;
using UnrealBuildTool;

public class Voxel : ModuleRules
{
    public Voxel(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bEnforceIWYU = true;
        bLegacyPublicIncludePaths = false;

#if UE_4_24_OR_LATER
#else
#endif

        PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));
        PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private"));

        // For raytracing
        PrivateIncludePaths.Add(EngineDirectory + "/Shaders/Shared");
        // For HLSL translator
        PrivateIncludePaths.Add(EngineDirectory + "/Source/Runtime/Engine/Private");

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "PhysicsCore",
                "Networking",
                "Sockets",
                "RHI",
                "RenderCore",
                "Landscape",
                "Projects",
#if UE_4_26_OR_LATER
                "DeveloperSettings",
                "TraceLog",
#endif
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "nvTessLib",
                "HTTP",
                "Projects",
                "Slate",
                "SlateCore",
                //"VHACD", // Not used, too slow
            }
        );

        SetupModulePhysicsSupport(Target);

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PrivateDependencyModuleNames.Add("ForsythTriOptimizer");
        }
        PrivateDependencyModuleNames.Add("zlib");

        if (Target.Configuration == UnrealTargetConfiguration.DebugGame ||
			Target.Configuration == UnrealTargetConfiguration.Debug)
        {
            PublicDefinitions.Add("VOXEL_DEBUG=1");
        }

        PublicDefinitions.Add("VOXEL_PLUGIN_NAME=TEXT(\"VoxelFree\")");
    }
}
