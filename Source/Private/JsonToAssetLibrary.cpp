#include "JsonToAssetLibrary.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDeviceNull.h"
#include "Misc/PackageName.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

namespace
{
	constexpr const TCHAR* JsonToAssetPatchSchema = TEXT("ue.json_to_asset.patch.v1");
	constexpr const TCHAR* JsonToAssetResultSchema = TEXT("ue.json_to_asset.result.v1");

	struct FJsonToAssetContext
	{
		TArray<FString> Changes;
		TArray<FString> Warnings;
		bool bAnyChange = false;

		void AddChange(const FString& Message)
		{
			Changes.Add(Message);
			bAnyChange = true;
		}

		void AddWarning(const FString& Message)
		{
			Warnings.Add(Message);
		}
	};

	FString MakeResultJson(bool bOk, const FString& Error, const FJsonToAssetContext& Context)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), JsonToAssetResultSchema);
		Root->SetBoolField(TEXT("ok"), bOk);
		Root->SetStringField(TEXT("error"), Error);

		TArray<TSharedPtr<FJsonValue>> Changes;
		for (const FString& Change : Context.Changes)
		{
			Changes.Add(MakeShared<FJsonValueString>(Change));
		}
		Root->SetArrayField(TEXT("changes"), Changes);
		Root->SetNumberField(TEXT("change_count"), Context.Changes.Num());

		TArray<TSharedPtr<FJsonValue>> Warnings;
		for (const FString& Warning : Context.Warnings)
		{
			Warnings.Add(MakeShared<FJsonValueString>(Warning));
		}
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetNumberField(TEXT("warning_count"), Context.Warnings.Num());

		FString JsonString;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonString);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return JsonString;
	}

	FString NormalizeObjectPath(const FString& AssetPath)
	{
		const FString TrimmedPath = AssetPath.TrimStartAndEnd();

		if (!TrimmedPath.Contains(TEXT(".")) && FPackageName::IsValidLongPackageName(TrimmedPath))
		{
			return FString::Printf(TEXT("%s.%s"), *TrimmedPath, *FPackageName::GetLongPackageAssetName(TrimmedPath));
		}

		return TrimmedPath;
	}

	bool ParseRootObject(const FString& JsonString, TSharedPtr<FJsonObject>& OutRoot, FString& OutError)
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
		if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
		{
			OutError = FString::Printf(TEXT("Failed to parse JSON: %s"), *Reader->GetErrorMessage());
			return false;
		}

		return true;
	}

	bool TryGetObjectField(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedPtr<FJsonObject>* ObjectField = nullptr;
		if (!JsonObject.IsValid() || !JsonObject->TryGetObjectField(FStringView(FieldName), ObjectField) || !ObjectField || !ObjectField->IsValid())
		{
			return false;
		}

		OutObject = *ObjectField;
		return true;
	}

	FProperty* FindPropertyByName(UClass* Class, const FString& PropertyName)
	{
		if (!Class || PropertyName.IsEmpty())
		{
			return nullptr;
		}

		const FName PropertyFName(*PropertyName);
		for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (Property && Property->GetFName() == PropertyFName)
			{
				return Property;
			}
		}

		return nullptr;
	}

	bool ExportPropertyValue(UObject* Object, FProperty* Property, FString& OutValue)
	{
		if (!Object || !Property)
		{
			return false;
		}

		Property->ExportText_InContainer(0, OutValue, Object, nullptr, Object, PPF_None);
		return true;
	}

	void ApplyDetailsProperties(UObject* Object, const TSharedPtr<FJsonObject>& DefaultsJson, const FString& ObjectLabel, FJsonToAssetContext& Context)
	{
		if (!Object || !DefaultsJson.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!DefaultsJson->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
		{
			const TSharedPtr<FJsonObject>* PropertyJson = nullptr;
			if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyJson) || !PropertyJson || !PropertyJson->IsValid())
			{
				continue;
			}

			FString PropertyName;
			FString NewValue;
			if (!(*PropertyJson)->TryGetStringField(TEXT("name"), PropertyName) || !(*PropertyJson)->TryGetStringField(TEXT("value"), NewValue))
			{
				continue;
			}

			FProperty* Property = FindPropertyByName(Object->GetClass(), PropertyName);
			if (!Property)
			{
				Context.AddWarning(FString::Printf(TEXT("Property not found on %s: %s"), *ObjectLabel, *PropertyName));
				continue;
			}

			FString OldValue;
			ExportPropertyValue(Object, Property, OldValue);
			if (OldValue == NewValue)
			{
				continue;
			}

			Object->Modify();
			FOutputDeviceNull ErrorOutput;
			const TCHAR* ImportResult = Property->ImportText_InContainer(*NewValue, Object, Object, PPF_None, &ErrorOutput);
			if (!ImportResult)
			{
				Context.AddWarning(FString::Printf(TEXT("Failed to import %s.%s from value: %s"), *ObjectLabel, *PropertyName, *NewValue));
				continue;
			}

			FString ImportedValue;
			ExportPropertyValue(Object, Property, ImportedValue);
			if (ImportedValue != OldValue)
			{
				Context.AddChange(FString::Printf(TEXT("Updated %s.%s"), *ObjectLabel, *PropertyName));
			}
		}
	}

	USCS_Node* FindSCSNodeByJson(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ComponentJson)
	{
		if (!Blueprint || !Blueprint->SimpleConstructionScript || !ComponentJson.IsValid())
		{
			return nullptr;
		}

		FString VariableName;
		ComponentJson->TryGetStringField(TEXT("variable_name"), VariableName);

		FString ComponentTemplatePath;
		ComponentJson->TryGetStringField(TEXT("component_template"), ComponentTemplatePath);

		const TArray<USCS_Node*>& AllNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
		for (USCS_Node* Node : AllNodes)
		{
			if (!Node)
			{
				continue;
			}

			if (!VariableName.IsEmpty() && Node->GetVariableName().ToString() == VariableName)
			{
				return Node;
			}

			if (!ComponentTemplatePath.IsEmpty() && Node->ComponentTemplate && Node->ComponentTemplate->GetPathName() == ComponentTemplatePath)
			{
				return Node;
			}
		}

		return nullptr;
	}

	void ApplyComponentJson(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ComponentJson, FJsonToAssetContext& Context)
	{
		USCS_Node* Node = FindSCSNodeByJson(Blueprint, ComponentJson);
		if (!Node)
		{
			FString VariableName;
			if (ComponentJson.IsValid())
			{
				ComponentJson->TryGetStringField(TEXT("variable_name"), VariableName);
			}
			Context.AddWarning(FString::Printf(TEXT("Component node not found: %s"), *VariableName));
		}
		else
		{
			TSharedPtr<FJsonObject> DefaultsJson;
			if (TryGetObjectField(ComponentJson, TEXT("defaults"), DefaultsJson))
			{
				ApplyDetailsProperties(Node->ComponentTemplate, DefaultsJson, FString::Printf(TEXT("component %s"), *Node->GetVariableName().ToString()), Context);
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (ComponentJson.IsValid() && ComponentJson->TryGetArrayField(TEXT("children"), Children) && Children)
		{
			for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
			{
				const TSharedPtr<FJsonObject>* ChildJson = nullptr;
				if (ChildValue.IsValid() && ChildValue->TryGetObject(ChildJson) && ChildJson && ChildJson->IsValid())
				{
					ApplyComponentJson(Blueprint, *ChildJson, Context);
				}
			}
		}
	}

	FGuid ParseGuid(const FString& GuidString)
	{
		FGuid Guid;
		FGuid::Parse(GuidString, Guid);
		return Guid;
	}

	UEdGraph* FindGraphByJson(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& GraphJson)
	{
		if (!Blueprint || !GraphJson.IsValid())
		{
			return nullptr;
		}

		FString GraphName;
		GraphJson->TryGetStringField(TEXT("name"), GraphName);

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph && Graph->GetName() == GraphName)
			{
				return Graph;
			}
		}

		return nullptr;
	}

	UEdGraphNode* FindNodeByJson(UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeJson)
	{
		if (!Graph || !NodeJson.IsValid())
		{
			return nullptr;
		}

		FString NodeId;
		NodeJson->TryGetStringField(TEXT("id"), NodeId);
		const FGuid NodeGuid = ParseGuid(NodeId);

		FString NodeName;
		NodeJson->TryGetStringField(TEXT("name"), NodeName);

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			if (NodeGuid.IsValid() && Node->NodeGuid == NodeGuid)
			{
				return Node;
			}

			if (!NodeName.IsEmpty() && Node->GetName() == NodeName)
			{
				return Node;
			}
		}

		return nullptr;
	}

	UEdGraphPin* FindPinByJson(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& PinJson)
	{
		if (!Node || !PinJson.IsValid())
		{
			return nullptr;
		}

		FString PinId;
		PinJson->TryGetStringField(TEXT("id"), PinId);
		const FGuid PinGuid = ParseGuid(PinId);

		FString PinName;
		PinJson->TryGetStringField(TEXT("name"), PinName);

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}

			if (PinGuid.IsValid() && Pin->PinId == PinGuid)
			{
				return Pin;
			}

			if (!PinName.IsEmpty() && Pin->PinName.ToString() == PinName)
			{
				return Pin;
			}
		}

		return nullptr;
	}

	UEdGraphPin* FindPinByRef(UEdGraph* Graph, const TSharedPtr<FJsonObject>& PinRefJson)
	{
		if (!Graph || !PinRefJson.IsValid())
		{
			return nullptr;
		}

		FString NodeId;
		PinRefJson->TryGetStringField(TEXT("node_id"), NodeId);
		const FGuid NodeGuid = ParseGuid(NodeId);

		FString PinId;
		PinRefJson->TryGetStringField(TEXT("pin_id"), PinId);
		const FGuid PinGuid = ParseGuid(PinId);

		FString PinName;
		PinRefJson->TryGetStringField(TEXT("pin_name"), PinName);

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || (NodeGuid.IsValid() && Node->NodeGuid != NodeGuid))
			{
				continue;
			}

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}

				if (PinGuid.IsValid() && Pin->PinId == PinGuid)
				{
					return Pin;
				}

				if (!PinGuid.IsValid() && !PinName.IsEmpty() && Pin->PinName.ToString() == PinName)
				{
					return Pin;
				}
			}
		}

		return nullptr;
	}

	bool TryGetIntField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, int32& OutValue)
	{
		if (!JsonObject.IsValid())
		{
			return false;
		}

		double NumberValue = 0.0;
		if (!JsonObject->TryGetNumberField(FieldName, NumberValue))
		{
			return false;
		}

		OutValue = static_cast<int32>(NumberValue);
		return true;
	}

	void ApplyPinDefaults(UEdGraph* Graph, UEdGraphNode* Node, UEdGraphPin* Pin, const TSharedPtr<FJsonObject>& PinJson, FJsonToAssetContext& Context)
	{
		if (!Graph || !Node || !Pin || !PinJson.IsValid())
		{
			return;
		}

		const UEdGraphSchema* Schema = Graph->GetSchema();
		FString DefaultValue;
		if (PinJson->TryGetStringField(TEXT("default_value"), DefaultValue) && Pin->DefaultValue != DefaultValue)
		{
			Node->Modify();
			if (Schema)
			{
				Schema->TrySetDefaultValue(*Pin, DefaultValue);
			}
			else
			{
				Pin->DefaultValue = DefaultValue;
			}
			Context.AddChange(FString::Printf(TEXT("Updated pin default %s.%s"), *Node->GetName(), *Pin->PinName.ToString()));
		}

		FString DefaultTextValue;
		if (PinJson->TryGetStringField(TEXT("default_text_value"), DefaultTextValue) && Pin->DefaultTextValue.ToString() != DefaultTextValue)
		{
			Node->Modify();
			if (Schema)
			{
				Schema->TrySetDefaultText(*Pin, FText::FromString(DefaultTextValue));
			}
			else
			{
				Pin->DefaultTextValue = FText::FromString(DefaultTextValue);
			}
			Context.AddChange(FString::Printf(TEXT("Updated pin text default %s.%s"), *Node->GetName(), *Pin->PinName.ToString()));
		}

		FString DefaultObjectPath;
		if (PinJson->TryGetStringField(TEXT("default_object"), DefaultObjectPath))
		{
			UObject* NewDefaultObject = nullptr;
			if (!DefaultObjectPath.IsEmpty())
			{
				NewDefaultObject = LoadObject<UObject>(nullptr, *DefaultObjectPath);
				if (!NewDefaultObject)
				{
					Context.AddWarning(FString::Printf(TEXT("Failed to load default object for %s.%s: %s"), *Node->GetName(), *Pin->PinName.ToString(), *DefaultObjectPath));
					return;
				}
			}

			if (Pin->DefaultObject != NewDefaultObject)
			{
				Node->Modify();
				if (Schema)
				{
					Schema->TrySetDefaultObject(*Pin, NewDefaultObject);
				}
				else
				{
					Pin->DefaultObject = NewDefaultObject;
				}
				Context.AddChange(FString::Printf(TEXT("Updated pin object default %s.%s"), *Node->GetName(), *Pin->PinName.ToString()));
			}
		}
	}

	void ApplyGraphNodes(UEdGraph* Graph, const TSharedPtr<FJsonObject>& GraphJson, FJsonToAssetContext& Context)
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!Graph || !GraphJson.IsValid() || !GraphJson->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
		{
			const TSharedPtr<FJsonObject>* NodeJson = nullptr;
			if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeJson) || !NodeJson || !NodeJson->IsValid())
			{
				continue;
			}

			UEdGraphNode* Node = FindNodeByJson(Graph, *NodeJson);
			if (!Node)
			{
				FString NodeName;
				(*NodeJson)->TryGetStringField(TEXT("name"), NodeName);
				Context.AddWarning(FString::Printf(TEXT("Node not found in graph %s: %s"), *Graph->GetName(), *NodeName));
				continue;
			}

			FString Comment;
			if ((*NodeJson)->TryGetStringField(TEXT("comment"), Comment) && Node->NodeComment != Comment)
			{
				Node->Modify();
				Node->NodeComment = Comment;
				Context.AddChange(FString::Printf(TEXT("Updated node comment %s.%s"), *Graph->GetName(), *Node->GetName()));
			}

			TSharedPtr<FJsonObject> PositionJson;
			if (TryGetObjectField(*NodeJson, TEXT("position"), PositionJson))
			{
				int32 NodePosX = Node->NodePosX;
				int32 NodePosY = Node->NodePosY;
				const bool bHasX = TryGetIntField(PositionJson, TEXT("x"), NodePosX);
				const bool bHasY = TryGetIntField(PositionJson, TEXT("y"), NodePosY);
				if ((bHasX || bHasY) && (Node->NodePosX != NodePosX || Node->NodePosY != NodePosY))
				{
					Node->Modify();
					Node->NodePosX = NodePosX;
					Node->NodePosY = NodePosY;
					Context.AddChange(FString::Printf(TEXT("Updated node position %s.%s"), *Graph->GetName(), *Node->GetName()));
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
			if ((*NodeJson)->TryGetArrayField(TEXT("pins"), Pins) && Pins)
			{
				for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
				{
					const TSharedPtr<FJsonObject>* PinJson = nullptr;
					if (!PinValue.IsValid() || !PinValue->TryGetObject(PinJson) || !PinJson || !PinJson->IsValid())
					{
						continue;
					}

					UEdGraphPin* Pin = FindPinByJson(Node, *PinJson);
					if (!Pin)
					{
						FString PinName;
						(*PinJson)->TryGetStringField(TEXT("name"), PinName);
						Context.AddWarning(FString::Printf(TEXT("Pin not found on node %s: %s"), *Node->GetName(), *PinName));
						continue;
					}

					ApplyPinDefaults(Graph, Node, Pin, *PinJson, Context);
				}
			}
		}
	}

	void ApplyGraphLinks(UEdGraph* Graph, const TSharedPtr<FJsonObject>& GraphJson, FJsonToAssetContext& Context)
	{
		const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
		if (!Graph || !GraphJson.IsValid() || !GraphJson->TryGetArrayField(TEXT("links"), Links) || !Links)
		{
			return;
		}

		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema)
		{
			Context.AddWarning(FString::Printf(TEXT("Graph has no schema: %s"), *Graph->GetName()));
			return;
		}

		bool bHadLinks = false;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->LinkedTo.Num() > 0)
				{
					bHadLinks = true;
					break;
				}
			}
		}

		Graph->Modify();
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			Node->Modify();
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin)
				{
					Schema->BreakPinLinks(*Pin, false);
				}
			}
		}

		int32 CreatedLinkCount = 0;
		for (const TSharedPtr<FJsonValue>& LinkValue : *Links)
		{
			const TSharedPtr<FJsonObject>* LinkJson = nullptr;
			if (!LinkValue.IsValid() || !LinkValue->TryGetObject(LinkJson) || !LinkJson || !LinkJson->IsValid())
			{
				continue;
			}

			TSharedPtr<FJsonObject> FromJson;
			TSharedPtr<FJsonObject> ToJson;
			if (!TryGetObjectField(*LinkJson, TEXT("from"), FromJson) || !TryGetObjectField(*LinkJson, TEXT("to"), ToJson))
			{
				continue;
			}

			UEdGraphPin* FromPin = FindPinByRef(Graph, FromJson);
			UEdGraphPin* ToPin = FindPinByRef(Graph, ToJson);
			if (!FromPin || !ToPin)
			{
				Context.AddWarning(FString::Printf(TEXT("Link endpoint not found in graph: %s"), *Graph->GetName()));
				continue;
			}

			if (Schema->TryCreateConnection(FromPin, ToPin))
			{
				++CreatedLinkCount;
			}
			else
			{
				Context.AddWarning(FString::Printf(TEXT("Failed to create link in graph %s: %s -> %s"), *Graph->GetName(), *FromPin->PinName.ToString(), *ToPin->PinName.ToString()));
			}
		}

		if (bHadLinks || CreatedLinkCount > 0)
		{
			Context.AddChange(FString::Printf(TEXT("Rebuilt %d links in graph %s"), CreatedLinkCount, *Graph->GetName()));
		}
	}

	void ApplyGraphs(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Root, bool bApplyGraphChanges, FJsonToAssetContext& Context)
	{
		if (!bApplyGraphChanges)
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (!Root->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
		{
			const TSharedPtr<FJsonObject>* GraphJson = nullptr;
			if (!GraphValue.IsValid() || !GraphValue->TryGetObject(GraphJson) || !GraphJson || !GraphJson->IsValid())
			{
				continue;
			}

			UEdGraph* Graph = FindGraphByJson(Blueprint, *GraphJson);
			if (!Graph)
			{
				FString GraphName;
				(*GraphJson)->TryGetStringField(TEXT("name"), GraphName);
				Context.AddWarning(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
				continue;
			}

			ApplyGraphNodes(Graph, *GraphJson, Context);
			ApplyGraphLinks(Graph, *GraphJson, Context);
		}
	}

	bool SaveAsset(UObject* Asset, FJsonToAssetContext& Context)
	{
		if (!Asset)
		{
			return false;
		}

		UPackage* Package = Asset->GetOutermost();
		if (!Package)
		{
			Context.AddWarning(TEXT("Asset has no outer package"));
			return false;
		}

		const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		if (PackageFilename.IsEmpty())
		{
			Context.AddWarning(FString::Printf(TEXT("Could not resolve package filename: %s"), *Package->GetName()));
			return false;
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, Asset, *PackageFilename, SaveArgs))
		{
			Context.AddWarning(FString::Printf(TEXT("Failed to save package: %s"), *PackageFilename));
			return false;
		}

		Context.AddChange(FString::Printf(TEXT("Saved asset package: %s"), *Package->GetName()));
		return true;
	}

	FString ApplyBlueprintJsonRoot(const TSharedPtr<FJsonObject>& Root, bool bSaveAsset, bool bCompileBlueprint, bool bApplyGraphChanges, bool bAllowStructuralChanges)
	{
		FJsonToAssetContext Context;
		if (!Root.IsValid())
		{
			return MakeResultJson(false, TEXT("JSON root is invalid"), Context);
		}

		FString Schema;
		Root->TryGetStringField(TEXT("schema"), Schema);
		if (!Schema.IsEmpty() && Schema != JsonToAssetPatchSchema)
		{
			Context.AddWarning(FString::Printf(TEXT("Unexpected JsonToAsset patch schema: %s"), *Schema));
		}

		TSharedPtr<FJsonObject> AssetObjectJson;
		if (!TryGetObjectField(Root, TEXT("asset"), AssetObjectJson) || !AssetObjectJson.IsValid())
		{
			return MakeResultJson(false, TEXT("Missing asset object"), Context);
		}

		FString ObjectPathString;
		AssetObjectJson->TryGetStringField(TEXT("object_path"), ObjectPathString);
		if (ObjectPathString.IsEmpty())
		{
			AssetObjectJson->TryGetStringField(TEXT("path"), ObjectPathString);
		}

		ObjectPathString = NormalizeObjectPath(ObjectPathString);
		if (ObjectPathString.IsEmpty())
		{
			return MakeResultJson(false, TEXT("Asset object_path/path is empty"), Context);
		}

		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPathString);
		if (!Blueprint)
		{
			return MakeResultJson(false, FString::Printf(TEXT("Asset is not a Blueprint or could not be loaded: %s"), *ObjectPathString), Context);
		}

		if (bAllowStructuralChanges)
		{
			Context.AddWarning(TEXT("Structural creation/deletion is not implemented in this MVP; existing objects are patched only."));
		}

		const FScopedTransaction Transaction(NSLOCTEXT("JsonToAsset", "ApplyBlueprintVisualScriptJson", "Apply Blueprint Visual Script JSON"));
		Blueprint->Modify();

		TSharedPtr<FJsonObject> ClassDefaultsJson;
		if (TryGetObjectField(Root, TEXT("class_defaults"), ClassDefaultsJson))
		{
			UObject* ClassDefaultObject = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
			ApplyDetailsProperties(ClassDefaultObject, ClassDefaultsJson, TEXT("class defaults"), Context);
		}

		const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
		if (Root->TryGetArrayField(TEXT("components"), Components) && Components)
		{
			for (const TSharedPtr<FJsonValue>& ComponentValue : *Components)
			{
				const TSharedPtr<FJsonObject>* ComponentJson = nullptr;
				if (ComponentValue.IsValid() && ComponentValue->TryGetObject(ComponentJson) && ComponentJson && ComponentJson->IsValid())
				{
					ApplyComponentJson(Blueprint, *ComponentJson, Context);
				}
			}
		}

		ApplyGraphs(Blueprint, Root, bApplyGraphChanges, Context);

		if (Context.bAnyChange)
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			Blueprint->GetOutermost()->MarkPackageDirty();

			if (bCompileBlueprint)
			{
				FKismetEditorUtilities::CompileBlueprint(Blueprint);
				Context.AddChange(TEXT("Compiled Blueprint"));
			}

			if (bSaveAsset)
			{
				SaveAsset(Blueprint, Context);
			}
		}

		return MakeResultJson(true, FString(), Context);
	}
}

