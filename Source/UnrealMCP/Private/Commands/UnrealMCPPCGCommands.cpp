#include "Commands/UnrealMCPPCGCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SplineComponent.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "PCGCommon.h"
#include "PCGComponent.h"
#include "PCGData.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGManagedResource.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"

namespace
{
TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& Value)
{
    TArray<TSharedPtr<FJsonValue>> Array;
    Array.Add(MakeShared<FJsonValueNumber>(Value.X));
    Array.Add(MakeShared<FJsonValueNumber>(Value.Y));
    Array.Add(MakeShared<FJsonValueNumber>(Value.Z));
    return Array;
}

TSharedPtr<FJsonObject> BoxToJsonObject(const FBox& Box)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    const bool bIsValid = Box.IsValid != 0;
    Object->SetBoolField(TEXT("valid"), bIsValid);
    if (bIsValid)
    {
        Object->SetArrayField(TEXT("min"), VectorToJsonArray(Box.Min));
        Object->SetArrayField(TEXT("max"), VectorToJsonArray(Box.Max));
        Object->SetArrayField(TEXT("center"), VectorToJsonArray(Box.GetCenter()));
        Object->SetArrayField(TEXT("extent"), VectorToJsonArray(Box.GetExtent()));
    }
    return Object;
}

UWorld* GetEditorWorldForPCGCommands()
{
    return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

FString GetActorLabelForPCGCommands(const AActor* Actor)
{
    if (!Actor)
    {
        return FString();
    }

#if WITH_EDITOR
    return Actor->GetActorLabel();
#else
    return Actor->GetName();
#endif
}

bool JsonBoolOrDefault(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, bool bDefault)
{
    if (!Params.IsValid() || !Params->HasField(FieldName))
    {
        return bDefault;
    }

    return Params->GetBoolField(FieldName);
}

bool TryReadJsonNumberCaseInsensitive(const TSharedPtr<FJsonObject>& Object, const TCHAR* LowerName, const TCHAR* UpperName, double& OutValue)
{
    return Object.IsValid() && (Object->TryGetNumberField(LowerName, OutValue) || Object->TryGetNumberField(UpperName, OutValue));
}

bool TryReadJsonVector(const TSharedPtr<FJsonValue>& Value, int32 PointIndex, FVector& OutPoint, FString& OutErrorMessage)
{
    if (!Value.IsValid())
    {
        OutErrorMessage = FString::Printf(TEXT("Point %d is invalid"), PointIndex);
        return false;
    }

    if (Value->Type == EJson::Array)
    {
        const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
        if (Array.Num() < 3)
        {
            OutErrorMessage = FString::Printf(TEXT("Point %d must contain at least 3 numeric values"), PointIndex);
            return false;
        }

        OutPoint = FVector(
            static_cast<float>(Array[0]->AsNumber()),
            static_cast<float>(Array[1]->AsNumber()),
            static_cast<float>(Array[2]->AsNumber()));
        return true;
    }

    if (Value->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        if (!TryReadJsonNumberCaseInsensitive(Object, TEXT("x"), TEXT("X"), X) ||
            !TryReadJsonNumberCaseInsensitive(Object, TEXT("y"), TEXT("Y"), Y) ||
            !TryReadJsonNumberCaseInsensitive(Object, TEXT("z"), TEXT("Z"), Z))
        {
            OutErrorMessage = FString::Printf(TEXT("Point %d object must contain x/y/z numeric fields"), PointIndex);
            return false;
        }

        OutPoint = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
        return true;
    }

    OutErrorMessage = FString::Printf(TEXT("Point %d must be an array or object"), PointIndex);
    return false;
}

bool ActorHasTagString(const AActor* Actor, const FString& Tag)
{
    return Actor && !Tag.IsEmpty() && Actor->Tags.Contains(FName(*Tag));
}

bool ComponentHasTagString(const UActorComponent* Component, const FString& Tag)
{
    return Component && !Tag.IsEmpty() && Component->ComponentTags.Contains(FName(*Tag));
}

bool ActorMatchesPCGCommandFilters(
    const AActor* Actor,
    const FString& ActorLabel,
    const FString& ActorName,
    const FString& ActorPath,
    const FString& ActorTag,
    const FString& ActorLabelPrefix)
{
    if (!Actor)
    {
        return false;
    }

    const FString Label = GetActorLabelForPCGCommands(Actor);
    const FString Name = Actor->GetName();
    const FString Path = Actor->GetPathName();

    if (!ActorLabel.IsEmpty() && !Label.Equals(ActorLabel, ESearchCase::IgnoreCase))
    {
        return false;
    }
    if (!ActorName.IsEmpty() && !Name.Equals(ActorName, ESearchCase::IgnoreCase))
    {
        return false;
    }
    if (!ActorPath.IsEmpty() && !Path.Equals(ActorPath, ESearchCase::IgnoreCase))
    {
        return false;
    }
    if (!ActorTag.IsEmpty() && !ActorHasTagString(Actor, ActorTag))
    {
        return false;
    }
    if (!ActorLabelPrefix.IsEmpty() && !Label.StartsWith(ActorLabelPrefix, ESearchCase::IgnoreCase))
    {
        return false;
    }

    return true;
}

TArray<AActor*> FindActorsForPCGCommands(
    UWorld* World,
    const FString& ActorLabel,
    const FString& ActorName,
    const FString& ActorPath,
    const FString& ActorTag,
    const FString& ActorLabelPrefix,
    bool bSelectedOnly)
{
    TArray<AActor*> Actors;
    if (!World)
    {
        return Actors;
    }

    auto AddIfMatches = [&](AActor* Actor)
    {
        if (IsValid(Actor) && ActorMatchesPCGCommandFilters(Actor, ActorLabel, ActorName, ActorPath, ActorTag, ActorLabelPrefix))
        {
            Actors.AddUnique(Actor);
        }
    };

    if (bSelectedOnly)
    {
        USelection* SelectedActors = GEditor ? GEditor->GetSelectedActors() : nullptr;
        if (SelectedActors)
        {
            for (FSelectionIterator It(*SelectedActors); It; ++It)
            {
                AddIfMatches(Cast<AActor>(*It));
            }
        }
        return Actors;
    }

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AddIfMatches(*It);
    }

    return Actors;
}

