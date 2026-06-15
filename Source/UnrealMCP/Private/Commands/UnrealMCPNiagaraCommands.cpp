#include "Commands/UnrealMCPNiagaraCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"

#include "AssetCompilingManager.h"
#include "EditorAssetLibrary.h"
#include "Engine/TextureRenderTarget.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/PlatformProcess.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "NiagaraDataInterface.h"
#include "NiagaraDataInterfaceRenderTarget2D.h"
#include "NiagaraDecalRendererProperties.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterBinding.h"
#include "NiagaraParameterMapHistory.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraScratchPadContainer.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSimulationStageCompileData.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "NiagaraVolumeRendererProperties.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

namespace
{
constexpr const TCHAR* NiagaraTempGenerationRoot = TEXT("/Game/_MCP_Temp/NiagaraGenerated/");

FString NormalizeNiagaraObjectPathForLoad(const FString& ObjectPath)
{
    FString NormalizedPath = FPackageName::ExportTextPathToObjectPath(ObjectPath).TrimStartAndEnd();
    NormalizedPath.TrimQuotesInline();

    if (NormalizedPath.StartsWith(TEXT("/")) && !NormalizedPath.StartsWith(TEXT("/Script/")) && !NormalizedPath.Contains(TEXT(".")))
    {
        const FString AssetName = FPackageName::GetShortName(NormalizedPath);
        NormalizedPath = FString::Printf(TEXT("%s.%s"), *NormalizedPath, *AssetName);
    }

    return NormalizedPath;
}

FString PackagePathFromObjectPath(const FString& ObjectPath)
{
    FString PackagePath = FPackageName::ExportTextPathToObjectPath(ObjectPath).TrimStartAndEnd();
    PackagePath.TrimQuotesInline();
    int32 DotIndex = INDEX_NONE;
    if (PackagePath.FindChar(TEXT('.'), DotIndex))
    {
        PackagePath.LeftInline(DotIndex);
    }
    return PackagePath;
}

bool IsTempGeneratedNiagaraPath(const FString& ObjectPath)
{
    return PackagePathFromObjectPath(ObjectPath).StartsWith(NiagaraTempGenerationRoot);
}

FString NiagaraObjectPathOrEmpty(const UObject* Object)
{
    return Object ? Object->GetPathName() : FString();
}

TSharedPtr<FJsonValue> NumberArrayToJson(std::initializer_list<double> Values)
{
    TArray<TSharedPtr<FJsonValue>> JsonValues;
    for (double Value : Values)
    {
        JsonValues.Add(MakeShared<FJsonValueNumber>(Value));
    }
    return MakeShared<FJsonValueArray>(JsonValues);
}

TArray<TSharedPtr<FJsonValue>> IntArrayToJsonValues(std::initializer_list<int32> Values)
{
    TArray<TSharedPtr<FJsonValue>> JsonValues;
    for (int32 Value : Values)
    {
        JsonValues.Add(MakeShared<FJsonValueNumber>(Value));
    }
    return JsonValues;
}

TSharedPtr<FJsonValue> NiagaraVariableDataToJsonValue(const FNiagaraVariable& Variable)
{
    if (!Variable.IsDataAllocated())
    {
        return MakeShared<FJsonValueNull>();
    }

    const FNiagaraTypeDefinition& TypeDef = Variable.GetType();
    if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
    {
        return MakeShared<FJsonValueNumber>(Variable.GetValue<float>());
    }
    if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
    {
        return MakeShared<FJsonValueNumber>(Variable.GetValue<int32>());
    }
    if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
    {
        const FNiagaraBool Value = Variable.GetValue<FNiagaraBool>();
        return MakeShared<FJsonValueBoolean>(Value.GetValue());
    }
    if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
    {
        const FLinearColor Value = Variable.GetValue<FLinearColor>();
        return NumberArrayToJson({ Value.R, Value.G, Value.B, Value.A });
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec2Def())
    {
        const FVector2f Value = Variable.GetValue<FVector2f>();
        return NumberArrayToJson({ Value.X, Value.Y });
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec3Def())
    {
        const FVector3f Value = Variable.GetValue<FVector3f>();
        return NumberArrayToJson({ Value.X, Value.Y, Value.Z });
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec4Def())
    {
        const FVector4f Value = Variable.GetValue<FVector4f>();
        return NumberArrayToJson({ Value.X, Value.Y, Value.Z, Value.W });
    }
    if (TypeDef == FNiagaraTypeDefinition::GetPositionDef())
    {
        const FVector3f Value = Variable.GetValue<FVector3f>();
        return NumberArrayToJson({ Value.X, Value.Y, Value.Z });
    }

    return MakeShared<FJsonValueString>(Variable.ToString());
}

bool JsonArrayToDoubles(const TSharedPtr<FJsonValue>& Value, TArray<double>& OutValues)
{
    if (!Value.IsValid() || Value->Type != EJson::Array)
    {
        return false;
    }

    for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
    {
        double NumberValue = 0.0;
        if (!Item.IsValid() || !Item->TryGetNumber(NumberValue))
        {
            return false;
        }
        OutValues.Add(NumberValue);
    }
    return true;
}

FString NiagaraTypeName(const FNiagaraTypeDefinition& TypeDef)
{
    return TypeDef.GetNameText().ToString();
}

FString NiagaraScriptUsageName(ENiagaraScriptUsage Usage)
{
    if (const UEnum* Enum = StaticEnum<ENiagaraScriptUsage>())
    {
        return Enum->GetNameStringByValue(static_cast<int64>(Usage));
    }
    return FString::FromInt(static_cast<int32>(Usage));
}

template<typename TEnum>
FString NiagaraEnumValueName(TEnum Value)
{
    if (const UEnum* Enum = StaticEnum<TEnum>())
    {
        return Enum->GetNameStringByValue(static_cast<int64>(Value));
    }
    return FString::FromInt(static_cast<int32>(Value));
}

FString NiagaraEnumValueName(ENiagaraIterationSource Value)
{
    switch (Value)
    {
    case ENiagaraIterationSource::Particles:
        return TEXT("Particles");
    case ENiagaraIterationSource::DataInterface:
        return TEXT("DataInterface");
    case ENiagaraIterationSource::DirectSet:
        return TEXT("DirectSet");
    default:
        return FString::FromInt(static_cast<int32>(Value));
    }
}

FString NiagaraEnumValueName(ENiagaraGpuDispatchType Value)
{
    switch (Value)
    {
    case ENiagaraGpuDispatchType::OneD:
        return TEXT("OneD");
    case ENiagaraGpuDispatchType::TwoD:
        return TEXT("TwoD");
    case ENiagaraGpuDispatchType::ThreeD:
        return TEXT("ThreeD");
    case ENiagaraGpuDispatchType::Custom:
        return TEXT("Custom");
    default:
        return FString::FromInt(static_cast<int32>(Value));
    }
}

FString NiagaraEnumValueName(ENiagaraDirectDispatchElementType Value)
{
    switch (Value)
    {
    case ENiagaraDirectDispatchElementType::NumThreads:
        return TEXT("NumThreads");
    case ENiagaraDirectDispatchElementType::NumThreadsNoClipping:
        return TEXT("NumThreadsNoClipping");
    case ENiagaraDirectDispatchElementType::NumGroups:
        return TEXT("NumGroups");
    default:
        return FString::FromInt(static_cast<int32>(Value));
    }
}

FString NiagaraEnumValueName(ENiagaraSimStageExecuteBehavior Value)
{
    switch (Value)
    {
    case ENiagaraSimStageExecuteBehavior::Always:
        return TEXT("Always");
    case ENiagaraSimStageExecuteBehavior::OnSimulationReset:
        return TEXT("OnSimulationReset");
    case ENiagaraSimStageExecuteBehavior::NotOnSimulationReset:
        return TEXT("NotOnSimulationReset");
    default:
        return FString::FromInt(static_cast<int32>(Value));
    }
}

FString TextureRenderTargetFormatName(ETextureRenderTargetFormat Format)
{
    switch (Format)
    {
    case RTF_R8:
        return TEXT("RTF_R8");
    case RTF_RG8:
        return TEXT("RTF_RG8");
    case RTF_RGBA8:
        return TEXT("RTF_RGBA8");
    case RTF_RGBA8_SRGB:
        return TEXT("RTF_RGBA8_SRGB");
    case RTF_R16f:
        return TEXT("RTF_R16f");
    case RTF_RG16f:
        return TEXT("RTF_RG16f");
    case RTF_RGBA16f:
        return TEXT("RTF_RGBA16f");
    case RTF_R32f:
        return TEXT("RTF_R32f");
    case RTF_RG32f:
        return TEXT("RTF_RG32f");
    case RTF_RGBA32f:
        return TEXT("RTF_RGBA32f");
    case RTF_RGB10A2:
        return TEXT("RTF_RGB10A2");
    default:
        return FString::FromInt(static_cast<int32>(Format));
    }
}

bool TryParseNiagaraScriptUsage(const FString& Value, ENiagaraScriptUsage& OutUsage)
{
    FString Normalized = Value.TrimStartAndEnd().ToLower();
    Normalized.ReplaceInline(TEXT("_"), TEXT(""));
    Normalized.ReplaceInline(TEXT("-"), TEXT(""));
    Normalized.ReplaceInline(TEXT(" "), TEXT(""));

    if (Normalized == TEXT("systemspawn") || Normalized == TEXT("systemspawnscript"))
    {
        OutUsage = ENiagaraScriptUsage::SystemSpawnScript;
        return true;
    }
    if (Normalized == TEXT("systemupdate") || Normalized == TEXT("systemupdatescript"))
    {
        OutUsage = ENiagaraScriptUsage::SystemUpdateScript;
        return true;
    }
    if (Normalized == TEXT("emitterspawn") || Normalized == TEXT("emitterspawnscript"))
    {
        OutUsage = ENiagaraScriptUsage::EmitterSpawnScript;
        return true;
    }
    if (Normalized == TEXT("emitterupdate") || Normalized == TEXT("emitterupdatescript"))
    {
        OutUsage = ENiagaraScriptUsage::EmitterUpdateScript;
        return true;
    }
    if (Normalized == TEXT("particlespawn") || Normalized == TEXT("particlespawnscript"))
    {
        OutUsage = ENiagaraScriptUsage::ParticleSpawnScript;
        return true;
    }
    if (Normalized == TEXT("particlespawninterpolated") || Normalized == TEXT("particlespawnscriptinterpolated"))
    {
        OutUsage = ENiagaraScriptUsage::ParticleSpawnScriptInterpolated;
        return true;
    }
    if (Normalized == TEXT("particleupdate") || Normalized == TEXT("particleupdatescript"))
    {
        OutUsage = ENiagaraScriptUsage::ParticleUpdateScript;
        return true;
    }
    if (Normalized == TEXT("particleevent") || Normalized == TEXT("particleeventscript"))
    {
        OutUsage = ENiagaraScriptUsage::ParticleEventScript;
        return true;
    }
    if (Normalized == TEXT("particlesimulationstage") || Normalized == TEXT("particlesimulationstagescript") || Normalized == TEXT("simulationstage"))
    {
        OutUsage = ENiagaraScriptUsage::ParticleSimulationStageScript;
        return true;
    }
    return false;
}

bool IsEmitterStackUsage(ENiagaraScriptUsage Usage)
{
    return Usage == ENiagaraScriptUsage::EmitterSpawnScript ||
        Usage == ENiagaraScriptUsage::EmitterUpdateScript ||
        Usage == ENiagaraScriptUsage::ParticleSpawnScript ||
        Usage == ENiagaraScriptUsage::ParticleSpawnScriptInterpolated ||
        Usage == ENiagaraScriptUsage::ParticleUpdateScript ||
        Usage == ENiagaraScriptUsage::ParticleEventScript ||
        Usage == ENiagaraScriptUsage::ParticleSimulationStageScript;
}

bool IsScratchPadScriptCompatibleWithStackUsage(const UNiagaraScript* Script, ENiagaraScriptUsage TargetUsage)
{
    if (!Script || Script->GetUsage() != ENiagaraScriptUsage::Module)
    {
        return false;
    }

    if (const FVersionedNiagaraScriptData* ScriptData = Script->GetLatestScriptData())
    {
        for (ENiagaraScriptUsage SupportedUsage : ScriptData->GetSupportedUsageContexts())
        {
            if (UNiagaraScript::IsEquivalentUsage(SupportedUsage, TargetUsage))
            {
                return true;
            }
        }
    }
    return false;
}

FString NiagaraCompileStatusName(ENiagaraScriptCompileStatus Status)
{
    if (const UEnum* Enum = StaticEnum<ENiagaraScriptCompileStatus>())
    {
        return Enum->GetNameStringByValue(static_cast<int64>(Status));
    }
    return FString::FromInt(static_cast<int32>(Status));
}

bool IsNiagaraCompileErrorStatus(ENiagaraScriptCompileStatus Status)
{
    return Status == ENiagaraScriptCompileStatus::NCS_Error;
}

bool IsNiagaraCompileWarningStatus(ENiagaraScriptCompileStatus Status)
{
    return Status == ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings ||
        Status == ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings;
}

TSharedPtr<FJsonObject> NiagaraVariableToJsonObject(const FNiagaraVariableBase& Variable)
{
    TSharedPtr<FJsonObject> VariableObject = MakeShared<FJsonObject>();
    VariableObject->SetStringField(TEXT("name"), Variable.GetName().ToString());
    VariableObject->SetStringField(TEXT("type"), NiagaraTypeName(Variable.GetType()));
    VariableObject->SetBoolField(TEXT("type_is_valid"), Variable.GetType().IsValid());
    VariableObject->SetBoolField(TEXT("type_is_data_interface"), Variable.GetType().IsDataInterface());
    VariableObject->SetBoolField(TEXT("type_is_uobject"), Variable.GetType().IsUObject());
    if (UClass* TypeClass = Variable.GetType().GetClass())
    {
        VariableObject->SetStringField(TEXT("type_class"), TypeClass->GetName());
        VariableObject->SetStringField(TEXT("type_class_path"), TypeClass->GetPathName());
    }
    return VariableObject;
}

TSharedPtr<FJsonObject> BuildNiagaraScriptCompileStatusJson(
    const UNiagaraScript* Script,
    const FString& OwnerKind,
    const FString& OwnerName,
    int32 ScriptIndex)
{
    TSharedPtr<FJsonObject> ScriptObject = MakeShared<FJsonObject>();
    ScriptObject->SetNumberField(TEXT("script_index"), ScriptIndex);
    ScriptObject->SetStringField(TEXT("owner_kind"), OwnerKind);
    ScriptObject->SetStringField(TEXT("owner_name"), OwnerName);
    if (!Script)
    {
        ScriptObject->SetStringField(TEXT("script_name"), FString());
        ScriptObject->SetStringField(TEXT("script_path"), FString());
        ScriptObject->SetStringField(TEXT("usage"), FString());
        ScriptObject->SetStringField(TEXT("compile_status"), TEXT("missing"));
        ScriptObject->SetBoolField(TEXT("has_error"), true);
        ScriptObject->SetBoolField(TEXT("has_warning"), false);
        return ScriptObject;
    }

    const ENiagaraScriptCompileStatus Status = Script->GetLastCompileStatus();
    ScriptObject->SetStringField(TEXT("script_name"), Script->GetName());
    ScriptObject->SetStringField(TEXT("script_path"), Script->GetPathName());
    ScriptObject->SetStringField(TEXT("usage"), NiagaraScriptUsageName(Script->GetUsage()));
    ScriptObject->SetStringField(TEXT("usage_id"), Script->GetUsageId().ToString(EGuidFormats::DigitsWithHyphens));
    ScriptObject->SetStringField(TEXT("compile_status"), NiagaraCompileStatusName(Status));
    ScriptObject->SetBoolField(TEXT("has_error"), IsNiagaraCompileErrorStatus(Status));
    ScriptObject->SetBoolField(TEXT("has_warning"), IsNiagaraCompileWarningStatus(Status));
    ScriptObject->SetBoolField(TEXT("is_ready_cpu"), Script->IsReadyToRun(ENiagaraSimTarget::CPUSim));
    ScriptObject->SetBoolField(TEXT("is_ready_gpu"), Script->IsReadyToRun(ENiagaraSimTarget::GPUComputeSim));
    return ScriptObject;
}

TSharedPtr<FJsonObject> NiagaraVariableWithDataToJsonObject(const FNiagaraVariable& Variable)
{
    TSharedPtr<FJsonObject> VariableObject = NiagaraVariableToJsonObject(Variable);
    VariableObject->SetBoolField(TEXT("has_data"), Variable.IsDataAllocated());
    VariableObject->SetNumberField(TEXT("data_size_bytes"), Variable.GetAllocatedSizeInBytes());
    VariableObject->SetField(TEXT("value"), NiagaraVariableDataToJsonValue(Variable));
    return VariableObject;
}

TSharedPtr<FJsonObject> NiagaraAttributeBindingToJsonObject(const FNiagaraVariableAttributeBinding& Binding)
{
    TSharedPtr<FJsonObject> BindingObject = MakeShared<FJsonObject>();
    BindingObject->SetBoolField(TEXT("is_valid"), Binding.IsValid());
    BindingObject->SetBoolField(TEXT("is_particle_binding"), Binding.IsParticleBinding());
    BindingObject->SetBoolField(TEXT("binding_exists_on_source"), Binding.DoesBindingExistOnSource());
    BindingObject->SetBoolField(TEXT("can_bind_to_host_parameter_map"), Binding.CanBindToHostParameterMap());
    BindingObject->SetStringField(TEXT("binding_source_mode"), NiagaraEnumValueName(Binding.GetBindingSourceMode()));
    BindingObject->SetObjectField(TEXT("param_map_bindable_variable"), NiagaraVariableToJsonObject(Binding.GetParamMapBindableVariable()));
    BindingObject->SetObjectField(TEXT("data_set_bindable_variable"), NiagaraVariableToJsonObject(Binding.GetDataSetBindableVariable()));
#if WITH_EDITORONLY_DATA
    BindingObject->SetStringField(TEXT("display_name"), Binding.GetName().ToString());
    BindingObject->SetStringField(TEXT("default_value_string"), Binding.GetDefaultValueString());
#endif
    return BindingObject;
}

TSharedPtr<FJsonObject> NiagaraDataInterfaceBindingToJsonObject(const FNiagaraVariableDataInterfaceBinding& Binding)
{
    TSharedPtr<FJsonObject> BindingObject = MakeShared<FJsonObject>();
    BindingObject->SetObjectField(TEXT("bound_variable"), NiagaraVariableWithDataToJsonObject(Binding.BoundVariable));
    BindingObject->SetBoolField(TEXT("is_valid"), Binding.BoundVariable.GetType().IsDataInterface());
    return BindingObject;
}

TSharedPtr<FJsonObject> NiagaraParameterBindingToJsonObject(FNiagaraParameterBinding& Binding)
{
    TSharedPtr<FJsonObject> BindingObject = MakeShared<FJsonObject>();
    BindingObject->SetObjectField(TEXT("resolved_parameter"), NiagaraVariableToJsonObject(Binding.ResolvedParameter));
#if WITH_EDITORONLY_DATA
    BindingObject->SetObjectField(TEXT("aliased_parameter"), NiagaraVariableToJsonObject(Binding.AliasedParameter));
    BindingObject->SetObjectField(TEXT("default_aliased_parameter"), NiagaraVariableWithDataToJsonObject(Binding.GetDefaultAliasedParameter()));
    BindingObject->SetObjectField(TEXT("default_resolved_parameter"), NiagaraVariableWithDataToJsonObject(Binding.GetDefaultResolvedParameter()));
    BindingObject->SetBoolField(TEXT("is_set_to_default"), Binding.IsSetToDefault());
    BindingObject->SetStringField(TEXT("display_string"), Binding.ToString());
    BindingObject->SetBoolField(TEXT("allow_user_parameters"), Binding.AllowUserParameters());
    BindingObject->SetBoolField(TEXT("allow_system_parameters"), Binding.AllowSystemParameters());
    BindingObject->SetBoolField(TEXT("allow_emitter_parameters"), Binding.AllowEmitterParameters());
    BindingObject->SetBoolField(TEXT("allow_particle_parameters"), Binding.AllowParticleParameters());
    BindingObject->SetBoolField(TEXT("allow_static_variables"), Binding.AllowStaticVariables());
#endif
    return BindingObject;
}

TSharedPtr<FJsonObject> NiagaraParameterBindingWithValueToJsonObject(FNiagaraParameterBindingWithValue& Binding)
{
    TSharedPtr<FJsonObject> BindingObject = NiagaraParameterBindingToJsonObject(Binding);
    const TConstArrayView<uint8> DefaultValueBytes = Binding.GetDefaultValueArray();
    BindingObject->SetNumberField(TEXT("default_value_byte_count"), DefaultValueBytes.Num());

    if (DefaultValueBytes.Num() > 0)
    {
        FNiagaraVariable DefaultValueVariable(Binding.ResolvedParameter);
#if WITH_EDITORONLY_DATA
        const FNiagaraVariable& DefaultAliasedParameter = Binding.GetDefaultAliasedParameter();
        if (DefaultAliasedParameter.GetType().IsValid())
        {
            DefaultValueVariable = DefaultAliasedParameter;
        }
#endif
        if (DefaultValueVariable.GetType().IsValid() &&
            DefaultValueVariable.GetType().GetSize() == DefaultValueBytes.Num())
        {
            DefaultValueVariable.SetData(DefaultValueBytes.GetData());
            BindingObject->SetField(TEXT("default_value"), NiagaraVariableDataToJsonValue(DefaultValueVariable));
            BindingObject->SetStringField(TEXT("default_value_decode_status"), TEXT("decoded"));
        }
        else
        {
            BindingObject->SetStringField(TEXT("default_value_decode_status"), TEXT("type_size_mismatch"));
        }
    }
    else
    {
        BindingObject->SetStringField(TEXT("default_value_decode_status"), TEXT("no_default_value_bytes"));
    }

    return BindingObject;
}

TSharedPtr<FJsonObject> NiagaraSimStageCompileDataToJsonObject(const FNiagaraSimulationStageCompilationData& CompileData)
{
    TSharedPtr<FJsonObject> CompileObject = MakeShared<FJsonObject>();
    CompileObject->SetStringField(TEXT("stage_guid"), CompileData.StageGuid.ToString(EGuidFormats::DigitsWithHyphens));
    CompileObject->SetStringField(TEXT("stage_name"), CompileData.StageName.ToString());
    CompileObject->SetStringField(TEXT("enabled_binding"), CompileData.EnabledBinding.ToString());
    CompileObject->SetArrayField(TEXT("element_count"), IntArrayToJsonValues({
        CompileData.ElementCount.X,
        CompileData.ElementCount.Y,
        CompileData.ElementCount.Z
    }));
    CompileObject->SetStringField(TEXT("element_count_x_binding"), CompileData.ElementCountXBinding.ToString());
    CompileObject->SetStringField(TEXT("element_count_y_binding"), CompileData.ElementCountYBinding.ToString());
    CompileObject->SetStringField(TEXT("element_count_z_binding"), CompileData.ElementCountZBinding.ToString());
    CompileObject->SetNumberField(TEXT("num_iterations"), CompileData.NumIterations);
    CompileObject->SetStringField(TEXT("num_iterations_binding"), CompileData.NumIterationsBinding.ToString());
    CompileObject->SetStringField(TEXT("iteration_source_type"), NiagaraEnumValueName(CompileData.IterationSourceType));
    CompileObject->SetStringField(TEXT("iteration_data_interface"), CompileData.IterationDataInterface.ToString());
    CompileObject->SetStringField(TEXT("iteration_direct_binding"), CompileData.IterationDirectBinding.ToString());
    CompileObject->SetStringField(TEXT("execute_behavior"), NiagaraEnumValueName(CompileData.ExecuteBehavior));
    CompileObject->SetBoolField(TEXT("partial_particle_update"), CompileData.PartialParticleUpdate);
    CompileObject->SetBoolField(TEXT("particle_iteration_state_enabled"), CompileData.bParticleIterationStateEnabled);
    CompileObject->SetStringField(TEXT("particle_iteration_state_binding"), CompileData.ParticleIterationStateBinding.ToString());
    CompileObject->SetArrayField(TEXT("particle_iteration_state_range"), IntArrayToJsonValues({
        CompileData.ParticleIterationStateRange.X,
        CompileData.ParticleIterationStateRange.Y
    }));
    CompileObject->SetBoolField(TEXT("gpu_dispatch_force_linear"), CompileData.bGpuDispatchForceLinear);
    CompileObject->SetStringField(TEXT("direct_dispatch_type"), NiagaraEnumValueName(CompileData.DirectDispatchType));
    CompileObject->SetStringField(TEXT("direct_dispatch_element_type"), NiagaraEnumValueName(CompileData.DirectDispatchElementType));
    CompileObject->SetBoolField(TEXT("override_gpu_dispatch_num_threads"), CompileData.bOverrideGpuDispatchNumThreads);
    CompileObject->SetArrayField(TEXT("override_gpu_dispatch_num_threads_value"), IntArrayToJsonValues({
        CompileData.OverrideGpuDispatchNumThreads.X,
        CompileData.OverrideGpuDispatchNumThreads.Y,
        CompileData.OverrideGpuDispatchNumThreads.Z
    }));
    CompileObject->SetObjectField(TEXT("override_gpu_dispatch_num_threads_x_binding"), NiagaraVariableToJsonObject(CompileData.OverrideGpuDispatchNumThreadsXBinding));
    CompileObject->SetObjectField(TEXT("override_gpu_dispatch_num_threads_y_binding"), NiagaraVariableToJsonObject(CompileData.OverrideGpuDispatchNumThreadsYBinding));
    CompileObject->SetObjectField(TEXT("override_gpu_dispatch_num_threads_z_binding"), NiagaraVariableToJsonObject(CompileData.OverrideGpuDispatchNumThreadsZBinding));
    return CompileObject;
}

TArray<TSharedPtr<FJsonValue>> NiagaraVariablesToJson(const TArray<FNiagaraVariable>& Variables)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FNiagaraVariable& Variable : Variables)
    {
        Values.Add(MakeShared<FJsonValueObject>(NiagaraVariableToJsonObject(Variable)));
    }
    return Values;
}

TArray<TSharedPtr<FJsonValue>> NiagaraVariableBasesToJson(const TArray<FNiagaraVariableBase>& Variables)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FNiagaraVariableBase& Variable : Variables)
    {
        Values.Add(MakeShared<FJsonValueObject>(NiagaraVariableToJsonObject(Variable)));
    }
    return Values;
}

TArray<TSharedPtr<FJsonValue>> NiagaraScriptUsagesToJson(const TArray<ENiagaraScriptUsage>& Usages)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (ENiagaraScriptUsage Usage : Usages)
    {
        Values.Add(MakeShared<FJsonValueString>(NiagaraScriptUsageName(Usage)));
    }
    return Values;
}

TSharedPtr<FJsonObject> GraphPinToJsonObject(const UEdGraphPin* Pin)
{
    TSharedPtr<FJsonObject> PinObject = MakeShared<FJsonObject>();
    if (!Pin)
    {
        return PinObject;
    }

    PinObject->SetStringField(TEXT("name"), Pin->PinName.ToString());
    PinObject->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
    PinObject->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
    PinObject->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
    PinObject->SetStringField(TEXT("default_value"), Pin->DefaultValue);
    PinObject->SetStringField(TEXT("default_object"), NiagaraObjectPathOrEmpty(Pin->DefaultObject));
    PinObject->SetNumberField(TEXT("linked_to_count"), Pin->LinkedTo.Num());
    return PinObject;
}

TSharedPtr<FJsonObject> LinkedPinToJsonObject(const UEdGraphPin* Pin)
{
    TSharedPtr<FJsonObject> PinObject = MakeShared<FJsonObject>();
    if (!Pin)
    {
        return PinObject;
    }

    const UEdGraphNode* OwningNode = Pin->GetOwningNode();
    PinObject->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
    PinObject->SetStringField(TEXT("node_name"), OwningNode ? OwningNode->GetName() : FString());
    PinObject->SetStringField(TEXT("node_class"), OwningNode ? OwningNode->GetClass()->GetName() : FString());
    PinObject->SetStringField(TEXT("default_value"), Pin->DefaultValue);
    PinObject->SetStringField(TEXT("default_object"), NiagaraObjectPathOrEmpty(Pin->DefaultObject));
    return PinObject;
}

TArray<TSharedPtr<FJsonValue>> LinkedPinsToJson(const UEdGraphPin* Pin, bool bIncludeLinkedSources)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    if (!Pin || !bIncludeLinkedSources)
    {
        return Values;
    }

    for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
    {
        Values.Add(MakeShared<FJsonValueObject>(LinkedPinToJsonObject(LinkedPin)));
    }
    return Values;
}

TArray<TSharedPtr<FJsonValue>> GraphPinsToJson(const TArray<UEdGraphPin*>& Pins)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const UEdGraphPin* Pin : Pins)
    {
        Values.Add(MakeShared<FJsonValueObject>(GraphPinToJsonObject(Pin)));
    }
    return Values;
}

TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values);
TArray<FString> BuildNiagaraControlHints(const FString& SearchText);