FString UJsonToAssetLibrary::ApplyBlueprintVisualScriptJsonString(
	const FString& JsonString,
	bool bSaveAsset,
	bool bCompileBlueprint,
	bool bApplyGraphChanges,
	bool bAllowStructuralChanges)
{
	FJsonToAssetContext Context;
	if (JsonString.TrimStartAndEnd().IsEmpty())
	{
		return MakeResultJson(false, TEXT("JsonString is empty"), Context);
	}

	TSharedPtr<FJsonObject> Root;
	FString Error;
	if (!ParseRootObject(JsonString, Root, Error))
	{
		return MakeResultJson(false, Error, Context);
	}

	return ApplyBlueprintJsonRoot(Root, bSaveAsset, bCompileBlueprint, bApplyGraphChanges, bAllowStructuralChanges);
}

FString UJsonToAssetLibrary::ApplyBlueprintVisualScriptJsonFile(
	const FString& JsonFilePath,
	bool bSaveAsset,
	bool bCompileBlueprint,
	bool bApplyGraphChanges,
	bool bAllowStructuralChanges)
{
	FJsonToAssetContext Context;
	if (JsonFilePath.TrimStartAndEnd().IsEmpty())
	{
		return MakeResultJson(false, TEXT("JsonFilePath is empty"), Context);
	}

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *JsonFilePath))
	{
		return MakeResultJson(false, FString::Printf(TEXT("Failed to read JSON file: %s"), *JsonFilePath), Context);
	}

	return ApplyBlueprintVisualScriptJsonString(JsonString, bSaveAsset, bCompileBlueprint, bApplyGraphChanges, bAllowStructuralChanges);
}
