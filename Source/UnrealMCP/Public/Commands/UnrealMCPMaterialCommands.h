#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for Material and Material Function graph MCP commands.
 */
class UNREALMCP_API FUnrealMCPMaterialCommands
{
public:
    FUnrealMCPMaterialCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleResolveMaterialGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListMaterialNodes(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAnalyzeMaterialGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddMaterialNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddCustomMaterialNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetMaterialNodeProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleConnectMaterialNodes(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleConnectMaterialProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDeleteMaterialNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleLayoutMaterialNodes(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCompileAndSaveMaterial(const TSharedPtr<FJsonObject>& Params);
};
