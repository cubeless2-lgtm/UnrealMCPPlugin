#include "Commands/UnrealMCPBlueprintCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Factories/BlueprintFactory.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphToken.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "UObject/Field.h"
#include "UObject/FieldPath.h"
#include "UObject/UnrealType.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/PackageName.h"
#include "Misc/UObjectToken.h"

namespace
{
FString GetBlueprintStatusName(EBlueprintStatus Status)
{
    switch (Status)
    {
    case BS_Unknown:
        return TEXT("unknown");
    case BS_Dirty:
        return TEXT("dirty");
    case BS_Error:
        return TEXT("error");
    case BS_UpToDate:
        return TEXT("up_to_date");
    case BS_BeingCreated:
        return TEXT("being_created");
    case BS_UpToDateWithWarnings:
        return TEXT("up_to_date_with_warnings");
    default:
        return TEXT("unknown");
    }
}

FString GetCompilerMessageSeverityName(EMessageSeverity::Type Severity)
{
    if (static_cast<int32>(Severity) <= static_cast<int32>(EMessageSeverity::Error))
    {
        return TEXT("error");
    }
    if (Severity == EMessageSeverity::PerformanceWarning)
    {
        return TEXT("performance_warning");
    }
    if (Severity == EMessageSeverity::Warning)
    {
        return TEXT("warning");
    }

    return TEXT("info");
}

bool IsCompilerErrorSeverity(EMessageSeverity::Type Severity)
{
    return static_cast<int32>(Severity) <= static_cast<int32>(EMessageSeverity::Error);
}

bool IsCompilerWarningSeverity(EMessageSeverity::Type Severity)
{
    return Severity == EMessageSeverity::Warning || Severity == EMessageSeverity::PerformanceWarning;
}

TSharedPtr<FJsonValue> NumberJsonValue(double Value)
{
    return MakeShared<FJsonValueNumber>(Value);
}

TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& Value)
{
    TArray<TSharedPtr<FJsonValue>> Array;
    Array.Add(NumberJsonValue(Value.X));
    Array.Add(NumberJsonValue(Value.Y));
    Array.Add(NumberJsonValue(Value.Z));
    return Array;
}

TArray<TSharedPtr<FJsonValue>> RotatorToJsonArray(const FRotator& Value)
{
    TArray<TSharedPtr<FJsonValue>> Array;
    Array.Add(NumberJsonValue(Value.Pitch));
    Array.Add(NumberJsonValue(Value.Yaw));
    Array.Add(NumberJsonValue(Value.Roll));
    return Array;
}

FString BlueprintObjectPathOrEmpty(const UObject* Object)
{
    return Object ? Object->GetPathName() : FString();
}

TSharedPtr<FJsonValue> PropertyValueToJsonValue(FProperty* Property, const void* Container)
{
    if (!Property || !Container)
    {
        return MakeShared<FJsonValueNull>();
    }

    const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
    if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
    {
        return MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue(ValuePtr));
    }
    if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
    {
        const double Value = NumericProperty->IsInteger()
            ? static_cast<double>(NumericProperty->GetSignedIntPropertyValue(ValuePtr))
            : NumericProperty->GetFloatingPointPropertyValue(ValuePtr);
        return NumberJsonValue(Value);
    }
    if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
    {
        return MakeShared<FJsonValueString>(StringProperty->GetPropertyValue(ValuePtr));
    }
    if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
    {
        return MakeShared<FJsonValueString>(NameProperty->GetPropertyValue(ValuePtr).ToString());
    }
    if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
    {
        return MakeShared<FJsonValueString>(TextProperty->GetPropertyValue(ValuePtr).ToString());
    }
    if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
    {
        const int64 Value = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
        UEnum* Enum = EnumProperty->GetEnum();
        return MakeShared<FJsonValueString>(Enum ? Enum->GetNameStringByValue(Value) : FString::FromInt(Value));
    }
    if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
    {
        const uint8 Value = ByteProperty->GetPropertyValue(ValuePtr);
        UEnum* Enum = ByteProperty->Enum;
        return Enum ? MakeShared<FJsonValueString>(Enum->GetNameStringByValue(Value)) : NumberJsonValue(Value);
    }
    if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
    {
        if (StructProperty->Struct == TBaseStructure<FVector>::Get())
        {
            return MakeShared<FJsonValueArray>(VectorToJsonArray(*static_cast<const FVector*>(ValuePtr)));
        }
        if (StructProperty->Struct == TBaseStructure<FRotator>::Get())
        {
            return MakeShared<FJsonValueArray>(RotatorToJsonArray(*static_cast<const FRotator*>(ValuePtr)));
        }
    }
    if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
    {
        return MakeShared<FJsonValueString>(BlueprintObjectPathOrEmpty(ObjectProperty->GetObjectPropertyValue(ValuePtr)));
    }

    FString ExportedValue;
    Property->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, nullptr, PPF_None);
    return MakeShared<FJsonValueString>(ExportedValue);
}

FString GetCompilerPinDirectionName(const UEdGraphPin* Pin)
{
    return Pin && Pin->Direction == EGPD_Output ? TEXT("output") : TEXT("input");
}

void AddGraphContextToDiagnostic(TSharedPtr<FJsonObject> DiagnosticObj, const UEdGraph* Graph)
{
    if (!DiagnosticObj.IsValid() || !Graph)
    {
        return;
    }

    DiagnosticObj->SetStringField(TEXT("graph_id"), Graph->GraphGuid.ToString());
    DiagnosticObj->SetStringField(TEXT("graph_name"), Graph->GetName());
    DiagnosticObj->SetStringField(TEXT("graph_path"), Graph->GetPathName());
}

void AddNodeContextToDiagnostic(TSharedPtr<FJsonObject> DiagnosticObj, const UEdGraphNode* Node)
{
    if (!DiagnosticObj.IsValid() || !Node)
    {
        return;
    }

    AddGraphContextToDiagnostic(DiagnosticObj, Node->GetGraph());
    DiagnosticObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
    DiagnosticObj->SetStringField(TEXT("node_name"), Node->GetName());
    DiagnosticObj->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
    DiagnosticObj->SetStringField(TEXT("node_class"), Node->GetClass() ? Node->GetClass()->GetName() : FString());
}

void AddPinContextToDiagnostic(TSharedPtr<FJsonObject> DiagnosticObj, const UEdGraphPin* Pin)
{
    if (!DiagnosticObj.IsValid() || !Pin)
    {
        return;
    }

    AddNodeContextToDiagnostic(DiagnosticObj, Pin->GetOwningNodeUnchecked());
    DiagnosticObj->SetStringField(TEXT("pin_id"), Pin->PinId.ToString());
    DiagnosticObj->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
    DiagnosticObj->SetStringField(TEXT("pin_direction"), GetCompilerPinDirectionName(Pin));
    DiagnosticObj->SetStringField(TEXT("pin_category"), Pin->PinType.PinCategory.ToString());
    DiagnosticObj->SetStringField(TEXT("pin_subcategory"), Pin->PinType.PinSubCategory.ToString());
}