TSharedPtr<FJsonObject> DetailedPinRefToJsonObject(const UEdGraphPin* Pin)
{
    TSharedPtr<FJsonObject> PinObject = MakeShared<FJsonObject>();
    if (!Pin)
    {
        return PinObject;
    }

    const UEdGraphNode* OwningNode = Pin->GetOwningNode();
    PinObject->SetStringField(TEXT("pin_id"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
    PinObject->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
    PinObject->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
    PinObject->SetStringField(TEXT("node_name"), OwningNode ? OwningNode->GetName() : FString());
    PinObject->SetStringField(TEXT("node_guid"), OwningNode ? OwningNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : FString());
    PinObject->SetStringField(TEXT("node_class"), OwningNode ? OwningNode->GetClass()->GetName() : FString());
    return PinObject;
}

TSharedPtr<FJsonObject> DetailedPinToJsonObject(const UEdGraphPin* Pin, bool bIncludeLinkedPins)
{
    TSharedPtr<FJsonObject> PinObject = GraphPinToJsonObject(Pin);
    if (!Pin)
    {
        return PinObject;
    }

    PinObject->SetStringField(TEXT("pin_id"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
    PinObject->SetStringField(TEXT("persistent_guid"), Pin->PersistentGuid.ToString(EGuidFormats::DigitsWithHyphens));
    PinObject->SetStringField(TEXT("friendly_name"), Pin->PinFriendlyName.ToString());
    PinObject->SetStringField(TEXT("tooltip"), Pin->PinToolTip);
    PinObject->SetBoolField(TEXT("hidden"), Pin->bHidden);
    PinObject->SetBoolField(TEXT("not_connectable"), Pin->bNotConnectable);
    PinObject->SetBoolField(TEXT("advanced_view"), Pin->bAdvancedView);

    if (bIncludeLinkedPins)
    {
        TArray<TSharedPtr<FJsonValue>> LinkedValues;
        for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
        {
            LinkedValues.Add(MakeShared<FJsonValueObject>(DetailedPinRefToJsonObject(LinkedPin)));
        }
        PinObject->SetArrayField(TEXT("linked_pins"), LinkedValues);
    }

    return PinObject;
}

TArray<TSharedPtr<FJsonValue>> DetailedPinsToJson(const TArray<UEdGraphPin*>& Pins, bool bIncludeLinkedPins)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const UEdGraphPin* Pin : Pins)
    {
        Values.Add(MakeShared<FJsonValueObject>(DetailedPinToJsonObject(Pin, bIncludeLinkedPins)));
    }
    return Values;
}

TSharedPtr<FJsonObject> DetailedGraphNodeToJsonObject(
    const UEdGraphNode* Node,
    int32 NodeIndex,
    bool bIncludePins,
    bool bIncludeLinkedPins)
{
    TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
    if (!Node)
    {
        return NodeObject;
    }

    NodeObject->SetNumberField(TEXT("node_index"), NodeIndex);
    NodeObject->SetStringField(TEXT("node_name"), Node->GetName());
    NodeObject->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    NodeObject->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
    NodeObject->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
    NodeObject->SetStringField(TEXT("tooltip"), Node->GetTooltipText().ToString());
    NodeObject->SetNumberField(TEXT("x"), Node->NodePosX);
    NodeObject->SetNumberField(TEXT("y"), Node->NodePosY);
    NodeObject->SetNumberField(TEXT("width"), Node->NodeWidth);
    NodeObject->SetNumberField(TEXT("height"), Node->NodeHeight);
    NodeObject->SetNumberField(TEXT("pin_count"), Node->Pins.Num());

    if (const UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node))
    {
        FString FunctionName = FunctionCall->GetFunctionName();
        if (FunctionName.IsEmpty())
        {
            FunctionName = FunctionCall->Signature.GetNameString();
        }

        const UNiagaraScript* FunctionScript = FunctionCall->FunctionScript;
        const UObject* ScriptOuter = FunctionScript ? FunctionScript->GetOuter() : nullptr;
        NodeObject->SetStringField(TEXT("niagara_node_kind"), TEXT("function_call"));
        NodeObject->SetStringField(TEXT("function_name"), FunctionName);
        NodeObject->SetStringField(TEXT("function_script"), NiagaraObjectPathOrEmpty(FunctionScript));
        NodeObject->SetStringField(TEXT("function_script_asset_object_path"), FunctionCall->FunctionScriptAssetObjectPath.ToString());
        NodeObject->SetStringField(TEXT("called_usage"), NiagaraScriptUsageName(FunctionCall->GetCalledUsage()));
        NodeObject->SetBoolField(TEXT("is_scratch_pad"), ScriptOuter && ScriptOuter->IsA<UNiagaraScratchPadContainer>());
        NodeObject->SetStringField(TEXT("signature_name"), FunctionCall->Signature.GetNameString());
        NodeObject->SetArrayField(TEXT("signature_inputs"), NiagaraVariablesToJson(FunctionCall->Signature.Inputs));
        NodeObject->SetArrayField(TEXT("signature_outputs"), NiagaraVariableBasesToJson(FunctionCall->Signature.Outputs));
        NodeObject->SetArrayField(TEXT("control_hints"), StringsToJson(BuildNiagaraControlHints(FunctionName)));
    }
    else if (const UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node))
    {
        NodeObject->SetStringField(TEXT("niagara_node_kind"), TEXT("input"));
        NodeObject->SetObjectField(TEXT("variable"), NiagaraVariableToJsonObject(InputNode->Input));
        NodeObject->SetNumberField(TEXT("call_sort_priority"), InputNode->CallSortPriority);
        NodeObject->SetBoolField(TEXT("is_exposed"), InputNode->IsExposed());
        NodeObject->SetBoolField(TEXT("is_required"), InputNode->IsRequired());
        NodeObject->SetBoolField(TEXT("is_hidden"), InputNode->IsHidden());
        NodeObject->SetBoolField(TEXT("can_auto_bind"), InputNode->CanAutoBind());
        NodeObject->SetArrayField(TEXT("control_hints"), StringsToJson(BuildNiagaraControlHints(InputNode->Input.GetName().ToString())));
    }
    else if (const UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(Node))
    {
        NodeObject->SetStringField(TEXT("niagara_node_kind"), TEXT("output"));
        NodeObject->SetStringField(TEXT("usage"), NiagaraScriptUsageName(OutputNode->GetUsage()));
        NodeObject->SetStringField(TEXT("usage_id"), OutputNode->GetUsageId().ToString(EGuidFormats::DigitsWithHyphens));
        NodeObject->SetArrayField(TEXT("outputs"), NiagaraVariablesToJson(OutputNode->GetOutputs()));
    }
    else
    {
        NodeObject->SetStringField(TEXT("niagara_node_kind"), TEXT("generic"));
    }

    if (bIncludePins)
    {
        NodeObject->SetArrayField(TEXT("pins"), DetailedPinsToJson(Node->Pins, bIncludeLinkedPins));
    }

    return NodeObject;
}

TArray<TSharedPtr<FJsonValue>> GraphLinksToJson(const UNiagaraGraph* Graph, int32 MaxLinks)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    if (!Graph || MaxLinks <= 0)
    {
        return Values;
    }

    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }

        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output)
            {
                continue;
            }

            for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                if (!LinkedPin)
                {
                    continue;
                }

                TSharedPtr<FJsonObject> LinkObject = MakeShared<FJsonObject>();
                LinkObject->SetObjectField(TEXT("from"), DetailedPinRefToJsonObject(Pin));
                LinkObject->SetObjectField(TEXT("to"), DetailedPinRefToJsonObject(LinkedPin));
                Values.Add(MakeShared<FJsonValueObject>(LinkObject));

                if (Values.Num() >= MaxLinks)
                {
                    return Values;
                }
            }
        }
    }

    return Values;
}

const UNiagaraNodeOutput* FindDownstreamNiagaraOutputNode(const UNiagaraNode& StartNode)
{
    TArray<const UEdGraphNode*> PendingNodes;
    TSet<const UEdGraphNode*> VisitedNodes;
    PendingNodes.Add(&StartNode);

    while (!PendingNodes.IsEmpty())
    {
        const UEdGraphNode* CurrentNode = PendingNodes.Pop(EAllowShrinking::No);
        if (!CurrentNode || VisitedNodes.Contains(CurrentNode))
        {
            continue;
        }
        VisitedNodes.Add(CurrentNode);

        if (const UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(CurrentNode))
        {
            return OutputNode;
        }

        for (const UEdGraphPin* Pin : CurrentNode->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output)
            {
                continue;
            }

            for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                const UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
                if (LinkedNode && !VisitedNodes.Contains(LinkedNode))
                {
                    PendingNodes.Add(LinkedNode);
                }
            }
        }
    }

    return nullptr;
}

bool IsFunctionCallInTargetOutputStack(
    const UNiagaraNodeFunctionCall& FunctionCall,
    const UNiagaraNodeOutput& TargetOutputNode,
    ENiagaraScriptUsage TargetUsage,
    const FGuid& TargetUsageId)
{
    if (const UNiagaraNodeOutput* ExistingOutputNode = FindDownstreamNiagaraOutputNode(FunctionCall))
    {
        return ExistingOutputNode == &TargetOutputNode ||
            (UNiagaraScript::IsEquivalentUsage(ExistingOutputNode->GetUsage(), TargetUsage) && ExistingOutputNode->GetUsageId() == TargetUsageId);
    }

    if (TargetUsageId.IsValid())
    {
        return false;
    }

    const ENiagaraScriptUsage ExistingStackUsage = FNiagaraStackGraphUtilities::GetOutputNodeUsage(FunctionCall);
    return UNiagaraScript::IsEquivalentUsage(ExistingStackUsage, TargetUsage);
}

TSharedPtr<FJsonObject> BuildDetailedGraphJson(
    const UNiagaraScriptSourceBase* SourceBase,
    const FString& Context,
    bool bIncludePins,
    bool bIncludeLinks,
    int32 MaxNodes,
    int32 MaxLinks)
{
    TSharedPtr<FJsonObject> GraphObject = MakeShared<FJsonObject>();
    GraphObject->SetStringField(TEXT("context"), Context);
    GraphObject->SetStringField(TEXT("source_class"), SourceBase ? SourceBase->GetClass()->GetName() : FString());
    GraphObject->SetStringField(TEXT("source_path"), NiagaraObjectPathOrEmpty(SourceBase));

    const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase);
    UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
    GraphObject->SetStringField(TEXT("graph_path"), NiagaraObjectPathOrEmpty(Graph));
    GraphObject->SetBoolField(TEXT("has_graph"), Graph != nullptr);

    if (!Graph)
    {
        GraphObject->SetNumberField(TEXT("node_count"), 0);
        GraphObject->SetNumberField(TEXT("included_node_count"), 0);
        GraphObject->SetNumberField(TEXT("link_count"), 0);
        GraphObject->SetArrayField(TEXT("nodes"), TArray<TSharedPtr<FJsonValue>>());
        GraphObject->SetArrayField(TEXT("links"), TArray<TSharedPtr<FJsonValue>>());
        return GraphObject;
    }

    TArray<UEdGraphNode*> Nodes = Graph->Nodes;
    Nodes.Sort(
        [](const UEdGraphNode& Left, const UEdGraphNode& Right)
        {
            if (Left.NodePosY == Right.NodePosY)
            {
                return Left.NodePosX < Right.NodePosX;
            }
            return Left.NodePosY < Right.NodePosY;
        });

    TMap<FString, int32> NodeClassCounts;
    TArray<TSharedPtr<FJsonValue>> NodeValues;
    const int32 SafeMaxNodes = FMath::Max(0, MaxNodes);
    for (int32 Index = 0; Index < Nodes.Num(); ++Index)
    {
        const UEdGraphNode* Node = Nodes[Index];
        if (!Node)
        {
            continue;
        }

        NodeClassCounts.FindOrAdd(Node->GetClass()->GetName()) += 1;
        if (NodeValues.Num() < SafeMaxNodes)
        {
            NodeValues.Add(MakeShared<FJsonValueObject>(DetailedGraphNodeToJsonObject(Node, Index, bIncludePins, bIncludeLinks)));
        }
    }

    int32 TotalLinkCount = 0;
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output)
            {
                TotalLinkCount += Pin->LinkedTo.Num();
            }
        }
    }

    TArray<TSharedPtr<FJsonValue>> NodeClassValues;
    for (const TPair<FString, int32>& Pair : NodeClassCounts)
    {
        TSharedPtr<FJsonObject> ClassObject = MakeShared<FJsonObject>();
        ClassObject->SetStringField(TEXT("node_class"), Pair.Key);
        ClassObject->SetNumberField(TEXT("count"), Pair.Value);
        NodeClassValues.Add(MakeShared<FJsonValueObject>(ClassObject));
    }

    GraphObject->SetNumberField(TEXT("node_count"), Nodes.Num());
    GraphObject->SetNumberField(TEXT("included_node_count"), NodeValues.Num());
    GraphObject->SetBoolField(TEXT("nodes_truncated"), Nodes.Num() > SafeMaxNodes);
    GraphObject->SetNumberField(TEXT("link_count"), TotalLinkCount);
    GraphObject->SetNumberField(TEXT("included_link_count"), bIncludeLinks ? FMath::Min(TotalLinkCount, FMath::Max(0, MaxLinks)) : 0);
    GraphObject->SetBoolField(TEXT("links_truncated"), bIncludeLinks && TotalLinkCount > FMath::Max(0, MaxLinks));
    GraphObject->SetArrayField(TEXT("node_class_counts"), NodeClassValues);
    GraphObject->SetArrayField(TEXT("nodes"), NodeValues);
    GraphObject->SetArrayField(TEXT("links"), bIncludeLinks ? GraphLinksToJson(Graph, MaxLinks) : TArray<TSharedPtr<FJsonValue>>());
    return GraphObject;
}

TSharedPtr<FJsonObject> BuildDetailedScratchPadScriptJson(
    const UNiagaraScript* Script,
    const FString& OwnerKind,
    bool bIncludePins,
    bool bIncludeLinks,
    int32 MaxNodes,
    int32 MaxLinks)
{
    TSharedPtr<FJsonObject> ScriptObject = MakeShared<FJsonObject>();
    if (!Script)
    {
        return ScriptObject;
    }

    ScriptObject->SetStringField(TEXT("name"), Script->GetName());
    ScriptObject->SetStringField(TEXT("path"), Script->GetPathName());
    ScriptObject->SetStringField(TEXT("owner_kind"), OwnerKind);
    ScriptObject->SetStringField(TEXT("usage"), NiagaraScriptUsageName(Script->GetUsage()));
    TArray<ENiagaraScriptUsage> SupportedUsageContexts;
    if (const FVersionedNiagaraScriptData* ScriptData = Script->GetLatestScriptData())
    {
        SupportedUsageContexts = ScriptData->GetSupportedUsageContexts();
    }
    ScriptObject->SetArrayField(TEXT("supported_usage_contexts"), NiagaraScriptUsagesToJson(SupportedUsageContexts));
    ScriptObject->SetObjectField(
        TEXT("graph"),
        BuildDetailedGraphJson(
            Script->GetLatestSource(),
            FString::Printf(TEXT("%s_scratch_pad:%s"), *OwnerKind, *Script->GetName()),
            bIncludePins,
            bIncludeLinks,
            MaxNodes,
            MaxLinks));
    return ScriptObject;
}

void AppendDetailedScratchPadContainerScripts(
    const UNiagaraScratchPadContainer* Container,
    const FString& OwnerKind,
    bool bIncludePins,
    bool bIncludeLinks,
    int32 MaxNodes,
    int32 MaxLinks,
    TArray<TSharedPtr<FJsonValue>>& OutScripts)
{
#if WITH_EDITORONLY_DATA
    if (!Container)
    {
        return;
    }

    for (const TObjectPtr<UNiagaraScript>& Script : Container->Scripts)
    {
        OutScripts.Add(MakeShared<FJsonValueObject>(
            BuildDetailedScratchPadScriptJson(Script.Get(), OwnerKind, bIncludePins, bIncludeLinks, MaxNodes, MaxLinks)));
    }
#endif
}

bool IsSettableNiagaraUserParameterType(const FNiagaraTypeDefinition& TypeDef)
{
    return TypeDef == FNiagaraTypeDefinition::GetFloatDef() ||
        TypeDef == FNiagaraTypeDefinition::GetIntDef() ||
        TypeDef == FNiagaraTypeDefinition::GetBoolDef() ||
        TypeDef == FNiagaraTypeDefinition::GetColorDef() ||
        TypeDef == FNiagaraTypeDefinition::GetVec2Def() ||
        TypeDef == FNiagaraTypeDefinition::GetVec3Def() ||
        TypeDef == FNiagaraTypeDefinition::GetVec4Def() ||
        TypeDef == FNiagaraTypeDefinition::GetPositionDef();
}

TSharedPtr<FJsonValue> NiagaraParameterValueToJson(const FNiagaraUserRedirectionParameterStore& Store, const FNiagaraVariable& Parameter)
{
    const FNiagaraTypeDefinition& TypeDef = Parameter.GetType();
    if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
    {
        return MakeShared<FJsonValueNumber>(Store.GetParameterValue<float>(Parameter));
    }
    if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
    {
        return MakeShared<FJsonValueNumber>(Store.GetParameterValue<int32>(Parameter));
    }
    if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
    {
        const FNiagaraBool Value = Store.GetParameterValue<FNiagaraBool>(Parameter);
        return MakeShared<FJsonValueBoolean>(Value.GetValue());
    }
    if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
    {
        const FLinearColor Value = Store.GetParameterValue<FLinearColor>(Parameter);
        return NumberArrayToJson({ Value.R, Value.G, Value.B, Value.A });
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec2Def())
    {
        const FVector2f Value = Store.GetParameterValue<FVector2f>(Parameter);
        return NumberArrayToJson({ Value.X, Value.Y });
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec3Def())
    {
        const FVector3f Value = Store.GetParameterValue<FVector3f>(Parameter);
        return NumberArrayToJson({ Value.X, Value.Y, Value.Z });
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec4Def())
    {
        const FVector4f Value = Store.GetParameterValue<FVector4f>(Parameter);
        return NumberArrayToJson({ Value.X, Value.Y, Value.Z, Value.W });
    }
    if (TypeDef == FNiagaraTypeDefinition::GetPositionDef())
    {
        const FVector* Value = Store.GetPositionParameterValue(Parameter.GetName());
        if (Value)
        {
            return NumberArrayToJson({ Value->X, Value->Y, Value->Z });
        }
    }
    return MakeShared<FJsonValueNull>();
}

bool SetNiagaraUserParameterValue(
    FNiagaraUserRedirectionParameterStore& Store,
    const FNiagaraVariable& Parameter,
    const TSharedPtr<FJsonValue>& JsonValue,
    FString& OutError)
{
    const FNiagaraTypeDefinition& TypeDef = Parameter.GetType();
    if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
    {
        double NumberValue = 0.0;
        if (!JsonValue.IsValid() || !JsonValue->TryGetNumber(NumberValue))
        {
            OutError = TEXT("Expected numeric value for float Niagara user parameter");
            return false;
        }
        return Store.SetParameterValue<float>(static_cast<float>(NumberValue), Parameter, false);
    }
    if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
    {
        double NumberValue = 0.0;
        if (!JsonValue.IsValid() || !JsonValue->TryGetNumber(NumberValue))
        {
            OutError = TEXT("Expected numeric value for int Niagara user parameter");
            return false;
        }
        return Store.SetParameterValue<int32>(static_cast<int32>(NumberValue), Parameter, false);
    }
    if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
    {
        bool BoolValue = false;
        if (!JsonValue.IsValid() || !JsonValue->TryGetBool(BoolValue))
        {
            OutError = TEXT("Expected boolean value for bool Niagara user parameter");
            return false;
        }
        return Store.SetParameterValue<FNiagaraBool>(FNiagaraBool(BoolValue), Parameter, false);
    }

    TArray<double> Values;
    if (!JsonArrayToDoubles(JsonValue, Values))
    {
        OutError = TEXT("Expected numeric array value for vector/color Niagara user parameter");
        return false;
    }

    if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
    {
        if (Values.Num() < 3)
        {
            OutError = TEXT("Expected color array with at least 3 values");
            return false;
        }
        const float Alpha = Values.Num() >= 4 ? static_cast<float>(Values[3]) : 1.0f;
        return Store.SetParameterValue<FLinearColor>(
            FLinearColor(static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]), Alpha),
            Parameter,
            false);
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec2Def())
    {
        if (Values.Num() < 2)
        {
            OutError = TEXT("Expected vec2 array with 2 values");
            return false;
        }
        return Store.SetParameterValue<FVector2f>(FVector2f(Values[0], Values[1]), Parameter, false);
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec3Def())
    {
        if (Values.Num() < 3)
        {
            OutError = TEXT("Expected vec3 array with 3 values");
            return false;
        }
        return Store.SetParameterValue<FVector3f>(FVector3f(Values[0], Values[1], Values[2]), Parameter, false);
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec4Def())
    {
        if (Values.Num() < 4)
        {
            OutError = TEXT("Expected vec4 array with 4 values");
            return false;
        }
        return Store.SetParameterValue<FVector4f>(FVector4f(Values[0], Values[1], Values[2], Values[3]), Parameter, false);
    }
    if (TypeDef == FNiagaraTypeDefinition::GetPositionDef())
    {
        if (Values.Num() < 3)
        {
            OutError = TEXT("Expected position array with 3 values");
            return false;
        }
        return Store.SetPositionParameterValue(FVector(Values[0], Values[1], Values[2]), Parameter.GetName(), false);
    }

    OutError = FString::Printf(TEXT("Unsupported Niagara user parameter type: %s"), *NiagaraTypeName(TypeDef));
    return false;
}

bool SetNiagaraVariableDataFromJson(FNiagaraVariable& Variable, const TSharedPtr<FJsonValue>& JsonValue, FString& OutError)
{
    const FNiagaraTypeDefinition& TypeDef = Variable.GetType();
    if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
    {
        double NumberValue = 0.0;
        if (!JsonValue.IsValid() || !JsonValue->TryGetNumber(NumberValue))
        {
            OutError = TEXT("Expected numeric value for float Niagara module input");
            return false;
        }
        Variable.SetValue<float>(static_cast<float>(NumberValue));
        return true;
    }
    if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
    {
        double NumberValue = 0.0;
        if (!JsonValue.IsValid() || !JsonValue->TryGetNumber(NumberValue))
        {
            OutError = TEXT("Expected numeric value for int Niagara module input");
            return false;
        }
        Variable.SetValue<int32>(static_cast<int32>(NumberValue));
        return true;
    }
    if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
    {
        bool BoolValue = false;
        if (!JsonValue.IsValid() || !JsonValue->TryGetBool(BoolValue))
        {
            OutError = TEXT("Expected boolean value for bool Niagara module input");
            return false;
        }
        Variable.SetValue<FNiagaraBool>(FNiagaraBool(BoolValue));
        return true;
    }

    TArray<double> Values;
    if (!JsonArrayToDoubles(JsonValue, Values))
    {
        OutError = TEXT("Expected numeric array value for vector/color Niagara module input");
        return false;
    }

    if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
    {
        if (Values.Num() < 3)
        {
            OutError = TEXT("Expected color array with at least 3 values");
            return false;
        }
        const float Alpha = Values.Num() >= 4 ? static_cast<float>(Values[3]) : 1.0f;
        Variable.SetValue<FLinearColor>(FLinearColor(static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]), Alpha));
        return true;
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec2Def())
    {
        if (Values.Num() < 2)
        {
            OutError = TEXT("Expected vec2 array with 2 values");
            return false;
        }
        Variable.SetValue<FVector2f>(FVector2f(Values[0], Values[1]));
        return true;
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec3Def())
    {
        if (Values.Num() < 3)
        {
            OutError = TEXT("Expected vec3 array with 3 values");
            return false;
        }
        Variable.SetValue<FVector3f>(FVector3f(Values[0], Values[1], Values[2]));
        return true;
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec4Def())
    {
        if (Values.Num() < 4)
        {
            OutError = TEXT("Expected vec4 array with 4 values");
            return false;
        }
        Variable.SetValue<FVector4f>(FVector4f(Values[0], Values[1], Values[2], Values[3]));
        return true;
    }
    if (TypeDef == FNiagaraTypeDefinition::GetPositionDef())
    {
        if (Values.Num() < 3)
        {
            OutError = TEXT("Expected position array with 3 values");
            return false;
        }
        Variable.SetValue<FVector3f>(FVector3f(Values[0], Values[1], Values[2]));
        return true;
    }

    OutError = FString::Printf(TEXT("Unsupported Niagara module input type: %s"), *NiagaraTypeName(TypeDef));
    return false;
}

TArray<TSharedPtr<FJsonValue>> MaterialArrayToJson(const TArray<UMaterialInterface*>& Materials)
{
    TArray<TSharedPtr<FJsonValue>> MaterialValues;
    for (const UMaterialInterface* Material : Materials)
    {
        MaterialValues.Add(MakeShared<FJsonValueString>(NiagaraObjectPathOrEmpty(Material)));
    }
    return MaterialValues;
}

FString RendererMaterialField(const UNiagaraRendererProperties* Renderer)
{
    if (Renderer->IsA<UNiagaraSpriteRendererProperties>())
    {
        return TEXT("Material");
    }
    if (Renderer->IsA<UNiagaraRibbonRendererProperties>())
    {
        return TEXT("Material");
    }
    if (Renderer->IsA<UNiagaraMeshRendererProperties>())
    {
        return TEXT("OverrideMaterials[material_slot].ExplicitMat");
    }
    if (Renderer->IsA<UNiagaraDecalRendererProperties>() || Renderer->IsA<UNiagaraVolumeRendererProperties>())
    {
        return TEXT("Material");
    }
    return FString();
}

bool SetRendererMaterial(
    UNiagaraRendererProperties* Renderer,
    UMaterialInterface* Material,
    int32 MaterialSlotIndex,
    FString& OutChangedField,
    FString& OutError)
{
    if (!Renderer)
    {
        OutError = TEXT("Renderer is null");
        return false;
    }

    if (!Material)
    {
        OutError = TEXT("Material is null");
        return false;
    }

    if (UNiagaraSpriteRendererProperties* SpriteRenderer = Cast<UNiagaraSpriteRendererProperties>(Renderer))
    {
        SpriteRenderer->Modify();
        SpriteRenderer->Material = Material;
        SpriteRenderer->MICMaterial = nullptr;
        OutChangedField = TEXT("Material");
        return true;
    }

    if (UNiagaraRibbonRendererProperties* RibbonRenderer = Cast<UNiagaraRibbonRendererProperties>(Renderer))
    {
        RibbonRenderer->Modify();
        RibbonRenderer->Material = Material;
        RibbonRenderer->MICMaterial = nullptr;
        OutChangedField = TEXT("Material");
        return true;
    }

    if (UNiagaraMeshRendererProperties* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(Renderer))
    {
        const int32 SafeMaterialSlot = FMath::Max(0, MaterialSlotIndex);
        MeshRenderer->Modify();
        MeshRenderer->bOverrideMaterials = true;
        if (MeshRenderer->OverrideMaterials.Num() <= SafeMaterialSlot)
        {
            MeshRenderer->OverrideMaterials.SetNum(SafeMaterialSlot + 1);
        }
        MeshRenderer->OverrideMaterials[SafeMaterialSlot].ExplicitMat = Material;
        OutChangedField = FString::Printf(TEXT("OverrideMaterials[%d].ExplicitMat"), SafeMaterialSlot);
        return true;
    }

    if (UNiagaraDecalRendererProperties* DecalRenderer = Cast<UNiagaraDecalRendererProperties>(Renderer))
    {
        DecalRenderer->Modify();
        DecalRenderer->Material = Material;
        DecalRenderer->MICMaterial = nullptr;
        OutChangedField = TEXT("Material");
        return true;
    }

    if (UNiagaraVolumeRendererProperties* VolumeRenderer = Cast<UNiagaraVolumeRendererProperties>(Renderer))
    {
        VolumeRenderer->Modify();
        VolumeRenderer->Material = Material;
        VolumeRenderer->MICMaterial = nullptr;
        OutChangedField = TEXT("Material");
        return true;
    }

    OutError = FString::Printf(TEXT("Unsupported Niagara renderer class: %s"), *Renderer->GetClass()->GetName());
    return false;
}

void AddUniqueString(TArray<FString>& Values, const FString& Value)
{
    if (!Value.IsEmpty() && !Values.Contains(Value))
    {
        Values.Add(Value);
    }
}

TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values)
{
    TArray<TSharedPtr<FJsonValue>> JsonValues;
    for (const FString& Value : Values)
    {
        JsonValues.Add(MakeShared<FJsonValueString>(Value));
    }
    return JsonValues;
}

void AddHintIfContains(const FString& SearchText, const FString& Needle, const FString& Hint, TArray<FString>& Hints)
{
    if (SearchText.Contains(Needle, ESearchCase::IgnoreCase))
    {
        AddUniqueString(Hints, Hint);
    }
}

TArray<FString> BuildNiagaraControlHints(const FString& SearchText)
{
    TArray<FString> Hints;
    AddHintIfContains(SearchText, TEXT("color"), TEXT("color_or_tint_control"), Hints);
    AddHintIfContains(SearchText, TEXT("colour"), TEXT("color_or_tint_control"), Hints);
    AddHintIfContains(SearchText, TEXT("tint"), TEXT("color_or_tint_control"), Hints);
    AddHintIfContains(SearchText, TEXT("ribbonwidth"), TEXT("ribbon_width_control"), Hints);
    AddHintIfContains(SearchText, TEXT("ribbon width"), TEXT("ribbon_width_control"), Hints);
    AddHintIfContains(SearchText, TEXT("width"), TEXT("width_control"), Hints);
    AddHintIfContains(SearchText, TEXT("scale"), TEXT("scale_control"), Hints);
    AddHintIfContains(SearchText, TEXT("size"), TEXT("size_control"), Hints);
    AddHintIfContains(SearchText, TEXT("lifetime"), TEXT("lifetime_control"), Hints);
    AddHintIfContains(SearchText, TEXT("life time"), TEXT("lifetime_control"), Hints);
    AddHintIfContains(SearchText, TEXT("duration"), TEXT("duration_control"), Hints);
    AddHintIfContains(SearchText, TEXT("velocity"), TEXT("velocity_control"), Hints);
    AddHintIfContains(SearchText, TEXT("speed"), TEXT("velocity_control"), Hints);
    AddHintIfContains(SearchText, TEXT("spawn"), TEXT("spawn_control"), Hints);
    AddHintIfContains(SearchText, TEXT("materialparam"), TEXT("dynamic_material_parameter_control"), Hints);
    AddHintIfContains(SearchText, TEXT("dynamicmaterial"), TEXT("dynamic_material_parameter_control"), Hints);
    AddHintIfContains(SearchText, TEXT("user."), TEXT("user_parameter_reference"), Hints);
    AddHintIfContains(SearchText, TEXT("scratch"), TEXT("scratch_pad_reference"), Hints);
    AddHintIfContains(SearchText, TEXT("trail"), TEXT("trail_behavior"), Hints);
    AddHintIfContains(SearchText, TEXT("ribbon"), TEXT("ribbon_behavior"), Hints);
    return Hints;
}

