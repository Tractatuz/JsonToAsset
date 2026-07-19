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
			"AnimGraph",
			"BlueprintGraph",
			"Json",
			"JsonUtilities",
			"Kismet",
			"TaskEvidence",
			"UnrealEd"
		});
	}
}