bool IsTrashComponentName(const UActorComponent* Component)
{
    return Component && Component->GetName().StartsWith(TEXT("TRASH_"));
}

TSharedPtr<FJsonObject> ActorSummaryToJson(const AActor* Actor)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    if (!Actor)
    {
        return Object;
    }

    Object->SetStringField(TEXT("label"), GetActorLabelForPCGCommands(Actor));
    Object->SetStringField(TEXT("name"), Actor->GetName());
    Object->SetStringField(TEXT("path"), Actor->GetPathName());

    TArray<TSharedPtr<FJsonValue>> Tags;
    for (const FName& Tag : Actor->Tags)
    {
        Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
    }
    Object->SetArrayField(TEXT("tags"), Tags);
    return Object;
}

USplineComponent* FindSplineComponentForPCGCommand(
    AActor* Actor,
    const FString& ComponentName,
    const FString& ComponentTag,
    TArray<USplineComponent*>& OutCandidates)
{
    OutCandidates.Reset();
    if (!Actor)
    {
        return nullptr;
    }

    TArray<USplineComponent*> SplineComponents;
    Actor->GetComponents<USplineComponent>(SplineComponents);

    for (USplineComponent* Component : SplineComponents)
    {
        if (IsValid(Component))
        {
            OutCandidates.Add(Component);
        }
    }

    auto IsNameMatch = [&ComponentName](USplineComponent* Component)
    {
        return ComponentName.IsEmpty() ||
            Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase) ||
            Component->GetFName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase);
    };

    auto IsTagMatch = [&ComponentTag](USplineComponent* Component)
    {
        return ComponentTag.IsEmpty() || ComponentHasTagString(Component, ComponentTag);
    };

    USplineComponent* TrashFallback = nullptr;
    for (USplineComponent* Component : OutCandidates)
    {
        if (IsNameMatch(Component) && IsTagMatch(Component))
        {
            if (!IsTrashComponentName(Component))
            {
                return Component;
            }
            if (!TrashFallback)
            {
                TrashFallback = Component;
            }
        }
    }

    return TrashFallback;
}