FString InferNiagaraInputControlKind(const FString& FunctionName, const FString& PinName, const FString& PinCategory)
{
    const FString SearchText = FString::Printf(TEXT("%s %s %s"), *FunctionName, *PinName, *PinCategory);

    if (SearchText.Contains(TEXT("dynamicmaterial"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("materialparam"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("material parameter"), ESearchCase::IgnoreCase))
    {
        return TEXT("dynamic_material_parameter");
    }
    if (SearchText.Contains(TEXT("color"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("colour"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("tint"), ESearchCase::IgnoreCase))
    {
        return TEXT("color");
    }
    if (SearchText.Contains(TEXT("ribbonwidth"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("ribbon width"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("width"), ESearchCase::IgnoreCase))
    {
        return TEXT("width");
    }
    if (SearchText.Contains(TEXT("scale"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("size"), ESearchCase::IgnoreCase))
    {
        return TEXT("scale_or_size");
    }
    if (SearchText.Contains(TEXT("velocity"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("speed"), ESearchCase::IgnoreCase))
    {
        return TEXT("velocity");
    }
    if (SearchText.Contains(TEXT("lifetime"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("life time"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("duration"), ESearchCase::IgnoreCase))
    {
        return TEXT("lifetime");
    }
    if (SearchText.Contains(TEXT("spawn"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("count"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("rate"), ESearchCase::IgnoreCase))
    {
        return TEXT("spawn");
    }
    if (SearchText.Contains(TEXT("user."), ESearchCase::IgnoreCase))
    {
        return TEXT("user_parameter_reference");
    }
    if (PinCategory.Contains(TEXT("float"), ESearchCase::IgnoreCase) ||
        PinCategory.Contains(TEXT("double"), ESearchCase::IgnoreCase) ||
        PinCategory.Contains(TEXT("int"), ESearchCase::IgnoreCase) ||
        PinCategory.Contains(TEXT("bool"), ESearchCase::IgnoreCase) ||
        PinCategory.Contains(TEXT("struct"), ESearchCase::IgnoreCase))
    {
        return TEXT("numeric_or_struct");
    }
    return TEXT("unknown");
}

int32 NiagaraInputControlPriority(const FString& ControlKind, const FString& FunctionName, const FString& PinName)
{
    int32 Priority = 10;
    if (ControlKind == TEXT("color") ||
        ControlKind == TEXT("dynamic_material_parameter") ||
        ControlKind == TEXT("scale_or_size") ||
        ControlKind == TEXT("width"))
    {
        Priority = 80;
    }
    else if (ControlKind == TEXT("velocity") ||
        ControlKind == TEXT("spawn") ||
        ControlKind == TEXT("lifetime"))
    {
        Priority = 75;
    }
    else if (ControlKind == TEXT("user_parameter_reference"))
    {
        Priority = 70;
    }
    else if (ControlKind == TEXT("numeric_or_struct"))
    {
        Priority = 35;
    }

    if (FunctionName.Contains(TEXT("ScaleColor"), ESearchCase::IgnoreCase) ||
        FunctionName.Contains(TEXT("DynamicMaterial"), ESearchCase::IgnoreCase))
    {
        Priority += 25;
    }
    else if (FunctionName.Contains(TEXT("AddVelocity"), ESearchCase::IgnoreCase) ||
        FunctionName.Contains(TEXT("SpawnBurst"), ESearchCase::IgnoreCase))
    {
        Priority += 18;
    }
    else if (FunctionName.Contains(TEXT("ScaleMesh"), ESearchCase::IgnoreCase))
    {
        Priority += 15;
    }
    else if (FunctionName.Equals(TEXT("EmitterState"), ESearchCase::IgnoreCase))
    {
        Priority -= 35;
    }

    if (PinName.StartsWith(TEXT("Use "), ESearchCase::IgnoreCase) ||
        PinName.StartsWith(TEXT("Use"), ESearchCase::IgnoreCase))
    {
        Priority -= 10;
    }
    if (PinName.Contains(TEXT("Curve"), ESearchCase::IgnoreCase) && FunctionName.Contains(TEXT("ScaleColor"), ESearchCase::IgnoreCase))
    {
        Priority += 8;
    }

    return FMath::Clamp(Priority, 0, 100);
}

TSharedPtr<FJsonObject> BuildFunctionCallJson(const UNiagaraNodeFunctionCall* FunctionCall, int32 NodeIndex, bool bIncludePins);
TSharedPtr<FJsonObject> BuildRenderTargetObjectJson(const UObject* Object);
TSharedPtr<FJsonObject> BuildDataInterfaceInspectionJson(const UNiagaraDataInterface* DataInterface, UNiagaraSystem* System = nullptr);
const UObject* GetObjectPropertyForInspection(const UObject* Object, const FName PropertyName);

bool IsLowSignalNiagaraInputPinName(const FString& PinName)
{
    return PinName.Equals(TEXT("InputMap"), ESearchCase::IgnoreCase) ||
        PinName.Equals(TEXT("OutputMap"), ESearchCase::IgnoreCase) ||
        PinName.Contains(TEXT("Parameter Map"), ESearchCase::IgnoreCase) ||
        PinName.Contains(TEXT("ParameterMap"), ESearchCase::IgnoreCase) ||
        PinName.Contains(TEXT("Write Parameter Index"), ESearchCase::IgnoreCase);
}

bool IsCandidateNiagaraInputPin(const UEdGraphPin* Pin)
{
    if (!Pin || Pin->Direction != EGPD_Input)
    {
        return false;
    }

    const FString PinName = Pin->PinName.ToString();
    const FString PinCategory = Pin->PinType.PinCategory.ToString();
    if (PinCategory.Equals(TEXT("exec"), ESearchCase::IgnoreCase) ||
        IsLowSignalNiagaraInputPinName(PinName))
    {
        return false;
    }

    return true;
}

TSharedPtr<FJsonObject> BuildModuleInputCandidateJson(
    const UNiagaraNodeFunctionCall* FunctionCall,
    const UEdGraphPin* Pin,
    const FString& EmitterName,
    int32 EmitterIndex,
    int32 ModuleIndex,
    bool bIncludeLinkedSources)
{
    TSharedPtr<FJsonObject> CandidateObject = MakeShared<FJsonObject>();
    if (!FunctionCall || !Pin)
    {
        return CandidateObject;
    }

    FString FunctionName = FunctionCall->GetFunctionName();
    if (FunctionName.IsEmpty())
    {
        FunctionName = FunctionCall->Signature.GetNameString();
    }

    const FString PinName = Pin->PinName.ToString();
    const FString PinCategory = Pin->PinType.PinCategory.ToString();
    const FString ControlKind = InferNiagaraInputControlKind(FunctionName, PinName, PinCategory);

    CandidateObject->SetStringField(TEXT("emitter_name"), EmitterName);
    CandidateObject->SetNumberField(TEXT("emitter_index"), EmitterIndex);
    CandidateObject->SetNumberField(TEXT("module_index"), ModuleIndex);
    CandidateObject->SetStringField(TEXT("module_name"), FunctionName);
    CandidateObject->SetStringField(TEXT("module_node_name"), FunctionCall->GetName());
    CandidateObject->SetStringField(TEXT("module_node_guid"), FunctionCall->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    CandidateObject->SetStringField(TEXT("pin_name"), PinName);
    CandidateObject->SetStringField(TEXT("pin_category"), PinCategory);
    CandidateObject->SetStringField(TEXT("pin_subcategory"), Pin->PinType.PinSubCategory.ToString());
    CandidateObject->SetStringField(TEXT("default_value"), Pin->DefaultValue);
    CandidateObject->SetStringField(TEXT("default_object"), NiagaraObjectPathOrEmpty(Pin->DefaultObject));
    CandidateObject->SetNumberField(TEXT("linked_to_count"), Pin->LinkedTo.Num());
    CandidateObject->SetArrayField(TEXT("linked_sources"), LinkedPinsToJson(Pin, bIncludeLinkedSources));
    CandidateObject->SetStringField(TEXT("control_kind"), ControlKind);
    CandidateObject->SetNumberField(TEXT("priority"), NiagaraInputControlPriority(ControlKind, FunctionName, PinName));
    CandidateObject->SetBoolField(TEXT("can_author_now"), false);
    CandidateObject->SetStringField(TEXT("authoring_status"), TEXT("read_only_candidate; module input writing is intentionally not enabled yet"));
    return CandidateObject;
}

UNiagaraScript* FindEmitterScriptForUsage(FVersionedNiagaraEmitterData* EmitterData, ENiagaraScriptUsage Usage)
{
    if (!EmitterData)
    {
        return nullptr;
    }

    TArray<UNiagaraScript*> EmitterScripts;
    EmitterData->GetScripts(EmitterScripts, false, false);
    for (UNiagaraScript* Script : EmitterScripts)
    {
        if (Script && Script->GetUsage() == Usage)
        {
            return Script;
        }
    }
    return nullptr;
}

FNiagaraVariable BuildRapidIterationParameterForInput(
    const UNiagaraNodeFunctionCall* FunctionCall,
    const FNiagaraVariable& InputVariable,
    const FString& EmitterName,
    ENiagaraScriptUsage ScriptUsage)
{
    if (!FunctionCall || EmitterName.IsEmpty())
    {
        return FNiagaraVariable();
    }

    const FNiagaraParameterHandle InputHandle(InputVariable.GetName());
    const FNiagaraParameterHandle ModuleHandle = InputHandle.IsModuleHandle()
        ? InputHandle
        : FNiagaraParameterHandle::CreateModuleParameterHandle(InputVariable.GetName());
    const FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleHandle, FunctionCall);
    FNiagaraVariable AliasedVariable(InputVariable.GetType(), AliasedHandle.GetParameterHandleString());
    return FNiagaraUtilities::ConvertVariableToRapidIterationConstantName(AliasedVariable, *EmitterName, ScriptUsage);
}

FNiagaraParameterHandle BuildAliasedModuleParameterHandleForInput(
    const UNiagaraNodeFunctionCall* FunctionCall,
    const FNiagaraVariable& InputVariable)
{
    if (!FunctionCall)
    {
        return FNiagaraParameterHandle();
    }

    const FNiagaraParameterHandle InputHandle(InputVariable.GetName());
    const FNiagaraParameterHandle ModuleHandle = InputHandle.IsModuleHandle()
        ? InputHandle
        : FNiagaraParameterHandle::CreateModuleParameterHandle(InputVariable.GetName());
    return FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleHandle, FunctionCall);
}

bool NiagaraModuleInputNameMatches(const FNiagaraVariable& Candidate, const FString& InputName)
{
    const FString CandidateName = Candidate.GetName().ToString();
    FString ShortName = CandidateName;
    ShortName.RemoveFromStart(TEXT("Module."));
    return CandidateName.Equals(InputName, ESearchCase::IgnoreCase) ||
        ShortName.Equals(InputName, ESearchCase::IgnoreCase);
}

bool NiagaraUserParameterNameMatches(const FNiagaraVariable& Candidate, const FString& UserParameterName)
{
    const FString CandidateName = Candidate.GetName().ToString();
    FString CandidateShortName = CandidateName;
    CandidateShortName.RemoveFromStart(TEXT("User."));
    FString RequestedShortName = UserParameterName;
    RequestedShortName.RemoveFromStart(TEXT("User."));
    return CandidateName.Equals(UserParameterName, ESearchCase::IgnoreCase) ||
        CandidateShortName.Equals(RequestedShortName, ESearchCase::IgnoreCase);
}

UEdGraphPin* FindNiagaraStackFunctionInputOverridePin(
    const UNiagaraNodeFunctionCall* FunctionCall,
    const FNiagaraParameterHandle& AliasedInputHandle)
{
    if (!FunctionCall)
    {
        return nullptr;
    }

    UEdGraph* Graph = FunctionCall->GetGraph();
    if (!Graph)
    {
        return nullptr;
    }

    const FName TargetPinName = AliasedInputHandle.GetParameterHandleString();
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node || !Node->GetClass()->GetName().Equals(TEXT("NiagaraNodeParameterMapSet")))
        {
            continue;
        }

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == TargetPinName)
            {
                return Pin;
            }
        }
    }

    return nullptr;
}

bool ValidateLinkedNiagaraDataInterfaceInputNode(UEdGraphPin& OverridePin, FString& OutError)
{
    if (OverridePin.LinkedTo.Num() == 0)
    {
        return true;
    }

    if (OverridePin.LinkedTo.Num() != 1)
    {
        OutError = FString::Printf(TEXT("Expected one linked override value node, found %d"), OverridePin.LinkedTo.Num());
        return false;
    }

    UEdGraphPin* LinkedPin = OverridePin.LinkedTo[0];
    UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
    UEdGraph* Graph = LinkedNode ? LinkedNode->GetGraph() : nullptr;
    if (!LinkedNode || !Graph)
    {
        OutError = TEXT("Linked override value node or graph is invalid");
        return false;
    }

    const bool bIsSupportedOverrideValueNode =
        LinkedNode->IsA<UNiagaraNodeInput>() ||
        LinkedNode->GetClass()->GetName().Equals(TEXT("NiagaraNodeParameterMapGet"));
    if (!bIsSupportedOverrideValueNode)
    {
        OutError = FString::Printf(
            TEXT("Existing override value node '%s' is not a direct Niagara data input or parameter-map-get node; refusing to rewrite an unsupported graph shape"),
            *LinkedNode->GetName());
        return false;
    }
    if (LinkedPin->LinkedTo.Num() != 1 || LinkedPin->LinkedTo[0] != &OverridePin)
    {
        OutError = FString::Printf(
            TEXT("Existing override value node '%s' output is shared by multiple graph links; refusing to remove it"),
            *LinkedNode->GetName());
        return false;
    }
    for (const UEdGraphPin* Pin : LinkedNode->Pins)
    {
        if (!Pin || Pin == LinkedPin || Pin->Direction != EGPD_Output)
        {
            continue;
        }
        if (Pin->LinkedTo.Num() > 0)
        {
            OutError = FString::Printf(
                TEXT("Existing override value node '%s' has additional linked outputs; refusing to remove it"),
                *LinkedNode->GetName());
            return false;
        }
    }

    return true;
}

bool ClearLinkedNiagaraDataInterfaceInputNode(UEdGraphPin& OverridePin, FString& OutError)
{
    if (!ValidateLinkedNiagaraDataInterfaceInputNode(OverridePin, OutError))
    {
        return false;
    }

    if (OverridePin.LinkedTo.Num() == 0)
    {
        return true;
    }

    UEdGraphPin* LinkedPin = OverridePin.LinkedTo[0];
    UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
    UEdGraph* Graph = LinkedNode ? LinkedNode->GetGraph() : nullptr;
    if (!LinkedNode || !Graph)
    {
        OutError = TEXT("Linked override value node or graph is invalid");
        return false;
    }

    Graph->Modify();
    LinkedNode->Modify();
    OverridePin.BreakAllPinLinks(true);
    Graph->RemoveNode(LinkedNode);
    return true;
}

TSharedPtr<FJsonObject> BuildResolvedStackInputJson(
    const UNiagaraNodeFunctionCall* FunctionCall,
    const FNiagaraVariable& InputVariable,
    const TSet<FNiagaraVariable>& HiddenVariables,
    UNiagaraScript* OwningScript,
    const FString& EmitterName)
{
    TSharedPtr<FJsonObject> InputObject = MakeShared<FJsonObject>();
    InputObject->SetObjectField(TEXT("variable"), NiagaraVariableToJsonObject(InputVariable));
    InputObject->SetBoolField(TEXT("is_hidden"), HiddenVariables.Contains(InputVariable));
    InputObject->SetBoolField(TEXT("can_author_now"), false);

    if (!OwningScript)
    {
        InputObject->SetStringField(TEXT("value_source"), TEXT("no_owning_script"));
        return InputObject;
    }

    FNiagaraVariable RapidIterationParameter = BuildRapidIterationParameterForInput(
        FunctionCall,
        InputVariable,
        EmitterName,
        OwningScript->GetUsage());
    const uint8* ValueData = RapidIterationParameter.IsValid()
        ? OwningScript->RapidIterationParameters.GetParameterData(RapidIterationParameter)
        : nullptr;

    TSharedPtr<FJsonObject> RapidIterationObject = NiagaraVariableToJsonObject(RapidIterationParameter);
    RapidIterationObject->SetBoolField(TEXT("has_value"), ValueData != nullptr);
    if (ValueData)
    {
        RapidIterationParameter.SetData(ValueData);
        RapidIterationObject->SetField(TEXT("value"), NiagaraVariableDataToJsonValue(RapidIterationParameter));
        RapidIterationObject->SetStringField(TEXT("value_string"), RapidIterationParameter.ToString());
        InputObject->SetStringField(TEXT("value_source"), TEXT("rapid_iteration"));
    }
    else
    {
        RapidIterationObject->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
        InputObject->SetStringField(TEXT("value_source"), TEXT("unresolved_default"));
    }
    InputObject->SetObjectField(TEXT("rapid_iteration_parameter"), RapidIterationObject);
    return InputObject;
}

TArray<TSharedPtr<FJsonValue>> BuildResolvedStackInputsJson(
    const UNiagaraNodeFunctionCall* FunctionCall,
    FVersionedNiagaraEmitter OwningEmitter,
    FVersionedNiagaraEmitterData* EmitterData,
    const FString& EmitterName,
    int32 MaxResolvedInputs)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    if (!FunctionCall)
    {
        return Values;
    }

    const ENiagaraScriptUsage OutputUsage = FNiagaraStackGraphUtilities::GetOutputNodeUsage(*FunctionCall);
    FCompileConstantResolver ConstantResolver(OwningEmitter, OutputUsage, FunctionCall->DebugState);

    TArray<FNiagaraVariable> InputVariables;
    TSet<FNiagaraVariable> HiddenVariables;
    FNiagaraStackGraphUtilities::GetStackFunctionInputs(
        *FunctionCall,
        InputVariables,
        HiddenVariables,
        ConstantResolver,
        FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly,
        true);

    UNiagaraScript* OwningScript = FindEmitterScriptForUsage(EmitterData, OutputUsage);
    const int32 SafeMaxInputs = FMath::Max(0, MaxResolvedInputs);
    for (int32 Index = 0; Index < InputVariables.Num() && Index < SafeMaxInputs; ++Index)
    {
        Values.Add(MakeShared<FJsonValueObject>(BuildResolvedStackInputJson(
            FunctionCall,
            InputVariables[Index],
            HiddenVariables,
            OwningScript,
            EmitterName)));
    }
    return Values;
}

TSharedPtr<FJsonObject> BuildModuleInputModuleJson(
    const UNiagaraNodeFunctionCall* FunctionCall,
    FVersionedNiagaraEmitter OwningEmitter,
    FVersionedNiagaraEmitterData* EmitterData,
    const FString& EmitterName,
    int32 EmitterIndex,
    int32 ModuleIndex,
    bool bIncludeLinkedSources,
    bool bIncludeResolvedStackInputs,
    int32 MaxCandidatesPerModule,
    int32 MaxResolvedInputsPerModule,
    int32& OutCandidateCount,
    TArray<TSharedPtr<FJsonObject>>& OutTopCandidates)
{
    TSharedPtr<FJsonObject> ModuleObject = BuildFunctionCallJson(FunctionCall, ModuleIndex, false);
    TArray<TSharedPtr<FJsonValue>> CandidateValues;
    if (!FunctionCall)
    {
        ModuleObject->SetArrayField(TEXT("input_candidates"), CandidateValues);
        ModuleObject->SetNumberField(TEXT("input_candidate_count"), 0);
        return ModuleObject;
    }

    const TArray<TSharedPtr<FJsonValue>> ResolvedStackInputValues = bIncludeResolvedStackInputs
        ? BuildResolvedStackInputsJson(FunctionCall, OwningEmitter, EmitterData, EmitterName, MaxResolvedInputsPerModule)
        : TArray<TSharedPtr<FJsonValue>>();
    ModuleObject->SetBoolField(TEXT("resolved_stack_inputs_enabled"), bIncludeResolvedStackInputs);
    ModuleObject->SetNumberField(TEXT("resolved_stack_input_count"), ResolvedStackInputValues.Num());
    ModuleObject->SetArrayField(TEXT("resolved_stack_inputs"), ResolvedStackInputValues);

    TArray<UEdGraphPin*> InputPins;
    FunctionCall->GetInputPins(InputPins);
    int32 CandidateCount = 0;
    for (const UEdGraphPin* Pin : InputPins)
    {
        if (!IsCandidateNiagaraInputPin(Pin))
        {
            continue;
        }

        TSharedPtr<FJsonObject> CandidateObject = BuildModuleInputCandidateJson(
            FunctionCall,
            Pin,
            EmitterName,
            EmitterIndex,
            ModuleIndex,
            bIncludeLinkedSources);
        ++CandidateCount;
        ++OutCandidateCount;
        OutTopCandidates.Add(CandidateObject);
        if (CandidateValues.Num() < MaxCandidatesPerModule)
        {
            CandidateValues.Add(MakeShared<FJsonValueObject>(CandidateObject));
        }
    }

    ModuleObject->SetNumberField(TEXT("input_candidate_count"), CandidateCount);
    ModuleObject->SetArrayField(TEXT("input_candidates"), CandidateValues);
    return ModuleObject;
}

TArray<UNiagaraNodeFunctionCall*> CollectSortedNiagaraFunctionCalls(const UNiagaraScriptSourceBase* SourceBase)
{
    TArray<UNiagaraNodeFunctionCall*> FunctionCalls;
    const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase);
    UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
    if (!Graph)
    {
        return FunctionCalls;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node))
        {
            FunctionCalls.Add(FunctionCall);
        }
    }

    FunctionCalls.Sort(
        [](const UNiagaraNodeFunctionCall& Left, const UNiagaraNodeFunctionCall& Right)
        {
            if (Left.NodePosY == Right.NodePosY)
            {
                return Left.NodePosX < Right.NodePosX;
            }
            return Left.NodePosY < Right.NodePosY;
        });
    return FunctionCalls;
}

TSharedPtr<FJsonObject> BuildFunctionCallJson(const UNiagaraNodeFunctionCall* FunctionCall, int32 NodeIndex, bool bIncludePins)
{
    TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
    if (!FunctionCall)
    {
        return NodeObject;
    }

    FString FunctionName = FunctionCall->GetFunctionName();
    if (FunctionName.IsEmpty())
    {
        FunctionName = FunctionCall->Signature.GetNameString();
    }

    const UNiagaraScript* FunctionScript = FunctionCall->FunctionScript;
    const UObject* ScriptOuter = FunctionScript ? FunctionScript->GetOuter() : nullptr;
    const bool bIsScratchPad = ScriptOuter && ScriptOuter->IsA<UNiagaraScratchPadContainer>();

    NodeObject->SetNumberField(TEXT("node_index"), NodeIndex);
    NodeObject->SetStringField(TEXT("node_name"), FunctionCall->GetName());
    NodeObject->SetStringField(TEXT("node_guid"), FunctionCall->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    NodeObject->SetStringField(TEXT("function_name"), FunctionName);
    NodeObject->SetStringField(TEXT("function_script"), NiagaraObjectPathOrEmpty(FunctionScript));
    NodeObject->SetStringField(TEXT("function_script_asset_object_path"), FunctionCall->FunctionScriptAssetObjectPath.ToString());
    NodeObject->SetStringField(TEXT("script_usage"), FunctionScript ? NiagaraScriptUsageName(FunctionScript->GetUsage()) : FString());
    NodeObject->SetStringField(TEXT("called_usage"), NiagaraScriptUsageName(FunctionCall->GetCalledUsage()));
    NodeObject->SetBoolField(TEXT("is_scratch_pad"), bIsScratchPad);
    NodeObject->SetStringField(TEXT("source_kind"), bIsScratchPad ? TEXT("scratch_pad") : (FunctionScript ? TEXT("script_asset") : TEXT("signature_only")));
    NodeObject->SetArrayField(TEXT("signature_inputs"), NiagaraVariablesToJson(FunctionCall->Signature.Inputs));
    NodeObject->SetArrayField(TEXT("signature_outputs"), NiagaraVariableBasesToJson(FunctionCall->Signature.Outputs));

    TArray<FString> SpecifierValues;
    for (const TPair<FName, FName>& Specifier : FunctionCall->FunctionSpecifiers)
    {
        AddUniqueString(SpecifierValues, FString::Printf(TEXT("%s=%s"), *Specifier.Key.ToString(), *Specifier.Value.ToString()));
    }
    NodeObject->SetArrayField(TEXT("function_specifiers"), StringsToJson(SpecifierValues));

    FString SearchText = FunctionName;
    SearchText += TEXT(" ");
    SearchText += NiagaraObjectPathOrEmpty(FunctionScript);
    for (const FNiagaraVariable& Variable : FunctionCall->Signature.Inputs)
    {
        SearchText += TEXT(" ");
        SearchText += Variable.GetName().ToString();
    }
    for (const FNiagaraVariable& Variable : FunctionCall->Signature.Outputs)
    {
        SearchText += TEXT(" ");
        SearchText += Variable.GetName().ToString();
    }
    NodeObject->SetArrayField(TEXT("control_hints"), StringsToJson(BuildNiagaraControlHints(SearchText)));

    if (bIncludePins)
    {
        TArray<UEdGraphPin*> InputPins;
        TArray<UEdGraphPin*> OutputPins;
        FunctionCall->GetInputPins(InputPins);
        FunctionCall->GetOutputPins(OutputPins);
        NodeObject->SetArrayField(TEXT("input_pins"), GraphPinsToJson(InputPins));
        NodeObject->SetArrayField(TEXT("output_pins"), GraphPinsToJson(OutputPins));
    }

    return NodeObject;
}

TSharedPtr<FJsonObject> BuildInputNodeJson(const UNiagaraNodeInput* InputNode)
{
    TSharedPtr<FJsonObject> InputObject = MakeShared<FJsonObject>();
    if (!InputNode)
    {
        return InputObject;
    }

    InputObject->SetStringField(TEXT("node_name"), InputNode->GetName());
    InputObject->SetStringField(TEXT("node_guid"), InputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    InputObject->SetObjectField(TEXT("variable"), NiagaraVariableToJsonObject(InputNode->Input));
    InputObject->SetNumberField(TEXT("call_sort_priority"), InputNode->CallSortPriority);
    InputObject->SetBoolField(TEXT("is_exposed"), InputNode->IsExposed());
    InputObject->SetBoolField(TEXT("is_required"), InputNode->IsRequired());
    InputObject->SetBoolField(TEXT("is_hidden"), InputNode->IsHidden());
    InputObject->SetBoolField(TEXT("can_auto_bind"), InputNode->CanAutoBind());
    InputObject->SetObjectField(TEXT("object_asset"), BuildRenderTargetObjectJson(GetObjectPropertyForInspection(InputNode, TEXT("ObjectAsset"))));
    InputObject->SetObjectField(TEXT("data_interface"), BuildDataInterfaceInspectionJson(Cast<UNiagaraDataInterface>(GetObjectPropertyForInspection(InputNode, TEXT("DataInterface")))));
    InputObject->SetArrayField(TEXT("control_hints"), StringsToJson(BuildNiagaraControlHints(InputNode->Input.GetName().ToString())));
    return InputObject;
}

TSharedPtr<FJsonObject> BuildOutputNodeJson(const UNiagaraNodeOutput* OutputNode)
{
    TSharedPtr<FJsonObject> OutputObject = MakeShared<FJsonObject>();
    if (!OutputNode)
    {
        return OutputObject;
    }

    OutputObject->SetStringField(TEXT("node_name"), OutputNode->GetName());
    OutputObject->SetStringField(TEXT("node_guid"), OutputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    OutputObject->SetStringField(TEXT("usage"), NiagaraScriptUsageName(OutputNode->GetUsage()));
    OutputObject->SetStringField(TEXT("usage_id"), OutputNode->GetUsageId().ToString(EGuidFormats::DigitsWithHyphens));
    OutputObject->SetArrayField(TEXT("outputs"), NiagaraVariablesToJson(OutputNode->GetOutputs()));
    return OutputObject;
}

TSharedPtr<FJsonObject> BuildUObjectReferenceJson(const UObject* Object)
{
    TSharedPtr<FJsonObject> ObjectJson = MakeShared<FJsonObject>();
    ObjectJson->SetBoolField(TEXT("is_valid"), Object != nullptr);
    if (!Object)
    {
        return ObjectJson;
    }

    ObjectJson->SetStringField(TEXT("name"), Object->GetName());
    ObjectJson->SetStringField(TEXT("path"), Object->GetPathName());
    ObjectJson->SetStringField(TEXT("class"), Object->GetClass() ? Object->GetClass()->GetName() : FString());
    ObjectJson->SetStringField(TEXT("class_path"), Object->GetClass() ? Object->GetClass()->GetPathName() : FString());
    ObjectJson->SetStringField(TEXT("outer_path"), Object->GetOuter() ? Object->GetOuter()->GetPathName() : FString());
    return ObjectJson;
}

TSharedPtr<FJsonObject> BuildRenderTargetObjectJson(const UObject* Object)
{
    TSharedPtr<FJsonObject> ObjectJson = BuildUObjectReferenceJson(Object);
    const UTextureRenderTarget* RenderTarget = Cast<UTextureRenderTarget>(Object);
    ObjectJson->SetBoolField(TEXT("is_texture_render_target"), RenderTarget != nullptr);
    if (RenderTarget)
    {
        ObjectJson->SetNumberField(TEXT("target_gamma"), RenderTarget->TargetGamma);
        ObjectJson->SetBoolField(TEXT("can_create_uav"), RenderTarget->bCanCreateUAV != 0);
        ObjectJson->SetBoolField(TEXT("needs_two_copies"), RenderTarget->bNeedsTwoCopies != 0);
    }

    const UTextureRenderTarget2D* RenderTarget2D = Cast<UTextureRenderTarget2D>(Object);
    ObjectJson->SetBoolField(TEXT("is_texture_render_target_2d"), RenderTarget2D != nullptr);
    if (RenderTarget2D)
    {
        ObjectJson->SetNumberField(TEXT("size_x"), RenderTarget2D->SizeX);
        ObjectJson->SetNumberField(TEXT("size_y"), RenderTarget2D->SizeY);
        ObjectJson->SetStringField(TEXT("render_target_format"), TextureRenderTargetFormatName(RenderTarget2D->RenderTargetFormat));

        TArray<TSharedPtr<FJsonValue>> ClearColorValues;
        ClearColorValues.Add(MakeShared<FJsonValueNumber>(RenderTarget2D->ClearColor.R));
        ClearColorValues.Add(MakeShared<FJsonValueNumber>(RenderTarget2D->ClearColor.G));
        ClearColorValues.Add(MakeShared<FJsonValueNumber>(RenderTarget2D->ClearColor.B));
        ClearColorValues.Add(MakeShared<FJsonValueNumber>(RenderTarget2D->ClearColor.A));
        ObjectJson->SetArrayField(TEXT("clear_color"), ClearColorValues);
    }
    return ObjectJson;
}

const UObject* GetObjectPropertyForInspection(const UObject* Object, const FName PropertyName)
{
    const FObjectPropertyBase* Property = Object
        ? FindFProperty<FObjectPropertyBase>(Object->GetClass(), PropertyName)
        : nullptr;
    return Property ? Property->GetObjectPropertyValue_InContainer(Object) : nullptr;
}

TSharedPtr<FJsonObject> BuildObjectPropertySnapshotJson(const UObject* Object, int32 MaxProperties = 120)
{
    TSharedPtr<FJsonObject> SnapshotJson = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> PropertyValues;
    int32 PropertyCount = 0;
    const int32 SafeMaxProperties = FMath::Clamp(MaxProperties, 0, 500);

    if (Object)
    {
        for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            const FProperty* Property = *It;
            if (!Property)
            {
                continue;
            }

            ++PropertyCount;
            if (PropertyValues.Num() >= SafeMaxProperties)
            {
                continue;
            }

            FString ValueText;
            Property->ExportText_InContainer(0, ValueText, Object, Object, const_cast<UObject*>(Object), PPF_None);

            TSharedPtr<FJsonObject> PropertyJson = MakeShared<FJsonObject>();
            PropertyJson->SetStringField(TEXT("name"), Property->GetName());
            PropertyJson->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
            PropertyJson->SetStringField(TEXT("value"), ValueText);
            PropertyJson->SetBoolField(TEXT("is_transient"), Property->HasAnyPropertyFlags(CPF_Transient));
            PropertyJson->SetBoolField(TEXT("is_editable"), Property->HasAnyPropertyFlags(CPF_Edit));
            PropertyValues.Add(MakeShared<FJsonValueObject>(PropertyJson));
        }
    }

    SnapshotJson->SetNumberField(TEXT("property_count"), PropertyCount);
    SnapshotJson->SetBoolField(TEXT("properties_truncated"), PropertyCount > PropertyValues.Num());
    SnapshotJson->SetArrayField(TEXT("properties"), PropertyValues);
    return SnapshotJson;
}

TSharedPtr<FJsonObject> BuildDataInterfaceInspectionJson(const UNiagaraDataInterface* DataInterface, UNiagaraSystem* System)
{
    TSharedPtr<FJsonObject> DataInterfaceJson = BuildUObjectReferenceJson(DataInterface);
    DataInterfaceJson->SetBoolField(TEXT("is_data_interface"), DataInterface != nullptr);
    DataInterfaceJson->SetBoolField(TEXT("is_render_target_2d_data_interface"), DataInterface && DataInterface->IsA<UNiagaraDataInterfaceRenderTarget2D>());
    DataInterfaceJson->SetObjectField(TEXT("properties"), BuildObjectPropertySnapshotJson(DataInterface));

    const UNiagaraDataInterfaceRenderTarget2D* RenderTargetDataInterface = Cast<UNiagaraDataInterfaceRenderTarget2D>(DataInterface);
    if (RenderTargetDataInterface)
    {
        const FNiagaraVariable& UserParameter = RenderTargetDataInterface->RenderTargetUserParameter.Parameter;
        DataInterfaceJson->SetBoolField(TEXT("inherit_user_parameter_settings"), RenderTargetDataInterface->bInheritUserParameterSettings);
        DataInterfaceJson->SetObjectField(TEXT("render_target_user_parameter"), NiagaraVariableToJsonObject(UserParameter));
        DataInterfaceJson->SetStringField(TEXT("render_target_user_parameter_name"), UserParameter.GetName().ToString());
        DataInterfaceJson->SetBoolField(TEXT("has_render_target_user_parameter"), UserParameter.IsValid());

        const UObject* UserParameterObject = nullptr;
        if (System && UserParameter.IsValid())
        {
            UserParameterObject = System->GetExposedParameters().GetUObject(UserParameter).Get();
        }
        DataInterfaceJson->SetObjectField(TEXT("user_parameter_object"), BuildRenderTargetObjectJson(UserParameterObject));
    }
    return DataInterfaceJson;
}

TSharedPtr<FJsonObject> BuildNiagaraDataInterfaceOverrideJson(
    const UEdGraphPin* OverridePin,
    UNiagaraSystem* System)
{
    TSharedPtr<FJsonObject> OverrideJson = MakeShared<FJsonObject>();
    OverrideJson->SetBoolField(TEXT("has_override_pin"), OverridePin != nullptr);
    if (!OverridePin)
    {
        OverrideJson->SetNumberField(TEXT("link_count"), 0);
        return OverrideJson;
    }

    OverrideJson->SetStringField(TEXT("pin_name"), OverridePin->PinName.ToString());
    OverrideJson->SetStringField(TEXT("pin_type_category"), OverridePin->PinType.PinCategory.ToString());
    OverrideJson->SetStringField(TEXT("default_value"), OverridePin->DefaultValue);
    OverrideJson->SetStringField(TEXT("default_object"), NiagaraObjectPathOrEmpty(OverridePin->DefaultObject));
    OverrideJson->SetNumberField(TEXT("link_count"), OverridePin->LinkedTo.Num());

    if (OverridePin->LinkedTo.Num() != 1)
    {
        OverrideJson->SetBoolField(TEXT("has_single_linked_value"), false);
        return OverrideJson;
    }

    OverrideJson->SetBoolField(TEXT("has_single_linked_value"), true);

    const UEdGraphPin* LinkedPin = OverridePin->LinkedTo[0];
    const UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
    TSharedPtr<FJsonObject> LinkedNodeJson = MakeShared<FJsonObject>();
    LinkedNodeJson->SetBoolField(TEXT("is_valid"), LinkedNode != nullptr);
    if (LinkedNode)
    {
        LinkedNodeJson->SetStringField(TEXT("node_name"), LinkedNode->GetName());
        LinkedNodeJson->SetStringField(TEXT("node_guid"), LinkedNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
        LinkedNodeJson->SetStringField(TEXT("node_class"), LinkedNode->GetClass() ? LinkedNode->GetClass()->GetName() : FString());
        LinkedNodeJson->SetStringField(TEXT("node_class_path"), LinkedNode->GetClass() ? LinkedNode->GetClass()->GetPathName() : FString());
    }
    if (LinkedPin)
    {
        LinkedNodeJson->SetStringField(TEXT("linked_pin_name"), LinkedPin->PinName.ToString());
        LinkedNodeJson->SetStringField(TEXT("linked_pin_type_category"), LinkedPin->PinType.PinCategory.ToString());
        LinkedNodeJson->SetNumberField(TEXT("linked_pin_link_count"), LinkedPin->LinkedTo.Num());
    }
    OverrideJson->SetObjectField(TEXT("linked_node"), LinkedNodeJson);

    const UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(LinkedNode);
    OverrideJson->SetBoolField(TEXT("linked_node_is_input_node"), InputNode != nullptr);
    if (!InputNode)
    {
        return OverrideJson;
    }

    OverrideJson->SetObjectField(TEXT("input_node"), BuildInputNodeJson(InputNode));
    OverrideJson->SetObjectField(TEXT("object_asset"), BuildRenderTargetObjectJson(GetObjectPropertyForInspection(InputNode, TEXT("ObjectAsset"))));

    const UNiagaraDataInterface* DataInterface = Cast<UNiagaraDataInterface>(GetObjectPropertyForInspection(InputNode, TEXT("DataInterface")));
    OverrideJson->SetObjectField(TEXT("data_interface"), BuildDataInterfaceInspectionJson(DataInterface, System));
    return OverrideJson;
}

TSharedPtr<FJsonObject> BuildGraphAnalysisJson(
    const UNiagaraScriptSourceBase* SourceBase,
    const FString& Context,
    bool bIncludePins,
    int32 MaxFunctionCalls)
{
    TSharedPtr<FJsonObject> GraphObject = MakeShared<FJsonObject>();
    GraphObject->SetStringField(TEXT("context"), Context);
    GraphObject->SetStringField(TEXT("source_class"), SourceBase ? SourceBase->GetClass()->GetName() : FString());
    GraphObject->SetStringField(TEXT("source_path"), NiagaraObjectPathOrEmpty(SourceBase));

    const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase);
    UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
    GraphObject->SetStringField(TEXT("graph_path"), NiagaraObjectPathOrEmpty(Graph));
    GraphObject->SetBoolField(TEXT("has_graph"), Graph != nullptr);

    if (!Graph)
    {
        GraphObject->SetNumberField(TEXT("node_count"), 0);
        GraphObject->SetNumberField(TEXT("function_call_count"), 0);
        GraphObject->SetNumberField(TEXT("input_node_count"), 0);
        GraphObject->SetNumberField(TEXT("output_node_count"), 0);
        GraphObject->SetArrayField(TEXT("function_calls"), TArray<TSharedPtr<FJsonValue>>());
        GraphObject->SetArrayField(TEXT("input_nodes"), TArray<TSharedPtr<FJsonValue>>());
        GraphObject->SetArrayField(TEXT("output_nodes"), TArray<TSharedPtr<FJsonValue>>());
        return GraphObject;
    }

    TArray<UNiagaraNodeFunctionCall*> FunctionCalls;
    TArray<UNiagaraNodeInput*> InputNodes;
    TArray<UNiagaraNodeOutput*> OutputNodes;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node))
        {
            FunctionCalls.Add(FunctionCall);
        }
        else if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node))
        {
            InputNodes.Add(InputNode);
        }
        else if (UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(Node))
        {
            OutputNodes.Add(OutputNode);
        }
    }

    FunctionCalls.Sort(
        [](const UNiagaraNodeFunctionCall& Left, const UNiagaraNodeFunctionCall& Right)
        {
            if (Left.NodePosY == Right.NodePosY)
            {
                return Left.NodePosX < Right.NodePosX;
            }
            return Left.NodePosY < Right.NodePosY;
        });
    InputNodes.Sort(
        [](const UNiagaraNodeInput& Left, const UNiagaraNodeInput& Right)
        {
            return Left.CallSortPriority < Right.CallSortPriority;
        });

    TArray<TSharedPtr<FJsonValue>> FunctionValues;
    TArray<FString> ControlHints;
    const int32 SafeMaxFunctionCalls = FMath::Max(0, MaxFunctionCalls);
    for (int32 Index = 0; Index < FunctionCalls.Num() && Index < SafeMaxFunctionCalls; ++Index)
    {
        TSharedPtr<FJsonObject> FunctionObject = BuildFunctionCallJson(FunctionCalls[Index], Index, bIncludePins);
        for (const TSharedPtr<FJsonValue>& HintValue : FunctionObject->GetArrayField(TEXT("control_hints")))
        {
            FString Hint;
            if (HintValue->TryGetString(Hint))
            {
                AddUniqueString(ControlHints, Hint);
            }
        }
        FunctionValues.Add(MakeShared<FJsonValueObject>(FunctionObject));
    }

    TArray<TSharedPtr<FJsonValue>> InputValues;
    for (const UNiagaraNodeInput* InputNode : InputNodes)
    {
        TSharedPtr<FJsonObject> InputObject = BuildInputNodeJson(InputNode);
        for (const TSharedPtr<FJsonValue>& HintValue : InputObject->GetArrayField(TEXT("control_hints")))
        {
            FString Hint;
            if (HintValue->TryGetString(Hint))
            {
                AddUniqueString(ControlHints, Hint);
            }
        }
        InputValues.Add(MakeShared<FJsonValueObject>(InputObject));
    }

    TArray<TSharedPtr<FJsonValue>> OutputValues;
    for (const UNiagaraNodeOutput* OutputNode : OutputNodes)
    {
        OutputValues.Add(MakeShared<FJsonValueObject>(BuildOutputNodeJson(OutputNode)));
    }

    GraphObject->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
    GraphObject->SetNumberField(TEXT("function_call_count"), FunctionCalls.Num());
    GraphObject->SetNumberField(TEXT("input_node_count"), InputNodes.Num());
    GraphObject->SetNumberField(TEXT("output_node_count"), OutputNodes.Num());
    GraphObject->SetBoolField(TEXT("function_calls_truncated"), FunctionCalls.Num() > SafeMaxFunctionCalls);
    GraphObject->SetArrayField(TEXT("control_hints"), StringsToJson(ControlHints));
    GraphObject->SetArrayField(TEXT("function_calls"), FunctionValues);
    GraphObject->SetArrayField(TEXT("input_nodes"), InputValues);
    GraphObject->SetArrayField(TEXT("output_nodes"), OutputValues);
    return GraphObject;
}

