#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "JsonToAssetLibrary.generated.h"

UCLASS()
class JSONTOASSET_API UJsonToAssetLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Applies JsonToAsset patch JSON from a string to an existing Blueprint asset. */
	UFUNCTION(BlueprintCallable, Category="JSON To Asset", meta=(AdvancedDisplay="bSaveAsset,bCompileBlueprint,bApplyGraphChanges,bAllowStructuralChanges"))
	static FString ApplyBlueprintVisualScriptJsonString(
		const FString& JsonString,
		bool bSaveAsset = true,
		bool bCompileBlueprint = true,
		bool bApplyGraphChanges = true,
		bool bAllowStructuralChanges = false);

	/** Applies JsonToAsset patch JSON from a file to an existing Blueprint asset. */
	UFUNCTION(BlueprintCallable, Category="JSON To Asset", meta=(AdvancedDisplay="bSaveAsset,bCompileBlueprint,bApplyGraphChanges,bAllowStructuralChanges"))
	static FString ApplyBlueprintVisualScriptJsonFile(
		const FString& JsonFilePath,
		bool bSaveAsset = true,
		bool bCompileBlueprint = true,
		bool bApplyGraphChanges = true,
		bool bAllowStructuralChanges = false);
};
