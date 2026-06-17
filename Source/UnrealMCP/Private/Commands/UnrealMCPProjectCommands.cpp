#include "Commands/UnrealMCPProjectCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Algo/Sort.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ActorComponent.h"
#include "Components/Widget.h"
#include "CoreGlobals.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/DataTable.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/VolumeTexture.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/Actor.h"
#include "IPythonScriptPlugin.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallFunctionOnMember.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionTerminator.h"
#include "K2Node_StructOperation.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/KismetReinstanceUtilities.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/StructureEditorUtils.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "PythonScriptTypes.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/SoundWave.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/ScriptInterface.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "WidgetBlueprint.h"

namespace
{
bool IsPlaySessionActive()
{
    return GEditor && GEditor->IsPlaySessionInProgress();
}

bool FindUnsafeEditorScriptingPattern(const FString& Code, FString& MatchedPattern)
{
    static const TCHAR* UnsafePatterns[] =
    {
        TEXT("EditorLevelLibrary.destroy_actor"),
        TEXT("EditorLevelLibrary.destroy_actors"),
        TEXT("EditorActorSubsystem.destroy_actor"),
        TEXT("EditorActorSubsystem.destroy_actors")
    };

    for (const TCHAR* Pattern : UnsafePatterns)
    {
        if (Code.Contains(Pattern, ESearchCase::IgnoreCase))
        {
            MatchedPattern = Pattern;
            return true;
        }
    }

    return false;
}

FString NormalizePythonCodeForGuard(const FString& Code)
{
    FString Normalized;
    Normalized.Reserve(Code.Len());

    for (const TCHAR Character : Code)
    {
        if (!FChar::IsWhitespace(Character))
        {
            Normalized.AppendChar(FChar::ToLower(Character));
        }
    }

    return Normalized;
}

bool FindBlockedPythonMapTransitionPattern(const FString& Code, FString& MatchedPattern)
{
    const FString NormalizedCode = NormalizePythonCodeForGuard(Code);
    static const TCHAR* BlockedPatterns[] =
    {
        TEXT("editorloadingandsavingutils.new_blank_map("),
        TEXT(".new_blank_map("),
        TEXT("new_blank_map("),
        TEXT("editorloadingandsavingutils.new_map_from_template("),
        TEXT(".new_map_from_template("),
        TEXT("new_map_from_template("),
        TEXT("editorloadingandsavingutils.load_map("),
        TEXT(".load_map("),
        TEXT("load_map("),
        TEXT("editorloadingandsavingutils.load_map_with_dialog("),
        TEXT(".load_map_with_dialog("),
        TEXT("load_map_with_dialog(")
    };

    for (const TCHAR* Pattern : BlockedPatterns)
    {
        if (NormalizedCode.Contains(Pattern, ESearchCase::CaseSensitive))
        {
            MatchedPattern = Pattern;
            return true;
        }
    }

    return false;
}

struct FPathStringReplacement
{
    FString Source;
    FString Target;
};

struct FDeferredGeneratedClassAsset
{
    FAssetData SourceAsset;
    FString TargetPath;
    FString Strategy;
    FString Detail;
};

struct FAcceptedFallbackRule
{
    FString SourcePath;
    FString SourcePathPrefix;
    FString ClassName;
    TArray<FString> DetailContainsAny;
    FString Reason;
};

struct FEditorLogMarker
{
    FString LogPath;
    int32 StartLineCount = 0;
    int64 StartFileSize = 0;
    bool bEnabled = false;
};

struct FEditorLogHealthResult
{
    FString LogPath;
    bool bChecked = false;
    bool bPass = true;
    int32 IssueCount = 0;
    int32 HandledEnsureCount = 0;
    int32 IgnoredHandledEnsureCount = 0;
    int32 FatalErrorCount = 0;
    int32 CriticalErrorCount = 0;
    int32 ForceDeleteIssueCount = 0;
    int32 WorldMemoryLeakCount = 0;
    TArray<FString> IssueSamples;
};

FString ApplyPathReplacements(FString Text, const TArray<FPathStringReplacement>& Replacements);
UObject* FindReplacementObject(const TMap<UObject*, UObject*>& ReplacementMap, UObject* Object);
int32 RemapBlueprintGraphReferences(UBlueprint* Blueprint, const TMap<UObject*, UObject*>& ReplacementMap, const TArray<FPathStringReplacement>& PathReplacements);
int32 ReplaceExportedPropertyPaths(UObject* Object, const TArray<FPathStringReplacement>& Replacements, const FString& SourceRoot);
TArray<UBlueprint*> SortBlueprintsByInheritance(const TArray<UBlueprint*>& Blueprints);
int32 CompileBlueprintsForMCP(
    const TArray<UBlueprint*>& Blueprints,
    int32 CompilePasses,
    bool bRefreshBlueprintNodes,
    bool bRefreshFailedBlueprintNodes,
    int32& OutCompileErrorCount,
    TArray<FString>* OutFailedAssets);
void FinishCompilationForObjectForMCP(UObject* Object);

FString NormalizeContentRoot(FString Path)
{
    Path.TrimStartAndEndInline();
    while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
    {
        Path.LeftChopInline(1);
    }
    return Path;
}

FString NormalizePackagePathParam(FString Path)
{
    Path.TrimStartAndEndInline();
    Path.TrimQuotesInline();

    FString LongPackageName;
    if (FPackageName::TryConvertFilenameToLongPackageName(Path, LongPackageName))
    {
        return NormalizeContentRoot(LongPackageName);
    }

    if (Path.Contains(TEXT(".")))
    {
        Path = FPackageName::ObjectPathToPackageName(Path);
    }
    return NormalizeContentRoot(Path);
}

UWorld* LoadWorldPackageForRepair(const FString& WorldPackageName)
{
    const FString WorldObjectPath = FString::Printf(
        TEXT("%s.%s"),
        *WorldPackageName,
        *FPackageName::GetShortName(WorldPackageName));

    if (UWorld* LoadedWorld = LoadObject<UWorld>(nullptr, *WorldObjectPath, nullptr, LOAD_None, nullptr))
    {
        return LoadedWorld;
    }

    UPackage* WorldPackage = LoadPackage(nullptr, *WorldPackageName, LOAD_None);
    if (!WorldPackage)
    {
        return nullptr;
    }

    WorldPackage->FullyLoad();
    return UWorld::FindWorldInPackage(WorldPackage);
}

bool SaveWorldPackageForRepair(UWorld* LoadedWorld, const FString& WorldPackageName)
{
    if (!LoadedWorld)
    {
        return false;
    }

    FString MapFilename;
    if (!FPackageName::TryConvertLongPackageNameToFilename(WorldPackageName, MapFilename, FPackageName::GetMapPackageExtension()))
    {
        return false;
    }

    UPackage* WorldPackage = LoadedWorld->GetOutermost();
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    SaveArgs.bSlowTask = false;

    const bool bSaved = UPackage::SavePackage(WorldPackage, LoadedWorld, *MapFilename, SaveArgs);
    if (bSaved)
    {
        WorldPackage->SetDirtyFlag(false);
    }
    return bSaved;
}

bool SaveLoadedAssetForMCP(UObject* Asset)
{
    if (!IsValid(Asset))
    {
        return false;
    }

    UPackage* Package = Asset->GetOutermost();
    if (!Package || Package == GetTransientPackage())
    {
        return false;
    }

    const FString PackageName = Package->GetName();
    if (!FPackageName::IsValidLongPackageName(PackageName, false))
    {
        return false;
    }

    UObject* TopLevelAsset = Asset;
    FString PackageExtension = FPackageName::GetAssetPackageExtension();
    if (UWorld* World = Cast<UWorld>(Asset))
    {
        TopLevelAsset = World;
        PackageExtension = FPackageName::GetMapPackageExtension();
    }
    else if (UWorld* WorldInPackage = UWorld::FindWorldInPackage(Package))
    {
        TopLevelAsset = WorldInPackage;
        PackageExtension = FPackageName::GetMapPackageExtension();
    }

    FString PackageFilename;
    if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, PackageFilename, PackageExtension))
    {
        return false;
    }

    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    SaveArgs.bSlowTask = false;

    const bool bSaved = UPackage::SavePackage(Package, TopLevelAsset, *PackageFilename, SaveArgs);
    if (bSaved)
    {
        Package->SetDirtyFlag(false);
    }
    return bSaved;
}

FString GetStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const FString& DefaultValue)
{
    FString Value;
    if (Params.IsValid() && Params->TryGetStringField(FieldName, Value) && !Value.IsEmpty())
    {
        return Value;
    }
    return DefaultValue;
}

bool GetBoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool bDefaultValue)
{
    bool bValue = bDefaultValue;
    if (Params.IsValid() && Params->HasTypedField<EJson::Boolean>(FieldName))
    {
        bValue = Params->GetBoolField(FieldName);
    }
    return bValue;
}

int32 GetIntParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32 DefaultValue)
{
    double Value = DefaultValue;
    if (Params.IsValid() && Params->TryGetNumberField(FieldName, Value))
    {
        return FMath::Max(0, FMath::RoundToInt(Value));
    }
    return DefaultValue;
}

bool TryGetRequiredStringParamForMCP(
    const TSharedPtr<FJsonObject>& Params,
    const TCHAR* FieldName,
    FString& OutValue,
    FString& OutError)
{
    FString Value;
    if (!Params.IsValid() || !Params->TryGetStringField(FieldName, Value))
    {
        OutError = FString::Printf(TEXT("Missing required parameter: %s"), FieldName);
        return false;
    }

    Value.TrimStartAndEndInline();
    if (Value.IsEmpty())
    {
        OutError = FString::Printf(TEXT("Missing required parameter: %s"), FieldName);
        return false;
    }

    OutValue = Value;
    return true;
}

bool TryGetRequiredContentRootParamForMCP(
    const TSharedPtr<FJsonObject>& Params,
    const TCHAR* FieldName,
    FString& OutRoot,
    FString& OutError)
{
    FString RawValue;
    if (!TryGetRequiredStringParamForMCP(Params, FieldName, RawValue, OutError))
    {
        return false;
    }

    OutRoot = NormalizeContentRoot(RawValue);
    if (!FPackageName::IsValidLongPackageName(OutRoot) || !OutRoot.StartsWith(TEXT("/Game/")))
    {
        OutError = FString::Printf(TEXT("Invalid %s: %s"), FieldName, *OutRoot);
        return false;
    }

    return true;
}

bool ValidateContentRootPairForMCP(const FString& SourceRoot, const FString& TargetRoot, FString& OutError)
{
    if (TargetRoot.Equals(SourceRoot) || TargetRoot.StartsWith(SourceRoot + TEXT("/")))
    {
        OutError = TEXT("target_root must not be the source root or a child of source_root");
        return false;
    }
    return true;
}

bool IsLiveObjectForMCP(const UObject* Object)
{
    if (!Object || !IsValid(Object))
    {
        return false;
    }

    if (Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
        || Object->HasAnyInternalFlags(EInternalObjectFlags::Garbage | EInternalObjectFlags::Unreachable))
    {
        return false;
    }

    return Object->GetClass() != nullptr && Object->GetOutermost() != nullptr;
}

bool IsPackageUnderRoot(const UObject* Object, const FString& Root)
{
    if (!IsLiveObjectForMCP(Object))
    {
        return false;
    }

    const UPackage* Package = Object->GetOutermost();
    if (!Package)
    {
        return false;
    }

    const FString PackageName = Package->GetName();
    return PackageName.Equals(Root) || PackageName.StartsWith(Root + TEXT("/"));
}

bool IsGeneratedClassInstanceFromRoot(const UObject* Object, const FString& Root)
{
    const UBlueprintGeneratedClass* GeneratedClass = IsLiveObjectForMCP(Object) ? Cast<UBlueprintGeneratedClass>(Object->GetClass()) : nullptr;
    const UBlueprint* ClassBlueprint = GeneratedClass ? Cast<UBlueprint>(GeneratedClass->ClassGeneratedBy.Get()) : nullptr;
    return ClassBlueprint && IsPackageUnderRoot(ClassBlueprint, Root);
}

bool ShouldSkipArchiveObjectReferenceRemap(const UObject* Object)
{
    if (!IsLiveObjectForMCP(Object))
    {
        return true;
    }

    // Blueprint CDOs validate their UberGraphFrame while serialized. Direct archive
    // replacement can trip that validation before the Blueprint compile pass repairs it.
    return Object->HasAnyFlags(RF_ClassDefaultObject) && Cast<UBlueprintGeneratedClass>(Object->GetClass()) != nullptr;
}

bool ShouldSkipAssetArchiveObjectReferenceRemap(const UObject* Object)
{
    if (ShouldSkipArchiveObjectReferenceRemap(Object))
    {
        return true;
    }

    if (Object->IsA<UBlueprint>())
    {
        return true;
    }

    return Cast<UBlueprintGeneratedClass>(Object->GetClass()) != nullptr;
}

int32 GetBlueprintInheritanceDepth(
    const UBlueprint* Blueprint,
    const TMap<UClass*, const UBlueprint*>& BlueprintByGeneratedClass,
    TMap<const UBlueprint*, int32>& DepthByBlueprint,
    TSet<const UBlueprint*>& VisitingBlueprints)
{
    if (!Blueprint)
    {
        return 0;
    }

    if (const int32* ExistingDepth = DepthByBlueprint.Find(Blueprint))
    {
        return *ExistingDepth;
    }

    if (VisitingBlueprints.Contains(Blueprint))
    {
        return 0;
    }

    VisitingBlueprints.Add(Blueprint);

    int32 Depth = 0;
    if (const UBlueprint* ParentBlueprint = BlueprintByGeneratedClass.FindRef(Blueprint->ParentClass))
    {
        if (ParentBlueprint != Blueprint)
        {
            Depth = GetBlueprintInheritanceDepth(ParentBlueprint, BlueprintByGeneratedClass, DepthByBlueprint, VisitingBlueprints) + 1;
        }
    }

    VisitingBlueprints.Remove(Blueprint);
    DepthByBlueprint.Add(Blueprint, Depth);
    return Depth;
}

TArray<UBlueprint*> SortBlueprintsByInheritance(const TArray<UBlueprint*>& Blueprints)
{
    TArray<UBlueprint*> OrderedBlueprints;
    TSet<UBlueprint*> AddedBlueprints;
    for (UBlueprint* Blueprint : Blueprints)
    {
        if (Blueprint && !AddedBlueprints.Contains(Blueprint))
        {
            OrderedBlueprints.Add(Blueprint);
            AddedBlueprints.Add(Blueprint);
        }
    }

    TMap<UClass*, const UBlueprint*> BlueprintByGeneratedClass;
    for (const UBlueprint* Blueprint : OrderedBlueprints)
    {
        if (!Blueprint)
        {
            continue;
        }
        if (Blueprint->GeneratedClass)
        {
            BlueprintByGeneratedClass.Add(Blueprint->GeneratedClass, Blueprint);
        }
        if (Blueprint->SkeletonGeneratedClass)
        {
            BlueprintByGeneratedClass.Add(Blueprint->SkeletonGeneratedClass, Blueprint);
        }
    }

    TMap<const UBlueprint*, int32> DepthByBlueprint;
    TSet<const UBlueprint*> VisitingBlueprints;
    for (const UBlueprint* Blueprint : OrderedBlueprints)
    {
        GetBlueprintInheritanceDepth(Blueprint, BlueprintByGeneratedClass, DepthByBlueprint, VisitingBlueprints);
    }

    Algo::Sort(OrderedBlueprints, [&DepthByBlueprint](const UBlueprint* A, const UBlueprint* B)
    {
        const int32 ADepth = DepthByBlueprint.FindRef(A);
        const int32 BDepth = DepthByBlueprint.FindRef(B);
        if (ADepth != BDepth)
        {
            return ADepth < BDepth;
        }

        const FString APath = A ? A->GetPathName() : FString();
        const FString BPath = B ? B->GetPathName() : FString();
        return APath < BPath;
    });

    return OrderedBlueprints;
}

int32 CompileBlueprintsForMCP(
    const TArray<UBlueprint*>& Blueprints,
    int32 CompilePasses,
    bool bRefreshBlueprintNodes,
    bool bRefreshFailedBlueprintNodes,
    int32& OutCompileErrorCount,
    TArray<FString>* OutFailedAssets)
{
    OutCompileErrorCount = 0;

    const TArray<UBlueprint*> OrderedBlueprints = SortBlueprintsByInheritance(Blueprints);
    TSet<UBlueprint*> FinalErrorBlueprints;
    TSet<UBlueprint*> BlueprintsToRefreshThisPass;
    int32 CompiledCount = 0;

    for (int32 PassIndex = 0; PassIndex < CompilePasses; ++PassIndex)
    {
        FinalErrorBlueprints.Reset();
        for (UBlueprint* Blueprint : OrderedBlueprints)
        {
            if (!Blueprint)
            {
                continue;
            }

            if (bRefreshBlueprintNodes || BlueprintsToRefreshThisPass.Contains(Blueprint))
            {
                FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
            }
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            FKismetEditorUtilities::CompileBlueprint(Blueprint);
            ++CompiledCount;
            if (Blueprint->Status == BS_Error)
            {
                FinalErrorBlueprints.Add(Blueprint);
            }
        }

        BlueprintsToRefreshThisPass.Reset();
        if (bRefreshFailedBlueprintNodes)
        {
            BlueprintsToRefreshThisPass = FinalErrorBlueprints;
        }
    }

    OutCompileErrorCount = FinalErrorBlueprints.Num();
    if (OutFailedAssets)
    {
        for (UBlueprint* ErrorBlueprint : FinalErrorBlueprints)
        {
            if (ErrorBlueprint)
            {
                OutFailedAssets->Add(ErrorBlueprint->GetPathName());
            }
        }
    }

    return CompiledCount;
}

int32 CompileUserDefinedStructsForMCP(const TArray<UUserDefinedStruct*>& Structs, TArray<FString>* OutFailedAssets)
{
    TSet<UUserDefinedStruct*> UniqueStructsToCompile;
    for (UUserDefinedStruct* Struct : Structs)
    {
        if (Struct)
        {
            UniqueStructsToCompile.Add(Struct);
        }
    }

    int32 CompileErrorCount = 0;
    for (UUserDefinedStruct* Struct : UniqueStructsToCompile)
    {
        Struct->Status = EUserDefinedStructureStatus::UDSS_Dirty;
        FStructureEditorUtils::CompileStructure(Struct);
        FStructureEditorUtils::RecreateDefaultInstanceInEditorData(Struct);
        Struct->UpdateStructFlags();
        if (Struct->Status == EUserDefinedStructureStatus::UDSS_Error)
        {
            ++CompileErrorCount;
            if (OutFailedAssets)
            {
                OutFailedAssets->Add(Struct->GetPathName());
            }
        }
        else
        {
            Struct->MarkPackageDirty();
            if (UPackage* Package = Struct->GetOutermost())
            {
                Package->MarkPackageDirty();
            }
        }
    }

    return CompileErrorCount;
}

void AddReplacementMapping(TMap<UObject*, UObject*>& ReplacementMap, UObject* SourceObject, UObject* TargetObject)
{
    if (SourceObject && TargetObject && SourceObject != TargetObject)
    {
        ReplacementMap.Add(SourceObject, TargetObject);
    }
}

void AddTargetWorldPackageName(TArray<FString>& TargetWorldPackageNames, UObject* TargetObject)
{
    if (UWorld* TargetWorld = Cast<UWorld>(TargetObject))
    {
        TargetWorldPackageNames.AddUnique(TargetWorld->GetOutermost()->GetName());
    }
}

void AddBlueprintReplacementMappings(TMap<UObject*, UObject*>& ReplacementMap, UObject* SourceObject, UObject* TargetObject)
{
    UBlueprint* SourceBlueprint = Cast<UBlueprint>(SourceObject);
    UBlueprint* TargetBlueprint = Cast<UBlueprint>(TargetObject);
    if (!SourceBlueprint || !TargetBlueprint)
    {
        return;
    }

    AddReplacementMapping(ReplacementMap, SourceBlueprint->GeneratedClass, TargetBlueprint->GeneratedClass);
    AddReplacementMapping(ReplacementMap, SourceBlueprint->SkeletonGeneratedClass, TargetBlueprint->SkeletonGeneratedClass);

    if (SourceBlueprint->GeneratedClass && TargetBlueprint->GeneratedClass)
    {
        AddReplacementMapping(
            ReplacementMap,
            SourceBlueprint->GeneratedClass->GetDefaultObject(false),
            TargetBlueprint->GeneratedClass->GetDefaultObject(false));
    }

    if (SourceBlueprint->SkeletonGeneratedClass && TargetBlueprint->SkeletonGeneratedClass)
    {
        AddReplacementMapping(
            ReplacementMap,
            SourceBlueprint->SkeletonGeneratedClass->GetDefaultObject(false),
            TargetBlueprint->SkeletonGeneratedClass->GetDefaultObject(false));
    }

    AddReplacementMapping(ReplacementMap, SourceBlueprint->SimpleConstructionScript, TargetBlueprint->SimpleConstructionScript);
    if (SourceBlueprint->SimpleConstructionScript && TargetBlueprint->SimpleConstructionScript)
    {
        const TArray<USCS_Node*>& SourceNodes = SourceBlueprint->SimpleConstructionScript->GetAllNodes();
        const TArray<USCS_Node*>& TargetNodes = TargetBlueprint->SimpleConstructionScript->GetAllNodes();
        for (USCS_Node* SourceNode : SourceNodes)
        {
            if (!SourceNode)
            {
                continue;
            }

            for (USCS_Node* TargetNode : TargetNodes)
            {
                if (TargetNode && TargetNode->GetVariableName() == SourceNode->GetVariableName())
                {
                    AddReplacementMapping(ReplacementMap, SourceNode, TargetNode);
                    AddReplacementMapping(ReplacementMap, SourceNode->ComponentTemplate, TargetNode->ComponentTemplate);
                    break;
                }
            }
        }
    }
}

UActorComponent* DuplicateComponentTemplateForTargetBlueprint(
    USCS_Node* SourceNode,
    UBlueprint* TargetBlueprint,
    UClass* TargetComponentClass,
    const FName TargetVariableName)
{
    if (!SourceNode || !TargetBlueprint || !TargetBlueprint->GeneratedClass || !TargetComponentClass ||
        !TargetComponentClass->IsChildOf(UActorComponent::StaticClass()) || TargetVariableName.IsNone())
    {
        return nullptr;
    }

    UObject* TemplateOuter = TargetBlueprint->GeneratedClass;
    const FString TemplateName = TargetVariableName.ToString() + USimpleConstructionScript::ComponentTemplateNameSuffix;
    if (UObject* ExistingTemplate = FindObject<UObject>(TemplateOuter, *TemplateName))
    {
        ExistingTemplate->Rename(nullptr, GetTransientPackage(), REN_DoNotDirty | REN_DontCreateRedirectors);
    }

    UActorComponent* NewTemplate = nullptr;
    if (SourceNode->ComponentTemplate)
    {
        const UClass* SourceTemplateClass = SourceNode->ComponentTemplate->GetClass();
        if (!SourceTemplateClass || TargetComponentClass->GetPropertiesSize() >= SourceTemplateClass->GetPropertiesSize())
        {
            FObjectDuplicationParameters DuplicationParameters(SourceNode->ComponentTemplate, TemplateOuter);
            DuplicationParameters.DestName = FName(*TemplateName);
            DuplicationParameters.DestClass = TargetComponentClass;
            DuplicationParameters.ApplyFlags = RF_ArchetypeObject | RF_Public | RF_Transactional;
            NewTemplate = Cast<UActorComponent>(StaticDuplicateObjectEx(DuplicationParameters));
        }
    }

    if (!NewTemplate)
    {
        NewTemplate = NewObject<UActorComponent>(
            TemplateOuter,
            TargetComponentClass,
            FName(*TemplateName),
            RF_ArchetypeObject | RF_Public | RF_Transactional);
    }

    if (NewTemplate)
    {
        NewTemplate->SetFlags(RF_ArchetypeObject | RF_Public | RF_Transactional);
        NewTemplate->ClearFlags(RF_Transient);
    }
    return NewTemplate;
}

int32 RepairBlueprintSCSNodes(UBlueprint* SourceBlueprint, UBlueprint* TargetBlueprint, TMap<UObject*, UObject*>& ReplacementMap)
{
    if (!SourceBlueprint || !TargetBlueprint || !SourceBlueprint->SimpleConstructionScript || !TargetBlueprint->SimpleConstructionScript)
    {
        return 0;
    }

    int32 RepairCount = 0;
    USimpleConstructionScript* SourceSCS = SourceBlueprint->SimpleConstructionScript;
    USimpleConstructionScript* TargetSCS = TargetBlueprint->SimpleConstructionScript;

    TMap<FName, USCS_Node*> TargetNodesByName;
    const TArray<USCS_Node*>& TargetNodes = TargetSCS->GetAllNodes();
    for (USCS_Node* TargetNode : TargetNodes)
    {
        if (TargetNode && !TargetNode->GetVariableName().IsNone())
        {
            TargetNodesByName.FindOrAdd(TargetNode->GetVariableName(), TargetNode);
        }
    }

    const TArray<USCS_Node*>& SourceNodes = SourceSCS->GetAllNodes();
    for (USCS_Node* SourceNode : SourceNodes)
    {
        if (!SourceNode || SourceNode->GetVariableName().IsNone() || !SourceNode->ComponentClass)
        {
            continue;
        }

        UClass* TargetComponentClass = Cast<UClass>(FindReplacementObject(ReplacementMap, SourceNode->ComponentClass));
        if (!TargetComponentClass)
        {
            TargetComponentClass = SourceNode->ComponentClass;
        }
        if (!TargetComponentClass || !TargetComponentClass->IsChildOf(UActorComponent::StaticClass()))
        {
            continue;
        }

        const FName VariableName = SourceNode->GetVariableName();
        USCS_Node* TargetNode = TargetNodesByName.FindRef(VariableName);
        if (TargetNode)
        {
            AddReplacementMapping(ReplacementMap, SourceNode, TargetNode);
            AddReplacementMapping(ReplacementMap, SourceNode->ComponentTemplate, TargetNode->ComponentTemplate);

            if (TargetNode->ComponentClass != TargetComponentClass)
            {
                if (UActorComponent* NewTemplate = DuplicateComponentTemplateForTargetBlueprint(SourceNode, TargetBlueprint, TargetComponentClass, VariableName))
                {
                    TargetNode->Modify();
                    TargetNode->ComponentClass = TargetComponentClass;
                    TargetNode->ComponentTemplate = NewTemplate;
                    TargetNode->VariableGuid = SourceNode->VariableGuid.IsValid() ? SourceNode->VariableGuid : TargetNode->VariableGuid;
                    TargetNode->CategoryName = SourceNode->CategoryName;
                    TargetNode->MetaDataArray = SourceNode->MetaDataArray;
                    TargetNode->AttachToName = SourceNode->AttachToName;
                    AddReplacementMapping(ReplacementMap, SourceNode->ComponentTemplate, NewTemplate);
                    ++RepairCount;
                }
            }
            continue;
        }

        UActorComponent* NewTemplate = DuplicateComponentTemplateForTargetBlueprint(SourceNode, TargetBlueprint, TargetComponentClass, VariableName);
        if (!NewTemplate)
        {
            continue;
        }

        USCS_Node* NewNode = TargetSCS->CreateNodeAndRenameComponent(NewTemplate);
        if (!NewNode)
        {
            continue;
        }

        NewNode->Modify();
        NewNode->ComponentClass = TargetComponentClass;
        NewNode->VariableGuid = SourceNode->VariableGuid.IsValid() ? SourceNode->VariableGuid : NewNode->VariableGuid;
        NewNode->CategoryName = SourceNode->CategoryName;
        NewNode->MetaDataArray = SourceNode->MetaDataArray;
        NewNode->AttachToName = SourceNode->AttachToName;
        NewNode->SetVariableName(VariableName, true);

        USCS_Node* SourceParentNode = SourceSCS->FindParentNode(SourceNode);
        USCS_Node* TargetParentNode = SourceParentNode ? TargetNodesByName.FindRef(SourceParentNode->GetVariableName()) : nullptr;
        if (TargetParentNode)
        {
            NewNode->SetParent(TargetParentNode);
            TargetParentNode->AddChildNode(NewNode);
        }
        else
        {
            TargetSCS->AddNode(NewNode);
        }

        TargetNodesByName.Add(VariableName, NewNode);
        AddReplacementMapping(ReplacementMap, SourceNode, NewNode);
        AddReplacementMapping(ReplacementMap, SourceNode->ComponentTemplate, NewNode->ComponentTemplate);
        ++RepairCount;
    }

    if (RepairCount > 0)
    {
        TargetSCS->Modify();
        TargetSCS->CreateNameToSCSNodeMap();
        TargetSCS->ValidateSceneRootNodes();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(TargetBlueprint);
        TargetBlueprint->MarkPackageDirty();
    }

    return RepairCount;
}

UWidget* DuplicateWidgetForTargetClass(UWidget* SourceWidget, UWidgetTree* TargetWidgetTree, UClass* TargetWidgetClass)
{
    if (!SourceWidget || !TargetWidgetTree || !TargetWidgetClass || !TargetWidgetClass->IsChildOf(UWidget::StaticClass()))
    {
        return nullptr;
    }

    const UClass* SourceWidgetClass = SourceWidget->GetClass();
    if (SourceWidgetClass && TargetWidgetClass->GetPropertiesSize() < SourceWidgetClass->GetPropertiesSize())
    {
        return nullptr;
    }

    const FName WidgetName = SourceWidget->GetFName();
    const FName TransientName = MakeUniqueObjectName(GetTransientPackage(), SourceWidget->GetClass(), WidgetName);
    SourceWidget->Rename(*TransientName.ToString(), GetTransientPackage(), REN_DoNotDirty | REN_DontCreateRedirectors);

    FObjectDuplicationParameters DuplicationParameters(SourceWidget, TargetWidgetTree);
    DuplicationParameters.DestName = WidgetName;
    DuplicationParameters.DestClass = TargetWidgetClass;
    DuplicationParameters.ApplyFlags = RF_Transactional;
    UWidget* NewWidget = Cast<UWidget>(StaticDuplicateObjectEx(DuplicationParameters));
    if (!NewWidget)
    {
        SourceWidget->Rename(*WidgetName.ToString(), TargetWidgetTree, REN_DoNotDirty | REN_DontCreateRedirectors);
        return nullptr;
    }

    NewWidget->SetFlags(RF_Transactional);
    NewWidget->bIsVariable = SourceWidget->bIsVariable;
    return NewWidget;
}

int32 RepairWidgetBlueprintTree(UBlueprint* TargetBlueprint, const TMap<UObject*, UObject*>& ReplacementMap)
{
    UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(TargetBlueprint);
    if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
    {
        return 0;
    }

    TArray<UWidget*> Widgets;
    WidgetBlueprint->WidgetTree->GetAllWidgets(Widgets);
    TMap<UObject*, UObject*> WidgetReplacementMap;
    int32 RepairCount = 0;

    for (UWidget* Widget : Widgets)
    {
        if (!Widget)
        {
            continue;
        }

        UClass* TargetWidgetClass = Cast<UClass>(FindReplacementObject(ReplacementMap, Widget->GetClass()));
        if (!TargetWidgetClass || TargetWidgetClass == Widget->GetClass() || !TargetWidgetClass->IsChildOf(UWidget::StaticClass()))
        {
            continue;
        }

        if (UWidget* NewWidget = DuplicateWidgetForTargetClass(Widget, WidgetBlueprint->WidgetTree, TargetWidgetClass))
        {
#if WITH_EDITORONLY_DATA
            NewWidget->WidgetGeneratedBy = WidgetBlueprint;
#endif
            WidgetReplacementMap.Add(Widget, NewWidget);
            ++RepairCount;
        }
    }

    if (WidgetReplacementMap.IsEmpty())
    {
        return 0;
    }

    TSet<UObject*> ObjectsToPatch;
    ObjectsToPatch.Add(WidgetBlueprint);
    ObjectsToPatch.Add(WidgetBlueprint->WidgetTree);
    if (UPackage* Package = WidgetBlueprint->GetOutermost())
    {
        TArray<UObject*> PackageObjects;
        GetObjectsWithPackage(Package, PackageObjects, true, RF_Transient, EInternalObjectFlags::Garbage);
        for (UObject* PackageObject : PackageObjects)
        {
            if (IsLiveObjectForMCP(PackageObject))
            {
                ObjectsToPatch.Add(PackageObject);
            }
        }
    }

    for (UObject* ObjectToPatch : ObjectsToPatch)
    {
        if (ShouldSkipArchiveObjectReferenceRemap(ObjectToPatch))
        {
            continue;
        }

        ObjectToPatch->Modify();
        FArchiveReplaceObjectRef<UObject> ReplaceAr(
            ObjectToPatch,
            WidgetReplacementMap,
            EArchiveReplaceObjectFlags::IgnoreOuterRef |
            EArchiveReplaceObjectFlags::IgnoreArchetypeRef |
            EArchiveReplaceObjectFlags::IncludeClassGeneratedByRef);
        if (ReplaceAr.GetCount() > 0)
        {
            ObjectToPatch->MarkPackageDirty();
        }
    }

    WidgetBlueprint->WidgetTree->Modify();
    WidgetBlueprint->WidgetTree->MarkPackageDirty();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
    WidgetBlueprint->MarkPackageDirty();
    return RepairCount;
}

int32 RepairTargetSubobjectsWithSourceClasses(const TSet<UPackage*>& TargetPackages, TMap<UObject*, UObject*>& ReplacementMap)
{
    if (TargetPackages.IsEmpty() || ReplacementMap.IsEmpty())
    {
        return 0;
    }

    TMap<UObject*, UObject*> LocalReplacementMap;
    for (UPackage* TargetPackage : TargetPackages)
    {
        if (!TargetPackage)
        {
            continue;
        }

        TArray<UObject*> PackageObjects;
        GetObjectsWithPackage(TargetPackage, PackageObjects, true, RF_Transient, EInternalObjectFlags::Garbage);
        for (UObject* PackageObject : PackageObjects)
        {
            if (!IsLiveObjectForMCP(PackageObject) || PackageObject->HasAnyFlags(RF_ClassDefaultObject) || PackageObject->IsA<UClass>() || PackageObject->IsA<UBlueprint>())
            {
                continue;
            }

            UBlueprintGeneratedClass* OuterGeneratedClass = Cast<UBlueprintGeneratedClass>(PackageObject->GetOuter());
            if (!OuterGeneratedClass)
            {
                continue;
            }

            UClass* SourceClass = PackageObject->GetClass();
            if (!IsLiveObjectForMCP(SourceClass))
            {
                continue;
            }

            UClass* TargetClass = Cast<UClass>(FindReplacementObject(ReplacementMap, SourceClass));
            if (!IsLiveObjectForMCP(TargetClass) || TargetClass == SourceClass || TargetClass->GetPropertiesSize() < SourceClass->GetPropertiesSize())
            {
                continue;
            }

            UObject* OriginalOuter = PackageObject->GetOuter();
            const FName OriginalName = PackageObject->GetFName();
            if (!IsLiveObjectForMCP(OriginalOuter) || OriginalName.IsNone())
            {
                continue;
            }

            const EObjectFlags OriginalFlags = PackageObject->GetFlags();
            const FName TransientName = MakeUniqueObjectName(GetTransientPackage(), SourceClass, OriginalName);
            PackageObject->Rename(*TransientName.ToString(), GetTransientPackage(), REN_DoNotDirty | REN_DontCreateRedirectors);

            FObjectDuplicationParameters DuplicationParameters(PackageObject, OriginalOuter);
            DuplicationParameters.DestName = OriginalName;
            DuplicationParameters.DestClass = TargetClass;
            DuplicationParameters.ApplyFlags = OriginalFlags & (RF_Public | RF_Standalone | RF_Transactional | RF_ArchetypeObject);

            UObject* NewObject = StaticDuplicateObjectEx(DuplicationParameters);
            if (!NewObject)
            {
                PackageObject->Rename(*OriginalName.ToString(), OriginalOuter, REN_DoNotDirty | REN_DontCreateRedirectors);
                continue;
            }

            NewObject->SetFlags(OriginalFlags & (RF_Public | RF_Standalone | RF_Transactional | RF_ArchetypeObject));
            NewObject->ClearFlags(RF_Transient);
            LocalReplacementMap.Add(PackageObject, NewObject);
            ReplacementMap.Add(PackageObject, NewObject);
        }
    }

    if (LocalReplacementMap.IsEmpty())
    {
        return 0;
    }

    TSet<UObject*> ObjectsToPatch;
    for (UPackage* TargetPackage : TargetPackages)
    {
        if (!TargetPackage)
        {
            continue;
        }

        TArray<UObject*> PackageObjects;
        GetObjectsWithPackage(TargetPackage, PackageObjects, true, RF_Transient, EInternalObjectFlags::Garbage);
        for (UObject* PackageObject : PackageObjects)
        {
            if (IsLiveObjectForMCP(PackageObject))
            {
                ObjectsToPatch.Add(PackageObject);
            }
        }
    }

    for (UObject* ObjectToPatch : ObjectsToPatch)
    {
        if (ShouldSkipArchiveObjectReferenceRemap(ObjectToPatch))
        {
            continue;
        }

        ObjectToPatch->Modify();
        FArchiveReplaceObjectRef<UObject> ReplaceAr(
            ObjectToPatch,
            LocalReplacementMap,
            EArchiveReplaceObjectFlags::IgnoreOuterRef |
            EArchiveReplaceObjectFlags::IgnoreArchetypeRef |
            EArchiveReplaceObjectFlags::IncludeClassGeneratedByRef);
        if (ReplaceAr.GetCount() > 0)
        {
            ObjectToPatch->MarkPackageDirty();
            if (UPackage* Package = ObjectToPatch->GetOutermost())
            {
                Package->MarkPackageDirty();
            }
        }
    }

    return LocalReplacementMap.Num();
}

void RefreshMaterialReferencesAndCaches(
    const TSet<UPackage*>& TargetPackages,
    const TMap<UObject*, UObject*>& ReplacementMap,
    int32& OutTextureReferenceRepairCount,
    int32& OutMaterialCacheRefreshCount)
{
    if (TargetPackages.IsEmpty())
    {
        return;
    }

    TSet<UMaterial*> Materials;
    TSet<UMaterialFunction*> MaterialFunctions;
    TSet<UMaterialInstance*> MaterialInstances;
    TSet<UPackage*> DirtyPackages;

    for (UPackage* TargetPackage : TargetPackages)
    {
        if (!TargetPackage)
        {
            continue;
        }

        TArray<UObject*> PackageObjects;
        GetObjectsWithPackage(TargetPackage, PackageObjects, true, RF_Transient, EInternalObjectFlags::Garbage);
        for (UObject* PackageObject : PackageObjects)
        {
            if (!IsLiveObjectForMCP(PackageObject))
            {
                continue;
            }

            if (UMaterialExpressionTextureBase* TextureExpression = Cast<UMaterialExpressionTextureBase>(PackageObject))
            {
                if (UTexture* ReplacementTexture = Cast<UTexture>(FindReplacementObject(ReplacementMap, TextureExpression->Texture)))
                {
                    if (ReplacementTexture != TextureExpression->Texture)
                    {
                        TextureExpression->Modify();
                        TextureExpression->Texture = ReplacementTexture;
                        TextureExpression->AutoSetSampleType();
                        TextureExpression->MarkPackageDirty();
                        DirtyPackages.Add(TextureExpression->GetOutermost());
                        ++OutTextureReferenceRepairCount;
                    }
                }
                else if (TextureExpression->Texture)
                {
                    TextureExpression->AutoSetSampleType();
                }
            }

            if (UMaterial* Material = Cast<UMaterial>(PackageObject))
            {
                Materials.Add(Material);
            }
            else if (UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(PackageObject))
            {
                MaterialFunctions.Add(MaterialFunction);
            }
            else if (UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(PackageObject))
            {
                MaterialInstances.Add(MaterialInstance);
            }
        }
    }

    for (UMaterialInstance* MaterialInstance : MaterialInstances)
    {
        if (!MaterialInstance)
        {
            continue;
        }

        bool bChanged = false;
        for (FTextureParameterValue& TextureParameter : MaterialInstance->TextureParameterValues)
        {
            if (UTexture* ReplacementTexture = Cast<UTexture>(FindReplacementObject(ReplacementMap, TextureParameter.ParameterValue)))
            {
                if (ReplacementTexture != TextureParameter.ParameterValue)
                {
                    if (!bChanged)
                    {
                        MaterialInstance->Modify();
                    }
                    TextureParameter.ParameterValue = ReplacementTexture;
                    bChanged = true;
                    ++OutTextureReferenceRepairCount;
                }
            }
        }

        if (bChanged)
        {
            MaterialInstance->MarkPackageDirty();
            DirtyPackages.Add(MaterialInstance->GetOutermost());
        }
    }

    FMaterialUpdateContext MaterialUpdateContext(FMaterialUpdateContext::EOptions::Default);

    for (UMaterialFunction* MaterialFunction : MaterialFunctions)
    {
        if (!MaterialFunction)
        {
            continue;
        }

        MaterialFunction->Modify();
        MaterialFunction->UpdateFromFunctionResource();
        MaterialFunction->UpdateInputOutputTypes();
        MaterialFunction->UpdateDependentFunctionCandidates();
        MaterialFunction->ForceRecompileForRendering(MaterialUpdateContext, nullptr);
        MaterialFunction->MarkPackageDirty();
        DirtyPackages.Add(MaterialFunction->GetOutermost());
        ++OutMaterialCacheRefreshCount;
    }

    for (UMaterial* Material : Materials)
    {
        if (!Material)
        {
            continue;
        }

        Material->Modify();
        Material->UpdateCachedExpressionData();
        Material->BuildEditorParameterList();
        Material->ForceRecompileForRendering(EMaterialShaderPrecompileMode::None);
        Material->MarkPackageDirty();
        DirtyPackages.Add(Material->GetOutermost());
        ++OutMaterialCacheRefreshCount;
    }

    for (UMaterialInstance* MaterialInstance : MaterialInstances)
    {
        if (!MaterialInstance)
        {
            continue;
        }

        MaterialInstance->Modify();
        MaterialInstance->ForceRecompileForRendering(EMaterialShaderPrecompileMode::None);
        MaterialInstance->MarkPackageDirty();
        DirtyPackages.Add(MaterialInstance->GetOutermost());
        ++OutMaterialCacheRefreshCount;
    }

    for (UPackage* DirtyPackage : DirtyPackages)
    {
        if (DirtyPackage)
        {
            DirtyPackage->MarkPackageDirty();
        }
    }
}

int32 RepairTargetWorldComponentClasses(
    UWorld* LoadedWorld,
    const TMap<UObject*, UObject*>& ReplacementMap,
    TArray<FString>& FailedAssets)
{
    if (!LoadedWorld || ReplacementMap.IsEmpty())
    {
        return 0;
    }

    TArray<TPair<UActorComponent*, UClass*>> ComponentsToRepair;
    for (ULevel* Level : LoadedWorld->GetLevels())
    {
        if (!Level)
        {
            continue;
        }

        for (AActor* Actor : Level->Actors)
        {
            if (!IsLiveObjectForMCP(Actor))
            {
                continue;
            }

            TArray<UActorComponent*> Components;
            Actor->GetComponents(Components);
            for (UActorComponent* Component : Components)
            {
                if (!IsLiveObjectForMCP(Component))
                {
                    continue;
                }

                UClass* SourceClass = Component->GetClass();
                if (!IsLiveObjectForMCP(SourceClass))
                {
                    continue;
                }

                UClass* TargetClass = Cast<UClass>(FindReplacementObject(ReplacementMap, SourceClass));
                if (!IsLiveObjectForMCP(TargetClass) || TargetClass == SourceClass || !TargetClass->IsChildOf(UActorComponent::StaticClass()))
                {
                    continue;
                }

                ComponentsToRepair.Emplace(Component, TargetClass);
            }
        }
    }

    if (ComponentsToRepair.IsEmpty())
    {
        return 0;
    }

    int32 RepairedComponentCount = 0;
    TMap<UObject*, UObject*> ComponentReplacementMap;
    TMap<UObject*, UObject*> CopyReplacementMap = ReplacementMap;

    for (const TPair<UActorComponent*, UClass*>& ComponentPair : ComponentsToRepair)
    {
        UActorComponent* OldComponent = ComponentPair.Key;
        UClass* TargetClass = ComponentPair.Value;
        if (!IsLiveObjectForMCP(OldComponent) || !IsLiveObjectForMCP(TargetClass))
        {
            continue;
        }

        AActor* Owner = OldComponent->GetOwner();
        UObject* OriginalOuter = OldComponent->GetOuter();
        const FName OriginalName = OldComponent->GetFName();
        if (!IsLiveObjectForMCP(Owner) || !IsLiveObjectForMCP(OriginalOuter))
        {
            FailedAssets.Add(OldComponent->GetPathName() + TEXT(" (failed to resolve owner/outer for component class repair)"));
            continue;
        }

        Owner->Modify();
        OriginalOuter->Modify();
        OldComponent->Modify();

        const bool bWasRegistered = OldComponent->IsRegistered();
        if (bWasRegistered)
        {
            OldComponent->UnregisterComponent();
        }
        Owner->RemoveInstanceComponent(OldComponent);
        Owner->RemoveOwnedComponent(OldComponent);

        const FName ReplacedName = MakeUniqueObjectName(
            GetTransientPackage(),
            OldComponent->GetClass(),
            FName(*FString::Printf(TEXT("%s_Replaced"), *OriginalName.ToString())));

        if (!OldComponent->Rename(*ReplacedName.ToString(), GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional))
        {
            FailedAssets.Add(OldComponent->GetPathName() + TEXT(" (failed to rename source-class component before repair)"));
            continue;
        }

        UActorComponent* NewComponent = NewObject<UActorComponent>(OriginalOuter, TargetClass, OriginalName, RF_Transactional);
        if (!NewComponent)
        {
            FailedAssets.Add(Owner->GetPathName() + TEXT(".") + OriginalName.ToString() + TEXT(" (failed to create target-class component)"));
            continue;
        }

        UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyParams;
        CopyParams.OptionalReplacementMappings = &CopyReplacementMap;
        CopyParams.bReplaceObjectClassReferences = true;
        CopyParams.bReplaceInternalReferenceUponRead = true;
        UEngine::CopyPropertiesForUnrelatedObjects(OldComponent, NewComponent, CopyParams);

        Owner->AddInstanceComponent(NewComponent);
        Owner->AddOwnedComponent(NewComponent);
        if (bWasRegistered && !NewComponent->IsRegistered())
        {
            NewComponent->RegisterComponent();
        }

        ComponentReplacementMap.Add(OldComponent, NewComponent);
        ++RepairedComponentCount;
    }

    if (ComponentReplacementMap.IsEmpty())
    {
        return 0;
    }

    TArray<UObject*> WorldPackageObjects;
    GetObjectsWithPackage(LoadedWorld->GetOutermost(), WorldPackageObjects, true, RF_Transient, EInternalObjectFlags::Garbage);
    for (UObject* WorldPackageObject : WorldPackageObjects)
    {
        if (ShouldSkipArchiveObjectReferenceRemap(WorldPackageObject))
        {
            continue;
        }

        FArchiveReplaceObjectRef<UObject> ReplaceRefs(
            WorldPackageObject,
            ComponentReplacementMap,
            EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
    }

    LoadedWorld->MarkPackageDirty();
    LoadedWorld->GetOutermost()->MarkPackageDirty();
    for (ULevel* Level : LoadedWorld->GetLevels())
    {
        if (Level)
        {
            Level->MarkPackageDirty();
        }
    }

    return RepairedComponentCount;
}

int32 RemapObjectPropertyValueForMCP(
    FObjectPropertyBase* ObjectProperty,
    void* ValuePtr,
    const TMap<UObject*, UObject*>& ReplacementMap)
{
    if (!ObjectProperty || !ValuePtr)
    {
        return 0;
    }

    UObject* CurrentObject = ObjectProperty->GetObjectPropertyValue(ValuePtr);
    UObject* const* ReplacementObject = ReplacementMap.Find(CurrentObject);
    if (!ReplacementObject || !IsLiveObjectForMCP(*ReplacementObject))
    {
        return 0;
    }

    ObjectProperty->SetObjectPropertyValue(ValuePtr, *ReplacementObject);
    return 1;
}

int32 RemapInterfacePropertyValueForMCP(
    FInterfaceProperty* InterfaceProperty,
    void* ValuePtr,
    const TMap<UObject*, UObject*>& ReplacementMap)
{
    if (!InterfaceProperty || !ValuePtr)
    {
        return 0;
    }

    FScriptInterface* InterfaceValue = static_cast<FScriptInterface*>(ValuePtr);
    UObject* CurrentObject = InterfaceValue ? InterfaceValue->GetObject() : nullptr;
    UObject* const* ReplacementObject = ReplacementMap.Find(CurrentObject);
    if (!ReplacementObject || !IsLiveObjectForMCP(*ReplacementObject))
    {
        return 0;
    }

    UClass* ReplacementClass = (*ReplacementObject)->GetClass();
    if (!ReplacementClass || !ReplacementClass->ImplementsInterface(InterfaceProperty->InterfaceClass))
    {
        return 0;
    }

    InterfaceValue->SetObject(*ReplacementObject);
    InterfaceValue->SetInterface((*ReplacementObject)->GetInterfaceAddress(InterfaceProperty->InterfaceClass));
    return 1;
}

int32 RemapObjectReferencesInPropertyValueForMCP(
    FProperty* Property,
    void* ValuePtr,
    const TMap<UObject*, UObject*>& ReplacementMap)
{
    if (!Property || !ValuePtr)
    {
        return 0;
    }

    if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
    {
        return RemapObjectPropertyValueForMCP(ObjectProperty, ValuePtr, ReplacementMap);
    }

    if (FInterfaceProperty* InterfaceProperty = CastField<FInterfaceProperty>(Property))
    {
        return RemapInterfacePropertyValueForMCP(InterfaceProperty, ValuePtr, ReplacementMap);
    }

    if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
    {
        int32 RemapCount = 0;
        FScriptArrayHelper ArrayHelper(ArrayProperty, ValuePtr);
        for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
        {
            RemapCount += RemapObjectReferencesInPropertyValueForMCP(
                ArrayProperty->Inner,
                ArrayHelper.GetRawPtr(Index),
                ReplacementMap);
        }
        return RemapCount;
    }

    if (FSetProperty* SetProperty = CastField<FSetProperty>(Property))
    {
        int32 RemapCount = 0;
        bool bChangedSetKey = false;
        FScriptSetHelper SetHelper(SetProperty, ValuePtr);
        for (int32 InternalIndex = 0; InternalIndex < SetHelper.GetMaxIndex(); ++InternalIndex)
        {
            if (!SetHelper.IsValidIndex(InternalIndex))
            {
                continue;
            }

            const int32 CountBefore = RemapCount;
            RemapCount += RemapObjectReferencesInPropertyValueForMCP(
                SetProperty->ElementProp,
                SetHelper.GetElementPtr(InternalIndex),
                ReplacementMap);
            bChangedSetKey |= RemapCount != CountBefore;
        }

        if (bChangedSetKey)
        {
            SetHelper.Rehash();
        }
        return RemapCount;
    }

    if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
    {
        int32 RemapCount = 0;
        bool bChangedMapKey = false;
        FScriptMapHelper MapHelper(MapProperty, ValuePtr);
        for (int32 InternalIndex = 0; InternalIndex < MapHelper.GetMaxIndex(); ++InternalIndex)
        {
            if (!MapHelper.IsValidIndex(InternalIndex))
            {
                continue;
            }

            const int32 CountBeforeKey = RemapCount;
            RemapCount += RemapObjectReferencesInPropertyValueForMCP(
                MapProperty->KeyProp,
                MapHelper.GetKeyPtr(InternalIndex),
                ReplacementMap);
            bChangedMapKey |= RemapCount != CountBeforeKey;

            RemapCount += RemapObjectReferencesInPropertyValueForMCP(
                MapProperty->ValueProp,
                MapHelper.GetValuePtr(InternalIndex),
                ReplacementMap);
        }

        if (bChangedMapKey)
        {
            MapHelper.Rehash();
        }
        return RemapCount;
    }

    if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
    {
        if (!StructProperty->Struct)
        {
            return 0;
        }

        int32 RemapCount = 0;
        for (TFieldIterator<FProperty> InnerPropertyIt(StructProperty->Struct); InnerPropertyIt; ++InnerPropertyIt)
        {
            FProperty* InnerProperty = *InnerPropertyIt;
            if (!InnerProperty)
            {
                continue;
            }

            for (int32 ArrayIndex = 0; ArrayIndex < InnerProperty->ArrayDim; ++ArrayIndex)
            {
                RemapCount += RemapObjectReferencesInPropertyValueForMCP(
                    InnerProperty,
                    InnerProperty->ContainerPtrToValuePtr<void>(ValuePtr, ArrayIndex),
                    ReplacementMap);
            }
        }
        return RemapCount;
    }

    return 0;
}

int32 RemapObjectReferencesInObjectPropertiesForMCP(UObject* Object, const TMap<UObject*, UObject*>& ReplacementMap)
{
    if (!IsLiveObjectForMCP(Object) || ReplacementMap.IsEmpty())
    {
        return 0;
    }

    Object->Modify();

    int32 RemapCount = 0;
    for (TFieldIterator<FProperty> PropertyIt(Object->GetClass()); PropertyIt; ++PropertyIt)
    {
        FProperty* Property = *PropertyIt;
        if (!Property)
        {
            continue;
        }

        for (int32 ArrayIndex = 0; ArrayIndex < Property->ArrayDim; ++ArrayIndex)
        {
            RemapCount += RemapObjectReferencesInPropertyValueForMCP(
                Property,
                Property->ContainerPtrToValuePtr<void>(Object, ArrayIndex),
                ReplacementMap);
        }
    }

    if (RemapCount > 0)
    {
        Object->MarkPackageDirty();
        if (UPackage* Package = Object->GetOutermost())
        {
            Package->MarkPackageDirty();
        }
    }

    return RemapCount;
}

int32 RemapTargetWorldActorInstanceReferences(UWorld* LoadedWorld, const TMap<UObject*, UObject*>& ReplacementMap)
{
    if (!LoadedWorld || ReplacementMap.IsEmpty())
    {
        return 0;
    }

    int32 RemapCount = 0;
    for (ULevel* Level : LoadedWorld->GetLevels())
    {
        if (!Level)
        {
            continue;
        }

        for (AActor* Actor : Level->Actors)
        {
            if (!IsLiveObjectForMCP(Actor))
            {
                continue;
            }

            int32 ActorRemapCount = RemapObjectReferencesInObjectPropertiesForMCP(Actor, ReplacementMap);
            TArray<UActorComponent*> Components;
            Actor->GetComponents(Components);
            for (UActorComponent* Component : Components)
            {
                ActorRemapCount += RemapObjectReferencesInObjectPropertiesForMCP(Component, ReplacementMap);
            }

            if (ActorRemapCount > 0)
            {
                RemapCount += ActorRemapCount;
                Actor->MarkPackageDirty();
            }
        }
    }

    return RemapCount;
}

bool ShouldRemoveRemappedMapKey(UObject* KeyObject, const FString& SourceRoot)
{
    if (!IsLiveObjectForMCP(KeyObject))
    {
        return true;
    }

    return IsPackageUnderRoot(KeyObject, SourceRoot)
        || IsPackageUnderRoot(KeyObject->GetClass(), SourceRoot)
        || IsGeneratedClassInstanceFromRoot(KeyObject, SourceRoot);
}

int32 RemoveSourceObjectKeyMapEntries(UObject* Object, const FString& SourceRoot)
{
    if (!IsLiveObjectForMCP(Object))
    {
        return 0;
    }

    int32 RemovedCount = 0;
    for (TFieldIterator<FProperty> PropertyIt(Object->GetClass()); PropertyIt; ++PropertyIt)
    {
        FMapProperty* MapProperty = CastField<FMapProperty>(*PropertyIt);
        if (!MapProperty)
        {
            continue;
        }

        FObjectPropertyBase* KeyObjectProperty = CastField<FObjectPropertyBase>(MapProperty->KeyProp);
        if (!KeyObjectProperty)
        {
            continue;
        }

        void* MapValuePtr = MapProperty->ContainerPtrToValuePtr<void>(Object);
        FScriptMapHelper MapHelper(MapProperty, MapValuePtr);
        bool bRemovedFromMap = false;
        for (int32 InternalIndex = MapHelper.GetMaxIndex() - 1; InternalIndex >= 0; --InternalIndex)
        {
            if (!MapHelper.IsValidIndex(InternalIndex))
            {
                continue;
            }

            UObject* KeyObject = KeyObjectProperty->GetObjectPropertyValue(MapHelper.GetKeyPtr(InternalIndex));
            if (!ShouldRemoveRemappedMapKey(KeyObject, SourceRoot))
            {
                continue;
            }

            MapHelper.RemoveAt(InternalIndex);
            ++RemovedCount;
            bRemovedFromMap = true;
        }

        if (bRemovedFromMap)
        {
            MapHelper.Rehash();
        }
    }

    if (RemovedCount > 0)
    {
        Object->Modify();
        Object->MarkPackageDirty();
    }
    return RemovedCount;
}

int32 RemoveSourceObjectKeyMapEntriesFromWorld(UWorld* LoadedWorld, const FString& SourceRoot)
{
    if (!LoadedWorld)
    {
        return 0;
    }

    int32 RemovedCount = 0;
    for (ULevel* Level : LoadedWorld->GetLevels())
    {
        if (!Level)
        {
            continue;
        }

        for (AActor* Actor : Level->Actors)
        {
            if (!IsLiveObjectForMCP(Actor))
            {
                continue;
            }

            RemovedCount += RemoveSourceObjectKeyMapEntries(Actor, SourceRoot);

            TArray<UActorComponent*> Components;
            Actor->GetComponents(Components);
            for (UActorComponent* Component : Components)
            {
                if (IsLiveObjectForMCP(Component))
                {
                    RemovedCount += RemoveSourceObjectKeyMapEntries(Component, SourceRoot);
                }
            }
        }
    }
    return RemovedCount;
}

int32 CountSourceObjectKeyMapEntries(UObject* Object, const FString& SourceRoot)
{
    if (!IsLiveObjectForMCP(Object))
    {
        return 0;
    }

    int32 SourceKeyCount = 0;
    for (TFieldIterator<FProperty> PropertyIt(Object->GetClass()); PropertyIt; ++PropertyIt)
    {
        FMapProperty* MapProperty = CastField<FMapProperty>(*PropertyIt);
        if (!MapProperty)
        {
            continue;
        }

        FObjectPropertyBase* KeyObjectProperty = CastField<FObjectPropertyBase>(MapProperty->KeyProp);
        if (!KeyObjectProperty)
        {
            continue;
        }

        void* MapValuePtr = MapProperty->ContainerPtrToValuePtr<void>(Object);
        FScriptMapHelper MapHelper(MapProperty, MapValuePtr);
        for (int32 InternalIndex = 0; InternalIndex < MapHelper.GetMaxIndex(); ++InternalIndex)
        {
            if (!MapHelper.IsValidIndex(InternalIndex))
            {
                continue;
            }

            UObject* KeyObject = KeyObjectProperty->GetObjectPropertyValue(MapHelper.GetKeyPtr(InternalIndex));
            if (ShouldRemoveRemappedMapKey(KeyObject, SourceRoot))
            {
                ++SourceKeyCount;
            }
        }
    }

    return SourceKeyCount;
}

int32 CountSourceObjectKeyMapEntriesFromWorld(UWorld* LoadedWorld, const FString& SourceRoot)
{
    if (!LoadedWorld)
    {
        return 0;
    }

    int32 SourceKeyCount = 0;
    for (ULevel* Level : LoadedWorld->GetLevels())
    {
        if (!Level)
        {
            continue;
        }

        for (AActor* Actor : Level->Actors)
        {
            if (!IsLiveObjectForMCP(Actor))
            {
                continue;
            }

            SourceKeyCount += CountSourceObjectKeyMapEntries(Actor, SourceRoot);

            TArray<UActorComponent*> Components;
            Actor->GetComponents(Components);
            for (UActorComponent* Component : Components)
            {
                if (IsLiveObjectForMCP(Component))
                {
                    SourceKeyCount += CountSourceObjectKeyMapEntries(Component, SourceRoot);
                }
            }
        }
    }

    return SourceKeyCount;
}

void CountWorldSourceClassInstances(
    UWorld* LoadedWorld,
    const FString& SourceRoot,
    int32& OutSourceActorCount,
    int32& OutSourceComponentCount)
{
    OutSourceActorCount = 0;
    OutSourceComponentCount = 0;

    if (!LoadedWorld)
    {
        return;
    }

    for (ULevel* Level : LoadedWorld->GetLevels())
    {
        if (!Level)
        {
            continue;
        }

        for (AActor* Actor : Level->Actors)
        {
            if (!IsLiveObjectForMCP(Actor))
            {
                continue;
            }

            UClass* ActorClass = Actor->GetClass();
            if (IsLiveObjectForMCP(ActorClass) && IsPackageUnderRoot(ActorClass, SourceRoot))
            {
                ++OutSourceActorCount;
            }

            TArray<UActorComponent*> Components;
            Actor->GetComponents(Components);
            for (UActorComponent* Component : Components)
            {
                UClass* ComponentClass = IsLiveObjectForMCP(Component) ? Component->GetClass() : nullptr;
                if (IsLiveObjectForMCP(ComponentClass) && IsPackageUnderRoot(ComponentClass, SourceRoot))
                {
                    ++OutSourceComponentCount;
                }
            }
        }
    }
}

int32 RepairLoadedWorldActorInstances(
    UWorld* LoadedWorld,
    const FString& WorldPackageName,
    const TMap<UObject*, UObject*>& ReplacementMap,
    const TArray<FPathStringReplacement>& PathReplacements,
    const FString& SourceRoot,
    int32& OutLevelActorReferenceRemapCount,
    int32& OutSavedMapCount,
    int32& OutLevelComponentClassRepairCount,
    TArray<FString>& FailedAssets,
    bool bRemapLevelBlueprint,
    bool bRepairActorClasses,
    bool bRepairComponentClasses,
    bool bRemapActorReferences,
    bool bRemoveSourceObjectMapKeys,
    bool bSaveMap)
{
    if (!LoadedWorld || ReplacementMap.IsEmpty())
    {
        return 0;
    }

    bool bWorldChanged = false;
    TArray<UObject*> WorldPackageObjects;
    GetObjectsWithPackage(LoadedWorld->GetOutermost(), WorldPackageObjects, true, RF_Transient, EInternalObjectFlags::Garbage);
    for (UObject* WorldPackageObject : WorldPackageObjects)
    {
        if (!IsLiveObjectForMCP(WorldPackageObject))
        {
            continue;
        }

        const int32 PathRemapCount = ReplaceExportedPropertyPaths(WorldPackageObject, PathReplacements, SourceRoot);
        if (PathRemapCount > 0)
        {
            OutLevelActorReferenceRemapCount += PathRemapCount;
            bWorldChanged = true;
        }

        if (!bRemapLevelBlueprint)
        {
            continue;
        }

        UBlueprint* WorldBlueprint = Cast<UBlueprint>(WorldPackageObject);
        if (WorldBlueprint)
        {
            const int32 GraphRemapCount = RemapBlueprintGraphReferences(WorldBlueprint, ReplacementMap, PathReplacements);
            if (GraphRemapCount > 0)
            {
                OutLevelActorReferenceRemapCount += GraphRemapCount;
                bWorldChanged = true;
                FBlueprintEditorUtils::RefreshAllNodes(WorldBlueprint);
                OutLevelActorReferenceRemapCount += RemapBlueprintGraphReferences(WorldBlueprint, ReplacementMap, PathReplacements);
                FBlueprintEditorUtils::MarkBlueprintAsModified(WorldBlueprint);
                FKismetEditorUtilities::CompileBlueprint(WorldBlueprint);
                if (WorldBlueprint->Status == BS_Error)
                {
                    FailedAssets.Add(WorldBlueprint->GetPathName() + TEXT(" (level blueprint compile error after reference repair)"));
                }
            }
        }
    }

    int32 RepairedActorCount = 0;
    TArray<AActor*> ActorsToRepair;
    TSet<UObject*> TargetActorInstances;
    TMap<UClass*, UClass*> OldToNewClassMap;

    if (bRepairActorClasses)
    {
        for (ULevel* Level : LoadedWorld->GetLevels())
        {
            if (!Level)
            {
                continue;
            }

            for (AActor* Actor : Level->Actors)
            {
                if (!IsLiveObjectForMCP(Actor))
                {
                    continue;
                }

                UClass* SourceClass = Actor->GetClass();
                if (!IsLiveObjectForMCP(SourceClass))
                {
                    continue;
                }

                UClass* TargetClass = Cast<UClass>(FindReplacementObject(ReplacementMap, SourceClass));
                if (!IsLiveObjectForMCP(TargetClass) || TargetClass == SourceClass || !TargetClass->IsChildOf(AActor::StaticClass()))
                {
                    continue;
                }

                ActorsToRepair.Add(Actor);
                TargetActorInstances.Add(Actor);
                OldToNewClassMap.Add(SourceClass, TargetClass);
            }
        }
    }

    if (!ActorsToRepair.IsEmpty() && !OldToNewClassMap.IsEmpty())
    {
        TSet<UObject*> InstancesThatShouldUseOldClass;
        for (TObjectIterator<UObject> It(RF_ClassDefaultObject, true, EInternalObjectFlags::Garbage); It; ++It)
        {
            UObject* Object = *It;
            if (!IsLiveObjectForMCP(Object) || TargetActorInstances.Contains(Object))
            {
                continue;
            }

            UClass* ObjectClass = Object->GetClass();
            if (IsLiveObjectForMCP(ObjectClass) && OldToNewClassMap.Contains(ObjectClass))
            {
                InstancesThatShouldUseOldClass.Add(Object);
            }
        }

        FReplaceInstancesOfClassParameters ReinstanceParams;
        ReinstanceParams.InstancesThatShouldUseOldClass = &InstancesThatShouldUseOldClass;
        ReinstanceParams.bPreserveRootComponent = true;
        ReinstanceParams.bReplaceReferencesToOldClasses = false;
        ReinstanceParams.bReplaceReferencesToOldCDOs = false;
        FBlueprintCompileReinstancer::BatchReplaceInstancesOfClass(OldToNewClassMap, ReinstanceParams);

        RepairedActorCount += ActorsToRepair.Num();
        bWorldChanged = true;
        LoadedWorld->MarkPackageDirty();
        for (ULevel* Level : LoadedWorld->GetLevels())
        {
            if (Level)
            {
                Level->MarkPackageDirty();
            }
        }
    }

    if (bRepairComponentClasses)
    {
        const int32 ComponentRepairCount = RepairTargetWorldComponentClasses(LoadedWorld, ReplacementMap, FailedAssets);
        if (ComponentRepairCount > 0)
        {
            OutLevelComponentClassRepairCount += ComponentRepairCount;
            bWorldChanged = true;
        }
    }

    if (bRemapActorReferences)
    {
        const int32 ActorInstanceReferenceRemapCount = RemapTargetWorldActorInstanceReferences(LoadedWorld, ReplacementMap);
        if (ActorInstanceReferenceRemapCount > 0)
        {
            OutLevelActorReferenceRemapCount += ActorInstanceReferenceRemapCount;
            bWorldChanged = true;
        }
    }

    if (bRemoveSourceObjectMapKeys)
    {
        const int32 RemovedSourceMapKeyCount = RemoveSourceObjectKeyMapEntriesFromWorld(LoadedWorld, SourceRoot);
        if (RemovedSourceMapKeyCount > 0)
        {
            OutLevelActorReferenceRemapCount += RemovedSourceMapKeyCount;
            bWorldChanged = true;
        }
    }

    if (!bWorldChanged)
    {
        return RepairedActorCount;
    }

    LoadedWorld->GetOutermost()->MarkPackageDirty();
    if (!bSaveMap)
    {
        return RepairedActorCount;
    }

    if (SaveWorldPackageForRepair(LoadedWorld, WorldPackageName))
    {
        ++OutSavedMapCount;
    }
    else
    {
        FailedAssets.Add(WorldPackageName + TEXT(" (failed to save target map after actor class repair)"));
    }

    return RepairedActorCount;
}

int32 RepairTargetWorldActorsWithSourceClasses(
    const TArray<FString>& TargetWorldPackageNames,
    const TMap<UObject*, UObject*>& ReplacementMap,
    const TArray<FPathStringReplacement>& PathReplacements,
    const FString& SourceRoot,
    int32& OutLevelActorReferenceRemapCount,
    int32& OutSavedMapCount,
    int32& OutLevelComponentClassRepairCount,
    TArray<FString>& FailedAssets,
    bool bRemapLevelBlueprint = true,
    bool bRepairActorClasses = true,
    bool bRepairComponentClasses = true,
    bool bRemapActorReferences = true,
    bool bRemoveSourceObjectMapKeys = true,
    bool bSaveMap = true)
{
    if (TargetWorldPackageNames.IsEmpty() || ReplacementMap.IsEmpty())
    {
        return 0;
    }

    int32 RepairedActorCount = 0;

    for (const FString& WorldPackageName : TargetWorldPackageNames)
    {
        UWorld* LoadedWorld = LoadWorldPackageForRepair(WorldPackageName);
        if (!LoadedWorld)
        {
            FailedAssets.Add(WorldPackageName + TEXT(" (failed to load target map for actor class repair)"));
            continue;
        }

        RepairedActorCount += RepairLoadedWorldActorInstances(
            LoadedWorld,
            WorldPackageName,
            ReplacementMap,
            PathReplacements,
            SourceRoot,
            OutLevelActorReferenceRemapCount,
            OutSavedMapCount,
            OutLevelComponentClassRepairCount,
            FailedAssets,
            bRemapLevelBlueprint,
            bRepairActorClasses,
            bRepairComponentClasses,
            bRemapActorReferences,
            bRemoveSourceObjectMapKeys,
            bSaveMap);
    }

    return RepairedActorCount;
}

UObject* FindReplacementObject(const TMap<UObject*, UObject*>& ReplacementMap, UObject* Object)
{
    if (!IsLiveObjectForMCP(Object))
    {
        return nullptr;
    }

    if (UObject* const* Replacement = ReplacementMap.Find(Object))
    {
        return IsLiveObjectForMCP(*Replacement) ? *Replacement : nullptr;
    }
    return nullptr;
}

bool RemapSimpleMemberReference(FSimpleMemberReference& Reference, const TMap<UObject*, UObject*>& ReplacementMap)
{
    if (UObject* ReplacementParent = FindReplacementObject(ReplacementMap, Reference.MemberParent))
    {
        Reference.MemberParent = ReplacementParent;
        return true;
    }
    return false;
}

bool RemapTerminalType(FEdGraphTerminalType& TerminalType, const TMap<UObject*, UObject*>& ReplacementMap)
{
    if (UObject* Replacement = FindReplacementObject(ReplacementMap, TerminalType.TerminalSubCategoryObject.Get(true)))
    {
        TerminalType.TerminalSubCategoryObject = Replacement;
        return true;
    }
    return false;
}

bool RemapPinType(FEdGraphPinType& PinType, const TMap<UObject*, UObject*>& ReplacementMap)
{
    bool bChanged = false;
    if (UObject* Replacement = FindReplacementObject(ReplacementMap, PinType.PinSubCategoryObject.Get(true)))
    {
        PinType.PinSubCategoryObject = Replacement;
        bChanged = true;
    }

    bChanged |= RemapSimpleMemberReference(PinType.PinSubCategoryMemberReference, ReplacementMap);
    bChanged |= RemapTerminalType(PinType.PinValueType, ReplacementMap);
    return bChanged;
}

bool RemapMemberReferenceParent(FMemberReference& Reference, const TMap<UObject*, UObject*>& ReplacementMap)
{
    if (Reference.IsSelfContext() || Reference.IsLocalScope())
    {
        return false;
    }

    UClass* ParentClass = Reference.GetMemberParentClass();
    UClass* ReplacementClass = Cast<UClass>(FindReplacementObject(ReplacementMap, ParentClass));
    if (!ReplacementClass)
    {
        return false;
    }

    Reference.SetExternalMember(Reference.GetMemberName(), ReplacementClass, Reference.GetMemberGuid());
    return true;
}

FGuid FindBlueprintSelfVariableGuid(UBlueprint* Blueprint, const FName MemberName)
{
    if (!Blueprint || MemberName.IsNone())
    {
        return FGuid();
    }

    FGuid Guid = FBlueprintEditorUtils::FindMemberVariableGuidByName(Blueprint, MemberName);
    if (Guid.IsValid())
    {
        return Guid;
    }

    if (Blueprint->SimpleConstructionScript)
    {
        const TArray<USCS_Node*>& SCSNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
        for (USCS_Node* SCSNode : SCSNodes)
        {
            if (SCSNode && SCSNode->GetVariableName() == MemberName && SCSNode->VariableGuid.IsValid())
            {
                return SCSNode->VariableGuid;
            }
        }
    }

    for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        if (Variable.VarName == MemberName && Variable.VarGuid.IsValid())
        {
            return Variable.VarGuid;
        }
    }

    for (const FBPVariableDescription& Variable : Blueprint->GeneratedVariables)
    {
        if (Variable.VarName == MemberName && Variable.VarGuid.IsValid())
        {
            return Variable.VarGuid;
        }
    }

    return FGuid();
}

FProperty* FindBlueprintPropertyByName(UBlueprint* Blueprint, const FName MemberName)
{
    if (!Blueprint || MemberName.IsNone())
    {
        return nullptr;
    }

    if (Blueprint->SkeletonGeneratedClass)
    {
        if (FProperty* Property = FindFProperty<FProperty>(Blueprint->SkeletonGeneratedClass, MemberName))
        {
            return Property;
        }
    }

    if (Blueprint->GeneratedClass)
    {
        if (FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, MemberName))
        {
            return Property;
        }
    }

    return nullptr;
}

UFunction* FindBlueprintFunctionByName(UBlueprint* Blueprint, const FName MemberName)
{
    if (!Blueprint || MemberName.IsNone())
    {
        return nullptr;
    }

    if (Blueprint->SkeletonGeneratedClass)
    {
        if (UFunction* Function = FindUField<UFunction>(Blueprint->SkeletonGeneratedClass, MemberName))
        {
            return Function;
        }
    }

    if (Blueprint->GeneratedClass)
    {
        if (UFunction* Function = FindUField<UFunction>(Blueprint->GeneratedClass, MemberName))
        {
            return Function;
        }
    }

    return nullptr;
}

bool RemapSelfMemberReference(FMemberReference& Reference, UBlueprint* Blueprint)
{
    if (!Blueprint || !Reference.IsSelfContext() || Reference.IsLocalScope())
    {
        return false;
    }

    const FName MemberName = Reference.GetMemberName();
    const FGuid Guid = FindBlueprintSelfVariableGuid(Blueprint, MemberName);
    if (Guid.IsValid() && Guid != Reference.GetMemberGuid())
    {
        Reference.SetSelfMember(MemberName, Guid);
        return true;
    }

    return false;
}

bool RemapVariableReference(UK2Node_Variable* VariableNode, UBlueprint* Blueprint, const TMap<UObject*, UObject*>& ReplacementMap)
{
    if (!VariableNode)
    {
        return false;
    }

    const FName MemberName = VariableNode->VariableReference.GetMemberName();
    if (VariableNode->VariableReference.IsLocalScope())
    {
        return false;
    }

    if (VariableNode->VariableReference.IsSelfContext())
    {
        VariableNode->Modify();
        if (FProperty* ReplacementProperty = FindBlueprintPropertyByName(Blueprint, MemberName))
        {
            VariableNode->SetFromProperty(ReplacementProperty, true, ReplacementProperty->GetOwnerClass());
            return true;
        }

        const FGuid Guid = FindBlueprintSelfVariableGuid(Blueprint, MemberName);
        if (Guid.IsValid() && Guid != VariableNode->VariableReference.GetMemberGuid())
        {
            VariableNode->VariableReference.SetSelfMember(MemberName, Guid);
            return true;
        }

        return false;
    }

    UClass* ParentClass = VariableNode->VariableReference.GetMemberParentClass();
    UClass* ReplacementClass = Cast<UClass>(FindReplacementObject(ReplacementMap, ParentClass));
    if (!ReplacementClass)
    {
        return false;
    }

    VariableNode->Modify();
    if (const FProperty* ReplacementProperty = FindFProperty<FProperty>(ReplacementClass, MemberName))
    {
        VariableNode->SetFromProperty(ReplacementProperty, false, ReplacementClass);
    }
    else
    {
        VariableNode->VariableReference.SetExternalMember(
            MemberName,
            ReplacementClass,
            VariableNode->VariableReference.GetMemberGuid());
    }
    return true;
}

bool RemapFunctionReference(UK2Node_CallFunction* FunctionNode, UBlueprint* Blueprint, const TMap<UObject*, UObject*>& ReplacementMap)
{
    if (!FunctionNode || FunctionNode->FunctionReference.IsLocalScope())
    {
        return false;
    }

    if (FunctionNode->FunctionReference.IsSelfContext())
    {
        if (const UFunction* ReplacementFunction = FindBlueprintFunctionByName(Blueprint, FunctionNode->FunctionReference.GetMemberName()))
        {
            FunctionNode->Modify();
            FunctionNode->SetFromFunction(ReplacementFunction);
            return true;
        }

        return false;
    }

    UClass* ParentClass = FunctionNode->FunctionReference.GetMemberParentClass();
    UClass* ReplacementClass = Cast<UClass>(FindReplacementObject(ReplacementMap, ParentClass));
    if (!ReplacementClass)
    {
        return false;
    }

    FunctionNode->Modify();
    if (const UFunction* ReplacementFunction = FindUField<UFunction>(ReplacementClass, FunctionNode->FunctionReference.GetMemberName()))
    {
        FunctionNode->SetFromFunction(ReplacementFunction);
    }
    else
    {
        FunctionNode->FunctionReference.SetExternalMember(
            FunctionNode->FunctionReference.GetMemberName(),
            ReplacementClass,
            FunctionNode->FunctionReference.GetMemberGuid());
    }
    return true;
}

bool RemapDelegateReference(UK2Node_BaseMCDelegate* DelegateNode, UBlueprint* Blueprint, const TMap<UObject*, UObject*>& ReplacementMap)
{
    if (!DelegateNode || DelegateNode->DelegateReference.IsLocalScope())
    {
        return false;
    }

    const FName MemberName = DelegateNode->DelegateReference.GetMemberName();
    if (DelegateNode->DelegateReference.IsSelfContext())
    {
        if (FProperty* ReplacementProperty = FindBlueprintPropertyByName(Blueprint, MemberName))
        {
            DelegateNode->Modify();
            DelegateNode->SetFromProperty(ReplacementProperty, true, ReplacementProperty->GetOwnerClass());
            return true;
        }

        return RemapSelfMemberReference(DelegateNode->DelegateReference, Blueprint);
    }

    UClass* ParentClass = DelegateNode->DelegateReference.GetMemberParentClass();
    UClass* ReplacementClass = Cast<UClass>(FindReplacementObject(ReplacementMap, ParentClass));
    if (!ReplacementClass)
    {
        return false;
    }

    DelegateNode->Modify();
    if (const FProperty* ReplacementProperty = FindFProperty<FProperty>(ReplacementClass, MemberName))
    {
        DelegateNode->SetFromProperty(ReplacementProperty, false, ReplacementClass);
    }
    else
    {
        DelegateNode->DelegateReference.SetExternalMember(MemberName, ReplacementClass, DelegateNode->DelegateReference.GetMemberGuid());
    }
    return true;
}

bool RemapEditableUserPins(UK2Node_EditablePinBase* EditableNode, const TMap<UObject*, UObject*>& ReplacementMap, const TArray<FPathStringReplacement>& PathReplacements)
{
    if (!EditableNode)
    {
        return false;
    }

    bool bChanged = false;
    for (TSharedPtr<FUserPinInfo>& UserPin : EditableNode->UserDefinedPins)
    {
        if (!UserPin.IsValid())
        {
            continue;
        }

        bChanged |= RemapPinType(UserPin->PinType, ReplacementMap);
        const FString NewDefaultValue = ApplyPathReplacements(UserPin->PinDefaultValue, PathReplacements);
        if (NewDefaultValue != UserPin->PinDefaultValue)
        {
            UserPin->PinDefaultValue = NewDefaultValue;
            bChanged = true;
        }
    }

    if (bChanged)
    {
        EditableNode->Modify();
    }
    return bChanged;
}

bool RemapFunctionEntryLocalVariables(UK2Node_FunctionEntry* FunctionEntry, const TMap<UObject*, UObject*>& ReplacementMap, const TArray<FPathStringReplacement>& PathReplacements)
{
    if (!FunctionEntry)
    {
        return false;
    }

    bool bChanged = false;
    for (FBPVariableDescription& LocalVariable : FunctionEntry->LocalVariables)
    {
        bChanged |= RemapPinType(LocalVariable.VarType, ReplacementMap);
        const FString NewDefaultValue = ApplyPathReplacements(LocalVariable.DefaultValue, PathReplacements);
        if (NewDefaultValue != LocalVariable.DefaultValue)
        {
            LocalVariable.DefaultValue = NewDefaultValue;
            bChanged = true;
        }
    }

    if (bChanged)
    {
        FunctionEntry->Modify();
    }
    return bChanged;
}

bool RemapStructOperation(UK2Node_StructOperation* StructNode, const TMap<UObject*, UObject*>& ReplacementMap)
{
    if (!StructNode || !StructNode->StructType)
    {
        return false;
    }

    UScriptStruct* ReplacementStruct = Cast<UScriptStruct>(FindReplacementObject(ReplacementMap, StructNode->StructType));
    if (!ReplacementStruct || ReplacementStruct == StructNode->StructType)
    {
        return false;
    }

    StructNode->Modify();
    StructNode->StructType = ReplacementStruct;
    return true;
}

bool RemapDynamicCastTarget(UK2Node_DynamicCast* CastNode, const TMap<UObject*, UObject*>& ReplacementMap)
{
    if (!CastNode || !CastNode->TargetType)
    {
        return false;
    }

    UClass* ReplacementClass = Cast<UClass>(FindReplacementObject(ReplacementMap, CastNode->TargetType));
    if (!ReplacementClass || ReplacementClass == CastNode->TargetType)
    {
        return false;
    }

    CastNode->Modify();
    CastNode->TargetType = ReplacementClass;
    return true;
}

bool RemapGraphPin(UEdGraphPin* Pin, const TMap<UObject*, UObject*>& ReplacementMap, const TArray<FPathStringReplacement>& PathReplacements)
{
    if (!Pin)
    {
        return false;
    }

    bool bChanged = RemapPinType(Pin->PinType, ReplacementMap);
    if (UObject* Replacement = FindReplacementObject(ReplacementMap, Pin->DefaultObject))
    {
        Pin->DefaultObject = Replacement;
        bChanged = true;
    }

    const FString NewDefaultValue = ApplyPathReplacements(Pin->DefaultValue, PathReplacements);
    if (NewDefaultValue != Pin->DefaultValue)
    {
        Pin->DefaultValue = NewDefaultValue;
        bChanged = true;
    }

    const FString NewAutogeneratedDefaultValue = ApplyPathReplacements(Pin->AutogeneratedDefaultValue, PathReplacements);
    if (NewAutogeneratedDefaultValue != Pin->AutogeneratedDefaultValue)
    {
        Pin->AutogeneratedDefaultValue = NewAutogeneratedDefaultValue;
        bChanged = true;
    }

    if (bChanged)
    {
        Pin->Modify();
    }
    return bChanged;
}

bool NodeHasSplitPins(const UEdGraphNode* Node)
{
    if (!Node)
    {
        return false;
    }

    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->SubPins.Num() > 0)
        {
            return true;
        }
    }
    return false;
}

int32 RemapBlueprintGraphReferences(UBlueprint* Blueprint, const TMap<UObject*, UObject*>& ReplacementMap, const TArray<FPathStringReplacement>& PathReplacements)
{
    if (!Blueprint || ReplacementMap.IsEmpty())
    {
        return 0;
    }

    int32 RemapCount = 0;
    bool bBlueprintChanged = false;

    for (FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        bool bVariableChanged = RemapPinType(Variable.VarType, ReplacementMap);
        const FString NewDefaultValue = ApplyPathReplacements(Variable.DefaultValue, PathReplacements);
        if (NewDefaultValue != Variable.DefaultValue)
        {
            Variable.DefaultValue = NewDefaultValue;
            bVariableChanged = true;
        }

        if (bVariableChanged)
        {
            ++RemapCount;
            bBlueprintChanged = true;
        }
    }

    for (FBPVariableDescription& Variable : Blueprint->GeneratedVariables)
    {
        bool bVariableChanged = RemapPinType(Variable.VarType, ReplacementMap);
        const FString NewDefaultValue = ApplyPathReplacements(Variable.DefaultValue, PathReplacements);
        if (NewDefaultValue != Variable.DefaultValue)
        {
            Variable.DefaultValue = NewDefaultValue;
            bVariableChanged = true;
        }

        if (bVariableChanged)
        {
            ++RemapCount;
            bBlueprintChanged = true;
        }
    }

    TArray<UEdGraph*> Graphs;
    Blueprint->GetAllGraphs(Graphs);
    TArray<UEdGraphNode*> NodesToReconstruct;
    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph)
        {
            continue;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }

            bool bNodeChanged = false;
            bool bNeedsReconstruct = false;
            const bool bHasSplitPins = NodeHasSplitPins(Node);
            const bool bIsStructOperation = Node->IsA<UK2Node_StructOperation>();
            if (UK2Node_CallFunction* FunctionNode = Cast<UK2Node_CallFunction>(Node))
            {
                const bool bFunctionChanged = RemapFunctionReference(FunctionNode, Blueprint, ReplacementMap);
                bNodeChanged |= bFunctionChanged;
                bNeedsReconstruct |= bFunctionChanged;
                if (UK2Node_CallFunctionOnMember* FunctionOnMemberNode = Cast<UK2Node_CallFunctionOnMember>(FunctionNode))
                {
                    const bool bMemberParentChanged = RemapMemberReferenceParent(FunctionOnMemberNode->MemberVariableToCallOn, ReplacementMap);
                    const bool bSelfMemberChanged = RemapSelfMemberReference(FunctionOnMemberNode->MemberVariableToCallOn, Blueprint);
                    bNodeChanged |= bMemberParentChanged || bSelfMemberChanged;
                    bNeedsReconstruct |= bMemberParentChanged || bSelfMemberChanged;
                }
            }
            if (UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
            {
                const bool bVariableChanged = RemapVariableReference(VariableNode, Blueprint, ReplacementMap);
                bNodeChanged |= bVariableChanged;
                bNeedsReconstruct |= bVariableChanged;
            }
            if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
            {
                const bool bEventParentChanged = RemapMemberReferenceParent(EventNode->EventReference, ReplacementMap);
                const bool bSelfEventChanged = RemapSelfMemberReference(EventNode->EventReference, Blueprint);
                bNodeChanged |= bEventParentChanged || bSelfEventChanged;
                bNeedsReconstruct |= bEventParentChanged || bSelfEventChanged;
            }
            if (UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(Node))
            {
                const bool bDelegateChanged = RemapDelegateReference(DelegateNode, Blueprint, ReplacementMap);
                bNodeChanged |= bDelegateChanged;
                bNeedsReconstruct |= bDelegateChanged;
            }
            if (UK2Node_FunctionTerminator* FunctionTerminator = Cast<UK2Node_FunctionTerminator>(Node))
            {
                const bool bTerminatorParentChanged = RemapMemberReferenceParent(FunctionTerminator->FunctionReference, ReplacementMap);
                const bool bSelfTerminatorChanged = RemapSelfMemberReference(FunctionTerminator->FunctionReference, Blueprint);
                bNodeChanged |= bTerminatorParentChanged || bSelfTerminatorChanged;
                bNeedsReconstruct |= bTerminatorParentChanged || bSelfTerminatorChanged;
            }
            if (UK2Node_EditablePinBase* EditableNode = Cast<UK2Node_EditablePinBase>(Node))
            {
                bNodeChanged |= RemapEditableUserPins(EditableNode, ReplacementMap, PathReplacements);
            }
            if (UK2Node_FunctionEntry* FunctionEntry = Cast<UK2Node_FunctionEntry>(Node))
            {
                bNodeChanged |= RemapFunctionEntryLocalVariables(FunctionEntry, ReplacementMap, PathReplacements);
            }
            if (UK2Node_StructOperation* StructNode = Cast<UK2Node_StructOperation>(Node))
            {
                bNodeChanged |= RemapStructOperation(StructNode, ReplacementMap);
            }
            if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
            {
                const bool bCastTargetChanged = RemapDynamicCastTarget(CastNode, ReplacementMap);
                bNodeChanged |= bCastTargetChanged;
                bNeedsReconstruct |= bCastTargetChanged;
            }

            for (UEdGraphPin* Pin : Node->Pins)
            {
                bNodeChanged |= RemapGraphPin(Pin, ReplacementMap, PathReplacements);
            }

            if (bNodeChanged)
            {
                ++RemapCount;
                bBlueprintChanged = true;
                if (bNeedsReconstruct && !bHasSplitPins && !bIsStructOperation)
                {
                    NodesToReconstruct.AddUnique(Node);
                }
            }
        }
    }

    for (UEdGraphNode* Node : NodesToReconstruct)
    {
        if (!Node)
        {
            continue;
        }

        Node->ReconstructNode();
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (RemapGraphPin(Pin, ReplacementMap, PathReplacements))
            {
                ++RemapCount;
            }
        }
    }

    if (bBlueprintChanged)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        Blueprint->MarkPackageDirty();
    }

    return RemapCount;
}

void AddPathReplacement(TArray<FPathStringReplacement>& Replacements, const FString& Source, const FString& Target)
{
    if (!Source.IsEmpty() && !Target.IsEmpty() && Source != Target)
    {
        Replacements.Add({Source, Target});
    }
}

void AddObjectPathReplacements(TArray<FPathStringReplacement>& Replacements, UObject* SourceObject, UObject* TargetObject)
{
    if (!IsValid(SourceObject) || !IsValid(TargetObject))
    {
        return;
    }

    UPackage* SourcePackage = SourceObject->GetOutermost();
    UPackage* TargetPackage = TargetObject->GetOutermost();
    if (!SourcePackage || !TargetPackage)
    {
        return;
    }

    AddPathReplacement(Replacements, SourceObject->GetPathName(), TargetObject->GetPathName());
    AddPathReplacement(Replacements, SourcePackage->GetName(), TargetPackage->GetName());

    UBlueprint* SourceBlueprint = Cast<UBlueprint>(SourceObject);
    UBlueprint* TargetBlueprint = Cast<UBlueprint>(TargetObject);
    if (SourceBlueprint && TargetBlueprint)
    {
        AddPathReplacement(Replacements,
            SourceObject->GetPathName() + TEXT("_C"),
            TargetObject->GetPathName() + TEXT("_C"));
        AddPathReplacement(Replacements,
            SourcePackage->GetName() + TEXT(".") + SourceObject->GetName() + TEXT("_C"),
            TargetPackage->GetName() + TEXT(".") + TargetObject->GetName() + TEXT("_C"));
        if (IsValid(SourceBlueprint->GeneratedClass) && IsValid(TargetBlueprint->GeneratedClass))
        {
            AddPathReplacement(Replacements, SourceBlueprint->GeneratedClass->GetPathName(), TargetBlueprint->GeneratedClass->GetPathName());
        }
        if (IsValid(SourceBlueprint->SkeletonGeneratedClass) && IsValid(TargetBlueprint->SkeletonGeneratedClass))
        {
            AddPathReplacement(Replacements, SourceBlueprint->SkeletonGeneratedClass->GetPathName(), TargetBlueprint->SkeletonGeneratedClass->GetPathName());
        }
    }
}

FString ApplyPathReplacements(FString Text, const TArray<FPathStringReplacement>& Replacements)
{
    for (const FPathStringReplacement& Replacement : Replacements)
    {
        if (Text.Contains(Replacement.Source, ESearchCase::CaseSensitive))
        {
            Text.ReplaceInline(*Replacement.Source, *Replacement.Target, ESearchCase::CaseSensitive);
        }
    }
    return Text;
}

int32 ReplaceExportedPropertyPaths(UObject* Object, const TArray<FPathStringReplacement>& Replacements, const FString& SourceRoot)
{
    if (!Object || Replacements.IsEmpty())
    {
        return 0;
    }

    int32 ReplaceCount = 0;
    bool bObjectModified = false;
    for (TFieldIterator<FProperty> PropertyIterator(Object->GetClass()); PropertyIterator; ++PropertyIterator)
    {
        FProperty* Property = *PropertyIterator;
        if (!Property || Property->HasAnyPropertyFlags(CPF_Transient))
        {
            continue;
        }

        FString ExportedValue;
        Property->ExportText_InContainer(0, ExportedValue, Object, Object, Object, PPF_SerializedAsImportText);
        if (!ExportedValue.Contains(SourceRoot, ESearchCase::CaseSensitive))
        {
            continue;
        }

        const FString ReplacedValue = ApplyPathReplacements(ExportedValue, Replacements);
        if (ReplacedValue == ExportedValue)
        {
            continue;
        }

        if (!bObjectModified)
        {
            Object->Modify();
            bObjectModified = true;
        }

        void* ValueAddress = Property->ContainerPtrToValuePtr<void>(Object);
        const TCHAR* ImportEnd = Property->ImportText_Direct(
            *ReplacedValue,
            ValueAddress,
            Object,
            PPF_SerializedAsImportText,
            GWarn);

        if (ImportEnd)
        {
            ++ReplaceCount;
        }
    }

    if (ReplaceCount > 0)
    {
        Object->MarkPackageDirty();
        if (UPackage* Package = Object->GetOutermost())
        {
            Package->MarkPackageDirty();
        }
    }
    return ReplaceCount;
}

int32 CollectOriginalDependencyPackages(
    IAssetRegistry& AssetRegistry,
    const TArray<FName>& TargetPackageNames,
    const FString& SourceRoot,
    TSet<FName>& OutPackagesWithOriginalDependencies,
    TArray<FString>& OutSamples,
    int32 MaxSamples)
{
    OutPackagesWithOriginalDependencies.Reset();
    OutSamples.Reset();

    for (const FName& TargetPackageName : TargetPackageNames)
    {
        TArray<FName> Dependencies;
        AssetRegistry.GetDependencies(TargetPackageName, Dependencies);
        bool bHasOriginalDependency = false;
        for (const FName& Dependency : Dependencies)
        {
            const FString DependencyPath = Dependency.ToString();
            if (DependencyPath.StartsWith(SourceRoot + TEXT("/")) || DependencyPath.Equals(SourceRoot))
            {
                bHasOriginalDependency = true;
                if (OutSamples.Num() < MaxSamples)
                {
                    OutSamples.Add(FString::Printf(TEXT("%s -> %s"), *TargetPackageName.ToString(), *DependencyPath));
                }
            }
        }

        if (bHasOriginalDependency)
        {
            OutPackagesWithOriginalDependencies.Add(TargetPackageName);
        }
    }

    return OutPackagesWithOriginalDependencies.Num();
}

FString BuildTargetAssetPath(const FAssetData& SourceAsset, const FString& SourceRoot, const FString& TargetRoot, const FString& Suffix)
{
    const FString SourceDirectory = SourceAsset.PackagePath.ToString();
    FString RelativeDirectory;
    if (SourceDirectory.Equals(SourceRoot))
    {
        RelativeDirectory = FString();
    }
    else if (SourceDirectory.StartsWith(SourceRoot + TEXT("/")))
    {
        RelativeDirectory = SourceDirectory.RightChop(SourceRoot.Len());
    }
    else
    {
        RelativeDirectory = TEXT("/") + FPackageName::GetShortName(SourceDirectory);
    }

    FString TargetName = SourceAsset.AssetName.ToString();
    if (!TargetName.EndsWith(Suffix))
    {
        TargetName += Suffix;
    }

    return TargetRoot + RelativeDirectory + TEXT("/") + TargetName;
}

int32 GetRecreateCreationPriority(const FAssetData& SourceAsset)
{
    const FString ClassName = SourceAsset.AssetClassPath.GetAssetName().ToString();
    if (ClassName == TEXT("UserDefinedEnum"))
    {
        return 0;
    }
    if (ClassName == TEXT("UserDefinedStruct"))
    {
        return 1;
    }
    if (ClassName == TEXT("DataTable"))
    {
        return 2;
    }
    return 10;
}

FString GetFallbackReason(const FAssetData& SourceAsset)
{
    const FString ClassName = SourceAsset.AssetClassPath.GetAssetName().ToString();
    return FString::Printf(
        TEXT("exact_behavior_requires_serialized_asset_data_for_%s"),
        *ClassName);
}

bool IsFreshPropertyCopyAssetClassName(const FString& ClassName)
{
    static const TSet<FString> FreshPropertyCopyClasses = {
        TEXT("MaterialInstanceConstant"),
        TEXT("CurveFloat"),
        TEXT("CurveLinearColor"),
        TEXT("CurveVector"),
        TEXT("TextureRenderTarget2D"),
        TEXT("TextureRenderTargetCube"),
        TEXT("TextureRenderTargetVolume"),
        TEXT("MaterialParameterCollection"),
        TEXT("NiagaraParameterCollection"),
        TEXT("AudioBus"),
        TEXT("SoundClass"),
        TEXT("SoundAttenuation"),
    };
    return FreshPropertyCopyClasses.Contains(ClassName);
}

FString GetFreshPropertyCopyReason(const FAssetData& SourceAsset)
{
    const FString ClassName = SourceAsset.AssetClassPath.GetAssetName().ToString();
    return FString::Printf(
        TEXT("fresh_new_object_property_copy_for_%s"),
        *ClassName);
}

UObject* CreateFreshPropertyCopyAsset(
    UObject* SourceObject,
    const FString& TargetPath,
    UClass* TargetClass,
    TMap<UObject*, UObject*>* OptionalReplacementMap,
    FString& OutDetail)
{
    if (!SourceObject)
    {
        OutDetail = TEXT("source asset is null");
        return nullptr;
    }

    if (!TargetClass)
    {
        OutDetail = TEXT("target class is null");
        return nullptr;
    }

    UPackage* TargetPackage = CreatePackage(*TargetPath);
    if (!TargetPackage)
    {
        OutDetail = TEXT("could not create target package");
        return nullptr;
    }

    const FName TargetObjectName(*FPackageName::GetShortName(TargetPath));
    UObject* TargetObject = NewObject<UObject>(
        TargetPackage,
        TargetClass,
        TargetObjectName,
        RF_Public | RF_Standalone | RF_Transactional);
    if (!TargetObject)
    {
        OutDetail = TEXT("could not create target object");
        return nullptr;
    }

    UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyParams;
    CopyParams.OptionalReplacementMappings = OptionalReplacementMap;
    CopyParams.bReplaceInternalReferenceUponRead = OptionalReplacementMap && OptionalReplacementMap->Num() > 0;
    UEngine::CopyPropertiesForUnrelatedObjects(SourceObject, TargetObject, CopyParams);

    TargetObject->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
    TargetObject->ClearFlags(RF_Transient);
    TargetObject->PostEditChange();
    TargetPackage->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(TargetObject);

    OutDetail = FString::Printf(TEXT("fresh_property_copy_from_%s"), *SourceObject->GetClass()->GetName());
    return TargetObject;
}

void DiscardFreshAssetForFallback(UObject* Object)
{
    if (!Object)
    {
        return;
    }

    UPackage* OriginalPackage = Object->GetOutermost();
    Object->ClearFlags(RF_Public | RF_Standalone);
    Object->SetFlags(RF_Transient);
    Object->Rename(nullptr, GetTransientPackage(), REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional);
    if (OriginalPackage && OriginalPackage != GetTransientPackage())
    {
        OriginalPackage->SetDirtyFlag(false);
    }
}

bool IsFreshMaterialGraphAssetClassName(const FString& ClassName)
{
    return ClassName == TEXT("Material") || ClassName == TEXT("MaterialFunction");
}

TConstArrayView<TObjectPtr<UMaterialExpression>> GetMaterialGraphExpressions(UObject* GraphObject)
{
    if (UMaterial* Material = Cast<UMaterial>(GraphObject))
    {
        return Material->GetExpressions();
    }
    if (UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(GraphObject))
    {
        return MaterialFunction->GetExpressions();
    }
    return TConstArrayView<TObjectPtr<UMaterialExpression>>();
}

FMaterialExpressionCollection* GetMutableMaterialGraphExpressionCollection(UObject* GraphObject)
{
    if (UMaterial* Material = Cast<UMaterial>(GraphObject))
    {
        return &Material->GetExpressionCollection();
    }
    if (UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(GraphObject))
    {
        return &MaterialFunction->GetExpressionCollection();
    }
    return nullptr;
}

const FMaterialExpressionCollection* GetMaterialGraphExpressionCollection(UObject* GraphObject)
{
    if (UMaterial* Material = Cast<UMaterial>(GraphObject))
    {
        return &Material->GetExpressionCollection();
    }
    if (UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(GraphObject))
    {
        return &MaterialFunction->GetExpressionCollection();
    }
    return nullptr;
}

bool ValidateFreshMaterialFunctionAsset(UMaterialFunction* SourceFunction, UMaterialFunction* TargetFunction, FString& OutDetail)
{
    if (!SourceFunction || !TargetFunction)
    {
        OutDetail = TEXT("source or target material function is null");
        return false;
    }

    if (SourceFunction->GetMaterialFunctionUsage() != TargetFunction->GetMaterialFunctionUsage())
    {
        OutDetail = TEXT("material function usage mismatch");
        return false;
    }

    TArray<FFunctionExpressionInput> SourceInputs;
    TArray<FFunctionExpressionOutput> SourceOutputs;
    TArray<FFunctionExpressionInput> TargetInputs;
    TArray<FFunctionExpressionOutput> TargetOutputs;
    SourceFunction->GetInputsAndOutputs(SourceInputs, SourceOutputs);
    TargetFunction->GetInputsAndOutputs(TargetInputs, TargetOutputs);

    if (SourceInputs.Num() != TargetInputs.Num() || SourceOutputs.Num() != TargetOutputs.Num())
    {
        OutDetail = FString::Printf(
            TEXT("material function io count mismatch: source_inputs=%d target_inputs=%d source_outputs=%d target_outputs=%d"),
            SourceInputs.Num(),
            TargetInputs.Num(),
            SourceOutputs.Num(),
            TargetOutputs.Num());
        return false;
    }

    for (int32 Index = 0; Index < SourceInputs.Num(); ++Index)
    {
        if (SourceInputs[Index].Input.InputName != TargetInputs[Index].Input.InputName)
        {
            OutDetail = FString::Printf(TEXT("material function input name mismatch at index %d"), Index);
            return false;
        }
    }

    for (int32 Index = 0; Index < SourceOutputs.Num(); ++Index)
    {
        if (SourceOutputs[Index].Output.OutputName != TargetOutputs[Index].Output.OutputName)
        {
            OutDetail = FString::Printf(TEXT("material function output name mismatch at index %d"), Index);
            return false;
        }
    }

    return true;
}

bool ValidateFreshMaterialGraphAsset(UObject* SourceObject, UObject* TargetObject, FString& OutDetail, int32& OutExpressionCount)
{
    const TConstArrayView<TObjectPtr<UMaterialExpression>> SourceExpressions = GetMaterialGraphExpressions(SourceObject);
    const TConstArrayView<TObjectPtr<UMaterialExpression>> TargetExpressions = GetMaterialGraphExpressions(TargetObject);
    OutExpressionCount = TargetExpressions.Num();

    if (SourceExpressions.Num() != TargetExpressions.Num())
    {
        OutDetail = FString::Printf(
            TEXT("material graph expression count mismatch: source=%d target=%d"),
            SourceExpressions.Num(),
            TargetExpressions.Num());
        return false;
    }

    for (int32 Index = 0; Index < SourceExpressions.Num(); ++Index)
    {
        UMaterialExpression* SourceExpression = SourceExpressions[Index].Get();
        UMaterialExpression* TargetExpression = TargetExpressions[Index].Get();
        if (!SourceExpression || !TargetExpression)
        {
            OutDetail = FString::Printf(TEXT("material graph expression null at index %d"), Index);
            return false;
        }
        if (SourceExpression->GetClass() != TargetExpression->GetClass())
        {
            OutDetail = FString::Printf(
                TEXT("material graph expression class mismatch at index %d: source=%s target=%s"),
                Index,
                *SourceExpression->GetClass()->GetName(),
                *TargetExpression->GetClass()->GetName());
            return false;
        }
        if (!TargetExpression->IsIn(TargetObject))
        {
            OutDetail = FString::Printf(
                TEXT("material graph expression at index %d is not outered to target asset: %s"),
                Index,
                *TargetExpression->GetPathName());
            return false;
        }
    }

    if (UMaterialFunction* SourceFunction = Cast<UMaterialFunction>(SourceObject))
    {
        if (!ValidateFreshMaterialFunctionAsset(SourceFunction, Cast<UMaterialFunction>(TargetObject), OutDetail))
        {
            return false;
        }
    }

    return true;
}

int32 AddMaterialGraphExpressionReplacementMappings(TMap<UObject*, UObject*>& ReplacementMap, UObject* SourceObject, UObject* TargetObject)
{
    const TConstArrayView<TObjectPtr<UMaterialExpression>> SourceExpressions = GetMaterialGraphExpressions(SourceObject);
    const TConstArrayView<TObjectPtr<UMaterialExpression>> TargetExpressions = GetMaterialGraphExpressions(TargetObject);
    if (SourceExpressions.Num() != TargetExpressions.Num())
    {
        return 0;
    }

    int32 AddedCount = 0;
    for (int32 Index = 0; Index < SourceExpressions.Num(); ++Index)
    {
        UMaterialExpression* SourceExpression = SourceExpressions[Index].Get();
        UMaterialExpression* TargetExpression = TargetExpressions[Index].Get();
        if (!SourceExpression || !TargetExpression || SourceExpression->GetClass() != TargetExpression->GetClass())
        {
            continue;
        }

        AddReplacementMapping(ReplacementMap, SourceExpression, TargetExpression);
        ++AddedCount;
    }
    return AddedCount;
}

void CopyFreshMaterialFunctionSettings(UMaterialFunction* SourceFunction, UMaterialFunction* TargetFunction)
{
    if (!SourceFunction || !TargetFunction)
    {
        return;
    }

    TargetFunction->Description = SourceFunction->Description;
    TargetFunction->UserExposedCaption = SourceFunction->UserExposedCaption;
    TargetFunction->bExposeToLibrary = SourceFunction->bExposeToLibrary;
    TargetFunction->bEnableNewHLSLGenerator = SourceFunction->bEnableNewHLSLGenerator;
    TargetFunction->SetMaterialFunctionUsage(SourceFunction->GetMaterialFunctionUsage());
#if WITH_EDITORONLY_DATA
    TargetFunction->LibraryCategoriesText = SourceFunction->LibraryCategoriesText;
    TargetFunction->PreviewBlendMode = SourceFunction->PreviewBlendMode;
    TargetFunction->PreviewMaterialDomain = SourceFunction->PreviewMaterialDomain;
#endif
}

bool CloneMaterialExpressionsIntoFreshGraph(UObject* SourceObject, UObject* TargetObject, FString& OutDetail)
{
    UMaterial* SourceMaterial = Cast<UMaterial>(SourceObject);
    UMaterial* TargetMaterial = Cast<UMaterial>(TargetObject);
    UMaterialFunction* TargetFunction = Cast<UMaterialFunction>(TargetObject);
    if (!SourceObject || !TargetObject || (!TargetMaterial && !TargetFunction))
    {
        OutDetail = TEXT("source or target material graph object is invalid");
        return false;
    }

    const FMaterialExpressionCollection* SourceCollection = GetMaterialGraphExpressionCollection(SourceObject);
    FMaterialExpressionCollection* TargetCollection = GetMutableMaterialGraphExpressionCollection(TargetObject);
    if (!SourceCollection || !TargetCollection)
    {
        OutDetail = TEXT("source or target material graph expression collection is invalid");
        return false;
    }

    TargetCollection->Empty();
    TMap<UMaterialExpression*, UMaterialExpression*> SourceToTargetExpressionMap;
    TMap<UObject*, UObject*> ExpressionReplacementMap;

    for (const TObjectPtr<UMaterialExpression>& SourceExpressionPtr : SourceCollection->Expressions)
    {
        UMaterialExpression* SourceExpression = SourceExpressionPtr.Get();
        if (!SourceExpression)
        {
            OutDetail = TEXT("source material graph expression is null");
            return false;
        }
        if (!SourceExpression->IsAllowedIn(TargetObject))
        {
            OutDetail = FString::Printf(
                TEXT("source material graph expression is not allowed in target asset: %s"),
                *SourceExpression->GetClass()->GetName());
            return false;
        }

        UMaterialExpression* NewExpression = Cast<UMaterialExpression>(StaticDuplicateObject(
            SourceExpression,
            TargetObject,
            NAME_None,
            RF_Transactional));
        if (!NewExpression)
        {
            OutDetail = FString::Printf(
                TEXT("could not duplicate material graph expression: %s"),
                *SourceExpression->GetPathName());
            return false;
        }

        NewExpression->Material = TargetMaterial;
        NewExpression->Function = TargetFunction;
        TargetCollection->AddExpression(NewExpression);
        SourceToTargetExpressionMap.Add(SourceExpression, NewExpression);
        ExpressionReplacementMap.Add(SourceExpression, NewExpression);
    }

    for (const TObjectPtr<UMaterialExpression>& TargetExpressionPtr : TargetCollection->Expressions)
    {
        UMaterialExpression* NewExpression = TargetExpressionPtr.Get();
        if (!NewExpression)
        {
            continue;
        }

        for (FExpressionInputIterator It{ NewExpression }; It; ++It)
        {
            if (UMaterialExpression* InputExpression = It->Expression)
            {
                if (UMaterialExpression** NewInputExpression = SourceToTargetExpressionMap.Find(InputExpression))
                {
                    It->Expression = *NewInputExpression;
                }
                else if (InputExpression->IsIn(SourceObject))
                {
                    It->Expression = nullptr;
                }
            }
        }

        if (UMaterialExpressionNamedRerouteUsage* NewNamedRerouteUsage = Cast<UMaterialExpressionNamedRerouteUsage>(NewExpression))
        {
            UMaterialExpressionNamedRerouteDeclaration* SourceDeclaration = NewNamedRerouteUsage->Declaration;
            if (SourceDeclaration)
            {
                if (UMaterialExpression** NewDeclaration = SourceToTargetExpressionMap.Find(SourceDeclaration))
                {
                    NewNamedRerouteUsage->Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(*NewDeclaration);
                }
                else if (SourceDeclaration->IsIn(SourceObject))
                {
                    NewNamedRerouteUsage->Declaration = nullptr;
                }
            }
        }
    }

    if (SourceMaterial && TargetMaterial)
    {
        for (int32 PropertyIndex = 0; PropertyIndex < static_cast<int32>(MP_MAX); ++PropertyIndex)
        {
            FExpressionInput* SourceInput = SourceMaterial->GetExpressionInputForProperty(static_cast<EMaterialProperty>(PropertyIndex));
            FExpressionInput* TargetInput = TargetMaterial->GetExpressionInputForProperty(static_cast<EMaterialProperty>(PropertyIndex));
            if (!SourceInput || !TargetInput)
            {
                continue;
            }

            *TargetInput = *SourceInput;
            if (TargetInput->Expression)
            {
                if (UMaterialExpression** NewInputExpression = SourceToTargetExpressionMap.Find(TargetInput->Expression))
                {
                    TargetInput->Expression = *NewInputExpression;
                }
                else if (TargetInput->Expression->IsIn(SourceObject))
                {
                    TargetInput->Expression = nullptr;
                }
            }
        }
    }

    for (const TObjectPtr<UMaterialExpressionComment>& SourceCommentPtr : SourceCollection->EditorComments)
    {
        UMaterialExpressionComment* SourceComment = SourceCommentPtr.Get();
        if (!SourceComment)
        {
            continue;
        }

        UMaterialExpressionComment* NewComment = Cast<UMaterialExpressionComment>(StaticDuplicateObject(
            SourceComment,
            TargetObject,
            NAME_None,
            RF_Transactional));
        if (!NewComment)
        {
            OutDetail = FString::Printf(
                TEXT("could not duplicate material comment: %s"),
                *SourceComment->GetPathName());
            return false;
        }

        NewComment->Material = TargetMaterial;
        NewComment->Function = TargetFunction;
        TargetCollection->AddComment(NewComment);
    }

    if (ExpressionReplacementMap.Num() > 0)
    {
        TargetObject->Modify();
        FArchiveReplaceObjectRef<UObject> ReplaceAr(
            TargetObject,
            ExpressionReplacementMap,
            EArchiveReplaceObjectFlags::IgnoreOuterRef |
            EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
        if (ReplaceAr.GetCount() > 0)
        {
            TargetObject->MarkPackageDirty();
        }
    }

    return true;
}

UObject* CreateFreshMaterialGraphAsset(
    UObject* SourceObject,
    const FString& TargetPath,
    TMap<UObject*, UObject*>* OptionalReplacementMap,
    FString& OutDetail)
{
    const bool bIsMaterial = Cast<UMaterial>(SourceObject) != nullptr;
    const bool bIsMaterialFunction = Cast<UMaterialFunction>(SourceObject) != nullptr;
    if (!bIsMaterial && !bIsMaterialFunction)
    {
        OutDetail = TEXT("source asset is not UMaterial or UMaterialFunction");
        return nullptr;
    }

    UPackage* TargetPackage = CreatePackage(*TargetPath);
    if (!TargetPackage)
    {
        OutDetail = TEXT("could not create target material graph package");
        return nullptr;
    }

    const FName TargetObjectName(*FPackageName::GetShortName(TargetPath));
    UObject* TargetObject = NewObject<UObject>(
        TargetPackage,
        SourceObject->GetClass(),
        TargetObjectName,
        RF_Public | RF_Standalone | RF_Transactional);
    if (!TargetObject)
    {
        OutDetail = TEXT("could not create target material graph object");
        return nullptr;
    }

    TargetObject->PreEditChange(nullptr);

    if (UMaterial* SourceMaterial = Cast<UMaterial>(SourceObject))
    {
        UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyParams;
        CopyParams.OptionalReplacementMappings = OptionalReplacementMap;
        CopyParams.bReplaceInternalReferenceUponRead = OptionalReplacementMap && OptionalReplacementMap->Num() > 0;
        UEngine::CopyPropertiesForUnrelatedObjects(SourceMaterial, TargetObject, CopyParams);

        if (!CloneMaterialExpressionsIntoFreshGraph(SourceMaterial, TargetObject, OutDetail))
        {
            DiscardFreshAssetForFallback(TargetObject);
            return nullptr;
        }
    }
    else if (UMaterialFunction* SourceFunction = Cast<UMaterialFunction>(SourceObject))
    {
        UMaterialFunction* TargetFunction = Cast<UMaterialFunction>(TargetObject);
        if (!TargetFunction || !TargetFunction->GetEditorOnlyData())
        {
            OutDetail = TEXT("target material function editor-only data is invalid");
            DiscardFreshAssetForFallback(TargetObject);
            return nullptr;
        }

        CopyFreshMaterialFunctionSettings(SourceFunction, TargetFunction);
        if (!CloneMaterialExpressionsIntoFreshGraph(SourceFunction, TargetFunction, OutDetail))
        {
            DiscardFreshAssetForFallback(TargetObject);
            return nullptr;
        }
    }

    TargetObject->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
    TargetObject->ClearFlags(RF_Transient);
    TargetObject->PostEditChange();

    if (UMaterial* TargetMaterial = Cast<UMaterial>(TargetObject))
    {
        TargetMaterial->UpdateCachedExpressionData();
        TargetMaterial->BuildEditorParameterList();
        TargetMaterial->ForceRecompileForRendering(EMaterialShaderPrecompileMode::None);
    }
    else if (UMaterialFunction* TargetFunction = Cast<UMaterialFunction>(TargetObject))
    {
        FMaterialUpdateContext MaterialUpdateContext(FMaterialUpdateContext::EOptions::Default);
        TargetFunction->UpdateFromFunctionResource();
        TargetFunction->UpdateInputOutputTypes();
        TargetFunction->UpdateDependentFunctionCandidates();
        TargetFunction->ForceRecompileForRendering(MaterialUpdateContext, nullptr);
    }

    int32 ExpressionCount = 0;
    if (!ValidateFreshMaterialGraphAsset(SourceObject, TargetObject, OutDetail, ExpressionCount))
    {
        DiscardFreshAssetForFallback(TargetObject);
        return nullptr;
    }

    TargetPackage->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(TargetObject);

    OutDetail = FString::Printf(
        TEXT("fresh_material_graph_copy_from_%s_expressions_%d"),
        *SourceObject->GetName(),
        ExpressionCount);
    return TargetObject;
}

bool ValidateFreshSoundWaveAsset(USoundWave* SourceSound, USoundWave* TargetSound, FString& OutDetail)
{
    if (!SourceSound || !TargetSound)
    {
        OutDetail = TEXT("source or target sound wave is null");
        return false;
    }

    if (!TargetSound->RawData.HasPayloadData())
    {
        OutDetail = TEXT("target sound wave has no editor raw payload");
        return false;
    }

    TArray<uint8> SourceRawPCMData;
    uint32 SourceSampleRate = 0;
    uint16 SourceNumChannels = 0;
    if (!SourceSound->GetImportedSoundWaveData(SourceRawPCMData, SourceSampleRate, SourceNumChannels))
    {
        OutDetail = TEXT("could not read source imported sound wave data");
        return false;
    }

    TArray<uint8> TargetRawPCMData;
    uint32 TargetSampleRate = 0;
    uint16 TargetNumChannels = 0;
    if (!TargetSound->GetImportedSoundWaveData(TargetRawPCMData, TargetSampleRate, TargetNumChannels))
    {
        OutDetail = TEXT("could not read target imported sound wave data");
        return false;
    }

    if (SourceSampleRate != TargetSampleRate || SourceNumChannels != TargetNumChannels)
    {
        OutDetail = FString::Printf(
            TEXT("sound wave imported metadata mismatch: source_rate=%u target_rate=%u source_channels=%u target_channels=%u"),
            SourceSampleRate,
            TargetSampleRate,
            SourceNumChannels,
            TargetNumChannels);
        return false;
    }

    if (SourceRawPCMData.Num() != TargetRawPCMData.Num())
    {
        OutDetail = FString::Printf(
            TEXT("sound wave imported pcm size mismatch: source=%d target=%d"),
            SourceRawPCMData.Num(),
            TargetRawPCMData.Num());
        return false;
    }

    if (SourceRawPCMData != TargetRawPCMData)
    {
        OutDetail = TEXT("sound wave imported pcm data mismatch");
        return false;
    }

    if (!FMath::IsNearlyEqual(SourceSound->Duration, TargetSound->Duration, 0.001f))
    {
        OutDetail = FString::Printf(
            TEXT("sound wave duration mismatch: source=%.6f target=%.6f"),
            SourceSound->Duration,
            TargetSound->Duration);
        return false;
    }

    if (!FMath::IsNearlyEqual(SourceSound->GetSampleRateForCurrentPlatform(), TargetSound->GetSampleRateForCurrentPlatform(), 0.1f) ||
        SourceSound->NumChannels != TargetSound->NumChannels)
    {
        OutDetail = FString::Printf(
            TEXT("sound wave runtime metadata mismatch: source_rate=%.3f target_rate=%.3f source_channels=%d target_channels=%d"),
            SourceSound->GetSampleRateForCurrentPlatform(),
            TargetSound->GetSampleRateForCurrentPlatform(),
            SourceSound->NumChannels,
            TargetSound->NumChannels);
        return false;
    }

    if (SourceSound->IsLooping() != TargetSound->IsLooping())
    {
        OutDetail = TEXT("sound wave looping flag mismatch");
        return false;
    }

    return true;
}

UObject* CreateFreshSoundWaveAsset(
    UObject* SourceObject,
    const FString& TargetPath,
    TMap<UObject*, UObject*>* OptionalReplacementMap,
    FString& OutDetail)
{
    USoundWave* SourceSound = Cast<USoundWave>(SourceObject);
    if (!SourceSound)
    {
        OutDetail = TEXT("source asset is not USoundWave");
        return nullptr;
    }

    if (!SourceSound->RawData.HasPayloadData())
    {
        OutDetail = TEXT("source sound wave has no editor raw payload");
        return nullptr;
    }

    TArray<uint8> SourceRawPCMData;
    uint32 SourceSampleRate = 0;
    uint16 SourceNumChannels = 0;
    if (!SourceSound->GetImportedSoundWaveData(SourceRawPCMData, SourceSampleRate, SourceNumChannels))
    {
        OutDetail = TEXT("could not read source imported sound wave data");
        return nullptr;
    }

    UPackage* TargetPackage = CreatePackage(*TargetPath);
    if (!TargetPackage)
    {
        OutDetail = TEXT("could not create target sound wave package");
        return nullptr;
    }

    const FName TargetObjectName(*FPackageName::GetShortName(TargetPath));
    UObject* TargetObject = NewObject<UObject>(
        TargetPackage,
        SourceObject->GetClass(),
        TargetObjectName,
        RF_Public | RF_Standalone | RF_Transactional);
    USoundWave* TargetSound = Cast<USoundWave>(TargetObject);
    if (!TargetSound)
    {
        OutDetail = TEXT("could not create target sound wave object");
        return nullptr;
    }

    TargetSound->PreEditChange(nullptr);

    UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyParams;
    CopyParams.OptionalReplacementMappings = OptionalReplacementMap;
    CopyParams.bReplaceInternalReferenceUponRead = OptionalReplacementMap && OptionalReplacementMap->Num() > 0;
    UEngine::CopyPropertiesForUnrelatedObjects(SourceSound, TargetSound, CopyParams);

    TFuture<FSharedBuffer> SourcePayloadFuture = SourceSound->RawData.GetPayload();
    const FSharedBuffer SourcePayload = SourcePayloadFuture.Get();
    if (SourcePayload.GetSize() <= 0)
    {
        OutDetail = TEXT("source sound wave raw payload is empty");
        DiscardFreshAssetForFallback(TargetSound);
        return nullptr;
    }

    TargetSound->RawData.UpdatePayload(FSharedBuffer::Clone(SourcePayload.GetData(), SourcePayload.GetSize()), TargetSound);
    TargetSound->SetImportedSampleRate(SourceSound->GetImportedSampleRate());
    if (SourceSampleRate > 0)
    {
        TargetSound->SetSampleRate(SourceSampleRate);
    }

    TargetSound->InvalidateCompressedData(true, true);
    TargetSound->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
    TargetSound->ClearFlags(RF_Transient);
    TargetSound->PostEditChange();
    FinishCompilationForObjectForMCP(TargetSound);

    if (!ValidateFreshSoundWaveAsset(SourceSound, TargetSound, OutDetail))
    {
        DiscardFreshAssetForFallback(TargetSound);
        return nullptr;
    }

    TargetPackage->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(TargetSound);

    OutDetail = FString::Printf(
        TEXT("fresh_sound_wave_payload_copy_from_%s_bytes_%lld_pcm_%d_rate_%u_channels_%u"),
        *SourceSound->GetName(),
        static_cast<int64>(SourcePayload.GetSize()),
        SourceRawPCMData.Num(),
        SourceSampleRate,
        SourceNumChannels);
    return TargetSound;
}

bool ValidateFreshTextureSourceCopy(
    UTexture* SourceTexture,
    UTexture* TargetTexture,
    const FString& DetailPrefix,
    FString& OutDetail)
{
    if (!SourceTexture || !TargetTexture)
    {
        OutDetail = TEXT("source or target texture is null");
        return false;
    }

#if WITH_EDITORONLY_DATA
    FTextureSource& Source = SourceTexture->Source;
    FTextureSource& Target = TargetTexture->Source;
    if (!Source.IsValid() || !Target.IsValid())
    {
        OutDetail = TEXT("source or target texture has no valid editor source data");
        return false;
    }

    if (SourceTexture->SRGB != TargetTexture->SRGB)
    {
        OutDetail = FString::Printf(
            TEXT("texture SRGB mismatch: source=%s target=%s"),
            SourceTexture->SRGB ? TEXT("true") : TEXT("false"),
            TargetTexture->SRGB ? TEXT("true") : TEXT("false"));
        return false;
    }

    if (Source.GetSizeX() != Target.GetSizeX() ||
        Source.GetSizeY() != Target.GetSizeY() ||
        Source.GetNumSlices() != Target.GetNumSlices() ||
        Source.GetNumMips() != Target.GetNumMips() ||
        Source.GetNumLayers() != Target.GetNumLayers() ||
        Source.GetNumBlocks() != Target.GetNumBlocks())
    {
        OutDetail = FString::Printf(
            TEXT("texture source shape mismatch: source=%lldx%lld slices=%d mips=%d layers=%d blocks=%d target=%lldx%lld slices=%d mips=%d layers=%d blocks=%d"),
            static_cast<int64>(Source.GetSizeX()),
            static_cast<int64>(Source.GetSizeY()),
            Source.GetNumSlices(),
            Source.GetNumMips(),
            Source.GetNumLayers(),
            Source.GetNumBlocks(),
            static_cast<int64>(Target.GetSizeX()),
            static_cast<int64>(Target.GetSizeY()),
            Target.GetNumSlices(),
            Target.GetNumMips(),
            Target.GetNumLayers(),
            Target.GetNumBlocks());
        return false;
    }

    for (int32 LayerIndex = 0; LayerIndex < Source.GetNumLayers(); ++LayerIndex)
    {
        if (Source.GetFormat(LayerIndex) != Target.GetFormat(LayerIndex))
        {
            OutDetail = FString::Printf(TEXT("texture source layer format mismatch at layer %d"), LayerIndex);
            return false;
        }
    }

    for (int32 BlockIndex = 0; BlockIndex < Source.GetNumBlocks(); ++BlockIndex)
    {
        FTextureSourceBlock SourceBlock;
        FTextureSourceBlock TargetBlock;
        Source.GetBlock(BlockIndex, SourceBlock);
        Target.GetBlock(BlockIndex, TargetBlock);
        if (SourceBlock.BlockX != TargetBlock.BlockX ||
            SourceBlock.BlockY != TargetBlock.BlockY ||
            SourceBlock.SizeX != TargetBlock.SizeX ||
            SourceBlock.SizeY != TargetBlock.SizeY ||
            SourceBlock.NumSlices != TargetBlock.NumSlices ||
            SourceBlock.NumMips != TargetBlock.NumMips)
        {
            OutDetail = FString::Printf(TEXT("texture source block mismatch at block %d"), BlockIndex);
            return false;
        }

        for (int32 LayerIndex = 0; LayerIndex < Source.GetNumLayers(); ++LayerIndex)
        {
            for (int32 MipIndex = 0; MipIndex < SourceBlock.NumMips; ++MipIndex)
            {
                TArray64<uint8> SourceMipData;
                TArray64<uint8> TargetMipData;
                if (!Source.GetMipData(SourceMipData, BlockIndex, LayerIndex, MipIndex) ||
                    !Target.GetMipData(TargetMipData, BlockIndex, LayerIndex, MipIndex))
                {
                    OutDetail = FString::Printf(
                        TEXT("texture source mip data read failed at block=%d layer=%d mip=%d"),
                        BlockIndex,
                        LayerIndex,
                        MipIndex);
                    return false;
                }

                if (SourceMipData.Num() != TargetMipData.Num() || SourceMipData != TargetMipData)
                {
                    OutDetail = FString::Printf(
                        TEXT("texture source mip data mismatch at block=%d layer=%d mip=%d source_bytes=%lld target_bytes=%lld"),
                        BlockIndex,
                        LayerIndex,
                        MipIndex,
                        SourceMipData.Num(),
                        TargetMipData.Num());
                    return false;
                }
            }
        }
    }

    OutDetail = FString::Printf(
        TEXT("%s_from_%s_%lldx%lld_mips_%d_slices_%d_layers_%d_blocks_%d"),
        *DetailPrefix,
        *SourceTexture->GetName(),
        static_cast<int64>(Source.GetSizeX()),
        static_cast<int64>(Source.GetSizeY()),
        Source.GetNumMips(),
        Source.GetNumSlices(),
        Source.GetNumLayers(),
        Source.GetNumBlocks());
    return true;
#else
    OutDetail = TEXT("texture source validation requires editor source data");
    return false;
#endif
}

UObject* CreateFreshTextureCubeAsset(
    UObject* SourceObject,
    const FString& TargetPath,
    TMap<UObject*, UObject*>* OptionalReplacementMap,
    FString& OutDetail)
{
    UTextureCube* SourceTexture = Cast<UTextureCube>(SourceObject);
    if (!SourceTexture)
    {
        OutDetail = TEXT("source asset is not UTextureCube");
        return nullptr;
    }
    SourceTexture->BlockOnAnyAsyncBuild();

#if WITH_EDITORONLY_DATA
    if (!SourceTexture->Source.IsValid())
    {
        OutDetail = TEXT("source texture cube has no valid editor source data");
        return nullptr;
    }
#else
    OutDetail = TEXT("texture cube source recreation requires editor source data");
    return nullptr;
#endif

    UPackage* TargetPackage = CreatePackage(*TargetPath);
    if (!TargetPackage)
    {
        OutDetail = TEXT("could not create target texture cube package");
        return nullptr;
    }

    const FName TargetObjectName(*FPackageName::GetShortName(TargetPath));
    UTextureCube* TargetTexture = NewObject<UTextureCube>(
        TargetPackage,
        UTextureCube::StaticClass(),
        TargetObjectName,
        RF_Public | RF_Standalone | RF_Transactional);
    if (!TargetTexture)
    {
        OutDetail = TEXT("could not create target texture cube object");
        return nullptr;
    }

    TargetTexture->PreEditChange(nullptr);

    UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyParams;
    CopyParams.OptionalReplacementMappings = OptionalReplacementMap;
    CopyParams.bReplaceInternalReferenceUponRead = OptionalReplacementMap && OptionalReplacementMap->Num() > 0;
    UEngine::CopyPropertiesForUnrelatedObjects(SourceTexture, TargetTexture, CopyParams);

#if WITH_EDITORONLY_DATA
    TargetTexture->Source = SourceTexture->Source.CopyTornOff();
    TargetTexture->Source.SetOwner(TargetTexture);
#endif

    TargetTexture->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
    TargetTexture->ClearFlags(RF_Transient);
    TargetTexture->PostEditChange();
    TargetTexture->BlockOnAnyAsyncBuild();

    FString ValidationDetail;
    if (!ValidateFreshTextureSourceCopy(SourceTexture, TargetTexture, TEXT("fresh_texturecube_source_copy"), ValidationDetail))
    {
        OutDetail = ValidationDetail;
        DiscardFreshAssetForFallback(TargetTexture);
        return nullptr;
    }

    TargetPackage->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(TargetTexture);

    OutDetail = ValidationDetail;
    return TargetTexture;
}

UObject* CreateFreshVolumeTextureAsset(
    UObject* SourceObject,
    const FString& TargetPath,
    TMap<UObject*, UObject*>* OptionalReplacementMap,
    FString& OutDetail)
{
    UVolumeTexture* SourceTexture = Cast<UVolumeTexture>(SourceObject);
    if (!SourceTexture)
    {
        OutDetail = TEXT("source asset is not UVolumeTexture");
        return nullptr;
    }
    SourceTexture->BlockOnAnyAsyncBuild();

#if WITH_EDITORONLY_DATA
    if (!SourceTexture->Source.IsValid())
    {
        OutDetail = TEXT("source volume texture has no valid editor source data");
        return nullptr;
    }
#else
    OutDetail = TEXT("volume texture source recreation requires editor source data");
    return nullptr;
#endif

    UPackage* TargetPackage = CreatePackage(*TargetPath);
    if (!TargetPackage)
    {
        OutDetail = TEXT("could not create target volume texture package");
        return nullptr;
    }

    const FName TargetObjectName(*FPackageName::GetShortName(TargetPath));
    UVolumeTexture* TargetTexture = NewObject<UVolumeTexture>(
        TargetPackage,
        UVolumeTexture::StaticClass(),
        TargetObjectName,
        RF_Public | RF_Standalone | RF_Transactional);
    if (!TargetTexture)
    {
        OutDetail = TEXT("could not create target volume texture object");
        return nullptr;
    }

    TargetTexture->PreEditChange(nullptr);

    UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyParams;
    CopyParams.OptionalReplacementMappings = OptionalReplacementMap;
    CopyParams.bReplaceInternalReferenceUponRead = OptionalReplacementMap && OptionalReplacementMap->Num() > 0;
    UEngine::CopyPropertiesForUnrelatedObjects(SourceTexture, TargetTexture, CopyParams);

#if WITH_EDITORONLY_DATA
    TargetTexture->Source = SourceTexture->Source.CopyTornOff();
    TargetTexture->Source.SetOwner(TargetTexture);
#endif

    TargetTexture->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
    TargetTexture->ClearFlags(RF_Transient);
    TargetTexture->PostEditChange();
    TargetTexture->BlockOnAnyAsyncBuild();

    FString ValidationDetail;
    if (!ValidateFreshTextureSourceCopy(SourceTexture, TargetTexture, TEXT("fresh_volumetexture_source_copy"), ValidationDetail))
    {
        OutDetail = ValidationDetail;
        DiscardFreshAssetForFallback(TargetTexture);
        return nullptr;
    }

    TargetPackage->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(TargetTexture);

    OutDetail = ValidationDetail;
    return TargetTexture;
}

UObject* CreateFreshTexture2DAsset(
    UObject* SourceObject,
    const FString& TargetPath,
    TMap<UObject*, UObject*>* OptionalReplacementMap,
    FString& OutDetail)
{
    UTexture2D* SourceTexture = Cast<UTexture2D>(SourceObject);
    if (!SourceTexture)
    {
        OutDetail = TEXT("source asset is not UTexture2D");
        return nullptr;
    }
    SourceTexture->BlockOnAnyAsyncBuild();

#if WITH_EDITORONLY_DATA
    if (!SourceTexture->Source.IsValid())
    {
        OutDetail = TEXT("source texture has no valid editor source data");
        return nullptr;
    }
#else
    OutDetail = TEXT("texture source recreation requires editor source data");
    return nullptr;
#endif

    UPackage* TargetPackage = CreatePackage(*TargetPath);
    if (!TargetPackage)
    {
        OutDetail = TEXT("could not create target texture package");
        return nullptr;
    }

    const FName TargetObjectName(*FPackageName::GetShortName(TargetPath));
    UTexture2D* TargetTexture = NewObject<UTexture2D>(
        TargetPackage,
        UTexture2D::StaticClass(),
        TargetObjectName,
        RF_Public | RF_Standalone | RF_Transactional);
    if (!TargetTexture)
    {
        OutDetail = TEXT("could not create target texture object");
        return nullptr;
    }

    TargetTexture->PreEditChange(nullptr);

    UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyParams;
    CopyParams.OptionalReplacementMappings = OptionalReplacementMap;
    CopyParams.bReplaceInternalReferenceUponRead = OptionalReplacementMap && OptionalReplacementMap->Num() > 0;
    UEngine::CopyPropertiesForUnrelatedObjects(SourceTexture, TargetTexture, CopyParams);

#if WITH_EDITORONLY_DATA
    TargetTexture->Source = SourceTexture->Source.CopyTornOff();
    TargetTexture->Source.SetOwner(TargetTexture);
#endif

    TargetTexture->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
    TargetTexture->ClearFlags(RF_Transient);
    TargetTexture->PostEditChange();
    TargetTexture->BlockOnAnyAsyncBuild();

    FString ValidationDetail;
    if (!ValidateFreshTextureSourceCopy(SourceTexture, TargetTexture, TEXT("fresh_texture2d_source_copy"), ValidationDetail))
    {
        OutDetail = ValidationDetail;
        DiscardFreshAssetForFallback(TargetTexture);
        return nullptr;
    }

    TargetPackage->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(TargetTexture);

    OutDetail = ValidationDetail;
    return TargetTexture;
}

bool ArePerPlatformFloatsEqualForMCP(const FPerPlatformFloat& A, const FPerPlatformFloat& B)
{
    if (!FMath::IsNearlyEqual(A.Default, B.Default))
    {
        return false;
    }
#if WITH_EDITORONLY_DATA
    if (A.PerPlatform.Num() != B.PerPlatform.Num())
    {
        return false;
    }
    for (const TPair<FName, float>& Pair : A.PerPlatform)
    {
        const float* OtherValue = B.PerPlatform.Find(Pair.Key);
        if (!OtherValue || !FMath::IsNearlyEqual(Pair.Value, *OtherValue))
        {
            return false;
        }
    }
#endif
    return true;
}

UObject* ResolveReplacementOrOriginalForMCP(const TMap<UObject*, UObject*>* OptionalReplacementMap, UObject* SourceObject)
{
    if (!SourceObject)
    {
        return nullptr;
    }
    if (OptionalReplacementMap)
    {
        if (UObject* Replacement = FindReplacementObject(*OptionalReplacementMap, SourceObject))
        {
            return Replacement;
        }
    }
    return SourceObject;
}

UMaterialInterface* ResolveMaterialReplacementForMCP(
    const TMap<UObject*, UObject*>* OptionalReplacementMap,
    UMaterialInterface* SourceMaterial)
{
    return Cast<UMaterialInterface>(ResolveReplacementOrOriginalForMCP(OptionalReplacementMap, SourceMaterial));
}

UStaticMesh* ResolveStaticMeshReplacementForMCP(
    const TMap<UObject*, UObject*>* OptionalReplacementMap,
    UStaticMesh* SourceMesh)
{
    return Cast<UStaticMesh>(ResolveReplacementOrOriginalForMCP(OptionalReplacementMap, SourceMesh));
}

void FinishCompilationForObjectForMCP(UObject* Object)
{
    if (!Object)
    {
        return;
    }

    TArray<UObject*> Objects;
    Objects.Add(Object);
    FAssetCompilingManager::Get().FinishCompilationForObjects(Objects);
}

bool CompareStaticMeshDescriptionShapeForMCP(
    const FMeshDescription& SourceDescription,
    const FMeshDescription& TargetDescription,
    int32 LodIndex,
    FString& OutDetail)
{
    if (SourceDescription.Vertices().Num() != TargetDescription.Vertices().Num() ||
        SourceDescription.VertexInstances().Num() != TargetDescription.VertexInstances().Num() ||
        SourceDescription.Edges().Num() != TargetDescription.Edges().Num() ||
        SourceDescription.Triangles().Num() != TargetDescription.Triangles().Num() ||
        SourceDescription.Polygons().Num() != TargetDescription.Polygons().Num() ||
        SourceDescription.PolygonGroups().Num() != TargetDescription.PolygonGroups().Num())
    {
        OutDetail = FString::Printf(
            TEXT("static mesh LOD %d mesh description mismatch: source vertices=%d instances=%d edges=%d triangles=%d polygons=%d groups=%d target vertices=%d instances=%d edges=%d triangles=%d polygons=%d groups=%d"),
            LodIndex,
            SourceDescription.Vertices().Num(),
            SourceDescription.VertexInstances().Num(),
            SourceDescription.Edges().Num(),
            SourceDescription.Triangles().Num(),
            SourceDescription.Polygons().Num(),
            SourceDescription.PolygonGroups().Num(),
            TargetDescription.Vertices().Num(),
            TargetDescription.VertexInstances().Num(),
            TargetDescription.Edges().Num(),
            TargetDescription.Triangles().Num(),
            TargetDescription.Polygons().Num(),
            TargetDescription.PolygonGroups().Num());
        return false;
    }
    return true;
}

bool GetStaticMeshDescriptionForFreshCopyForMCP(
    UStaticMesh* Mesh,
    int32 LodIndex,
    FMeshDescription& OutDescription,
    bool& bOutUsedRenderExport,
    FString& OutDetail)
{
    bOutUsedRenderExport = false;
    if (!Mesh)
    {
        OutDetail = TEXT("static mesh is null");
        return false;
    }

    if (Mesh->CloneMeshDescription(LodIndex, OutDescription))
    {
        return true;
    }

    if (LodIndex <= 0 || LodIndex >= Mesh->GetNumLODs() || !Mesh->HasValidRenderData(true, LodIndex))
    {
        OutDetail = FString::Printf(TEXT("could not clone source static mesh description at LOD %d"), LodIndex);
        return false;
    }

    Mesh->ExportStaticMeshLOD(Mesh->GetLODForExport(LodIndex), OutDescription);
    if (OutDescription.Vertices().Num() <= 0 || OutDescription.Triangles().Num() <= 0)
    {
        OutDetail = FString::Printf(TEXT("could not export source static mesh render LOD %d to mesh description"), LodIndex);
        return false;
    }

    bOutUsedRenderExport = true;
    return true;
}

bool CompareStaticMeshRenderStatsForMCP(UStaticMesh* SourceMesh, UStaticMesh* TargetMesh, FString& OutDetail)
{
    const int32 SourceRenderLODCount = SourceMesh->GetNumLODs();
    const int32 TargetRenderLODCount = TargetMesh->GetNumLODs();
    if (SourceRenderLODCount != TargetRenderLODCount)
    {
        OutDetail = FString::Printf(TEXT("static mesh render LOD count mismatch: source=%d target=%d"), SourceRenderLODCount, TargetRenderLODCount);
        return false;
    }

    for (int32 LodIndex = 0; LodIndex < SourceRenderLODCount; ++LodIndex)
    {
        if (SourceMesh->GetNumVertices(LodIndex) != TargetMesh->GetNumVertices(LodIndex) ||
            SourceMesh->GetNumTriangles(LodIndex) != TargetMesh->GetNumTriangles(LodIndex) ||
            SourceMesh->GetNumSections(LodIndex) != TargetMesh->GetNumSections(LodIndex) ||
            SourceMesh->GetNumUVChannels(LodIndex) != TargetMesh->GetNumUVChannels(LodIndex))
        {
            OutDetail = FString::Printf(
                TEXT("static mesh render LOD %d mismatch: source vertices=%d triangles=%d sections=%d uv=%d target vertices=%d triangles=%d sections=%d uv=%d"),
                LodIndex,
                SourceMesh->GetNumVertices(LodIndex),
                SourceMesh->GetNumTriangles(LodIndex),
                SourceMesh->GetNumSections(LodIndex),
                SourceMesh->GetNumUVChannels(LodIndex),
                TargetMesh->GetNumVertices(LodIndex),
                TargetMesh->GetNumTriangles(LodIndex),
                TargetMesh->GetNumSections(LodIndex),
                TargetMesh->GetNumUVChannels(LodIndex));
            return false;
        }
    }

    return true;
}

bool CompareStaticMeshSectionInfoMapForMCP(
    const FMeshSectionInfoMap& SourceMap,
    const FMeshSectionInfoMap& TargetMap,
    const TCHAR* Label,
    FString& OutDetail)
{
    if (SourceMap.Map.Num() != TargetMap.Map.Num())
    {
        OutDetail = FString::Printf(
            TEXT("static mesh %s section map count mismatch: source=%d target=%d"),
            Label,
            SourceMap.Map.Num(),
            TargetMap.Map.Num());
        return false;
    }

    for (const TPair<uint32, FMeshSectionInfo>& SourcePair : SourceMap.Map)
    {
        const FMeshSectionInfo* TargetInfo = TargetMap.Map.Find(SourcePair.Key);
        if (!TargetInfo)
        {
            OutDetail = FString::Printf(TEXT("static mesh %s section map missing key %u"), Label, SourcePair.Key);
            return false;
        }
        if (SourcePair.Value.MaterialIndex != TargetInfo->MaterialIndex ||
            SourcePair.Value.bEnableCollision != TargetInfo->bEnableCollision ||
            SourcePair.Value.bCastShadow != TargetInfo->bCastShadow ||
            SourcePair.Value.bVisibleInRayTracing != TargetInfo->bVisibleInRayTracing ||
            SourcePair.Value.bAffectDistanceFieldLighting != TargetInfo->bAffectDistanceFieldLighting ||
            SourcePair.Value.bForceOpaque != TargetInfo->bForceOpaque)
        {
            OutDetail = FString::Printf(TEXT("static mesh %s section map value mismatch at key %u"), Label, SourcePair.Key);
            return false;
        }
    }
    return true;
}

bool CompareStaticMeshMaterialsForMCP(
    UStaticMesh* SourceMesh,
    UStaticMesh* TargetMesh,
    const TMap<UObject*, UObject*>* OptionalReplacementMap,
    FString& OutDetail)
{
    const TArray<FStaticMaterial>& SourceMaterials = SourceMesh->GetStaticMaterials();
    const TArray<FStaticMaterial>& TargetMaterials = TargetMesh->GetStaticMaterials();
    if (SourceMaterials.Num() != TargetMaterials.Num())
    {
        OutDetail = FString::Printf(
            TEXT("static mesh material slot count mismatch: source=%d target=%d"),
            SourceMaterials.Num(),
            TargetMaterials.Num());
        return false;
    }

    for (int32 Index = 0; Index < SourceMaterials.Num(); ++Index)
    {
        const FStaticMaterial& SourceMaterial = SourceMaterials[Index];
        const FStaticMaterial& TargetMaterial = TargetMaterials[Index];
        if (SourceMaterial.MaterialSlotName != TargetMaterial.MaterialSlotName ||
            SourceMaterial.ImportedMaterialSlotName != TargetMaterial.ImportedMaterialSlotName)
        {
            OutDetail = FString::Printf(TEXT("static mesh material slot name mismatch at index %d"), Index);
            return false;
        }

        UMaterialInterface* ExpectedMaterial = ResolveMaterialReplacementForMCP(OptionalReplacementMap, SourceMaterial.MaterialInterface);
        UMaterialInterface* ExpectedOverlayMaterial = ResolveMaterialReplacementForMCP(OptionalReplacementMap, SourceMaterial.OverlayMaterialInterface);
        if (TargetMaterial.MaterialInterface != ExpectedMaterial ||
            TargetMaterial.OverlayMaterialInterface != ExpectedOverlayMaterial)
        {
            OutDetail = FString::Printf(TEXT("static mesh material reference mismatch at index %d"), Index);
            return false;
        }
    }
    return true;
}

bool CompareStaticMeshSocketsForMCP(UStaticMesh* SourceMesh, UStaticMesh* TargetMesh, FString& OutDetail)
{
    if (SourceMesh->Sockets.Num() != TargetMesh->Sockets.Num())
    {
        OutDetail = FString::Printf(
            TEXT("static mesh socket count mismatch: source=%d target=%d"),
            SourceMesh->Sockets.Num(),
            TargetMesh->Sockets.Num());
        return false;
    }

    for (const TObjectPtr<UStaticMeshSocket>& SourceSocketPtr : SourceMesh->Sockets)
    {
        const UStaticMeshSocket* SourceSocket = SourceSocketPtr.Get();
        if (!SourceSocket)
        {
            continue;
        }

        const UStaticMeshSocket* TargetSocket = TargetMesh->FindSocket(SourceSocket->SocketName);
        if (!TargetSocket)
        {
            OutDetail = FString::Printf(TEXT("static mesh missing socket: %s"), *SourceSocket->SocketName.ToString());
            return false;
        }

        if (TargetSocket->GetOuter() != TargetMesh ||
            TargetSocket->RelativeLocation != SourceSocket->RelativeLocation ||
            TargetSocket->RelativeRotation != SourceSocket->RelativeRotation ||
            TargetSocket->RelativeScale != SourceSocket->RelativeScale ||
            TargetSocket->Tag != SourceSocket->Tag)
        {
            OutDetail = FString::Printf(TEXT("static mesh socket data mismatch: %s"), *SourceSocket->SocketName.ToString());
            return false;
        }
    }
    return true;
}

int32 GetBodySetupCollisionElementCountForMCP(const UBodySetup* BodySetup)
{
    return BodySetup ? BodySetup->AggGeom.GetElementCount() : 0;
}

bool CompareStaticMeshBodySetupForMCP(UStaticMesh* SourceMesh, UStaticMesh* TargetMesh, FString& OutDetail)
{
    const UBodySetup* SourceBodySetup = SourceMesh->GetBodySetup();
    const UBodySetup* TargetBodySetup = TargetMesh->GetBodySetup();
    if (!SourceBodySetup && !TargetBodySetup)
    {
        return true;
    }
    if (!SourceBodySetup || !TargetBodySetup)
    {
        OutDetail = TEXT("static mesh body setup presence mismatch");
        return false;
    }
    if (TargetBodySetup == SourceBodySetup || TargetBodySetup->GetOuter() != TargetMesh)
    {
        OutDetail = TEXT("static mesh body setup was not recreated under target mesh");
        return false;
    }
    if (GetBodySetupCollisionElementCountForMCP(SourceBodySetup) != GetBodySetupCollisionElementCountForMCP(TargetBodySetup))
    {
        OutDetail = FString::Printf(
            TEXT("static mesh collision element count mismatch: source=%d target=%d"),
            GetBodySetupCollisionElementCountForMCP(SourceBodySetup),
            GetBodySetupCollisionElementCountForMCP(TargetBodySetup));
        return false;
    }
    if (SourceBodySetup->GetCollisionTraceFlag() != TargetBodySetup->GetCollisionTraceFlag() ||
        SourceBodySetup->bDoubleSidedGeometry != TargetBodySetup->bDoubleSidedGeometry)
    {
        OutDetail = TEXT("static mesh body setup collision settings mismatch");
        return false;
    }
    return true;
}

bool ValidateFreshStaticMeshAsset(
    UStaticMesh* SourceMesh,
    UStaticMesh* TargetMesh,
    const TMap<UObject*, UObject*>* OptionalReplacementMap,
    FString& OutDetail)
{
    if (!SourceMesh || !TargetMesh)
    {
        OutDetail = TEXT("source or target static mesh is null");
        return false;
    }

    FinishCompilationForObjectForMCP(SourceMesh);
    FinishCompilationForObjectForMCP(TargetMesh);

    const int32 SourceLODCount = SourceMesh->GetNumSourceModels();
    const int32 TargetLODCount = TargetMesh->GetNumSourceModels();
    if (SourceLODCount != TargetLODCount)
    {
        OutDetail = FString::Printf(TEXT("static mesh LOD count mismatch: source=%d target=%d"), SourceLODCount, TargetLODCount);
        return false;
    }

    int32 RenderExportValidatedLODCount = 0;
    for (int32 LodIndex = 0; LodIndex < SourceLODCount; ++LodIndex)
    {
        FMeshDescription SourceDescription;
        FMeshDescription TargetDescription;
        bool bSourceUsedRenderExport = false;
        bool bTargetUsedRenderExport = false;
        if (!GetStaticMeshDescriptionForFreshCopyForMCP(SourceMesh, LodIndex, SourceDescription, bSourceUsedRenderExport, OutDetail) ||
            !GetStaticMeshDescriptionForFreshCopyForMCP(TargetMesh, LodIndex, TargetDescription, bTargetUsedRenderExport, OutDetail))
        {
            return false;
        }
        if (!CompareStaticMeshDescriptionShapeForMCP(SourceDescription, TargetDescription, LodIndex, OutDetail))
        {
            return false;
        }
        if (bSourceUsedRenderExport || bTargetUsedRenderExport)
        {
            ++RenderExportValidatedLODCount;
        }
        if (SourceMesh->GetNumUVChannels(LodIndex) != TargetMesh->GetNumUVChannels(LodIndex))
        {
            OutDetail = FString::Printf(
                TEXT("static mesh UV channel count mismatch at LOD %d: source=%d target=%d"),
                LodIndex,
                SourceMesh->GetNumUVChannels(LodIndex),
                TargetMesh->GetNumUVChannels(LodIndex));
            return false;
        }

        const FStaticMeshSourceModel& SourceModel = SourceMesh->GetSourceModel(LodIndex);
        const FStaticMeshSourceModel& TargetModel = TargetMesh->GetSourceModel(LodIndex);
        FMeshBuildSettings ExpectedBuildSettings = SourceModel.BuildSettings;
        ExpectedBuildSettings.DistanceFieldReplacementMesh = ResolveStaticMeshReplacementForMCP(
            OptionalReplacementMap,
            ExpectedBuildSettings.DistanceFieldReplacementMesh);
        if (TargetModel.BuildSettings != ExpectedBuildSettings)
        {
            OutDetail = FString::Printf(TEXT("static mesh build settings mismatch at LOD %d"), LodIndex);
            return false;
        }
        if (TargetModel.ReductionSettings != SourceModel.ReductionSettings)
        {
            OutDetail = FString::Printf(TEXT("static mesh reduction settings mismatch at LOD %d"), LodIndex);
            return false;
        }
        if (!ArePerPlatformFloatsEqualForMCP(SourceModel.ScreenSize, TargetModel.ScreenSize))
        {
            OutDetail = FString::Printf(TEXT("static mesh screen size mismatch at LOD %d"), LodIndex);
            return false;
        }
        if (TargetModel.SourceImportFilename != SourceModel.SourceImportFilename)
        {
            OutDetail = FString::Printf(TEXT("static mesh source import filename mismatch at LOD %d"), LodIndex);
            return false;
        }
#if WITH_EDITORONLY_DATA
        if (TargetModel.bImportWithBaseMesh != SourceModel.bImportWithBaseMesh)
        {
            OutDetail = FString::Printf(TEXT("static mesh import-with-base flag mismatch at LOD %d"), LodIndex);
            return false;
        }
#endif
    }

    if (!CompareStaticMeshRenderStatsForMCP(SourceMesh, TargetMesh, OutDetail))
    {
        return false;
    }

    if (!CompareStaticMeshMaterialsForMCP(SourceMesh, TargetMesh, OptionalReplacementMap, OutDetail) ||
        !CompareStaticMeshSectionInfoMapForMCP(SourceMesh->GetSectionInfoMap(), TargetMesh->GetSectionInfoMap(), TEXT("current"), OutDetail) ||
        !CompareStaticMeshSectionInfoMapForMCP(SourceMesh->GetOriginalSectionInfoMap(), TargetMesh->GetOriginalSectionInfoMap(), TEXT("original"), OutDetail) ||
        !CompareStaticMeshSocketsForMCP(SourceMesh, TargetMesh, OutDetail) ||
        !CompareStaticMeshBodySetupForMCP(SourceMesh, TargetMesh, OutDetail))
    {
        return false;
    }

#if WITH_EDITORONLY_DATA
    UStaticMesh* ExpectedComplexCollisionMesh = ResolveStaticMeshReplacementForMCP(OptionalReplacementMap, SourceMesh->ComplexCollisionMesh);
    if (TargetMesh->ComplexCollisionMesh != ExpectedComplexCollisionMesh)
    {
        OutDetail = TEXT("static mesh complex collision mesh mismatch");
        return false;
    }
#endif

    if (TargetMesh->GetNaniteSettings() != SourceMesh->GetNaniteSettings())
    {
        OutDetail = TEXT("static mesh nanite settings mismatch");
        return false;
    }

    OutDetail = FString::Printf(
        TEXT("fresh_static_mesh_from_%s_lods_%d_materials_%d_sockets_%d_collision_shapes_%d"),
        *SourceMesh->GetName(),
        SourceLODCount,
        SourceMesh->GetStaticMaterials().Num(),
        SourceMesh->Sockets.Num(),
        GetBodySetupCollisionElementCountForMCP(SourceMesh->GetBodySetup()));
    if (RenderExportValidatedLODCount > 0)
    {
        OutDetail += FString::Printf(TEXT("_render_export_validated_lods_%d"), RenderExportValidatedLODCount);
    }
    return true;
}

void CopyStaticMeshMaterialsForMCP(
    UStaticMesh* SourceMesh,
    UStaticMesh* TargetMesh,
    const TMap<UObject*, UObject*>* OptionalReplacementMap)
{
    TArray<FStaticMaterial> TargetMaterials = SourceMesh->GetStaticMaterials();
    for (FStaticMaterial& TargetMaterial : TargetMaterials)
    {
        TargetMaterial.MaterialInterface = ResolveMaterialReplacementForMCP(OptionalReplacementMap, TargetMaterial.MaterialInterface);
        TargetMaterial.OverlayMaterialInterface = ResolveMaterialReplacementForMCP(OptionalReplacementMap, TargetMaterial.OverlayMaterialInterface);
    }
    TargetMesh->SetStaticMaterials(TargetMaterials);
}

bool CopyStaticMeshBodySetupForMCP(UStaticMesh* SourceMesh, UStaticMesh* TargetMesh, FString& OutDetail)
{
    UBodySetup* SourceBodySetup = SourceMesh ? SourceMesh->GetBodySetup() : nullptr;
    if (!SourceBodySetup)
    {
        TargetMesh->SetBodySetup(nullptr);
        return true;
    }

    const FName BodySetupName = MakeUniqueObjectName(TargetMesh, UBodySetup::StaticClass(), UStaticMesh::GetBodySetupName());
    UBodySetup* TargetBodySetup = Cast<UBodySetup>(StaticDuplicateObject(
        SourceBodySetup,
        TargetMesh,
        BodySetupName,
        RF_Transactional));
    if (!TargetBodySetup)
    {
        OutDetail = TEXT("could not duplicate static mesh body setup");
        return false;
    }
    TargetBodySetup->ClearFlags(RF_Transient);
    TargetMesh->SetBodySetup(TargetBodySetup);
    return true;
}

bool CopyStaticMeshSocketsForMCP(
    UStaticMesh* SourceMesh,
    UStaticMesh* TargetMesh,
    const TMap<UObject*, UObject*>* OptionalReplacementMap,
    FString& OutDetail)
{
    TargetMesh->Sockets.Reset();
    for (const TObjectPtr<UStaticMeshSocket>& SourceSocketPtr : SourceMesh->Sockets)
    {
        UStaticMeshSocket* SourceSocket = SourceSocketPtr.Get();
        if (!SourceSocket)
        {
            continue;
        }

        const FName SocketObjectName = MakeUniqueObjectName(TargetMesh, UStaticMeshSocket::StaticClass(), SourceSocket->GetFName());
        UStaticMeshSocket* TargetSocket = Cast<UStaticMeshSocket>(StaticDuplicateObject(
            SourceSocket,
            TargetMesh,
            SocketObjectName,
            RF_Transactional));
        if (!TargetSocket)
        {
            OutDetail = FString::Printf(TEXT("could not duplicate static mesh socket: %s"), *SourceSocket->SocketName.ToString());
            return false;
        }
#if WITH_EDITORONLY_DATA
        TargetSocket->PreviewStaticMesh = ResolveStaticMeshReplacementForMCP(OptionalReplacementMap, SourceSocket->PreviewStaticMesh);
#endif
        TargetMesh->AddSocket(TargetSocket);
    }
    return true;
}

UObject* CreateFreshStaticMeshAsset(
    UObject* SourceObject,
    const FString& TargetPath,
    TMap<UObject*, UObject*>* OptionalReplacementMap,
    FString& OutDetail)
{
#if WITH_EDITORONLY_DATA
    UStaticMesh* SourceMesh = Cast<UStaticMesh>(SourceObject);
    if (!SourceMesh)
    {
        OutDetail = TEXT("source asset is not UStaticMesh");
        return nullptr;
    }

    FinishCompilationForObjectForMCP(SourceMesh);

    const int32 SourceLODCount = SourceMesh->GetNumSourceModels();
    if (SourceLODCount <= 0)
    {
        OutDetail = TEXT("source static mesh has no source models");
        return nullptr;
    }

    TArray<FMeshDescription> SourceMeshDescriptions;
    int32 RenderExportedLODCount = 0;
    SourceMeshDescriptions.SetNum(SourceLODCount);
    for (int32 LodIndex = 0; LodIndex < SourceLODCount; ++LodIndex)
    {
        bool bUsedRenderExport = false;
        if (!GetStaticMeshDescriptionForFreshCopyForMCP(SourceMesh, LodIndex, SourceMeshDescriptions[LodIndex], bUsedRenderExport, OutDetail))
        {
            return nullptr;
        }
        if (bUsedRenderExport)
        {
            ++RenderExportedLODCount;
        }
    }

    UPackage* TargetPackage = CreatePackage(*TargetPath);
    if (!TargetPackage)
    {
        OutDetail = TEXT("could not create target static mesh package");
        return nullptr;
    }

    const FName TargetObjectName(*FPackageName::GetShortName(TargetPath));
    UStaticMesh* TargetMesh = NewObject<UStaticMesh>(
        TargetPackage,
        UStaticMesh::StaticClass(),
        TargetObjectName,
        RF_Public | RF_Standalone | RF_Transactional);
    if (!TargetMesh)
    {
        OutDetail = TEXT("could not create target static mesh object");
        return nullptr;
    }

    TargetMesh->PreEditChange(nullptr);

    UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyParams;
    CopyParams.OptionalReplacementMappings = OptionalReplacementMap;
    CopyParams.bReplaceInternalReferenceUponRead = OptionalReplacementMap && OptionalReplacementMap->Num() > 0;
    UEngine::CopyPropertiesForUnrelatedObjects(SourceMesh, TargetMesh, CopyParams);

    TArray<FStaticMeshSourceModel> EmptySourceModels;
    TargetMesh->SetSourceModels(MoveTemp(EmptySourceModels));
    TargetMesh->SetNumSourceModels(SourceLODCount);
    for (int32 LodIndex = 0; LodIndex < SourceLODCount; ++LodIndex)
    {
        FMeshDescription TargetMeshDescription = MoveTemp(SourceMeshDescriptions[LodIndex]);
        TargetMesh->CreateMeshDescription(LodIndex, MoveTemp(TargetMeshDescription));

        UStaticMesh::FCommitMeshDescriptionParams CommitParams;
        CommitParams.bMarkPackageDirty = false;
        CommitParams.bUseHashAsGuid = true;
        TargetMesh->CommitMeshDescription(LodIndex, CommitParams);

        const FStaticMeshSourceModel& SourceModel = SourceMesh->GetSourceModel(LodIndex);
        FStaticMeshSourceModel& TargetModel = TargetMesh->GetSourceModel(LodIndex);
        TargetModel.BuildSettings = SourceModel.BuildSettings;
        TargetModel.BuildSettings.DistanceFieldReplacementMesh = ResolveStaticMeshReplacementForMCP(
            OptionalReplacementMap,
            SourceModel.BuildSettings.DistanceFieldReplacementMesh);
        TargetModel.ReductionSettings = SourceModel.ReductionSettings;
        TargetModel.ScreenSize = SourceModel.ScreenSize;
        TargetModel.SourceImportFilename = SourceModel.SourceImportFilename;
        TargetModel.bImportWithBaseMesh = SourceModel.bImportWithBaseMesh;
    }

    CopyStaticMeshMaterialsForMCP(SourceMesh, TargetMesh, OptionalReplacementMap);
    TargetMesh->GetSectionInfoMap().CopyFrom(SourceMesh->GetSectionInfoMap());
    TargetMesh->GetOriginalSectionInfoMap().CopyFrom(SourceMesh->GetOriginalSectionInfoMap());
    TargetMesh->SetNaniteSettings(SourceMesh->GetNaniteSettings());
    TargetMesh->ComplexCollisionMesh = ResolveStaticMeshReplacementForMCP(OptionalReplacementMap, SourceMesh->ComplexCollisionMesh);

    if (!CopyStaticMeshBodySetupForMCP(SourceMesh, TargetMesh, OutDetail) ||
        !CopyStaticMeshSocketsForMCP(SourceMesh, TargetMesh, OptionalReplacementMap, OutDetail))
    {
        DiscardFreshAssetForFallback(TargetMesh);
        return nullptr;
    }

    TargetMesh->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
    TargetMesh->ClearFlags(RF_Transient);

    TArray<FText> BuildErrors;
    UStaticMesh::FBuildParameters BuildParameters;
    BuildParameters.bInSilent = true;
    BuildParameters.OutErrors = &BuildErrors;
    BuildParameters.bInRebuildUVChannelData = true;
    TargetMesh->Build(BuildParameters);
    FinishCompilationForObjectForMCP(TargetMesh);
    TargetMesh->PostEditChange();
    FinishCompilationForObjectForMCP(TargetMesh);

    if (!BuildErrors.IsEmpty())
    {
        TArray<FString> ErrorStrings;
        for (const FText& BuildError : BuildErrors)
        {
            ErrorStrings.Add(BuildError.ToString());
        }
        OutDetail = FString::Printf(TEXT("static mesh build failed: %s"), *FString::Join(ErrorStrings, TEXT("; ")));
        DiscardFreshAssetForFallback(TargetMesh);
        return nullptr;
    }

    FString ValidationDetail;
    if (!ValidateFreshStaticMeshAsset(SourceMesh, TargetMesh, OptionalReplacementMap, ValidationDetail))
    {
        OutDetail = ValidationDetail;
        DiscardFreshAssetForFallback(TargetMesh);
        return nullptr;
    }

    TargetPackage->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(TargetMesh);

    OutDetail = ValidationDetail;
    if (RenderExportedLODCount > 0)
    {
        OutDetail += FString::Printf(TEXT("_render_exported_lods_%d"), RenderExportedLODCount);
    }
    return TargetMesh;
#else
    OutDetail = TEXT("static mesh recreation requires editor-only mesh description data");
    return nullptr;
#endif
}

bool ValidateFreshDataTableAsset(
    UDataTable* SourceTable,
    UDataTable* TargetTable,
    const UScriptStruct* ExpectedTargetRowStruct,
    FString& OutDetail)
{
    if (!SourceTable || !TargetTable)
    {
        OutDetail = TEXT("source or target data table is null");
        return false;
    }

    if (!ExpectedTargetRowStruct || TargetTable->GetRowStruct() != ExpectedTargetRowStruct)
    {
        OutDetail = TEXT("target data table row struct mismatch");
        return false;
    }

    TArray<FName> SourceRowNames = SourceTable->GetRowNames();
    TArray<FName> TargetRowNames = TargetTable->GetRowNames();
    if (SourceRowNames.Num() != TargetRowNames.Num())
    {
        OutDetail = FString::Printf(
            TEXT("data table row count mismatch: source=%d target=%d"),
            SourceRowNames.Num(),
            TargetRowNames.Num());
        return false;
    }

    SourceRowNames.Sort([](const FName& A, const FName& B)
    {
        return A.ToString() < B.ToString();
    });
    TargetRowNames.Sort([](const FName& A, const FName& B)
    {
        return A.ToString() < B.ToString();
    });

    for (int32 Index = 0; Index < SourceRowNames.Num(); ++Index)
    {
        if (SourceRowNames[Index] != TargetRowNames[Index])
        {
            OutDetail = FString::Printf(
                TEXT("data table row name mismatch at index %d: source=%s target=%s"),
                Index,
                *SourceRowNames[Index].ToString(),
                *TargetRowNames[Index].ToString());
            return false;
        }
    }

#if WITH_EDITOR
    const FString SourceJson = SourceTable->GetTableAsJSON();
    const FString TargetJson = TargetTable->GetTableAsJSON();
    if (SourceJson != TargetJson)
    {
        OutDetail = TEXT("data table JSON export mismatch");
        return false;
    }
#endif

    return true;
}

UObject* CreateFreshDataTableAsset(
    UObject* SourceObject,
    const FString& TargetPath,
    const TMap<UObject*, UObject*>& ReplacementMap,
    const FString& SourceRoot,
    FString& OutDetail)
{
    UDataTable* SourceTable = Cast<UDataTable>(SourceObject);
    if (!SourceTable)
    {
        OutDetail = TEXT("source asset is not UDataTable");
        return nullptr;
    }

    const UScriptStruct* SourceRowStruct = SourceTable->GetRowStruct();
    if (!SourceRowStruct)
    {
        OutDetail = TEXT("source data table has no row struct");
        return nullptr;
    }

    UObject* RowStructObject = const_cast<UScriptStruct*>(SourceRowStruct);
    UObject* ReplacementRowStructObject = FindReplacementObject(ReplacementMap, RowStructObject);
    UScriptStruct* TargetRowStruct = Cast<UScriptStruct>(ReplacementRowStructObject ? ReplacementRowStructObject : RowStructObject);
    if (!TargetRowStruct)
    {
        OutDetail = TEXT("could not resolve target data table row struct");
        return nullptr;
    }

    const FString SourceRowStructPackageName = SourceRowStruct->GetOutermost()
        ? SourceRowStruct->GetOutermost()->GetName()
        : FString();
    const bool bRowStructFromSourceRoot = SourceRowStructPackageName.Equals(SourceRoot)
        || SourceRowStructPackageName.StartsWith(SourceRoot + TEXT("/"));
    if (bRowStructFromSourceRoot && TargetRowStruct == SourceRowStruct)
    {
        OutDetail = FString::Printf(
            TEXT("source data table row struct has no target replacement: %s"),
            *SourceRowStruct->GetPathName());
        return nullptr;
    }

    UPackage* TargetPackage = CreatePackage(*TargetPath);
    if (!TargetPackage)
    {
        OutDetail = TEXT("could not create target data table package");
        return nullptr;
    }

    const FName TargetObjectName(*FPackageName::GetShortName(TargetPath));
    UDataTable* TargetTable = NewObject<UDataTable>(
        TargetPackage,
        UDataTable::StaticClass(),
        TargetObjectName,
        RF_Public | RF_Standalone | RF_Transactional);
    if (!TargetTable)
    {
        OutDetail = TEXT("could not create target data table object");
        return nullptr;
    }

    TargetTable->PreEditChange(nullptr);

#if WITH_EDITOR
    TargetTable->CopyImportOptions(SourceTable);
#endif

    TMap<FName, const uint8*> SourceRows;
    SourceRows.Reserve(SourceTable->GetRowMap().Num());
    for (const TPair<FName, uint8*>& RowPair : SourceTable->GetRowMap())
    {
        if (!RowPair.Value)
        {
            OutDetail = FString::Printf(TEXT("source data table row is null: %s"), *RowPair.Key.ToString());
            DiscardFreshAssetForFallback(TargetTable);
            return nullptr;
        }
        SourceRows.Add(RowPair.Key, RowPair.Value);
    }

    TArray<FString> CreateProblems = TargetTable->CreateTableFromRawData(SourceRows, TargetRowStruct);
    if (!CreateProblems.IsEmpty())
    {
        OutDetail = FString::Printf(TEXT("fresh data table raw import failed: %s"), *FString::Join(CreateProblems, TEXT("; ")));
        DiscardFreshAssetForFallback(TargetTable);
        return nullptr;
    }

#if WITH_EDITORONLY_DATA
    TargetTable->RowStructName_DEPRECATED = TargetRowStruct->GetFName();
    TargetTable->RowStructPathName = TargetRowStruct->GetStructPathName();
#endif

    if (!ValidateFreshDataTableAsset(SourceTable, TargetTable, TargetRowStruct, OutDetail))
    {
        DiscardFreshAssetForFallback(TargetTable);
        return nullptr;
    }

    TargetTable->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
    TargetTable->ClearFlags(RF_Transient);
    TargetTable->PostEditChange();
    TargetPackage->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(TargetTable);

    OutDetail = FString::Printf(
        TEXT("fresh_data_table_from_%s_rows_%d_row_struct_%s"),
        *SourceTable->GetName(),
        SourceTable->GetRowMap().Num(),
        *TargetRowStruct->GetPathName());
    return TargetTable;
}

void CopyEnumMetaDataValue(const UEnum* SourceEnum, UEnum* TargetEnum, const TCHAR* Key, int32 NameIndex = INDEX_NONE)
{
#if WITH_METADATA
    if (SourceEnum && TargetEnum && SourceEnum->HasMetaData(Key, NameIndex))
    {
        TargetEnum->SetMetaData(Key, *SourceEnum->GetMetaData(Key, NameIndex, false), NameIndex);
    }
#endif
}

UObject* CreateFreshUserDefinedEnumAsset(UObject* SourceObject, const FString& TargetPath, FString& OutDetail)
{
    UUserDefinedEnum* SourceEnum = Cast<UUserDefinedEnum>(SourceObject);
    if (!SourceEnum)
    {
        OutDetail = TEXT("source asset is not UUserDefinedEnum");
        return nullptr;
    }

    UPackage* TargetPackage = CreatePackage(*TargetPath);
    if (!TargetPackage)
    {
        OutDetail = TEXT("could not create target enum package");
        return nullptr;
    }

    const FName TargetObjectName(*FPackageName::GetShortName(TargetPath));
    UUserDefinedEnum* TargetEnum = Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(
        TargetPackage,
        TargetObjectName,
        RF_Public | RF_Standalone | RF_Transactional));
    if (!TargetEnum)
    {
        OutDetail = TEXT("could not create target user-defined enum");
        return nullptr;
    }

    TArray<TPair<FName, int64>> TargetEnumNames;
    const int32 SourceEnumeratorCount = FMath::Max(SourceEnum->NumEnums() - 1, 0);
    TargetEnumNames.Reserve(SourceEnumeratorCount);
    for (int32 Index = 0; Index < SourceEnumeratorCount; ++Index)
    {
        const FString ShortName = SourceEnum->GetNameStringByIndex(Index);
        const FString TargetFullName = TargetEnum->GenerateFullEnumName(*ShortName);
        TargetEnumNames.Emplace(FName(*TargetFullName), SourceEnum->GetValueByIndex(Index));
    }

    const EEnumFlags TargetEnumFlags = SourceEnum->HasAnyEnumFlags(EEnumFlags::Flags)
        ? EEnumFlags::Flags
        : EEnumFlags::None;
    TargetEnum->SetEnums(TargetEnumNames, SourceEnum->GetCppForm(), TargetEnumFlags);
    TargetEnum->SetMetaData(TEXT("BlueprintType"), TEXT("true"));
    CopyEnumMetaDataValue(SourceEnum, TargetEnum, TEXT("Bitflags"));
    CopyEnumMetaDataValue(SourceEnum, TargetEnum, TEXT("UseEnumValuesAsMaskValuesInEditor"));
    CopyEnumMetaDataValue(SourceEnum, TargetEnum, TEXT("ToolTip"));
    CopyEnumMetaDataValue(SourceEnum, TargetEnum, TEXT("ShortTooltip"));

#if WITH_EDITORONLY_DATA
    TargetEnum->EnumDescription = SourceEnum->EnumDescription;
#endif

    TargetEnum->DisplayNameMap.Empty(SourceEnumeratorCount);
    for (int32 Index = 0; Index < SourceEnumeratorCount; ++Index)
    {
        const FName TargetEntryName(*TargetEnum->GetNameStringByIndex(Index));
        TargetEnum->DisplayNameMap.Add(TargetEntryName, SourceEnum->GetDisplayNameTextByIndex(Index));
        CopyEnumMetaDataValue(SourceEnum, TargetEnum, TEXT("DisplayName"), Index);
        CopyEnumMetaDataValue(SourceEnum, TargetEnum, TEXT("ToolTip"), Index);
        CopyEnumMetaDataValue(SourceEnum, TargetEnum, TEXT("Hidden"), Index);
    }
    FEnumEditorUtils::EnsureAllDisplayNamesExist(TargetEnum);

    TargetEnum->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
    TargetEnum->ClearFlags(RF_Transient);
    TargetEnum->PostEditChange();
    TargetPackage->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(TargetEnum);

    OutDetail = FString::Printf(TEXT("fresh_user_defined_enum_from_%s_with_%d_enumerator(s)"), *SourceEnum->GetName(), SourceEnumeratorCount);
    return TargetEnum;
}

UObject* RemapObjectForFreshUserDefinedStruct(UObject* SourceObject, const TMap<UObject*, UObject*>& ReplacementMap)
{
    if (!SourceObject)
    {
        return nullptr;
    }

    if (UObject* ReplacementObject = FindReplacementObject(ReplacementMap, SourceObject))
    {
        return ReplacementObject;
    }
    return SourceObject;
}

bool RemapFreshStructVariableDescription(FStructVariableDescription& Description, const TMap<UObject*, UObject*>& ReplacementMap)
{
    bool bChanged = false;
    if (UObject* SubCategoryObject = Description.SubCategoryObject.LoadSynchronous())
    {
        UObject* ReplacementObject = RemapObjectForFreshUserDefinedStruct(SubCategoryObject, ReplacementMap);
        if (ReplacementObject != SubCategoryObject)
        {
            Description.SubCategoryObject = ReplacementObject;
            bChanged = true;
        }
    }

    if (UObject* TerminalObject = Description.PinValueType.TerminalSubCategoryObject.Get())
    {
        UObject* ReplacementObject = RemapObjectForFreshUserDefinedStruct(TerminalObject, ReplacementMap);
        if (ReplacementObject != TerminalObject)
        {
            Description.PinValueType.TerminalSubCategoryObject = ReplacementObject;
            bChanged = true;
        }
    }

    return bChanged;
}

UObject* CreateFreshUserDefinedStructAsset(
    UObject* SourceObject,
    const FString& TargetPath,
    const TMap<UObject*, UObject*>& ReplacementMap,
    FString& OutDetail)
{
    UUserDefinedStruct* SourceStruct = Cast<UUserDefinedStruct>(SourceObject);
    if (!SourceStruct)
    {
        OutDetail = TEXT("source asset is not UUserDefinedStruct");
        return nullptr;
    }

    UPackage* TargetPackage = CreatePackage(*TargetPath);
    if (!TargetPackage)
    {
        OutDetail = TEXT("could not create target struct package");
        return nullptr;
    }

    const FName TargetObjectName(*FPackageName::GetShortName(TargetPath));
    UUserDefinedStruct* TargetStruct = FStructureEditorUtils::CreateUserDefinedStruct(
        TargetPackage,
        TargetObjectName,
        RF_Public | RF_Standalone | RF_Transactional);
    if (!TargetStruct)
    {
        OutDetail = TEXT("could not create target user-defined struct");
        return nullptr;
    }

    const TArray<FStructVariableDescription>& SourceDescriptions = FStructureEditorUtils::GetVarDesc(SourceStruct);
    TArray<FStructVariableDescription>& TargetDescriptions = FStructureEditorUtils::GetVarDesc(TargetStruct);
    TargetDescriptions = SourceDescriptions;
    for (FStructVariableDescription& Description : TargetDescriptions)
    {
        RemapFreshStructVariableDescription(Description, ReplacementMap);
    }

    const FString SourceTooltip = FStructureEditorUtils::GetTooltip(SourceStruct);
    if (!SourceTooltip.IsEmpty())
    {
        FStructureEditorUtils::ChangeTooltip(TargetStruct, SourceTooltip);
    }
    TargetStruct->SetMetaData(TEXT("BlueprintType"), TEXT("true"));
    TargetStruct->Status = EUserDefinedStructureStatus::UDSS_Dirty;
    FStructureEditorUtils::CompileStructure(TargetStruct);
    FStructureEditorUtils::RecreateDefaultInstanceInEditorData(TargetStruct);
    TargetStruct->UpdateStructFlags();

    if (TargetStruct->Status == EUserDefinedStructureStatus::UDSS_Error)
    {
        OutDetail = FString::Printf(TEXT("fresh user-defined struct compile failed: %s"), *TargetStruct->ErrorMessage);
        DiscardFreshAssetForFallback(TargetStruct);
        return nullptr;
    }

    TargetStruct->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
    TargetStruct->ClearFlags(RF_Transient);
    TargetStruct->PostEditChange();
    TargetPackage->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(TargetStruct);

    OutDetail = FString::Printf(TEXT("fresh_user_defined_struct_from_%s_with_%d_member(s)"), *SourceStruct->GetName(), SourceDescriptions.Num());
    return TargetStruct;
}

int32 RemapUserDefinedStructDescriptionPaths(
    const TArray<UUserDefinedStruct*>& Structs,
    const TArray<FPathStringReplacement>& PathReplacements,
    const TMap<UObject*, UObject*>& ReplacementMap,
    const FString& SourceRoot)
{
    if (Structs.IsEmpty() || PathReplacements.IsEmpty())
    {
        return 0;
    }

    TSet<UUserDefinedStruct*> UniqueStructs;
    for (UUserDefinedStruct* Struct : Structs)
    {
        if (IsValid(Struct))
        {
            UniqueStructs.Add(Struct);
        }
    }

    int32 ReplaceCount = 0;
    for (UUserDefinedStruct* Struct : UniqueStructs)
    {
        if (!Struct)
        {
            continue;
        }

#if WITH_EDITORONLY_DATA
        if (!Struct->EditorData)
        {
            continue;
        }
#endif

        bool bStructChanged = false;
        TArray<FStructVariableDescription>& Descriptions = FStructureEditorUtils::GetVarDesc(Struct);
        for (FStructVariableDescription& Description : Descriptions)
        {
            bStructChanged |= RemapFreshStructVariableDescription(Description, ReplacementMap);

            auto RemapStringField = [&PathReplacements, &SourceRoot, &ReplaceCount](FString& Value) -> bool
            {
                if (Value.IsEmpty() || !Value.Contains(SourceRoot, ESearchCase::CaseSensitive))
                {
                    return false;
                }

                const FString ReplacedValue = ApplyPathReplacements(Value, PathReplacements);
                if (ReplacedValue == Value)
                {
                    return false;
                }

                Value = ReplacedValue;
                ++ReplaceCount;
                return true;
            };

            bStructChanged |= RemapStringField(Description.DefaultValue);
            bStructChanged |= RemapStringField(Description.CurrentDefaultValue);
            bStructChanged |= RemapStringField(Description.ToolTip);
            for (TPair<FName, FString>& MetaDataPair : Description.MetaData)
            {
                bStructChanged |= RemapStringField(MetaDataPair.Value);
            }
        }

        if (bStructChanged)
        {
            Struct->Modify();
            FStructureEditorUtils::ModifyStructData(Struct);
            Struct->Status = EUserDefinedStructureStatus::UDSS_Dirty;
            FStructureEditorUtils::CompileStructure(Struct);
            FStructureEditorUtils::RecreateDefaultInstanceInEditorData(Struct);
            Struct->UpdateStructFlags();
            Struct->PostEditChange();
            Struct->MarkPackageDirty();
            if (UPackage* Package = Struct->GetOutermost())
            {
                Package->MarkPackageDirty();
            }
        }
    }

    return ReplaceCount;
}

int32 CollectLoadedPackagesUnderRoot(const FString& Root, TArray<UPackage*>& OutPackages, TArray<FString>& OutSamples, int32 MaxSamples = 20)
{
    OutPackages.Reset();
    OutSamples.Reset();

    for (TObjectIterator<UPackage> It; It; ++It)
    {
        UPackage* Package = *It;
        if (!Package)
        {
            continue;
        }

        const FString PackageName = Package->GetName();
        if (PackageName.Equals(Root) || PackageName.StartsWith(Root + TEXT("/")))
        {
            OutPackages.Add(Package);
            if (OutSamples.Num() < MaxSamples)
            {
                OutSamples.Add(PackageName);
            }
        }
    }

    return OutPackages.Num();
}

bool DeleteTargetRootForRecreate(
    const FString& TargetRoot,
    FString& OutDeleteMode,
    TArray<FString>& OutWarnings)
{
    OutDeleteMode = TEXT("none");
    OutWarnings.Reset();

    UE_LOG(LogTemp, Display, TEXT("MCP recreate delete: preparing target root %s"), *TargetRoot);
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

    TArray<UPackage*> LoadedPackages;
    TArray<FString> LoadedPackageSamples;
    CollectLoadedPackagesUnderRoot(TargetRoot, LoadedPackages, LoadedPackageSamples);
    if (!LoadedPackages.IsEmpty())
    {
        UE_LOG(LogTemp, Display, TEXT("MCP recreate delete: unloading %d loaded package(s) under %s"), LoadedPackages.Num(), *TargetRoot);
        FText UnloadErrorMessage;
        bool bUnloadSucceeded = false;
        try
        {
            bUnloadSucceeded = UPackageTools::UnloadPackages(LoadedPackages, UnloadErrorMessage, true);
        }
        catch (const std::exception& Exception)
        {
            const FString ExceptionMessage = UTF8_TO_TCHAR(Exception.what());
            const FString SafeExceptionMessage = ExceptionMessage.IsEmpty()
                ? TEXT("<empty std::exception message>")
                : ExceptionMessage;
            UE_LOG(LogTemp, Error, TEXT("MCP recreate delete: package unload threw std::exception for %s: %s"), *TargetRoot, *SafeExceptionMessage);
            OutWarnings.Add(FString::Printf(
                TEXT("Package unload threw std::exception for %s: %s"),
                *TargetRoot,
                *SafeExceptionMessage));
        }
        catch (...)
        {
            UE_LOG(LogTemp, Error, TEXT("MCP recreate delete: package unload threw unknown exception for %s"), *TargetRoot);
            OutWarnings.Add(FString::Printf(TEXT("Package unload threw unknown exception for %s"), *TargetRoot));
        }
        if (!bUnloadSucceeded)
        {
            OutWarnings.Add(FString::Printf(
                TEXT("Failed to unload packages under %s before delete: %s"),
                *TargetRoot,
                *UnloadErrorMessage.ToString()));
        }
        UE_LOG(LogTemp, Display, TEXT("MCP recreate delete: unload result for %s = %s"), *TargetRoot, bUnloadSucceeded ? TEXT("true") : TEXT("false"));
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    }

    TArray<UPackage*> RemainingPackages;
    TArray<FString> RemainingPackageSamples;
    if (CollectLoadedPackagesUnderRoot(TargetRoot, RemainingPackages, RemainingPackageSamples) > 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("MCP recreate delete: %d package(s) still loaded under %s"), RemainingPackages.Num(), *TargetRoot);
        OutWarnings.Add(FString::Printf(
            TEXT("Cannot delete %s while %d packages are still loaded"),
            *TargetRoot,
            RemainingPackages.Num()));
        for (const FString& Sample : RemainingPackageSamples)
        {
            OutWarnings.Add(TEXT("loaded: ") + Sample);
        }
        return false;
    }

    if (TargetRoot.StartsWith(TEXT("/Game/_MCP_Temp/")))
    {
        FString TargetDirectory;
        if (!FPackageName::TryConvertLongPackageNameToFilename(TargetRoot, TargetDirectory))
        {
            OutWarnings.Add(FString::Printf(TEXT("Could not convert target root to filename: %s"), *TargetRoot));
            return false;
        }

        OutDeleteMode = TEXT("filesystem_temp_delete");
        UE_LOG(LogTemp, Display, TEXT("MCP recreate delete: deleting filesystem directory %s"), *TargetDirectory);
        if (IFileManager::Get().DirectoryExists(*TargetDirectory))
        {
            if (!IFileManager::Get().DeleteDirectory(*TargetDirectory, false, true))
            {
                OutWarnings.Add(FString::Printf(TEXT("Filesystem delete failed for %s"), *TargetDirectory));
                return false;
            }
        }
        return true;
    }

    OutDeleteMode = TEXT("editor_asset_delete");
    UE_LOG(LogTemp, Display, TEXT("MCP recreate delete: deleting editor asset directory %s"), *TargetRoot);
    if (UEditorAssetLibrary::DoesDirectoryExist(TargetRoot) && !UEditorAssetLibrary::DeleteDirectory(TargetRoot))
    {
        OutWarnings.Add(FString::Printf(TEXT("Editor asset delete failed for %s"), *TargetRoot));
        return false;
    }
    return true;
}

TSharedPtr<FJsonObject> AssetReportToJson(
    const FAssetData& SourceAsset,
    const FString& TargetPath,
    const FString& Strategy,
    const FString& Status,
    const FString& Detail)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("source_path"), SourceAsset.PackageName.ToString());
    Object->SetStringField(TEXT("source_name"), SourceAsset.AssetName.ToString());
    Object->SetStringField(TEXT("class"), SourceAsset.AssetClassPath.GetAssetName().ToString());
    Object->SetStringField(TEXT("target_path"), TargetPath);
    Object->SetStringField(TEXT("strategy"), Strategy);
    Object->SetStringField(TEXT("status"), Status);
    if (!Detail.IsEmpty())
    {
        Object->SetStringField(TEXT("detail"), Detail);
    }
    return Object;
}

void AddFallbackRuleDetailTokensForMCP(const TSharedPtr<FJsonObject>& RuleObject, TArray<FString>& OutTokens)
{
    FString DetailContains;
    if (RuleObject->TryGetStringField(TEXT("detail_contains"), DetailContains) && !DetailContains.IsEmpty())
    {
        OutTokens.Add(DetailContains);
    }

    const TArray<TSharedPtr<FJsonValue>>* DetailContainsAny = nullptr;
    if (RuleObject->TryGetArrayField(TEXT("detail_contains_any"), DetailContainsAny))
    {
        for (const TSharedPtr<FJsonValue>& Value : *DetailContainsAny)
        {
            FString Token;
            if (Value.IsValid() && Value->TryGetString(Token) && !Token.IsEmpty())
            {
                OutTokens.Add(Token);
            }
        }
    }
}

TArray<FAcceptedFallbackRule> ParseAcceptedFallbackRulesForMCP(const TSharedPtr<FJsonObject>& Params)
{
    TArray<FAcceptedFallbackRule> Rules;

    const TArray<TSharedPtr<FJsonValue>>* RuleValues = nullptr;
    if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("accepted_fallback_rules"), RuleValues))
    {
        return Rules;
    }

    for (const TSharedPtr<FJsonValue>& RuleValue : *RuleValues)
    {
        TSharedPtr<FJsonObject> RuleObject = RuleValue.IsValid() ? RuleValue->AsObject() : nullptr;
        if (!RuleObject.IsValid())
        {
            continue;
        }

        FAcceptedFallbackRule Rule;
        RuleObject->TryGetStringField(TEXT("source_path"), Rule.SourcePath);
        RuleObject->TryGetStringField(TEXT("source_path_prefix"), Rule.SourcePathPrefix);
        if (!RuleObject->TryGetStringField(TEXT("class"), Rule.ClassName))
        {
            RuleObject->TryGetStringField(TEXT("asset_class"), Rule.ClassName);
        }
        RuleObject->TryGetStringField(TEXT("reason"), Rule.Reason);
        AddFallbackRuleDetailTokensForMCP(RuleObject, Rule.DetailContainsAny);

        Rule.SourcePath = NormalizeContentRoot(Rule.SourcePath);
        Rule.SourcePathPrefix = NormalizeContentRoot(Rule.SourcePathPrefix);
        Rule.ClassName.TrimStartAndEndInline();
        Rule.Reason.TrimStartAndEndInline();

        if (Rule.Reason.IsEmpty())
        {
            Rule.Reason = TEXT("accepted_by_fallback_rule");
        }
        if (Rule.SourcePath.IsEmpty() && Rule.SourcePathPrefix.IsEmpty())
        {
            continue;
        }

        Rules.Add(MoveTemp(Rule));
    }

    return Rules;
}

bool AcceptedFallbackRuleMatchesForMCP(
    const FAcceptedFallbackRule& Rule,
    const FString& SourcePath,
    const FString& ClassName,
    const FString& FreshCreateDetail)
{
    const bool bSourcePathMatches = !Rule.SourcePath.IsEmpty() && SourcePath == Rule.SourcePath;
    const bool bSourcePathPrefixMatches = !Rule.SourcePathPrefix.IsEmpty()
        && (SourcePath == Rule.SourcePathPrefix || SourcePath.StartsWith(Rule.SourcePathPrefix + TEXT("/")));
    if (!bSourcePathMatches && !bSourcePathPrefixMatches)
    {
        return false;
    }

    if (!Rule.ClassName.IsEmpty() && Rule.ClassName != ClassName)
    {
        return false;
    }

    if (Rule.DetailContainsAny.IsEmpty())
    {
        return true;
    }

    for (const FString& Token : Rule.DetailContainsAny)
    {
        if (!Token.IsEmpty() && FreshCreateDetail.Contains(Token))
        {
            return true;
        }
    }

    return false;
}

FString GetAcceptedFreshCreateFallbackReasonForMCP(
    const FAssetData& SourceAsset,
    const FString& FreshCreateDetail,
    const TArray<FAcceptedFallbackRule>& AcceptedFallbackRules)
{
    const FString SourcePath = SourceAsset.PackageName.ToString();
    const FString ClassName = SourceAsset.AssetClassPath.GetAssetName().ToString();
    for (const FAcceptedFallbackRule& Rule : AcceptedFallbackRules)
    {
        if (AcceptedFallbackRuleMatchesForMCP(Rule, SourcePath, ClassName, FreshCreateDetail))
        {
            return Rule.Reason;
        }
    }

    return FString();
}

void ClassifyFreshCreateFallbackForMCP(
    const FAssetData& SourceAsset,
    const FString& TargetPath,
    const FString& FreshCreateDetail,
    const TArray<FAcceptedFallbackRule>& AcceptedFallbackRules,
    TSharedPtr<FJsonObject> AssetReport,
    int32& AcceptedFallbackCount,
    int32& UnresolvedFallbackCount,
    TArray<FString>& AcceptedFallbackSamples,
    TArray<FString>& UnresolvedFallbackSamples)
{
    const FString AcceptedReason = GetAcceptedFreshCreateFallbackReasonForMCP(SourceAsset, FreshCreateDetail, AcceptedFallbackRules);
    const bool bAccepted = !AcceptedReason.IsEmpty();
    AssetReport->SetStringField(TEXT("fallback_resolution"), bAccepted ? TEXT("accepted") : TEXT("unresolved"));
    if (bAccepted)
    {
        AssetReport->SetStringField(TEXT("fallback_acceptance_reason"), AcceptedReason);
        ++AcceptedFallbackCount;
        if (AcceptedFallbackSamples.Num() < 50)
        {
            AcceptedFallbackSamples.Add(FString::Printf(
                TEXT("%s -> %s | %s | %s"),
                *SourceAsset.PackageName.ToString(),
                *TargetPath,
                *AcceptedReason,
                *FreshCreateDetail));
        }
    }
    else
    {
        ++UnresolvedFallbackCount;
        if (UnresolvedFallbackSamples.Num() < 50)
        {
            UnresolvedFallbackSamples.Add(FString::Printf(
                TEXT("%s -> %s | %s"),
                *SourceAsset.PackageName.ToString(),
                *TargetPath,
                FreshCreateDetail.IsEmpty() ? TEXT("fresh create fallback without detail") : *FreshCreateDetail));
        }
    }
}

void AddStringArrayField(TSharedPtr<FJsonObject> Object, const FString& FieldName, const TArray<FString>& Values)
{
    TArray<TSharedPtr<FJsonValue>> JsonValues;
    for (const FString& Value : Values)
    {
        JsonValues.Add(MakeShared<FJsonValueString>(Value));
    }
    Object->SetArrayField(FieldName, JsonValues);
}

FString MakeSafeReportFilenameStemForMCP(FString Stem)
{
    Stem.TrimStartAndEndInline();
    if (Stem.IsEmpty())
    {
        Stem = TEXT("content");
    }

    for (int32 CharacterIndex = 0; CharacterIndex < Stem.Len(); ++CharacterIndex)
    {
        TCHAR& Character = Stem[CharacterIndex];
        if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-') && Character != TEXT('.'))
        {
            Character = TEXT('_');
        }
    }

    return Stem;
}

FString BuildMCPReportFilename(const FString& PackagePathOrRoot, const FString& OperationName)
{
    FString Stem = FPackageName::GetShortName(PackagePathOrRoot);
    if (Stem.IsEmpty())
    {
        Stem = FPackageName::GetShortName(NormalizePackagePathParam(PackagePathOrRoot));
    }
    Stem = MakeSafeReportFilenameStemForMCP(Stem);
    return Stem + TEXT("_") + MakeSafeReportFilenameStemForMCP(OperationName) + TEXT("_report.json");
}

FString SaveJsonReportForMCP(const TSharedPtr<FJsonObject>& ReportObject, const FString& ReportFilename)
{
    const FString ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MCP"));
    IFileManager::Get().MakeDirectory(*ReportDirectory, true);
    const FString ReportPath = FPaths::Combine(ReportDirectory, ReportFilename);
    ReportObject->SetStringField(TEXT("report_path"), ReportPath);

    FString ReportJson;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ReportJson);
    FJsonSerializer::Serialize(ReportObject.ToSharedRef(), Writer);

    FFileHelper::SaveStringToFile(ReportJson, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8);
    return ReportPath;
}

TSharedPtr<FJsonObject> CloneJsonObjectForMCP(const TSharedPtr<FJsonObject>& Source)
{
    TSharedPtr<FJsonObject> Clone = MakeShared<FJsonObject>();
    if (!Source.IsValid())
    {
        return Clone;
    }

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
    {
        Clone->SetField(Pair.Key, Pair.Value);
    }
    return Clone;
}

void SetStringFieldIfMissingForMCP(TSharedPtr<FJsonObject> Object, const FString& FieldName, const FString& Value)
{
    if (Object.IsValid() && !Object->HasField(FieldName))
    {
        Object->SetStringField(FieldName, Value);
    }
}

void SetBoolFieldIfMissingForMCP(TSharedPtr<FJsonObject> Object, const FString& FieldName, bool bValue)
{
    if (Object.IsValid() && !Object->HasField(FieldName))
    {
        Object->SetBoolField(FieldName, bValue);
    }
}

void SetNumberFieldIfMissingForMCP(TSharedPtr<FJsonObject> Object, const FString& FieldName, double Value)
{
    if (Object.IsValid() && !Object->HasField(FieldName))
    {
        Object->SetNumberField(FieldName, Value);
    }
}

bool GetJsonBoolFieldForMCP(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, bool bDefaultValue = false)
{
    bool bValue = bDefaultValue;
    if (Object.IsValid() && Object->TryGetBoolField(FieldName, bValue))
    {
        return bValue;
    }
    return bDefaultValue;
}

int32 GetJsonIntFieldForMCP(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, int32 DefaultValue = 0)
{
    double Value = DefaultValue;
    if (Object.IsValid() && Object->TryGetNumberField(FieldName, Value))
    {
        return FMath::RoundToInt(Value);
    }
    return DefaultValue;
}

FString GetJsonStringFieldForMCP(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
    FString Value;
    if (Object.IsValid())
    {
        Object->TryGetStringField(FieldName, Value);
    }
    return Value;
}

bool PipelineStepResultPassesForMCP(const TSharedPtr<FJsonObject>& Result)
{
    return GetJsonBoolFieldForMCP(Result, TEXT("success"), false)
        && GetJsonBoolFieldForMCP(Result, TEXT("verification_pass"), false);
}

TSharedPtr<FJsonObject> MakePipelineStepSummaryForMCP(
    const FString& StepName,
    bool bRan,
    const TSharedPtr<FJsonObject>& Result,
    const FString& SkippedReason = FString())
{
    TSharedPtr<FJsonObject> StepObject = MakeShared<FJsonObject>();
    StepObject->SetStringField(TEXT("step"), StepName);
    StepObject->SetBoolField(TEXT("ran"), bRan);

    if (!bRan)
    {
        StepObject->SetBoolField(TEXT("success"), true);
        StepObject->SetBoolField(TEXT("verification_pass"), true);
        StepObject->SetStringField(TEXT("skipped_reason"), SkippedReason);
        return StepObject;
    }

    StepObject->SetBoolField(TEXT("success"), GetJsonBoolFieldForMCP(Result, TEXT("success"), false));
    StepObject->SetBoolField(TEXT("verification_pass"), GetJsonBoolFieldForMCP(Result, TEXT("verification_pass"), false));

    const FString ReportPath = GetJsonStringFieldForMCP(Result, TEXT("report_path"));
    if (!ReportPath.IsEmpty())
    {
        StepObject->SetStringField(TEXT("report_path"), ReportPath);
    }

    const FString VerificationError = GetJsonStringFieldForMCP(Result, TEXT("verification_error"));
    if (!VerificationError.IsEmpty())
    {
        StepObject->SetStringField(TEXT("verification_error"), VerificationError);
    }

    const FString Error = GetJsonStringFieldForMCP(Result, TEXT("error"));
    if (!Error.IsEmpty())
    {
        StepObject->SetStringField(TEXT("error"), Error);
    }

    return StepObject;
}

void CopyNumberFieldIfPresentForMCP(
    const TSharedPtr<FJsonObject>& Source,
    TSharedPtr<FJsonObject> Target,
    const FString& SourceFieldName,
    const FString& TargetFieldName)
{
    double Value = 0.0;
    if (Source.IsValid() && Target.IsValid() && Source->TryGetNumberField(SourceFieldName, Value))
    {
        Target->SetNumberField(TargetFieldName, Value);
    }
}

FString GetEditorLogPath()
{
    FString ProjectName = FApp::GetProjectName();
    if (ProjectName.IsEmpty() && !FPaths::GetProjectFilePath().IsEmpty())
    {
        ProjectName = FPaths::GetBaseFilename(FPaths::GetProjectFilePath());
    }
    if (!ProjectName.IsEmpty())
    {
        return FPaths::Combine(FPaths::ProjectLogDir(), ProjectName + TEXT(".log"));
    }
    return FString();
}

int32 CountLogLines(const FString& Text)
{
    if (Text.IsEmpty())
    {
        return 0;
    }

    int32 Count = 0;
    for (int32 Index = 0; Index < Text.Len(); ++Index)
    {
        if (Text[Index] == TEXT('\n'))
        {
            ++Count;
        }
    }
    if (!Text.EndsWith(TEXT("\n")))
    {
        ++Count;
    }
    return Count;
}

FEditorLogMarker CaptureEditorLogMarker(bool bEnabled)
{
    FEditorLogMarker Marker;
    Marker.bEnabled = bEnabled;
    Marker.LogPath = GetEditorLogPath();
    if (!bEnabled || Marker.LogPath.IsEmpty())
    {
        return Marker;
    }

    Marker.StartFileSize = IFileManager::Get().FileSize(*Marker.LogPath);
    FString LogText;
    if (FFileHelper::LoadFileToString(LogText, *Marker.LogPath, FFileHelper::EHashOptions::None, FILEREAD_AllowWrite))
    {
        Marker.StartLineCount = CountLogLines(LogText);
    }
    return Marker;
}

void AddEditorLogIssueSample(FEditorLogHealthResult& Result, const TArray<FString>& Lines, int32 LineIndex, const FString& Type)
{
    if (Result.IssueSamples.Num() >= 20 || !Lines.IsValidIndex(LineIndex))
    {
        return;
    }

    FString Sample = Type + TEXT(": ");
    const int32 LastLine = FMath::Min(LineIndex + 3, Lines.Num() - 1);
    for (int32 Index = LineIndex; Index <= LastLine; ++Index)
    {
        FString Line = Lines[Index];
        Line.TrimStartAndEndInline();
        if (!Line.IsEmpty())
        {
            if (Index > LineIndex)
            {
                Sample += TEXT(" | ");
            }
            Sample += Line;
        }
    }

    if (Sample.Len() > 1200)
    {
        Sample = Sample.Left(1200) + TEXT("...");
    }
    Result.IssueSamples.Add(Sample);
}

bool IsBenignBlueprintUberGraphFrameEnsure(const TArray<FString>& Lines, int32 LineIndex, const FString& TargetRoot)
{
    if (TargetRoot.IsEmpty())
    {
        return false;
    }

    bool bHasUberGraphFrameKeyMismatch = false;
    bool bHasKeyMismatchDiagnostic = false;
    bool bHasTargetPath = false;
    const int32 LastLine = FMath::Min(LineIndex + 8, Lines.Num() - 1);
    for (int32 Index = LineIndex; Index <= LastLine; ++Index)
    {
        bHasUberGraphFrameKeyMismatch |= Lines[Index].Contains(TEXT("PointerToUberGraphFrame->UberGraphFunctionKey"), ESearchCase::CaseSensitive);
        bHasKeyMismatchDiagnostic |= Lines[Index].Contains(TEXT("Detected key mismatch in uber graph frame for instance"), ESearchCase::CaseSensitive);
        bHasTargetPath |= Lines[Index].Contains(TargetRoot, ESearchCase::CaseSensitive);
        if (bHasUberGraphFrameKeyMismatch && bHasKeyMismatchDiagnostic && bHasTargetPath)
        {
            return true;
        }
    }
    return false;
}

FEditorLogHealthResult ScanEditorLogSince(const FEditorLogMarker& Marker, const FString& BenignBlueprintEnsureRoot = FString())
{
    FEditorLogHealthResult Result;
    Result.LogPath = Marker.LogPath;
    Result.bChecked = Marker.bEnabled && !Marker.LogPath.IsEmpty();
    if (!Result.bChecked)
    {
        return Result;
    }

    FString LogText;
    if (!FFileHelper::LoadFileToString(LogText, *Marker.LogPath, FFileHelper::EHashOptions::None, FILEREAD_AllowWrite))
    {
        Result.bChecked = false;
        return Result;
    }

    TArray<FString> Lines;
    LogText.ParseIntoArrayLines(Lines, false);
    const int32 StartIndex = FMath::Clamp(Marker.StartLineCount, 0, Lines.Num());
    for (int32 Index = StartIndex; Index < Lines.Num(); ++Index)
    {
        const FString& Line = Lines[Index];
        if (Line.Contains(TEXT("=== Handled ensure: ==="), ESearchCase::CaseSensitive))
        {
            if (IsBenignBlueprintUberGraphFrameEnsure(Lines, Index, BenignBlueprintEnsureRoot))
            {
                ++Result.IgnoredHandledEnsureCount;
                continue;
            }
            ++Result.HandledEnsureCount;
            AddEditorLogIssueSample(Result, Lines, Index, TEXT("handled_ensure"));
        }
        if (Line.Contains(TEXT("Fatal error:"), ESearchCase::CaseSensitive))
        {
            ++Result.FatalErrorCount;
            AddEditorLogIssueSample(Result, Lines, Index, TEXT("fatal_error"));
        }
        if (Line.Contains(TEXT("=== Critical error: ==="), ESearchCase::CaseSensitive))
        {
            ++Result.CriticalErrorCount;
            AddEditorLogIssueSample(Result, Lines, Index, TEXT("critical_error"));
        }
        if (Line.Contains(TEXT("ForceDeleteObjects"), ESearchCase::CaseSensitive) ||
            Line.Contains(TEXT("ObjectTools.cpp"), ESearchCase::CaseSensitive))
        {
            ++Result.ForceDeleteIssueCount;
            AddEditorLogIssueSample(Result, Lines, Index, TEXT("force_delete_issue"));
        }
        if (Line.Contains(TEXT("World Memory Leaks"), ESearchCase::CaseSensitive))
        {
            ++Result.WorldMemoryLeakCount;
            AddEditorLogIssueSample(Result, Lines, Index, TEXT("world_memory_leak"));
        }
    }

    Result.IssueCount =
        Result.HandledEnsureCount +
        Result.FatalErrorCount +
        Result.CriticalErrorCount +
        Result.ForceDeleteIssueCount +
        Result.WorldMemoryLeakCount;
    Result.bPass = Result.IssueCount == 0;
    return Result;
}

void AddEditorLogHealthFields(TSharedPtr<FJsonObject> Object, const FEditorLogHealthResult& Result)
{
    Object->SetBoolField(TEXT("editor_log_health_checked"), Result.bChecked);
    Object->SetBoolField(TEXT("editor_log_health_pass"), Result.bPass);
    Object->SetStringField(TEXT("editor_log_path"), Result.LogPath);
    Object->SetNumberField(TEXT("editor_log_issue_count"), Result.IssueCount);
    Object->SetNumberField(TEXT("editor_log_handled_ensure_count"), Result.HandledEnsureCount);
    Object->SetNumberField(TEXT("editor_log_ignored_handled_ensure_count"), Result.IgnoredHandledEnsureCount);
    Object->SetNumberField(TEXT("editor_log_fatal_error_count"), Result.FatalErrorCount);
    Object->SetNumberField(TEXT("editor_log_critical_error_count"), Result.CriticalErrorCount);
    Object->SetNumberField(TEXT("editor_log_force_delete_issue_count"), Result.ForceDeleteIssueCount);
    Object->SetNumberField(TEXT("editor_log_world_memory_leak_count"), Result.WorldMemoryLeakCount);
    AddStringArrayField(Object, TEXT("editor_log_issue_samples"), Result.IssueSamples);
}

bool IsBlueprintWidgetFallbackClassName(const FString& ClassName)
{
    return ClassName == TEXT("Blueprint") ||
        ClassName == TEXT("WidgetBlueprint") ||
        ClassName == TEXT("EditorUtilityBlueprint") ||
        ClassName == TEXT("EditorUtilityWidgetBlueprint");
}

TSharedPtr<FJsonObject> StringIntMapToJsonObject(const TMap<FString, int32>& Map)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    TArray<FString> Keys;
    Map.GetKeys(Keys);
    Keys.Sort();
    for (const FString& Key : Keys)
    {
        Object->SetNumberField(Key, Map.FindRef(Key));
    }
    return Object;
}

FString ProjectObjectPathOrEmpty(const UObject* Object)
{
    return Object ? Object->GetPathName() : FString();
}

TSharedPtr<FJsonObject> BlueprintStructureSummaryToJson(UBlueprint* Blueprint)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    if (!Blueprint)
    {
        Object->SetBoolField(TEXT("loaded"), false);
        return Object;
    }

    Object->SetBoolField(TEXT("loaded"), true);
    Object->SetStringField(TEXT("path"), Blueprint->GetPathName());
    Object->SetStringField(TEXT("class"), Blueprint->GetClass()->GetName());
    Object->SetStringField(TEXT("parent_class"), ProjectObjectPathOrEmpty(Blueprint->ParentClass));
    Object->SetStringField(TEXT("generated_class"), ProjectObjectPathOrEmpty(Blueprint->GeneratedClass));
    Object->SetStringField(TEXT("skeleton_generated_class"), ProjectObjectPathOrEmpty(Blueprint->SkeletonGeneratedClass));
    Object->SetNumberField(TEXT("new_variable_count"), Blueprint->NewVariables.Num());
    Object->SetNumberField(TEXT("generated_variable_count"), Blueprint->GeneratedVariables.Num());

    TArray<UEdGraph*> Graphs;
    Blueprint->GetAllGraphs(Graphs);
    TArray<TSharedPtr<FJsonValue>> GraphValues;
    TMap<FString, int32> NodeClassCounts;
    int32 TotalNodeCount = 0;
    int32 NonEmptyGraphCount = 0;
    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph)
        {
            continue;
        }

        TSharedPtr<FJsonObject> GraphObject = MakeShared<FJsonObject>();
        GraphObject->SetStringField(TEXT("name"), Graph->GetName());
        GraphObject->SetStringField(TEXT("class"), Graph->GetClass()->GetName());
        GraphObject->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
        GraphValues.Add(MakeShared<FJsonValueObject>(GraphObject));

        TotalNodeCount += Graph->Nodes.Num();
        if (Graph->Nodes.Num() > 0)
        {
            ++NonEmptyGraphCount;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node)
            {
                NodeClassCounts.FindOrAdd(Node->GetClass()->GetName())++;
            }
        }
    }
    Object->SetNumberField(TEXT("graph_count"), Graphs.Num());
    Object->SetNumberField(TEXT("non_empty_graph_count"), NonEmptyGraphCount);
    Object->SetNumberField(TEXT("total_node_count"), TotalNodeCount);
    Object->SetObjectField(TEXT("node_class_counts"), StringIntMapToJsonObject(NodeClassCounts));
    Object->SetArrayField(TEXT("graphs"), GraphValues);

    int32 SCSNodeCount = 0;
    TMap<FString, int32> SCSComponentClassCounts;
    if (Blueprint->SimpleConstructionScript)
    {
        const TArray<USCS_Node*>& Nodes = Blueprint->SimpleConstructionScript->GetAllNodes();
        SCSNodeCount = Nodes.Num();
        for (USCS_Node* Node : Nodes)
        {
            if (Node && Node->ComponentClass)
            {
                SCSComponentClassCounts.FindOrAdd(Node->ComponentClass->GetName())++;
            }
        }
    }
    Object->SetNumberField(TEXT("scs_node_count"), SCSNodeCount);
    Object->SetObjectField(TEXT("scs_component_class_counts"), StringIntMapToJsonObject(SCSComponentClassCounts));

    int32 WidgetCount = 0;
    FString RootWidgetClass;
    TMap<FString, int32> WidgetClassCounts;
    if (UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint))
    {
        if (WidgetBlueprint->WidgetTree)
        {
            TArray<UWidget*> Widgets;
            WidgetBlueprint->WidgetTree->GetAllWidgets(Widgets);
            WidgetCount = Widgets.Num();
            if (WidgetBlueprint->WidgetTree->RootWidget)
            {
                RootWidgetClass = WidgetBlueprint->WidgetTree->RootWidget->GetClass()->GetName();
            }
            for (UWidget* Widget : Widgets)
            {
                if (Widget)
                {
                    WidgetClassCounts.FindOrAdd(Widget->GetClass()->GetName())++;
                }
            }
        }
    }
    Object->SetNumberField(TEXT("widget_count"), WidgetCount);
    Object->SetStringField(TEXT("root_widget_class"), RootWidgetClass);
    Object->SetObjectField(TEXT("widget_class_counts"), StringIntMapToJsonObject(WidgetClassCounts));

    const bool bHasSerializedGraphWork = TotalNodeCount > 0 || SCSNodeCount > 0 || WidgetCount > 0 || Blueprint->NewVariables.Num() > 0;
    Object->SetBoolField(TEXT("data_only_like"), !bHasSerializedGraphWork);
    Object->SetStringField(
        TEXT("fresh_recreation_risk"),
        bHasSerializedGraphWork ? TEXT("high_serialized_graph_or_widget_state") : TEXT("low_data_only_like"));
    return Object;
}

int32 GetJsonIntField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
    return Object.IsValid() && Object->HasTypedField<EJson::Number>(FieldName)
        ? static_cast<int32>(Object->GetNumberField(FieldName))
        : 0;
}

bool CompareBlueprintStructureSummaries(
    const TSharedPtr<FJsonObject>& SourceSummary,
    const TSharedPtr<FJsonObject>& TargetSummary,
    TArray<FString>& OutMismatchFields)
{
    static const TArray<FString> CountFields = {
        TEXT("graph_count"),
        TEXT("non_empty_graph_count"),
        TEXT("total_node_count"),
        TEXT("new_variable_count"),
        TEXT("generated_variable_count"),
        TEXT("scs_node_count"),
        TEXT("widget_count")
    };

    for (const FString& FieldName : CountFields)
    {
        if (GetJsonIntField(SourceSummary, FieldName) != GetJsonIntField(TargetSummary, FieldName))
        {
            OutMismatchFields.Add(FieldName);
        }
    }

    return OutMismatchFields.IsEmpty();
}
}

FUnrealMCPProjectCommands::FUnrealMCPProjectCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("create_input_mapping"))
    {
        return HandleCreateInputMapping(Params);
    }
    if (CommandType == TEXT("execute_python"))
    {
        return HandleExecutePython(Params);
    }
    if (CommandType == TEXT("recreate_content_folder_mcp"))
    {
        return HandleRecreateContentFolderMCP(Params);
    }
    if (CommandType == TEXT("postprocess_content_folder_mcp"))
    {
        return HandlePostprocessContentFolderMCP(Params);
    }
    if (CommandType == TEXT("repair_world_actor_instances_mcp"))
    {
        return HandleRepairWorldActorInstancesMCP(Params);
    }
    if (CommandType == TEXT("run_content_validation_pipeline_mcp"))
    {
        return HandleRunContentValidationPipelineMCP(Params);
    }
    if (CommandType == TEXT("analyze_blueprint_widget_fallbacks_mcp"))
    {
        return HandleAnalyzeBlueprintWidgetFallbacksMCP(Params);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown project command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleAnalyzeBlueprintWidgetFallbacksMCP(const TSharedPtr<FJsonObject>& Params)
{
    FString SourceRoot;
    FString TargetRoot;
    FString RootParamError;
    if (!TryGetRequiredContentRootParamForMCP(Params, TEXT("source_root"), SourceRoot, RootParamError) ||
        !TryGetRequiredContentRootParamForMCP(Params, TEXT("target_root"), TargetRoot, RootParamError) ||
        !ValidateContentRootPairForMCP(SourceRoot, TargetRoot, RootParamError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(RootParamError);
    }
    const FString Suffix = GetStringParam(Params, TEXT("suffix"), TEXT("_MCP"));

    if (!FPackageName::IsValidLongPackageName(SourceRoot) || !SourceRoot.StartsWith(TEXT("/Game/")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid source_root: %s"), *SourceRoot));
    }
    if (!FPackageName::IsValidLongPackageName(TargetRoot) || !TargetRoot.StartsWith(TEXT("/Game/")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid target_root: %s"), *TargetRoot));
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    TArray<FString> InitialScanPaths;
    InitialScanPaths.Add(SourceRoot);
    InitialScanPaths.Add(TargetRoot);
    AssetRegistry.ScanPathsSynchronous(InitialScanPaths, true);

    TArray<FAssetData> SourceAssets;
    AssetRegistry.GetAssetsByPath(FName(*SourceRoot), SourceAssets, true);
    SourceAssets.Sort([](const FAssetData& A, const FAssetData& B)
    {
        return A.PackageName.LexicalLess(B.PackageName);
    });

    int32 SourceAssetCount = 0;
    int32 PairedAssetCount = 0;
    int32 MissingTargetAssetCount = 0;
    int32 MismatchPairCount = 0;
    int32 DataOnlyLikeCount = 0;
    int32 HighRiskCount = 0;
    int32 TotalSourceGraphCount = 0;
    int32 TotalSourceNodeCount = 0;
    int32 TotalSourceSCSNodeCount = 0;
    int32 TotalSourceWidgetCount = 0;
    TMap<FString, int32> ClassCounts;
    TMap<FString, int32> HighRiskClassCounts;
    TArray<FString> MissingTargetSamples;
    TArray<FString> MismatchSamples;
    TArray<TSharedPtr<FJsonValue>> AssetValues;

    for (const FAssetData& SourceAsset : SourceAssets)
    {
        const FString ClassName = SourceAsset.AssetClassPath.GetAssetName().ToString();
        if (!IsBlueprintWidgetFallbackClassName(ClassName))
        {
            continue;
        }

        ++SourceAssetCount;
        ClassCounts.FindOrAdd(ClassName)++;

        const FString SourcePath = SourceAsset.PackageName.ToString();
        const FString TargetPath = BuildTargetAssetPath(SourceAsset, SourceRoot, TargetRoot, Suffix);
        UObject* SourceObject = SourceAsset.GetAsset();
        UObject* TargetObject = UEditorAssetLibrary::LoadAsset(TargetPath);
        UBlueprint* SourceBlueprint = Cast<UBlueprint>(SourceObject);
        UBlueprint* TargetBlueprint = Cast<UBlueprint>(TargetObject);

        TSharedPtr<FJsonObject> AssetObject = MakeShared<FJsonObject>();
        AssetObject->SetStringField(TEXT("source_path"), SourcePath);
        AssetObject->SetStringField(TEXT("target_path"), TargetPath);
        AssetObject->SetStringField(TEXT("class"), ClassName);
        AssetObject->SetBoolField(TEXT("target_exists"), TargetObject != nullptr);

        TSharedPtr<FJsonObject> SourceSummary = BlueprintStructureSummaryToJson(SourceBlueprint);
        TSharedPtr<FJsonObject> TargetSummary = BlueprintStructureSummaryToJson(TargetBlueprint);
        AssetObject->SetObjectField(TEXT("source"), SourceSummary);
        AssetObject->SetObjectField(TEXT("target"), TargetSummary);

        TotalSourceGraphCount += GetJsonIntField(SourceSummary, TEXT("graph_count"));
        TotalSourceNodeCount += GetJsonIntField(SourceSummary, TEXT("total_node_count"));
        TotalSourceSCSNodeCount += GetJsonIntField(SourceSummary, TEXT("scs_node_count"));
        TotalSourceWidgetCount += GetJsonIntField(SourceSummary, TEXT("widget_count"));

        bool bDataOnlyLike = false;
        SourceSummary->TryGetBoolField(TEXT("data_only_like"), bDataOnlyLike);
        if (bDataOnlyLike)
        {
            ++DataOnlyLikeCount;
            AssetObject->SetStringField(TEXT("fresh_recreation_recommendation"), TEXT("candidate_after_explicit_single_asset_test"));
        }
        else
        {
            ++HighRiskCount;
            HighRiskClassCounts.FindOrAdd(ClassName)++;
            AssetObject->SetStringField(TEXT("fresh_recreation_recommendation"), TEXT("keep_fallback_duplicate_until_graph_rebuild_support_exists"));
        }

        if (!TargetBlueprint)
        {
            ++MissingTargetAssetCount;
            if (MissingTargetSamples.Num() < 50)
            {
                MissingTargetSamples.Add(TargetPath);
            }
        }
        else
        {
            ++PairedAssetCount;
            TArray<FString> MismatchFields;
            const bool bStructureMatches = CompareBlueprintStructureSummaries(SourceSummary, TargetSummary, MismatchFields);
            AssetObject->SetBoolField(TEXT("structure_match"), bStructureMatches);
            AddStringArrayField(AssetObject, TEXT("structure_mismatch_fields"), MismatchFields);
            if (!bStructureMatches)
            {
                ++MismatchPairCount;
                if (MismatchSamples.Num() < 50)
                {
                    MismatchSamples.Add(FString::Printf(TEXT("%s -> %s (%s)"), *SourcePath, *TargetPath, *FString::Join(MismatchFields, TEXT(","))));
                }
            }
        }

        AssetValues.Add(MakeShared<FJsonValueObject>(AssetObject));
    }

    TSharedPtr<FJsonObject> ReportObject = MakeShared<FJsonObject>();
    ReportObject->SetStringField(TEXT("operation"), TEXT("analyze_blueprint_widget_fallbacks_mcp"));
    ReportObject->SetStringField(TEXT("source_root"), SourceRoot);
    ReportObject->SetStringField(TEXT("target_root"), TargetRoot);
    ReportObject->SetStringField(TEXT("suffix"), Suffix);
    ReportObject->SetNumberField(TEXT("source_asset_count"), SourceAssetCount);
    ReportObject->SetNumberField(TEXT("paired_asset_count"), PairedAssetCount);
    ReportObject->SetNumberField(TEXT("missing_target_asset_count"), MissingTargetAssetCount);
    ReportObject->SetNumberField(TEXT("mismatch_pair_count"), MismatchPairCount);
    ReportObject->SetNumberField(TEXT("data_only_like_count"), DataOnlyLikeCount);
    ReportObject->SetNumberField(TEXT("high_risk_count"), HighRiskCount);
    ReportObject->SetNumberField(TEXT("total_source_graph_count"), TotalSourceGraphCount);
    ReportObject->SetNumberField(TEXT("total_source_node_count"), TotalSourceNodeCount);
    ReportObject->SetNumberField(TEXT("total_source_scs_node_count"), TotalSourceSCSNodeCount);
    ReportObject->SetNumberField(TEXT("total_source_widget_count"), TotalSourceWidgetCount);
    ReportObject->SetObjectField(TEXT("class_counts"), StringIntMapToJsonObject(ClassCounts));
    ReportObject->SetObjectField(TEXT("high_risk_class_counts"), StringIntMapToJsonObject(HighRiskClassCounts));
    AddStringArrayField(ReportObject, TEXT("missing_target_samples"), MissingTargetSamples);
    AddStringArrayField(ReportObject, TEXT("mismatch_samples"), MismatchSamples);
    ReportObject->SetArrayField(TEXT("assets"), AssetValues);
    ReportObject->SetBoolField(TEXT("verification_pass"), MissingTargetAssetCount == 0 && MismatchPairCount == 0);
    ReportObject->SetStringField(
        TEXT("fresh_recreation_conclusion"),
        HighRiskCount > 0
            ? TEXT("Blueprint/Widget fresh recreation should stay disabled for high-risk serialized graph/widget assets until graph, SCS, and WidgetTree rebuild support is implemented.")
            : TEXT("Only data-only-like Blueprint assets were found; fresh recreation can be tested per asset."));

    FString ReportJson;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ReportJson);
    FJsonSerializer::Serialize(ReportObject.ToSharedRef(), Writer);

    const FString ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MCP"));
    IFileManager::Get().MakeDirectory(*ReportDirectory, true);
    const FString ReportPath = FPaths::Combine(ReportDirectory, BuildMCPReportFilename(TargetRoot, TEXT("blueprint_widget_diagnostic")));
    FFileHelper::SaveStringToFile(ReportJson, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetBoolField(TEXT("verification_pass"), MissingTargetAssetCount == 0 && MismatchPairCount == 0);
    ResultObj->SetStringField(TEXT("report_path"), ReportPath);
    ResultObj->SetNumberField(TEXT("source_asset_count"), SourceAssetCount);
    ResultObj->SetNumberField(TEXT("paired_asset_count"), PairedAssetCount);
    ResultObj->SetNumberField(TEXT("missing_target_asset_count"), MissingTargetAssetCount);
    ResultObj->SetNumberField(TEXT("mismatch_pair_count"), MismatchPairCount);
    ResultObj->SetNumberField(TEXT("data_only_like_count"), DataOnlyLikeCount);
    ResultObj->SetNumberField(TEXT("high_risk_count"), HighRiskCount);
    ResultObj->SetNumberField(TEXT("total_source_graph_count"), TotalSourceGraphCount);
    ResultObj->SetNumberField(TEXT("total_source_node_count"), TotalSourceNodeCount);
    ResultObj->SetNumberField(TEXT("total_source_scs_node_count"), TotalSourceSCSNodeCount);
    ResultObj->SetNumberField(TEXT("total_source_widget_count"), TotalSourceWidgetCount);
    ResultObj->SetObjectField(TEXT("class_counts"), StringIntMapToJsonObject(ClassCounts));
    ResultObj->SetObjectField(TEXT("high_risk_class_counts"), StringIntMapToJsonObject(HighRiskClassCounts));
    AddStringArrayField(ResultObj, TEXT("missing_target_samples"), MissingTargetSamples);
    AddStringArrayField(ResultObj, TEXT("mismatch_samples"), MismatchSamples);
    ResultObj->SetStringField(TEXT("fresh_recreation_conclusion"), ReportObject->GetStringField(TEXT("fresh_recreation_conclusion")));
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleRunContentValidationPipelineMCP(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing parameters"));
    }

    FString SourceRoot;
    FString TargetRoot;
    FString RootParamError;
    if (!TryGetRequiredContentRootParamForMCP(Params, TEXT("source_root"), SourceRoot, RootParamError) ||
        !TryGetRequiredContentRootParamForMCP(Params, TEXT("target_root"), TargetRoot, RootParamError) ||
        !ValidateContentRootPairForMCP(SourceRoot, TargetRoot, RootParamError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(RootParamError);
    }

    const FString Suffix = GetStringParam(Params, TEXT("suffix"), TEXT("_MCP"));
    const FString MapPath = NormalizePackagePathParam(GetStringParam(Params, TEXT("map_path"), FString()));
    const bool bContinueOnFailure = GetBoolParam(Params, TEXT("continue_on_failure"), false);
    const bool bRunWorldRepair = GetBoolParam(Params, TEXT("run_world_repair"), !MapPath.IsEmpty());

    if (bRunWorldRepair)
    {
        if (MapPath.IsEmpty() || !FPackageName::IsValidLongPackageName(MapPath))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("run_world_repair requires a valid map_path"));
        }
        if (!MapPath.Equals(TargetRoot) && !MapPath.StartsWith(TargetRoot + TEXT("/")))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("map_path must be under target_root"));
        }
    }

    TArray<TSharedPtr<FJsonValue>> PipelineStepValues;

    TSharedPtr<FJsonObject> RecreateParams = CloneJsonObjectForMCP(Params);
    RecreateParams->SetStringField(TEXT("source_root"), SourceRoot);
    RecreateParams->SetStringField(TEXT("target_root"), TargetRoot);
    RecreateParams->SetStringField(TEXT("suffix"), Suffix);
    SetBoolFieldIfMissingForMCP(RecreateParams, TEXT("allow_fallback_duplicate"), true);
    SetBoolFieldIfMissingForMCP(RecreateParams, TEXT("dry_run"), false);
    SetBoolFieldIfMissingForMCP(RecreateParams, TEXT("overwrite_existing"), true);
    SetBoolFieldIfMissingForMCP(RecreateParams, TEXT("delete_target_root_first"), true);
    SetBoolFieldIfMissingForMCP(RecreateParams, TEXT("remap_references"), true);
    SetBoolFieldIfMissingForMCP(RecreateParams, TEXT("compile_blueprints"), true);
    SetNumberFieldIfMissingForMCP(RecreateParams, TEXT("compile_passes"), 3);
    SetBoolFieldIfMissingForMCP(RecreateParams, TEXT("save_assets"), true);
    SetBoolFieldIfMissingForMCP(RecreateParams, TEXT("repair_level_actors"), true);
    SetBoolFieldIfMissingForMCP(RecreateParams, TEXT("force_repair_level_actors"), true);
    SetBoolFieldIfMissingForMCP(RecreateParams, TEXT("check_editor_log_health"), true);
    SetBoolFieldIfMissingForMCP(RecreateParams, TEXT("fail_on_editor_log_issues"), true);
    SetBoolFieldIfMissingForMCP(RecreateParams, TEXT("suppress_editor_prompts"), true);

    TSharedPtr<FJsonObject> RecreateResult = HandleRecreateContentFolderMCP(RecreateParams);
    const bool bRecreatePass = PipelineStepResultPassesForMCP(RecreateResult);
    PipelineStepValues.Add(MakeShared<FJsonValueObject>(MakePipelineStepSummaryForMCP(
        TEXT("recreate"),
        true,
        RecreateResult)));

    const bool bCanRunPostprocess = bRecreatePass || bContinueOnFailure;
    TSharedPtr<FJsonObject> PostprocessResult;
    bool bPostprocessPass = false;
    if (bCanRunPostprocess)
    {
        TSharedPtr<FJsonObject> PostprocessParams = CloneJsonObjectForMCP(Params);
        PostprocessParams->SetStringField(TEXT("source_root"), SourceRoot);
        PostprocessParams->SetStringField(TEXT("target_root"), TargetRoot);
        PostprocessParams->SetStringField(TEXT("suffix"), Suffix);
        SetBoolFieldIfMissingForMCP(PostprocessParams, TEXT("dry_run"), false);
        SetBoolFieldIfMissingForMCP(PostprocessParams, TEXT("remap_references"), true);
        SetBoolFieldIfMissingForMCP(PostprocessParams, TEXT("compile_blueprints"), true);
        SetNumberFieldIfMissingForMCP(PostprocessParams, TEXT("compile_passes"), 3);
        SetBoolFieldIfMissingForMCP(PostprocessParams, TEXT("save_assets"), true);
        SetBoolFieldIfMissingForMCP(PostprocessParams, TEXT("repair_level_actors"), false);
        SetBoolFieldIfMissingForMCP(PostprocessParams, TEXT("check_editor_log_health"), true);
        SetBoolFieldIfMissingForMCP(PostprocessParams, TEXT("fail_on_editor_log_issues"), true);
        SetBoolFieldIfMissingForMCP(PostprocessParams, TEXT("suppress_editor_prompts"), true);

        PostprocessResult = HandlePostprocessContentFolderMCP(PostprocessParams);
        bPostprocessPass = PipelineStepResultPassesForMCP(PostprocessResult);
        PipelineStepValues.Add(MakeShared<FJsonValueObject>(MakePipelineStepSummaryForMCP(
            TEXT("postprocess"),
            true,
            PostprocessResult)));
    }
    else
    {
        PipelineStepValues.Add(MakeShared<FJsonValueObject>(MakePipelineStepSummaryForMCP(
            TEXT("postprocess"),
            false,
            nullptr,
            TEXT("recreate_failed"))));
    }

    TSharedPtr<FJsonObject> WorldRepairResult;
    bool bWorldRepairPass = !bRunWorldRepair;
    if (bRunWorldRepair)
    {
        const bool bCanRunWorldRepair = (bPostprocessPass || bContinueOnFailure) && bCanRunPostprocess;
        if (bCanRunWorldRepair)
        {
            TSharedPtr<FJsonObject> WorldRepairParams = CloneJsonObjectForMCP(Params);
            WorldRepairParams->SetStringField(TEXT("source_root"), SourceRoot);
            WorldRepairParams->SetStringField(TEXT("target_root"), TargetRoot);
            WorldRepairParams->SetStringField(TEXT("suffix"), Suffix);
            WorldRepairParams->SetStringField(TEXT("map_path"), MapPath);
            SetBoolFieldIfMissingForMCP(WorldRepairParams, TEXT("dry_run"), false);
            SetBoolFieldIfMissingForMCP(WorldRepairParams, TEXT("save_map"), true);
            SetBoolFieldIfMissingForMCP(WorldRepairParams, TEXT("remap_actor_references"), true);
            SetBoolFieldIfMissingForMCP(WorldRepairParams, TEXT("repair_actor_classes"), true);
            SetBoolFieldIfMissingForMCP(WorldRepairParams, TEXT("repair_component_classes"), true);
            SetBoolFieldIfMissingForMCP(WorldRepairParams, TEXT("remove_source_object_map_keys"), true);
            SetBoolFieldIfMissingForMCP(WorldRepairParams, TEXT("check_editor_log_health"), true);
            SetBoolFieldIfMissingForMCP(WorldRepairParams, TEXT("fail_on_editor_log_issues"), true);
            SetBoolFieldIfMissingForMCP(WorldRepairParams, TEXT("suppress_editor_prompts"), true);

            WorldRepairResult = HandleRepairWorldActorInstancesMCP(WorldRepairParams);
            bWorldRepairPass = PipelineStepResultPassesForMCP(WorldRepairResult);
            PipelineStepValues.Add(MakeShared<FJsonValueObject>(MakePipelineStepSummaryForMCP(
                TEXT("world_repair"),
                true,
                WorldRepairResult)));
        }
        else
        {
            PipelineStepValues.Add(MakeShared<FJsonValueObject>(MakePipelineStepSummaryForMCP(
                TEXT("world_repair"),
                false,
                nullptr,
                TEXT("previous_step_failed"))));
        }
    }
    else
    {
        PipelineStepValues.Add(MakeShared<FJsonValueObject>(MakePipelineStepSummaryForMCP(
            TEXT("world_repair"),
            false,
            nullptr,
            TEXT("map_path_not_supplied_or_run_world_repair_false"))));
    }

    const bool bVerificationPass = bRecreatePass && bPostprocessPass && (!bRunWorldRepair || bWorldRepairPass);
    const int32 TotalBlueprintCompileErrors =
        GetJsonIntFieldForMCP(RecreateResult, TEXT("blueprint_compile_error_count"))
        + GetJsonIntFieldForMCP(PostprocessResult, TEXT("blueprint_compile_error_count"));
    const int32 TotalEditorLogIssueCount =
        GetJsonIntFieldForMCP(RecreateResult, TEXT("editor_log_issue_count"))
        + GetJsonIntFieldForMCP(PostprocessResult, TEXT("editor_log_issue_count"))
        + GetJsonIntFieldForMCP(WorldRepairResult, TEXT("editor_log_issue_count"));

    TSharedPtr<FJsonObject> ReportObject = MakeShared<FJsonObject>();
    ReportObject->SetStringField(TEXT("operation"), TEXT("run_content_validation_pipeline_mcp"));
    ReportObject->SetBoolField(TEXT("success"), true);
    ReportObject->SetBoolField(TEXT("verification_pass"), bVerificationPass);
    ReportObject->SetStringField(TEXT("source_root"), SourceRoot);
    ReportObject->SetStringField(TEXT("target_root"), TargetRoot);
    ReportObject->SetStringField(TEXT("suffix"), Suffix);
    ReportObject->SetBoolField(TEXT("continue_on_failure"), bContinueOnFailure);
    ReportObject->SetBoolField(TEXT("run_world_repair"), bRunWorldRepair);
    if (!MapPath.IsEmpty())
    {
        ReportObject->SetStringField(TEXT("map_path"), MapPath);
    }
    ReportObject->SetArrayField(TEXT("pipeline_steps"), PipelineStepValues);
    ReportObject->SetNumberField(TEXT("blueprint_compile_error_count"), TotalBlueprintCompileErrors);
    ReportObject->SetNumberField(TEXT("editor_log_issue_count"), TotalEditorLogIssueCount);
    CopyNumberFieldIfPresentForMCP(RecreateResult, ReportObject, TEXT("accepted_fallback_rule_count"), TEXT("accepted_fallback_rule_count"));
    CopyNumberFieldIfPresentForMCP(RecreateResult, ReportObject, TEXT("accepted_fallback_count"), TEXT("accepted_fallback_count"));
    CopyNumberFieldIfPresentForMCP(RecreateResult, ReportObject, TEXT("unresolved_fallback_count"), TEXT("unresolved_fallback_count"));
    CopyNumberFieldIfPresentForMCP(RecreateResult, ReportObject, TEXT("fresh_create_fallback_count"), TEXT("fresh_create_fallback_count"));
    CopyNumberFieldIfPresentForMCP(RecreateResult, ReportObject, TEXT("original_dependency_asset_count"), TEXT("recreate_original_dependency_asset_count"));
    CopyNumberFieldIfPresentForMCP(PostprocessResult, ReportObject, TEXT("original_dependency_asset_count"), TEXT("postprocess_original_dependency_asset_count"));
    CopyNumberFieldIfPresentForMCP(PostprocessResult, ReportObject, TEXT("missing_target_asset_count"), TEXT("postprocess_missing_target_asset_count"));
    CopyNumberFieldIfPresentForMCP(WorldRepairResult, ReportObject, TEXT("source_hard_dependency_count"), TEXT("world_repair_source_hard_dependency_count"));
    CopyNumberFieldIfPresentForMCP(WorldRepairResult, ReportObject, TEXT("after_source_actor_count"), TEXT("world_repair_after_source_actor_count"));
    CopyNumberFieldIfPresentForMCP(WorldRepairResult, ReportObject, TEXT("after_source_component_count"), TEXT("world_repair_after_source_component_count"));
    if (RecreateResult.IsValid())
    {
        ReportObject->SetObjectField(TEXT("recreate_result"), RecreateResult);
    }
    if (PostprocessResult.IsValid())
    {
        ReportObject->SetObjectField(TEXT("postprocess_result"), PostprocessResult);
    }
    if (WorldRepairResult.IsValid())
    {
        ReportObject->SetObjectField(TEXT("world_repair_result"), WorldRepairResult);
    }
    if (!bVerificationPass)
    {
        ReportObject->SetStringField(TEXT("verification_error"), TEXT("Content validation pipeline failed one or more verification steps"));
    }

    const FString ReportPath = SaveJsonReportForMCP(
        ReportObject,
        BuildMCPReportFilename(TargetRoot, TEXT("validation_pipeline")));

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetBoolField(TEXT("verification_pass"), bVerificationPass);
    ResultObj->SetStringField(TEXT("source_root"), SourceRoot);
    ResultObj->SetStringField(TEXT("target_root"), TargetRoot);
    ResultObj->SetStringField(TEXT("suffix"), Suffix);
    ResultObj->SetBoolField(TEXT("continue_on_failure"), bContinueOnFailure);
    ResultObj->SetBoolField(TEXT("run_world_repair"), bRunWorldRepair);
    if (!MapPath.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("map_path"), MapPath);
    }
    ResultObj->SetStringField(TEXT("report_path"), ReportPath);
    ResultObj->SetArrayField(TEXT("pipeline_steps"), PipelineStepValues);
    ResultObj->SetNumberField(TEXT("blueprint_compile_error_count"), TotalBlueprintCompileErrors);
    ResultObj->SetNumberField(TEXT("editor_log_issue_count"), TotalEditorLogIssueCount);
    CopyNumberFieldIfPresentForMCP(RecreateResult, ResultObj, TEXT("accepted_fallback_rule_count"), TEXT("accepted_fallback_rule_count"));
    CopyNumberFieldIfPresentForMCP(RecreateResult, ResultObj, TEXT("accepted_fallback_count"), TEXT("accepted_fallback_count"));
    CopyNumberFieldIfPresentForMCP(RecreateResult, ResultObj, TEXT("unresolved_fallback_count"), TEXT("unresolved_fallback_count"));
    CopyNumberFieldIfPresentForMCP(RecreateResult, ResultObj, TEXT("fresh_create_fallback_count"), TEXT("fresh_create_fallback_count"));
    CopyNumberFieldIfPresentForMCP(RecreateResult, ResultObj, TEXT("original_dependency_asset_count"), TEXT("recreate_original_dependency_asset_count"));
    CopyNumberFieldIfPresentForMCP(PostprocessResult, ResultObj, TEXT("original_dependency_asset_count"), TEXT("postprocess_original_dependency_asset_count"));
    CopyNumberFieldIfPresentForMCP(PostprocessResult, ResultObj, TEXT("missing_target_asset_count"), TEXT("postprocess_missing_target_asset_count"));
    CopyNumberFieldIfPresentForMCP(WorldRepairResult, ResultObj, TEXT("source_hard_dependency_count"), TEXT("world_repair_source_hard_dependency_count"));
    CopyNumberFieldIfPresentForMCP(WorldRepairResult, ResultObj, TEXT("after_source_actor_count"), TEXT("world_repair_after_source_actor_count"));
    CopyNumberFieldIfPresentForMCP(WorldRepairResult, ResultObj, TEXT("after_source_component_count"), TEXT("world_repair_after_source_component_count"));
    if (RecreateResult.IsValid())
    {
        ResultObj->SetObjectField(TEXT("recreate_result"), RecreateResult);
    }
    if (PostprocessResult.IsValid())
    {
        ResultObj->SetObjectField(TEXT("postprocess_result"), PostprocessResult);
    }
    if (WorldRepairResult.IsValid())
    {
        ResultObj->SetObjectField(TEXT("world_repair_result"), WorldRepairResult);
    }
    if (!bVerificationPass)
    {
        ResultObj->SetStringField(TEXT("verification_error"), TEXT("Content validation pipeline failed one or more verification steps"));
    }
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleCreateInputMapping(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString ActionName;
    if (!Params->TryGetStringField(TEXT("action_name"), ActionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'action_name' parameter"));
    }

    FString Key;
    if (!Params->TryGetStringField(TEXT("key"), Key))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'key' parameter"));
    }

    // Get the input settings
    UInputSettings* InputSettings = GetMutableDefault<UInputSettings>();
    if (!InputSettings)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get input settings"));
    }

    // Create the input action mapping
    FInputActionKeyMapping ActionMapping;
    ActionMapping.ActionName = FName(*ActionName);
    ActionMapping.Key = FKey(*Key);

    // Add modifiers if provided
    if (Params->HasField(TEXT("shift")))
    {
        ActionMapping.bShift = Params->GetBoolField(TEXT("shift"));
    }
    if (Params->HasField(TEXT("ctrl")))
    {
        ActionMapping.bCtrl = Params->GetBoolField(TEXT("ctrl"));
    }
    if (Params->HasField(TEXT("alt")))
    {
        ActionMapping.bAlt = Params->GetBoolField(TEXT("alt"));
    }
    if (Params->HasField(TEXT("cmd")))
    {
        ActionMapping.bCmd = Params->GetBoolField(TEXT("cmd"));
    }

    // Add the mapping
    InputSettings->AddActionMapping(ActionMapping);
    InputSettings->SaveConfig();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("action_name"), ActionName);
    ResultObj->SetStringField(TEXT("key"), Key);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleRecreateContentFolderMCP(const TSharedPtr<FJsonObject>& Params)
{
    FString SourceRoot;
    FString TargetRoot;
    FString RootParamError;
    if (!TryGetRequiredContentRootParamForMCP(Params, TEXT("source_root"), SourceRoot, RootParamError) ||
        !TryGetRequiredContentRootParamForMCP(Params, TEXT("target_root"), TargetRoot, RootParamError) ||
        !ValidateContentRootPairForMCP(SourceRoot, TargetRoot, RootParamError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(RootParamError);
    }
    const FString Suffix = GetStringParam(Params, TEXT("suffix"), TEXT("_MCP"));
    const TArray<FAcceptedFallbackRule> AcceptedFallbackRules = ParseAcceptedFallbackRulesForMCP(Params);
    const bool bAllowFallbackDuplicate = GetBoolParam(Params, TEXT("allow_fallback_duplicate"), true);
    const bool bDryRun = GetBoolParam(Params, TEXT("dry_run"), false);
    const bool bOverwriteExisting = GetBoolParam(Params, TEXT("overwrite_existing"), false);
    const bool bDeleteTargetRootFirst = GetBoolParam(Params, TEXT("delete_target_root_first"), false);
    const bool bRemapReferences = GetBoolParam(Params, TEXT("remap_references"), true);
    const bool bCompileBlueprints = GetBoolParam(Params, TEXT("compile_blueprints"), true);
    const bool bLogArchiveRemapObjects = GetBoolParam(Params, TEXT("log_archive_remap_objects"), false);
    const bool bRefreshBlueprintNodes = GetBoolParam(Params, TEXT("refresh_blueprint_nodes"), false);
    const bool bRefreshFailedBlueprintNodes = GetBoolParam(Params, TEXT("refresh_failed_blueprint_nodes"), true);
    const bool bSaveAssets = GetBoolParam(Params, TEXT("save_assets"), true);
    const bool bRepairLevelActors = GetBoolParam(Params, TEXT("repair_level_actors"), true);
    const bool bForceRepairLevelActors = GetBoolParam(Params, TEXT("force_repair_level_actors"), true);
    const int32 CompilePasses = FMath::Max(1, GetIntParam(Params, TEXT("compile_passes"), 2));
    const bool bCheckEditorLogHealth = GetBoolParam(Params, TEXT("check_editor_log_health"), true);
    const bool bFailOnEditorLogIssues = GetBoolParam(Params, TEXT("fail_on_editor_log_issues"), true);
    const bool bSuppressEditorPrompts = GetBoolParam(Params, TEXT("suppress_editor_prompts"), true);
    TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, GIsRunningUnattendedScript || bSuppressEditorPrompts);
    const FEditorLogMarker EditorLogMarker = CaptureEditorLogMarker(bCheckEditorLogHealth);

    if (!FPackageName::IsValidLongPackageName(SourceRoot) || !SourceRoot.StartsWith(TEXT("/Game/")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid source_root: %s"), *SourceRoot));
    }

    if (!FPackageName::IsValidLongPackageName(TargetRoot) || !TargetRoot.StartsWith(TEXT("/Game/")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid target_root: %s"), *TargetRoot));
    }

    if (TargetRoot.Equals(SourceRoot) || TargetRoot.StartsWith(SourceRoot + TEXT("/")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("target_root must not be the source root or a child of source_root"));
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    TArray<FAssetData> SourceAssets;
    AssetRegistry.GetAssetsByPath(FName(*SourceRoot), SourceAssets, true);
    SourceAssets.Sort([](const FAssetData& A, const FAssetData& B)
    {
        const int32 APriority = GetRecreateCreationPriority(A);
        const int32 BPriority = GetRecreateCreationPriority(B);
        if (APriority != BPriority)
        {
            return APriority < BPriority;
        }
        return A.PackageName.LexicalLess(B.PackageName);
    });

    if (SourceAssets.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("No source assets found under %s"), *SourceRoot));
    }

    TSharedPtr<FJsonObject> ReportObject = MakeShared<FJsonObject>();
    ReportObject->SetStringField(TEXT("source_root"), SourceRoot);
    ReportObject->SetStringField(TEXT("target_root"), TargetRoot);
    ReportObject->SetStringField(TEXT("suffix"), Suffix);
    ReportObject->SetBoolField(TEXT("dry_run"), bDryRun);

    TArray<TSharedPtr<FJsonValue>> AssetReports;
    TMap<FString, int32> ClassCounts;
    TMap<UObject*, UObject*> ReplacementMap;
    TArray<FPathStringReplacement> PathReplacements;
    TArray<TPair<UObject*, UObject*>> SourceTargetPairs;
    TArray<UObject*> TargetObjects;
    TArray<UBlueprint*> TargetBlueprints;
    TArray<UUserDefinedStruct*> TargetUserDefinedStructs;
    TArray<FName> TargetPackageNames;
    TArray<FString> TargetWorldPackageNames;
    TSet<UPackage*> TargetPackages;
    TArray<FDeferredGeneratedClassAsset> DeferredGeneratedClassAssets;
    TArray<FString> FailedAssets;
    TArray<FString> SkippedExistingAssets;
    TArray<FString> DeleteTargetRootWarnings;
    FString DeleteTargetRootMode = TEXT("none");

    int32 FallbackDuplicateCount = 0;
    int32 CreatedCount = 0;
    int32 SavedCount = 0;
    int32 RemappedObjectCount = 0;
    int32 ArchiveObjectRemapSkipCount = 0;
    int32 PathPropertyRemapCount = 0;
    int32 GeneratedClassAssetRecreateCount = 0;
    int32 FreshPropertyCopyAssetCount = 0;
    int32 FreshGeneratedClassAssetCount = 0;
    int32 FreshMaterialAssetCount = 0;
    int32 FreshMaterialFunctionAssetCount = 0;
    int32 FreshMaterialExpressionMappingCount = 0;
    int32 FreshTexture2DAssetCount = 0;
    int32 FreshTextureCubeAssetCount = 0;
    int32 FreshVolumeTextureAssetCount = 0;
    int32 FreshStaticMeshAssetCount = 0;
    int32 FreshSoundWaveAssetCount = 0;
    int32 FreshDataTableAssetCount = 0;
    int32 FreshUserDefinedEnumAssetCount = 0;
    int32 FreshUserDefinedStructAssetCount = 0;
    int32 FreshCreateFallbackCount = 0;
    int32 AcceptedFallbackCount = 0;
    int32 UnresolvedFallbackCount = 0;
    int32 UserDefinedStructDescriptionPathRemapCount = 0;
    int32 UserDefinedStructCompileErrorCount = 0;
    int32 SCSNodeRepairCount = 0;
    int32 WidgetTreeRepairCount = 0;
    int32 GeneratedClassSubobjectRepairCount = 0;
    int32 MaterialTextureReferenceRepairCount = 0;
    int32 MaterialCacheRefreshCount = 0;
    int32 LevelActorReferenceRemapCount = 0;
    int32 LevelActorClassRepairCount = 0;
    int32 LevelComponentClassRepairCount = 0;
    int32 SavedMapCount = 0;
    int32 SkippedCleanWorldRepairCount = 0;
    int32 BlueprintGraphReferenceRemapCount = 0;
    int32 CompiledBlueprintCount = 0;
    int32 BlueprintCompileErrorCount = 0;
    TArray<FString> AcceptedFallbackSamples;
    TArray<FString> UnresolvedFallbackSamples;

    if (!bDryRun)
    {
        if (bDeleteTargetRootFirst && UEditorAssetLibrary::DoesDirectoryExist(TargetRoot))
        {
            if (!DeleteTargetRootForRecreate(TargetRoot, DeleteTargetRootMode, DeleteTargetRootWarnings))
            {
                TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
                ResultObj->SetBoolField(TEXT("success"), false);
                ResultObj->SetStringField(TEXT("source_root"), SourceRoot);
                ResultObj->SetStringField(TEXT("target_root"), TargetRoot);
                ResultObj->SetStringField(TEXT("delete_target_root_mode"), DeleteTargetRootMode);
                AddStringArrayField(ResultObj, TEXT("delete_target_root_warnings"), DeleteTargetRootWarnings);
                ResultObj->SetStringField(TEXT("message"), TEXT("Failed to prepare target_root for deletion. Restart the editor or unload target packages, then rerun."));
                return ResultObj;
            }
            TArray<FString> RescanPaths;
            RescanPaths.Add(TargetRoot);
            AssetRegistry.ScanPathsSynchronous(RescanPaths, true);
        }
        UEditorAssetLibrary::MakeDirectory(TargetRoot);
    }

    for (const FAssetData& SourceAsset : SourceAssets)
    {
        const FString ClassName = SourceAsset.AssetClassPath.GetAssetName().ToString();
        ClassCounts.FindOrAdd(ClassName)++;

        const FString TargetPath = BuildTargetAssetPath(SourceAsset, SourceRoot, TargetRoot, Suffix);
        const FString TargetDirectory = FPackageName::GetLongPackagePath(TargetPath);
        const bool bFreshPropertyCopyCandidate = IsFreshPropertyCopyAssetClassName(ClassName);
        const bool bFreshTexture2DCandidate = ClassName == TEXT("Texture2D");
        const bool bFreshTextureCubeCandidate = ClassName == TEXT("TextureCube");
        const bool bFreshVolumeTextureCandidate = ClassName == TEXT("VolumeTexture");
        const bool bFreshStaticMeshCandidate = ClassName == TEXT("StaticMesh");
        const bool bFreshSoundWaveCandidate = ClassName == TEXT("SoundWave");
        const bool bFreshMaterialCandidate = ClassName == TEXT("Material");
        const bool bFreshMaterialFunctionCandidate = ClassName == TEXT("MaterialFunction");
        const bool bFreshMaterialGraphCandidate = IsFreshMaterialGraphAssetClassName(ClassName);
        const bool bFreshDataTableCandidate = ClassName == TEXT("DataTable");
        const bool bFreshUserDefinedEnumCandidate = ClassName == TEXT("UserDefinedEnum");
        const bool bFreshUserDefinedStructCandidate = ClassName == TEXT("UserDefinedStruct");
        const bool bFreshCreateCandidate = bFreshPropertyCopyCandidate || bFreshTexture2DCandidate || bFreshTextureCubeCandidate || bFreshVolumeTextureCandidate || bFreshStaticMeshCandidate || bFreshSoundWaveCandidate || bFreshMaterialGraphCandidate || bFreshDataTableCandidate || bFreshUserDefinedEnumCandidate || bFreshUserDefinedStructCandidate;
        FString PlannedStrategy = TEXT("fallback_duplicate");
        FString PlannedDetail = GetFallbackReason(SourceAsset);
        if (bFreshTexture2DCandidate)
        {
            PlannedStrategy = TEXT("fresh_texture2d_source_copy");
            PlannedDetail = TEXT("fresh_texture2d_source_copy");
        }
        else if (bFreshTextureCubeCandidate)
        {
            PlannedStrategy = TEXT("fresh_texturecube_source_copy");
            PlannedDetail = TEXT("fresh_texturecube_source_copy");
        }
        else if (bFreshVolumeTextureCandidate)
        {
            PlannedStrategy = TEXT("fresh_volumetexture_source_copy");
            PlannedDetail = TEXT("fresh_volumetexture_source_copy");
        }
        else if (bFreshStaticMeshCandidate)
        {
            PlannedStrategy = TEXT("fresh_static_mesh_meshdescription_copy");
            PlannedDetail = TEXT("fresh_static_mesh_meshdescription_copy");
        }
        else if (bFreshSoundWaveCandidate)
        {
            PlannedStrategy = TEXT("fresh_sound_wave_payload_copy");
            PlannedDetail = TEXT("fresh_sound_wave_payload_copy");
        }
        else if (bFreshMaterialCandidate)
        {
            PlannedStrategy = TEXT("fresh_material_graph_copy");
            PlannedDetail = TEXT("fresh_material_graph_copy");
        }
        else if (bFreshMaterialFunctionCandidate)
        {
            PlannedStrategy = TEXT("fresh_material_function_graph_copy");
            PlannedDetail = TEXT("fresh_material_graph_copy");
        }
        else if (bFreshUserDefinedEnumCandidate)
        {
            PlannedStrategy = TEXT("fresh_user_defined_enum");
            PlannedDetail = TEXT("fresh_user_defined_enum_copy");
        }
        else if (bFreshDataTableCandidate)
        {
            PlannedStrategy = TEXT("fresh_data_table");
            PlannedDetail = TEXT("fresh_data_table_row_copy");
        }
        else if (bFreshUserDefinedStructCandidate)
        {
            PlannedStrategy = TEXT("fresh_user_defined_struct");
            PlannedDetail = TEXT("fresh_user_defined_struct_copy");
        }
        else if (bFreshPropertyCopyCandidate)
        {
            PlannedStrategy = TEXT("fresh_property_copy");
            PlannedDetail = GetFreshPropertyCopyReason(SourceAsset);
        }

        if (bDryRun)
        {
            AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(SourceAsset, TargetPath, PlannedStrategy, TEXT("planned"), PlannedDetail)));
            if (bFreshTexture2DCandidate)
            {
                ++FreshTexture2DAssetCount;
            }
            else if (bFreshTextureCubeCandidate)
            {
                ++FreshTextureCubeAssetCount;
            }
            else if (bFreshVolumeTextureCandidate)
            {
                ++FreshVolumeTextureAssetCount;
            }
            else if (bFreshStaticMeshCandidate)
            {
                ++FreshStaticMeshAssetCount;
            }
            else if (bFreshSoundWaveCandidate)
            {
                ++FreshSoundWaveAssetCount;
            }
            else if (bFreshMaterialCandidate)
            {
                ++FreshMaterialAssetCount;
            }
            else if (bFreshMaterialFunctionCandidate)
            {
                ++FreshMaterialFunctionAssetCount;
            }
            else if (bFreshUserDefinedEnumCandidate)
            {
                ++FreshUserDefinedEnumAssetCount;
            }
            else if (bFreshDataTableCandidate)
            {
                ++FreshDataTableAssetCount;
            }
            else if (bFreshUserDefinedStructCandidate)
            {
                ++FreshUserDefinedStructAssetCount;
            }
            else if (bFreshPropertyCopyCandidate)
            {
                ++FreshPropertyCopyAssetCount;
            }
            else
            {
                ++FallbackDuplicateCount;
            }
            continue;
        }

        if (UEditorAssetLibrary::DoesAssetExist(TargetPath))
        {
            if (!bOverwriteExisting)
            {
                SkippedExistingAssets.Add(TargetPath);
                UObject* ExistingTarget = UEditorAssetLibrary::LoadAsset(TargetPath);
                UObject* SourceObject = SourceAsset.GetAsset();
                if (SourceObject && ExistingTarget)
                {
                    AddReplacementMapping(ReplacementMap, SourceObject, ExistingTarget);
                    SourceTargetPairs.Add(TPair<UObject*, UObject*>(SourceObject, ExistingTarget));
                    TargetObjects.Add(ExistingTarget);
                    TargetPackageNames.Add(ExistingTarget->GetOutermost()->GetFName());
                    AddTargetWorldPackageName(TargetWorldPackageNames, ExistingTarget);
                    TargetPackages.Add(ExistingTarget->GetOutermost());
                    if (UBlueprint* ExistingBlueprint = Cast<UBlueprint>(ExistingTarget))
                    {
                        TargetBlueprints.Add(ExistingBlueprint);
                    }
                    if (UUserDefinedStruct* ExistingStruct = Cast<UUserDefinedStruct>(ExistingTarget))
                    {
                        TargetUserDefinedStructs.Add(ExistingStruct);
                    }
                }
                AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(SourceAsset, TargetPath, PlannedStrategy, TEXT("skipped_existing"), TEXT("target exists"))));
                continue;
            }

            if (!UEditorAssetLibrary::DeleteAsset(TargetPath))
            {
                FailedAssets.Add(TargetPath);
                AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(SourceAsset, TargetPath, PlannedStrategy, TEXT("failed"), TEXT("could not delete existing target"))));
                continue;
            }
        }

        UEditorAssetLibrary::MakeDirectory(TargetDirectory);
        UObject* SourceObject = SourceAsset.GetAsset();
        if (!SourceObject)
        {
            FailedAssets.Add(SourceAsset.PackageName.ToString());
            AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(SourceAsset, TargetPath, PlannedStrategy, TEXT("failed"), TEXT("could not load source asset"))));
            continue;
        }

        if (IsGeneratedClassInstanceFromRoot(SourceObject, SourceRoot))
        {
            DeferredGeneratedClassAssets.Add({
                SourceAsset,
                TargetPath,
                TEXT("fresh_generated_class_property_copy"),
                TEXT("fresh_target_generated_class_property_copy")});
            continue;
        }

        FString FreshCreateDetail;
        if (bFreshTexture2DCandidate)
        {
            if (UObject* TargetObject = CreateFreshTexture2DAsset(SourceObject, TargetPath, &ReplacementMap, FreshCreateDetail))
            {
                AddReplacementMapping(ReplacementMap, SourceObject, TargetObject);
                SourceTargetPairs.Add(TPair<UObject*, UObject*>(SourceObject, TargetObject));
                TargetObjects.Add(TargetObject);
                TargetPackageNames.Add(TargetObject->GetOutermost()->GetFName());
                AddTargetWorldPackageName(TargetWorldPackageNames, TargetObject);
                TargetPackages.Add(TargetObject->GetOutermost());
                ++CreatedCount;
                ++FreshTexture2DAssetCount;

                AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                    SourceAsset,
                    TargetPath,
                    TEXT("fresh_texture2d_source_copy"),
                    TEXT("created"),
                    FreshCreateDetail)));
                continue;
            }
        }
        else if (bFreshTextureCubeCandidate)
        {
            if (UObject* TargetObject = CreateFreshTextureCubeAsset(SourceObject, TargetPath, &ReplacementMap, FreshCreateDetail))
            {
                AddReplacementMapping(ReplacementMap, SourceObject, TargetObject);
                SourceTargetPairs.Add(TPair<UObject*, UObject*>(SourceObject, TargetObject));
                TargetObjects.Add(TargetObject);
                TargetPackageNames.Add(TargetObject->GetOutermost()->GetFName());
                AddTargetWorldPackageName(TargetWorldPackageNames, TargetObject);
                TargetPackages.Add(TargetObject->GetOutermost());
                ++CreatedCount;
                ++FreshTextureCubeAssetCount;

                AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                    SourceAsset,
                    TargetPath,
                    TEXT("fresh_texturecube_source_copy"),
                    TEXT("created"),
                    FreshCreateDetail)));
                continue;
            }
        }
        else if (bFreshVolumeTextureCandidate)
        {
            if (UObject* TargetObject = CreateFreshVolumeTextureAsset(SourceObject, TargetPath, &ReplacementMap, FreshCreateDetail))
            {
                AddReplacementMapping(ReplacementMap, SourceObject, TargetObject);
                SourceTargetPairs.Add(TPair<UObject*, UObject*>(SourceObject, TargetObject));
                TargetObjects.Add(TargetObject);
                TargetPackageNames.Add(TargetObject->GetOutermost()->GetFName());
                AddTargetWorldPackageName(TargetWorldPackageNames, TargetObject);
                TargetPackages.Add(TargetObject->GetOutermost());
                ++CreatedCount;
                ++FreshVolumeTextureAssetCount;

                AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                    SourceAsset,
                    TargetPath,
                    TEXT("fresh_volumetexture_source_copy"),
                    TEXT("created"),
                    FreshCreateDetail)));
                continue;
            }
        }
        else if (bFreshStaticMeshCandidate)
        {
            if (UObject* TargetObject = CreateFreshStaticMeshAsset(SourceObject, TargetPath, &ReplacementMap, FreshCreateDetail))
            {
                AddReplacementMapping(ReplacementMap, SourceObject, TargetObject);
                SourceTargetPairs.Add(TPair<UObject*, UObject*>(SourceObject, TargetObject));
                TargetObjects.Add(TargetObject);
                TargetPackageNames.Add(TargetObject->GetOutermost()->GetFName());
                AddTargetWorldPackageName(TargetWorldPackageNames, TargetObject);
                TargetPackages.Add(TargetObject->GetOutermost());
                ++CreatedCount;
                ++FreshStaticMeshAssetCount;

                AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                    SourceAsset,
                    TargetPath,
                    TEXT("fresh_static_mesh_meshdescription_copy"),
                    TEXT("created"),
                    FreshCreateDetail)));
                continue;
            }
        }
        else if (bFreshSoundWaveCandidate)
        {
            if (UObject* TargetObject = CreateFreshSoundWaveAsset(SourceObject, TargetPath, &ReplacementMap, FreshCreateDetail))
            {
                AddReplacementMapping(ReplacementMap, SourceObject, TargetObject);
                SourceTargetPairs.Add(TPair<UObject*, UObject*>(SourceObject, TargetObject));
                TargetObjects.Add(TargetObject);
                TargetPackageNames.Add(TargetObject->GetOutermost()->GetFName());
                AddTargetWorldPackageName(TargetWorldPackageNames, TargetObject);
                TargetPackages.Add(TargetObject->GetOutermost());
                ++CreatedCount;
                ++FreshSoundWaveAssetCount;

                AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                    SourceAsset,
                    TargetPath,
                    TEXT("fresh_sound_wave_payload_copy"),
                    TEXT("created"),
                    FreshCreateDetail)));
                continue;
            }
        }
        else if (bFreshMaterialGraphCandidate)
        {
            if (UObject* TargetObject = CreateFreshMaterialGraphAsset(SourceObject, TargetPath, &ReplacementMap, FreshCreateDetail))
            {
                AddReplacementMapping(ReplacementMap, SourceObject, TargetObject);
                FreshMaterialExpressionMappingCount += AddMaterialGraphExpressionReplacementMappings(ReplacementMap, SourceObject, TargetObject);
                SourceTargetPairs.Add(TPair<UObject*, UObject*>(SourceObject, TargetObject));
                TargetObjects.Add(TargetObject);
                TargetPackageNames.Add(TargetObject->GetOutermost()->GetFName());
                AddTargetWorldPackageName(TargetWorldPackageNames, TargetObject);
                TargetPackages.Add(TargetObject->GetOutermost());
                ++CreatedCount;
                if (bFreshMaterialCandidate)
                {
                    ++FreshMaterialAssetCount;
                }
                else
                {
                    ++FreshMaterialFunctionAssetCount;
                }

                AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                    SourceAsset,
                    TargetPath,
                    bFreshMaterialCandidate ? TEXT("fresh_material_graph_copy") : TEXT("fresh_material_function_graph_copy"),
                    TEXT("created"),
                    FreshCreateDetail)));
                continue;
            }
        }
        else if (bFreshUserDefinedEnumCandidate)
        {
            if (UObject* TargetObject = CreateFreshUserDefinedEnumAsset(SourceObject, TargetPath, FreshCreateDetail))
            {
                AddReplacementMapping(ReplacementMap, SourceObject, TargetObject);
                SourceTargetPairs.Add(TPair<UObject*, UObject*>(SourceObject, TargetObject));
                TargetObjects.Add(TargetObject);
                TargetPackageNames.Add(TargetObject->GetOutermost()->GetFName());
                AddTargetWorldPackageName(TargetWorldPackageNames, TargetObject);
                TargetPackages.Add(TargetObject->GetOutermost());
                ++CreatedCount;
                ++FreshUserDefinedEnumAssetCount;

                AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                    SourceAsset,
                    TargetPath,
                    TEXT("fresh_user_defined_enum"),
                    TEXT("created"),
                    FreshCreateDetail)));
                continue;
            }
        }
        else if (bFreshDataTableCandidate)
        {
            if (UObject* TargetObject = CreateFreshDataTableAsset(SourceObject, TargetPath, ReplacementMap, SourceRoot, FreshCreateDetail))
            {
                AddReplacementMapping(ReplacementMap, SourceObject, TargetObject);
                SourceTargetPairs.Add(TPair<UObject*, UObject*>(SourceObject, TargetObject));
                TargetObjects.Add(TargetObject);
                TargetPackageNames.Add(TargetObject->GetOutermost()->GetFName());
                AddTargetWorldPackageName(TargetWorldPackageNames, TargetObject);
                TargetPackages.Add(TargetObject->GetOutermost());
                ++CreatedCount;
                ++FreshDataTableAssetCount;

                AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                    SourceAsset,
                    TargetPath,
                    TEXT("fresh_data_table"),
                    TEXT("created"),
                    FreshCreateDetail)));
                continue;
            }
        }
        else if (bFreshUserDefinedStructCandidate)
        {
            if (UObject* TargetObject = CreateFreshUserDefinedStructAsset(SourceObject, TargetPath, ReplacementMap, FreshCreateDetail))
            {
                AddReplacementMapping(ReplacementMap, SourceObject, TargetObject);
                SourceTargetPairs.Add(TPair<UObject*, UObject*>(SourceObject, TargetObject));
                TargetObjects.Add(TargetObject);
                TargetPackageNames.Add(TargetObject->GetOutermost()->GetFName());
                AddTargetWorldPackageName(TargetWorldPackageNames, TargetObject);
                TargetPackages.Add(TargetObject->GetOutermost());
                if (UUserDefinedStruct* TargetStruct = Cast<UUserDefinedStruct>(TargetObject))
                {
                    TargetUserDefinedStructs.Add(TargetStruct);
                }
                ++CreatedCount;
                ++FreshUserDefinedStructAssetCount;

                AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                    SourceAsset,
                    TargetPath,
                    TEXT("fresh_user_defined_struct"),
                    TEXT("created"),
                    FreshCreateDetail)));
                continue;
            }
        }
        else if (bFreshPropertyCopyCandidate)
        {
            if (UObject* TargetObject = CreateFreshPropertyCopyAsset(SourceObject, TargetPath, SourceObject->GetClass(), &ReplacementMap, FreshCreateDetail))
            {
                AddReplacementMapping(ReplacementMap, SourceObject, TargetObject);
                SourceTargetPairs.Add(TPair<UObject*, UObject*>(SourceObject, TargetObject));
                TargetObjects.Add(TargetObject);
                TargetPackageNames.Add(TargetObject->GetOutermost()->GetFName());
                AddTargetWorldPackageName(TargetWorldPackageNames, TargetObject);
                TargetPackages.Add(TargetObject->GetOutermost());
                ++CreatedCount;
                ++FreshPropertyCopyAssetCount;

                AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                    SourceAsset,
                    TargetPath,
                    TEXT("fresh_property_copy"),
                    TEXT("created"),
                    FreshCreateDetail)));
                continue;
            }
        }

        if (!bAllowFallbackDuplicate)
        {
            FailedAssets.Add(SourceAsset.PackageName.ToString());
            const FString FailureDetail = FreshCreateDetail.IsEmpty()
                ? TEXT("fallback duplicate disabled")
                : FString::Printf(TEXT("fresh create failed: %s; fallback duplicate disabled"), *FreshCreateDetail);
            AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(SourceAsset, TargetPath, PlannedStrategy, TEXT("failed"), FailureDetail)));
            continue;
        }

        UObject* TargetObject = UEditorAssetLibrary::DuplicateAsset(SourceAsset.PackageName.ToString(), TargetPath);
        if (!TargetObject)
        {
            FailedAssets.Add(SourceAsset.PackageName.ToString());
            AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(SourceAsset, TargetPath, TEXT("fallback_duplicate"), TEXT("failed"), TEXT("duplicate fallback failed"))));
            continue;
        }

        AddReplacementMapping(ReplacementMap, SourceObject, TargetObject);
        SourceTargetPairs.Add(TPair<UObject*, UObject*>(SourceObject, TargetObject));
        TargetObjects.Add(TargetObject);
        TargetPackageNames.Add(TargetObject->GetOutermost()->GetFName());
        AddTargetWorldPackageName(TargetWorldPackageNames, TargetObject);
        TargetPackages.Add(TargetObject->GetOutermost());
        ++CreatedCount;
        ++FallbackDuplicateCount;
        if (bFreshCreateCandidate)
        {
            ++FreshCreateFallbackCount;
        }

        if (UBlueprint* TargetBlueprint = Cast<UBlueprint>(TargetObject))
        {
            TargetBlueprints.Add(TargetBlueprint);
        }
        if (UUserDefinedStruct* TargetStruct = Cast<UUserDefinedStruct>(TargetObject))
        {
            TargetUserDefinedStructs.Add(TargetStruct);
        }

        const FString FallbackDetail = bFreshCreateCandidate && !FreshCreateDetail.IsEmpty()
            ? FString::Printf(TEXT("fresh create failed: %s; fallback duplicate used"), *FreshCreateDetail)
            : GetFallbackReason(SourceAsset);
        TSharedPtr<FJsonObject> FallbackReport = AssetReportToJson(SourceAsset, TargetPath, TEXT("fallback_duplicate"), TEXT("created"), FallbackDetail);
        if (bFreshCreateCandidate)
        {
            ClassifyFreshCreateFallbackForMCP(
                SourceAsset,
                TargetPath,
                FreshCreateDetail,
                AcceptedFallbackRules,
                FallbackReport,
                AcceptedFallbackCount,
                UnresolvedFallbackCount,
                AcceptedFallbackSamples,
                UnresolvedFallbackSamples);
        }
        AssetReports.Add(MakeShared<FJsonValueObject>(FallbackReport));
    }

    for (const TPair<UObject*, UObject*>& Pair : SourceTargetPairs)
    {
        AddBlueprintReplacementMappings(ReplacementMap, Pair.Key, Pair.Value);
    }

    for (const FDeferredGeneratedClassAsset& DeferredAsset : DeferredGeneratedClassAssets)
    {
        UObject* SourceObject = DeferredAsset.SourceAsset.GetAsset();
        UClass* SourceClass = SourceObject ? SourceObject->GetClass() : nullptr;
        UClass* TargetClass = Cast<UClass>(FindReplacementObject(ReplacementMap, SourceClass));
        if (!SourceObject || !SourceClass || !TargetClass)
        {
            FailedAssets.Add(DeferredAsset.SourceAsset.PackageName.ToString());
            AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                DeferredAsset.SourceAsset,
                DeferredAsset.TargetPath,
                DeferredAsset.Strategy,
                TEXT("failed"),
                TEXT("could not resolve target generated class"))));
            continue;
        }

        if (TargetClass->GetPropertiesSize() < SourceClass->GetPropertiesSize())
        {
            FailedAssets.Add(DeferredAsset.SourceAsset.PackageName.ToString());
            AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                DeferredAsset.SourceAsset,
                DeferredAsset.TargetPath,
                DeferredAsset.Strategy,
                TEXT("failed"),
                TEXT("target generated class is not serialization-compatible"))));
            continue;
        }

        const FString TargetDirectory = FPackageName::GetLongPackagePath(DeferredAsset.TargetPath);
        UEditorAssetLibrary::MakeDirectory(TargetDirectory);

        FString FreshCreateDetail;
        UObject* TargetObject = CreateFreshPropertyCopyAsset(
            SourceObject,
            DeferredAsset.TargetPath,
            TargetClass,
            &ReplacementMap,
            FreshCreateDetail);
        bool bFreshGeneratedClassCreated = TargetObject != nullptr;

        if (!TargetObject && !bAllowFallbackDuplicate)
        {
            FailedAssets.Add(DeferredAsset.SourceAsset.PackageName.ToString());
            AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                DeferredAsset.SourceAsset,
                DeferredAsset.TargetPath,
                DeferredAsset.Strategy,
                TEXT("failed"),
                FreshCreateDetail.IsEmpty()
                    ? TEXT("fresh generated-class property copy failed; fallback duplicate disabled")
                    : FString::Printf(TEXT("fresh generated-class property copy failed: %s; fallback duplicate disabled"), *FreshCreateDetail))));
            continue;
        }

        if (!TargetObject)
        {
            UPackage* TargetPackage = CreatePackage(*DeferredAsset.TargetPath);
            if (!TargetPackage)
            {
                FailedAssets.Add(DeferredAsset.SourceAsset.PackageName.ToString());
                AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                    DeferredAsset.SourceAsset,
                    DeferredAsset.TargetPath,
                    TEXT("fallback_duplicate"),
                    TEXT("failed"),
                    TEXT("could not create target package"))));
                continue;
            }

            FObjectDuplicationParameters DuplicationParameters(SourceObject, TargetPackage);
            DuplicationParameters.DestName = FName(*FPackageName::GetShortName(DeferredAsset.TargetPath));
            DuplicationParameters.DestClass = TargetClass;
            DuplicationParameters.ApplyFlags = RF_Public | RF_Standalone | RF_Transactional;
            TargetObject = StaticDuplicateObjectEx(DuplicationParameters);
            if (!TargetObject)
            {
                FailedAssets.Add(DeferredAsset.SourceAsset.PackageName.ToString());
                AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                    DeferredAsset.SourceAsset,
                    DeferredAsset.TargetPath,
                    TEXT("fallback_duplicate"),
                    TEXT("failed"),
                    TEXT("generated-class asset duplication failed"))));
                continue;
            }

            TargetObject->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
            TargetObject->ClearFlags(RF_Transient);
            TargetPackage->MarkPackageDirty();
            FAssetRegistryModule::AssetCreated(TargetObject);
            bFreshGeneratedClassCreated = false;
        }

        AddReplacementMapping(ReplacementMap, SourceObject, TargetObject);
        SourceTargetPairs.Add(TPair<UObject*, UObject*>(SourceObject, TargetObject));
        TargetObjects.Add(TargetObject);
        TargetPackageNames.Add(TargetObject->GetOutermost()->GetFName());
        AddTargetWorldPackageName(TargetWorldPackageNames, TargetObject);
        TargetPackages.Add(TargetObject->GetOutermost());
        ++CreatedCount;
        ++GeneratedClassAssetRecreateCount;
        if (bFreshGeneratedClassCreated)
        {
            ++FreshGeneratedClassAssetCount;
        }
        else
        {
            ++FallbackDuplicateCount;
            ++FreshCreateFallbackCount;
        }

        const FString DeferredFallbackDetail = FreshCreateDetail.IsEmpty()
            ? TEXT("fresh generated-class property copy failed; duplicated_with_target_generated_class")
            : FString::Printf(TEXT("fresh generated-class property copy failed: %s; duplicated_with_target_generated_class"), *FreshCreateDetail);
        TSharedPtr<FJsonObject> DeferredAssetReport = AssetReportToJson(
            DeferredAsset.SourceAsset,
            DeferredAsset.TargetPath,
            bFreshGeneratedClassCreated ? DeferredAsset.Strategy : TEXT("fallback_duplicate"),
            TEXT("created"),
            bFreshGeneratedClassCreated ? FreshCreateDetail : DeferredFallbackDetail);
        if (!bFreshGeneratedClassCreated)
        {
            ClassifyFreshCreateFallbackForMCP(
                DeferredAsset.SourceAsset,
                DeferredAsset.TargetPath,
                FreshCreateDetail,
                AcceptedFallbackRules,
                DeferredAssetReport,
                AcceptedFallbackCount,
                UnresolvedFallbackCount,
                AcceptedFallbackSamples,
                UnresolvedFallbackSamples);
        }
        AssetReports.Add(MakeShared<FJsonValueObject>(DeferredAssetReport));
    }

    for (const TPair<UObject*, UObject*>& Pair : SourceTargetPairs)
    {
        UBlueprint* SourceBlueprint = Cast<UBlueprint>(Pair.Key);
        UBlueprint* TargetBlueprint = Cast<UBlueprint>(Pair.Value);
        if (SourceBlueprint && TargetBlueprint)
        {
            SCSNodeRepairCount += RepairBlueprintSCSNodes(SourceBlueprint, TargetBlueprint, ReplacementMap);
            WidgetTreeRepairCount += RepairWidgetBlueprintTree(TargetBlueprint, ReplacementMap);
        }
    }
    GeneratedClassSubobjectRepairCount += RepairTargetSubobjectsWithSourceClasses(TargetPackages, ReplacementMap);

    for (const TPair<UObject*, UObject*>& Pair : SourceTargetPairs)
    {
        AddObjectPathReplacements(PathReplacements, Pair.Key, Pair.Value);
    }
    PathReplacements.Sort([](const FPathStringReplacement& A, const FPathStringReplacement& B)
    {
        return A.Source.Len() > B.Source.Len();
    });

    if (!bDryRun && bRemapReferences && !TargetUserDefinedStructs.IsEmpty())
    {
        UserDefinedStructDescriptionPathRemapCount += RemapUserDefinedStructDescriptionPaths(
            TargetUserDefinedStructs,
            PathReplacements,
            ReplacementMap,
            SourceRoot);
    }

    if (!bDryRun && bRemapReferences && ReplacementMap.Num() > 0)
    {
        TSet<UObject*> ObjectsToRemap;
        for (UObject* TargetObject : TargetObjects)
        {
            if (IsLiveObjectForMCP(TargetObject))
            {
                ObjectsToRemap.Add(TargetObject);
            }
        }
        for (UPackage* TargetPackage : TargetPackages)
        {
            if (!TargetPackage)
            {
                continue;
            }

            TArray<UObject*> PackageObjects;
            GetObjectsWithPackage(TargetPackage, PackageObjects, true, RF_Transient, EInternalObjectFlags::Garbage);
            for (UObject* PackageObject : PackageObjects)
            {
                if (IsLiveObjectForMCP(PackageObject))
                {
                    ObjectsToRemap.Add(PackageObject);
                }
            }
        }

        for (UObject* TargetObject : ObjectsToRemap)
        {
            if (!IsLiveObjectForMCP(TargetObject))
            {
                continue;
            }

            if (ShouldSkipAssetArchiveObjectReferenceRemap(TargetObject))
            {
                ++ArchiveObjectRemapSkipCount;
                continue;
            }

            if (bLogArchiveRemapObjects)
            {
                const UClass* TargetObjectClass = TargetObject->GetClass();
                UE_LOG(LogTemp, Display, TEXT("MCP recreate archive remap object: %s | class=%s"),
                    *TargetObject->GetPathName(),
                    TargetObjectClass ? *TargetObjectClass->GetPathName() : TEXT("<null>"));
            }

            TargetObject->Modify();
            FArchiveReplaceObjectRef<UObject> ReplaceAr(
                TargetObject,
                ReplacementMap,
                EArchiveReplaceObjectFlags::IgnoreOuterRef |
                EArchiveReplaceObjectFlags::IgnoreArchetypeRef |
                EArchiveReplaceObjectFlags::IncludeClassGeneratedByRef);

            if (ReplaceAr.GetCount() > 0)
            {
                ++RemappedObjectCount;
                TargetObject->MarkPackageDirty();
                if (UPackage* Package = TargetObject->GetOutermost())
                {
                    Package->MarkPackageDirty();
                }
            }
        }
    }

    if (!bDryRun && !TargetUserDefinedStructs.IsEmpty())
    {
        UserDefinedStructCompileErrorCount += CompileUserDefinedStructsForMCP(TargetUserDefinedStructs, &FailedAssets);
    }

    if (!bDryRun && bRemapReferences && ReplacementMap.Num() > 0)
    {
        for (UBlueprint* Blueprint : TargetBlueprints)
        {
            BlueprintGraphReferenceRemapCount += RemapBlueprintGraphReferences(Blueprint, ReplacementMap, PathReplacements);
        }
    }

    if (!bDryRun && bRemapReferences)
    {
        RefreshMaterialReferencesAndCaches(
            TargetPackages,
            ReplacementMap,
            MaterialTextureReferenceRepairCount,
            MaterialCacheRefreshCount);

        if (!TargetUserDefinedStructs.IsEmpty())
        {
            UserDefinedStructCompileErrorCount += CompileUserDefinedStructsForMCP(TargetUserDefinedStructs, &FailedAssets);
        }
    }

    if (!bDryRun && bSaveAssets)
    {
        for (UObject* TargetObject : TargetObjects)
        {
            if (SaveLoadedAssetForMCP(TargetObject))
            {
                ++SavedCount;
            }
        }
    }

    TArray<FString> OriginalDependencySamples;
    TSet<FName> OriginalDependencyPackages;
    int32 OriginalDependencyAssetCount = 0;
    if (!bDryRun)
    {
        TArray<FString> PathsToScan;
        PathsToScan.Add(TargetRoot);
        AssetRegistry.ScanPathsSynchronous(PathsToScan, true);
        OriginalDependencyAssetCount = CollectOriginalDependencyPackages(
            AssetRegistry,
            TargetPackageNames,
            SourceRoot,
            OriginalDependencyPackages,
            OriginalDependencySamples,
            50);
    }

    if (!bDryRun && bRemapReferences && PathReplacements.Num() > 0 && OriginalDependencyPackages.Num() > 0)
    {
        TSet<UObject*> ObjectsToPathRemap;
        for (const FName& PackageName : OriginalDependencyPackages)
        {
            UPackage* TargetPackage = FindPackage(nullptr, *PackageName.ToString());
            if (!TargetPackage)
            {
                TargetPackage = LoadPackage(nullptr, *PackageName.ToString(), LOAD_None);
            }
            if (!TargetPackage)
            {
                continue;
            }

            TArray<UObject*> PackageObjects;
            GetObjectsWithPackage(TargetPackage, PackageObjects, true, RF_Transient, EInternalObjectFlags::Garbage);
            for (UObject* PackageObject : PackageObjects)
            {
                if (IsLiveObjectForMCP(PackageObject))
                {
                    ObjectsToPathRemap.Add(PackageObject);
                }
            }
        }

        for (UObject* TargetObject : ObjectsToPathRemap)
        {
            if (!IsLiveObjectForMCP(TargetObject))
            {
                continue;
            }

            PathPropertyRemapCount += ReplaceExportedPropertyPaths(TargetObject, PathReplacements, SourceRoot);
        }

        if (!TargetUserDefinedStructs.IsEmpty())
        {
            UserDefinedStructDescriptionPathRemapCount += RemapUserDefinedStructDescriptionPaths(
                TargetUserDefinedStructs,
                PathReplacements,
                ReplacementMap,
                SourceRoot);
        }

        RefreshMaterialReferencesAndCaches(
            TargetPackages,
            ReplacementMap,
            MaterialTextureReferenceRepairCount,
            MaterialCacheRefreshCount);
    }

    if (!bDryRun && bSaveAssets && (PathPropertyRemapCount > 0 || UserDefinedStructDescriptionPathRemapCount > 0))
    {
        for (UObject* TargetObject : TargetObjects)
        {
            if (SaveLoadedAssetForMCP(TargetObject))
            {
                ++SavedCount;
            }
        }
    }

    if (!bDryRun && bCompileBlueprints)
    {
        CompiledBlueprintCount = CompileBlueprintsForMCP(
            TargetBlueprints,
            CompilePasses,
            bRefreshBlueprintNodes,
            bRefreshFailedBlueprintNodes,
            BlueprintCompileErrorCount,
            &FailedAssets);
    }

    if (!bDryRun && bSaveAssets)
    {
        for (UBlueprint* TargetBlueprint : TargetBlueprints)
        {
            if (SaveLoadedAssetForMCP(TargetBlueprint))
            {
                ++SavedCount;
            }
        }
    }

    if (!bDryRun && bRemapReferences && bRepairLevelActors)
    {
        TArray<FString> WorldPackagesForLevelRepair;
        if (bForceRepairLevelActors)
        {
            WorldPackagesForLevelRepair = TargetWorldPackageNames;
        }
        else
        {
            for (const FString& WorldPackageName : TargetWorldPackageNames)
            {
                if (OriginalDependencyPackages.Contains(FName(*WorldPackageName)))
                {
                    WorldPackagesForLevelRepair.Add(WorldPackageName);
                }
                else
                {
                    ++SkippedCleanWorldRepairCount;
                }
            }
        }

        LevelActorClassRepairCount += RepairTargetWorldActorsWithSourceClasses(
            WorldPackagesForLevelRepair,
            ReplacementMap,
            PathReplacements,
            SourceRoot,
            LevelActorReferenceRemapCount,
            SavedMapCount,
            LevelComponentClassRepairCount,
            FailedAssets);
    }

    if (!bDryRun)
    {
        TArray<FString> PathsToScan;
        PathsToScan.Add(TargetRoot);
        AssetRegistry.ScanPathsSynchronous(PathsToScan, true);
        OriginalDependencyAssetCount = CollectOriginalDependencyPackages(
            AssetRegistry,
            TargetPackageNames,
            SourceRoot,
            OriginalDependencyPackages,
            OriginalDependencySamples,
            50);
    }

    TArray<TSharedPtr<FJsonValue>> ClassCountValues;
    for (const TPair<FString, int32>& Pair : ClassCounts)
    {
        TSharedPtr<FJsonObject> ClassObject = MakeShared<FJsonObject>();
        ClassObject->SetStringField(TEXT("class"), Pair.Key);
        ClassObject->SetNumberField(TEXT("count"), Pair.Value);
        ClassCountValues.Add(MakeShared<FJsonValueObject>(ClassObject));
    }

    const FEditorLogHealthResult EditorLogHealth = ScanEditorLogSince(EditorLogMarker, TargetRoot);
    const bool bEditorLogVerificationPass = !bCheckEditorLogHealth || !bFailOnEditorLogIssues || EditorLogHealth.bPass;
    const bool bVerificationPass = FailedAssets.IsEmpty()
        && UnresolvedFallbackCount == 0
        && BlueprintCompileErrorCount == 0
        && UserDefinedStructCompileErrorCount == 0
        && OriginalDependencyAssetCount == 0
        && bEditorLogVerificationPass;

    ReportObject->SetNumberField(TEXT("source_asset_count"), SourceAssets.Num());
    ReportObject->SetNumberField(TEXT("fallback_duplicate_count"), FallbackDuplicateCount);
    ReportObject->SetNumberField(TEXT("created_count"), CreatedCount);
    ReportObject->SetNumberField(TEXT("saved_count"), SavedCount);
    ReportObject->SetStringField(TEXT("asset_save_mode"), TEXT("package_save_no_prompt"));
    ReportObject->SetNumberField(TEXT("remapped_object_count"), RemappedObjectCount);
    ReportObject->SetNumberField(TEXT("archive_object_remap_skip_count"), ArchiveObjectRemapSkipCount);
    ReportObject->SetNumberField(TEXT("path_property_remap_count"), PathPropertyRemapCount);
    ReportObject->SetNumberField(TEXT("generated_class_asset_recreate_count"), GeneratedClassAssetRecreateCount);
    ReportObject->SetNumberField(TEXT("fresh_property_copy_asset_count"), FreshPropertyCopyAssetCount);
    ReportObject->SetNumberField(TEXT("fresh_generated_class_asset_count"), FreshGeneratedClassAssetCount);
    ReportObject->SetNumberField(TEXT("fresh_material_asset_count"), FreshMaterialAssetCount);
    ReportObject->SetNumberField(TEXT("fresh_material_function_asset_count"), FreshMaterialFunctionAssetCount);
    ReportObject->SetNumberField(TEXT("fresh_material_expression_mapping_count"), FreshMaterialExpressionMappingCount);
    ReportObject->SetNumberField(TEXT("fresh_texture2d_asset_count"), FreshTexture2DAssetCount);
    ReportObject->SetNumberField(TEXT("fresh_texturecube_asset_count"), FreshTextureCubeAssetCount);
    ReportObject->SetNumberField(TEXT("fresh_volumetexture_asset_count"), FreshVolumeTextureAssetCount);
    ReportObject->SetNumberField(TEXT("fresh_static_mesh_asset_count"), FreshStaticMeshAssetCount);
    ReportObject->SetNumberField(TEXT("fresh_sound_wave_asset_count"), FreshSoundWaveAssetCount);
    ReportObject->SetNumberField(TEXT("fresh_data_table_asset_count"), FreshDataTableAssetCount);
    ReportObject->SetNumberField(TEXT("fresh_user_defined_enum_asset_count"), FreshUserDefinedEnumAssetCount);
    ReportObject->SetNumberField(TEXT("fresh_user_defined_struct_asset_count"), FreshUserDefinedStructAssetCount);
    ReportObject->SetNumberField(TEXT("fresh_create_fallback_count"), FreshCreateFallbackCount);
    ReportObject->SetNumberField(TEXT("accepted_fallback_rule_count"), AcceptedFallbackRules.Num());
    ReportObject->SetNumberField(TEXT("accepted_fallback_count"), AcceptedFallbackCount);
    ReportObject->SetNumberField(TEXT("unresolved_fallback_count"), UnresolvedFallbackCount);
    ReportObject->SetNumberField(TEXT("user_defined_struct_description_path_remap_count"), UserDefinedStructDescriptionPathRemapCount);
    ReportObject->SetNumberField(TEXT("scs_node_repair_count"), SCSNodeRepairCount);
    ReportObject->SetNumberField(TEXT("widget_tree_repair_count"), WidgetTreeRepairCount);
    ReportObject->SetNumberField(TEXT("generated_class_subobject_repair_count"), GeneratedClassSubobjectRepairCount);
    ReportObject->SetNumberField(TEXT("material_texture_reference_repair_count"), MaterialTextureReferenceRepairCount);
    ReportObject->SetNumberField(TEXT("material_cache_refresh_count"), MaterialCacheRefreshCount);
    ReportObject->SetNumberField(TEXT("level_actor_reference_remap_count"), LevelActorReferenceRemapCount);
    ReportObject->SetNumberField(TEXT("level_actor_class_repair_count"), LevelActorClassRepairCount);
    ReportObject->SetNumberField(TEXT("level_component_class_repair_count"), LevelComponentClassRepairCount);
    ReportObject->SetNumberField(TEXT("saved_map_count"), SavedMapCount);
    ReportObject->SetNumberField(TEXT("skipped_clean_world_repair_count"), SkippedCleanWorldRepairCount);
    ReportObject->SetNumberField(TEXT("blueprint_graph_reference_remap_count"), BlueprintGraphReferenceRemapCount);
    ReportObject->SetNumberField(TEXT("compiled_blueprint_count"), CompiledBlueprintCount);
    ReportObject->SetNumberField(TEXT("blueprint_compile_error_count"), BlueprintCompileErrorCount);
    ReportObject->SetNumberField(TEXT("user_defined_struct_compile_error_count"), UserDefinedStructCompileErrorCount);
    ReportObject->SetNumberField(TEXT("original_dependency_asset_count"), OriginalDependencyAssetCount);
    AddEditorLogHealthFields(ReportObject, EditorLogHealth);
    ReportObject->SetStringField(TEXT("delete_target_root_mode"), DeleteTargetRootMode);
    ReportObject->SetBoolField(TEXT("verification_pass"), bVerificationPass);
    ReportObject->SetBoolField(TEXT("check_editor_log_health"), bCheckEditorLogHealth);
    ReportObject->SetBoolField(TEXT("fail_on_editor_log_issues"), bFailOnEditorLogIssues);
    ReportObject->SetBoolField(TEXT("suppress_editor_prompts"), bSuppressEditorPrompts);
    ReportObject->SetBoolField(TEXT("log_archive_remap_objects"), bLogArchiveRemapObjects);
    ReportObject->SetBoolField(TEXT("refresh_blueprint_nodes"), bRefreshBlueprintNodes);
    ReportObject->SetBoolField(TEXT("refresh_failed_blueprint_nodes"), bRefreshFailedBlueprintNodes);
    ReportObject->SetBoolField(TEXT("repair_level_actors"), bRepairLevelActors);
    ReportObject->SetBoolField(TEXT("force_repair_level_actors"), bForceRepairLevelActors);
    ReportObject->SetArrayField(TEXT("class_counts"), ClassCountValues);
    AddStringArrayField(ReportObject, TEXT("failed_assets"), FailedAssets);
    AddStringArrayField(ReportObject, TEXT("skipped_existing_assets"), SkippedExistingAssets);
    AddStringArrayField(ReportObject, TEXT("accepted_fallback_samples"), AcceptedFallbackSamples);
    AddStringArrayField(ReportObject, TEXT("unresolved_fallback_samples"), UnresolvedFallbackSamples);
    AddStringArrayField(ReportObject, TEXT("delete_target_root_warnings"), DeleteTargetRootWarnings);
    AddStringArrayField(ReportObject, TEXT("original_dependency_samples"), OriginalDependencySamples);
    ReportObject->SetArrayField(TEXT("assets"), AssetReports);
    if (!FailedAssets.IsEmpty())
    {
        ReportObject->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("Recreate finished with %d failed assets"), FailedAssets.Num()));
    }
    else if (UnresolvedFallbackCount > 0)
    {
        ReportObject->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("Recreate finished with %d unresolved fresh-create fallback asset(s)"), UnresolvedFallbackCount));
    }
    else if (OriginalDependencyAssetCount > 0)
    {
        ReportObject->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("Recreate finished with %d target assets still depending on the source root"), OriginalDependencyAssetCount));
    }
    else if (bCheckEditorLogHealth && bFailOnEditorLogIssues && !EditorLogHealth.bPass)
    {
        ReportObject->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("Recreate editor log health check found %d issue(s)"), EditorLogHealth.IssueCount));
    }

    FString ReportJson;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ReportJson);
    FJsonSerializer::Serialize(ReportObject.ToSharedRef(), Writer);

    const FString ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MCP"));
    IFileManager::Get().MakeDirectory(*ReportDirectory, true);
    const FString ReportPath = FPaths::Combine(ReportDirectory, BuildMCPReportFilename(TargetRoot, TEXT("recreate")));
    FFileHelper::SaveStringToFile(ReportJson, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetBoolField(TEXT("verification_pass"), bVerificationPass);
    ResultObj->SetStringField(TEXT("source_root"), SourceRoot);
    ResultObj->SetStringField(TEXT("target_root"), TargetRoot);
    ResultObj->SetStringField(TEXT("report_path"), ReportPath);
    ResultObj->SetNumberField(TEXT("source_asset_count"), SourceAssets.Num());
    ResultObj->SetNumberField(TEXT("fallback_duplicate_count"), FallbackDuplicateCount);
    ResultObj->SetNumberField(TEXT("created_count"), CreatedCount);
    ResultObj->SetNumberField(TEXT("saved_count"), SavedCount);
    ResultObj->SetStringField(TEXT("asset_save_mode"), TEXT("package_save_no_prompt"));
    ResultObj->SetNumberField(TEXT("remapped_object_count"), RemappedObjectCount);
    ResultObj->SetNumberField(TEXT("archive_object_remap_skip_count"), ArchiveObjectRemapSkipCount);
    ResultObj->SetNumberField(TEXT("path_property_remap_count"), PathPropertyRemapCount);
    ResultObj->SetNumberField(TEXT("generated_class_asset_recreate_count"), GeneratedClassAssetRecreateCount);
    ResultObj->SetNumberField(TEXT("fresh_property_copy_asset_count"), FreshPropertyCopyAssetCount);
    ResultObj->SetNumberField(TEXT("fresh_generated_class_asset_count"), FreshGeneratedClassAssetCount);
    ResultObj->SetNumberField(TEXT("fresh_material_asset_count"), FreshMaterialAssetCount);
    ResultObj->SetNumberField(TEXT("fresh_material_function_asset_count"), FreshMaterialFunctionAssetCount);
    ResultObj->SetNumberField(TEXT("fresh_material_expression_mapping_count"), FreshMaterialExpressionMappingCount);
    ResultObj->SetNumberField(TEXT("fresh_texture2d_asset_count"), FreshTexture2DAssetCount);
    ResultObj->SetNumberField(TEXT("fresh_texturecube_asset_count"), FreshTextureCubeAssetCount);
    ResultObj->SetNumberField(TEXT("fresh_volumetexture_asset_count"), FreshVolumeTextureAssetCount);
    ResultObj->SetNumberField(TEXT("fresh_static_mesh_asset_count"), FreshStaticMeshAssetCount);
    ResultObj->SetNumberField(TEXT("fresh_sound_wave_asset_count"), FreshSoundWaveAssetCount);
    ResultObj->SetNumberField(TEXT("fresh_data_table_asset_count"), FreshDataTableAssetCount);
    ResultObj->SetNumberField(TEXT("fresh_user_defined_enum_asset_count"), FreshUserDefinedEnumAssetCount);
    ResultObj->SetNumberField(TEXT("fresh_user_defined_struct_asset_count"), FreshUserDefinedStructAssetCount);
    ResultObj->SetNumberField(TEXT("fresh_create_fallback_count"), FreshCreateFallbackCount);
    ResultObj->SetNumberField(TEXT("accepted_fallback_rule_count"), AcceptedFallbackRules.Num());
    ResultObj->SetNumberField(TEXT("accepted_fallback_count"), AcceptedFallbackCount);
    ResultObj->SetNumberField(TEXT("unresolved_fallback_count"), UnresolvedFallbackCount);
    ResultObj->SetNumberField(TEXT("user_defined_struct_description_path_remap_count"), UserDefinedStructDescriptionPathRemapCount);
    ResultObj->SetNumberField(TEXT("scs_node_repair_count"), SCSNodeRepairCount);
    ResultObj->SetNumberField(TEXT("widget_tree_repair_count"), WidgetTreeRepairCount);
    ResultObj->SetNumberField(TEXT("generated_class_subobject_repair_count"), GeneratedClassSubobjectRepairCount);
    ResultObj->SetNumberField(TEXT("material_texture_reference_repair_count"), MaterialTextureReferenceRepairCount);
    ResultObj->SetNumberField(TEXT("material_cache_refresh_count"), MaterialCacheRefreshCount);
    ResultObj->SetNumberField(TEXT("level_actor_reference_remap_count"), LevelActorReferenceRemapCount);
    ResultObj->SetNumberField(TEXT("level_actor_class_repair_count"), LevelActorClassRepairCount);
    ResultObj->SetNumberField(TEXT("level_component_class_repair_count"), LevelComponentClassRepairCount);
    ResultObj->SetNumberField(TEXT("saved_map_count"), SavedMapCount);
    ResultObj->SetNumberField(TEXT("skipped_clean_world_repair_count"), SkippedCleanWorldRepairCount);
    ResultObj->SetNumberField(TEXT("blueprint_graph_reference_remap_count"), BlueprintGraphReferenceRemapCount);
    ResultObj->SetNumberField(TEXT("compiled_blueprint_count"), CompiledBlueprintCount);
    ResultObj->SetNumberField(TEXT("blueprint_compile_error_count"), BlueprintCompileErrorCount);
    ResultObj->SetNumberField(TEXT("user_defined_struct_compile_error_count"), UserDefinedStructCompileErrorCount);
    ResultObj->SetNumberField(TEXT("original_dependency_asset_count"), OriginalDependencyAssetCount);
    AddEditorLogHealthFields(ResultObj, EditorLogHealth);
    ResultObj->SetStringField(TEXT("delete_target_root_mode"), DeleteTargetRootMode);
    ResultObj->SetBoolField(TEXT("check_editor_log_health"), bCheckEditorLogHealth);
    ResultObj->SetBoolField(TEXT("fail_on_editor_log_issues"), bFailOnEditorLogIssues);
    ResultObj->SetBoolField(TEXT("suppress_editor_prompts"), bSuppressEditorPrompts);
    ResultObj->SetBoolField(TEXT("log_archive_remap_objects"), bLogArchiveRemapObjects);
    ResultObj->SetBoolField(TEXT("refresh_blueprint_nodes"), bRefreshBlueprintNodes);
    ResultObj->SetBoolField(TEXT("refresh_failed_blueprint_nodes"), bRefreshFailedBlueprintNodes);
    ResultObj->SetBoolField(TEXT("repair_level_actors"), bRepairLevelActors);
    ResultObj->SetBoolField(TEXT("force_repair_level_actors"), bForceRepairLevelActors);
    AddStringArrayField(ResultObj, TEXT("failed_asset_samples"), FailedAssets);
    AddStringArrayField(ResultObj, TEXT("accepted_fallback_samples"), AcceptedFallbackSamples);
    AddStringArrayField(ResultObj, TEXT("unresolved_fallback_samples"), UnresolvedFallbackSamples);
    AddStringArrayField(ResultObj, TEXT("delete_target_root_warnings"), DeleteTargetRootWarnings);
    AddStringArrayField(ResultObj, TEXT("original_dependency_samples"), OriginalDependencySamples);
    if (!FailedAssets.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("Recreate finished with %d failed assets"), FailedAssets.Num()));
    }
    else if (UnresolvedFallbackCount > 0)
    {
        ResultObj->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("Recreate finished with %d unresolved fresh-create fallback asset(s)"), UnresolvedFallbackCount));
    }
    else if (OriginalDependencyAssetCount > 0)
    {
        ResultObj->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("Recreate finished with %d target assets still depending on the source root"), OriginalDependencyAssetCount));
    }
    else if (bCheckEditorLogHealth && bFailOnEditorLogIssues && !EditorLogHealth.bPass)
    {
        ResultObj->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("Recreate editor log health check found %d issue(s)"), EditorLogHealth.IssueCount));
    }
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandlePostprocessContentFolderMCP(const TSharedPtr<FJsonObject>& Params)
{
    FString SourceRoot;
    FString TargetRoot;
    FString RootParamError;
    if (!TryGetRequiredContentRootParamForMCP(Params, TEXT("source_root"), SourceRoot, RootParamError) ||
        !TryGetRequiredContentRootParamForMCP(Params, TEXT("target_root"), TargetRoot, RootParamError) ||
        !ValidateContentRootPairForMCP(SourceRoot, TargetRoot, RootParamError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(RootParamError);
    }
    const FString Suffix = GetStringParam(Params, TEXT("suffix"), TEXT("_MCP"));
    const bool bDryRun = GetBoolParam(Params, TEXT("dry_run"), false);
    const bool bRemapReferences = GetBoolParam(Params, TEXT("remap_references"), true);
    const bool bCompileBlueprints = GetBoolParam(Params, TEXT("compile_blueprints"), true);
    const bool bRefreshBlueprintNodes = GetBoolParam(Params, TEXT("refresh_blueprint_nodes"), false);
    const bool bRefreshFailedBlueprintNodes = GetBoolParam(Params, TEXT("refresh_failed_blueprint_nodes"), true);
    const bool bSaveAssets = GetBoolParam(Params, TEXT("save_assets"), true);
    const bool bRepairLevelActors = GetBoolParam(Params, TEXT("repair_level_actors"), false);
    const bool bForceRepairLevelActors = GetBoolParam(Params, TEXT("force_repair_level_actors"), false);
    const int32 CompilePasses = FMath::Max(1, GetIntParam(Params, TEXT("compile_passes"), 2));
    const bool bCheckEditorLogHealth = GetBoolParam(Params, TEXT("check_editor_log_health"), true);
    const bool bFailOnEditorLogIssues = GetBoolParam(Params, TEXT("fail_on_editor_log_issues"), true);
    const bool bSuppressEditorPrompts = GetBoolParam(Params, TEXT("suppress_editor_prompts"), true);
    TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, GIsRunningUnattendedScript || bSuppressEditorPrompts);
    const FEditorLogMarker EditorLogMarker = CaptureEditorLogMarker(bCheckEditorLogHealth);

    if (!FPackageName::IsValidLongPackageName(SourceRoot) || !SourceRoot.StartsWith(TEXT("/Game/")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid source_root: %s"), *SourceRoot));
    }

    if (!FPackageName::IsValidLongPackageName(TargetRoot) || !TargetRoot.StartsWith(TEXT("/Game/")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid target_root: %s"), *TargetRoot));
    }

    if (TargetRoot.Equals(SourceRoot) || TargetRoot.StartsWith(SourceRoot + TEXT("/")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("target_root must not be the source root or a child of source_root"));
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    TArray<FString> InitialScanPaths;
    InitialScanPaths.Add(SourceRoot);
    InitialScanPaths.Add(TargetRoot);
    AssetRegistry.ScanPathsSynchronous(InitialScanPaths, true);

    TArray<FAssetData> SourceAssets;
    AssetRegistry.GetAssetsByPath(FName(*SourceRoot), SourceAssets, true);
    SourceAssets.Sort([](const FAssetData& A, const FAssetData& B)
    {
        return A.PackageName.LexicalLess(B.PackageName);
    });

    if (SourceAssets.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("No source assets found under %s"), *SourceRoot));
    }

    TSharedPtr<FJsonObject> ReportObject = MakeShared<FJsonObject>();
    ReportObject->SetStringField(TEXT("operation"), TEXT("postprocess_content_folder_mcp"));
    ReportObject->SetStringField(TEXT("source_root"), SourceRoot);
    ReportObject->SetStringField(TEXT("target_root"), TargetRoot);
    ReportObject->SetStringField(TEXT("suffix"), Suffix);
    ReportObject->SetBoolField(TEXT("dry_run"), bDryRun);

    TArray<TSharedPtr<FJsonValue>> AssetReports;
    TMap<UObject*, UObject*> ReplacementMap;
    TArray<FPathStringReplacement> PathReplacements;
    TArray<TPair<UObject*, UObject*>> SourceTargetPairs;
    TArray<UObject*> TargetObjects;
    TArray<UBlueprint*> TargetBlueprints;
    TArray<UUserDefinedStruct*> TargetUserDefinedStructs;
    TArray<FName> TargetPackageNames;
    TArray<FString> TargetWorldPackageNames;
    TSet<UPackage*> TargetPackages;
    TArray<FString> FailedAssets;
    TArray<FString> MissingTargetAssets;

    int32 PairedAssetCount = 0;
    int32 SavedCount = 0;
    int32 RemappedObjectCount = 0;
    int32 ArchiveObjectRemapSkipCount = 0;
    int32 PathPropertyRemapCount = 0;
    int32 UserDefinedStructDescriptionPathRemapCount = 0;
    int32 SCSNodeRepairCount = 0;
    int32 WidgetTreeRepairCount = 0;
    int32 GeneratedClassSubobjectRepairCount = 0;
    int32 MaterialTextureReferenceRepairCount = 0;
    int32 MaterialCacheRefreshCount = 0;
    int32 LevelActorReferenceRemapCount = 0;
    int32 LevelActorClassRepairCount = 0;
    int32 LevelComponentClassRepairCount = 0;
    int32 SavedMapCount = 0;
    int32 SkippedCleanWorldRepairCount = 0;
    int32 BlueprintGraphReferenceRemapCount = 0;
    int32 CompiledBlueprintCount = 0;
    int32 BlueprintCompileErrorCount = 0;

    for (const FAssetData& SourceAsset : SourceAssets)
    {
        const FString TargetPath = BuildTargetAssetPath(SourceAsset, SourceRoot, TargetRoot, Suffix);
        if (!UEditorAssetLibrary::DoesAssetExist(TargetPath))
        {
            MissingTargetAssets.Add(FString::Printf(TEXT("%s -> %s"), *SourceAsset.PackageName.ToString(), *TargetPath));
            AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                SourceAsset,
                TargetPath,
                TEXT("postprocess_existing_target"),
                TEXT("missing_target"),
                TEXT("target asset does not exist"))));
            continue;
        }

        if (bDryRun)
        {
            AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                SourceAsset,
                TargetPath,
                TEXT("postprocess_existing_target"),
                TEXT("planned"),
                TEXT("target asset exists"))));
            ++PairedAssetCount;
            continue;
        }

        UObject* SourceObject = SourceAsset.GetAsset();
        UObject* TargetObject = UEditorAssetLibrary::LoadAsset(TargetPath);
        if (!SourceObject || !TargetObject)
        {
            FailedAssets.Add(FString::Printf(TEXT("%s -> %s (failed to load source or target asset)"), *SourceAsset.PackageName.ToString(), *TargetPath));
            AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
                SourceAsset,
                TargetPath,
                TEXT("postprocess_existing_target"),
                TEXT("failed"),
                TEXT("failed to load source or target asset"))));
            continue;
        }

        AddReplacementMapping(ReplacementMap, SourceObject, TargetObject);
        SourceTargetPairs.Add(TPair<UObject*, UObject*>(SourceObject, TargetObject));
        TargetObjects.Add(TargetObject);
        TargetPackageNames.Add(TargetObject->GetOutermost()->GetFName());
        AddTargetWorldPackageName(TargetWorldPackageNames, TargetObject);
        TargetPackages.Add(TargetObject->GetOutermost());
        ++PairedAssetCount;

        if (UBlueprint* TargetBlueprint = Cast<UBlueprint>(TargetObject))
        {
            TargetBlueprints.Add(TargetBlueprint);
        }
        if (UUserDefinedStruct* TargetStruct = Cast<UUserDefinedStruct>(TargetObject))
        {
            TargetUserDefinedStructs.Add(TargetStruct);
        }

        AssetReports.Add(MakeShared<FJsonValueObject>(AssetReportToJson(
            SourceAsset,
            TargetPath,
            TEXT("postprocess_existing_target"),
            TEXT("paired"),
            TEXT("loaded_existing_target"))));
    }

    for (const TPair<UObject*, UObject*>& Pair : SourceTargetPairs)
    {
        AddBlueprintReplacementMappings(ReplacementMap, Pair.Key, Pair.Value);
    }

    for (const TPair<UObject*, UObject*>& Pair : SourceTargetPairs)
    {
        UBlueprint* SourceBlueprint = Cast<UBlueprint>(Pair.Key);
        UBlueprint* TargetBlueprint = Cast<UBlueprint>(Pair.Value);
        if (SourceBlueprint && TargetBlueprint)
        {
            SCSNodeRepairCount += RepairBlueprintSCSNodes(SourceBlueprint, TargetBlueprint, ReplacementMap);
            WidgetTreeRepairCount += RepairWidgetBlueprintTree(TargetBlueprint, ReplacementMap);
        }
    }

    if (!bDryRun)
    {
        GeneratedClassSubobjectRepairCount += RepairTargetSubobjectsWithSourceClasses(TargetPackages, ReplacementMap);
    }

    for (const TPair<UObject*, UObject*>& Pair : SourceTargetPairs)
    {
        AddObjectPathReplacements(PathReplacements, Pair.Key, Pair.Value);
    }
    PathReplacements.Sort([](const FPathStringReplacement& A, const FPathStringReplacement& B)
    {
        return A.Source.Len() > B.Source.Len();
    });

    if (!bDryRun && bRemapReferences && !TargetUserDefinedStructs.IsEmpty())
    {
        UserDefinedStructDescriptionPathRemapCount += RemapUserDefinedStructDescriptionPaths(
            TargetUserDefinedStructs,
            PathReplacements,
            ReplacementMap,
            SourceRoot);
    }

    if (!bDryRun && bRemapReferences && ReplacementMap.Num() > 0)
    {
        TSet<UObject*> ObjectsToRemap;
        for (UObject* TargetObject : TargetObjects)
        {
            if (IsLiveObjectForMCP(TargetObject))
            {
                ObjectsToRemap.Add(TargetObject);
            }
        }
        for (UPackage* TargetPackage : TargetPackages)
        {
            if (!TargetPackage)
            {
                continue;
            }

            TArray<UObject*> PackageObjects;
            GetObjectsWithPackage(TargetPackage, PackageObjects, true, RF_Transient, EInternalObjectFlags::Garbage);
            for (UObject* PackageObject : PackageObjects)
            {
                if (IsLiveObjectForMCP(PackageObject))
                {
                    ObjectsToRemap.Add(PackageObject);
                }
            }
        }

        for (UObject* TargetObject : ObjectsToRemap)
        {
            if (!IsLiveObjectForMCP(TargetObject))
            {
                continue;
            }

            if (ShouldSkipAssetArchiveObjectReferenceRemap(TargetObject))
            {
                ++ArchiveObjectRemapSkipCount;
                continue;
            }

            TargetObject->Modify();
            FArchiveReplaceObjectRef<UObject> ReplaceAr(
                TargetObject,
                ReplacementMap,
                EArchiveReplaceObjectFlags::IgnoreOuterRef |
                EArchiveReplaceObjectFlags::IgnoreArchetypeRef |
                EArchiveReplaceObjectFlags::IncludeClassGeneratedByRef);

            if (ReplaceAr.GetCount() > 0)
            {
                ++RemappedObjectCount;
                TargetObject->MarkPackageDirty();
                if (UPackage* Package = TargetObject->GetOutermost())
                {
                    Package->MarkPackageDirty();
                }
            }
        }
    }

    if (!bDryRun && bRemapReferences && ReplacementMap.Num() > 0)
    {
        for (UBlueprint* Blueprint : TargetBlueprints)
        {
            BlueprintGraphReferenceRemapCount += RemapBlueprintGraphReferences(Blueprint, ReplacementMap, PathReplacements);
        }
    }

    if (!bDryRun && bRemapReferences)
    {
        RefreshMaterialReferencesAndCaches(
            TargetPackages,
            ReplacementMap,
            MaterialTextureReferenceRepairCount,
            MaterialCacheRefreshCount);
    }

    if (!bDryRun && bSaveAssets)
    {
        for (UObject* TargetObject : TargetObjects)
        {
            if (SaveLoadedAssetForMCP(TargetObject))
            {
                ++SavedCount;
            }
        }
    }

    TArray<FString> OriginalDependencySamples;
    TSet<FName> OriginalDependencyPackages;
    int32 OriginalDependencyAssetCount = 0;
    if (!bDryRun)
    {
        TArray<FString> PathsToScan;
        PathsToScan.Add(TargetRoot);
        AssetRegistry.ScanPathsSynchronous(PathsToScan, true);
        OriginalDependencyAssetCount = CollectOriginalDependencyPackages(
            AssetRegistry,
            TargetPackageNames,
            SourceRoot,
            OriginalDependencyPackages,
            OriginalDependencySamples,
            50);
    }

    if (!bDryRun && bRemapReferences && PathReplacements.Num() > 0 && OriginalDependencyPackages.Num() > 0)
    {
        TSet<UObject*> ObjectsToPathRemap;
        for (const FName& PackageName : OriginalDependencyPackages)
        {
            UPackage* TargetPackage = FindPackage(nullptr, *PackageName.ToString());
            if (!TargetPackage)
            {
                TargetPackage = LoadPackage(nullptr, *PackageName.ToString(), LOAD_None);
            }
            if (!TargetPackage)
            {
                continue;
            }

            TArray<UObject*> PackageObjects;
            GetObjectsWithPackage(TargetPackage, PackageObjects, true, RF_Transient, EInternalObjectFlags::Garbage);
            for (UObject* PackageObject : PackageObjects)
            {
                if (IsLiveObjectForMCP(PackageObject))
                {
                    ObjectsToPathRemap.Add(PackageObject);
                }
            }
        }

        for (UObject* TargetObject : ObjectsToPathRemap)
        {
            if (!IsLiveObjectForMCP(TargetObject))
            {
                continue;
            }

            PathPropertyRemapCount += ReplaceExportedPropertyPaths(TargetObject, PathReplacements, SourceRoot);
        }

        if (!TargetUserDefinedStructs.IsEmpty())
        {
            UserDefinedStructDescriptionPathRemapCount += RemapUserDefinedStructDescriptionPaths(
                TargetUserDefinedStructs,
                PathReplacements,
                ReplacementMap,
                SourceRoot);
        }

        RefreshMaterialReferencesAndCaches(
            TargetPackages,
            ReplacementMap,
            MaterialTextureReferenceRepairCount,
            MaterialCacheRefreshCount);
    }

    if (!bDryRun && bSaveAssets && (PathPropertyRemapCount > 0 || UserDefinedStructDescriptionPathRemapCount > 0))
    {
        for (UObject* TargetObject : TargetObjects)
        {
            if (SaveLoadedAssetForMCP(TargetObject))
            {
                ++SavedCount;
            }
        }
    }

    if (!bDryRun && bCompileBlueprints)
    {
        CompiledBlueprintCount = CompileBlueprintsForMCP(
            TargetBlueprints,
            CompilePasses,
            bRefreshBlueprintNodes,
            bRefreshFailedBlueprintNodes,
            BlueprintCompileErrorCount,
            &FailedAssets);
    }

    if (!bDryRun && bSaveAssets)
    {
        for (UBlueprint* TargetBlueprint : TargetBlueprints)
        {
            if (SaveLoadedAssetForMCP(TargetBlueprint))
            {
                ++SavedCount;
            }
        }
    }

    if (!bDryRun && bRemapReferences && bRepairLevelActors)
    {
        TArray<FString> WorldPackagesForLevelRepair;
        if (bForceRepairLevelActors)
        {
            WorldPackagesForLevelRepair = TargetWorldPackageNames;
        }
        else
        {
            for (const FString& WorldPackageName : TargetWorldPackageNames)
            {
                if (OriginalDependencyPackages.Contains(FName(*WorldPackageName)))
                {
                    WorldPackagesForLevelRepair.Add(WorldPackageName);
                }
                else
                {
                    ++SkippedCleanWorldRepairCount;
                }
            }
        }

        LevelActorClassRepairCount += RepairTargetWorldActorsWithSourceClasses(
            WorldPackagesForLevelRepair,
            ReplacementMap,
            PathReplacements,
            SourceRoot,
            LevelActorReferenceRemapCount,
            SavedMapCount,
            LevelComponentClassRepairCount,
            FailedAssets);
    }

    if (!bDryRun)
    {
        TArray<FString> PathsToScan;
        PathsToScan.Add(TargetRoot);
        AssetRegistry.ScanPathsSynchronous(PathsToScan, true);
        OriginalDependencyAssetCount = CollectOriginalDependencyPackages(
            AssetRegistry,
            TargetPackageNames,
            SourceRoot,
            OriginalDependencyPackages,
            OriginalDependencySamples,
            50);
    }

    const FEditorLogHealthResult EditorLogHealth = ScanEditorLogSince(EditorLogMarker, TargetRoot);
    const bool bEditorLogVerificationPass = !bCheckEditorLogHealth || !bFailOnEditorLogIssues || EditorLogHealth.bPass;
    const bool bVerificationPass = bDryRun
        ? FailedAssets.IsEmpty() && MissingTargetAssets.IsEmpty() && bEditorLogVerificationPass
        : FailedAssets.IsEmpty() && MissingTargetAssets.IsEmpty() && BlueprintCompileErrorCount == 0 && OriginalDependencyAssetCount == 0 && bEditorLogVerificationPass;

    ReportObject->SetNumberField(TEXT("source_asset_count"), SourceAssets.Num());
    ReportObject->SetNumberField(TEXT("paired_asset_count"), PairedAssetCount);
    ReportObject->SetNumberField(TEXT("missing_target_asset_count"), MissingTargetAssets.Num());
    ReportObject->SetNumberField(TEXT("saved_count"), SavedCount);
    ReportObject->SetStringField(TEXT("asset_save_mode"), TEXT("package_save_no_prompt"));
    ReportObject->SetNumberField(TEXT("remapped_object_count"), RemappedObjectCount);
    ReportObject->SetNumberField(TEXT("archive_object_remap_skip_count"), ArchiveObjectRemapSkipCount);
    ReportObject->SetNumberField(TEXT("path_property_remap_count"), PathPropertyRemapCount);
    ReportObject->SetNumberField(TEXT("user_defined_struct_description_path_remap_count"), UserDefinedStructDescriptionPathRemapCount);
    ReportObject->SetNumberField(TEXT("scs_node_repair_count"), SCSNodeRepairCount);
    ReportObject->SetNumberField(TEXT("widget_tree_repair_count"), WidgetTreeRepairCount);
    ReportObject->SetNumberField(TEXT("generated_class_subobject_repair_count"), GeneratedClassSubobjectRepairCount);
    ReportObject->SetNumberField(TEXT("material_texture_reference_repair_count"), MaterialTextureReferenceRepairCount);
    ReportObject->SetNumberField(TEXT("material_cache_refresh_count"), MaterialCacheRefreshCount);
    ReportObject->SetNumberField(TEXT("level_actor_reference_remap_count"), LevelActorReferenceRemapCount);
    ReportObject->SetNumberField(TEXT("level_actor_class_repair_count"), LevelActorClassRepairCount);
    ReportObject->SetNumberField(TEXT("level_component_class_repair_count"), LevelComponentClassRepairCount);
    ReportObject->SetNumberField(TEXT("saved_map_count"), SavedMapCount);
    ReportObject->SetNumberField(TEXT("skipped_clean_world_repair_count"), SkippedCleanWorldRepairCount);
    ReportObject->SetNumberField(TEXT("blueprint_graph_reference_remap_count"), BlueprintGraphReferenceRemapCount);
    ReportObject->SetNumberField(TEXT("compiled_blueprint_count"), CompiledBlueprintCount);
    ReportObject->SetNumberField(TEXT("blueprint_compile_error_count"), BlueprintCompileErrorCount);
    ReportObject->SetNumberField(TEXT("original_dependency_asset_count"), OriginalDependencyAssetCount);
    AddEditorLogHealthFields(ReportObject, EditorLogHealth);
    ReportObject->SetBoolField(TEXT("verification_pass"), bVerificationPass);
    ReportObject->SetBoolField(TEXT("check_editor_log_health"), bCheckEditorLogHealth);
    ReportObject->SetBoolField(TEXT("fail_on_editor_log_issues"), bFailOnEditorLogIssues);
    ReportObject->SetBoolField(TEXT("suppress_editor_prompts"), bSuppressEditorPrompts);
    ReportObject->SetBoolField(TEXT("refresh_blueprint_nodes"), bRefreshBlueprintNodes);
    ReportObject->SetBoolField(TEXT("refresh_failed_blueprint_nodes"), bRefreshFailedBlueprintNodes);
    ReportObject->SetBoolField(TEXT("repair_level_actors"), bRepairLevelActors);
    ReportObject->SetBoolField(TEXT("force_repair_level_actors"), bForceRepairLevelActors);
    AddStringArrayField(ReportObject, TEXT("failed_assets"), FailedAssets);
    AddStringArrayField(ReportObject, TEXT("missing_target_assets"), MissingTargetAssets);
    AddStringArrayField(ReportObject, TEXT("original_dependency_samples"), OriginalDependencySamples);
    ReportObject->SetArrayField(TEXT("assets"), AssetReports);
    if (!FailedAssets.IsEmpty())
    {
        ReportObject->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("Postprocess finished with %d failed assets"), FailedAssets.Num()));
    }
    else if (!MissingTargetAssets.IsEmpty())
    {
        ReportObject->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("Postprocess found %d missing target assets"), MissingTargetAssets.Num()));
    }
    else if (OriginalDependencyAssetCount > 0)
    {
        ReportObject->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("Postprocess finished with %d target assets still depending on the source root"), OriginalDependencyAssetCount));
    }
    else if (bCheckEditorLogHealth && bFailOnEditorLogIssues && !EditorLogHealth.bPass)
    {
        ReportObject->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("Postprocess editor log health check found %d issue(s)"), EditorLogHealth.IssueCount));
    }

    FString ReportJson;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ReportJson);
    FJsonSerializer::Serialize(ReportObject.ToSharedRef(), Writer);

    const FString ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MCP"));
    IFileManager::Get().MakeDirectory(*ReportDirectory, true);
    const FString ReportFilename = BuildMCPReportFilename(TargetRoot, TEXT("postprocess"));
    const FString ReportPath = FPaths::Combine(ReportDirectory, ReportFilename);
    FFileHelper::SaveStringToFile(ReportJson, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetBoolField(TEXT("verification_pass"), bVerificationPass);
    ResultObj->SetStringField(TEXT("source_root"), SourceRoot);
    ResultObj->SetStringField(TEXT("target_root"), TargetRoot);
    ResultObj->SetStringField(TEXT("report_path"), ReportPath);
    ResultObj->SetNumberField(TEXT("source_asset_count"), SourceAssets.Num());
    ResultObj->SetNumberField(TEXT("paired_asset_count"), PairedAssetCount);
    ResultObj->SetNumberField(TEXT("missing_target_asset_count"), MissingTargetAssets.Num());
    ResultObj->SetNumberField(TEXT("saved_count"), SavedCount);
    ResultObj->SetStringField(TEXT("asset_save_mode"), TEXT("package_save_no_prompt"));
    ResultObj->SetNumberField(TEXT("remapped_object_count"), RemappedObjectCount);
    ResultObj->SetNumberField(TEXT("archive_object_remap_skip_count"), ArchiveObjectRemapSkipCount);
    ResultObj->SetNumberField(TEXT("path_property_remap_count"), PathPropertyRemapCount);
    ResultObj->SetNumberField(TEXT("user_defined_struct_description_path_remap_count"), UserDefinedStructDescriptionPathRemapCount);
    ResultObj->SetNumberField(TEXT("scs_node_repair_count"), SCSNodeRepairCount);
    ResultObj->SetNumberField(TEXT("widget_tree_repair_count"), WidgetTreeRepairCount);
    ResultObj->SetNumberField(TEXT("generated_class_subobject_repair_count"), GeneratedClassSubobjectRepairCount);
    ResultObj->SetNumberField(TEXT("material_texture_reference_repair_count"), MaterialTextureReferenceRepairCount);
    ResultObj->SetNumberField(TEXT("material_cache_refresh_count"), MaterialCacheRefreshCount);
    ResultObj->SetNumberField(TEXT("level_actor_reference_remap_count"), LevelActorReferenceRemapCount);
    ResultObj->SetNumberField(TEXT("level_actor_class_repair_count"), LevelActorClassRepairCount);
    ResultObj->SetNumberField(TEXT("level_component_class_repair_count"), LevelComponentClassRepairCount);
    ResultObj->SetNumberField(TEXT("saved_map_count"), SavedMapCount);
    ResultObj->SetNumberField(TEXT("skipped_clean_world_repair_count"), SkippedCleanWorldRepairCount);
    ResultObj->SetNumberField(TEXT("blueprint_graph_reference_remap_count"), BlueprintGraphReferenceRemapCount);
    ResultObj->SetNumberField(TEXT("compiled_blueprint_count"), CompiledBlueprintCount);
    ResultObj->SetNumberField(TEXT("blueprint_compile_error_count"), BlueprintCompileErrorCount);
    ResultObj->SetNumberField(TEXT("original_dependency_asset_count"), OriginalDependencyAssetCount);
    AddEditorLogHealthFields(ResultObj, EditorLogHealth);
    ResultObj->SetBoolField(TEXT("check_editor_log_health"), bCheckEditorLogHealth);
    ResultObj->SetBoolField(TEXT("fail_on_editor_log_issues"), bFailOnEditorLogIssues);
    ResultObj->SetBoolField(TEXT("suppress_editor_prompts"), bSuppressEditorPrompts);
    ResultObj->SetBoolField(TEXT("refresh_blueprint_nodes"), bRefreshBlueprintNodes);
    ResultObj->SetBoolField(TEXT("refresh_failed_blueprint_nodes"), bRefreshFailedBlueprintNodes);
    ResultObj->SetBoolField(TEXT("repair_level_actors"), bRepairLevelActors);
    ResultObj->SetBoolField(TEXT("force_repair_level_actors"), bForceRepairLevelActors);
    AddStringArrayField(ResultObj, TEXT("failed_asset_samples"), FailedAssets);
    AddStringArrayField(ResultObj, TEXT("missing_target_asset_samples"), MissingTargetAssets);
    AddStringArrayField(ResultObj, TEXT("original_dependency_samples"), OriginalDependencySamples);
    if (!FailedAssets.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("Postprocess finished with %d failed assets"), FailedAssets.Num()));
    }
    else if (!MissingTargetAssets.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("Postprocess found %d missing target assets"), MissingTargetAssets.Num()));
    }
    else if (OriginalDependencyAssetCount > 0)
    {
        ResultObj->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("Postprocess finished with %d target assets still depending on the source root"), OriginalDependencyAssetCount));
    }
    else if (bCheckEditorLogHealth && bFailOnEditorLogIssues && !EditorLogHealth.bPass)
    {
        ResultObj->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("Postprocess editor log health check found %d issue(s)"), EditorLogHealth.IssueCount));
    }
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleRepairWorldActorInstancesMCP(const TSharedPtr<FJsonObject>& Params)
{
    FString SourceRoot;
    FString TargetRoot;
    FString RootParamError;
    if (!TryGetRequiredContentRootParamForMCP(Params, TEXT("source_root"), SourceRoot, RootParamError) ||
        !TryGetRequiredContentRootParamForMCP(Params, TEXT("target_root"), TargetRoot, RootParamError) ||
        !ValidateContentRootPairForMCP(SourceRoot, TargetRoot, RootParamError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(RootParamError);
    }
    const FString Suffix = GetStringParam(Params, TEXT("suffix"), TEXT("_MCP"));
    const FString MapPath = NormalizePackagePathParam(GetStringParam(Params, TEXT("map_path"), FString()));
    const bool bDryRun = GetBoolParam(Params, TEXT("dry_run"), false);
    const bool bSaveMap = GetBoolParam(Params, TEXT("save_map"), true);
    const bool bRemapLevelBlueprint = GetBoolParam(Params, TEXT("remap_level_blueprint"), false);
    const bool bRepairActorClasses = GetBoolParam(Params, TEXT("repair_actor_classes"), true);
    const bool bRepairComponentClasses = GetBoolParam(Params, TEXT("repair_component_classes"), true);
    const bool bRemapActorReferences = GetBoolParam(Params, TEXT("remap_actor_references"), true);
    const bool bRemoveSourceObjectMapKeys = GetBoolParam(Params, TEXT("remove_source_object_map_keys"), true);
    const bool bCheckEditorLogHealth = GetBoolParam(Params, TEXT("check_editor_log_health"), true);
    const bool bFailOnEditorLogIssues = GetBoolParam(Params, TEXT("fail_on_editor_log_issues"), true);
    const bool bSuppressEditorPrompts = GetBoolParam(Params, TEXT("suppress_editor_prompts"), true);
    TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, GIsRunningUnattendedScript || bSuppressEditorPrompts);
    const FEditorLogMarker EditorLogMarker = CaptureEditorLogMarker(bCheckEditorLogHealth);

    if (!FPackageName::IsValidLongPackageName(SourceRoot) || !SourceRoot.StartsWith(TEXT("/Game/")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid source_root: %s"), *SourceRoot));
    }

    if (!FPackageName::IsValidLongPackageName(TargetRoot) || !TargetRoot.StartsWith(TEXT("/Game/")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid target_root: %s"), *TargetRoot));
    }

    if (TargetRoot.Equals(SourceRoot) || TargetRoot.StartsWith(SourceRoot + TEXT("/")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("target_root must not be the source root or a child of source_root"));
    }

    if (MapPath.IsEmpty() || !FPackageName::IsValidLongPackageName(MapPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid map_path: %s"), *MapPath));
    }

    if (!MapPath.Equals(TargetRoot) && !MapPath.StartsWith(TargetRoot + TEXT("/")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("map_path must be under target_root"));
    }

    if (!UEditorAssetLibrary::DoesAssetExist(MapPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Target map does not exist: %s"), *MapPath));
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    TArray<FString> InitialScanPaths;
    InitialScanPaths.Add(SourceRoot);
    InitialScanPaths.Add(TargetRoot);
    AssetRegistry.ScanPathsSynchronous(InitialScanPaths, true);

    TArray<FAssetData> SourceAssets;
    AssetRegistry.GetAssetsByPath(FName(*SourceRoot), SourceAssets, true);
    SourceAssets.Sort([](const FAssetData& A, const FAssetData& B)
    {
        return A.PackageName.LexicalLess(B.PackageName);
    });

    if (SourceAssets.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("No source assets found under %s"), *SourceRoot));
    }

    TMap<UObject*, UObject*> ReplacementMap;
    TArray<TPair<UObject*, UObject*>> SourceTargetPairs;
    TArray<FString> MissingTargetAssets;
    TArray<FString> FailedAssets;
    int32 PairedAssetCount = 0;

    for (const FAssetData& SourceAsset : SourceAssets)
    {
        const FString TargetPath = BuildTargetAssetPath(SourceAsset, SourceRoot, TargetRoot, Suffix);
        if (!UEditorAssetLibrary::DoesAssetExist(TargetPath))
        {
            MissingTargetAssets.Add(FString::Printf(TEXT("%s -> %s"), *SourceAsset.PackageName.ToString(), *TargetPath));
            continue;
        }

        UObject* SourceObject = SourceAsset.GetAsset();
        UObject* TargetObject = UEditorAssetLibrary::LoadAsset(TargetPath);
        if (!SourceObject || !TargetObject)
        {
            FailedAssets.Add(FString::Printf(TEXT("%s -> %s (failed to load source or target asset)"), *SourceAsset.PackageName.ToString(), *TargetPath));
            continue;
        }

        AddReplacementMapping(ReplacementMap, SourceObject, TargetObject);
        SourceTargetPairs.Add(TPair<UObject*, UObject*>(SourceObject, TargetObject));
        ++PairedAssetCount;
    }

    for (const TPair<UObject*, UObject*>& Pair : SourceTargetPairs)
    {
        AddBlueprintReplacementMappings(ReplacementMap, Pair.Key, Pair.Value);
    }

    TArray<FPathStringReplacement> PathReplacements;
    for (const TPair<UObject*, UObject*>& Pair : SourceTargetPairs)
    {
        AddObjectPathReplacements(PathReplacements, Pair.Key, Pair.Value);
    }
    PathReplacements.Sort([](const FPathStringReplacement& A, const FPathStringReplacement& B)
    {
        return A.Source.Len() > B.Source.Len();
    });

    UWorld* LoadedWorld = LoadWorldPackageForRepair(MapPath);
    if (!LoadedWorld)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load target map: %s"), *MapPath));
    }

    int32 BeforeSourceActorCount = 0;
    int32 BeforeSourceComponentCount = 0;
    CountWorldSourceClassInstances(LoadedWorld, SourceRoot, BeforeSourceActorCount, BeforeSourceComponentCount);
    const int32 BeforeSourceObjectMapKeyCount = CountSourceObjectKeyMapEntriesFromWorld(LoadedWorld, SourceRoot);

    int32 LevelActorReferenceRemapCount = 0;
    int32 SavedMapCount = 0;
    int32 LevelComponentClassRepairCount = 0;
    int32 LevelActorClassRepairCount = 0;

    if (!bDryRun)
    {
        LevelActorClassRepairCount = RepairLoadedWorldActorInstances(
            LoadedWorld,
            MapPath,
            ReplacementMap,
            PathReplacements,
            SourceRoot,
            LevelActorReferenceRemapCount,
            SavedMapCount,
            LevelComponentClassRepairCount,
            FailedAssets,
            bRemapLevelBlueprint,
            bRepairActorClasses,
            bRepairComponentClasses,
            bRemapActorReferences,
            bRemoveSourceObjectMapKeys,
            bSaveMap);
    }

    int32 AfterSourceActorCount = 0;
    int32 AfterSourceComponentCount = 0;
    CountWorldSourceClassInstances(LoadedWorld, SourceRoot, AfterSourceActorCount, AfterSourceComponentCount);
    const int32 AfterSourceObjectMapKeyCount = CountSourceObjectKeyMapEntriesFromWorld(LoadedWorld, SourceRoot);

    TArray<FString> SourceDependencySamples;
    int32 SourceHardDependencyCount = 0;
    {
        TArray<FString> PathsToScan;
        PathsToScan.Add(FPackageName::GetLongPackagePath(MapPath));
        AssetRegistry.ScanPathsSynchronous(PathsToScan, true);

        TArray<FName> Dependencies;
        AssetRegistry.GetDependencies(FName(*MapPath), Dependencies);
        for (const FName& Dependency : Dependencies)
        {
            const FString DependencyPath = Dependency.ToString();
            if (DependencyPath.Equals(SourceRoot) || DependencyPath.StartsWith(SourceRoot + TEXT("/")))
            {
                ++SourceHardDependencyCount;
                if (SourceDependencySamples.Num() < 50)
                {
                    SourceDependencySamples.Add(FString::Printf(TEXT("%s -> %s"), *MapPath, *DependencyPath));
                }
            }
        }
    }

    const FEditorLogHealthResult EditorLogHealth = ScanEditorLogSince(EditorLogMarker, TargetRoot);
    const bool bEditorLogVerificationPass = !bCheckEditorLogHealth || !bFailOnEditorLogIssues || EditorLogHealth.bPass;
    const bool bVerificationPass = FailedAssets.IsEmpty()
        && MissingTargetAssets.IsEmpty()
        && AfterSourceActorCount == 0
        && AfterSourceComponentCount == 0
        && AfterSourceObjectMapKeyCount == 0
        && SourceHardDependencyCount == 0
        && bEditorLogVerificationPass;

    TSharedPtr<FJsonObject> ReportObject = MakeShared<FJsonObject>();
    ReportObject->SetStringField(TEXT("operation"), TEXT("repair_world_actor_instances_mcp"));
    ReportObject->SetBoolField(TEXT("success"), true);
    ReportObject->SetBoolField(TEXT("verification_pass"), bVerificationPass);
    ReportObject->SetStringField(TEXT("source_root"), SourceRoot);
    ReportObject->SetStringField(TEXT("target_root"), TargetRoot);
    ReportObject->SetStringField(TEXT("map_path"), MapPath);
    ReportObject->SetStringField(TEXT("suffix"), Suffix);
    ReportObject->SetBoolField(TEXT("dry_run"), bDryRun);
    ReportObject->SetBoolField(TEXT("save_map"), bSaveMap);
    ReportObject->SetBoolField(TEXT("remap_level_blueprint"), bRemapLevelBlueprint);
    ReportObject->SetBoolField(TEXT("repair_actor_classes"), bRepairActorClasses);
    ReportObject->SetBoolField(TEXT("repair_component_classes"), bRepairComponentClasses);
    ReportObject->SetBoolField(TEXT("remap_actor_references"), bRemapActorReferences);
    ReportObject->SetBoolField(TEXT("remove_source_object_map_keys"), bRemoveSourceObjectMapKeys);
    ReportObject->SetBoolField(TEXT("check_editor_log_health"), bCheckEditorLogHealth);
    ReportObject->SetBoolField(TEXT("fail_on_editor_log_issues"), bFailOnEditorLogIssues);
    ReportObject->SetBoolField(TEXT("suppress_editor_prompts"), bSuppressEditorPrompts);
    ReportObject->SetNumberField(TEXT("source_asset_count"), SourceAssets.Num());
    ReportObject->SetNumberField(TEXT("paired_asset_count"), PairedAssetCount);
    ReportObject->SetNumberField(TEXT("missing_target_asset_count"), MissingTargetAssets.Num());
    ReportObject->SetNumberField(TEXT("before_source_actor_count"), BeforeSourceActorCount);
    ReportObject->SetNumberField(TEXT("before_source_component_count"), BeforeSourceComponentCount);
    ReportObject->SetNumberField(TEXT("before_source_object_map_key_count"), BeforeSourceObjectMapKeyCount);
    ReportObject->SetNumberField(TEXT("after_source_actor_count"), AfterSourceActorCount);
    ReportObject->SetNumberField(TEXT("after_source_component_count"), AfterSourceComponentCount);
    ReportObject->SetNumberField(TEXT("after_source_object_map_key_count"), AfterSourceObjectMapKeyCount);
    ReportObject->SetNumberField(TEXT("level_actor_reference_remap_count"), LevelActorReferenceRemapCount);
    ReportObject->SetNumberField(TEXT("level_actor_class_repair_count"), LevelActorClassRepairCount);
    ReportObject->SetNumberField(TEXT("level_component_class_repair_count"), LevelComponentClassRepairCount);
    ReportObject->SetNumberField(TEXT("saved_map_count"), SavedMapCount);
    ReportObject->SetNumberField(TEXT("source_hard_dependency_count"), SourceHardDependencyCount);
    AddEditorLogHealthFields(ReportObject, EditorLogHealth);
    AddStringArrayField(ReportObject, TEXT("failed_assets"), FailedAssets);
    AddStringArrayField(ReportObject, TEXT("missing_target_assets"), MissingTargetAssets);
    AddStringArrayField(ReportObject, TEXT("source_dependency_samples"), SourceDependencySamples);
    if (!bVerificationPass)
    {
        if (bCheckEditorLogHealth && bFailOnEditorLogIssues && !EditorLogHealth.bPass)
        {
            ReportObject->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("World actor repair editor log health check found %d issue(s)"), EditorLogHealth.IssueCount));
        }
        else
        {
            ReportObject->SetStringField(TEXT("verification_error"), TEXT("World actor repair finished with residual source references or failed assets"));
        }
    }

    FString ReportJson;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ReportJson);
    FJsonSerializer::Serialize(ReportObject.ToSharedRef(), Writer);

    const FString ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MCP"));
    IFileManager::Get().MakeDirectory(*ReportDirectory, true);
    const FString ReportFilename = BuildMCPReportFilename(MapPath, TEXT("world_repair"));
    const FString ReportPath = FPaths::Combine(ReportDirectory, ReportFilename);
    FFileHelper::SaveStringToFile(ReportJson, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetBoolField(TEXT("verification_pass"), bVerificationPass);
    ResultObj->SetStringField(TEXT("source_root"), SourceRoot);
    ResultObj->SetStringField(TEXT("target_root"), TargetRoot);
    ResultObj->SetStringField(TEXT("map_path"), MapPath);
    ResultObj->SetStringField(TEXT("report_path"), ReportPath);
    ResultObj->SetBoolField(TEXT("check_editor_log_health"), bCheckEditorLogHealth);
    ResultObj->SetBoolField(TEXT("fail_on_editor_log_issues"), bFailOnEditorLogIssues);
    ResultObj->SetBoolField(TEXT("suppress_editor_prompts"), bSuppressEditorPrompts);
    ResultObj->SetNumberField(TEXT("source_asset_count"), SourceAssets.Num());
    ResultObj->SetNumberField(TEXT("paired_asset_count"), PairedAssetCount);
    ResultObj->SetNumberField(TEXT("missing_target_asset_count"), MissingTargetAssets.Num());
    ResultObj->SetNumberField(TEXT("before_source_actor_count"), BeforeSourceActorCount);
    ResultObj->SetNumberField(TEXT("before_source_component_count"), BeforeSourceComponentCount);
    ResultObj->SetNumberField(TEXT("before_source_object_map_key_count"), BeforeSourceObjectMapKeyCount);
    ResultObj->SetNumberField(TEXT("after_source_actor_count"), AfterSourceActorCount);
    ResultObj->SetNumberField(TEXT("after_source_component_count"), AfterSourceComponentCount);
    ResultObj->SetNumberField(TEXT("after_source_object_map_key_count"), AfterSourceObjectMapKeyCount);
    ResultObj->SetNumberField(TEXT("level_actor_reference_remap_count"), LevelActorReferenceRemapCount);
    ResultObj->SetNumberField(TEXT("level_actor_class_repair_count"), LevelActorClassRepairCount);
    ResultObj->SetNumberField(TEXT("level_component_class_repair_count"), LevelComponentClassRepairCount);
    ResultObj->SetNumberField(TEXT("saved_map_count"), SavedMapCount);
    ResultObj->SetNumberField(TEXT("source_hard_dependency_count"), SourceHardDependencyCount);
    AddEditorLogHealthFields(ResultObj, EditorLogHealth);
    AddStringArrayField(ResultObj, TEXT("failed_asset_samples"), FailedAssets);
    AddStringArrayField(ResultObj, TEXT("missing_target_asset_samples"), MissingTargetAssets);
    AddStringArrayField(ResultObj, TEXT("source_dependency_samples"), SourceDependencySamples);
    if (!bVerificationPass)
    {
        if (bCheckEditorLogHealth && bFailOnEditorLogIssues && !EditorLogHealth.bPass)
        {
            ResultObj->SetStringField(TEXT("verification_error"), FString::Printf(TEXT("World actor repair editor log health check found %d issue(s)"), EditorLogHealth.IssueCount));
        }
        else
        {
            ResultObj->SetStringField(TEXT("verification_error"), TEXT("World actor repair finished with residual source references or failed assets"));
        }
    }
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleExecutePython(const TSharedPtr<FJsonObject>& Params)
{
    FString Code;
    if (!Params->TryGetStringField(TEXT("code"), Code))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'code' parameter"));
    }

    IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
    if (!PythonPlugin || !PythonPlugin->IsPythonAvailable())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("PythonScriptPlugin is not available"));
    }

    bool bAllowUnsafeEditorScriptingDuringPIE = false;
    Params->TryGetBoolField(TEXT("allow_unsafe_editor_scripting_during_pie"), bAllowUnsafeEditorScriptingDuringPIE);

    FString UnsafePattern;
    if (!bAllowUnsafeEditorScriptingDuringPIE && IsPlaySessionActive() && FindUnsafeEditorScriptingPattern(Code, UnsafePattern))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Unsafe editor scripting call '%s' is blocked while PIE/SIE is active. End play mode first or rerun outside PIE."),
            *UnsafePattern));
    }

    FString BlockedMapTransitionPattern;
    if (FindBlockedPythonMapTransitionPattern(Code, BlockedMapTransitionPattern))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Blocked Python map transition call '%s' in execute_python. Map load/new-map calls through this Python bridge can keep old world packages referenced by FPyReferenceCollector and crash the editor with World Memory Leaks. Use native open_editor_level for existing maps, native safe_new_preview_map for temporary preview maps, or switch maps manually in the editor."),
            *BlockedMapTransitionPattern));
    }

    FString Mode;
    Params->TryGetStringField(TEXT("mode"), Mode);

    FPythonCommandEx PythonCommand;
    PythonCommand.Command = Code;
    PythonCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteStatement;
    PythonCommand.FileExecutionScope = EPythonFileExecutionScope::Public;

    if (!Mode.IsEmpty())
    {
        EPythonCommandExecutionMode ParsedMode;
        if (LexTryParseString(ParsedMode, *Mode))
        {
            PythonCommand.ExecutionMode = ParsedMode;
        }
    }

    const bool bExecuted = PythonPlugin->ExecPythonCommandEx(PythonCommand);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), bExecuted);
    ResultObj->SetStringField(TEXT("command_result"), PythonCommand.CommandResult);

    TArray<TSharedPtr<FJsonValue>> Logs;
    for (const FPythonLogOutputEntry& Entry : PythonCommand.LogOutput)
    {
        TSharedPtr<FJsonObject> LogEntryJson = MakeShared<FJsonObject>();
        LogEntryJson->SetStringField(TEXT("type"), LexToString(Entry.Type));
        LogEntryJson->SetStringField(TEXT("output"), Entry.Output);
        Logs.Add(MakeShared<FJsonValueObject>(LogEntryJson));
    }
    ResultObj->SetArrayField(TEXT("logs"), Logs);

    if (!bExecuted && PythonCommand.CommandResult.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("error"), TEXT("Python execution failed"));
    }

    return ResultObj;
}