void AddObjectContextToDiagnostic(TSharedPtr<FJsonObject> DiagnosticObj, const UObject* Object)
{
    if (!DiagnosticObj.IsValid() || !Object)
    {
        return;
    }

    if (const UEdGraphNode* Node = Cast<UEdGraphNode>(Object))
    {
        AddNodeContextToDiagnostic(DiagnosticObj, Node);
    }
    else if (const UEdGraph* Graph = Cast<UEdGraph>(Object))
    {
        AddGraphContextToDiagnostic(DiagnosticObj, Graph);
    }
}

TSharedPtr<FJsonObject> CompilerMessageToDiagnosticJson(const TSharedRef<FTokenizedMessage>& Message, const FCompilerResultsLog& ResultsLog)
{
    TSharedPtr<FJsonObject> DiagnosticObj = MakeShared<FJsonObject>();

    const EMessageSeverity::Type Severity = Message->GetSeverity();
    DiagnosticObj->SetStringField(TEXT("severity"), GetCompilerMessageSeverityName(Severity));
    DiagnosticObj->SetStringField(TEXT("message"), Message->ToText().ToString());

    const FName MessageId = Message->GetIdentifier();
    if (!MessageId.IsNone())
    {
        DiagnosticObj->SetStringField(TEXT("message_id"), MessageId.ToString());
    }

    for (const TSharedRef<IMessageToken>& Token : Message->GetMessageTokens())
    {
        if (Token->GetType() == EMessageToken::EdGraph)
        {
            const FEdGraphToken& EdGraphToken = static_cast<const FEdGraphToken&>(Token.Get());
            if (const UEdGraphPin* Pin = ResultsLog.FindSourcePin(EdGraphToken.GetPin()))
            {
                AddPinContextToDiagnostic(DiagnosticObj, Pin);
            }
            if (const UObject* ReferencedObject = EdGraphToken.GetGraphObject())
            {
                if (const UObject* GraphObject = ResultsLog.FindSourceObject(ReferencedObject))
                {
                    AddObjectContextToDiagnostic(DiagnosticObj, GraphObject);
                }
            }
        }
        else if (Token->GetType() == EMessageToken::Object)
        {
            const FUObjectToken& ObjectToken = static_cast<const FUObjectToken&>(Token.Get());
            if (const UObject* ReferencedObject = ObjectToken.GetObject().Get())
            {
                if (const UObject* Object = ResultsLog.FindSourceObject(ReferencedObject))
                {
                    AddObjectContextToDiagnostic(DiagnosticObj, Object);
                }
            }
        }
    }

    if (const TSharedPtr<IMessageToken> MessageLink = Message->GetMessageLink())
    {
        const IMessageToken& LinkToken = *MessageLink.Get();
        if (LinkToken.GetType() == EMessageToken::EdGraph)
        {
            const FEdGraphToken& EdGraphToken = static_cast<const FEdGraphToken&>(LinkToken);
            if (const UEdGraphPin* Pin = ResultsLog.FindSourcePin(EdGraphToken.GetPin()))
            {
                AddPinContextToDiagnostic(DiagnosticObj, Pin);
            }
            if (const UObject* ReferencedObject = EdGraphToken.GetGraphObject())
            {
                if (const UObject* GraphObject = ResultsLog.FindSourceObject(ReferencedObject))
                {
                    AddObjectContextToDiagnostic(DiagnosticObj, GraphObject);
                }
            }
        }
        else if (LinkToken.GetType() == EMessageToken::Object)
        {
            const FUObjectToken& ObjectToken = static_cast<const FUObjectToken&>(LinkToken);
            if (const UObject* ReferencedObject = ObjectToken.GetObject().Get())
            {
                if (const UObject* Object = ResultsLog.FindSourceObject(ReferencedObject))
                {
                    AddObjectContextToDiagnostic(DiagnosticObj, Object);
                }
            }
        }
    }

    return DiagnosticObj;
}

void AddCompilerFallbackDiagnostic(TArray<TSharedPtr<FJsonValue>>& SummaryMessages, TArray<TSharedPtr<FJsonValue>>& Diagnostics, const FString& Severity, const FString& Message)
{
    SummaryMessages.Add(MakeShared<FJsonValueString>(Message));

    TSharedPtr<FJsonObject> DiagnosticObj = MakeShared<FJsonObject>();
    DiagnosticObj->SetStringField(TEXT("severity"), Severity);
    DiagnosticObj->SetStringField(TEXT("message"), Message);
    Diagnostics.Add(MakeShared<FJsonValueObject>(DiagnosticObj));
}

