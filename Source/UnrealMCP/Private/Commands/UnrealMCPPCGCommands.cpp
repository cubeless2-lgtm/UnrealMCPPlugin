#include "Commands/UnrealMCPPCGCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"

namespace
{
FString NormalizePCGObjectPathForLoad(const FString& ObjectPath)
{
    FString NormalizedPath = FPackageName::ExportTextPathToObjectPath(ObjectPath).TrimStartAndEnd();
    NormalizedPath.TrimQuotesInline();

    if ((NormalizedPath.StartsWith(TEXT("/Game/")) || NormalizedPath.StartsWith(TEXT("/Engine/"))) && !NormalizedPath.Contains(TEXT(".")))
    {
        const FString AssetName = FPackageName::GetShortName(NormalizedPath);
        NormalizedPath = FString::Printf(TEXT("%s.%s"), *NormalizedPath, *AssetName);
    }

    return NormalizedPath;
}

TArray<FString> FindPCGGraphAssetPaths(const FString& GraphPathOrName)
{
    TArray<FString> CandidatePaths;
    const FString Query = NormalizePCGObjectPathForLoad(GraphPathOrName);

    if (Query.StartsWith(TEXT("/Game/")) || Query.StartsWith(TEXT("/Engine/")))
    {
        if (LoadObject<UPCGGraph>(nullptr, *Query))
        {
            CandidatePaths.Add(Query);
            return CandidatePaths;
        }
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FAssetData> Assets;
    AssetRegistryModule.Get().GetAssetsByClass(UPCGGraph::StaticClass()->GetClassPathName(), Assets, true);

    const FString ShortQuery = FPackageName::GetShortName(Query);
    for (const FAssetData& AssetData : Assets)
    {
        const FString AssetName = AssetData.AssetName.ToString();
        const FString PackageName = AssetData.PackageName.ToString();
        const FString ObjectPath = AssetData.GetObjectPathString();

        if (AssetName.Equals(GraphPathOrName, ESearchCase::IgnoreCase) ||
            AssetName.Equals(ShortQuery, ESearchCase::IgnoreCase) ||
            PackageName.Equals(GraphPathOrName, ESearchCase::IgnoreCase) ||
            ObjectPath.Equals(Query, ESearchCase::IgnoreCase))
        {
            CandidatePaths.Add(ObjectPath);
        }
    }

    return CandidatePaths;
}

UPCGGraph* FindPCGGraph(const FString& GraphPathOrName)
{
    const FString Query = NormalizePCGObjectPathForLoad(GraphPathOrName);
    if (UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *Query))
    {
        return Graph;
    }

    const TArray<FString> CandidatePaths = FindPCGGraphAssetPaths(GraphPathOrName);
    if (CandidatePaths.Num() == 1)
    {
        return LoadObject<UPCGGraph>(nullptr, *CandidatePaths[0]);
    }

    return nullptr;
}

FString StripUClassPrefix(const FString& ClassName)
{
    if (ClassName.StartsWith(TEXT("U")) && ClassName.Len() > 1)
    {
        return ClassName.RightChop(1);
    }

    return ClassName;
}

UClass* FindSettingsClass(const FString& SettingsClassName)
{
    const FString NormalizedPath = NormalizePCGObjectPathForLoad(SettingsClassName);
    if (UClass* Class = LoadObject<UClass>(nullptr, *NormalizedPath))
    {
        return Class->IsChildOf(UPCGSettings::StaticClass()) ? Class : nullptr;
    }

    TArray<FString> CandidateNames;
    CandidateNames.Add(SettingsClassName);
    CandidateNames.Add(StripUClassPrefix(SettingsClassName));
    CandidateNames.Add(FString::Printf(TEXT("/Script/PCG.%s"), *SettingsClassName));
    CandidateNames.Add(FString::Printf(TEXT("/Script/PCG.%s"), *StripUClassPrefix(SettingsClassName)));

    for (const FString& CandidateName : CandidateNames)
    {
        if (UClass* Class = FindFirstObject<UClass>(*CandidateName))
        {
            if (Class->IsChildOf(UPCGSettings::StaticClass()))
            {
                return Class;
            }
        }

        if (UClass* Class = LoadObject<UClass>(nullptr, *CandidateName))
        {
            if (Class->IsChildOf(UPCGSettings::StaticClass()))
            {
                return Class;
            }
        }
    }

    return nullptr;
}

UPCGNode* FindPCGNodeById(UPCGGraph* Graph, const FString& NodeId)
{
    if (!Graph)
    {
        return nullptr;
    }

    auto MatchesNode = [&NodeId](UPCGNode* Node)
    {
        if (!Node)
        {
            return false;
        }

        const FString AuthoredTitle = Node->GetAuthoredTitleName().ToString();
        return Node->GetName().Equals(NodeId, ESearchCase::IgnoreCase) ||
            Node->GetPathName().Equals(NodeId, ESearchCase::IgnoreCase) ||
            AuthoredTitle.Equals(NodeId, ESearchCase::IgnoreCase);
    };

    if (MatchesNode(Graph->GetInputNode()))
    {
        return Graph->GetInputNode();
    }
    if (MatchesNode(Graph->GetOutputNode()))
    {
        return Graph->GetOutputNode();
    }

    for (UPCGNode* Node : Graph->GetNodes())
    {
        if (MatchesNode(Node))
        {
            return Node;
        }
    }

    return nullptr;
}

FString JsonValueToPCGImportText(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value.IsValid())
    {
        return FString();
    }

