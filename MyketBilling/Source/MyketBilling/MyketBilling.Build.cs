using UnrealBuildTool;
using System.IO;

public class MyketBilling : ModuleRules
{
	public MyketBilling(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities",
			"UMG",
			"Slate",
			"SlateCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects"
		});

		if (Target.Platform == UnrealTargetPlatform.Android)
		{
			PrivateDependencyModuleNames.Add("Launch");

			// Hook the UPL (Unreal Plugin Language) file so the Gradle project gets:
			//  - the ir.mservices.market.BILLING permission
			//  - the JitPack repository + myket-billing-client dependency
			//  - our Java bridge class copied into the Android src tree
			//  - onActivityResult / onCreate / onDestroy hooks into GameActivity.java
			string PluginRootDir = Path.Combine(ModuleDirectory, "..", "..");
			string UPLFilePath = Path.Combine(PluginRootDir, "MyketBilling_UPL.xml");

			AdditionalPropertiesForReceipt.Add("AndroidPlugin", UPLFilePath);
		}
	}
}