TSharedPtr<FJsonObject> CompileBlueprintAndBuildValidationResult(UBlueprint* Blueprint, bool bSave, bool bRefreshNodes)
{
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    if (!Blueprint)
    {
        ResultObj->SetBoolField(TEXT("compiled"), false);
        ResultObj->SetBoolField(TEXT("validation_pass"), false);
        ResultObj->SetNumberField(TEXT("compile_error_count"), 1);
        ResultObj->SetNumberField(TEXT("compile_warning_count"), 0);
        return ResultObj;
    }

    const bool bWasDirtyBeforeCompile = Blueprint->GetOutermost() ? Blueprint->GetOutermost()->IsDirty() : false;

    if (bRefreshNodes)
    {
        FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
    }

    FCompilerResultsLog ResultsLog;
    ResultsLog.SetSourcePath(Blueprint->GetPathName());
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &ResultsLog);

    const EBlueprintStatus CompileStatus = Blueprint->Status;
    const bool bCompiledByStatus = CompileStatus == BS_UpToDate || CompileStatus == BS_UpToDateWithWarnings;

    TArray<TSharedPtr<FJsonValue>> CompileErrors;
    TArray<TSharedPtr<FJsonValue>> CompileWarnings;
    TArray<TSharedPtr<FJsonValue>> Diagnostics;
    int32 CompileErrorCount = ResultsLog.NumErrors;
    int32 CompileWarningCount = ResultsLog.NumWarnings;

    for (const TSharedRef<FTokenizedMessage>& Message : ResultsLog.Messages)
    {
        const EMessageSeverity::Type Severity = Message->GetSeverity();
        if (!IsCompilerErrorSeverity(Severity) && !IsCompilerWarningSeverity(Severity))
        {
            continue;
        }

        const FString MessageText = Message->ToText().ToString();
        TSharedPtr<FJsonObject> DiagnosticObj = CompilerMessageToDiagnosticJson(Message, ResultsLog);
        Diagnostics.Add(MakeShared<FJsonValueObject>(DiagnosticObj));

        if (IsCompilerErrorSeverity(Severity))
        {
            CompileErrors.Add(MakeShared<FJsonValueString>(MessageText));
        }
        else if (IsCompilerWarningSeverity(Severity))
        {
            CompileWarnings.Add(MakeShared<FJsonValueString>(MessageText));
        }
    }

    CompileErrorCount = FMath::Max(CompileErrorCount, CompileErrors.Num());
    CompileWarningCount = FMath::Max(CompileWarningCount, CompileWarnings.Num());

    if (CompileErrorCount > 0 && CompileErrors.Num() == 0)
    {
        const FString FallbackError = FString::Printf(
            TEXT("Blueprint compiler reported %d error(s), but no tokenized error messages were available."),
            CompileErrorCount);
        AddCompilerFallbackDiagnostic(CompileErrors, Diagnostics, TEXT("error"), FallbackError);
    }

    if (!bCompiledByStatus && CompileErrors.Num() == 0)
    {
        const FString FallbackError = FString::Printf(
            TEXT("Blueprint compile status is '%s'. Check the editor Message Log for node-level compiler diagnostics."),
            *GetBlueprintStatusName(CompileStatus));
        AddCompilerFallbackDiagnostic(CompileErrors, Diagnostics, TEXT("error"), FallbackError);
        CompileErrorCount = 1;
    }

    if (CompileWarningCount > 0 && CompileWarnings.Num() == 0)
    {
        const FString FallbackWarning = FString::Printf(
            TEXT("Blueprint compiler reported %d warning(s), but no tokenized warning messages were available."),
            CompileWarningCount);
        AddCompilerFallbackDiagnostic(CompileWarnings, Diagnostics, TEXT("warning"), FallbackWarning);
    }

    CompileErrorCount = FMath::Max(CompileErrorCount, CompileErrors.Num());
    CompileWarningCount = FMath::Max(CompileWarningCount, CompileWarnings.Num());

    const bool bCompiled = bCompiledByStatus && CompileErrorCount == 0;
    const bool bHasWarnings = CompileStatus == BS_UpToDateWithWarnings || CompileWarningCount > 0;

    bool bSaved = false;
    if (bSave && bCompiled)
    {
        bSaved = UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false);
    }

    ResultObj->SetStringField(TEXT("name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    ResultObj->SetBoolField(TEXT("compiled"), bCompiled);
    ResultObj->SetBoolField(TEXT("validation_pass"), bCompiled);
    ResultObj->SetStringField(TEXT("status"), GetBlueprintStatusName(CompileStatus));
    ResultObj->SetBoolField(TEXT("has_warnings"), bHasWarnings);
    ResultObj->SetNumberField(TEXT("compile_error_count"), CompileErrorCount);
    ResultObj->SetNumberField(TEXT("compile_warning_count"), CompileWarningCount);
    ResultObj->SetArrayField(TEXT("compile_errors"), CompileErrors);
    ResultObj->SetArrayField(TEXT("compile_warnings"), CompileWarnings);
    ResultObj->SetArrayField(TEXT("diagnostics"), Diagnostics);
    ResultObj->SetNumberField(TEXT("diagnostic_count"), Diagnostics.Num());
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    ResultObj->SetBoolField(TEXT("requested_save"), bSave);
    ResultObj->SetBoolField(TEXT("refreshed_nodes"), bRefreshNodes);
    ResultObj->SetBoolField(TEXT("was_dirty_before_compile"), bWasDirtyBeforeCompile);
    ResultObj->SetBoolField(TEXT("dirty_after_compile"), Blueprint->GetOutermost() ? Blueprint->GetOutermost()->IsDirty() : false);
    return ResultObj;
}
}

