#include "Commands/UnrealMCPEditorCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Bookmarks/IBookmarkTypeTools.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "FileHelpers.h"
#include "LevelEditorViewport.h"
#include "ImageUtils.h"
#include "HighResScreenshot.h"
#include "Engine/GameViewportClient.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "Components/StaticMeshComponent.h"
#include "EditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "HAL/PlatformTime.h"
#include "UI/NiagaraPreviewPlayerWindow.h"

namespace
{
    TArray<TSharedPtr<FJsonValue>> EditorVectorToJsonArray(const FVector& Value)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        Array.Add(MakeShared<FJsonValueNumber>(Value.X));
        Array.Add(MakeShared<FJsonValueNumber>(Value.Y));
        Array.Add(MakeShared<FJsonValueNumber>(Value.Z));
        return Array;
    }

    TArray<TSharedPtr<FJsonValue>> EditorRotatorToJsonArray(const FRotator& Value)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        Array.Add(MakeShared<FJsonValueNumber>(Value.Pitch));
        Array.Add(MakeShared<FJsonValueNumber>(Value.Yaw));
        Array.Add(MakeShared<FJsonValueNumber>(Value.Roll));
        return Array;
    }

    TArray<TSharedPtr<FJsonValue>> StringArrayToJsonArray(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FString& Value : Values)
        {
            Array.Add(MakeShared<FJsonValueString>(Value));
        }
        return Array;
    }

    TArray<FString> GetDirtyPackageNames()
    {
        TArray<UPackage*> DirtyPackages;
        FEditorFileUtils::GetDirtyPackages(DirtyPackages);

        TArray<FString> DirtyPackageNames;
        TSet<FString> SeenPackageNames;
        for (UPackage* Package : DirtyPackages)
        {
            if (!Package)
            {
                continue;
            }

            const FString PackageName = Package->GetName();
            if (SeenPackageNames.Contains(PackageName))
            {
                continue;
            }

            SeenPackageNames.Add(PackageName);
            DirtyPackageNames.Add(PackageName);
        }

        DirtyPackageNames.Sort();
        return DirtyPackageNames;
    }

    FString NormalizeLevelPackageName(const FString& InLevelPath)
    {
        FString LevelPath = FPackageName::ExportTextPathToObjectPath(InLevelPath).TrimStartAndEnd();
        LevelPath.TrimQuotesInline();
        LevelPath.ReplaceInline(TEXT("\\"), TEXT("/"));

        if (LevelPath.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
        {
            FString LongPackageName;
            if (FPackageName::TryConvertFilenameToLongPackageName(LevelPath, LongPackageName))
            {
                return LongPackageName;
            }
        }

        if (LevelPath.Contains(TEXT(".")))
        {
            LevelPath = FPackageName::ObjectPathToPackageName(LevelPath);
        }

        return LevelPath;
    }

    FString GetCurrentEditorWorldPackageName()
    {
        if (!GEditor)
        {
            return FString();
        }

        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World || !World->GetOutermost())
        {
            return FString();
        }

        return World->GetOutermost()->GetName();
    }

    void AddDirtyPackageSummary(TSharedPtr<FJsonObject>& ResultObj)
    {
        const TArray<FString> DirtyPackageNames = GetDirtyPackageNames();
        ResultObj->SetNumberField(TEXT("dirty_package_count"), DirtyPackageNames.Num());
        ResultObj->SetArrayField(TEXT("dirty_packages"), StringArrayToJsonArray(DirtyPackageNames));
    }

    void AddDirtyPackageCommandDelta(TSharedPtr<FJsonObject>& ResultObj, const TArray<FString>& DirtyPackagesBefore)
    {
        const TArray<FString> DirtyPackagesAfter = GetDirtyPackageNames();

        TSet<FString> BeforeSet;
        for (const FString& PackageName : DirtyPackagesBefore)
        {
            BeforeSet.Add(PackageName);
        }

        TSet<FString> AfterSet;
        for (const FString& PackageName : DirtyPackagesAfter)
        {
            AfterSet.Add(PackageName);
        }

        TArray<FString> AddedPackages;
        for (const FString& PackageName : DirtyPackagesAfter)
        {
            if (!BeforeSet.Contains(PackageName))
            {
                AddedPackages.Add(PackageName);
            }
        }

        TArray<FString> RemovedPackages;
        for (const FString& PackageName : DirtyPackagesBefore)
        {
            if (!AfterSet.Contains(PackageName))
            {
                RemovedPackages.Add(PackageName);
            }
        }

        AddedPackages.Sort();
        RemovedPackages.Sort();

        ResultObj->SetNumberField(TEXT("dirty_package_count_before"), DirtyPackagesBefore.Num());
        ResultObj->SetArrayField(TEXT("dirty_packages_before"), StringArrayToJsonArray(DirtyPackagesBefore));
        ResultObj->SetNumberField(TEXT("dirty_package_count_after"), DirtyPackagesAfter.Num());
        ResultObj->SetArrayField(TEXT("dirty_packages_after"), StringArrayToJsonArray(DirtyPackagesAfter));
        ResultObj->SetNumberField(TEXT("dirty_package_added_count"), AddedPackages.Num());
        ResultObj->SetArrayField(TEXT("dirty_packages_added_by_command"), StringArrayToJsonArray(AddedPackages));
        ResultObj->SetNumberField(TEXT("dirty_package_removed_count"), RemovedPackages.Num());
        ResultObj->SetArrayField(TEXT("dirty_packages_removed_by_command"), StringArrayToJsonArray(RemovedPackages));
    }

    TSharedPtr<FJsonObject> CaptureActiveEditorViewportToPng(
        const FString& InFilePath,
        FViewport* Viewport,
        FEditorViewportClient* ViewportClient,
        int32 RedrawCount)
    {
        if (!Viewport || !ViewportClient)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No active editor viewport is available"));
        }

        FString FilePath = InFilePath;
        if (!FilePath.EndsWith(TEXT(".png"), ESearchCase::IgnoreCase))
        {
            FilePath += TEXT(".png");
        }
        FPaths::NormalizeFilename(FilePath);

        const FString Directory = FPaths::GetPath(FilePath);
        if (!Directory.IsEmpty())
        {
            IFileManager::Get().MakeDirectory(*Directory, true);
        }

        ViewportClient->Invalidate();
        Viewport->Invalidate();
        for (int32 DrawIndex = 0; DrawIndex < FMath::Clamp(RedrawCount, 0, 3); ++DrawIndex)
        {
            Viewport->Draw(false);
        }

        const FIntPoint Size = Viewport->GetSizeXY();
        if (Size.X <= 0 || Size.Y <= 0)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Active viewport has invalid size"));
        }

        TArray<FColor> Bitmap;
        const FIntRect ViewportRect(0, 0, Size.X, Size.Y);
        if (!Viewport->ReadPixels(Bitmap, FReadSurfaceDataFlags(), ViewportRect))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to read active viewport pixels"));
        }

        TArray64<uint8> CompressedBitmap;
        FImageUtils::PNGCompressImageArray(
            Size.X,
            Size.Y,
            TArrayView64<const FColor>(Bitmap.GetData(), Bitmap.Num()),
            CompressedBitmap);

        if (!FFileHelper::SaveArrayToFile(CompressedBitmap, *FilePath))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to save screenshot: %s"), *FilePath));
        }

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetBoolField(TEXT("success"), true);
        ResultObj->SetStringField(TEXT("filepath"), FilePath);
        ResultObj->SetNumberField(TEXT("width"), Size.X);
        ResultObj->SetNumberField(TEXT("height"), Size.Y);
        ResultObj->SetNumberField(TEXT("pixel_count"), Bitmap.Num());
        ResultObj->SetNumberField(TEXT("compressed_byte_count"), static_cast<double>(CompressedBitmap.Num()));
        ResultObj->SetNumberField(TEXT("file_size_bytes"), static_cast<double>(IFileManager::Get().FileSize(*FilePath)));
        ResultObj->SetNumberField(TEXT("redraw_count"), FMath::Clamp(RedrawCount, 0, 3));
        ResultObj->SetArrayField(TEXT("view_location"), EditorVectorToJsonArray(ViewportClient->GetViewLocation()));
        ResultObj->SetArrayField(TEXT("view_rotation"), EditorRotatorToJsonArray(ViewportClient->GetViewRotation()));
        ResultObj->SetBoolField(TEXT("is_perspective"), ViewportClient->IsPerspective());
        AddDirtyPackageSummary(ResultObj);
        return ResultObj;
    }

    constexpr const TCHAR* NiagaraPreviewLabMapPackage = TEXT("/Game/SampleTestMap/Niagara_TestMap");
    constexpr const TCHAR* NiagaraPreviewLabMapObject = TEXT("/Game/SampleTestMap/Niagara_TestMap.Niagara_TestMap");
    constexpr const TCHAR* NiagaraPreviewLabPrefix = TEXT("MCP_NiagaraPreviewLab_");
    constexpr const TCHAR* LegacyNiagaraReviewPrefix = TEXT("MCP_NiagaraReview_");

    bool IsNiagaraPreviewLabActor(const AActor* Actor)
    {
        if (!Actor)
        {
            return false;
        }

        const FString Label = Actor->GetActorLabel();
        return Label.StartsWith(NiagaraPreviewLabPrefix) || Label.StartsWith(LegacyNiagaraReviewPrefix);
    }

    UWorld* GetEditorWorldForNiagaraPreviewLab()
    {
        if (!GEditor)
        {
            return nullptr;
        }

        return GEditor->GetEditorWorldContext().World();
    }

    FString GetNiagaraPreviewLabOutputRoot()
    {
        FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MCP"), TEXT("NiagaraReviews"));
        Root = FPaths::ConvertRelativePathToFull(Root);
        FPaths::NormalizeFilename(Root);
        return Root;
    }

    FLevelEditorViewportClient* GetActiveLevelEditorViewportClient()
    {
        if (!GEditor)
        {
            return nullptr;
        }

        FViewport* ActiveViewport = GEditor->GetActiveViewport();
        if (!ActiveViewport)
        {
            return GCurrentLevelEditingViewportClient;
        }

        for (FLevelEditorViewportClient* Candidate : GEditor->GetLevelViewportClients())
        {
            if (Candidate && Candidate->Viewport == ActiveViewport)
            {
                return Candidate;
            }
        }

        return GCurrentLevelEditingViewportClient;
    }

    bool ResolveNiagaraPreviewLabFilePath(const FString& InputPath, FString& OutPath, FString& OutError)
    {
        if (InputPath.IsEmpty())
        {
            OutError = TEXT("Missing 'filepath' parameter");
            return false;
        }

        FString Candidate = InputPath;
        if (!Candidate.EndsWith(TEXT(".png")))
        {
            Candidate += TEXT(".png");
        }

        const FString AllowedRoot = GetNiagaraPreviewLabOutputRoot();
        if (FPaths::IsRelative(Candidate))
        {
            Candidate = FPaths::Combine(AllowedRoot, Candidate);
        }

        Candidate = FPaths::ConvertRelativePathToFull(Candidate);
        FPaths::NormalizeFilename(Candidate);

        if (!FPaths::IsUnderDirectory(Candidate, AllowedRoot))
        {
            OutError = FString::Printf(TEXT("Niagara Preview Lab captures must be written under %s"), *AllowedRoot);
            return false;
        }

        OutPath = Candidate;
        return true;
    }

    void AddActorLabelsToJson(const TArray<AActor*>& Actors, TArray<TSharedPtr<FJsonValue>>& OutArray)
    {
        for (const AActor* Actor : Actors)
        {
            if (Actor)
            {
                OutArray.Add(MakeShared<FJsonValueString>(Actor->GetActorLabel()));
            }
        }
    }

    TArray<AActor*> GetNiagaraPreviewLabActors(UWorld* World)
    {
        TArray<AActor*> PreviewActors;
        if (!World)
        {
            return PreviewActors;
        }

        TArray<AActor*> AllActors;
        UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);
        for (AActor* Actor : AllActors)
        {
            if (IsNiagaraPreviewLabActor(Actor))
            {
                PreviewActors.Add(Actor);
            }
        }

        return PreviewActors;
    }

    TArray<TSharedPtr<FJsonValue>> DestroyNiagaraPreviewLabActors(UWorld* World)
    {
        TArray<TSharedPtr<FJsonValue>> DeletedLabels;
        for (AActor* Actor : GetNiagaraPreviewLabActors(World))
        {
            if (Actor)
            {
                DeletedLabels.Add(MakeShared<FJsonValueString>(Actor->GetActorLabel()));
                Actor->Destroy();
            }
        }
        return DeletedLabels;
    }

    bool GetBoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool DefaultValue)
    {
        bool Value = DefaultValue;
        if (Params.IsValid() && Params->HasField(FieldName))
        {
            Params->TryGetBoolField(FieldName, Value);
        }
        return Value;
    }

    double GetNumberParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, double DefaultValue)
    {
        double Value = DefaultValue;
        if (Params.IsValid() && Params->HasField(FieldName))
        {
            Params->TryGetNumberField(FieldName, Value);
        }
        return Value;
    }

    FString GetStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const FString& DefaultValue)
    {
        FString Value = DefaultValue;
        if (Params.IsValid() && Params->HasField(FieldName))
        {
            Params->TryGetStringField(FieldName, Value);
        }
        return Value;
    }

    FString NormalizeNiagaraSystemObjectPath(const FString& InputPath)
    {
        FString Path = InputPath.TrimStartAndEnd();
        Path.TrimQuotesInline();

        if ((Path.StartsWith(TEXT("/Game/")) || Path.StartsWith(TEXT("/Engine/")) || Path.StartsWith(TEXT("/Niagara/"))) && !Path.Contains(TEXT(".")))
        {
            const FString AssetName = FPaths::GetBaseFilename(Path);
            Path = FString::Printf(TEXT("%s.%s"), *Path, *AssetName);
        }

        return Path;
    }

    FVector GetVectorParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const FVector& DefaultValue)
    {
        if (!Params.IsValid() || !Params->HasTypedField<EJson::Array>(FieldName))
        {
            return DefaultValue;
        }

        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (!Params->TryGetArrayField(FieldName, Array) || !Array || Array->Num() < 3)
        {
            return DefaultValue;
        }

        return FVector((*Array)[0]->AsNumber(), (*Array)[1]->AsNumber(), (*Array)[2]->AsNumber());
    }

    TArray<double> GetNumberArrayParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const TArray<double>& DefaultValue, double MinValue, double MaxValue, int32 MaxCount)
    {
        if (!Params.IsValid() || !Params->HasTypedField<EJson::Array>(FieldName))
        {
            return DefaultValue;
        }

        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (!Params->TryGetArrayField(FieldName, Array) || !Array || Array->IsEmpty())
        {
            return DefaultValue;
        }

        TArray<double> Values;
        const int32 Count = FMath::Min(Array->Num(), MaxCount);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Values.Add(FMath::Clamp((*Array)[Index]->AsNumber(), MinValue, MaxValue));
        }
        return Values.IsEmpty() ? DefaultValue : Values;
    }

    TArray<int32> GetViewArrayParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const TArray<int32>& DefaultValue)
    {
        if (!Params.IsValid() || !Params->HasTypedField<EJson::Array>(FieldName))
        {
            return DefaultValue;
        }

        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (!Params->TryGetArrayField(FieldName, Array) || !Array || Array->IsEmpty())
        {
            return DefaultValue;
        }

        TArray<int32> Views;
        const int32 Count = FMath::Min(Array->Num(), 3);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Views.AddUnique(FMath::Clamp(static_cast<int32>((*Array)[Index]->AsNumber()), 1, 3));
        }
        return Views.IsEmpty() ? DefaultValue : Views;
    }

    void AddVectorField(TSharedPtr<FJsonObject> Object, const TCHAR* FieldName, const FVector& Value)
    {
        TSharedPtr<FJsonObject> VectorObject = MakeShared<FJsonObject>();
        VectorObject->SetNumberField(TEXT("x"), Value.X);
        VectorObject->SetNumberField(TEXT("y"), Value.Y);
        VectorObject->SetNumberField(TEXT("z"), Value.Z);
        Object->SetObjectField(FieldName, VectorObject);
    }

    void AddRotatorField(TSharedPtr<FJsonObject> Object, const TCHAR* FieldName, const FRotator& Value)
    {
        TSharedPtr<FJsonObject> RotatorObject = MakeShared<FJsonObject>();
        RotatorObject->SetNumberField(TEXT("pitch"), Value.Pitch);
        RotatorObject->SetNumberField(TEXT("yaw"), Value.Yaw);
        RotatorObject->SetNumberField(TEXT("roll"), Value.Roll);
        Object->SetObjectField(FieldName, RotatorObject);
    }

    FRotator GetNiagaraPreviewLabRotationForView(int32 View)
    {
        return FRotator(-8.0f, 90.0f, 0.0f);
    }

    FVector GetNiagaraPreviewLabLocationForView(int32 View)
    {
        switch (View)
        {
        case 2:
            return FVector(0.0f, -720.0f, 260.0f);
        case 3:
            return FVector(0.0f, -1150.0f, 380.0f);
        case 1:
        default:
            return FVector(0.0f, -420.0f, 180.0f);
        }
    }

    bool TryBuildNiagaraPreviewLabCameraFrame(
        UWorld* World,
        int32 View,
        FVector& OutCameraLocation,
        FRotator& OutCameraRotation,
        FVector& OutFrameCenter,
        double& OutFrameRadius,
        TArray<TSharedPtr<FJsonValue>>& OutFrameActorLabels)
    {
        const TArray<AActor*> PreviewActors = GetNiagaraPreviewLabActors(World);
        if (PreviewActors.IsEmpty())
        {
            return false;
        }

        FBox CombinedBounds(ForceInit);
        for (const AActor* Actor : PreviewActors)
        {
            if (!Actor)
            {
                continue;
            }

            OutFrameActorLabels.Add(MakeShared<FJsonValueString>(Actor->GetActorLabel()));

            FBox ActorBounds = Actor->GetComponentsBoundingBox(true);
            const double ExtentSize = ActorBounds.IsValid ? ActorBounds.GetExtent().Size() : 0.0;
            if (!ActorBounds.IsValid || ExtentSize < 1.0 || ExtentSize > 3000.0)
            {
                ActorBounds = FBox::BuildAABB(Actor->GetActorLocation(), FVector(180.0, 180.0, 180.0));
            }

            CombinedBounds += ActorBounds;
        }

        if (!CombinedBounds.IsValid)
        {
            return false;
        }

        OutFrameCenter = CombinedBounds.GetCenter();
        OutFrameRadius = FMath::Clamp(static_cast<double>(CombinedBounds.GetExtent().Size()), 120.0, 1400.0);

        const double ViewDistanceMultiplier = View == 3 ? 4.2 : (View == 2 ? 3.1 : 2.25);
        const double Distance = FMath::Clamp(OutFrameRadius * ViewDistanceMultiplier, 360.0, 3600.0);
        const double HeightOffset = FMath::Clamp(OutFrameRadius * 0.22, 20.0, 280.0);

        OutCameraLocation = FVector(OutFrameCenter.X, OutFrameCenter.Y - Distance, OutFrameCenter.Z + HeightOffset);
        OutCameraRotation = (OutFrameCenter - OutCameraLocation).Rotation();
        return true;
    }
}