TSharedPtr<FJsonObject> BuildScratchPadScriptJson(
    const UNiagaraScript* Script,
    const FString& OwnerKind,
    bool bIncludePins,
    int32 MaxFunctionCalls)
{
    TSharedPtr<FJsonObject> ScriptObject = MakeShared<FJsonObject>();
    if (!Script)
    {
        return ScriptObject;
    }

    ScriptObject->SetStringField(TEXT("name"), Script->GetName());
    ScriptObject->SetStringField(TEXT("path"), Script->GetPathName());
    ScriptObject->SetStringField(TEXT("owner_kind"), OwnerKind);
    ScriptObject->SetStringField(TEXT("usage"), NiagaraScriptUsageName(Script->GetUsage()));
    TArray<ENiagaraScriptUsage> SupportedUsageContexts;
    if (const FVersionedNiagaraScriptData* ScriptData = Script->GetLatestScriptData())
    {
        SupportedUsageContexts = ScriptData->GetSupportedUsageContexts();
    }
    ScriptObject->SetArrayField(TEXT("supported_usage_contexts"), NiagaraScriptUsagesToJson(SupportedUsageContexts));
    ScriptObject->SetObjectField(
        TEXT("graph"),
        BuildGraphAnalysisJson(Script->GetLatestSource(), FString::Printf(TEXT("%s_scratch_pad:%s"), *OwnerKind, *Script->GetName()), bIncludePins, MaxFunctionCalls));
    return ScriptObject;
}

void AppendScratchPadContainerScripts(
    const UNiagaraScratchPadContainer* Container,
    const FString& OwnerKind,
    bool bIncludePins,
    int32 MaxFunctionCalls,
    TArray<TSharedPtr<FJsonValue>>& OutScripts)
{
#if WITH_EDITORONLY_DATA
    if (!Container)
    {
        return;
    }

    for (const TObjectPtr<UNiagaraScript>& Script : Container->Scripts)
    {
        OutScripts.Add(MakeShared<FJsonValueObject>(BuildScratchPadScriptJson(Script.Get(), OwnerKind, bIncludePins, MaxFunctionCalls)));
    }
#endif
}

TSharedPtr<FJsonObject> BuildScratchPadInterfaceJson(
    const UNiagaraScript* Script,
    const FString& OwnerKind,
    const FString& OwnerName,
    int32 OwnerEmitterIndex,
    const FString& ContainerKind,
    int32 ScriptIndex,
    bool bIncludeGraphSummary,
    int32 MaxFunctionCalls)
{
    TSharedPtr<FJsonObject> ScriptObject = MakeShared<FJsonObject>();
    if (!Script)
    {
        return ScriptObject;
    }

    TArray<ENiagaraScriptUsage> SupportedUsageContexts;
    if (const FVersionedNiagaraScriptData* ScriptData = Script->GetLatestScriptData())
    {
        SupportedUsageContexts = ScriptData->GetSupportedUsageContexts();
    }

    const FString Context = FString::Printf(
        TEXT("%s:%s:%s"),
        *OwnerKind,
        *ContainerKind,
        *Script->GetName());
    TSharedPtr<FJsonObject> GraphObject = BuildGraphAnalysisJson(
        Script->GetLatestSource(),
        Context,
        false,
        MaxFunctionCalls);

    ScriptObject->SetStringField(TEXT("name"), Script->GetName());
    ScriptObject->SetStringField(TEXT("path"), Script->GetPathName());
    ScriptObject->SetStringField(TEXT("owner_kind"), OwnerKind);
    ScriptObject->SetStringField(TEXT("owner_name"), OwnerName);
    ScriptObject->SetNumberField(TEXT("owner_emitter_index"), OwnerEmitterIndex);
    ScriptObject->SetStringField(TEXT("container_kind"), ContainerKind);
    ScriptObject->SetNumberField(TEXT("script_index"), ScriptIndex);
    ScriptObject->SetStringField(TEXT("usage"), NiagaraScriptUsageName(Script->GetUsage()));
    ScriptObject->SetArrayField(TEXT("supported_usage_contexts"), NiagaraScriptUsagesToJson(SupportedUsageContexts));
    ScriptObject->SetBoolField(TEXT("has_graph"), GraphObject->GetBoolField(TEXT("has_graph")));
    ScriptObject->SetNumberField(TEXT("node_count"), GraphObject->GetNumberField(TEXT("node_count")));
    ScriptObject->SetNumberField(TEXT("function_call_count"), GraphObject->GetNumberField(TEXT("function_call_count")));
    ScriptObject->SetNumberField(TEXT("input_count"), GraphObject->GetNumberField(TEXT("input_node_count")));
    ScriptObject->SetNumberField(TEXT("output_count"), GraphObject->GetNumberField(TEXT("output_node_count")));
    const TArray<TSharedPtr<FJsonValue>>* ControlHints = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
    ScriptObject->SetArrayField(TEXT("control_hints"), GraphObject->TryGetArrayField(TEXT("control_hints"), ControlHints) ? *ControlHints : TArray<TSharedPtr<FJsonValue>>());
    ScriptObject->SetArrayField(TEXT("inputs"), GraphObject->TryGetArrayField(TEXT("input_nodes"), Inputs) ? *Inputs : TArray<TSharedPtr<FJsonValue>>());
    ScriptObject->SetArrayField(TEXT("outputs"), GraphObject->TryGetArrayField(TEXT("output_nodes"), Outputs) ? *Outputs : TArray<TSharedPtr<FJsonValue>>());
    ScriptObject->SetBoolField(TEXT("read_only"), true);
    ScriptObject->SetStringField(TEXT("authoring_status"), TEXT("interface_inspection_only"));
    if (bIncludeGraphSummary)
    {
        ScriptObject->SetObjectField(TEXT("graph_summary"), GraphObject);
    }
    return ScriptObject;
}
}

