#include "JsonToAssetLibrary.h"

#include "Animation/AnimBlueprint.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "AnimationStateGraph.h"
#include "AnimationStateGraphSchema.h"
#include "AnimationTransitionGraph.h"
#include "AnimationTransitionSchema.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/MemberReference.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "HAL/FileManager.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
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
#include "TaskEvidenceBuilder.h"
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

	bool ParseRootObject(const FString& JsonString, TSharedPtr<FJsonObject>& OutRoot, FString& OutError);

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

	void WriteJsonToAssetEvidence(const FString& ResultJson, const FString& InputFilePath)
	{
		TSharedPtr<FJsonObject> ResultObject;
		FString ParseError;
		bool bOk = false;
		FString Error;
		int32 ChangeCount = 0;
		int32 WarningCount = 0;

		if (ParseRootObject(ResultJson, ResultObject, ParseError) && ResultObject.IsValid())
		{
			ResultObject->TryGetBoolField(TEXT("ok"), bOk);
			ResultObject->TryGetStringField(TEXT("error"), Error);

			double NumberValue = 0.0;
			if (ResultObject->TryGetNumberField(TEXT("change_count"), NumberValue))
			{
				ChangeCount = static_cast<int32>(NumberValue);
			}

			if (ResultObject->TryGetNumberField(TEXT("warning_count"), NumberValue))
			{
				WarningCount = static_cast<int32>(NumberValue);
			}
		}
		else
		{
			Error = ParseError;
		}

		FTaskEvidenceBuilder Evidence(TEXT("JsonToAsset"), TEXT("ApplyBlueprintVisualScriptJson"));
		Evidence
			.SetStatus(bOk ? TEXT("succeeded") : TEXT("failed"))
			.SetSummary(bOk ? TEXT("Blueprint visual script JSON applied.") : TEXT("Blueprint visual script JSON apply failed."), Error)
			.AddFact(TEXT("json_to_asset.ok"), bOk)
			.AddFact(TEXT("json_to_asset.change_count"), ChangeCount)
			.AddFact(TEXT("json_to_asset.warning_count"), WarningCount);

		if (!InputFilePath.IsEmpty())
		{
			Evidence.AddArtifact(InputFilePath, TEXT("input_patch"), TEXT("application/json"), TEXT("Input JsonToAsset patch JSON."));
		}

		if (ResultObject.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
			if (ResultObject->TryGetArrayField(TEXT("changes"), Changes) && Changes)
			{
				for (const TSharedPtr<FJsonValue>& Change : *Changes)
				{
					Evidence.AddLog(TEXT("info"), TEXT("JsonToAsset"), Change.IsValid() ? Change->AsString() : FString());
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
			if (ResultObject->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings)
			{
				for (const TSharedPtr<FJsonValue>& Warning : *Warnings)
				{
					Evidence.AddLog(TEXT("warning"), TEXT("JsonToAsset"), Warning.IsValid() ? Warning->AsString() : FString());
				}
			}
		}

		FString EvidencePath;
		FString EvidenceError;
		Evidence.WriteToDefaultLocation(EvidencePath, EvidenceError);
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

	bool TryGetStringFieldRecursive(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName, FString& OutValue)
	{
		return JsonObject.IsValid() && JsonObject->TryGetStringField(FStringView(FieldName), OutValue);
	}

	bool TryGetStringFieldRecursive(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* ObjectFieldName, const TCHAR* FieldName, FString& OutValue)
	{
		TSharedPtr<FJsonObject> ChildObject;
		return TryGetObjectField(JsonObject, ObjectFieldName, ChildObject) && TryGetStringFieldRecursive(ChildObject, FieldName, OutValue);
	}

	bool WantsDelete(const TSharedPtr<FJsonObject>& JsonObject)
	{
		if (!JsonObject.IsValid())
		{
			return false;
		}

		bool bDelete = false;
		if (JsonObject->TryGetBoolField(TEXT("delete"), bDelete) && bDelete)
		{
			return true;
		}

		if (JsonObject->TryGetBoolField(TEXT("remove"), bDelete) && bDelete)
		{
			return true;
		}

		FString Operation;
		return JsonObject->TryGetStringField(TEXT("operation"), Operation) && Operation.Equals(TEXT("delete"), ESearchCase::IgnoreCase);
	}

	TSharedPtr<FJsonObject> GetNodeSemanticJson(const TSharedPtr<FJsonObject>& NodeJson)
	{
		TSharedPtr<FJsonObject> SemanticJson;
		TryGetObjectField(NodeJson, TEXT("semantic"), SemanticJson);
		return SemanticJson;
	}

	FString GetNodeClassPath(const TSharedPtr<FJsonObject>& NodeJson)
	{
		FString ClassPath;
		if (NodeJson.IsValid())
		{
			NodeJson->TryGetStringField(TEXT("class"), ClassPath);
		}

		if (ClassPath.IsEmpty())
		{
			TSharedPtr<FJsonObject> SemanticJson = GetNodeSemanticJson(NodeJson);
			if (SemanticJson.IsValid())
			{
				SemanticJson->TryGetStringField(TEXT("node_class"), ClassPath);
				if (ClassPath.IsEmpty())
				{
					TryGetStringFieldRecursive(SemanticJson, TEXT("anim_graph_node"), TEXT("node_class"), ClassPath);
				}
			}
		}

		return ClassPath;
	}

	UClass* LoadClassFromJsonPath(const FString& ClassPath, UClass* RequiredBaseClass, FJsonToAssetContext& Context, const FString& Label)
	{
		if (ClassPath.IsEmpty())
		{
			Context.AddWarning(FString::Printf(TEXT("Missing class path for %s"), *Label));
			return nullptr;
		}

		UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassPath);
		if (!LoadedClass)
		{
			Context.AddWarning(FString::Printf(TEXT("Failed to load class for %s: %s"), *Label, *ClassPath));
			return nullptr;
		}

		if (RequiredBaseClass && !LoadedClass->IsChildOf(RequiredBaseClass))
		{
			Context.AddWarning(FString::Printf(TEXT("Class for %s is not a %s: %s"), *Label, *RequiredBaseClass->GetName(), *ClassPath));
			return nullptr;
		}

		return LoadedClass;
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

	void ApplyPropertyMap(UObject* Object, const TSharedPtr<FJsonObject>& PropertyMapJson, const FString& ObjectLabel, FJsonToAssetContext& Context)
	{
		if (!Object || !PropertyMapJson.IsValid())
		{
			return;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : PropertyMapJson->Values)
		{
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
			{
				continue;
			}

			FProperty* Property = FindPropertyByName(Object->GetClass(), Pair.Key);
			if (!Property)
			{
				Context.AddWarning(FString::Printf(TEXT("Property not found on %s: %s"), *ObjectLabel, *Pair.Key));
				continue;
			}

			FString OldValue;
			ExportPropertyValue(Object, Property, OldValue);

			const FString NewValue = Pair.Value->AsString();
			if (OldValue == NewValue)
			{
				continue;
			}

			Object->Modify();
			FOutputDeviceNull ErrorOutput;
			const TCHAR* ImportResult = Property->ImportText_InContainer(*NewValue, Object, Object, PPF_None, &ErrorOutput);
			if (!ImportResult)
			{
				Context.AddWarning(FString::Printf(TEXT("Failed to import %s.%s from value: %s"), *ObjectLabel, *Pair.Key, *NewValue));
				continue;
			}

			Context.AddChange(FString::Printf(TEXT("Updated %s.%s"), *ObjectLabel, *Pair.Key));
		}
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

	void ApplyNodePosition(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeJson)
	{
		if (!Node || !NodeJson.IsValid())
		{
			return;
		}

		TSharedPtr<FJsonObject> PositionJson;
		if (TryGetObjectField(NodeJson, TEXT("position"), PositionJson))
		{
			int32 NodePosX = Node->NodePosX;
			int32 NodePosY = Node->NodePosY;
			TryGetIntField(PositionJson, TEXT("x"), NodePosX);
			TryGetIntField(PositionJson, TEXT("y"), NodePosY);
			Node->NodePosX = NodePosX;
			Node->NodePosY = NodePosY;
		}
	}

	UClass* LoadClassByPath(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}

		return LoadObject<UClass>(nullptr, *ObjectPath);
	}

	void ConfigureK2NodeFromSemantic(UBlueprint* Blueprint, UEdGraphNode* Node, const TSharedPtr<FJsonObject>& SemanticJson, FJsonToAssetContext& Context)
	{
		if (!Node || !SemanticJson.IsValid())
		{
			return;
		}

		FString Kind;
		SemanticJson->TryGetStringField(TEXT("kind"), Kind);

		if (UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
		{
			FString CustomFunctionName;
			if (SemanticJson->TryGetStringField(TEXT("custom_function_name"), CustomFunctionName) || SemanticJson->TryGetStringField(TEXT("function_name"), CustomFunctionName))
			{
				CustomEventNode->CustomFunctionName = FName(*CustomFunctionName);
			}
		}
		else if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			FString FunctionName;
			if (SemanticJson->TryGetStringField(TEXT("function_name"), FunctionName))
			{
				UClass* OwnerClass = Blueprint ? Blueprint->ParentClass : nullptr;
				TSharedPtr<FJsonObject> SignatureJson;
				FString SignatureOwnerClassPath;
				if (TryGetObjectField(SemanticJson, TEXT("signature_function"), SignatureJson))
				{
					SignatureJson->TryGetStringField(TEXT("owner_class"), SignatureOwnerClassPath);
				}

				if (!SignatureOwnerClassPath.IsEmpty())
				{
					OwnerClass = LoadClassByPath(SignatureOwnerClassPath);
				}

				EventNode->EventReference.SetExternalMember(FName(*FunctionName), OwnerClass);
			}
		}
		else if (UK2Node_CallFunction* CallFunctionNode = Cast<UK2Node_CallFunction>(Node))
		{
			TSharedPtr<FJsonObject> TargetFunctionJson;
			FString FunctionPath;
			if (TryGetObjectField(SemanticJson, TEXT("target_function"), TargetFunctionJson))
			{
				TargetFunctionJson->TryGetStringField(TEXT("path"), FunctionPath);
			}

			if (UFunction* Function = FunctionPath.IsEmpty() ? nullptr : LoadObject<UFunction>(nullptr, *FunctionPath))
			{
				CallFunctionNode->FunctionReference.SetFromField<UFunction>(Function, false);
			}
			else
			{
				FString FunctionName;
				SemanticJson->TryGetStringField(TEXT("function_name"), FunctionName);
				FString OwnerClassPath;
				if (TargetFunctionJson.IsValid())
				{
					TargetFunctionJson->TryGetStringField(TEXT("owner_class"), OwnerClassPath);
				}

				if (!FunctionName.IsEmpty())
				{
					CallFunctionNode->FunctionReference.SetExternalMember(FName(*FunctionName), LoadClassByPath(OwnerClassPath));
				}
			}
		}
		else if (UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
		{
			FString VariableName;
			if (SemanticJson->TryGetStringField(TEXT("variable_name"), VariableName))
			{
				FString SourceClassPath;
				SemanticJson->TryGetStringField(TEXT("variable_source_class"), SourceClassPath);
				if (UClass* SourceClass = LoadClassByPath(SourceClassPath))
				{
					VariableNode->VariableReference.SetExternalMember(FName(*VariableName), SourceClass);
				}
				else
				{
					VariableNode->VariableReference.SetSelfMember(FName(*VariableName));
				}
			}
		}
		else if (UK2Node_DynamicCast* DynamicCastNode = Cast<UK2Node_DynamicCast>(Node))
		{
			FString TargetTypePath;
			if (SemanticJson->TryGetStringField(TEXT("target_type"), TargetTypePath))
			{
				DynamicCastNode->TargetType = LoadClassByPath(TargetTypePath);
			}
		}
	}

	UEdGraphNode* CreateGraphNodeFromJson(UBlueprint* Blueprint, UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeJson, FJsonToAssetContext& Context)
	{
		if (!Graph || !NodeJson.IsValid())
		{
			return nullptr;
		}

		const FString ClassPath = GetNodeClassPath(NodeJson);
		UClass* NodeClass = LoadClassFromJsonPath(ClassPath, UEdGraphNode::StaticClass(), Context, FString::Printf(TEXT("node in graph %s"), *Graph->GetName()));
		if (!NodeClass)
		{
			return nullptr;
		}

		Graph->Modify();
		UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass, NAME_None, RF_Transactional);
		if (!Node)
		{
			Context.AddWarning(FString::Printf(TEXT("Failed to create node in graph %s from class %s"), *Graph->GetName(), *ClassPath));
			return nullptr;
		}

		FString NodeId;
		NodeJson->TryGetStringField(TEXT("id"), NodeId);
		const FGuid NodeGuid = ParseGuid(NodeId);
		Node->NodeGuid = NodeGuid.IsValid() ? NodeGuid : FGuid::NewGuid();

		ApplyNodePosition(Node, NodeJson);

		FString Comment;
		if (NodeJson->TryGetStringField(TEXT("comment"), Comment))
		{
			Node->NodeComment = Comment;
		}

		TSharedPtr<FJsonObject> SemanticJson = GetNodeSemanticJson(NodeJson);
		ConfigureK2NodeFromSemantic(Blueprint, Node, SemanticJson, Context);

		TSharedPtr<FJsonObject> PropertiesJson;
		if (TryGetObjectField(NodeJson, TEXT("properties"), PropertiesJson))
		{
			ApplyPropertyMap(Node, PropertiesJson, FString::Printf(TEXT("node %s"), *Node->GetName()), Context);
		}

		Node->CreateNewGuid();
		if (NodeGuid.IsValid())
		{
			Node->NodeGuid = NodeGuid;
		}

		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		Graph->AddNode(Node, true, false);

		Context.AddChange(FString::Printf(TEXT("Created node %s in graph %s"), *Node->GetName(), *Graph->GetName()));
		return Node;
	}

	UEdGraph* CreateGraphFromJson(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& GraphJson, FJsonToAssetContext& Context)
	{
		if (!Blueprint || !GraphJson.IsValid())
		{
			return nullptr;
		}

		FString GraphName;
		GraphJson->TryGetStringField(TEXT("name"), GraphName);
		if (GraphName.IsEmpty())
		{
			Context.AddWarning(TEXT("Cannot create graph without name"));
			return nullptr;
		}

		FString SchemaPath;
		GraphJson->TryGetStringField(TEXT("schema"), SchemaPath);
		UClass* SchemaClass = SchemaPath.IsEmpty() ? UEdGraphSchema_K2::StaticClass() : LoadClassByPath(SchemaPath);
		if (!SchemaClass || !SchemaClass->IsChildOf(UEdGraphSchema::StaticClass()))
		{
			SchemaClass = UEdGraphSchema_K2::StaticClass();
		}

		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*GraphName), UEdGraph::StaticClass(), SchemaClass);
		if (!Graph)
		{
			Context.AddWarning(FString::Printf(TEXT("Failed to create graph: %s"), *GraphName));
			return nullptr;
		}

		FString GraphType;
		GraphJson->TryGetStringField(TEXT("type"), GraphType);
		if (GraphType.Equals(TEXT("function"), ESearchCase::IgnoreCase))
		{
			FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, Graph, true, nullptr);
		}
		else if (GraphType.Equals(TEXT("macro"), ESearchCase::IgnoreCase))
		{
			FBlueprintEditorUtils::AddMacroGraph(Blueprint, Graph, true, nullptr);
		}
		else
		{
			FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
		}

		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (Schema && Graph->Nodes.Num() == 0)
		{
			Schema->CreateDefaultNodesForGraph(*Graph);
		}

		Context.AddChange(FString::Printf(TEXT("Created graph %s"), *GraphName));
		return Graph;
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

	void ApplyGraphNodes(UBlueprint* Blueprint, UEdGraph* Graph, const TSharedPtr<FJsonObject>& GraphJson, bool bAllowStructuralChanges, FJsonToAssetContext& Context)
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
			if (WantsDelete(*NodeJson))
			{
				if (!bAllowStructuralChanges)
				{
					Context.AddWarning(FString::Printf(TEXT("Node delete requested without structural changes enabled in graph %s"), *Graph->GetName()));
					continue;
				}

				if (Node)
				{
					const FString NodeName = Node->GetName();
					Graph->Modify();
					Node->Modify();
					Node->DestroyNode();
					Context.AddChange(FString::Printf(TEXT("Deleted node %s from graph %s"), *NodeName, *Graph->GetName()));
				}
				else
				{
					FString NodeName;
					(*NodeJson)->TryGetStringField(TEXT("name"), NodeName);
					Context.AddWarning(FString::Printf(TEXT("Node delete target not found in graph %s: %s"), *Graph->GetName(), *NodeName));
				}

				continue;
			}

			if (!Node)
			{
				if (bAllowStructuralChanges)
				{
					Node = CreateGraphNodeFromJson(Blueprint, Graph, *NodeJson, Context);
				}

				if (!Node)
				{
					FString NodeName;
					(*NodeJson)->TryGetStringField(TEXT("name"), NodeName);
					Context.AddWarning(FString::Printf(TEXT("Node not found in graph %s: %s"), *Graph->GetName(), *NodeName));
					continue;
				}
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

	UAnimationStateMachineGraph* FindAnimationStateMachineGraph(UAnimBlueprint* AnimBlueprint, const FString& StateMachineName)
	{
		if (!AnimBlueprint || StateMachineName.IsEmpty())
		{
			return nullptr;
		}

		TArray<UEdGraph*> AllGraphs;
		AnimBlueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph)
			{
				continue;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UAnimGraphNode_StateMachineBase* StateMachineNode = Cast<UAnimGraphNode_StateMachineBase>(Node);
				if (StateMachineNode && StateMachineNode->EditorStateMachineGraph && StateMachineNode->GetStateMachineName() == StateMachineName)
				{
					return StateMachineNode->EditorStateMachineGraph;
				}
			}
		}

		return nullptr;
	}

	UAnimStateNode* FindAnimStateNode(UAnimationStateMachineGraph* StateMachineGraph, const FString& StateName)
	{
		if (!StateMachineGraph || StateName.IsEmpty())
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : StateMachineGraph->Nodes)
		{
			UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node);
			if (StateNode && StateNode->GetStateName() == StateName)
			{
				return StateNode;
			}
		}

		return nullptr;
	}

	UAnimStateTransitionNode* FindAnimTransitionNode(UAnimationStateMachineGraph* StateMachineGraph, const FString& PreviousStateName, const FString& NextStateName)
	{
		if (!StateMachineGraph || PreviousStateName.IsEmpty() || NextStateName.IsEmpty())
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : StateMachineGraph->Nodes)
		{
			UAnimStateTransitionNode* TransitionNode = Cast<UAnimStateTransitionNode>(Node);
			if (!TransitionNode)
			{
				continue;
			}

			const UAnimStateNodeBase* PreviousState = TransitionNode->GetPreviousState();
			const UAnimStateNodeBase* NextState = TransitionNode->GetNextState();
			if (PreviousState && NextState && PreviousState->GetStateName() == PreviousStateName && NextState->GetStateName() == NextStateName)
			{
				return TransitionNode;
			}
		}

		return nullptr;
	}

	UAnimStateNode* CreateAnimStateNode(UAnimationStateMachineGraph* StateMachineGraph, const TSharedPtr<FJsonObject>& StateJson, FJsonToAssetContext& Context)
	{
		if (!StateMachineGraph || !StateJson.IsValid())
		{
			return nullptr;
		}

		FString StateName;
		StateJson->TryGetStringField(TEXT("name"), StateName);
		if (StateName.IsEmpty())
		{
			Context.AddWarning(FString::Printf(TEXT("State in state machine %s has no name"), *StateMachineGraph->GetName()));
			return nullptr;
		}

		UAnimStateNode* StateNode = FindAnimStateNode(StateMachineGraph, StateName);
		const bool bCreatedState = StateNode == nullptr;

		int32 NodePosX = StateNode ? StateNode->NodePosX : 0;
		int32 NodePosY = StateNode ? StateNode->NodePosY : 0;
		TSharedPtr<FJsonObject> PositionJson;
		const bool bHasPosition = TryGetObjectField(StateJson, TEXT("position"), PositionJson);
		if (bHasPosition)
		{
			TryGetIntField(PositionJson, TEXT("x"), NodePosX);
			TryGetIntField(PositionJson, TEXT("y"), NodePosY);
		}

		if (bCreatedState)
		{
			StateMachineGraph->Modify();
			StateNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(
				StateMachineGraph,
				NewObject<UAnimStateNode>(),
				FVector2f(static_cast<float>(NodePosX), static_cast<float>(NodePosY)),
				false);
		}

		if (!StateNode)
		{
			Context.AddWarning(FString::Printf(TEXT("Failed to create anim state %s in state machine %s"), *StateName, *StateMachineGraph->GetName()));
			return nullptr;
		}

		if (bCreatedState && StateNode->BoundGraph)
		{
			FBlueprintEditorUtils::RenameGraph(StateNode->BoundGraph, StateName);
		}

		if (!bCreatedState && bHasPosition && (StateNode->NodePosX != NodePosX || StateNode->NodePosY != NodePosY))
		{
			StateNode->Modify();
			StateNode->NodePosX = NodePosX;
			StateNode->NodePosY = NodePosY;
			Context.AddChange(FString::Printf(TEXT("Updated anim state position %s in state machine %s"), *StateName, *StateMachineGraph->GetName()));
		}

		bool bAlwaysResetOnEntry = false;
		if (StateJson->TryGetBoolField(TEXT("always_reset_on_entry"), bAlwaysResetOnEntry) && StateNode->bAlwaysResetOnEntry != bAlwaysResetOnEntry)
		{
			StateNode->Modify();
			StateNode->bAlwaysResetOnEntry = bAlwaysResetOnEntry;
			Context.AddChange(FString::Printf(TEXT("Updated anim state %s always_reset_on_entry"), *StateName));
		}

		TSharedPtr<FJsonObject> PropertiesJson;
		if (TryGetObjectField(StateJson, TEXT("properties"), PropertiesJson))
		{
			ApplyPropertyMap(StateNode, PropertiesJson, FString::Printf(TEXT("anim state %s"), *StateName), Context);
		}

		if (bCreatedState)
		{
			Context.AddChange(FString::Printf(TEXT("Created anim state %s in state machine %s"), *StateName, *StateMachineGraph->GetName()));
		}
		return StateNode;
	}

	void DeleteAnimTransitionNode(UAnimationStateMachineGraph* StateMachineGraph, UAnimStateTransitionNode* TransitionNode, FJsonToAssetContext& Context)
	{
		if (!StateMachineGraph || !TransitionNode)
		{
			return;
		}

		const UAnimStateNodeBase* PreviousState = TransitionNode->GetPreviousState();
		const UAnimStateNodeBase* NextState = TransitionNode->GetNextState();
		const FString PreviousStateName = PreviousState ? PreviousState->GetStateName() : FString(TEXT("<unknown>"));
		const FString NextStateName = NextState ? NextState->GetStateName() : FString(TEXT("<unknown>"));

		StateMachineGraph->Modify();
		TransitionNode->Modify();
		TransitionNode->DestroyNode();
		Context.AddChange(FString::Printf(TEXT("Deleted anim transition %s -> %s in state machine %s"), *PreviousStateName, *NextStateName, *StateMachineGraph->GetName()));
	}

	void DeleteAnimStateNode(UAnimationStateMachineGraph* StateMachineGraph, UAnimStateNode* StateNode, FJsonToAssetContext& Context)
	{
		if (!StateMachineGraph || !StateNode)
		{
			return;
		}

		TArray<UAnimStateTransitionNode*> ConnectedTransitions;
		StateNode->GetTransitionList(ConnectedTransitions, false);
		for (UAnimStateTransitionNode* TransitionNode : ConnectedTransitions)
		{
			DeleteAnimTransitionNode(StateMachineGraph, TransitionNode, Context);
		}

		const FString StateName = StateNode->GetStateName();
		StateMachineGraph->Modify();
		StateNode->Modify();
		StateNode->DestroyNode();
		Context.AddChange(FString::Printf(TEXT("Deleted anim state %s from state machine %s"), *StateName, *StateMachineGraph->GetName()));
	}

	void ApplyTransitionJson(UAnimStateTransitionNode* TransitionNode, const TSharedPtr<FJsonObject>& TransitionJson, FJsonToAssetContext& Context)
	{
		if (!TransitionNode || !TransitionJson.IsValid())
		{
			return;
		}

		bool bTransitionChanged = false;
		auto ModifyTransition = [&TransitionNode, &bTransitionChanged]()
		{
			if (!bTransitionChanged)
			{
				TransitionNode->Modify();
				bTransitionChanged = true;
			}
		};

		double NumberValue = 0.0;
		if (TransitionJson->TryGetNumberField(TEXT("priority_order"), NumberValue))
		{
			const int32 NewValue = static_cast<int32>(NumberValue);
			if (TransitionNode->PriorityOrder != NewValue)
			{
				ModifyTransition();
				TransitionNode->PriorityOrder = NewValue;
			}
		}
		if (TransitionJson->TryGetNumberField(TEXT("crossfade_duration"), NumberValue))
		{
			const float NewValue = static_cast<float>(NumberValue);
			if (TransitionNode->CrossfadeDuration != NewValue)
			{
				ModifyTransition();
				TransitionNode->CrossfadeDuration = NewValue;
			}
		}
		if (TransitionJson->TryGetNumberField(TEXT("automatic_rule_trigger_time"), NumberValue))
		{
			const float NewValue = static_cast<float>(NumberValue);
			if (TransitionNode->AutomaticRuleTriggerTime != NewValue)
			{
				ModifyTransition();
				TransitionNode->AutomaticRuleTriggerTime = NewValue;
			}
		}
		if (TransitionJson->TryGetNumberField(TEXT("min_time_before_reentry"), NumberValue))
		{
			const float NewValue = static_cast<float>(NumberValue);
			if (TransitionNode->MinTimeBeforeReentry != NewValue)
			{
				ModifyTransition();
				TransitionNode->MinTimeBeforeReentry = NewValue;
			}
		}

		bool BoolValue = false;
		if (TransitionJson->TryGetBoolField(TEXT("automatic_rule_based_on_sequence_player_in_state"), BoolValue))
		{
			if (TransitionNode->bAutomaticRuleBasedOnSequencePlayerInState != BoolValue)
			{
				ModifyTransition();
				TransitionNode->bAutomaticRuleBasedOnSequencePlayerInState = BoolValue;
			}
		}
		if (TransitionJson->TryGetBoolField(TEXT("bidirectional"), BoolValue))
		{
			if (TransitionNode->Bidirectional != BoolValue)
			{
				ModifyTransition();
				TransitionNode->Bidirectional = BoolValue;
			}
		}
		if (TransitionJson->TryGetBoolField(TEXT("disabled"), BoolValue))
		{
			if (TransitionNode->bDisabled != BoolValue)
			{
				ModifyTransition();
				TransitionNode->bDisabled = BoolValue;
			}
		}

		if (bTransitionChanged)
		{
			Context.AddChange(FString::Printf(TEXT("Updated anim transition %s"), *TransitionNode->GetStateName()));
		}

		TSharedPtr<FJsonObject> PropertiesJson;
		if (TryGetObjectField(TransitionJson, TEXT("properties"), PropertiesJson))
		{
			ApplyPropertyMap(TransitionNode, PropertiesJson, FString::Printf(TEXT("anim transition %s"), *TransitionNode->GetStateName()), Context);
		}
	}

	void ApplyTransitionGraphJson(UBlueprint* Blueprint, UAnimStateTransitionNode* TransitionNode, const TSharedPtr<FJsonObject>& TransitionJson, bool bAllowStructuralChanges, FJsonToAssetContext& Context)
	{
		if (!TransitionNode || !TransitionJson.IsValid() || !TransitionNode->BoundGraph)
		{
			return;
		}

		TSharedPtr<FJsonObject> TransitionGraphJson;
		if (!TryGetObjectField(TransitionJson, TEXT("graph"), TransitionGraphJson))
		{
			return;
		}

		ApplyGraphNodes(Blueprint, TransitionNode->BoundGraph, TransitionGraphJson, bAllowStructuralChanges, Context);
		ApplyGraphLinks(TransitionNode->BoundGraph, TransitionGraphJson, Context);
	}

	UAnimStateTransitionNode* CreateAnimTransitionNode(UAnimationStateMachineGraph* StateMachineGraph, const TSharedPtr<FJsonObject>& TransitionJson, FJsonToAssetContext& Context)
	{
		if (!StateMachineGraph || !TransitionJson.IsValid())
		{
			return nullptr;
		}

		FString FromStateName;
		FString ToStateName;
		TransitionJson->TryGetStringField(TEXT("from"), FromStateName);
		TransitionJson->TryGetStringField(TEXT("to"), ToStateName);
		if (FromStateName.IsEmpty())
		{
			TransitionJson->TryGetStringField(TEXT("previous_state"), FromStateName);
		}
		if (ToStateName.IsEmpty())
		{
			TransitionJson->TryGetStringField(TEXT("next_state"), ToStateName);
		}

		UAnimStateNode* FromState = FindAnimStateNode(StateMachineGraph, FromStateName);
		UAnimStateNode* ToState = FindAnimStateNode(StateMachineGraph, ToStateName);
		if (!FromState || !ToState)
		{
			Context.AddWarning(FString::Printf(TEXT("Transition endpoint not found in %s: %s -> %s"), *StateMachineGraph->GetName(), *FromStateName, *ToStateName));
			return nullptr;
		}

		if (UAnimStateTransitionNode* ExistingTransition = FindAnimTransitionNode(StateMachineGraph, FromStateName, ToStateName))
		{
			ApplyTransitionJson(ExistingTransition, TransitionJson, Context);
			return ExistingTransition;
		}

		StateMachineGraph->Modify();
		UAnimStateTransitionNode* TransitionNode = NewObject<UAnimStateTransitionNode>(StateMachineGraph, UAnimStateTransitionNode::StaticClass(), NAME_None, RF_Transactional);
		TransitionNode->CreateNewGuid();
		ApplyNodePosition(TransitionNode, TransitionJson);
		if (TransitionNode->NodePosX == 0 && TransitionNode->NodePosY == 0)
		{
			TransitionNode->NodePosX = (FromState->NodePosX + ToState->NodePosX) / 2;
			TransitionNode->NodePosY = (FromState->NodePosY + ToState->NodePosY) / 2;
		}

		TransitionNode->AllocateDefaultPins();
		StateMachineGraph->AddNode(TransitionNode, true, false);
		TransitionNode->PostPlacedNewNode();
		TransitionNode->CreateConnections(FromState, ToState);
		ApplyTransitionJson(TransitionNode, TransitionJson, Context);

		Context.AddChange(FString::Printf(TEXT("Created anim transition %s -> %s in state machine %s"), *FromStateName, *ToStateName, *StateMachineGraph->GetName()));
		return TransitionNode;
	}

	void ApplyAnimationStateMachines(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Root, bool bAllowStructuralChanges, FJsonToAssetContext& Context)
	{
		if (!bAllowStructuralChanges)
		{
			return;
		}

		UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Blueprint);
		if (!AnimBlueprint)
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* StateMachines = nullptr;
		if (!Root->TryGetArrayField(TEXT("animation_state_machines"), StateMachines) || !StateMachines)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& StateMachineValue : *StateMachines)
		{
			const TSharedPtr<FJsonObject>* StateMachineJson = nullptr;
			if (!StateMachineValue.IsValid() || !StateMachineValue->TryGetObject(StateMachineJson) || !StateMachineJson || !StateMachineJson->IsValid())
			{
				continue;
			}

			FString StateMachineName;
			(*StateMachineJson)->TryGetStringField(TEXT("name"), StateMachineName);
			UAnimationStateMachineGraph* StateMachineGraph = FindAnimationStateMachineGraph(AnimBlueprint, StateMachineName);
			if (!StateMachineGraph)
			{
				Context.AddWarning(FString::Printf(TEXT("Animation state machine not found: %s"), *StateMachineName));
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Transitions = nullptr;
			if ((*StateMachineJson)->TryGetArrayField(TEXT("transitions"), Transitions) && Transitions)
			{
				for (const TSharedPtr<FJsonValue>& TransitionValue : *Transitions)
				{
					const TSharedPtr<FJsonObject>* TransitionJson = nullptr;
					if (TransitionValue.IsValid() && TransitionValue->TryGetObject(TransitionJson) && TransitionJson && TransitionJson->IsValid())
					{
						if (WantsDelete(*TransitionJson))
						{
							FString FromStateName;
							FString ToStateName;
							(*TransitionJson)->TryGetStringField(TEXT("from"), FromStateName);
							(*TransitionJson)->TryGetStringField(TEXT("to"), ToStateName);
							if (FromStateName.IsEmpty())
							{
								(*TransitionJson)->TryGetStringField(TEXT("previous_state"), FromStateName);
							}
							if (ToStateName.IsEmpty())
							{
								(*TransitionJson)->TryGetStringField(TEXT("next_state"), ToStateName);
							}

							if (UAnimStateTransitionNode* TransitionNode = FindAnimTransitionNode(StateMachineGraph, FromStateName, ToStateName))
							{
								DeleteAnimTransitionNode(StateMachineGraph, TransitionNode, Context);
							}
							else
							{
								Context.AddWarning(FString::Printf(TEXT("Anim transition delete target not found in %s: %s -> %s"), *StateMachineGraph->GetName(), *FromStateName, *ToStateName));
							}
						}
					}
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* States = nullptr;
			if ((*StateMachineJson)->TryGetArrayField(TEXT("states"), States) && States)
			{
				for (const TSharedPtr<FJsonValue>& StateValue : *States)
				{
					const TSharedPtr<FJsonObject>* StateJson = nullptr;
					if (StateValue.IsValid() && StateValue->TryGetObject(StateJson) && StateJson && StateJson->IsValid())
					{
						FString StateName;
						(*StateJson)->TryGetStringField(TEXT("name"), StateName);
						if (WantsDelete(*StateJson))
						{
							if (UAnimStateNode* StateNode = FindAnimStateNode(StateMachineGraph, StateName))
							{
								DeleteAnimStateNode(StateMachineGraph, StateNode, Context);
							}
							else
							{
								Context.AddWarning(FString::Printf(TEXT("Anim state delete target not found in %s: %s"), *StateMachineGraph->GetName(), *StateName));
							}
						}
						else
						{
							UAnimStateNode* StateNode = CreateAnimStateNode(StateMachineGraph, *StateJson, Context);
							TSharedPtr<FJsonObject> StateGraphJson;
							if (StateNode && StateNode->BoundGraph && TryGetObjectField(*StateJson, TEXT("graph"), StateGraphJson))
							{
								ApplyGraphNodes(Blueprint, StateNode->BoundGraph, StateGraphJson, bAllowStructuralChanges, Context);
								ApplyGraphLinks(StateNode->BoundGraph, StateGraphJson, Context);
							}
						}
					}
				}
			}

			if (Transitions)
			{
				for (const TSharedPtr<FJsonValue>& TransitionValue : *Transitions)
				{
					const TSharedPtr<FJsonObject>* TransitionJson = nullptr;
					if (TransitionValue.IsValid() && TransitionValue->TryGetObject(TransitionJson) && TransitionJson && TransitionJson->IsValid() && !WantsDelete(*TransitionJson))
					{
						UAnimStateTransitionNode* TransitionNode = CreateAnimTransitionNode(StateMachineGraph, *TransitionJson, Context);
						ApplyTransitionGraphJson(Blueprint, TransitionNode, *TransitionJson, bAllowStructuralChanges, Context);
					}
				}
			}
		}
	}

	void ApplyGraphs(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Root, bool bApplyGraphChanges, bool bAllowStructuralChanges, FJsonToAssetContext& Context)
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
			if (WantsDelete(*GraphJson))
			{
				if (!bAllowStructuralChanges)
				{
					Context.AddWarning(TEXT("Graph delete requested without structural changes enabled"));
					continue;
				}

				if (Graph)
				{
					const FString GraphName = Graph->GetName();
					Graph->Modify();
					FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::Recompile);
					Context.AddChange(FString::Printf(TEXT("Deleted graph %s"), *GraphName));
				}
				else
				{
					FString GraphName;
					(*GraphJson)->TryGetStringField(TEXT("name"), GraphName);
					Context.AddWarning(FString::Printf(TEXT("Graph delete target not found: %s"), *GraphName));
				}

				continue;
			}

			if (!Graph)
			{
				if (bAllowStructuralChanges)
				{
					Graph = CreateGraphFromJson(Blueprint, *GraphJson, Context);
				}

				if (!Graph)
				{
					FString GraphName;
					(*GraphJson)->TryGetStringField(TEXT("name"), GraphName);
					Context.AddWarning(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
					continue;
				}
			}

			ApplyGraphNodes(Blueprint, Graph, *GraphJson, bAllowStructuralChanges, Context);
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
		const bool bApplyStructuralGraphChanges = bApplyGraphChanges && bAllowStructuralChanges;
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

		if (bApplyStructuralGraphChanges)
		{
			Context.AddWarning(TEXT("Structural changes are enabled. Node, graph, and AnimBP state-machine creation/deletion are supported."));
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

		if (bApplyGraphChanges)
		{
			ApplyAnimationStateMachines(Blueprint, Root, bApplyStructuralGraphChanges, Context);
		}
		ApplyGraphs(Blueprint, Root, bApplyGraphChanges, bApplyStructuralGraphChanges, Context);

		if (Context.bAnyChange)
		{
			if (bApplyStructuralGraphChanges)
			{
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			}
			else
			{
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			}
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
		const FString ResultJson = MakeResultJson(false, TEXT("JsonString is empty"), Context);
		WriteJsonToAssetEvidence(ResultJson, FString());
		return ResultJson;
	}

	TSharedPtr<FJsonObject> Root;
	FString Error;
	if (!ParseRootObject(JsonString, Root, Error))
	{
		const FString ResultJson = MakeResultJson(false, Error, Context);
		WriteJsonToAssetEvidence(ResultJson, FString());
		return ResultJson;
	}

	const FString ResultJson = ApplyBlueprintJsonRoot(Root, bSaveAsset, bCompileBlueprint, bApplyGraphChanges, bAllowStructuralChanges);
	WriteJsonToAssetEvidence(ResultJson, FString());
	return ResultJson;
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
		const FString ResultJson = MakeResultJson(false, TEXT("JsonFilePath is empty"), Context);
		WriteJsonToAssetEvidence(ResultJson, JsonFilePath);
		return ResultJson;
	}

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *JsonFilePath))
	{
		const FString ResultJson = MakeResultJson(false, FString::Printf(TEXT("Failed to read JSON file: %s"), *JsonFilePath), Context);
		WriteJsonToAssetEvidence(ResultJson, JsonFilePath);
		return ResultJson;
	}

	TSharedPtr<FJsonObject> Root;
	FString Error;
	if (!ParseRootObject(JsonString, Root, Error))
	{
		const FString ResultJson = MakeResultJson(false, Error, Context);
		WriteJsonToAssetEvidence(ResultJson, JsonFilePath);
		return ResultJson;
	}

	const FString ResultJson = ApplyBlueprintJsonRoot(Root, bSaveAsset, bCompileBlueprint, bApplyGraphChanges, bAllowStructuralChanges);
	WriteJsonToAssetEvidence(ResultJson, JsonFilePath);
	return ResultJson;
}