TSharedPtr<FJsonObject> SplineComponentSummaryToJson(USplineComponent* SplineComponent)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    if (!SplineComponent)
    {
        return Object;
    }

    Object->SetStringField(TEXT("name"), SplineComponent->GetName());
    Object->SetStringField(TEXT("path"), SplineComponent->GetPathName());
    Object->SetBoolField(TEXT("is_trash_component"), IsTrashComponentName(SplineComponent));
    Object->SetNumberField(TEXT("point_count"), SplineComponent->GetNumberOfSplinePoints());
    Object->SetNumberField(TEXT("spline_length"), SplineComponent->GetSplineLength());
    Object->SetBoolField(TEXT("closed_loop"), SplineComponent->IsClosedLoop());

    TArray<TSharedPtr<FJsonValue>> Tags;
    for (const FName& Tag : SplineComponent->ComponentTags)
    {
        Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
    }
    Object->SetArrayField(TEXT("tags"), Tags);
    return Object;
}

int32 GetPCGManagedResourceCount(UPCGComponent* PCGComponent)
{
    if (!PCGComponent)
    {
        return 0;
    }

    int32 ManagedResourceCount = 0;
    if (PCGComponent->AreManagedResourcesAccessible())
    {
        PCGComponent->ForEachConstManagedResource([&ManagedResourceCount](const UPCGManagedResource*)
        {
            ++ManagedResourceCount;
        });
    }
    return ManagedResourceCount;
}

bool IsPCGComponentBusy(UPCGComponent* PCGComponent)
{
    if (!PCGComponent)
    {
        return false;
    }

    bool bRefreshInProgress = false;
#if WITH_EDITOR
    bRefreshInProgress = PCGComponent->IsRefreshInProgress();
#endif
    return PCGComponent->IsGenerating() || PCGComponent->IsCleaningUp() || bRefreshInProgress;
}

bool PCGComponentMeetsReadbackMinimums(UPCGComponent* PCGComponent, int32 MinGeneratedOutputDataCount, int32 MinManagedResourceCount)
{
    if (!PCGComponent)
    {
        return false;
    }

    if (MinGeneratedOutputDataCount >= 0)
    {
        const FPCGDataCollection& GeneratedOutput = PCGComponent->GetGeneratedGraphOutput();
        if (GeneratedOutput.TaggedData.Num() < MinGeneratedOutputDataCount)
        {
            return false;
        }
    }

    if (MinManagedResourceCount >= 0 && GetPCGManagedResourceCount(PCGComponent) < MinManagedResourceCount)
    {
        return false;
    }

    return true;
}

TSharedPtr<FJsonObject> PCGComponentSummaryToJson(UPCGComponent* PCGComponent)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    if (!PCGComponent)
    {
        return Object;
    }

    const int32 ManagedResourceCount = GetPCGManagedResourceCount(PCGComponent);

    const FPCGDataCollection& GeneratedOutput = PCGComponent->GetGeneratedGraphOutput();
    const UPCGGraph* Graph = PCGComponent->GetGraph();

    Object->SetStringField(TEXT("name"), PCGComponent->GetName());
    Object->SetStringField(TEXT("path"), PCGComponent->GetPathName());
    Object->SetBoolField(TEXT("active"), PCGComponent->IsActive());
    Object->SetBoolField(TEXT("registered"), PCGComponent->IsRegistered());
    Object->SetBoolField(TEXT("generating"), PCGComponent->IsGenerating());
    Object->SetBoolField(TEXT("cleaning_up"), PCGComponent->IsCleaningUp());
    Object->SetBoolField(TEXT("managed_resources_accessible"), PCGComponent->AreManagedResourcesAccessible());
    Object->SetNumberField(TEXT("managed_resource_count"), ManagedResourceCount);
    Object->SetNumberField(TEXT("generated_output_data_count"), GeneratedOutput.TaggedData.Num());
    Object->SetStringField(TEXT("graph"), Graph ? Graph->GetPathName() : FString());
    Object->SetObjectField(TEXT("last_generated_bounds"), BoxToJsonObject(PCGComponent->GetLastGeneratedBounds()));