    if (Value->Type == EJson::String)
    {
        return Value->AsString();
    }
    if (Value->Type == EJson::Boolean)
    {
        return Value->AsBool() ? TEXT("True") : TEXT("False");
    }
    if (Value->Type == EJson::Number)
    {
        return FString::SanitizeFloat(Value->AsNumber());
    }

    FString Serialized;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
    FJsonSerializer::Serialize(Value, TEXT(""), Writer);
    return Serialized;
}

UObject* LoadPCGObjectValue(const FString& ObjectPath)
{
    return LoadObject<UObject>(nullptr, *NormalizePCGObjectPathForLoad(ObjectPath));
}

UClass* LoadPCGClassValue(const FString& ClassPathOrName)
{
    const FString NormalizedPath = NormalizePCGObjectPathForLoad(ClassPathOrName);
    if (UClass* Class = LoadObject<UClass>(nullptr, *NormalizedPath))
    {
        return Class;
    }

    if (UClass* Class = FindFirstObject<UClass>(*ClassPathOrName))
    {
        return Class;
    }

    return nullptr;
}

bool SetPCGSettingsProperty(UPCGSettings* Settings, const FString& PropertyName, const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage)
{
    if (!Settings)
    {
        OutErrorMessage = TEXT("Invalid PCG settings object");
        return false;
    }

    Settings->Modify();

    if (FUnrealMCPCommonUtils::SetObjectProperty(Settings, PropertyName, Value, OutErrorMessage))
    {
        return true;
    }

    FProperty* Property = Settings->GetClass()->FindPropertyByName(*PropertyName);
    if (!Property)
    {
        OutErrorMessage = FString::Printf(TEXT("Property not found: %s"), *PropertyName);
        return false;
    }

    void* PropertyAddr = Property->ContainerPtrToValuePtr<void>(Settings);
    if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
    {
        NameProperty->SetPropertyValue(PropertyAddr, FName(*Value->AsString()));
        return true;
    }

    if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
    {
        TextProperty->SetPropertyValue(PropertyAddr, FText::FromString(Value->AsString()));
        return true;
    }

    if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
    {
        if (Value->Type != EJson::Number)
        {
            OutErrorMessage = FString::Printf(TEXT("Numeric property requires number value: %s"), *PropertyName);
            return false;
        }

        if (NumericProperty->IsInteger())
        {
            NumericProperty->SetIntPropertyValue(PropertyAddr, static_cast<int64>(Value->AsNumber()));
        }
        else
        {
            NumericProperty->SetFloatingPointPropertyValue(PropertyAddr, Value->AsNumber());
        }
        return true;
    }

    if (FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
    {
        UClass* ClassValue = LoadPCGClassValue(Value->AsString());
        if (!ClassValue)
        {
            OutErrorMessage = FString::Printf(TEXT("Class not found: %s"), *Value->AsString());
            return false;
        }
        ClassProperty->SetPropertyValue(PropertyAddr, ClassValue);
        return true;
    }

    if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
    {
        UObject* ObjectValue = LoadPCGObjectValue(Value->AsString());
        if (!ObjectValue)
        {
            OutErrorMessage = FString::Printf(TEXT("Object not found: %s"), *Value->AsString());
            return false;
        }
        ObjectProperty->SetObjectPropertyValue(PropertyAddr, ObjectValue);
        return true;
    }

    if (Value->Type == EJson::String)
    {
        const FString ImportText = JsonValueToPCGImportText(Value);
        if (Property->ImportText_Direct(*ImportText, PropertyAddr, Settings, PPF_None) != nullptr)
        {
            return true;
        }
    }

    OutErrorMessage = FString::Printf(TEXT("Unsupported property type: %s for property %s"), *Property->GetClass()->GetName(), *PropertyName);
    return false;
}

TSharedPtr<FJsonObject> PCGPinToJson(const UPCGPin* Pin)
{
    TSharedPtr<FJsonObject> PinObject = MakeShared<FJsonObject>();
    if (!Pin)
    {
        return PinObject;
    }

    PinObject->SetStringField(TEXT("label"), Pin->Properties.Label.ToString());
    PinObject->SetStringField(TEXT("direction"), Pin->IsOutputPin() ? TEXT("output") : TEXT("input"));
    PinObject->SetStringField(TEXT("allowed_types"), Pin->Properties.AllowedTypes.ToString());
    PinObject->SetStringField(TEXT("current_types"), Pin->GetCurrentTypesID().ToString());
    PinObject->SetBoolField(TEXT("allows_multiple_data"), Pin->AllowsMultipleData());
    PinObject->SetBoolField(TEXT("allows_multiple_connections"), Pin->AllowsMultipleConnections());
    PinObject->SetBoolField(TEXT("connected"), Pin->IsConnected());

    TArray<TSharedPtr<FJsonValue>> Edges;
    for (const UPCGEdge* Edge : Pin->Edges)
    {
        if (!Edge || !Edge->IsValid())
        {
            continue;
        }

        const UPCGPin* OtherPin = Edge->GetOtherPin(Pin);
        const UPCGNode* OtherNode = OtherPin ? OtherPin->Node.Get() : nullptr;
        if (!OtherPin || !OtherNode)
        {
            continue;
        }

        TSharedPtr<FJsonObject> EdgeObject = MakeShared<FJsonObject>();
        EdgeObject->SetStringField(TEXT("node_id"), OtherNode->GetName());
        EdgeObject->SetStringField(TEXT("node_path"), OtherNode->GetPathName());
        EdgeObject->SetStringField(TEXT("pin_label"), OtherPin->Properties.Label.ToString());
        EdgeObject->SetStringField(TEXT("pin_direction"), OtherPin->IsOutputPin() ? TEXT("output") : TEXT("input"));
        Edges.Add(MakeShared<FJsonValueObject>(EdgeObject));
    }

    PinObject->SetArrayField(TEXT("edges"), Edges);
    return PinObject;
}

TSharedPtr<FJsonObject> PCGNodeToJson(UPCGNode* Node, bool bIncludePins)
{
    TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
    if (!Node)
    {
        return NodeObject;
    }

    int32 PositionX = 0;
    int32 PositionY = 0;
#if WITH_EDITOR
    Node->GetNodePosition(PositionX, PositionY);
#endif

    UPCGSettings* Settings = Node->GetSettings();
    NodeObject->SetStringField(TEXT("node_id"), Node->GetName());
    NodeObject->SetStringField(TEXT("object_path"), Node->GetPathName());
    NodeObject->SetStringField(TEXT("title"), Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
    NodeObject->SetStringField(TEXT("authored_title"), Node->GetAuthoredTitleName().ToString());
    NodeObject->SetStringField(TEXT("settings_class"), Settings ? Settings->GetClass()->GetName() : FString());
    NodeObject->SetStringField(TEXT("settings_path"), Settings ? Settings->GetPathName() : FString());
    NodeObject->SetNumberField(TEXT("x"), PositionX);
    NodeObject->SetNumberField(TEXT("y"), PositionY);

    if (bIncludePins)
    {
        TArray<TSharedPtr<FJsonValue>> InputPins;
        for (const UPCGPin* Pin : Node->GetInputPins())
        {
            InputPins.Add(MakeShared<FJsonValueObject>(PCGPinToJson(Pin)));
        }

        TArray<TSharedPtr<FJsonValue>> OutputPins;
        for (const UPCGPin* Pin : Node->GetOutputPins())
        {
            OutputPins.Add(MakeShared<FJsonValueObject>(PCGPinToJson(Pin)));
        }

        NodeObject->SetArrayField(TEXT("input_pins"), InputPins);
        NodeObject->SetArrayField(TEXT("output_pins"), OutputPins);
    }

    return NodeObject;
}

bool MatchesNodeFilter(UPCGNode* Node, const FString& NodeType, const FString& TitleContains)
{
    if (!Node)
    {
        return false;
    }

    UPCGSettings* Settings = Node->GetSettings();
    const FString SettingsClassName = Settings ? Settings->GetClass()->GetName() : FString();
    const FString Title = Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString();

    if (!NodeType.IsEmpty() && !SettingsClassName.Contains(NodeType) && !Node->GetName().Contains(NodeType))
    {
        return false;
    }

    if (!TitleContains.IsEmpty() && !Title.Contains(TitleContains))
    {
        return false;
    }

    return true;
}

void NotifySettingsChanged(UPCGGraph* Graph, UPCGSettings* Settings, const FString& PropertyName)
{
    if (!Graph || !Settings)
    {
        return;
    }

#if WITH_EDITOR
    const EPCGChangeType ChangeType = EPCGChangeType::Settings | EPCGChangeType::Structural;
    Settings->OnSettingsChangedDelegate.Broadcast(Settings, ChangeType);
#endif
}
}

FUnrealMCPPCGCommands::FUnrealMCPPCGCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("resolve_pcg_graph"))
    {
        return HandleResolvePCGGraph(Params);
    }
    if (CommandType == TEXT("list_pcg_graph_nodes"))
    {
        return HandleListPCGGraphNodes(Params);
    }
    if (CommandType == TEXT("add_pcg_node"))
    {
        return HandleAddPCGNode(Params);
    }
    if (CommandType == TEXT("connect_pcg_nodes"))
    {
        return HandleConnectPCGNodes(Params);
    }
    if (CommandType == TEXT("set_pcg_node_setting"))
    {
        return HandleSetPCGNodeSetting(Params);
    }
    if (CommandType == TEXT("compile_or_notify_pcg_graph"))
    {
        return HandleCompileOrNotifyPCGGraph(Params);
    }
    if (CommandType == TEXT("save_pcg_graph"))
    {
        return HandleSavePCGGraph(Params);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown PCG command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleResolvePCGGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    if (!Params->TryGetStringField(TEXT("graph_path"), GraphPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'graph_path' parameter"));
    }

    TArray<TSharedPtr<FJsonValue>> CandidateArray;
    const TArray<FString> CandidatePaths = FindPCGGraphAssetPaths(GraphPath);
    for (const FString& CandidatePath : CandidatePaths)
    {
        CandidateArray.Add(MakeShared<FJsonValueString>(CandidatePath));
    }

    UPCGGraph* Graph = FindPCGGraph(GraphPath);
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("resolved"), Graph != nullptr);
    if (Graph)
    {
        ResultObj->SetStringField(TEXT("name"), Graph->GetName());
        ResultObj->SetStringField(TEXT("asset_path"), Graph->GetPathName());
    }
    ResultObj->SetArrayField(TEXT("candidates"), CandidateArray);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleListPCGGraphNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    if (!Params->TryGetStringField(TEXT("graph_path"), GraphPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'graph_path' parameter"));
    }

    FString NodeType;
    Params->TryGetStringField(TEXT("node_type"), NodeType);

    FString TitleContains;
    Params->TryGetStringField(TEXT("title_contains"), TitleContains);

    bool bIncludePins = true;
    if (Params->HasField(TEXT("include_pins")))
    {
        bIncludePins = Params->GetBoolField(TEXT("include_pins"));
    }

    UPCGGraph* Graph = FindPCGGraph(GraphPath);
    if (!Graph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));
    }

    TArray<TSharedPtr<FJsonValue>> Nodes;
    auto AddNodeIfMatching = [&Nodes, bIncludePins, &NodeType, &TitleContains](UPCGNode* Node)
    {
        if (MatchesNodeFilter(Node, NodeType, TitleContains))
        {
            Nodes.Add(MakeShared<FJsonValueObject>(PCGNodeToJson(Node, bIncludePins)));
        }
    };

    AddNodeIfMatching(Graph->GetInputNode());
    AddNodeIfMatching(Graph->GetOutputNode());
    for (UPCGNode* Node : Graph->GetNodes())
    {
        AddNodeIfMatching(Node);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("graph"), Graph->GetPathName());
    ResultObj->SetArrayField(TEXT("nodes"), Nodes);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleAddPCGNode(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    if (!Params->TryGetStringField(TEXT("graph_path"), GraphPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'graph_path' parameter"));
    }

    FString SettingsClassName;
    if (!Params->TryGetStringField(TEXT("settings_class"), SettingsClassName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'settings_class' parameter"));
    }

    UPCGGraph* Graph = FindPCGGraph(GraphPath);
    if (!Graph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));
    }

    UClass* SettingsClass = FindSettingsClass(SettingsClassName);
    if (!SettingsClass)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("PCG settings class not found: %s"), *SettingsClassName));
    }

    UPCGSettings* Settings = nullptr;
    UPCGNode* Node = Graph->AddNodeOfType(SettingsClass, Settings);
    if (!Node || !Settings)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to add PCG node"));
    }

    if (Params->HasField(TEXT("node_position")))
    {
        const FVector2D NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
#if WITH_EDITOR
        Node->SetNodePosition(static_cast<int32>(NodePosition.X), static_cast<int32>(NodePosition.Y));
#endif
    }

    FString NodeTitle;
    if (Params->TryGetStringField(TEXT("node_title"), NodeTitle) && !NodeTitle.IsEmpty())
    {
#if WITH_EDITOR
        Node->SetNodeTitle(FName(*NodeTitle));
#endif
    }

    const TSharedPtr<FJsonObject>* SettingsObject = nullptr;
    if (Params->TryGetObjectField(TEXT("settings"), SettingsObject))
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*SettingsObject)->Values)
        {
            FString ErrorMessage;
            if (!SetPCGSettingsProperty(Settings, Pair.Key, Pair.Value, ErrorMessage))
            {
                Graph->RemoveNode(Node);
                return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
            }
            NotifySettingsChanged(Graph, Settings, Pair.Key);
        }
    }

    TSharedPtr<FJsonObject> ResultObj = PCGNodeToJson(Node, true);
    ResultObj->SetStringField(TEXT("graph"), Graph->GetPathName());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleConnectPCGNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    if (!Params->TryGetStringField(TEXT("graph_path"), GraphPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'graph_path' parameter"));
    }

    FString FromNodeId;
    FString ToNodeId;
    FString FromPin;
    FString ToPin;
    if (!Params->TryGetStringField(TEXT("from_node_id"), FromNodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'from_node_id' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("to_node_id"), ToNodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'to_node_id' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("from_pin"), FromPin))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'from_pin' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("to_pin"), ToPin))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'to_pin' parameter"));
    }

    UPCGGraph* Graph = FindPCGGraph(GraphPath);
    if (!Graph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));
    }

    UPCGNode* FromNode = FindPCGNodeById(Graph, FromNodeId);
    UPCGNode* ToNode = FindPCGNodeById(Graph, ToNodeId);
    if (!FromNode || !ToNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Source or target PCG node not found"));
    }

    if (!FromNode->GetOutputPin(FName(*FromPin)))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Source output pin not found: %s"), *FromPin));
    }
    if (!ToNode->GetInputPin(FName(*ToPin)))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Target input pin not found: %s"), *ToPin));
    }

    Graph->AddEdge(FromNode, FName(*FromPin), ToNode, FName(*ToPin));
    Graph->MarkPackageDirty();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("graph"), Graph->GetPathName());
    ResultObj->SetStringField(TEXT("from_node_id"), FromNode->GetName());
    ResultObj->SetStringField(TEXT("from_pin"), FromPin);
    ResultObj->SetStringField(TEXT("to_node_id"), ToNode->GetName());
    ResultObj->SetStringField(TEXT("to_pin"), ToPin);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleSetPCGNodeSetting(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    if (!Params->TryGetStringField(TEXT("graph_path"), GraphPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'graph_path' parameter"));
    }

    FString NodeId;
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'node_id' parameter"));
    }

    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_name' parameter"));
    }

    TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
    if (!Value.IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'value' parameter"));
    }

    UPCGGraph* Graph = FindPCGGraph(GraphPath);
    if (!Graph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));
    }

    UPCGNode* Node = FindPCGNodeById(Graph, NodeId);
    if (!Node)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("PCG node not found: %s"), *NodeId));
    }

    UPCGSettings* Settings = Node->GetSettings();
    FString ErrorMessage;
    if (!SetPCGSettingsProperty(Settings, PropertyName, Value, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    NotifySettingsChanged(Graph, Settings, PropertyName);
    Graph->MarkPackageDirty();

    TSharedPtr<FJsonObject> ResultObj = PCGNodeToJson(Node, true);
    ResultObj->SetStringField(TEXT("updated_property"), PropertyName);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleCompileOrNotifyPCGGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    if (!Params->TryGetStringField(TEXT("graph_path"), GraphPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'graph_path' parameter"));
    }

    UPCGGraph* Graph = FindPCGGraph(GraphPath);
    if (!Graph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));
    }

    bool bRecompiled = false;
#if WITH_EDITOR
    Graph->ForceNotificationForEditor(EPCGChangeType::Structural);
    bRecompiled = Graph->Recompile();
#endif

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("graph"), Graph->GetPathName());
    ResultObj->SetBoolField(TEXT("notified"), true);
    ResultObj->SetBoolField(TEXT("recompiled"), bRecompiled);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleSavePCGGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    if (!Params->TryGetStringField(TEXT("graph_path"), GraphPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'graph_path' parameter"));
    }

    UPCGGraph* Graph = FindPCGGraph(GraphPath);
    if (!Graph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));
    }

    const bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(Graph, false);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("graph"), Graph->GetPathName());
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    return ResultObj;
}
