#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for Project-wide MCP commands
 */
class UNREALMCP_API FUnrealMCPProjectCommands
{
public:
    FUnrealMCPProjectCommands();

    // Handle project commands
    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    // Specific project command handlers
    TSharedPtr<FJsonObject> HandleCreateInputMapping(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleExecutePython(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRecreateContentFolderMCP(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandlePostprocessContentFolderMCP(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRepairWorldActorInstancesMCP(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRunContentValidationPipelineMCP(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAuditContentRootMCP(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAnalyzeBlueprintWidgetFallbacksMCP(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCreateVolumeTextureFrom2DSheetMCP(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCreateVolumeTextureFromRawRGBA16SheetMCP(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetMaterialTextureSampleSamplerTypeMCP(const TSharedPtr<FJsonObject>& Params);
}; 
