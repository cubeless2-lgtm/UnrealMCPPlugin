#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for Editor-related MCP commands
 * Handles viewport control, actor manipulation, and level management
 */
class UNREALMCP_API FUnrealMCPEditorCommands
{
public:
    FUnrealMCPEditorCommands();

    // Handle editor commands
    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    // Actor manipulation commands
    TSharedPtr<FJsonObject> HandleGetActorsInLevel(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleFindActorsByName(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSpawnActor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDeleteActor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetActorTransform(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetActorProperties(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetActorProperty(const TSharedPtr<FJsonObject>& Params);

    // Blueprint actor spawning
    TSharedPtr<FJsonObject> HandleSpawnBlueprintActor(const TSharedPtr<FJsonObject>& Params);

    // Editor viewport commands
    TSharedPtr<FJsonObject> HandleFocusViewport(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleTakeScreenshot(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListViewportBookmarks(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCaptureViewportBookmarkScreenshot(const TSharedPtr<FJsonObject>& Params);

    // Niagara Preview Lab commands
    TSharedPtr<FJsonObject> HandleOpenNiagaraPreviewPlayer(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetNiagaraPreviewPlayerState(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetNiagaraPreviewLabState(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCleanupNiagaraPreviewLab(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCaptureNiagaraPreviewLabView(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandlePreviewNiagaraSystemInPreviewLab(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSampleNiagaraSystemInPreviewLab(const TSharedPtr<FJsonObject>& Params);

    // Level management commands
    TSharedPtr<FJsonObject> HandleOpenEditorLevel(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSafeNewPreviewMap(const TSharedPtr<FJsonObject>& Params);
};