#if WITH_EDITOR
    Object->SetBoolField(TEXT("refresh_in_progress"), PCGComponent->IsRefreshInProgress());
    Object->SetBoolField(TEXT("was_generated_this_session"), PCGComponent->WasGeneratedThisSession());
#endif
    return Object;
}

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
    if (CommandType == TEXT("set_spline_component_points"))
    {
        return HandleSetSplineComponentPoints(Params);
    }
    if (CommandType == TEXT("refresh_pcg_components"))
    {
        return HandleRefreshPCGComponents(Params);
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

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleSetSplineComponentPoints(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorldForPCGCommands();
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    FString ActorLabel;
    FString ActorName;
    FString ActorPath;
    FString ActorTag;
    FString ActorLabelPrefix;
    Params->TryGetStringField(TEXT("actor_label"), ActorLabel);
    Params->TryGetStringField(TEXT("actor_name"), ActorName);
    Params->TryGetStringField(TEXT("actor_path"), ActorPath);
    Params->TryGetStringField(TEXT("actor_tag"), ActorTag);
    Params->TryGetStringField(TEXT("actor_label_prefix"), ActorLabelPrefix);
    const bool bSelectedOnly = JsonBoolOrDefault(Params, TEXT("selected_only"), false);

    if (!bSelectedOnly && ActorLabel.IsEmpty() && ActorName.IsEmpty() && ActorPath.IsEmpty() && ActorTag.IsEmpty() && ActorLabelPrefix.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("set_spline_component_points requires actor_label, actor_name, actor_path, actor_tag, actor_label_prefix, or selected_only=true"));
    }

    const TArray<AActor*> MatchingActors = FindActorsForPCGCommands(World, ActorLabel, ActorName, ActorPath, ActorTag, ActorLabelPrefix, bSelectedOnly);
    if (MatchingActors.Num() == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No matching actor found"));
    }
    if (MatchingActors.Num() > 1 && !JsonBoolOrDefault(Params, TEXT("allow_multiple_actor_matches"), false))
    {
        TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> ActorArray;
        for (AActor* Actor : MatchingActors)
        {
            ActorArray.Add(MakeShared<FJsonValueObject>(ActorSummaryToJson(Actor)));
        }
        ErrorData->SetStringField(TEXT("error"), TEXT("Multiple matching actors found"));
        ErrorData->SetArrayField(TEXT("actors"), ActorArray);
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Multiple matching actors found; pass allow_multiple_actor_matches=true to use the first match"));
    }

    const TArray<TSharedPtr<FJsonValue>>* PointValues = nullptr;
    if (!Params->TryGetArrayField(TEXT("points"), PointValues) || !PointValues || PointValues->Num() < 2)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'points' must contain at least two points"));
    }

    TArray<FVector> Points;
    Points.Reserve(PointValues->Num());
    for (int32 Index = 0; Index < PointValues->Num(); ++Index)
    {
        FVector Point = FVector::ZeroVector;
        FString ErrorMessage;
        if (!TryReadJsonVector((*PointValues)[Index], Index, Point, ErrorMessage))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
        }
        Points.Add(Point);
    }

    FString ComponentName;
    FString ComponentTag;
    FString CoordinateSpaceName;
    FString PointTypeName;
    Params->TryGetStringField(TEXT("component_name"), ComponentName);
    Params->TryGetStringField(TEXT("component_tag"), ComponentTag);
    Params->TryGetStringField(TEXT("coordinate_space"), CoordinateSpaceName);
    Params->TryGetStringField(TEXT("point_type"), PointTypeName);

    const ESplineCoordinateSpace::Type CoordinateSpace =
        CoordinateSpaceName.Equals(TEXT("local"), ESearchCase::IgnoreCase)
            ? ESplineCoordinateSpace::Local
            : ESplineCoordinateSpace::World;

    ESplinePointType::Type PointType = ESplinePointType::Linear;
    if (PointTypeName.Equals(TEXT("curve"), ESearchCase::IgnoreCase))
    {
        PointType = ESplinePointType::Curve;
    }
    else if (PointTypeName.Equals(TEXT("curve_clamped"), ESearchCase::IgnoreCase))
    {
        PointType = ESplinePointType::CurveClamped;
    }
    else if (PointTypeName.Equals(TEXT("constant"), ESearchCase::IgnoreCase))
    {
        PointType = ESplinePointType::Constant;
    }

    AActor* Actor = MatchingActors[0];
    TArray<USplineComponent*> CandidateComponents;
    USplineComponent* SplineComponent = FindSplineComponentForPCGCommand(Actor, ComponentName, ComponentTag, CandidateComponents);
    if (!SplineComponent)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("No matching spline component found on actor: %s"), *GetActorLabelForPCGCommands(Actor)));
    }

    const bool bHasClosedLoopField = Params->HasField(TEXT("closed_loop"));
    const bool bClosedLoop = JsonBoolOrDefault(Params, TEXT("closed_loop"), SplineComponent->IsClosedLoop());
    const bool bInputToConstructionScript = JsonBoolOrDefault(Params, TEXT("input_to_construction_script"), true);
    const bool bRerunConstructionScripts = JsonBoolOrDefault(Params, TEXT("rerun_construction_scripts"), false);
    const bool bMarkDirty = JsonBoolOrDefault(Params, TEXT("mark_dirty"), true);

    Actor->Modify();
    SplineComponent->Modify();
    SplineComponent->bInputSplinePointsToConstructionScript = bInputToConstructionScript;
    SplineComponent->bSplineHasBeenEdited = true;
    SplineComponent->SetSplinePoints(Points, CoordinateSpace, false);
    for (int32 Index = 0; Index < Points.Num(); ++Index)
    {
        SplineComponent->SetSplinePointType(Index, PointType, false);
    }
    if (bHasClosedLoopField)
    {
        SplineComponent->SetClosedLoop(bClosedLoop, false);
    }
    SplineComponent->UpdateSpline();
    SplineComponent->MarkRenderStateDirty();

    if (bMarkDirty)
    {
        SplineComponent->MarkPackageDirty();
        Actor->MarkPackageDirty();
    }

    if (bRerunConstructionScripts)
    {
        Actor->RerunConstructionScripts();
        CandidateComponents.Reset();
        SplineComponent = FindSplineComponentForPCGCommand(Actor, ComponentName, ComponentTag, CandidateComponents);
        if (!SplineComponent)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Spline component was not found after rerunning construction scripts"));
        }
    }

    const int32 FinalPointCount = SplineComponent->GetNumberOfSplinePoints();
    const int32 ComparedPointCount = FMath::Min(FinalPointCount, Points.Num());
    double MaxPointDelta = 0.0;
    TArray<TSharedPtr<FJsonValue>> FinalPointsArray;
    for (int32 Index = 0; Index < FinalPointCount; ++Index)
    {
        const FVector FinalPoint = SplineComponent->GetLocationAtSplinePoint(Index, CoordinateSpace);
        FinalPointsArray.Add(MakeShared<FJsonValueArray>(VectorToJsonArray(FinalPoint)));
        if (Index < ComparedPointCount)
        {
            MaxPointDelta = FMath::Max(MaxPointDelta, static_cast<double>(FVector::Dist(FinalPoint, Points[Index])));
        }
    }

    TArray<TSharedPtr<FJsonValue>> CandidateArray;
    for (USplineComponent* Candidate : CandidateComponents)
    {
        CandidateArray.Add(MakeShared<FJsonValueObject>(SplineComponentSummaryToJson(Candidate)));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetObjectField(TEXT("actor"), ActorSummaryToJson(Actor));
    ResultObj->SetObjectField(TEXT("spline_component"), SplineComponentSummaryToJson(SplineComponent));
    ResultObj->SetArrayField(TEXT("candidate_components"), CandidateArray);
    ResultObj->SetArrayField(TEXT("points"), FinalPointsArray);
    ResultObj->SetNumberField(TEXT("requested_point_count"), Points.Num());
    ResultObj->SetNumberField(TEXT("final_point_count"), FinalPointCount);
    ResultObj->SetNumberField(TEXT("max_point_delta_cm"), MaxPointDelta);
    const double Tolerance = JsonBoolOrDefault(Params, TEXT("strict_tolerance"), false) ? 0.01 : 5.0;
    ResultObj->SetNumberField(TEXT("tolerance_cm"), Tolerance);
    ResultObj->SetBoolField(TEXT("within_tolerance"), MaxPointDelta <= Tolerance);
    ResultObj->SetBoolField(TEXT("reran_construction_scripts"), bRerunConstructionScripts);
    ResultObj->SetStringField(TEXT("coordinate_space"), CoordinateSpace == ESplineCoordinateSpace::Local ? TEXT("local") : TEXT("world"));
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleRefreshPCGComponents(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorldForPCGCommands();
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    FString ActorLabel;
    FString ActorName;
    FString ActorPath;
    FString ActorTag;
    FString ActorLabelPrefix;
    FString ComponentName;
    FString ComponentTag;
    Params->TryGetStringField(TEXT("actor_label"), ActorLabel);
    Params->TryGetStringField(TEXT("actor_name"), ActorName);
    Params->TryGetStringField(TEXT("actor_path"), ActorPath);
    Params->TryGetStringField(TEXT("actor_tag"), ActorTag);
    Params->TryGetStringField(TEXT("actor_label_prefix"), ActorLabelPrefix);
    Params->TryGetStringField(TEXT("component_name"), ComponentName);
    Params->TryGetStringField(TEXT("component_tag"), ComponentTag);

    const bool bSelectedOnly = JsonBoolOrDefault(Params, TEXT("selected_only"), false);
    const bool bCleanup = JsonBoolOrDefault(Params, TEXT("cleanup"), false);
    const bool bImmediateCleanup = JsonBoolOrDefault(Params, TEXT("immediate_cleanup"), true);
    const bool bRemoveComponents = JsonBoolOrDefault(Params, TEXT("remove_components"), true);
    const bool bNotifyPropertiesChanged = JsonBoolOrDefault(Params, TEXT("notify_properties_changed"), false);
    const bool bRefresh = JsonBoolOrDefault(Params, TEXT("refresh"), false);
    const bool bCancelExistingRefresh = JsonBoolOrDefault(Params, TEXT("cancel_existing_refresh"), true);
    const bool bGenerate = JsonBoolOrDefault(Params, TEXT("generate"), true);
    const bool bForce = JsonBoolOrDefault(Params, TEXT("force"), true);
    const bool bWaitUntilComplete = JsonBoolOrDefault(Params, TEXT("wait_until_complete"), false);

    double WaitTimeoutSeconds = 10.0;
    if (Params->HasField(TEXT("timeout_seconds")))
    {
        WaitTimeoutSeconds = Params->GetNumberField(TEXT("timeout_seconds"));
    }
    WaitTimeoutSeconds = FMath::Clamp(WaitTimeoutSeconds, 0.0, 300.0);

    double PollIntervalSeconds = 0.05;
    if (Params->HasField(TEXT("poll_interval_seconds")))
    {
        PollIntervalSeconds = Params->GetNumberField(TEXT("poll_interval_seconds"));
    }
    PollIntervalSeconds = FMath::Clamp(PollIntervalSeconds, 0.005, 1.0);

    int32 MinGeneratedOutputDataCount = -1;
    if (Params->HasField(TEXT("min_generated_output_data_count")))
    {
        MinGeneratedOutputDataCount = Params->GetIntegerField(TEXT("min_generated_output_data_count"));
    }

    int32 MinManagedResourceCount = -1;
    if (Params->HasField(TEXT("min_managed_resource_count")))
    {
        MinManagedResourceCount = Params->GetIntegerField(TEXT("min_managed_resource_count"));
    }

    int32 MaxComponents = 1000;
    if (Params->HasField(TEXT("max_components")))
    {
        MaxComponents = FMath::Max(1, Params->GetIntegerField(TEXT("max_components")));
    }

    const TArray<AActor*> MatchingActors = FindActorsForPCGCommands(World, ActorLabel, ActorName, ActorPath, ActorTag, ActorLabelPrefix, bSelectedOnly);
    TArray<TSharedPtr<FJsonValue>> ActorArray;
    TArray<TSharedPtr<FJsonValue>> ComponentArray;

    int32 PCGActorCount = 0;
    int32 ComponentCount = 0;
    int32 CleanupCount = 0;
    int32 NotifyCount = 0;
    int32 RefreshCount = 0;
    int32 GenerateCount = 0;
    bool bComponentLimitHit = false;
    TArray<UPCGComponent*> ProcessedComponents;

    for (AActor* Actor : MatchingActors)
    {
        TArray<UPCGComponent*> PCGComponents;
        Actor->GetComponents<UPCGComponent>(PCGComponents);
        if (PCGComponents.Num() == 0)
        {
            continue;
        }

        bool bActorHasIncludedComponent = false;
        for (UPCGComponent* PCGComponent : PCGComponents)
        {
            if (!IsValid(PCGComponent))
            {
                continue;
            }
            if (!ComponentName.IsEmpty() && !PCGComponent->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
            {
                continue;
            }
            if (!ComponentTag.IsEmpty() && !ComponentHasTagString(PCGComponent, ComponentTag))
            {
                continue;
            }

            if (ComponentCount >= MaxComponents)
            {
                bComponentLimitHit = true;
                break;
            }

            if (bCleanup)
            {
                if (bImmediateCleanup)
                {
                    PCGComponent->CleanupLocalImmediate(bRemoveComponents, true);
                }
                else
                {
                    PCGComponent->CleanupLocal(bRemoveComponents);
                }
                ++CleanupCount;
            }

            if (bNotifyPropertiesChanged)
            {
                PCGComponent->NotifyPropertiesChangedFromBlueprint();
                ++NotifyCount;
            }

#if WITH_EDITOR
            if (bRefresh)
            {
                PCGComponent->DirtyGenerated();
                PCGComponent->Refresh(EPCGChangeType::Input, bCancelExistingRefresh);
                ++RefreshCount;
            }
#endif

            if (bGenerate)
            {
                PCGComponent->GenerateLocal(bForce);
                ++GenerateCount;
            }

            TSharedPtr<FJsonObject> ComponentObject = PCGComponentSummaryToJson(PCGComponent);
            ComponentObject->SetObjectField(TEXT("actor"), ActorSummaryToJson(Actor));
            ComponentArray.Add(MakeShared<FJsonValueObject>(ComponentObject));
            ProcessedComponents.Add(PCGComponent);
            bActorHasIncludedComponent = true;
            ++ComponentCount;
        }

        if (bActorHasIncludedComponent)
        {
            ActorArray.Add(MakeShared<FJsonValueObject>(ActorSummaryToJson(Actor)));
            ++PCGActorCount;
        }

        if (bComponentLimitHit)
        {
            break;
        }
    }

    bool bWaitCompleted = !bWaitUntilComplete;
    bool bWaitTimedOut = false;
    int32 WaitIterations = 0;
    double WaitElapsedSeconds = 0.0;

    auto AreProcessedComponentsReady = [&ProcessedComponents, MinGeneratedOutputDataCount, MinManagedResourceCount]()
    {
        for (UPCGComponent* PCGComponent : ProcessedComponents)
        {
            if (!IsValid(PCGComponent))
            {
                continue;
            }

            if (IsPCGComponentBusy(PCGComponent))
            {
                return false;
            }

            if (!PCGComponentMeetsReadbackMinimums(PCGComponent, MinGeneratedOutputDataCount, MinManagedResourceCount))
            {
                return false;
            }
        }

        return true;
    };

    if (bWaitUntilComplete && ProcessedComponents.Num() > 0)
    {
        bWaitCompleted = AreProcessedComponentsReady();
    }

    TArray<TSharedPtr<FJsonValue>> FinalComponentArray;
    for (UPCGComponent* PCGComponent : ProcessedComponents)
    {
        if (!IsValid(PCGComponent))
        {
            continue;
        }

        TSharedPtr<FJsonObject> ComponentObject = PCGComponentSummaryToJson(PCGComponent);
        if (AActor* Owner = PCGComponent->GetOwner())
        {
            ComponentObject->SetObjectField(TEXT("actor"), ActorSummaryToJson(Owner));
        }
        FinalComponentArray.Add(MakeShared<FJsonValueObject>(ComponentObject));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetNumberField(TEXT("matched_actor_count"), MatchingActors.Num());
    ResultObj->SetNumberField(TEXT("pcg_actor_count"), PCGActorCount);
    ResultObj->SetNumberField(TEXT("component_count"), ComponentCount);
    ResultObj->SetNumberField(TEXT("cleanup_count"), CleanupCount);
    ResultObj->SetNumberField(TEXT("notify_properties_changed_count"), NotifyCount);
    ResultObj->SetNumberField(TEXT("refresh_count"), RefreshCount);
    ResultObj->SetNumberField(TEXT("generate_count"), GenerateCount);
    ResultObj->SetBoolField(TEXT("component_limit_hit"), bComponentLimitHit);
    ResultObj->SetBoolField(TEXT("wait_requested"), bWaitUntilComplete);
    ResultObj->SetBoolField(TEXT("wait_supported"), true);
    ResultObj->SetStringField(TEXT("wait_mode"), TEXT("single_frame_readback"));
    ResultObj->SetStringField(TEXT("wait_note"), TEXT("Native bridge commands run on the game thread, so this command does not sleep-poll inside Unreal. External MCP callers should poll with generate=false until wait_completed is true."));
    ResultObj->SetBoolField(TEXT("wait_completed"), bWaitCompleted);
    ResultObj->SetBoolField(TEXT("wait_timed_out"), bWaitTimedOut);
    ResultObj->SetNumberField(TEXT("wait_elapsed_seconds"), WaitElapsedSeconds);
    ResultObj->SetNumberField(TEXT("wait_iterations"), WaitIterations);
    ResultObj->SetNumberField(TEXT("timeout_seconds"), WaitTimeoutSeconds);
    ResultObj->SetNumberField(TEXT("poll_interval_seconds"), PollIntervalSeconds);
    ResultObj->SetNumberField(TEXT("min_generated_output_data_count"), MinGeneratedOutputDataCount);
    ResultObj->SetNumberField(TEXT("min_managed_resource_count"), MinManagedResourceCount);
    ResultObj->SetArrayField(TEXT("actors"), ActorArray);
    if (bWaitUntilComplete)
    {
        ResultObj->SetArrayField(TEXT("initial_components"), ComponentArray);
    }
    ResultObj->SetArrayField(TEXT("components"), bWaitUntilComplete ? FinalComponentArray : ComponentArray);
    return ResultObj;
}