FUnrealMCPNiagaraCommands::FUnrealMCPNiagaraCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("analyze_niagara_system"))
    {
        return HandleAnalyzeNiagaraSystem(Params);
    }
    if (CommandType == TEXT("inspect_niagara_renderers"))
    {
        return HandleInspectNiagaraRenderers(Params);
    }
    if (CommandType == TEXT("set_niagara_renderer_material"))
    {
        return HandleSetNiagaraRendererMaterial(Params);
    }
    if (CommandType == TEXT("inspect_niagara_user_parameters"))
    {
        return HandleInspectNiagaraUserParameters(Params);
    }
    if (CommandType == TEXT("set_niagara_user_parameter"))
    {
        return HandleSetNiagaraUserParameter(Params);
    }
    if (CommandType == TEXT("inspect_niagara_stack"))
    {
        return HandleInspectNiagaraStack(Params);
    }
    if (CommandType == TEXT("inspect_niagara_graph"))
    {
        return HandleInspectNiagaraGraph(Params);
    }
    if (CommandType == TEXT("inspect_niagara_compile_status"))
    {
        return HandleInspectNiagaraCompileStatus(Params);
    }
    if (CommandType == TEXT("inspect_niagara_simulation_stages"))
    {
        return HandleInspectNiagaraSimulationStages(Params);
    }
    if (CommandType == TEXT("inspect_niagara_scratch_pad_interface"))
    {
        return HandleInspectNiagaraScratchPadInterface(Params);
    }
    if (CommandType == TEXT("duplicate_or_attach_emitter_from_source"))
    {
        return HandleDuplicateOrAttachEmitterFromSource(Params);
    }
    if (CommandType == TEXT("create_or_duplicate_scratch_pad_module"))
    {
        return HandleCreateOrDuplicateScratchPadModule(Params);
    }
    if (CommandType == TEXT("add_scratch_pad_module_to_stack"))
    {
        return HandleAddScratchPadModuleToStack(Params);
    }
    if (CommandType == TEXT("inspect_niagara_module_inputs"))
    {
        return HandleInspectNiagaraModuleInputs(Params);
    }
    if (CommandType == TEXT("inspect_niagara_data_interface_overrides"))
    {
        return HandleInspectNiagaraDataInterfaceOverrides(Params);
    }
    if (CommandType == TEXT("create_niagara_module_input_override"))
    {
        return HandleCreateNiagaraModuleInputOverride(Params);
    }
    if (CommandType == TEXT("set_niagara_render_target2d_module_input"))
    {
        return HandleSetNiagaraRenderTarget2DModuleInput(Params);
    }
    if (CommandType == TEXT("set_niagara_module_inputs_batch"))
    {
        return HandleSetNiagaraModuleInputsBatch(Params);
    }
    if (CommandType == TEXT("set_niagara_module_input_value"))
    {
        return HandleSetNiagaraModuleInputValue(Params);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown Niagara command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleAnalyzeNiagaraSystem(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    bool bIncludeRenderers = true;
    bool bIncludeUserParameters = true;
    bool bIncludeStack = true;
    bool bIncludeGraph = true;
    bool bIncludeModuleInputs = true;
    bool bIncludeCompileStatus = true;
    Params->TryGetBoolField(TEXT("include_renderers"), bIncludeRenderers);
    Params->TryGetBoolField(TEXT("include_user_parameters"), bIncludeUserParameters);
    Params->TryGetBoolField(TEXT("include_stack"), bIncludeStack);
    Params->TryGetBoolField(TEXT("include_graph"), bIncludeGraph);
    Params->TryGetBoolField(TEXT("include_module_inputs"), bIncludeModuleInputs);
    Params->TryGetBoolField(TEXT("include_compile_status"), bIncludeCompileStatus);

    bool bIncludePins = false;
    bool bIncludeLinks = false;
    bool bIncludeScratchPads = true;
    bool bIncludeResolvedStackInputs = false;
    Params->TryGetBoolField(TEXT("include_pins"), bIncludePins);
    Params->TryGetBoolField(TEXT("include_links"), bIncludeLinks);
    Params->TryGetBoolField(TEXT("include_scratch_pads"), bIncludeScratchPads);
    Params->TryGetBoolField(TEXT("include_resolved_stack_inputs"), bIncludeResolvedStackInputs);

    int32 MaxFunctionCalls = 200;
    int32 MaxNodesPerGraph = 300;
    int32 MaxLinksPerGraph = 0;
    int32 MaxModules = 200;
    int32 MaxCandidatesPerModule = 24;
    int32 MaxResolvedInputsPerModule = 8;
    int32 MaxTopCandidates = 80;
    Params->TryGetNumberField(TEXT("max_function_calls"), MaxFunctionCalls);
    Params->TryGetNumberField(TEXT("max_nodes_per_graph"), MaxNodesPerGraph);
    Params->TryGetNumberField(TEXT("max_links_per_graph"), MaxLinksPerGraph);
    Params->TryGetNumberField(TEXT("max_modules"), MaxModules);
    Params->TryGetNumberField(TEXT("max_candidates_per_module"), MaxCandidatesPerModule);
    Params->TryGetNumberField(TEXT("max_resolved_inputs_per_module"), MaxResolvedInputsPerModule);
    Params->TryGetNumberField(TEXT("max_top_candidates"), MaxTopCandidates);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetBoolField(TEXT("read_only"), true);
    Result->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
    Result->SetStringField(TEXT("source_policy"), TEXT("read_only_analysis_no_save_no_compile_request"));

    TSharedPtr<FJsonObject> SectionStatus = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Limitations;

    auto AddSection = [&Result, &SectionStatus, &Limitations](const FString& SectionName, const TSharedPtr<FJsonObject>& SectionResult)
    {
        const bool bSectionSucceeded = SectionResult.IsValid() && SectionResult->GetBoolField(TEXT("success"));
        SectionStatus->SetBoolField(SectionName, bSectionSucceeded);
        if (SectionResult.IsValid())
        {
            Result->SetObjectField(SectionName, SectionResult);
            if (!bSectionSucceeded)
            {
                FString Error;
                SectionResult->TryGetStringField(TEXT("error"), Error);
                if (!Error.IsEmpty())
                {
                    Limitations.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: %s"), *SectionName, *Error)));
                }
            }
        }
        else
        {
            Limitations.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: no result"), *SectionName)));
        }
    };

    TSharedPtr<FJsonObject> BaseParams = MakeShared<FJsonObject>();
    BaseParams->SetStringField(TEXT("system_path"), System->GetPathName());

    if (bIncludeRenderers)
    {
        TSharedPtr<FJsonObject> RendererResult = HandleInspectNiagaraRenderers(BaseParams);
        AddSection(TEXT("renderers"), RendererResult);
        if (RendererResult.IsValid() && RendererResult->GetBoolField(TEXT("success")))
        {
            Summary->SetNumberField(TEXT("renderer_count"), RendererResult->GetNumberField(TEXT("renderer_count")));
        }
    }
    if (bIncludeUserParameters)
    {
        TSharedPtr<FJsonObject> UserResult = HandleInspectNiagaraUserParameters(BaseParams);
        AddSection(TEXT("user_parameters"), UserResult);
        if (UserResult.IsValid() && UserResult->GetBoolField(TEXT("success")))
        {
            Summary->SetNumberField(TEXT("user_parameter_count"), UserResult->GetNumberField(TEXT("parameter_count")));
            Summary->SetNumberField(TEXT("settable_user_parameter_count"), UserResult->GetNumberField(TEXT("settable_count")));
        }
    }
    if (bIncludeStack)
    {
        TSharedPtr<FJsonObject> StackParams = MakeShared<FJsonObject>();
        StackParams->SetStringField(TEXT("system_path"), System->GetPathName());
        StackParams->SetBoolField(TEXT("include_pins"), bIncludePins);
        StackParams->SetNumberField(TEXT("max_function_calls"), MaxFunctionCalls);
        TSharedPtr<FJsonObject> StackResult = HandleInspectNiagaraStack(StackParams);
        AddSection(TEXT("stack"), StackResult);
        if (StackResult.IsValid() && StackResult->GetBoolField(TEXT("success")))
        {
            Summary->SetNumberField(TEXT("stack_function_call_count"), StackResult->GetNumberField(TEXT("total_emitter_function_call_count")));
            Summary->SetNumberField(TEXT("stack_scratch_pad_count"), StackResult->GetNumberField(TEXT("total_scratch_pad_count")));
        }
    }
    if (bIncludeGraph)
    {
        TSharedPtr<FJsonObject> GraphParams = MakeShared<FJsonObject>();
        GraphParams->SetStringField(TEXT("system_path"), System->GetPathName());
        GraphParams->SetBoolField(TEXT("include_pins"), bIncludePins);
        GraphParams->SetBoolField(TEXT("include_links"), bIncludeLinks);
        GraphParams->SetBoolField(TEXT("include_scratch_pads"), bIncludeScratchPads);
        GraphParams->SetNumberField(TEXT("max_nodes_per_graph"), MaxNodesPerGraph);
        GraphParams->SetNumberField(TEXT("max_links_per_graph"), MaxLinksPerGraph);
        TSharedPtr<FJsonObject> GraphResult = HandleInspectNiagaraGraph(GraphParams);
        AddSection(TEXT("graph"), GraphResult);
        if (GraphResult.IsValid() && GraphResult->GetBoolField(TEXT("success")))
        {
            Summary->SetNumberField(TEXT("graph_count"), GraphResult->GetNumberField(TEXT("total_graph_count")));
            Summary->SetNumberField(TEXT("graph_node_count"), GraphResult->GetNumberField(TEXT("total_node_count")));
            Summary->SetNumberField(TEXT("graph_link_count"), GraphResult->GetNumberField(TEXT("total_link_count")));
            Summary->SetNumberField(TEXT("graph_scratch_pad_count"), GraphResult->GetNumberField(TEXT("total_scratch_pad_count")));
        }
    }
    if (bIncludeModuleInputs)
    {
        TSharedPtr<FJsonObject> ModuleParams = MakeShared<FJsonObject>();
        ModuleParams->SetStringField(TEXT("system_path"), System->GetPathName());
        ModuleParams->SetBoolField(TEXT("include_linked_sources"), true);
        ModuleParams->SetBoolField(TEXT("include_resolved_stack_inputs"), bIncludeResolvedStackInputs);
        ModuleParams->SetNumberField(TEXT("max_modules"), MaxModules);
        ModuleParams->SetNumberField(TEXT("max_candidates_per_module"), MaxCandidatesPerModule);
        ModuleParams->SetNumberField(TEXT("max_resolved_inputs_per_module"), MaxResolvedInputsPerModule);
        ModuleParams->SetNumberField(TEXT("max_top_candidates"), MaxTopCandidates);
        TSharedPtr<FJsonObject> ModuleResult = HandleInspectNiagaraModuleInputs(ModuleParams);
        AddSection(TEXT("module_inputs"), ModuleResult);
        if (ModuleResult.IsValid() && ModuleResult->GetBoolField(TEXT("success")))
        {
            Summary->SetNumberField(TEXT("module_count"), ModuleResult->GetNumberField(TEXT("module_count")));
            Summary->SetNumberField(TEXT("module_input_candidate_count"), ModuleResult->GetNumberField(TEXT("candidate_count")));
        }
    }
    if (bIncludeCompileStatus)
    {
        TSharedPtr<FJsonObject> CompileParams = MakeShared<FJsonObject>();
        CompileParams->SetStringField(TEXT("system_path"), System->GetPathName());
        CompileParams->SetBoolField(TEXT("request_compile"), false);
        CompileParams->SetBoolField(TEXT("force"), false);
        CompileParams->SetBoolField(TEXT("allow_source_compile"), false);
        CompileParams->SetBoolField(TEXT("wait_for_completion"), false);
        TSharedPtr<FJsonObject> CompileResult = HandleInspectNiagaraCompileStatus(CompileParams);
        AddSection(TEXT("compile_status"), CompileResult);
        if (CompileResult.IsValid() && CompileResult->GetBoolField(TEXT("success")))
        {
            Summary->SetNumberField(TEXT("compile_script_count"), CompileResult->GetNumberField(TEXT("script_count")));
            Summary->SetNumberField(TEXT("compile_error_count"), CompileResult->GetNumberField(TEXT("error_count")));
            Summary->SetNumberField(TEXT("compile_warning_count"), CompileResult->GetNumberField(TEXT("warning_count")));
            Summary->SetNumberField(TEXT("compile_dirty_count"), CompileResult->GetNumberField(TEXT("dirty_count")));
            Summary->SetBoolField(TEXT("compile_outstanding"), CompileResult->GetBoolField(TEXT("outstanding_compilation_requests_after")));
        }
    }

    Result->SetObjectField(TEXT("section_status"), SectionStatus);
    Result->SetObjectField(TEXT("summary"), Summary);
    Result->SetArrayField(TEXT("limitations"), Limitations);
    Result->SetBoolField(TEXT("complete"), Limitations.Num() == 0);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::BuildRendererJson(
    const UNiagaraSystem* System,
    const FString& EmitterName,
    int32 EmitterIndex,
    const UNiagaraRendererProperties* Renderer,
    int32 RendererIndex) const
{
    TSharedPtr<FJsonObject> RendererObject = MakeShared<FJsonObject>();
    RendererObject->SetStringField(TEXT("system_path"), System ? System->GetPathName() : FString());
    RendererObject->SetStringField(TEXT("emitter_name"), EmitterName);
    RendererObject->SetNumberField(TEXT("emitter_index"), EmitterIndex);
    RendererObject->SetNumberField(TEXT("renderer_index"), RendererIndex);
    RendererObject->SetStringField(TEXT("renderer_class"), Renderer ? Renderer->GetClass()->GetName() : FString());
    RendererObject->SetStringField(TEXT("renderer_name"), Renderer ? Renderer->GetName() : FString());
    RendererObject->SetBoolField(TEXT("enabled"), Renderer ? Renderer->GetIsEnabled() : false);
    RendererObject->SetStringField(TEXT("material_field"), Renderer ? RendererMaterialField(Renderer) : FString());

    TArray<UMaterialInterface*> UsedMaterials;
    if (Renderer)
    {
        Renderer->GetUsedMaterials(nullptr, UsedMaterials);
    }
    RendererObject->SetArrayField(TEXT("used_materials"), MaterialArrayToJson(UsedMaterials));

    if (const UNiagaraSpriteRendererProperties* SpriteRenderer = Cast<UNiagaraSpriteRendererProperties>(Renderer))
    {
        RendererObject->SetStringField(TEXT("primary_material"), NiagaraObjectPathOrEmpty(SpriteRenderer->Material));
    }
    else if (const UNiagaraRibbonRendererProperties* RibbonRenderer = Cast<UNiagaraRibbonRendererProperties>(Renderer))
    {
        RendererObject->SetStringField(TEXT("primary_material"), NiagaraObjectPathOrEmpty(RibbonRenderer->Material));
    }
    else if (const UNiagaraMeshRendererProperties* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(Renderer))
    {
        TArray<TSharedPtr<FJsonValue>> OverrideValues;
        for (int32 OverrideIndex = 0; OverrideIndex < MeshRenderer->OverrideMaterials.Num(); ++OverrideIndex)
        {
            TSharedPtr<FJsonObject> OverrideObject = MakeShared<FJsonObject>();
            OverrideObject->SetNumberField(TEXT("slot_index"), OverrideIndex);
            OverrideObject->SetStringField(TEXT("explicit_material"), NiagaraObjectPathOrEmpty(MeshRenderer->OverrideMaterials[OverrideIndex].ExplicitMat));
            OverrideValues.Add(MakeShared<FJsonValueObject>(OverrideObject));
        }
        RendererObject->SetBoolField(TEXT("override_materials_enabled"), MeshRenderer->bOverrideMaterials != 0);
        RendererObject->SetArrayField(TEXT("override_materials"), OverrideValues);
    }
    else if (const UNiagaraDecalRendererProperties* DecalRenderer = Cast<UNiagaraDecalRendererProperties>(Renderer))
    {
        RendererObject->SetStringField(TEXT("primary_material"), NiagaraObjectPathOrEmpty(DecalRenderer->Material));
    }
    else if (const UNiagaraVolumeRendererProperties* VolumeRenderer = Cast<UNiagaraVolumeRendererProperties>(Renderer))
    {
        RendererObject->SetStringField(TEXT("primary_material"), NiagaraObjectPathOrEmpty(VolumeRenderer->Material));
    }

    return RendererObject;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::BuildUserParameterJson(const UNiagaraSystem* System, const FNiagaraVariable& Parameter) const
{
    const FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
    TSharedPtr<FJsonObject> ParameterObject = MakeShared<FJsonObject>();
    ParameterObject->SetStringField(TEXT("name"), Parameter.GetName().ToString());
    ParameterObject->SetStringField(TEXT("type"), NiagaraTypeName(Parameter.GetType()));
    ParameterObject->SetNumberField(TEXT("size_bytes"), Parameter.GetSizeInBytes());
    ParameterObject->SetBoolField(TEXT("settable_by_mcp"), IsSettableNiagaraUserParameterType(Parameter.GetType()));
    ParameterObject->SetField(TEXT("value"), NiagaraParameterValueToJson(Store, Parameter));
    return ParameterObject;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleInspectNiagaraRenderers(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    TArray<TSharedPtr<FJsonValue>> RendererValues;
    int32 EmitterIndex = 0;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        const FString EmitterName = EmitterHandle.GetName().ToString();
        EmitterHandle.ForEachEnabledRendererWithIndex(
            [this, System, EmitterName, EmitterIndex, &RendererValues](const UNiagaraRendererProperties* Renderer, int32 RendererIndex)
            {
                RendererValues.Add(MakeShared<FJsonValueObject>(BuildRendererJson(System, EmitterName, EmitterIndex, Renderer, RendererIndex)));
            });
        ++EmitterIndex;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
    Result->SetNumberField(TEXT("renderer_count"), RendererValues.Num());
    Result->SetArrayField(TEXT("renderers"), RendererValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraRendererMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString MaterialPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_path' parameter"));
    }

    bool bAllowSourceEdit = false;
    Params->TryGetBoolField(TEXT("allow_source_edit"), bAllowSourceEdit);
    if (!bAllowSourceEdit && !IsTempGeneratedNiagaraPath(SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Refusing to edit Niagara system outside %s: %s"), NiagaraTempGenerationRoot, *SystemPath));
    }

    int32 TargetEmitterIndex = INDEX_NONE;
    int32 TargetRendererIndex = INDEX_NONE;
    int32 MaterialSlotIndex = 0;
    FString TargetEmitterName;
    Params->TryGetNumberField(TEXT("emitter_index"), TargetEmitterIndex);
    Params->TryGetNumberField(TEXT("renderer_index"), TargetRendererIndex);
    Params->TryGetNumberField(TEXT("material_slot_index"), MaterialSlotIndex);
    Params->TryGetStringField(TEXT("emitter_name"), TargetEmitterName);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *NormalizeNiagaraObjectPathForLoad(MaterialPath));
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load material: %s"), *MaterialPath));
    }

    UNiagaraRendererProperties* MatchedRenderer = nullptr;
    int32 MatchedEmitterIndex = INDEX_NONE;
    int32 MatchedRendererIndex = INDEX_NONE;
    FString MatchedEmitterName;

    int32 CurrentEmitterIndex = 0;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        const FString EmitterName = EmitterHandle.GetName().ToString();
        const bool bEmitterMatches =
            (TargetEmitterIndex == INDEX_NONE || TargetEmitterIndex == CurrentEmitterIndex) &&
            (TargetEmitterName.IsEmpty() || TargetEmitterName.Equals(EmitterName, ESearchCase::IgnoreCase));

        if (bEmitterMatches)
        {
            if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
            {
                EmitterData->ForEachEnabledRendererWithIndex(
                    [&MatchedRenderer, &MatchedEmitterIndex, &MatchedRendererIndex, &MatchedEmitterName, CurrentEmitterIndex, EmitterName, TargetRendererIndex](UNiagaraRendererProperties* Renderer, int32 RendererIndex)
                    {
                        if (MatchedRenderer == nullptr && (TargetRendererIndex == INDEX_NONE || TargetRendererIndex == RendererIndex))
                        {
                            MatchedRenderer = Renderer;
                            MatchedEmitterIndex = CurrentEmitterIndex;
                            MatchedRendererIndex = RendererIndex;
                            MatchedEmitterName = EmitterName;
                        }
                    });
            }
        }

        ++CurrentEmitterIndex;
    }

    if (!MatchedRenderer)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No matching enabled Niagara renderer was found"));
    }

    FString ChangedField;
    FString ErrorMessage;
    if (!SetRendererMaterial(MatchedRenderer, Material, MaterialSlotIndex, ChangedField, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    System->Modify();
    System->MarkPackageDirty();
    System->RequestCompile(false);

    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveLoadedAsset(System, false);
        if (!bSaved)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to save Niagara system after renderer material update"));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetStringField(TEXT("material_path"), Material->GetPathName());
    Result->SetStringField(TEXT("emitter_name"), MatchedEmitterName);
    Result->SetNumberField(TEXT("emitter_index"), MatchedEmitterIndex);
    Result->SetNumberField(TEXT("renderer_index"), MatchedRendererIndex);
    Result->SetStringField(TEXT("renderer_class"), MatchedRenderer->GetClass()->GetName());
    Result->SetStringField(TEXT("changed_field"), ChangedField);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleInspectNiagaraUserParameters(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    TArray<FNiagaraVariable> UserParameters;
    System->GetExposedParameters().GetUserParameters(UserParameters);

    TArray<TSharedPtr<FJsonValue>> ParameterValues;
    int32 SettableCount = 0;
    for (const FNiagaraVariable& Parameter : UserParameters)
    {
        if (IsSettableNiagaraUserParameterType(Parameter.GetType()))
        {
            ++SettableCount;
        }
        ParameterValues.Add(MakeShared<FJsonValueObject>(BuildUserParameterJson(System, Parameter)));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetNumberField(TEXT("parameter_count"), ParameterValues.Num());
    Result->SetNumberField(TEXT("settable_count"), SettableCount);
    Result->SetArrayField(TEXT("parameters"), ParameterValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleInspectNiagaraStack(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }

    bool bIncludePins = false;
    Params->TryGetBoolField(TEXT("include_pins"), bIncludePins);

    int32 MaxFunctionCalls = 200;
    Params->TryGetNumberField(TEXT("max_function_calls"), MaxFunctionCalls);
    MaxFunctionCalls = FMath::Clamp(MaxFunctionCalls, 0, 1000);

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    TArray<TSharedPtr<FJsonValue>> SystemScriptValues;
    if (const UNiagaraScript* SystemSpawnScript = System->GetSystemSpawnScript())
    {
        TSharedPtr<FJsonObject> ScriptObject = MakeShared<FJsonObject>();
        ScriptObject->SetStringField(TEXT("name"), SystemSpawnScript->GetName());
        ScriptObject->SetStringField(TEXT("path"), SystemSpawnScript->GetPathName());
        ScriptObject->SetStringField(TEXT("usage"), NiagaraScriptUsageName(SystemSpawnScript->GetUsage()));
        ScriptObject->SetObjectField(TEXT("graph"), BuildGraphAnalysisJson(SystemSpawnScript->GetLatestSource(), TEXT("system_spawn"), bIncludePins, MaxFunctionCalls));
        SystemScriptValues.Add(MakeShared<FJsonValueObject>(ScriptObject));
    }
    if (const UNiagaraScript* SystemUpdateScript = System->GetSystemUpdateScript())
    {
        TSharedPtr<FJsonObject> ScriptObject = MakeShared<FJsonObject>();
        ScriptObject->SetStringField(TEXT("name"), SystemUpdateScript->GetName());
        ScriptObject->SetStringField(TEXT("path"), SystemUpdateScript->GetPathName());
        ScriptObject->SetStringField(TEXT("usage"), NiagaraScriptUsageName(SystemUpdateScript->GetUsage()));
        ScriptObject->SetObjectField(TEXT("graph"), BuildGraphAnalysisJson(SystemUpdateScript->GetLatestSource(), TEXT("system_update"), bIncludePins, MaxFunctionCalls));
        SystemScriptValues.Add(MakeShared<FJsonValueObject>(ScriptObject));
    }

    TArray<TSharedPtr<FJsonValue>> SystemScratchPadValues;
#if WITH_EDITORONLY_DATA
    for (const TObjectPtr<UNiagaraScript>& ScratchPadScript : System->ScratchPadScripts)
    {
        SystemScratchPadValues.Add(MakeShared<FJsonValueObject>(BuildScratchPadScriptJson(ScratchPadScript.Get(), TEXT("system"), bIncludePins, MaxFunctionCalls)));
    }
#endif

    TArray<TSharedPtr<FJsonValue>> EmitterValues;
    int32 TotalFunctionCallCount = 0;
    int32 TotalScratchPadCount = SystemScratchPadValues.Num();
    int32 EmitterIndex = 0;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        TSharedPtr<FJsonObject> EmitterObject = MakeShared<FJsonObject>();
        EmitterObject->SetStringField(TEXT("name"), EmitterHandle.GetName().ToString());
        EmitterObject->SetNumberField(TEXT("emitter_index"), EmitterIndex);
        EmitterObject->SetBoolField(TEXT("enabled"), EmitterHandle.GetIsEnabled());

        TArray<TSharedPtr<FJsonValue>> ScriptValues;
        TArray<TSharedPtr<FJsonValue>> ScratchPadValues;
        TArray<TSharedPtr<FJsonValue>> ParentScratchPadValues;

        if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
        {
            TSharedPtr<FJsonObject> GraphObject = BuildGraphAnalysisJson(
                EmitterData->GraphSource,
                FString::Printf(TEXT("emitter:%s"), *EmitterHandle.GetName().ToString()),
                bIncludePins,
                MaxFunctionCalls);
            EmitterObject->SetObjectField(TEXT("graph"), GraphObject);
            TotalFunctionCallCount += static_cast<int32>(GraphObject->GetNumberField(TEXT("function_call_count")));

            TArray<UNiagaraScript*> EmitterScripts;
            EmitterData->GetScripts(EmitterScripts, false, false);
            for (const UNiagaraScript* Script : EmitterScripts)
            {
                if (!Script)
                {
                    continue;
                }
                TSharedPtr<FJsonObject> ScriptObject = MakeShared<FJsonObject>();
                ScriptObject->SetStringField(TEXT("name"), Script->GetName());
                ScriptObject->SetStringField(TEXT("path"), Script->GetPathName());
                ScriptObject->SetStringField(TEXT("usage"), NiagaraScriptUsageName(Script->GetUsage()));
                ScriptValues.Add(MakeShared<FJsonValueObject>(ScriptObject));
            }

#if WITH_EDITORONLY_DATA
            AppendScratchPadContainerScripts(EmitterData->ScratchPads, TEXT("emitter"), bIncludePins, MaxFunctionCalls, ScratchPadValues);
            AppendScratchPadContainerScripts(EmitterData->ParentScratchPads, TEXT("parent_emitter"), bIncludePins, MaxFunctionCalls, ParentScratchPadValues);
#endif
        }
        else
        {
            EmitterObject->SetObjectField(TEXT("graph"), BuildGraphAnalysisJson(nullptr, FString::Printf(TEXT("emitter:%s"), *EmitterHandle.GetName().ToString()), bIncludePins, MaxFunctionCalls));
        }

        TotalScratchPadCount += ScratchPadValues.Num() + ParentScratchPadValues.Num();
        EmitterObject->SetArrayField(TEXT("scripts"), ScriptValues);
        EmitterObject->SetArrayField(TEXT("scratch_pad_scripts"), ScratchPadValues);
        EmitterObject->SetArrayField(TEXT("parent_scratch_pad_scripts"), ParentScratchPadValues);
        EmitterValues.Add(MakeShared<FJsonValueObject>(EmitterObject));
        ++EmitterIndex;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetBoolField(TEXT("include_pins"), bIncludePins);
    Result->SetNumberField(TEXT("max_function_calls"), MaxFunctionCalls);
    Result->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
    Result->SetNumberField(TEXT("system_script_count"), SystemScriptValues.Num());
    Result->SetNumberField(TEXT("system_scratch_pad_count"), SystemScratchPadValues.Num());
    Result->SetNumberField(TEXT("total_scratch_pad_count"), TotalScratchPadCount);
    Result->SetNumberField(TEXT("total_emitter_function_call_count"), TotalFunctionCallCount);
    Result->SetArrayField(TEXT("system_scripts"), SystemScriptValues);
    Result->SetArrayField(TEXT("system_scratch_pad_scripts"), SystemScratchPadValues);
    Result->SetArrayField(TEXT("emitters"), EmitterValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleInspectNiagaraCompileStatus(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }

    bool bRequestCompile = false;
    Params->TryGetBoolField(TEXT("request_compile"), bRequestCompile);

    bool bForce = false;
    Params->TryGetBoolField(TEXT("force"), bForce);

    bool bAllowSourceCompile = false;
    Params->TryGetBoolField(TEXT("allow_source_compile"), bAllowSourceCompile);

    bool bWaitForCompletion = false;
    Params->TryGetBoolField(TEXT("wait_for_completion"), bWaitForCompletion);

    double TimeoutSeconds = 10.0;
    Params->TryGetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
    TimeoutSeconds = FMath::Clamp(TimeoutSeconds, 0.0, 60.0);

    double PollIntervalSeconds = 0.1;
    Params->TryGetNumberField(TEXT("poll_interval_seconds"), PollIntervalSeconds);
    PollIntervalSeconds = FMath::Clamp(PollIntervalSeconds, 0.02, 1.0);

    if (bRequestCompile && !bAllowSourceCompile && !IsTempGeneratedNiagaraPath(SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Refusing to request Niagara compile outside %s: %s"), NiagaraTempGenerationRoot, *SystemPath));
    }

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    const bool bOutstandingBefore = System->HasOutstandingCompilationRequests(true);
    bool bCompileRequested = false;
    if (bRequestCompile)
    {
        bCompileRequested = System->RequestCompile(bForce);
    }
    const bool bOutstandingAfterRequest = System->HasOutstandingCompilationRequests(true);

    bool bWaitTimedOut = false;
    int32 WaitIterations = 0;
    const double WaitStartSeconds = FPlatformTime::Seconds();
    if (bWaitForCompletion)
    {
        const double WaitDeadlineSeconds = WaitStartSeconds + TimeoutSeconds;
        while (System->HasOutstandingCompilationRequests(true) || System->HasActiveCompilations())
        {
            System->PollForCompilationComplete();
            FAssetCompilingManager::Get().ProcessAsyncTasks(true);
            if (!System->HasOutstandingCompilationRequests(true) && !System->HasActiveCompilations())
            {
                break;
            }
            if (FPlatformTime::Seconds() >= WaitDeadlineSeconds)
            {
                bWaitTimedOut = true;
                break;
            }
            ++WaitIterations;
            FPlatformProcess::Sleep(PollIntervalSeconds);
        }
    }
    const double WaitElapsedSeconds = FPlatformTime::Seconds() - WaitStartSeconds;
    const bool bOutstandingAfter = System->HasOutstandingCompilationRequests(true);

    TArray<TSharedPtr<FJsonValue>> ScriptValues;
    int32 ScriptIndex = 0;
    int32 ErrorCount = 0;
    int32 WarningCount = 0;
    int32 DirtyCount = 0;
    int32 UnknownCount = 0;
    int32 MissingCount = 0;

    auto AddScriptStatus = [&ScriptValues, &ScriptIndex, &ErrorCount, &WarningCount, &DirtyCount, &UnknownCount, &MissingCount](
        const UNiagaraScript* Script,
        const FString& OwnerKind,
        const FString& OwnerName)
    {
        TSharedPtr<FJsonObject> ScriptObject = BuildNiagaraScriptCompileStatusJson(Script, OwnerKind, OwnerName, ScriptIndex++);
        const FString CompileStatus = ScriptObject->GetStringField(TEXT("compile_status"));
        if (ScriptObject->GetBoolField(TEXT("has_error")))
        {
            ++ErrorCount;
        }
        if (ScriptObject->GetBoolField(TEXT("has_warning")))
        {
            ++WarningCount;
        }
        if (CompileStatus == TEXT("NCS_Dirty"))
        {
            ++DirtyCount;
        }
        else if (CompileStatus == TEXT("NCS_Unknown"))
        {
            ++UnknownCount;
        }
        else if (CompileStatus == TEXT("missing"))
        {
            ++MissingCount;
        }
        ScriptValues.Add(MakeShared<FJsonValueObject>(ScriptObject));
    };

    AddScriptStatus(System->GetSystemSpawnScript(), TEXT("system"), TEXT("system_spawn"));
    AddScriptStatus(System->GetSystemUpdateScript(), TEXT("system"), TEXT("system_update"));

    int32 EmitterIndex = 0;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        const FString EmitterName = EmitterHandle.GetName().ToString();
        if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
        {
            TArray<UNiagaraScript*> EmitterScripts;
            EmitterData->GetScripts(EmitterScripts, false, false);
            for (UNiagaraScript* Script : EmitterScripts)
            {
                AddScriptStatus(Script, FString::Printf(TEXT("emitter:%d"), EmitterIndex), EmitterName);
            }
        }
        ++EmitterIndex;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetBoolField(TEXT("read_only"), !bRequestCompile);
    Result->SetBoolField(TEXT("request_compile"), bRequestCompile);
    Result->SetBoolField(TEXT("compile_requested"), bCompileRequested);
    Result->SetBoolField(TEXT("wait_for_completion"), bWaitForCompletion);
    Result->SetBoolField(TEXT("wait_timed_out"), bWaitTimedOut);
    Result->SetNumberField(TEXT("wait_elapsed_seconds"), WaitElapsedSeconds);
    Result->SetNumberField(TEXT("wait_iterations"), WaitIterations);
    Result->SetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
    Result->SetNumberField(TEXT("poll_interval_seconds"), PollIntervalSeconds);
    Result->SetBoolField(TEXT("outstanding_compilation_requests_before"), bOutstandingBefore);
    Result->SetBoolField(TEXT("outstanding_compilation_requests_after_request"), bOutstandingAfterRequest);
    Result->SetBoolField(TEXT("outstanding_compilation_requests_after"), bOutstandingAfter);
    Result->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
    Result->SetNumberField(TEXT("script_count"), ScriptValues.Num());
    Result->SetNumberField(TEXT("error_count"), ErrorCount);
    Result->SetNumberField(TEXT("warning_count"), WarningCount);
    Result->SetNumberField(TEXT("dirty_count"), DirtyCount);
    Result->SetNumberField(TEXT("unknown_count"), UnknownCount);
    Result->SetNumberField(TEXT("missing_count"), MissingCount);
    Result->SetBoolField(TEXT("has_errors"), ErrorCount > 0);
    Result->SetBoolField(TEXT("has_warnings"), WarningCount > 0);
    Result->SetArrayField(TEXT("scripts"), ScriptValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleInspectNiagaraSimulationStages(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }

    bool bIncludeCompileData = true;
    Params->TryGetBoolField(TEXT("include_compile_data"), bIncludeCompileData);

    bool bIncludeScriptCompileStatus = true;
    Params->TryGetBoolField(TEXT("include_script_compile_status"), bIncludeScriptCompileStatus);

    int32 MaxStages = 128;
    Params->TryGetNumberField(TEXT("max_stages"), MaxStages);
    MaxStages = FMath::Clamp(MaxStages, 0, 1024);

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    TArray<TSharedPtr<FJsonValue>> EmitterValues;
    int32 TotalStageCount = 0;
    int32 IncludedStageCount = 0;
    bool bStagesTruncated = false;

    int32 EmitterIndex = 0;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        TSharedPtr<FJsonObject> EmitterObject = MakeShared<FJsonObject>();
        const FString EmitterName = EmitterHandle.GetName().ToString();
        EmitterObject->SetStringField(TEXT("name"), EmitterName);
        EmitterObject->SetNumberField(TEXT("emitter_index"), EmitterIndex);
        EmitterObject->SetBoolField(TEXT("enabled"), EmitterHandle.GetIsEnabled());

        TArray<TSharedPtr<FJsonValue>> StageValues;
        if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
        {
            EmitterObject->SetStringField(TEXT("sim_target"), NiagaraEnumValueName(EmitterData->SimTarget));
            const TArray<UNiagaraSimulationStageBase*>& SimulationStages = EmitterData->GetSimulationStages();
            EmitterObject->SetNumberField(TEXT("simulation_stage_count"), SimulationStages.Num());
            TotalStageCount += SimulationStages.Num();

            for (int32 StageIndex = 0; StageIndex < SimulationStages.Num(); ++StageIndex)
            {
                UNiagaraSimulationStageBase* Stage = SimulationStages[StageIndex];
                if (!Stage)
                {
                    continue;
                }
                if (IncludedStageCount >= MaxStages)
                {
                    bStagesTruncated = true;
                    break;
                }

                TSharedPtr<FJsonObject> StageObject = MakeShared<FJsonObject>();
                StageObject->SetNumberField(TEXT("stage_index"), StageIndex);
                StageObject->SetStringField(TEXT("stage_name"), Stage->SimulationStageName.ToString());
                StageObject->SetStringField(TEXT("stage_path"), Stage->GetPathName());
                StageObject->SetStringField(TEXT("stage_class"), Stage->GetClass() ? Stage->GetClass()->GetName() : FString());
                StageObject->SetBoolField(TEXT("enabled"), Stage->bEnabled != 0);
                StageObject->SetStringField(TEXT("script_path"), Stage->Script ? Stage->Script->GetPathName() : FString());

                if (bIncludeScriptCompileStatus)
                {
                    StageObject->SetObjectField(
                        TEXT("script_compile_status"),
                        BuildNiagaraScriptCompileStatusJson(
                            Stage->Script,
                            FString::Printf(TEXT("emitter:%d:stage:%d"), EmitterIndex, StageIndex),
                            EmitterName,
                            StageIndex));
                }

                if (UNiagaraSimulationStageGeneric* GenericStage = Cast<UNiagaraSimulationStageGeneric>(Stage))
                {
                    StageObject->SetBoolField(TEXT("is_generic_stage"), true);
                    StageObject->SetStringField(TEXT("iteration_source"), NiagaraEnumValueName(GenericStage->IterationSource));
                    StageObject->SetStringField(TEXT("execute_behavior"), NiagaraEnumValueName(GenericStage->ExecuteBehavior));
                    StageObject->SetBoolField(TEXT("disable_partial_particle_update"), GenericStage->bDisablePartialParticleUpdate != 0);
                    StageObject->SetBoolField(TEXT("particle_iteration_state_enabled"), GenericStage->bParticleIterationStateEnabled != 0);
                    StageObject->SetArrayField(TEXT("particle_iteration_state_range"), IntArrayToJsonValues({
                        GenericStage->ParticleIterationStateRange.X,
                        GenericStage->ParticleIterationStateRange.Y
                    }));
                    StageObject->SetBoolField(TEXT("gpu_dispatch_force_linear"), GenericStage->bGpuDispatchForceLinear != 0);
                    StageObject->SetBoolField(TEXT("override_gpu_dispatch_num_threads"), GenericStage->bOverrideGpuDispatchNumThreads != 0);
                    StageObject->SetStringField(TEXT("direct_dispatch_type"), NiagaraEnumValueName(GenericStage->DirectDispatchType));
                    StageObject->SetStringField(TEXT("direct_dispatch_element_type"), NiagaraEnumValueName(GenericStage->DirectDispatchElementType));

                    StageObject->SetObjectField(TEXT("enabled_binding"), NiagaraAttributeBindingToJsonObject(GenericStage->EnabledBinding));
                    StageObject->SetObjectField(TEXT("data_interface"), NiagaraDataInterfaceBindingToJsonObject(GenericStage->DataInterface));
                    StageObject->SetObjectField(TEXT("particle_iteration_state_binding"), NiagaraAttributeBindingToJsonObject(GenericStage->ParticleIterationStateBinding));
                    StageObject->SetObjectField(TEXT("num_iterations"), NiagaraParameterBindingWithValueToJsonObject(GenericStage->NumIterations));
                    StageObject->SetObjectField(TEXT("element_count_x"), NiagaraParameterBindingWithValueToJsonObject(GenericStage->ElementCountX));
                    StageObject->SetObjectField(TEXT("element_count_y"), NiagaraParameterBindingWithValueToJsonObject(GenericStage->ElementCountY));
                    StageObject->SetObjectField(TEXT("element_count_z"), NiagaraParameterBindingWithValueToJsonObject(GenericStage->ElementCountZ));
                    StageObject->SetObjectField(TEXT("override_gpu_dispatch_num_threads_x"), NiagaraParameterBindingWithValueToJsonObject(GenericStage->OverrideGpuDispatchNumThreadsX));
                    StageObject->SetObjectField(TEXT("override_gpu_dispatch_num_threads_y"), NiagaraParameterBindingWithValueToJsonObject(GenericStage->OverrideGpuDispatchNumThreadsY));
                    StageObject->SetObjectField(TEXT("override_gpu_dispatch_num_threads_z"), NiagaraParameterBindingWithValueToJsonObject(GenericStage->OverrideGpuDispatchNumThreadsZ));

#if WITH_EDITORONLY_DATA
                    if (bIncludeCompileData)
                    {
                        TArray<FNiagaraSimulationStageCompilationData> CompilationData;
                        const bool bFilledCompilationData = GenericStage->FillCompilationData(CompilationData);
                        TArray<TSharedPtr<FJsonValue>> CompileDataValues;
                        for (const FNiagaraSimulationStageCompilationData& CompileData : CompilationData)
                        {
                            CompileDataValues.Add(MakeShared<FJsonValueObject>(NiagaraSimStageCompileDataToJsonObject(CompileData)));
                        }
                        StageObject->SetBoolField(TEXT("filled_compile_data"), bFilledCompilationData);
                        StageObject->SetNumberField(TEXT("compile_data_count"), CompileDataValues.Num());
                        StageObject->SetArrayField(TEXT("compile_data"), CompileDataValues);
                    }
#else
                    StageObject->SetBoolField(TEXT("compile_data_unavailable"), bIncludeCompileData);
#endif
                }
                else
                {
                    StageObject->SetBoolField(TEXT("is_generic_stage"), false);
                    StageObject->SetStringField(TEXT("inspection_note"), TEXT("Only generic simulation-stage fields are expanded"));
                }

                StageValues.Add(MakeShared<FJsonValueObject>(StageObject));
                ++IncludedStageCount;
            }
        }
        else
        {
            EmitterObject->SetNumberField(TEXT("simulation_stage_count"), 0);
            EmitterObject->SetStringField(TEXT("inspection_error"), TEXT("Emitter data unavailable"));
        }

        EmitterObject->SetArrayField(TEXT("simulation_stages"), StageValues);
        EmitterObject->SetNumberField(TEXT("included_simulation_stage_count"), StageValues.Num());
        EmitterValues.Add(MakeShared<FJsonValueObject>(EmitterObject));
        ++EmitterIndex;

        if (bStagesTruncated)
        {
            break;
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetBoolField(TEXT("read_only"), true);
    Result->SetBoolField(TEXT("include_compile_data"), bIncludeCompileData);
    Result->SetBoolField(TEXT("include_script_compile_status"), bIncludeScriptCompileStatus);
    Result->SetNumberField(TEXT("max_stages"), MaxStages);
    Result->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
    Result->SetNumberField(TEXT("simulation_stage_count"), TotalStageCount);
    Result->SetNumberField(TEXT("included_simulation_stage_count"), IncludedStageCount);
    Result->SetBoolField(TEXT("stages_truncated"), bStagesTruncated);
    Result->SetArrayField(TEXT("emitters"), EmitterValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleInspectNiagaraGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }

    bool bIncludePins = true;
    Params->TryGetBoolField(TEXT("include_pins"), bIncludePins);

    bool bIncludeLinks = true;
    Params->TryGetBoolField(TEXT("include_links"), bIncludeLinks);

    bool bIncludeScratchPads = true;
    Params->TryGetBoolField(TEXT("include_scratch_pads"), bIncludeScratchPads);

    int32 MaxNodesPerGraph = 600;
    Params->TryGetNumberField(TEXT("max_nodes_per_graph"), MaxNodesPerGraph);
    MaxNodesPerGraph = FMath::Clamp(MaxNodesPerGraph, 0, 5000);

    int32 MaxLinksPerGraph = 2000;
    Params->TryGetNumberField(TEXT("max_links_per_graph"), MaxLinksPerGraph);
    MaxLinksPerGraph = FMath::Clamp(MaxLinksPerGraph, 0, 10000);

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    TArray<TSharedPtr<FJsonValue>> SystemScriptValues;
    TArray<TSharedPtr<FJsonValue>> EmitterValues;
    int32 TotalGraphCount = 0;
    int32 TotalScratchPadCount = 0;
    int32 TotalNodeCount = 0;
    int32 TotalLinkCount = 0;

    auto AccumulateGraphCounts = [&TotalNodeCount, &TotalLinkCount](const TSharedPtr<FJsonObject>& GraphObject)
    {
        if (!GraphObject.IsValid())
        {
            return;
        }
        TotalNodeCount += static_cast<int32>(GraphObject->GetNumberField(TEXT("node_count")));
        TotalLinkCount += static_cast<int32>(GraphObject->GetNumberField(TEXT("link_count")));
    };

    if (const UNiagaraScript* SystemSpawnScript = System->GetSystemSpawnScript())
    {
        TSharedPtr<FJsonObject> ScriptObject = MakeShared<FJsonObject>();
        ScriptObject->SetStringField(TEXT("name"), SystemSpawnScript->GetName());
        ScriptObject->SetStringField(TEXT("path"), SystemSpawnScript->GetPathName());
        ScriptObject->SetStringField(TEXT("usage"), NiagaraScriptUsageName(SystemSpawnScript->GetUsage()));
        TSharedPtr<FJsonObject> GraphObject = BuildDetailedGraphJson(
            SystemSpawnScript->GetLatestSource(),
            TEXT("system_spawn"),
            bIncludePins,
            bIncludeLinks,
            MaxNodesPerGraph,
            MaxLinksPerGraph);
        ScriptObject->SetObjectField(TEXT("graph"), GraphObject);
        AccumulateGraphCounts(GraphObject);
        SystemScriptValues.Add(MakeShared<FJsonValueObject>(ScriptObject));
        ++TotalGraphCount;
    }
    if (const UNiagaraScript* SystemUpdateScript = System->GetSystemUpdateScript())
    {
        TSharedPtr<FJsonObject> ScriptObject = MakeShared<FJsonObject>();
        ScriptObject->SetStringField(TEXT("name"), SystemUpdateScript->GetName());
        ScriptObject->SetStringField(TEXT("path"), SystemUpdateScript->GetPathName());
        ScriptObject->SetStringField(TEXT("usage"), NiagaraScriptUsageName(SystemUpdateScript->GetUsage()));
        TSharedPtr<FJsonObject> GraphObject = BuildDetailedGraphJson(
            SystemUpdateScript->GetLatestSource(),
            TEXT("system_update"),
            bIncludePins,
            bIncludeLinks,
            MaxNodesPerGraph,
            MaxLinksPerGraph);
        ScriptObject->SetObjectField(TEXT("graph"), GraphObject);
        AccumulateGraphCounts(GraphObject);
        SystemScriptValues.Add(MakeShared<FJsonValueObject>(ScriptObject));
        ++TotalGraphCount;
    }

    TArray<TSharedPtr<FJsonValue>> SystemScratchPadValues;
#if WITH_EDITORONLY_DATA
    if (bIncludeScratchPads)
    {
        for (const TObjectPtr<UNiagaraScript>& ScratchPadScript : System->ScratchPadScripts)
        {
            SystemScratchPadValues.Add(MakeShared<FJsonValueObject>(
                BuildDetailedScratchPadScriptJson(
                    ScratchPadScript.Get(),
                    TEXT("system"),
                    bIncludePins,
                    bIncludeLinks,
                    MaxNodesPerGraph,
                    MaxLinksPerGraph)));
        }
    }
#endif

    TotalScratchPadCount = SystemScratchPadValues.Num();
    TotalGraphCount += SystemScratchPadValues.Num();

    int32 EmitterIndex = 0;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        TSharedPtr<FJsonObject> EmitterObject = MakeShared<FJsonObject>();
        EmitterObject->SetStringField(TEXT("name"), EmitterHandle.GetName().ToString());
        EmitterObject->SetNumberField(TEXT("emitter_index"), EmitterIndex);
        EmitterObject->SetBoolField(TEXT("enabled"), EmitterHandle.GetIsEnabled());

        TArray<TSharedPtr<FJsonValue>> ScratchPadValues;
        TArray<TSharedPtr<FJsonValue>> ParentScratchPadValues;

        if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
        {
            TSharedPtr<FJsonObject> GraphObject = BuildDetailedGraphJson(
                EmitterData->GraphSource,
                FString::Printf(TEXT("emitter:%s"), *EmitterHandle.GetName().ToString()),
                bIncludePins,
                bIncludeLinks,
                MaxNodesPerGraph,
                MaxLinksPerGraph);
            EmitterObject->SetObjectField(TEXT("graph"), GraphObject);
            AccumulateGraphCounts(GraphObject);
            ++TotalGraphCount;

#if WITH_EDITORONLY_DATA
            if (bIncludeScratchPads)
            {
                AppendDetailedScratchPadContainerScripts(
                    EmitterData->ScratchPads,
                    TEXT("emitter"),
                    bIncludePins,
                    bIncludeLinks,
                    MaxNodesPerGraph,
                    MaxLinksPerGraph,
                    ScratchPadValues);
                AppendDetailedScratchPadContainerScripts(
                    EmitterData->ParentScratchPads,
                    TEXT("parent_emitter"),
                    bIncludePins,
                    bIncludeLinks,
                    MaxNodesPerGraph,
                    MaxLinksPerGraph,
                    ParentScratchPadValues);
            }
#endif
        }
        else
        {
            EmitterObject->SetObjectField(
                TEXT("graph"),
                BuildDetailedGraphJson(
                    nullptr,
                    FString::Printf(TEXT("emitter:%s"), *EmitterHandle.GetName().ToString()),
                    bIncludePins,
                    bIncludeLinks,
                    MaxNodesPerGraph,
                    MaxLinksPerGraph));
        }

        TotalScratchPadCount += ScratchPadValues.Num() + ParentScratchPadValues.Num();
        TotalGraphCount += ScratchPadValues.Num() + ParentScratchPadValues.Num();
        EmitterObject->SetArrayField(TEXT("scratch_pad_scripts"), ScratchPadValues);
        EmitterObject->SetArrayField(TEXT("parent_scratch_pad_scripts"), ParentScratchPadValues);
        EmitterValues.Add(MakeShared<FJsonValueObject>(EmitterObject));
        ++EmitterIndex;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetBoolField(TEXT("read_only"), true);
    Result->SetBoolField(TEXT("include_pins"), bIncludePins);
    Result->SetBoolField(TEXT("include_links"), bIncludeLinks);
    Result->SetBoolField(TEXT("include_scratch_pads"), bIncludeScratchPads);
    Result->SetNumberField(TEXT("max_nodes_per_graph"), MaxNodesPerGraph);
    Result->SetNumberField(TEXT("max_links_per_graph"), MaxLinksPerGraph);
    Result->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
    Result->SetNumberField(TEXT("system_script_count"), SystemScriptValues.Num());
    Result->SetNumberField(TEXT("system_scratch_pad_count"), SystemScratchPadValues.Num());
    Result->SetNumberField(TEXT("total_scratch_pad_count"), TotalScratchPadCount);
    Result->SetNumberField(TEXT("total_graph_count"), TotalGraphCount);
    Result->SetNumberField(TEXT("total_node_count"), TotalNodeCount);
    Result->SetNumberField(TEXT("total_link_count"), TotalLinkCount);
    Result->SetArrayField(TEXT("system_scripts"), SystemScriptValues);
    Result->SetArrayField(TEXT("system_scratch_pad_scripts"), SystemScratchPadValues);
    Result->SetArrayField(TEXT("emitters"), EmitterValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleInspectNiagaraScratchPadInterface(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }

    bool bIncludeGraphSummary = true;
    Params->TryGetBoolField(TEXT("include_graph_summary"), bIncludeGraphSummary);

    bool bIncludeParentScratchPads = true;
    Params->TryGetBoolField(TEXT("include_parent_scratch_pads"), bIncludeParentScratchPads);

    int32 MaxScripts = 200;
    Params->TryGetNumberField(TEXT("max_scripts"), MaxScripts);
    MaxScripts = FMath::Clamp(MaxScripts, 0, 1000);

    int32 MaxFunctionCalls = 80;
    Params->TryGetNumberField(TEXT("max_function_calls"), MaxFunctionCalls);
    MaxFunctionCalls = FMath::Clamp(MaxFunctionCalls, 0, 1000);

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    TArray<TSharedPtr<FJsonValue>> FlatScriptValues;
    TArray<TSharedPtr<FJsonValue>> SystemScratchPadValues;
    TArray<TSharedPtr<FJsonValue>> EmitterValues;
    int32 AvailableScratchPadCount = 0;
    int32 IncludedScratchPadCount = 0;
    int32 SystemScratchPadCount = 0;
    int32 EmitterScratchPadCount = 0;
    int32 ParentScratchPadCount = 0;
    int32 TotalInputCount = 0;
    int32 TotalOutputCount = 0;
    bool bScriptsTruncated = false;

    auto AddScratchPadScript = [&](
        const UNiagaraScript* Script,
        const FString& OwnerKind,
        const FString& OwnerName,
        int32 OwnerEmitterIndex,
        const FString& ContainerKind,
        int32 ScriptIndex,
        TArray<TSharedPtr<FJsonValue>>& OwnerScripts)
    {
        if (!Script)
        {
            return;
        }

        ++AvailableScratchPadCount;
        if (IncludedScratchPadCount >= MaxScripts)
        {
            bScriptsTruncated = true;
            return;
        }

        TSharedPtr<FJsonObject> ScriptObject = BuildScratchPadInterfaceJson(
            Script,
            OwnerKind,
            OwnerName,
            OwnerEmitterIndex,
            ContainerKind,
            ScriptIndex,
            bIncludeGraphSummary,
            MaxFunctionCalls);

        TotalInputCount += static_cast<int32>(ScriptObject->GetNumberField(TEXT("input_count")));
        TotalOutputCount += static_cast<int32>(ScriptObject->GetNumberField(TEXT("output_count")));
        ++IncludedScratchPadCount;
        OwnerScripts.Add(MakeShared<FJsonValueObject>(ScriptObject));
        FlatScriptValues.Add(MakeShared<FJsonValueObject>(ScriptObject));
    };

#if WITH_EDITORONLY_DATA
    for (int32 ScriptIndex = 0; ScriptIndex < System->ScratchPadScripts.Num(); ++ScriptIndex)
    {
        ++SystemScratchPadCount;
        AddScratchPadScript(
            System->ScratchPadScripts[ScriptIndex].Get(),
            TEXT("system"),
            System->GetName(),
            INDEX_NONE,
            TEXT("system_scratch_pads"),
            ScriptIndex,
            SystemScratchPadValues);
    }
#endif

    int32 EmitterIndex = 0;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        const FString EmitterName = EmitterHandle.GetName().ToString();
        TSharedPtr<FJsonObject> EmitterObject = MakeShared<FJsonObject>();
        EmitterObject->SetStringField(TEXT("name"), EmitterName);
        EmitterObject->SetNumberField(TEXT("emitter_index"), EmitterIndex);
        EmitterObject->SetBoolField(TEXT("enabled"), EmitterHandle.GetIsEnabled());

        TArray<TSharedPtr<FJsonValue>> ScratchPadValues;
        TArray<TSharedPtr<FJsonValue>> ParentScratchPadValues;

#if WITH_EDITORONLY_DATA
        if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
        {
            if (EmitterData->ScratchPads)
            {
                for (int32 ScriptIndex = 0; ScriptIndex < EmitterData->ScratchPads->Scripts.Num(); ++ScriptIndex)
                {
                    ++EmitterScratchPadCount;
                    AddScratchPadScript(
                        EmitterData->ScratchPads->Scripts[ScriptIndex].Get(),
                        TEXT("emitter"),
                        EmitterName,
                        EmitterIndex,
                        TEXT("emitter_scratch_pads"),
                        ScriptIndex,
                        ScratchPadValues);
                }
            }

            if (bIncludeParentScratchPads && EmitterData->ParentScratchPads)
            {
                for (int32 ScriptIndex = 0; ScriptIndex < EmitterData->ParentScratchPads->Scripts.Num(); ++ScriptIndex)
                {
                    ++ParentScratchPadCount;
                    AddScratchPadScript(
                        EmitterData->ParentScratchPads->Scripts[ScriptIndex].Get(),
                        TEXT("parent_emitter"),
                        EmitterName,
                        EmitterIndex,
                        TEXT("parent_scratch_pads"),
                        ScriptIndex,
                        ParentScratchPadValues);
                }
            }
        }
#endif

        EmitterObject->SetNumberField(TEXT("scratch_pad_count"), ScratchPadValues.Num());
        EmitterObject->SetNumberField(TEXT("parent_scratch_pad_count"), ParentScratchPadValues.Num());
        EmitterObject->SetArrayField(TEXT("scratch_pad_scripts"), ScratchPadValues);
        EmitterObject->SetArrayField(TEXT("parent_scratch_pad_scripts"), ParentScratchPadValues);
        EmitterValues.Add(MakeShared<FJsonValueObject>(EmitterObject));
        ++EmitterIndex;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetBoolField(TEXT("read_only"), true);
    Result->SetStringField(TEXT("source_policy"), TEXT("read_only"));
    Result->SetStringField(TEXT("authoring_status"), TEXT("scratch_pad_interface_inspection_only"));
    Result->SetBoolField(TEXT("include_graph_summary"), bIncludeGraphSummary);
    Result->SetBoolField(TEXT("include_parent_scratch_pads"), bIncludeParentScratchPads);
    Result->SetNumberField(TEXT("max_scripts"), MaxScripts);
    Result->SetNumberField(TEXT("max_function_calls"), MaxFunctionCalls);
    Result->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
    Result->SetNumberField(TEXT("system_scratch_pad_count"), SystemScratchPadCount);
    Result->SetNumberField(TEXT("emitter_scratch_pad_count"), EmitterScratchPadCount);
    Result->SetNumberField(TEXT("parent_scratch_pad_count"), ParentScratchPadCount);
    Result->SetNumberField(TEXT("available_scratch_pad_count"), AvailableScratchPadCount);
    Result->SetNumberField(TEXT("included_scratch_pad_count"), IncludedScratchPadCount);
    Result->SetNumberField(TEXT("total_input_count"), TotalInputCount);
    Result->SetNumberField(TEXT("total_output_count"), TotalOutputCount);
    Result->SetBoolField(TEXT("scripts_truncated"), bScriptsTruncated);
    Result->SetArrayField(TEXT("scratch_pad_scripts"), FlatScriptValues);
    Result->SetArrayField(TEXT("system_scratch_pad_scripts"), SystemScratchPadValues);
    Result->SetArrayField(TEXT("emitters"), EmitterValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleDuplicateOrAttachEmitterFromSource(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITORONLY_DATA
    FString TargetSystemPath;
    if (!Params.IsValid() || (!Params->TryGetStringField(TEXT("target_system_path"), TargetSystemPath) && !Params->TryGetStringField(TEXT("system_path"), TargetSystemPath)))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'target_system_path' parameter"));
    }

    bool bAllowSourceEdit = false;
    Params->TryGetBoolField(TEXT("allow_source_edit"), bAllowSourceEdit);
    if (!bAllowSourceEdit && !IsTempGeneratedNiagaraPath(TargetSystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Refusing to edit Niagara system outside %s: %s"), NiagaraTempGenerationRoot, *TargetSystemPath));
    }

    FString SourceEmitterPath;
    FString SourceSystemPath;
    Params->TryGetStringField(TEXT("source_emitter_path"), SourceEmitterPath);
    Params->TryGetStringField(TEXT("source_system_path"), SourceSystemPath);
    if (SourceEmitterPath.IsEmpty() && SourceSystemPath.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'source_emitter_path' or 'source_system_path' parameter"));
    }

    int32 SourceEmitterIndex = INDEX_NONE;
    FString SourceEmitterName;
    Params->TryGetNumberField(TEXT("source_emitter_index"), SourceEmitterIndex);
    Params->TryGetStringField(TEXT("source_emitter_name"), SourceEmitterName);

    FString NewEmitterName;
    Params->TryGetStringField(TEXT("new_emitter_name"), NewEmitterName);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    bool bRequestCompile = true;
    Params->TryGetBoolField(TEXT("request_compile"), bRequestCompile);

    bool bEnabled = true;
    const bool bHasEnabledOverride = Params->TryGetBoolField(TEXT("enabled"), bEnabled);

    UNiagaraSystem* TargetSystem = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(TargetSystemPath));
    if (!TargetSystem)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load target Niagara system: %s"), *TargetSystemPath));
    }

    const int32 EmitterCountBefore = TargetSystem->GetEmitterHandles().Num();
    FName DesiredEmitterName(NAME_None);
    FString SourceKind;
    FString SourceObjectPath;
    FString SourceHandleName;
    int32 MatchedSourceEmitterIndex = INDEX_NONE;

    TargetSystem->Modify();

    if (!SourceEmitterPath.IsEmpty())
    {
        UNiagaraEmitter* SourceEmitter = LoadObject<UNiagaraEmitter>(nullptr, *NormalizeNiagaraObjectPathForLoad(SourceEmitterPath));
        if (!SourceEmitter)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load source Niagara emitter: %s"), *SourceEmitterPath));
        }

        const FGuid SourceVersion = SourceEmitter->IsVersioningEnabled() && SourceEmitter->VersionToOpenInEditor.IsValid()
            ? SourceEmitter->VersionToOpenInEditor
            : SourceEmitter->GetExposedVersion().VersionGuid;
        DesiredEmitterName = FName(NewEmitterName.IsEmpty() ? SourceEmitter->GetUniqueEmitterName() : NewEmitterName);
        SourceKind = TEXT("emitter_asset");
        SourceObjectPath = SourceEmitter->GetPathName();
        SourceHandleName = SourceEmitter->GetUniqueEmitterName();
        TargetSystem->AddEmitterHandle(*SourceEmitter, DesiredEmitterName, SourceVersion);
    }
    else
    {
        UNiagaraSystem* SourceSystem = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SourceSystemPath));
        if (!SourceSystem)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load source Niagara system: %s"), *SourceSystemPath));
        }

        const FNiagaraEmitterHandle* MatchedSourceHandle = nullptr;
        int32 CurrentEmitterIndex = 0;
        for (const FNiagaraEmitterHandle& EmitterHandle : SourceSystem->GetEmitterHandles())
        {
            const FString EmitterName = EmitterHandle.GetName().ToString();
            const bool bEmitterMatches =
                (SourceEmitterIndex == INDEX_NONE || SourceEmitterIndex == CurrentEmitterIndex) &&
                (SourceEmitterName.IsEmpty() || SourceEmitterName.Equals(EmitterName, ESearchCase::IgnoreCase));
            if (bEmitterMatches)
            {
                MatchedSourceHandle = &EmitterHandle;
                MatchedSourceEmitterIndex = CurrentEmitterIndex;
                break;
            }
            ++CurrentEmitterIndex;
        }

        if (!MatchedSourceHandle)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No matching source Niagara emitter handle was found"));
        }

        DesiredEmitterName = FName(NewEmitterName.IsEmpty() ? MatchedSourceHandle->GetName().ToString() : NewEmitterName);
        SourceKind = TEXT("system_emitter_handle");
        SourceObjectPath = SourceSystem->GetPathName();
        SourceHandleName = MatchedSourceHandle->GetName().ToString();
        TargetSystem->DuplicateEmitterHandle(*MatchedSourceHandle, DesiredEmitterName);
    }

    if (TargetSystem->GetEmitterHandles().Num() <= EmitterCountBefore)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Emitter attach did not add a new emitter handle"));
    }

    const int32 NewEmitterIndex = TargetSystem->GetEmitterHandles().Num() - 1;
    FNiagaraEmitterHandle& NewEmitterHandle = TargetSystem->GetEmitterHandles()[NewEmitterIndex];
    NewEmitterHandle.SetName(DesiredEmitterName, *TargetSystem);
    if (bHasEnabledOverride)
    {
        NewEmitterHandle.SetIsEnabled(bEnabled, *TargetSystem, false);
    }

    TargetSystem->MarkPackageDirty();
    if (bRequestCompile)
    {
        TargetSystem->RequestCompile(false);
    }

    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveLoadedAsset(TargetSystem, false);
        if (!bSaved)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to save target Niagara system after emitter attach"));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("target_system_path"), TargetSystem->GetPathName());
    Result->SetStringField(TEXT("system_path"), TargetSystem->GetPathName());
    Result->SetStringField(TEXT("source_kind"), SourceKind);
    Result->SetStringField(TEXT("source_path"), SourceObjectPath);
    Result->SetStringField(TEXT("source_emitter_name"), SourceHandleName);
    Result->SetNumberField(TEXT("source_emitter_index"), MatchedSourceEmitterIndex);
    Result->SetStringField(TEXT("new_emitter_name"), NewEmitterHandle.GetName().ToString());
    Result->SetStringField(TEXT("new_emitter_id"), NewEmitterHandle.GetId().ToString(EGuidFormats::DigitsWithHyphens));
    Result->SetNumberField(TEXT("new_emitter_index"), NewEmitterIndex);
    Result->SetBoolField(TEXT("new_emitter_enabled"), NewEmitterHandle.GetIsEnabled());
    Result->SetStringField(TEXT("new_emitter_unique_instance_name"), NewEmitterHandle.GetUniqueInstanceName());
    Result->SetNumberField(TEXT("emitter_count_before"), EmitterCountBefore);
    Result->SetNumberField(TEXT("emitter_count_after"), TargetSystem->GetEmitterHandles().Num());
    Result->SetBoolField(TEXT("compile_requested"), bRequestCompile);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("write_scope"), TEXT("target_temp_system_emitter_attach"));
    return Result;
#else
    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("duplicate_or_attach_emitter_from_source requires editor-only Niagara data"));
#endif
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleCreateOrDuplicateScratchPadModule(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITORONLY_DATA
    FString TargetSystemPath;
    if (!Params.IsValid() || (!Params->TryGetStringField(TEXT("target_system_path"), TargetSystemPath) && !Params->TryGetStringField(TEXT("system_path"), TargetSystemPath)))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'target_system_path' parameter"));
    }

    bool bAllowSourceEdit = false;
    Params->TryGetBoolField(TEXT("allow_source_edit"), bAllowSourceEdit);
    if (!bAllowSourceEdit && !IsTempGeneratedNiagaraPath(TargetSystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Refusing to edit Niagara system outside %s: %s"), NiagaraTempGenerationRoot, *TargetSystemPath));
    }

    FString SourceScriptPath;
    FString SourceSystemPath;
    FString SourceOwnerKind = TEXT("system");
    FString SourceScratchPadName;
    int32 SourceScriptIndex = INDEX_NONE;
    int32 SourceEmitterIndex = INDEX_NONE;
    FString SourceEmitterName;
    Params->TryGetStringField(TEXT("source_script_path"), SourceScriptPath);
    Params->TryGetStringField(TEXT("source_system_path"), SourceSystemPath);
    Params->TryGetStringField(TEXT("source_owner_kind"), SourceOwnerKind);
    Params->TryGetStringField(TEXT("source_scratch_pad_name"), SourceScratchPadName);
    Params->TryGetNumberField(TEXT("source_script_index"), SourceScriptIndex);
    Params->TryGetNumberField(TEXT("source_emitter_index"), SourceEmitterIndex);
    Params->TryGetStringField(TEXT("source_emitter_name"), SourceEmitterName);

    if (SourceScriptPath.IsEmpty() && SourceSystemPath.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'source_script_path' or 'source_system_path' parameter"));
    }

    FString TargetOwnerKind = TEXT("system");
    int32 TargetEmitterIndex = INDEX_NONE;
    FString TargetEmitterName;
    FString NewScriptName;
    Params->TryGetStringField(TEXT("target_owner_kind"), TargetOwnerKind);
    Params->TryGetNumberField(TEXT("target_emitter_index"), TargetEmitterIndex);
    Params->TryGetStringField(TEXT("target_emitter_name"), TargetEmitterName);
    Params->TryGetStringField(TEXT("new_script_name"), NewScriptName);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    bool bRequestCompile = true;
    Params->TryGetBoolField(TEXT("request_compile"), bRequestCompile);

    UNiagaraSystem* TargetSystem = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(TargetSystemPath));
    if (!TargetSystem)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load target Niagara system: %s"), *TargetSystemPath));
    }

    UNiagaraScript* SourceScript = nullptr;
    FString ResolvedSourceOwnerKind;
    FString ResolvedSourceOwnerName;
    int32 ResolvedSourceEmitterIndex = INDEX_NONE;
    int32 ResolvedSourceScriptIndex = INDEX_NONE;

    auto MatchScript = [&SourceScratchPadName, SourceScriptIndex](const TArray<TObjectPtr<UNiagaraScript>>& Scripts, int32& OutIndex) -> UNiagaraScript*
    {
        for (int32 ScriptIndex = 0; ScriptIndex < Scripts.Num(); ++ScriptIndex)
        {
            UNiagaraScript* Candidate = Scripts[ScriptIndex].Get();
            if (!Candidate)
            {
                continue;
            }
            const bool bIndexMatches = SourceScriptIndex == INDEX_NONE || SourceScriptIndex == ScriptIndex;
            const bool bNameMatches = SourceScratchPadName.IsEmpty() ||
                Candidate->GetName().Equals(SourceScratchPadName, ESearchCase::IgnoreCase);
            if (bIndexMatches && bNameMatches)
            {
                OutIndex = ScriptIndex;
                return Candidate;
            }
        }
        return nullptr;
    };

    if (!SourceScriptPath.IsEmpty())
    {
        SourceScript = LoadObject<UNiagaraScript>(nullptr, *NormalizeNiagaraObjectPathForLoad(SourceScriptPath));
        if (!SourceScript)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load source Niagara Scratch Pad script: %s"), *SourceScriptPath));
        }
        ResolvedSourceOwnerKind = TEXT("script_path");
        ResolvedSourceOwnerName = SourceScript->GetOuter() ? SourceScript->GetOuter()->GetName() : FString();
    }
    else
    {
        UNiagaraSystem* SourceSystem = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SourceSystemPath));
        if (!SourceSystem)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load source Niagara system: %s"), *SourceSystemPath));
        }

        if (SourceOwnerKind.Equals(TEXT("system"), ESearchCase::IgnoreCase))
        {
            SourceScript = MatchScript(SourceSystem->ScratchPadScripts, ResolvedSourceScriptIndex);
            ResolvedSourceOwnerKind = TEXT("system");
            ResolvedSourceOwnerName = SourceSystem->GetName();
        }
        else
        {
            int32 CurrentEmitterIndex = 0;
            for (const FNiagaraEmitterHandle& EmitterHandle : SourceSystem->GetEmitterHandles())
            {
                const FString EmitterName = EmitterHandle.GetName().ToString();
                const bool bEmitterMatches =
                    (SourceEmitterIndex == INDEX_NONE || SourceEmitterIndex == CurrentEmitterIndex) &&
                    (SourceEmitterName.IsEmpty() || SourceEmitterName.Equals(EmitterName, ESearchCase::IgnoreCase));
                if (bEmitterMatches)
                {
                    if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
                    {
                        const bool bParent = SourceOwnerKind.Equals(TEXT("parent_emitter"), ESearchCase::IgnoreCase) ||
                            SourceOwnerKind.Equals(TEXT("parent"), ESearchCase::IgnoreCase);
                        UNiagaraScratchPadContainer* SourceContainer = bParent ? EmitterData->ParentScratchPads : EmitterData->ScratchPads;
                        if (SourceContainer)
                        {
                            SourceScript = MatchScript(SourceContainer->Scripts, ResolvedSourceScriptIndex);
                        }
                        ResolvedSourceOwnerKind = bParent ? TEXT("parent_emitter") : TEXT("emitter");
                        ResolvedSourceOwnerName = EmitterName;
                        ResolvedSourceEmitterIndex = CurrentEmitterIndex;
                    }
                    break;
                }
                ++CurrentEmitterIndex;
            }
        }
    }

    if (!SourceScript)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No matching source Scratch Pad script was found"));
    }

    if (SourceScript->GetUsage() != ENiagaraScriptUsage::Module && SourceScript->GetUsage() != ENiagaraScriptUsage::DynamicInput)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported Scratch Pad script usage for this first pass: %s"), *NiagaraScriptUsageName(SourceScript->GetUsage())));
    }

    UObject* TargetOuter = nullptr;
    TArray<TObjectPtr<UNiagaraScript>>* TargetScripts = nullptr;
    FString ResolvedTargetOwnerKind;
    FString ResolvedTargetOwnerName;
    int32 ResolvedTargetEmitterIndex = INDEX_NONE;

    if (TargetOwnerKind.Equals(TEXT("system"), ESearchCase::IgnoreCase))
    {
        TargetOuter = TargetSystem;
        TargetScripts = &TargetSystem->ScratchPadScripts;
        ResolvedTargetOwnerKind = TEXT("system");
        ResolvedTargetOwnerName = TargetSystem->GetName();
    }
    else if (TargetOwnerKind.Equals(TEXT("emitter"), ESearchCase::IgnoreCase))
    {
        int32 CurrentEmitterIndex = 0;
        for (FNiagaraEmitterHandle& EmitterHandle : TargetSystem->GetEmitterHandles())
        {
            const FString EmitterName = EmitterHandle.GetName().ToString();
            const bool bEmitterMatches =
                (TargetEmitterIndex == INDEX_NONE || TargetEmitterIndex == CurrentEmitterIndex) &&
                (TargetEmitterName.IsEmpty() || TargetEmitterName.Equals(EmitterName, ESearchCase::IgnoreCase));
            if (bEmitterMatches)
            {
                if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
                {
                    UNiagaraEmitter* TargetEmitter = EmitterHandle.GetInstance().Emitter;
                    if (!EmitterData->ScratchPads)
                    {
                        EmitterData->ScratchPads = NewObject<UNiagaraScratchPadContainer>(TargetEmitter ? static_cast<UObject*>(TargetEmitter) : static_cast<UObject*>(TargetSystem), NAME_None, RF_Transactional);
                    }
                    TargetOuter = EmitterData->ScratchPads;
                    TargetScripts = &EmitterData->ScratchPads->Scripts;
                    ResolvedTargetOwnerKind = TEXT("emitter");
                    ResolvedTargetOwnerName = EmitterName;
                    ResolvedTargetEmitterIndex = CurrentEmitterIndex;
                }
                break;
            }
            ++CurrentEmitterIndex;
        }
    }
    else
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Unsupported target_owner_kind. Use 'system' or 'emitter'."));
    }

    if (!TargetOuter || !TargetScripts)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No matching target Scratch Pad owner was found"));
    }

    TargetSystem->Modify();
    TargetOuter->Modify();
    const FName DesiredName = FName(NewScriptName.IsEmpty() ? SourceScript->GetName() : NewScriptName);
    const FName UniqueName = MakeUniqueObjectName(TargetOuter, UNiagaraScript::StaticClass(), DesiredName);
    UNiagaraScript* NewScript = Cast<UNiagaraScript>(StaticDuplicateObject(SourceScript, TargetOuter, UniqueName, RF_Transactional));
    if (!NewScript)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to duplicate Scratch Pad script"));
    }
    NewScript->ClearFlags(RF_Public | RF_Standalone);
    TargetScripts->Add(NewScript);

    if (ResolvedTargetOwnerKind == TEXT("emitter"))
    {
        if (TargetSystem->GetEmitterHandles().IsValidIndex(ResolvedTargetEmitterIndex))
        {
            if (UNiagaraEmitter* TargetEmitter = TargetSystem->GetEmitterHandles()[ResolvedTargetEmitterIndex].GetInstance().Emitter)
            {
                TargetEmitter->NotifyScratchPadScriptsChanged();
            }
        }
    }

    TargetSystem->MarkPackageDirty();
    if (bRequestCompile)
    {
        TargetSystem->RequestCompile(false);
    }

    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveLoadedAsset(TargetSystem, false);
        if (!bSaved)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to save target Niagara system after Scratch Pad duplication"));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("target_system_path"), TargetSystem->GetPathName());
    Result->SetStringField(TEXT("system_path"), TargetSystem->GetPathName());
    Result->SetStringField(TEXT("source_script_path"), SourceScript->GetPathName());
    Result->SetStringField(TEXT("source_script_name"), SourceScript->GetName());
    Result->SetStringField(TEXT("source_owner_kind"), ResolvedSourceOwnerKind);
    Result->SetStringField(TEXT("source_owner_name"), ResolvedSourceOwnerName);
    Result->SetNumberField(TEXT("source_emitter_index"), ResolvedSourceEmitterIndex);
    Result->SetNumberField(TEXT("source_script_index"), ResolvedSourceScriptIndex);
    Result->SetStringField(TEXT("target_owner_kind"), ResolvedTargetOwnerKind);
    Result->SetStringField(TEXT("target_owner_name"), ResolvedTargetOwnerName);
    Result->SetNumberField(TEXT("target_emitter_index"), ResolvedTargetEmitterIndex);
    Result->SetStringField(TEXT("new_script_name"), NewScript->GetName());
    Result->SetStringField(TEXT("new_script_path"), NewScript->GetPathName());
    Result->SetStringField(TEXT("usage"), NiagaraScriptUsageName(NewScript->GetUsage()));
    Result->SetNumberField(TEXT("target_scratch_pad_count"), TargetScripts->Num());
    Result->SetBoolField(TEXT("compile_requested"), bRequestCompile);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("write_scope"), TEXT("target_temp_scratch_pad_duplicate_only"));
    Result->SetObjectField(
        TEXT("interface"),
        BuildScratchPadInterfaceJson(
            NewScript,
            ResolvedTargetOwnerKind,
            ResolvedTargetOwnerName,
            ResolvedTargetEmitterIndex,
            ResolvedTargetOwnerKind == TEXT("system") ? TEXT("system_scratch_pads") : TEXT("emitter_scratch_pads"),
            TargetScripts->Num() - 1,
            true,
            80));
    return Result;
