#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for PCG graph MCP commands.
 */
class UNREALMCP_API FUnrealMCPPCGCommands
{
public:
    FUnrealMCPPCGCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleResolvePCGGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListPCGGraphNodes(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddPCGNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleConnectPCGNodes(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetPCGNodeSetting(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetPCGAttributeSelector(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCompileOrNotifyPCGGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSavePCGGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetSplineComponentPoints(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRefreshPCGComponents(const TSharedPtr<FJsonObject>& Params);
};
