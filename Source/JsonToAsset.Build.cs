using UnrealBuildTool;

public class JsonToAsset : ModuleRules
{
	public JsonToAsset(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"BlueprintGraph",
			"Json",
			"JsonUtilities",
			"Kismet",
			"UnrealEd"
		});
	}
}