#else
    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("create_or_duplicate_scratch_pad_module requires editor-only Niagara data"));
#endif
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleAddScratchPadModuleToStack(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITORONLY_DATA
    FString TargetSystemPath;
    if (!Params.IsValid() || (!Params->TryGetStringField(TEXT("target_system_path"), TargetSystemPath) && !Params->TryGetStringField(TEXT("system_path"), TargetSystemPath)))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'target_system_path' parameter"));
    }

    bool bAllowSourceEdit = false;
    Params->TryGetBoolField(TEXT("allow_source_edit"), bAllowSourceEdit);
    if (!bAllowSourceEdit && !IsTempGeneratedNiagaraPath(TargetSystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Refusing to edit Niagara system outside %s: %s"), NiagaraTempGenerationRoot, *TargetSystemPath));
    }

    FString TargetUsageString = TEXT("ParticleUpdateScript");
    Params->TryGetStringField(TEXT("target_usage"), TargetUsageString);
    ENiagaraScriptUsage TargetUsage = ENiagaraScriptUsage::ParticleUpdateScript;
    if (!TryParseNiagaraScriptUsage(TargetUsageString, TargetUsage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unsupported target_usage: %s"), *TargetUsageString));
    }

    FGuid TargetUsageId;
    FString TargetUsageIdString;
    Params->TryGetStringField(TEXT("target_usage_id"), TargetUsageIdString);
    if (!TargetUsageIdString.IsEmpty() && !FGuid::Parse(TargetUsageIdString, TargetUsageId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid target_usage_id GUID: %s"), *TargetUsageIdString));
    }

    FString ScratchPadScriptPath;
    FString ScratchPadOwnerKind = TEXT("system");
    FString ScratchPadName;
    int32 ScratchPadScriptIndex = INDEX_NONE;
    int32 ScratchPadEmitterIndex = INDEX_NONE;
    FString ScratchPadEmitterName;
    Params->TryGetStringField(TEXT("scratch_pad_script_path"), ScratchPadScriptPath);
    Params->TryGetStringField(TEXT("scratch_pad_owner_kind"), ScratchPadOwnerKind);
    Params->TryGetStringField(TEXT("scratch_pad_name"), ScratchPadName);
    Params->TryGetStringField(TEXT("scratch_pad_script_name"), ScratchPadName);
    Params->TryGetNumberField(TEXT("scratch_pad_script_index"), ScratchPadScriptIndex);
    Params->TryGetNumberField(TEXT("scratch_pad_emitter_index"), ScratchPadEmitterIndex);
    Params->TryGetStringField(TEXT("scratch_pad_emitter_name"), ScratchPadEmitterName);

    int32 TargetEmitterIndex = INDEX_NONE;
    FString TargetEmitterName;
    Params->TryGetNumberField(TEXT("target_emitter_index"), TargetEmitterIndex);
    Params->TryGetStringField(TEXT("target_emitter_name"), TargetEmitterName);

    int32 TargetIndex = INDEX_NONE;
    Params->TryGetNumberField(TEXT("target_index"), TargetIndex);

    FString SuggestedName;
    Params->TryGetStringField(TEXT("suggested_name"), SuggestedName);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    bool bRequestCompile = true;
    Params->TryGetBoolField(TEXT("request_compile"), bRequestCompile);

    bool bSkipIfDuplicate = true;
    Params->TryGetBoolField(TEXT("skip_if_duplicate"), bSkipIfDuplicate);

    UNiagaraSystem* TargetSystem = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(TargetSystemPath));
    if (!TargetSystem)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load target Niagara system: %s"), *TargetSystemPath));
    }

    UNiagaraScript* RequestedScript = nullptr;
    if (!ScratchPadScriptPath.IsEmpty())
    {
        RequestedScript = LoadObject<UNiagaraScript>(nullptr, *NormalizeNiagaraObjectPathForLoad(ScratchPadScriptPath));
        if (!RequestedScript)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Scratch Pad script: %s"), *ScratchPadScriptPath));
        }
    }

    UNiagaraScript* ScratchPadScript = nullptr;
    FString ResolvedScratchPadOwnerKind;
    FString ResolvedScratchPadOwnerName;
    int32 ResolvedScratchPadEmitterIndex = INDEX_NONE;
    int32 ResolvedScratchPadScriptIndex = INDEX_NONE;

    auto TryMatchScratchPadScript = [&RequestedScript, &ScratchPadName, ScratchPadScriptIndex](
        const TArray<TObjectPtr<UNiagaraScript>>& Scripts,
        int32& OutScriptIndex) -> UNiagaraScript*
    {
        for (int32 ScriptIndex = 0; ScriptIndex < Scripts.Num(); ++ScriptIndex)
        {
            UNiagaraScript* Candidate = Scripts[ScriptIndex].Get();
            if (!Candidate)
            {
                continue;
            }
            const bool bPathMatches = !RequestedScript || Candidate == RequestedScript || Candidate->GetPathName().Equals(RequestedScript->GetPathName(), ESearchCase::IgnoreCase);
            const bool bIndexMatches = RequestedScript || ScratchPadScriptIndex == INDEX_NONE || ScratchPadScriptIndex == ScriptIndex;
            const bool bNameMatches = RequestedScript || ScratchPadName.IsEmpty() || Candidate->GetName().Equals(ScratchPadName, ESearchCase::IgnoreCase);
            if (bPathMatches && bIndexMatches && bNameMatches)
            {
                OutScriptIndex = ScriptIndex;
                return Candidate;
            }
        }
        return nullptr;
    };

    auto TryResolveSystemScratchPad = [&]()
    {
        int32 MatchedScriptIndex = INDEX_NONE;
        if (UNiagaraScript* Candidate = TryMatchScratchPadScript(TargetSystem->ScratchPadScripts, MatchedScriptIndex))
        {
            ScratchPadScript = Candidate;
            ResolvedScratchPadOwnerKind = TEXT("system");
            ResolvedScratchPadOwnerName = TargetSystem->GetName();
            ResolvedScratchPadEmitterIndex = INDEX_NONE;
            ResolvedScratchPadScriptIndex = MatchedScriptIndex;
            return true;
        }
        return false;
    };

    auto TryResolveEmitterScratchPad = [&]()
    {
        int32 CurrentEmitterIndex = 0;
        for (FNiagaraEmitterHandle& EmitterHandle : TargetSystem->GetEmitterHandles())
        {
            const FString EmitterName = EmitterHandle.GetName().ToString();
            const bool bEmitterMatches =
                RequestedScript ||
                ((ScratchPadEmitterIndex == INDEX_NONE || ScratchPadEmitterIndex == CurrentEmitterIndex) &&
                (ScratchPadEmitterName.IsEmpty() || ScratchPadEmitterName.Equals(EmitterName, ESearchCase::IgnoreCase)));
            if (bEmitterMatches)
            {
                if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
                {
                    if (EmitterData->ScratchPads)
                    {
                        int32 MatchedScriptIndex = INDEX_NONE;
                        if (UNiagaraScript* Candidate = TryMatchScratchPadScript(EmitterData->ScratchPads->Scripts, MatchedScriptIndex))
                        {
                            ScratchPadScript = Candidate;
                            ResolvedScratchPadOwnerKind = TEXT("emitter");
                            ResolvedScratchPadOwnerName = EmitterName;
                            ResolvedScratchPadEmitterIndex = CurrentEmitterIndex;
                            ResolvedScratchPadScriptIndex = MatchedScriptIndex;
                            return true;
                        }
                    }
                }
            }
            ++CurrentEmitterIndex;
        }
        return false;
    };

    if (RequestedScript)
    {
        TryResolveSystemScratchPad();
        if (!ScratchPadScript)
        {
            TryResolveEmitterScratchPad();
        }
    }
    else if (ScratchPadOwnerKind.Equals(TEXT("system"), ESearchCase::IgnoreCase))
    {
        TryResolveSystemScratchPad();
    }
    else if (ScratchPadOwnerKind.Equals(TEXT("emitter"), ESearchCase::IgnoreCase))
    {
        if (ScratchPadEmitterIndex == INDEX_NONE && ScratchPadEmitterName.IsEmpty() && TargetEmitterIndex != INDEX_NONE)
        {
            ScratchPadEmitterIndex = TargetEmitterIndex;
        }
        if (ScratchPadEmitterName.IsEmpty() && !TargetEmitterName.IsEmpty())
        {
            ScratchPadEmitterName = TargetEmitterName;
        }
        TryResolveEmitterScratchPad();
    }
    else
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Unsupported scratch_pad_owner_kind. Use 'system' or 'emitter'."));
    }

    if (!ScratchPadScript)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No matching target-local Scratch Pad script was found. Duplicate it into the temp target system first."));
    }

    if (ScratchPadScript->GetUsage() != ENiagaraScriptUsage::Module)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Only Module Scratch Pad scripts can be inserted into a stack. Found: %s"), *NiagaraScriptUsageName(ScratchPadScript->GetUsage())));
    }

    if (!IsScratchPadScriptCompatibleWithStackUsage(ScratchPadScript, TargetUsage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(
                TEXT("Scratch Pad '%s' does not advertise support for target_usage '%s'"),
                *ScratchPadScript->GetName(),
                *NiagaraScriptUsageName(TargetUsage)));
    }

    UNiagaraGraph* TargetGraph = nullptr;
    UNiagaraNodeOutput* TargetOutputNode = nullptr;
    FString ResolvedStackOwnerKind;
    FString ResolvedStackOwnerName;
    int32 ResolvedTargetEmitterIndex = INDEX_NONE;

    if (TargetUsage == ENiagaraScriptUsage::SystemSpawnScript || TargetUsage == ENiagaraScriptUsage::SystemUpdateScript)
    {
        UNiagaraScript* TargetScript = TargetUsage == ENiagaraScriptUsage::SystemSpawnScript
            ? TargetSystem->GetSystemSpawnScript()
            : TargetSystem->GetSystemUpdateScript();
        UNiagaraScriptSource* ScriptSource = TargetScript ? Cast<UNiagaraScriptSource>(TargetScript->GetLatestSource()) : nullptr;
        TargetGraph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
        ResolvedStackOwnerKind = TEXT("system");
        ResolvedStackOwnerName = TargetSystem->GetName();
    }
    else if (IsEmitterStackUsage(TargetUsage))
    {
        int32 CurrentEmitterIndex = 0;
        for (FNiagaraEmitterHandle& EmitterHandle : TargetSystem->GetEmitterHandles())
        {
            const FString EmitterName = EmitterHandle.GetName().ToString();
            const bool bEmitterMatches =
                (TargetEmitterIndex == INDEX_NONE || TargetEmitterIndex == CurrentEmitterIndex) &&
                (TargetEmitterName.IsEmpty() || TargetEmitterName.Equals(EmitterName, ESearchCase::IgnoreCase));
            if (bEmitterMatches)
            {
                if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
                {
                    UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
                    TargetGraph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
                    ResolvedStackOwnerKind = TEXT("emitter");
                    ResolvedStackOwnerName = EmitterName;
                    ResolvedTargetEmitterIndex = CurrentEmitterIndex;
                }
                break;
            }
            ++CurrentEmitterIndex;
        }
    }

    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No matching Niagara stack graph was found for the requested target_usage/emitter selector"));
    }

    TargetOutputNode = TargetGraph->FindEquivalentOutputNode(TargetUsage, TargetUsageId);
    if (!TargetOutputNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("No matching output node was found for target_usage '%s'"), *NiagaraScriptUsageName(TargetUsage)));
    }

    const int32 NodeCountBefore = TargetGraph->Nodes.Num();
    if (bSkipIfDuplicate)
    {
        for (UEdGraphNode* Node : TargetGraph->Nodes)
        {
            UNiagaraNodeFunctionCall* ExistingFunctionCall = Cast<UNiagaraNodeFunctionCall>(Node);
            if (!ExistingFunctionCall || !ExistingFunctionCall->FunctionScript)
            {
                continue;
            }
            const bool bSameScript = ExistingFunctionCall->FunctionScript == ScratchPadScript ||
                ExistingFunctionCall->FunctionScript->GetPathName().Equals(ScratchPadScript->GetPathName(), ESearchCase::IgnoreCase);
            if (!bSameScript)
            {
                continue;
            }

            if (!IsFunctionCallInTargetOutputStack(*ExistingFunctionCall, *TargetOutputNode, TargetUsage, TargetUsageId))
            {
                continue;
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("success"), true);
            Result->SetBoolField(TEXT("skipped_duplicate"), true);
            Result->SetStringField(TEXT("skip_reason"), TEXT("scratch_pad_module_already_exists_in_target_usage"));
            Result->SetStringField(TEXT("target_system_path"), TargetSystem->GetPathName());
            Result->SetStringField(TEXT("system_path"), TargetSystem->GetPathName());
            Result->SetStringField(TEXT("scratch_pad_script_path"), ScratchPadScript->GetPathName());
            Result->SetStringField(TEXT("scratch_pad_script_name"), ScratchPadScript->GetName());
            Result->SetStringField(TEXT("scratch_pad_owner_kind"), ResolvedScratchPadOwnerKind);
            Result->SetStringField(TEXT("scratch_pad_owner_name"), ResolvedScratchPadOwnerName);
            Result->SetNumberField(TEXT("scratch_pad_emitter_index"), ResolvedScratchPadEmitterIndex);
            Result->SetNumberField(TEXT("scratch_pad_script_index"), ResolvedScratchPadScriptIndex);
            Result->SetStringField(TEXT("target_usage"), NiagaraScriptUsageName(TargetUsage));
            Result->SetStringField(TEXT("target_usage_id"), TargetUsageId.ToString(EGuidFormats::DigitsWithHyphens));
            Result->SetStringField(TEXT("stack_owner_kind"), ResolvedStackOwnerKind);
            Result->SetStringField(TEXT("stack_owner_name"), ResolvedStackOwnerName);
            Result->SetNumberField(TEXT("target_emitter_index"), ResolvedTargetEmitterIndex);
            Result->SetNumberField(TEXT("target_index"), TargetIndex);
            Result->SetStringField(TEXT("suggested_name"), SuggestedName);
            Result->SetStringField(TEXT("existing_module_node_guid"), ExistingFunctionCall->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
            Result->SetStringField(TEXT("existing_module_node_name"), ExistingFunctionCall->GetName());
            Result->SetStringField(TEXT("existing_module_function_name"), ExistingFunctionCall->GetFunctionName());
            Result->SetStringField(TEXT("output_node_guid"), TargetOutputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
            Result->SetStringField(TEXT("graph_path"), TargetGraph->GetPathName());
            Result->SetNumberField(TEXT("graph_node_count_before"), NodeCountBefore);
            Result->SetNumberField(TEXT("graph_node_count_after"), TargetGraph->Nodes.Num());
            Result->SetBoolField(TEXT("compile_requested"), false);
            Result->SetBoolField(TEXT("saved"), false);
            Result->SetStringField(TEXT("write_scope"), TEXT("target_temp_scratch_pad_stack_insert_duplicate_skip"));
            Result->SetObjectField(TEXT("module"), BuildFunctionCallJson(ExistingFunctionCall, INDEX_NONE, false));
            return Result;
        }
    }

    TargetSystem->Modify();
    TargetGraph->Modify();
    TargetOutputNode->Modify();

    UNiagaraNodeFunctionCall* NewModuleNode = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
        ScratchPadScript,
        *TargetOutputNode,
        TargetIndex,
        SuggestedName);
    if (!NewModuleNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Niagara stack utility failed to add the Scratch Pad module"));
    }

    TargetSystem->MarkPackageDirty();
    if (bRequestCompile)
    {
        TargetSystem->RequestCompile(false);
    }

    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveLoadedAsset(TargetSystem, false);
        if (!bSaved)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to save target Niagara system after Scratch Pad stack insertion"));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetBoolField(TEXT("skipped_duplicate"), false);
    Result->SetStringField(TEXT("target_system_path"), TargetSystem->GetPathName());
    Result->SetStringField(TEXT("system_path"), TargetSystem->GetPathName());
    Result->SetStringField(TEXT("scratch_pad_script_path"), ScratchPadScript->GetPathName());
    Result->SetStringField(TEXT("scratch_pad_script_name"), ScratchPadScript->GetName());
    Result->SetStringField(TEXT("scratch_pad_owner_kind"), ResolvedScratchPadOwnerKind);
    Result->SetStringField(TEXT("scratch_pad_owner_name"), ResolvedScratchPadOwnerName);
    Result->SetNumberField(TEXT("scratch_pad_emitter_index"), ResolvedScratchPadEmitterIndex);
    Result->SetNumberField(TEXT("scratch_pad_script_index"), ResolvedScratchPadScriptIndex);
    Result->SetStringField(TEXT("target_usage"), NiagaraScriptUsageName(TargetUsage));
    Result->SetStringField(TEXT("target_usage_id"), TargetUsageId.ToString(EGuidFormats::DigitsWithHyphens));
    Result->SetStringField(TEXT("stack_owner_kind"), ResolvedStackOwnerKind);
    Result->SetStringField(TEXT("stack_owner_name"), ResolvedStackOwnerName);
    Result->SetNumberField(TEXT("target_emitter_index"), ResolvedTargetEmitterIndex);
    Result->SetNumberField(TEXT("target_index"), TargetIndex);
    Result->SetStringField(TEXT("suggested_name"), SuggestedName);
    Result->SetStringField(TEXT("new_module_node_guid"), NewModuleNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    Result->SetStringField(TEXT("new_module_node_name"), NewModuleNode->GetName());
    Result->SetStringField(TEXT("new_module_function_name"), NewModuleNode->GetFunctionName());
    Result->SetStringField(TEXT("output_node_guid"), TargetOutputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    Result->SetStringField(TEXT("graph_path"), TargetGraph->GetPathName());
    Result->SetNumberField(TEXT("graph_node_count_before"), NodeCountBefore);
    Result->SetNumberField(TEXT("graph_node_count_after"), TargetGraph->Nodes.Num());
    Result->SetBoolField(TEXT("compile_requested"), bRequestCompile);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("write_scope"), TEXT("target_temp_scratch_pad_stack_insert"));
    Result->SetObjectField(TEXT("module"), BuildFunctionCallJson(NewModuleNode, INDEX_NONE, false));
    return Result;
#else
    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_scratch_pad_module_to_stack requires editor-only Niagara data"));
#endif
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleInspectNiagaraModuleInputs(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }

    bool bIncludeLinkedSources = true;
    Params->TryGetBoolField(TEXT("include_linked_sources"), bIncludeLinkedSources);

    bool bIncludeResolvedStackInputs = false;
    Params->TryGetBoolField(TEXT("include_resolved_stack_inputs"), bIncludeResolvedStackInputs);

    int32 MaxModules = 200;
    Params->TryGetNumberField(TEXT("max_modules"), MaxModules);
    MaxModules = FMath::Clamp(MaxModules, 0, 1000);

    int32 MaxCandidatesPerModule = 24;
    Params->TryGetNumberField(TEXT("max_candidates_per_module"), MaxCandidatesPerModule);
    MaxCandidatesPerModule = FMath::Clamp(MaxCandidatesPerModule, 0, 200);

    int32 MaxResolvedInputsPerModule = 8;
    Params->TryGetNumberField(TEXT("max_resolved_inputs_per_module"), MaxResolvedInputsPerModule);
    MaxResolvedInputsPerModule = FMath::Clamp(MaxResolvedInputsPerModule, 0, 200);

    int32 MaxTopCandidates = 80;
    Params->TryGetNumberField(TEXT("max_top_candidates"), MaxTopCandidates);
    MaxTopCandidates = FMath::Clamp(MaxTopCandidates, 0, 500);

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    TArray<TSharedPtr<FJsonValue>> EmitterValues;
    TArray<TSharedPtr<FJsonObject>> TopCandidateObjects;
    int32 TotalModuleCount = 0;
    int32 TotalCandidateCount = 0;

    int32 EmitterIndex = 0;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        TSharedPtr<FJsonObject> EmitterObject = MakeShared<FJsonObject>();
        const FString EmitterName = EmitterHandle.GetName().ToString();
        EmitterObject->SetStringField(TEXT("name"), EmitterName);
        EmitterObject->SetNumberField(TEXT("emitter_index"), EmitterIndex);
        EmitterObject->SetBoolField(TEXT("enabled"), EmitterHandle.GetIsEnabled());

        TArray<TSharedPtr<FJsonValue>> ModuleValues;
        if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
        {
            const FVersionedNiagaraEmitter OwningEmitter = EmitterHandle.GetInstance();
            const TArray<UNiagaraNodeFunctionCall*> FunctionCalls = CollectSortedNiagaraFunctionCalls(EmitterData->GraphSource);
            const int32 SafeModuleCount = FMath::Min(FunctionCalls.Num(), MaxModules);
            for (int32 ModuleIndex = 0; ModuleIndex < SafeModuleCount; ++ModuleIndex)
            {
                ModuleValues.Add(MakeShared<FJsonValueObject>(BuildModuleInputModuleJson(
                    FunctionCalls[ModuleIndex],
                    OwningEmitter,
                    EmitterData,
                    EmitterName,
                    EmitterIndex,
                    ModuleIndex,
                    bIncludeLinkedSources,
                    bIncludeResolvedStackInputs,
                    MaxCandidatesPerModule,
                    MaxResolvedInputsPerModule,
                    TotalCandidateCount,
                    TopCandidateObjects)));
            }
            TotalModuleCount += FunctionCalls.Num();
            EmitterObject->SetBoolField(TEXT("modules_truncated"), FunctionCalls.Num() > SafeModuleCount);
            EmitterObject->SetNumberField(TEXT("module_count"), FunctionCalls.Num());
        }
        else
        {
            EmitterObject->SetBoolField(TEXT("modules_truncated"), false);
            EmitterObject->SetNumberField(TEXT("module_count"), 0);
        }

        EmitterObject->SetArrayField(TEXT("modules"), ModuleValues);
        EmitterValues.Add(MakeShared<FJsonValueObject>(EmitterObject));
        ++EmitterIndex;
    }

    TopCandidateObjects.Sort(
        [](const TSharedPtr<FJsonObject>& Left, const TSharedPtr<FJsonObject>& Right)
        {
            const double LeftPriority = Left.IsValid() ? Left->GetNumberField(TEXT("priority")) : 0.0;
            const double RightPriority = Right.IsValid() ? Right->GetNumberField(TEXT("priority")) : 0.0;
            if (!FMath::IsNearlyEqual(LeftPriority, RightPriority))
            {
                return LeftPriority > RightPriority;
            }
            const FString LeftModule = Left.IsValid() ? Left->GetStringField(TEXT("module_name")) : FString();
            const FString RightModule = Right.IsValid() ? Right->GetStringField(TEXT("module_name")) : FString();
            return LeftModule < RightModule;
        });

    TArray<TSharedPtr<FJsonValue>> TopCandidateValues;
    for (int32 Index = 0; Index < TopCandidateObjects.Num() && Index < MaxTopCandidates; ++Index)
    {
        TopCandidateValues.Add(MakeShared<FJsonValueObject>(TopCandidateObjects[Index]));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetBoolField(TEXT("include_linked_sources"), bIncludeLinkedSources);
    Result->SetBoolField(TEXT("include_resolved_stack_inputs"), bIncludeResolvedStackInputs);
    Result->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
    Result->SetNumberField(TEXT("module_count"), TotalModuleCount);
    Result->SetNumberField(TEXT("candidate_count"), TotalCandidateCount);
    Result->SetNumberField(TEXT("top_candidate_count"), TopCandidateValues.Num());
    Result->SetBoolField(TEXT("can_author_module_inputs"), false);
    Result->SetStringField(TEXT("authoring_status"), TEXT("read_only; use this result as generation planning input before enabling temp-asset module writes"));
    Result->SetArrayField(TEXT("emitters"), EmitterValues);
    Result->SetArrayField(TEXT("top_candidates"), TopCandidateValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleInspectNiagaraDataInterfaceOverrides(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }

    FString TargetInputName;
    Params->TryGetStringField(TEXT("input_name"), TargetInputName);

    int32 TargetEmitterIndex = INDEX_NONE;
    int32 TargetModuleIndex = INDEX_NONE;
    FString TargetEmitterName;
    FString TargetModuleName;
    FString TargetModuleGuid;
    Params->TryGetNumberField(TEXT("emitter_index"), TargetEmitterIndex);
    Params->TryGetNumberField(TEXT("module_index"), TargetModuleIndex);
    Params->TryGetStringField(TEXT("emitter_name"), TargetEmitterName);
    Params->TryGetStringField(TEXT("module_name"), TargetModuleName);
    Params->TryGetStringField(TEXT("module_node_guid"), TargetModuleGuid);

    int32 MaxModules = 200;
    Params->TryGetNumberField(TEXT("max_modules"), MaxModules);
    MaxModules = FMath::Clamp(MaxModules, 0, 1000);

    int32 MaxInputsPerModule = 64;
    Params->TryGetNumberField(TEXT("max_inputs_per_module"), MaxInputsPerModule);
    MaxInputsPerModule = FMath::Clamp(MaxInputsPerModule, 0, 200);

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    TArray<TSharedPtr<FJsonValue>> EmitterValues;
    int32 TotalScannedModuleCount = 0;
    int32 TotalMatchedModuleCount = 0;
    int32 TotalIncludedModuleCount = 0;
    int32 TotalDataInterfaceInputCount = 0;
    int32 TotalOverriddenInputCount = 0;

    int32 CurrentEmitterIndex = 0;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        const FString EmitterName = EmitterHandle.GetName().ToString();
        const bool bEmitterMatches =
            (TargetEmitterIndex == INDEX_NONE || TargetEmitterIndex == CurrentEmitterIndex) &&
            (TargetEmitterName.IsEmpty() || TargetEmitterName.Equals(EmitterName, ESearchCase::IgnoreCase));

        if (!bEmitterMatches)
        {
            ++CurrentEmitterIndex;
            continue;
        }

        TSharedPtr<FJsonObject> EmitterObject = MakeShared<FJsonObject>();
        EmitterObject->SetStringField(TEXT("name"), EmitterName);
        EmitterObject->SetNumberField(TEXT("emitter_index"), CurrentEmitterIndex);
        EmitterObject->SetBoolField(TEXT("enabled"), EmitterHandle.GetIsEnabled());

        TArray<TSharedPtr<FJsonValue>> ModuleValues;
        int32 EmitterScannedModuleCount = 0;
        int32 EmitterMatchedModuleCount = 0;
        int32 EmitterDataInterfaceInputCount = 0;
        int32 EmitterOverriddenInputCount = 0;
        bool bModulesTruncated = false;

        if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
        {
            const FVersionedNiagaraEmitter OwningEmitter = EmitterHandle.GetInstance();
            const TArray<UNiagaraNodeFunctionCall*> FunctionCalls = CollectSortedNiagaraFunctionCalls(EmitterData->GraphSource);
            EmitterScannedModuleCount = FunctionCalls.Num();
            TotalScannedModuleCount += FunctionCalls.Num();

            for (int32 ModuleIndex = 0; ModuleIndex < FunctionCalls.Num(); ++ModuleIndex)
            {
                UNiagaraNodeFunctionCall* FunctionCall = FunctionCalls[ModuleIndex];
                if (!FunctionCall)
                {
                    continue;
                }

                FString FunctionName = FunctionCall->GetFunctionName();
                if (FunctionName.IsEmpty())
                {
                    FunctionName = FunctionCall->Signature.GetNameString();
                }
                const FString FunctionGuid = FunctionCall->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
                const bool bModuleMatches =
                    (TargetModuleIndex == INDEX_NONE || TargetModuleIndex == ModuleIndex) &&
                    (TargetModuleName.IsEmpty() || TargetModuleName.Equals(FunctionName, ESearchCase::IgnoreCase)) &&
                    (TargetModuleGuid.IsEmpty() || TargetModuleGuid.Equals(FunctionGuid, ESearchCase::IgnoreCase));

                if (!bModuleMatches)
                {
                    continue;
                }

                const ENiagaraScriptUsage OutputUsage = FNiagaraStackGraphUtilities::GetOutputNodeUsage(*FunctionCall);
                FCompileConstantResolver ConstantResolver(OwningEmitter, OutputUsage, FunctionCall->DebugState);

                TArray<FNiagaraVariable> InputVariables;
                TSet<FNiagaraVariable> HiddenVariables;
                FNiagaraStackGraphUtilities::GetStackFunctionInputs(
                    *FunctionCall,
                    InputVariables,
                    HiddenVariables,
                    ConstantResolver,
                    FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly,
                    true);

                TArray<TSharedPtr<FJsonValue>> DataInterfaceInputValues;
                int32 ModuleDataInterfaceInputCount = 0;
                int32 ModuleOverriddenInputCount = 0;
                bool bInputsTruncated = false;
                for (const FNiagaraVariable& InputVariable : InputVariables)
                {
                    if (!InputVariable.GetType().IsDataInterface())
                    {
                        continue;
                    }
                    if (!TargetInputName.IsEmpty() && !NiagaraModuleInputNameMatches(InputVariable, TargetInputName))
                    {
                        continue;
                    }

                    ++ModuleDataInterfaceInputCount;
                    ++EmitterDataInterfaceInputCount;
                    ++TotalDataInterfaceInputCount;

                    const FNiagaraParameterHandle AliasedInputHandle = BuildAliasedModuleParameterHandleForInput(FunctionCall, InputVariable);
                    const UEdGraphPin* OverridePin = FindNiagaraStackFunctionInputOverridePin(FunctionCall, AliasedInputHandle);
                    const int32 LinkCount = OverridePin ? OverridePin->LinkedTo.Num() : 0;
                    if (LinkCount > 0)
                    {
                        ++ModuleOverriddenInputCount;
                        ++EmitterOverriddenInputCount;
                        ++TotalOverriddenInputCount;
                    }

                    if (DataInterfaceInputValues.Num() >= MaxInputsPerModule)
                    {
                        bInputsTruncated = true;
                        continue;
                    }

                    TSharedPtr<FJsonObject> InputObject = MakeShared<FJsonObject>();
                    InputObject->SetObjectField(TEXT("variable"), NiagaraVariableToJsonObject(InputVariable));
                    InputObject->SetStringField(TEXT("input_name"), InputVariable.GetName().ToString());
                    InputObject->SetStringField(TEXT("input_type"), NiagaraTypeName(InputVariable.GetType()));
                    InputObject->SetBoolField(TEXT("is_hidden"), HiddenVariables.Contains(InputVariable));
                    InputObject->SetStringField(TEXT("aliased_input_handle"), AliasedInputHandle.GetParameterHandleString().ToString());
                    InputObject->SetBoolField(TEXT("has_override_pin"), OverridePin != nullptr);
                    InputObject->SetNumberField(TEXT("override_link_count"), LinkCount);
                    InputObject->SetObjectField(TEXT("override"), BuildNiagaraDataInterfaceOverrideJson(OverridePin, System));
                    DataInterfaceInputValues.Add(MakeShared<FJsonValueObject>(InputObject));
                }

                if (ModuleDataInterfaceInputCount == 0)
                {
                    continue;
                }

                ++EmitterMatchedModuleCount;
                ++TotalMatchedModuleCount;

                if (ModuleValues.Num() >= MaxModules)
                {
                    bModulesTruncated = true;
                    continue;
                }

                TSharedPtr<FJsonObject> ModuleObject = BuildFunctionCallJson(FunctionCall, ModuleIndex, false);
                ModuleObject->SetStringField(TEXT("emitter_name"), EmitterName);
                ModuleObject->SetNumberField(TEXT("emitter_index"), CurrentEmitterIndex);
                ModuleObject->SetNumberField(TEXT("data_interface_input_count"), ModuleDataInterfaceInputCount);
                ModuleObject->SetNumberField(TEXT("overridden_input_count"), ModuleOverriddenInputCount);
                ModuleObject->SetBoolField(TEXT("data_interface_inputs_truncated"), bInputsTruncated);
                ModuleObject->SetArrayField(TEXT("data_interface_inputs"), DataInterfaceInputValues);
                ModuleValues.Add(MakeShared<FJsonValueObject>(ModuleObject));
                ++TotalIncludedModuleCount;
            }
        }

        EmitterObject->SetNumberField(TEXT("scanned_module_count"), EmitterScannedModuleCount);
        EmitterObject->SetNumberField(TEXT("matched_module_count"), EmitterMatchedModuleCount);
        EmitterObject->SetNumberField(TEXT("data_interface_input_count"), EmitterDataInterfaceInputCount);
        EmitterObject->SetNumberField(TEXT("overridden_input_count"), EmitterOverriddenInputCount);
        EmitterObject->SetBoolField(TEXT("modules_truncated"), bModulesTruncated);
        EmitterObject->SetArrayField(TEXT("modules"), ModuleValues);
        EmitterValues.Add(MakeShared<FJsonValueObject>(EmitterObject));

        ++CurrentEmitterIndex;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetBoolField(TEXT("read_only"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetStringField(TEXT("input_name_filter"), TargetInputName);
    Result->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
    Result->SetNumberField(TEXT("scanned_module_count"), TotalScannedModuleCount);
    Result->SetNumberField(TEXT("matched_module_count"), TotalMatchedModuleCount);
    Result->SetNumberField(TEXT("included_module_count"), TotalIncludedModuleCount);
    Result->SetNumberField(TEXT("data_interface_input_count"), TotalDataInterfaceInputCount);
    Result->SetNumberField(TEXT("overridden_input_count"), TotalOverriddenInputCount);
    Result->SetStringField(TEXT("authoring_status"), TEXT("read_only; inspects Data Interface override graph nodes and user-parameter object bindings without compiling or saving"));
    Result->SetArrayField(TEXT("emitters"), EmitterValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraModuleInputValue(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString InputName;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("input_name"), InputName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'input_name' parameter"));
    }

    const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
    if (!Value.IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'value' parameter"));
    }

    bool bAllowSourceEdit = false;
    Params->TryGetBoolField(TEXT("allow_source_edit"), bAllowSourceEdit);
    if (!bAllowSourceEdit && !IsTempGeneratedNiagaraPath(SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Refusing to edit Niagara module input outside %s: %s"), NiagaraTempGenerationRoot, *SystemPath));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    bool bRequestCompile = true;
    Params->TryGetBoolField(TEXT("request_compile"), bRequestCompile);

    int32 TargetEmitterIndex = INDEX_NONE;
    int32 TargetModuleIndex = INDEX_NONE;
    FString TargetEmitterName;
    FString TargetModuleName;
    FString TargetModuleGuid;
    Params->TryGetNumberField(TEXT("emitter_index"), TargetEmitterIndex);
    Params->TryGetNumberField(TEXT("module_index"), TargetModuleIndex);
    Params->TryGetStringField(TEXT("emitter_name"), TargetEmitterName);
    Params->TryGetStringField(TEXT("module_name"), TargetModuleName);
    Params->TryGetStringField(TEXT("module_node_guid"), TargetModuleGuid);

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    UNiagaraNodeFunctionCall* MatchedFunctionCall = nullptr;
    FVersionedNiagaraEmitterData* MatchedEmitterData = nullptr;
    FVersionedNiagaraEmitter MatchedOwningEmitter;
    FString MatchedEmitterName;
    int32 MatchedEmitterIndex = INDEX_NONE;
    int32 MatchedModuleIndex = INDEX_NONE;

    int32 CurrentEmitterIndex = 0;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        const FString EmitterName = EmitterHandle.GetName().ToString();
        const bool bEmitterMatches =
            (TargetEmitterIndex == INDEX_NONE || TargetEmitterIndex == CurrentEmitterIndex) &&
            (TargetEmitterName.IsEmpty() || TargetEmitterName.Equals(EmitterName, ESearchCase::IgnoreCase));

        if (bEmitterMatches)
        {
            if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
            {
                const TArray<UNiagaraNodeFunctionCall*> FunctionCalls = CollectSortedNiagaraFunctionCalls(EmitterData->GraphSource);
                for (int32 ModuleIndex = 0; ModuleIndex < FunctionCalls.Num(); ++ModuleIndex)
                {
                    UNiagaraNodeFunctionCall* FunctionCall = FunctionCalls[ModuleIndex];
                    if (!FunctionCall)
                    {
                        continue;
                    }

                    FString FunctionName = FunctionCall->GetFunctionName();
                    if (FunctionName.IsEmpty())
                    {
                        FunctionName = FunctionCall->Signature.GetNameString();
                    }
                    const FString FunctionGuid = FunctionCall->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
                    const bool bModuleMatches =
                        (TargetModuleIndex == INDEX_NONE || TargetModuleIndex == ModuleIndex) &&
                        (TargetModuleName.IsEmpty() || TargetModuleName.Equals(FunctionName, ESearchCase::IgnoreCase)) &&
                        (TargetModuleGuid.IsEmpty() || TargetModuleGuid.Equals(FunctionGuid, ESearchCase::IgnoreCase));

                    if (bModuleMatches)
                    {
                        MatchedFunctionCall = FunctionCall;
                        MatchedEmitterData = EmitterData;
                        MatchedOwningEmitter = EmitterHandle.GetInstance();
                        MatchedEmitterName = EmitterName;
                        MatchedEmitterIndex = CurrentEmitterIndex;
                        MatchedModuleIndex = ModuleIndex;
                        break;
                    }
                }
            }
        }

        if (MatchedFunctionCall)
        {
            break;
        }
        ++CurrentEmitterIndex;
    }

    if (!MatchedFunctionCall || !MatchedEmitterData)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No matching Niagara module was found"));
    }

    const ENiagaraScriptUsage OutputUsage = FNiagaraStackGraphUtilities::GetOutputNodeUsage(*MatchedFunctionCall);
    UNiagaraScript* OwningScript = FindEmitterScriptForUsage(MatchedEmitterData, OutputUsage);
    if (!OwningScript)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No owning Niagara script was found for the matched module"));
    }

    FCompileConstantResolver ConstantResolver(MatchedOwningEmitter, OutputUsage, MatchedFunctionCall->DebugState);
    TArray<FNiagaraVariable> InputVariables;
    TSet<FNiagaraVariable> HiddenVariables;
    FNiagaraStackGraphUtilities::GetStackFunctionInputs(
        *MatchedFunctionCall,
        InputVariables,
        HiddenVariables,
        ConstantResolver,
        FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly,
        true);

    FNiagaraVariable* MatchedInputVariable = InputVariables.FindByPredicate(
        [&InputName](const FNiagaraVariable& Candidate)
        {
            const FString CandidateName = Candidate.GetName().ToString();
            FString ShortName = CandidateName;
            ShortName.RemoveFromStart(TEXT("Module."));
            return CandidateName.Equals(InputName, ESearchCase::IgnoreCase) ||
                ShortName.Equals(InputName, ESearchCase::IgnoreCase);
        });

    if (!MatchedInputVariable)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("No matching module input was found: %s"), *InputName));
    }

    FNiagaraVariable RapidIterationParameter = BuildRapidIterationParameterForInput(
        MatchedFunctionCall,
        *MatchedInputVariable,
        MatchedEmitterName,
        OwningScript->GetUsage());

    const uint8* ExistingData = RapidIterationParameter.IsValid()
        ? OwningScript->RapidIterationParameters.GetParameterData(RapidIterationParameter)
        : nullptr;
    if (!ExistingData)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Refusing to create a new module input override. Existing RapidIteration value was not found for %s"), *MatchedInputVariable->GetName().ToString()));
    }

    RapidIterationParameter.SetData(ExistingData);
    TSharedPtr<FJsonValue> PreviousValue = NiagaraVariableDataToJsonValue(RapidIterationParameter);

    FString ErrorMessage;
    if (!SetNiagaraVariableDataFromJson(RapidIterationParameter, Value, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    System->Modify();
    OwningScript->Modify();
    OwningScript->RapidIterationParameters.SetParameterData(RapidIterationParameter.GetData(), RapidIterationParameter, false);
    System->MarkPackageDirty();
    if (bRequestCompile)
    {
        System->RequestCompile(false);
    }

    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveLoadedAsset(System, false);
        if (!bSaved)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to save Niagara system after module input value update"));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetStringField(TEXT("emitter_name"), MatchedEmitterName);
    Result->SetNumberField(TEXT("emitter_index"), MatchedEmitterIndex);
    Result->SetStringField(TEXT("module_name"), MatchedFunctionCall->GetFunctionName());
    Result->SetStringField(TEXT("module_node_guid"), MatchedFunctionCall->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    Result->SetNumberField(TEXT("module_index"), MatchedModuleIndex);
    Result->SetStringField(TEXT("input_name"), MatchedInputVariable->GetName().ToString());
    Result->SetStringField(TEXT("input_type"), NiagaraTypeName(MatchedInputVariable->GetType()));
    Result->SetObjectField(TEXT("rapid_iteration_parameter"), NiagaraVariableWithDataToJsonObject(RapidIterationParameter));
    Result->SetField(TEXT("previous_value"), PreviousValue);
    Result->SetField(TEXT("new_value"), NiagaraVariableDataToJsonValue(RapidIterationParameter));
    Result->SetBoolField(TEXT("compile_requested"), bRequestCompile);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("write_scope"), TEXT("existing_rapid_iteration_parameter_only"));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleCreateNiagaraModuleInputOverride(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString InputName;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("input_name"), InputName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'input_name' parameter"));
    }

    const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
    if (!Value.IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'value' parameter"));
    }

    bool bAllowSourceEdit = false;
    Params->TryGetBoolField(TEXT("allow_source_edit"), bAllowSourceEdit);
    if (!bAllowSourceEdit && !IsTempGeneratedNiagaraPath(SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Refusing to create Niagara module input override outside %s: %s"), NiagaraTempGenerationRoot, *SystemPath));
    }

    bool bOverwriteExisting = false;
    Params->TryGetBoolField(TEXT("overwrite_existing"), bOverwriteExisting);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    bool bRequestCompile = true;
    Params->TryGetBoolField(TEXT("request_compile"), bRequestCompile);

    int32 TargetEmitterIndex = INDEX_NONE;
    int32 TargetModuleIndex = INDEX_NONE;
    FString TargetEmitterName;
    FString TargetModuleName;
    FString TargetModuleGuid;
    Params->TryGetNumberField(TEXT("emitter_index"), TargetEmitterIndex);
    Params->TryGetNumberField(TEXT("module_index"), TargetModuleIndex);
    Params->TryGetStringField(TEXT("emitter_name"), TargetEmitterName);
    Params->TryGetStringField(TEXT("module_name"), TargetModuleName);
    Params->TryGetStringField(TEXT("module_node_guid"), TargetModuleGuid);

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    UNiagaraNodeFunctionCall* MatchedFunctionCall = nullptr;
    FVersionedNiagaraEmitterData* MatchedEmitterData = nullptr;
    FVersionedNiagaraEmitter MatchedOwningEmitter;
    FString MatchedEmitterName;
    int32 MatchedEmitterIndex = INDEX_NONE;
    int32 MatchedModuleIndex = INDEX_NONE;

    int32 CurrentEmitterIndex = 0;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        const FString EmitterName = EmitterHandle.GetName().ToString();
        const bool bEmitterMatches =
            (TargetEmitterIndex == INDEX_NONE || TargetEmitterIndex == CurrentEmitterIndex) &&
            (TargetEmitterName.IsEmpty() || TargetEmitterName.Equals(EmitterName, ESearchCase::IgnoreCase));

        if (bEmitterMatches)
        {
            if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
            {
                const TArray<UNiagaraNodeFunctionCall*> FunctionCalls = CollectSortedNiagaraFunctionCalls(EmitterData->GraphSource);
                for (int32 ModuleIndex = 0; ModuleIndex < FunctionCalls.Num(); ++ModuleIndex)
                {
                    UNiagaraNodeFunctionCall* FunctionCall = FunctionCalls[ModuleIndex];
                    if (!FunctionCall)
                    {
                        continue;
                    }

                    FString FunctionName = FunctionCall->GetFunctionName();
                    if (FunctionName.IsEmpty())
                    {
                        FunctionName = FunctionCall->Signature.GetNameString();
                    }
                    const FString FunctionGuid = FunctionCall->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
                    const bool bModuleMatches =
                        (TargetModuleIndex == INDEX_NONE || TargetModuleIndex == ModuleIndex) &&
                        (TargetModuleName.IsEmpty() || TargetModuleName.Equals(FunctionName, ESearchCase::IgnoreCase)) &&
                        (TargetModuleGuid.IsEmpty() || TargetModuleGuid.Equals(FunctionGuid, ESearchCase::IgnoreCase));

                    if (bModuleMatches)
                    {
                        MatchedFunctionCall = FunctionCall;
                        MatchedEmitterData = EmitterData;
                        MatchedOwningEmitter = EmitterHandle.GetInstance();
                        MatchedEmitterName = EmitterName;
                        MatchedEmitterIndex = CurrentEmitterIndex;
                        MatchedModuleIndex = ModuleIndex;
                        break;
                    }
                }
            }
        }

        if (MatchedFunctionCall)
        {
            break;
        }
        ++CurrentEmitterIndex;
    }

    if (!MatchedFunctionCall || !MatchedEmitterData)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No matching Niagara module was found"));
    }

    const ENiagaraScriptUsage OutputUsage = FNiagaraStackGraphUtilities::GetOutputNodeUsage(*MatchedFunctionCall);
    UNiagaraScript* OwningScript = FindEmitterScriptForUsage(MatchedEmitterData, OutputUsage);
    if (!OwningScript)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No owning Niagara script was found for the matched module"));
    }

    FCompileConstantResolver ConstantResolver(MatchedOwningEmitter, OutputUsage, MatchedFunctionCall->DebugState);
    TArray<FNiagaraVariable> InputVariables;
    TSet<FNiagaraVariable> HiddenVariables;
    FNiagaraStackGraphUtilities::GetStackFunctionInputs(
        *MatchedFunctionCall,
        InputVariables,
        HiddenVariables,
        ConstantResolver,
        FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly,
        true);

    FNiagaraVariable* MatchedInputVariable = InputVariables.FindByPredicate(
        [&InputName](const FNiagaraVariable& Candidate)
        {
            const FString CandidateName = Candidate.GetName().ToString();
            FString ShortName = CandidateName;
            ShortName.RemoveFromStart(TEXT("Module."));
            return CandidateName.Equals(InputName, ESearchCase::IgnoreCase) ||
                ShortName.Equals(InputName, ESearchCase::IgnoreCase);
        });

    if (!MatchedInputVariable)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("No matching module input was found: %s"), *InputName));
    }

    FNiagaraVariable RapidIterationParameter = BuildRapidIterationParameterForInput(
        MatchedFunctionCall,
        *MatchedInputVariable,
        MatchedEmitterName,
        OwningScript->GetUsage());

    if (!RapidIterationParameter.IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to build RapidIteration parameter for module input"));
    }

    const uint8* ExistingData = OwningScript->RapidIterationParameters.GetParameterData(RapidIterationParameter);
    if (ExistingData && !bOverwriteExisting)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("RapidIteration override already exists for %s. Use overwrite_existing=true or set_niagara_module_input_value."), *MatchedInputVariable->GetName().ToString()));
    }

    TSharedPtr<FJsonValue> PreviousValue = MakeShared<FJsonValueNull>();
    if (ExistingData)
    {
        RapidIterationParameter.SetData(ExistingData);
        PreviousValue = NiagaraVariableDataToJsonValue(RapidIterationParameter);
    }
    else
    {
        RapidIterationParameter.AllocateData();
    }

    FString ErrorMessage;
    if (!SetNiagaraVariableDataFromJson(RapidIterationParameter, Value, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    System->Modify();
    OwningScript->Modify();
    OwningScript->RapidIterationParameters.SetParameterData(RapidIterationParameter.GetData(), RapidIterationParameter, true);
    System->MarkPackageDirty();
    if (bRequestCompile)
    {
        System->RequestCompile(false);
    }

    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveLoadedAsset(System, false);
        if (!bSaved)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to save Niagara system after module input override creation"));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetStringField(TEXT("emitter_name"), MatchedEmitterName);
    Result->SetNumberField(TEXT("emitter_index"), MatchedEmitterIndex);
    Result->SetStringField(TEXT("module_name"), MatchedFunctionCall->GetFunctionName());
    Result->SetStringField(TEXT("module_node_guid"), MatchedFunctionCall->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    Result->SetNumberField(TEXT("module_index"), MatchedModuleIndex);
    Result->SetStringField(TEXT("input_name"), MatchedInputVariable->GetName().ToString());
    Result->SetStringField(TEXT("input_type"), NiagaraTypeName(MatchedInputVariable->GetType()));
    Result->SetObjectField(TEXT("rapid_iteration_parameter"), NiagaraVariableWithDataToJsonObject(RapidIterationParameter));
    Result->SetBoolField(TEXT("created"), ExistingData == nullptr);
    Result->SetBoolField(TEXT("overwrote_existing"), ExistingData != nullptr);
    Result->SetField(TEXT("previous_value"), PreviousValue);
    Result->SetField(TEXT("new_value"), NiagaraVariableDataToJsonValue(RapidIterationParameter));
    Result->SetBoolField(TEXT("compile_requested"), bRequestCompile);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("write_scope"), TEXT("new_or_explicitly_overwritten_rapid_iteration_parameter"));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraRenderTarget2DModuleInput(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString InputName;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("input_name"), InputName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'input_name' parameter"));
    }

    bool bAllowSourceEdit = false;
    Params->TryGetBoolField(TEXT("allow_source_edit"), bAllowSourceEdit);
    if (!bAllowSourceEdit && !IsTempGeneratedNiagaraPath(SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Refusing to edit Niagara RenderTarget2D module input outside %s: %s"), NiagaraTempGenerationRoot, *SystemPath));
    }

    FString UserParameterName = TEXT("User.RT_IF_Deform");
    Params->TryGetStringField(TEXT("user_parameter_name"), UserParameterName);
    UserParameterName.TrimStartAndEndInline();
    if (UserParameterName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'user_parameter_name' cannot be empty"));
    }
    if (!UserParameterName.StartsWith(TEXT("User.")))
    {
        UserParameterName = FString::Printf(TEXT("User.%s"), *UserParameterName);
    }

    FString RenderTargetAssetPath;
    Params->TryGetStringField(TEXT("render_target_asset_path"), RenderTargetAssetPath);
    RenderTargetAssetPath.TrimStartAndEndInline();

    bool bInheritUserParameterSettings = true;
    Params->TryGetBoolField(TEXT("inherit_user_parameter_settings"), bInheritUserParameterSettings);

    bool bOverwriteExisting = false;
    Params->TryGetBoolField(TEXT("overwrite_existing"), bOverwriteExisting);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    bool bRequestCompile = true;
    Params->TryGetBoolField(TEXT("request_compile"), bRequestCompile);

    int32 TargetEmitterIndex = INDEX_NONE;
    int32 TargetModuleIndex = INDEX_NONE;
    FString TargetEmitterName;
    FString TargetModuleName;
    FString TargetModuleGuid;
    Params->TryGetNumberField(TEXT("emitter_index"), TargetEmitterIndex);
    Params->TryGetNumberField(TEXT("module_index"), TargetModuleIndex);
    Params->TryGetStringField(TEXT("emitter_name"), TargetEmitterName);
    Params->TryGetStringField(TEXT("module_name"), TargetModuleName);
    Params->TryGetStringField(TEXT("module_node_guid"), TargetModuleGuid);

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    UTextureRenderTarget* RenderTargetAsset = nullptr;
    if (!RenderTargetAssetPath.IsEmpty())
    {
        RenderTargetAsset = LoadObject<UTextureRenderTarget>(nullptr, *NormalizeNiagaraObjectPathForLoad(RenderTargetAssetPath));
        if (!RenderTargetAsset)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load render target asset: %s"), *RenderTargetAssetPath));
        }
    }

    UNiagaraNodeFunctionCall* MatchedFunctionCall = nullptr;
    FVersionedNiagaraEmitterData* MatchedEmitterData = nullptr;
    FVersionedNiagaraEmitter MatchedOwningEmitter;
    FString MatchedEmitterName;
    FString MatchedModuleName;
    int32 MatchedEmitterIndex = INDEX_NONE;
    int32 MatchedModuleIndex = INDEX_NONE;

    int32 CurrentEmitterIndex = 0;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        const FString EmitterName = EmitterHandle.GetName().ToString();
        const bool bEmitterMatches =
            (TargetEmitterIndex == INDEX_NONE || TargetEmitterIndex == CurrentEmitterIndex) &&
            (TargetEmitterName.IsEmpty() || TargetEmitterName.Equals(EmitterName, ESearchCase::IgnoreCase));

        if (bEmitterMatches)
        {
            if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
            {
                const TArray<UNiagaraNodeFunctionCall*> FunctionCalls = CollectSortedNiagaraFunctionCalls(EmitterData->GraphSource);
                for (int32 ModuleIndex = 0; ModuleIndex < FunctionCalls.Num(); ++ModuleIndex)
                {
                    UNiagaraNodeFunctionCall* FunctionCall = FunctionCalls[ModuleIndex];
                    if (!FunctionCall)
                    {
                        continue;
                    }

                    FString FunctionName = FunctionCall->GetFunctionName();
                    if (FunctionName.IsEmpty())
                    {
                        FunctionName = FunctionCall->Signature.GetNameString();
                    }
                    const FString FunctionGuid = FunctionCall->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
                    const bool bModuleMatches =
                        (TargetModuleIndex == INDEX_NONE || TargetModuleIndex == ModuleIndex) &&
                        (TargetModuleName.IsEmpty() || TargetModuleName.Equals(FunctionName, ESearchCase::IgnoreCase)) &&
                        (TargetModuleGuid.IsEmpty() || TargetModuleGuid.Equals(FunctionGuid, ESearchCase::IgnoreCase));

                    if (bModuleMatches)
                    {
                        MatchedFunctionCall = FunctionCall;
                        MatchedEmitterData = EmitterData;
                        MatchedOwningEmitter = EmitterHandle.GetInstance();
                        MatchedEmitterName = EmitterName;
                        MatchedModuleName = FunctionName;
                        MatchedEmitterIndex = CurrentEmitterIndex;
                        MatchedModuleIndex = ModuleIndex;
                        break;
                    }
                }
            }
        }

        if (MatchedFunctionCall)
        {
            break;
        }
        ++CurrentEmitterIndex;
    }

    if (!MatchedFunctionCall || !MatchedEmitterData)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No matching Niagara module was found"));
    }

    const ENiagaraScriptUsage OutputUsage = FNiagaraStackGraphUtilities::GetOutputNodeUsage(*MatchedFunctionCall);
    FCompileConstantResolver ConstantResolver(MatchedOwningEmitter, OutputUsage, MatchedFunctionCall->DebugState);
    TArray<FNiagaraVariable> InputVariables;
    TSet<FNiagaraVariable> HiddenVariables;
    FNiagaraStackGraphUtilities::GetStackFunctionInputs(
        *MatchedFunctionCall,
        InputVariables,
        HiddenVariables,
        ConstantResolver,
        FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly,
        true);

    FNiagaraVariable* MatchedInputVariable = InputVariables.FindByPredicate(
        [&InputName](const FNiagaraVariable& Candidate)
        {
            return NiagaraModuleInputNameMatches(Candidate, InputName);
        });

    if (!MatchedInputVariable)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("No matching module input was found: %s"), *InputName));
    }

    const FNiagaraTypeDefinition InputType = MatchedInputVariable->GetType();
    UClass* InputDataInterfaceClass = InputType.GetClass();
    if (!InputType.IsDataInterface() ||
        !InputDataInterfaceClass ||
        !InputDataInterfaceClass->IsChildOf(UNiagaraDataInterfaceRenderTarget2D::StaticClass()))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Module input '%s' is not a RenderTarget2D data interface. Actual type: %s"),
                *MatchedInputVariable->GetName().ToString(),
                *NiagaraTypeName(InputType)));
    }

    FNiagaraVariable UserRenderTargetParameter(FNiagaraTypeDefinition::GetUTextureRenderTargetDef(), FName(*UserParameterName));
    FNiagaraUserRedirectionParameterStore& UserParameterStore = System->GetExposedParameters();

    bool bUserParameterAlreadyExisted = false;
    TArray<FNiagaraVariable> UserParameters;
    UserParameterStore.GetUserParameters(UserParameters);
    for (const FNiagaraVariable& Candidate : UserParameters)
    {
        if (NiagaraUserParameterNameMatches(Candidate, UserParameterName))
        {
            bUserParameterAlreadyExisted = true;
            if (Candidate.GetType() != FNiagaraTypeDefinition::GetUTextureRenderTargetDef())
            {
                return FUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Existing user parameter '%s' has incompatible type: %s"),
                        *UserParameterName,
                        *NiagaraTypeName(Candidate.GetType())));
            }
            break;
        }
    }

    const FNiagaraParameterHandle AliasedInputHandle = BuildAliasedModuleParameterHandleForInput(MatchedFunctionCall, *MatchedInputVariable);

    UEdGraphPin* ExistingOverridePin = FindNiagaraStackFunctionInputOverridePin(MatchedFunctionCall, AliasedInputHandle);
    const int32 PreviousLinkCount = ExistingOverridePin ? ExistingOverridePin->LinkedTo.Num() : 0;
    if (PreviousLinkCount > 0 && !bOverwriteExisting)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("RenderTarget2D module input override already exists for %s. Use overwrite_existing=true to replace it."),
                *MatchedInputVariable->GetName().ToString()));
    }
    if (PreviousLinkCount > 1)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("RenderTarget2D module input override for %s has %d links; refusing to overwrite an unexpected graph shape."),
                *MatchedInputVariable->GetName().ToString(),
                PreviousLinkCount));
    }
    if (PreviousLinkCount == 1 && ExistingOverridePin)
    {
        FString ValidateError;
        if (!ValidateLinkedNiagaraDataInterfaceInputNode(*ExistingOverridePin, ValidateError))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(ValidateError);
        }
    }

    System->Modify();

    if (!bUserParameterAlreadyExisted)
    {
        if (!UserParameterStore.AddParameter(UserRenderTargetParameter, true, true))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Failed to add Niagara user parameter: %s"), *UserRenderTargetParameter.GetName().ToString()));
        }
    }

    MatchedFunctionCall->Modify();

    UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
        *MatchedFunctionCall,
        AliasedInputHandle,
        InputType,
        FGuid(),
        FGuid());

    if (PreviousLinkCount == 1)
    {
        FString ClearError;
        if (!ClearLinkedNiagaraDataInterfaceInputNode(OverridePin, ClearError))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(ClearError);
        }
    }

    UNiagaraDataInterface* CreatedDataInterface = nullptr;
    FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(
        OverridePin,
        UNiagaraDataInterfaceRenderTarget2D::StaticClass(),
        AliasedInputHandle.GetParameterHandleString().ToString(),
        CreatedDataInterface);

    UNiagaraDataInterfaceRenderTarget2D* RenderTargetDataInterface = Cast<UNiagaraDataInterfaceRenderTarget2D>(CreatedDataInterface);
    if (!RenderTargetDataInterface)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create Niagara RenderTarget2D data interface override"));
    }

    RenderTargetDataInterface->Modify();
    RenderTargetDataInterface->bInheritUserParameterSettings = bInheritUserParameterSettings;
    RenderTargetDataInterface->RenderTargetUserParameter.Parameter = UserRenderTargetParameter;
    if (RenderTargetAsset)
    {
        UserParameterStore.SetUObject(RenderTargetAsset, UserRenderTargetParameter);
    }

    System->MarkPackageDirty();
    if (bRequestCompile)
    {
        System->RequestCompile(false);
    }

    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveLoadedAsset(System, false);
        if (!bSaved)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to save Niagara system after RenderTarget2D module input update"));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetStringField(TEXT("emitter_name"), MatchedEmitterName);
    Result->SetNumberField(TEXT("emitter_index"), MatchedEmitterIndex);
    Result->SetStringField(TEXT("module_name"), MatchedModuleName);
    Result->SetStringField(TEXT("module_node_guid"), MatchedFunctionCall->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    Result->SetNumberField(TEXT("module_index"), MatchedModuleIndex);
    Result->SetStringField(TEXT("input_name"), MatchedInputVariable->GetName().ToString());
    Result->SetStringField(TEXT("input_type"), NiagaraTypeName(InputType));
    Result->SetStringField(TEXT("aliased_input_handle"), AliasedInputHandle.GetParameterHandleString().ToString());
    Result->SetStringField(TEXT("user_parameter_name"), UserRenderTargetParameter.GetName().ToString());
    Result->SetBoolField(TEXT("user_parameter_created"), !bUserParameterAlreadyExisted);
    Result->SetStringField(TEXT("render_target_asset"), RenderTargetAsset ? RenderTargetAsset->GetPathName() : FString());
    Result->SetBoolField(TEXT("inherit_user_parameter_settings"), bInheritUserParameterSettings);
    Result->SetStringField(TEXT("data_interface_class"), RenderTargetDataInterface->GetClass()->GetName());
    Result->SetStringField(TEXT("data_interface_user_parameter_name"), RenderTargetDataInterface->RenderTargetUserParameter.Parameter.GetName().ToString());
    Result->SetBoolField(TEXT("data_interface_inherit_user_parameter_settings"), RenderTargetDataInterface->bInheritUserParameterSettings);
    Result->SetNumberField(TEXT("previous_override_link_count"), PreviousLinkCount);
    Result->SetBoolField(TEXT("overwrote_existing"), PreviousLinkCount > 0);
    Result->SetBoolField(TEXT("compile_requested"), bRequestCompile);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("write_scope"), TEXT("render_target2d_data_interface_module_input"));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraModuleInputsBatch(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }

    const TArray<TSharedPtr<FJsonValue>>* EditValues = nullptr;
    if (!Params->TryGetArrayField(TEXT("edits"), EditValues) || !EditValues || EditValues->Num() == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing non-empty 'edits' array"));
    }

    bool bAllowSourceEdit = false;
    Params->TryGetBoolField(TEXT("allow_source_edit"), bAllowSourceEdit);
    if (!bAllowSourceEdit && !IsTempGeneratedNiagaraPath(SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Refusing to batch edit Niagara module inputs outside %s: %s"), NiagaraTempGenerationRoot, *SystemPath));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    bool bContinueOnError = false;
    Params->TryGetBoolField(TEXT("continue_on_error"), bContinueOnError);

    bool bRequestCompile = true;
    Params->TryGetBoolField(TEXT("request_compile"), bRequestCompile);

    FString DefaultOperation = TEXT("set_existing");
    Params->TryGetStringField(TEXT("operation"), DefaultOperation);

    bool bDefaultOverwriteExisting = false;
    Params->TryGetBoolField(TEXT("overwrite_existing"), bDefaultOverwriteExisting);

    const TArray<FString> InheritedFieldNames = {
        TEXT("emitter_index"),
        TEXT("emitter_name"),
        TEXT("module_index"),
        TEXT("module_name"),
        TEXT("module_node_guid"),
        TEXT("input_name"),
        TEXT("value"),
        TEXT("overwrite_existing")
    };

    auto CopyFields = [&InheritedFieldNames](const TSharedPtr<FJsonObject>& Source, const TSharedPtr<FJsonObject>& Target)
    {
        if (!Source.IsValid() || !Target.IsValid())
        {
            return;
        }

        for (const FString& FieldName : InheritedFieldNames)
        {
            const TSharedPtr<FJsonValue> FieldValue = Source->TryGetField(FieldName);
            if (FieldValue.IsValid())
            {
                Target->SetField(FieldName, FieldValue);
            }
        }
    };

    TArray<TSharedPtr<FJsonValue>> ResultValues;
    int32 AppliedCount = 0;
    int32 FailedCount = 0;

    for (int32 EditIndex = 0; EditIndex < EditValues->Num(); ++EditIndex)
    {
        const TSharedPtr<FJsonObject> EditObject = (*EditValues)[EditIndex].IsValid()
            ? (*EditValues)[EditIndex]->AsObject()
            : nullptr;
        if (!EditObject.IsValid())
        {
            TSharedPtr<FJsonObject> ErrorItem = MakeShared<FJsonObject>();
            ErrorItem->SetNumberField(TEXT("edit_index"), EditIndex);
            ErrorItem->SetBoolField(TEXT("success"), false);
            ErrorItem->SetStringField(TEXT("error"), TEXT("Batch edit entry must be an object"));
            ResultValues.Add(MakeShared<FJsonValueObject>(ErrorItem));
            ++FailedCount;
            if (!bContinueOnError)
            {
                break;
            }
            continue;
        }

        FString Operation = DefaultOperation;
        EditObject->TryGetStringField(TEXT("operation"), Operation);

        TSharedPtr<FJsonObject> ChildParams = MakeShared<FJsonObject>();
        ChildParams->SetStringField(TEXT("system_path"), SystemPath);
        ChildParams->SetBoolField(TEXT("allow_source_edit"), bAllowSourceEdit);
        ChildParams->SetBoolField(TEXT("save"), false);
        ChildParams->SetBoolField(TEXT("overwrite_existing"), bDefaultOverwriteExisting);
        CopyFields(Params, ChildParams);
        CopyFields(EditObject, ChildParams);
        ChildParams->SetBoolField(TEXT("allow_source_edit"), bAllowSourceEdit);
        ChildParams->SetBoolField(TEXT("save"), false);
        ChildParams->SetBoolField(TEXT("request_compile"), false);

        if (Operation.Equals(TEXT("upsert"), ESearchCase::IgnoreCase) &&
            !EditObject->HasField(TEXT("overwrite_existing")) &&
            !Params->HasField(TEXT("overwrite_existing")))
        {
            ChildParams->SetBoolField(TEXT("overwrite_existing"), true);
        }

        TSharedPtr<FJsonObject> ItemResult;
        if (Operation.Equals(TEXT("create_override"), ESearchCase::IgnoreCase) ||
            Operation.Equals(TEXT("create"), ESearchCase::IgnoreCase) ||
            Operation.Equals(TEXT("upsert"), ESearchCase::IgnoreCase))
        {
            ItemResult = HandleCreateNiagaraModuleInputOverride(ChildParams);
        }
        else if (Operation.Equals(TEXT("set_existing"), ESearchCase::IgnoreCase) ||
            Operation.Equals(TEXT("set"), ESearchCase::IgnoreCase))
        {
            ItemResult = HandleSetNiagaraModuleInputValue(ChildParams);
        }
        else
        {
            ItemResult = FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unsupported batch operation: %s"), *Operation));
        }

        bool bItemSuccess = false;
        if (ItemResult.IsValid())
        {
            ItemResult->TryGetBoolField(TEXT("success"), bItemSuccess);
            ItemResult->SetNumberField(TEXT("edit_index"), EditIndex);
            ItemResult->SetStringField(TEXT("operation"), Operation);
        }
        else
        {
            ItemResult = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Batch edit returned no result"));
            ItemResult->SetNumberField(TEXT("edit_index"), EditIndex);
            ItemResult->SetStringField(TEXT("operation"), Operation);
        }

        if (bItemSuccess)
        {
            ++AppliedCount;
        }
        else
        {
            ++FailedCount;
        }

        ResultValues.Add(MakeShared<FJsonValueObject>(ItemResult));
        if (!bItemSuccess && !bContinueOnError)
        {
            break;
        }
    }

    bool bSaved = false;
    bool bCompileRequested = false;
    bool bPostProcessFailed = false;
    FString PostProcessError;
    if (AppliedCount > 0)
    {
        if (UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath)))
        {
            if (bRequestCompile)
            {
                System->RequestCompile(false);
                bCompileRequested = true;
            }
            if (bSave)
            {
                bSaved = UEditorAssetLibrary::SaveLoadedAsset(System, false);
                if (!bSaved)
                {
                    bPostProcessFailed = true;
                    PostProcessError = TEXT("Failed to save Niagara system after batch module input edits");
                }
            }
        }
        else
        {
            bPostProcessFailed = true;
            PostProcessError = FString::Printf(TEXT("Failed to reload Niagara system after batch edits: %s"), *SystemPath);
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    const bool bPartialSuccess = AppliedCount > 0 && FailedCount > 0;
    const bool bSuccess = FailedCount == 0 && !bPostProcessFailed;
    Result->SetBoolField(TEXT("success"), bSuccess);
    Result->SetStringField(TEXT("system_path"), SystemPath);
    Result->SetNumberField(TEXT("requested_count"), EditValues->Num());
    Result->SetNumberField(TEXT("processed_count"), ResultValues.Num());
    Result->SetNumberField(TEXT("applied_count"), AppliedCount);
    Result->SetNumberField(TEXT("failed_count"), FailedCount);
    Result->SetBoolField(TEXT("partial_success"), bPartialSuccess);
    Result->SetBoolField(TEXT("continue_on_error"), bContinueOnError);
    Result->SetBoolField(TEXT("compile_requested"), bCompileRequested);
    Result->SetBoolField(TEXT("saved"), bSaved);
    if (!bSuccess && PostProcessError.IsEmpty() && FailedCount > 0)
    {
        PostProcessError = FString::Printf(TEXT("%d of %d Niagara module input batch edits failed"), FailedCount, EditValues->Num());
    }
    if (!bSuccess && !PostProcessError.IsEmpty())
    {
        Result->SetStringField(TEXT("error"), PostProcessError);
    }
    Result->SetStringField(TEXT("write_scope"), TEXT("batch_rapid_iteration_module_inputs"));
    Result->SetArrayField(TEXT("results"), ResultValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraUserParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString ParameterName;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'parameter_name' parameter"));
    }

    const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
    if (!Value.IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'value' parameter"));
    }

    bool bAllowSourceEdit = false;
    Params->TryGetBoolField(TEXT("allow_source_edit"), bAllowSourceEdit);
    if (!bAllowSourceEdit && !IsTempGeneratedNiagaraPath(SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Refusing to edit Niagara system outside %s: %s"), NiagaraTempGenerationRoot, *SystemPath));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    TArray<FNiagaraVariable> UserParameters;
    System->GetExposedParameters().GetUserParameters(UserParameters);
    FNiagaraVariable* MatchedParameter = UserParameters.FindByPredicate(
        [&ParameterName](const FNiagaraVariable& Candidate)
        {
            const FString CandidateName = Candidate.GetName().ToString();
            return CandidateName.Equals(ParameterName, ESearchCase::IgnoreCase) ||
                CandidateName.Equals(FString::Printf(TEXT("User.%s"), *ParameterName), ESearchCase::IgnoreCase);
        });

    if (!MatchedParameter)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("No matching Niagara user parameter was found: %s"), *ParameterName));
    }

    if (!IsSettableNiagaraUserParameterType(MatchedParameter->GetType()))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported Niagara user parameter type: %s"), *NiagaraTypeName(MatchedParameter->GetType())));
    }

    FString ErrorMessage;
    FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
    if (!SetNiagaraUserParameterValue(Store, *MatchedParameter, Value, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    System->Modify();
    System->MarkPackageDirty();
    System->RequestCompile(false);

    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveLoadedAsset(System, false);
        if (!bSaved)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to save Niagara system after user parameter update"));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetStringField(TEXT("parameter_name"), MatchedParameter->GetName().ToString());
    Result->SetStringField(TEXT("type"), NiagaraTypeName(MatchedParameter->GetType()));
    Result->SetField(TEXT("value"), NiagaraParameterValueToJson(Store, *MatchedParameter));
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}