FUnrealMCPBlueprintCommands::FUnrealMCPBlueprintCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("create_blueprint"))
    {
        return HandleCreateBlueprint(Params);
    }
    else if (CommandType == TEXT("add_component_to_blueprint"))
    {
        return HandleAddComponentToBlueprint(Params);
    }
    else if (CommandType == TEXT("list_blueprint_components"))
    {
        return HandleListBlueprintComponents(Params);
    }
    else if (CommandType == TEXT("set_component_property"))
    {
        return HandleSetComponentProperty(Params);
    }
    else if (CommandType == TEXT("get_component_property"))
    {
        return HandleGetComponentProperty(Params);
    }
    else if (CommandType == TEXT("set_physics_properties"))
    {
        return HandleSetPhysicsProperties(Params);
    }
    else if (CommandType == TEXT("compile_blueprint"))
    {
        return HandleCompileBlueprint(Params);
    }
    else if (CommandType == TEXT("compile_and_save_blueprint"))
    {
        return HandleCompileAndSaveBlueprint(Params);
    }
    else if (CommandType == TEXT("compile_and_validate_blueprint"))
    {
        return HandleCompileAndValidateBlueprint(Params);
    }
    else if (CommandType == TEXT("spawn_blueprint_actor"))
    {
        return HandleSpawnBlueprintActor(Params);
    }
    else if (CommandType == TEXT("set_blueprint_property"))
    {
        return HandleSetBlueprintProperty(Params);
    }
    else if (CommandType == TEXT("set_static_mesh_properties"))
    {
        return HandleSetStaticMeshProperties(Params);
    }
    else if (CommandType == TEXT("set_pawn_properties"))
    {
        return HandleSetPawnProperties(Params);
    }
    
    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown blueprint command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleCreateBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    FString PackagePath = TEXT("/Game/Blueprints/");
    if (Params->HasField(TEXT("package_path")))
    {
        if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'package_path' must be a string"));
        }

        PackagePath.TrimStartAndEndInline();
        if (PackagePath.IsEmpty())
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'package_path' cannot be empty"));
        }

        while (PackagePath.Len() > 1 && PackagePath.EndsWith(TEXT("/")))
        {
            PackagePath.LeftChopInline(1);
        }

        if (PackagePath != TEXT("/Game") && !PackagePath.StartsWith(TEXT("/Game/")))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'package_path' must be under /Game"));
        }
        if (PackagePath.Contains(TEXT(".")))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'package_path' must be a package directory, not an object path"));
        }

        if (PackagePath != TEXT("/Game"))
        {
            FText PackagePathFailureReason;
            if (!FPackageName::IsValidLongPackageName(PackagePath, false, &PackagePathFailureReason))
            {
                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
                    TEXT("'package_path' is not a valid long package path: %s"),
                    *PackagePathFailureReason.ToString()));
            }
        }

        PackagePath += TEXT("/");
    }

    // Check if blueprint already exists
    FString AssetName = BlueprintName;
    const FString NewPackageName = PackagePath + AssetName;
    FText PackageNameFailureReason;
    if (!FPackageName::IsValidLongPackageName(NewPackageName, false, &PackageNameFailureReason))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Invalid Blueprint package name '%s': %s"),
            *NewPackageName,
            *PackageNameFailureReason.ToString()));
    }

    if (UEditorAssetLibrary::DoesAssetExist(PackagePath + AssetName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint already exists: %s"), *BlueprintName));
    }

    // Create the blueprint factory
    UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
    
    // Handle parent class
    FString ParentClass;
    Params->TryGetStringField(TEXT("parent_class"), ParentClass);
    
    // Default to Actor if no parent class specified
    UClass* SelectedParentClass = AActor::StaticClass();
    
    // Try to find the specified parent class
    if (!ParentClass.IsEmpty())
    {
        FString ClassName = ParentClass;
        if (!ClassName.StartsWith(TEXT("A")))
        {
            ClassName = TEXT("A") + ClassName;
        }
        
        // First try direct StaticClass lookup for common classes
        UClass* FoundClass = nullptr;
        if (ClassName == TEXT("APawn"))
        {
            FoundClass = APawn::StaticClass();
        }
        else if (ClassName == TEXT("AActor"))
        {
            FoundClass = AActor::StaticClass();
        }
        else
        {
            // Try loading the class using LoadClass which is more reliable than FindObject
            const FString ClassPath = FString::Printf(TEXT("/Script/Engine.%s"), *ClassName);
            FoundClass = LoadClass<AActor>(nullptr, *ClassPath);
            
            if (!FoundClass)
            {
                // Try alternate paths if not found
                const FString GameClassPath = FString::Printf(TEXT("/Script/Game.%s"), *ClassName);
                FoundClass = LoadClass<AActor>(nullptr, *GameClassPath);
            }
        }

        if (FoundClass)
        {
            SelectedParentClass = FoundClass;
            UE_LOG(LogTemp, Log, TEXT("Successfully set parent class to '%s'"), *ClassName);
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("Could not find specified parent class '%s' at paths: /Script/Engine.%s or /Script/Game.%s, defaulting to AActor"), 
                *ClassName, *ClassName, *ClassName);
        }
    }
    
    Factory->ParentClass = SelectedParentClass;

    // Create the blueprint
    UPackage* Package = CreatePackage(*NewPackageName);
    UBlueprint* NewBlueprint = Cast<UBlueprint>(Factory->FactoryCreateNew(UBlueprint::StaticClass(), Package, *AssetName, RF_Standalone | RF_Public, nullptr, GWarn));

    if (NewBlueprint)
    {
        // Notify the asset registry
        FAssetRegistryModule::AssetCreated(NewBlueprint);

        // Mark the package dirty
        Package->MarkPackageDirty();

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("name"), AssetName);
        ResultObj->SetStringField(TEXT("path"), PackagePath + AssetName);
        return ResultObj;
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create blueprint"));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleAddComponentToBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ComponentType;
    if (!Params->TryGetStringField(TEXT("component_type"), ComponentType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'type' parameter"));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    FString ParentComponentName;
    Params->TryGetStringField(TEXT("parent_component_name"), ParentComponentName);

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }
    if (!Blueprint->SimpleConstructionScript)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("SimpleConstructionScript not found for blueprint: %s"), *BlueprintName));
    }

    // Create the component - dynamically find the component class by name
    UClass* ComponentClass = nullptr;

    // Try to find the class with exact name first
    ComponentClass = FindFirstObject<UClass>(*ComponentType);
    
    // If not found, try with "Component" suffix
    if (!ComponentClass && !ComponentType.EndsWith(TEXT("Component")))
    {
        FString ComponentTypeWithSuffix = ComponentType + TEXT("Component");
        ComponentClass = FindFirstObject<UClass>(*ComponentTypeWithSuffix);
    }
    
    // If still not found, try with "U" prefix
    if (!ComponentClass && !ComponentType.StartsWith(TEXT("U")))
    {
        FString ComponentTypeWithPrefix = TEXT("U") + ComponentType;
        ComponentClass = FindFirstObject<UClass>(*ComponentTypeWithPrefix);
        
        // Try with both prefix and suffix
        if (!ComponentClass && !ComponentType.EndsWith(TEXT("Component")))
        {
            FString ComponentTypeWithBoth = TEXT("U") + ComponentType + TEXT("Component");
            ComponentClass = FindFirstObject<UClass>(*ComponentTypeWithBoth);
        }
    }
    
    // Verify that the class is a valid component type
    if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown component type: %s"), *ComponentType));
    }

    USCS_Node* ParentNodeForAttachment = nullptr;
    if (!ParentComponentName.IsEmpty())
    {
        ParentNodeForAttachment = Blueprint->SimpleConstructionScript->FindSCSNode(FName(*ParentComponentName));
        if (!ParentNodeForAttachment)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Parent component not found: %s"), *ParentComponentName)
            );
        }
        if (!Cast<USceneComponent>(ParentNodeForAttachment->ComponentTemplate))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Parent attachment requires a scene component parent: %s"), *ParentComponentName)
            );
        }
        if (!ComponentClass->IsChildOf(USceneComponent::StaticClass()))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Parent attachment requires a scene component child class: %s"), *ComponentType)
            );
        }
    }

    // Add the component to the blueprint
    USCS_Node* NewNode = Blueprint->SimpleConstructionScript->CreateNode(ComponentClass, *ComponentName);
    if (NewNode)
    {
        // Set transform if provided
        USceneComponent* SceneComponent = Cast<USceneComponent>(NewNode->ComponentTemplate);
        if (SceneComponent)
        {
            if (Params->HasField(TEXT("location")))
            {
                SceneComponent->SetRelativeLocation(FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location")));
            }
            if (Params->HasField(TEXT("rotation")))
            {
                SceneComponent->SetRelativeRotation(FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation")));
            }
            if (Params->HasField(TEXT("scale")))
            {
                SceneComponent->SetRelativeScale3D(FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale")));
            }
        }

        if (!ParentComponentName.IsEmpty())
        {
            if (!Cast<USceneComponent>(NewNode->ComponentTemplate))
            {
                return FUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Parent attachment requires a scene component child: %s"), *ComponentName)
                );
            }
            ParentNodeForAttachment->AddChildNode(NewNode);
        }
        else
        {
            Blueprint->SimpleConstructionScript->AddNode(NewNode);
        }

        // Compile the blueprint
        FKismetEditorUtilities::CompileBlueprint(Blueprint);

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("component_name"), ComponentName);
        ResultObj->SetStringField(TEXT("component_type"), ComponentType);
        ResultObj->SetStringField(TEXT("parent_component_name"), ParentComponentName);
        return ResultObj;
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to add component to blueprint"));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleListBlueprintComponents(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ComponentNameFilter;
    Params->TryGetStringField(TEXT("component_name"), ComponentNameFilter);

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    TArray<TSharedPtr<FJsonValue>> Components;
    const bool bHasSCS = Blueprint->SimpleConstructionScript != nullptr;

    if (bHasSCS)
    {
        const TArray<USCS_Node*>& Nodes = Blueprint->SimpleConstructionScript->GetAllNodes();
        for (USCS_Node* Node : Nodes)
        {
            if (!Node)
            {
                continue;
            }

            const FString VariableName = Node->GetVariableName().ToString();
            if (!ComponentNameFilter.IsEmpty() && VariableName != ComponentNameFilter)
            {
                continue;
            }

            UActorComponent* ComponentTemplate = Node->ComponentTemplate;
            TSharedPtr<FJsonObject> ComponentObject = MakeShared<FJsonObject>();
            ComponentObject->SetStringField(TEXT("component_name"), VariableName);
            ComponentObject->SetStringField(TEXT("variable_name"), VariableName);
            ComponentObject->SetStringField(TEXT("node_guid"), Node->VariableGuid.ToString());
            ComponentObject->SetStringField(TEXT("component_class"), Node->ComponentClass ? Node->ComponentClass->GetName() : FString());
            ComponentObject->SetStringField(TEXT("component_class_path"), Node->ComponentClass ? Node->ComponentClass->GetPathName() : FString());
            ComponentObject->SetStringField(TEXT("template_name"), ComponentTemplate ? ComponentTemplate->GetName() : FString());
            ComponentObject->SetStringField(TEXT("template_path"), BlueprintObjectPathOrEmpty(ComponentTemplate));

            USCS_Node* ParentNode = Blueprint->SimpleConstructionScript->FindParentNode(Node);
            ComponentObject->SetStringField(TEXT("parent_component_name"), ParentNode ? ParentNode->GetVariableName().ToString() : FString());

            if (USceneComponent* SceneComponent = Cast<USceneComponent>(ComponentTemplate))
            {
                TSharedPtr<FJsonObject> TransformObject = MakeShared<FJsonObject>();
                TransformObject->SetArrayField(TEXT("location"), VectorToJsonArray(SceneComponent->GetRelativeLocation()));
                TransformObject->SetArrayField(TEXT("rotation"), RotatorToJsonArray(SceneComponent->GetRelativeRotation()));
                TransformObject->SetArrayField(TEXT("scale"), VectorToJsonArray(SceneComponent->GetRelativeScale3D()));
                ComponentObject->SetObjectField(TEXT("relative_transform"), TransformObject);
            }

            if (UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(ComponentTemplate))
            {
                UStaticMesh* StaticMesh = StaticMeshComponent->GetStaticMesh();
                ComponentObject->SetStringField(TEXT("static_mesh"), BlueprintObjectPathOrEmpty(StaticMesh));
            }

            Components.Add(MakeShared<FJsonValueObject>(ComponentObject));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), BlueprintName);
    ResultObj->SetBoolField(TEXT("has_simple_construction_script"), bHasSCS);
    ResultObj->SetNumberField(TEXT("component_count"), Components.Num());
    ResultObj->SetArrayField(TEXT("components"), Components);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSetComponentProperty(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'component_name' parameter"));
    }

    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_name' parameter"));
    }

    // Log all input parameters for debugging
    UE_LOG(LogTemp, Warning, TEXT("SetComponentProperty - Blueprint: %s, Component: %s, Property: %s"), 
        *BlueprintName, *ComponentName, *PropertyName);
    
    // Log property_value if available
    if (Params->HasField(TEXT("property_value")))
    {
        TSharedPtr<FJsonValue> JsonValue = Params->Values.FindRef(TEXT("property_value"));
        FString ValueType;
        
        switch(JsonValue->Type)
        {
            case EJson::Boolean: ValueType = FString::Printf(TEXT("Boolean: %s"), JsonValue->AsBool() ? TEXT("true") : TEXT("false")); break;
            case EJson::Number: ValueType = FString::Printf(TEXT("Number: %f"), JsonValue->AsNumber()); break;
            case EJson::String: ValueType = FString::Printf(TEXT("String: %s"), *JsonValue->AsString()); break;
            case EJson::Array: ValueType = TEXT("Array"); break;
            case EJson::Object: ValueType = TEXT("Object"); break;
            default: ValueType = TEXT("Unknown"); break;
        }
        
        UE_LOG(LogTemp, Warning, TEXT("SetComponentProperty - Value Type: %s"), *ValueType);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("SetComponentProperty - No property_value provided"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - Blueprint not found: %s"), *BlueprintName);
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }
    else
    {
        UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Blueprint found: %s (Class: %s)"), 
            *BlueprintName, 
            Blueprint->GeneratedClass ? *Blueprint->GeneratedClass->GetName() : TEXT("NULL"));
    }

    // Find the component
    USCS_Node* ComponentNode = nullptr;
    UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Searching for component %s in blueprint nodes"), *ComponentName);
    
    if (!Blueprint->SimpleConstructionScript)
    {
        UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - SimpleConstructionScript is NULL for blueprint %s"), *BlueprintName);
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Invalid blueprint construction script"));
    }
    
    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (Node)
        {
            UE_LOG(LogTemp, Verbose, TEXT("SetComponentProperty - Found node: %s"), *Node->GetVariableName().ToString());
            if (Node->GetVariableName().ToString() == ComponentName)
            {
                ComponentNode = Node;
                break;
            }
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("SetComponentProperty - Found NULL node in blueprint"));
        }
    }

    if (!ComponentNode)
    {
        UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - Component not found: %s"), *ComponentName);
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
    }
    else
    {
        UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Component found: %s (Class: %s)"), 
            *ComponentName, 
            ComponentNode->ComponentTemplate ? *ComponentNode->ComponentTemplate->GetClass()->GetName() : TEXT("NULL"));
    }

    // Get the component template
    UObject* ComponentTemplate = ComponentNode->ComponentTemplate;
    if (!ComponentTemplate)
    {
        UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - Component template is NULL for %s"), *ComponentName);
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Invalid component template"));
    }

    // Check if this is a Spring Arm component and log special debug info
    if (ComponentTemplate->GetClass()->GetName().Contains(TEXT("SpringArm")))
    {
        UE_LOG(LogTemp, Warning, TEXT("SetComponentProperty - SpringArm component detected! Class: %s"), 
            *ComponentTemplate->GetClass()->GetPathName());
            
        // Log all properties of the SpringArm component class
        UE_LOG(LogTemp, Warning, TEXT("SetComponentProperty - SpringArm properties:"));
        for (TFieldIterator<FProperty> PropIt(ComponentTemplate->GetClass()); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            UE_LOG(LogTemp, Warning, TEXT("  - %s (%s)"), *Prop->GetName(), *Prop->GetCPPType());
        }

        // Special handling for Spring Arm properties
        if (Params->HasField(TEXT("property_value")))
        {
            TSharedPtr<FJsonValue> JsonValue = Params->Values.FindRef(TEXT("property_value"));
            
            // Get the property using the new FField system
            FProperty* Property = FindFProperty<FProperty>(ComponentTemplate->GetClass(), *PropertyName);
            if (!Property)
            {
                UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - Property %s not found on SpringArm component"), *PropertyName);
                return FUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Property %s not found on SpringArm component"), *PropertyName));
            }

            // Create a scope guard to ensure property cleanup
            struct FScopeGuard
            {
                UObject* Object;
                FScopeGuard(UObject* InObject) : Object(InObject) 
                {
                    if (Object)
                    {
                        Object->Modify();
                    }
                }
                ~FScopeGuard()
                {
                    if (Object)
                    {
                        Object->PostEditChange();
                    }
                }
            } ScopeGuard(ComponentTemplate);

            bool bSuccess = false;
            FString ErrorMessage;

            // Handle specific Spring Arm property types
            if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Property))
            {
                if (JsonValue->Type == EJson::Number)
                {
                    const float Value = JsonValue->AsNumber();
                    UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Setting float property %s to %f"), *PropertyName, Value);
                    FloatProp->SetPropertyValue_InContainer(ComponentTemplate, Value);
                    bSuccess = true;
                }
            }
            else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
            {
                if (JsonValue->Type == EJson::Boolean)
                {
                    const bool Value = JsonValue->AsBool();
                    UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Setting bool property %s to %d"), *PropertyName, Value);
                    BoolProp->SetPropertyValue_InContainer(ComponentTemplate, Value);
                    bSuccess = true;
                }
            }
            else if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
            {
                UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Handling struct property %s of type %s"), 
                    *PropertyName, *StructProp->Struct->GetName());
                
                // Special handling for common Spring Arm struct properties
                if (StructProp->Struct == TBaseStructure<FVector>::Get())
                {
                    if (JsonValue->Type == EJson::Array)
                    {
                        const TArray<TSharedPtr<FJsonValue>>& Arr = JsonValue->AsArray();
                        if (Arr.Num() == 3)
                        {
                            FVector Vec(
                                Arr[0]->AsNumber(),
                                Arr[1]->AsNumber(),
                                Arr[2]->AsNumber()
                            );
                            void* PropertyAddr = StructProp->ContainerPtrToValuePtr<void>(ComponentTemplate);
                            StructProp->CopySingleValue(PropertyAddr, &Vec);
                            bSuccess = true;
                        }
                    }
                }
                else if (StructProp->Struct == TBaseStructure<FRotator>::Get())
                {
                    if (JsonValue->Type == EJson::Array)
                    {
                        const TArray<TSharedPtr<FJsonValue>>& Arr = JsonValue->AsArray();
                        if (Arr.Num() == 3)
                        {
                            FRotator Rot(
                                Arr[0]->AsNumber(),
                                Arr[1]->AsNumber(),
                                Arr[2]->AsNumber()
                            );
                            void* PropertyAddr = StructProp->ContainerPtrToValuePtr<void>(ComponentTemplate);
                            StructProp->CopySingleValue(PropertyAddr, &Rot);
                            bSuccess = true;
                        }
                    }
                }
            }

            if (bSuccess)
            {
                // Mark the blueprint as modified
                UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Successfully set SpringArm property %s"), *PropertyName);
                FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

                TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
                ResultObj->SetStringField(TEXT("component"), ComponentName);
                ResultObj->SetStringField(TEXT("property"), PropertyName);
                ResultObj->SetBoolField(TEXT("success"), true);
                return ResultObj;
            }
            else
            {
                UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - Failed to set SpringArm property %s"), *PropertyName);
                return FUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Failed to set SpringArm property %s"), *PropertyName));
            }
        }
    }

    // Regular property handling for non-Spring Arm components continues...

    // Set the property value
    if (Params->HasField(TEXT("property_value")))
    {
        TSharedPtr<FJsonValue> JsonValue = Params->Values.FindRef(TEXT("property_value"));
        
        // Get the property
        FProperty* Property = FindFProperty<FProperty>(ComponentTemplate->GetClass(), *PropertyName);
        if (!Property)
        {
            UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - Property %s not found on component %s"), 
                *PropertyName, *ComponentName);
            
            // List all available properties for this component
            UE_LOG(LogTemp, Warning, TEXT("SetComponentProperty - Available properties for %s:"), *ComponentName);
            for (TFieldIterator<FProperty> PropIt(ComponentTemplate->GetClass()); PropIt; ++PropIt)
            {
                FProperty* Prop = *PropIt;
                UE_LOG(LogTemp, Warning, TEXT("  - %s (%s)"), *Prop->GetName(), *Prop->GetCPPType());
            }
            
            return FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Property %s not found on component %s"), *PropertyName, *ComponentName));
        }
        else
        {
            UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Property found: %s (Type: %s)"), 
                *PropertyName, *Property->GetCPPType());
        }

        bool bSuccess = false;
        FString ErrorMessage;

        // Handle different property types
        UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Attempting to set property %s"), *PropertyName);
        
        // Add try-catch block to catch and log any crashes
        try
        {
            if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
            {
                // Handle vector properties
                UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Property is a struct: %s"), 
                    StructProp->Struct ? *StructProp->Struct->GetName() : TEXT("NULL"));
                    
                if (StructProp->Struct == TBaseStructure<FVector>::Get())
                {
                    if (JsonValue->Type == EJson::Array)
                    {
                        // Handle array input [x, y, z]
                        const TArray<TSharedPtr<FJsonValue>>& Arr = JsonValue->AsArray();
                        if (Arr.Num() == 3)
                        {
                            FVector Vec(
                                Arr[0]->AsNumber(),
                                Arr[1]->AsNumber(),
                                Arr[2]->AsNumber()
                            );
                            void* PropertyAddr = StructProp->ContainerPtrToValuePtr<void>(ComponentTemplate);
                            UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Setting Vector(%f, %f, %f)"), 
                                Vec.X, Vec.Y, Vec.Z);
                            StructProp->CopySingleValue(PropertyAddr, &Vec);
                            bSuccess = true;
                        }
                        else
                        {
                            ErrorMessage = FString::Printf(TEXT("Vector property requires 3 values, got %d"), Arr.Num());
                            UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - %s"), *ErrorMessage);
                        }
                    }
                    else if (JsonValue->Type == EJson::Number)
                    {
                        // Handle scalar input (sets all components to same value)
                        float Value = JsonValue->AsNumber();
                        FVector Vec(Value, Value, Value);
                        void* PropertyAddr = StructProp->ContainerPtrToValuePtr<void>(ComponentTemplate);
                        UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Setting Vector(%f, %f, %f) from scalar"), 
                            Vec.X, Vec.Y, Vec.Z);
                        StructProp->CopySingleValue(PropertyAddr, &Vec);
                        bSuccess = true;
                    }
                    else
                    {
                        ErrorMessage = TEXT("Vector property requires either a single number or array of 3 numbers");
                        UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - %s"), *ErrorMessage);
                    }
                }
                else
                {
                    // Handle other struct properties using default handler
                    UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Using generic struct handler for %s"), 
                        *PropertyName);
                    bSuccess = FUnrealMCPCommonUtils::SetObjectProperty(ComponentTemplate, PropertyName, JsonValue, ErrorMessage);
                    if (!bSuccess)
                    {
                        UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - Failed to set struct property: %s"), *ErrorMessage);
                    }
                }
            }
            else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
            {
                // Handle enum properties
                UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Property is an enum"));
                if (JsonValue->Type == EJson::String)
                {
                    FString EnumValueName = JsonValue->AsString();
                    UEnum* Enum = EnumProp->GetEnum();
                    UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Setting enum from string: %s"), *EnumValueName);
                    
                    if (Enum)
                    {
                        int64 EnumValue = Enum->GetValueByNameString(EnumValueName);
                        
                        if (EnumValue != INDEX_NONE)
                        {
                            UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Found enum value: %lld"), EnumValue);
                            EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(
                                ComponentTemplate, 
                                EnumValue
                            );
                            bSuccess = true;
                        }
                        else
                        {
                            // List all possible enum values
                            UE_LOG(LogTemp, Warning, TEXT("SetComponentProperty - Available enum values for %s:"), 
                                *Enum->GetName());
                            for (int32 i = 0; i < Enum->NumEnums(); i++)
                            {
                                UE_LOG(LogTemp, Warning, TEXT("  - %s (%lld)"), 
                                    *Enum->GetNameStringByIndex(i),
                                    Enum->GetValueByIndex(i));
                            }
                            
                            ErrorMessage = FString::Printf(TEXT("Invalid enum value '%s' for property %s"), 
                                *EnumValueName, *PropertyName);
                            UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - %s"), *ErrorMessage);
                        }
                    }
                    else
                    {
                        ErrorMessage = TEXT("Enum object is NULL");
                        UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - %s"), *ErrorMessage);
                    }
                }
                else if (JsonValue->Type == EJson::Number)
                {
                    // Allow setting enum by integer value
                    int64 EnumValue = JsonValue->AsNumber();
                    UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Setting enum from number: %lld"), EnumValue);
                    EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(
                        ComponentTemplate, 
                        EnumValue
                    );
                    bSuccess = true;
                }
                else
                {
                    ErrorMessage = TEXT("Enum property requires either a string name or integer value");
                    UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - %s"), *ErrorMessage);
                }
            }
            else if (FNumericProperty* NumericProp = CastField<FNumericProperty>(Property))
            {
                // Handle numeric properties
                UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Property is numeric: IsInteger=%d, IsFloat=%d"), 
                    NumericProp->IsInteger(), NumericProp->IsFloatingPoint());
                    
                if (JsonValue->Type == EJson::Number)
                {
                    double Value = JsonValue->AsNumber();
                    UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Setting numeric value: %f"), Value);
                    
                    if (NumericProp->IsInteger())
                    {
                        NumericProp->SetIntPropertyValue(ComponentTemplate, (int64)Value);
                        UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Set integer value: %lld"), (int64)Value);
                        bSuccess = true;
                    }
                    else if (NumericProp->IsFloatingPoint())
                    {
                        NumericProp->SetFloatingPointPropertyValue(ComponentTemplate, Value);
                        UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Set float value: %f"), Value);
                        bSuccess = true;
                    }
                }
                else
                {
                    ErrorMessage = TEXT("Numeric property requires a number value");
                    UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - %s"), *ErrorMessage);
                }
            }
            else
            {
                // Handle all other property types using default handler
                UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Using generic property handler for %s (Type: %s)"), 
                    *PropertyName, *Property->GetCPPType());
                bSuccess = FUnrealMCPCommonUtils::SetObjectProperty(ComponentTemplate, PropertyName, JsonValue, ErrorMessage);
                if (!bSuccess)
                {
                    UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - Failed to set property: %s"), *ErrorMessage);
                }
            }
        }
        catch (const std::exception& Ex)
        {
            UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - EXCEPTION: %s"), ANSI_TO_TCHAR(Ex.what()));
            return FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Exception while setting property %s: %s"), *PropertyName, ANSI_TO_TCHAR(Ex.what())));
        }
        catch (...)
        {
            UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - UNKNOWN EXCEPTION occurred while setting property %s"), *PropertyName);
            return FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unknown exception while setting property %s"), *PropertyName));
        }

        if (bSuccess)
        {
            // Mark the blueprint as modified
            UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Successfully set property %s on component %s"), 
                *PropertyName, *ComponentName);
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetStringField(TEXT("component"), ComponentName);
            ResultObj->SetStringField(TEXT("property"), PropertyName);
            ResultObj->SetBoolField(TEXT("success"), true);
            return ResultObj;
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - Failed to set property %s: %s"), 
                *PropertyName, *ErrorMessage);
            return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
        }
    }

    UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - Missing 'property_value' parameter"));
    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_value' parameter"));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleGetComponentProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'component_name' parameter"));
    }

    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_name' parameter"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }
    if (!Blueprint->SimpleConstructionScript)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("SimpleConstructionScript not found for blueprint: %s"), *BlueprintName));
    }

    USCS_Node* ComponentNode = nullptr;
    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (Node && Node->GetVariableName().ToString() == ComponentName)
        {
            ComponentNode = Node;
            break;
        }
    }
    if (!ComponentNode || !ComponentNode->ComponentTemplate)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
    }

    UActorComponent* ComponentTemplate = ComponentNode->ComponentTemplate;
    FProperty* Property = FindFProperty<FProperty>(ComponentTemplate->GetClass(), *PropertyName);
    if (!Property)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Property %s not found on component %s"), *PropertyName, *ComponentName)
        );
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("component_name"), ComponentName);
    ResultObj->SetStringField(TEXT("component_class"), ComponentTemplate->GetClass()->GetName());
    ResultObj->SetStringField(TEXT("property_name"), PropertyName);
    ResultObj->SetStringField(TEXT("property_type"), Property->GetCPPType());
    ResultObj->SetField(TEXT("property_value"), PropertyValueToJsonValue(Property, ComponentTemplate));
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSetPhysicsProperties(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'component_name' parameter"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Find the component
    USCS_Node* ComponentNode = nullptr;
    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (Node && Node->GetVariableName().ToString() == ComponentName)
        {
            ComponentNode = Node;
            break;
        }
    }

    if (!ComponentNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
    }

    UPrimitiveComponent* PrimComponent = Cast<UPrimitiveComponent>(ComponentNode->ComponentTemplate);
    if (!PrimComponent)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Component is not a primitive component"));
    }

    // Set physics properties
    if (Params->HasField(TEXT("simulate_physics")))
    {
        PrimComponent->SetSimulatePhysics(Params->GetBoolField(TEXT("simulate_physics")));
    }

    if (Params->HasField(TEXT("mass")))
    {
        float Mass = Params->GetNumberField(TEXT("mass"));
        // In UE5.5, use proper overrideMass instead of just scaling
        PrimComponent->SetMassOverrideInKg(NAME_None, Mass);
        UE_LOG(LogTemp, Display, TEXT("Set mass for component %s to %f kg"), *ComponentName, Mass);
    }

    if (Params->HasField(TEXT("linear_damping")))
    {
        PrimComponent->SetLinearDamping(Params->GetNumberField(TEXT("linear_damping")));
    }

    if (Params->HasField(TEXT("angular_damping")))
    {
        PrimComponent->SetAngularDamping(Params->GetNumberField(TEXT("angular_damping")));
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("component"), ComponentName);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleCompileBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    return CompileBlueprintAndBuildValidationResult(Blueprint, false, false);
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleCompileAndSaveBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    bool bSave = true;
    if (Params->HasField(TEXT("save")))
    {
        bSave = Params->GetBoolField(TEXT("save"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    return CompileBlueprintAndBuildValidationResult(Blueprint, bSave, false);
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleCompileAndValidateBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    bool bSave = false;
    if (Params->HasField(TEXT("save")))
    {
        bSave = Params->GetBoolField(TEXT("save"));
    }

    bool bRefreshNodes = true;
    if (Params->HasField(TEXT("refresh_nodes")))
    {
        bRefreshNodes = Params->GetBoolField(TEXT("refresh_nodes"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    return CompileBlueprintAndBuildValidationResult(Blueprint, bSave, bRefreshNodes);
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSpawnBlueprintActor(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ActorName;
    if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'actor_name' parameter"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get transform parameters
    FVector Location(0.0f, 0.0f, 0.0f);
    FRotator Rotation(0.0f, 0.0f, 0.0f);

    if (Params->HasField(TEXT("location")))
    {
        Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        Rotation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"));
    }

    // Spawn the actor
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    FTransform SpawnTransform;
    SpawnTransform.SetLocation(Location);
    SpawnTransform.SetRotation(FQuat(Rotation));

    AActor* NewActor = World->SpawnActor<AActor>(Blueprint->GeneratedClass, SpawnTransform);
    if (NewActor)
    {
        NewActor->SetActorLabel(*ActorName);
        return FUnrealMCPCommonUtils::ActorToJsonObject(NewActor, true);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to spawn blueprint actor"));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSetBlueprintProperty(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_name' parameter"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get the default object
    UObject* DefaultObject = Blueprint->GeneratedClass->GetDefaultObject();
    if (!DefaultObject)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get default object"));
    }

    // Set the property value
    if (Params->HasField(TEXT("property_value")))
    {
        TSharedPtr<FJsonValue> JsonValue = Params->Values.FindRef(TEXT("property_value"));
        
        FString ErrorMessage;
        if (FUnrealMCPCommonUtils::SetObjectProperty(DefaultObject, PropertyName, JsonValue, ErrorMessage))
        {
            // Mark the blueprint as modified
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetStringField(TEXT("property"), PropertyName);
            ResultObj->SetBoolField(TEXT("success"), true);
            return ResultObj;
        }
        else
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
        }
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_value' parameter"));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSetStaticMeshProperties(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'component_name' parameter"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Find the component
    USCS_Node* ComponentNode = nullptr;
    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (Node && Node->GetVariableName().ToString() == ComponentName)
        {
            ComponentNode = Node;
            break;
        }
    }

    if (!ComponentNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
    }

    UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(ComponentNode->ComponentTemplate);
    if (!MeshComponent)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Component is not a static mesh component"));
    }

    // Set static mesh properties
    if (Params->HasField(TEXT("static_mesh")))
    {
        FString MeshPath = Params->GetStringField(TEXT("static_mesh"));
        UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(MeshPath));
        if (Mesh)
        {
            MeshComponent->SetStaticMesh(Mesh);
        }
    }

    if (Params->HasField(TEXT("material")))
    {
        FString MaterialPath = Params->GetStringField(TEXT("material"));
        UMaterialInterface* Material = Cast<UMaterialInterface>(UEditorAssetLibrary::LoadAsset(MaterialPath));
        if (Material)
        {
            MeshComponent->SetMaterial(0, Material);
        }
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("component"), ComponentName);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSetPawnProperties(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get the default object
    UObject* DefaultObject = Blueprint->GeneratedClass->GetDefaultObject();
    if (!DefaultObject)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get default object"));
    }

    // Track if any properties were set successfully
    bool bAnyPropertiesSet = false;
    TSharedPtr<FJsonObject> ResultsObj = MakeShared<FJsonObject>();
    
    // Set auto possess player if specified
    if (Params->HasField(TEXT("auto_possess_player")))
    {
        TSharedPtr<FJsonValue> AutoPossessValue = Params->Values.FindRef(TEXT("auto_possess_player"));
        
        FString ErrorMessage;
        if (FUnrealMCPCommonUtils::SetObjectProperty(DefaultObject, TEXT("AutoPossessPlayer"), AutoPossessValue, ErrorMessage))
        {
            bAnyPropertiesSet = true;
            TSharedPtr<FJsonObject> PropResultObj = MakeShared<FJsonObject>();
            PropResultObj->SetBoolField(TEXT("success"), true);
            ResultsObj->SetObjectField(TEXT("AutoPossessPlayer"), PropResultObj);
        }
        else
        {
            TSharedPtr<FJsonObject> PropResultObj = MakeShared<FJsonObject>();
            PropResultObj->SetBoolField(TEXT("success"), false);
            PropResultObj->SetStringField(TEXT("error"), ErrorMessage);
            ResultsObj->SetObjectField(TEXT("AutoPossessPlayer"), PropResultObj);
        }
    }
    
    // Set controller rotation properties
    const TCHAR* RotationProps[] = {
        TEXT("bUseControllerRotationYaw"),
        TEXT("bUseControllerRotationPitch"),
        TEXT("bUseControllerRotationRoll")
    };
    
    const TCHAR* ParamNames[] = {
        TEXT("use_controller_rotation_yaw"),
        TEXT("use_controller_rotation_pitch"),
        TEXT("use_controller_rotation_roll")
    };
    
    for (int32 i = 0; i < 3; i++)
    {
        if (Params->HasField(ParamNames[i]))
        {
            TSharedPtr<FJsonValue> Value = Params->Values.FindRef(ParamNames[i]);
            
            FString ErrorMessage;
            if (FUnrealMCPCommonUtils::SetObjectProperty(DefaultObject, RotationProps[i], Value, ErrorMessage))
            {
                bAnyPropertiesSet = true;
                TSharedPtr<FJsonObject> PropResultObj = MakeShared<FJsonObject>();
                PropResultObj->SetBoolField(TEXT("success"), true);
                ResultsObj->SetObjectField(RotationProps[i], PropResultObj);
            }
            else
            {
                TSharedPtr<FJsonObject> PropResultObj = MakeShared<FJsonObject>();
                PropResultObj->SetBoolField(TEXT("success"), false);
                PropResultObj->SetStringField(TEXT("error"), ErrorMessage);
                ResultsObj->SetObjectField(RotationProps[i], PropResultObj);
            }
        }
    }
    
    // Set can be damaged property
    if (Params->HasField(TEXT("can_be_damaged")))
    {
        TSharedPtr<FJsonValue> Value = Params->Values.FindRef(TEXT("can_be_damaged"));
        
        FString ErrorMessage;
        if (FUnrealMCPCommonUtils::SetObjectProperty(DefaultObject, TEXT("bCanBeDamaged"), Value, ErrorMessage))
        {
            bAnyPropertiesSet = true;
            TSharedPtr<FJsonObject> PropResultObj = MakeShared<FJsonObject>();
            PropResultObj->SetBoolField(TEXT("success"), true);
            ResultsObj->SetObjectField(TEXT("bCanBeDamaged"), PropResultObj);
        }
        else
        {
            TSharedPtr<FJsonObject> PropResultObj = MakeShared<FJsonObject>();
            PropResultObj->SetBoolField(TEXT("success"), false);
            PropResultObj->SetStringField(TEXT("error"), ErrorMessage);
            ResultsObj->SetObjectField(TEXT("bCanBeDamaged"), PropResultObj);
        }
    }

    // Mark the blueprint as modified if any properties were set
    if (bAnyPropertiesSet)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    }
    else if (ResultsObj->Values.Num() == 0)
    {
        // No properties were specified
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No properties specified to set"));
    }

    TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
    ResponseObj->SetStringField(TEXT("blueprint"), BlueprintName);
    ResponseObj->SetBoolField(TEXT("success"), bAnyPropertiesSet);
    ResponseObj->SetObjectField(TEXT("results"), ResultsObj);
    return ResponseObj;
} 