FUnrealMCPEditorCommands::FUnrealMCPEditorCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    // Actor manipulation commands
    if (CommandType == TEXT("get_actors_in_level"))
    {
        return HandleGetActorsInLevel(Params);
    }
    else if (CommandType == TEXT("find_actors_by_name"))
    {
        return HandleFindActorsByName(Params);
    }
    else if (CommandType == TEXT("spawn_actor") || CommandType == TEXT("create_actor"))
    {
        if (CommandType == TEXT("create_actor"))
        {
            UE_LOG(LogTemp, Warning, TEXT("'create_actor' command is deprecated and will be removed in a future version. Please use 'spawn_actor' instead."));
        }
        return HandleSpawnActor(Params);
    }
    else if (CommandType == TEXT("delete_actor"))
    {
        return HandleDeleteActor(Params);
    }
    else if (CommandType == TEXT("set_actor_transform"))
    {
        return HandleSetActorTransform(Params);
    }
    else if (CommandType == TEXT("get_actor_properties"))
    {
        return HandleGetActorProperties(Params);
    }
    else if (CommandType == TEXT("set_actor_property"))
    {
        return HandleSetActorProperty(Params);
    }
    // Blueprint actor spawning
    else if (CommandType == TEXT("spawn_blueprint_actor"))
    {
        return HandleSpawnBlueprintActor(Params);
    }
    // Editor viewport commands
    else if (CommandType == TEXT("focus_viewport"))
    {
        return HandleFocusViewport(Params);
    }
    else if (CommandType == TEXT("take_screenshot"))
    {
        return HandleTakeScreenshot(Params);
    }
    else if (CommandType == TEXT("list_viewport_bookmarks"))
    {
        return HandleListViewportBookmarks(Params);
    }
    else if (CommandType == TEXT("capture_viewport_bookmark_screenshot"))
    {
        return HandleCaptureViewportBookmarkScreenshot(Params);
    }
    else if (CommandType == TEXT("open_editor_level"))
    {
        return HandleOpenEditorLevel(Params);
    }
    else if (CommandType == TEXT("open_niagara_preview_player"))
    {
        return HandleOpenNiagaraPreviewPlayer(Params);
    }
    else if (CommandType == TEXT("get_niagara_preview_player_state"))
    {
        return HandleGetNiagaraPreviewPlayerState(Params);
    }
    else if (CommandType == TEXT("get_niagara_preview_lab_state"))
    {
        return HandleGetNiagaraPreviewLabState(Params);
    }
    else if (CommandType == TEXT("cleanup_niagara_preview_lab"))
    {
        return HandleCleanupNiagaraPreviewLab(Params);
    }
    else if (CommandType == TEXT("capture_niagara_preview_lab_view"))
    {
        return HandleCaptureNiagaraPreviewLabView(Params);
    }
    else if (CommandType == TEXT("preview_niagara_system_in_preview_lab"))
    {
        return HandlePreviewNiagaraSystemInPreviewLab(Params);
    }
    else if (CommandType == TEXT("sample_niagara_system_in_preview_lab"))
    {
        return HandleSampleNiagaraSystemInPreviewLab(Params);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown editor command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleOpenNiagaraPreviewPlayer(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraPreviewPlayerWindow::Show();
    TSharedPtr<FJsonObject> State = FNiagaraPreviewPlayerWindow::GetStateJson();
    State->SetBoolField(TEXT("opened"), true);

    FString SystemPath;
    if (Params.IsValid() && Params->TryGetStringField(TEXT("system_path"), SystemPath) && !SystemPath.IsEmpty())
    {
        FString PreviewError;
        const bool bPreviewLoaded = FNiagaraPreviewPlayerWindow::PreviewSystemByPath(SystemPath, PreviewError);
        State = FNiagaraPreviewPlayerWindow::GetStateJson();
        State->SetBoolField(TEXT("opened"), true);
        State->SetBoolField(TEXT("preview_loaded"), bPreviewLoaded);
        if (!PreviewError.IsEmpty())
        {
            State->SetStringField(TEXT("preview_error"), PreviewError);
        }
    }

    return State;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetNiagaraPreviewPlayerState(const TSharedPtr<FJsonObject>& Params)
{
    (void)Params;
    return FNiagaraPreviewPlayerWindow::GetStateJson();
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleListViewportBookmarks(const TSharedPtr<FJsonObject>& Params)
{
    if (!GEditor || !GEditor->GetActiveViewport())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No active editor viewport is available"));
    }

    FViewport* Viewport = GEditor->GetActiveViewport();
    FEditorViewportClient* ViewportClient = static_cast<FEditorViewportClient*>(Viewport->GetClient());
    if (!ViewportClient)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Active viewport does not have an editor viewport client"));
    }

    const uint32 MaxBookmarkCount = IBookmarkTypeTools::Get().GetMaxNumberOfBookmarks(ViewportClient);
    TArray<TSharedPtr<FJsonValue>> BookmarkArray;
    TArray<TSharedPtr<FJsonValue>> ExistingIndices;
    for (uint32 Index = 0; Index < MaxBookmarkCount; ++Index)
    {
        const bool bExists = IBookmarkTypeTools::Get().CheckBookmark(Index, ViewportClient);

        TSharedPtr<FJsonObject> BookmarkObject = MakeShared<FJsonObject>();
        BookmarkObject->SetNumberField(TEXT("index"), Index);
        BookmarkObject->SetBoolField(TEXT("exists"), bExists);
        BookmarkArray.Add(MakeShared<FJsonValueObject>(BookmarkObject));

        if (bExists)
        {
            ExistingIndices.Add(MakeShared<FJsonValueNumber>(Index));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetNumberField(TEXT("max_bookmark_count"), MaxBookmarkCount);
    ResultObj->SetArrayField(TEXT("bookmarks"), BookmarkArray);
    ResultObj->SetArrayField(TEXT("existing_indices"), ExistingIndices);
    ResultObj->SetArrayField(TEXT("view_location"), EditorVectorToJsonArray(ViewportClient->GetViewLocation()));
    ResultObj->SetArrayField(TEXT("view_rotation"), EditorRotatorToJsonArray(ViewportClient->GetViewRotation()));
    ResultObj->SetBoolField(TEXT("is_perspective"), ViewportClient->IsPerspective());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleCaptureViewportBookmarkScreenshot(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<FString> DirtyPackagesBeforeCommand = GetDirtyPackageNames();

    FString FilePath;
    if (!Params->TryGetStringField(TEXT("filepath"), FilePath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'filepath' parameter"));
    }

    if (!GEditor || !GEditor->GetActiveViewport())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No active editor viewport is available"));
    }

    FViewport* Viewport = GEditor->GetActiveViewport();
    FEditorViewportClient* ViewportClient = static_cast<FEditorViewportClient*>(Viewport->GetClient());
    if (!ViewportClient)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Active viewport does not have an editor viewport client"));
    }

    int32 RedrawCount = 1;
    if (Params->HasField(TEXT("redraw_count")))
    {
        RedrawCount = Params->GetIntegerField(TEXT("redraw_count"));
    }

    bool bBookmarkRequested = false;
    bool bBookmarkExists = false;
    int32 BookmarkIndex = INDEX_NONE;
    if (Params->HasField(TEXT("bookmark_index")))
    {
        bBookmarkRequested = true;
        BookmarkIndex = Params->GetIntegerField(TEXT("bookmark_index"));
        if (BookmarkIndex < 0)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("bookmark_index must be >= 0"));
        }

        const uint32 MaxBookmarkCount = IBookmarkTypeTools::Get().GetMaxNumberOfBookmarks(ViewportClient);
        if (static_cast<uint32>(BookmarkIndex) >= MaxBookmarkCount)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
                TEXT("bookmark_index %d is outside the valid range 0-%u"),
                BookmarkIndex,
                MaxBookmarkCount > 0 ? MaxBookmarkCount - 1 : 0));
        }

        bBookmarkExists = IBookmarkTypeTools::Get().CheckBookmark(static_cast<uint32>(BookmarkIndex), ViewportClient);
        if (!bBookmarkExists)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("No bookmark exists at index %d"), BookmarkIndex));
        }

        IBookmarkTypeTools::Get().JumpToBookmark(static_cast<uint32>(BookmarkIndex), TSharedPtr<FBookmarkBaseJumpToSettings>(), ViewportClient);
        ViewportClient->Invalidate();
    }

    TSharedPtr<FJsonObject> ResultObj = CaptureActiveEditorViewportToPng(FilePath, Viewport, ViewportClient, RedrawCount);
    if (ResultObj.IsValid() && ResultObj->HasField(TEXT("success")) && ResultObj->GetBoolField(TEXT("success")))
    {
        ResultObj->SetBoolField(TEXT("bookmark_requested"), bBookmarkRequested);
        ResultObj->SetBoolField(TEXT("bookmark_exists"), bBookmarkExists);
        ResultObj->SetNumberField(TEXT("bookmark_index"), BookmarkIndex);
        ResultObj->SetStringField(TEXT("capture_mode"), bBookmarkRequested ? TEXT("bookmark") : TEXT("active_viewport"));
        AddDirtyPackageCommandDelta(ResultObj, DirtyPackagesBeforeCommand);
    }
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleOpenEditorLevel(const TSharedPtr<FJsonObject>& Params)
{
    FString RequestedLevelPath;
    if (!Params->TryGetStringField(TEXT("level_path"), RequestedLevelPath) &&
        !Params->TryGetStringField(TEXT("map_path"), RequestedLevelPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'level_path' parameter"));
    }

    bool bDryRun = true;
    Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

    bool bAllowDirtyPackages = false;
    Params->TryGetBoolField(TEXT("allow_dirty_packages"), bAllowDirtyPackages);

    bool bLoadAsTemplate = false;
    Params->TryGetBoolField(TEXT("load_as_template"), bLoadAsTemplate);

    bool bShowProgress = true;
    Params->TryGetBoolField(TEXT("show_progress"), bShowProgress);

    const FString TargetLongPackageName = NormalizeLevelPackageName(RequestedLevelPath);
    FText InvalidPackageReason;
    if (TargetLongPackageName.IsEmpty() ||
        TargetLongPackageName.Contains(TEXT("//")) ||
        !FPackageName::IsValidLongPackageName(TargetLongPackageName, true, &InvalidPackageReason))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Invalid level package path '%s': %s"),
            *RequestedLevelPath,
            *InvalidPackageReason.ToString()));
    }

    const FString TargetFilename = FPackageName::LongPackageNameToFilename(
        TargetLongPackageName,
        FPackageName::GetMapPackageExtension());
    const bool bTargetExists = FPaths::FileExists(TargetFilename);
    const FString CurrentWorldPackageName = GetCurrentEditorWorldPackageName();
    const bool bAlreadyOpen = CurrentWorldPackageName == TargetLongPackageName;
    const TArray<FString> DirtyPackagesBeforeCommand = GetDirtyPackageNames();

    TArray<FString> BlockedReasons;
    if (!bTargetExists)
    {
        BlockedReasons.Add(TEXT("target_map_file_missing"));
    }
    if (!bAlreadyOpen && DirtyPackagesBeforeCommand.Num() > 0 && !bAllowDirtyPackages)
    {
        BlockedReasons.Add(TEXT("dirty_packages_present"));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("requested_level_path"), RequestedLevelPath);
    ResultObj->SetStringField(TEXT("target_long_package_name"), TargetLongPackageName);
    ResultObj->SetStringField(TEXT("target_filename"), TargetFilename);
    ResultObj->SetBoolField(TEXT("target_exists"), bTargetExists);
    ResultObj->SetStringField(TEXT("current_world_package_name"), CurrentWorldPackageName);
    ResultObj->SetBoolField(TEXT("already_open"), bAlreadyOpen);
    ResultObj->SetBoolField(TEXT("dry_run"), bDryRun);
    ResultObj->SetBoolField(TEXT("allow_dirty_packages"), bAllowDirtyPackages);
    ResultObj->SetBoolField(TEXT("can_load"), BlockedReasons.Num() == 0 || bAlreadyOpen);
    ResultObj->SetArrayField(TEXT("blocked_reasons"), StringArrayToJsonArray(BlockedReasons));
    ResultObj->SetNumberField(TEXT("dirty_package_count_before"), DirtyPackagesBeforeCommand.Num());
    ResultObj->SetArrayField(TEXT("dirty_packages_before"), StringArrayToJsonArray(DirtyPackagesBeforeCommand));

    if (bDryRun || bAlreadyOpen || BlockedReasons.Num() > 0)
    {
        ResultObj->SetBoolField(TEXT("load_attempted"), false);
        ResultObj->SetBoolField(TEXT("loaded"), bAlreadyOpen);
        ResultObj->SetStringField(
            TEXT("message"),
            bDryRun
                ? TEXT("Dry run only; no editor level transition was attempted")
                : (bAlreadyOpen ? TEXT("Target level is already open") : TEXT("Editor level transition blocked")));
        return ResultObj;
    }

    const bool bLoaded = FEditorFileUtils::LoadMap(TargetFilename, bLoadAsTemplate, bShowProgress);
    ResultObj->SetBoolField(TEXT("load_attempted"), true);
    ResultObj->SetBoolField(TEXT("loaded"), bLoaded);
    ResultObj->SetStringField(TEXT("current_world_package_name_after"), GetCurrentEditorWorldPackageName());
    AddDirtyPackageCommandDelta(ResultObj, DirtyPackagesBeforeCommand);

    if (!bLoaded)
    {
        ResultObj->SetBoolField(TEXT("success"), false);
        ResultObj->SetStringField(TEXT("message"), TEXT("FEditorFileUtils::LoadMap returned false"));
    }

    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetActorsInLevel(const TSharedPtr<FJsonObject>& Params)
{
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);

    TArray<TSharedPtr<FJsonValue>> ActorArray;
    for (AActor* Actor : AllActors)
    {
        if (Actor)
        {
            ActorArray.Add(FUnrealMCPCommonUtils::ActorToJson(Actor));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("actors"), ActorArray);

    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleFindActorsByName(const TSharedPtr<FJsonObject>& Params)
{
    FString Pattern;
    if (!Params->TryGetStringField(TEXT("pattern"), Pattern))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'pattern' parameter"));
    }

    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);

    TArray<TSharedPtr<FJsonValue>> MatchingActors;
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName().Contains(Pattern))
        {
            MatchingActors.Add(FUnrealMCPCommonUtils::ActorToJson(Actor));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("actors"), MatchingActors);

    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSpawnActor(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString ActorType;
    if (!Params->TryGetStringField(TEXT("type"), ActorType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'type' parameter"));
    }

    // Get actor name (required parameter)
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Get optional transform parameters
    FVector Location(0.0f, 0.0f, 0.0f);
    FRotator Rotation(0.0f, 0.0f, 0.0f);
    FVector Scale(1.0f, 1.0f, 1.0f);

    if (Params->HasField(TEXT("location")))
    {
        Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        Rotation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"));
    }
    if (Params->HasField(TEXT("scale")))
    {
        Scale = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale"));
    }

    // Create the actor based on type
    AActor* NewActor = nullptr;
    UWorld* World = GEditor->GetEditorWorldContext().World();

    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    // Check if an actor with this name already exists
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor with name '%s' already exists"), *ActorName));
        }
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = *ActorName;

    if (ActorType == TEXT("StaticMeshActor"))
    {
        NewActor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType == TEXT("PointLight"))
    {
        NewActor = World->SpawnActor<APointLight>(APointLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType == TEXT("SpotLight"))
    {
        NewActor = World->SpawnActor<ASpotLight>(ASpotLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType == TEXT("DirectionalLight"))
    {
        NewActor = World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType == TEXT("CameraActor"))
    {
        NewActor = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), Location, Rotation, SpawnParams);
    }
    else
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown actor type: %s"), *ActorType));
    }

    if (NewActor)
    {
        // Set scale (since SpawnActor only takes location and rotation)
        FTransform Transform = NewActor->GetTransform();
        Transform.SetScale3D(Scale);
        NewActor->SetActorTransform(Transform);

        // Return the created actor's details
        return FUnrealMCPCommonUtils::ActorToJsonObject(NewActor, true);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create actor"));
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleDeleteActor(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);

    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            // Store actor info before deletion for the response
            TSharedPtr<FJsonObject> ActorInfo = FUnrealMCPCommonUtils::ActorToJsonObject(Actor);

            // Delete the actor
            Actor->Destroy();

            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetObjectField(TEXT("deleted_actor"), ActorInfo);
            return ResultObj;
        }
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSetActorTransform(const TSharedPtr<FJsonObject>& Params)
{
    // Get actor name
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Find the actor
    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);

    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            TargetActor = Actor;
            break;
        }
    }

    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    // Get transform parameters
    FTransform NewTransform = TargetActor->GetTransform();

    if (Params->HasField(TEXT("location")))
    {
        NewTransform.SetLocation(FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location")));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        NewTransform.SetRotation(FQuat(FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"))));
    }
    if (Params->HasField(TEXT("scale")))
    {
        NewTransform.SetScale3D(FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale")));
    }

    // Set the new transform
    TargetActor->SetActorTransform(NewTransform);

    // Return updated actor info
    return FUnrealMCPCommonUtils::ActorToJsonObject(TargetActor, true);
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetActorProperties(const TSharedPtr<FJsonObject>& Params)
{
    // Get actor name
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Find the actor
    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);

    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            TargetActor = Actor;
            break;
        }
    }

    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    // Always return detailed properties for this command
    return FUnrealMCPCommonUtils::ActorToJsonObject(TargetActor, true);
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSetActorProperty(const TSharedPtr<FJsonObject>& Params)
{
    // Get actor name
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Find the actor
    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);

    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            TargetActor = Actor;
            break;
        }
    }

    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    // Get property name
    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_name' parameter"));
    }

    // Get property value
    if (!Params->HasField(TEXT("property_value")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_value' parameter"));
    }

    TSharedPtr<FJsonValue> PropertyValue = Params->Values.FindRef(TEXT("property_value"));

    // Set the property using our utility function
    FString ErrorMessage;
    if (FUnrealMCPCommonUtils::SetObjectProperty(TargetActor, PropertyName, PropertyValue, ErrorMessage))
    {
        // Property set successfully
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("actor"), ActorName);
        ResultObj->SetStringField(TEXT("property"), PropertyName);
        ResultObj->SetBoolField(TEXT("success"), true);

        // Also include the full actor details
        ResultObj->SetObjectField(TEXT("actor_details"), FUnrealMCPCommonUtils::ActorToJsonObject(TargetActor, true));
        return ResultObj;
    }
    else
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSpawnBlueprintActor(const TSharedPtr<FJsonObject>& Params)
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
    if (BlueprintName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Blueprint name is empty"));
    }

    FString Root      = TEXT("/Game/Blueprints/");
    FString AssetPath = Root + BlueprintName;

    if (!FPackageName::DoesPackageExist(AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint '%s' not found – it must reside under /Game/Blueprints"), *BlueprintName));
    }

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get transform parameters
    FVector Location(0.0f, 0.0f, 0.0f);
    FRotator Rotation(0.0f, 0.0f, 0.0f);
    FVector Scale(1.0f, 1.0f, 1.0f);

    if (Params->HasField(TEXT("location")))
    {
        Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        Rotation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"));
    }
    if (Params->HasField(TEXT("scale")))
    {
        Scale = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale"));
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
    SpawnTransform.SetScale3D(Scale);

    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = *ActorName;

    AActor* NewActor = World->SpawnActor<AActor>(Blueprint->GeneratedClass, SpawnTransform, SpawnParams);
    if (NewActor)
    {
        return FUnrealMCPCommonUtils::ActorToJsonObject(NewActor, true);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to spawn blueprint actor"));
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleFocusViewport(const TSharedPtr<FJsonObject>& Params)
{
    // Get target actor name if provided
    FString TargetActorName;
    bool HasTargetActor = Params->TryGetStringField(TEXT("target"), TargetActorName);

    // Get location if provided
    FVector Location(0.0f, 0.0f, 0.0f);
    bool HasLocation = false;
    if (Params->HasField(TEXT("location")))
    {
        Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
        HasLocation = true;
    }

    // Get distance
    float Distance = 1000.0f;
    if (Params->HasField(TEXT("distance")))
    {
        Distance = Params->GetNumberField(TEXT("distance"));
    }

    // Get orientation if provided
    FRotator Orientation(0.0f, 0.0f, 0.0f);
    bool HasOrientation = false;
    if (Params->HasField(TEXT("orientation")))
    {
        Orientation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("orientation"));
        HasOrientation = true;
    }

    // Get the active viewport
    FLevelEditorViewportClient* ViewportClient = GetActiveLevelEditorViewportClient();
    if (!ViewportClient)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get active viewport"));
    }

    // If we have a target actor, focus on it
    if (HasTargetActor)
    {
        // Find the actor
        AActor* TargetActor = nullptr;
        TArray<AActor*> AllActors;
        UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);

        for (AActor* Actor : AllActors)
        {
            if (Actor && Actor->GetName() == TargetActorName)
            {
                TargetActor = Actor;
                break;
            }
        }

        if (!TargetActor)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *TargetActorName));
        }

        // Focus on the actor
        ViewportClient->SetViewLocation(TargetActor->GetActorLocation() - FVector(Distance, 0.0f, 0.0f));
    }
    // Otherwise use the provided location
    else if (HasLocation)
    {
        ViewportClient->SetViewLocation(Location - FVector(Distance, 0.0f, 0.0f));
    }
    else
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Either 'target' or 'location' must be provided"));
    }

    // Set orientation if provided
    if (HasOrientation)
    {
        ViewportClient->SetViewRotation(Orientation);
    }

    // Force viewport to redraw
    ViewportClient->Invalidate();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleTakeScreenshot(const TSharedPtr<FJsonObject>& Params)
{
    // Get file path parameter
    FString FilePath;
    if (!Params->TryGetStringField(TEXT("filepath"), FilePath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'filepath' parameter"));
    }

    // Ensure the file path has a proper extension
    if (!FilePath.EndsWith(TEXT(".png")))
    {
        FilePath += TEXT(".png");
    }

    // Get the active viewport
    if (GEditor && GEditor->GetActiveViewport())
    {
        FViewport* Viewport = GEditor->GetActiveViewport();
        TArray<FColor> Bitmap;
        FIntRect ViewportRect(0, 0, Viewport->GetSizeXY().X, Viewport->GetSizeXY().Y);

        if (Viewport->ReadPixels(Bitmap, FReadSurfaceDataFlags(), ViewportRect))
        {
            TArray<uint8> CompressedBitmap;
            FImageUtils::CompressImageArray(Viewport->GetSizeXY().X, Viewport->GetSizeXY().Y, Bitmap, CompressedBitmap);

            if (FFileHelper::SaveArrayToFile(CompressedBitmap, *FilePath))
            {
                TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
                ResultObj->SetStringField(TEXT("filepath"), FilePath);
                return ResultObj;
            }
        }
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to take screenshot"));
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetNiagaraPreviewLabState(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorldForNiagaraPreviewLab();
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    const FString CurrentMapPackage = World->GetOutermost() ? World->GetOutermost()->GetName() : FString();
    const bool bIsPreviewLabMap = CurrentMapPackage == NiagaraPreviewLabMapPackage;
    const bool bMapDirty = World->GetOutermost() ? World->GetOutermost()->IsDirty() : false;

    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);

    TArray<AActor*> PreviewActors;
    for (AActor* Actor : AllActors)
    {
        if (IsNiagaraPreviewLabActor(Actor))
        {
            PreviewActors.Add(Actor);
        }
    }

    TArray<TSharedPtr<FJsonValue>> PreviewActorLabels;
    AddActorLabelsToJson(PreviewActors, PreviewActorLabels);

    const bool bRequiresEditorRestart = bIsPreviewLabMap && bMapDirty;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("preview_system"), TEXT("Niagara Preview Lab"));
    ResultObj->SetStringField(TEXT("expected_map"), NiagaraPreviewLabMapObject);
    ResultObj->SetStringField(TEXT("current_map_package"), CurrentMapPackage);
    ResultObj->SetBoolField(TEXT("is_preview_lab_map"), bIsPreviewLabMap);
    ResultObj->SetBoolField(TEXT("map_dirty"), bMapDirty);
    ResultObj->SetNumberField(TEXT("preview_actor_count"), PreviewActors.Num());
    ResultObj->SetArrayField(TEXT("preview_actor_labels"), PreviewActorLabels);
    ResultObj->SetBoolField(TEXT("requires_editor_restart"), bRequiresEditorRestart);
    ResultObj->SetStringField(
        TEXT("safety_rule"),
        TEXT("Do not reload Niagara_TestMap from the same Python/editor session after preview actors or world references existed. Clean actors by prefix and restart the editor for a full reset."));

    if (bRequiresEditorRestart)
    {
        ResultObj->SetStringField(TEXT("recommended_action"), TEXT("Do not reload or save the map. Restart Unreal Editor and open Niagara_TestMap fresh if a clean reset is required."));
    }
    else
    {
        ResultObj->SetStringField(TEXT("recommended_action"), TEXT("Reuse the loaded map. Cleanup preview actors by prefix after capture."));
    }

    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleCleanupNiagaraPreviewLab(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorldForNiagaraPreviewLab();
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    TArray<TSharedPtr<FJsonValue>> DeletedLabels = DestroyNiagaraPreviewLabActors(World);

    const FString CurrentMapPackage = World->GetOutermost() ? World->GetOutermost()->GetName() : FString();
    const bool bMapDirty = World->GetOutermost() ? World->GetOutermost()->IsDirty() : false;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("preview_system"), TEXT("Niagara Preview Lab"));
    ResultObj->SetStringField(TEXT("current_map_package"), CurrentMapPackage);
    ResultObj->SetNumberField(TEXT("deleted_actor_count"), DeletedLabels.Num());
    ResultObj->SetArrayField(TEXT("deleted_actor_labels"), DeletedLabels);
    ResultObj->SetBoolField(TEXT("map_dirty_after_cleanup"), bMapDirty);
    ResultObj->SetBoolField(TEXT("map_was_reloaded"), false);
    ResultObj->SetBoolField(TEXT("map_was_saved"), false);
    ResultObj->SetStringField(TEXT("safety_note"), TEXT("Cleanup never reloads or saves the Niagara Preview Lab map. Restart Unreal Editor for a full reset if the map remains dirty."));
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleCaptureNiagaraPreviewLabView(const TSharedPtr<FJsonObject>& Params)
{
    FString FilePathParam;
    if (!Params->TryGetStringField(TEXT("filepath"), FilePathParam))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'filepath' parameter"));
    }

    FString FilePath;
    FString PathError;
    if (!ResolveNiagaraPreviewLabFilePath(FilePathParam, FilePath, PathError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(PathError);
    }

    int32 View = 1;
    if (Params->HasField(TEXT("view")))
    {
        View = Params->GetIntegerField(TEXT("view"));
    }
    View = FMath::Clamp(View, 1, 3);

    UWorld* World = GetEditorWorldForNiagaraPreviewLab();
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    const FString CurrentMapPackage = World->GetOutermost() ? World->GetOutermost()->GetName() : FString();
    if (CurrentMapPackage != NiagaraPreviewLabMapPackage)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Niagara Preview Lab capture requires %s to be loaded. Current map: %s. Open the map fresh; this command will not reload maps."),
            NiagaraPreviewLabMapObject,
            *CurrentMapPackage));
    }

    if (!GEditor || !GEditor->GetActiveViewport())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get active viewport"));
    }

    FViewport* Viewport = GEditor->GetActiveViewport();
    FLevelEditorViewportClient* ViewportClient = GetActiveLevelEditorViewportClient();
    if (!ViewportClient)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Active viewport is not a level editor viewport"));
    }

    IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);

    FVector CameraLocation = GetNiagaraPreviewLabLocationForView(View);
    FRotator CameraRotation = GetNiagaraPreviewLabRotationForView(View);
    FVector FrameCenter = FVector::ZeroVector;
    double FrameRadius = 0.0;
    TArray<TSharedPtr<FJsonValue>> FrameActorLabels;
    const bool bUsedAutoFrame = TryBuildNiagaraPreviewLabCameraFrame(
        World,
        View,
        CameraLocation,
        CameraRotation,
        FrameCenter,
        FrameRadius,
        FrameActorLabels);

    const bool bPreviousGameView = ViewportClient->IsInGameView();
    const bool bPreviousSelectionOutline = ViewportClient->EngineShowFlags.SelectionOutline;
    const bool bPreviousModeWidgets = ViewportClient->EngineShowFlags.ModeWidgets;
    const bool bPreviousParticles = ViewportClient->EngineShowFlags.Particles;

    TArray64<uint8> CompressedBitmap;
    FIntPoint ViewportSize = Viewport->GetSizeXY();
    bool bReadPixels = false;
    bool bSaved = false;

    ViewportClient->SetViewLocation(CameraLocation);
    ViewportClient->SetViewRotation(CameraRotation);
    ViewportClient->SetGameView(true);
    ViewportClient->EngineShowFlags.SetSelectionOutline(false);
    ViewportClient->EngineShowFlags.SetModeWidgets(false);
    ViewportClient->EngineShowFlags.SetParticles(true);
    ViewportClient->Invalidate();
    Viewport->Invalidate();
    Viewport->Draw(false);

    TArray<FColor> Bitmap;
    FIntRect ViewportRect(0, 0, ViewportSize.X, ViewportSize.Y);
    bReadPixels = Viewport->ReadPixels(Bitmap, FReadSurfaceDataFlags(), ViewportRect);
    if (bReadPixels)
    {
        FImageUtils::PNGCompressImageArray(
            ViewportSize.X,
            ViewportSize.Y,
            TArrayView64<const FColor>(Bitmap.GetData(), Bitmap.Num()),
            CompressedBitmap);
        bSaved = FFileHelper::SaveArrayToFile(CompressedBitmap, *FilePath);
    }

    ViewportClient->EngineShowFlags.SetParticles(bPreviousParticles);
    ViewportClient->EngineShowFlags.SetModeWidgets(bPreviousModeWidgets);
    ViewportClient->EngineShowFlags.SetSelectionOutline(bPreviousSelectionOutline);
    ViewportClient->SetGameView(bPreviousGameView);
    ViewportClient->Invalidate();

    if (!bReadPixels || !bSaved)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to capture Niagara Preview Lab viewport"));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("preview_system"), TEXT("Niagara Preview Lab"));
    ResultObj->SetStringField(TEXT("filepath"), FilePath);
    ResultObj->SetNumberField(TEXT("view"), View);
    ResultObj->SetNumberField(TEXT("width"), ViewportSize.X);
    ResultObj->SetNumberField(TEXT("height"), ViewportSize.Y);
    ResultObj->SetBoolField(TEXT("clean_game_view"), true);
    ResultObj->SetStringField(TEXT("camera_mode"), bUsedAutoFrame ? TEXT("auto_preview_actor_frame") : TEXT("fixed_view_fallback"));
    AddVectorField(ResultObj, TEXT("camera_location"), CameraLocation);
    AddRotatorField(ResultObj, TEXT("camera_rotation"), CameraRotation);
    if (bUsedAutoFrame)
    {
        AddVectorField(ResultObj, TEXT("frame_center"), FrameCenter);
        ResultObj->SetNumberField(TEXT("frame_radius"), FrameRadius);
        ResultObj->SetArrayField(TEXT("frame_actor_labels"), FrameActorLabels);
    }
    ResultObj->SetBoolField(TEXT("map_was_reloaded"), false);
    ResultObj->SetBoolField(TEXT("map_was_saved"), false);
    ResultObj->SetStringField(TEXT("safety_note"), TEXT("Capture reuses the already loaded Niagara Preview Lab map, frames temporary preview actors when present, and never reloads or saves it."));
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandlePreviewNiagaraSystemInPreviewLab(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPathParam;
    if (!Params->TryGetStringField(TEXT("system_path"), SystemPathParam))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }

    FString FilePathParam;
    if (!Params->TryGetStringField(TEXT("filepath"), FilePathParam))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'filepath' parameter"));
    }

    FString FilePath;
    FString PathError;
    if (!ResolveNiagaraPreviewLabFilePath(FilePathParam, FilePath, PathError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(PathError);
    }

    UWorld* World = GetEditorWorldForNiagaraPreviewLab();
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    const FString CurrentMapPackage = World->GetOutermost() ? World->GetOutermost()->GetName() : FString();
    if (CurrentMapPackage != NiagaraPreviewLabMapPackage)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Niagara Preview Lab preview requires %s to be loaded. Current map: %s. Open the map fresh; this command will not reload maps."),
            NiagaraPreviewLabMapObject,
            *CurrentMapPackage));
    }

    const FString SystemObjectPath = NormalizeNiagaraSystemObjectPath(SystemPathParam);
    UNiagaraSystem* NiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *SystemObjectPath);
    if (!NiagaraSystem)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemObjectPath));
    }

    const bool bCleanupBefore = GetBoolParam(Params, TEXT("cleanup_before"), true);
    const bool bCleanupAfter = GetBoolParam(Params, TEXT("cleanup_after"), true);
    const int32 View = FMath::Clamp(static_cast<int32>(GetNumberParam(Params, TEXT("view"), 1.0)), 1, 3);
    const double WarmupTime = FMath::Clamp(GetNumberParam(Params, TEXT("warmup_time"), 0.35), 0.0, 5.0);
    const double WarmupTickDelta = FMath::Clamp(GetNumberParam(Params, TEXT("warmup_tick_delta"), 1.0 / 30.0), 1.0 / 240.0, 0.25);
    const FVector SpawnLocation = GetVectorParam(Params, TEXT("location"), FVector(0.0, 0.0, 120.0));
    const FVector SpawnScale = GetVectorParam(Params, TEXT("scale"), FVector(1.0, 1.0, 1.0));
    const FString LabelSuffix = GetStringParam(Params, TEXT("label"), FPaths::GetBaseFilename(SystemObjectPath));
    const FString ActorLabel = FString::Printf(TEXT("%s%s"), NiagaraPreviewLabPrefix, *LabelSuffix);

    TArray<TSharedPtr<FJsonValue>> CleanupBeforeLabels;
    if (bCleanupBefore)
    {
        CleanupBeforeLabels = DestroyNiagaraPreviewLabActors(World);
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.ObjectFlags = RF_Transient;
    ANiagaraActor* PreviewActor = World->SpawnActor<ANiagaraActor>(ANiagaraActor::StaticClass(), SpawnLocation, FRotator::ZeroRotator, SpawnParams);
    if (!PreviewActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to spawn Niagara Preview Lab actor"));
    }

    PreviewActor->SetActorLabel(ActorLabel, false);
    PreviewActor->SetActorScale3D(SpawnScale);
    PreviewActor->SetFolderPath(TEXT("MCP/NiagaraPreviewLab"));

    UNiagaraComponent* NiagaraComponent = PreviewActor->GetNiagaraComponent();
    if (!NiagaraComponent)
    {
        PreviewActor->Destroy();
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Spawned Niagara actor has no Niagara component"));
    }

    NiagaraComponent->SetAsset(NiagaraSystem);
    NiagaraComponent->SetAutoActivate(true);
    NiagaraComponent->Activate(true);
    if (WarmupTime > 0.0)
    {
        NiagaraComponent->AdvanceSimulationByTime(static_cast<float>(WarmupTime), static_cast<float>(WarmupTickDelta));
    }

    TSharedPtr<FJsonObject> CaptureParams = MakeShared<FJsonObject>();
    CaptureParams->SetStringField(TEXT("filepath"), FilePath);
    CaptureParams->SetNumberField(TEXT("view"), View);
    TSharedPtr<FJsonObject> CaptureResult = HandleCaptureNiagaraPreviewLabView(CaptureParams);

    TArray<TSharedPtr<FJsonValue>> CleanupAfterLabels;
    if (bCleanupAfter)
    {
        CleanupAfterLabels = DestroyNiagaraPreviewLabActors(World);
    }

    if (!CaptureResult.IsValid() || !CaptureResult->GetBoolField(TEXT("success")))
    {
        const FString ErrorMessage = CaptureResult.IsValid()
            ? CaptureResult->GetStringField(TEXT("error"))
            : FString(TEXT("Capture command returned no result"));
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    const bool bMapDirty = World->GetOutermost() ? World->GetOutermost()->IsDirty() : false;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("preview_system"), TEXT("Niagara Preview Lab"));
    ResultObj->SetStringField(TEXT("system_path"), SystemObjectPath);
    ResultObj->SetStringField(TEXT("actor_label"), ActorLabel);
    ResultObj->SetStringField(TEXT("filepath"), FilePath);
    ResultObj->SetNumberField(TEXT("view"), View);
    ResultObj->SetNumberField(TEXT("warmup_time"), WarmupTime);
    ResultObj->SetNumberField(TEXT("warmup_tick_delta"), WarmupTickDelta);
    ResultObj->SetArrayField(TEXT("cleanup_before_labels"), CleanupBeforeLabels);
    ResultObj->SetArrayField(TEXT("cleanup_after_labels"), CleanupAfterLabels);
    ResultObj->SetBoolField(TEXT("cleanup_after"), bCleanupAfter);
    ResultObj->SetBoolField(TEXT("map_dirty_after_preview"), bMapDirty);
    AddVectorField(ResultObj, TEXT("spawn_location"), SpawnLocation);
    AddVectorField(ResultObj, TEXT("spawn_scale"), SpawnScale);
    ResultObj->SetObjectField(TEXT("capture"), CaptureResult);
    ResultObj->SetBoolField(TEXT("map_was_reloaded"), false);
    ResultObj->SetBoolField(TEXT("map_was_saved"), false);
    ResultObj->SetStringField(TEXT("safety_note"), TEXT("One-call preview loads a read-only Niagara system, spawns a transient Preview Lab actor, captures it with auto framing, and never reloads or saves the map."));
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSampleNiagaraSystemInPreviewLab(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPathParam;
    if (!Params->TryGetStringField(TEXT("system_path"), SystemPathParam))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }

    UWorld* World = GetEditorWorldForNiagaraPreviewLab();
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    const FString CurrentMapPackage = World->GetOutermost() ? World->GetOutermost()->GetName() : FString();
    if (CurrentMapPackage != NiagaraPreviewLabMapPackage)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Niagara Preview Lab sampling requires %s to be loaded. Current map: %s. Open the map fresh; this command will not reload maps."),
            NiagaraPreviewLabMapObject,
            *CurrentMapPackage));
    }

    const FString SystemObjectPath = NormalizeNiagaraSystemObjectPath(SystemPathParam);
    if (!LoadObject<UNiagaraSystem>(nullptr, *SystemObjectPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemObjectPath));
    }

    const FString Label = FPaths::MakeValidFileName(GetStringParam(Params, TEXT("label"), FPaths::GetBaseFilename(SystemObjectPath)));
    const FString OutputDir = GetStringParam(Params, TEXT("output_dir"), FString::Printf(TEXT("Samples/%s"), *Label));
    const TArray<double> WarmupTimes = GetNumberArrayParam(Params, TEXT("warmup_times"), TArray<double>{0.1, 0.35, 0.7}, 0.0, 5.0, 12);
    const TArray<int32> Views = GetViewArrayParam(Params, TEXT("views"), TArray<int32>{1});
    const double WarmupTickDelta = FMath::Clamp(GetNumberParam(Params, TEXT("warmup_tick_delta"), 1.0 / 30.0), 1.0 / 240.0, 0.25);
    const FVector SpawnLocation = GetVectorParam(Params, TEXT("location"), FVector(0.0, 0.0, 120.0));
    const FVector SpawnScale = GetVectorParam(Params, TEXT("scale"), FVector(1.0, 1.0, 1.0));
    const bool bCleanupBefore = GetBoolParam(Params, TEXT("cleanup_before"), true);
    const bool bCleanupAfterEach = GetBoolParam(Params, TEXT("cleanup_after_each"), true);
    const bool bCleanupAfterAll = GetBoolParam(Params, TEXT("cleanup_after_all"), true);

    const double StartSeconds = FPlatformTime::Seconds();
    TArray<TSharedPtr<FJsonValue>> CleanupBeforeLabels;
    if (bCleanupBefore)
    {
        CleanupBeforeLabels = DestroyNiagaraPreviewLabActors(World);
    }

    int32 SuccessCount = 0;
    int32 FailureCount = 0;
    int32 SampleIndex = 0;
    TArray<TSharedPtr<FJsonValue>> Samples;

    for (double WarmupTime : WarmupTimes)
    {
        for (int32 View : Views)
        {
            const int32 WarmupMs = FMath::RoundToInt(WarmupTime * 1000.0);
            const FString FilePath = FString::Printf(
                TEXT("%s/%s_t%04d_v%d.png"),
                *OutputDir,
                *Label,
                WarmupMs,
                View);

            TSharedPtr<FJsonObject> PreviewParams = MakeShared<FJsonObject>();
            PreviewParams->SetStringField(TEXT("system_path"), SystemObjectPath);
            PreviewParams->SetStringField(TEXT("filepath"), FilePath);
            PreviewParams->SetNumberField(TEXT("view"), View);
            PreviewParams->SetStringField(TEXT("label"), FString::Printf(TEXT("%s_S%02d_T%04d_V%d"), *Label, SampleIndex, WarmupMs, View));
            PreviewParams->SetNumberField(TEXT("warmup_time"), WarmupTime);
            PreviewParams->SetNumberField(TEXT("warmup_tick_delta"), WarmupTickDelta);
            PreviewParams->SetBoolField(TEXT("cleanup_before"), false);
            PreviewParams->SetBoolField(TEXT("cleanup_after"), bCleanupAfterEach);

            TArray<TSharedPtr<FJsonValue>> LocationArray;
            LocationArray.Add(MakeShared<FJsonValueNumber>(SpawnLocation.X));
            LocationArray.Add(MakeShared<FJsonValueNumber>(SpawnLocation.Y));
            LocationArray.Add(MakeShared<FJsonValueNumber>(SpawnLocation.Z));
            PreviewParams->SetArrayField(TEXT("location"), LocationArray);

            TArray<TSharedPtr<FJsonValue>> ScaleArray;
            ScaleArray.Add(MakeShared<FJsonValueNumber>(SpawnScale.X));
            ScaleArray.Add(MakeShared<FJsonValueNumber>(SpawnScale.Y));
            ScaleArray.Add(MakeShared<FJsonValueNumber>(SpawnScale.Z));
            PreviewParams->SetArrayField(TEXT("scale"), ScaleArray);

            TSharedPtr<FJsonObject> PreviewResult = HandlePreviewNiagaraSystemInPreviewLab(PreviewParams);
            TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
            Sample->SetNumberField(TEXT("sample_index"), SampleIndex);
            Sample->SetNumberField(TEXT("warmup_time"), WarmupTime);
            Sample->SetNumberField(TEXT("view"), View);
            Sample->SetStringField(TEXT("requested_filepath"), FilePath);

            const bool bSampleSuccess = PreviewResult.IsValid() && PreviewResult->GetBoolField(TEXT("success"));
            Sample->SetBoolField(TEXT("success"), bSampleSuccess);
            if (bSampleSuccess)
            {
                ++SuccessCount;
                Sample->SetObjectField(TEXT("preview"), PreviewResult);
            }
            else
            {
                ++FailureCount;
                Sample->SetStringField(TEXT("error"), PreviewResult.IsValid() ? PreviewResult->GetStringField(TEXT("error")) : TEXT("No preview result"));
            }

            Samples.Add(MakeShared<FJsonValueObject>(Sample));
            ++SampleIndex;
        }
    }

    TArray<TSharedPtr<FJsonValue>> CleanupAfterAllLabels;
    if (bCleanupAfterAll)
    {
        CleanupAfterAllLabels = DestroyNiagaraPreviewLabActors(World);
    }

    const bool bMapDirty = World->GetOutermost() ? World->GetOutermost()->IsDirty() : false;
    const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), SuccessCount > 0);
    ResultObj->SetStringField(TEXT("preview_system"), TEXT("Niagara Preview Lab"));
    ResultObj->SetStringField(TEXT("system_path"), SystemObjectPath);
    ResultObj->SetStringField(TEXT("label"), Label);
    ResultObj->SetStringField(TEXT("output_dir"), OutputDir);
    ResultObj->SetNumberField(TEXT("elapsed_seconds"), ElapsedSeconds);
    ResultObj->SetNumberField(TEXT("sample_count"), Samples.Num());
    ResultObj->SetNumberField(TEXT("success_count"), SuccessCount);
    ResultObj->SetNumberField(TEXT("failure_count"), FailureCount);
    ResultObj->SetNumberField(TEXT("warmup_tick_delta"), WarmupTickDelta);
    ResultObj->SetArrayField(TEXT("cleanup_before_labels"), CleanupBeforeLabels);
    ResultObj->SetArrayField(TEXT("cleanup_after_all_labels"), CleanupAfterAllLabels);
    ResultObj->SetArrayField(TEXT("samples"), Samples);
    ResultObj->SetBoolField(TEXT("map_dirty_after_sampling"), bMapDirty);
    ResultObj->SetBoolField(TEXT("map_was_reloaded"), false);
    ResultObj->SetBoolField(TEXT("map_was_saved"), false);
    AddVectorField(ResultObj, TEXT("spawn_location"), SpawnLocation);
    AddVectorField(ResultObj, TEXT("spawn_scale"), SpawnScale);
    ResultObj->SetStringField(TEXT("safety_note"), TEXT("Sampling reuses the loaded Preview Lab map, captures multiple warmup/view candidates in one MCP round trip, and leaves no preview actors when cleanup is enabled."));
    return ResultObj;
}
