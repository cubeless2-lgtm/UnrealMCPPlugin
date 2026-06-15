#pragma once

#include "CoreMinimal.h"
#include "Json.h"

class UNiagaraRendererProperties;
class UNiagaraSystem;
class UMaterialInterface;

/**
 * Handler class for Niagara MCP commands.
 */
class UNREALMCP_API FUnrealMCPNiagaraCommands
{
public:
    FUnrealMCPNiagaraCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleAnalyzeNiagaraSystem(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleInspectNiagaraRenderers(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNiagaraRendererMaterial(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleInspectNiagaraUserParameters(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNiagaraUserParameter(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleInspectNiagaraStack(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleInspectNiagaraGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleInspectNiagaraCompileStatus(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleInspectNiagaraSimulationStages(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleInspectNiagaraScratchPadInterface(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDuplicateOrAttachEmitterFromSource(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCreateOrDuplicateScratchPadModule(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddScratchPadModuleToStack(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleInspectNiagaraModuleInputs(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleInspectNiagaraDataInterfaceOverrides(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCreateNiagaraModuleInputOverride(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNiagaraRenderTarget2DModuleInput(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNiagaraModuleInputsBatch(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNiagaraModuleInputValue(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> BuildRendererJson(
        const UNiagaraSystem* System,
        const FString& EmitterName,
        int32 EmitterIndex,
        const UNiagaraRendererProperties* Renderer,
        int32 RendererIndex) const;
    TSharedPtr<FJsonObject> BuildUserParameterJson(const UNiagaraSystem* System, const struct FNiagaraVariable& Parameter) const;
};
