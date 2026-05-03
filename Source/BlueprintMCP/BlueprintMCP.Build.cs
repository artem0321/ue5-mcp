using UnrealBuildTool;

public class BlueprintMCP : ModuleRules
{
	public BlueprintMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"BlueprintGraph",
			"Json",
			"JsonUtilities",
			"HTTPServer",
			"Sockets",
			"Networking"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AssetRegistry",
			"AssetTools",
			"Kismet",
			"KismetCompiler",
			"EditorSubsystem",
			"MaterialEditor",
			"AnimGraph",
			"AnimGraphRuntime",
			"RHI",
			"Slate",
			"UMG",
			"UMGEditor",
			"SlateCore"
		});

		// PythonScriptPlugin is UncookedOnly; we are an Editor module so this is fine.
		// Used by HandleRunPython to capture stdout/stderr/exception via ExecPythonCommandEx.
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("PythonScriptPlugin");
		}
	}
}
